#include "blk.h"
#include "apic.h"
#include "heap.h"
#include "kprintf.h"
#include "paging.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

/* NVMe 1.x: Admin-Queue plus eine I/O-Queue, PRP-Listen, Polling (keine Interrupts). Pro aktivem Namespace mit
 * 512-Byte-Bloecken wird ein BlkDev registriert. */

/* Controller-Register */
#define REG_CAP   0x00 /* 64 Bit */
#define REG_VS    0x08
#define REG_CC    0x14
#define REG_CSTS  0x1C
#define REG_AQA   0x24
#define REG_ASQ   0x28 /* 64 Bit */
#define REG_ACQ   0x30 /* 64 Bit */
#define DOORBELLS 0x1000

#define CC_EN        (1u << 0)
#define CC_IOSQES(n) ((uint32_t)(n) << 16)
#define CC_IOCQES(n) ((uint32_t)(n) << 20)
#define CSTS_RDY     (1u << 0)
#define CSTS_CFS     (1u << 1)

/* Admin-Kommandos */
#define ADMIN_DELETE_SQ  0x00
#define ADMIN_CREATE_SQ  0x01
#define ADMIN_CREATE_CQ  0x05
#define ADMIN_IDENTIFY   0x06
/* NVM-Kommandos */
#define NVM_FLUSH        0x00
#define NVM_WRITE        0x01
#define NVM_READ         0x02

#define ADMIN_DEPTH  16
#define IO_DEPTH     64
#define BOUNCE_SECTORS 64
#define PAGE 4096ULL

typedef struct {
    uint32_t cdw[16];
} NvmeCmd;

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; /* Bit 0: Phase, Bit 1-15: Status */
} NvmeCpl;

typedef struct {
    volatile NvmeCmd *sq;
    volatile NvmeCpl *cq;
    uint16_t depth, sq_tail, cq_head, next_cid;
    uint8_t  phase;
    volatile uint32_t *sq_doorbell, *cq_doorbell;
} Queue;

typedef struct {
    volatile uint8_t *regs;
    uint32_t stride;              /* Doorbell-Abstand in Bytes */
    Queue    admin, io;
    uint8_t *bounce;              /* DMA-Datenpuffer */
    uint64_t *prp_list;           /* PRP-Liste fuer Transfers > 2 Seiten */
    uint32_t max_sectors;         /* durch MDTS begrenzt */
    Mutex    lock;
} Ctrl;

typedef struct {
    Ctrl    *ctrl;
    uint32_t nsid;
} Ns;

static inline uint32_t rd32(Ctrl *c, uint32_t off) { return *(volatile uint32_t *)(c->regs + off); }
static inline void wr32(Ctrl *c, uint32_t off, uint32_t v) { *(volatile uint32_t *)(c->regs + off) = v; }
static inline uint64_t rd64(Ctrl *c, uint32_t off) { return rd32(c, off) | ((uint64_t)rd32(c, off + 4) << 32); }
static inline void wr64(Ctrl *c, uint32_t off, uint64_t v) { wr32(c, off, (uint32_t)v); wr32(c, off + 4, (uint32_t)(v >> 32)); }

static inline void barrier(void) { __asm__ __volatile__("mfence" : : : "memory"); }

/* Legt ein Kommando in die Queue, klingelt und wartet auf die Completion. Ergebnis: 0 = Erfolg, sonst Status
 * (SCT/SC, nie 0) bzw. -1 bei Timeout. dw0 = Ergebniswort des Kommandos. Aufruf mit gehaltenem Controller-Lock. */
static int submit(Ctrl *c, Queue *q, const NvmeCmd *cmd, uint32_t *dw0, uint32_t timeout_ms)
{
    uint16_t cid = q->next_cid++;
    volatile NvmeCmd *slot = &q->sq[q->sq_tail];
    for (int i = 0; i < 16; i++)
        slot->cdw[i] = cmd->cdw[i];
    slot->cdw[0] = (cmd->cdw[0] & 0xFFFF) | ((uint32_t)cid << 16);

    q->sq_tail = (q->sq_tail + 1) % q->depth;
    barrier();
    *q->sq_doorbell = q->sq_tail;

    volatile NvmeCpl *cpl = &q->cq[q->cq_head];
    if (!WAIT_UNTIL((cpl->status & 1) == q->phase, timeout_ms)) {
        kprintf("nvme: Timeout (Opcode %#x)\n", cmd->cdw[0] & 0xFF);
        return -1;
    }
    barrier();
    uint16_t status = cpl->status >> 1;
    if (dw0)
        *dw0 = cpl->dw0;

    if (++q->cq_head == q->depth) {
        q->cq_head = 0;
        q->phase ^= 1;
    }
    *q->cq_doorbell = q->cq_head;
    (void)c;
    return status; /* 0 = Erfolg */
}

static int alloc_queue(Ctrl *c, Queue *q, uint16_t depth, uint16_t qid)
{
    q->sq = blk_dma_alloc(depth * sizeof(NvmeCmd));
    q->cq = blk_dma_alloc(depth * sizeof(NvmeCpl));
    if (!q->sq || !q->cq)
        return -1;
    q->depth = depth;
    q->phase = 1;
    q->sq_doorbell = (volatile uint32_t *)(c->regs + DOORBELLS + (2 * qid) * c->stride);
    q->cq_doorbell = (volatile uint32_t *)(c->regs + DOORBELLS + (2 * qid + 1) * c->stride);
    return 0;
}

static int admin(Ctrl *c, NvmeCmd *cmd, uint32_t *dw0)
{
    return submit(c, &c->admin, cmd, dw0, 5000);
}

static int identify(Ctrl *c, uint32_t cns, uint32_t nsid, void *buf)
{
    NvmeCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw[0] = ADMIN_IDENTIFY;
    cmd.cdw[1] = nsid;
    cmd.cdw[6] = (uint32_t)(uint64_t)buf;
    cmd.cdw[7] = (uint32_t)((uint64_t)buf >> 32);
    cmd.cdw[10] = cns;
    return admin(c, &cmd, 0);
}

/* ---------- I/O ---------- */

static int io_command(Ctrl *c, uint8_t opcode, uint32_t nsid, uint64_t lba, uint32_t count)
{
    NvmeCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw[0] = opcode;
    cmd.cdw[1] = nsid;
    if (opcode != NVM_FLUSH) {
        uint32_t pages = (count * BLK_SECTOR_SIZE + PAGE - 1) / PAGE;
        uint64_t base = (uint64_t)c->bounce;
        uint64_t prp2 = 0;
        if (pages == 2) {
            prp2 = base + PAGE;
        } else if (pages > 2) {
            for (uint32_t i = 0; i < pages - 1; i++)
                c->prp_list[i] = base + (i + 1) * PAGE;
            prp2 = (uint64_t)c->prp_list;
        }
        cmd.cdw[6] = (uint32_t)base;
        cmd.cdw[7] = (uint32_t)(base >> 32);
        cmd.cdw[8] = (uint32_t)prp2;
        cmd.cdw[9] = (uint32_t)(prp2 >> 32);
        cmd.cdw[10] = (uint32_t)lba;
        cmd.cdw[11] = (uint32_t)(lba >> 32);
        cmd.cdw[12] = count - 1; /* 0-basiert */
    }
    int r = submit(c, &c->io, &cmd, 0, 10000);
    if (r != 0)
        kprintf("nvme: Kommando %#x LBA %lu fehlgeschlagen (Status %#x)\n", opcode, lba, r);
    return r;
}

static int transfer(Ns *ns, int write, uint64_t lba, uint32_t count, void *buf)
{
    Ctrl *c = ns->ctrl;
    mutex_lock(&c->lock);
    int r = 0;
    uint8_t *b = buf;
    while (count && r == 0) {
        uint32_t n = count > c->max_sectors ? c->max_sectors : count;
        if (write)
            memcpy(c->bounce, b, (uint64_t)n * BLK_SECTOR_SIZE);
        r = io_command(c, write ? NVM_WRITE : NVM_READ, ns->nsid, lba, n);
        if (r == 0 && !write)
            memcpy(b, c->bounce, (uint64_t)n * BLK_SECTOR_SIZE);
        lba += n;
        b += (uint64_t)n * BLK_SECTOR_SIZE;
        count -= n;
    }
    mutex_unlock(&c->lock);
    return r == 0 ? 0 : -1;
}

static int nvme_read(BlkDev *d, uint64_t lba, uint32_t count, void *buf)
{
    return transfer(d->priv, 0, lba, count, buf);
}

static int nvme_write(BlkDev *d, uint64_t lba, uint32_t count, const void *buf)
{
    return transfer(d->priv, 1, lba, count, (void *)buf);
}

static int nvme_flush(BlkDev *d)
{
    Ns *ns = d->priv;
    mutex_lock(&ns->ctrl->lock);
    int r = io_command(ns->ctrl, NVM_FLUSH, ns->nsid, 0, 0);
    mutex_unlock(&ns->ctrl->lock);
    return r == 0 ? 0 : -1;
}

/* ---------- Initialisierung ---------- */

static int register_namespace(Ctrl *c, uint32_t nsid, int ctrl_index, uint8_t *ident, const char *model)
{
    if (identify(c, 0, nsid, ident) != 0)
        return -1;
    uint64_t nsze;
    memcpy(&nsze, ident, 8);
    uint8_t flbas = ident[26];
    uint32_t idx = (flbas & 0xF) | ((flbas >> 1) & 0x30); /* Bit 6:5 = hoehere Formatindizes */
    uint32_t lbaf;
    memcpy(&lbaf, ident + 128 + 4 * idx, 4);
    uint32_t lbads = (lbaf >> 16) & 0xFF; /* log2 der Blockgroesse */
    uint32_t ms = lbaf & 0xFFFF;          /* Metadaten pro Block */

    if (nsze == 0)
        return 0;
    if (lbads != 9 || ms != 0) {
        kprintf("nvme: Namespace %u: Blockgroesse 2^%u (Metadaten %u) wird nicht unterstuetzt\n", nsid, lbads, ms);
        return 0;
    }

    Ns *ns = kcalloc(1, sizeof(*ns));
    if (!ns)
        return -1;
    ns->ctrl = c;
    ns->nsid = nsid;

    BlkDev bd;
    memset(&bd, 0, sizeof(bd));
    bd.name[0] = 'n'; bd.name[1] = 'v'; bd.name[2] = 'm'; bd.name[3] = 'e';
    bd.name[4] = (char)('0' + ctrl_index);
    bd.name[5] = 'n';
    bd.name[6] = (char)('0' + nsid % 10);
    for (int i = 0; i < 40 && model[i]; i++)
        bd.model[i] = model[i];
    bd.sectors = nsze;
    bd.read = nvme_read;
    bd.write = nvme_write;
    bd.flush = nvme_flush;
    bd.priv = ns;
    return blk_register(&bd);
}

static void probe_controller(const PciDevice *pci, int index)
{
    uint64_t bar0 = pci_bar_mem(pci, 0);
    if (!bar0) {
        kprintf("nvme: Controller %02x:%02x.%u ohne BAR0\n", pci->bus, pci->dev, pci->fn);
        return;
    }
    pci_enable(pci, 0, 1, 1);
    pci_set_driver(pci, "nvme");

    Ctrl *c = kcalloc(1, sizeof(*c));
    if (!c)
        return;
    c->lock = (Mutex)MUTEX_INIT;

    /* Register (0x1000 Doorbells + Platz fuer Queue 0 und 1) abbilden. Der Doorbell-Abstand steht in CAP, also
     * erst die ersten zwei Seiten mappen, CAP lesen, dann ggf. mehr. */
    if (paging_map_mmio(bar0, 0x2000) != 0) {
        kprintf("nvme: BAR0 nicht mappbar\n");
        return;
    }
    c->regs = (volatile uint8_t *)bar0;

    uint64_t cap = rd64(c, REG_CAP);
    uint32_t dstrd = (cap >> 32) & 0xF;
    uint32_t mpsmin = (cap >> 48) & 0xF;
    uint32_t timeout_ms = (uint32_t)(((cap >> 24) & 0xFF) + 1) * 500;
    c->stride = 4u << dstrd;
    uint64_t needed = DOORBELLS + 4ULL * c->stride;
    if (needed > 0x2000 && paging_map_mmio(bar0, needed) != 0) {
        kprintf("nvme: Doorbells nicht mappbar\n");
        return;
    }
    kprintf("nvme: Controller %02x:%02x.%u @ %#lx, Version %u.%u, MQES %u, Timeout %u ms\n", pci->bus, pci->dev, pci->fn,
            bar0, rd32(c, REG_VS) >> 16, (rd32(c, REG_VS) >> 8) & 0xFF, (unsigned)(cap & 0xFFFF) + 1, timeout_ms);
    if (mpsmin != 0) {
        kprintf("nvme: Mindest-Seitengroesse != 4 KiB wird nicht unterstuetzt\n");
        return;
    }
    if (!(((cap >> 37) & 0xFF) & 1)) {
        kprintf("nvme: NVM-Kommandosatz nicht verfuegbar\n");
        return;
    }

    /* 1. Controller abschalten (die Firmware hat ihn evtl. laufen lassen) */
    wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
    if (!WAIT_UNTIL(!(rd32(c, REG_CSTS) & CSTS_RDY), timeout_ms)) {
        kprintf("nvme: Controller laesst sich nicht abschalten\n");
        return;
    }

    /* 2. Admin-Queues */
    if (alloc_queue(c, &c->admin, ADMIN_DEPTH, 0) != 0) {
        kprintf("nvme: kein Speicher fuer die Admin-Queue\n");
        return;
    }
    wr32(c, REG_AQA, ((uint32_t)(ADMIN_DEPTH - 1) << 16) | (ADMIN_DEPTH - 1));
    wr64(c, REG_ASQ, (uint64_t)c->admin.sq);
    wr64(c, REG_ACQ, (uint64_t)c->admin.cq);

    /* 3. Einschalten: NVM-Kommandosatz, 4-KiB-Seiten, Eintragsgroessen 64 Byte (SQ) und 16 Byte (CQ) */
    wr32(c, REG_CC, CC_EN | CC_IOSQES(6) | CC_IOCQES(4));
    if (!WAIT_UNTIL((rd32(c, REG_CSTS) & (CSTS_RDY | CSTS_CFS)) != 0, timeout_ms) || (rd32(c, REG_CSTS) & CSTS_CFS) ||
        !(rd32(c, REG_CSTS) & CSTS_RDY)) {
        kprintf("nvme: Controller wird nicht bereit (CSTS %#x)\n", rd32(c, REG_CSTS));
        return;
    }

    /* 4. Controller-Daten: Modell, max. Transfergroesse */
    uint8_t *ident = blk_dma_alloc(PAGE);
    if (!ident || identify(c, 1, 0, ident) != 0) {
        kprintf("nvme: Identify Controller fehlgeschlagen\n");
        return;
    }
    char model[41];
    memcpy(model, ident + 24, 40);
    model[40] = 0;
    blk_trim_model(model);
    uint32_t mdts = ident[77]; /* Potenz von 2 (in 4-KiB-Seiten), 0 = unbegrenzt */
    uint32_t ns_count;         /* NN: Anzahl der Namespaces (hoechste gueltige NSID) */
    memcpy(&ns_count, ident + 516, 4);
    c->max_sectors = BOUNCE_SECTORS;
    if (mdts && ((1u << mdts) * (PAGE / BLK_SECTOR_SIZE)) < c->max_sectors)
        c->max_sectors = (1u << mdts) * (PAGE / BLK_SECTOR_SIZE);

    /* 5. I/O-Queue-Paar anlegen (erst CQ, dann SQ) */
    if (alloc_queue(c, &c->io, IO_DEPTH, 1) != 0) {
        kprintf("nvme: kein Speicher fuer die I/O-Queue\n");
        return;
    }
    NvmeCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw[0] = ADMIN_CREATE_CQ;
    cmd.cdw[6] = (uint32_t)(uint64_t)c->io.cq;
    cmd.cdw[7] = (uint32_t)((uint64_t)c->io.cq >> 32);
    cmd.cdw[10] = ((uint32_t)(IO_DEPTH - 1) << 16) | 1;
    cmd.cdw[11] = 1; /* physisch zusammenhaengend, keine Interrupts */
    if (admin(c, &cmd, 0) != 0) {
        kprintf("nvme: I/O-Completion-Queue nicht anlegbar\n");
        return;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw[0] = ADMIN_CREATE_SQ;
    cmd.cdw[6] = (uint32_t)(uint64_t)c->io.sq;
    cmd.cdw[7] = (uint32_t)((uint64_t)c->io.sq >> 32);
    cmd.cdw[10] = ((uint32_t)(IO_DEPTH - 1) << 16) | 1;
    cmd.cdw[11] = (1u << 16) | 1; /* CQ 1, physisch zusammenhaengend */
    if (admin(c, &cmd, 0) != 0) {
        kprintf("nvme: I/O-Submission-Queue nicht anlegbar\n");
        return;
    }

    c->bounce = blk_dma_alloc(BOUNCE_SECTORS * BLK_SECTOR_SIZE);
    c->prp_list = blk_dma_alloc(PAGE);
    if (!c->bounce || !c->prp_list) {
        kprintf("nvme: kein Speicher fuer Datenpuffer\n");
        return;
    }

    /* 6. Aktive Namespaces. Ab NVMe 1.1 gibt es dafuer eine Liste (CNS 2). Aeltere Controller (z.B. VMware: 1.0)
     *    kennen sie nicht; dann werden die NSIDs 1..NN einzeln abgefragt (unbenutzte liefern Groesse 0 oder Fehler). */
    uint32_t *list = (uint32_t *)ident;
    memset(ident, 0, PAGE);
#ifdef NVME_NO_NSLIST /* Testhilfe: den Fallback erzwingen */
    int have_list = 0;
#else
    int have_list = identify(c, 2, 0, ident) == 0 && list[0] != 0;
#endif
    if (have_list) {
        uint32_t ids[16];
        int n = 0;
        for (; n < 16 && list[n]; n++)
            ids[n] = list[n];
        for (int i = 0; i < n; i++)
            register_namespace(c, ids[i], index, ident, model);
    } else {
        if (ns_count == 0 || ns_count > 16)
            ns_count = ns_count == 0 ? 1 : 16;
        for (uint32_t nsid = 1; nsid <= ns_count; nsid++)
            register_namespace(c, nsid, index, ident, model);
    }
}

void nvme_probe(void)
{
    PciDevice pci;
    for (unsigned i = 0; pci_find_class(0x01, 0x08, 0x02, i, &pci) == 0; i++)
        probe_controller(&pci, (int)i);
}
