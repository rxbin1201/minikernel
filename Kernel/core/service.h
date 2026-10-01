#ifndef SERVICE_H
#define SERVICE_H

#include <stdint.h>

/* Benannte Dienste (SYS_SERVICE): ein Prozess meldet einen Namen an (z.B. der Desktop "desktop"), andere verbinden sich
 * damit und bekommen je zwei Pipes - wie ein sehr einfacher Unix-Socket. Der Dienst holt wartende Verbindungen mit
 * "annehmen" ab (ohne zu warten). Endet der Anbieter, verschwindet der Dienst; noch nicht angenommene Verbindungen
 * sehen das Ende ihrer Pipes. */

#define SERVICE_NAME_MAX 16

int64_t service_register(const char *name);                  /* 0, ERR_EXIST (vergeben), ERR_NOMEM */
int64_t service_unregister(const char *name);                /* nur der Anbieter */
int64_t service_connect(const char *name, int fds[2]);       /* fds[0] lesen, fds[1] schreiben; ERR_NOENT, ERR_AGAIN (voll) */
int64_t service_accept(const char *name, int out[3]);        /* fds wie oben, out[2] = PID des Gegenuebers; ERR_AGAIN = keine */
void    service_owner_exit(uint32_t pid);                    /* Prozessende: seine Dienste abmelden */

#endif
