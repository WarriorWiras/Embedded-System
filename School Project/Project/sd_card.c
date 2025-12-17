/*
 * SD Card Implementation using FatFs
 * Simple and reliable SD card file operations
 */

#include "sd_card.h"
#include "fatfs/ff.h"
#include "pico/stdlib.h"
#include "fatfs/diskio.h"
#include "flash_benchmark.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// SD on Maker Pi Pico W (SPI1)
#define SD_SPI_INST spi1
#define SD_CS_PIN 15
#define SD_SCK_PIN 10
#define SD_MOSI_PIN 11
#define SD_MISO_PIN 12


// ======= SAFE MODE BACKUP: tiny 512B writes with gentle pacing =======
bool sd_backup_flash_safe(const char *dir, const char *filename)
{
    if (!sd_is_mounted()) {
        printf("❌ SD not mounted\n");
        return false;
    }

    if (!dir || !*dir) dir = "SPI_Backup";
    if (!filename || !*filename) filename = "microchip_backup_safe.bin";

    // Make sure folder exists (recover once on FR_DISK_ERR)
    FRESULT fr = f_mkdir(dir);
    if (!(fr == FR_OK || fr == FR_EXIST)) {
        if (fr == FR_DISK_ERR) {
            sd_unmount(); sleep_ms(50);
            if (!sd_mount()) { printf("❌ Remount failed (mkdir)\n"); return false; }
            fr = f_mkdir(dir);
            if (!(fr == FR_OK || fr == FR_EXIST)) {
                printf("❌ f_mkdir(%s) failed (%d)\n", dir, fr);
                return false;
            }
        } else {
            printf("❌ f_mkdir(%s) failed (%d)\n", dir, fr);
            return false;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    printf("💾 SAFE backup to: %s\n", path);

    FIL f;
    fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR) {
            sd_unmount(); sleep_ms(50);
            if (!sd_mount()) { printf("❌ Remount failed (open)\n"); return false; }
            fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
        }
        if (fr != FR_OK) {
            printf("❌ f_open failed (%d)\n", fr);
            return false;
        }
    }

    const size_t total = flash_capacity_bytes();
    const UINT   CHUNK = 512;              // 1 sector
    const int    FLUSH_BLOCKS = 128;       // sync every 64 KB
    const int    REOPEN_EVERY = 2048;      // reopen file every 1 MB
    uint8_t      buf[512];

    size_t done = 0;
    int blocks_since_sync = 0;
    int blocks_since_reopen = 0;

    while (done < total) {
        UINT n = (total - done) >= CHUNK ? CHUNK : (UINT)(total - done);

        // Read from SPI flash
        if (!flash_read_data((uint32_t)done, buf, n)) {
            printf("❌ flash_read_data failed at 0x%06lX\n", (unsigned long)done);
            f_close(&f);
            return false;
        }

        // Write this 512B to SD
        UINT bw = 0;
        fr = f_write(&f, buf, n, &bw);
        if (!(fr == FR_OK && bw == n)) {
            printf("⚠️  f_write err fr=%d bw=%u at 0x%06lX — recover\n",
                   fr, bw, (unsigned long)done);

            // close, remount, reopen, seek to position and retry once
            f_close(&f);
            sd_unmount(); sleep_ms(50);
            if (!sd_mount()) { printf("❌ Remount failed\n"); return false; }

            fr = f_open(&f, path, FA_WRITE);
            if (fr != FR_OK) { printf("❌ reopen failed (%d)\n", fr); return false; }

            if (f_lseek(&f, (FSIZE_t)done) != FR_OK) {
                printf("❌ seek failed after reopen\n");
                f_close(&f);
                return false;
            }

            // retry this same 512B
            bw = 0;
            fr = f_write(&f, buf, n, &bw);
            if (!(fr == FR_OK && bw == n)) {
                printf("❌ write still failing fr=%d bw=%u at 0x%06lX\n", fr, bw, (unsigned long)done);
                f_close(&f);
                return false;
            }
        }

        done += n;
        blocks_since_sync++;
        blocks_since_reopen++;

        // gentle pause so the card can finish internal program/erase
        sleep_ms(3);

        // periodic sync (every 64KB)
        if (blocks_since_sync >= FLUSH_BLOCKS) {
            FRESULT fs = f_sync(&f);
            if (fs != FR_OK) {
                printf("⚠️  f_sync err (%d) at 0x%06lX — recovering\n", fs, (unsigned long)done);
                f_close(&f);
                sd_unmount(); sleep_ms(50);
                if (!sd_mount()) return false;
                fr = f_open(&f, path, FA_WRITE);
                if (fr != FR_OK) return false;
                if (f_lseek(&f, (FSIZE_t)done) != FR_OK) { f_close(&f); return false; }
                fs = f_sync(&f);
                if (fs != FR_OK) { printf("❌ f_sync still failing (%d)\n", fs); f_close(&f); return false; }
            }
            blocks_since_sync = 0;
        }

        // periodic reopen (every 1 MB) to avoid long continuous handle
        if (blocks_since_reopen >= REOPEN_EVERY) {
            f_close(&f);
            fr = f_open(&f, path, FA_WRITE);
            if (fr != FR_OK) { printf("❌ reopen failed (%d)\n", fr); return false; }
            if (f_lseek(&f, (FSIZE_t)done) != FR_OK) { f_close(&f); return false; }
            blocks_since_reopen = 0;
        }

        if ((done % (256 * 1024)) == 0 || done == total) {
            printf("   … %lu / %lu bytes\n", (unsigned long)done, (unsigned long)total);
        }
    }

    f_sync(&f);
    f_close(&f);
    printf("✅ SAFE backup complete: %s (%lu bytes)\n", path, (unsigned long)total);
    return true;
}

// Return a list of files in the root directory (fills up to max_files entries)
int sd_get_file_list(sd_file_info_t *files, int max_files)
{
    if (!sd_is_mounted() || !files || max_files <= 0) {
        return 0;
    }

    DIR dir;
    FILINFO fno;
    FRESULT fr;
    int file_count = 0;

    // Always open directory fresh (no caching)
    fr = f_opendir(&dir, "/");
    if (fr != FR_OK) {
        printf("[!] Failed to open root directory (error: %d)\n", fr);
        return 0;
    }

    // Read all directory entries fresh from disk
    printf("[*] Reading directory entries...\n");
    int total_entries_checked = 0;
    while (file_count < max_files) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK) {
            printf("[!] f_readdir error: %d\n", fr);
            break;
        }

        total_entries_checked++;

        // End of directory
        if (fno.fname[0] == 0) {
            printf("[*] End of directory reached after %d entries\n", total_entries_checked);
            break;
        }

        // Skip directories and hidden files
        if (fno.fattrib & AM_DIR) {
            printf("[*] Skipping directory: %s\n", fno.fname);
            continue;
        }
        if (fno.fname[0] == '.') {
            printf("[*] Skipping hidden file: %s\n", fno.fname);
            continue;
        }

        // Store file info
        printf("[+] Found file: %s (%lu bytes)\n", fno.fname, (unsigned long)fno.fsize);
        strncpy(files[file_count].filename, fno.fname, sizeof(files[file_count].filename) - 1);
        files[file_count].filename[sizeof(files[file_count].filename) - 1] = '\0';
        files[file_count].size = fno.fsize;
        file_count++;
    }
    
    printf("[*] Total files found: %d (checked %d directory entries)\n", file_count, total_entries_checked);

    f_closedir(&dir);
    return file_count;
}

bool sd_restore_flash_safe(const char *dir, const char *filename)
{
    if (!sd_is_mounted()) {
        printf("❌ SD not mounted\n");
        return false;
    }

    if (!dir || !*dir) dir = "SPI_Backup";
    if (!filename || !*filename) filename = "microchip_backup_safe.bin";

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    printf("💾 SAFE restore from: %s\n", path);

    FIL f;
    FRESULT fr = f_open(&f, path, FA_READ);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR) {
            sd_unmount(); sleep_ms(50);
            if (!sd_mount()) {
                printf("❌ Remount failed (open for restore)\n");
                return false;
            }
            fr = f_open(&f, path, FA_READ);
        }
        if (fr != FR_OK) {
            printf("❌ f_open (restore) failed (%d)\n", fr);
            return false;
        }
    }

    // File size vs flash capacity
    FSIZE_t backup_size = f_size(&f);
    size_t flash_total = flash_capacity_bytes();

    if ((size_t)backup_size > flash_total) {
        printf("❌ Backup file (%lu bytes) larger than flash (%lu bytes)\n",
               (unsigned long)backup_size, (unsigned long)flash_total);
        f_close(&f);
        return false;
    }

    printf("📦 Backup size: %lu bytes, flash size: %lu bytes\n",
           (unsigned long)backup_size, (unsigned long)flash_total);

    // ---- ERASE USED REGION SECTOR-BY-SECTOR ----
    printf("🧨 Erasing used flash region sector-by-sector…\n");
    for (uint32_t a = 0; a < (uint32_t)backup_size; a += FLASH_SECTOR_SIZE) {
        if (!flash_sector_erase(a)) {   // int-returning, treat 0 as failure just like flash_read_data()
            printf("❌ flash_sector_erase(0x%06lX) failed\n", (unsigned long)a);
            f_close(&f);
            return false;
        }
    }
    printf("🧨 Erase complete, restoring contents…\n");

    const UINT   CHUNK = 512;   // 1 sector on SD
    uint8_t      buf[512];
    FSIZE_t      done = 0;

    while (done < backup_size) {
        UINT n = (backup_size - done) >= CHUNK ? CHUNK : (UINT)(backup_size - done);

        // Read from SD
        UINT br = 0;
        fr = f_read(&f, buf, n, &br);
        if (!(fr == FR_OK && br == n)) {
            printf("❌ f_read (restore) failed fr=%d br=%u at 0x%06lX\n",
                   fr, br, (unsigned long)done);
            f_close(&f);
            return false;
        }

        // -------- Program to SPI flash in PAGE-sized chunks, respecting page boundaries --------
        size_t offset = 0;
        while (offset < n) {
            uint32_t addr = (uint32_t)done + (uint32_t)offset;

            // How much space left in this flash page?
            uint32_t page_offset   = addr % FLASH_PAGE_SIZE;
            uint32_t space_in_page = FLASH_PAGE_SIZE - page_offset;

            uint32_t bytes_left_in_buf = (uint32_t)(n - offset);
            uint32_t bytes_this = bytes_left_in_buf;
            if (bytes_this > space_in_page) {
                bytes_this = space_in_page;
            }

            if (!flash_page_program(addr, buf + offset, bytes_this)) {
                printf("❌ flash_page_program failed at 0x%06lX (len=%lu)\n",
                       (unsigned long)addr, (unsigned long)bytes_this);
                f_close(&f);
                return false;
            }

            offset += bytes_this;
        }

        done += n;

        // gentle pause
        sleep_ms(3);

        if ((done % (256 * 1024)) == 0 || done == backup_size) {
            printf("   … %lu / %lu bytes restored\n",
                   (unsigned long)done, (unsigned long)backup_size);
        }
    }

    f_close(&f);
    printf("✅ SAFE restore complete: %s (%lu bytes written back to flash)\n",
           path, (unsigned long)backup_size);
    return true;
}




/* Put the SD bus in a safe idle: CS=HIGH, SCK=LOW, MOSI=HIGH, MISO=input */
static void sd_bus_idle(void)
{
    // Drive as GPIO to force levels
    gpio_set_function(SD_CS_PIN, GPIO_FUNC_SIO);
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SIO);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SIO);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SIO);

    gpio_put(SD_CS_PIN, 1);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    gpio_put(SD_SCK_PIN, 0);
    gpio_set_dir(SD_SCK_PIN, GPIO_OUT); // CPOL=0 idle
    gpio_put(SD_MOSI_PIN, 1);
    gpio_set_dir(SD_MOSI_PIN, GPIO_OUT); // send 1's when idle
    gpio_set_dir(SD_MISO_PIN, GPIO_IN);

    sleep_ms(1);
}

/* Prepare pins for SPI and keep CS HIGH (deselected) */
static void sd_bus_to_spi(void)
{
    gpio_set_function(SD_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO_PIN, GPIO_FUNC_SPI);

    // keep CS as GPIO so we can guarantee HIGH
    gpio_set_function(SD_CS_PIN, GPIO_FUNC_SIO);
    gpio_put(SD_CS_PIN, 1);
    gpio_set_dir(SD_CS_PIN, GPIO_OUT);
    sleep_ms(1);
}

#undef BACKUP_CHUNK
#define BACKUP_CHUNK 512 // 4 KB is safer on many cards

static FRESULT write_with_retries(FIL *fp, const void *buf, UINT len)
{
    const uint8_t *p = (const uint8_t *)buf;
    UINT left = len;

    while (left)
    {
        UINT chunk = (left > BACKUP_CHUNK) ? BACKUP_CHUNK : left;

        for (int attempt = 1; attempt <= 5; ++attempt)
        {
            UINT bw = 0;
            FRESULT fr = f_write(fp, p, chunk, &bw);
            if (fr == FR_OK && bw == chunk)
            {
                break; // success
            }
            printf("⚠️  f_write retry %d (fr=%d, bw=%u/%u)\n", attempt, fr, bw, chunk);
            sleep_ms(5);
            if (attempt == 3)
                return fr ? fr : FR_DISK_ERR;
        }

        p += chunk;
        left -= chunk;
    }

    return FR_OK;
}

static bool csv_has_header(const char *filename)
{
    FIL f;
    // Open existing file read-only; if it doesn't exist, obviously no header
    if (f_open(&f, filename, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;

    UINT br = 0;
    char first[16] = {0};

    // Read first few bytes
    f_lseek(&f, 0);
    f_read(&f, first, sizeof(first) - 1, &br);
    f_close(&f);

    // True if it literally begins with "jedec_id,"
    return (br >= 8 && memcmp(first, "jedec_id,", 8) == 0);
}

// Global FatFs objects
static FATFS fatfs;
static bool sd_mounted = false;

bool sd_card_init(void)
{
    printf("🔧 Initializing 32GB FAT32 SD Card System...\n");
    printf("============================================\n");

    // Enhanced initialization with comprehensive diagnostics
    printf("📋 SD Card Requirements Check:\n");
    printf("   - Capacity: 32GB (recommended)\n");
    printf("   - Format: FAT32 (required)\n");
    printf("   - Connection: Maker Pi Pico W SD slot (GP10-GP15)\n");
    printf("   - Status: Checking...\n\n");

    printf("🔌 Hardware Configuration:\n");
    printf("   - SPI Port: spi1 (hardware SPI)\n");
    printf("   - CS Pin: GP15 (Chip Select)\n");
    printf("   - SCK Pin: GP10 (Serial Clock)\n");
    printf("   - MOSI Pin: GP11 (Master Out Slave In)\n");
    printf("   - MISO Pin: GP12 (Master In Slave Out)\n\n");

    // The actual SD card hardware initialization is handled in diskio.c
    printf("⚡ Starting low-level SD card initialization...\n");
    printf("   (Detailed SPI communication logs will follow)\n\n");

    // Success indicator - actual initialization happens in diskio.c
    printf("✅ SD Card hardware interface ready\n");
    printf("   Next step: Filesystem mounting (f_mount)\n");
    printf("============================================\n");

    return true;
}

bool sd_backup_flash_full(const char *dir, const char *filename)
{
    // ✅ CHANGE: actively (re)validate the mount instead of trusting sd_mounted
    if (!sd_mount()) {
        printf("❌ SD not present or mount failed\n");
        return false;
    }

    if (!dir || !*dir) dir = "SPI_Backup";
    if (!filename || !*filename) filename = "Flash_Backup.bin";

    // ✅ CHANGE: mkdir with one-shot recovery on FR_DISK_ERR
    FRESULT fr = f_mkdir(dir);
    if (fr == FR_OK) {
        printf("📁 Created folder: %s\n", dir);
    } else if (fr == FR_EXIST) {
        // already exists
    } else {
        if (fr == FR_DISK_ERR) {
            printf("⚠️  f_mkdir(%s) FR_DISK_ERR — attempting SD recover\n", dir);
            sd_unmount();
            sleep_ms(50);
            if (!sd_mount()) {
                printf("❌ Remount failed during mkdir recovery\n");
                return false;
            }
            fr = f_mkdir(dir);
            if (!(fr == FR_OK || fr == FR_EXIST)) {
                printf("❌ f_mkdir(%s) failed after recovery (%d)\n", dir, fr);
                return false;
            }
        } else {
            printf("❌ f_mkdir(%s) failed (%d)\n", dir, fr);
            return false;
        }
    }

    // Build full path "SPI_Backup/Flash_Backup.bin"
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    printf("💾 Backing up SPI flash to: %s\n", path);

    FIL f;
    // ✅ CHANGE: f_open with one-shot recovery on FR_DISK_ERR
    fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK) {
        if (fr == FR_DISK_ERR) {
            printf("⚠️  f_open FR_DISK_ERR — attempting SD recover\n");
            sd_unmount();
            sleep_ms(50);
            if (!sd_mount()) {
                printf("❌ Remount failed during open recovery\n");
                return false;
            }
            fr = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
        }
        if (fr != FR_OK) {
            printf("❌ f_open failed (%d)\n", fr);
            return false;
        }
    }

    // Determine full flash size once
    const size_t total = flash_capacity_bytes();

#if FF_USE_EXPAND
    {
        FRESULT fre = f_expand(&f, (FSIZE_t)total, 1 /* contiguous if possible */);
        if (fre != FR_OK) {
            printf("⚠️  f_expand failed (%d) — continuing without preallocation\n", fre);
        }
    }
#else
    {
        FRESULT frs = f_lseek(&f, (FSIZE_t)total - 1);
        if (frs == FR_OK) {
            UINT bw = 0; BYTE z = 0;
            frs = f_write(&f, &z, 1, &bw);
            if (frs == FR_OK && bw == 1) {
                f_sync(&f);
            } else {
                printf("⚠️  Preallocate write failed (%d)\n", frs);
            }
            f_lseek(&f, 0);
        } else {
            printf("⚠️  Preallocate seek failed (%d)\n", frs);
        }
    }
#endif

    size_t done = 0;
    size_t since_sync = 0;
    const UINT CHUNK = BACKUP_CHUNK;      // 4096
    const size_t SYNC_EVERY = 512 * 1024; // 256 KB

    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!buf) {
        printf("❌ malloc failed for backup buffer\n");
        f_close(&f);
        return false;
    }

    int remount_attempts = 0;

    while (done < total)
    {
        UINT n = (total - done) > CHUNK ? CHUNK : (UINT)(total - done);

        if (!flash_read_data((uint32_t)done, buf, n)) {
            printf("❌ flash_read_data failed at 0x%06lX\n", (unsigned long)done);
            free(buf);
            f_close(&f);
            return false;
        }

        // existing write-with-retries + recovery
        FRESULT frw = write_with_retries(&f, buf, n);
        if (frw != FR_OK) {
            printf("⚠️  Write error (%d) at 0x%06lX — attempting remount/reopen\n",
                   frw, (unsigned long)done);

            f_close(&f);
            sd_unmount();
            sleep_ms(50);
            if (!sd_mount()) {
                printf("❌ Remount failed\n");
                free(buf);
                return false;
            }

            FRESULT fro = f_open(&f, path, FA_WRITE);
            if (fro != FR_OK) {
                printf("❌ Reopen failed (%d)\n", fro);
                free(buf);
                return false;
            }
            if (f_lseek(&f, (FSIZE_t)done) != FR_OK) {
                printf("❌ Seek failed after reopen\n");
                free(buf);
                f_close(&f);
                return false;
            }

            frw = write_with_retries(&f, buf, n);
            if (frw != FR_OK) {
                printf("❌ Write still failing (%d) at 0x%06lX\n", frw, (unsigned long)done);
                free(buf);
                f_close(&f);
                return false;
            }

            if (++remount_attempts > 4) {
                printf("❌ Too many remounts; aborting\n");
                free(buf);
                f_close(&f);
                return false;
            }
        }
        sleep_ms(5); 

        done += n;
        since_sync += n;

        if (since_sync >= SYNC_EVERY || done == total) {
            FRESULT frs = f_sync(&f);
            if (frs != FR_OK) {
                printf("⚠️  f_sync err (%d) at 0x%06lX — trying remount\n", frs, (unsigned long)done);
                f_close(&f);
                sd_unmount();
                sleep_ms(50);
                if (!sd_mount()) {
                    free(buf);
                    return false;
                }
                FRESULT fro = f_open(&f, path, FA_WRITE);
                if (fro != FR_OK) {
                    free(buf);
                    return false;
                }
                if (f_lseek(&f, (FSIZE_t)done) != FR_OK) {
                    free(buf);
                    f_close(&f);
                    return false;
                }
                frs = f_sync(&f);
                if (frs != FR_OK) {
                    printf("❌ f_sync still failing (%d)\n", frs);
                    free(buf);
                    f_close(&f);
                    return false;
                }
            }
            since_sync = 0;
        }

        if ((done % (256 * 1024)) == 0 || done == total) {
            printf("   … %lu / %lu bytes\n", (unsigned long)done, (unsigned long)total);
        }
    }

    free(buf);
    f_close(&f);
    printf("✅ Backup complete: %s (%lu bytes)\n", path, (unsigned long)total);
    return true;
}


bool sd_mount(void)
{
    // Ensure bus is in a clean idle before any probe
    sd_bus_idle();
    sd_bus_to_spi();

    // If we think we're mounted, verify the card/FS
    if (sd_mounted)
    {
        DSTATUS st = disk_status(0);
        if (!(st & (STA_NODISK | STA_NOINIT)))
        {
            DWORD fre;
            FATFS *pfs;
            if (f_getfree("", &fre, &pfs) == FR_OK)
            {
                printf("ℹ️  SD card filesystem already mounted\n");
                return true;
            }
            printf("⚠️  SD mount looks stale. Re-mounting…\n");
            f_mount(NULL, "", 0);
            sd_mounted = false;
        }
        else
        {
            printf("⚠️  SD was marked mounted but card not ready. Re-mounting…\n");
            f_mount(NULL, "", 0);
            sd_mounted = false;
        }
    }

    printf("📁 Mounting 32GB FAT32 SD Card Filesystem...\n");
    printf("===========================================\n");

    printf("🔧 Calling disk_initialize(0)...\n");
    DSTATUS st = disk_initialize(0);
    if (st & STA_NOINIT)
    {
        printf("❌ disk_initialize failed (STA_NOINIT)\n");
        // Make sure CS is HIGH when we exit
        sd_bus_idle();
        return false;
    }

    FRESULT fr = f_mount(&fatfs, "", 1);
    printf("\n📊 Mount operation result: ");
    if (fr == FR_OK)
    {
        printf("FR_OK (0) - Success!\n");
        sd_mounted = true;
        printf("✅ 32GB FAT32 SD Card filesystem mounted successfully!\n");
        printf("📂 Ready for file operations (create/read/write/append)\n");
        printf("===========================================\n");
        return true;
    }

    printf("\n❌ Failed to mount 32GB FAT32 filesystem (error: %d)\n", fr);
    printf("===========================================\n");
    // Leave bus idle with CS HIGH
    sd_bus_idle();
    return false;
}

bool sd_file_exists(const char *filename)
{
    if (!sd_mounted)
    {
        printf("❌ Cannot check file existence - SD card not mounted\n");
        return false;
    }

    printf("🔍 Checking if file exists: %s\n", filename);

    FILINFO fno;
    FRESULT fr = f_stat(filename, &fno);

    if (fr == FR_OK)
    {
        printf("✅ File %s EXISTS (size: %lu bytes)\n", filename, fno.fsize);
        return true;
    }
    else
    {
        printf("📄 File %s NOT FOUND (error: %d)\n", filename, fr);
        return false;
    }
}

bool sd_write_file(const char *filename, const char *content)
{
    if (!sd_mounted)
    {
        printf("❌ SD card not mounted\n");
        return false;
    }

    printf("📝 Creating or opening file: %s\n", filename);

    FIL file;
    FRESULT fr = f_open(&file, filename, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        printf("❌ Failed to open/create file (error: %d)\n", fr);
        return false;
    }

    // Write header only if it truly isn't there
    bool need_header = (f_size(&file) == 0) || !csv_has_header(filename);
    if (need_header)
    {
        const char *header =
            "jedec_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes\r\n";
        UINT bw = 0;

        // Always append the header, do not overwrite anything
        f_lseek(&file, f_size(&file));

        fr = f_write(&file, header, (UINT)strlen(header), &bw);
        if (fr != FR_OK)
        {
            printf("❌ Failed to write header (error: %d)\n", fr);
            f_close(&file);
            return false;
        }
    }

    // If caller passed extra content (rare for header creation), append it.
    if (content && *content)
    {
        // Seek to end
        fr = f_lseek(&file, f_size(&file));
        if (fr != FR_OK)
        {
            f_close(&file);
            return false;
        }

        UINT bytes_written = 0;
        fr = f_write(&file, content, (UINT)strlen(content), &bytes_written);
        if (fr != FR_OK)
        {
            f_close(&file);
            return false;
        }
    }

    fr = f_sync(&file);
    f_close(&file);
    if (fr != FR_OK)
    {
        printf("❌ Failed to sync file (error: %d)\n", fr);
        return false;
    }

    printf("✅ File ready with header\n");
    return true;
}

bool sd_append_to_file(const char *filename, const char *content)
{
    if (!sd_mounted)
    {
        printf("❌ SD card not mounted\n");
        return false;
    }

    // Less verbose logging for frequent operations
    if (strstr(filename, "RESULTS.CSV") != NULL)
    {
        printf("📝 Appending CSV row with Windows compatibility...\n");
    }
    else
    {
        printf("📝 Appending to file: %s\n", filename);
    }

    FIL file;
    FRESULT fr = f_open(&file, filename, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        printf("❌ Failed to open file for append (error: %d)\n", fr);
        return false;
    }

    // If new/empty → write header. If not empty, verify header exists.
    if (f_size(&file) == 0)
    {
        const char *header =
            "jedec_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes\r\n";
        UINT bw = 0;
        fr = f_write(&file, header, (UINT)strlen(header), &bw);
        if (fr != FR_OK)
        {
            printf("❌ Failed to write header (error: %d)\n", fr);
            f_close(&file);
            return false;
        }
    }
    else
    {
        if (!csv_has_header(filename))
        {
            printf("⚠️  File not empty but header missing — adding it now.\n");
            const char *header =
                "jedec_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes\r\n";
            UINT bw = 0;
            f_lseek(&file, f_size(&file));
            fr = f_write(&file, header, (UINT)strlen(header), &bw);
            if (fr != FR_OK)
            {
                printf("❌ Failed to append header (error: %d)\n", fr);
                f_close(&file);
                return false;
            }
        }
    }

    // Seek to end of file and append content
    fr = f_lseek(&file, f_size(&file));
    if (fr != FR_OK)
    {
        printf("❌ Failed to seek to end of file (error: %d)\n", fr);
        f_close(&file);
        return false;
    }

    UINT bytes_written = 0;
    size_t len = strlen(content);
    fr = f_write(&file, content, (UINT)len, &bytes_written);
    if (fr != FR_OK)
    {
        printf("❌ Failed to write to file (error: %d)\n", fr);
        f_close(&file);
        return false;
    }

    // Ensure each row ends with CRLF
    bool has_crlf = (len >= 2 && content[len - 2] == '\r' && content[len - 1] == '\n');
    if (!has_crlf)
    {
        const char *crlf = "\r\n";
        fr = f_write(&file, crlf, 2, &bytes_written);
        if (fr != FR_OK)
        {
            printf("❌ Failed to write CRLF (error: %d)\n", fr);
            f_close(&file);
            return false;
        }
    }

    fr = f_sync(&file); // push data to card
    f_close(&file);
    if (fr != FR_OK)
    {
        printf("❌ Failed to sync file (error: %d)\n", fr);
        return false;
    }

    // Short delay to help some cards settle
    sleep_ms(10);

    if (strstr(filename, "RESULTS.CSV") != NULL)
    {
        printf("✅ CSV row saved with Windows compatibility\n");
    }
    else
    {
        printf("✅ Content appended successfully\n");
    }
    return true;
}

void sd_unmount(void)
{
    if (sd_mounted)
    {
        f_mount(NULL, "", 0);
        sd_mounted = false;
        printf("📁 SD Card unmounted\n");
    }
    // Always force CS HIGH and bus idle when leaving
    sd_bus_idle();
}

bool sd_is_mounted(void)
{
    return sd_mounted;
}

int sd_count_csv_rows(const char *filename, int *out_total_lines, int *out_data_rows)
{
    if (!sd_mounted)
    {
        printf("❌ SD not mounted\n");
        if (out_total_lines)
            *out_total_lines = 0;
        if (out_data_rows)
            *out_data_rows = 0;
        return -1;
    }

    FIL f;
    FRESULT fr = f_open(&f, filename, FA_OPEN_EXISTING | FA_READ);
    if (fr == FR_NO_FILE)
    {
        printf("ℹ️  %s not found -> 0 rows\n", filename);
        if (out_total_lines)
            *out_total_lines = 0;
        if (out_data_rows)
            *out_data_rows = 0;
        return 0;
    }
    if (fr != FR_OK)
    {
        printf("❌ f_open failed (%d)\n", fr);
        return -1;
    }

    const char *HEADER =
        "jedec_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes";

    // check header quickly
    char peek[200] = {0};
    UINT br = 0;
    f_lseek(&f, 0);
    f_read(&f, peek, sizeof(peek) - 1, &br);
    bool header_present = (br > 0 && strncmp(peek, HEADER, strlen(HEADER)) == 0);

    // count '\n' across the file
    int total_lines = 0;
    f_lseek(&f, 0);
    char buf[256];
    while (1)
    {
        br = 0;
        fr = f_read(&f, buf, sizeof(buf), &br);
        if (fr != FR_OK)
        {
            printf("❌ f_read error (%d)\n", fr);
            break;
        }
        if (br == 0)
            break; // EOF
        for (UINT i = 0; i < br; i++)
        {
            if (buf[i] == '\n')
                total_lines++;
        }
    }

    f_close(&f);

    int data_rows = total_lines - (header_present ? 1 : 0);
    if (data_rows < 0)
        data_rows = 0;

    if (out_total_lines)
        *out_total_lines = total_lines;
    if (out_data_rows)
        *out_data_rows = data_rows;

    printf("📄 %s: total lines=%d, header=%s, data rows=%d\n",
           filename, total_lines, header_present ? "YES" : "NO", data_rows);
    return 0;
}
