#include "usb.h"
#include "blk.h"
#include "heap.h"
#include "kprintf.h"
#include "sched.h"
#include "string.h"

/* USB-Massenspeicher (Bulk-Only-Transport, SCSI-Befehlssatz) als Blockgeraet, z.B. USB-Sticks. 512-Byte-Sektoren, 32-Bit-LBA. */

#define CBW_SIG 0x43425355u /* "USBC" */
#define CSW_SIG 0x53425355u /* "USBS" */
#define DATA_SECTORS 64     /* pro Anfrage (32 KiB) */

typedef struct {
    UsbDevice *d;
    uint8_t    in_ep, out_ep, iface;
    uint32_t   tag;
    uint8_t   *cbw;      /* im DMA-Speicher: CBW, CSW und Datenpuffer */
    uint8_t   *csw;
    uint8_t   *data;
    uint64_t   sectors;
} Msc;

static int msc_count;

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Nach einem schweren Fehler: Bulk-Only Mass Storage Reset und beide Endpunkte freigeben */
static void reset_recovery(Msc *m)
{
    usb_control(m->d, 0x21, 0xFF, 0, m->iface, 0, 0);
    usb_endpoint_recover(m->d, m->in_ep);
    usb_endpoint_recover(m->d, m->out_ep);
}

/* Ein SCSI-Kommando: CBW, optional Daten, CSW. Datenrichtung in = 1: Geraet -> Host. 0 = Erfolg, -1 = Kommando fehlgeschlagen
 * (CSW-Status 1), -2 = Uebertragungsfehler. Aufruf mit gehaltener Controller-Sperre. */
static int bot(Msc *m, const uint8_t *cb, int cb_len, int in, uint32_t data_len)
{
    memset(m->cbw, 0, 32);
    uint32_t *w = (uint32_t *)m->cbw;
    w[0] = CBW_SIG;
    w[1] = ++m->tag;
    w[2] = data_len;
    m->cbw[12] = in ? 0x80 : 0;
    m->cbw[13] = 0; /* LUN 0 */
    m->cbw[14] = (uint8_t)cb_len;
    memcpy(m->cbw + 15, cb, (size_t)cb_len);

    int r = usb_bulk(m->d, m->out_ep, m->cbw, 31, 3000);
    if (r < 0) {
        if (r == -6)
            usb_endpoint_recover(m->d, m->out_ep);
        reset_recovery(m);
        return -2;
    }

    if (data_len) {
        r = usb_bulk(m->d, in ? m->in_ep : m->out_ep, m->data, data_len, 15000);
        if (r == -6) /* Stall: Endpunkt freigeben, danach den CSW lesen (BOT 6.7) */
            usb_endpoint_recover(m->d, in ? m->in_ep : m->out_ep);
        else if (r < 0) {
            reset_recovery(m);
            return -2;
        }
    }

    memset(m->csw, 0, 16);
    r = usb_bulk(m->d, m->in_ep, m->csw, 13, 5000);
    if (r == -6) {
        usb_endpoint_recover(m->d, m->in_ep);
        r = usb_bulk(m->d, m->in_ep, m->csw, 13, 5000);
    }
    uint32_t *c = (uint32_t *)m->csw;
    if (r != 13 || c[0] != CSW_SIG || c[1] != m->tag) {
        reset_recovery(m);
        return -2;
    }
    if (m->csw[12] == 2) { /* Phasenfehler */
        reset_recovery(m);
        return -2;
    }
    return m->csw[12] == 0 ? 0 : -1;
}

static int scsi_simple(Msc *m, uint8_t opcode)
{
    uint8_t cb[6] = {opcode, 0, 0, 0, 0, 0};
    return bot(m, cb, 6, 0, 0);
}

static int rw(BlkDev *bd, uint64_t lba, uint32_t count, void *buf, int write)
{
    Msc *m = bd->priv;
    if (!usb_alive(m->d) || lba + count > 0xFFFFFFFFull + 1)
        return -1;
    uint8_t *p = buf;
    int result = 0;

    usb_lock(m->d);
    while (count && result == 0) {
        uint32_t n = count > DATA_SECTORS ? DATA_SECTORS : count;
        uint8_t cb[10] = {write ? 0x2A : 0x28, 0, 0, 0, 0, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, 0};
        put_be32(cb + 2, (uint32_t)lba);
        if (write)
            memcpy(m->data, p, (size_t)n * BLK_SECTOR_SIZE);
        if (bot(m, cb, 10, !write, n * BLK_SECTOR_SIZE) != 0) {
            kprintf("usb: %s-Fehler bei LBA %lu\n", write ? "Schreib" : "Lese", lba);
            result = -1;
            break;
        }
        if (!write)
            memcpy(p, m->data, (size_t)n * BLK_SECTOR_SIZE);
        lba += n;
        p += (size_t)n * BLK_SECTOR_SIZE;
        count -= n;
    }
    usb_unlock(m->d);
    return result;
}

static int msc_read(BlkDev *bd, uint64_t lba, uint32_t count, void *buf)         { return rw(bd, lba, count, buf, 0); }
static int msc_write(BlkDev *bd, uint64_t lba, uint32_t count, const void *buf)  { return rw(bd, lba, count, (void *)buf, 1); }

static int msc_flush(BlkDev *bd)
{
    Msc *m = bd->priv;
    if (!usb_alive(m->d))
        return -1;
    uint8_t cb[10] = {0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* SYNCHRONIZE CACHE(10) */
    usb_lock(m->d);
    int r = bot(m, cb, 10, 0, 0);
    usb_unlock(m->d);
    return r == 0 ? 0 : -1;
}

int usb_msc_probe(UsbDevice *d, const UsbIface *iface, const char *name_hint)
{
    (void)name_hint;
    const UsbEpDesc *in = 0, *out = 0;
    for (int i = 0; i < iface->ep_count; i++) {
        if ((iface->eps[i].attrs & 3) != 2)
            continue;
        if (iface->eps[i].address & 0x80)
            in = &iface->eps[i];
        else
            out = &iface->eps[i];
    }
    if (!in || !out)
        return -1;

    UsbEpDesc eps[2] = {*in, *out};
    if (usb_add_endpoints(d, eps, 2) != 0)
        return -1;

    Msc *m = kcalloc(1, sizeof(*m));
    uint8_t *mem = blk_dma_alloc(4096 + DATA_SECTORS * BLK_SECTOR_SIZE);
    if (!m || !mem)
        return -1;
    m->d = d;
    m->in_ep = in->address;
    m->out_ep = out->address;
    m->iface = iface->number;
    m->cbw = mem;
    m->csw = mem + 64;
    m->data = mem + 4096;

    /* Maximale LUN erfragen (viele Geraete antworten mit Stall; dann gilt LUN 0) und bereit werden lassen */
    uint8_t lun;
    usb_control(d, 0xA1, 0xFE, 0, iface->number, &lun, 1);

    int ready = 0;
    uint8_t inq[36];
    memset(inq, 0, sizeof(inq));
    for (int attempt = 0; attempt < 40 && !ready; attempt++) {
        if (scsi_simple(m, 0x00) == 0) { /* TEST UNIT READY */
            ready = 1;
            break;
        }
        uint8_t sense_cb[6] = {0x03, 0, 0, 0, 18, 0}; /* REQUEST SENSE: loescht Attention-Zustaende */
        bot(m, sense_cb, 6, 1, 18);
        thread_sleep_ms(100);
    }
    if (!ready) {
        kprintf("usb: Massenspeicher wird nicht bereit\n");
        return -1;
    }

    uint8_t inq_cb[6] = {0x12, 0, 0, 0, 36, 0}; /* INQUIRY: Hersteller/Produkt */
    if (bot(m, inq_cb, 6, 1, 36) == 0)
        memcpy(inq, m->data, 36);

    uint8_t cap_cb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}; /* READ CAPACITY(10) */
    if (bot(m, cap_cb, 10, 1, 8) != 0) {
        kprintf("usb: READ CAPACITY fehlgeschlagen\n");
        return -1;
    }
    uint32_t last = get_be32(m->data), block = get_be32(m->data + 4);
    if (block != BLK_SECTOR_SIZE) {
        kprintf("usb: Blockgroesse %u wird nicht unterstuetzt (nur 512)\n", block);
        return -1;
    }
    m->sectors = (uint64_t)last + 1;

    BlkDev bd;
    memset(&bd, 0, sizeof(bd));
    bd.name[0] = 'u'; bd.name[1] = 's'; bd.name[2] = 'b';
    bd.name[3] = (char)('0' + msc_count++);
    for (int i = 0; i < 24; i++) /* "Hersteller Produkt" aus INQUIRY */
        bd.model[i] = (char)(inq[8 + i] >= 32 && inq[8 + i] < 127 ? inq[8 + i] : ' ');
    bd.model[24] = 0;
    blk_trim_model(bd.model);
    bd.sectors = m->sectors;
    bd.read = msc_read;
    bd.write = msc_write;
    bd.flush = msc_flush;
    bd.priv = m;
    blk_register(&bd);
    kprintf("usb: Massenspeicher %s: %lu Sektoren (%lu MiB) %s\n", bd.name, bd.sectors, bd.sectors * 512 / (1024 * 1024), bd.model);
    return 0;
}
