/*
 * SPI Flash Benchmark Library
 * Comprehensive performance testing and forensic analysis
 */
#pragma once
#ifndef FLASH_BENCHMARK_H
#define FLASH_BENCHMARK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "chip_db.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ============================== Data Types =============================== */
/** Optional container mirroring your CSV schema (handy for in-memory use). */
typedef struct {
    char     chip_id[32];       // e.g., "EF 40 16" or friendly name if desired
    char     operation[16];     // "read", "program", "erase"
    uint32_t block_size;        // bytes
    uint32_t address;           // offset
    uint64_t elapsed_us;        // microseconds
    float    throughput_MBps;   // derived
    int      run_number;        // trial index
    float    temp_C;            // environment snapshot
    float    voltage_V;         // environment snapshot
    char     pattern[16];       // "0xFF", "incremental", etc.
    char     notes[64];         // freeform
} benchmark_result_t;

/* ============================== Public API =============================== */
/* Init + identification */
int      flash_benchmark_init(void);
int      flash_identify_chip(char *chip_name, size_t name_size);
void     flash_get_jedec_str(char *out, size_t n);     // "EF 40 16" etc.
size_t   flash_capacity_bytes(void);

/* Timed benchmarks */
uint64_t benchmark_flash_read   (uint32_t address, uint32_t size, const char *pattern);
uint64_t benchmark_flash_program(uint32_t address, uint32_t size, const char *pattern);
uint64_t benchmark_flash_erase  (uint32_t address, uint32_t size);
// Returns the actual SPI SCK frequency (Hz) used for the flash, or 0 if unknown.
uint32_t flash_spi_get_baud_hz(void);

/* Patterns */
void     generate_test_pattern(uint8_t *buffer, uint32_t size, const char *pattern_type);
void flash_unprotect_all(void);

/* Low-level operations (blocking, basic checks) */
int      flash_read_jedec_id (uint8_t *manufacturer, uint8_t *device_id_1, uint8_t *device_id_2);
int      flash_wait_busy     (void);
int      flash_write_enable  (void);
int      flash_sector_erase  (uint32_t address);
int      flash_page_program  (uint32_t address, const uint8_t *data, uint32_t size);
int      flash_read_data     (uint32_t address, uint8_t *buffer, uint32_t size);
int      flash_soft_reset    (void);   // 0x66 -> 0x99 -> 0xAB
int      flash_dump          (uint32_t address, uint32_t len);

/* ============================== Command Set ============================== */
#define FLASH_CMD_READ_DATA         0x03
#define FLASH_CMD_FAST_READ         0x0B
#define FLASH_CMD_PAGE_PROGRAM      0x02
#define FLASH_CMD_SECTOR_ERASE      0x20
#define FLASH_CMD_BLOCK_ERASE_32K   0x52
#define FLASH_CMD_BLOCK_ERASE_64K   0xD8
#define FLASH_CMD_CHIP_ERASE        0xC7
#define FLASH_CMD_WRITE_ENABLE      0x06
#define FLASH_CMD_WRITE_DISABLE     0x04
#define FLASH_CMD_READ_STATUS       0x05
#define FLASH_CMD_JEDEC_ID          0x9F
#define FLASH_CMD_POWER_DOWN        0xB9
#define FLASH_CMD_POWER_UP          0xAB
#define FLASH_CMD_RESET_ENABLE      0x66
#define FLASH_CMD_RESET             0x99

/* Status bits */
#define FLASH_STATUS_BUSY           0x01
#define FLASH_STATUS_WEL            0x02

/* Geometry */
#define FLASH_PAGE_SIZE             256
#define FLASH_SECTOR_SIZE           4096
#define FLASH_BLOCK_SIZE_32K        32768
#define FLASH_BLOCK_SIZE_64K        65536

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FLASH_BENCHMARK_H */
