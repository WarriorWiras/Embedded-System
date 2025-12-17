/*
 * SPI Flash Benchmark Library
 * Goal: time how fast a flash chip can read, write (program), and erase,
 *       and provide simple helper functions for low-level SPI flash ops.
 */

#ifndef _FLASH_BENCHMARK_H
#define _FLASH_BENCHMARK_H

#include "pico.h"
#include "hardware/spi.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {                 // Make the header usable from C and C++
#endif

    // ------------------------------------------------------------
    // Result of one benchmark run (matches the CSV column order)
    // ------------------------------------------------------------
    typedef struct
    {
        char    chip_id[32];        // Example: "Winbond_25Q32", "Unknown_ChipA"
        char    operation[16];      // "read", "program", or "erase"
        uint32_t block_size;        // Bytes processed in this run
        uint32_t address;           // Start offset inside flash
        uint64_t elapsed_us;        // Time taken (microseconds)
        float    throughput_MBps;   // block_size / time, in MB/s
        int      run_number;        // Which run this was (1, 2, 3, …)
        float    temp_C;            // Temperature during the run (°C)
        float    voltage_V;         // Supply voltage during the run (V)
        char     pattern[16];       // Data pattern used (e.g., "0xFF", "random")
        char     notes[64];         // Any extra info
    } benchmark_result_t;

    // ------------------------------------------------------------
    // High-level benchmark API (called by main.c)
    // ------------------------------------------------------------

    /**
     * flash_benchmark_init
     * Set up SPI and any pins needed for the external flash.
     * @return 1 on success, 0 on failure.
     */
    int flash_benchmark_init(void);

    /**
     * flash_identify_chip
     * Read JEDEC ID and turn it into a friendly name (if known).
     * @param chip_name  output buffer
     * @param name_size  size of output buffer
     * @return 1 on success, 0 on failure.
     */
    int flash_identify_chip(char *chip_name, size_t name_size);

    /**
     * benchmark_flash_read / program / erase
     * Time how long the operation takes. Return elapsed microseconds.
     * - address: where in flash to start
     * - size: bytes to process (for erase, usually sector size)
     * - pattern: for read/program, how to fill/check data ("0xFF", "random", …)
     */
    uint64_t benchmark_flash_read(uint32_t address, uint32_t size, const char *pattern);
    uint64_t benchmark_flash_program(uint32_t address, uint32_t size, const char *pattern);
    uint64_t benchmark_flash_erase(uint32_t address, uint32_t size);

    /**
     * generate_test_pattern
     * Fill a buffer with the requested data pattern:
     *   - "0xFF", "0x00", "0x55", "incremental", "random", etc.
     */
    void generate_test_pattern(uint8_t *buffer, uint32_t size, const char *pattern_type);

    // ------------------------------------------------------------
    // Low-level flash helpers (used by the benchmark functions)
    // ------------------------------------------------------------

    /**
     * flash_read_jedec_id
     * Read standard 3-byte JEDEC ID: manufacturer + 2 device bytes.
     * @return 1 on success, 0 on failure.
     */
    int flash_read_jedec_id(uint8_t *manufacturer, uint8_t *device_id_1, uint8_t *device_id_2);

    /**
     * flash_wait_busy
     * Poll the status register until the chip is not busy.
     * @return 1 when ready, 0 on timeout/error.
     */
    int flash_wait_busy(void);

    /**
     * flash_write_enable
     * Set the WEL (Write Enable Latch) before program/erase.
     * @return 1 on success, 0 on failure.
     */
    int flash_write_enable(void);

    /**
     * flash_sector_erase
     * Erase one sector at 'address' (usually 4 KB).
     * @return 1 on success, 0 on failure.
     */
    int flash_sector_erase(uint32_t address);

    /**
     * flash_page_program
     * Program up to one page (typically 256 bytes) at 'address'.
     * If 'size' > page, implementation should loop or limit to a page.
     * @return 1 on success, 0 on failure.
     */
    int flash_page_program(uint32_t address, const uint8_t *data, uint32_t size);

    /**
     * flash_read_data
     * Read 'size' bytes from 'address' into 'buffer'.
     * @return 1 on success, 0 on failure.
     */
    int flash_read_data(uint32_t address, uint8_t *buffer, uint32_t size);

    // ------------------------------------------------------------
    // Common SPI flash command codes (JEDEC standard)
    // ------------------------------------------------------------
    #define FLASH_CMD_READ_DATA        0x03
    #define FLASH_CMD_FAST_READ        0x0B
    #define FLASH_CMD_PAGE_PROGRAM     0x02
    #define FLASH_CMD_SECTOR_ERASE     0x20
    #define FLASH_CMD_BLOCK_ERASE_32K  0x52
    #define FLASH_CMD_BLOCK_ERASE_64K  0xD8
    #define FLASH_CMD_CHIP_ERASE       0xC7
    #define FLASH_CMD_WRITE_ENABLE     0x06
    #define FLASH_CMD_WRITE_DISABLE    0x04
    #define FLASH_CMD_READ_STATUS      0x05
    #define FLASH_CMD_JEDEC_ID         0x9F
    #define FLASH_CMD_POWER_DOWN       0xB9
    #define FLASH_CMD_POWER_UP         0xAB

    // Status register bits
    #define FLASH_STATUS_BUSY          0x01   // 1 = busy (program/erase in progress)
    #define FLASH_STATUS_WEL           0x02   // 1 = write enabled

    // ------------------------------------------------------------
    // Typical sizes for many SPI NOR flash chips
    // ------------------------------------------------------------
    #define FLASH_PAGE_SIZE            256     // page program granularity
    #define FLASH_SECTOR_SIZE          4096    // 4 KB erase
    #define FLASH_BLOCK_SIZE_32K       32768   // 32 KB erase
    #define FLASH_BLOCK_SIZE_64K       65536   // 64 KB erase

#ifdef __cplusplus
}
#endif

#endif // _FLASH_BENCHMARK_H
