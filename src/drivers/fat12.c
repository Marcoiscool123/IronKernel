#include "fat12.h"
#include "ata.h"
#include "vga.h"
#include "../kernel/types.h"

/* ── GLOBAL STATE ───────────────────────────────────────────────── */

fat12_fs_t fat12_fs;

#define FAT_BUF_SECTORS  64
static uint8_t fat_buf[FAT_BUF_SECTORS * 512];
/* In-memory copy of the FAT table.
   FAT12 on a 10MB disk uses at most 12 sectors = 6144 bytes.
   We allocate 16 sectors = 8192 bytes to have headroom. */

static uint8_t io_buf[512];
/* Scratch buffer for single-sector reads.
   Reused across operations — not safe for concurrent use,
   but IRONKERNEL is single-threaded so this is fine. */

static uint8_t cluster_buf[512];
/* Sector read buffer used by fat12_read_file cluster chain walk.
   Kept at file scope — 512 bytes on the kernel stack would
   overflow when combined with nested call frames. */

/* ── INTERNAL HELPERS ───────────────────────────────────────────── */

static void print_uint32(uint32_t n)
{
    if (n == 0) { vga_print("0"); return; }
    char buf[12]; buf[11] = '\0'; int i = 10;
    while (n > 0 && i >= 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    vga_print(&buf[i + 1]);
}

static uint16_t fat12_next_cluster(uint16_t cluster)
{
    uint32_t max_offset = FAT_BUF_SECTORS * 512;
    /* FAT12 entries are 12 bits — 1.5 bytes each.
       Entry for cluster N occupies bytes at offset N + N/2.
       Two adjacent entries share a byte:
         Even cluster N : bits 11-0 of the shared 16-bit word
         Odd  cluster N : bits 15-4 of the shared 16-bit word */
    uint32_t offset = (uint32_t)cluster + (cluster / 2);
    if (offset + 1 >= max_offset) return 0xFFF;
    /* Out of bounds — treat as end of chain. */
    uint16_t val = (uint16_t)fat_buf[offset]
                 | ((uint16_t)fat_buf[offset + 1] << 8);
    if (cluster & 1)
        return (val >> 4) & 0x0FFF;
    else
        return val & 0x0FFF;
}

static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

/* ── PUBLIC FUNCTIONS ───────────────────────────────────────────── */

int fat12_init(void)
{
    fat12_fs.mounted = 0;

    if (!ata_master.present) return -1;

    /* Read and parse the boot sector. */
    if (ata_read_sector(0, io_buf) != 0) return -1;

    fat12_bpb_t* bpb = (fat12_bpb_t*)(io_buf + 0x0B);
    /* BPB starts at byte 11 of the boot sector.
       The first 11 bytes are the jump instruction and OEM name. */

    /* Derive all filesystem geometry from BPB fields. */
    fat12_fs.bytes_per_sector    = bpb->bytes_per_sector;
    fat12_fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fat12_fs.root_entry_count    = bpb->root_entry_count;

    fat12_fs.first_fat_sector  = bpb->reserved_sectors;
    fat12_fs.sectors_per_fat   = bpb->sectors_per_fat;
    /* sectors_per_fat must be stored in the fs struct so fat12_flush_fat
       knows how many sectors to write back. Without this it wrote 0
       sectors and the FAT never reached disk — all cluster allocs lost. */
    fat12_fs.first_root_sector = bpb->reserved_sectors
                               + (uint32_t)bpb->fat_count * bpb->sectors_per_fat;
    fat12_fs.root_dir_sectors  = ((uint32_t)bpb->root_entry_count * 32 + 511) / 512;
    /* Each entry is 32 bytes. Round up to full sectors. */

    fat12_fs.first_data_sector = fat12_fs.first_root_sector
                               + fat12_fs.root_dir_sectors;
    fat12_fs.total_sectors     = bpb->total_sectors_16
                               ? bpb->total_sectors_16
                               : bpb->total_sectors_32;

    /* Load FAT table into fat_buf. */
    uint32_t fat_sectors = bpb->sectors_per_fat;
    if (fat_sectors > FAT_BUF_SECTORS) fat_sectors = FAT_BUF_SECTORS;
    for (uint32_t i = 0; i < fat_sectors; i++) {
        if (ata_read_sector(fat12_fs.first_fat_sector + i,
                            fat_buf + i * 512) != 0) {
            return -1;
        }
    }

    fat12_fs.mounted = 1;
    return 0;
}

void fat12_list_root(void)
{
    if (!fat12_fs.mounted) {
        vga_print("  Filesystem not mounted.\n");
        return;
    }

    uint32_t found = 0;

    for (uint32_t s = 0; s < fat12_fs.root_dir_sectors; s++) {
        if (ata_read_sector(fat12_fs.first_root_sector + s, io_buf) != 0) {
            vga_print("  Read error.\n");
            return;
        }

        fat12_dirent_t* entries = (fat12_dirent_t*)io_buf;
        uint32_t entries_per_sector = 512 / sizeof(fat12_dirent_t);

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint8_t first = entries[i].name[0];

            if (first == 0x00) return;
            /* 0x00 = free entry and no entries follow — stop scanning. */

            if (first == 0xE5) continue;
            /* 0xE5 = deleted entry — skip. */

            if (entries[i].attr & FAT12_ATTR_VOLUME_ID) continue;
            /* Volume label — not a real file. */

            if (entries[i].attr & FAT12_ATTR_SYSTEM) continue;
            /* System file — skip. */

            /* Build display name from 8.3 fields. */
            char display[13];
            int  d = 0;

            for (int n = 0; n < 8 && entries[i].name[n] != ' '; n++) {
                char ch = entries[i].name[n];
                display[d++] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
            }

            int has_ext = 0;
            for (int n = 0; n < 3; n++)
                if (entries[i].ext[n] != ' ') { has_ext = 1; break; }

            if (has_ext && !(entries[i].attr & FAT12_ATTR_DIRECTORY)) {
                display[d++] = '.';
                for (int n = 0; n < 3 && entries[i].ext[n] != ' '; n++) {
                    char ch = entries[i].ext[n];
                    display[d++] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
                }
            }
            display[d] = '\0';

            vga_print("  ");

            if (entries[i].attr & FAT12_ATTR_DIRECTORY) {
                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                vga_print("[DIR]  ");
                vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            } else {
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                vga_print("       ");
            }

            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_print(display);

            if (!(entries[i].attr & FAT12_ATTR_DIRECTORY)) {
                vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                vga_print("  ");
                print_uint32(entries[i].file_size);
                vga_print(" B");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            }
            vga_print("\n");
            found++;
        }
    }

    if (found == 0) {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_print("  (empty)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static int fat12_find_dir_entry(uint16_t cluster, const char* name, const char* ext, uint32_t* out_sector, uint32_t* out_idx);
/* Forward declaration — defined later in file. */

int fat12_read_file(const char* name, const char* ext,
                    uint8_t* buf, uint32_t buf_size,
                    uint32_t* bytes_read)
{
    if (!fat12_fs.mounted) return -1;

    *bytes_read = 0;

    /* Search current directory using fat12_cwd_cluster.
       Works in both root (cluster=0) and subdirs (cluster>0). */
    uint32_t esector = 0, eidx = 0;
    if (fat12_find_dir_entry(fat12_cwd_cluster, name, ext, &esector, &eidx) != 0)
        return -1;
    if (ata_read_sector(esector, io_buf) != 0) return -1;
    fat12_dirent_t match = ((fat12_dirent_t*)io_buf)[eidx];

    /* Follow the FAT cluster chain and read file data. */
    uint16_t cluster   = match.first_cluster;
    uint32_t remaining = match.file_size;

    uint32_t chain_limit = 4096;
    /* Safety cap — prevents infinite loop on corrupted FAT chain.
       4096 clusters * 512 bytes = 2MB max file size. */

    while (cluster >= 2 && cluster < FAT12_EOC
           && remaining > 0 && chain_limit-- > 0) {
        uint32_t sector = fat12_fs.first_data_sector
                        + ((uint32_t)cluster - 2) * fat12_fs.sectors_per_cluster;

        for (uint8_t s = 0; s < fat12_fs.sectors_per_cluster; s++) {
            if (ata_read_sector(sector + s, cluster_buf) != 0) return -1;

            uint32_t to_copy = 512;
            if (to_copy > remaining)              to_copy = remaining;
            if (*bytes_read + to_copy > buf_size) to_copy = buf_size - *bytes_read;
            if (to_copy == 0) break;

            for (uint32_t b = 0; b < to_copy; b++)
                buf[(*bytes_read)++] = cluster_buf[b];

            remaining -= to_copy;
        }

        cluster = fat12_next_cluster(cluster);
    }

    return 0;
}

void fat12_print_info(void)
{
    if (!fat12_fs.mounted) {
        vga_print("  Not mounted.\n");
        return;
    }

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  FAT12 Volume Info\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print("    Bytes/sector      : "); print_uint32(fat12_fs.bytes_per_sector);  vga_print("\n");
    vga_print("    Sectors/cluster   : "); print_uint32(fat12_fs.sectors_per_cluster); vga_print("\n");
    vga_print("    FAT start sector  : "); print_uint32(fat12_fs.first_fat_sector);  vga_print("\n");
    vga_print("    Root dir sector   : "); print_uint32(fat12_fs.first_root_sector); vga_print("\n");
    vga_print("    Root dir sectors  : "); print_uint32(fat12_fs.root_dir_sectors);  vga_print("\n");
    vga_print("    Data start sector : "); print_uint32(fat12_fs.first_data_sector); vga_print("\n");
    vga_print("    Total sectors     : "); print_uint32(fat12_fs.total_sectors);     vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    Status            : MOUNTED\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ═══════════════════════════════════════════════════════════════════
   FAT12 WRITE ENGINE — added Node [18]
   ═══════════════════════════════════════════════════════════════════ */

#include "../drivers/ata.h"

/* ── CWD STATE ─────────────────────────────────────────────────────── */
char     fat12_cwd[FAT12_CWD_MAX] = "/";
/* Full path string e.g. "/", "/DOCS", "/DOCS/SRC". Printed in prompt. */
uint16_t fat12_cwd_cluster = 0;
/* 0 = root directory. FAT12 root has no cluster number. */

/* Parent cluster stack — lets cd .. go up one level. */
#define CWD_STACK_DEPTH 8
static uint16_t cwd_parent_stack[CWD_STACK_DEPTH];
static int      cwd_depth = 0;

/* Previous directory state for "cd -" support. */
static char     fat12_prev_cwd[FAT12_CWD_MAX] = "/";
static uint16_t fat12_prev_cluster = 0;
static uint16_t prev_parent_stack[CWD_STACK_DEPTH];
static int      prev_depth = 0;

/* ── INTERNAL: flush fat_buf sector N back to disk ─────────────────── */
static int fat12_flush_fat(void)
{
    /* Write both FAT copies back so the volume stays consistent.
       FAT12 always has two copies (fat_count=2) for redundancy. */
    uint32_t fat_sectors = fat12_fs.sectors_per_fat;
    for (uint32_t i = 0; i < fat_sectors; i++) {
        /* Write to FAT copy 0 */
        if (ata_write_sector(fat12_fs.first_fat_sector + i,
                             fat_buf + i * 512) != 0) return -1;
        /* Write to FAT copy 1 (backup) */
        if (ata_write_sector(fat12_fs.first_fat_sector +
                             fat12_fs.sectors_per_fat + i,
                             fat_buf + i * 512) != 0) return -1;
    }
    return 0;
}

/* ── INTERNAL: set FAT12 entry for cluster ─────────────────────────── */
static void fat12_set_cluster(uint16_t cluster, uint16_t val)
{
    /* FAT12 entries are 12 bits — 1.5 bytes each, same packing as read.
       Even cluster N: bits 11-0 of the 16-bit word at byte offset N+(N/2).
       Odd  cluster N: bits 15-4 of the same word. */
    uint32_t offset = (uint32_t)cluster + (cluster / 2);
    uint16_t cur = (uint16_t)fat_buf[offset] |
                   ((uint16_t)fat_buf[offset+1] << 8);
    if (cluster & 1) {
        /* Odd: place val in upper 12 bits, preserve lower 4. */
        cur = (cur & 0x000F) | ((val & 0x0FFF) << 4);
    } else {
        /* Even: place val in lower 12 bits, preserve upper 4. */
        cur = (cur & 0xF000) | (val & 0x0FFF);
    }
    fat_buf[offset]   = (uint8_t)(cur & 0xFF);
    fat_buf[offset+1] = (uint8_t)((cur >> 8) & 0xFF);
}

/* ── INTERNAL: find first free cluster (≥ 2) ───────────────────────── */
static uint16_t fat12_alloc_cluster(void)
{
    /* Scan FAT entries starting at cluster 2 (cluster 0 and 1 reserved).
       A value of 0x000 means the cluster is free. */
    if (fat12_fs.total_sectors <= fat12_fs.first_data_sector) return 0;
    /* Guard: unsigned subtraction below would wrap on a malformed BPB. */
    uint32_t max_cluster = (fat12_fs.total_sectors -
                            fat12_fs.first_data_sector) /
                            fat12_fs.sectors_per_cluster + 2;
    if (max_cluster > 4096) max_cluster = 4096;
    for (uint16_t c = 2; c < (uint16_t)max_cluster; c++) {
        if (fat12_next_cluster(c) == 0x000) return c;
        /* 0x000 in the FAT = free cluster. */
    }
    return 0; /* 0 = no free cluster found */
}

/* ── INTERNAL: free an entire FAT chain starting at cluster ─────────── */
static void fat12_free_chain(uint16_t cluster)
{
    uint32_t limit = 4096;
    while (cluster >= 2 && cluster < FAT12_EOC && limit-- > 0) {
        uint16_t next = fat12_next_cluster(cluster);
        fat12_set_cluster(cluster, 0x000); /* mark as free */
        cluster = next;
    }
}

/* ── INTERNAL: find or create a root dir entry slot ────────────────── */
/* Returns sector LBA and index within that sector, or -1 on failure.
   If match_name/ext != NULL, finds existing entry.
   If match_name == NULL, finds first free (0x00 or 0xE5) slot. */
/* ── fat12_find_dir_entry ───────────────────────────────────────────
   Searches either the root directory (cluster=0) or a subdirectory
   (cluster>0) for a named entry or a free slot.
   name==NULL means "find free slot". name!=NULL means "find this file".
   Stores the sector LBA and index within that sector in out_sector/out_idx.
   Returns 0 on success, -1 if not found. */
static int fat12_find_dir_entry(uint16_t cluster,
                                 const char* name, const char* ext,
                                 uint32_t* out_sector, uint32_t* out_idx)
{
    if (cluster == 0) {
        /* Root directory — fixed location, fixed sector count. */
        for (uint32_t s = 0; s < fat12_fs.root_dir_sectors; s++) {
            uint32_t lba = fat12_fs.first_root_sector + s;
            if (ata_read_sector(lba, io_buf) != 0) return -1;
            fat12_dirent_t* entries = (fat12_dirent_t*)io_buf;
            uint32_t ecount = 512 / sizeof(fat12_dirent_t);
            for (uint32_t i = 0; i < ecount; i++) {
                uint8_t first = entries[i].name[0];
                if (name == 0) {
                    if (first == 0x00 || first == 0xE5) {
                        *out_sector = lba; *out_idx = i; return 0;
                    }
                } else {
                    if (first == 0x00) return -1;
                    if (first == 0xE5) continue;
                    int ok = 1;
                    for (int c = 0; c < 8 && ok; c++) {
                        char e = (name[c]>='a'&&name[c]<='z')?name[c]-32:name[c];
                        if ((uint8_t)entries[i].name[c] != (uint8_t)e) ok=0;
                    }
                    for (int c = 0; c < 3 && ok; c++) {
                        char e = (ext[c]>='a'&&ext[c]<='z')?ext[c]-32:ext[c];
                        if ((uint8_t)entries[i].ext[c] != (uint8_t)e) ok=0;
                    }
                    if (ok) { *out_sector = lba; *out_idx = i; return 0; }
                }
            }
        }
        return -1;
    } else {
        /* Subdirectory — follow FAT cluster chain. */
        uint16_t cur = cluster;
        uint32_t limit = 4096;
        while (cur >= 2 && cur < FAT12_EOC && limit-- > 0) {
            uint32_t lba = fat12_fs.first_data_sector +
                           ((uint32_t)cur - 2) * fat12_fs.sectors_per_cluster;
            for (uint32_t s = 0; s < fat12_fs.sectors_per_cluster; s++) {
                if (ata_read_sector(lba + s, io_buf) != 0) return -1;
                fat12_dirent_t* entries = (fat12_dirent_t*)io_buf;
                uint32_t ecount = 512 / sizeof(fat12_dirent_t);
                for (uint32_t i = 0; i < ecount; i++) {
                    uint8_t first = entries[i].name[0];
                    if (name == 0) {
                        if (first == 0x00 || first == 0xE5) {
                            *out_sector = lba + s; *out_idx = i; return 0;
                        }
                    } else {
                        if (first == 0x00) return -1;
                        if (first == 0xE5) continue;
                        int ok = 1;
                        for (int c = 0; c < 8 && ok; c++) {
                            char e = (name[c]>='a'&&name[c]<='z')?name[c]-32:name[c];
                            if ((uint8_t)entries[i].name[c]!=(uint8_t)e) ok=0;
                        }
                        for (int c = 0; c < 3 && ok; c++) {
                            char e = (ext[c]>='a'&&ext[c]<='z')?ext[c]-32:ext[c];
                            if ((uint8_t)entries[i].ext[c]!=(uint8_t)e) ok=0;
                        }
                        if (ok) { *out_sector = lba+s; *out_idx = i; return 0; }
                    }
                }
            }
            cur = fat12_next_cluster(cur);
        }
        return -1;
    }
}

/* Backward-compat wrapper — always searches root. Used by fat12_chdir. */
static int fat12_find_root_entry(const char* name, const char* ext,
                                  uint32_t* out_sector, uint32_t* out_idx)
{
    return fat12_find_dir_entry(0, name, ext, out_sector, out_idx);
}

/* ── fat12_write_file ───────────────────────────────────────────────── */
int fat12_write_file(const char* name, const char* ext,
                     const uint8_t* buf, uint32_t len)
{
    if (!fat12_fs.mounted) return -1;

    /* Step 1: if file exists, free its old cluster chain. */
    uint32_t esector = 0, eidx = 0;
    int exists = fat12_find_dir_entry(fat12_cwd_cluster, name, ext, &esector, &eidx);
    if (exists == 0) {
        /* Read the sector again to get the entry */
        if (ata_read_sector(esector, io_buf) != 0) return -1;
        fat12_dirent_t* e = (fat12_dirent_t*)io_buf;
        /* Refuse to overwrite a directory entry with file data. */
        if (e[eidx].attr & FAT12_ATTR_DIRECTORY) return -1;
        uint16_t old_cluster = e[eidx].first_cluster;
        if (old_cluster >= 2) fat12_free_chain(old_cluster);
    }

    /* Step 2: allocate clusters for new data. */
    uint32_t spc    = fat12_fs.sectors_per_cluster;
    uint32_t bytes_per_cluster = spc * 512;
    uint32_t clusters_needed = (len + bytes_per_cluster - 1) / bytes_per_cluster;
    if (clusters_needed == 0) clusters_needed = 1;
    /* Always allocate at least one cluster even for empty files. */

    uint16_t first_cluster = 0;
    uint16_t prev_cluster  = 0;
    const uint8_t* src = buf;
    uint32_t remaining = len;

    for (uint32_t ci = 0; ci < clusters_needed; ci++) {
        uint16_t c = fat12_alloc_cluster();
        if (c == 0) return -1; /* disk full */

        /* Mark cluster as end-of-chain for now. */
        fat12_set_cluster(c, 0xFFF);
        if (prev_cluster != 0)
            fat12_set_cluster(prev_cluster, c); /* link previous → current */
        if (first_cluster == 0) first_cluster = c;
        prev_cluster = c;

        /* Write data sectors for this cluster. */
        uint32_t lba = fat12_fs.first_data_sector +
                       ((uint32_t)c - 2) * spc;
        for (uint32_t s = 0; s < spc; s++) {
            uint8_t sector_buf[512];
            uint32_t to_copy = remaining > 512 ? 512 : remaining;
            for (uint32_t b = 0; b < to_copy; b++)
                sector_buf[b] = src[b];
            for (uint32_t b = to_copy; b < 512; b++)
                sector_buf[b] = 0; /* zero-pad partial sector */
            if (ata_write_sector(lba + s, sector_buf) != 0) return -1;
            src       += to_copy;
            remaining -= to_copy;
        }
    }

    /* Step 3: flush FAT to disk before reading dir sector into io_buf,
       because fat12_flush_fat uses io_buf internally and would trash it. */
    if (fat12_flush_fat() != 0) return -1;

    /* Step 4: write/update root directory entry. */
    if (exists == 0) {
        /* Overwrite existing entry in place. */
        if (ata_read_sector(esector, io_buf) != 0) return -1;
        fat12_dirent_t* e = (fat12_dirent_t*)io_buf;
        e[eidx].first_cluster = first_cluster;
        e[eidx].file_size     = len;
        return ata_write_sector(esector, io_buf);
    }

    /* New entry — find free slot in current directory. */
    uint32_t fsector = 0, fidx = 0;
    if (fat12_find_dir_entry(fat12_cwd_cluster, 0, 0, &fsector, &fidx) != 0) return -1;
    if (ata_read_sector(fsector, io_buf) != 0) return -1;

    fat12_dirent_t* e = (fat12_dirent_t*)io_buf;
    /* Write 8.3 name — space pad, uppercase. */
    for (int i = 0; i < 8; i++) {
        char c = (i < 8 && name[i] && name[i]!=' ') ? name[i] : ' ';
        e[fidx].name[i] = (c>='a'&&c<='z') ? c-32 : c;
    }
    for (int i = 0; i < 3; i++) {
        char c = (i < 3 && ext[i] && ext[i]!=' ') ? ext[i] : ' ';
        e[fidx].ext[i] = (c>='a'&&c<='z') ? c-32 : c;
    }
    e[fidx].attr          = FAT12_ATTR_ARCHIVE;
    e[fidx].first_cluster = first_cluster;
    e[fidx].file_size     = len;
    e[fidx].time          = 0;
    e[fidx].date          = 0;
    for (int i = 0; i < 10; i++) e[fidx].reserved[i] = 0;

    return ata_write_sector(fsector, io_buf);
}

/* ── fat12_delete ───────────────────────────────────────────────────── */
int fat12_delete(const char* name, const char* ext)
{
    if (!fat12_fs.mounted) return -1;
    uint32_t esector = 0, eidx = 0;
    if (fat12_find_dir_entry(fat12_cwd_cluster, name, ext, &esector, &eidx) != 0) return -1;
    if (ata_read_sector(esector, io_buf) != 0) return -1;
    fat12_dirent_t* e = (fat12_dirent_t*)io_buf;
    /* Free the cluster chain before deleting the entry. */
    if (e[eidx].first_cluster >= 2)
        fat12_free_chain(e[eidx].first_cluster);
    fat12_flush_fat();
    /* Mark entry as deleted — 0xE5 is the FAT deleted-entry sentinel. */
    e[eidx].name[0] = 0xE5;
    return ata_write_sector(esector, io_buf);
}

/* ── fat12_mkdir ────────────────────────────────────────────────────── */
int fat12_mkdir(const char* name)
{
    if (!fat12_fs.mounted) return -1;

    /* Build space-padded 8-char uppercase name. */
    char n8[9];
    int i = 0;
    for (; i < 8 && name[i] && name[i]!=' '; i++) {
        char c = name[i];
        n8[i] = (c>='a'&&c<='z') ? c-32 : c;
    }
    for (; i < 8; i++) n8[i] = ' ';
    n8[8] = 0;

    /* Check if name already exists. */
    uint32_t es, ei;
    if (fat12_find_dir_entry(fat12_cwd_cluster, n8, "   ", &es, &ei) == 0) return -1;
    /* Already exists — return error. */

    /* Find a free slot in the current directory. */
    uint32_t fsector = 0, fidx = 0;
    if (fat12_find_dir_entry(fat12_cwd_cluster, 0, 0, &fsector, &fidx) != 0) return -1;
    if (ata_read_sector(fsector, io_buf) != 0) return -1;

    fat12_dirent_t* e = (fat12_dirent_t*)io_buf;
    for (int j = 0; j < 8; j++) e[fidx].name[j] = (uint8_t)n8[j];
    for (int j = 0; j < 3; j++) e[fidx].ext[j]  = ' ';
    /* Allocate a cluster for the directory data.
       Without a real cluster, cd sets cwd_cluster=0 which IS root —
       that is why writes and ls were going to root after cd. */
    uint16_t dir_cluster = fat12_alloc_cluster();
    if (dir_cluster == 0) return -1;
    fat12_set_cluster(dir_cluster, 0xFFF); /* mark end-of-chain */
    fat12_flush_fat();

    /* Zero out the cluster so it has no stale entries. */
    uint8_t zero[512];
    for (int z = 0; z < 512; z++) zero[z] = 0;
    uint32_t dir_lba = fat12_fs.first_data_sector +
                       ((uint32_t)dir_cluster - 2) * fat12_fs.sectors_per_cluster;
    for (uint32_t s = 0; s < fat12_fs.sectors_per_cluster; s++)
        ata_write_sector(dir_lba + s, zero);

    e[fidx].attr          = FAT12_ATTR_DIRECTORY;
    e[fidx].first_cluster = dir_cluster;
    e[fidx].file_size     = 0;
    e[fidx].time          = 0;
    e[fidx].date          = 0;
    for (int j = 0; j < 10; j++) e[fidx].reserved[j] = 0;

    return ata_write_sector(fsector, io_buf);
}

/* ── fat12_chdir_one ─────────────────────────────────────────────────
   Navigate one single path component (no slashes). Internal helper.
   Handles "." (no-op) and ".." (go up one level). */
static int fat12_chdir_one(const char* name)
{
    /* "." — current directory, no-op */
    if (name[0] == '.' && (name[1] == 0 || name[1] == ' ')) return 0;

    /* ".." — go up one level using the parent cluster stack. */
    if (name[0] == '.' && name[1] == '.' && (name[2] == 0 || name[2] == ' ')) {
        if (cwd_depth == 0) return 0; /* already at root */
        cwd_depth--;
        fat12_cwd_cluster = cwd_parent_stack[cwd_depth];
        /* Trim last path component. */
        int j = 0;
        while (fat12_cwd[j]) j++;
        j--;
        while (j > 0 && fat12_cwd[j] != '/') j--;
        if (j == 0) { fat12_cwd[0] = '/'; fat12_cwd[1] = 0; }
        else         { fat12_cwd[j] = 0; }
        return 0;
    }

    /* Build space-padded uppercase 8-char name. */
    char n8[9]; int i = 0;
    for (; i < 8 && name[i] && name[i] != ' ' && name[i] != '/'; i++) {
        char c = name[i];
        n8[i] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    for (; i < 8; i++) n8[i] = ' ';
    n8[8] = 0;

    uint32_t csector = 0, cidx = 0;
    if (fat12_find_dir_entry(fat12_cwd_cluster, n8, "   ", &csector, &cidx) != 0)
        return -1;
    if (ata_read_sector(csector, io_buf) != 0) return -1;
    fat12_dirent_t* entries = (fat12_dirent_t*)io_buf;
    if (!(entries[cidx].attr & FAT12_ATTR_DIRECTORY)) return -1;

    if (cwd_depth < CWD_STACK_DEPTH)
        cwd_parent_stack[cwd_depth++] = fat12_cwd_cluster;

    int pathlen = 0;
    while (fat12_cwd[pathlen]) pathlen++;
    if (pathlen > 1 && pathlen < FAT12_CWD_MAX - 1)
        fat12_cwd[pathlen++] = '/';
    for (int c = 0; c < 8 && entries[cidx].name[c] != ' '
                           && pathlen < FAT12_CWD_MAX - 1; c++) {
        char ch = (char)entries[cidx].name[c];
        /* Display dirs in lowercase in path for readability. */
        fat12_cwd[pathlen++] = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    }
    fat12_cwd[pathlen] = 0;
    fat12_cwd_cluster = entries[cidx].first_cluster;
    return 0;
}

/* ── fat12_chdir ────────────────────────────────────────────────────── */
int fat12_chdir(const char* name)
{
    if (!fat12_fs.mounted) return -1;

    /* Save current state for "cd -" */
    int ci = 0;
    while (fat12_cwd[ci]) { fat12_prev_cwd[ci] = fat12_cwd[ci]; ci++; }
    fat12_prev_cwd[ci] = 0;
    fat12_prev_cluster = fat12_cwd_cluster;
    prev_depth = cwd_depth;
    for (int i = 0; i < cwd_depth && i < CWD_STACK_DEPTH; i++)
        prev_parent_stack[i] = cwd_parent_stack[i];

    /* "cd -" — swap to previous directory */
    if (name[0] == '-' && (name[1] == 0 || name[1] == ' ')) {
        /* swap current ↔ prev */
        char   tmp_cwd[FAT12_CWD_MAX];
        uint16_t tmp_cluster = fat12_cwd_cluster;
        uint16_t tmp_stack[CWD_STACK_DEPTH];
        int tmp_depth = cwd_depth;
        for (int i = 0; i < FAT12_CWD_MAX; i++) tmp_cwd[i] = fat12_cwd[i];
        for (int i = 0; i < CWD_STACK_DEPTH; i++) tmp_stack[i] = cwd_parent_stack[i];

        for (int i = 0; i < FAT12_CWD_MAX; i++) fat12_cwd[i] = fat12_prev_cwd[i];
        fat12_cwd_cluster = fat12_prev_cluster;
        cwd_depth = prev_depth;
        for (int i = 0; i < CWD_STACK_DEPTH; i++) cwd_parent_stack[i] = prev_parent_stack[i];

        for (int i = 0; i < FAT12_CWD_MAX; i++) fat12_prev_cwd[i] = tmp_cwd[i];
        fat12_prev_cluster = tmp_cluster;
        prev_depth = tmp_depth;
        for (int i = 0; i < CWD_STACK_DEPTH; i++) prev_parent_stack[i] = tmp_stack[i];
        return 0;
    }

    /* "cd" or "cd /" — go to root */
    if (name[0] == 0 || name[0] == ' ' ||
        (name[0] == '/' && (name[1] == 0 || name[1] == ' '))) {
        fat12_cwd[0] = '/'; fat12_cwd[1] = 0;
        fat12_cwd_cluster = 0;
        cwd_depth = 0;
        return 0;
    }

    /* Absolute path: starts with '/' — reset to root first */
    const char* p = name;
    if (*p == '/') {
        fat12_cwd[0] = '/'; fat12_cwd[1] = 0;
        fat12_cwd_cluster = 0;
        cwd_depth = 0;
        p++;
    }

    /* Walk each slash-separated component. */
    while (*p) {
        /* Extract next component into component[] */
        char component[9];
        int ci2 = 0;
        while (*p && *p != '/' && ci2 < 8) {
            component[ci2++] = *p++;
        }
        component[ci2] = 0;
        if (*p == '/') p++;  /* skip slash separator */
        if (ci2 == 0) continue;  /* skip empty components (double-slash) */

        if (fat12_chdir_one(component) != 0) return -1;
    }
    return 0;
}

/* ── fat12_list_dir ─────────────────────────────────────────────────
   Lists entries in a directory. cluster=0 = root directory.
   cluster>0 = subdirectory starting at that data cluster.
   Only shows non-hidden, non-deleted, non-volume-label entries.
   This replaces fat12_list_root for CWD-aware ls. */
void fat12_list_dir(uint16_t cluster)
{
    if (!fat12_fs.mounted) return;

    if (cluster == 0) {
        /* Root directory: fixed location, fixed size. */
        for (uint32_t s = 0; s < fat12_fs.root_dir_sectors; s++) {
            if (ata_read_sector(fat12_fs.first_root_sector + s, io_buf) != 0)
                return;
            fat12_dirent_t* entries = (fat12_dirent_t*)io_buf;
            uint32_t ecount = 512 / sizeof(fat12_dirent_t);
            for (uint32_t i = 0; i < ecount; i++) {
                uint8_t first = entries[i].name[0];
                if (first == 0x00) return; /* no more entries */
                if (first == 0xE5) continue; /* deleted */
                if (first == '.') continue;  /* . and .. internal entries */
                uint8_t attr = entries[i].attr;
                if (attr & FAT12_ATTR_VOLUME_ID) continue;
                if (attr & FAT12_ATTR_HIDDEN)    continue;
                if (attr & FAT12_ATTR_SYSTEM)    continue;

                /* Print name */
                if (attr & FAT12_ATTR_DIRECTORY) {
                    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                    vga_print("  [");
                } else {
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    vga_print("   ");
                }
                for (int c = 0; c < 8; c++) {
                    if (entries[i].name[c] == ' ') break;
                    char ch[2] = {(char)entries[i].name[c], 0};
                    vga_print(ch);
                }
                if (!(attr & FAT12_ATTR_DIRECTORY)) {
                    vga_print(".");
                    for (int c = 0; c < 3; c++) {
                        if (entries[i].ext[c] == ' ') break;
                        char ch[2] = {(char)entries[i].ext[c], 0};
                        vga_print(ch);
                    }
                } else {
                    vga_print("]");
                }
                vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                if (!(attr & FAT12_ATTR_DIRECTORY)) {
                    vga_print("  (");
                    /* Print file size */
                    uint32_t sz = entries[i].file_size;
                    if (sz == 0) { vga_print("0"); }
                    else {
                        char nb[12]; nb[11]=0; int ni=10;
                        while (sz && ni>=0) {nb[ni--]='0'+(sz%10);sz/=10;}
                        vga_print(&nb[ni+1]);
                    }
                    vga_print(" bytes)");
                }
                vga_print("\n");
            }
        }
    } else {
        /* Subdirectory: follow FAT cluster chain. */
        uint16_t cur = cluster;
        uint32_t limit = 4096;
        while (cur >= 2 && cur < FAT12_EOC && limit-- > 0) {
            uint32_t lba = fat12_fs.first_data_sector +
                           ((uint32_t)cur - 2) * fat12_fs.sectors_per_cluster;
            for (uint32_t s = 0; s < fat12_fs.sectors_per_cluster; s++) {
                if (ata_read_sector(lba + s, io_buf) != 0) return;
                fat12_dirent_t* entries = (fat12_dirent_t*)io_buf;
                uint32_t ecount = 512 / sizeof(fat12_dirent_t);
                for (uint32_t i = 0; i < ecount; i++) {
                    uint8_t first = entries[i].name[0];
                    if (first == 0x00) return;
                    if (first == 0xE5) continue;
                    if (first == '.') continue;  /* . and .. internal entries */
                    uint8_t attr = entries[i].attr;
                    if (attr & FAT12_ATTR_VOLUME_ID) continue;
                    if (attr & FAT12_ATTR_HIDDEN)    continue;
                    if (attr & FAT12_ATTR_SYSTEM)    continue;
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    vga_print("   ");
                    for (int c = 0; c < 8; c++) {
                        if (entries[i].name[c] == ' ') break;
                        char ch[2] = {(char)entries[i].name[c], 0};
                        vga_print(ch);
                    }
                    vga_print(".");
                    for (int c = 0; c < 3; c++) {
                        if (entries[i].ext[c] == ' ') break;
                        char ch[2] = {(char)entries[i].ext[c], 0};
                        vga_print(ch);
                    }
                    vga_print("\n");
                }
            }
            cur = fat12_next_cluster(cur);
        }
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── fat12_save_cwd / fat12_restore_cwd ─────────────────────────────
   Save and restore the full CWD state (path, cluster, depth, stack).
   Used by shell commands that need to temporarily navigate elsewhere. */
void fat12_save_cwd(fat12_cwd_state_t *s)
{
    int i = 0;
    while (fat12_cwd[i]) { s->cwd[i] = fat12_cwd[i]; i++; }
    s->cwd[i]   = 0;
    s->cluster  = fat12_cwd_cluster;
    s->depth    = cwd_depth;
    for (int j = 0; j < CWD_STACK_DEPTH; j++)
        s->parent_stack[j] = cwd_parent_stack[j];
}

void fat12_restore_cwd(const fat12_cwd_state_t *s)
{
    int i = 0;
    while (s->cwd[i]) { fat12_cwd[i] = s->cwd[i]; i++; }
    fat12_cwd[i]      = 0;
    fat12_cwd_cluster = s->cluster;
    cwd_depth         = s->depth;
    for (int j = 0; j < CWD_STACK_DEPTH; j++)
        cwd_parent_stack[j] = s->parent_stack[j];
}

/* ── fat12_rename ────────────────────────────────────────────────────
   Rename an entry in the current directory by rewriting its 8.3 name.
   Returns 0 on success, -1 if not found. */
int fat12_rename(const char* old_name, const char* old_ext,
                 const char* new_name, const char* new_ext)
{
    if (!fat12_fs.mounted) return -1;
    uint32_t esector = 0, eidx = 0;
    if (fat12_find_dir_entry(fat12_cwd_cluster, old_name, old_ext,
                             &esector, &eidx) != 0) return -1;
    if (ata_read_sector(esector, io_buf) != 0) return -1;
    fat12_dirent_t *e = (fat12_dirent_t*)io_buf;
    for (int i = 0; i < 8; i++) {
        char c = (i < 8 && new_name[i] && new_name[i] != ' ') ? new_name[i] : ' ';
        e[eidx].name[i] = (c >= 'a' && c <= 'z') ? (uint8_t)(c-32) : (uint8_t)c;
    }
    for (int i = 0; i < 3; i++) {
        char c = (i < 3 && new_ext[i] && new_ext[i] != ' ') ? new_ext[i] : ' ';
        e[eidx].ext[i]  = (c >= 'a' && c <= 'z') ? (uint8_t)(c-32) : (uint8_t)c;
    }
    return ata_write_sector(esector, io_buf);
}

/* ── fat12_readdir ───────────────────────────────────────────────────────
   Fill buf with up to max_entries user-facing directory entries from the
   directory at cluster (0 = root, >0 = subdir).
   Returns number of entries stored. */
int fat12_readdir(uint16_t cluster, fat12_dentry_t *buf, int max_entries)
{
    if (!fat12_fs.mounted || !buf || max_entries <= 0) return 0;

    int count = 0;

#define FILL_DENTRY(raw, dst) do {                                    \
    int _n = 0;                                                       \
    for (int _i = 0; _i < 8 && (raw)->name[_i] != ' '; _i++)        \
        (dst)->name[_n++] = (char)(raw)->name[_i];                    \
    (dst)->name[_n] = 0;                                              \
    int _e = 0;                                                       \
    for (int _i = 0; _i < 3 && (raw)->ext[_i] != ' '; _i++)         \
        (dst)->ext[_e++] = (char)(raw)->ext[_i];                      \
    (dst)->ext[_e]  = 0;                                              \
    (dst)->is_dir   = ((raw)->attr & FAT12_ATTR_DIRECTORY) ? 1 : 0;  \
    (dst)->size     = (raw)->file_size;                               \
} while(0)

    static uint8_t rd_buf[512];

    if (cluster == 0) {
        for (uint32_t s = 0; s < fat12_fs.root_dir_sectors && count < max_entries; s++) {
            if (ata_read_sector(fat12_fs.first_root_sector + s, rd_buf) != 0) break;
            fat12_dirent_t *entries = (fat12_dirent_t*)rd_buf;
            uint32_t ecount = 512 / sizeof(fat12_dirent_t);
            for (uint32_t i = 0; i < ecount && count < max_entries; i++) {
                uint8_t first = entries[i].name[0];
                if (first == 0x00) goto done;
                if (first == 0xE5) continue;
                if (first == (uint8_t)'.') continue;
                uint8_t attr = entries[i].attr;
                if (attr & FAT12_ATTR_VOLUME_ID) continue;
                if (attr & FAT12_ATTR_HIDDEN)    continue;
                if (attr & FAT12_ATTR_SYSTEM)    continue;
                FILL_DENTRY(&entries[i], &buf[count]);
                count++;
            }
        }
    } else {
        uint16_t cur = cluster;
        uint32_t limit = 4096;
        while (cur >= 2 && cur < FAT12_EOC && limit-- > 0 && count < max_entries) {
            uint32_t lba = fat12_fs.first_data_sector +
                           ((uint32_t)cur - 2) * fat12_fs.sectors_per_cluster;
            for (uint32_t s = 0; s < fat12_fs.sectors_per_cluster && count < max_entries; s++) {
                if (ata_read_sector(lba + s, rd_buf) != 0) goto done;
                fat12_dirent_t *entries = (fat12_dirent_t*)rd_buf;
                uint32_t ecount = 512 / sizeof(fat12_dirent_t);
                for (uint32_t i = 0; i < ecount && count < max_entries; i++) {
                    uint8_t first = entries[i].name[0];
                    if (first == 0x00) goto done;
                    if (first == 0xE5) continue;
                    if (first == (uint8_t)'.') continue;
                    uint8_t attr = entries[i].attr;
                    if (attr & FAT12_ATTR_VOLUME_ID) continue;
                    if (attr & FAT12_ATTR_HIDDEN)    continue;
                    if (attr & FAT12_ATTR_SYSTEM)    continue;
                    FILL_DENTRY(&entries[i], &buf[count]);
                    count++;
                }
            }
            cur = fat12_next_cluster(cur);
        }
    }
done:
#undef FILL_DENTRY
    return count;
}
