#include "net.h"
#include "apic.h"
#include "blk.h"
#include "cmdline.h"
#include "heap.h"
#include "kprintf.h"
#include "paging.h"
#include "pci.h"
#include "sched.h"
#include "string.h"

/* Intel-Gigabit-Netzwerkkarten mit e1000-Registersatz:
 *   82540EM/82545EM ("e1000", QEMU und VMware), 82574L ("e1000e", QEMU und VMware),
 *   I217/I218/I219 (im Chipsatz eingebaut, "PCH"; z.B. I219-V auf vielen Mainboards).
 * Einfache (Legacy-)Deskriptoren, ein Empfangs- und ein Sende-Ring, keine Interrupts (der Netzwerk-Thread pollt).
 *
 * Bei den Chipsatz-Varianten (PCH) wird der Controller absichtlich NICHT komplett zurueckgesetzt: Ein Reset setzt
 * dort auch den PHY-Baustein zurueck, der dann ueber die Management Engine neu eingerichtet werden muesste. Statt dessen
 * wird Empfang/Senden gestoppt, der Stromsparmodus (ULP) des PHY verlassen und alles neu programmiert.
 * "e1000reset" in cmdline.txt erzwingt trotzdem einen Reset (zum Ausprobieren). */

enum { K_8254X, K_82574, K_PCH_LPT, K_PCH_SPT };

typedef struct {
    uint16_t    id;
    uint8_t     kind;
    const char *name;
} NicId;

static const NicId nic_ids[] = {
    {0x100E, K_8254X, "Intel 82540EM"},     {0x100F, K_8254X, "Intel 82545EM"},    {0x1004, K_8254X, "Intel 82543GC"},
    {0x1026, K_8254X, "Intel 82545GM"},     {0x1076, K_8254X, "Intel 82541GI"},    {0x107C, K_8254X, "Intel 82541PI"},
    {0x10D3, K_82574, "Intel 82574L"},      {0x10F6, K_82574, "Intel 82574LA"},
    {0x153A, K_PCH_LPT, "Intel I217-LM"},   {0x153B, K_PCH_LPT, "Intel I217-V"},
    {0x155A, K_PCH_LPT, "Intel I218-LM"},   {0x1559, K_PCH_LPT, "Intel I218-V"},
    {0x15A0, K_PCH_LPT, "Intel I218-LM"},   {0x15A1, K_PCH_LPT, "Intel I218-V"},
    {0x15A2, K_PCH_LPT, "Intel I218-LM"},   {0x15A3, K_PCH_LPT, "Intel I218-V"},
    {0x156F, K_PCH_SPT, "Intel I219-LM"},   {0x1570, K_PCH_SPT, "Intel I219-V"},
    {0x15B7, K_PCH_SPT, "Intel I219-LM"},   {0x15B8, K_PCH_SPT, "Intel I219-V"},   {0x15B9, K_PCH_SPT, "Intel I219-LM"},
    {0x15D7, K_PCH_SPT, "Intel I219-LM"},   {0x15D8, K_PCH_SPT, "Intel I219-V"},   {0x15E3, K_PCH_SPT, "Intel I219-LM"},
    {0x15D6, K_PCH_SPT, "Intel I219-V"},    {0x15BD, K_PCH_SPT, "Intel I219-LM"},  {0x15BE, K_PCH_SPT, "Intel I219-V"},
    {0x15BB, K_PCH_SPT, "Intel I219-LM"},   {0x15BC, K_PCH_SPT, "Intel I219-V"},   {0x15DF, K_PCH_SPT, "Intel I219-LM"},
    {0x15E0, K_PCH_SPT, "Intel I219-V"},    {0x15E1, K_PCH_SPT, "Intel I219-LM"},  {0x15E2, K_PCH_SPT, "Intel I219-V"},
    {0x0D4E, K_PCH_SPT, "Intel I219-LM"},   {0x0D4F, K_PCH_SPT, "Intel I219-V"},   {0x0D4C, K_PCH_SPT, "Intel I219-LM"},
    {0x0D4D, K_PCH_SPT, "Intel I219-V"},    {0x0D53, K_PCH_SPT, "Intel I219-LM"},  {0x0D55, K_PCH_SPT, "Intel I219-V"},
    {0x15FB, K_PCH_SPT, "Intel I219-LM"},   {0x15FC, K_PCH_SPT, "Intel I219-V"},   {0x15F9, K_PCH_SPT, "Intel I219-LM"},
    {0x15FA, K_PCH_SPT, "Intel I219-V"},    {0x15F4, K_PCH_SPT, "Intel I219-LM"},  {0x15F5, K_PCH_SPT, "Intel I219-V"},
    {0x1A1E, K_PCH_SPT, "Intel I219-LM"},   {0x1A1F, K_PCH_SPT, "Intel I219-V"},   {0x1A1C, K_PCH_SPT, "Intel I219-LM"},
    {0x1A1D, K_PCH_SPT, "Intel I219-V"},    {0x0DC5, K_PCH_SPT, "Intel I219-LM"},  {0x0DC6, K_PCH_SPT, "Intel I219-V"},
    {0x0DC7, K_PCH_SPT, "Intel I219-LM"},   {0x0DC8, K_PCH_SPT, "Intel I219-V"},   {0x550A, K_PCH_SPT, "Intel I219-LM"},
    {0x550B, K_PCH_SPT, "Intel I219-V"},    {0x550C, K_PCH_SPT, "Intel I219-LM"},  {0x550D, K_PCH_SPT, "Intel I219-V"},
    {0x550E, K_PCH_SPT, "Intel I219-LM"},   {0x550F, K_PCH_SPT, "Intel I219-V"},   {0x5510, K_PCH_SPT, "Intel I219-LM"},
    {0x5511, K_PCH_SPT, "Intel I219-V"},    {0x57A0, K_PCH_SPT, "Intel I219-LM"},  {0x57A1, K_PCH_SPT, "Intel I219-V"},
    {0x57B3, K_PCH_SPT, "Intel I219-LM"},   {0x57B4, K_PCH_SPT, "Intel I219-V"},
};
#define NIC_IDS ((int)(sizeof(nic_ids) / sizeof(nic_ids[0])))

/* Register */
#define REG_CTRL      0x0000
#define REG_STATUS    0x0008
#define REG_EERD      0x0014
#define REG_CTRL_EXT  0x0018
#define REG_MDIC      0x0020
#define REG_FEXTNVM3  0x003C
#define REG_ICR       0x00C0
#define REG_IMC       0x00D8
#define REG_RCTL      0x0100
#define REG_TCTL      0x0400
#define REG_TIPG      0x0410
#define REG_EXTCNF    0x0F00
#define REG_IOSFPC    0x0F28
#define REG_RDBAL     0x2800
#define REG_RDBAH     0x2804
#define REG_RDLEN     0x2808
#define REG_RDH       0x2810
#define REG_RDT       0x2818
#define REG_TDBAL     0x3800
#define REG_TDBAH     0x3804
#define REG_TDLEN     0x3808
#define REG_TDH       0x3810
#define REG_TDT       0x3818
#define REG_TXDCTL    0x3828
#define REG_TARC0     0x3840
#define REG_MTA       0x5200
#define REG_RAL0      0x5400
#define REG_RAH0      0x5404
#define REG_H2ME      0x5B50
#define REG_FWSM      0x5B54

#define CTRL_FD        (1u << 0)
#define CTRL_LRST      (1u << 3)
#define CTRL_ASDE      (1u << 5)
#define CTRL_SLU       (1u << 6)
#define CTRL_ILOS      (1u << 7)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE    (1u << 17)
#define CTRL_RST       (1u << 26)
#define CTRL_VME       (1u << 30)
#define CTRL_PHY_RST   (1u << 31)
#define STATUS_FD      (1u << 0)
#define STATUS_LU      (1u << 1)
#define CTRL_EXT_LPCD        (1u << 2)
#define CTRL_EXT_FORCE_SMBUS (1u << 11)
#define RCTL_EN        (1u << 1)
#define RCTL_BAM       (1u << 15)
#define RCTL_SECRC     (1u << 26)
#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)
#define TCTL_RTLC      (1u << 24)
#define EXTCNF_SWFLAG  (1u << 5)
#define FWSM_FW_VALID      (1u << 15)
#define FWSM_ULP_CFG_DONE  (1u << 10)
#define H2ME_ULP           (1u << 11)
#define H2ME_ENFORCE       (1u << 12)
#define MDIC_READY     (1u << 28)
#define MDIC_ERROR     (1u << 30)

#define RX_COUNT 128
#define TX_COUNT 64
#define BUF_SIZE 2048

typedef struct {
    uint64_t addr;
    uint16_t length, csum;
    uint8_t  status, errors;
    uint16_t special;
} __attribute__((packed)) RxDesc;

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed)) TxDesc;

#define TXCMD_EOP  0x01
#define TXCMD_IFCS 0x02
#define TXCMD_RS   0x08
#define DESC_DD    0x01
#define RXSTA_EOP  0x02

typedef struct {
    volatile uint8_t *regs;
    int      kind;
    uint32_t phy_addr;
    volatile RxDesc *rx;
    volatile TxDesc *tx;
    uint8_t *rx_buf, *tx_buf;
    uint32_t rx_next, tx_next;
} Nic;

static inline uint32_t rd(Nic *n, uint32_t off) { return *(volatile uint32_t *)(n->regs + off); }
static inline void wr(Nic *n, uint32_t off, uint32_t v) { *(volatile uint32_t *)(n->regs + off) = v; }
static inline void flush(Nic *n) { (void)rd(n, REG_STATUS); }
static inline void barrier(void) { __asm__ __volatile__("mfence" : : : "memory"); }

static int is_pch(const Nic *n) { return n->kind == K_PCH_LPT || n->kind == K_PCH_SPT; }

/* ---------- PHY (ueber MDIC); beim PCH mit Software-Flag, weil sich Treiber und Management Engine den PHY teilen ---------- */

static int swflag_get(Nic *n)
{
    if (!is_pch(n))
        return 0;
    if (!WAIT_UNTIL(!(rd(n, REG_EXTCNF) & EXTCNF_SWFLAG), 1000))
        kprintf("e1000: PHY-Sperre (SWFLAG) wird nicht frei, EXTCNF_CTRL %#x\n", rd(n, REG_EXTCNF));
    wr(n, REG_EXTCNF, rd(n, REG_EXTCNF) | EXTCNF_SWFLAG);
    if (!WAIT_UNTIL(rd(n, REG_EXTCNF) & EXTCNF_SWFLAG, 1000)) {
        kprintf("e1000: PHY-Sperre (SWFLAG) nicht erhalten\n");
        return -1;
    }
    return 0;
}

static void swflag_put(Nic *n)
{
    if (is_pch(n))
        wr(n, REG_EXTCNF, rd(n, REG_EXTCNF) & ~EXTCNF_SWFLAG);
}

static int mdic(Nic *n, int write, uint32_t reg, uint16_t *val)
{
    uint32_t cmd = ((reg & 0x1F) << 16) | (n->phy_addr << 21) | (write ? (1u << 26) | *val : (2u << 26));
    wr(n, REG_MDIC, cmd);
    if (!WAIT_UNTIL(rd(n, REG_MDIC) & (MDIC_READY | MDIC_ERROR), 20))
        return -1;
    uint32_t r = rd(n, REG_MDIC);
    if ((r & MDIC_ERROR) || !(r & MDIC_READY))
        return -1;
    if (!write)
        *val = (uint16_t)r;
    return 0;
}

static int phy_read(Nic *n, uint32_t reg, uint16_t *val)
{
    if (swflag_get(n) != 0)
        return -1;
    int r = mdic(n, 0, reg, val);
    swflag_put(n);
    return r;
}

static int phy_write(Nic *n, uint32_t reg, uint16_t val)
{
    if (swflag_get(n) != 0)
        return -1;
    int r = mdic(n, 1, reg, &val);
    swflag_put(n);
    return r;
}

/* PHY-Kennung (Register 2 und 3); 0 = nicht erreichbar */
static uint32_t phy_id(Nic *n)
{
    uint16_t a = 0, b = 0;
    if (phy_read(n, 2, &a) != 0 || phy_read(n, 3, &b) != 0 || a == 0xFFFF || (a == 0 && b == 0))
        return 0;
    return ((uint32_t)a << 16) | b;
}

/* PCH: PHY durch Umschalten von LANPHYPC aus- und wieder einschalten (verlaesst dabei auch den ULP-Modus) */
static void toggle_lanphypc(Nic *n)
{
    uint32_t v = rd(n, REG_FEXTNVM3);
    v = (v & ~0x0C000000u) | 0x08000000u; /* PHY_CFG_COUNTER: 50 ms */
    wr(n, REG_FEXTNVM3, v);
    v = rd(n, REG_CTRL);
    v |= CTRL_LANPHYPC_OVERRIDE;
    v &= ~CTRL_LANPHYPC_VALUE;
    wr(n, REG_CTRL, v);
    flush(n);
    thread_sleep_ms(10);
    v &= ~CTRL_LANPHYPC_OVERRIDE;
    wr(n, REG_CTRL, v);
    flush(n);
    WAIT_UNTIL(rd(n, REG_CTRL_EXT) & CTRL_EXT_LPCD, 120);
    thread_sleep_ms(30);
}

/* PCH: PHY aus dem Stromsparmodus holen und erreichbar machen */
static void pch_wake_phy(Nic *n)
{
    uint32_t fwsm = rd(n, REG_FWSM);
    if (fwsm & FWSM_FW_VALID) { /* Management Engine vorhanden: sie soll ULP beenden */
        uint32_t h = rd(n, REG_H2ME);
        wr(n, REG_H2ME, (h & ~H2ME_ULP) | H2ME_ENFORCE);
        if (!WAIT_UNTIL(!(rd(n, REG_FWSM) & FWSM_ULP_CFG_DONE), 300))
            kprintf("e1000: ME beendet den ULP-Modus nicht (FWSM %#x)\n", rd(n, REG_FWSM));
        wr(n, REG_H2ME, rd(n, REG_H2ME) & ~H2ME_ENFORCE);
    }
    if (phy_id(n))
        goto ok;
    kprintf("e1000: PHY antwortet nicht, schalte ihn neu ein (LANPHYPC)\n");
    wr(n, REG_CTRL_EXT, rd(n, REG_CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
    toggle_lanphypc(n);
    wr(n, REG_CTRL_EXT, rd(n, REG_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
    thread_sleep_ms(50);
ok:
    wr(n, REG_CTRL_EXT, rd(n, REG_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
}

/* ---------- MAC-Adresse ---------- */

static int eeprom_read(Nic *n, uint32_t word, uint16_t *out)
{
    uint32_t start, done, shift;
    if (n->kind == K_8254X) {
        start = 1u;
        done = 1u << 4;
        shift = 8;
    } else {
        start = 1u;
        done = 1u << 1;
        shift = 2;
    }
    wr(n, REG_EERD, (word << shift) | start);
    if (!WAIT_UNTIL(rd(n, REG_EERD) & done, 20))
        return -1;
    *out = (uint16_t)(rd(n, REG_EERD) >> 16);
    return 0;
}

static void read_mac(Nic *n, uint8_t mac[6])
{
    uint32_t lo = rd(n, REG_RAL0), hi = rd(n, REG_RAH0);
    if ((hi & (1u << 31)) && (lo || (hi & 0xFFFF))) { /* von der Firmware bzw. aus dem NVM geladen */
        for (int i = 0; i < 4; i++)
            mac[i] = (uint8_t)(lo >> (8 * i));
        mac[4] = (uint8_t)hi;
        mac[5] = (uint8_t)(hi >> 8);
        return;
    }
    uint16_t w[3];
    if (!is_pch(n) && eeprom_read(n, 0, &w[0]) == 0 && eeprom_read(n, 1, &w[1]) == 0 && eeprom_read(n, 2, &w[2]) == 0) {
        for (int i = 0; i < 3; i++) {
            mac[2 * i] = (uint8_t)w[i];
            mac[2 * i + 1] = (uint8_t)(w[i] >> 8);
        }
        return;
    }
    /* keine Adresse gefunden: zufaellige, lokal verwaltete Adresse */
    uint64_t r = time_us() * 6364136223846793005ULL + 1442695040888963407ULL;
    for (int i = 0; i < 6; i++)
        mac[i] = (uint8_t)(r >> (8 * i + 8));
    mac[0] = (mac[0] & 0xFE) | 0x02;
    kprintf("e1000: keine MAC-Adresse gespeichert, benutze eine zufaellige\n");
}

/* ---------- Senden / Empfangen ---------- */

static int nic_send(NetDev *d, const void *frame, uint32_t len)
{
    Nic *n = d->priv;
    if (len > BUF_SIZE)
        return -1;
    volatile TxDesc *t = &n->tx[n->tx_next];
    if (t->cmd && !(t->status & DESC_DD) && !WAIT_UNTIL(t->status & DESC_DD, 20))
        return -1; /* Ring voll */
    uint8_t *buf = n->tx_buf + (uint64_t)n->tx_next * BUF_SIZE;
    memcpy(buf, frame, len);
    t->addr = (uint64_t)buf;
    t->length = (uint16_t)len;
    t->cso = 0;
    t->css = 0;
    t->special = 0;
    t->status = 0;
    t->cmd = TXCMD_EOP | TXCMD_IFCS | TXCMD_RS;
    n->tx_next = (n->tx_next + 1) % TX_COUNT;
    barrier();
    wr(n, REG_TDT, n->tx_next);
    return 0;
}

static int nic_recv(NetDev *d, void *out, uint32_t max)
{
    Nic *n = d->priv;
    for (;;) {
        volatile RxDesc *r = &n->rx[n->rx_next];
        uint8_t st = r->status;
        if (!(st & DESC_DD))
            return 0;
        barrier();
        uint32_t len = r->length;
        int ok = (st & RXSTA_EOP) && !r->errors && len >= 14 && len <= max;
        if (ok)
            memcpy(out, n->rx_buf + (uint64_t)n->rx_next * BUF_SIZE, len);
        r->status = 0;
        uint32_t done = n->rx_next;
        n->rx_next = (n->rx_next + 1) % RX_COUNT;
        barrier();
        wr(n, REG_RDT, done); /* Deskriptor zurueck an die Karte */
        if (ok)
            return (int)len;
    }
}

static int nic_link(NetDev *d, uint32_t *mbps, int *fd)
{
    Nic *n = d->priv;
    uint32_t s = rd(n, REG_STATUS);
    uint32_t sp = (s >> 6) & 3;
    if (mbps)
        *mbps = sp == 0 ? 10 : sp == 1 ? 100 : 1000;
    if (fd)
        *fd = (s & STATUS_FD) != 0;
    return (s & STATUS_LU) != 0;
}

/* ---------- Initialisierung ---------- */

static void probe_one(const PciDevice *pci, const NicId *id)
{
    uint64_t bar0 = pci_bar_mem(pci, 0);
    kprintf("e1000: %s (%04x:%04x) an %02x:%02x.%u, BAR0 %#lx\n", id->name, pci->vendor, pci->device, pci->bus, pci->dev,
            pci->fn, (unsigned long)bar0);
    if (!bar0 || paging_map_mmio(bar0, 0x20000) != 0) {
        kprintf("e1000: Register nicht erreichbar\n");
        return;
    }
    pci_enable(pci, 0, 1, 1);
    pci_set_driver(pci, "e1000");

    Nic *n = kcalloc(1, sizeof(*n));
    if (!n)
        return;
    n->regs = (volatile uint8_t *)bar0;
    n->kind = id->kind;
    n->phy_addr = is_pch(n) ? 2 : 1;

    kprintf("e1000: CTRL %#x STATUS %#x CTRL_EXT %#x%s", rd(n, REG_CTRL), rd(n, REG_STATUS), rd(n, REG_CTRL_EXT),
            is_pch(n) ? "" : "\n");
    if (is_pch(n))
        kprintf(" FWSM %#x EXTCNF %#x\n", rd(n, REG_FWSM), rd(n, REG_EXTCNF));

    /* 1. Interrupts aus, Empfang und Senden anhalten (die Firmware kann die Karte benutzt haben) */
    wr(n, REG_IMC, 0xFFFFFFFF);
    wr(n, REG_RCTL, 0);
    wr(n, REG_TCTL, TCTL_PSP);
    flush(n);
    thread_sleep_ms(10);

    /* 2. Reset (nicht beim PCH, siehe oben) */
    if (!is_pch(n) || cmdline_has("e1000reset")) {
        wr(n, REG_CTRL, rd(n, REG_CTRL) | CTRL_RST);
        thread_sleep_ms(20);
        if (!WAIT_UNTIL(!(rd(n, REG_CTRL) & CTRL_RST), 500))
            kprintf("e1000: Reset endet nicht\n");
        wr(n, REG_IMC, 0xFFFFFFFF);
        (void)rd(n, REG_ICR);
    }
    if (is_pch(n))
        pch_wake_phy(n);

    /* 3. Verbindung: Link hochsetzen, PHY einschalten und Autonegotiation starten */
    uint32_t ctrl = rd(n, REG_CTRL);
    ctrl |= CTRL_SLU;
    if (n->kind == K_8254X)
        ctrl |= CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_ILOS | CTRL_VME | CTRL_PHY_RST | CTRL_LANPHYPC_OVERRIDE);
    wr(n, REG_CTRL, ctrl);
    uint32_t pid = phy_id(n);
    uint16_t bmcr = 0;
    if (phy_read(n, 0, &bmcr) == 0) {
        kprintf("e1000: PHY %#x an Adresse %u, BMCR %#x\n", pid, n->phy_addr, bmcr);
        if (bmcr & ((1u << 11) | (1u << 10))) { /* ausgeschaltet oder isoliert */
            bmcr &= (uint16_t)~((1u << 11) | (1u << 10));
            bmcr |= (1u << 12) | (1u << 9); /* Autonegotiation an und neu starten */
            phy_write(n, 0, bmcr);
            kprintf("e1000: PHY eingeschaltet\n");
        }
    } else {
        kprintf("e1000: PHY nicht erreichbar (MDIC %#x)\n", rd(n, REG_MDIC));
    }

    /* 4. MAC-Adresse und Filter */
    NetDev d;
    memset(&d, 0, sizeof(d));
    read_mac(n, d.mac);
    wr(n, REG_RAL0, d.mac[0] | ((uint32_t)d.mac[1] << 8) | ((uint32_t)d.mac[2] << 16) | ((uint32_t)d.mac[3] << 24));
    wr(n, REG_RAH0, d.mac[4] | ((uint32_t)d.mac[5] << 8) | (1u << 31));
    for (int i = 0; i < 128; i++)
        wr(n, REG_MTA + 4 * i, 0);

    /* 5. Ringe */
    n->rx = blk_dma_alloc(sizeof(RxDesc) * RX_COUNT);
    n->tx = blk_dma_alloc(sizeof(TxDesc) * TX_COUNT);
    n->rx_buf = blk_dma_alloc((uint64_t)RX_COUNT * BUF_SIZE);
    n->tx_buf = blk_dma_alloc((uint64_t)TX_COUNT * BUF_SIZE);
    if (!n->rx || !n->tx || !n->rx_buf || !n->tx_buf) {
        kprintf("e1000: kein Speicher fuer die Ringe\n");
        return;
    }
    for (int i = 0; i < RX_COUNT; i++)
        n->rx[i].addr = (uint64_t)(n->rx_buf + (uint64_t)i * BUF_SIZE);
    wr(n, REG_RDBAL, (uint32_t)(uint64_t)n->rx);
    wr(n, REG_RDBAH, (uint32_t)((uint64_t)n->rx >> 32));
    wr(n, REG_RDLEN, sizeof(RxDesc) * RX_COUNT);
    wr(n, REG_RDH, 0);
    wr(n, REG_RDT, RX_COUNT - 1);
    n->rx_next = 0;

    wr(n, REG_TDBAL, (uint32_t)(uint64_t)n->tx);
    wr(n, REG_TDBAH, (uint32_t)((uint64_t)n->tx >> 32));
    wr(n, REG_TDLEN, sizeof(TxDesc) * TX_COUNT);
    wr(n, REG_TDH, 0);
    wr(n, REG_TDT, 0);
    n->tx_next = 0;

    if (n->kind == K_8254X) {
        wr(n, REG_TIPG, 0x0060200A);
    } else {
        wr(n, REG_TIPG, 0x00602008);
        uint32_t txdctl = rd(n, REG_TXDCTL);
        txdctl = (txdctl & ~0x003F0000u) | 0x01010000u; /* WTHRESH = 1, GRAN = Deskriptoren */
        txdctl = (txdctl & ~0x3Fu) | 0x1Fu;             /* PTHRESH */
        if (n->kind == K_82574)
            txdctl |= 1u << 22;
        wr(n, REG_TXDCTL, txdctl);
    }
    if (n->kind == K_PCH_SPT) { /* Errata der I219 (wie Linux e1000e): sonst kann das Senden haengen bleiben */
        wr(n, REG_IOSFPC, rd(n, REG_IOSFPC) | 0x00010000);
        uint32_t tarc = rd(n, REG_TARC0);
        tarc = (tarc & ~0x30000000u) | 0x20000000u;
        wr(n, REG_TARC0, tarc);
    }
    wr(n, REG_TCTL, TCTL_EN | TCTL_PSP | (15u << 4) | (63u << 12) | (n->kind == K_8254X ? 0 : TCTL_RTLC));
    wr(n, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC); /* Puffer 2048 Byte */
    flush(n);

    for (int i = 0; i < 39 && id->name[i]; i++)
        d.model[i] = id->name[i];
    d.send = nic_send;
    d.recv = nic_recv;
    d.link = nic_link;
    d.priv = n;
    uint32_t mbps;
    int fd;
    int up = nic_link(&d, &mbps, &fd);
    kprintf("e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x, Verbindung %s\n", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4],
            d.mac[5], up ? "steht" : "(noch) nicht da");
    net_register(&d);
}

void e1000_probe(void)
{
    PciDevice pci;
    for (unsigned i = 0; pci_find_class(0x02, 0x00, -1, i, &pci) == 0; i++) {
        if (pci.vendor != 0x8086) {
            kprintf("net: Netzwerkkarte %04x:%04x an %02x:%02x.%u: kein Treiber\n", pci.vendor, pci.device, pci.bus, pci.dev,
                    pci.fn);
            continue;
        }
        const NicId *id = 0;
        for (int k = 0; k < NIC_IDS; k++)
            if (nic_ids[k].id == pci.device)
                id = &nic_ids[k];
        if (!id) {
            kprintf("net: Intel-Netzwerkkarte 8086:%04x an %02x:%02x.%u ist (noch) nicht bekannt\n", pci.device, pci.bus,
                    pci.dev, pci.fn);
            continue;
        }
        probe_one(&pci, id);
    }
}
