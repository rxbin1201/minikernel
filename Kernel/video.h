#ifndef VIDEO_H
#define VIDEO_H

#include "boot_info.h"
#include "syscall.h" /* VideoInfo */

/* Grafikmodi, die der Bootloader gefunden hat. Umschalten kann nur der Bootloader (vor dem Kernel): "mode=1920x1080"
 * oder "mode=max" in \cmdline.txt, gilt ab dem naechsten Start (siehe Programm "resolution"). */
void video_init(const BootInfo *info);
unsigned video_mode_count(void);
int  video_mode_info(unsigned index, VideoInfo *out); /* 0 oder -1 */

#endif
