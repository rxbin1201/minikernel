# MiniKernel: UEFI-Bootloader, Kernel und Userland. 'make help' zeigt die Ziele und Optionen.

CC = gcc
LD = ld
AR = ar

.DEFAULT_GOAL := all

# ---------------------------------------------------------------------------------------------------------------------
# Bootloader (UEFI-Anwendung, gnu-efi)
# ---------------------------------------------------------------------------------------------------------------------

BOOTLOADER_SOURCE_FILES := $(shell find Sources -name '*.c')
BOOTLOADER_OBJECT_FILES := $(patsubst Sources/%.c, Build/%.o, $(BOOTLOADER_SOURCE_FILES))

EFI_CFLAGS  = -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -I gnu-efi/inc/ -I Includes/ -DEFI_FUNCTION_WRAPPER
EFI_LDFLAGS = gnu-efi/x86_64/crt0-efi-x86_64.o -nostdlib -znocombreloc -T gnu-efi/x86_64/elf_x86_64_efi.lds -shared \
              -Bsymbolic -L gnu-efi/x86_64/ -l:libgnuefi.a -l:libefi.a -z noexecstack

$(BOOTLOADER_OBJECT_FILES): Build/%.o : Sources/%.c Includes/boot_info.h
	@mkdir -p $(dir $@)
	$(CC) $(EFI_CFLAGS) -c $< -o $@

Image/bootx64.efi: $(BOOTLOADER_OBJECT_FILES)
	@mkdir -p $(dir $@)
	$(LD) $(BOOTLOADER_OBJECT_FILES) $(EFI_LDFLAGS) -o Build/bootx64.so
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64 \
	    Build/bootx64.so $@

# ---------------------------------------------------------------------------------------------------------------------
# Kernel (Quellen in Unterordnern von Kernel/, Includes relativ zu Kernel/, z.B. #include "mm/heap.h")
# ---------------------------------------------------------------------------------------------------------------------

KERNEL_C_FILES := $(shell find Kernel -name '*.c')
KERNEL_S_FILES := $(shell find Kernel -name '*.S')
KERNEL_OBJECT_FILES := $(patsubst Kernel/%.c, Build/kernel/%.o, $(KERNEL_C_FILES)) \
                       $(patsubst Kernel/%.S, Build/kernel/%.o, $(KERNEL_S_FILES))

# -fno-tree-loop-distribute-patterns: gcc soll keine Schleifen durch memset/memcpy-Aufrufe ersetzen (auch nicht in string.c)
# -fno-omit-frame-pointer: rbp-Kette fuer Backtraces bei Exceptions (lib/ksyms.c)
KERNEL_CFLAGS  = -O2 -g -Wall -Wextra -ffreestanding -fno-tree-loop-distribute-patterns -fno-omit-frame-pointer \
                 -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-mmx -MMD -MP -I Includes/ -I Kernel/ \
                 $(KERNEL_EXTRA_CFLAGS)
KERNEL_LDFLAGS = -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T Kernel/kernel.ld

Build/kernel/%.o: Kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

Build/kernel/%.o: Kernel/%.S
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

-include $(KERNEL_OBJECT_FILES:.o=.d)

# Symboltabelle fuer Backtraces, in zwei Durchgaengen: erst mit leerer Tabelle linken, daraus die Tabelle erzeugen und
# erneut linken. Sie liegt in .rodata hinter .text, die Funktionsadressen bleiben gleich; das wird nachgeprueft.
Build/ksyms/empty.c: tools/mksyms.py
	@mkdir -p $(dir $@)
	python3 tools/mksyms.py --empty $@

Build/ksyms/%.o: Build/ksyms/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

Build/ksyms/pass1.elf: $(KERNEL_OBJECT_FILES) Build/ksyms/empty.o Kernel/kernel.ld
	$(LD) $(KERNEL_OBJECT_FILES) Build/ksyms/empty.o $(KERNEL_LDFLAGS) -o $@

Build/ksyms/table.c: Build/ksyms/pass1.elf tools/mksyms.py
	python3 tools/mksyms.py $< $@

# Build/kernel.debug.elf behaelt die Debug-Infos (z.B. fuer gdb oder addr2line), Image/kernel.elf wird gebootet
Build/kernel.debug.elf: $(KERNEL_OBJECT_FILES) Build/ksyms/table.o Kernel/kernel.ld
	$(LD) $(KERNEL_OBJECT_FILES) Build/ksyms/table.o $(KERNEL_LDFLAGS) -o $@
	@python3 tools/mksyms.py $@ Build/ksyms/check.c && cmp -s Build/ksyms/table.c Build/ksyms/check.c || \
	    { echo "Symboltabelle passt nicht zu den Funktionsadressen"; rm -f $@; exit 1; }

Image/kernel.elf: Build/kernel.debug.elf
	@mkdir -p $(dir $@)
	objcopy --strip-debug $< $@

# ---------------------------------------------------------------------------------------------------------------------
# Userland: jede Userland/bin/NAME.c (oder alle .c in Userland/bin/NAME/) wird zum Programm /bin/NAME in der initrd
# (tar); Initrd/ wird 1:1 dazukopiert.
# Gemeinsamer Code liegt in Userland/lib/ (libuser.a: nur benutzte Teile landen im Programm), Header in Userland/include/.
# ---------------------------------------------------------------------------------------------------------------------

# statisch, bei USER_BASE gelinkt; -fpie erzeugt RIP-relativen Code (Adresse liegt ueber 2 GiB)
USER_CFLAGS  = -O2 -Wall -Wextra -ffreestanding -fno-tree-loop-distribute-patterns -fpie -fno-stack-protector \
               -mno-red-zone -U_FORTIFY_SOURCE -fno-math-errno -MMD -MP -I Userland/include/
USER_LDFLAGS = -nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -T Userland/user.ld

USER_PROGS    := $(patsubst Userland/bin/%.c,%,$(wildcard Userland/bin/*.c))
USER_DIRPROGS := $(patsubst Userland/bin/%/,%,$(wildcard Userland/bin/*/))
USER_BINS     := $(addprefix Build/initrd/bin/,$(USER_PROGS) $(USER_DIRPROGS))
USER_SOURCES  := $(wildcard Userland/bin/*.c Userland/bin/*/*.c Userland/lib/*.c)
USER_LIB_OBJS := $(patsubst Userland/lib/%.c,Build/user/lib/%.o,$(wildcard Userland/lib/*.c))
USER_LIB      := Build/user/libuser.a
INITRD_FILES  := $(shell find Initrd -type f 2>/dev/null)
# Firmware fuer Geraete (z.B. Intel WLAN/Bluetooth, aus linux-firmware): liegt sie in firmware/, kommt sie nach
# /firmware in der initrd. Nicht im Repository (Lizenz von Intel, Binaerdateien) - siehe README
FIRMWARE_FILES := $(wildcard firmware/*)

Build/user/%.o: Userland/%.c
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

-include $(patsubst Userland/%.c,Build/user/%.d,$(USER_SOURCES))

$(USER_LIB): $(USER_LIB_OBJS)
	@rm -f $@
	$(AR) rcs $@ $^

Build/initrd/bin/%: Build/user/bin/%.o $(USER_LIB) Userland/user.ld
	@mkdir -p $(dir $@)
	$(LD) $< $(USER_LIB) $(USER_LDFLAGS) -o $@

# Programme aus mehreren Dateien
define USER_DIRPROG
Build/initrd/bin/$(1): $(patsubst Userland/%.c,Build/user/%.o,$(wildcard Userland/bin/$(1)/*.c)) $(USER_LIB) Userland/user.ld
	@mkdir -p $$(dir $$@)
	$(LD) $$(filter %.o,$$^) $(USER_LIB) $(USER_LDFLAGS) -o $$@
endef
$(foreach p,$(USER_DIRPROGS),$(eval $(call USER_DIRPROG,$(p))))

Image/initrd.tar: $(USER_BINS) $(INITRD_FILES) $(FIRMWARE_FILES)
	@mkdir -p $(dir $@)
	cp -r Initrd/. Build/initrd/
	rm -rf Build/initrd/firmware
	$(if $(FIRMWARE_FILES),mkdir -p Build/initrd/firmware && cp $(FIRMWARE_FILES) Build/initrd/firmware/)
	tar --format=ustar --owner=0 --group=0 -cf $@ -C Build/initrd .

# ---------------------------------------------------------------------------------------------------------------------
# Bauen
# ---------------------------------------------------------------------------------------------------------------------

.PHONY: all build
all build: Image/bootx64.efi Image/kernel.elf Image/initrd.tar

# ---------------------------------------------------------------------------------------------------------------------
# In QEMU starten
# ---------------------------------------------------------------------------------------------------------------------

DISK ?= virtio
DISK_LAYOUT ?= none
TESTS ?=
KEEP ?=
CMDLINE ?=
NET ?= e1000
SOUND ?= none
STICK ?=
MOUSE ?= 1
SMP ?= 4
QEMU_EXTRA ?=
QEMU_GDB ?=
HEADLESS ?=
OVMF ?= /usr/share/ovmf/OVMF.fd
# KVM: automatisch, wenn /dev/kvm fuer den Benutzer beschreibbar ist (sonst reine Emulation); KVM=0 schaltet es ab
KVM ?= $(shell test -w /dev/kvm && echo 1)

KERNEL_CMDLINE := $(strip $(if $(TESTS),$(if $(filter 1,$(TESTS)),selftest,selftest=$(TESTS))) $(if $(KEEP),keep) $(CMDLINE))
LOG := Build/Out.log

# Datenplatte (FAT32, 64 MiB). Bleibt bei 'make clean' erhalten, damit Dateien Neustarts ueberleben;
# 'make cleandisk' setzt sie zurueck.
Image/disk.img:
	@mkdir -p $(dir $@)
	python3 tools/mkdisk.py $@ 64 --layout $(DISK_LAYOUT)

# Boot-Platte fuer QEMU: GPT + EFI-Systempartition mit Bootloader, Kernel, initrd und Kommandozeile. Sie haengt am
# IDE-Controller, fuer den der Kernel keinen Treiber hat, und heisst KERNELBOOT (der Kernel nimmt nur MINIKERNEL).
Build/esp.img: Image/bootx64.efi Image/kernel.elf Image/initrd.tar FORCE
	python3 tools/mkesp.py $@ Image/bootx64.efi Image/kernel.elf Image/initrd.tar 64 \
	    $(if $(KERNEL_CMDLINE),--cmdline "$(KERNEL_CMDLINE)") > /dev/null

# Test-Stick fuer die Selbsttests "foreign" (tools/mkstick.py), als zusaetzlicher USB-Stick am xHCI
Build/stick-%.img: tools/mkstick.py
	@mkdir -p $(dir $@)
	python3 tools/mkstick.py $@ --fat $*

QEMU_DISK_virtio = -device virtio-blk-pci,drive=hd0,disable-modern=on
QEMU_DISK_nvme   = -device nvme,drive=hd0,serial=minikernel
QEMU_DISK_ahci   = -device ich9-ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0
QEMU_DISK_usb    = -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd0

QEMU_ACCEL := $(if $(filter 1,$(KVM)),-enable-kvm -cpu host)
QEMU_NET   := $(if $(filter none,$(NET)),,-netdev user,id=net0 -device $(NET),netdev=net0,romfile=)
# Ton: Intel HD Audio mit einem Codec (Line-Out). SOUND=none spielt ins Leere (laeuft aber in Echtzeit), SOUND=wav
# schreibt nach Build/sound.wav, SOUND=pa spielt ueber PulseAudio (WSLg: Windows-Lautsprecher), SOUND=off ohne Karte
QEMU_SOUND  = $(if $(filter off,$(SOUND)),,-audiodev $(if $(filter wav,$(SOUND)),wav$(,)id=snd0$(,)path=Build/sound.wav,$(SOUND)$(,)id=snd0) \
              -device intel-hda -device hda-output$(,)audiodev=snd0$(if $(HDA_TIMER),$(,)use-timer=$(HDA_TIMER)))
# HEADLESS=1: ohne Fenster (Ausgabe nur in $(LOG)), z.B. fuer Tests im Hintergrund oder in CI
QEMU_DISPLAY := $(if $(filter 1,$(HEADLESS)),-display none)
# Mit Selbsttests nicht neu starten: ein Absturz (Triple Fault) wuerde sonst still einen neuen Lauf beginnen
QEMU_REBOOT  := $(if $(TESTS),-no-reboot)
,          := ,
QEMU_XHCI  := $(if $(filter usb,$(DISK)),,-device qemu-xhci$(,)id=xhci)
QEMU_STICK := $(if $(STICK),$(QEMU_XHCI) -drive file=Build/stick-$(STICK).img$(,)format=raw$(,)if=none$(,)id=stick \
              -device usb-storage$(,)bus=xhci.0$(,)drive=stick)
# Maus: der Kernel kennt nur USB-Maeuse (PS/2-Mausdaten verwirft er). usb-tablet meldet absolute Koordinaten - der
# Zeiger folgt der Maus des Rechners, ohne sie im Fenster einzufangen. Nicht bei den Selbsttests (die bewegen die Maus
# selbst) und nicht mit MOUSE=0. Ein xHCI-Controller kommt dazu, wenn ihn nicht schon DISK=usb oder STICK anlegt.
QEMU_MOUSE := $(if $(TESTS)$(filter 0,$(MOUSE)),,$(if $(STICK),,$(QEMU_XHCI)) -device usb-tablet$(,)bus=xhci.0)

.PHONY: run efi
run efi: Build/esp.img Image/disk.img $(if $(STICK),Build/stick-$(STICK).img)
	qemu-system-x86_64 -bios $(OVMF) $(QEMU_ACCEL) -smp $(SMP) -m 512 -rtc base=localtime -serial file:$(LOG) $(QEMU_DISPLAY) \
	    -drive file=Build/esp.img,format=raw,index=0,media=disk \
	    -drive file=Image/disk.img,format=raw,if=none,id=hd0 $(QEMU_DISK_$(DISK)) \
	    $(QEMU_NET) $(QEMU_SOUND) $(QEMU_STICK) $(QEMU_MOUSE) $(QEMU_GDB) $(QEMU_REBOOT) $(QEMU_EXTRA)

# Debuggen mit gdb: 'make debug' startet QEMU angehalten mit gdb-Server auf Port 1234 (ohne KVM, damit normale
# Breakpoints gehen), 'make gdb' in einem zweiten Terminal verbindet sich (tools/gdbinit: Symbole, Breakpoint kmain).
.PHONY: debug gdb
debug:
	@echo "QEMU wartet auf gdb (Port 1234): in einem zweiten Terminal 'make gdb'"
	@$(MAKE) --no-print-directory run KVM=0 QEMU_GDB="-s -S"

gdb: Build/kernel.debug.elf
	gdb -q -x tools/gdbinit

# Selbsttests starten und das Ergebnis zusammenfassen (Exit-Code 1 bei Fehlern)
.PHONY: test
test:
	@$(MAKE) --no-print-directory run TESTS=$(or $(TESTS),1)
	@sed 's/\x1b\[[0-9;]*m//g' $(LOG) | grep -a ': FEHLER' || true
	@sum=$$(grep -a '^Selbsttests: .* OK, .* FEHLER' $(LOG)); \
	    if [ -z "$$sum" ]; then echo "Selbsttests nicht vollstaendig durchgelaufen (Protokoll: $(LOG))"; exit 1; fi; \
	    echo "$$sum (Protokoll: $(LOG))"; echo "$$sum" | grep -q ' 0 FEHLER'

# Datenplatte auf Konsistenz pruefen
.PHONY: fatcheck
fatcheck: Image/disk.img
	python3 tools/fatcheck.py Image/disk.img

# ---------------------------------------------------------------------------------------------------------------------
# Images fuer echte Hardware und VMware
# ---------------------------------------------------------------------------------------------------------------------

VMWARE_BUS ?= sata
.PHONY: vmware
vmware: all Image/disk.img
	mkdir -p Image/vmware
	python3 tools/mkesp.py Image/vmware/boot.img Image/bootx64.efi Image/kernel.elf Image/initrd.tar 64 $(if $(KERNEL_CMDLINE),--cmdline "$(KERNEL_CMDLINE)")
	qemu-img convert -f raw -O vmdk Image/vmware/boot.img Image/vmware/boot.vmdk
	qemu-img convert -f raw -O vmdk Image/disk.img Image/vmware/data.vmdk
	rm -f Image/vmware/boot.img
	python3 tools/mkvmx.py Image/vmware/minikernel.vmx --bus $(VMWARE_BUS)

.PHONY: usb
usb: all
	mkdir -p Image/usb
	python3 tools/mkesp.py Image/usb/minikernel-usb.img Image/bootx64.efi Image/kernel.elf Image/initrd.tar 64 $(if $(KERNEL_CMDLINE),--cmdline "$(KERNEL_CMDLINE)")

.PHONY: usbfiles
usbfiles: all
	rm -rf Image/usbfiles
	mkdir -p Image/usbfiles/EFI/BOOT
	cp Image/bootx64.efi Image/usbfiles/EFI/BOOT/BOOTX64.EFI
	cp Image/kernel.elf Image/usbfiles/KERNEL.ELF
	cp Image/initrd.tar Image/usbfiles/INITRD.TAR
	$(if $(KERNEL_CMDLINE),printf '%s\n' "$(KERNEL_CMDLINE)" > Image/usbfiles/cmdline.txt)

# ---------------------------------------------------------------------------------------------------------------------
# Schriften (Kernel/console/font*.c aus den Konsolenschriften des Systems erzeugen)
# ---------------------------------------------------------------------------------------------------------------------

.PHONY: fonts
fonts:
	python3 tools/gen_font.py
	python3 tools/gen_font_ext.py

# ---------------------------------------------------------------------------------------------------------------------
# Aufraeumen, Hilfe
# ---------------------------------------------------------------------------------------------------------------------

.PHONY: clean
clean:
	rm -rf Build Image/bootx64.efi Image/kernel.elf Image/initrd.tar Image/cmdline.txt Image/vmware

.PHONY: cleandisk
cleandisk:
	rm -f Image/disk.img

.PHONY: help
help:
	@echo "Ziele:"
	@echo "  make              Bootloader, Kernel und initrd bauen (Image/)"
	@echo "  make run          bauen und in QEMU starten (Log der seriellen Schnittstelle: $(LOG))"
	@echo "  make test         Selbsttests in QEMU, danach Zusammenfassung (TESTS=disk,net: nur diese Gruppen)"
	@echo "  make debug        QEMU angehalten mit gdb-Server starten; 'make gdb' im zweiten Terminal verbindet sich"
	@echo "  make fatcheck     Datenplatte Image/disk.img pruefen"
	@echo "  make usb          Image fuer einen USB-Stick (Image/usb/minikernel-usb.img, mit Rufus/Etcher schreiben)"
	@echo "  make usbfiles     Dateien zum Kopieren auf einen FAT32-Stick (Image/usbfiles/)"
	@echo "  make vmware       VMware-VM nach Image/vmware/ (VMWARE_BUS=sata|nvme)"
	@echo "  make fonts        Kernel/console/font*.c neu erzeugen"
	@echo "  make clean        Build-Ergebnisse loeschen (Datenplatte bleibt)"
	@echo "  make cleandisk    Datenplatte loeschen (wird beim naechsten Start neu angelegt)"
	@echo ""
	@echo "Optionen fuer run/test:"
	@echo "  DISK=virtio|nvme|ahci|usb   Controller der Datenplatte (Standard virtio)"
	@echo "  DISK_LAYOUT=none|mbr|gpt    Partitionierung beim Neuanlegen der Datenplatte"
	@echo "  NET=e1000|e1000e|none       Netzwerkkarte (QEMU-User-Netz, Gast 10.0.2.15)"
	@echo "  SOUND=none|wav|pa|off       Ton: ins Leere, nach Build/sound.wav, ueber PulseAudio, ohne Soundkarte"
	@echo "  MOUSE=0                     ohne USB-Maus (Standard: usb-tablet, nicht bei den Selbsttests)"
	@echo "  SMP=4                       Anzahl CPUs in QEMU (Standard 4)"
	@echo "  STICK=12|16|exfat           zusaetzlichen Test-Stick anschliessen (tools/mkstick.py)"
	@echo "  TESTS=1 | TESTS=disk,user   Selbsttests beim Start; KEEP=1 bleibt danach im System"
	@echo "  CMDLINE=\"mode=1600x900\"     weitere Kernel-Kommandozeile (mode=, scale=, kbd=, init=, ...)"
	@echo "  KVM=0                       Hardware-Virtualisierung abschalten (Standard: an, wenn /dev/kvm nutzbar)"
	@echo "  HEADLESS=1                  ohne Fenster (Ausgabe nur in $(LOG))"
	@echo "  QEMU_EXTRA=\"...\"            weitere QEMU-Argumente"

.PHONY: FORCE
FORCE:

# Objektdateien der Programme behalten (sonst loescht make sie als Zwischenergebnisse nach jedem Lauf)
.SECONDARY: $(patsubst Userland/%.c,Build/user/%.o,$(USER_SOURCES))
