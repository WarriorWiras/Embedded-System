#include "bench_write.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include "flash_benchmark.h" // flash_* APIs, sizes, generate_test_pattern, etc.
#include "sd_card.h"         // RESULTS.CSV I/O

/* ---------- Units (ASCII fallback like your read module) ---------- */
#ifdef ASCII_UNITS
#define UNIT_US "us"
#else
#define UNIT_US "\xC2\xB5" \
                "s" /* "µs" */
#endif

/* ---------- Config ---------- */
#define CSV_FILENAME "RESULTS.CSV"
#define N_ITERS 100

/* Test sizes (match read) */
static const struct
{
    const char *label;
    uint32_t size;
} k_sizes[] = {
    {"1-byte", 1},
    {"1-page", FLASH_PAGE_SIZE},
    {"1-sector", FLASH_SECTOR_SIZE},
    {"32k-block", 32 * 1024},
    {"64k-block", 64 * 1024},
};

/* ---------- SCK banner ---------- */
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

/* ---------- Notes builder: write_bench_* + @<MHz> + pattern ---------- */
static const char *notes_for_write(const char *label, uint32_t size_bytes, const char *pattern)
{
    static char note_buf[80];

    const size_t cap = flash_capacity_bytes();
    if (label)
    {
        if (!strcmp(label, "1-byte") || size_bytes == 1)
            snprintf(note_buf, sizeof note_buf, "write_bench_1_byte");
        else if (!strcmp(label, "1-page") || size_bytes == FLASH_PAGE_SIZE)
            snprintf(note_buf, sizeof note_buf, "write_bench_1_page");
        else if (!strcmp(label, "1-sector") || size_bytes == FLASH_SECTOR_SIZE)
            snprintf(note_buf, sizeof note_buf, "write_bench_1_sector");
        else if (!strcmp(label, "32k-block") || size_bytes == (32u * 1024u))
            snprintf(note_buf, sizeof note_buf, "write_bench_32k_block");
        else if (!strcmp(label, "64k-block") || size_bytes == (64u * 1024u))
            snprintf(note_buf, sizeof note_buf, "write_bench_64k_block");
        else if (!strcmp(label, "whole-chip") || size_bytes == cap)
            snprintf(note_buf, sizeof note_buf, "write_bench_whole_chip");
        else
            snprintf(note_buf, sizeof note_buf, "write_bench_%u_bytes", (unsigned)size_bytes);
    }
    else
    {
        if (size_bytes == cap)
            snprintf(note_buf, sizeof note_buf, "write_bench_whole_chip");
        else if (size_bytes == 1)
            snprintf(note_buf, sizeof note_buf, "write_bench_1_byte");
        else if (size_bytes == FLASH_PAGE_SIZE)
            snprintf(note_buf, sizeof note_buf, "write_bench_1_page");
        else if (size_bytes == FLASH_SECTOR_SIZE)
            snprintf(note_buf, sizeof note_buf, "write_bench_1_sector");
        else if (size_bytes == (32u * 1024u))
            snprintf(note_buf, sizeof note_buf, "write_bench_32k_block");
        else if (size_bytes == (64u * 1024u))
            snprintf(note_buf, sizeof note_buf, "write_bench_64k_block");
        else
            snprintf(note_buf, sizeof note_buf, "write_bench_%u_bytes", (unsigned)size_bytes);
    }

    uint32_t hz = flash_spi_get_baud_hz();
    if (hz)
    {
        unsigned mhz = (unsigned)((hz + 500000u) / 1000000u);
        size_t used = strlen(note_buf);
        if (used + 8 < sizeof(note_buf))
        {
            snprintf(note_buf + used, sizeof note_buf - used, "@%uMHz", mhz);
        }
    }
    if (pattern && *pattern)
    {
        size_t used = strlen(note_buf);
        if (used + 1 + strlen(pattern) + 1 < sizeof(note_buf))
        {
            snprintf(note_buf + used, sizeof note_buf - used, "_%s", pattern);
        }
    }
    return note_buf;
}

/* ---------- Small helpers (env, time, throughput, next run) ---------- */
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
    return 27.0f - (v - 0.706f) / 0.001721f;
}
static inline float read_vsys_V(void)
{
    env_init_once();
    adc_select_input(ADC_VSYS_CH);
    uint16_t raw = adc_read();
    return raw * ADC_CONV * ADC_VSYS_DIV;
}
static inline double mbps(uint32_t bytes, uint64_t us)
{
    if (!us)
        return 0.0;
    return ((double)bytes / (1024.0 * 1024.0)) / ((double)us / 1e6);
}
static int next_run_number(void)
{
    int total = 0, data = 0;
    if (sd_count_csv_rows(CSV_FILENAME, &total, &data) == 0)
        return data + 1;
    return 1;
}
static inline void make_timestamp(char *buf, size_t n)
{
    uint64_t us = to_us_since_boot(get_absolute_time());
    uint32_t s = (uint32_t)(us / 1000000ULL);
    uint32_t hh = s / 3600, mm = (s % 3600) / 60, ss = s % 60;
    snprintf(buf, n, "2025-09-28 %02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
}

/* ---------- Stats storage ---------- */
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

/* ---------- stats helpers ---------- */
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
    int lo = (int)floor(idx), hi = (int)ceil(idx);
    if (lo == hi)
        return sorted[lo];
    double t = idx - lo;
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

/* ---------- yes/no ---------- */
static bool ask_yes_no(const char *q)
{
    printf("%s (y/n): ", q);
    fflush(stdout);
    for (;;)
    {
        int ch = getchar_timeout_us(1000 * 1000);
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
    }
}

/* ---------- erase a span [addr, addr+size) sector-wise (not timed) ---------- */
static void erase_span(uint32_t base_addr, uint32_t size)
{
    uint32_t start = base_addr & ~(FLASH_SECTOR_SIZE - 1);
    uint32_t end = base_addr + size;
    while (start < end)
    {
        flash_sector_erase(start);
        start += FLASH_SECTOR_SIZE;
    }
}

/* ---------- page-program streamed (measured), pattern generated per chunk --- */
static uint64_t program_streamed_measure(uint32_t base_addr, uint32_t size, const char *pattern)
{
    uint8_t buf[FLASH_PAGE_SIZE]; // 256B typical
    uint32_t remaining = size, addr = base_addr;

    uint64_t t0 = time_us_64();

    while (remaining)
    {
        /* write at most to end-of-page each step */
        uint32_t page_off = addr & (FLASH_PAGE_SIZE - 1);
        uint32_t room = FLASH_PAGE_SIZE - page_off;
        uint32_t this_len = (remaining < room) ? remaining : room;

        generate_test_pattern(buf, this_len, pattern);
        flash_page_program(addr, buf, this_len);

        addr += this_len;
        remaining -= this_len;
    }

    return time_us_64() - t0; // elapsed µs (write only)
}

/* ---------- one size × N_ITERS with CSV logging ---------- */
static void run_size_log_series_write(const char *label,
                                      uint32_t size_bytes,
                                      uint32_t base_addr,
                                      const char *pattern,
                                      series_t *S,
                                      int *p_run_no)
{
    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0)
    {
        printf("⛔ Flash not live (JEDEC unknown). Skipping %s\n", label ? label : "(unnamed)");
        return;
    }

    /* clamp to capacity */
    size_t cap = flash_capacity_bytes();
    if (cap > 0)
    {
        uint64_t end = (uint64_t)base_addr + (uint64_t)size_bytes;
        if (end > cap)
            size_bytes = (uint32_t)((cap > base_addr) ? (cap - base_addr) : 0);
    }
    if (size_bytes == 0)
    {
        printf("⚠️  Size is 0 after clamping; skipping %s\n", label ? label : "(unnamed)");
        return;
    }

    S->label = label;
    S->size = size_bytes;
    S->n = 0;

    for (int i = 0; i < N_ITERS; ++i)
    {
        float tempC = read_temp_C();
        float vV = read_vsys_V();

        /* ERASE (not timed) so every iteration is fresh */
        erase_span(base_addr, size_bytes);

        /* WRITE (timed) */
        uint64_t us = program_streamed_measure(base_addr, size_bytes, pattern);

        if (!us)
            printf("⚠️  Program returned 0 µs; logging as 0 and continuing\n");

        double th = mbps(size_bytes, us);

        char ts[32];
        make_timestamp(ts, sizeof ts);
        const char *note = notes_for_write(label, size_bytes, pattern);

        char row[256];
        int len = snprintf(row, sizeof row,
                           "%s,%s,%u,0x%06X,%llu,%.6f,%d,%.2f,%.2f,%s,%s,%s",
                           jedec, "write", size_bytes, base_addr,
                           (unsigned long long)us, th,
                           (*p_run_no)++, tempC, vV,
                           pattern ? pattern : "n/a", ts, note);

        if (len > 0 && len < (int)sizeof row)
        {
            if (!sd_append_to_file(CSV_FILENAME, row))
            {
                printf("❌ Failed to append RESULTS.CSV; continuing\n");
            }
        }

        if (S->n < N_ITERS)
            S->samples[S->n++] = us;

        sleep_ms(10);
    }
}

/* ---------- Public: run suite ---------- */
void bench_write_run_100(bool confirm_whole_chip, const char *pattern)
{
    if (!sd_is_mounted())
    {
        printf("⛔ SD not mounted; cannot run write suite.\n");
        return;
    }
    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0)
    {
        printf("⛔ Flash not live (JEDEC unknown). Aborting write suite.\n");
        return;
    }

    if (!sd_file_exists(CSV_FILENAME))
    {
        if (!sd_write_file(CSV_FILENAME, NULL))
        {
            printf("❌ Cannot create RESULTS.CSV\n");
            return;
        }
    }

    int run_no = next_run_number();
    g_series_count = 0;

    printf("\n=== SPI Flash WRITE benchmark (100 iterations per size) ===\n");
    printf("⚠️  Each iteration ERASES the affected region, then measures PROGRAM (write) time only.\n");
    printf("Pattern: %s\n", pattern ? pattern : "n/a");
    print_flash_sck_banner("");
    printf("Logging to %s (latency in microseconds; throughput in MB/s)\n", CSV_FILENAME);

    for (size_t i = 0; i < (sizeof k_sizes / sizeof k_sizes[0]); ++i)
    {
        if (g_series_count >= MAX_SERIES)
            break;
        printf("\n--- Running %s, %u bytes, %d iterations ---\n",
               k_sizes[i].label, k_sizes[i].size, N_ITERS);
        if (!ask_yes_no("Proceed with ERASE+WRITE for this size?"))
        {
            puts("↩️  Skipped by user.");
        }
        else
        {
            run_size_log_series_write(k_sizes[i].label, k_sizes[i].size, 0x000000,
                                      pattern, &g_series[g_series_count], &run_no);
            g_series_count++;
        }
    }

    if (confirm_whole_chip)
    {
        puts("");
        if (ask_yes_no("⚠️  WHOLE-CHIP test will ERASE + WRITE the ENTIRE device 100×. Are you sure?") &&
            ask_yes_no("⚠️  REALLY sure? This can take a long time and wears the flash."))
        {
            size_t total = flash_capacity_bytes();
            if (total > 0 && g_series_count < MAX_SERIES)
            {
                printf("\n--- Running whole-chip, %lu bytes, %d iterations ---\n",
                       (unsigned long)total, N_ITERS);
                run_size_log_series_write("whole-chip", (uint32_t)total, 0x000000,
                                          pattern, &g_series[g_series_count], &run_no);
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

/* ---------- Public: print summary (split prints to avoid varargs quirk) --- */
void bench_write_print_summary(void)
{
    if (g_series_count == 0)
    {
        printf("\n(no recent WRITE benchmark data to summarize — run 'write' first)\n");
        return;
    }

    printf("\n=== WRITE benchmark summary ===\n");
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
        printf("Average program time        = %.1f %s\n", avg_us, UNIT_US);
        printf("25th percentile program time= %llu %s\n",
            (unsigned long long)p25, UNIT_US);
        printf("Median program time (50th)  = %llu %s\n",
            (unsigned long long)p50, UNIT_US);
        printf("75th percentile program time= %llu %s\n",
            (unsigned long long)p75, UNIT_US);

        printf("Minimum program time        = %llu %s\n",
            (unsigned long long)vmin, UNIT_US);
        printf("Maximum program time        = %llu %s\n",
            (unsigned long long)vmax, UNIT_US);
        printf("Standard deviation          = %.2f %s\n", sd_us, UNIT_US);
        printf("Throughput (based on avg)   = %.2f MB/s\n",
            mbps(S->size, (uint64_t)llround(avg_us)));
    }
    printf("\n--- end of summary ---\n");
}

bool bench_write_has_data(void)
{
    return g_series_count > 0;
}
