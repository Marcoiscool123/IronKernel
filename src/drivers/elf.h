/* IronKernel v0.04 — elf.h  (ELF64 loader) */
#ifndef ELF_H
#define ELF_H
#include "../kernel/types.h"

/* ELF64 header */
typedef struct __attribute__((packed)) {
    uint8_t  ident[16];  /* [0..3]=magic [4]=class [5]=endian [6]=version */
    uint16_t type;       /* ET_EXEC = 2 */
    uint16_t machine;    /* EM_X86_64 = 62 */
    uint32_t version;
    uint64_t entry;      /* 64-bit entry point */
    uint64_t phoff;      /* offset to program header table */
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} Elf64Hdr;

/* ELF64 program header — NOTE: flags is at offset 4 (after type),
   NOT after filesz as in ELF32. Getting this wrong silently corrupts loads. */
typedef struct __attribute__((packed)) {
    uint32_t type;     /* PT_LOAD = 1 */
    uint32_t flags;    /* PF_R=4 PF_W=2 PF_X=1 */
    uint64_t offset;   /* offset of segment data in file */
    uint64_t vaddr;    /* virtual load address */
    uint64_t paddr;    /* physical (ignored) */
    uint64_t filesz;   /* bytes in file image */
    uint64_t memsz;    /* bytes in memory (memsz-filesz = BSS zeroing) */
    uint64_t align;
} Elf64Phdr;

#define ELF_MAGIC0   0x7F
#define ELF_MAGIC1   'E'
#define ELF_MAGIC2   'L'
#define ELF_MAGIC3   'F'
#define ELFCLASS64   2
#define ET_EXEC      2
#define EM_X86_64    62
#define PT_LOAD      1

int elf_exec(const char *name, const char *ext);

#endif

extern void user_mode_enter(uint64_t entry, uint64_t user_rsp);
