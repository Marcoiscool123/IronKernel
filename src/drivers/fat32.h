#ifndef FAT32_H
#define FAT32_H

#include "../kernel/types.h"

/* ── FAT32 directory entry attribute flags ──────────────────────── */
#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F   /* Long File Name entry — skip */

#define FAT32_EOC  0x0FFFFFF8u       /* End-of-chain threshold */

/* ── CWD state ──────────────────────────────────────────────────── */
#define FAT32_CWD_MAX      256
#define FAT32_STACK_DEPTH  8

extern char     fat32_cwd[FAT32_CWD_MAX];
extern uint32_t fat32_cwd_cluster;

typedef struct {
    char     cwd[FAT32_CWD_MAX];
    uint32_t cluster;
    uint32_t parent_stack[FAT32_STACK_DEPTH];
    int      depth;
} fat32_cwd_state_t;

/* ── User-facing directory entry ───────────────────────────────── */
typedef struct {
    char     name[9];    /* null-terminated, no trailing spaces */
    char     ext[4];     /* null-terminated */
    uint8_t  is_dir;
    uint32_t size;
} fat32_dentry_t;

/* ── Raw 32-byte directory entry (on-disk format) ───────────────── */
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;   /* last access date (FAT32 offset 18) */
    uint16_t first_cluster_hi;   /* high 16 bits of first cluster */
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t first_cluster_lo;   /* low 16 bits of first cluster  */
    uint32_t file_size;
} fat32_dirent_t;

/* ── Public API ─────────────────────────────────────────────────── */

int  fat32_init(void);
void fat32_print_info(void);

int  fat32_read_file(const char *name, const char *ext,
                     uint8_t *buf, uint32_t buf_size, uint32_t *bytes_read);

int  fat32_write_file(const char *name, const char *ext,
                      const uint8_t *buf, uint32_t len);

int  fat32_delete(const char *name, const char *ext);

int  fat32_mkdir(const char *name);

int  fat32_chdir(const char *name);

void fat32_save_cwd(fat32_cwd_state_t *s);
void fat32_restore_cwd(const fat32_cwd_state_t *s);

int  fat32_rename(const char *old_name, const char *old_ext,
                  const char *new_name, const char *new_ext);

int  fat32_readdir(uint32_t cluster, fat32_dentry_t *buf, int max_entries);
void fat32_list_dir(uint32_t cluster);

#endif
