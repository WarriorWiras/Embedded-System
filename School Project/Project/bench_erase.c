#include "bench_erase.h"

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "pico/time.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include "flash_benchmark.h" // flash_* APIs, sizes, generate_test_pattern()
#include "sd_card.h"         // RESULTS.CSV I/O

/* ========================== Units (ASCII fallback) ========================== */
#ifdef ASCII_UNITS
#define UNIT_US "us"
#else
#define UNIT_US "\xC2\xB5" \
                "s" /* "µs" */
#endif

/* ================================ Config =================================== */
#define CSV_FILENAME "RESULTS.CSV"
#define N_ITERS 100


/* --------------------- Bench behaviour toggles (safe defaults) --------------------- */
#ifndef LOG_PREFILL_PROGRAM_ROW
#define LOG_PREFILL_PROGRAM_ROW 0   /* 1=log a CSV row for the prefill program time */
#endif

#ifndef LOG_VERIFY_FAIL_ROW
#define LOG_VERIFY_FAIL_ROW 0   /* keep CSV pure: only erase rows */
#endif

#ifndef CLEAN_BEFORE_FIRST_ONLY
#define CLEAN_BEFORE_FIRST_ONLY 1   /* 1=only erase-clean on i==0 to reduce wear */
#endif

#ifndef VERIFY_PREFILL_STRICT
#define VERIFY_PREFILL_STRICT 1     /* 1=check written data before timing erase */
#endif

/* Compute the *physical* erase span in bytes (rounded to sector boundaries). */
static inline uint32_t compute_physical_erase_bytes(uint32_t addr, uint32_t size)
{
    uint32_t base = addr & ~(FLASH_SECTOR_SIZE - 1u);
    uint32_t end  = (addr + size + (FLASH_SECTOR_SIZE - 1u)) & ~(FLASH_SECTOR_SIZE - 1u);
    return end - base;
}

/* Verify that the span matches the pattern we *expect* after prefill.
   Returns 1 on match, 0 on mismatch. */
static int verify_span_pattern(uint32_t base_addr, uint32_t size, const char *pattern)
{
#if VERIFY_PREFILL_STRICT
    /* Read-back in chunks to avoid big allocs */
    uint8_t expect[256];
    uint8_t buf[256];
    for (uint32_t i = 0; i < sizeof expect; ++i) expect[i] = 0xFF;

    /* Prepare the expected pattern for the first chunk */
    if (strcmp(pattern, "0xFF") == 0)      memset(expect, 0xFF, sizeof expect);
    else if (strcmp(pattern, "0x00") == 0) memset(expect, 0x00, sizeof expect);
    else if (strcmp(pattern, "0x55") == 0) memset(expect, 0x55, sizeof expect);
    else if (strcmp(pattern, "incremental") == 0) {
        for (uint32_t i = 0; i < sizeof expect; ++i) expect[i] = (uint8_t)i;
    } else if (strcmp(pattern, "random") == 0) {
        /* Can't deterministically verify random; just skip strict check. */
        return 1;
    } else {
        memset(expect, 0xFF, sizeof expect);
    }

    uint32_t remaining = size, addr = base_addr;
    while (remaining) {
        uint32_t n = remaining > sizeof buf ? (uint32_t)sizeof buf : remaining;
        if (!flash_read_data(addr, buf, n)) return 0;

        /* For incremental, recompute expected window */
        if (strcmp(pattern, "incremental") == 0) {
            for (uint32_t i = 0; i < n; ++i) expect[i] = (uint8_t)((addr - base_addr) + i);
        }

        if (memcmp(buf, expect, n) != 0) return 0;
        addr += n; remaining -= n;
    }
#endif
    return 1;
}


/* Optional: distribute wear by rotating start address within a ring (bytes).
   0 = disabled (always erase the same span starting 0x000000). */
#ifndef ERASE_DISTRIBUTE_RING_BYTES
#define ERASE_DISTRIBUTE_RING_BYTES (0) /* e.g., 256*1024 to spread over 256KiB */
#endif

/* Test sizes – erase granularity is at least one sector; no 1-byte / 1-page. */
/* Test sizes
 * Note: physically, erases always happen in whole sectors (at least 4 KiB).
 * For the "1-byte" and "1-page" cases we still erase the full sector that
 * contains that region, but we log the logical size (1 or page size) so the
 * CSV stays aligned with the READ / WRITE tests.
 */
static const struct
{
    const char *label;
    uint32_t size;
} k_sizes[] = {
    {"1-byte", 1},                   // erases the sector containing this byte
    {"1-page", FLASH_PAGE_SIZE},     // erases the sector containing this page
    {"1-sector", FLASH_SECTOR_SIZE}, // typically 4 KiB
    {"32k-block", 32 * 1024},
    {"64k-block", 64 * 1024},
};

/* Choose a safe base address for each logical erase size.
 * We avoid sector 0 (0x000000) because it may be factory-locked or
 * boot-protected on some devices.
 */
#define ERASE_BASE_ADDR 0x050000u
static uint32_t erase_base_for_label(const char *label)
{
    if (label && !strcmp(label, "whole-chip"))
    {
        // Whole-chip erase really does the entire device
        return 0x000000u;
    }

    // All other sizes (1-byte, 1-page, 1-sector, 32k-block, 64k-block)
    // start at the same safe base.
    return ERASE_BASE_ADDR;
}


/* ============================ SPI SCK banner =============================== */
static void print_flash_sck_banner(const char *prefix)
{
    uint32_t hz = flash_spi_get_baud_hz();
    if (hz)
    {
        double mhz = (double)hz / 1e6;
        printf("%sFlash SPI SCK: %.2f MHz\n", prefix ? prefix : "", mhz);
    }
    else
    {
        printf("%sFlash SPI SCK: (unknown)\n", prefix ? prefix : "");
    }
}

/* ============================= Notes builder =============================== */
static const char *notes_for_erase(const char *label, uint32_t size_bytes, bool prefilled)
{
    static char note_buf[96];
    const size_t cap = flash_capacity_bytes();

    /* base tag */
    if (label)
    {
        if (!strcmp(label, "1-sector") || size_bytes == FLASH_SECTOR_SIZE)
            snprintf(note_buf, sizeof note_buf, "erase_bench_1_sector");
        else if (!strcmp(label, "32k-block") || size_bytes == (32u * 1024u))
            snprintf(note_buf, sizeof note_buf, "erase_bench_32k_block");
        else if (!strcmp(label, "64k-block") || size_bytes == (64u * 1024u))
            snprintf(note_buf, sizeof note_buf, "erase_bench_64k_block");
        else if (!strcmp(label, "whole-chip") || size_bytes == cap)
            snprintf(note_buf, sizeof note_buf, "erase_bench_whole_chip");
        else
            snprintf(note_buf, sizeof note_buf, "erase_bench_%u_bytes", (unsigned)size_bytes);
    }
    else
    {
        if (size_bytes == cap)
            snprintf(note_buf, sizeof note_buf, "erase_bench_whole_chip");
        else if (size_bytes == FLASH_SECTOR_SIZE)
            snprintf(note_buf, sizeof note_buf, "erase_bench_1_sector");
        else if (size_bytes == (32u * 1024u))
            snprintf(note_buf, sizeof note_buf, "erase_bench_32k_block");
        else if (size_bytes == (64u * 1024u))
            snprintf(note_buf, sizeof note_buf, "erase_bench_64k_block");
        else
            snprintf(note_buf, sizeof note_buf, "erase_bench_%u_bytes", (unsigned)size_bytes);
    }

    /* append suffixes */
    size_t used = strlen(note_buf);
    if (used + 16 < sizeof(note_buf))
    {
        snprintf(note_buf + used, sizeof note_buf - used, "%s", prefilled ? "_prefilled" : "");
    }

    uint32_t hz = flash_spi_get_baud_hz();
    if (hz)
    {
        unsigned mhz = (unsigned)((hz + 500000u) / 1000000u);
        used = strlen(note_buf);
        if (used + 8 < sizeof(note_buf))
        {
            snprintf(note_buf + used, sizeof note_buf - used, "@%uMHz", mhz);
        }
    }
    return note_buf;
}

/* ===================== Env + small helpers (same style) ==================== */
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
    uint16_t r = adc_read();
    float v = r * ADC_CONV;
    return 27.0f - (v - 0.706f) / 0.001721f;
}
static inline float read_vsys_V(void)
{
    env_init_once();
    adc_select_input(ADC_VSYS_CH);
    uint16_t r = adc_read();
    return r * ADC_CONV * ADC_VSYS_DIV;
}

static inline double mbps(uint32_t bytes, uint64_t us)
{
    if (!us)
        return 0.0;
    return ((double)bytes / (1024.0 * 1024.0)) / ((double)us / 1e6);
}

static inline void make_timestamp(char *buf, size_t n)
{
    uint64_t us = to_us_since_boot(get_absolute_time());
    uint32_t s = (uint32_t)(us / 1000000ULL);
    uint32_t hh = s / 3600, mm = (s % 3600) / 60, ss = s % 60;
    snprintf(buf, n, "2025-09-28 %02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
}

/* ============================ Stats containers ============================= */
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

/* ============================== Stats helpers ============================== */
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

/* =============================== UI helper ================================= */
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

/* ===================== Prefill (program) the span (untimed) ================ */
static void prefill_span(uint32_t base_addr, uint32_t size, const char *pattern)
{
    uint32_t remaining = size;
    uint32_t addr = base_addr;
    uint8_t buf[FLASH_PAGE_SIZE];

    while (remaining)
    {
        uint32_t page_off = (addr & (FLASH_PAGE_SIZE - 1));
        uint32_t room = FLASH_PAGE_SIZE - page_off;
        uint32_t this_len = (remaining < room) ? remaining : room;

        generate_test_pattern(buf, this_len, pattern);
        flash_page_program(addr, buf, this_len);

        addr += this_len;
        remaining -= this_len;
    }
}

/* ============ One size × N_ITERS: prefill -> timed erase -> log ============ */
/* ============ One size × N_ITERS: prefill -> timed erase -> log ============ */
static void run_size_log_series_erase(const char *label,
                                      uint32_t size_bytes,
                                      uint32_t base_addr,
                                      series_t *S,
                                      int *p_run_no)
{
    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0) {
        printf("⛔ Flash not live (JEDEC unknown). Skipping %s\n", label ? label : "(unnamed)");
        return;
    }

    /* Clamp + align to sector boundary */
    size_t cap = flash_capacity_bytes();
    if (cap > 0) {
        if ((uint64_t)base_addr + (uint64_t)size_bytes > cap) {
            size_bytes = (uint32_t)((cap > base_addr) ? (cap - base_addr) : 0);
        }
    }
    if (size_bytes == 0) {
        printf("⚠️  Size is 0 after clamping; skipping %s\n", label ? label : "(unnamed)");
        return;
    }
    base_addr &= ~(FLASH_SECTOR_SIZE - 1); /* align start */

    S->label = label;
    S->size  = size_bytes;
    S->n     = 0;

    /* Pattern we write BEFORE each erase so the erase has real work to do */
    const char *prefill_pattern = "0x55"; // toggle-y, easy to see in dumps

    for (int i = 0; i < N_ITERS; ++i) {
        flash_unprotect_all();
        float tempC = read_temp_C();
        float vV    = read_vsys_V();

        uint32_t iter_base = base_addr;
#if ERASE_DISTRIBUTE_RING_BYTES
        {
            const uint32_t ring = (ERASE_DISTRIBUTE_RING_BYTES / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;
            if (ring && ring >= FLASH_SECTOR_SIZE) {
                uint32_t hop = (i % (ring / FLASH_SECTOR_SIZE)) * FLASH_SECTOR_SIZE;
                uint32_t safe_end = (uint32_t)((cap > 0) ? (cap - size_bytes) : 0xFFFFFFFFu);
                uint32_t cand = base_addr + hop;
                iter_base = (cand <= safe_end) ? cand : base_addr;
            }
        }
#endif

        printf("[erase] %s iter %d/%d at 0x%06X, logical=%u bytes\n",
               label ? label : "?", i + 1, N_ITERS, iter_base, size_bytes);

        /* Optional clean-erase only on the first iteration to reduce wear */
#if CLEAN_BEFORE_FIRST_ONLY
        if (i == 0) (void)flash_erase_span(iter_base, size_bytes);
#else
        (void)flash_erase_span(iter_base, size_bytes);
#endif

        /* 1) Program prefill (either timed row, or untimed helper) */
        uint64_t us_prog = 0;
#if LOG_PREFILL_PROGRAM_ROW
        us_prog = benchmark_flash_program(iter_base, size_bytes, prefill_pattern);
        if (!us_prog) {
            printf("⚠️  Prefill program returned 0 µs; addr=0x%06X size=%u\n", iter_base, size_bytes);
        }
#else
        prefill_span(iter_base, size_bytes, prefill_pattern);
#endif

        /* Optional read-back verify of the prefill */
#if VERIFY_PREFILL_STRICT
        if (!verify_span_pattern(iter_base, size_bytes, prefill_pattern)) {
            printf("❌ Prefill verify failed @0x%06X (size=%u). Skipping erase.\n", iter_base, size_bytes);

#if LOG_VERIFY_FAIL_ROW
/* Log a diagnostic CSV row for troubleshooting */
char ts_bad[32]; make_timestamp(ts_bad, sizeof ts_bad);
const char *note_bad = "prefill_verify_failed";
char row_bad[256];
int len_bad = snprintf(row_bad, sizeof row_bad,
    "%s,%s,%u,0x%06X,%llu,%.6f,%d,%.2f,%.2f,%s,%s,%s",
    jedec, "program_verify_fail", size_bytes, iter_base,
    (unsigned long long)0, 0.0, (*p_run_no)++, tempC, vV,
    prefill_pattern, ts_bad, note_bad);
if (len_bad > 0 && len_bad < (int)sizeof row_bad)
    (void)sd_append_to_file(CSV_FILENAME, row_bad);
#endif


            /* Try to recover the region so the next iteration can proceed */
            (void)flash_erase_span(iter_base, size_bytes);
            sleep_ms(10);
            continue;
        }
#endif

#if LOG_PREFILL_PROGRAM_ROW
        /* Log the prefill PROGRAM row */
        {
            double th_prog = mbps(size_bytes, us_prog);
            char ts_prog[32]; make_timestamp(ts_prog, sizeof ts_prog);
            char note_prog[96]; snprintf(note_prog, sizeof note_prog, "%s_prefill", notes_for_erase(label, size_bytes, true));
            char row_prog[256];
            int lenp = snprintf(row_prog, sizeof row_prog,
                                "%s,%s,%u,0x%06X,%llu,%.6f,%d,%.2f,%.2f,%s,%s,%s",
                                jedec, "program", size_bytes, iter_base,
                                (unsigned long long)us_prog, th_prog,
                                (*p_run_no)++, tempC, vV,
                                prefill_pattern, ts_prog, note_prog);
            if (lenp > 0 && lenp < (int)sizeof row_prog)
                (void)sd_append_to_file(CSV_FILENAME, row_prog);
        }
#endif

        /* 2) Timed ERASE of the same region (throughput uses *physical* bytes) */
        uint32_t phys_bytes = compute_physical_erase_bytes(iter_base, size_bytes);
        uint64_t us = benchmark_flash_erase(iter_base, size_bytes);
        if (!us) {
            printf("⚠️  Erase returned 0 µs; size=%u bytes, addr=0x%06X (protection? unsupported opcode?)\n",
                   size_bytes, iter_base);
        }
        double th_erase = mbps(phys_bytes, us);
        if (!us) th_erase = 0.0;

        char ts[32]; make_timestamp(ts, sizeof ts);
        const char *note = notes_for_erase(label, size_bytes, /*prefilled=*/true);

        char row[256];
        int len = snprintf(row, sizeof row,
                           "%s,%s,%u,0x%06X,%llu,%.6f,%d,%.2f,%.2f,%s,%s,%s",
                           jedec, "erase", size_bytes, iter_base,
                           (unsigned long long)us, th_erase,
                           (*p_run_no)++, tempC, vV,
                           prefill_pattern, ts, note);

        if (len > 0 && len < (int)sizeof row) {
            if (!sd_append_to_file(CSV_FILENAME, row))
                printf("❌ Failed to append RESULTS.CSV; continuing\n");
        }

        if (S->n < N_ITERS) S->samples[S->n++] = us;
        sleep_ms(10);
    }
}


/* ============================ Public: run suite ============================ */
void bench_erase_run_100(bool confirm_whole_chip)
{
    if (!sd_is_mounted())
    {
        printf("⛔ SD not mounted; cannot run erase suite.\n");
        return;
    }
    char jedec[24] = {0};
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!jedec[0] || strcmp(jedec, "No / Unknown_Flash") == 0)
    {
        printf("⛔ Flash not live (JEDEC unknown). Aborting erase suite.\n");
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

    int total = 0, data = 0;
    (void)sd_count_csv_rows(CSV_FILENAME, &total, &data);
    int run_no = data + 1;

    g_series_count = 0;

    printf("\n=== SPI Flash ERASE benchmark (100 iterations per size) ===\n");
    printf("Flow per iteration: program test pattern (untimed) ➜ time ERASE only.\n");
    print_flash_sck_banner("");
    printf("Logging to %s (latency in microseconds; throughput = bytes erased per second)\n", CSV_FILENAME);

    for (size_t i = 0; i < sizeof k_sizes / sizeof k_sizes[0]; ++i)
    {
        if (g_series_count >= MAX_SERIES)
            break;
        printf("\n--- Running %s, %u bytes, %d iterations ---\n",
               k_sizes[i].label, k_sizes[i].size, N_ITERS);
        if (!ask_yes_no("Proceed with prefill + ERASE for this size?"))
        {
            puts("↩️  Skipped by user.");
        }
        else
        {
        uint32_t base = erase_base_for_label(k_sizes[i].label);
        run_size_log_series_erase(k_sizes[i].label,
                                  k_sizes[i].size,
                                  base,
                                  &g_series[g_series_count],
                                  &run_no);
            g_series_count++;
        }
    }

    if (confirm_whole_chip)
    {
        puts("");
        if (ask_yes_no("⚠️  WHOLE-CHIP ERASE x100 will wear the flash. Are you sure?") && ask_yes_no("⚠️  REALLY sure? This can take a very long time."))
        {
            size_t total_bytes = flash_capacity_bytes();
            if (total_bytes > 0 && g_series_count < MAX_SERIES)
            {
                printf("\n--- Running whole-chip, %lu bytes, %d iterations ---\n",
                       (unsigned long)total_bytes, N_ITERS);
                run_size_log_series_erase("whole-chip", (uint32_t)total_bytes, 0x000000,
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

/* ============================ Public: summary ============================== */
void bench_erase_print_summary(void)
{
    if (g_series_count == 0)
    {
        printf("\n(no recent ERASE benchmark data to summarize — run 'erase' first)\n");
        return;
    }

    printf("\n=== ERASE benchmark summary ===\n");
    print_flash_sck_banner("");
    printf("(latency: microseconds  |  throughput: MB/s (bytes erased / time))\n");

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

        printf("\n--- Erase size: %s (%u bytes) ---\n", S->label, S->size);

        printf("Number of samples           = %d\n", S->n);
        printf("Average erase time          = %.1f %s\n", avg_us, UNIT_US);
        printf("25th percentile erase time  = %llu %s\n",
            (unsigned long long)p25, UNIT_US);
        printf("Median erase time (50th)    = %llu %s\n",
            (unsigned long long)p50, UNIT_US);
        printf("75th percentile erase time  = %llu %s\n",
            (unsigned long long)p75, UNIT_US);

        printf("Minimum erase time          = %llu %s\n",
            (unsigned long long)vmin, UNIT_US);
        printf("Maximum erase time          = %llu %s\n",
            (unsigned long long)vmax, UNIT_US);
        printf("Standard deviation          = %.2f %s\n", sd_us, UNIT_US);
        printf("Throughput (bytes/time avg) = %.2f MB/s\n",
            mbps(S->size, (uint64_t)llround(avg_us)));
    }
    printf("\n--- end of summary ---\n");
}

bool bench_erase_has_data(void)
{
    return g_series_count > 0;
}
