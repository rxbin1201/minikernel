#ifndef SOUND_H
#define SOUND_H

/* Toene fuer Programme (Spiele, Desktop): eine eigene Stimme im Mischer des Kernels, 48 kHz Mono. Was gleichzeitig
 * laeuft (z.B. Musik mit play), wird dazugemischt. Ohne Soundkarte tun alle Funktionen nichts.
 * snd_tone haengt an das an, was noch gespielt wird; blockiert nur, wenn schon ueber 0,3 s anstehen. */

int  snd_open(void);                       /* 0 = Ton verfuegbar */
void snd_tone(int hz, int ms, int volume); /* Sinus mit weichem Ein-/Ausklang; volume 0-100 */
void snd_rest(int ms);                     /* Pause zwischen Toenen */
void snd_close(void);

#endif
