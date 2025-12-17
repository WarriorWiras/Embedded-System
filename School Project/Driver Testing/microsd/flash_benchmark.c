/*
 * SPI Flash Benchmark Implementation
 * High-precision timing and forensic analysis of flash memory performance
 *
 * What this file does:
 * - Sets up SPI to talk to an external NOR flash chip.
 * - Provides low-level helpers (read JEDEC ID, read/program/erase).
 * - Provides high-level timed benchmarks for read/program/erase.
 */

#include "flash_benchmark.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ------------------------------------------------------------
// Hardware configuration for flash chip (on SPI0)
// Adjust pins to match your wiring/board.
// ------------------------------------------------------------
#define FLASH_SPI_INST  spi0
#define FLASH_CS_PIN    17
#define FLASH_SCK_PIN   18
#define FLASH_MOSI_PIN  19
#define FLASH_MISO_PIN  16

// Global flash chip initialization state
static bool flash_initialized = false;

// ------------------------------------------------------------
// Timing helper: microseconds since boot
// ------------------------------------------------------------
static inline uint64_t get_time_us(void)
{
    return to_us_since_boot(get_absolute_time());
}

// ------------------------------------------------------------
// SPI chip-select helpers (active low)
// A tiny delay helps meet tCSS/tCSH on some parts.
// ------------------------------------------------------------
static void flash_cs_select(void)
{
    gpio_put(FLASH_CS_PIN, 0);
    sleep_us(1);
}

static void flash_cs_deselect(void)
{
    sleep_us(1);
    gpio_put(FLASH_CS_PIN, 1);
}

// ------------------------------------------------------------
// Write a single-byte command over SPI
// ------------------------------------------------------------
static void flash_write_cmd(uint8_t cmd)
{
    spi_write_blocking(FLASH_SPI_INST, &cmd, 1);
}

// ------------------------------------------------------------
// Send a 24-bit address (most common for SPI NOR)
// ------------------------------------------------------------
static void flash_write_addr(uint32_t addr)
{
    uint8_t addr_bytes[3] = {
        (addr >> 16) & 0xFF,
        (addr >> 8)  & 0xFF,
        (addr)       & 0xFF
    };
    spi_write_blocking(FLASH_SPI_INST, addr_bytes, 3);
}

// ------------------------------------------------------------
// Initialize SPI and probe the flash by reading its JEDEC ID.
// Returns 1 on success (chip found), 0 otherwise.
// ------------------------------------------------------------
int flash_benchmark_init(void)
{
    printf("# Initializing Flash SPI interface...\n");

    // Start SPI0 at 8 MHz (conservative, reliable default)
    spi_init(FLASH_SPI_INST, 8000000);

    // Set SPI pins to their SPI function
    gpio_set_function(FLASH_SCK_PIN,  GPIO_FUNC_SPI);
    gpio_set_function(FLASH_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_MISO_PIN, GPIO_FUNC_SPI);

    // CS pin is manual GPIO
    gpio_init(FLASH_CS_PIN);
    gpio_set_dir(FLASH_CS_PIN, GPIO_OUT);
    gpio_put(FLASH_CS_PIN, 1);  // deselect

    sleep_ms(10);

    // Probe: read JEDEC ID
    uint8_t manufacturer, device_id_1, device_id_2;
    if (flash_read_jedec_id(&manufacturer, &device_id_1, &device_id_2))
    {
        printf("# Flash detected: Mfg=0x%02X, Dev=0x%02X%02X\n",
               manufacturer, device_id_1, device_id_2);
        flash_initialized = true;
        return 1;
    }
    else
    {
        printf("### No flash chip detected\n");
        return 0;
    }
}

// ------------------------------------------------------------
// Read JEDEC ID (manufacturer + 2 device bytes).
// Returns 1 if response seems valid, 0 if not.
// ------------------------------------------------------------
int flash_read_jedec_id(uint8_t *manufacturer, uint8_t *device_id_1, uint8_t *device_id_2)
{
    uint8_t id_data[3];

    flash_cs_select();
    flash_write_cmd(FLASH_CMD_JEDEC_ID);
    spi_read_blocking(FLASH_SPI_INST, 0xFF, id_data, 3);
    flash_cs_deselect();

    *manufacturer = id_data[0];
    *device_id_1  = id_data[1];
    *device_id_2  = id_data[2];

    // Basic sanity: reject all-0xFF or all-0x00 manufacturer
    return (id_data[0] != 0xFF && id_data[0] != 0x00);
}

// ------------------------------------------------------------
// Turn JEDEC ID into a friendly chip string (best-effort).
// Returns 1 if we got an answer from the chip (even if "Unknown_*").
// Returns 0 if flash is uninitialized or no response.
// ------------------------------------------------------------
int flash_identify_chip(char *chip_name, size_t name_size)
{
    if (!flash_initialized)
    {
        strncpy(chip_name, "Unknown_Uninitialized", name_size - 1);
        chip_name[name_size - 1] = '\0';
        return 0;
    }

    uint8_t manufacturer, device_id_1, device_id_2;
    if (!flash_read_jedec_id(&manufacturer, &device_id_1, &device_id_2))
    {
        strncpy(chip_name, "Unknown_NoResponse", name_size - 1);
        chip_name[name_size - 1] = '\0';
        return 0;
    }

    // A few common manufacturers; otherwise print raw IDs.
    if (manufacturer == 0xEF)            // Winbond
    {
        if (device_id_1 == 0x40 && device_id_2 == 0x16)
            strncpy(chip_name, "Winbond_W25Q32", name_size - 1);
        else if (device_id_1 == 0x40 && device_id_2 == 0x17)
            strncpy(chip_name, "Winbond_W25Q64", name_size - 1);
        else
            snprintf(chip_name, name_size, "Winbond_Unknown_%02X%02X", device_id_1, device_id_2);
    }
    else if (manufacturer == 0x20)       // Micron
    {
        snprintf(chip_name, name_size, "Micron_%02X%02X", device_id_1, device_id_2);
    }
    else if (manufacturer == 0xC2)       // Macronix
    {
        snprintf(chip_name, name_size, "Macronix_%02X%02X", device_id_1, device_id_2);
    }
    else if (manufacturer == 0x1F)       // Atmel/Microchip
    {
        snprintf(chip_name, name_size, "Atmel_%02X%02X", device_id_1, device_id_2);
    }
    else
    {
        snprintf(chip_name, name_size, "Unknown_%02X_%02X%02X",
                 manufacturer, device_id_1, device_id_2);
    }

    chip_name[name_size - 1] = '\0';
    return 1;
}

// ------------------------------------------------------------
// Poll status register until BUSY bit clears (or timeout).
// Returns 1 when ready, 0 on timeout.
// ------------------------------------------------------------
int flash_wait_busy(void)
{
    uint8_t status;
    int timeout = 10000; // ~10 s worst-case safeguard

    do
    {
        flash_cs_select();
        flash_write_cmd(FLASH_CMD_READ_STATUS);
        spi_read_blocking(FLASH_SPI_INST, 0xFF, &status, 1);
        flash_cs_deselect();

        if (!(status & FLASH_STATUS_BUSY))
            return 1; // ready

        sleep_us(100);
        timeout--;
    } while (timeout > 0);

    return 0; // timed out
}

// ------------------------------------------------------------
// Send WRITE ENABLE (needed before program/erase).
// Always returns 1 (assumes bus is OK).
// ------------------------------------------------------------
int flash_write_enable(void)
{
    flash_cs_select();
    flash_write_cmd(FLASH_CMD_WRITE_ENABLE);
    flash_cs_deselect();
    return 1;
}

// ------------------------------------------------------------
// Read 'size' bytes from 'address' into 'buffer'.
// Returns 1 on success.
// ------------------------------------------------------------
int flash_read_data(uint32_t address, uint8_t *buffer, uint32_t size)
{
    flash_cs_select();
    flash_write_cmd(FLASH_CMD_READ_DATA);
    flash_write_addr(address);
    spi_read_blocking(FLASH_SPI_INST, 0xFF, buffer, size);
    flash_cs_deselect();
    return 1;
}

// ------------------------------------------------------------
// Program up to one page (typically 256 bytes) at 'address'.
// If 'size' > page size, we clamp to one page (caller loops).
// Returns 1 on success (after wait), 0 on timeout.
// ------------------------------------------------------------
int flash_page_program(uint32_t address, const uint8_t *data, uint32_t size)
{
    if (size > FLASH_PAGE_SIZE)
        size = FLASH_PAGE_SIZE;

    flash_write_enable();

    flash_cs_select();
    flash_write_cmd(FLASH_CMD_PAGE_PROGRAM);
    flash_write_addr(address);
    spi_write_blocking(FLASH_SPI_INST, data, size);
    flash_cs_deselect();

    return flash_wait_busy();
}

// ------------------------------------------------------------
// Erase one 4 KB sector that contains 'address'.
// Returns 1 on success (after wait), 0 on timeout.
// ------------------------------------------------------------
int flash_sector_erase(uint32_t address)
{
    flash_write_enable();

    flash_cs_select();
    flash_write_cmd(FLASH_CMD_SECTOR_ERASE);
    flash_write_addr(address);
    flash_cs_deselect();

    return flash_wait_busy();
}

// ------------------------------------------------------------
// Fill a buffer with a specific data pattern for testing.
// Supported: "0xFF", "0x00", "0x55", "random", "incremental"
// Defaults to 0xFF if unknown.
// ------------------------------------------------------------
void generate_test_pattern(uint8_t *buffer, uint32_t size, const char *pattern_type)
{
    if (strcmp(pattern_type, "0xFF") == 0)
    {
        memset(buffer, 0xFF, size);
    }
    else if (strcmp(pattern_type, "0x00") == 0)
    {
        memset(buffer, 0x00, size);
    }
    else if (strcmp(pattern_type, "0x55") == 0)
    {
        memset(buffer, 0x55, size);
    }
    else if (strcmp(pattern_type, "random") == 0)
    {
        for (uint32_t i = 0; i < size; i++)
            buffer[i] = rand() & 0xFF;
    }
    else if (strcmp(pattern_type, "incremental") == 0)
    {
        for (uint32_t i = 0; i < size; i++)
            buffer[i] = i & 0xFF;
    }
    else
    {
        memset(buffer, 0xFF, size); // default
    }
}

// ------------------------------------------------------------
// Timed READ benchmark:
// - Allocates a temporary buffer of 'size'.
// - Reads from flash, times it, prints MB/s.
// - Returns elapsed microseconds (0 on failure).
// ------------------------------------------------------------
uint64_t benchmark_flash_read(uint32_t address, uint32_t size, const char *pattern)
{
    (void)pattern; // pattern unused for read timing only

    if (!flash_initialized)
        return 0;

    uint8_t *buffer = malloc(size);
    if (!buffer)
        return 0;

    printf("📖 Reading %d bytes from 0x%06X... ", size, address);

    uint64_t start_time = get_time_us();
    flash_read_data(address, buffer, size);
    uint64_t end_time   = get_time_us();

    uint64_t elapsed = end_time - start_time;

    printf("%.2f ms (%.2f MB/s)\n",
           elapsed / 1000.0,
           (size / 1024.0 / 1024.0) / (elapsed / 1000000.0));

    free(buffer);
    return elapsed;
}

// ------------------------------------------------------------
// Timed PROGRAM (write) benchmark:
// - Generates requested pattern into a temp buffer.
// - Programs in page-sized chunks.
// - Returns elapsed microseconds (0 on failure).
// ------------------------------------------------------------
uint64_t benchmark_flash_program(uint32_t address, uint32_t size, const char *pattern)
{
    if (!flash_initialized)
        return 0;

    uint8_t *buffer = malloc(size);
    if (!buffer)
        return 0;

    // Build the data to be written
    generate_test_pattern(buffer, size, pattern);

    printf("#  Programming %d bytes to 0x%06X with %s... ", size, address, pattern);

    uint64_t start_time = get_time_us();

    // Program in 256-byte pages
    uint32_t remaining    = size;
    uint32_t current_addr = address;
    uint8_t *current_data = buffer;

    while (remaining > 0)
    {
        uint32_t chunk_size = (remaining > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remaining;
        flash_page_program(current_addr, current_data, chunk_size);

        current_addr += chunk_size;
        current_data += chunk_size;
        remaining    -= chunk_size;
    }

    uint64_t end_time = get_time_us();
    uint64_t elapsed  = end_time - start_time;

    printf("%.2f ms (%.2f MB/s)\n",
           elapsed / 1000.0,
           (size / 1024.0 / 1024.0) / (elapsed / 1000000.0));

    free(buffer);
    return elapsed;
}

// ------------------------------------------------------------
// Timed ERASE benchmark:
// - Erases in 4 KB sector steps across 'size' bytes.
// - Returns elapsed microseconds (0 on failure).
// ------------------------------------------------------------
uint64_t benchmark_flash_erase(uint32_t address, uint32_t size)
{
    if (!flash_initialized)
        return 0;

    printf("🗑️  Erasing %d bytes from 0x%06X... ", size, address);

    uint64_t start_time = get_time_us();

    uint32_t remaining    = size;
    uint32_t current_addr = address;

    // Walk sector-by-sector
    while (remaining > 0)
    {
        flash_sector_erase(current_addr);
        current_addr += FLASH_SECTOR_SIZE;
        remaining     = (remaining > FLASH_SECTOR_SIZE) ? (remaining - FLASH_SECTOR_SIZE) : 0;
    }

    uint64_t end_time = get_time_us();
    uint64_t elapsed  = end_time - start_time;

    printf("%.2f ms\n", elapsed / 1000.0);

    return elapsed;
}
