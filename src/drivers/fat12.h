#ifndef FAT12_H
#define FAT12_H

#include "../kernel/types.h"

/* ── BIOS PARAMETER BLOCK ───────────────────────────────────────────── */
typedef struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed)) fat12_bpb_t;

/* ── ROOT DIRECTORY ENTRY (32 bytes) ────────────────────────────────── */
typedef struct {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  reserved[10];
    uint16_t time;
    uint16_t date;
    uint16_t first_cluster;
    uint32_t file_size;
} __attribute__((packed)) fat12_dirent_t;

/* ── ATTRIBUTE FLAGS ────────────────────────────────────────────────── */
#define FAT12_ATTR_READ_ONLY  0x01
#define FAT12_ATTR_HIDDEN     0x02
#define FAT12_ATTR_SYSTEM     0x04
#define FAT12_ATTR_VOLUME_ID  0x08
#define FAT12_ATTR_DIRECTORY  0x10
#define FAT12_ATTR_ARCHIVE    0x20

/* ── FILESYSTEM STATE ───────────────────────────────────────────────── */
typedef struct {
    int      mounted;
    uint32_t first_fat_sector;
    uint32_t first_root_sector;
    uint32_t root_dir_sectors;
    uint32_t first_data_sector;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t root_entry_count;
    uint32_t total_sectors;
    uint32_t sectors_per_fat;
} fat12_fs_t;

#define FAT12_EOC  0xFF8

/* ── CWD STATE ──────────────────────────────────────────────────────── */
#define FAT12_CWD_MAX    64
#define FAT12_STACK_DEPTH 8
extern char     fat12_cwd[FAT12_CWD_MAX];
extern uint16_t fat12_cwd_cluster;

/* Snapshot of the full CWD state — used by fat12_save/restore_cwd. */
typedef struct {
    char     cwd[FAT12_CWD_MAX];
    uint16_t cluster;
    uint16_t parent_stack[FAT12_STACK_DEPTH];
    int      depth;
} fat12_cwd_state_t;

/* ── PUBLIC READ API ────────────────────────────────────────────────── */
int  fat12_init(void);
void fat12_list_root(void);
void fat12_list_dir(uint16_t cluster);
/* List directory contents. cluster=0 means root directory.
   cluster>0 means a subdirectory starting at that FAT cluster.
   Called by the shell ls command with fat12_cwd_cluster. */
int  fat12_read_file(const char* name, const char* ext,
                     uint8_t* buf, uint32_t buf_size,
                     uint32_t* bytes_read);
void fat12_print_info(void);

/* ── PUBLIC WRITE API ───────────────────────────────────────────────── */

int fat12_write_file(const char* name, const char* ext,
                     const uint8_t* buf, uint32_t len);
/* Create or overwrite a file in the root directory.
   Allocates FAT clusters, writes data sectors, updates root entry.
   Returns 0 on success, -1 on failure. */

int fat12_delete(const char* name, const char* ext);
/* Mark a root directory entry as deleted (0xE5) and free its FAT chain.
   Returns 0 on success, -1 if not found. */

int fat12_mkdir(const char* name);
/* Create a directory entry in the root directory with ATTR_DIRECTORY.
   FAT12 root dirs are fixed-size so this just writes an entry.
   Returns 0 on success, -1 if root dir full or name exists. */

int fat12_chdir(const char* name);
/* Change CWD. Supports multi-component paths (a/b/c), absolute paths (/a/b),
   "..", ".", "/", "-" (previous dir). Returns 0 on success, -1 if not found. */

void fat12_save_cwd(fat12_cwd_state_t *s);
void fat12_restore_cwd(const fat12_cwd_state_t *s);
/* Snapshot and restore the full CWD state for temporary navigation. */

int  fat12_rename(const char* old_name, const char* old_ext,
                  const char* new_name, const char* new_ext);
/* Rename an entry in the current directory (in-place 8.3 name rewrite).
   Returns 0 on success, -1 if not found. */

/* ── READDIR ────────────────────────────────────────────────────────── */
/* User-facing directory entry (distinct from raw fat12_dirent_t). */
typedef struct {
    char     name[9];    /* null-terminated display name (no trailing spaces) */
    char     ext[4];     /* null-terminated extension                          */
    uint8_t  is_dir;     /* 1 = directory, 0 = file                           */
    uint32_t size;       /* file size in bytes (0 for directories)             */
} fat12_dentry_t;

int fat12_readdir(uint16_t cluster, fat12_dentry_t *buf, int max_entries);
/* Fill buf with up to max_entries entries from the directory at cluster.
   cluster=0 = root directory; cluster>0 = subdirectory.
   Returns count of entries stored. */

extern fat12_fs_t fat12_fs;

#endif
