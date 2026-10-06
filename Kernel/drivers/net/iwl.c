/* Intel WLAN der Familie 22000 (AX200, PCI 8086:2723), Stufe 1: Karte finden, BAR0 einblenden, Kennungen aus den
 * CSR-Registern lesen (nur lesen - die Karte bleibt, wie die Firmware des PCs sie hinterlassen hat) und die Firmware
 * aus der initrd zerlegen (/firmware/iwlwifi-cc-a0-NN.ucode aus linux-firmware, die hoechste vorhandene Nummer).
 *
 * Firmware-Datei (TLV-Format wie bei Linux, iwl-drv.c): 4 Byte 0, "IWL\n", 64 Byte Text, Version, Build, 8 Byte, dann
 * Eintraege (Typ, Laenge, Daten auf 4 Byte aufgefuellt). Die Laufzeit-Firmware steht in den Abschnitten SEC_RT
 * (Typ 19: Zieladresse, Daten) - erst die des LMAC (CPU 1), nach der Trennmarke 0xFFFFCCCC die des UMAC (CPU 2),
 * nach 0xAAAABBBB die fuer das Paging (Seiten, die die Firmware bei Bedarf aus dem RAM holt). */

#include "drivers/net/iwl.h"
#include "drivers/net/iwl_internal.h"
#include "drivers/pci.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/paging.h"
#include "arch/x86_64/apic.h"
#include "core/sched.h"
#include "mm/pmm.h"
#include "arch/x86_64/acpi.h"

#define CSR_HW_IF_CONFIG_REG 0x000
#define CSR_GP_CNTRL         0x024
#define CSR_HW_REV           0x028
#define CSR_HW_RF_ID         0x09C
#define CSR_INT_             0x008
#define PRPH_RADDR           0x448 /* Peripherie-Register lesen: Adresse (24 Bit, mit 3 << 24), dann Wert aus PRPH_RDAT */
#define PRPH_RDAT            0x450
#define PRPH_WADDR_          0x444 /* Peripherie-Register schreiben: Adresse, dann Wert nach PRPH_WDAT */
#define PRPH_WDAT_           0x44C
/* Bits in CSR_GP_CNTRL */
#define GP_MAC_CLOCK_READY   (1u << 0)
#define GP_INIT_DONE         (1u << 2)
#define GP_MAC_ACCESS_REQ    (1u << 3)
#define GP_GOING_TO_SLEEP    (1u << 4)
#define CNVI_AUX_MISC_CHIP   0xA200B0 /* Kennung des CNVi (liest Linux auch bei der AX200) */
#define WFPM_CTRL_REG        0xA03030 /* Bit 31 ENABLE_WFPM: erst damit sind die Peripherie-Register erreichbar */
#define ENABLE_WFPM          (1u << 31)
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
Mutex                   iwl_op_lock = MUTEX_INIT, iwl_ring_lock = MUTEX_INIT;
static volatile int     rings_ok; /* Ringe eingerichtet und die Firmware laeuft: Empfang darf ausgewertet werden */
/* Laufzeit-Abschnitte der Firmware (zeigen in die initrd): 0 LMAC, 1 UMAC, 2 Paging; Daten ohne die Zieladresse */
#define MAX_SEC 64
static struct {
    const uint8_t *data;
    uint32_t       len;
} sec[3][MAX_SEC];
static int nsec[3];
static volatile uint8_t *regs;
static PciDevice pdev;

static uint32_t rd(uint32_t off)
{
    return *(volatile uint32_t *)(regs + off);
}

static void wr(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(regs + off) = v;
}

static uint32_t prph_rd(uint32_t addr)
{
    wr(PRPH_RADDR, (addr & 0xFFFFFF) | (3u << 24));
    return rd(PRPH_RDAT);
}

static void prph_wr(uint32_t addr, uint32_t val)
{
    wr(PRPH_WADDR_, (addr & 0xFFFFFF) | (3u << 24));
    wr(PRPH_WDAT_, val);
}

/* mit Zugriff: Peripherie-Register einschalten (WFPM); liefert WFPM_CTRL_REG vorher */
static uint32_t enable_wfpm(void)
{
    uint32_t v = prph_rd(WFPM_CTRL_REG);
    prph_wr(WFPM_CTRL_REG, v | ENABLE_WFPM);
    return v;
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

uint32_t iwl_rd(uint32_t off)
{
    return rd(off);
}

void iwl_wr(uint32_t off, uint32_t v)
{
    wr(off, v);
}

uint32_t iwl_le32(const uint8_t *p)
{
    return le32(p);
}

const WlanInfo *iwl_state(void)
{
    return &info;
}

/* Firmware in der initrd suchen und zerlegen */
static uint32_t fw_capa_words[4];

int iwl_fw_capa(unsigned bit)
{
    return bit < 128 && (fw_capa_words[bit / 32] >> (bit % 32)) & 1;
}

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
            if (le32(v) < 4)
                fw_capa_words[le32(v)] = le32(v + 4); /* Index, 32 Bits */
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
    info.bridges_fixed = (uint32_t)pci_enable_upstream(&pci);
    info.pci_cmd = pci_read32(&pci, 0x04);
    pci_set_driver(&pci, "iwl");
    pdev = pci;
    regs = (volatile uint8_t *)info.bar;
    info.hw_if_config = rd(CSR_HW_IF_CONFIG_REG);
    info.gp_cntrl = rd(CSR_GP_CNTRL);
    info.hw_rev = rd(CSR_HW_REV);
    info.rf_id = rd(CSR_HW_RF_ID);
    kprintf("iwl: PCI Kommando/Status %#x, %u Bruecke(n) fuer DMA freigeschaltet\n", info.pci_cmd, info.bridges_fixed);
    kprintf("iwl: HW_REV %#x (Typ %#x, Schritt %u), RF_ID %#x, GP_CNTRL %#x, HW_IF_CONFIG %#x\n", info.hw_rev,
            (info.hw_rev >> 4) & 0xFFF, (info.hw_rev >> 2) & 3, info.rf_id, info.gp_cntrl, info.hw_if_config);
    if (!info.state[0])
        ksnprintf(info.state, sizeof(info.state), "erkannt (Stufe 1: noch keine Firmware geladen)");
    if (info.fw_found)
        iwl_sta_register(); /* wlan0: Verbindung steht erst nach "wlan connect" */
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
    /* 4. inneren Prozessor aus dem Reset lassen (Bit 0 NEVO_RESET blieb nach dem Software-Reset stehen) */
    info.reset_before = rd(CSR_RESET);
    wr(CSR_RESET, 0);
    thread_sleep_ms(1);
    info.reset_after = rd(CSR_RESET);
    kprintf("iwl: 4 CSR_RESET %#x -> %#x\n", info.reset_before, info.reset_after);
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) | GP_MAC_ACCESS_REQ);
    info.wake_access = WAIT_UNTIL((rd(CSR_GP_CNTRL) & (GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP)) == GP_MAC_CLOCK_READY, 25);
    info.wake_us = (uint32_t)(time_us() - t0);
    info.gp_after = rd(CSR_GP_CNTRL);
    if (info.wake_access) {
        uint32_t int0 = rd(CSR_INT_);
        info.wfpm_before = enable_wfpm();
        info.wfpm_after = prph_rd(WFPM_CTRL_REG);
        info.cnvi_id = prph_rd(CNVI_AUX_MISC_CHIP);
        info.prph_load = prph_rd(UREG_UCODE_LOAD_STATUS);
        info.prph_cpu1 = prph_rd(SB_CPU_1_STATUS);
        info.int_after = rd(CSR_INT_);
        kprintf("iwl: 5 WFPM_CTRL %#x -> %#x, CSR_INT %#x -> %#x\n", info.wfpm_before, info.wfpm_after, int0,
                info.int_after);
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
#define CSR_UCODE_DRV_GP1_CLR  0x05C  /* Uebergabe Treiber/Firmware: Bits loeschen */
#define GP1_SW_RFKILL          (1u << 1)
#define GP1_CMD_BLOCKED        (1u << 2)
#define CSR_MAC_SHADOW_REG_CTRL 0x0A8 /* Schattenregister fuer die Schreibzeiger der Warteschlangen */

#define RX_RING   256  /* Empfangspuffer zu je 4 KiB: genug fuer ein A-MPDU (bis 64 Rahmen) zwischen zwei Abfragen */
#define CMD_RING  32   /* Befehlswarteschlange (TFDs zu 256 Byte) */
#define TFD_SIZE  256

typedef struct __attribute__((packed)) {
    uint16_t mac_id, version, size, reserved;
} CtxtVersion;

/* Layout wie struct iwl_context_info in Linux (iwl-context-info.h) */
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
    uint64_t    early_debug_addr;
    uint32_t    early_debug_size, edbg_reserved;
    uint64_t    pnvm_addr;
    uint32_t    pnvm_size, pnvm_reserved;
    uint32_t    reserved2[16];
    uint64_t    umac_img[64], lmac_img[64], virtual_img[64];
    uint32_t    reserved3[16];
} CtxtInfo;
_Static_assert(__builtin_offsetof(CtxtInfo, free_rbd_addr) == 24, "Context Info: Empfang");
_Static_assert(__builtin_offsetof(CtxtInfo, cmd_queue_addr) == 48, "Context Info: Befehle");
_Static_assert(__builtin_offsetof(CtxtInfo, core_dump_addr) == 80, "Context Info: Speicherauszug");
_Static_assert(__builtin_offsetof(CtxtInfo, umac_img) == 192, "Context Info: Abschnitte");
_Static_assert(sizeof(CtxtInfo) == 1792, "Context Info: Groesse");

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

void *iwl_dma(uint64_t bytes)
{
    return dma(bytes);
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

#define UREG_UMAC_CURRENT_PC  0xA05C18
#define UREG_LMAC1_CURRENT_PC 0xA05C1C
#define SB_CPU_2_STATUS       0xA01E34
#define CSR_UCODE_DRV_GP1     0x054
#define UMAG_SB_CPU_1_STATUS  0xA038C0
#define UMAG_SB_CPU_2_STATUS  0xA038C4

/* Zustand der Firmware ins Log: Ladestatus, Befehlszaehler beider Prozessoren, Status der CPUs, Uebergabe-Register */
static void fw_state(uint32_t ms)
{
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) | GP_MAC_ACCESS_REQ);
    if (!WAIT_UNTIL((rd(CSR_GP_CNTRL) & (GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP)) == GP_MAC_CLOCK_READY, 25)) {
        kprintf("iwl:   %u ms: kein Zugriff (GP_CNTRL %#x)\n", ms, rd(CSR_GP_CNTRL));
        return;
    }
    uint32_t load = prph_rd(UREG_UCODE_LOAD_STATUS), upc = prph_rd(UREG_UMAC_CURRENT_PC);
    uint32_t lpc = prph_rd(UREG_LMAC1_CURRENT_PC), c1 = prph_rd(SB_CPU_1_STATUS), c2 = prph_rd(SB_CPU_2_STATUS);
    uint32_t wfpm = prph_rd(WFPM_CTRL_REG);
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
    kprintf("iwl:   %u ms: LOAD_STATUS %#x, PC UMAC %#x LMAC %#x, CPU1 %#x CPU2 %#x, WFPM %#x, GP1 %#x, CSR_INT %#x\n", ms,
            load, upc, lpc, c1, c2, wfpm, rd(CSR_UCODE_DRV_GP1), rd(CSR_INT));
    info.st_load = load;
    info.st_umac_pc = upc;
    info.st_lmac_pc = lpc;
}

/* IOMMU (Intel VT-d): Die ACPI-Tabelle DMAR nennt die Einheiten (DRHD, Typ 0: Registeradresse bei +8). Hat die Firmware
 * des PCs die Adressuebersetzung (GSTS Bit 31) oder geschuetzte Speicherbereiche (PMEN Bit 0, "Pre-boot DMA
 * Protection") angelassen, kommen DMA-Zugriffe der Karte nicht an - das ROM bliebe beim Lesen der Context Info
 * haengen. Der Kernel benutzt die IOMMU nicht, also wird beides abgeschaltet. */
#define VTD_GCMD  0x18
#define VTD_GSTS  0x1C
#define VTD_PMEN  0x64
#define VTD_TES   (1u << 31)
#define VTD_PRS   (1u << 0)
#define VTD_EPM   (1u << 31)

static void iommu_check(void)
{
    uint32_t len = 0;
    const uint8_t *t = acpi_table("DMAR", &len);
    info.dmar_found = t != 0;
    info.iommu_units = info.iommu_active = info.iommu_off = 0;
    if (!t || len < 48) {
        kprintf("iwl: keine DMAR-Tabelle (keine IOMMU gemeldet)\n");
        return;
    }
    info.dmar_flags = t[37];
    kprintf("iwl: DMAR: Adressbreite %u Bit, Flags %#x%s\n", t[36] + 1, t[37],
            t[37] & 4 ? " (DMA-Schutz beim Booten verlangt)" : "");
    for (uint32_t off = 48; off + 4 <= len;) {
        uint16_t type = (uint16_t)(t[off] | t[off + 1] << 8), l = (uint16_t)(t[off + 2] | t[off + 3] << 8);
        if (l < 4 || off + l > len)
            break;
        if (type == 0 && l >= 16) {
            uint64_t base = (uint64_t)le32(t + off + 8) | (uint64_t)le32(t + off + 12) << 32;
            int n = (int)info.iommu_units++;
            if (paging_map_mmio(base, 4096) != 0) {
                kprintf("iwl:   IOMMU %d @ %#lx: Register nicht einblendbar\n", n, (unsigned long)base);
            } else {
                volatile uint32_t *r = (volatile uint32_t *)base;
                uint32_t gsts = r[VTD_GSTS / 4], pmen = r[VTD_PMEN / 4];
                if (n < 4) {
                    info.iommu_gsts[n] = gsts;
                    info.iommu_pmen[n] = pmen;
                }
                kprintf("iwl:   IOMMU %d @ %#lx%s: GSTS %#x (Uebersetzung %s), PMEN %#x (Schutzbereiche %s)\n", n,
                        (unsigned long)base, t[off + 4] & 1 ? " (alle PCI-Geraete)" : "", gsts,
                        gsts & VTD_TES ? "AN" : "aus", pmen, pmen & VTD_PRS ? "AN" : "aus");
                if (gsts & VTD_TES || pmen & VTD_PRS) {
                    info.iommu_active++;
                    if (gsts & VTD_TES) { /* GCMD: aktuellen Zustand ohne die Einmal-Bits, TE weg */
                        r[VTD_GCMD / 4] = (gsts & 0x96FFFFFFu) & ~VTD_TES;
                        WAIT_UNTIL(!(r[VTD_GSTS / 4] & VTD_TES), 50);
                    }
                    if (pmen & VTD_PRS) {
                        r[VTD_PMEN / 4] = pmen & ~VTD_EPM;
                        WAIT_UNTIL(!(r[VTD_PMEN / 4] & VTD_PRS), 50);
                    }
                    gsts = r[VTD_GSTS / 4];
                    pmen = r[VTD_PMEN / 4];
                    int off_now = !(gsts & VTD_TES) && !(pmen & VTD_PRS);
                    info.iommu_off += (uint32_t)off_now;
                    kprintf("iwl:   IOMMU %d abgeschaltet: %s (GSTS %#x, PMEN %#x)\n", n, off_now ? "ja" : "NEIN", gsts,
                            pmen);
                }
            }
        }
        off += l;
    }
}

/* LTR (Latency Tolerance Reporting): Linux stellt bei der AX200 vor dem Start etwa 250 us ein - sonst kann das ROM
 * beim Booten an Verzoegerungen der Plattform scheitern (die Firmware setzt den Wert spaeter selbst) */
#define CSR_LTR_LONG_VAL_AD 0x0D4
#define LTR_250US           ((1u << 31) | (2u << 26) | (250u << 16) | (1u << 15) | (2u << 10) | 250u)

/* Puffer fuer Laden und Betrieb (identisch eingeblendet: virtuelle = physische Adresse) */
#define CMD_SLOT 2048 /* Platz je Befehl: Kopf (8) + Daten (die Scan-Anfrage hat 1940 Byte) */
static CtxtInfo          *ci;
static uint64_t          *free_rbd;
static uint32_t          *used_rbd;
static volatile uint16_t *status;   /* Zahl der geschlossenen Empfangspuffer (closed_rb_num) */
static uint8_t           *cmdq, *cmdbuf;
static uint8_t           *rb[RX_RING];
static uint8_t           *secbuf[3][MAX_SEC];
static uint32_t           rx_read, rx_write, tx_write;
static int                rx_skip; /* den ersten Puffer nur zurueckgeben (ALIVE schon gelesen) */

static int fw_init(void);

int iwl_load_fw(void)
{
    mutex_lock(&iwl_op_lock);
    int r = iwl_load_fw_op();
    mutex_unlock(&iwl_op_lock);
    return r;
}

int iwl_load_fw_op(void)
{
    if (!info.present || !regs)
        return -1;
    if (!info.fw_found)
        return -4;
    /* Verbindung und Ringe vergessen: bis ALIVE wertet niemand den Empfang aus, und keiner sendet */
    iwl_sta_fw_reset();
    mutex_lock(&iwl_ring_lock);
    rings_ok = 0;
    mutex_unlock(&iwl_ring_lock);
    info.init_step = 0;
    info.load_done = 1;
    info.load_alive = 0;
    if (iwl_wake_test() == -2) /* Bereitschaft, Reset, Grundeinstellungen, Takt (Zugriff wird wieder abgegeben) */
        return -2;

    /* Firmware-Abschnitte, Empfang, Befehle, Context Info - einmal angelegt, bei jedem Laden neu gefuellt */
    if (!ci) {
        ci = dma(sizeof(CtxtInfo));
        free_rbd = dma(RX_RING * 8);
        used_rbd = dma(RX_RING * 4);
        status = dma(64);
        cmdq = dma(CMD_RING * TFD_SIZE);
        cmdbuf = dma(CMD_RING * CMD_SLOT);
        if (!ci || !free_rbd || !used_rbd || !status || !cmdq || !cmdbuf) {
            ci = 0;
            return -5;
        }
    }
    memset(ci, 0, sizeof(CtxtInfo));
    memset(used_rbd, 0, RX_RING * 4);
    memset((void *)status, 0, 64);
    memset(cmdq, 0, CMD_RING * TFD_SIZE);
    for (int i = 0; i < RX_RING; i++) {
        if (!rb[i] && !(rb[i] = dma(4096)))
            return -5;
        memset(rb[i], 0, 4096);
        free_rbd[i] = (uint64_t)rb[i] | (uint64_t)(i + 1); /* Kennung (vid) 1..64 in den unteren Bits */
    }
    rx_read = 0;
    rx_write = (RX_RING - 1) & ~7u;
    tx_write = 0;
    uint64_t *img[3] = {ci->lmac_img, ci->umac_img, ci->virtual_img};
    for (int p = 0; p < 3; p++)
        for (int i = 0; i < nsec[p]; i++) {
            if (!secbuf[p][i] && !(secbuf[p][i] = dma(sec[p][i].len)))
                return -5;
            memcpy(secbuf[p][i], sec[p][i].data, sec[p][i].len);
            img[p][i] = (uint64_t)secbuf[p][i];
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

    iommu_check();

    /* Status-Bits der Karte loeschen (1 schreiben), um danach zu sehen, ob ihre DMA-Zugriffe abgewiesen wurden */
    pci_write32(&pdev, 0x04, (pci_read32(&pdev, 0x04) & 0xFFFF) | 0xF9000000u);

    /* Start wie bei Linux (iwl_trans_pcie_gen2_start_fw): Handshake-Bits loeschen, Schattenregister, Adresse der
     * Context Info, LTR, dann den inneren Prozessor starten. Die freien Empfangspuffer meldet Linux erst, wenn sich
     * die Firmware per Interrupt meldet - den Empfang richtet sie selbst ein */
    wr(CSR_UCODE_DRV_GP1_CLR, GP1_SW_RFKILL | GP1_CMD_BLOCKED);
    wr(CSR_INT, 0xFFFFFFFFu);
    wr(CSR_FH_INT_STATUS, 0xFFFFFFFFu);
    wr(CSR_MAC_SHADOW_REG_CTRL, rd(CSR_MAC_SHADOW_REG_CTRL) | 0x800FFFFFu);
    wr(CSR_CTXT_INFO_BA, (uint32_t)(uint64_t)ci);
    wr(CSR_CTXT_INFO_BA + 4, (uint32_t)((uint64_t)ci >> 32));
    info.ltr_before = rd(CSR_LTR_LONG_VAL_AD);
    wr(CSR_LTR_LONG_VAL_AD, LTR_250US);
    info.ltr_after = rd(CSR_LTR_LONG_VAL_AD);
    kprintf("iwl: LTR %#x -> %#x\n", info.ltr_before, info.ltr_after);
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) | GP_MAC_ACCESS_REQ);
    int access = WAIT_UNTIL((rd(CSR_GP_CNTRL) & (GP_MAC_CLOCK_READY | GP_GOING_TO_SLEEP)) == GP_MAC_CLOCK_READY, 25);
    enable_wfpm(); /* sicherheitshalber noch einmal: der Startbefehl ist ein Peripherie-Register */
    uint32_t pc_before = prph_rd(UREG_UMAC_CURRENT_PC), init_before = prph_rd(UREG_CPU_INIT_RUN);
    prph_write(UREG_CPU_INIT_RUN, 1);
    /* gleich danach: kam alles an, und bewegt sich das ROM? (PC in schneller Folge, 50 ms lang) */
    uint32_t init_after = prph_rd(UREG_CPU_INIT_RUN), ba_lo = rd(CSR_CTXT_INFO_BA), ba_hi = rd(CSR_CTXT_INFO_BA + 4);
    uint32_t pcs[10];
    for (int i = 0; i < 10; i++) {
        pcs[i] = prph_rd(UREG_UMAC_CURRENT_PC);
        for (uint64_t t = time_us(); time_us() - t < 5000;) /* aktiv warten: der Zugriff bleibt bestehen */
            ;
    }
    uint32_t umag1 = prph_rd(UMAG_SB_CPU_1_STATUS), umag2 = prph_rd(UMAG_SB_CPU_2_STATUS);
    wr(CSR_GP_CNTRL, rd(CSR_GP_CNTRL) & ~GP_MAC_ACCESS_REQ);
    kprintf("iwl: Firmware gestartet (Context Info %#lx, %d+%d+%d Abschnitte, Zugriff %s)\n", (unsigned long)(uint64_t)ci,
            nsec[0], nsec[1], nsec[2], access ? "ok" : "NICHT bekommen");
    kprintf("iwl:   CPU_INIT_RUN %#x -> %#x, CTXT_INFO_BA %#x:%#x, UMAG CPU1 %#x CPU2 %#x\n", init_before, init_after, ba_hi,
            ba_lo, umag1, umag2);
    kprintf("iwl:   PC UMAC vorher %#x, danach alle 5 ms: %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x\n", pc_before, pcs[0], pcs[1],
            pcs[2], pcs[3], pcs[4], pcs[5], pcs[6], pcs[7], pcs[8], pcs[9]);

    /* auf ALIVE warten: der Status zaehlt die gefuellten Empfangspuffer. Alle 250 ms ins Log, was die Karte tut.
     * Beim ersten Interrupt (sonst nach 500 ms) die freien Puffer melden: Schreibzeiger auf ein Vielfaches von 8,
     * hoechstens Ringgroesse - 8 (64 waere wieder 0 = Ring leer) */
    uint64_t t0 = time_us(), next_dump = 0;
    uint32_t last_int = 0;
    int stocked = 0;
    while (time_us() - t0 < 2000000) {
        uint32_t ms = (uint32_t)((time_us() - t0) / 1000);
        if (time_us() - t0 >= next_dump) {
            next_dump += 250000;
            fw_state(ms);
        }
        uint32_t ci_int = rd(CSR_INT), fh_int = rd(CSR_FH_INT_STATUS);
        if (ci_int != last_int) {
            kprintf("iwl:   nach %u ms: CSR_INT %#x, FH_INT %#x, Status %u\n", ms, ci_int, fh_int,
                    *status);
            last_int = ci_int;
        }
        if (!stocked && (ci_int || fh_int || ms >= 500)) {
            wr(RFH_Q0_FRBDCB_WIDX_TRG, rx_write);
            stocked = 1;
            kprintf("iwl:   nach %u ms: %u freie Empfangspuffer gemeldet (%s)\n", ms, (RX_RING - 1) & ~7u,
                    ci_int || fh_int ? "nach Interrupt" : "ohne Interrupt");
        }
        if (*status) {
            info.load_alive = 1;
            break;
        }
        thread_sleep_ms(1);
    }
    info.load_ms = (uint32_t)((time_us() - t0) / 1000);
    fw_state(info.load_ms);
    uint32_t pst = pci_read32(&pdev, 0x04) >> 16;
    info.pci_cmd = pci_read32(&pdev, 0x04);
    kprintf("iwl:   PCI-Status danach %#x%s%s%s%s\n", pst, pst & (1u << 13) ? ", DMA abgewiesen (Master Abort)" : "",
            pst & (1u << 12) ? ", Target Abort empfangen" : "", pst & (1u << 14) ? ", Systemfehler gemeldet" : "",
            pst & (1u << 15) ? ", Paritaetsfehler" : "");
    info.load_int = rd(CSR_INT);
    info.load_status = *status;
    if (!info.load_alive) {
        kprintf("iwl: keine Nachricht der Firmware nach %u ms (CSR_INT %#x%s, GP_CNTRL %#x)\n", info.load_ms, info.load_int,
                info.load_int & (1u << 29) ? " = Hardware-Fehler" : info.load_int & (1u << 25) ? " = Firmware-Fehler" : "",
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
    if (info.alive_cmd != 1 || info.alive_status != 0xCAFE)
        return -3;
    mutex_lock(&iwl_ring_lock);
    rx_skip = 1; /* ALIVE ist schon ausgewertet */
    rings_ok = 1;
    mutex_unlock(&iwl_ring_lock);
    return fw_init();
}

/* ---------- Stufe 3: Befehle an die Firmware, Antworten und Meldungen ----------
 * Befehle: Warteschlange 0 (32 TFDs zu 256 Byte). Jeder TFD zeigt auf bis zu 25 Puffer (tb_len 16 Bit, Adresse
 * 64 Bit); der erste hoechstens 20 Byte, wie bei Linux. Ein Befehl beginnt mit dem breiten Kopf (Befehl, Gruppe,
 * Folgenummer = Warteschlange << 8 | Index, Laenge der Daten, 0, Version). Dann Schreibzeiger (0..255) nach
 * HBUS_TARG_WRPTR, Warteschlange in Bit 16..
 * Empfang: Der Status nennt die Zahl der geschlossenen Puffer; je Puffer steht im Ring der benutzten Puffer die
 * Kennung (vid). In einem Puffer koennen mehrere Pakete liegen (Laenge | Flags, Kopf: Befehl, Gruppe, Folgenummer,
 * Daten), je auf 64 Byte ausgerichtet; 0x55550000 beendet die Liste. Antworten tragen die Folgenummer des Befehls,
 * Meldungen der Firmware haben Bit 15 (SEQ_RX_FRAME) gesetzt. Lehnt die Firmware einen Befehl ab, kommt statt der
 * Antwort REPLY_ERROR (Fehlerart, Befehl, Folgenummer). */
#define HBUS_TARG_WRPTR    0x460
#define SEQ_RX_FRAME       0x8000
#define RX_FRAME_INVALID   0x55550000u
#define CMD_ALIVE          0x01
#define CMD_REPLY_ERROR    0x02
#define CMD_INIT_COMPLETE  0x04
#define CMD_INIT_EXT_CFG   0x03 /* SYSTEM_GROUP */
#define CMD_NVM_ACCESS_END 0x00 /* REGULATORY_AND_NVM_GROUP: NVM_ACCESS_COMPLETE */
#define CMD_NVM_GET_INFO   0x02
#define CMD_DEBUG_LOG      0xF7
#define CMD_TX_ANT_CFG     0x98
#define CMD_BT_CONFIG      0x9B
#define CMD_MCC_UPDATE     0xC8
#define CMD_SCAN_CFG       0x0C
#define CMD_SCAN_REQ       0x0D
#define CMD_SCAN_COMPLETE  0x0F
#define INIT_NVM           (1u << 1) /* INIT_EXTENDED_CFG: der Treiber schickt NVM-Befehle */
#define CSR_INT_SW_ERR     (1u << 25)
#define CSR_INT_HW_ERR     (1u << 29)

static void scan_frame(const uint8_t *d, uint32_t len);
static int  scan_busy;

/* Worauf gewartet wird (es wartet immer nur einer: wer iwl_op_lock haelt): want auf die Antwort eines Befehls,
 * want_n auf eine Meldung - die kann schon vor dem Absenden des Befehls scharf gemacht werden, denn sie kommt oft im
 * selben Empfangspuffer wie die Antwort. Gefuellt beim Empfang, von dem Thread, der gerade den Ring auswertet. */
typedef struct {
    volatile int got, err; /* err: REPLY_ERROR fuer diesen Befehl (Fehlerart in data) */
    int          active, seq; /* seq < 0: Meldung, jede Folgenummer */
    uint8_t      group, cmd;
    uint32_t     len;
    uint8_t      data[512];
} Want;
static Want     want, want_n;
static uint32_t logged;

static void want_arm(Want *w, uint8_t group, uint8_t cmd, int seq)
{
    w->got = w->err = 0;
    w->group = group;
    w->cmd = cmd;
    w->seq = seq;
    w->active = 1;
}

static void want_set(uint8_t group, uint8_t cmd, int seq)
{
    want_arm(&want, group, cmd, seq);
}

static int want_match(Want *w, uint8_t grp, uint8_t cmd, uint16_t seq, const uint8_t *d, uint32_t plen)
{
    if (!w->active || w->got || cmd != w->cmd || !(w->group == GRP_ANY || w->seq >= 0 || grp == w->group) ||
        !(w->seq < 0 ? (seq & SEQ_RX_FRAME) != 0 : seq == (uint16_t)w->seq))
        return 0;
    w->len = plen < sizeof(w->data) ? plen : sizeof(w->data);
    memcpy(w->data, d, w->len);
    __sync_synchronize();
    w->got = 1;
    return 1;
}

void iwl_expect_notif(uint8_t group, uint8_t cmd)
{
    want_arm(&want_n, group, cmd, -1);
}

static void rx_packet(const uint8_t *pkt, uint32_t len)
{
    uint8_t cmd = pkt[4], grp = pkt[5];
    uint16_t seq = (uint16_t)(pkt[6] | pkt[7] << 8);
    uint32_t plen = len - 4;
    const uint8_t *d = pkt + 8;
    info.rx_packets++;
    if (grp == GRP_LEGACY && cmd == CMD_INIT_COMPLETE)
        info.init_complete = 1;
    want_match(&want, grp, cmd, seq, d, plen);
    want_match(&want_n, grp, cmd, seq, d, plen);
    if (cmd == CMD_REPLY_ERROR && plen >= 8) { /* Fehlerart, Befehl, reserviert, Folgenummer des Befehls */
        uint16_t bad = (uint16_t)(d[6] | d[7] << 8);
        kprintf("iwl:   REPLY_ERROR: Fehlerart %#x fuer Befehl %#x (Folge %#x), Dienst %#x\n", le32(d), d[4], bad,
                plen >= 12 ? le32(d + 8) : 0);
        if (want.active && !want.got && want.seq >= 0 && bad == (uint16_t)want.seq) {
            want.len = 4;
            memcpy(want.data, d, 4);
            want.err = 1;
            __sync_synchronize();
            want.got = 1;
        }
        return;
    }
    if (cmd == CMD_RX_MPDU) {
        if (scan_busy || !iwl_sta_wants_rx())
            scan_frame(d, plen);
        if (iwl_sta_wants_rx())
            iwl_sta_rx_mpdu(d, plen);
        return;
    }
    if (cmd == CMD_TX && !(seq & SEQ_RX_FRAME)) { /* gesendeter Rahmen: Ergebnis (Warteschlange im Folgefeld) */
        iwl_sta_tx_resp(seq, d, plen);
        return;
    }
    if (iwl_sta_notif(grp, cmd, d, plen)) /* z.B. neue Senderate (TLC) */
        return;
    if (grp == GRP_LEGACY && (cmd == 0xC2 || cmd == 0xC3 || cmd == 0xC5))
        return; /* BAR_FRAME_RELEASE, FRAME_RELEASE (Hinweise zum Umsortieren bei Block-Ack - wir reichen jeden Rahmen
                 * sofort weiter), BA_NOTIF */
    if (grp == GRP_LEGACY && cmd == CMD_DEBUG_LOG)
        return; /* Protokoll der Firmware: zu viel fuers Log */
    if (logged++ < 40)
        kprintf("iwl:   <- %s %#x.%#x, Folge %#x, %u Byte: %08x %08x %08x %08x\n",
                seq & SEQ_RX_FRAME ? "Meldung" : "Antwort", grp, cmd, seq, plen, plen >= 4 ? le32(d) : 0,
                plen >= 8 ? le32(d + 4) : 0, plen >= 12 ? le32(d + 8) : 0, plen >= 16 ? le32(d + 12) : 0);
}

/* Geschlossene Empfangspuffer auswerten, geleert wieder in den Ring der freien Puffer stellen (mit iwl_ring_lock) */
static void rx_poll(void)
{
    if (!rings_ok)
        return;
    uint32_t closed = *status & (RX_RING - 1);
    __sync_synchronize(); /* erst den Status, dann die Puffer lesen */
    int n = 0;
    while (rx_read != closed) {
        uint32_t vid = used_rbd[rx_read] & 0xFFF;
        if (vid >= 1 && vid <= RX_RING) {
            uint8_t *b = rb[vid - 1];
            if (!rx_skip)
                for (uint32_t off = 0; off + 12 <= 4096;) {
                    uint32_t lnf = le32(b + off), len = lnf & 0x3FFF;
                    if (lnf == RX_FRAME_INVALID || len < 4 || off + 4 + len > 4096)
                        break;
                    rx_packet(b + off, len);
                    off += (len + 4 + 63) & ~63u;
                }
            rx_skip = 0;
            memset(b, 0, 4096);
            free_rbd[rx_write] = (uint64_t)b | vid;
            rx_write = (rx_write + 1) & (RX_RING - 1);
        } else {
            kprintf("iwl:   Empfang: ungueltige Kennung %u an Stelle %u\n", vid, rx_read);
        }
        rx_read = (rx_read + 1) & (RX_RING - 1);
        n++;
    }
    if (n) {
        __sync_synchronize(); /* Ring vor dem Schreibzeiger fertig */
        wr(RFH_Q0_FRBDCB_WIDX_TRG, rx_write & ~7u);
    }
}

void iwl_poll(void)
{
    mutex_lock(&iwl_ring_lock);
    rx_poll();
    mutex_unlock(&iwl_ring_lock);
}

int iwl_fw_failed(void)
{
    return rings_ok && (rd(CSR_INT) & (CSR_INT_SW_ERR | CSR_INT_HW_ERR)) != 0;
}

/* Befehl in die Warteschlange; w (falls da) bekommt die Folgenummer, bevor die Karte vom Befehl erfaehrt - sonst
 * koennte ein anderer Thread die Antwort schon abholen, bevor jemand auf sie wartet */
static int send_cmd(uint8_t group, uint8_t cmd, const void *data, uint32_t len, Want *w)
{
    if (len + 8 > CMD_SLOT)
        return -1;
    mutex_lock(&iwl_ring_lock);
    uint32_t idx = tx_write & (CMD_RING - 1);
    uint8_t *c = cmdbuf + idx * CMD_SLOT, *tfd = cmdq + idx * TFD_SIZE;
    uint16_t seq = (uint16_t)(tx_write & 0xFF); /* Warteschlange 0 */
    c[0] = cmd;
    c[1] = group;
    c[2] = (uint8_t)seq;
    c[3] = (uint8_t)(seq >> 8);
    c[4] = (uint8_t)len;
    c[5] = (uint8_t)(len >> 8);
    c[6] = 0;
    c[7] = 0;
    memcpy(c + 8, data, len);
    memset(tfd, 0, TFD_SIZE);
    uint32_t total = 8 + len, first = total < 20 ? total : 20, n = 0;
    uint64_t addr[2] = {(uint64_t)c, (uint64_t)c + first};
    uint32_t tlen[2] = {first, total - first};
    for (int i = 0; i < 2; i++) {
        if (!tlen[i])
            continue;
        uint8_t *tb = tfd + 2 + n * 10;
        tb[0] = (uint8_t)tlen[i];
        tb[1] = (uint8_t)(tlen[i] >> 8);
        memcpy(tb + 2, &addr[i], 8);
        n++;
    }
    tfd[0] = (uint8_t)n;
    tfd[1] = 0;
    tx_write = (tx_write + 1) & 0xFF;
    if (w)
        w->seq = seq;
    __sync_synchronize(); /* Befehl und TFD stehen im Speicher, bevor die Karte davon erfaehrt */
    wr(HBUS_TARG_WRPTR, tx_write | (0u << 16));
    mutex_unlock(&iwl_ring_lock);
    return seq;
}

/* bis die Antwort (seq >= 0) bzw. Meldung da ist; Laenge der Daten oder -1 (Zeit um), -2 (Fehler der Firmware),
 * -3 (REPLY_ERROR). Zwischen dem Nachsehen schlaeft der Thread ohne iwl_ring_lock. */
static int wait_for(Want *w, uint8_t group, uint8_t cmd, uint32_t ms, const char *what)
{
    uint64_t t0 = time_us();
    for (;;) {
        iwl_poll();
        __sync_synchronize();
        if (w->got)
            break;
        uint32_t ci_int = rd(CSR_INT);
        if (ci_int & (CSR_INT_SW_ERR | CSR_INT_HW_ERR)) {
            kprintf("iwl:   %s: %s-Fehler (CSR_INT %#x)\n", what, ci_int & CSR_INT_SW_ERR ? "Firmware" : "Hardware",
                    ci_int);
            fw_state((uint32_t)((time_us() - t0) / 1000));
            w->active = 0;
            return -2;
        }
        if (time_us() - t0 > (uint64_t)ms * 1000) {
            kprintf("iwl:   %s: keine Antwort nach %u ms (%#x.%#x)\n", what, ms, group, cmd);
            fw_state(ms);
            w->active = 0;
            return -1;
        }
        thread_sleep_ms(1);
    }
    w->active = 0;
    if (w->err)
        return -3;
    kprintf("iwl:   %s: Antwort nach %u ms, %u Byte\n", what, (uint32_t)((time_us() - t0) / 1000), w->len);
    return (int)w->len;
}

static int cmd_sync(uint8_t group, uint8_t cmd, const void *data, uint32_t len, const char *what)
{
    want_set(group, cmd, 0x7FFFFFFF); /* Folgenummer setzt send_cmd */
    int seq = send_cmd(group, cmd, data, len, &want);
    if (seq < 0) {
        want.active = 0;
        return -1;
    }
    kprintf("iwl:   -> %s (%#x.%#x, Folge %#x, %u Byte)\n", what, group, cmd, seq, len);
    return wait_for(&want, group, cmd, 1000, what);
}

int iwl_cmd(uint8_t group, uint8_t cmd, const void *data, uint32_t len, const char *what, void *resp, uint32_t max)
{
    int n = cmd_sync(group, cmd, data, len, what);
    if (n >= 0 && resp)
        memcpy(resp, want.data, (uint32_t)n < max ? (uint32_t)n : max);
    return n;
}

int iwl_wait_notif(uint8_t group, uint8_t cmd, uint32_t ms, const char *what, void *resp, uint32_t max)
{
    if (!want_n.active || want_n.group != group || want_n.cmd != cmd)
        iwl_expect_notif(group, cmd); /* nicht vorher scharf gemacht */
    int n = wait_for(&want_n, group, cmd, ms, what);
    if (n >= 0 && resp)
        memcpy(resp, want_n.data, (uint32_t)n < max ? (uint32_t)n : max);
    return n;
}

static int valid_mac(const uint8_t *m)
{
    return !(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5]);
}

/* MAC-Adresse aus den Registern der Karte (Linux: iwl_set_hw_address_from_csr, Basis 0x380 bei der Familie 22000):
 * erst die vom Hersteller gesetzte (STRAP), sonst die aus dem OTP-Speicher. Byte-Reihenfolge vertauscht. */
static void read_mac(void)
{
    static const uint32_t regs_[2][2] = {{0x388, 0x38C}, {0x380, 0x384}};
    for (int k = 0; k < 2; k++) {
        uint32_t a = rd(regs_[k][0]), b = rd(regs_[k][1]);
        uint8_t m[6] = {(uint8_t)(a >> 24), (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a, (uint8_t)(b >> 8),
                        (uint8_t)b};
        kprintf("iwl:   MAC-Register %#x/%#x: %08x %08x -> %02x:%02x:%02x:%02x:%02x:%02x\n", regs_[k][0], regs_[k][1], a,
                b, m[0], m[1], m[2], m[3], m[4], m[5]);
        if (valid_mac(m)) {
            memcpy(info.mac, m, 6);
            return;
        }
    }
}

/* Ablauf wie Linux (iwl_run_unified_mvm_ucode): INIT_EXTENDED_CFG (NVM folgt), NVM_ACCESS_COMPLETE, auf
 * INIT_COMPLETE warten, dann NVM_GET_INFO: Faehigkeiten, Antennen, Kanaele. Dazu die MAC-Adresse. */
static int fw_init(void)
{
    info.init_step = 1;
    info.init_complete = 0;
    info.rx_packets = 0;
    logged = 0;
    uint32_t flags = INIT_NVM, zero = 0;
    if (cmd_sync(GRP_SYSTEM, CMD_INIT_EXT_CFG, &flags, 4, "INIT_EXTENDED_CFG") < 0)
        return -6;
    info.init_step = 2;
    if (cmd_sync(GRP_NVM, CMD_NVM_ACCESS_END, &zero, 4, "NVM_ACCESS_COMPLETE") < 0)
        return -6;
    info.init_step = 3;
    if (!info.init_complete && iwl_wait_notif(GRP_LEGACY, CMD_INIT_COMPLETE, 2000, "INIT_COMPLETE", 0, 0) < 0 &&
        !info.init_complete)
        return -6;
    info.init_step = 4;
    int n = cmd_sync(GRP_NVM, CMD_NVM_GET_INFO, &zero, 4, "NVM_GET_INFO");
    if (n < 24)
        return -6;
    const uint8_t *d = want.data;
    info.nvm_flags = le32(d);
    info.nvm_version = (uint32_t)(d[4] | d[5] << 8);
    info.nvm_board = d[6];
    info.nvm_hw_addrs = d[7];
    info.nvm_sku = le32(d + 8);
    info.nvm_tx_chains = le32(d + 12);
    info.nvm_rx_chains = le32(d + 16);
    info.nvm_lar = le32(d + 20);
    info.nvm_channels = n >= 28 && n != 132 ? le32(d + 24) : 51; /* Antwort v4 (472 Byte) nennt die Zahl, v3: 51 */
    kprintf("iwl: NVM Version %#x, Flags %#x, %u MAC-Adressen, SKU %#x, Antennen TX %#x RX %#x, LAR %u, %u Kanaele\n",
            info.nvm_version, info.nvm_flags, info.nvm_hw_addrs, info.nvm_sku, info.nvm_tx_chains, info.nvm_rx_chains,
            info.nvm_lar, info.nvm_channels);
    read_mac();

    /* wie iwl_mvm_up, soweit es zum Suchen noetig ist: Antennen, Koexistenz mit Bluetooth, Land (LAR: "ZZ" = die
     * Voreinstellung der Karte, Quelle GET_CURRENT), Grundeinstellung der Suche */
    info.init_step = 5;
    uint32_t ant = info.nvm_tx_chains ? info.nvm_tx_chains : 3;
    if (cmd_sync(GRP_LONG, CMD_TX_ANT_CFG, &ant, 4, "TX_ANT_CONFIGURATION") < 0)
        return -6;
    info.init_step = 6;
    uint32_t bt[2] = {1 /* BT_COEX_NW */, 0x15 /* MPLUT, SYNC2SCO, HIGH_BAND_RET */};
    if (cmd_sync(GRP_LONG, CMD_BT_CONFIG, bt, 8, "BT_CONFIG") < 0)
        return -6;
    info.init_step = 7;
    uint8_t mcc[28] = {'Z', 'Z', 0x10 /* MCC_SOURCE_GET_CURRENT */};
    int m = cmd_sync(GRP_LONG, CMD_MCC_UPDATE, mcc, sizeof(mcc), "MCC_UPDATE");
    if (m < 0)
        return -6;
    if (m >= 20) { /* Antwort v4: Status, Land, Faehigkeiten, Zeit, Geo, Quelle, 3, Zahl der Kanaele */
        info.mcc = (uint32_t)(want.data[4] << 8 | want.data[5]);
        info.mcc_status = le32(want.data);
        info.mcc_channels = le32(want.data + 16);
        kprintf("iwl: Land '%c%c' (Status %u), %u Kanaele\n", (char)want.data[5], (char)want.data[4], info.mcc_status,
                info.mcc_channels);
    }
    info.init_step = 8;
    uint32_t scfg[3] = {0, ant, info.nvm_rx_chains ? info.nvm_rx_chains : 3}; /* SCAN_CONFIG v5 */
    if (cmd_sync(GRP_LONG, CMD_SCAN_CFG, scfg, sizeof(scfg), "SCAN_CFG") < 0)
        return -6;
    info.init_step = 9;
    kprintf("iwl: Firmware bereit, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", info.mac[0], info.mac[1], info.mac[2],
            info.mac[3], info.mac[4], info.mac[5]);
    return 0;
}

/* ---------- Stufe 4: Netze suchen ----------
 * SCAN_REQ_UMAC Version 15 (Aufbau wie struct iwl_scan_req_umac_v17 in Linux, 1940 Byte): passiv auf allen Kanaelen
 * von 2,4 und 5 GHz - die Karte hoert je Kanal ~110 ms auf Beacons. Jeder empfangene Rahmen kommt als
 * REPLY_RX_MPDU (Beschreibung 48 Byte wie iwl_rx_mpdu_desc bis einschliesslich v1, dann der 802.11-Rahmen); das Ende meldet SCAN_COMPLETE_UMAC. */
typedef struct __attribute__((packed)) {
    uint32_t uid, ooc_priority;
    /* general_params_v11 */
    uint16_t gflags;
    uint8_t  greserved, scan_start_mac;
    uint8_t  active_dwell[2], adwell_2g, adwell_5g, adwell_social, gflags2;
    uint16_t adwell_max_budget;
    uint32_t max_out_of_time[2], suspend_time[2], scan_priority;
    uint8_t  passive_dwell[2], num_fragments[2];
    /* channel_params_v7 */
    uint8_t  cflags, count, n_aps_override[2];
    struct __attribute__((packed)) {
        uint32_t flags;
        uint8_t  channel, band, iter_count, iter_interval;
    } chan[67];
    /* periodic_params_v1 */
    struct __attribute__((packed)) {
        uint16_t interval;
        uint8_t  iter_count, reserved;
    } schedule[2];
    uint16_t delay, preserved;
    /* probe_params_v4 */
    uint8_t  preq[4 + 12 + 4 + 512];
    uint8_t  short_ssid_num, bssid_num;
    uint16_t probe_reserved;
    uint8_t  direct_scan[20][34];
    uint32_t short_ssid[8];
    uint8_t  bssid_array[16][6];
} ScanReq;
_Static_assert(__builtin_offsetof(ScanReq, cflags) == 44, "Scan: Kanaele");
_Static_assert(__builtin_offsetof(ScanReq, schedule) == 584, "Scan: Zeitplan");
_Static_assert(sizeof(ScanReq) == 1940, "Scan: Groesse");

#define RX_DESC 48 /* Laenge, Flags, Phy, Status (+12), Reihenfolge, dann v1: RSS, Filter, Rate, Energie A/B (+32), Kanal */
static WlanNet nets[WLAN_MAX_NETS];
static IwlBss   nets_x[WLAN_MAX_NETS]; /* dazu, was zum Verbinden noetig ist */
static uint32_t n_nets, scan_frames;

static const uint8_t scan_chans[] = {1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  36,  40,  44,  48,
                                     52,  56,  60,  64,  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
                                     149, 153, 157, 161, 165};

/* Beacon oder Antwort auf eine Suchanfrage: BSSID, Staerke, Kanal, Name (SSID), Verschluesselung */
static void scan_frame(const uint8_t *d, uint32_t len)
{
    scan_frames++;
    if (len < RX_DESC + 36)
        return;
    uint32_t mpdu_len = (uint32_t)(d[0] | d[1] << 8), status = le32(d + 12);
    uint8_t flags2 = d[3], energy_a = d[32], energy_b = d[33], chan = d[34];
    if (!(status & 1) || RX_DESC + mpdu_len > len) /* CRC nicht in Ordnung */
        return;
    const uint8_t *f = d + RX_DESC;
    if (f[0] != 0x80 && f[0] != 0x50) /* nur Beacon (Typ 0, Untertyp 8) und Probe Response (5) */
        return;
    uint32_t hdr = 24 + (flags2 & 0x20 ? 2 : 0), flen = mpdu_len;
    if (flen < hdr + 12)
        return;
    const uint8_t *bssid = f + 16, *body = f + hdr;
    uint16_t cap = (uint16_t)(body[10] | body[11] << 8);
    int sig = -(int)(energy_a && (!energy_b || energy_a < energy_b) ? energy_a : energy_b ? energy_b : 100);
    WlanNet n;
    static IwlBss x;
    memset(&n, 0, sizeof(n));
    memset(&x, 0, sizeof(x));
    memcpy(n.bssid, bssid, 6);
    n.signal = (int8_t)sig;
    n.channel = chan;
    n.security = cap & 0x10 ? WLAN_SEC_WEP : WLAN_SEC_OPEN;
    x.cap = cap;
    x.beacon_int = (uint16_t)(body[8] | body[9] << 8);
    x.dtim_period = 1;
    int psk = 0, sae = 0;
    for (uint32_t o = hdr + 12; o + 2 <= flen;) {
        uint8_t id = f[o], l = f[o + 1];
        if (o + 2 + l > flen)
            break;
        const uint8_t *v = f + o + 2;
        if (id == 0 && l <= 32) {
            memcpy(n.ssid, v, l);
            n.ssid_len = l;
        } else if ((id == 1 || id == 50) && x.n_rates + l <= sizeof(x.rates)) { /* (erweiterte) Datenraten */
            memcpy(x.rates + x.n_rates, v, l);
            x.n_rates = (uint8_t)(x.n_rates + l);
        } else if (id == 3 && l >= 1) {
            n.channel = v[0];
        } else if (id == 5 && l >= 2 && v[1]) { /* TIM: DTIM-Zaehler, DTIM-Periode */
            x.dtim_period = v[1];
        } else if (id == 48) {
            n.security = WLAN_SEC_WPA2;
            if (l + 2u <= sizeof(x.rsn)) {
                memcpy(x.rsn, f + o, l + 2u);
                x.rsn_len = (uint8_t)(l + 2);
            }
            /* AKM-Liste: 00-0F-AC-02 = PSK, 00-0F-AC-08 = SAE (WPA3) */
            if (l >= 8) {
                uint32_t pc = (uint32_t)(v[6] | v[7] << 8), ak = 8 + pc * 4;
                if (ak + 2 <= l) {
                    uint32_t na = (uint32_t)(v[ak] | v[ak + 1] << 8);
                    for (uint32_t i = 0; i < na && ak + 2 + i * 4 + 4 <= l; i++) {
                        psk |= v[ak + 2 + i * 4 + 3] == 2;
                        sae |= v[ak + 2 + i * 4 + 3] == 8;
                    }
                }
            }
        } else if (id == 221 && l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 1 &&
                   n.security < WLAN_SEC_WPA2) {
            n.security = WLAN_SEC_WPA;
        } else if (id == 221 && l >= 7 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 2) {
            x.has_wmm = 1; /* WMM (QoS): Informations- (Untertyp 0) oder Parameterelement (1, mit EDCA je Klasse) */
            if (v[4] == 1 && l >= 24) {
                for (int a = 0; a < 4; a++) {
                    const uint8_t *r = v + 8 + a * 4;
                    memcpy(x.wmm_ac[(r[0] >> 5) & 3], r, 4);
                }
                x.wmm_params = 1;
            }
        } else if (id == 45 && l >= 26) { /* HT Capabilities */
            x.has_ht = 1;
            x.ht_cap = (uint16_t)(v[0] | v[1] << 8);
            x.ht_ampdu = v[2];
            x.ht_mcs[0] = v[3];
            x.ht_mcs[1] = v[4];
        } else if (id == 61 && l >= 5) { /* HT Operation */
            x.ht_primary = v[0];
            x.ht_sec = v[1] & 3;
            x.ht_wide = (v[1] >> 2) & 1;
            x.ht_prot = v[2] & 3;
        } else if (id == 191 && l >= 12) { /* VHT Capabilities */
            x.has_vht = 1;
            x.vht_cap = le32(v);
            x.vht_rx_mcs = (uint16_t)(v[4] | v[5] << 8);
        } else if (id == 192 && l >= 3) { /* VHT Operation */
            x.vht_width = v[0];
            x.vht_center = v[1];
        }
        o += 2 + l;
    }
    if (n.security == WLAN_SEC_WPA2 && sae)
        n.security = psk ? WLAN_SEC_WPA2_3 : WLAN_SEC_WPA3;
    for (uint32_t i = 0; i < n_nets; i++)
        if (memcmp(nets[i].bssid, n.bssid, 6) == 0) {
            if (n.signal > nets[i].signal)
                nets[i].signal = n.signal;
            nets[i].seen++;
            if (!nets[i].ssid_len && n.ssid_len) { /* verstecktes Netz: die Probe Response nennt den Namen */
                memcpy(nets[i].ssid, n.ssid, sizeof(n.ssid));
                nets[i].ssid_len = n.ssid_len;
            }
            return;
        }
    if (n_nets < WLAN_MAX_NETS) {
        n.seen = 1;
        nets_x[n_nets] = x;
        nets[n_nets++] = n;
    }
}

int iwl_scan_bss(unsigned i, WlanNet *n, IwlBss *x)
{
    if (i >= n_nets)
        return -1;
    *n = nets[i];
    *x = nets_x[i];
    return 0;
}

int iwl_scan(void)
{
    if (scan_busy)
        return -7;
    mutex_lock(&iwl_op_lock);
    int r = iwl_scan_op();
    mutex_unlock(&iwl_op_lock);
    return r;
}

int iwl_scan_op(void)
{
    if (info.init_step != 9) {
        int r = iwl_load_fw_op();
        if (r < 0)
            return r;
    }
    scan_busy = 1;
    static ScanReq rq;
    memset(&rq, 0, sizeof(rq));
    rq.uid = 0;
    rq.ooc_priority = 6;                         /* IWL_SCAN_PRIORITY_EXT_6 */
    rq.gflags = (1u << 11) | (1u << 1) | (1u << 7); /* FORCE_PASSIVE, PASS_ALL, ADAPTIVE_DWELL */
    rq.active_dwell[0] = rq.active_dwell[1] = 10;
    rq.passive_dwell[0] = rq.passive_dwell[1] = 110;
    rq.adwell_2g = 2;
    rq.adwell_5g = 8;
    rq.adwell_social = 10;
    rq.adwell_max_budget = 300;
    rq.scan_priority = 6;
    rq.cflags = 1u << 5; /* ENABLE_CHAN_ORDER */
    rq.n_aps_override[0] = 10;
    rq.n_aps_override[1] = 2;
    for (unsigned i = 0; i < sizeof(scan_chans); i++) {
        rq.chan[i].channel = scan_chans[i];
        rq.chan[i].band = scan_chans[i] <= 14 ? 1 : 0; /* PHY_BAND_24 / PHY_BAND_5 */
        rq.chan[i].iter_count = 1;
    }
    rq.count = (uint8_t)sizeof(scan_chans);
    rq.schedule[0].iter_count = 1;
    n_nets = 0;
    scan_frames = 0;
    logged = 0;
    uint64_t t0 = time_us();
    iwl_expect_notif(GRP_ANY, CMD_SCAN_COMPLETE); /* kann im selben Puffer wie die Antwort kommen */
    int r = cmd_sync(GRP_LONG, CMD_SCAN_REQ, &rq, sizeof(rq), "SCAN_REQ_UMAC");
    if (r >= 4 && le32(want.data) != 0) {
        kprintf("iwl: Suche abgelehnt, Status %#x\n", le32(want.data));
        r = -8;
    }
    if (r >= 0) {
        r = wait_for(&want_n, GRP_ANY, CMD_SCAN_COMPLETE, 15000, "SCAN_COMPLETE");
        if (r >= 8)
            kprintf("iwl: Suche fertig nach %u ms: Status %u, %u Rahmen, %u Netze\n",
                    (uint32_t)((time_us() - t0) / 1000), want_n.data[6], scan_frames, n_nets);
    }
    want_n.active = 0;
    info.scan_ms = (uint32_t)((time_us() - t0) / 1000);
    info.scan_frames = scan_frames;
    info.scan_nets = n_nets;
    scan_busy = 0;
    return r < 0 ? (r == -8 ? -8 : -6) : (int)n_nets;
}

int iwl_scan_result(unsigned i, WlanNet *out)
{
    if (i >= n_nets)
        return -1;
    *out = nets[i];
    return 0;
}
