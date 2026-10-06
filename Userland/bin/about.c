#include "gfx.h"
#include "ui.h"

/* about: "Ueber MiniKernel" im Stil von "Ueber diesen Mac" - Symbol und Name mittig, darunter Prozessor (Name per
 * CPUID), Arbeitsspeicher, Grafik, Bildschirm, Festplatte, Betriebssystem und Laufzeit (jede Sekunde neu). Unter dem
 * Desktop im Fenster, sonst im Vollbild (Esc/q beendet). */

static int  scr_w, scr_h;
static char cpu_name[64], vm_name[16], gpu_name[64], os_txt[64];

static void cpuid(unsigned leaf, unsigned r[4])
{
    __asm__ __volatile__("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

/* Prozessorname (CPUID 0x80000002-4) ohne "(R)", "(TM)", "CPU" und doppelte Leerzeichen; Hypervisor (Leaf 0x40000000) */
static void read_cpu(void)
{
    unsigned r[4];
    char raw[49] = {0};
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004) {
        for (unsigned i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, r);
            memcpy(raw + i * 16, r, 16);
        }
    } else {
        cpuid(0, r); /* nur der Hersteller: GenuineIntel, AuthenticAMD */
        memcpy(raw, &r[1], 4);
        memcpy(raw + 4, &r[3], 4);
        memcpy(raw + 8, &r[2], 4);
    }
    static const char *drop[] = {"(R)", "(r)", "(TM)", "(tm)", " CPU @", " Processor"}; /* " CPU @" -> " @" */
    for (unsigned d = 0; d < sizeof(drop) / sizeof(drop[0]); d++) {
        char *p;
        int len = strcmp(drop[d], " CPU @") == 0 ? 4 : (int)strlen(drop[d]);
        while ((p = strstr(raw, drop[d])) != 0)
            memmove(p, p + len, strlen(p + len) + 1);
    }
    int n = 0;
    for (const char *p = raw; *p; p++)
        if (*p != ' ' || (n && cpu_name[n - 1] != ' '))
            if (n < (int)sizeof(cpu_name) - 1)
                cpu_name[n++] = *p;
    while (n && cpu_name[n - 1] == ' ')
        n--;
    cpu_name[n] = 0;

    cpuid(1, r);
    if (r[2] & (1u << 31)) { /* Hypervisor-Bit */
        char sig[13] = {0};
        cpuid(0x40000000, r);
        memcpy(sig, &r[1], 4);
        memcpy(sig + 4, &r[2], 4);
        memcpy(sig + 8, &r[3], 4);
        snprintf(vm_name, sizeof(vm_name), "%s", strncmp(sig, "KVMKVMKVM", 9) == 0 ? "KVM" :
                 strcmp(sig, "TCGTCGTCGTCG") == 0 ? "QEMU" : strncmp(sig, "Microsoft Hv", 12) == 0 ? "Hyper-V" :
                 strncmp(sig, "VMwareVMware", 12) == 0 ? "VMware" : strncmp(sig, "VBoxVBoxVBox", 12) == 0 ? "VirtualBox" :
                 "ja");
    }
}

/* Grafik: die Intel-GPU kennt der Kernel; sonst die erste Grafikkarte am PCI-Bus */
static void read_gpu(const SysInfo *si)
{
    if (si->gpu[0]) {
        snprintf(gpu_name, sizeof(gpu_name), "%s", si->gpu);
        return;
    }
    PciInfo p;
    for (u64 i = 0; sys_pciinfo(i, &p) == 0; i++) {
        if (p.class_code != 3)
            continue;
        const char *v = p.vendor == 0x8086 ? "Intel" : p.vendor == 0x10DE ? "NVIDIA" : p.vendor == 0x1002 ? "AMD" :
                        p.vendor == 0x15AD ? "VMware" : p.vendor == 0x80EE ? "VirtualBox" : 0;
        if (p.vendor == 0x1234 && p.device == 0x1111)
            snprintf(gpu_name, sizeof(gpu_name), "Standard-VGA (QEMU)");
        else if ((p.vendor == 0x1AF4) && (p.device == 0x1050))
            snprintf(gpu_name, sizeof(gpu_name), "virtio-GPU");
        else if (v)
            snprintf(gpu_name, sizeof(gpu_name), "%s-Grafik (%04x:%04x)", v, p.vendor, p.device);
        else
            snprintf(gpu_name, sizeof(gpu_name), "Grafikkarte %04x:%04x", p.vendor, p.device);
        return;
    }
    snprintf(gpu_name, sizeof(gpu_name), "Framebuffer der Firmware");
}

/* "Oct  6 2026 14:03:12" (__DATE__ __TIME__) -> "6. Oktober 2026" */
static void build_date(const char *b, char *out, int max)
{
    static const char *en[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *de[] = {"Januar", "Februar", "M\xC3\xA4rz", "April", "Mai", "Juni", "Juli", "August",
                               "September", "Oktober", "November", "Dezember"};
    int m = -1;
    for (int i = 0; i < 12; i++)
        if (strncmp(b, en[i], 3) == 0)
            m = i;
    if (m < 0 || strlen(b) < 11) {
        snprintf(out, max, "%s", b);
        return;
    }
    int day = atoi(b + 4);
    snprintf(out, max, "%d. %s %.4s", day, de[m], b + 7);
}

/* Bytes als "16 GB" / "512 MB"; round: auf ganze GB (bzw. 64 MB) aufrunden - die Firmware behaelt einen Teil */
static void size_txt(char *out, int max, u64 bytes, int round)
{
    const u64 MB = 1024ull * 1024, GB = MB * 1024;
    if (round && bytes >= GB)
        snprintf(out, max, "%llu GB", (unsigned long long)((bytes + GB - 1) / GB));
    else if (round)
        snprintf(out, max, "%llu MB", (unsigned long long)((bytes + 64 * MB - 1) / (64 * MB) * 64));
    else if (bytes >= 10 * GB)
        snprintf(out, max, "%llu GB", (unsigned long long)((bytes + GB / 2) / GB));
    else if (bytes >= GB)
        snprintf(out, max, "%llu,%llu GB", (unsigned long long)(bytes / GB), (unsigned long long)(bytes % GB * 10 / GB));
    else
        snprintf(out, max, "%llu MB", (unsigned long long)(bytes / MB));
}

/* Wert ab x, hoechstens w breit; zu lang: an einem Leerzeichen umbrechen. Ergebnis: Zahl der Zeilen */
static int draw_value(Surface *s, int x, int y, int w, const char *t, u32 c)
{
    char line[96];
    int lines = 0, lh = text_height(font_ui, FS);
    while (*t) {
        int n = (int)strlen(t), cut = n;
        if (n > (int)sizeof(line) - 1)
            n = cut = sizeof(line) - 1;
        memcpy(line, t, n);
        line[n] = 0;
        while (text_width(font_ui, FS, line) > w) {
            int sp = cut - 1;
            while (sp > 0 && line[sp] != ' ')
                sp--;
            if (sp <= 0)
                break;
            cut = sp;
            line[cut] = 0;
        }
        text_draw(s, font_ui, FS, x, y + lines * lh, line, c);
        lines++;
        t += cut;
        while (*t == ' ')
            t++;
    }
    return lines ? lines : 1;
}

static void draw(void)
{
    Surface *s = &gfx_screen;
    gfx_fill(s, 0, 0, s->w, s->h, C_WINDOW);
    int cx = s->w / 2;

    int isz = U(104), ty = U(28);
    ui_app_icon(s, ICON_ABOUT, cx - isz / 2, ty, isz);
    ty += isz + U(14);
    text_draw(s, font_bold, U(26), cx - text_width(font_bold, U(26), "MiniKernel") / 2, ty, "MiniKernel", C_TEXT);
    ty += text_height(font_bold, U(26));
    text_draw(s, font_ui, FS, cx - text_width(font_ui, FS, os_txt) / 2, ty, os_txt, C_TEXT2);
    ty += text_height(font_ui, FS) + U(22);

    SysInfo si;
    if (sys_sysinfo(&si) != 0)
        memset(&si, 0, sizeof(si));
    int cpus = 0;
    CpuInfo ci;
    while (sys_cpuinfo((u64)cpus, &ci) == 0)
        cpus++;

    enum { ROWS = 9 };
    const char *lab[ROWS];
    char val[ROWS][96];
    int n = 0;
    lab[n] = "Prozessor";
    snprintf(val[n++], sizeof(val[0]), "%s", cpu_name);
    lab[n] = "Kerne";
    snprintf(val[n++], sizeof(val[0]), "%d", cpus);
    char a[24], b[24];
    size_txt(a, sizeof(a), si.mem_total, 1);
    size_txt(b, sizeof(b), si.mem_free, 0);
    lab[n] = "Arbeitsspeicher";
    snprintf(val[n++], sizeof(val[0]), "%s (%s frei)", a, b);
    lab[n] = "Grafik";
    snprintf(val[n++], sizeof(val[0]), "%s", gpu_name);
    lab[n] = "Bildschirm";
    {
        VideoInfo vi;
        unsigned hz = 0;
        for (u64 i = 0; sys_videoinfo(i, &vi) == 0; i++)
            if (vi.current && vi.width == (unsigned)scr_w && vi.height == (unsigned)scr_h)
                hz = vi.hz100;
        if (hz)
            snprintf(val[n++], sizeof(val[0]), "%d \xC3\x97 %d, %u Hz", scr_w, scr_h, (hz + 50) / 100);
        else
            snprintf(val[n++], sizeof(val[0]), "%d \xC3\x97 %d", scr_w, scr_h);
    }
    u64 fs[2];
    if (sys_statfs("/disk", fs) == 0 && fs[0]) {
        size_txt(a, sizeof(a), fs[0], 0);
        size_txt(b, sizeof(b), fs[1], 0);
        lab[n] = "Festplatte";
        snprintf(val[n++], sizeof(val[0]), "%s (%s frei)", a, b);
    }
    if (vm_name[0]) {
        lab[n] = "Virtuelle Maschine";
        snprintf(val[n++], sizeof(val[0]), "%s", vm_name);
    }
    lab[n] = "Kernel-Build";
    build_date(si.build, a, sizeof(a));
    snprintf(val[n++], sizeof(val[0]), "%s", si.build[0] ? a : "unbekannt");
    u64 up = (u64)sys_ticks() / 100;
    lab[n] = "L\xC3\xA4uft seit";
    if (up >= 86400)
        snprintf(val[n++], sizeof(val[0]), "%llu d %llu:%02llu:%02llu", (unsigned long long)(up / 86400),
                 (unsigned long long)(up / 3600 % 24), (unsigned long long)(up / 60 % 60), (unsigned long long)(up % 60));
    else
        snprintf(val[n++], sizeof(val[0]), "%llu:%02llu:%02llu", (unsigned long long)(up / 3600),
                 (unsigned long long)(up / 60 % 60), (unsigned long long)(up % 60));

    /* zwei Spalten wie bei macOS: Namen rechtsbuendig bis zur Mitte, Werte links ab der Mitte */
    int split = s->w * 2 / 5, gap = U(12), lh = text_height(font_ui, FS);
    for (int i = 0; i < n; i++) {
        text_draw(s, font_bold, FS, split - text_width(font_bold, FS, lab[i]), ty, lab[i], C_TEXT);
        int lines = draw_value(s, split + gap, ty, s->w - split - gap - U(24), val[i], C_TEXT2);
        ty += lines * lh + U(5);
    }

    const char *foot = "Eigener 64-Bit-Kernel mit UEFI-Bootloader";
    text_draw(s, font_ui, FS_SMALL, cx - text_width(font_ui, FS_SMALL, foot) / 2, s->h - U(34), foot, C_TEXT2);
    gfx_present_all();
}

void _start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    ui_setup(0);
    if (gfx_open_window(U(520), U(490), "\xC3\x9C" "ber MiniKernel") != 0)
        sys_exit(1);
    if (!gfx_windowed())
        ui_setup(0);
    gfx_display_size(&scr_w, &scr_h);
    read_cpu();
    SysInfo si;
    if (sys_sysinfo(&si) != 0)
        memset(&si, 0, sizeof(si));
    read_gpu(&si);
    snprintf(os_txt, sizeof(os_txt), "Version %s", si.version[0] ? si.version : "1.0");
    draw();
    s64 shown = sys_time();
    for (;;) {
        Event e;
        int got = gfx_wait(&e, 200);
        if (got && (e.type == EV_CLOSE || (e.type == EV_KEY && (e.key == 0x1B || e.key == 'q'))))
            break;
        if (sys_time() != shown) {
            shown = sys_time();
            draw();
        }
    }
    gfx_close();
    sys_exit(0);
}
