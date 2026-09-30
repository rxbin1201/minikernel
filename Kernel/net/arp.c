/* IPv4-Stack: ARP (Adressaufloesung und Tabelle) */

#include "arch/x86_64/apic.h"
#include "net/net_internal.h"

/* ---------- ARP ---------- */

int arp_lookup(const uint8_t ip[4], uint8_t mac[6])
{
    for (int i = 0; i < ARP_SIZE; i++)
        if (arp_table[i].valid && ip_eq(arp_table[i].ip, ip)) {
            memcpy(mac, arp_table[i].mac, 6);
            return 1;
        }
    return 0;
}

/* Eintrag setzen; create = 0: nur vorhandene Eintraege auffrischen */
void arp_update(const uint8_t ip[4], const uint8_t mac[6], int iface, int create)
{
    if (ip_zero(ip))
        return;
    int slot = -1, free_slot = -1, oldest = 0;
    for (int i = 0; i < ARP_SIZE; i++) {
        if (arp_table[i].valid && ip_eq(arp_table[i].ip, ip))
            slot = i;
        else if (!arp_table[i].valid && free_slot < 0)
            free_slot = i;
        if (arp_table[i].valid && arp_table[i].ms < arp_table[oldest].ms)
            oldest = i;
    }
    if (slot < 0) {
        if (!create)
            return;
        slot = free_slot >= 0 ? free_slot : oldest; /* voll: aeltesten Eintrag ersetzen */
    }
    arp_table[slot].valid = 1;
    memcpy(arp_table[slot].ip, ip, 4);
    memcpy(arp_table[slot].mac, mac, 6);
    arp_table[slot].iface = iface;
    arp_table[slot].ms = time_ms();
}

void arp_send(Iface *f, uint16_t op, const uint8_t dst_mac[6], const uint8_t tha[6], const uint8_t tpa[4])
{
    uint8_t *e = net_txbuf, *a = net_txbuf + ETH_HDR;
    memcpy(e, dst_mac, 6);
    memcpy(e + 6, f->dev.mac, 6);
    put16(e + 12, 0x0806);
    put16(a, 1);
    put16(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, f->dev.mac, 6);
    memcpy(a + 14, f->ip, 4);
    memcpy(a + 18, tha, 6);
    memcpy(a + 24, tpa, 4);
    net_dev_send(f, net_txbuf, ETH_HDR + 28);
}

void arp_in(Iface *f, const uint8_t *a, uint32_t len)
{
    if (len < 28 || be16(a) != 1 || be16(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4)
        return;
    uint16_t op = be16(a + 6);
    const uint8_t *sha = a + 8, *spa = a + 14, *tpa = a + 24;
    int for_us = !ip_zero(f->ip) && ip_eq(tpa, f->ip);
    arp_update(spa, sha, (int)(f - net_ifs), for_us);
    if (for_us && op == 1)
        arp_send(f, 2, sha, sha, spa);
}
