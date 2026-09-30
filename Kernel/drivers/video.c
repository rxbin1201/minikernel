#include "drivers/video.h"
#include "console/console.h"

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

int video_mode_info(unsigned index, VideoInfo *out)
{
    if (index >= mode_count)
        return -1;
    out->width = modes[index].width;
    out->height = modes[index].height;
    out->current = index == mode_current;
    out->scale = console_scale();
    out->cols = console_cols();
    out->rows = console_rows();
    out->pad = 0;
    out->kernel_size = kernel_size;
    return 0;
}
