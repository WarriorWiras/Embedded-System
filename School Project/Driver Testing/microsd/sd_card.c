/*
 * SD Card Implementation using FatFs
 * Simple and reliable SD card file operations
 *
 * Big picture:
 * - We mount the SD card so we can read/write files.
 * - We make sure CSV files always have a header.
 * - We can write a new file, append to a file, check if a file exists,
 *   unmount the card, and count CSV lines.
 */

#include "sd_card.h"
#include "fatfs/ff.h"      // FatFs: file system API (f_open, f_read, f_write, etc.)
#include "pico/stdlib.h"   // Pico helpers (sleep_ms, printf, etc.)
#include "fatfs/diskio.h"  // Low-level disk functions (disk_initialize)
#include <stdio.h>
#include <string.h>
#include <stdlib.h>   // for strtod


/* ----------------------------------------------------------
 * Helper: check if a CSV file already starts with the header.
 * Returns true if the file exists and begins with "chip_id,"
 * ---------------------------------------------------------- */
static bool csv_has_header(const char *filename)
{
    FIL f;  // File object
    // Try to open the file read-only. If it doesn't exist, no header.
    if (f_open(&f, filename, FA_OPEN_EXISTING | FA_READ) != FR_OK)
        return false;

    UINT br = 0;           // bytes read
    char first[16] = {0};  // small peek buffer

    // Read the first few bytes at the start of the file
    f_lseek(&f, 0);
    f_read(&f, first, sizeof(first) - 1, &br);
    f_close(&f);

    // If it starts with "chip_id,", we consider the header present
    return (br >= 8 && memcmp(first, "chip_id,", 8) == 0);
}

/* -----------------------------------------
 * Global FatFs state for the mounted disk
 * ----------------------------------------- */
static FATFS fatfs;        // The filesystem object (one per drive)
static bool sd_mounted = false;

/* ----------------------------------------------------------
 * sd_card_init
 * High-level "hello" step. We only print info here since the
 * real hardware bring-up is done in diskio.c (via mount step).
 * ---------------------------------------------------------- */
bool sd_card_init(void)
{
    printf("# Initializing 32GB FAT32 SD Card System...\n");
    printf("============================================\n");

    // Friendly, human-readable info for users
    printf("# SD Card Requirements Check:\n");
    printf("   - Capacity: 32GB (recommended)\n");
    printf("   - Format: FAT32 (required)\n");
    printf("   - Connection: Maker Pi Pico W SD slot (GP10-GP15)\n");
    printf("   - Status: Checking...\n\n");

    printf("# Hardware Configuration:\n");
    printf("   - SPI Port: spi1 (hardware SPI)\n");
    printf("   - CS Pin: GP15 (Chip Select)\n");
    printf("   - SCK Pin: GP10 (Serial Clock)\n");
    printf("   - MOSI Pin: GP11 (Master Out Slave In)\n");
    printf("   - MISO Pin: GP12 (Master In Slave Out)\n\n");

    // The actual wires/SD commands are done in diskio.c when we mount
    printf("# Starting low-level SD card initialization...\n");
    printf("   (Detailed SPI communication logs will follow)\n\n");

    printf("# SD Card hardware interface ready\n");
    printf("   Next step: Filesystem mounting (f_mount)\n");
    printf("============================================\n");

    return true;
}

/* ----------------------------------------------------------
 * sd_mount
 * Mount (open) the filesystem so we can use files/folders.
 * Steps:
 * 1) Initialize the low-level disk (disk_initialize).
 * 2) Call f_mount to make the filesystem ready.
 * ---------------------------------------------------------- */
bool sd_mount(void)
{
    if (sd_mounted)
    {
        // If we already mounted, don't do it again
        printf("#  SD card filesystem already mounted\n");
        return true;
    }

    printf("# Mounting 32GB FAT32 SD Card Filesystem...\n");
    printf("===========================================\n");

    // Make sure low-level driver is ready (SPI + SD card init)
    printf("# Calling disk_initialize(0)...\n");
    DSTATUS st = disk_initialize(0);
    if (st & STA_NOINIT)
    {
        printf("### disk_initialize failed (STA_NOINIT)\n");
        return false;
    }

    // Now it’s safe for FatFs to read the boot sector / MBR
    FRESULT fr = f_mount(&fatfs, "", 1);

    printf("\n# Mount operation result: ");
    if (fr == FR_OK)
    {
        printf("FR_OK (0) - Success!\n");
        sd_mounted = true;
        printf("# 32GB FAT32 SD Card filesystem mounted successfully!\n");
        printf("# Ready for file operations (create/read/write/append)\n");
        printf("===========================================\n");
        return true;
    }

    // You can add a switch(fr) here to decode error codes if desired
    printf("\n### Failed to mount 32GB FAT32 filesystem (error: %d)\n", fr);
    printf("===========================================\n");
    return false;
}

/* ----------------------------------------------------------
 * sd_file_exists
 * Quick check: does a given file name exist on the card?
 * Uses FatFs f_stat. Returns true if found.
 * ---------------------------------------------------------- */
bool sd_file_exists(const char *filename)
{
    if (!sd_mounted)
    {
        printf("### Cannot check file existence - SD card not mounted\n");
        return false;
    }

    printf("# Checking if file exists: %s\n", filename);

    FILINFO fno;
    FRESULT fr = f_stat(filename, &fno);

    if (fr == FR_OK)
    {
        printf("# File %s EXISTS (size: %lu bytes)\n", filename, fno.fsize);
        return true;
    }
    else
    {
        printf("# File %s NOT FOUND (error: %d)\n", filename, fr);
        return false;
    }
}

/* ----------------------------------------------------------
 * sd_write_file
 * Create a file if missing (or open if present) and:
 * - Ensure CSV header exists.
 * - Optionally write extra content provided by caller.
 *
 * NOTE: This function never erases existing data; it seeks to
 * the end before writing header/content.
 * ---------------------------------------------------------- */
bool sd_write_file(const char *filename, const char *content)
{
    if (!sd_mounted)
    {
        printf("### SD card not mounted\n");
        return false;
    }

    printf("# Creating or opening file: %s\n", filename);

    FIL file;
    // FA_OPEN_ALWAYS: open if exists, otherwise create
    FRESULT fr = f_open(&file, filename, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        printf("### Failed to open/create file (error: %d)\n", fr);
        return false;
    }

    // If empty OR header missing → add header once
    bool need_header = (f_size(&file) == 0) || !csv_has_header(filename);
    if (need_header)
    {
        const char *header =
            "chip_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes\r\n";
        UINT bw = 0;

        // Append header at end (even if file had leftover bytes)
        f_lseek(&file, f_size(&file));

        fr = f_write(&file, header, (UINT)strlen(header), &bw);
        if (fr != FR_OK)
        {
            printf("### Failed to write header (error: %d)\n", fr);
            f_close(&file);
            return false;
        }
    }

    // If caller passed extra content, append it after the header
    if (content && *content)
    {
        // Seek to end so we never overwrite
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

    // Make sure data is actually flushed to the card
    fr = f_sync(&file);
    f_close(&file);
    if (fr != FR_OK)
    {
        printf("### Failed to sync file (error: %d)\n", fr);
        return false;
    }

    printf("# File ready with header\n");
    return true;
}

/* ----------------------------------------------------------
 * sd_append_to_file
 * Append a CSV row (or any text) to an existing file.
 * - If file is new/empty → write header first.
 * - If file has data but no header → add the header.
 * - Always end each row with CRLF for Windows CSVs.
 * ---------------------------------------------------------- */
bool sd_append_to_file(const char *filename, const char *content)
{
    if (!sd_mounted)
    {
        printf("### SD card not mounted\n");
        return false;
    }

    // Less spammy logs if we're appending the main results file
    if (strstr(filename, "RESULTS.CSV") != NULL)
    {
        printf("# Appending CSV row with Windows compatibility...\n");
    }
    else
    {
        printf("# Appending to file: %s\n", filename);
    }

    FIL file;
    FRESULT fr = f_open(&file, filename, FA_OPEN_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        printf("### Failed to open file for append (error: %d)\n", fr);
        return false;
    }

    // If the file is new/empty, write the header
    if (f_size(&file) == 0)
    {
        const char *header =
            "chip_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes\r\n";
        UINT bw = 0;
        fr = f_write(&file, header, (UINT)strlen(header), &bw);
        if (fr != FR_OK)
        {
            printf("### Failed to write header (error: %d)\n", fr);
            f_close(&file);
            return false;
        }
    }
    else
    {
        // If not empty but somehow missing header, add it now
        if (!csv_has_header(filename))
        {
            printf("###  File not empty but header missing — adding it now.\n");
            const char *header =
                "chip_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes\r\n";
            UINT bw = 0;
            f_lseek(&file, f_size(&file));
            fr = f_write(&file, header, (UINT)strlen(header), &bw);
            if (fr != FR_OK)
            {
                printf("### Failed to append header (error: %d)\n", fr);
                f_close(&file);
                return false;
            }
        }
    }

    // Go to end of file and write the content
    fr = f_lseek(&file, f_size(&file));
    if (fr != FR_OK)
    {
        printf("### Failed to seek to end of file (error: %d)\n", fr);
        f_close(&file);
        return false;
    }

    UINT bytes_written = 0;
    size_t len = strlen(content);
    fr = f_write(&file, content, (UINT)len, &bytes_written);
    if (fr != FR_OK)
    {
        printf("### Failed to write to file (error: %d)\n", fr);
        f_close(&file);
        return false;
    }

    // Make sure each row ends with CRLF (\r\n) so Excel/Windows is happy
    bool has_crlf = (len >= 2 && content[len - 2] == '\r' && content[len - 1] == '\n');
    if (!has_crlf)
    {
        const char *crlf = "\r\n";
        fr = f_write(&file, crlf, 2, &bytes_written);
        if (fr != FR_OK)
        {
            printf("### Failed to write CRLF (error: %d)\n", fr);
            f_close(&file);
            return false;
        }
    }

    // Push buffered data to the card before closing
    fr = f_sync(&file);
    f_close(&file);
    if (fr != FR_OK)
    {
        printf("### Failed to sync file (error: %d)\n", fr);
        return false;
    }

    // Small settle time helps some slower SD cards
    sleep_ms(10);

    if (strstr(filename, "RESULTS.CSV") != NULL)
    {
        printf("# CSV row saved with Windows compatibility\n");
    }
    else
    {
        printf("# Content appended successfully\n");
    }
    return true;
}

/* ----------------------------------------------------------
 * sd_unmount
 * Cleanly detach the filesystem. Do this before power-off or
 * removing the SD card to avoid corruption.
 * ---------------------------------------------------------- */
void sd_unmount(void)
{
    if (sd_mounted)
    {
        f_mount(NULL, "", 0);  // Unmount
        sd_mounted = false;
        printf("# SD Card unmounted\n");
    }
}

/* ----------------------------------------------------------
 * sd_count_csv_rows
 * Count the total lines in a CSV (by counting '\n') and the
 * number of "data" rows (total minus header if present).
 * - Returns 0 on success, -1 on error.
 * - Writes totals into out_* pointers if provided.
 * ---------------------------------------------------------- */
int sd_count_csv_rows(const char *filename, int *out_total_lines, int *out_data_rows)
{
    if (!sd_mounted)
    {
        printf("### SD not mounted\n");
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
        // File not found → zero rows
        printf("#  %s not found -> 0 rows\n", filename);
        if (out_total_lines)
            *out_total_lines = 0;
        if (out_data_rows)
            *out_data_rows = 0;
        return 0;
    }
    if (fr != FR_OK)
    {
        printf("### f_open failed (%d)\n", fr);
        return -1;
    }

    // CSV header string we expect at the top of the file
    const char *HEADER =
        "chip_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes";

    // Quick header check by peeking at the start of the file
    char peek[200] = {0};
    UINT br = 0;
    f_lseek(&f, 0);
    f_read(&f, peek, sizeof(peek) - 1, &br);
    bool header_present = (br > 0 && strncmp(peek, HEADER, strlen(HEADER)) == 0);

    // Count number of '\n' characters across the whole file
    int total_lines = 0;
    f_lseek(&f, 0);
    char buf[256];
    while (1)
    {
        br = 0;
        fr = f_read(&f, buf, sizeof(buf), &br);
        if (fr != FR_OK)
        {
            printf("### f_read error (%d)\n", fr);
            break;
        }
        if (br == 0)
            break; // End of file

        for (UINT i = 0; i < br; i++)
        {
            if (buf[i] == '\n')
                total_lines++;
        }
    }

    f_close(&f);

    // Data rows = total minus header (if present)
    int data_rows = total_lines - (header_present ? 1 : 0);
    if (data_rows < 0)
        data_rows = 0;

    if (out_total_lines)
        *out_total_lines = total_lines;
    if (out_data_rows)
        *out_data_rows = data_rows;

    printf("# %s: total lines=%d, header=%s, data rows=%d\n",
           filename, total_lines, header_present ? "YES" : "NO", data_rows);
    return 0;
}

// ---- Average temperature over CSV (temp_C column) ---------------------

// Extract the temp_C field (8th column, 0-based index 7) from one CSV line.
// Returns true and writes *out_temp if parsed, else false.
static bool csv_extract_tempC(const char *line, double *out_temp) {
    // Skip header or blank lines
    if (!line || !*line) return false;
    if (strncmp(line, "chip_id,", 8) == 0) return false; // header

    // We want the token after 7 commas (0..6 skip, 7th is temp_C).
    const char *p = line;
    int commas = 0;

    while (*p && commas < 7) {
        if (*p == ',') commas++;
        p++;
    }
    if (commas < 7) return false; // not enough fields

    // p now points to start of temp_C token
    const char *start = p;
    while (*p && *p != ',' && *p != '\r' && *p != '\n') p++;
    size_t len = (size_t)(p - start);
    if (len == 0) return false;

    char buf[32];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    char *endptr = NULL;
    double val = strtod(buf, &endptr);
    if (endptr == buf) return false; // no conversion

    if (out_temp) *out_temp = val;
    return true;
}

bool sd_compute_avg_temp(const char *filename, double *out_avg, int *out_count)
{
    if (out_avg) *out_avg = 0.0;
    if (out_count) *out_count = 0;

    if (!sd_mounted) {
        printf("### SD not mounted\n");
        return false;
    }

    FIL f;
    FRESULT fr = f_open(&f, filename, FA_OPEN_EXISTING | FA_READ);
    if (fr == FR_NO_FILE) {
        printf("# %s not found; no temperatures to average\n", filename);
        return true; // empty dataset is not an error
    }
    if (fr != FR_OK) {
        printf("### f_open failed (%d)\n", fr);
        return false;
    }

    // Read file in chunks and process line-by-line.
    char carry[256];
    size_t carry_len = 0;

    double sum = 0.0;
    int count = 0;

    UINT br = 0;
    char buf[256];

    while (1) {
        br = 0;
        fr = f_read(&f, buf, sizeof(buf), &br);
        if (fr != FR_OK) {
            printf("### f_read error (%d)\n", fr);
            break;
        }
        if (br == 0) {
            // EOF: process any trailing partial line
            if (carry_len > 0) {
                carry[carry_len] = '\0';
                double t;
                if (csv_extract_tempC(carry, &t)) {
                    sum += t;
                    count++;
                }
                carry_len = 0;
            }
            break;
        }

        size_t i = 0;
        while (i < br) {
            char c = buf[i++];
            if (c == '\n') {
                // complete line: terminate and process
                carry[carry_len] = '\0';
                double t;
                if (csv_extract_tempC(carry, &t)) {
                    sum += t;
                    count++;
                }
                carry_len = 0;
            } else if (c != '\r') {
                if (carry_len + 1 < sizeof(carry)) {
                    carry[carry_len++] = c;
                } else {
                    // Line too long: skip until newline
                    while (i < br && buf[i - 1] != '\n') i++;
                    carry_len = 0;
                }
            }
        }
    }

    f_close(&f);

    if (count > 0) {
        if (out_avg) *out_avg = sum / (double)count;
        if (out_count) *out_count = count;
    } else {
        if (out_avg) *out_avg = 0.0;
        if (out_count) *out_count = 0;
    }

    return true;
}

// Dump a whole text file to the serial monitor (CRLF -> LF for prettier output)
bool sd_print_file(const char *filename)
{
    if (!sd_mounted) {
        printf("❌ Cannot print file - SD card not mounted\n");
        return false;
    }

    FIL f;
    FRESULT fr = f_open(&f, filename, FA_OPEN_EXISTING | FA_READ);
    if (fr == FR_NO_FILE) {
        printf("ℹ️  %s not found\n", filename);
        return false;
    }
    if (fr != FR_OK) {
        printf("❌ f_open(%s) failed (%d)\n", filename, fr);
        return false;
    }

    printf("\n----- BEGIN %s -----\n", filename);

    UINT br = 0;
    char buf[256];
    for (;;) {
        fr = f_read(&f, buf, sizeof(buf), &br);
        if (fr != FR_OK) {
            printf("\n❌ f_read error (%d)\n", fr);
            break;
        }
        if (br == 0) break; // EOF

        // Print, skipping '\r' so CRLF appears as single newline on most terminals
        for (UINT i = 0; i < br; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            putchar(c);
        }
    }

    printf("\n-----  END %s  -----\n", filename);
    f_close(&f);
    return true;
}

