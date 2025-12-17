/*
 * FatFs - Generic FAT Filesystem Module  R0.15 (Simplified)
 * Real implementation for SD card file operations with Windows compatibility
 *
 * What this file does (big picture):
 * - Mounts a FAT32 volume from an SD card (supports MBR and super-floppy layouts).
 * - Opens/creates files in the root directory (8.3 short names only).
 * - Reads and writes file data using a simple, contiguous data-area model.
 * - Updates directory entries so files are visible/editable on Windows.
 *
 * Fixes called out at the top (why they matter):
 *  1) f_mount: only copies *fs into FatFs[0] AFTER parsing the boot sector
 *     (prevents using an uninitialized/incorrect state globally)
 *  2) f_open: stores dir info (dir_sect/dir_ptr/dir_index) and fp->fs BEFORE any truncate
 *     (ensures truncation updates the correct directory entry)
 *  3) f_write: removes double-adding the partition offset
 *     (prevents writing to the wrong sectors)
 *  4) MBR vs super-floppy detection is robust + extra NOFS diagnostics
 *     (better handling of real-world cards and clearer errors)
 */

#include "ff.h"
#include "diskio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* -----------------------------
 * Minimal global filesystem state
 * ----------------------------- */
static FATFS   FatFs[1];              // single mounted volume (drive 0)
static uint8_t sector_buffer[512];    // shared 512-byte scratch buffer
static bool    fs_ready = false;      // set true after successful mount
static uint32_t partition_start_sector = 0; // LBA of the partition boot sector; 0 for super-floppy

/* -----------------------------------------------------
 * On-disk structs: we only keep the fields we actually use
 * ----------------------------------------------------- */

/* FAT32 BIOS Parameter Block / Boot Sector (subset) */
typedef struct
{
    uint8_t  BS_jmpBoot[3];
    uint8_t  BS_OEMName[8];
    uint16_t BPB_BytsPerSec;   // bytes per sector (must be 512 in this impl)
    uint8_t  BPB_SecPerClus;   // sectors per cluster
    uint16_t BPB_RsvdSecCnt;   // reserved sectors (before FAT)
    uint8_t  BPB_NumFATs;      // typically 2
    uint16_t BPB_RootEntCnt;   // 0 on FAT32 (root is a cluster chain)
    uint16_t BPB_TotSec16;
    uint8_t  BPB_Media;
    uint16_t BPB_FATSz16;
    uint16_t BPB_SecPerTrk;
    uint16_t BPB_NumHeads;
    uint32_t BPB_HiddSec;      // hidden sectors (LBA of partition)
    uint32_t BPB_TotSec32;
    uint32_t BPB_FATSz32;      // sectors per FAT (FAT32)
    uint16_t BPB_ExtFlags;
    uint16_t BPB_FSVer;
    uint32_t BPB_RootClus;     // first cluster of root dir (usually 2)
    uint16_t BPB_FSInfo;
    uint16_t BPB_BkBootSec;
    uint8_t  BPB_Reserved[12];
    uint8_t  BS_DrvNum;
    uint8_t  BS_Reserved1;
    uint8_t  BS_BootSig;
    uint32_t BS_VolID;
    uint8_t  BS_VolLab[11];
    uint8_t  BS_FilSysType[8]; // "FAT32   "
} __attribute__((packed)) BOOT_SECTOR;

/* FAT directory entry for 8.3 short names (LFN not supported here) */
typedef struct
{
    uint8_t  Name[11];     // 8 + 3, space-padded, upper case
    uint8_t  Attr;
    uint8_t  NTRes;
    uint8_t  CrtTimeTenth;
    uint16_t CrtTime;
    uint16_t CrtDate;
    uint16_t LstAccDate;
    uint16_t FstClusHI;
    uint16_t WrtTime;
    uint16_t WrtDate;
    uint16_t FstClusLO;
    uint32_t FileSize;
} __attribute__((packed)) DIR_ENTRY;

/* -----------------------------
 * Tiny memory helpers (no libc deps)
 * ----------------------------- */
static void mem_set(uint8_t *d, uint8_t c, int n) { for (int i = 0; i < n; i++) d[i] = c; }
static void mem_cpy(uint8_t *d, const uint8_t *s, int n) { for (int i = 0; i < n; i++) d[i] = s[i]; }
static int  mem_cmp(const uint8_t *a, const uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

/* Convert "NAME.EXT" to FAT 8.3 "NAME    EXT" (upper case, space-padded) */
static void name_to_fat(const char *name, uint8_t *fat)
{
    mem_set(fat, ' ', 11);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        fat[j++] = (name[i] >= 'a' && name[i] <= 'z') ? (name[i] - 'a' + 'A') : name[i];
        i++;
    }
    if (name[i] == '.') { i++; j = 8; }
    while (name[i] && j < 11) {
        fat[j++] = (name[i] >= 'a' && name[i] <= 'z') ? (name[i] - 'a' + 'A') : name[i];
        i++;
    }
}

/* ===================== MOUNT =====================
 * - Reads sector 0 and decides: MBR or super-floppy
 * - Finds and validates the boot sector
 * - Fills the FATFS structure with essential layout info
 * - Copies the local 'fs' into global FatFs[0] only after success
 * ================================================= */
FRESULT f_mount(FATFS *fs, const char *path, uint8_t opt)
{
    (void)path; (void)opt;
    printf("Mounting FAT32 filesystem with Windows compatibility...\n");
    if (fs_ready) {
        printf("Filesystem already mounted\n");
        return FR_OK;
    }

    partition_start_sector = 0; // reset each attempt

    // Read LBA 0 (could be MBR or boot sector)
    if (disk_read(0, sector_buffer, 0, 1) != RES_OK) {
        printf("Failed to read sector 0\n");
        return FR_DISK_ERR;
    }

    // Check 0x55AA signature
    uint16_t sig0 = *(uint16_t *)(sector_buffer + 510);
    printf("Boot signature: 0x%04X\n", sig0);
    if (sig0 != 0xAA55) {
        printf("NOFS: bad boot signature (0x%04X)\n", sig0);
        return FR_NO_FILESYSTEM;
    }

    BOOT_SECTOR *bs = (BOOT_SECTOR *)sector_buffer;

    // Heuristic: if partition type in first MBR entry != 0, assume MBR
    bool looks_mbr = (sector_buffer[446 + 4] != 0x00);

    if (looks_mbr) {
        printf("Detected MBR - looking for FAT partition...\n");

        // First partition entry @ 446
        uint8_t *p = sector_buffer + 446;
        uint8_t  ptype  = p[4];
        uint32_t pstart = *(uint32_t *)(p + 8);

        printf("Partition 1: Status=0x%02X, Type=0x%02X, StartSector=%lu\n", p[0], ptype, pstart);

        // Accept common FAT partition types
        if (ptype == 0x0B || ptype == 0x0C || ptype == 0x06) {
            partition_start_sector = pstart;
            printf("Using partition start at sector %lu\n", partition_start_sector);

            // Read partition boot sector
            if (disk_read(0, sector_buffer, partition_start_sector, 1) != RES_OK) {
                printf("Failed to read partition boot sector\n");
                return FR_DISK_ERR;
            }
            bs = (BOOT_SECTOR *)sector_buffer;

            // Validate boot sig at partition boot sector
            uint16_t psig = *(uint16_t *)(sector_buffer + 510);
            printf("Partition boot signature: 0x%04X\n", psig);
            if (psig != 0xAA55) {
                printf("NOFS: bad partition boot signature (0x%04X)\n", psig);
                return FR_NO_FILESYSTEM;
            }
        } else {
            printf("NOFS: unsupported partition type 0x%02X\n", ptype);
            return FR_NO_FILESYSTEM;
        }
    } else {
        // No MBR → assume super-floppy (boot sector at LBA 0)
        printf("No MBR detected (super-floppy); using sector 0 as boot sector\n");
    }

    // Basic boot-sector validation
    printf("OEM Name: '%.8s'\n", bs->BS_OEMName);
    printf("File System Type: '%.8s'\n", bs->BS_FilSysType);
    printf("Raw sector size: %u\n", bs->BPB_BytsPerSec);

    if (bs->BPB_BytsPerSec == 0) {
        printf("NOFS: sector size is 0\n");
        return FR_NO_FILESYSTEM;
    }
    if (bs->BPB_BytsPerSec != 512) {
        printf("NOFS: unsupported sector size %u (expected 512)\n", bs->BPB_BytsPerSec);
        return FR_NO_FILESYSTEM;
    }

    // Fill provided *fs with computed layout; do not touch global yet
    if (fs) {
        if (bs->BPB_SecPerClus == 0 || bs->BPB_NumFATs == 0) {
            printf("NOFS: invalid params SecPerClus=%u NumFATs=%u\n",
                   bs->BPB_SecPerClus, bs->BPB_NumFATs);
            return FR_NO_FILESYSTEM;
        }

        fs->fs_type = 1; // "FAT32" marker in this tiny impl
        fs->pdrv    = 0;
        fs->csize   = bs->BPB_SecPerClus;
        fs->n_fats  = bs->BPB_NumFATs;
        fs->fsize   = bs->BPB_FATSz32 ? bs->BPB_FATSz32 : 16; // fallback if zero
        fs->volbase = partition_start_sector;
        fs->fatbase = partition_start_sector + bs->BPB_RsvdSecCnt;

        if (bs->BPB_RootEntCnt == 0) {
            // FAT32: root directory is in a cluster chain starting at RootClus
            uint32_t root_clus = bs->BPB_RootClus ? bs->BPB_RootClus : 2;
            fs->dirbase = partition_start_sector
                        + bs->BPB_RsvdSecCnt
                        + (bs->BPB_NumFATs * fs->fsize)
                        + ((root_clus - 2) * bs->BPB_SecPerClus);
            printf("FAT32 root directory at sector %lu (cluster %lu)\n", fs->dirbase, root_clus);
        } else {
            // FAT16 fallback (not used here, but kept for clarity)
            fs->dirbase = partition_start_sector
                        + bs->BPB_RsvdSecCnt
                        + (bs->BPB_NumFATs * fs->fsize);
            printf("FAT16 root directory at sector %lu\n", fs->dirbase);
        }

        printf("Filesystem info: csize=%d, n_fats=%d, fsize=%lu\n",
               fs->csize, fs->n_fats, fs->fsize);
        printf("Base sectors: vol=%lu, fat=%lu, dir=%lu\n",
               fs->volbase, fs->fatbase, fs->dirbase);
    }

    // Copy to global AFTER success -> avoids partially-filled globals
    if (fs) {
        FatFs[0] = *fs;
    }

    fs_ready = true;
    printf("Filesystem mounted successfully with Windows compatibility\n");
    return FR_OK;
}

/* ===================== OPEN =====================
 * - Converts "path" to FAT 8.3 short name
 * - Scans a single root directory sector (toy impl: 16 entries)
 * - Opens existing file or creates one if allowed by mode flags
 * - Records directory entry location for later f_sync updates
 * - Applies CREATE_ALWAYS truncation after dir info is saved
 * =============================================== */
FRESULT f_open(FIL *fp, const char *path, uint8_t mode)
{
    if (!fs_ready || !fp) return FR_NOT_READY;
    printf("Opening file: %s (mode: 0x%02X)\n", path, mode);

    // 8.3 name conversion
    uint8_t fat_name[11];
    name_to_fat(path, fat_name);
    printf("FAT name: '%.11s'\n", fat_name);

    // Read a single root directory sector (simplified)
    uint32_t root_sector = FatFs[0].dirbase;
    if (disk_read(0, sector_buffer, root_sector, 1) != RES_OK)
        return FR_DISK_ERR;

    DIR_ENTRY *entries = (DIR_ENTRY *)sector_buffer;
    bool  found = false;
    int   entry_idx = -1;

    // Look for existing entry
    for (int i = 0; i < 16; i++) {
        if (entries[i].Name[0] == 0) break;          // end of directory
        if (entries[i].Name[0] == 0xE5) continue;    // deleted
        if (mem_cmp(entries[i].Name, fat_name, 11) == 0) {
            printf("File found at index %d\n", i);
            fp->fsize  = entries[i].FileSize;
            fp->fptr   = 0;
            fp->sclust = entries[i].FstClusLO | ((uint32_t)entries[i].FstClusHI << 16);
            if (fp->sclust == 0) fp->sclust = 3;     // fixed start cluster in this toy impl
            found = true;
            entry_idx = i;
            break;
        }
    }

    // Create if allowed and not found
    if (!found && (mode & (FA_CREATE_NEW | FA_CREATE_ALWAYS | FA_OPEN_ALWAYS))) {
        for (int i = 0; i < 16; i++) {
            if (entries[i].Name[0] == 0 || entries[i].Name[0] == 0xE5) {
                // Initialize new 8.3 entry
                mem_set((uint8_t *)&entries[i], 0, sizeof(DIR_ENTRY));
                mem_cpy(entries[i].Name, fat_name, 11);
                entries[i].Attr = 0x20;    // archive
                entries[i].FileSize = 0;
                entries[i].FstClusHI = 0;
                entries[i].FstClusLO = 3;  // toy: fixed cluster

                // Minimal timestamps (arbitrary constants)
                uint16_t t = 0x0000; // 00:00
                uint16_t d = 0x52C8; // 2025-01-08
                entries[i].CrtTimeTenth = 0;
                entries[i].CrtTime  = t;
                entries[i].CrtDate  = d;
                entries[i].LstAccDate = d;
                entries[i].WrtTime  = t;
                entries[i].WrtDate  = d;

                printf("Creating Windows-compatible file entry for: %.11s\n", fat_name);
                if (disk_write(0, sector_buffer, root_sector, 1) != RES_OK)
                    return FR_DISK_ERR;
                disk_ioctl(0, CTRL_SYNC, NULL);
                sleep_ms(5);

                fp->fsize = 0;
                fp->sclust = 3;
                entry_idx = i;
                found = true;
                printf("Windows-compatible file entry created\n");
                break;
            }
        }
    }

    if (!found) return FR_NO_FILE;

    // Save dir-entry location BEFORE any truncate; associate file with fs
    fp->dir_sect  = root_sector;
    fp->dir_ptr   = (uint8_t *)&entries[entry_idx];
    fp->dir_index = entry_idx;
    fp->stat      = mode;
    fp->fs        = &FatFs[0];

    // CREATE_ALWAYS → truncate to zero (after dir info is valid)
    if (found && (mode & FA_CREATE_ALWAYS)) {
        printf("Truncating existing file (CREATE_ALWAYS)\n");
        fp->fsize = 0;
        fp->fptr  = 0;
        if (disk_read(0, sector_buffer, fp->dir_sect, 1) == RES_OK) {
            DIR_ENTRY *d = (DIR_ENTRY *)sector_buffer;
            d[entry_idx].FileSize = 0;
            if (disk_write(0, sector_buffer, fp->dir_sect, 1) != RES_OK)
                return FR_DISK_ERR;
        }
    }

    printf("File opened: size=%lu, cluster=%lu\n", fp->fsize, fp->sclust);
    return FR_OK;
}

/* ===================== WRITE =====================
 * - Writes 'btw' bytes from 'buff' to the file starting at fptr
 * - Works across sector boundaries (multi-sector)
 * - Updates in-memory fptr/fsize as we go
 * - Updates directory entry (size/cluster) once at the end
 * NOTE: This toy model treats data area as a single contiguous
 *       region after the root directory (no FAT chaining).
 * ================================================= */
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    if (!fp || !buff || !bw) return FR_INVALID_PARAMETER;
    *bw = 0;
    if (btw == 0) return FR_OK;

    const uint8_t *p = (const uint8_t *)buff;
    uint32_t remaining = btw;

    // Toy data-area base: one cluster after root dir sector
    const uint32_t data_sector_base = FatFs[0].dirbase + FatFs[0].csize;

    while (remaining > 0) {
        uint32_t target_sector = data_sector_base + (fp->fptr / 512);
        uint32_t byte_off      = fp->fptr % 512;

        // Read-modify-write sector to preserve untouched bytes
        if (disk_read(0, sector_buffer, target_sector, 1) != RES_OK)
            return FR_DISK_ERR;

        uint32_t space    = 512 - byte_off;
        uint32_t to_write = (remaining < space) ? remaining : space;

        mem_cpy(sector_buffer + byte_off, p, (int)to_write);

        if (disk_write(0, sector_buffer, target_sector, 1) != RES_OK)
            return FR_DISK_ERR;

        p           += to_write;
        remaining   -= to_write;
        fp->fptr    += to_write;
        if (fp->fptr > fp->fsize) fp->fsize = fp->fptr;
    }

    // Update directory entry (size/first cluster) once
    if (fp->dir_sect) {
        if (disk_read(0, sector_buffer, fp->dir_sect, 1) == RES_OK) {
            DIR_ENTRY *d = (DIR_ENTRY *)sector_buffer;
            if (fp->dir_index < 16) {
                if (d[fp->dir_index].FstClusLO == 0)
                    d[fp->dir_index].FstClusLO = (uint16_t)(fp->sclust ? fp->sclust : 3);
                d[fp->dir_index].FileSize = fp->fsize;
                disk_write(0, sector_buffer, fp->dir_sect, 1);
            }
        }
    }

    *bw = btw;
    printf("%u bytes written successfully (multi-sector)\n", btw);
    return FR_OK;
}

/* ===================== READ ======================
 * - Reads up to 'btr' bytes into 'buff' starting at fptr
 * - Stops at EOF; updates fptr and *br
 * - Handles sector boundaries (multi-sector)
 * ================================================= */
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    if (!fp || !buff || !br) return FR_INVALID_PARAMETER;
    *br = 0;
    if (btr == 0) return FR_OK;

    // Do not read past end of file
    uint32_t remain = (fp->fptr + btr > fp->fsize) ? (fp->fsize - fp->fptr) : btr;
    uint8_t *dst = (uint8_t *)buff;

    const uint32_t data_sector_base = FatFs[0].dirbase + FatFs[0].csize;

    while (remain > 0) {
        uint32_t target_sector = data_sector_base + (fp->fptr / 512);
        uint32_t byte_off      = fp->fptr % 512;

        if (disk_read(0, sector_buffer, target_sector, 1) != RES_OK)
            return FR_DISK_ERR;

        uint32_t space   = 512 - byte_off;
        uint32_t to_copy = (remain < space) ? remain : space;

        mem_cpy(dst, sector_buffer + byte_off, (int)to_copy);

        dst     += to_copy;
        remain  -= to_copy;
        fp->fptr += to_copy;
        *br     += to_copy;
    }
    return FR_OK;
}

/* ===================== SYNC ======================
 * - Flushes/updates directory entry so PC (Windows) sees correct size
 * - Also writes minimal timestamps and marks as 'archive'
 * ================================================= */
FRESULT f_sync(FIL *fp)
{
    if (!fp) return FR_INVALID_OBJECT;
    printf("Syncing file for Windows compatibility...\n");

    if (fp->dir_sect) {
        if (disk_read(0, sector_buffer, fp->dir_sect, 1) == RES_OK) {
            DIR_ENTRY *e = (DIR_ENTRY *)sector_buffer;
            if (fp->dir_index < 16) {
                e[fp->dir_index].FileSize = fp->fsize;
                if (e[fp->dir_index].FstClusLO == 0 && fp->fsize > 0) {
                    e[fp->dir_index].FstClusLO = (uint16_t)(fp->sclust ? fp->sclust : 3);
                }
                e[fp->dir_index].Attr = 0x20; // archive

                // Minimal timestamp update (fixed constants)
                uint16_t t = 0x0000, d = 0x52C8;
                e[fp->dir_index].WrtTime = t;
                e[fp->dir_index].WrtDate = d;
                e[fp->dir_index].LstAccDate = d;
                e[fp->dir_index].CrtTime = t;
                e[fp->dir_index].CrtDate = d;

                if (disk_write(0, sector_buffer, fp->dir_sect, 1) != RES_OK) {
                    printf("Failed to write directory sector\n");
                    return FR_DISK_ERR;
                }
                // Extra syncs + small delay to be nice to some cards
                disk_ioctl(0, CTRL_SYNC, NULL);
                sleep_ms(10);
                disk_ioctl(0, CTRL_SYNC, NULL);
            }
        } else {
            printf("Failed to read directory sector for sync\n");
        }
    }

    printf("File synced with Windows compatibility\n");
    return FR_OK;
}

/* ===================== STAT ======================
 * - Looks up a file by short 8.3 name in the root directory
 * - Fills FILINFO with size, attributes, and short name
 * ================================================= */
FRESULT f_stat(const char *path, FILINFO *fno)
{
    if (!fs_ready || !fno) return FR_NOT_READY;

    uint8_t fat_name[11];
    name_to_fat(path, fat_name);

    uint32_t root_sector = FatFs[0].dirbase;
    if (disk_read(0, sector_buffer, root_sector, 1) != RES_OK)
        return FR_DISK_ERR;

    DIR_ENTRY *entries = (DIR_ENTRY *)sector_buffer;
    for (int i = 0; i < 16; i++) {
        if (entries[i].Name[0] == 0) break;   // end of directory
        if (entries[i].Name[0] == 0xE5) continue; // deleted
        if (mem_cmp(entries[i].Name, fat_name, 11) == 0) {
            // Fill FILINFO
            fno->fsize  = entries[i].FileSize;
            fno->fattrib = entries[i].Attr;

            // Reconstruct short name "NAME.EXT"
            int j = 0;
            for (int k = 0; k < 8 && entries[i].Name[k] != ' '; k++)
                fno->fname[j++] = entries[i].Name[k];
            if (entries[i].Name[8] != ' ') {
                fno->fname[j++] = '.';
                for (int k = 8; k < 11 && entries[i].Name[k] != ' '; k++)
                    fno->fname[j++] = entries[i].Name[k];
            }
            fno->fname[j] = 0;
            return FR_OK;
        }
    }
    return FR_NO_FILE;
}

/* = LSEEK / SIZE / CLOSE (basics) =
 * f_lseek : move file pointer (clamped to file size)
 * f_size  : return current logical file size
 * f_close : call f_sync then clear FIL fields
 * ================================= */
FRESULT f_lseek(FIL *fp, uint32_t ofs)
{
    if (!fp) return FR_INVALID_OBJECT;
    if (ofs > fp->fsize) ofs = fp->fsize; // clamp
    fp->fptr = ofs;
    printf("File position set to %lu\n", ofs);
    return FR_OK;
}

uint32_t f_size(FIL *fp)
{
    if (!fp) return 0;
    return fp->fsize;
}

FRESULT f_close(FIL *fp)
{
    if (!fp) return FR_INVALID_OBJECT;
    FRESULT r = f_sync(fp);        // ensure directory entry is updated
    if (r != FR_OK) return r;

    // Clear fields (not strictly necessary, but tidy)
    fp->fsize = 0;
    fp->fptr = 0;
    fp->sclust = 0;
    fp->stat = 0;
    fp->dir_sect = 0;
    fp->dir_index = 0;

    printf("File closed successfully\n");
    return FR_OK;
}
