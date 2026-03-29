# ============================================================
# IRONKERNEL — Makefile
# Build system: asm + C → obj → bin → iso
# Updated: VGA TEXT MODE DRIVER node
# ============================================================

# -- TOOLS ----------------------------------------------------
ASM         := nasm
CC          := gcc
LD          := ld
GRUB        := grub-mkrescue
QEMU        := qemu-system-x86_64
# gcc: GNU C Compiler — compiles our .c files to ELF32 objects.
# All other tools carry over from the previous Node.

# -- FLAGS ----------------------------------------------------
ASM_FLAGS   := -f elf64
# -f elf32: NASM outputs a 32-bit ELF object (unchanged)

CC_FLAGS    := -ffreestanding -fno-stack-protector -nostdlib -nostdinc -fno-builtin -mno-red-zone -c -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie
# -m32              : compile for 32-bit x86 (matches our ELF32 linker target)
# -ffreestanding    : tells gcc we have no OS beneath us — no libc, no crt0
# -fno-stack-protector: disables stack canary insertion — we have no __stack_chk_fail
# -nostdlib         : do not link standard libraries automatically
# -nostdinc         : do not search standard system include directories
# -fno-builtin      : do not substitute calls with gcc built-in versions
# -c                : compile only — do not link (ld handles linking separately)

LD_FLAGS    := -T src/boot/linker.ld -m elf_x86_64

UCFLAGS     := -ffreestanding -nostdlib -nostdinc -fno-builtin -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie
# Unchanged — linker script and 32-bit ELF target remain the same

# -- PATHS ----------------------------------------------------
BUILD_DIR   := build
ISO_DIR     := iso_root

.DEFAULT_GOAL := all
include iklibc/Makefile.inc

# -- OBJECTS --------------------------------------------------
OBJ         := $(BUILD_DIR)/boot.o \
               $(BUILD_DIR)/kernel.o \
               $(BUILD_DIR)/vga.o \
               $(BUILD_DIR)/gdt.o \
               $(BUILD_DIR)/gdt_asm.o \
               $(BUILD_DIR)/idt.o \
               $(BUILD_DIR)/idt_asm.o \
               $(BUILD_DIR)/pmm.o \
               $(BUILD_DIR)/vmm.o \
               $(BUILD_DIR)/heap.o \
               $(BUILD_DIR)/pit.o \
               $(BUILD_DIR)/irq_asm.o \
               $(BUILD_DIR)/keyboard.o \
               $(BUILD_DIR)/shell.o \
               $(BUILD_DIR)/scroll.o \
               $(BUILD_DIR)/ata.o \
               $(BUILD_DIR)/fat12.o \
               $(BUILD_DIR)/elf.o \
               $(BUILD_DIR)/tss.o \
               $(BUILD_DIR)/syscall.o \
               $(BUILD_DIR)/usermode.o \
               $(BUILD_DIR)/sched.o \
               $(BUILD_DIR)/switch.o \
               $(BUILD_DIR)/pipe.o \
               $(BUILD_DIR)/pci.o \
               $(BUILD_DIR)/e1000.o \
               $(BUILD_DIR)/arp.o \
               $(BUILD_DIR)/ip.o \
               $(BUILD_DIR)/tcp.o \
               $(BUILD_DIR)/dns.o \
               $(BUILD_DIR)/dhcp.o \
               $(BUILD_DIR)/fat32.o \
               $(BUILD_DIR)/mouse.o \
               $(BUILD_DIR)/serial.o \
               $(BUILD_DIR)/wm.o \
               $(BUILD_DIR)/panic.o \
               $(BUILD_DIR)/klog.o \
               $(BUILD_DIR)/speaker.o \
               $(BUILD_DIR)/ac97.o
# All three object files must be linked together into myos.bin.
# The linker resolves: boot.asm's 'call kernel_main' → kernel.o's kernel_main
# and kernel.c's calls to vga_init/vga_print → vga.o's implementations.

KERNEL      := myos.bin
ISO         := myos.iso

# -- TARGETS --------------------------------------------------

.PHONY: all clean run debug verify

all: $(ISO)

$(BUILD_DIR)/boot.o: src/boot/boot.asm
	$(ASM) $(ASM_FLAGS) $< -o $@
# Assemble boot.asm → build/boot.o (unchanged from previous Node)

$(BUILD_DIR)/kernel.o: src/kernel/kernel.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile kernel.c → build/kernel.o
# $< = src/kernel/kernel.c   $@ = build/kernel.o

$(BUILD_DIR)/vga.o: src/drivers/vga.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile vga.c → build/vga.o
# Same pattern: CC + CC_FLAGS applied to each .c file independently

$(BUILD_DIR)/gdt.o: src/kernel/gdt.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile gdt.c → build/gdt.o
# Contains gdt_init(), the GDT entry array, and gdt_ptr structure.

$(BUILD_DIR)/gdt_asm.o: src/boot/gdt.asm
	$(ASM) $(ASM_FLAGS) $< -o $@
# Assemble gdt.asm → build/gdt_asm.o
# Contains gdt_flush — the lgdt + far jump + segment register reload routine.

$(BUILD_DIR)/idt.o: src/kernel/idt.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile idt.c → build/idt.o
# Contains idt_init, PIC remap, isr_handler, gate setup.

$(BUILD_DIR)/idt_asm.o: src/boot/idt.asm
	$(ASM) $(ASM_FLAGS) $< -o $@
# Assemble idt.asm → build/idt_asm.o
# Contains all 32 ISR stubs and idt_flush.

$(BUILD_DIR)/pmm.o: src/kernel/pmm.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile pmm.c → build/pmm.o
# Contains pmm_init, pmm_alloc_frame, pmm_free_frame, bitmap ops.

$(BUILD_DIR)/vmm.o: src/kernel/vmm.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile vmm.c → build/vmm.o
# Contains vmm_init, vmm_map_page, vmm_unmap_page, page fault handler.

$(BUILD_DIR)/heap.o: src/kernel/heap.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile heap.c → build/heap.o
# Contains heap_init, kmalloc, kfree, coalescing logic.

$(BUILD_DIR)/pit.o: src/drivers/pit.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile pit.c → build/pit.o
# Contains pit_init, timer_callback, irq_handler dispatcher.

$(BUILD_DIR)/irq_asm.o: src/boot/irq.asm
	$(ASM) $(ASM_FLAGS) $< -o $@
# Assemble irq.asm → build/irq_asm.o
# Contains IRQ stubs 0–15 and irq_common_stub.

$(BUILD_DIR)/keyboard.o: src/drivers/keyboard.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile keyboard.c -> build/keyboard.o
# Contains keyboard_init, IRQ1 handler, scancode tables, ring buffer.

$(BUILD_DIR)/scroll.o: src/kernel/scroll.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/shell.o: src/kernel/shell.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile shell.c -> build/shell.o
# Contains shell_run, command dispatcher, all built-ins, systemfetch.

$(BUILD_DIR)/ata.o: src/drivers/ata.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile ata.c -> build/ata.o
# Contains ata_init, ata_read_sector, ata_write_sector, IDENTIFY.

$(BUILD_DIR)/tss.o: src/kernel/tss.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/syscall.o: src/kernel/syscall.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/sched.o: src/kernel/sched.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/switch.o: src/boot/switch.asm
	$(ASM) $(ASM_FLAGS) $< -o $@

$(BUILD_DIR)/usermode.o: src/boot/usermode.asm
	$(ASM) $(ASM_FLAGS) $< -o $@

$(BUILD_DIR)/elf.o: src/drivers/elf.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/fat12.o: src/drivers/fat12.c
	$(CC) $(CC_FLAGS) $< -o $@
# Compile fat12.c -> build/fat12.o

$(BUILD_DIR)/pipe.o: src/kernel/pipe.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/pci.o: src/drivers/pci.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/e1000.o: src/drivers/e1000.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/arp.o: src/drivers/arp.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/ip.o: src/drivers/ip.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/tcp.o: src/drivers/tcp.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/dns.o: src/drivers/dns.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/dhcp.o: src/drivers/dhcp.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/fat32.o: src/drivers/fat32.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/mouse.o: src/drivers/mouse.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/serial.o: src/drivers/serial.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/wm.o: src/kernel/wm.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/panic.o: src/kernel/panic.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/klog.o: src/kernel/klog.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/speaker.o: src/drivers/speaker.c
	$(CC) $(CC_FLAGS) $< -o $@

$(BUILD_DIR)/ac97.o: src/drivers/ac97.c
	$(CC) $(CC_FLAGS) $< -o $@

$(KERNEL): $(OBJ) src/boot/linker.ld
	$(LD) $(LD_FLAGS) $(OBJ) -o $@
# Link all three objects into the final ELF32 kernel binary.
# Order matters: boot.o must come first so _start is the entry symbol.
# ld resolves all cross-object symbol references here.

$(ISO): $(KERNEL) $(ISO_DIR)/boot/grub/grub.cfg
	mkdir -p $(ISO_DIR)/boot/grub
	mkdir -p $(BUILD_DIR)/grub_tmp
	cp $(KERNEL) $(ISO_DIR)/boot/$(KERNEL)
	TMPDIR=$(BUILD_DIR)/grub_tmp $(GRUB) --locales="" -o $@ $(ISO_DIR)
# ISO creation unchanged — pack iso_root into bootable ISO with GRUB

clean:
	rm -f $(OBJ) $(IKLIBC_OBJS) $(KERNEL) $(ISO)
	rm -rf $(ISO_DIR)/boot/$(KERNEL)

run: $(ISO)
	rm -f /tmp/ik_monitor.sock
	SDL_VIDEO_HIGHDPI_DISABLED=1 qemu-system-x86_64 \
	    -cpu Skylake-Client \
	    -cdrom myos.iso \
	    -drive file=disk32.img,format=raw,if=ide,bus=0,unit=0 \
	    -boot order=d \
	    -m 256M \
	    -vga std \
	    -display sdl,gl=off \
	    -nic user,model=e1000 \
	    -usb -device usb-tablet \
	    -machine pc,pcspk-audiodev=snd0 \
	    -audiodev pa,id=snd0 \
	    -device AC97,audiodev=snd0 \
	    -serial file:/tmp/ik_serial.log \
	    -debugcon file:/tmp/ik_debug.log \
	    -D /tmp/ik_qemu.log -d guest_errors,cpu_reset,int \
	    -monitor unix:/tmp/ik_monitor.sock,server,nowait


debug: $(ISO)
	SDL_VIDEO_HIGHDPI_DISABLED=1 qemu-system-x86_64 \
	    -cdrom myos.iso \
	    -drive file=disk32.img,format=raw,if=ide,bus=0,unit=0 \
	    -boot order=d \
	    -m 256M \
	    -vga std \
	    -display sdl,gl=off \
	    -nic user,model=e1000 \
	    -usb -device usb-tablet \
	    -debugcon file:/tmp/ik_debug.log \
	    -D /tmp/ik_qemu.log -d guest_errors,cpu_reset,int \
	    -s -S

verify: $(KERNEL)
	@echo "--- VERIFICATION SEQUENCE ---"
	ls -lh $(KERNEL)
	file $(KERNEL)
	objdump -h $(KERNEL)
	readelf -h $(KERNEL)
	@grub-file --is-x86-multiboot2 $(KERNEL) && echo "✅ MULTIBOOT2: VALID" || echo "❌ MULTIBOOT2: INVALID"

# ── User-mode ELF64 test programs ──────────────────────────────────────
UCFLAGS := -ffreestanding -nostdlib -nostdinc -fno-builtin \
           -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie

# User-mode ELF64 test programs
UCFLAGS := -ffreestanding -nostdlib -nostdinc -fno-builtin -fno-stack-protector -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -fno-pic -fno-pie -no-pie -m64

test/hello.elf: test/hello.c test/link.ld
	$(CC) $(UCFLAGS) -T test/link.ld -o test/hello.elf test/hello.c

test/hi.elf: test/hi.c test/link.ld
	$(CC) $(UCFLAGS) -T test/link.ld -o test/hi.elf test/hi.c

test/hiboiii.elf: test/hiboiii.c test/link.ld
	$(CC) $(UCFLAGS) -T test/link.ld -o test/hiboiii.elf test/hiboiii.c

test/wsp.elf: test/wsp.c test/link.ld
	$(CC) $(UCFLAGS) -T test/link.ld -o test/wsp.elf test/wsp.c

test/sysinfo.elf: test/sysinfo.c test/link.ld
	$(CC) $(UCFLAGS) -T test/link.ld -o test/sysinfo.elf test/sysinfo.c

# ── iklibc-linked programs ──────────────────────────────────────────
$(eval $(call iklibc_prog, test/demo.elf,      test/demo.c))
$(eval $(call iklibc_prog, test/calc.elf,      test/calc.c))
$(eval $(call iklibc_prog, test/fork_test.elf, test/fork_test.c))
$(eval $(call iklibc_prog, test/pipe_test.elf, test/pipe_test.c))
$(eval $(call iklibc_prog, test/shell2.elf,    test/shell2.c))
$(eval $(call iklibc_prog, test/ikls.elf,     test/ikls.c))
$(eval $(call iklibc_prog, test/ikcat.elf,    test/ikcat.c))
$(eval $(call iklibc_prog, test/advance.elf,  test/advance.c))
$(eval $(call iklibc_prog, test/edit.elf,    test/edit.c))
$(eval $(call iklibc_prog, test/error.elf,  test/error.c))
$(eval $(call iklibc_prog, test/beep.elf,   test/beep.c))

# ── Copy ELFs to FAT32 disk image ──────────────────────────────────
DISK_ELFS := test/hello.elf test/hi.elf test/hiboiii.elf test/wsp.elf \
             test/sysinfo.elf test/demo.elf test/calc.elf \
             test/fork_test.elf test/pipe_test.elf test/shell2.elf \
             test/ikls.elf test/ikcat.elf test/advance.elf test/edit.elf \
             test/error.elf \
             test/beep.elf

# ── Import a WAV as BOOT.WAV ────────────────────────────────────────
# Converts any audio file to 16-bit PCM stereo 44100 Hz (what AC97
# expects) and installs it on the FAT32 disk image as BOOT.WAV.
# The GUI will play it automatically at startup instead of the
# synthesised chime.
#
# Usage:  make import-wav WAV=/path/to/windows-startup.wav
#         make import-wav WAV=/path/to/any.mp3
#
.PHONY: import-wav
import-wav:
	@if [ -z "$(WAV)" ]; then \
	    echo "Usage: make import-wav WAV=/path/to/sound.wav"; \
	    exit 1; \
	fi
	@echo "[import-wav] converting $(WAV) → /tmp/ik_boot.wav"
	@ffmpeg -y -i "$(WAV)" \
	    -ar 44100 -ac 2 -acodec pcm_s16le \
	    /tmp/ik_boot.wav 2>&1 | tail -3
	@echo "[import-wav] copying to disk32.img as BOOT.WAV"
	@mcopy -i disk32.img -o /tmp/ik_boot.wav ::BOOT.WAV
	@ls -lh /tmp/ik_boot.wav | awk '{print "[import-wav] done:", $$5, "→ BOOT.WAV (run make run)"}'

.PHONY: disk
disk: $(DISK_ELFS)
	@echo "Copying ELFs to disk32.img..."
	@for elf in $(DISK_ELFS); do \
	    base=$$(basename $$elf); \
	    upper=$$(echo $$base | tr '[:lower:]' '[:upper:]'); \
	    echo "  $$elf -> ::$$upper"; \
	    mcopy -i disk32.img -o $$elf ::$$upper 2>/dev/null || true; \
	done
	@echo "Done."
