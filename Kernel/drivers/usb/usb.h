#ifndef USB_H
#define USB_H

#include <stdint.h>

/* USB-Stapel: xHCI-Controller (deckt USB 1.x/2.0/3.x-Geraete an den Root-Ports ab), darauf Klassentreiber fuer HID-Tastaturen
 * (Boot-Protokoll) und Massenspeicher (Bulk-Only-Transport mit SCSI, als Blockgeraet).
 *
 * Hubs (USB 2.0 und 3.x, auch verschachtelt bis 5 Ebenen) werden abgefragt (Polling, kein Status-Interrupt-Endpunkt).
 * Nicht enthalten: UHCI/OHCI/EHCI-Controller, Isochron-Endpunkte. */

typedef struct UsbDevice UsbDevice;

/* Geschwindigkeiten (wie im xHCI-Port-Status) */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4
#define USB_SPEED_SUPER_PLUS 5

/* Endpunkt-Beschreibung aus dem Konfigurationsdeskriptor */
typedef struct {
    uint8_t  address;   /* Bit 7 = IN, Bits 3:0 = Endpunktnummer */
    uint8_t  attrs;     /* Bits 1:0: 0 Control, 1 Isochron, 2 Bulk, 3 Interrupt */
    uint16_t max_packet;
    uint8_t  interval;
} UsbEpDesc;

/* Schnittstelle (Interface) mit ihren Endpunkten */
typedef struct {
    uint8_t   number;
    uint8_t   cls, sub, proto;
    UsbEpDesc eps[8];
    int       ep_count;
} UsbIface;

/* ---- Dienste des Controllers fuer die Klassentreiber (nur mit gehaltener Controller-Sperre aufrufen; die Probe-
 * ---- Funktionen und die Callbacks laufen bereits darin, Blockgeraete-Zugriffe nehmen sie mit usb_lock/usb_unlock) ---- */

/* Control-Transfer auf Endpunkt 0. Liefert die Zahl der uebertragenen Bytes (bei IN) oder negativ (-Completion-Code). */
int usb_control(UsbDevice *d, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *buf, uint16_t len);

/* Endpunkte konfigurieren (Configure-Endpoint-Kommando). 0 = ok. */
int usb_add_endpoints(UsbDevice *d, const UsbEpDesc *eps, int count);

/* Bulk-Transfer; buf muss DMA-Speicher (physisch = virtuell) sein. Liefert die Zahl der Bytes oder negativ
 * (-Completion-Code, -1 Timeout, -2 Geraet weg). */
int usb_bulk(UsbDevice *d, uint8_t endpoint_address, void *buf, uint32_t len, uint32_t timeout_ms);

/* Nach einem Stall: Endpunkt zuruecksetzen und ENDPOINT_HALT loeschen. 0 = ok. */
int usb_endpoint_recover(UsbDevice *d, uint8_t endpoint_address);

/* Wiederkehrender Interrupt-IN-Transfer; callback(dev, buf, actual_len) laeuft im Controller-Kontext (nicht blockieren!). */
typedef void (*UsbInterruptCallback)(UsbDevice *d, const uint8_t *buf, uint32_t len);
int usb_interrupt_start(UsbDevice *d, uint8_t endpoint_address, void *buf, uint32_t len, UsbInterruptCallback callback);

void usb_lock(UsbDevice *d);
void usb_unlock(UsbDevice *d);
int  usb_alive(const UsbDevice *d);

/* ---- Klassentreiber (von der Enumeration aufgerufen) ---- */
int usb_hid_probe(UsbDevice *d, const UsbIface *iface);
void    usb_hid_tick(void);           /* Tastenwiederholung (vom USB-Thread) */
int64_t usb_hid_next_repeat_ms(void); /* ms bis zur naechsten faelligen Wiederholung, -1 = keine Taste gehalten */
uint64_t usb_irq_count(void);         /* empfangene xHCI-Interrupts (MSI-X/MSI) */
int usb_mouse_probe(UsbDevice *d, const UsbIface *iface);
int usb_msc_probe(UsbDevice *d, const UsbIface *iface, const char *name_hint);

/* ---- Oeffentlich ---- */

/* Sucht xHCI-Controller (PCI muss gescannt sein, Scheduler laeuft), initialisiert sie und ihre angeschlossenen Geraete
 * und startet den Hintergrund-Thread fuer Ereignisse und Hot-Plug. Liefert die Zahl der Controller. */
int usb_init(void);

/* Auskunft (lsusb) */
typedef struct {
    uint32_t vid, pid;
    uint32_t cls, speed, port, slot;
    uint32_t driver; /* 0 keiner, 1 HID-Tastatur, 2 Massenspeicher, 3 Hub, 4 Maus */
    char     name[32];
    char     path[16];  /* "1" = Root-Port 1, "1.3" = Port 3 des Hubs an Root-Port 1 */
} UsbInfo;
int usb_device_info(unsigned index, UsbInfo *out); /* 0 oder -2 am Ende */

#endif
