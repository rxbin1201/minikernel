#include "drivers/video.h"
#include "console/console.h"
#include "drivers/gpu/igd.h"

static BootVideoMode modes[BOOT_MAX_MODES];
static unsigned      mode_count, mode_current;
static uint64_t      kernel_size;

void video_init(const BootInfo *info)
{
    mode_count = info->mode_count > BOOT_MAX_MODES ? BOOT_MAX_MODES : info->mode_count;
    for (unsigned i = 0; i < mode_count; i++)
        modes[i] = info->modes[i];
    mode_current = info->mode_current < mode_count ? info->mode_current : 0;
    kernel_size = info->kernel_size;
}

unsigned video_mode_count(void)
{
    return mode_count;
}

/* Mit Intel-Treiber: dessen Modi (im Betrieb umschaltbar, mit Bildrate), sonst die der Firmware (gelten ab Neustart) */
int video_mode_info(unsigned index, VideoInfo *out)
{
    if (igd_mode_count() > 0) {
        int cur;
        if (igd_mode_info((int)index, &out->width, &out->height, &out->hz100, &cur) != 0)
            return -1;
        out->current = (uint32_t)cur;
    } else {
        if (index >= mode_count)
            return -1;
        out->width = modes[index].width;
        out->height = modes[index].height;
        out->current = index == mode_current;
        out->hz100 = 0;
    }
    out->scale = console_scale();
    out->cols = console_cols();
    out->rows = console_rows();
    out->kernel_size = kernel_size;
    return 0;
}
