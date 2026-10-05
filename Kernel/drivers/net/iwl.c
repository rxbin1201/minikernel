/* Intel WLAN der Familie 22000 (AX200, PCI 8086:2723), Stufe 1: Karte finden, BAR0 einblenden, Kennungen aus den
 * CSR-Registern lesen (nur lesen - die Karte bleibt, wie die Firmware des PCs sie hinterlassen hat) und die Firmware
 * aus der initrd zerlegen (/firmware/iwlwifi-cc-a0-NN.ucode aus linux-firmware, die hoechste vorhandene Nummer).
 *
 * Firmware-Datei (TLV-Format wie bei Linux, iwl-drv.c): 4 Byte 0, "IWL\n", 64 Byte Text, Version, Build, 8 Byte, dann
 * Eintraege (Typ, Laenge, Daten auf 4 Byte aufgefuellt). Die Laufzeit-Firmware steht in den Abschnitten SEC_RT
 * (Typ 19: Zieladresse, Daten) - erst die des LMAC (CPU 1), nach der Trennmarke 0xFFFFCCCC die des UMAC (CPU 2),
 * nach 0xAAAABBBB die fuer das Paging (Seiten, die die Firmware bei Bedarf aus dem RAM holt). */

#include "drivers/net/iwl.h"
#include "drivers/pci.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/paging.h"
#include "arch/x86_64/apic.h"
#include "core/sched.h"
#include "mm/pmm.h"

#define CSR_HW_IF_CONFIG_REG 0x000
#define CSR_GP_CNTRL         0x024
#define CSR_HW_REV           0x028
#define CSR_HW_RF_ID         0x09C
#define PRPH_RADDR           0x448 /* Peripherie-Register lesen: Adresse (24 Bit, mit 3 << 24), dann Wert aus PRPH_RDAT */
#define PRPH_RDAT            0x450
/* Bits in CSR_GP_CNTRL */
#define GP_MAC_CLOCK_READY   (1u << 0)
#define GP_INIT_DONE         (1u << 2)
#define GP_MAC_ACCESS_REQ    (1u << 3)
#define GP_GOING_TO_SLEEP    (1u << 4)
#define CNVI_AUX_MISC_CHIP   0xA200B0
#define CSR_RESET            0x020
#define RESET_SW             (1u << 7)
/* Bits in CSR_HW_IF_CONFIG_REG */
#define HWIF_NIC_READY       (1u << 22) /* setzt der Treiber; bleibt es stehen, ist die Karte bereit */
#define HWIF_PREPARE_DONE    (1u << 25)
#define HWIF_PREPARE         (1u << 27) /* sonst: vorbereiten lassen, bis PREPARE_DONE weg ist, dann noch einmal */
#define UREG_UCODE_LOAD_STATUS 0xA05C40 /* Peripherie-Register jeder Karte der Familie: Ladestatus, Status CPU 1 */
#define SB_CPU_1_STATUS        0xA01E30
#define BAD_PATTERN(v)       (((v) & 0xFFFFFFF0u) == 0xA5A5A5A0u) /* Antwort auf ein nicht erreichbares Register */
#define HWIF_HAP_WAKE_L1A    (1u << 19)
#define CSR_GIO_CHICKEN_BITS 0x100
#define GIO_L1A_NO_L0S_RX    (1u << 23)
#define GIO_DIS_L0S_TIMER    (1u << 29)
#define CSR_DBG_HPET_MEM     0x240

enum { TLV_SEC_RT = 19, TLV_NUM_OF_CPU = 27, TLV_API_CHANGES_SET = 29, TLV_ENABLED_CAPABILITIES = 30,
       TLV_N_SCAN_CHANNELS = 31, TLV_PAGING = 32, TLV_FW_VERSION = 36 };
#define SEP_CPU1_CPU2 0xFFFFCCCCu
#define SEP_PAGING    0xAAAABBBBu

static WlanInfo         info;
/* Laufzeit-Abschnitte der Firmware (zeigen in die initrd): 0 LMAC, 1 UMAC, 2 Paging; Daten ohne die Zieladresse */
#define MAX_SEC 64
static struct {
    const uint8_t *data;
    uint32_t       len;
} sec[3][MAX_SEC];
static int nsec[3];
static volatile uint8_t *regs;

static uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

static void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(regs + off) = v;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Firmware in der initrd suchen und zerlegen */
static void parse_fw(void)
{
    const VfsNode *best = 0;
    uint32_t best_api = 0;
    for (unsigned i = 0;; i++) {
        const VfsNode *n = vfs_readdir("/firmware", i);
        if (!n)
            break;
        const char *b = vfs_basename(n->path);
        if (n->is_dir || strncmp(b, "iwlwifi-cc-a0-", 14) != 0)
            continue;
        uint32_t api = 0;
        for (const char *p = b + 14; *p >= '0' && *p <= '9'; p++)
            api = api * 10 + (uint32_t)(*p - '0');
        if (!best || api > best_api) {
            best = n;
            best_api = api;
        }
    }
    if (!best) {
        ksnprintf(info.state, sizeof(info.state), "keine Firmware in /firmware (iwlwifi-cc-a0-*.ucode)");
        return;
    }
    ksnprintf(info.fw_name, sizeof(info.fw_name), "%s", vfs_basename(best->path));
    const uint8_t *d = best->data;
    uint64_t len = best->size;
    if (len < 88 || le32(d) != 0 || le32(d + 4) != 0x0A4C5749) { /* "IWL\n" */
        ksnprintf(info.state, sizeof(info.state), "%s: kein TLV-Format", info.fw_name);
        return;
    }
    memcpy(info.fw_human, d + 8, 63);
    info.fw_human[63] = 0;
    info.fw_api = le32(d + 72) & 0xFF;
    int part = 0; /* 0 LMAC, 1 UMAC, 2 Paging */
    for (uint64_t off = 88; off + 8 <= len;) {
        uint32_t type = le32(d + off), l = le32(d + off + 4);
        const uint8_t *v = d + off + 8;
        if (off + 8 + l > len) {
            ksnprintf(info.state, sizeof(info.state), "%s: Eintrag bei %lu zu lang", info.fw_name, (unsigned long)off);
            return;
        }
        if (type == TLV_SEC_RT && l >= 4) {
            uint32_t addr = le32(v);
            if (addr == SEP_CPU1_CPU2)
                part = 1;
            else if (addr == SEP_PAGING)
                part = 2;
            else {
                *(part == 0 ? &info.fw_lmac : part == 1 ? &info.fw_umac : &info.fw_paging) += 1;
                info.fw_bytes += l - 4;
                if (nsec[part] < MAX_SEC) { /* in den Puffer kommen nur die Daten (die Zieladresse kennt die Karte) */
                    sec[part][nsec[part]].data = v + 4;
                    sec[part][nsec[part]].len = l - 4;
                    nsec[part]++;
                }
            }
        } else if (type == TLV_NUM_OF_CPU && l >= 4) {
            info.fw_cpus = le32(v);
        } else if (type == TLV_PAGING && l >= 4) {
            info.fw_paging_bytes = le32(v);
        } else if (type == TLV_N_SCAN_CHANNELS && l >= 4) {
            info.fw_scan_channels = le32(v);
        } else if (type == TLV_ENABLED_CAPABILITIES && l >= 8) {
            for (uint32_t b = 0; b < 32; b++)
                info.fw_capa += (le32(v + 4) >> b) & 1;
        } else if (type == TLV_API_CHANGES_SET && l >= 8) {
            for (uint32_t b = 0; b < 32; b++)
                info.fw_api_flags += (le32(v + 4) >> b) & 1;
        } else if (type == TLV_FW_VERSION && l >= 12) {
            info.fw_major = le32(v);
            info.fw_minor = le32(v + 4);
            info.fw_local = le32(v + 8);
        }
        off += 8 + ((l + 3) & ~3u);
    }
    info.fw_found = info.fw_lmac && info.fw_umac;
    if (!info.fw_found)
        ksnprintf(info.state, sizeof(info.state), "%s: keine Laufzeit-Abschnitte", info.fw_name);
}

void iwl_probe(void)
{
    parse_fw();
    if (info.fw_found)
        kprintf("iwl: Firmware %s (%s): %u+%u Abschnitte, %u Paging, %u KiB\n", info.fw_name, info.fw_human, info.fw_lmac,
                info.fw_umac, info.fw_paging, info.fw_bytes / 1024);
    PciDevice pci;
    if (pci_find(0x8086, 0x2723, &pci) != 0) {
        if (!info.state[0])
            ksnprintf(info.state, sizeof(info.state), "keine Intel AX200 (8086:2723) gefunden");
        return;
    }
    info.present = 1;
    info.bus = pci.bus;
    info.dev = pci.dev;
    info.fn = pci.fn;
    info.vendor = pci.vendor;
    info.device = pci.device;
    info.sub_vendor = pci.sub_vendor;
    info.sub_device = pci.sub_device;
    info.bar = pci_bar_mem(&pci, 0);
    kprintf("iwl: Intel AX200 (8086:2723, Subsystem %04x:%04x) an %02x:%02x.%u, BAR0 %#lx\n", pci.sub_vendor,
            pci.sub_device, pci.bus, pci.dev, pci.fn, (unsigned long)info.bar);
    if (!info.bar || paging_map_mmio(info.bar, 0x4000) != 0) {
        ksnprintf(info.state, sizeof(info.state), "Register nicht erreichbar");
        return;
    }
    pci_enable(&pci, 0, 1, 1);
    pci_set_driver(&pci, "iwl");
    regs = (volatile uint8_t *)info.bar;
    info.hw_if_config = rd(CSR_HW_IF_CONFIG_REG);
    info.gp_cntrl = rd(CSR_GP_CNTRL);
    info.hw_rev = rd(CSR_HW_REV);
    info.rf_id = rd(CSR_HW_RF_ID);
    kprintf("iwl: HW_REV %#x (Typ %#x, Schritt %u), RF_ID %#x, GP_CNTRL %#x, HW_IF_CONFIG %#x\n", info.hw_rev,
            (info.hw_rev >> 4) & 0xFFF, (info.hw_rev >> 2) & 3, info.rf_id, info.gp_cntrl, info.hw_if_config);
    if (!info.state[0])
        ksnprintf(info.state, sizeof(info.state), "erkannt (Stufe 1: noch keine Firmware geladen)");
}

/* Stufe 2a: die Karte meldet "Initialisierung fertig" (INIT_DONE) und bekommt ihren Takt; dann bittet der Treiber um
 * Zugriff auf die inneren Register (MAC_ACCESS_REQ) und wartet, bis der Takt steht und die Karte nicht gerade
 * einschlaeft. Mit dem Zugriff liest er ein Peripherie-Register (Kennung des CNVi) und gibt den Zugriff wieder ab.
 * Es wird nichts geladen und nichts zurueckgesetzt. */
int iwl_wake_test(void)
{
    if (!info.present || !regs)
        return -1;
    info.wake_done = 1;
    info.wake_clock = info.wake_access = 0;
    info.cnvi_id = info.prph_load = info.prph_cpu1 = 0;
    uint64_t t0 = time_us();
    /* 1. Bereitschaft: NIC_READY setzen - bleibt es stehen, ist die Karte bereit. Sonst PREPARE setzen, warten, bis
     *    die Karte PREPARE_DONE loescht, und NIC_READY noch einmal versuchen */
    wr(CSR_HW_IF_CONFIG_REG, rd(CSR_HW_IF_CONFIG_REG) | HWIF_NIC_READY);
    info.wake_ready = WAIT_UNTIL(rd(CSR_HW_IF_CONFIG_REG) & HWIF_NIC_READY, 50);
    if (!info.wake_ready) {
        wr(CSR_HW_IF_CONFIG_REG, rd(CSR_HW_IF_CONFIG_REG) | HWIF_PREPARE);
        WAIT_UNTIL(!(rd(CSR_HW_IF_CONFIG_REG) & HWIF_PREPARE_DONE), 150);
        wr(CSR_HW_IF_CONFIG_REG, rd(CSR_HW_IF_CONFIG_REG) | HWIF_NIC_READY);
        info.wake_ready = WAIT_UNTIL(rd(CSR_HW_IF_CONFIG_REG) & HWIF_NIC_READY, 50) ? 2 : 0;
    }
    info.hwif_after = rd(CSR_HW_IF_CONFIG_REG);
    kprintf("iwl: 1 bereit %s, HW_IF_CONFIG %#x, GP_CNTRL %#x\n", info.wake_ready ? "ja" : "NEIN", info.hwif_after,
            rd(CSR_GP_CNTRL));
    /* 2. per Software zuruecksetzen und kurz warten */
    wr(CSR_RESET, rd(CSR_RESET) | RESET_SW);
    thread_sleep_ms(6);
    WAIT_UNTIL(!BAD_PATTERN(rd(CSR_GP_CNTRL)), 100); /* waehrend des Neustarts antwortet die Karte nicht */
    kprintf("iwl: 2 nach Reset: RESET %#x, HW_IF_CONFIG %#x, GP_CNTRL %#x\n", rd(CSR_RESET), rd(CSR_HW_IF_CONFIG_REG),
            rd(CSR_GP_CNTRL));
    /* 3. Grundeinstellungen nach dem Reset: kein L0s beim Empfang in L1a, Weck-Bit fuer L1a, Hilfsregister
     *    (wie beim Einschalten durch einen Treiber), dann Initialisierung fertig und auf den Takt warten */
    wr(CSR_GIO_CHICKEN_BITS, rd(CSR_GIO_CHICKEN_BITS) | GIO_L1A_NO_L0S_RX | GIO_DIS_L0S_TIMER);
    wr(CSR_DBG_HPET_MEM, rd(CSR_DBG_HPET_MEM) | 0xFFFF0000u);
    wr(CSR_HW_IF_CONFIG_REG, rd(CSR_HW_IF_CONFIG_REG) | HWIF_HAP_WAKE_L1A);
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) | GP_INIT_DONE);
    uint64_t tc = time_us();
    info.wake_clock = WAIT_UNTIL(rd(CSR_GP_CNTRL) & GP_MAC_CLOCK_READY, 200);
    kprintf("iwl: 3 Takt %s nach %u us, GP_CNTRL %#x, GIO_CHICKEN %#x\n", info.wake_clock ? "bereit" : "NICHT bereit",
            (uint32_t)(time_us() - tc), rd(CSR_GP_CNTRL), rd(CSR_GIO_CHICKEN_BITS));
    if (!info.wake_clock) {
        info.gp_after = rd(CSR_GP_CNTRL);
        return -2;
    }
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) | GP_MAC_ACCESS_REQ);
    info.wake_access = WAIT_UNTIL((rd(CSR_GP_CNTRL) & (GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP)) == GP_MAC_CLOCK_READY, 25);
    info.wake_us = (uint32_t)(time_us() - t0);
    info.gp_after = rd(CSR_GP_CNTRL);
    if (info.wake_access) {
        wr(PRPH_RADDR, (CNVI_AUX_MISC_CHIP & 0xFFFFFF) | (3u << 24)); /* nur bei integriertem CNVi, nicht AX200 */
        info.cnvi_id = rd(PRPH_RDAT);
        wr(PRPH_RADDR, (UREG_UCODE_LOAD_STATUS & 0xFFFFFF) | (3u << 24));
        info.prph_load = rd(PRPH_RDAT);
        wr(PRPH_RADDR, (SB_CPU_1_STATUS & 0xFFFFFF) | (3u << 24));
        info.prph_cpu1 = rd(PRPH_RDAT);
    }
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
    kprintf("iwl: bereit %s (HW_IF_CONFIG %#x), Aufwecken %s nach %u us, GP_CNTRL %#x, LOAD_STATUS %#x, CPU1_STATUS "
            "%#x, CNVI %#x\n", info.wake_ready == 1 ? "ja" : info.wake_ready == 2 ? "ja (nach PREPARE)" : "NEIN",
            info.hwif_after, info.wake_access ? "ok" : "OHNE Zugriff", info.wake_us, info.gp_after, info.prph_load,
            info.prph_cpu1, info.cnvi_id);
    return info.wake_access ? 0 : -3;
}

int iwl_info(WlanInfo *out)
{
    *out = info;
    return 0;
}

/* ---------- Stufe 2b: Firmware laden ("wlan load") ----------
 *
 * Bei dieser Familie liest die Karte die Firmware selbst per DMA: der Treiber legt jeden Abschnitt in einen eigenen
 * Puffer, dazu Empfangsringe (freie Puffer, benutzte Puffer, Status) und eine Befehlswarteschlange, und beschreibt das
 * alles in der "Context Info". Deren Adresse kommt nach CSR_CTXT_INFO_BA; UREG_CPU_INIT_RUN startet die Karte. Die
 * erste Nachricht der Firmware ist ALIVE: sie landet im ersten Empfangspuffer, und die Firmware zaehlt im Status
 * (closed_rb_num) mit - das reicht zum Erkennen, ohne Interrupts. Alles im RAM ist 1:1 eingeblendet (physisch = virtuell). */

#define CSR_INT              0x008
#define CSR_FH_INT_STATUS    0x010
#define CSR_CTXT_INFO_BA     0x040
#define PRPH_WADDR           0x444
#define PRPH_WDAT            0x44C
#define UREG_CPU_INIT_RUN    0xA05C44
#define RFH_Q0_FRBDCB_WIDX_TRG 0x1C80 /* Schreibzeiger der freien Empfangspuffer (Queue 0) */

#define RX_RING   64   /* Empfangspuffer zu je 4 KiB */
#define CMD_RING  32   /* Befehlswarteschlange (TFDs zu 256 Byte) */
#define TFD_SIZE  256

typedef struct __attribute__((packed)) {
    uint16_t mac_id, version, size, reserved;
} CtxtVersion;

typedef struct {
    CtxtVersion version;
    uint32_t    control_flags, control_reserved;
    uint64_t    reserved0;
    uint64_t    free_rbd_addr, used_rbd_addr, status_wr_ptr; /* Empfang */
    uint64_t    cmd_queue_addr;                             /* Befehle */
    uint8_t     cmd_queue_size, cmd_reserved[7];
    uint32_t    reserved1[4];
    uint64_t    core_dump_addr;
    uint32_t    core_dump_size, dump_reserved;
    uint32_t    reserved2[4];
    uint64_t    pnvm_addr;
    uint32_t    pnvm_size, pnvm_reserved;
    uint32_t    reserved3[16];
    uint64_t    early_debug_addr;
    uint32_t    early_debug_size, edbg_reserved;
    uint32_t    reserved4[16];
    uint64_t    umac_img[64], lmac_img[64], virtual_img[64];
    uint32_t    reserved5[16];
} CtxtInfo;
_Static_assert(__builtin_offsetof(CtxtInfo, free_rbd_addr) == 24, "Context Info: Empfang");
_Static_assert(__builtin_offsetof(CtxtInfo, cmd_queue_addr) == 48, "Context Info: Befehle");
_Static_assert(__builtin_offsetof(CtxtInfo, umac_img) == 272, "Context Info: Abschnitte");
_Static_assert(sizeof(CtxtInfo) == 1872, "Context Info: Groesse");

#define CTXT_TFD_FORMAT_LONG (1u << 8)
#define CTXT_RB_CB_SIZE_POS  4
#define CTXT_RB_SIZE_POS     9
#define CTXT_RB_SIZE_4K      0x4

static void *dma(uint64_t bytes)
{
    uint64_t pages = (bytes + 4095) / 4096, f = pmm_alloc_frames(pages ? pages : 1);
    if (f)
        memset((void *)f, 0, pages * 4096);
    return (void *)f;
}

static void prph_write(uint32_t addr, uint32_t val)
{
    wr(PRPH_WADDR, (addr & 0xFFFFFF) | (3u << 24));
    wr(PRPH_WDAT, val);
}

static int ilog2(uint32_t x)
{
    int n = 0;
    while (x > 1) {
        x >>= 1;
        n++;
    }
    return n;
}

int iwl_load_fw(void)
{
    if (!info.present || !regs)
        return -1;
    if (!info.fw_found)
        return -4;
    info.load_done = 1;
    info.load_alive = 0;
    if (iwl_wake_test() == -2) /* Bereitschaft, Reset, Grundeinstellungen, Takt (Zugriff wird wieder abgegeben) */
        return -2;

    /* Firmware-Abschnitte, Empfang, Befehle, Context Info */
    CtxtInfo *ci = dma(sizeof(CtxtInfo));
    uint64_t *free_rbd = dma(RX_RING * 8);
    uint32_t *used_rbd = dma(RX_RING * 4);
    uint16_t *status = dma(64);
    uint8_t *cmdq = dma(CMD_RING * TFD_SIZE);
    static uint8_t *rb[RX_RING];
    if (!ci || !free_rbd || !used_rbd || !status || !cmdq)
        return -5;
    for (int i = 0; i < RX_RING; i++) {
        if (!rb[i] && !(rb[i] = dma(4096)))
            return -5;
        free_rbd[i] = (uint64_t)rb[i] | (uint64_t)(i + 1); /* Kennung (vid) 1..64 in den unteren Bits */
    }
    uint64_t *img[3] = {ci->lmac_img, ci->umac_img, ci->virtual_img};
    for (int p = 0; p < 3; p++)
        for (int i = 0; i < nsec[p]; i++) {
            uint8_t *b = dma(sec[p][i].len);
            if (!b)
                return -5;
            memcpy(b, sec[p][i].data, sec[p][i].len);
            img[p][i] = (uint64_t)b;
        }
    ci->version.mac_id = (uint16_t)rd(CSR_HW_REV);
    ci->version.version = 0;
    ci->version.size = (uint16_t)(sizeof(CtxtInfo) / 4);
    ci->control_flags = CTXT_TFD_FORMAT_LONG | ((uint32_t)ilog2(RX_RING) << CTXT_RB_CB_SIZE_POS) |
                        (CTXT_RB_SIZE_4K << CTXT_RB_SIZE_POS);
    ci->free_rbd_addr = (uint64_t)free_rbd;
    ci->used_rbd_addr = (uint64_t)used_rbd;
    ci->status_wr_ptr = (uint64_t)status;
    ci->cmd_queue_addr = (uint64_t)cmdq;
    ci->cmd_queue_size = (uint8_t)(ilog2(CMD_RING) - 3);

    /* Adresse uebergeben, freie Puffer melden, starten */
    wr(CSR_INT, 0xFFFFFFFFu);
    wr(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    wr(CSR_CTXT_INFO_BA, (uint32_t)(uint64_t)ci);
    wr(CSR_CTXT_INFO_BA + 4, (uint32_t)((uint64_t)ci >> 32));
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) | GP_MAC_ACCESS_REQ);
    int access = WAIT_UNTIL((rd(CSR_GP_CNTRL) & (GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP)) == GP_MAC_CLOCK_READY, 25);
    wr(RFH_Q0_FRBDCB_WIDX_TRG, RX_RING & ~7u);
    prph_write(UREG_CPU_INIT_RUN, 1);
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
    kprintf("iwl: Firmware gestartet (Context Info %#lx, %d+%d+%d Abschnitte, Zugriff %s)\n", (unsigned long)(uint64_t)ci,
            nsec[0], nsec[1], nsec[2], access ? "ok" : "NICHT bekommen");

    /* auf ALIVE warten: der Status zaehlt die gefuellten Empfangspuffer */
    uint64_t t0 = time_us();
    uint32_t last_int = 0;
    while (time_us() - t0 < 2000000) {
        uint32_t ci_int = rd(CSR_INT);
        if (ci_int != last_int) {
            kprintf("iwl:   nach %u ms: CSR_INT %#x, FH_INT %#x, Status %u\n", (uint32_t)((time_us() - t0) / 1000), ci_int,
                    rd(CSR_FH_INT_STATUS), *(volatile uint16_t *)status);
            last_int = ci_int;
        }
        if (*(volatile uint16_t *)status) {
            info.load_alive = 1;
            break;
        }
        thread_sleep_ms(1);
    }
    info.load_ms = (uint32_t)((time_us() - t0) / 1000);
    info.load_int = rd(CSR_INT);
    info.load_status = *(volatile uint16_t *)status;
    if (!info.load_alive) {
        kprintf("iwl: keine Nachricht der Firmware nach %u ms (CSR_INT %#x, GP_CNTRL %#x)\n", info.load_ms, info.load_int,
                rd(CSR_GP_CNTRL));
        return -3;
    }
    /* erste Nachricht: welcher Puffer (vid), dann Laenge, Befehl, Gruppe; bei ALIVE (1) der Status 0xCAFE */
    uint32_t vid = *(volatile uint32_t *)used_rbd & 0xFFF;
    const uint8_t *pkt = vid >= 1 && vid <= RX_RING ? rb[vid - 1] : rb[0];
    info.alive_len = le32(pkt) & 0x3FFF;
    info.alive_cmd = pkt[4];
    info.alive_group = pkt[5];
    info.alive_status = pkt[8] | pkt[9] << 8;
    kprintf("iwl: erste Nachricht nach %u ms: Puffer %u, Laenge %u, Befehl %#x, Gruppe %#x, Status %#x%s\n",
            info.load_ms, vid, info.alive_len, info.alive_cmd, info.alive_group, info.alive_status,
            info.alive_cmd == 1 && info.alive_status == 0xCAFE ? " - ALIVE, Firmware laeuft" : "");
    return 0;
}
