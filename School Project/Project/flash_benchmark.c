/*
 * SPI Flash Benchmark Implementation
 * High-precision timing and forensic analysis of flash memory performance
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
#include <stdbool.h>
#include "chip_db.h"

#define CHIP_DB_PRIMARY "datasheet.csv" // your chosen filename on SD root
#define CHIP_DB_FALLBACK "database.csv" // optional fallback

/* -------------------- Hardware wiring (SPI0 on GP4..GP7) -------------------*
 *   CE# -> GP5 (CS)
 *   SO  -> GP4 (MISO)
 *   SCK -> GP6 (SCK)
 *   SI  -> GP7 (MOSI)
 * -------------------------------------------------------------------------- */
#define FLASH_SPI_INST spi0
#define FLASH_CS_PIN 5
#define FLASH_SCK_PIN 6
#define FLASH_MOSI_PIN 7
#define FLASH_MISO_PIN 4

/* ------------------------------- SPI speeds -------------------------------- */
#define BAUD_INIT_HZ 100000  // very safe for bring-up
#define BAUD_ID_HZ 1000000   // robust JEDEC reading
#define BAUD_RUN_HZ 10000000 // normal operation

/* ----------------------------- JEDEC probing --------------------------------*/
#define JEDEC_MAX_BYTES 8 // oversample to slide past junk
#define JEDEC_RETRIES 4

/* --- Erase opcodes and status bits (fallbacks if header didn't define) --- */
#ifndef FLASH_CMD_READ_STATUS
#define FLASH_CMD_READ_STATUS 0x05
#endif
#ifndef FLASH_CMD_WRITE_ENABLE
#define FLASH_CMD_WRITE_ENABLE 0x06
#endif
#ifndef FLASH_CMD_SECTOR_ERASE
#define FLASH_CMD_SECTOR_ERASE 0x20 /* 4 KiB */
#endif
#ifndef FLASH_CMD_BLOCK32_ERASE
#define FLASH_CMD_BLOCK32_ERASE 0x52 /* 32 KiB (not on all parts) */
#endif
#ifndef FLASH_CMD_BLOCK64_ERASE
#define FLASH_CMD_BLOCK64_ERASE 0xD8 /* 64 KiB */
#endif
#ifndef FLASH_CMD_CHIP_ERASE
#define FLASH_CMD_CHIP_ERASE 0xC7 /* also 0x60 on some parts */
#endif
#ifndef FLASH_CMD_GLOBAL_UNPROTECT
#define FLASH_CMD_GLOBAL_UNPROTECT 0x98 /* SST26 ULBPR; ignored if N/A */
#endif

#ifndef FLASH_STATUS_BUSY
#define FLASH_STATUS_BUSY 0x01 /* WIP bit */
#endif
#ifndef FLASH_STATUS_WEL
#define FLASH_STATUS_WEL 0x02
#endif

#ifndef FLASH_SECTOR_SIZE
#define FLASH_SECTOR_SIZE 4096
#endif

#ifndef FLASH_CMD_READ_STATUS2
#define FLASH_CMD_READ_STATUS2 0x35
#endif
#ifndef FLASH_CMD_WRITE_STATUS
#define FLASH_CMD_WRITE_STATUS 0x01
#endif
#ifndef FLASH_CMD_WRITE_STATUS2
#define FLASH_CMD_WRITE_STATUS2 0x31
#endif
#ifndef FLASH_CMD_WRITE_ENABLE_SR
#define FLASH_CMD_WRITE_ENABLE_SR 0x50
#endif

#define ERASE_BENCH_BASE_ADDR 0x050000u

/* ------------------------ SPI / CS line helpers ---------------------------- */
static inline void flash_cs_select(void)
{
    gpio_put(FLASH_CS_PIN, 0);
    sleep_us(1);
}
static inline void flash_cs_deselect(void)
{
    sleep_us(1);
    gpio_put(FLASH_CS_PIN, 1);
}

static inline void flash_write_cmd(uint8_t cmd) { spi_write_blocking(FLASH_SPI_INST, &cmd, 1); }

static inline void flash_write_addr(uint32_t addr)
{
    uint8_t b[3] = {(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr};
    spi_write_blocking(FLASH_SPI_INST, b, 3);
}

/* ------------------------------- State ------------------------------------- */
static bool flash_initialized = false;
static char s_last_jedec[16] = ""; // cached "BF 26 41"
/* Track effective SPI baud for user display */
static uint32_t g_flash_spi_baud_hz = 0;
/* ------------------------- Small timing helper ----------------------------- */
static inline uint64_t get_time_us(void)
{
    return to_us_since_boot(get_absolute_time());
}

// forward decls (put these near the top of flash_benchmark.c)
static int flash_wait_wip_clear(int timeout_ms);
static int flash_do_erase_opcode(uint8_t opcode, uint32_t address, int timeout_ms);
static void flash_unprotect_vendor_aware(void);
int flash_read_jedec_id(uint8_t *manufacturer,
                        uint8_t *device_id_1,
                        uint8_t *device_id_2);


uint32_t flash_spi_get_baud_hz(void)
{
    uint32_t hw = spi_get_baudrate(FLASH_SPI_INST);
    return hw ? hw : g_flash_spi_baud_hz;
}

/* ===== ERASE HELPERS / PROTECTION / VERIFY ===== */

static inline uint8_t flash_read_status_once(void)
{
    uint8_t s = 0;
    flash_cs_select();
    flash_write_cmd(FLASH_CMD_READ_STATUS);
    spi_read_blocking(FLASH_SPI_INST, 0xFF, &s, 1);
    flash_cs_deselect();
    return s;
}

static int flash_wait_until(uint8_t mask, uint8_t value, int timeout_us)
{
    int waited = 0;
    while (waited < timeout_us)
    {
        uint8_t s = flash_read_status_once();
        if ((s & mask) == value)
            return 1;
        sleep_us(100);
        waited += 100;
    }
    return 0;
}

static int flash_wait_wel(void)
{
    /* WEL must be 1 after WREN */
    return flash_wait_until(FLASH_STATUS_WEL, FLASH_STATUS_WEL, 5000);
}

/* Write two status bytes (SR1 + SR2) – used to clear BP bits on many parts */
static void flash_write_status2(uint8_t sr1, uint8_t sr2)
{
    uint8_t buf[3];
    buf[0] = FLASH_CMD_WRITE_STATUS;
    buf[1] = sr1;
    buf[2] = sr2;

    flash_write_enable();
    flash_cs_select();
    spi_write_blocking(FLASH_SPI_INST, buf, 3);
    flash_cs_deselect();
    (void)flash_wait_busy();
}

/* Best-effort “global unprotect”.
 *  - For Microchip/SST (mfg 0xBF): use ULBPR (0x98) if available.
 *  - For others: try clearing SR1+SR2 to 0 (BP bits off).
 * This is harmless on parts that don't support 0x98 or 2-byte status writes –
 * they will simply ignore the command.
 */
static void flash_global_unprotect_if_supported(void)
{
    uint8_t m = 0, d1 = 0, d2 = 0;
    if (!flash_read_jedec_id(&m, &d1, &d2))
    {
        // No live chip; nothing to do
        return;
    }

    if (m == 0xBF)
    {
#ifdef FLASH_CMD_GLOBAL_UNPROTECT
        // Microchip / SST26: use ULBPR (0x98)
        flash_write_enable();
        flash_cs_select();
        uint8_t c = FLASH_CMD_GLOBAL_UNPROTECT; // 0x98 on SST26
        spi_write_blocking(FLASH_SPI_INST, &c, 1);
        flash_cs_deselect();
        (void)flash_wait_busy();
        sleep_ms(1);
#endif
    }
    else
    {
        // Generic: clear status register bits (BP bits, TB/SEC, etc.)
        flash_write_status2(0x00, 0x00);
    }

    sleep_ms(1);
}

// Public helper: fully unprotect flash (safe to call many times)
void flash_unprotect_all(void)
{
    // For SST26 (0xBF), this does ULBPR; for others, clears BP bits.
    flash_global_unprotect_if_supported();
    // For non-SST parts, additionally clear BP2..0 and CMP in SR1/SR2.
    flash_unprotect_vendor_aware();
}

// Read status register-2 (many flashes use 0x35 for SR2)
static inline uint8_t flash_read_status2_once(void)
{
    uint8_t s = 0;
    uint8_t cmd = FLASH_CMD_READ_STATUS2;

    flash_cs_select();
    spi_write_blocking(FLASH_SPI_INST, &cmd, 1);
    spi_read_blocking(FLASH_SPI_INST, 0xFF, &s, 1);
    flash_cs_deselect();

    return s;
}

// Stronger, vendor-aware unprotect:
// - clears BP2..BP0 (block protect bits) in SR1
// - clears CMP (complement protect) bit in SR2 (bit 6 on many parts)
// Stronger, vendor-aware unprotect for NON-SST parts:
// - clears BP2..BP0 (block protect bits) in SR1
// - clears CMP (complement protect) bit in SR2 (bit 6 on many parts)
static void flash_unprotect_vendor_aware(void)
{
    uint8_t m = 0, d1 = 0, d2 = 0;
    if (!flash_read_jedec_id(&m, &d1, &d2))
    {
        return;
    }

    // For Microchip/SST26 (0xBF), ULBPR (0x98) is the right mechanism.
    // Don't try to mess with SR1/SR2 BP bits there.
    if (m == 0xBF)
    {
        return;
    }

    uint8_t sr1_before = flash_read_status_once();
    uint8_t sr2_before = flash_read_status2_once();

    uint8_t sr1_after = (uint8_t)(sr1_before & ~(uint8_t)0x1C); // clear BP2..BP0 (bits 4:2)
    uint8_t sr2_after = (uint8_t)(sr2_before & ~(1u << 6));     // clear CMP bit

    // Nothing to do?
    if (sr1_after == sr1_before && sr2_after == sr2_before)
        return;

    // 0x50 = "enable SR write" on many devices (no-op on others)
    uint8_t cmd = FLASH_CMD_WRITE_ENABLE_SR;
    flash_cs_select();
    spi_write_blocking(FLASH_SPI_INST, &cmd, 1);
    flash_cs_deselect();
    sleep_us(5);

    // Write SR1 (0x01)
    flash_write_enable();
    cmd = FLASH_CMD_WRITE_STATUS;
    flash_cs_select();
    spi_write_blocking(FLASH_SPI_INST, &cmd, 1);
    spi_write_blocking(FLASH_SPI_INST, &sr1_after, 1);
    flash_cs_deselect();
    (void)flash_wait_busy();

    // Write SR2 (0x31)
    flash_write_enable();
    cmd = FLASH_CMD_WRITE_STATUS2;
    flash_cs_select();
    spi_write_blocking(FLASH_SPI_INST, &cmd, 1);
    spi_write_blocking(FLASH_SPI_INST, &sr2_after, 1);
    flash_cs_deselect();
    (void)flash_wait_busy();

    uint8_t sr1_new = flash_read_status_once();
    uint8_t sr2_new = flash_read_status2_once();

    if (sr1_new & 0x1C)
    {
        printf("⚠️  UNPROTECT partial: SR1 0x%02X→0x%02X, SR2 0x%02X→0x%02X\n",
               sr1_before, sr1_new, sr2_before, sr2_new);
    }
    else
    {
        printf("✅ UNPROTECT: SR1 0x%02X→0x%02X, SR2 0x%02X→0x%02X\n",
               sr1_before, sr1_new, sr2_before, sr2_new);
    }
}

/* 32K and 64K block erases (aligned). Return 1 on success. */
int flash_block32_erase(uint32_t address)
{
#ifndef FLASH_CMD_BLOCK32_ERASE
    return 0;
#else
    if (address & ((32u * 1024u) - 1))
        return 0;                                                         // must be 32K-aligned
    return flash_do_erase_opcode(FLASH_CMD_BLOCK32_ERASE, address, 2000); // 2s timeout
#endif
}

int flash_block64_erase(uint32_t address)
{
    if (address & ((64u * 1024u) - 1)) return 0; // must be 64K-aligned
    printf("[dbg] 64K erase opcode 0xD8 @0x%06X\n", address);
    int ok = flash_do_erase_opcode(FLASH_CMD_BLOCK64_ERASE, address, 4000);
    printf("[dbg] 64K erase result: %s\n", ok ? "OK (block)" : "FAIL (will fallback)");
    return ok;
}

int flash_chip_erase(void)
{
    flash_write_enable();
    if (!flash_wait_wel())
        return 0;
    flash_cs_select();
    flash_write_cmd(FLASH_CMD_CHIP_ERASE);
    flash_cs_deselect();
    return flash_wait_busy();
}

/* Quick verify that a span is erased (reads in chunks, checks 0xFF). */
static int flash_verify_erased(uint32_t addr, uint32_t size)
{
    uint8_t buf[512];
    while (size)
    {
        uint32_t n = size > sizeof buf ? (uint32_t)sizeof buf : size;
        flash_read_data(addr, buf, n);
        for (uint32_t i = 0; i < n; i++)
            if (buf[i] != 0xFF)
                return 0;
        addr += n;
        size -= n;
    }
    return 1;
}

/* Erase a span using largest legal granularity (64K→32K→4K). */
int flash_erase_span(uint32_t address, uint32_t size)
{
    size_t cap = flash_capacity_bytes();
    if (cap && (uint64_t)address + size > cap)
    {
        if (address >= cap)
            return 0;
        size = (uint32_t)(cap - address);
    }
    /* align start down to 4K boundary; extend size if needed to cover original range */
    uint32_t start = address & ~(FLASH_SECTOR_SIZE - 1);
    uint32_t end = (address + size + FLASH_SECTOR_SIZE - 1) & ~(FLASH_SECTOR_SIZE - 1);

    flash_global_unprotect_if_supported();

    uint32_t p = start;
    while (p < end)
    {
        uint32_t remain = end - p;

        /* Prefer 64K when aligned & large enough */
        if ((p % (64u * 1024u) == 0) && remain >= (64u * 1024u))
        {
            if (!flash_block64_erase(p))
                return 0;
            p += 64u * 1024u;
            continue;
        }
        /* Then 32K */
        if ((p % (32u * 1024u) == 0) && remain >= (32u * 1024u) && flash_block32_erase(p))
        {
            p += 32u * 1024u;
            continue;
        }
        /* Fallback 4K */
        if (!flash_sector_erase(p))
            return 0;
        p += FLASH_SECTOR_SIZE;
    }
    return 1;
}

/* ---------------------------- Soft reset / wake ---------------------------- */
/* Safe on most parts: 0x66 (RSTEN) -> 0x99 (RESET) -> 0xAB (POWER_UP) */
int flash_soft_reset(void)
{
    uint8_t c;

    flash_cs_select();
    c = FLASH_CMD_RESET_ENABLE;
    spi_write_blocking(FLASH_SPI_INST, &c, 1);
    flash_cs_deselect();
    sleep_us(10);
    flash_cs_select();
    c = FLASH_CMD_RESET;
    spi_write_blocking(FLASH_SPI_INST, &c, 1);
    flash_cs_deselect();
    sleep_ms(1);
    flash_cs_select();
    c = FLASH_CMD_POWER_UP;
    spi_write_blocking(FLASH_SPI_INST, &c, 1);
    flash_cs_deselect();
    sleep_ms(1);

    return 1;
}

/* ---------------------- Plausibility filter for vendor --------------------- */
static inline bool is_plausible_mfr(uint8_t m)
{
    return (m == 0xBF)    /* Microchip/SST  */
           || (m == 0xEF) /* Winbond        */
           || (m == 0xC2) /* Macronix       */
           || (m == 0x20) /* Micron         */
           || (m == 0x1F) /* Adesto/Atmel   */
           || (m == 0x9D) /* ISSI           */
           || (m == 0x34) /* GigaDevice     */
           || (m == 0x62) /* Boyamicro      */;
}

/* ------------------ Basic JEDEC read (3 bytes, no retries) ----------------- */
int flash_read_jedec_id(uint8_t *manufacturer, uint8_t *device_id_1, uint8_t *device_id_2)
{
    uint8_t id[3] = {0};

    flash_cs_select();
    flash_write_cmd(FLASH_CMD_JEDEC_ID);
    spi_read_blocking(FLASH_SPI_INST, 0xFF, id, 3);
    flash_cs_deselect();

    *manufacturer = id[0];
    *device_id_1 = id[1];
    *device_id_2 = id[2];

    /* Reject obvious garbage */
    return (id[0] != 0x00 && id[0] != 0xFF);
}

/* -------------- Robust JEDEC read (oversample + sliding window) ------------ */
static bool flash_read_jedec_robust(uint8_t *m, uint8_t *d1, uint8_t *d2)
{
    uint32_t prev = spi_get_baudrate(FLASH_SPI_INST);
    spi_set_baudrate(FLASH_SPI_INST, BAUD_ID_HZ);

    flash_cs_deselect();
    sleep_us(5);

    bool ok = false;
    for (int attempt = 0; attempt < JEDEC_RETRIES && !ok; ++attempt)
    {
        flash_soft_reset();
        sleep_ms(2); // allow power/exit reset

        uint8_t raw[JEDEC_MAX_BYTES] = {0};

        flash_cs_select();
        flash_write_cmd(FLASH_CMD_JEDEC_ID);
        spi_read_blocking(FLASH_SPI_INST, 0xFF, raw, sizeof raw);
        flash_cs_deselect();

        for (int i = 0; i <= (int)sizeof(raw) - 3; ++i)
        {
            uint8_t a = raw[i], b = raw[i + 1], c = raw[i + 2];
            if (a != 0x00 && a != 0xFF && a != 0xFE && is_plausible_mfr(a))
            {
                *m = a;
                *d1 = b;
                *d2 = c;
                ok = true;
                break;
            }
        }

        if (!ok)
            sleep_ms(1);
    }

    spi_set_baudrate(FLASH_SPI_INST, prev);
    return ok;
}

/* ----------------------- Public: format JEDEC as text ---------------------- */
void flash_get_jedec_str(char *out, size_t n)
{
    uint8_t m = 0, d1 = 0, d2 = 0;
    bool ok = false;

    if (flash_initialized)
    {
        ok = flash_read_jedec_id(&m, &d1, &d2) && m != 0x00 && m != 0xFF && m != 0xFE && is_plausible_mfr(m);

        if (!ok)
            ok = flash_read_jedec_robust(&m, &d1, &d2);
    }

    if (ok)
    {
        (void)snprintf(out, n, "%02X %02X %02X", m, d1, d2);
        out[n - 1] = '\0';
        (void)snprintf(s_last_jedec, sizeof s_last_jedec, "%02X %02X %02X", m, d1, d2);
        s_last_jedec[sizeof s_last_jedec - 1] = '\0';
    }
    else if (s_last_jedec[0])
    {
        (void)snprintf(out, n, "%s", s_last_jedec);
        out[n - 1] = '\0';
    }
    else
    {
        (void)snprintf(out, n, "No / Unknown_Flash");
        out[n - 1] = '\0';
    }
}

/* ----------------------------- Library init -------------------------------- */
int flash_benchmark_init(void)
{
    printf("🔧 Initializing Flash SPI interface...\n");

    uint32_t actual_init = spi_init(FLASH_SPI_INST, BAUD_INIT_HZ); // <— NEW (optional)
    (void)actual_init;                                             // silence unused if you don't print it
    spi_set_format(FLASH_SPI_INST, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);

    gpio_set_function(FLASH_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(FLASH_MISO_PIN, GPIO_FUNC_SPI);

    gpio_init(FLASH_CS_PIN);
    gpio_set_dir(FLASH_CS_PIN, GPIO_OUT);
    gpio_put(FLASH_CS_PIN, 1); // deselect

    sleep_ms(10);
    flash_soft_reset();

    uint8_t m = 0, d1 = 0, d2 = 0;
    if (flash_read_jedec_id(&m, &d1, &d2))
    {
        printf("✅ Flash detected: Mfg=0x%02X, Dev=0x%02X%02X\n", m, d1, d2);

        /* Set run speed and remember actual */
        uint32_t actual_run = spi_set_baudrate(FLASH_SPI_INST, BAUD_RUN_HZ); // <— CHANGED
        g_flash_spi_baud_hz = actual_run;                                    // <— NEW

        flash_initialized = true;
        // flash_global_unprotect_if_supported();
        // flash_unprotect_vendor_aware();
        flash_unprotect_all();

        (void)snprintf(s_last_jedec, sizeof s_last_jedec, "%02X %02X %02X", m, d1, d2);
        s_last_jedec[sizeof s_last_jedec - 1] = '\0';
        return 1;
    }

    printf("❌ No flash chip detected\n");
    return 0;
}

/* ------------------------- Capacity convenience ---------------------------- */
size_t flash_capacity_bytes(void)
{
    if (!flash_initialized)
        return 1 * 1024 * 1024;

    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0)
    {
        printf("⚠️  No/Unknown JEDEC; using 1 MiB fallback\n");
        return 1 * 1024 * 1024;
    }

    size_t bytes = 0;

    // Try your primary file first
    if (chipdb_lookup_capacity_bytes(CHIP_DB_PRIMARY, jedec, &bytes))
    {
        return bytes;
    }

    // Optional: try a fallback filename
    if (chipdb_lookup_capacity_bytes(CHIP_DB_FALLBACK, jedec, &bytes))
    {
        return bytes;
    }

    printf("⚠️  JEDEC %s not found in %s or %s; using 1 MiB fallback\n",
           jedec, CHIP_DB_PRIMARY, CHIP_DB_FALLBACK);
    return 1 * 1024 * 1024;
}

/* ----------------------------- Status polling ------------------------------ */
/* Legacy name used elsewhere – keep it but make it robust */
int flash_wait_busy(void)
{
    uint8_t status = 0;
    int timeout_us = 20 * 1000 * 1000; // 20s safety for chip erase; plenty for sector/block

    while (timeout_us > 0)
    {
        flash_cs_select();
        flash_write_cmd(FLASH_CMD_READ_STATUS);
        spi_read_blocking(FLASH_SPI_INST, 0xFF, &status, 1);
        flash_cs_deselect();

        if ((status & FLASH_STATUS_BUSY) == 0)
            return 1;
        sleep_us(1000);
        timeout_us -= 1000;
    }
    return 0; // timeout
}

int flash_write_enable(void)
{
    uint8_t c = FLASH_CMD_WRITE_ENABLE;
    flash_cs_select();
    spi_write_blocking(FLASH_SPI_INST, &c, 1);
    flash_cs_deselect();
    return 1;
}

/* ------------------------------ Data I/O ----------------------------------- */
int flash_read_data(uint32_t address, uint8_t *buffer, uint32_t size)
{
    flash_cs_select();
    flash_write_cmd(FLASH_CMD_READ_DATA);
    flash_write_addr(address);
    spi_read_blocking(FLASH_SPI_INST, 0xFF, buffer, size);
    flash_cs_deselect();
    return 1;
}

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

int flash_sector_erase(uint32_t address)
{
    // Always work on a 4K-aligned boundary
    address &= ~(FLASH_SECTOR_SIZE - 1u);

    // Use the same robust WREN → opcode → BUSY polling helper as 32K/64K
    // 1000 ms is plenty for a 4K sector on typical parts.
    return flash_do_erase_opcode(FLASH_CMD_SECTOR_ERASE, address, 1000);
}

/* ------------------------- Pattern generation ------------------------------ */
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
        for (uint32_t i = 0; i < size; ++i)
            buffer[i] = rand() & 0xFF;
    }
    else if (strcmp(pattern_type, "incremental") == 0)
    {
        for (uint32_t i = 0; i < size; ++i)
            buffer[i] = (uint8_t)i;
    }
    else
    {
        memset(buffer, 0xFF, size);
    }
}

/* ------------------------------- Utilities --------------------------------- */
static void dump_hex(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i)
    {
        printf("%02X%s", p[i], ((i + 1) % 16) ? " " : "\n");
    }
    if (n % 16)
        puts("");
}

int flash_dump(uint32_t address, uint32_t len)
{
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf)
    {
        puts("alloc fail");
        return 0;
    }

    int ok = flash_read_data(address, buf, len);
    if (!ok)
    {
        puts("read fail");
        free(buf);
        return 0;
    }

    printf("Data @ 0x%06X (%u bytes):\n", address, len);
    dump_hex(buf, len);
    free(buf);
    return 1;
}

/* ------------------------------ Benchmarks --------------------------------- */
uint64_t benchmark_flash_read(uint32_t address, uint32_t size, const char *pattern)
{
    (void)pattern;
    if (!flash_initialized)
        return 0;

    uint8_t *buffer = (uint8_t *)malloc(size);
    if (!buffer)
        return 0;

        printf("\n=== READ operation ===\n");
    printf("Address:   0x%06X\n", address);
    printf("Size:      %u bytes\n", size);
    printf("📖 Reading %u bytes from 0x%06X... ", size, address);

    uint64_t t0 = get_time_us();
    flash_read_data(address, buffer, size);
    uint64_t t1 = get_time_us();

    uint64_t elapsed = t1 - t0;
    double   elapsed_ms = elapsed / 1000.0;
    double   elapsed_s  = elapsed / 1000000.0;
    printf("Elapsed time: %.3f ms\n", elapsed_ms);

    if (elapsed_s > 0.0) {
        double throughput_mbps = (size / 1048576.0) / elapsed_s;  // MB/s
        printf("Throughput:   %.2f MB/s\n", throughput_mbps);
    } else {
        printf("Throughput:   N/A (elapsed time too small)\n");
    }

    free(buffer);
    return elapsed;
}

uint64_t benchmark_flash_program(uint32_t address, uint32_t size, const char *pattern)
{
    if (!flash_initialized)
        return 0;

    uint8_t *buffer = (uint8_t *)malloc(size);
    if (!buffer)
        return 0;
    generate_test_pattern(buffer, size, pattern);

        printf("\n=== PROGRAM (write) operation ===\n");
    printf("Address:   0x%06X\n", address);
    printf("Size:      %u bytes\n", size);
    printf("Pattern:   %s\n", pattern ? pattern : "(none)");

    uint64_t t0 = get_time_us();

    uint32_t remain = size;
    uint32_t addr = address;
    uint8_t *ptr = buffer;
    while (remain > 0)
    {
        uint32_t chunk = (remain > FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE : remain;
        flash_page_program(addr, ptr, chunk);
        addr += chunk;
        ptr += chunk;
        remain -= chunk;
    }

    uint64_t t1 = get_time_us();
    uint64_t elapsed = t1 - t0;
    double   elapsed_ms = elapsed / 1000.0;
    double   elapsed_s  = elapsed / 1000000.0;


    printf("Elapsed time: %.3f ms\n", elapsed_ms);

    if (elapsed_s > 0.0) {
        double throughput_mbps = (size / 1048576.0) / elapsed_s;  // MB/s
        printf("Throughput:   %.2f MB/s\n", throughput_mbps);
    } else {
        printf("Throughput:   N/A (elapsed time too small)\n");
    }
    free(buffer);
    return elapsed;
}

uint64_t benchmark_flash_erase(uint32_t address, uint32_t size)
{
    if (!flash_initialized)
        return 0;

    size_t cap = flash_capacity_bytes();
    if (cap && (uint64_t)address + size > cap)
    {
        if (size > cap)
            return 0;
        address = (uint32_t)((cap - size) & ~(FLASH_SECTOR_SIZE - 1u));
    }

    int ok = 0;
    uint64_t t0 = 0, t1 = 0;
    uint32_t verify_start = address;
    uint32_t verify_size = size;

    if (size == (64u * 1024u) && (address % (64u * 1024u) == 0))
    {
        t0 = get_time_us();
        ok = flash_block64_erase(address);
        t1 = get_time_us();
        if (!ok)
        {
            t0 = get_time_us();
            ok = 1;
            for (int i = 0; i < 16; ++i)
            {
                if (!flash_sector_erase(address + i * FLASH_SECTOR_SIZE))
                {
                    ok = 0;
                    break;
                }
            }
            t1 = get_time_us();
        }
        verify_start = address;
        verify_size = (64u * 1024u);
    }
    else if (size == (32u * 1024u) && (address % (32u * 1024u) == 0))
    {
        t0 = get_time_us();
        ok = flash_block32_erase(address);
        t1 = get_time_us();
        if (!ok)
        {
            t0 = get_time_us();
            ok = 1;
            for (int i = 0; i < 8; ++i)
            {
                if (!flash_sector_erase(address + i * FLASH_SECTOR_SIZE))
                {
                    ok = 0;
                    break;
                }
            }
            t1 = get_time_us();
        }
        verify_start = address;
        verify_size = (32u * 1024u);
    }
    else if (size == FLASH_SECTOR_SIZE && (address % FLASH_SECTOR_SIZE == 0))
    {
        t0 = get_time_us();
        ok = flash_sector_erase(address);
        t1 = get_time_us();
        verify_start = address;
        verify_size = FLASH_SECTOR_SIZE;
    }
    else
    {
        uint32_t base = address & ~(FLASH_SECTOR_SIZE - 1u);
        uint32_t end = (address + size + (FLASH_SECTOR_SIZE - 1u)) & ~(FLASH_SECTOR_SIZE - 1u);
        t0 = get_time_us();
        ok = 1;
        for (uint32_t a = base; a < end; a += FLASH_SECTOR_SIZE)
        {
            if (!flash_sector_erase(a))
            {
                ok = 0;
                break;
            }
        }
        t1 = get_time_us();
        verify_start = base;
        verify_size = end - base;
    }

    if (!ok)
        return 0;
    if (!flash_verify_erased(verify_start, verify_size))
        return 0;

    return t1 - t0;
}

/* Wait until WIP clears (BUSY=0). timeout in ms. */
static int flash_wait_wip_clear(int timeout_ms)
{
    const int step_us = 500;
    int waited_us = 0;
    while (waited_us < timeout_ms * 1000)
    {
        uint8_t s = flash_read_status_once();
        if ((s & FLASH_STATUS_BUSY) == 0)
            return 1;
        sleep_us(step_us);
        waited_us += step_us;
    }
    return 0; // timeout
}

/* Generic erase helper: WREN -> check WEL -> issue opcode+addr -> wait WIP=0 */
// Robust helper: WREN → opcode → must see WIP=1 quickly → wait clear
// Robust helper: WREN → ensure WEL → opcode+addr → wait for BUSY to clear
static int flash_do_erase_opcode(uint8_t opcode, uint32_t address, int timeout_ms)
{
    flash_write_enable();

    flash_cs_select();
    flash_write_cmd(opcode);
    flash_write_addr(address);
    flash_cs_deselect();

    // Quick sanity: did the device actually start an erase?
    int saw_busy = 0;
    for (int us = 0; us < 3000; us += 50)
    {
        uint8_t s = flash_read_status_once();
        if (s & FLASH_STATUS_BUSY)
        {
            saw_busy = 1;
            break;
        }
        sleep_us(50);
    }
    if (!saw_busy)
    {
        // command ignored (e.g. still protected)
        return 0;
    }

    return flash_wait_wip_clear(timeout_ms);
}
