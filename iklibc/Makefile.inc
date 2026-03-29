# IronKernel iklibc — Makefile.inc
# Include this from the top-level Makefile with:  include iklibc/Makefile.inc

IKLIBC_DIR := iklibc
IKLIBC_INC := -I$(IKLIBC_DIR)/include
IKLIBC_CC  := $(CC) -ffreestanding -fno-stack-protector -nostdlib -nostdinc \
              -fno-builtin -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
              -fno-pic -fno-pie -no-pie -m64 $(IKLIBC_INC)

IKLIBC_OBJS := \
    $(BUILD_DIR)/ik_crt0.o    \
    $(BUILD_DIR)/ik_syscall.o \
    $(BUILD_DIR)/ik_string.o  \
    $(BUILD_DIR)/ik_stdio.o   \
    $(BUILD_DIR)/ik_stdlib.o  \
    $(BUILD_DIR)/ik_errno.o

$(BUILD_DIR)/ik_crt0.o: $(IKLIBC_DIR)/src/crt0.asm
	$(ASM) $(ASM_FLAGS) $< -o $@

$(BUILD_DIR)/ik_syscall.o: $(IKLIBC_DIR)/src/syscall.c
	$(IKLIBC_CC) -c $< -o $@

$(BUILD_DIR)/ik_string.o: $(IKLIBC_DIR)/src/string.c
	$(IKLIBC_CC) -c $< -o $@

$(BUILD_DIR)/ik_stdio.o: $(IKLIBC_DIR)/src/stdio.c
	$(IKLIBC_CC) -c $< -o $@

$(BUILD_DIR)/ik_stdlib.o: $(IKLIBC_DIR)/src/stdlib.c
	$(IKLIBC_CC) -c $< -o $@

$(BUILD_DIR)/ik_errno.o: $(IKLIBC_DIR)/src/errno.c
	$(IKLIBC_CC) -c $< -o $@

# ── Helper macro: build an iklibc-linked user program ──────────────
# Usage: $(eval $(call iklibc_prog, test/myprog.elf, test/myprog.c))
define iklibc_prog
$(1): $(2) $(IKLIBC_OBJS) $(IKLIBC_DIR)/link.ld
	$(IKLIBC_CC) -T $(IKLIBC_DIR)/link.ld -o $(1) \
	    $(BUILD_DIR)/ik_crt0.o $(2) \
	    $(BUILD_DIR)/ik_syscall.o \
	    $(BUILD_DIR)/ik_string.o  \
	    $(BUILD_DIR)/ik_stdio.o   \
	    $(BUILD_DIR)/ik_stdlib.o  \
	    $(BUILD_DIR)/ik_errno.o
endef
