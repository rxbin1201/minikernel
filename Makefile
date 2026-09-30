CC =gcc
LD =ld

BOOTLOADER_SOURCE_FILES := $(shell find Sources -name *.c)
BOOTLOADER_OBJECT_FILES := $(patsubst Sources/%.c, Build/%.o, $(BOOTLOADER_SOURCE_FILES))

EFI_CFLAGS=-fno-stack-protector -fpic -fshort-wchar -mno-red-zone -I gnu-efi/inc/ -I Includes/ -DEFI_FUNCTION_WRAPPER
EFI_LDFLAGS=gnu-efi/x86_64/crt0-efi-x86_64.o -nostdlib -znocombreloc -T gnu-efi/x86_64/elf_x86_64_efi.lds -shared -Bsymbolic -L gnu-efi/x86_64/ -l:libgnuefi.a -l:libefi.a -z noexecstack

KERNEL_C_FILES := $(wildcard Kernel/*.c)
KERNEL_S_FILES := $(wildcard Kernel/*.S)
KERNEL_OBJECT_FILES := $(patsubst Kernel/%.c, Build/kernel/%.o, $(KERNEL_C_FILES)) \
                       $(patsubst Kernel/%.S, Build/kernel/%.o, $(KERNEL_S_FILES))

# -fno-tree-loop-distribute-patterns: gcc soll keine Schleifen durch memset/memcpy-Aufrufe ersetzen (auch nicht in string.c)
KERNEL_CFLAGS=-O2 -g -Wall -Wextra -ffreestanding -fno-tree-loop-distribute-patterns -fno-stack-protector -fno-pic -mno-red-zone -mno-sse -mno-mmx -MMD -MP -I Includes/ -I Kernel/ $(KERNEL_EXTRA_CFLAGS)
KERNEL_LDFLAGS=-nostdlib -static -z max-page-size=0x1000 -z noexecstack -T Kernel/kernel.ld

# User-Programm: statisch, bei USER_BASE gelinkt; -fpie erzeugt RIP-relativen Code (Adresse liegt ueber 2 GiB)
USER_CFLAGS=-O2 -Wall -Wextra -ffreestanding -fno-tree-loop-distribute-patterns -fpie -fno-stack-protector -mno-red-zone -mno-sse -mno-mmx -I Userland/
USER_LDFLAGS=-nostdlib -static -no-pie -z max-page-size=0x1000 -z noexecstack -T Userland/user.ld

# Jede Userland/NAME.c wird zum Programm /bin/NAME in der initrd (tar); Initrd/ wird 1:1 dazukopiert.
USER_PROGS   := $(patsubst Userland/%.c,%,$(wildcard Userland/*.c))
USER_BINS    := $(addprefix Build/initrd/bin/,$(USER_PROGS))
INITRD_FILES := $(shell find Initrd -type f 2>/dev/null)

USER_LIB_OBJ := Build/user/lib/libc.o

Build/user/%.o: Userland/%.c $(wildcard Userland/*.h)
	mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

Build/user/lib/%.o: Userland/lib/%.c Userland/user.h Userland/libc.h
	mkdir -p $(dir $@) && $(CC) $(USER_CFLAGS) -c $< -o $@

Build/initrd/bin/%: Build/user/%.o $(USER_LIB_OBJ) Userland/user.ld
	mkdir -p $(dir $@) && $(LD) $< $(USER_LIB_OBJ) $(USER_LDFLAGS) -o $@

Image/initrd.tar: $(USER_BINS) $(INITRD_FILES)
	cp -r Initrd/. Build/initrd/
	tar --format=ustar --owner=0 --group=0 -cf $@ -C Build/initrd .

Build/kernel/%.o: Kernel/%.c
	mkdir -p $(dir $@) && $(CC) $(KERNEL_CFLAGS) -c $< -o $@

Build/kernel/%.o: Kernel/%.S
	mkdir -p $(dir $@) && $(CC) $(KERNEL_CFLAGS) -c $< -o $@

-include $(KERNEL_OBJECT_FILES:.o=.d)

$(BOOTLOADER_OBJECT_FILES): Build/%.o : Sources/%.c Includes/boot_info.h
	mkdir -p $(dir $@) && \
	$(CC) $(EFI_CFLAGS) -c $(patsubst Build/%.o, Sources/%.c, $@) -o $@

.PHONY: build
build: $(BOOTLOADER_OBJECT_FILES) $(KERNEL_OBJECT_FILES) Image/initrd.tar
	$(LD) $(BOOTLOADER_OBJECT_FILES) $(EFI_LDFLAGS) -o Build/bootx64.so && \
	objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela -j .reloc --target=efi-app-x86_64 Build/bootx64.so Image/bootx64.efi && \
	$(LD) $(KERNEL_OBJECT_FILES) $(KERNEL_LDFLAGS) -o Image/kernel.elf

# Daten-Platte (FAT32, 64 MiB). Bleibt bei 'make clean' erhalten, damit Dateien Neustarts ueberleben;
# 'make cleandisk' setzt sie zurueck.
#   DISK=virtio|nvme|ahci|usb an welchem Controller die Platte in QEMU haengt (Standard virtio)
#   DISK_LAYOUT=none|mbr|gpt  Partitionierung des Images (nur beim Neuanlegen, siehe tools/mkdisk.py)
DISK ?= virtio
TESTS ?=        # TESTS=1: alle Selbsttests beim Start, danach schaltet der Kernel aus; TESTS=editor,disk: nur diese Gruppen
CMDLINE ?=      # weitere Kernel-Kommandozeile, z.B. CMDLINE="init=/bin/poweroff"
KEEP ?=         # KEEP=1 (mit TESTS=1): nach den Selbsttests im System bleiben statt auszuschalten
KERNEL_CMDLINE := $(strip $(if $(TESTS),$(if $(filter 1,$(TESTS)),selftest,selftest=$(TESTS))) $(if $(KEEP),keep) $(CMDLINE))
QEMU_EXTRA ?=   # weitere QEMU-Argumente, z.B. QEMU_EXTRA="-cpu max"
# Netzwerkkarte in QEMU: NET=e1000 (Standard, 82540EM), NET=e1000e (82574L) oder NET=none. Angeschlossen an QEMUs
# eingebautes Netz ("user"): Router/DHCP 10.0.2.2, DNS 10.0.2.3, der Gast bekommt 10.0.2.15.
NET ?= e1000
QEMU_NET := $(if $(filter none,$(NET)),,-netdev user,id=net0 -device $(NET),netdev=net0,romfile=)
DISK_LAYOUT ?= none

Image/disk.img:
	python3 tools/mkdisk.py $@ 64 --layout $(DISK_LAYOUT)

QEMU_DISK_virtio = -device virtio-blk-pci,drive=hd0,disable-modern=on
QEMU_DISK_nvme   = -device nvme,drive=hd0,serial=minikernel
QEMU_DISK_ahci   = -device ich9-ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0
QEMU_DISK_usb    = -device qemu-xhci,id=xhci -device usb-storage,bus=xhci.0,drive=hd0

# Bootloader + Kernel bauen und in QEMU starten (Kernel und initrd landen im EFI-Image, die Platte am gewaehlten Controller)
.PHONY: efi
efi: build Image/disk.img
	@rm -f Image/cmdline.txt
	@if [ -n "$(KERNEL_CMDLINE)" ]; then printf '%s\n' "$(KERNEL_CMDLINE)" > Image/cmdline.txt; else rm -f Image/cmdline.txt; fi
	uefi-run -s 256 -d Image/bootx64.efi -f Image/kernel.elf:kernel.elf -f Image/initrd.tar:initrd.tar $(if $(KERNEL_CMDLINE),-f Image/cmdline.txt:cmdline.txt) -- -m 512 -rtc base=localtime --serial file:./Out.log \
	    -drive file=Image/disk.img,format=raw,if=none,id=hd0 $(QEMU_DISK_$(DISK)) $(QEMU_NET) $(QEMU_EXTRA)

# VMware: bootfaehiges GPT/ESP-Image plus Datenplatte als VMDK und eine passende .vmx nach Image/vmware/.
#   make vmware               Platten am SATA-Controller (AHCI)
#   make vmware VMWARE_BUS=nvme
VMWARE_BUS ?= sata
.PHONY: vmware
vmware: build Image/disk.img
	mkdir -p Image/vmware
	python3 tools/mkesp.py Image/vmware/boot.img Image/bootx64.efi Image/kernel.elf Image/initrd.tar 64 $(if $(KERNEL_CMDLINE),--cmdline "$(KERNEL_CMDLINE)")
	qemu-img convert -f raw -O vmdk Image/vmware/boot.img Image/vmware/boot.vmdk
	qemu-img convert -f raw -O vmdk Image/disk.img Image/vmware/data.vmdk
	rm -f Image/vmware/boot.img
	python3 tools/mkvmx.py Image/vmware/minikernel.vmx --bus $(VMWARE_BUS)

# Rohes Image fuer einen echten USB-Stick (GPT + EFI-Systempartition, bootet per UEFI ueber /EFI/BOOT/BOOTX64.EFI):
#   make usb                  -> Image/usb/minikernel-usb.img   (mit Rufus im DD-Modus oder Balena Etcher auf den Stick schreiben)
.PHONY: usb
usb: build
	mkdir -p Image/usb
	python3 tools/mkesp.py Image/usb/minikernel-usb.img Image/bootx64.efi Image/kernel.elf Image/initrd.tar 64 $(if $(KERNEL_CMDLINE),--cmdline "$(KERNEL_CMDLINE)")

# Dateien zum Kopieren auf einen normalen FAT32-Stick (kein Rufus noetig): Inhalt von Image/usbfiles in die Wurzel des Sticks
#   make usbfiles             -> Image/usbfiles/EFI/BOOT/BOOTX64.EFI, KERNEL.ELF, INITRD.TAR (und cmdline.txt)
.PHONY: usbfiles
usbfiles: build
	rm -rf Image/usbfiles
	mkdir -p Image/usbfiles/EFI/BOOT
	cp Image/bootx64.efi Image/usbfiles/EFI/BOOT/BOOTX64.EFI
	cp Image/kernel.elf Image/usbfiles/KERNEL.ELF
	cp Image/initrd.tar Image/usbfiles/INITRD.TAR
	$(if $(KERNEL_CMDLINE),printf '%s\n' "$(KERNEL_CMDLINE)" > Image/usbfiles/cmdline.txt)

.PHONY: clean
clean:
	rm -rf Build Image/bootx64.efi Image/kernel.elf Image/initrd.tar Image/vmware

.PHONY: cleandisk
cleandisk:
	rm -f Image/disk.img

.SECONDARY:
