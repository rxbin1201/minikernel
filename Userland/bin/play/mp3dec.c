/* Implementierung von minimp3 (siehe mp3.c). Eigene Datei ohne libc.h: minimp3 bindet <string.h>/<stdlib.h> des
 * Compilers ein, deren size_t anders heisst als unseres (beide 64 Bit, gleiche Aufrufkonvention); gelinkt wird gegen
 * memcpy/memset/memmove unserer libc. */

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
