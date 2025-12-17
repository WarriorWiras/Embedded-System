#include "bench_read.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "flash_benchmark.h" // flash_* and benchmark_* APIs
#include "sd_card.h"         // RESULTS.CSV I/O

/* Use a single, consistent unit string for microseconds.
   If your terminal still garbles it, compile with -DASCII_UNITS to fall back. */
/* --- Unit strings (ASCII-safe fallback via -DASCII_UNITS) --- */
#ifdef ASCII_UNITS
#define UNIT_US "us"
#else
#define UNIT_US "\xC2\xB5" \
                "s" /* "µs" in UTF-8 */
#endif

// -----------------------------------------------------------------------------
// Config
// -----------------------------------------------------------------------------
#define CSV_FILENAME "RESULTS.CSV"
#define N_ITERS 100

// Test sizes
static const struct
{
    const char *label;
    uint32_t size;
} k_sizes[] = {
    {"1-byte", 1},
    {"1-page", FLASH_PAGE_SIZE},     // was 256
    {"1-sector", FLASH_SECTOR_SIZE}, // was 4096
    {"32k-block", 32 * 1024},
    {"64k-block", 64 * 1024},
};

static void print_flash_sck_banner(const char *prefix)
{
    uint32_t hz = flash_spi_get_baud_hz();
    if (hz)
    {
        double mhz = (double)hz / 1e6;
        printf("%sFlash SPI SCK: %.2f MHz\n", (prefix ? prefix : ""), mhz);
    }
    else
    {
        printf("%sFlash SPI SCK: (unknown)\n", (prefix ? prefix : ""));
    }
}

static const char *notes_for_read(const char *label, uint32_t size_bytes)
{
    static char note_buf[64];

    // Build the base tag first
    const size_t cap = flash_capacity_bytes();
    if (label)
    {
        if (!strcmp(label, "1-byte") || size_bytes == 1)
            snprintf(note_buf, sizeof note_buf, "read_bench_1_byte");
        else if (!strcmp(label, "1-page") || size_bytes == FLASH_PAGE_SIZE)
            snprintf(note_buf, sizeof note_buf, "read_bench_1_page");
        else if (!strcmp(label, "1-sector") || size_bytes == FLASH_SECTOR_SIZE)
            snprintf(note_buf, sizeof note_buf, "read_bench_1_sector");
        else if (!strcmp(label, "32k-block") || size_bytes == (32u * 1024u))
            snprintf(note_buf, sizeof note_buf, "read_bench_32k_block");
        else if (!strcmp(label, "64k-block") || size_bytes == (64u * 1024u))
            snprintf(note_buf, sizeof note_buf, "read_bench_64k_block");
        else if (!strcmp(label, "wholechip") || !strcmp(label, "whole-chip") || size_bytes == cap)
            snprintf(note_buf, sizeof note_buf, "read_bench_whole_chip");
        else
            snprintf(note_buf, sizeof note_buf, "read_bench_%u_bytes", (unsigned)size_bytes);
    }
    else
    {
        if (size_bytes == cap)
            snprintf(note_buf, sizeof note_buf, "read_bench_whole_chip");
        else if (size_bytes == 1)
            snprintf(note_buf, sizeof note_buf, "read_bench_1_byte");
        else if (size_bytes == FLASH_PAGE_SIZE)
            snprintf(note_buf, sizeof note_buf, "read_bench_1_page");
        else if (size_bytes == FLASH_SECTOR_SIZE)
            snprintf(note_buf, sizeof note_buf, "read_bench_1_sector");
        else if (size_bytes == (32u * 1024u))
            snprintf(note_buf, sizeof note_buf, "read_bench_32k_block");
        else if (size_bytes == (64u * 1024u))
            snprintf(note_buf, sizeof note_buf, "read_bench_64k_block");
        else
            snprintf(note_buf, sizeof note_buf, "read_bench_%u_bytes", (unsigned)size_bytes);
    }

    // Append "@<MHz>" if we know it
    uint32_t hz = flash_spi_get_baud_hz();
    if (hz)
    {
        unsigned mhz = (unsigned)((hz + 500000u) / 1000000u); // nearest MHz
        size_t used = strlen(note_buf);
        if (used + 8 < sizeof(note_buf))
        {
            snprintf(note_buf + used, sizeof note_buf - used, "@%uMHz", mhz);
        }
    }
    return note_buf;
}

// -----------------------------------------------------------------------------
// Small helpers (kept minimal; no duplicates beyond what we must re-use)
// -----------------------------------------------------------------------------

// Blocking y/n prompt (waits until user presses 'y' or 'n')
static bool ask_yes_no(const char *q)
{
    printf("%s (y/n): ", q);
    fflush(stdout);
    for (;;)
    {
        int ch = getchar_timeout_us(1000 * 1000); // poll 1s
        if (ch < 0)
            continue;
        if (ch == '\r' || ch == '\n')
            continue;
        if (ch == 'y' || ch == 'Y')
        {
            puts("y");
            return true;
        }
        if (ch == 'n' || ch == 'N')
        {
            puts("n");
            return false;
        }
        // ignore others
    }
}

// Timestamp (no RTC) — same string shape you log elsewhere
static inline void make_timestamp(char *buf, size_t n)
{
    uint64_t us = to_us_since_boot(get_absolute_time());
    uint32_t s = (uint32_t)(us / 1000000ULL);
    uint32_t hh = s / 3600;
    uint32_t mm = (s % 3600) / 60;
    uint32_t ss = s % 60;
    snprintf(buf, n, "2025-09-28 %02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
}

// Minimal environmental reads (duplicated here because main.c helpers are static)
#define ADC_CONV (3.3f / (1 << 12))
#define ADC_VSYS_DIV 3.0f
#define ADC_TEMP_CH 4
#define ADC_VSYS_CH 3
#define ADC_VSYS_PIN 29
static void env_init_once(void)
{
    static bool inited = false;
    if (inited)
        return;
    adc_init();
    adc_gpio_init(ADC_VSYS_PIN);
    adc_set_temp_sensor_enabled(true);
    inited = true;
}
static inline float read_temp_C(void)
{
    env_init_once();
    adc_select_input(ADC_TEMP_CH);
    uint16_t raw = adc_read();
    float v = raw * ADC_CONV;
    return 27.0f - (v - 0.706f) / 0.001721f; // RP2040 formula
}
static inline float read_vsys_V(void)
{
    env_init_once();
    adc_select_input(ADC_VSYS_CH);
    uint16_t raw = adc_read();
    return raw * ADC_CONV * ADC_VSYS_DIV;
}

// mb/s from bytes and elapsed microseconds
static inline double mbps(uint32_t bytes, uint64_t us)
{
    if (us == 0)
        return 0.0;
    double mb = (double)bytes / (1024.0 * 1024.0);
    double s = (double)us / 1e6;
    return (s > 0.0) ? (mb / s) : 0.0;
}

// Run counter sourced from file so we don’t depend on main.c’s static
static int next_run_number(void)
{
    int total = 0, data = 0;
    if (sd_count_csv_rows(CSV_FILENAME, &total, &data) == 0)
    {
        return data + 1;
    }
    // on error, fall back
    return 1;
}

// -----------------------------------------------------------------------------
// Storage for summary stats
// -----------------------------------------------------------------------------
typedef struct
{
    const char *label;
    uint32_t size;
    uint64_t samples[N_ITERS];
    int n;
} series_t;

#define MAX_SERIES 6
static series_t g_series[MAX_SERIES];
static int g_series_count = 0;

// percentile helper (0..1), using sorted array copy
static int cmp_u64(const void *a, const void *b)
{
    const uint64_t aa = *(const uint64_t *)a, bb = *(const uint64_t *)b;
    return (aa < bb) ? -1 : (aa > bb);
}
static uint64_t pct_u64(const uint64_t *sorted, int n, double p01)
{
    if (n <= 0)
        return 0;
    if (p01 <= 0)
        return sorted[0];
    if (p01 >= 1)
        return sorted[n - 1];
    double idx = p01 * (n - 1);
    int lo = (int)floor(idx);
    int hi = (int)ceil(idx);
    if (lo == hi)
        return sorted[lo];
    double t = idx - lo;
    // linear interp
    double val = (1.0 - t) * (double)sorted[lo] + t * (double)sorted[hi];
    if (val < 0)
        val = 0;
    return (uint64_t)(val + 0.5);
}
static double mean_u64(const uint64_t *v, int n)
{
    long double acc = 0.0L;
    for (int i = 0; i < n; i++)
        acc += (long double)v[i];
    return (double)(acc / (long double)n);
}
static double stddev_sample_u64(const uint64_t *v, int n, double mean)
{
    if (n < 2)
        return 0.0;
    long double acc = 0.0L;
    for (int i = 0; i < n; i++)
    {
        long double d = (long double)v[i] - (long double)mean;
        acc += d * d;
    }
    return (double)sqrt((double)(acc / (long double)(n - 1)));
}

// -----------------------------------------------------------------------------
// Core: run one size for N_ITERS, append to CSV, collect samples
// -----------------------------------------------------------------------------
static void run_size_log_series(const char *label,
                                uint32_t size_bytes,
                                uint32_t base_addr,
                                series_t *S,
                                int *p_run_no)
{
    // Ensure JEDEC looks sane before starting (live check)
    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0)
    {
        printf("⛔ Flash not live (JEDEC unknown). Skipping %s\n", label ? label : "(unnamed)");
        return;
    }

    // Clamp requested span to device capacity (avoid overflow)
    size_t cap = flash_capacity_bytes();
    if (cap > 0)
    {
        uint64_t end = (uint64_t)base_addr + (uint64_t)size_bytes;
        if (end > cap)
        {
            size_bytes = (uint32_t)((cap > base_addr) ? (cap - base_addr) : 0);
        }
    }
    if (size_bytes == 0)
    {
        printf("⚠️  Size is 0 after clamping; skipping %s\n", label ? label : "(unnamed)");
        return;
    }

    // Prepare series bucket (for summary)
    S->label = label;
    S->size = size_bytes;
    S->n = 0;

    // Streamed read buffer (no big malloc!)
    const uint32_t CHUNK_MAX = 4096; // 4 KiB chunking
    uint32_t buf_len = (size_bytes < CHUNK_MAX) ? size_bytes : CHUNK_MAX;
    uint8_t *buf = (uint8_t *)malloc(buf_len);
    if (!buf)
    {
        // last-ditch fallback to 512 bytes
        buf_len = 512;
        buf = (uint8_t *)malloc(buf_len);
    }
    if (!buf)
    {
        printf("⛔ Unable to allocate even 512 bytes for streaming. Aborting %s\n",
               label ? label : "read");
        return;
    }

    for (int i = 0; i < N_ITERS; ++i)
    {
        // Per-iteration env snapshot for CSV
        float tempC = read_temp_C(); // °C
        float vV = read_vsys_V();    // V

        // Time the full streamed read (elapsed in microseconds)
        uint64_t t0 = time_us_64();
        uint32_t remaining = size_bytes;
        uint32_t addr = base_addr;

        while (remaining)
        {
            uint32_t this_len = (remaining > buf_len) ? buf_len : remaining;
            flash_read_data(addr, buf, this_len);
            addr += this_len;
            remaining -= this_len;
        }
        uint64_t us = time_us_64() - t0;

        if (us == 0)
        {
            printf("⚠️  Read returned 0 µs; logging as 0 and continuing\n");
        }

        // Throughput (MiB/s using 1024^2)
        double th = mbps(size_bytes, us);

        // Timestamp + notes
        char ts[32];
        make_timestamp(ts, sizeof ts);
        const char *note = notes_for_read(label, size_bytes);

        // CSV row
        char row[256];
        int len = snprintf(row, sizeof row,
                           "%s,%s,%u,0x%06X,%llu,%.6f,%d,%.2f,%.2f,%s,%s,%s",
                           jedec, "read", size_bytes, base_addr,
                           (unsigned long long)us, th,
                           (*p_run_no)++, tempC, vV,
                           "n/a", ts, note);

                           

        if (len > 0 && len < (int)sizeof row)
        {
            if (!sd_append_to_file(CSV_FILENAME, row))
            {
                printf("❌ Failed to append RESULTS.CSV; continuing\n");
            }
        }

        // Save sample for summary (µs)
        if (S->n < N_ITERS)
            S->samples[S->n++] = us;

        sleep_ms(10); // tiny spacing
    }

    free(buf);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void bench_read_run_100(bool confirm_whole_chip)
{
    // Guard rails: SD + flash must be live now (menu already mounted SD).
    if (!sd_is_mounted())
    {
        printf("⛔ SD not mounted; cannot run read suite.\n");
        return;
    }
    // Initialize flash (if caller didn’t yet)
    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0)
    {
        printf("⛔ Flash not live (JEDEC unknown). Aborting read suite.\n");
        return;
    }

    // Make sure RESULTS.CSV exists & has header
    if (!sd_file_exists(CSV_FILENAME))
    {
        if (!sd_write_file(CSV_FILENAME, NULL))
        {
            printf("❌ Cannot create RESULTS.CSV\n");
            return;
        }
    }

    // Where to log run numbers
    int run_no = next_run_number();

    // Reset series list
    g_series_count = 0;

    printf("\n=== SPI Flash READ-only benchmark (100 iterations per size) ===\n");
    printf("Logging to %s (latency in microseconds; throughput in MB/s)\n", CSV_FILENAME);
    print_flash_sck_banner("");

    // Fixed sizes first
    for (size_t i = 0; i < (sizeof k_sizes / sizeof k_sizes[0]); ++i)
    {
        if (g_series_count >= MAX_SERIES)
            break;
        printf("\n--- Running %s, %u bytes, %d iterations ---\n",
               k_sizes[i].label, k_sizes[i].size, N_ITERS);
        run_size_log_series(k_sizes[i].label, k_sizes[i].size, 0x000000,
                            &g_series[g_series_count], &run_no);
        g_series_count++;
    }

    // Optional whole-chip
    if (confirm_whole_chip)
    {
        if (ask_yes_no("\nRun WHOLE-CHIP 100x (can be very slow)?"))
        {
            size_t total = flash_capacity_bytes();
            if (total > 0 && g_series_count < MAX_SERIES)
            {
                printf("\n--- Running whole-chip, %lu bytes, %d iterations ---\n",
                       (unsigned long)total, N_ITERS);
                run_size_log_series("whole-chip", (uint32_t)total, 0x000000,
                                    &g_series[g_series_count], &run_no);
                g_series_count++;
            }
            else
            {
                printf("⚠️  Whole-chip size unavailable; skipping.\n");
            }
        }
        else
        {
            printf("↩️  Whole-chip run skipped by user.\n");
        }
    }
}

void bench_read_print_summary(void)
{
    if (g_series_count == 0)
    {
        printf("\n(no recent benchmark data to summarize — run 'r' first)\n");
        return;
    }

    printf("\n=== READ-only benchmark summary ===\n");
    print_flash_sck_banner("");
    printf("(latency: microseconds  |  throughput: MB/s (from avg latency))\n");

    for (int s = 0; s < g_series_count; ++s)
    {
        series_t *S = &g_series[s];
        if (S->n == 0)
            continue;

        uint64_t sorted[N_ITERS];
        for (int i = 0; i < S->n; ++i)
            sorted[i] = S->samples[i];
        qsort(sorted, S->n, sizeof(sorted[0]), cmp_u64);

        double avg_us = mean_u64(S->samples, S->n);
        double sd_us = stddev_sample_u64(S->samples, S->n, avg_us);
        uint64_t p25 = pct_u64(sorted, S->n, 0.25);
        uint64_t p50 = pct_u64(sorted, S->n, 0.50);
        uint64_t p75 = pct_u64(sorted, S->n, 0.75);
        uint64_t vmin = sorted[0];
        uint64_t vmax = sorted[S->n - 1];

        printf("\n--- Block size: %s (%u bytes) ---\n", S->label, S->size);

        printf("Number of samples           = %d\n", S->n);
        printf("Average latency             = %.1f %s\n", avg_us, UNIT_US);
        printf("25th percentile latency     = %llu %s\n",
            (unsigned long long)p25, UNIT_US);
        printf("Median latency (50th pct)   = %llu %s\n",
            (unsigned long long)p50, UNIT_US);
        printf("75th percentile latency     = %llu %s\n",
            (unsigned long long)p75, UNIT_US);

        printf("Minimum latency             = %llu %s\n",
            (unsigned long long)vmin, UNIT_US);
        printf("Maximum latency             = %llu %s\n",
            (unsigned long long)vmax, UNIT_US);
        printf("Standard deviation          = %.2f %s\n", sd_us, UNIT_US);
        printf("Throughput (based on avg)   = %.2f MB/s\n",
            mbps(S->size, (uint64_t)llround(avg_us)));
    }
    printf("\n--- end of summary ---\n");
}

bool bench_read_has_data(void) {
    return g_series_count > 0;
}
// Map label/size -> notes value for CSV
// Derive a descriptive "notes" tag for the RESULTS.CSV row, e.g.:
//   read_bench_1_byte, read_bench_1_page, read_bench_1_sector,
//   read_bench_32k_block, read_bench_64k_block, read_bench_whole_chip (4KB at a time)
