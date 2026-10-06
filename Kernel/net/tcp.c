/* IPv4-Stack: TCP (Verbindungen nach aussen, also als Client)
 *
 * Pro Verbindung ein Empfangs- (256 KB) und ein Sendepuffer (32 KB). Gesendete, noch nicht bestaetigte Daten bleiben im
 * Sendepuffer, bis die Gegenseite sie bestaetigt; laeuft der Wiederholungs-Timer ab (RTO aus der gemessenen Laufzeit,
 * mindestens 200 ms, bei jedem Versuch doppelt so lang), wird ab der ersten unbestaetigten Stelle neu gesendet
 * (ebenso nach drei doppelten ACKs). Segmente, die vor der Zeit kommen (Luecke davor), werden an ihrer Stelle im
 * Empfangspuffer abgelegt und gemerkt (bis 8 Bereiche); schliesst die Wiederholung die Luecke, springt rcv_nxt ueber
 * alles. Angekuendigt wird als Fenster der freie Platz im Empfangspuffer - mit Window Scaling (Faktor 8, wenn die
 * Gegenseite es auch kann) der ganze, sonst hoechstens 64 KB; bestaetigt wird jedes zweite Segment, ein einzelnes am
 * Ende jedes Empfangsdurchgangs (net_poll_locked).
 *
 * Nach close() (Deskriptor zu) lebt die Verbindung weiter, bis die gepufferten Daten und das FIN bestaetigt sind; dann
 * raeumt tcp_timer sie ab. Alles laeuft unter net_lock. */

#include "arch/x86_64/apic.h"
#include "mm/heap.h"
#include "core/syscall.h"
#include "net/net_internal.h"

#define TCP_HDR   20
#define TCP_CONNS 16
#define RX_SIZE   262144u /* Empfangspuffer: mit Window Scaling wird er ganz als Fenster angekuendigt */
#define TX_SIZE   32768u
#define OUR_WS    3       /* unser Fensterfaktor: Fenster = Feld << 3 (bis 512 KB) */
#define OOO_MAX   8       /* Luecken, die wir uns merken (Segmente, die vor der Zeit kamen) */
#define OUR_MSS   (NET_MTU - IP_HDR - TCP_HDR)

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { T_FREE, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT_1, T_FIN_WAIT_2, T_CLOSING, T_TIME_WAIT, T_CLOSE_WAIT, T_LAST_ACK,
       T_CLOSED };

struct TcpConn {
    int      state;
    int      orphan;     /* Deskriptor geschlossen: nur noch zu Ende senden und abbauen */
    int      err;        /* Grund fuer T_CLOSED (ERR_CONNRESET, ERR_CONNREFUSED, ERR_TIMEDOUT) */
    Iface   *f;
    uint8_t  ip[4], mac[6];
    uint16_t lport, rport, mss;
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd;
    uint32_t rcv_nxt, adv_wnd;
    uint8_t *rx, *tx;
    uint32_t rx_head, rx_count; /* empfangen, noch nicht gelesen */
    uint32_t tx_head, tx_count; /* ab snd_una: gesendet (unbestaetigt) und noch nicht gesendet */
    int      fin_queued;        /* close: nach den Daten ein FIN (es hat die Nummer snd_una + tx_count) */
    int      peer_fin;
    int      ack_pending, dupacks, retries;
    int      unacked;           /* empfangene Segmente seit dem letzten ACK */
    uint64_t rto, rtx_at;       /* Wiederholungs-Timer (0 = aus) */
    uint32_t rtt_seq;           /* Laufzeitmessung: wird gemessen, bis rtt_seq bestaetigt ist */
    uint64_t rtt_start;
    int      rtt_on;
    uint64_t srtt, rttvar;      /* in ms (x8 bzw. x4 wie ueblich gespeichert) */
    uint64_t deadline;          /* TIME_WAIT bzw. FIN_WAIT_2 ohne Deskriptor: dann abraeumen */
    uint8_t  snd_scale, rcv_scale; /* Window Scaling (RFC 7323): Faktor der Gegenseite bzw. unserer, 0 = ohne */
    int      n_ooo;             /* gemerkte Bereiche [ooo_start, ooo_end), schon im Empfangspuffer abgelegt */
    uint32_t ooo_start[OOO_MAX], ooo_end[OOO_MAX];
    Event    ev;
};

static TcpConn  conns[TCP_CONNS];
static uint16_t next_port;

static inline int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static inline int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }

static void conn_free(TcpConn *c)
{
    uint8_t *rx = c->rx, *tx = c->tx;
    memset(c, 0, sizeof(*c));
    kfree(rx);
    kfree(tx);
}

static uint32_t rx_free(const TcpConn *c)
{
    return RX_SIZE - c->rx_count;
}

/* ---------- Senden ---------- */

static uint16_t tcp_csum(const uint8_t src[4], const uint8_t dst[4], const uint8_t *seg, uint32_t len)
{
    uint8_t pseudo[12];
    memcpy(pseudo, src, 4);
    memcpy(pseudo + 4, dst, 4);
    pseudo[8] = 0;
    pseudo[9] = 6;
    put16(pseudo + 10, (uint16_t)len);
    return net_csum_fold(net_csum_add(net_csum_add(0, pseudo, 12), seg, len));
}

/* Segment mit n Datenbytes ab Position off (relativ zu snd_una) des Sendepuffers */
static void send_seg(TcpConn *c, uint32_t seq, uint8_t flags, uint32_t off, uint32_t n)
{
    uint8_t *t = net_txbuf + ETH_HDR + IP_HDR;
    uint32_t hl = (flags & F_SYN) ? TCP_HDR + 8 : TCP_HDR;
    uint32_t wnd = c->orphan ? 65535 : rx_free(c);
    uint32_t field = (flags & F_SYN) ? wnd : wnd >> c->rcv_scale; /* im SYN nie skaliert */
    if (field > 65535)
        field = 65535;
    wnd = (flags & F_SYN) ? field : field << c->rcv_scale;
    put16(t, c->lport);
    put16(t + 2, c->rport);
    put32(t + 4, seq);
    put32(t + 8, (flags & F_ACK) ? c->rcv_nxt : 0);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    put16(t + 14, (uint16_t)field);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & F_SYN) { /* Optionen MSS und Window Scaling (NOP davor: auf 4 Byte ausgerichtet) */
        t[20] = 2;
        t[21] = 4;
        put16(t + 22, OUR_MSS);
        t[24] = 1;
        t[25] = 3;
        t[26] = 3;
        t[27] = OUR_WS;
    }
    for (uint32_t i = 0; i < n;) { /* aus dem Ringpuffer (hoechstens ein Umbruch) */
        uint32_t pos = (c->tx_head + off + i) % TX_SIZE, k = TX_SIZE - pos;
        if (k > n - i)
            k = n - i;
        memcpy(t + hl + i, c->tx + pos, k);
        i += k;
    }
    put16(t + 16, tcp_csum(c->f->ip, c->ip, t, hl + n));
    net_ip_send(c->f, c->mac, c->f->ip, c->ip, 6, hl + n);
    if (flags & F_ACK) {
        c->ack_pending = 0;
        c->unacked = 0;
        c->adv_wnd = wnd;
    }
}

static void timer_start(TcpConn *c)
{
    if (!c->rtx_at)
        c->rtx_at = time_ms() + c->rto;
}

/* Sendet, was der Puffer hergibt und das Fenster der Gegenseite erlaubt, danach ggf. das FIN.
 * probe: auch bei Fenster 0 ein Byte (die Gegenseite meldet so ihr neues Fenster) */
static void tcp_output(TcpConn *c, int probe)
{
    if (c->state != T_ESTABLISHED && c->state != T_CLOSE_WAIT && c->state != T_FIN_WAIT_1 && c->state != T_LAST_ACK &&
        c->state != T_CLOSING)
        return;
    uint32_t wnd = c->snd_wnd ? c->snd_wnd : (uint32_t)probe;
    for (;;) {
        uint32_t off = c->snd_nxt - c->snd_una;
        uint32_t avail = off < c->tx_count ? c->tx_count - off : 0;
        uint32_t room = off < wnd ? wnd - off : 0;
        uint32_t n = avail < room ? avail : room;
        if (n > c->mss)
            n = c->mss;
        if (!n)
            break;
        send_seg(c, c->snd_nxt, F_ACK | (n == avail ? F_PSH : 0), off, n);
        if (!c->rtt_on && c->snd_nxt == c->snd_max) { /* Laufzeit messen (nur an erstmals gesendeten Daten) */
            c->rtt_on = 1;
            c->rtt_seq = c->snd_nxt + n;
            c->rtt_start = time_ms();
        }
        c->snd_nxt += n;
        if (seq_lt(c->snd_max, c->snd_nxt))
            c->snd_max = c->snd_nxt;
        timer_start(c);
    }
    if (c->fin_queued && c->snd_nxt == c->snd_una + c->tx_count) {
        send_seg(c, c->snd_nxt, F_FIN | F_ACK, 0, 0);
        c->snd_nxt++;
        if (seq_lt(c->snd_max, c->snd_nxt))
            c->snd_max = c->snd_nxt;
        timer_start(c);
    }
}

/* RST als Antwort auf ein Segment, zu dem es keine Verbindung gibt */
static void send_reset(Iface *f, const uint8_t *iph, const uint8_t *t, uint32_t dlen, const uint8_t *src_mac)
{
    uint8_t flags = t[13];
    if ((flags & F_RST) || ip_zero(f->ip))
        return;
    uint8_t src[4], *r = net_txbuf + ETH_HDR + IP_HDR;
    memcpy(src, iph + 12, 4);
    put16(r, be16(t + 2));
    put16(r + 2, be16(t));
    if (flags & F_ACK) {
        put32(r + 4, be32(t + 8));
        put32(r + 8, 0);
        r[13] = F_RST;
    } else {
        put32(r + 4, 0);
        put32(r + 8, be32(t + 4) + dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0));
        r[13] = F_RST | F_ACK;
    }
    r[12] = (TCP_HDR / 4) << 4;
    put16(r + 14, 0);
    put16(r + 16, 0);
    put16(r + 18, 0);
    put16(r + 16, tcp_csum(f->ip, src, r, TCP_HDR));
    net_ip_send(f, src_mac, f->ip, src, 6, TCP_HDR);
}

/* ---------- Empfang ---------- */

static void rtt_sample(TcpConn *c, uint64_t m)
{
    if (!c->srtt) {
        c->srtt = m * 8;
        c->rttvar = m * 2;
    } else {
        int64_t d = (int64_t)m - (int64_t)(c->srtt / 8);
        if (d < 0)
            d = -d;
        c->rttvar = c->rttvar - c->rttvar / 4 + (uint64_t)d;
        c->srtt = c->srtt - c->srtt / 8 + m;
    }
    c->rto = c->srtt / 8 + c->rttvar;
    if (c->rto < 200)
        c->rto = 200;
    if (c->rto > 60000)
        c->rto = 60000;
}

/* ACK-Feld auswerten; 1 = unser FIN ist (jetzt oder schon frueher) bestaetigt */
static int process_ack(TcpConn *c, uint32_t ack, uint32_t wnd, uint32_t dlen)
{
    uint32_t fin_seq = c->snd_una + c->tx_count;
    if (seq_lt(c->snd_max, ack)) { /* bestaetigt Ungesendetes */
        c->ack_pending = 1;
        return 0;
    }
    if (seq_lt(c->snd_una, ack)) {
        uint32_t n = ack - c->snd_una;
        if (n > c->tx_count)
            n = c->tx_count; /* der Rest ist das FIN */
        c->tx_head = (c->tx_head + n) % TX_SIZE;
        c->tx_count -= n;
        c->snd_una = ack;
        if (seq_lt(c->snd_nxt, ack))
            c->snd_nxt = ack;
        if (c->rtt_on && seq_le(c->rtt_seq, ack)) {
            rtt_sample(c, time_ms() - c->rtt_start);
            c->rtt_on = 0;
        }
        c->retries = 0;
        c->dupacks = 0;
        c->rtx_at = 0;
        if (c->snd_una != c->snd_max)
            timer_start(c);
        c->snd_wnd = wnd;
    } else if (ack == c->snd_una) {
        if (!dlen && wnd == c->snd_wnd && c->snd_una != c->snd_max && ++c->dupacks == 3) {
            c->snd_nxt = c->snd_una; /* schnelle Wiederholung: verlorenes Segment und alles danach */
            c->rtt_on = 0;
        }
        c->snd_wnd = wnd;
    }
    if (!c->snd_wnd && c->tx_count) /* Fenster zu: der Timer fragt spaeter mit einem Byte nach */
        timer_start(c);
    return c->fin_queued && seq_lt(fin_seq, c->snd_una);
}

/* Segment, das vor der Zeit kam (Luecke davor, z.B. umsortiert durch WLAN-Aggregation): an seiner Stelle im
 * Empfangspuffer ablegen und den Bereich merken - schliesst die Wiederholung die Luecke, ist alles schon da */
static void ooo_store(TcpConn *c, uint32_t seq, const uint8_t *data, uint32_t dlen)
{
    if (c->orphan || (c->state != T_ESTABLISHED && c->state != T_FIN_WAIT_1 && c->state != T_FIN_WAIT_2))
        return;
    uint32_t off = seq - c->rcv_nxt;
    if (off >= rx_free(c) || dlen > rx_free(c) - off)
        return; /* ausserhalb des Fensters */
    for (uint32_t i = 0; i < dlen;) {
        uint32_t pos = (c->rx_head + c->rx_count + off + i) % RX_SIZE, k = RX_SIZE - pos;
        if (k > dlen - i)
            k = dlen - i;
        memcpy(c->rx + pos, data + i, k);
        i += k;
    }
    uint32_t s = seq, e = seq + dlen;
    for (int i = 0; i < c->n_ooo; i++) /* mit ueberlappenden oder angrenzenden Bereichen vereinen */
        if (seq_le(c->ooo_start[i], e) && seq_le(s, c->ooo_end[i])) {
            if (seq_lt(c->ooo_start[i], s))
                s = c->ooo_start[i];
            if (seq_lt(e, c->ooo_end[i]))
                e = c->ooo_end[i];
            c->ooo_start[i] = c->ooo_start[--c->n_ooo];
            c->ooo_end[i] = c->ooo_end[c->n_ooo];
            i = -1;
        }
    if (c->n_ooo < OOO_MAX) {
        c->ooo_start[c->n_ooo] = s;
        c->ooo_end[c->n_ooo++] = e;
    }
}

/* Nach neuen Daten in der Reihe: anschliessende gemerkte Bereiche uebernehmen. 1 = rcv_nxt ist weitergesprungen */
static int ooo_advance(TcpConn *c)
{
    int moved = 0;
    for (int i = 0; i < c->n_ooo; i++) {
        if (seq_lt(c->rcv_nxt, c->ooo_start[i]))
            continue;
        if (seq_lt(c->rcv_nxt, c->ooo_end[i])) {
            c->rx_count += c->ooo_end[i] - c->rcv_nxt;
            c->rcv_nxt = c->ooo_end[i];
            moved = 1;
        }
        c->ooo_start[i] = c->ooo_start[--c->n_ooo]; /* erledigt (oder schon ueberholt) */
        c->ooo_end[i] = c->ooo_end[c->n_ooo];
        i = -1;
    }
    return moved;
}

void tcp_in(Iface *f, const uint8_t *iph, const uint8_t *t, uint32_t len, const uint8_t *src_mac)
{
    if (len < TCP_HDR)
        return;
    uint32_t hl = (t[12] >> 4) * 4u;
    if (hl < TCP_HDR || hl > len || tcp_csum(iph + 12, iph + 16, t, len) != 0) {
        f->rx_dropped++;
        return;
    }
    uint16_t sport = be16(t), dport = be16(t + 2);
    uint32_t seq = be32(t + 4), ack = be32(t + 8), wnd = be16(t + 14);
    uint8_t flags = t[13];
    const uint8_t *data = t + hl;
    uint32_t dlen = len - hl;

    TcpConn *c = 0;
    for (int i = 0; i < TCP_CONNS && !c; i++)
        if (conns[i].state != T_FREE && conns[i].lport == dport && conns[i].rport == sport &&
            ip_eq(conns[i].ip, iph + 12))
            c = &conns[i];
    if (!c || c->state == T_CLOSED) {
        send_reset(f, iph, t, dlen, src_mac);
        return;
    }

    if (c->state == T_SYN_SENT) {
        if ((flags & F_ACK) && ack != c->iss + 1) {
            send_reset(f, iph, t, dlen, src_mac);
            return;
        }
        if (flags & F_RST) {
            if (flags & F_ACK) {
                c->state = T_CLOSED;
                c->err = ERR_CONNREFUSED;
                event_signal(&c->ev);
            }
            return;
        }
        if (!(flags & F_SYN) || !(flags & F_ACK))
            return; /* gleichzeitiges Oeffnen gibt es hier nicht */
        c->mss = 536;
        int ws = -1;
        for (uint32_t o = TCP_HDR; o < hl;) { /* Optionen: MSS, Window Scaling */
            uint8_t kind = t[o];
            if (kind == 0)
                break;
            if (kind == 1) {
                o++;
                continue;
            }
            if (o + 1 >= hl || t[o + 1] < 2)
                break;
            if (kind == 2 && t[o + 1] == 4 && o + 4 <= hl)
                c->mss = be16(t + o + 2);
            if (kind == 3 && t[o + 1] == 3 && o + 3 <= hl)
                ws = t[o + 2] > 14 ? 14 : t[o + 2];
            o += t[o + 1];
        }
        if (ws >= 0) { /* beide Seiten koennen es: ab jetzt gelten die Faktoren */
            c->snd_scale = (uint8_t)ws;
            c->rcv_scale = OUR_WS;
        }
        if (c->mss > OUR_MSS)
            c->mss = OUR_MSS;
        if (c->mss < 64)
            c->mss = 64;
        c->rcv_nxt = seq + 1;
        c->snd_una = c->snd_nxt = c->snd_max = ack;
        c->snd_wnd = wnd;
        if (c->retries == 0) /* Laufzeit des SYN (nur ohne Wiederholung eindeutig) */
            rtt_sample(c, time_ms() - c->rtt_start);
        c->retries = 0;
        c->rtx_at = 0;
        c->rtt_on = 0;
        c->state = T_ESTABLISHED;
        send_seg(c, c->snd_nxt, F_ACK, 0, 0);
        event_signal(&c->ev);
        return;
    }

    /* Bereits Empfangenes (Wiederholung) vorne abschneiden; spaeteres (Luecke davor) im Puffer ablegen */
    if (flags & F_SYN) { /* wiederholtes SYN-ACK: unser ACK ging verloren */
        c->ack_pending = 1;
        return;
    }
    wnd <<= c->snd_scale; /* ausser im SYN gilt der Faktor der Gegenseite */
    if (seq_lt(seq, c->rcv_nxt)) {
        uint32_t skip = c->rcv_nxt - seq;
        if (skip >= dlen + ((flags & F_FIN) ? 1 : 0)) {
            if (dlen || (flags & F_FIN))
                c->ack_pending = 1;
            skip = dlen;
            flags &= (uint8_t)~F_FIN;
        }
        data += skip;
        dlen -= skip;
        seq += skip;
        flags &= (uint8_t)~F_RST;
    } else if (seq != c->rcv_nxt) {
        if (dlen)
            ooo_store(c, seq, data, dlen);
        if (dlen || (flags & F_FIN)) /* sofort ein doppeltes ACK: die Gegenseite wiederholt dann schnell */
            send_seg(c, c->snd_nxt, F_ACK, 0, 0);
        return;
    }

    if (flags & F_RST) {
        int was = c->state;
        c->state = T_CLOSED;
        c->err = ERR_CONNRESET;
        c->rtx_at = 0;
        if (was == T_LAST_ACK || was == T_TIME_WAIT || was == T_CLOSING)
            c->err = 0;
        event_signal(&c->ev);
        return;
    }
    if (!(flags & F_ACK))
        return;

    int fin_acked = process_ack(c, ack, wnd, dlen);
    if (fin_acked) {
        if (c->state == T_FIN_WAIT_1)
            c->state = T_FIN_WAIT_2;
        else if (c->state == T_CLOSING) {
            c->state = T_TIME_WAIT;
            c->deadline = time_ms() + 2000;
        } else if (c->state == T_LAST_ACK) {
            c->state = T_CLOSED;
            c->rtx_at = 0;
        }
        if (c->state == T_FIN_WAIT_2 && c->orphan)
            c->deadline = time_ms() + 30000;
    }

    int accepts = c->state == T_ESTABLISHED || c->state == T_FIN_WAIT_1 || c->state == T_FIN_WAIT_2;
    if (dlen && accepts) {
        uint32_t n = dlen;
        if (!c->orphan) {
            if (n > rx_free(c))
                n = rx_free(c); /* mehr als angekuendigt: der Rest kommt wieder */
            for (uint32_t i = 0; i < n;) {
                uint32_t pos = (c->rx_head + c->rx_count + i) % RX_SIZE, k = RX_SIZE - pos;
                if (k > n - i)
                    k = n - i;
                memcpy(c->rx + pos, data + i, k);
                i += k;
            }
            c->rx_count += n;
        }
        c->rcv_nxt += n;
        c->ack_pending = 1;
        if (c->n_ooo && ooo_advance(c)) { /* Luecke geschlossen: gleich bestaetigen, was jetzt alles da ist */
            send_seg(c, c->snd_nxt, F_ACK, 0, 0);
            flags &= (uint8_t)~F_FIN; /* ein FIN hinter der Luecke kommt noch einmal */
        } else if (++c->unacked >= 2) { /* wie ueblich: spaetestens jedes zweite Segment bestaetigen */
            send_seg(c, c->snd_nxt, F_ACK, 0, 0);
        }
        if (n < dlen)
            flags &= (uint8_t)~F_FIN;
    }
    if ((flags & F_FIN) && accepts) {
        c->rcv_nxt++;
        c->peer_fin = 1;
        send_seg(c, c->snd_nxt, F_ACK, 0, 0);
        if (c->state == T_ESTABLISHED)
            c->state = T_CLOSE_WAIT;
        else if (c->state == T_FIN_WAIT_1)
            c->state = T_CLOSING;
        else {
            c->state = T_TIME_WAIT;
            c->deadline = time_ms() + 2000;
        }
    }
    tcp_output(c, 0);
    event_signal(&c->ev);
}

/* Nach jedem Empfangsdurchgang: ein ACK je Verbindung fuer alles, was in diesem Durchgang kam */
void tcp_flush_acks(void)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        TcpConn *c = &conns[i];
        if (c->state != T_FREE && c->state != T_SYN_SENT && c->state != T_CLOSED && c->ack_pending)
            send_seg(c, c->snd_nxt, F_ACK, 0, 0);
    }
}

/* Alle 100 ms aus dem Netzwerk-Thread: Wiederholungen, Abbau */
void tcp_timer(uint64_t now)
{
    for (int i = 0; i < TCP_CONNS; i++) {
        TcpConn *c = &conns[i];
        if (c->state == T_FREE)
            continue;
        if (c->orphan && (c->state == T_CLOSED || (c->deadline && now >= c->deadline &&
                                                   (c->state == T_TIME_WAIT || c->state == T_FIN_WAIT_2)))) {
            conn_free(c);
            continue;
        }
        if (c->state == T_TIME_WAIT && now >= c->deadline)
            c->state = T_CLOSED;
        if (!c->rtx_at || now < c->rtx_at)
            continue;
        c->rtx_at = 0;
        c->rtt_on = 0;
        if (++c->retries > (c->state == T_SYN_SENT ? 5 : 10)) {
            if (c->state != T_SYN_SENT)
                send_seg(c, c->snd_nxt, F_RST | F_ACK, 0, 0);
            c->state = T_CLOSED;
            c->err = ERR_TIMEDOUT;
            event_signal(&c->ev);
            continue;
        }
        c->rto = c->rto * 2 > 60000 ? 60000 : c->rto * 2;
        if (c->state == T_SYN_SENT) {
            send_seg(c, c->iss, F_SYN, 0, 0);
            timer_start(c);
            continue;
        }
        c->snd_nxt = c->snd_una; /* ab der ersten unbestaetigten Stelle neu */
        tcp_output(c, 1);
        if (c->snd_una != c->snd_max || c->tx_count)
            timer_start(c);
    }
}

/* ---------- Schnittstelle ---------- */

/* Wartet kurz auf Neues fuer diese Verbindung (Sperre wird so lange freigegeben) */
static void conn_wait(TcpConn *c)
{
    net_poll_locked();
    mutex_unlock(&net_lock);
    event_wait(&c->ev, 10);
    mutex_lock(&net_lock);
}

int tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms, TcpConn **out)
{
    if (!port)
        return ERR_INVAL;
    uint8_t *rx = kmalloc(RX_SIZE), *tx = kmalloc(TX_SIZE);
    if (!rx || !tx) {
        kfree(rx);
        kfree(tx);
        return ERR_NOMEM;
    }
    mutex_lock(&net_lock);
    uint8_t next[4], mac[6];
    Iface *f = net_is_local(ip) ? 0 : net_route(ip, next);
    int r = net_is_local(ip) ? ERR_CONNREFUSED : !f || ip_zero(f->ip) ? ERR_NETUNREACH : arp_resolve(f, next, mac, 3000);
    TcpConn *c = 0;
    for (int i = 0; i < TCP_CONNS && !c && r == 0; i++)
        if (conns[i].state == T_FREE)
            c = &conns[i];
    if (r == 0 && !c)
        r = ERR_NOMEM;
    if (r) {
        mutex_unlock(&net_lock);
        kfree(rx);
        kfree(tx);
        return r;
    }
    if (!next_port)
        next_port = (uint16_t)(49152 + net_rnd() % 16384);
    for (int tries = 0; tries < 16384; tries++) { /* freier Port 49152-65535 */
        uint16_t p = next_port;
        next_port = next_port == 65535 ? 49152 : next_port + 1;
        int used = 0;
        for (int i = 0; i < TCP_CONNS; i++)
            if (conns[i].state != T_FREE && conns[i].lport == p)
                used = 1;
        if (!used) {
            c->lport = p;
            break;
        }
    }
    c->state = T_SYN_SENT;
    c->f = f;
    memcpy(c->ip, ip, 4);
    memcpy(c->mac, mac, 6);
    c->rport = port;
    c->rx = rx;
    c->tx = tx;
    c->mss = 536;
    c->iss = net_rnd();
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->rto = 1000;
    c->rtt_start = time_ms();
    send_seg(c, c->iss, F_SYN, 0, 0);
    timer_start(c);

    uint64_t start = time_ms();
    while (c->state == T_SYN_SENT) {
        if (net_interrupted())
            r = ERR_INTR;
        else if (timeout_ms != NET_WAIT_FOREVER && time_ms() - start >= timeout_ms)
            r = ERR_TIMEDOUT;
        if (r)
            break;
        conn_wait(c);
    }
    if (!r && c->state != T_ESTABLISHED && c->state != T_CLOSE_WAIT)
        r = c->err ? c->err : ERR_CONNRESET;
    if (r)
        conn_free(c);
    else
        *out = c;
    mutex_unlock(&net_lock);
    return r;
}

/* Liest, was da ist (wartet, bis mindestens ein Byte kommt). 0 = Gegenseite hat geschlossen */
int64_t tcp_recv(TcpConn *c, void *buf, uint64_t max)
{
    mutex_lock(&net_lock);
    int64_t r;
    for (;;) {
        if (!c->rx_count)
            net_poll_locked();
        if (c->rx_count) {
            uint32_t n = c->rx_count < max ? c->rx_count : (uint32_t)max;
            for (uint32_t i = 0; i < n;) {
                uint32_t k = RX_SIZE - c->rx_head;
                if (k > n - i)
                    k = n - i;
                memcpy((uint8_t *)buf + i, c->rx + c->rx_head, k);
                c->rx_head = (c->rx_head + k) % RX_SIZE;
                i += k;
            }
            c->rx_count -= n;
            uint32_t most = 65535u << c->rcv_scale, fr = rx_free(c) > most ? most : rx_free(c);
            fr &= ~((1u << c->rcv_scale) - 1); /* so, wie es im Feld ankommt */
            if (c->state != T_CLOSED && c->state != T_SYN_SENT && fr - c->adv_wnd >= 2u * c->mss && fr > c->adv_wnd)
                send_seg(c, c->snd_nxt, F_ACK, 0, 0); /* das Fenster ist deutlich groesser geworden */
            r = n;
            break;
        }
        if (c->peer_fin) {
            r = 0;
            break;
        }
        if (c->state == T_CLOSED) {
            r = c->err ? c->err : 0;
            break;
        }
        if (net_interrupted()) {
            r = ERR_INTR;
            break;
        }
        conn_wait(c);
    }
    mutex_unlock(&net_lock);
    return r;
}

/* Schreibt alles (wartet auf Platz im Sendepuffer); Bytes oder Fehler */
int64_t tcp_send(TcpConn *c, const void *buf, uint64_t len)
{
    mutex_lock(&net_lock);
    uint64_t done = 0;
    int64_t r = 0;
    while (done < len) {
        if (c->state == T_CLOSED) {
            r = c->err ? c->err : ERR_PIPE;
            break;
        }
        if (c->fin_queued || (c->state != T_ESTABLISHED && c->state != T_CLOSE_WAIT)) {
            r = ERR_PIPE;
            break;
        }
        uint32_t space = TX_SIZE - c->tx_count;
        if (space) {
            uint32_t n = len - done < space ? (uint32_t)(len - done) : space;
            for (uint32_t i = 0; i < n;) {
                uint32_t pos = (c->tx_head + c->tx_count) % TX_SIZE, k = TX_SIZE - pos;
                if (k > n - i)
                    k = n - i;
                memcpy(c->tx + pos, (const uint8_t *)buf + done + i, k);
                c->tx_count += k;
                i += k;
            }
            done += n;
            tcp_output(c, 0);
            continue;
        }
        if (net_interrupted()) {
            r = ERR_INTR;
            break;
        }
        conn_wait(c);
    }
    mutex_unlock(&net_lock);
    return done ? (int64_t)done : r;
}

/* Wartende Bytes; -1 = nichts mehr zu erwarten (geschlossen oder Fehler) */
int64_t tcp_pending(TcpConn *c)
{
    mutex_lock(&net_lock);
    net_poll_locked();
    int64_t r = c->rx_count ? (int64_t)c->rx_count : (c->peer_fin || c->state == T_CLOSED) ? -1 : 0;
    mutex_unlock(&net_lock);
    return r;
}

/* Deskriptor zu: ungelesene Daten -> RST (wie ueblich), sonst geordnet abbauen (FIN nach den gepufferten Daten) */
void tcp_close(TcpConn *c)
{
    if (!c)
        return;
    mutex_lock(&net_lock);
    if (c->state == T_SYN_SENT || c->state == T_CLOSED || c->state == T_TIME_WAIT) {
        conn_free(c);
    } else if (c->rx_count) {
        send_seg(c, c->snd_nxt, F_RST | F_ACK, 0, 0);
        conn_free(c);
    } else {
        c->orphan = 1;
        if (c->state == T_ESTABLISHED || c->state == T_CLOSE_WAIT) {
            c->fin_queued = 1;
            c->state = c->state == T_ESTABLISHED ? T_FIN_WAIT_1 : T_LAST_ACK;
            tcp_output(c, 0);
        } else if (c->state == T_FIN_WAIT_2) {
            c->deadline = time_ms() + 30000;
        }
    }
    mutex_unlock(&net_lock);
}

int tcp_info(int index, TcpInfo *out)
{
    mutex_lock(&net_lock);
    int n = 0, r = -1;
    for (int i = 0; i < TCP_CONNS; i++) {
        TcpConn *c = &conns[i];
        if (c->state == T_FREE || n++ != index)
            continue;
        memset(out, 0, sizeof(*out));
        memcpy(out->ip, c->ip, 4);
        memcpy(out->local_ip, c->f->ip, 4);
        out->lport = c->lport;
        out->rport = c->rport;
        out->state = (uint32_t)c->state;
        out->rx_queued = c->rx_count;
        out->tx_queued = c->tx_count;
        out->rto_ms = (uint32_t)c->rto;
        out->srtt_ms = (uint32_t)(c->srtt / 8);
        r = 0;
        break;
    }
    mutex_unlock(&net_lock);
    return r;
}
