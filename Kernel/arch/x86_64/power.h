#ifndef POWER_H
#define POWER_H

/* Ausschalten und Neustart. Vorher werden die Schreibcaches aller Blockgeraete geleert (das FAT-Dateisystem schreibt
 * ohnehin sofort durch). Beide Funktionen kehren nicht zurueck. Laufende Prozesse werden nicht beendet. */

void power_off(void) __attribute__((noreturn));
void power_reboot(void) __attribute__((noreturn));

#endif
