/*
 * fat32.c — FAT32 filesystem driver for IronKernel
 *
 * Freestanding x86-64 kernel, no stdlib, no libc.
 * All buffers are static; no dynamic allocation.
 *
 * Disk parameters assumed (256 MB image via mkfs.fat -F 32 -s 8):
 *   512 bytes/sector, 8 sectors/cluster (4096 bytes/cluster)
 *   32 reserved sectors, 2 FAT copies
 *   FAT32 entries: 4 bytes each, bits 27:0 used (mask 0x0FFFFFFF)
 *
 * Compile flags:
 *   gcc -ffreestanding -fno-stack-protector -nostdlib -nostdinc
 *       -fno-builtin -mno-red-zone -c -mno-sse -mno-sse2 -mno-mmx
 *       -fno-pic -fno-pie
 */

#include "fat32.h"
#include "ata.h"
#include "../drivers/vga.h"

#define FAT32_FREE  0x00000000u

/* ═══════════════════════════════════════════════════════════════════
   STATIC BUFFERS
   ═══════════════════════════════════════════════════════════════════ */

/*
 * In-memory copy of FAT copy 0.
 * 512 sectors * 512 bytes = 262 144 bytes = 256 KB.
 * A 256 MB disk with 8 sectors/cluster has at most
 *   256*1024*1024 / (8*512) = 65 536 clusters
 * Each FAT32 entry is 4 bytes → 65 536 * 4 = 262 144 bytes.  Exact fit.
 */
static uint8_t g_fat_buf[512 * 512];

/*
 * Single-sector scratch buffer.  Reused across all operations.
 * IronKernel is single-threaded, so this is safe.
 */
static uint8_t g_io_buf[512];

/* ═══════════════════════════════════════════════════════════════════
   FILESYSTEM GEOMETRY  (populated by fat32_init)
   ═══════════════════════════════════════════════════════════════════ */

static int      g_mounted            = 0;
static uint32_t g_bytes_per_sector   = 512;
static uint32_t g_sectors_per_cluster = 1;
static uint32_t g_reserved_sectors   = 32;
static uint32_t g_fat_count          = 2;
static uint32_t g_sectors_per_fat    = 0;   /* FAT32 extended field */
static uint32_t g_total_sectors      = 0;
static uint32_t g_root_cluster       = 2;
static uint32_t g_first_fat_sector   = 0;
static uint32_t g_first_data_sector  = 0;

/* ═══════════════════════════════════════════════════════════════════
   CWD STATE
   ═══════════════════════════════════════════════════════════════════ */

char     fat32_cwd[FAT32_CWD_MAX]  = "/";
uint32_t fat32_cwd_cluster         = 0;   /* 0 → use g_root_cluster */

/* Parent-cluster stack for ".." navigation. */
#define CWD_STACK_DEPTH FAT32_STACK_DEPTH
static uint32_t g_parent_stack[CWD_STACK_DEPTH];
static int      g_cwd_depth = 0;

/* Previous-directory state for "cd -". */
static char     g_prev_cwd[FAT32_CWD_MAX] = "/";
static uint32_t g_prev_cluster            = 0;
static uint32_t g_prev_stack[CWD_STACK_DEPTH];
static int      g_prev_depth              = 0;

/* ═══════════════════════════════════════════════════════════════════
   SMALL INTERNAL HELPERS
   ═══════════════════════════════════════════════════════════════════ */

/* Print an unsigned 32-bit decimal integer via VGA. */
static void print_u32(uint32_t n)
{
    if (n == 0) { vga_print("0"); return; }
    char tmp[12];
    tmp[11] = '\0';
    int i = 10;
    while (n > 0 && i >= 0) {
        tmp[i--] = '0' + (int)(n % 10);
        n /= 10;
    }
    vga_print(&tmp[i + 1]);
}

/* Uppercase ASCII character. */
static char to_upper(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

/* Copy at most dst_max-1 characters of src into dst; always NUL-terminate. */
static void str_copy(char *dst, const char *src, int dst_max)
{
    int i = 0;
    while (i < dst_max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* String length (no stdlib). */
static int str_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Resolve the effective cluster for the CWD.
   When fat32_cwd_cluster == 0, treat it as g_root_cluster. */
static uint32_t cwd_cluster(void)
{
    return (fat32_cwd_cluster == 0) ? g_root_cluster : fat32_cwd_cluster;
}

/* ═══════════════════════════════════════════════════════════════════
   FAT ACCESS HELPERS
   ═══════════════════════════════════════════════════════════════════ */

/*
 * fat32_cluster_to_lba: first sector of data cluster N.
 * Formula: first_data_sector + (N - 2) * sectors_per_cluster
 */
static uint32_t fat32_cluster_to_lba(uint32_t cluster)
{
    return g_first_data_sector + (cluster - 2) * g_sectors_per_cluster;
}

/*
 * fat32_next_cluster: follow the FAT chain from cluster c.
 * Returns the next cluster number (masked to 28 bits).
 * Returns >= FAT32_EOC when at end-of-chain.
 */
static uint32_t fat32_next_cluster(uint32_t c)
{
    uint32_t byte_off = c * 4;
    if (byte_off + 3 >= (uint32_t)sizeof(g_fat_buf)) return FAT32_EOC;
    uint32_t val = (uint32_t)g_fat_buf[byte_off]
                 | ((uint32_t)g_fat_buf[byte_off + 1] << 8)
                 | ((uint32_t)g_fat_buf[byte_off + 2] << 16)
                 | ((uint32_t)g_fat_buf[byte_off + 3] << 24);
    return val & 0x0FFFFFFFU;
}

/*
 * fat32_set_cluster: write a FAT entry for cluster c.
 * Preserves the top 4 reserved bits of the existing entry.
 */
static void fat32_set_cluster(uint32_t c, uint32_t val)
{
    uint32_t byte_off = c * 4;
    if (byte_off + 3 >= (uint32_t)sizeof(g_fat_buf)) return;

    /* Read existing top nibble so we can preserve it. */
    uint32_t existing = (uint32_t)g_fat_buf[byte_off + 3];
    uint32_t top_nibble = existing & 0xF0; /* bits 31:28 */

    uint32_t entry = (val & 0x0FFFFFFFU) | ((uint32_t)top_nibble << 24);
    g_fat_buf[byte_off]     = (uint8_t)(entry & 0xFF);
    g_fat_buf[byte_off + 1] = (uint8_t)((entry >>  8) & 0xFF);
    g_fat_buf[byte_off + 2] = (uint8_t)((entry >> 16) & 0xFF);
    g_fat_buf[byte_off + 3] = (uint8_t)((entry >> 24) & 0xFF);
}

/*
 * fat32_alloc_cluster: find the first free cluster (value == 0).
 * Marks it as EOC before returning.
 * Returns 0 if disk is full.
 */
static uint32_t fat32_alloc_cluster(void)
{
    /* Derive maximum valid cluster number from disk geometry. */
    if (g_total_sectors <= g_first_data_sector) return 0; /* sanity guard */
    uint32_t data_sectors  = g_total_sectors - g_first_data_sector;
    uint32_t max_cluster   = (data_sectors / g_sectors_per_cluster) + 2;
    uint32_t fat_max       = (uint32_t)(sizeof(g_fat_buf) / 4);
    if (max_cluster > fat_max) max_cluster = fat_max;

    for (uint32_t c = 2; c < max_cluster; c++) {
        if (fat32_next_cluster(c) == FAT32_FREE) {
            fat32_set_cluster(c, 0x0FFFFFFFU); /* mark EOC */
            return c;
        }
    }
    return 0; /* no free cluster */
}

/*
 * fat32_free_chain: mark every cluster in the chain starting at c as free.
 * Stops at EOC or after a safety limit to avoid infinite loops.
 */
static void fat32_free_chain(uint32_t c)
{
    uint32_t limit = 65536;
    while (c >= 2 && (c & 0x0FFFFFFFU) < FAT32_EOC && limit-- > 0) {
        uint32_t next = fat32_next_cluster(c);
        fat32_set_cluster(c, FAT32_FREE);
        c = next;
    }
}

/*
 * fat32_flush_fat: write g_fat_buf back to BOTH FAT copies on disk.
 * Must be called after any FAT modification to make it durable.
 */
static int fat32_flush_fat(void)
{
    uint32_t sectors = g_sectors_per_fat;
    /* Cap to what we actually hold in g_fat_buf. */
    uint32_t buf_sectors = (uint32_t)(sizeof(g_fat_buf) / 512);
    if (sectors > buf_sectors) sectors = buf_sectors;

    for (uint32_t i = 0; i < sectors; i++) {
        const uint8_t *src = g_fat_buf + i * 512;

        /* FAT copy 0 */
        if (ata_write_sector(g_first_fat_sector + i, src) != 0) return -1;

        /* FAT copy 1 (backup) */
        if (g_fat_count >= 2) {
            if (ata_write_sector(g_first_fat_sector + g_sectors_per_fat + i,
                                 src) != 0) return -1;
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   DIRECTORY ENTRY HELPERS
   ═══════════════════════════════════════════════════════════════════ */

/*
 * Entries per 512-byte sector (each entry is 32 bytes).
 */
#define ENTRIES_PER_SECTOR  16

/*
 * Compare a raw 8-char FAT name field against a caller-supplied name,
 * case-insensitively, where the raw field is space-padded to 8 chars.
 * Returns 1 if equal, 0 if not.
 */
static int match_name83(const uint8_t *raw8, const char *name)
{
    int i = 0;
    /* Compare provided characters. */
    for (; i < 8 && name[i] != '\0'; i++) {
        char rch = (char)raw8[i];
        char nch = to_upper(name[i]);
        char rup = to_upper(rch);
        if (rup != nch) return 0;
    }
    /* After the name ends, the remaining raw bytes must be spaces. */
    for (; i < 8; i++) {
        if (raw8[i] != ' ') return 0;
    }
    return 1;
}

/*
 * Same as match_name83 but for the 3-char extension field.
 */
static int match_ext83(const uint8_t *raw3, const char *ext)
{
    int i = 0;
    for (; i < 3 && ext[i] != '\0'; i++) {
        char rch = (char)raw3[i];
        char ech = to_upper(ext[i]);
        char rup = to_upper(rch);
        if (rup != ech) return 0;
    }
    for (; i < 3; i++) {
        if (raw3[i] != ' ') return 0;
    }
    return 1;
}

/*
 * Write a space-padded, uppercase 8-char name into dst[8].
 */
static void encode_name83(uint8_t *dst, const char *src, int maxlen)
{
    int i = 0;
    for (; i < maxlen && src[i] != '\0' && src[i] != ' '; i++)
        dst[i] = (uint8_t)to_upper(src[i]);
    for (; i < maxlen; i++)
        dst[i] = ' ';
}

/*
 * Extract a cluster number from a raw directory entry.
 */
static uint32_t dirent_cluster(const fat32_dirent_t *e)
{
    return ((uint32_t)e->first_cluster_hi << 16) |
            (uint32_t)e->first_cluster_lo;
}

/*
 * Write a cluster number into a raw directory entry.
 */
static void dirent_set_cluster(fat32_dirent_t *e, uint32_t c)
{
    e->first_cluster_hi = (uint16_t)((c >> 16) & 0xFFFF);
    e->first_cluster_lo = (uint16_t)(c & 0xFFFF);
}

/* ═══════════════════════════════════════════════════════════════════
   FAT32 DIRECTORY ITERATION ENGINE
   ═══════════════════════════════════════════════════════════════════

   Two modes controlled by the `name` pointer:
     name == NULL  →  find the first free (0x00 or 0xE5) slot.
     name != NULL  →  find an existing entry matching name+ext.

   Either way, on success:
     *out_lba  = LBA of the sector containing the entry
     *out_idx  = index (0..15) within that sector
   Returns 0 on success, -1 if not found.

   The function also supports appending a new cluster to the chain
   when looking for a free slot and none is found in existing clusters
   (only when need_free == 1).  This is handled by the caller.
   ═══════════════════════════════════════════════════════════════════ */

static int fat32_find_entry(uint32_t dir_cluster,
                            const char *name, const char *ext,
                            uint32_t *out_lba, uint32_t *out_idx)
{
    uint32_t cur   = dir_cluster;
    uint32_t limit = 65536; /* cluster chain safety cap */

    while (cur >= 2 && (cur & 0x0FFFFFFFU) < FAT32_EOC && limit-- > 0) {
        uint32_t lba = fat32_cluster_to_lba(cur);

        for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
            if (ata_read_sector(lba + s, g_io_buf) != 0) return -1;
            fat32_dirent_t *entries = (fat32_dirent_t *)g_io_buf;

            for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                uint8_t first = entries[i].name[0];

                if (name == (void*)0) {
                    /* Looking for a free slot. */
                    if (first == 0x00 || first == 0xE5) {
                        *out_lba = lba + s;
                        *out_idx = (uint32_t)i;
                        return 0;
                    }
                } else {
                    /* Looking for a named entry. */
                    if (first == 0x00) return -1; /* no more entries in dir */
                    if (first == 0xE5) continue;  /* deleted */

                    uint8_t attr = entries[i].attr;
                    if (attr == FAT32_ATTR_LFN)        continue; /* LFN */
                    if (attr & FAT32_ATTR_VOLUME_ID)   continue; /* volume label */

                    if (match_name83(entries[i].name, name) &&
                        match_ext83(entries[i].ext,   ext)) {
                        *out_lba = lba + s;
                        *out_idx = (uint32_t)i;
                        return 0;
                    }
                }
            }
        }
        cur = fat32_next_cluster(cur);
    }
    return -1;
}

/*
 * fat32_find_free_slot: find a free directory slot in dir_cluster.
 * If the cluster chain is full, allocate and zero a new cluster,
 * link it to the chain, flush FAT, then return the first slot of the
 * new cluster.
 */
static int fat32_find_free_slot(uint32_t dir_cluster,
                                uint32_t *out_lba, uint32_t *out_idx)
{
    if (fat32_find_entry(dir_cluster, (void*)0, (void*)0,
                         out_lba, out_idx) == 0)
        return 0;

    /*
     * No free slot found — walk to the last cluster in the chain
     * and allocate a new one.
     */
    uint32_t cur   = dir_cluster;
    uint32_t limit = 65536;
    while (1) {
        uint32_t next = fat32_next_cluster(cur);
        if ((next & 0x0FFFFFFFU) >= FAT32_EOC) break; /* cur is last */
        if (next < 2 || limit-- == 0) return -1;
        cur = next;
    }

    uint32_t new_c = fat32_alloc_cluster();
    if (new_c == 0) return -1; /* disk full */

    /* Link cur → new_c → EOC */
    fat32_set_cluster(cur, new_c);
    fat32_set_cluster(new_c, 0x0FFFFFFFU);

    if (fat32_flush_fat() != 0) return -1;

    /* Zero-fill the new cluster so existing entries are clean. */
    uint32_t lba = fat32_cluster_to_lba(new_c);
    static uint8_t zero_sector[512]; /* static to avoid large stack frame */
    for (int z = 0; z < 512; z++) zero_sector[z] = 0;
    for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
        if (ata_write_sector(lba + s, zero_sector) != 0) return -1;
    }

    *out_lba = lba;
    *out_idx = 0;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
   PUBLIC API IMPLEMENTATION
   ═══════════════════════════════════════════════════════════════════ */

/* ── fat32_init ──────────────────────────────────────────────────── */
int fat32_init(void)
{
    g_mounted = 0;

    if (!ata_master.present) return -1;

    /* Read and parse boot sector (LBA 0). */
    if (ata_read_sector(0, g_io_buf) != 0) return -1;

    /* --- BPB fields (all little-endian) --- */
    uint16_t bytes_per_sector  = (uint16_t)g_io_buf[11]
                               | ((uint16_t)g_io_buf[12] << 8);
    uint8_t  spc               = g_io_buf[13];
    uint16_t reserved          = (uint16_t)g_io_buf[14]
                               | ((uint16_t)g_io_buf[15] << 8);
    uint8_t  fat_count_raw     = g_io_buf[16];
    uint16_t root_entry_count  = (uint16_t)g_io_buf[17]
                               | ((uint16_t)g_io_buf[18] << 8);
    uint16_t total_sec_16      = (uint16_t)g_io_buf[19]
                               | ((uint16_t)g_io_buf[20] << 8);
    uint16_t spf16             = (uint16_t)g_io_buf[22]
                               | ((uint16_t)g_io_buf[23] << 8);
    uint32_t total_sec_32      = (uint32_t)g_io_buf[32]
                               | ((uint32_t)g_io_buf[33] << 8)
                               | ((uint32_t)g_io_buf[34] << 16)
                               | ((uint32_t)g_io_buf[35] << 24);
    uint32_t spf32             = (uint32_t)g_io_buf[36]
                               | ((uint32_t)g_io_buf[37] << 8)
                               | ((uint32_t)g_io_buf[38] << 16)
                               | ((uint32_t)g_io_buf[39] << 24);
    uint32_t root_cluster_raw  = (uint32_t)g_io_buf[44]
                               | ((uint32_t)g_io_buf[45] << 8)
                               | ((uint32_t)g_io_buf[46] << 16)
                               | ((uint32_t)g_io_buf[47] << 24);

    /*
     * FAT32 validation:
     *   sectors_per_fat_16 == 0  AND  root_entry_count == 0
     * OR the filesystem-type string at offset 82 reads "FAT32   ".
     */
    int is_fat32 = 0;
    if (spf16 == 0 && root_entry_count == 0 && spf32 != 0) {
        is_fat32 = 1;
    }
    /* Also check the FS type string at offset 82. */
    if (!is_fat32) {
        const char *sig = "FAT32   ";
        int match = 1;
        for (int k = 0; k < 8; k++) {
            if (g_io_buf[82 + k] != (uint8_t)sig[k]) { match = 0; break; }
        }
        if (match) is_fat32 = 1;
    }
    if (!is_fat32) return -1;

    /* Populate geometry. */
    g_bytes_per_sector    = bytes_per_sector ? bytes_per_sector : 512;
    g_sectors_per_cluster = spc ? spc : 1;
    g_reserved_sectors    = reserved;
    g_fat_count           = fat_count_raw;
    g_sectors_per_fat     = spf32;
    g_total_sectors       = total_sec_16 ? (uint32_t)total_sec_16 : total_sec_32;
    g_root_cluster        = root_cluster_raw ? root_cluster_raw : 2;

    g_first_fat_sector    = g_reserved_sectors;
    g_first_data_sector   = g_reserved_sectors
                          + g_fat_count * g_sectors_per_fat;

    /* Load FAT copy 0 into g_fat_buf. */
    uint32_t fat_sectors = g_sectors_per_fat;
    uint32_t buf_max     = (uint32_t)(sizeof(g_fat_buf) / 512);
    if (fat_sectors > buf_max) fat_sectors = buf_max;

    for (uint32_t i = 0; i < fat_sectors; i++) {
        if (ata_read_sector(g_first_fat_sector + i,
                            g_fat_buf + i * 512) != 0) return -1;
    }

    /* Initialise CWD to root. */
    fat32_cwd[0] = '/'; fat32_cwd[1] = '\0';
    fat32_cwd_cluster = 0; /* 0 = use g_root_cluster */
    g_cwd_depth = 0;

    g_mounted = 1;
    vga_print("[FAT32] mounted OK\n");
    return 0;
}

/* ── fat32_print_info ────────────────────────────────────────────── */
void fat32_print_info(void)
{
    if (!g_mounted) { vga_print("  FAT32 not mounted.\n"); return; }

    /* Scan FAT for free clusters — O(total_clusters) but fine at boot. */
    uint32_t data_sectors = (g_total_sectors > g_first_data_sector)
                          ? g_total_sectors - g_first_data_sector : 0;
    uint32_t total_clust  = (g_sectors_per_cluster > 0)
                          ? data_sectors / g_sectors_per_cluster : 0;
    uint32_t max_clust    = total_clust + 2;
    uint32_t fat_max      = (uint32_t)(sizeof(g_fat_buf) / 4);
    if (max_clust > fat_max) max_clust = fat_max;

    uint32_t free_clust = 0;
    for (uint32_t c = 2; c < max_clust; c++) {
        if (fat32_next_cluster(c) == FAT32_FREE) free_clust++;
    }

    /* Try to find a volume label in the root directory. */
    char label[12];
    label[0] = '\0';
    {
        uint32_t cur   = g_root_cluster;
        uint32_t limit = 256;
        int found_label = 0;
        while (cur >= 2 && (cur & 0x0FFFFFFFU) < FAT32_EOC
               && limit-- > 0 && !found_label) {
            uint32_t lba = fat32_cluster_to_lba(cur);
            for (uint32_t s = 0; s < g_sectors_per_cluster && !found_label; s++) {
                if (ata_read_sector(lba + s, g_io_buf) != 0) break;
                fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;
                for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                    if (e[i].name[0] == 0x00) { found_label = 1; break; }
                    if (e[i].name[0] == 0xE5) continue;
                    if (e[i].attr == FAT32_ATTR_VOLUME_ID) {
                        int li = 0;
                        for (int k = 0; k < 8 && li < 11; k++) {
                            if (e[i].name[k] != ' ')
                                label[li++] = (char)e[i].name[k];
                        }
                        for (int k = 0; k < 3 && li < 11; k++) {
                            if (e[i].ext[k] != ' ')
                                label[li++] = (char)e[i].ext[k];
                        }
                        label[li] = '\0';
                        found_label = 1;
                        break;
                    }
                }
            }
            cur = fat32_next_cluster(cur);
        }
    }

    vga_print("\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print("  FAT32 Volume Info\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    vga_print("    Label             : ");
    vga_print(label[0] ? label : "(none)");
    vga_print("\n");

    vga_print("    Bytes/sector      : "); print_u32(g_bytes_per_sector);    vga_print("\n");
    vga_print("    Sectors/cluster   : "); print_u32(g_sectors_per_cluster); vga_print("\n");
    vga_print("    Bytes/cluster     : ");
    print_u32(g_bytes_per_sector * g_sectors_per_cluster);
    vga_print("\n");
    vga_print("    Reserved sectors  : "); print_u32(g_reserved_sectors);    vga_print("\n");
    vga_print("    FAT copies        : "); print_u32(g_fat_count);           vga_print("\n");
    vga_print("    Sectors/FAT       : "); print_u32(g_sectors_per_fat);     vga_print("\n");
    vga_print("    Total sectors     : "); print_u32(g_total_sectors);       vga_print("\n");
    vga_print("    Root cluster      : "); print_u32(g_root_cluster);        vga_print("\n");
    vga_print("    Data start sector : "); print_u32(g_first_data_sector);   vga_print("\n");
    vga_print("    Total clusters    : "); print_u32(total_clust);           vga_print("\n");
    vga_print("    Free clusters     : "); print_u32(free_clust);            vga_print("\n");
    vga_print("    Free space (KB)   : ");
    print_u32(free_clust * g_sectors_per_cluster / 2);
    vga_print("\n");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print("    Status            : MOUNTED\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── fat32_readdir ───────────────────────────────────────────────── */
int fat32_readdir(uint32_t cluster, fat32_dentry_t *buf, int max_entries)
{
    if (!g_mounted || !buf || max_entries <= 0) return 0;

    /* A cluster of 0 from the caller means root. */
    uint32_t cur   = (cluster == 0) ? g_root_cluster : cluster;
    int      count = 0;
    uint32_t limit = 65536;

    while (cur >= 2 && (cur & 0x0FFFFFFFU) < FAT32_EOC
           && count < max_entries && limit-- > 0) {
        uint32_t lba = fat32_cluster_to_lba(cur);

        for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
            if (ata_read_sector(lba + s, g_io_buf) != 0) return count;
            fat32_dirent_t *entries = (fat32_dirent_t *)g_io_buf;

            for (int i = 0; i < ENTRIES_PER_SECTOR && count < max_entries; i++) {
                uint8_t first = entries[i].name[0];
                if (first == 0x00) return count; /* end of directory */
                if (first == 0xE5) continue;     /* deleted */
                if (first == ' ')  continue;     /* invalid/corrupt entry */

                uint8_t attr = entries[i].attr;
                if (attr == FAT32_ATTR_LFN)      continue; /* LFN */
                if (attr & FAT32_ATTR_VOLUME_ID) continue; /* volume label */

                /* Fill user-facing entry. */
                fat32_dentry_t *d = &buf[count];
                int ni = 0;
                for (int k = 0; k < 8 && entries[i].name[k] != ' '; k++)
                    d->name[ni++] = (char)entries[i].name[k];
                d->name[ni] = '\0';

                int ei = 0;
                for (int k = 0; k < 3 && entries[i].ext[k] != ' '; k++)
                    d->ext[ei++] = (char)entries[i].ext[k];
                d->ext[ei] = '\0';

                d->is_dir = (attr & FAT32_ATTR_DIRECTORY) ? 1 : 0;
                d->size   = entries[i].file_size;
                count++;
            }
        }
        cur = fat32_next_cluster(cur);
    }
    return count;
}

/* ── fat32_list_dir ──────────────────────────────────────────────── */
void fat32_list_dir(uint32_t cluster)
{
    if (!g_mounted) return;

    uint32_t cur   = (cluster == 0) ? g_root_cluster : cluster;
    uint32_t found = 0;
    uint32_t limit = 65536;

    while (cur >= 2 && (cur & 0x0FFFFFFFU) < FAT32_EOC && limit-- > 0) {
        uint32_t lba = fat32_cluster_to_lba(cur);

        for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
            if (ata_read_sector(lba + s, g_io_buf) != 0) return;
            fat32_dirent_t *entries = (fat32_dirent_t *)g_io_buf;

            for (int i = 0; i < ENTRIES_PER_SECTOR; i++) {
                uint8_t first = entries[i].name[0];
                if (first == 0x00) goto list_done;
                if (first == 0xE5) continue;
                if (first == '.')  continue; /* hide . and .. */
                if (first == ' ')  continue; /* skip invalid/corrupt entries */

                uint8_t attr = entries[i].attr;
                if (attr == FAT32_ATTR_LFN)      continue;
                if (attr & FAT32_ATTR_VOLUME_ID) continue;
                if (attr & FAT32_ATTR_HIDDEN)    continue;
                if (attr & FAT32_ATTR_SYSTEM)    continue;

                if (attr & FAT32_ATTR_DIRECTORY) {
                    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                    vga_print("  [");
                } else {
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    vga_print("   ");
                }

                /* Print name. */
                for (int k = 0; k < 8; k++) {
                    if (entries[i].name[k] == ' ') break;
                    char ch[2]; ch[0] = (char)entries[i].name[k]; ch[1] = '\0';
                    vga_print(ch);
                }

                if (attr & FAT32_ATTR_DIRECTORY) {
                    vga_print("]");
                } else {
                    /* Print extension. */
                    int has_ext = 0;
                    for (int k = 0; k < 3; k++)
                        if (entries[i].ext[k] != ' ') { has_ext = 1; break; }
                    if (has_ext) {
                        vga_print(".");
                        for (int k = 0; k < 3; k++) {
                            if (entries[i].ext[k] == ' ') break;
                            char ch[2]; ch[0] = (char)entries[i].ext[k]; ch[1] = '\0';
                            vga_print(ch);
                        }
                    }
                    /* Print file size. */
                    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                    vga_print("  (");
                    print_u32(entries[i].file_size);
                    vga_print(" bytes)");
                }
                vga_print("\n");
                found++;
            }
        }
        cur = fat32_next_cluster(cur);
    }
list_done:
    if (found == 0) {
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_print("  (empty)\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ── fat32_read_file ─────────────────────────────────────────────── */
int fat32_read_file(const char *name, const char *ext,
                    uint8_t *buf, uint32_t buf_size,
                    uint32_t *bytes_read)
{
    if (!g_mounted) return -1;
    *bytes_read = 0;

    uint32_t dir = cwd_cluster();
    uint32_t elba = 0, eidx = 0;
    if (fat32_find_entry(dir, name, ext, &elba, &eidx) != 0) return -1;

    /* Re-read the sector to get the entry. */
    if (ata_read_sector(elba, g_io_buf) != 0) return -1;
    fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;

    /* Must be a file, not a directory. */
    if (e[eidx].attr & FAT32_ATTR_DIRECTORY) return -1;

    uint32_t file_cluster = dirent_cluster(&e[eidx]);
    uint32_t remaining    = e[eidx].file_size;

    /* We need a separate read buffer so we don't clobber g_io_buf
       while iterating; reuse g_io_buf sector-by-sector is fine since
       we copy before advancing. */
    static uint8_t rd_buf[512];

    uint32_t cur   = file_cluster;
    uint32_t limit = 65536;

    while (cur >= 2 && (cur & 0x0FFFFFFFU) < FAT32_EOC
           && remaining > 0 && limit-- > 0) {
        uint32_t lba = fat32_cluster_to_lba(cur);

        for (uint32_t s = 0; s < g_sectors_per_cluster && remaining > 0; s++) {
            if (ata_read_sector(lba + s, rd_buf) != 0) return -1;

            uint32_t to_copy = 512;
            if (to_copy > remaining)              to_copy = remaining;
            if (*bytes_read + to_copy > buf_size) to_copy = buf_size - *bytes_read;
            if (to_copy == 0) break;

            for (uint32_t b = 0; b < to_copy; b++)
                buf[(*bytes_read)++] = rd_buf[b];

            remaining -= to_copy;
        }
        cur = fat32_next_cluster(cur);
    }
    return 0;
}

/* ── fat32_write_file ────────────────────────────────────────────── */
int fat32_write_file(const char *name, const char *ext,
                     const uint8_t *buf, uint32_t len)
{
    if (!g_mounted) return -1;

    uint32_t dir    = cwd_cluster();
    uint32_t elba   = 0, eidx = 0;
    int      exists = fat32_find_entry(dir, name, ext, &elba, &eidx);

    /* If file exists, free its old cluster chain. */
    if (exists == 0) {
        if (ata_read_sector(elba, g_io_buf) != 0) return -1;
        fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;
        /* Do not overwrite a directory with file data. */
        if (e[eidx].attr & FAT32_ATTR_DIRECTORY) return -1;
        uint32_t old_clust = dirent_cluster(&e[eidx]);
        if (old_clust >= 2) fat32_free_chain(old_clust);
    }

    /* Allocate clusters and write data. */
    uint32_t bytes_per_cluster = g_sectors_per_cluster * 512;
    uint32_t clusters_needed   = (len + bytes_per_cluster - 1) / bytes_per_cluster;
    if (clusters_needed == 0) clusters_needed = 1;

    uint32_t        first_cluster = 0;
    uint32_t        prev_cluster  = 0;
    const uint8_t  *src           = buf;
    uint32_t        remaining     = len;

    static uint8_t wr_sector[512];

    for (uint32_t ci = 0; ci < clusters_needed; ci++) {
        uint32_t c = fat32_alloc_cluster();
        if (c == 0) return -1; /* disk full */

        /* fat32_alloc_cluster already marks c as EOC. */
        if (prev_cluster != 0)
            fat32_set_cluster(prev_cluster, c); /* link chain */
        if (first_cluster == 0)
            first_cluster = c;
        prev_cluster = c;

        uint32_t lba = fat32_cluster_to_lba(c);
        for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
            uint32_t to_copy = (remaining > 512) ? 512 : remaining;

            for (uint32_t b = 0; b < to_copy; b++)
                wr_sector[b] = src[b];
            for (uint32_t b = to_copy; b < 512; b++)
                wr_sector[b] = 0;

            if (ata_write_sector(lba + s, wr_sector) != 0) return -1;

            src       += to_copy;
            remaining -= to_copy;
        }
    }

    /* Flush FAT before touching the directory sector (g_io_buf is shared). */
    if (fat32_flush_fat() != 0) return -1;

    if (exists == 0) {
        /* Update existing entry in-place. */
        if (ata_read_sector(elba, g_io_buf) != 0) return -1;
        fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;
        dirent_set_cluster(&e[eidx], first_cluster);
        e[eidx].file_size = len;
        return ata_write_sector(elba, g_io_buf);
    }

    /* New entry — find a free slot in the current directory. */
    uint32_t fsector = 0, fidx = 0;
    if (fat32_find_free_slot(dir, &fsector, &fidx) != 0) return -1;
    if (ata_read_sector(fsector, g_io_buf) != 0) return -1;

    fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;
    encode_name83(e[fidx].name, name, 8);
    encode_name83(e[fidx].ext,  ext,  3);
    e[fidx].attr          = FAT32_ATTR_ARCHIVE;
    e[fidx].nt_reserved   = 0;
    e[fidx].crt_time_tenth = 0;
    e[fidx].crt_time      = 0;
    e[fidx].crt_date      = 0;
    e[fidx].wrt_time      = 0;
    e[fidx].wrt_date      = 0;
    dirent_set_cluster(&e[fidx], first_cluster);
    e[fidx].file_size     = len;

    return ata_write_sector(fsector, g_io_buf);
}

/* ── fat32_delete ────────────────────────────────────────────────── */
int fat32_delete(const char *name, const char *ext)
{
    if (!g_mounted) return -1;

    uint32_t dir  = cwd_cluster();
    uint32_t elba = 0, eidx = 0;
    if (fat32_find_entry(dir, name, ext, &elba, &eidx) != 0) return -1;

    if (ata_read_sector(elba, g_io_buf) != 0) return -1;
    fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;

    uint32_t c = dirent_cluster(&e[eidx]);
    if (c >= 2) fat32_free_chain(c);
    fat32_flush_fat();

    e[eidx].name[0] = 0xE5; /* deleted marker */
    return ata_write_sector(elba, g_io_buf);
}

/* ── fat32_mkdir ─────────────────────────────────────────────────── */
int fat32_mkdir(const char *name)
{
    if (!g_mounted) return -1;

    uint32_t dir = cwd_cluster();

    /* Build space-padded uppercase 8-char name for duplicate check. */
    char n8[9];
    encode_name83((uint8_t *)n8, name, 8);
    n8[8] = '\0';

    /* Check if name already exists. */
    uint32_t tes, tei;
    if (fat32_find_entry(dir, n8, "   ", &tes, &tei) == 0) return -1;

    /* Allocate one cluster for the new directory's data. */
    uint32_t new_c = fat32_alloc_cluster();
    if (new_c == 0) return -1;
    fat32_set_cluster(new_c, 0x0FFFFFFFU); /* EOC */

    /* Determine the cluster to record for ".." */
    uint32_t parent_c = dir;

    if (fat32_flush_fat() != 0) return -1;

    /* Zero-fill the new cluster. */
    uint32_t new_lba = fat32_cluster_to_lba(new_c);
    static uint8_t zero_sector[512];
    for (int z = 0; z < 512; z++) zero_sector[z] = 0;
    for (uint32_t s = 0; s < g_sectors_per_cluster; s++) {
        if (ata_write_sector(new_lba + s, zero_sector) != 0) return -1;
    }

    /* Write the "." and ".." entries into the first sector. */
    if (ata_read_sector(new_lba, g_io_buf) != 0) return -1;
    fat32_dirent_t *entries = (fat32_dirent_t *)g_io_buf;

    /* "." entry */
    for (int k = 0; k < 8; k++) entries[0].name[k] = ' ';
    entries[0].name[0] = '.';
    for (int k = 0; k < 3; k++) entries[0].ext[k]  = ' ';
    entries[0].attr           = FAT32_ATTR_DIRECTORY;
    entries[0].nt_reserved    = 0;
    entries[0].crt_time_tenth = 0;
    entries[0].crt_time       = 0;
    entries[0].crt_date       = 0;
    entries[0].wrt_time       = 0;
    entries[0].wrt_date       = 0;
    dirent_set_cluster(&entries[0], new_c);
    entries[0].file_size      = 0;

    /* ".." entry */
    for (int k = 0; k < 8; k++) entries[1].name[k] = ' ';
    entries[1].name[0] = '.';
    entries[1].name[1] = '.';
    for (int k = 0; k < 3; k++) entries[1].ext[k]  = ' ';
    entries[1].attr           = FAT32_ATTR_DIRECTORY;
    entries[1].nt_reserved    = 0;
    entries[1].crt_time_tenth = 0;
    entries[1].crt_time       = 0;
    entries[1].crt_date       = 0;
    entries[1].wrt_time       = 0;
    entries[1].wrt_date       = 0;
    /* On FAT32, ".." in a directory directly under root points to root_cluster.
       More generally it points to the parent cluster. */
    dirent_set_cluster(&entries[1], parent_c);
    entries[1].file_size      = 0;

    if (ata_write_sector(new_lba, g_io_buf) != 0) return -1;

    /* Now add a directory entry in the current (parent) directory. */
    uint32_t fsector = 0, fidx = 0;
    if (fat32_find_free_slot(dir, &fsector, &fidx) != 0) return -1;
    if (ata_read_sector(fsector, g_io_buf) != 0) return -1;

    fat32_dirent_t *pe = (fat32_dirent_t *)g_io_buf;
    encode_name83(pe[fidx].name, name, 8);
    for (int k = 0; k < 3; k++) pe[fidx].ext[k] = ' ';
    pe[fidx].attr           = FAT32_ATTR_DIRECTORY;
    pe[fidx].nt_reserved    = 0;
    pe[fidx].crt_time_tenth = 0;
    pe[fidx].crt_time       = 0;
    pe[fidx].crt_date       = 0;
    pe[fidx].wrt_time       = 0;
    pe[fidx].wrt_date       = 0;
    dirent_set_cluster(&pe[fidx], new_c);
    pe[fidx].file_size      = 0;

    return ata_write_sector(fsector, g_io_buf);
}

/* ═══════════════════════════════════════════════════════════════════
   CHDIR HELPERS
   ═══════════════════════════════════════════════════════════════════ */

/*
 * Trim the last slash-delimited component from the CWD path string.
 */
static void cwd_path_trim_last(void)
{
    int j = str_len(fat32_cwd);
    j--;
    /* Skip trailing slash if any (shouldn't happen on non-root, but guard). */
    while (j > 0 && fat32_cwd[j] == '/') j--;
    while (j > 0 && fat32_cwd[j] != '/') j--;
    if (j == 0) {
        fat32_cwd[0] = '/';
        fat32_cwd[1] = '\0';
    } else {
        fat32_cwd[j] = '\0';
    }
}

/*
 * Navigate one path component (no slashes). Internal helper.
 * Handles ".", "..", and normal directory names.
 * Returns 0 on success, -1 on error.
 */
static int fat32_chdir_one(const char *component)
{
    /* "." — current directory, no-op. */
    if (component[0] == '.' &&
        (component[1] == '\0' || component[1] == ' '))
        return 0;

    /* ".." — go up one level. */
    if (component[0] == '.' && component[1] == '.' &&
        (component[2] == '\0' || component[2] == ' ')) {
        if (g_cwd_depth == 0) {
            /* Already at root — nothing to do. */
            return 0;
        }
        g_cwd_depth--;
        fat32_cwd_cluster = g_parent_stack[g_cwd_depth];
        cwd_path_trim_last();
        return 0;
    }

    /* Normal directory name — look it up. */
    uint32_t dir = cwd_cluster();
    uint32_t elba = 0, eidx = 0;
    if (fat32_find_entry(dir, component, "   ", &elba, &eidx) != 0) return -1;

    if (ata_read_sector(elba, g_io_buf) != 0) return -1;
    fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;

    if (!(e[eidx].attr & FAT32_ATTR_DIRECTORY)) return -1; /* not a dir */

    uint32_t new_cluster = dirent_cluster(&e[eidx]);

    /* Push current cluster to parent stack. */
    if (g_cwd_depth < CWD_STACK_DEPTH)
        g_parent_stack[g_cwd_depth++] = fat32_cwd_cluster;

    /* Update path string. */
    int pathlen = str_len(fat32_cwd);
    if (pathlen > 1 && pathlen < FAT32_CWD_MAX - 1)
        fat32_cwd[pathlen++] = '/';
    /* Append directory name in lowercase for readability. */
    for (int k = 0; k < 8 && e[eidx].name[k] != ' '
                           && pathlen < FAT32_CWD_MAX - 1; k++) {
        char ch = (char)e[eidx].name[k];
        fat32_cwd[pathlen++] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
    }
    fat32_cwd[pathlen] = '\0';

    fat32_cwd_cluster = new_cluster;
    return 0;
}

/* ── fat32_chdir ─────────────────────────────────────────────────── */
int fat32_chdir(const char *name)
{
    if (!g_mounted) return -1;

    /* Save current state for "cd -". */
    str_copy(g_prev_cwd, fat32_cwd, FAT32_CWD_MAX);
    g_prev_cluster = fat32_cwd_cluster;
    g_prev_depth   = g_cwd_depth;
    for (int i = 0; i < CWD_STACK_DEPTH; i++)
        g_prev_stack[i] = g_parent_stack[i];

    /* "cd -" — swap to previous directory. */
    if (name[0] == '-' && (name[1] == '\0' || name[1] == ' ')) {
        static char     tmp_cwd[FAT32_CWD_MAX];
        uint32_t        tmp_cluster;
        static uint32_t tmp_stack[CWD_STACK_DEPTH];
        int             tmp_depth;

        str_copy(tmp_cwd, fat32_cwd, FAT32_CWD_MAX);
        tmp_cluster = fat32_cwd_cluster;
        tmp_depth   = g_cwd_depth;
        for (int i = 0; i < CWD_STACK_DEPTH; i++)
            tmp_stack[i] = g_parent_stack[i];

        str_copy(fat32_cwd, g_prev_cwd, FAT32_CWD_MAX);
        fat32_cwd_cluster = g_prev_cluster;
        g_cwd_depth       = g_prev_depth;
        for (int i = 0; i < CWD_STACK_DEPTH; i++)
            g_parent_stack[i] = g_prev_stack[i];

        str_copy(g_prev_cwd, tmp_cwd, FAT32_CWD_MAX);
        g_prev_cluster = tmp_cluster;
        g_prev_depth   = tmp_depth;
        for (int i = 0; i < CWD_STACK_DEPTH; i++)
            g_prev_stack[i] = tmp_stack[i];

        return 0;
    }

    /* "cd" or "cd /" — go to root. */
    if (name[0] == '\0' || name[0] == ' ' ||
        (name[0] == '/' && (name[1] == '\0' || name[1] == ' '))) {
        fat32_cwd[0] = '/'; fat32_cwd[1] = '\0';
        fat32_cwd_cluster = 0;
        g_cwd_depth = 0;
        return 0;
    }

    /* Absolute path: starts with '/'. */
    const char *p = name;
    if (*p == '/') {
        fat32_cwd[0] = '/'; fat32_cwd[1] = '\0';
        fat32_cwd_cluster = 0;
        g_cwd_depth = 0;
        p++;
    }

    /* Walk each slash-separated component. */
    while (*p) {
        char component[12];
        int  ci = 0;
        while (*p && *p != '/' && ci < 11)
            component[ci++] = *p++;
        component[ci] = '\0';
        if (*p == '/') p++; /* skip slash */
        if (ci == 0)   continue; /* skip empty components (double slash) */

        if (fat32_chdir_one(component) != 0) return -1;
    }
    return 0;
}

/* ── fat32_save_cwd / fat32_restore_cwd ─────────────────────────── */
void fat32_save_cwd(fat32_cwd_state_t *s)
{
    str_copy(s->cwd, fat32_cwd, FAT32_CWD_MAX);
    s->cluster = fat32_cwd_cluster;
    s->depth   = g_cwd_depth;
    for (int i = 0; i < CWD_STACK_DEPTH; i++)
        s->parent_stack[i] = g_parent_stack[i];
}

void fat32_restore_cwd(const fat32_cwd_state_t *s)
{
    str_copy(fat32_cwd, s->cwd, FAT32_CWD_MAX);
    fat32_cwd_cluster = s->cluster;
    g_cwd_depth       = s->depth;
    for (int i = 0; i < CWD_STACK_DEPTH; i++)
        g_parent_stack[i] = s->parent_stack[i];
}

/* ── fat32_rename ────────────────────────────────────────────────── */
int fat32_rename(const char *old_name, const char *old_ext,
                 const char *new_name, const char *new_ext)
{
    if (!g_mounted) return -1;

    uint32_t dir  = cwd_cluster();
    uint32_t elba = 0, eidx = 0;
    if (fat32_find_entry(dir, old_name, old_ext, &elba, &eidx) != 0)
        return -1;

    if (ata_read_sector(elba, g_io_buf) != 0) return -1;
    fat32_dirent_t *e = (fat32_dirent_t *)g_io_buf;

    encode_name83(e[eidx].name, new_name, 8);
    encode_name83(e[eidx].ext,  new_ext,  3);

    return ata_write_sector(elba, g_io_buf);
}
