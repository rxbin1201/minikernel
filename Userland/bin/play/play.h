#ifndef PLAY_H
#define PLAY_H

/* play: WAV (main.c) und MP3 (mp3.c) */
int is_mp3(int fd);                                            /* Datei beginnt mit ID3-Tag oder MPEG-Frame */
int play_mp3(const char *file, int fd, const char *wav_out);   /* wav_out != 0: in WAV umwandeln statt spielen */

#endif
