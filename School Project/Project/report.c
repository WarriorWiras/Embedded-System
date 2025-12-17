// report.c — transposed report: columns = {read,write,erase}, rows = metrics/sizes
#include "report.h"

#include "pico/stdlib.h"
#include "pico/time.h"
#include "fatfs/ff.h"

#include "flash_benchmark.h" // flash_spi_get_baud_hz(), flash_get_jedec_str()
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>

#define DB_FILENAME "datasheet.csv"
#define RESULTS_FILENAME "RESULTS.CSV"
#define REPORT_FILENAME "report.csv"

#define MAX_DB_ROWS 512
#define MAX_LINE 512
#define NA_STR "NA"
#define REPORT_READ_MEAN_FROM_AVG_LATENCY 1

static void write_three_cols(FIL *rf, const char *title, const char *R, const char *W, const char *E);
static void f3_or_na(char *out, size_t n, float v);
static void f2_or_na(char *out, size_t n, float v);
static void append_token(char *dst, size_t n, const char *tok);


#ifndef PAGE_SIZE_BYTES
#define PAGE_SIZE_BYTES 256u
#endif

/* --------------------- Optional weak feature gates --------------------- */
__attribute__((weak)) int report_enable_erase(void) { return 1; }
__attribute__((weak)) int report_enable_prog(void) { return 1; }
__attribute__((weak)) int report_enable_read(void) { return 1; }

/* ----------------------------- DB schema ------------------------------- */
typedef struct
{
    char jedec_norm[7]; // "BF2641"
    char chip_model[64];
    char company[48];
    char family[48];
    int capacity_mbit; // -1 if N/A

    // Datasheet timing / speed
    float typ_4k_ms;   // typical 4KB sector erase (ms)
    float typ_32k_ms;  // typical 32KB block erase (ms)
    float typ_64k_ms;  // typical 64KB block erase (ms)
    float typ_page_ms; // typical page program (per 256B page) (ms)
    float read50_MBps; // optional: MB/s @ 50MHz (if present in CSV)
} db_row_t;

/* --------------------------- Groups / Sizes ---------------------------- */
typedef enum
{
    G_1B = 0,
    G_256B,
    G_4K,
    G_32K,
    G_64K,
    G_WHOLE,
    G_COUNT
} group_t;

static const uint32_t GROUP_BYTES[G_COUNT] = {
    1u, 256u, 4096u, 32768u, 65536u, 0u // WHOLE=0 (computed from capacity)
};

// Write one pivot row: title,read,write,erase
static void write_pivot_row(FIL *rf, const char *title,
                            const char *readv, const char *writev, const char *erasev)
{
    char line[512];
    UINT bw;
    int n = snprintf(line, sizeof line, "%s,%s,%s,%s\n",
                     title, readv ? readv : "NA",
                     writev ? writev : "NA",
                     erasev ? erasev : "NA");
    if (n > 0)
        f_write(rf, line, (UINT)n, &bw);
}

static const char *group_suffix(group_t g)
{
    switch (g)
    {
    case G_1B:
        return "1B";
    case G_256B:
        return "256B";
    case G_4K:
        return "4096B";
    case G_32K:
        return "32768B";
    case G_64K:
        return "65536B";
    case G_WHOLE:
        return "WHOLE";
    default:
        return "?";
    }
}

/* --------------------------- Small utilities --------------------------- */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        ++i;
    if (i)
        memmove(s, s + i, strlen(s + i) + 1);
}
static void upper_str(char *s)
{
    for (; *s; ++s)
        *s = (char)toupper((unsigned char)*s);
}
static int split_fields(char *line, char *out[], int max_fields)
{
    char *sep = (strchr(line, ',') ? "," : "\t");
    int n = 0;
    char *tok = strtok(line, sep);
    while (tok && n < max_fields)
    {
        out[n++] = tok;
        tok = strtok(NULL, sep);
    }
    return n;
}
static float parse_float_or(const char *s, float fallback)
{
    if (!s || !*s)
        return fallback;
    char *end = 0;
    float v = strtof(s, &end);
    return (end == s ? fallback : v);
}
static int parse_int_or(const char *s, int fallback)
{
    if (!s || !*s)
        return fallback;
    char *end = 0;
    long v = strtol(s, &end, 10);
    return (end == s ? fallback : (int)v);
}
static void normalize_jedec(const char *in, char out6[7])
{
    char tmp[16] = {0};
    size_t w = 0;
    for (size_t i = 0; in[i] && w < 6; ++i)
    {
        char c = in[i];
        if (c == 'x' || c == 'X')
            continue;
        if (isxdigit((unsigned char)c))
            tmp[w++] = (char)toupper((unsigned char)c);
    }
    tmp[w] = 0;
    if (strlen(tmp) == 6)
        memcpy(out6, tmp, 7);
    else
        out6[0] = 0;
}
static int fatfs_gets(char *buf, int len, FIL *fp)
{
    if (len <= 1)
        return 0;
    UINT br = 0;
    int i = 0;
    char ch;
    for (;;)
    {
        FRESULT fr = f_read(fp, &ch, 1, &br);
        if (fr != FR_OK || br == 0)
            break;
        if (i < len - 1)
            buf[i++] = ch;
        if (ch == '\n')
            break;
    }
    buf[i] = 0;
    return (i > 0);
}

/* ---------------------------- Statistics -------------------------------- */
typedef struct
{
    int n;
    float mean, p25, p50, p75, minv, maxv, stddev;
} stats_t;

static int cmp_float_asc(const void *a, const void *b)
{
    const float *x = (const float *)a, *y = (const float *)b;
    return (*x < *y) ? -1 : (*x > *y) ? 1
                                      : 0;
}
static int safe_copy_sort(const float *src, int n, float **dst_out)
{
    if (n <= 0)
    {
        *dst_out = NULL;
        return 0;
    }
    float *v = (float *)malloc(n * sizeof(float));
    if (!v)
    {
        *dst_out = NULL;
        return 0;
    }
    memcpy(v, src, n * sizeof(float));
    qsort(v, n, sizeof(float), cmp_float_asc);
    *dst_out = v;
    return n;
}
static float percentile_sorted(const float *v, int n, float q)
{
    if (n <= 0)
        return NAN;
    if (q <= 0)
        return v[0];
    if (q >= 1)
        return v[n - 1];
    float pos = q * (n - 1);
    int i = (int)floorf(pos);
    int j = (int)ceilf(pos);
    float t = pos - i;
    return (1.0f - t) * v[i] + t * v[j];
}
static void calc_stats_from_vec(const float *src, int n, stats_t *S)
{
    S->n = n;
    if (n <= 0)
    {
        S->mean = S->p25 = S->p50 = S->p75 = S->minv = S->maxv = S->stddev = NAN;
        return;
    }
    float sum = 0.0f, mn = src[0], mx = src[0];
    for (int i = 0; i < n; i++)
    {
        float v = src[i];
        sum += v;
        if (v < mn)
            mn = v;
        if (v > mx)
            mx = v;
    }
    S->mean = sum / n;

    float *sorted = NULL;
    int m = safe_copy_sort(src, n, &sorted);
    if (m > 0)
    {
        S->p25 = percentile_sorted(sorted, m, 0.25f);
        S->p50 = percentile_sorted(sorted, m, 0.50f);
        S->p75 = percentile_sorted(sorted, m, 0.75f);
        free(sorted);
    }
    else
    {
        S->p25 = S->p50 = S->p75 = NAN;
    }
    S->minv = mn;
    S->maxv = mx;

    // population stddev
    float var = 0.0f;
    for (int i = 0; i < n; i++)
    {
        float d = src[i] - S->mean;
        var += d * d;
    }
    var /= n;
    S->stddev = sqrtf(var);
}

/* --------------------------- Aggregation ------------------------------- */
typedef struct
{
    stats_t s[G_COUNT]; // per size group
} section_stats_t;

typedef struct
{
    float sck_MHz;
    section_stats_t read_s;      // read values in MB/s (per-sample)
    section_stats_t write_s;     // write values in ms/op
    section_stats_t erase_s;     // erase values in ms/op
    section_stats_t read_lat_ms; // NEW: read latency stats in ms (per-sample)
    float read_mean_us[G_COUNT]; // kept: average latency (µs) per size group
} agg_t;

typedef struct
{
    float *v;
    int n, cap;
} vec_t;

static void vec_push(vec_t *V, float x)
{
    if (V->n == V->cap)
    {
        int nc = V->cap ? V->cap * 2 : 32;
        V->v = (float *)realloc(V->v, nc * sizeof(float));
        V->cap = nc;
    }
    V->v[V->n++] = x;
}

static group_t classify_group(uint32_t bytes, uint32_t whole_bytes)
{
    if (bytes == 1u)
        return G_1B;
    if (bytes == 256u)
        return G_256B;
    if (bytes == 4096u)
        return G_4K;
    if (bytes == 32768u)
        return G_32K;
    if (bytes == 65536u)
        return G_64K;
    if (whole_bytes && bytes == whole_bytes)
        return G_WHOLE;
    return (group_t)(-1);
}

/* RESULTS.CSV columns assumed:
   0: JEDEC, 1: op(read|program|write|erase), 2: size(bytes), 3: addr, 4: elapsed_us, 5: throughput_MBps, ...
*/
static void collect_aggregates(agg_t *A, uint32_t capacity_bytes)
{
    memset(A, 0, sizeof(*A));
    A->sck_MHz = flash_spi_get_baud_hz() / 1e6f;

    vec_t read_v[G_COUNT] = {0};
    vec_t write_v[G_COUNT] = {0};
    vec_t erase_v[G_COUNT] = {0};
    vec_t read_us[G_COUNT] = {0};     // per-sample READ latency in microseconds
    vec_t read_lat_ms[G_COUNT] = {0}; // NEW: per-sample READ latency in milliseconds

    FIL f;
    if (f_open(&f, RESULTS_FILENAME, FA_READ) != FR_OK)
    {
        for (int g = 0; g < G_COUNT; ++g)
        {
            calc_stats_from_vec(NULL, 0, &A->read_s.s[g]);
            calc_stats_from_vec(NULL, 0, &A->write_s.s[g]);
            calc_stats_from_vec(NULL, 0, &A->erase_s.s[g]);
        }
        return;
    }

    char line[MAX_LINE];
    while (fatfs_gets(line, sizeof line, &f))
    {
        trim(line);
        if (!line[0])
            continue;

        char work[MAX_LINE];
        strncpy(work, line, sizeof work);
        work[sizeof work - 1] = 0;
        char *flds[16];
        int nf = 0;
        char *tok = strtok(work, ",");
        while (tok && nf < 16)
        {
            flds[nf++] = tok;
            tok = strtok(NULL, ",");
        }
        if (nf < 6)
            continue;

        const char *op = flds[1];
        uint32_t size = (uint32_t)parse_int_or(flds[2], 0);
        float elapsed_us = parse_float_or(flds[4], -1.0f);

        group_t g = classify_group(size, capacity_bytes);
        if ((int)g < 0)
            continue;

        if (!strcmp(op, "read"))
        {
            if (elapsed_us > 0 && size > 0)
            {
                float secs = elapsed_us / 1e6f;
                float mb = size / (1024.0f * 1024.0f);
                float mbps = (secs > 0.0f) ? (mb / secs) : NAN;
                if (mbps == mbps && mbps > 0.0f)
                { // not NaN and positive
                    vec_push(&read_v[g], mbps);
                }
                // keep avg-latency for console-style MB/s(avg)
                vec_push(&read_us[g], (float)elapsed_us);

                // also keep per-sample latency in ms for summary
                vec_push(&read_lat_ms[g], (float)elapsed_us / 1000.0f);
            }
            continue;
        }

        else if (!strcmp(op, "program") || !strcmp(op, "write"))
        {
            if (elapsed_us > 0)
            {
                float ms = elapsed_us / 1000.0f; // total op time (ms)
                vec_push(&write_v[g], ms);
            }
        }
        else if (!strcmp(op, "erase"))
        {
            if (elapsed_us > 0)
            {
                float ms = elapsed_us / 1000.0f; // total op time (ms)
                vec_push(&erase_v[g], ms);
            }
        }
    }
    f_close(&f);

    for (int g = 0; g < G_COUNT; ++g)
    {
        calc_stats_from_vec(read_v[g].v, read_v[g].n, &A->read_s.s[g]);
        calc_stats_from_vec(write_v[g].v, write_v[g].n, &A->write_s.s[g]);
        calc_stats_from_vec(erase_v[g].v, erase_v[g].n, &A->erase_s.s[g]);
        // NEW: read latency stats (ms)
        calc_stats_from_vec(read_lat_ms[g].v, read_lat_ms[g].n, &A->read_lat_ms.s[g]);

        // Compute average latency (µs) for console-style read MB/s(avg)
        if (read_us[g].n > 0)
        {
            double acc = 0.0;
            for (int i = 0; i < read_us[g].n; ++i)
                acc += read_us[g].v[i];
            A->read_mean_us[g] = (float)(acc / (double)read_us[g].n);
        }
        else
        {
            A->read_mean_us[g] = NAN;
        }

        free(read_v[g].v);
        free(write_v[g].v);
        free(erase_v[g].v);
        free(read_us[g].v);
        free(read_lat_ms[g].v);
    }
}

static float mbps_from_avg_latency(group_t g, const agg_t *A, uint32_t capacity_bytes)
{
    float mean_us = A->read_mean_us[g];
    if (!(mean_us > 0.0f))
        return NAN;
    uint32_t bytes = (g == G_WHOLE) ? capacity_bytes : GROUP_BYTES[g];
    if (g == G_WHOLE && bytes == 0)
        return NAN;
    float mb = bytes / (1024.0f * 1024.0f);
    float secs = mean_us / 1e6f;
    return (secs > 0.0f) ? (mb / secs) : NAN;
}

/* --------------------------- DB loader ---------------------------------- */
static int load_database(FIL *fp, db_row_t *rows, int max_rows)
{
    if (f_open(fp, DB_FILENAME, FA_READ) != FR_OK)
        return 0;

    char buf[MAX_LINE];
    if (!fatfs_gets(buf, sizeof buf, fp))
    {
        f_close(fp);
        return 0;
    }
    trim(buf);

    char headcpy[MAX_LINE];
    strncpy(headcpy, buf, sizeof headcpy);
    headcpy[sizeof headcpy - 1] = 0;
    char *cols[64];
    int hc = split_fields(headcpy, cols, 64);

    int idx_model = -1, idx_company = -1, idx_family = -1, idx_capacity = -1, idx_jedec = -1;
    int idx_typprog = -1, idx_typ4k = -1, idx_typ32k = -1, idx_typ64k = -1, idx_read50 = -1;

    for (int i = 0; i < hc; i++)
    {
        char name[96];
        strncpy(name, cols[i], sizeof name);
        name[sizeof name - 1] = 0;
        trim(name);
        upper_str(name);
        if (strstr(name, "CHIP_MODEL"))
            idx_model = i;
        else if (strstr(name, "COMPANY"))
            idx_company = i;
        else if (strstr(name, "CHIP_FAMILY"))
            idx_family = i;
        else if (strstr(name, "CAPACITY") && strstr(name, "MBIT"))
            idx_capacity = i;
        else if (strstr(name, "JEDEC"))
            idx_jedec = i;
        else if (strstr(name, "TYP_PAGE_PROGRAM"))
            idx_typprog = i;
        else if (strstr(name, "TYP_4KB"))
            idx_typ4k = i;
        else if (strstr(name, "TYP_32KB"))
            idx_typ32k = i;
        else if (strstr(name, "TYP_64KB"))
            idx_typ64k = i;
        else if (strstr(name, "50MHZ_READ_SPEED") || strstr(name, "50MHZ_READ") || strstr(name, "READ50"))
            idx_read50 = i;
    }

    int n = 0;
    while (n < max_rows && fatfs_gets(buf, sizeof buf, fp))
    {
        trim(buf);
        if (!buf[0])
            continue;

        char linecpy[MAX_LINE];
        strncpy(linecpy, buf, sizeof linecpy);
        linecpy[sizeof linecpy - 1] = 0;
        char *f[64];
        int c = split_fields(linecpy, f, 64);
        if (c <= 1)
            continue;

        db_row_t *r = &rows[n];
        memset(r, 0, sizeof *r);
        r->capacity_mbit = -1;
        r->typ_4k_ms = -1.0f;
        r->typ_32k_ms = -1.0f;
        r->typ_64k_ms = -1.0f;
        r->typ_page_ms = -1.0f;
        r->read50_MBps = -1.0f;

        if (idx_jedec >= 0 && idx_jedec < c)
        {
            char j[32];
            strncpy(j, f[idx_jedec], sizeof j);
            j[sizeof j - 1] = 0;
            trim(j);
            normalize_jedec(j, r->jedec_norm);
        }
        if (idx_model >= 0 && idx_model < c)
        {
            strncpy(r->chip_model, f[idx_model], sizeof r->chip_model);
            r->chip_model[sizeof r->chip_model - 1] = 0;
            trim(r->chip_model);
        }
        if (idx_company >= 0 && idx_company < c)
        {
            strncpy(r->company, f[idx_company], sizeof r->company);
            r->company[sizeof r->company - 1] = 0;
            trim(r->company);
        }
        if (idx_family >= 0 && idx_family < c)
        {
            strncpy(r->family, f[idx_family], sizeof r->family);
            r->family[sizeof r->family - 1] = 0;
            trim(r->family);
        }
        if (idx_capacity >= 0 && idx_capacity < c)
            r->capacity_mbit = parse_int_or(f[idx_capacity], -1);
        if (idx_typprog >= 0 && idx_typprog < c)
            r->typ_page_ms = parse_float_or(f[idx_typprog], -1.0f);
        if (idx_typ4k >= 0 && idx_typ4k < c)
            r->typ_4k_ms = parse_float_or(f[idx_typ4k], -1.0f);
        if (idx_typ32k >= 0 && idx_typ32k < c)
            r->typ_32k_ms = parse_float_or(f[idx_typ32k], -1.0f);
        if (idx_typ64k >= 0 && idx_typ64k < c)
            r->typ_64k_ms = parse_float_or(f[idx_typ64k], -1.0f);
        if (idx_read50 >= 0 && idx_read50 < c)
            r->read50_MBps = parse_float_or(f[idx_read50], -1.0f);

        n++;
    }
    f_close(fp);
    return n;
}

/* ---------------- Identity fill (per your exact rule) ------------------- */
static void fill_identity_fields(const db_row_t *match_row,
                                 const char *jedec_norm,
                                 char *f_detected_jedec, size_t n_j,
                                 char *f_model, size_t n_m,
                                 char *f_family, size_t n_fam,
                                 char *f_company, size_t n_c,
                                 char *f_capacity_mbit, size_t n_cm,
                                 char *f_capacity_bytes, size_t n_cb)
{
    if (jedec_norm && jedec_norm[0])
        snprintf(f_detected_jedec, n_j, "%s", jedec_norm);
    else
    {
        strncpy(f_detected_jedec, NA_STR, n_j);
        f_detected_jedec[n_j - 1] = 0;
    }

    if (match_row && match_row->jedec_norm[0])
    {
        snprintf(f_model, n_m, "%s", match_row->chip_model[0] ? match_row->chip_model : NA_STR);
        snprintf(f_family, n_fam, "%s", match_row->family[0] ? match_row->family : NA_STR);
        snprintf(f_company, n_c, "%s", match_row->company[0] ? match_row->company : NA_STR);

        if (match_row->capacity_mbit > 0)
        {
            snprintf(f_capacity_mbit, n_cm, "%d", match_row->capacity_mbit);
            unsigned long bytes = (unsigned long)((match_row->capacity_mbit / 8.0f) * 1024.0f * 1024.0f);
            snprintf(f_capacity_bytes, n_cb, "%lu", bytes);
        }
        else
        {
            strncpy(f_capacity_mbit, NA_STR, n_cm);
            f_capacity_mbit[n_cm - 1] = 0;
            strncpy(f_capacity_bytes, NA_STR, n_cb);
            f_capacity_bytes[n_cb - 1] = 0;
        }
    }
    else
    {
        strncpy(f_model, NA_STR, n_m);
        f_model[n_m - 1] = 0;
        strncpy(f_family, NA_STR, n_fam);
        f_family[n_fam - 1] = 0;
        strncpy(f_company, NA_STR, n_c);
        f_company[n_c - 1] = 0;
        strncpy(f_capacity_mbit, NA_STR, n_cm);
        f_capacity_mbit[n_cm - 1] = 0;
        strncpy(f_capacity_bytes, NA_STR, n_cb);
        f_capacity_bytes[n_cb - 1] = 0;
    }
}

static void f3_or_na(char *out, size_t n, float v)
{
    if (v != v)
    { // NaN
        strncpy(out, NA_STR, n);
        out[n - 1] = 0;
        return;
    }
    snprintf(out, n, "%.3f", v);
}

static void f2_or_na(char *out, size_t n, float v)
{
    if (v != v)
    { // NaN
        strncpy(out, NA_STR, n);
        out[n - 1] = 0;
        return;
    }
    snprintf(out, n, "%.2f", v);
}

static void i_or_na(char *out, size_t n, int v)
{
    if (v <= 0)
    {
        strncpy(out, NA_STR, n);
        out[n - 1] = 0;
    }
    else
    {
        snprintf(out, n, "%d", v);
    }
}

static void write_three_cols_i(FIL *rf, const char *title, int Ri, int Wi, int Ei)
{
    char r[32], w[32], e[32];
    i_or_na(r, sizeof r, Ri);
    i_or_na(w, sizeof w, Wi);
    i_or_na(e, sizeof e, Ei);
    write_three_cols(rf, title, r, w, e);
}

/* ------------------- Helpers for CSV formatting ------------------------ */
static void f_auto_std_or_na(char *out, size_t n, float v)
{
    if (v != v)
    { // NaN
        strncpy(out, NA_STR, n);
        out[n - 1] = 0;
        return;
    }
    if (fabsf(v) > 0.0f && fabsf(v) < 0.001f)
    {
        snprintf(out, n, "%.6f", v);
    }
    else
    {
        snprintf(out, n, "%.3f", v);
    }
}

static void write_three_cols_f_std(FIL *rf, const char *title, float R, float W, float E)
{
    char r[32], w[32], e[32];
    f_auto_std_or_na(r, sizeof r, R);
    f_auto_std_or_na(w, sizeof w, W);
    f_auto_std_or_na(e, sizeof e, E);
    write_three_cols(rf, title, r, w, e);
}

static void write_three_cols(FIL *rf, const char *title, const char *R, const char *W, const char *E)
{
    char row[1024];
    UINT bw;
    snprintf(row, sizeof row, "%s,%s,%s,%s\n", title, R, W, E);
    f_write(rf, row, strlen(row), &bw);
}
static void write_three_cols_f(FIL *rf, const char *title, float R, float W, float E)
{
    char r[32], w[32], e[32];
    f3_or_na(r, sizeof r, R);
    f3_or_na(w, sizeof w, W);
    f3_or_na(e, sizeof e, E);
    write_three_cols(rf, title, r, w, e);
}

static float absf(float x) { return x < 0 ? -x : x; }

/* Small helper to compare floats that came from the same formulas/file.
   This avoids issues with tiny rounding differences. */
static int float_almost_equal(float a, float b)
{
    if (a != a || b != b) return 0; // treat NaN as "not equal"

    float diff = fabsf(a - b);
    if (diff < 1e-4f) return 1;

    float aa = fabsf(a);
    float bb = fabsf(b);
    float maxab = (aa > bb) ? aa : bb;
    if (maxab < 1e-6f)
        return diff < 1e-6f;

    return (diff / maxab) < 1e-3f;
}


/* ------------- DB means (closest to measured means) -------------------- */
static void compute_db_means_closest(const db_row_t *rows, int n_rows,
                                     const agg_t *A, uint32_t capacity_bytes,
                                     float db_read[G_COUNT],
                                     float db_write[G_COUNT],
                                     float db_erase[G_COUNT],
                                     int *out_read_winner_idx,
                                     int read_winner_idx_by_group[G_COUNT])
{
    for (int g = 0; g < G_COUNT; ++g)
    {
        db_read[g] = NAN;
        db_write[g] = NAN;
        db_erase[g] = NAN;
        if (read_winner_idx_by_group)
            read_winner_idx_by_group[g] = -1;
    }
    if (out_read_winner_idx)
        *out_read_winner_idx = -1;

    /* READ: compare measured MB/s vs predicted read50 * scale */
    float diff_sum[MAX_DB_ROWS];
    int diff_cnt[MAX_DB_ROWS];
    for (int i = 0; i < n_rows; i++)
    {
        diff_sum[i] = 0.0f;
        diff_cnt[i] = 0;
    }

    if (A->sck_MHz > 0)
    {
        for (int g = 0; g < G_COUNT; ++g)
        {
            const stats_t *S = &A->read_s.s[g];
            if (!(S->n > 0))
                continue;

            float best_d = 1e9f;
            float best_pred = NAN;
            int best_i = -1;
            for (int i = 0; i < n_rows; i++)
            {
                if (rows[i].read50_MBps > 0)
                {
                    float pred = rows[i].read50_MBps * (A->sck_MHz / 50.0f);
                    float d = fabsf(pred - S->mean);
                    if (d < best_d)
                    {
                        best_d = d;
                        best_pred = pred;
                        best_i = i;
                    }
                    diff_sum[i] += d;
                    diff_cnt[i] += 1;
                }
            }
            if (best_i >= 0)
            {
                db_read[g] = best_pred;
                if (read_winner_idx_by_group)
                    read_winner_idx_by_group[g] = best_i;
            }
        }
    }

    if (out_read_winner_idx)
    {
        float best_avg = 1e9f;
        int best_i = -1;
        for (int i = 0; i < n_rows; i++)
        {
            if (diff_cnt[i] > 0)
            {
                float avg = diff_sum[i] / (float)diff_cnt[i];
                if (avg < best_avg)
                {
                    best_avg = avg;
                    best_i = i;
                }
            }
        }
        *out_read_winner_idx = best_i;
    }

    /* WRITE: compare measured ms/op vs typ_page_ms * pages */
    for (int g = 0; g < G_COUNT; ++g)
    {
        const stats_t *S = &A->write_s.s[g];
        if (!(S->n > 0))
            continue;

        uint32_t bytes = (g == G_WHOLE) ? capacity_bytes : GROUP_BYTES[g];
        if (g == G_WHOLE && bytes == 0)
            continue;
        unsigned pages = (unsigned)((bytes + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES);
        if (pages == 0)
            continue;

        float best_d = 1e9f;
        float best_pred = NAN;
        for (int i = 0; i < n_rows; i++)
        {
            if (rows[i].typ_page_ms > 0)
            {
                float pred = rows[i].typ_page_ms * (float)pages;
                float d = fabsf(pred - S->mean);
                if (d < best_d)
                {
                    best_d = d;
                    best_pred = pred;
                }
            }
        }
        if (best_d < 1e9f)
            db_write[g] = best_pred;
    }

    /* ERASE: compare measured ms/op vs typ_4k/32k/64k */
    for (int g = 0; g < G_COUNT; ++g)
    {
        const stats_t *S = &A->erase_s.s[g];
        if (!(S->n > 0))
            continue;

        float best_d = 1e9f;
        float best_ref = NAN;
        for (int i = 0; i < n_rows; i++)
        {
            float ref = NAN;
            if (g == G_4K)
                ref = rows[i].typ_4k_ms;
            else if (g == G_32K)
                ref = rows[i].typ_32k_ms;
            else if (g == G_64K)
                ref = rows[i].typ_64k_ms;
            if (ref > 0)
            {
                float d = fabsf(ref - S->mean);
                if (d < best_d)
                {
                    best_d = d;
                    best_ref = ref;
                }
            }
        }
        if (best_d < 1e9f)
            db_erase[g] = best_ref;
    }
}

/* For every (group, op) we want to list all JEDEC IDs whose *datasheet*
   timing/value produced the db_mean_* number for that cell.

   read  column:  match predicted MB/s = read50_MBps * (A->sck_MHz / 50)
   write column:  match typ_page_ms * pages (pages based on group size)
   erase column:  match typ_4k_ms / typ_32k_ms / typ_64k_ms (per group)
*/
static void build_possible_chips_for_all_groups(
    const db_row_t *rows, int n_rows,
    const agg_t *A,
    uint32_t capacity_bytes,
    const float db_read[G_COUNT],
    const float db_write[G_COUNT],
    const float db_erase[G_COUNT],
    char poss_read[G_COUNT][256],
    char poss_write[G_COUNT][256],
    char poss_erase[G_COUNT][256])
{
    for (int g = 0; g < G_COUNT; ++g)
    {
        poss_read[g][0]  = 0;
        poss_write[g][0] = 0;
        poss_erase[g][0] = 0;

        /* ---- READ candidates for this group ---- */
        if (db_read[g] == db_read[g] && A->sck_MHz > 0.0f) // db_read[g] not NaN
        {
            float scale = A->sck_MHz / 50.0f;
            for (int i = 0; i < n_rows; ++i)
            {
                if (!(rows[i].read50_MBps > 0.0f) || !rows[i].jedec_norm[0])
                    continue;

                // Same formula as in compute_db_means_closest()
                float pred = rows[i].read50_MBps * scale;
                if (float_almost_equal(pred, db_read[g]))
                {
                    append_token(poss_read[g], 256, rows[i].jedec_norm);
                }
            }
        }
        if (!poss_read[g][0])
        {
            strncpy(poss_read[g], NA_STR, 256);
            poss_read[g][255] = 0;
        }

        /* ---- WRITE candidates for this group ---- */
        if (db_write[g] == db_write[g])
        {
            uint32_t bytes = (g == G_WHOLE) ? capacity_bytes : GROUP_BYTES[g];
            if (!(g == G_WHOLE && bytes == 0) && bytes > 0)
            {
                unsigned pages = (unsigned)((bytes + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES);
                if (pages > 0)
                {
                    for (int i = 0; i < n_rows; ++i)
                    {
                        if (!(rows[i].typ_page_ms > 0.0f) || !rows[i].jedec_norm[0])
                            continue;

                        // Same formula as for db_write[g]
                        float pred = rows[i].typ_page_ms * (float)pages;
                        if (float_almost_equal(pred, db_write[g]))
                        {
                            append_token(poss_write[g], 256, rows[i].jedec_norm);
                        }
                    }
                }
            }
        }
        if (!poss_write[g][0])
        {
            strncpy(poss_write[g], NA_STR, 256);
            poss_write[g][255] = 0;
        }

        /* ---- ERASE candidates for this group ---- */
        if (db_erase[g] == db_erase[g])
        {
            for (int i = 0; i < n_rows; ++i)
            {
                if (!rows[i].jedec_norm[0])
                    continue;

                float ref = NAN;
                if (g == G_4K)
                    ref = rows[i].typ_4k_ms;
                else if (g == G_32K)
                    ref = rows[i].typ_32k_ms;
                else if (g == G_64K)
                    ref = rows[i].typ_64k_ms;

                if (ref > 0.0f && float_almost_equal(ref, db_erase[g]))
                {
                    append_token(poss_erase[g], 256, rows[i].jedec_norm);
                }
            }
        }
        if (!poss_erase[g][0])
        {
            strncpy(poss_erase[g], NA_STR, 256);
            poss_erase[g][255] = 0;
        }
    }
}

/* Build an intersection across per-group ERASE candidate sets.
   We still keep the CSV row name "conclusion_possible_read_chips"
   for compatibility, but the logic now says:
   "JEDEC IDs whose erase timings match *all* groups that had db_mean_*". */
/* Build an intersection across per-group candidate sets for one operation.
   E.g. call once with poss_read, once with poss_write, once with poss_erase. */
static void conclude_possible_chips_across_groups(
    char poss[G_COUNT][256],
    char *out, size_t cap)
{
    out[0] = 0;

    /* Find first group that actually has real candidates (not NA/empty). */
    int first_group = -1;
    for (int g = 0; g < G_COUNT; ++g)
    {
        if (poss[g][0] && strcmp(poss[g], NA_STR) != 0)
        {
            first_group = g;
            break;
        }
    }

    if (first_group < 0)
    {
        strncpy(out, NA_STR, cap);
        out[cap - 1] = 0;
        return;
    }

    /* Tokenise the first non-NA candidate list and keep only IDs that appear
       in *all* the other non-NA groups. */
    char base[256];
    strncpy(base, poss[first_group], sizeof base);
    base[sizeof base - 1] = 0;

    char *tok = strtok(base, "/");
    while (tok)
    {
        while (*tok == ' ')
            ++tok;
        if (*tok)
        {
            int in_all = 1;
            for (int g = 0; g < G_COUNT; ++g)
            {
                if (g == first_group)
                    continue;
                if (!poss[g][0] || strcmp(poss[g], NA_STR) == 0)
                    continue; // this group doesn't constrain the set

                if (!strstr(poss[g], tok))
                {
                    in_all = 0;
                    break;
                }
            }
            if (in_all)
                append_token(out, cap, tok);
        }
        tok = strtok(NULL, "/");
    }

    if (!out[0])
    {
        strncpy(out, NA_STR, cap);
        out[cap - 1] = 0;
    }
}



/* ------------ Possible read chips (by read winner's read50) ------------- */
static void append_token(char *dst, size_t n, const char *tok)
{
    if (!tok || !tok[0] || n == 0)
        return;

    size_t cur_len = strlen(dst);
    if (cur_len == 0)
    {
        // first token
        strncat(dst, tok, n - 1);
    }
    else
    {
        if (cur_len + 1 < n - 1)
            strncat(dst, "/", n - 1 - cur_len);
        strncat(dst, tok, n - 1 - strlen(dst));
    }
}


// Build an intersection across per-group candidate sets.
// We use read_idx_by_group[g] to look up each group's winning read50_MBps,
// then include all rows whose read50 equals that group's winner.
// Groups with no winner / no candidates are skipped (they don't constrain the set).


/* ------------------------ Final guess scoring --------------------------- */
static float norm_diff(float meas, float ref)
{
    if (!(meas > 0) || !(ref > 0))
        return 3.0f; // capped penalty
    float d = fabsf(meas - ref) / ref;
    if (d > 3.0f)
        d = 3.0f;
    return d;
}

static int pick_best_candidate(const db_row_t *rows, int n, const agg_t *A, const char *jedec_norm,
                               uint32_t capacity_bytes, float *out_score, int *used_metrics_out)
{
    int best = -1;
    float best_score = 1e9f;
    int best_used = 0;

    for (int i = 0; i < n; i++)
    {
        float score = 0.0f;
        int used = 0;

        // READ: per group measured
        if (rows[i].read50_MBps > 0 && A->sck_MHz > 0)
        {
            for (int g = 0; g < G_COUNT; ++g)
            {
                const stats_t *S = &A->read_s.s[g];
                if (S->n > 0)
                {
                    float pred = rows[i].read50_MBps * (A->sck_MHz / 50.0f);
                    score += norm_diff(S->mean, pred);
                    used++;
                }
            }
        }

        // WRITE: per group measured
        if (rows[i].typ_page_ms > 0)
        {
            for (int g = 0; g < G_COUNT; ++g)
            {
                const stats_t *S = &A->write_s.s[g];
                if (S->n > 0)
                {
                    uint32_t bytes = (g == G_WHOLE) ? capacity_bytes : GROUP_BYTES[g];
                    if (g == G_WHOLE && bytes == 0)
                        continue;
                    unsigned pages = (unsigned)((bytes + PAGE_SIZE_BYTES - 1) / PAGE_SIZE_BYTES);
                    if (pages == 0)
                        continue;
                    float pred = rows[i].typ_page_ms * (float)pages;
                    score += norm_diff(S->mean, pred);
                    used++;
                }
            }
        }

        // ERASE: per group measured
        for (int g = 0; g < G_COUNT; ++g)
        {
            const stats_t *S = &A->erase_s.s[g];
            if (S->n <= 0)
                continue;
            float ref = NAN;
            if (g == G_4K)
                ref = rows[i].typ_4k_ms;
            else if (g == G_32K)
                ref = rows[i].typ_32k_ms;
            else if (g == G_64K)
                ref = rows[i].typ_64k_ms;
            if (ref > 0)
            {
                score += norm_diff(S->mean, ref);
                used++;
            }
        }

        if (!used)
            continue; // this row can't be scored

        if (jedec_norm && jedec_norm[0] && rows[i].jedec_norm[0] &&
            strcmp(jedec_norm, rows[i].jedec_norm) == 0)
        {
            score *= 0.25f; // strong bias if JEDEC matches
        }

        if (score < best_score)
        {
            best_score = score;
            best = i;
            best_used = used;
        }
    }

    if (out_score)
        *out_score = (best >= 0 ? best_score : NAN);
    if (used_metrics_out)
        *used_metrics_out = best_used;
    return best;
}

/* ------------------------------ CSV writer ----------------------------- */
static void write_all_stats_for_group_ex(FIL *rf, const char *suffix,
                                         const agg_t *A, group_t g, uint32_t capacity_bytes,
                                         const stats_t *Sr, const stats_t *Sw, const stats_t *Se)
{
    char name[64];

    // READ mean: either console-style MB/s(avg) or average of per-sample MB/s
    float read_mean_to_write = Sr ? Sr->mean : NAN;
#if REPORT_READ_MEAN_FROM_AVG_LATENCY
    float alt = mbps_from_avg_latency(g, A, capacity_bytes);
    if (alt == alt)
        read_mean_to_write = alt; // use if not NaN
#endif

    snprintf(name, sizeof name, "mean_%s", suffix);
    write_three_cols_f(rf, name, read_mean_to_write, Sw ? Sw->mean : NAN, Se ? Se->mean : NAN);

    snprintf(name, sizeof name, "p25_%s", suffix);
    write_three_cols_f(rf, name, Sr ? Sr->p25 : NAN, Sw ? Sw->p25 : NAN, Se ? Se->p25 : NAN);

    snprintf(name, sizeof name, "p50_%s", suffix);
    write_three_cols_f(rf, name, Sr ? Sr->p50 : NAN, Sw ? Sw->p50 : NAN, Se ? Se->p50 : NAN);

    snprintf(name, sizeof name, "p75_%s", suffix);
    write_three_cols_f(rf, name, Sr ? Sr->p75 : NAN, Sw ? Sw->p75 : NAN, Se ? Se->p75 : NAN);

    snprintf(name, sizeof name, "min_%s", suffix);
    write_three_cols_f(rf, name, Sr ? Sr->minv : NAN, Sw ? Sw->minv : NAN, Se ? Se->minv : NAN);

    snprintf(name, sizeof name, "max_%s", suffix);
    write_three_cols_f(rf, name, Sr ? Sr->maxv : NAN, Sw ? Sw->maxv : NAN, Se ? Se->maxv : NAN);

    snprintf(name, sizeof name, "stddev_%s", suffix);
    write_three_cols_f_std(rf, name, Sr ? Sr->stddev : NAN, Sw ? Sw->stddev : NAN, Se ? Se->stddev : NAN);
}

static void write_summary_ms_for_group(FIL *rf, const char *suffix,
                                       const stats_t *Sr_lat_ms, // read latency (ms)
                                       const stats_t *Sw_ms,
                                       const stats_t *Se_ms)
{
    char name[64];

    // counts
    snprintf(name, sizeof name, "n_%s", suffix);
    write_three_cols_i(rf, name,
                       Sr_lat_ms ? Sr_lat_ms->n : 0,
                       Sw_ms ? Sw_ms->n : 0,
                       Se_ms ? Se_ms->n : 0);

    // ms stats (avg/p25/p50/p75/min/max/stddev)
    snprintf(name, sizeof name, "avg_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->mean : NAN,
                           Sw_ms ? Sw_ms->mean : NAN,
                           Se_ms ? Se_ms->mean : NAN);

    snprintf(name, sizeof name, "p25_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->p25 : NAN,
                           Sw_ms ? Sw_ms->p25 : NAN,
                           Se_ms ? Se_ms->p25 : NAN);

    snprintf(name, sizeof name, "p50_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->p50 : NAN,
                           Sw_ms ? Sw_ms->p50 : NAN,
                           Se_ms ? Se_ms->p50 : NAN);

    snprintf(name, sizeof name, "p75_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->p75 : NAN,
                           Sw_ms ? Sw_ms->p75 : NAN,
                           Se_ms ? Se_ms->p75 : NAN);

    snprintf(name, sizeof name, "min_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->minv : NAN,
                           Sw_ms ? Sw_ms->minv : NAN,
                           Se_ms ? Se_ms->minv : NAN);

    snprintf(name, sizeof name, "max_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->maxv : NAN,
                           Sw_ms ? Sw_ms->maxv : NAN,
                           Se_ms ? Se_ms->maxv : NAN);

    snprintf(name, sizeof name, "stddev_%s_ms", suffix);
    write_three_cols_f_std(rf, name,
                           Sr_lat_ms ? Sr_lat_ms->stddev : NAN,
                           Sw_ms ? Sw_ms->stddev : NAN,
                           Se_ms ? Se_ms->stddev : NAN);
}

static void write_report_csv(const db_row_t *rows, int n_rows,
                             const agg_t *A,
                             const db_row_t *match_row,
                             const char *jedec_norm,
                             uint32_t capacity_bytes)
{
    // Identity fields
    char f_detected[16], f_model[64], f_family[48], f_company[48], f_cap_mbit[16], f_cap_bytes[32];
    fill_identity_fields(match_row, jedec_norm,
                         f_detected, sizeof f_detected,
                         f_model, sizeof f_model,
                         f_family, sizeof f_family,
                         f_company, sizeof f_company,
                         f_cap_mbit, sizeof f_cap_mbit,
                         f_cap_bytes, sizeof f_cap_bytes);


    // Pre-compute db_means per (section,size) based on measured means only
    float db_r[G_COUNT], db_w[G_COUNT], db_e[G_COUNT];
    int read_winner_idx = -1;
    int read_idx_by_group[G_COUNT];
    compute_db_means_closest(rows, n_rows, A, capacity_bytes,
                             db_r, db_w, db_e,
                             &read_winner_idx,
                             read_idx_by_group);

                                 // Build JEDEC candidate lists from db_mean_* values
    char poss_read[G_COUNT][256];
    char poss_write[G_COUNT][256];
    char poss_erase[G_COUNT][256];
    build_possible_chips_for_all_groups(rows, n_rows, A, capacity_bytes,
                                        db_r, db_w, db_e,
                                        poss_read, poss_write, poss_erase);


    // Possible read chips (overall winner)

    // Final guess
    float final_score = NAN;
    int used_metrics = 0;
    int best = pick_best_candidate(rows, n_rows, A, jedec_norm, capacity_bytes, &final_score, &used_metrics);

    // Special-case: no measurements at all
    int any_meas = 0;
    for (int g = 0; g < G_COUNT; ++g)
    {
        any_meas |= (A->read_s.s[g].n > 0) || (A->write_s.s[g].n > 0) || (A->erase_s.s[g].n > 0);
    }
    const char *final_j = "undecided", *final_m = "undecided", *final_c = "undecided";
    char fscore[32];
    f3_or_na(fscore, sizeof fscore, final_score);

    if (!any_meas)
    {
        if (jedec_norm && jedec_norm[0])
        {
            // conclude on JEDEC alone
            final_j = jedec_norm;
            if (match_row)
            {
                final_m = match_row->chip_model[0] ? match_row->chip_model : NA_STR;
                final_c = match_row->company[0] ? match_row->company : NA_STR;
            }
            else
            {
                final_m = NA_STR;
                final_c = NA_STR;
            }
            strcpy(fscore, "0.000");
        }
        else
        {
            final_j = "undecided";
            final_m = "undecided";
            final_c = "undecided";
            strcpy(fscore, NA_STR);
        }
    }
    else
    {
        if (best >= 0)
        {
            final_j = rows[best].jedec_norm[0] ? rows[best].jedec_norm : NA_STR;
            final_m = rows[best].chip_model[0] ? rows[best].chip_model : NA_STR;
            final_c = rows[best].company[0] ? rows[best].company : NA_STR;
            f3_or_na(fscore, sizeof fscore, final_score);
        }
        else if (jedec_norm && jedec_norm[0])
        {
            final_j = jedec_norm;
            if (match_row)
            {
                final_m = match_row->chip_model[0] ? match_row->chip_model : NA_STR;
                final_c = match_row->company[0] ? match_row->company : NA_STR;
            }
            else
            {
                final_m = NA_STR;
                final_c = NA_STR;
            }
            // score unknown; leave NA
        }
        else
        {
            final_j = "undecided";
            final_m = "undecided";
            final_c = "undecided";
        }
    }

    // ---------------- Write CSV ----------------
    FIL rf;
    UINT bw;
    FRESULT fr = f_open(&rf, REPORT_FILENAME, FA_CREATE_ALWAYS | FA_WRITE);
    if (fr != FR_OK)
    {
        printf("⛔ Failed to open %s (FR=%d)\n", REPORT_FILENAME, fr);
        return;
    }

    const char *header = "title,read,write,erase\n";
    f_write(&rf, header, strlen(header), &bw);

    // Identity rows
    // Identity rows — SAME values for read | write | erase
    write_three_cols(&rf, "detected_jedec", f_detected, f_detected, f_detected);
    write_three_cols(&rf, "chip_model", f_model, f_model, f_model);
    write_three_cols(&rf, "chip_family", f_family, f_family, f_family);
    write_three_cols(&rf, "company", f_company, f_company, f_company);
    write_three_cols(&rf, "capacity_mbit", f_cap_mbit, f_cap_mbit, f_cap_mbit);
    write_three_cols(&rf, "capacity_bytes", f_cap_bytes, f_cap_bytes, f_cap_bytes);

    // Units + SPI clock
    char sck[32];
    f2_or_na(sck, sizeof sck, (A->sck_MHz > 0 ? A->sck_MHz : NAN));
    write_three_cols(&rf, "spi_sck_MHz", sck, sck, sck);

    // All stats rows per group
    // Explicit units for the summary section (all ms)
    write_three_cols(&rf, "units_summary", "ms", "ms", "ms");

    // Summary rows in ms (follow console summary style)
    for (int g = 0; g < G_COUNT; ++g)
    {
        const char *suf = group_suffix((group_t)g);
        const stats_t *Sr_lat_ms = &A->read_lat_ms.s[g]; // read latency (ms)
        const stats_t *Sw = &A->write_s.s[g];            // ms/op
        const stats_t *Se = &A->erase_s.s[g];            // ms/op
        write_summary_ms_for_group(&rf, suf, Sr_lat_ms, Sw, Se);
    }

    // DB mean rows (NA where no measurement for that size/section)
    for (int g = 0; g < G_COUNT; ++g)
    {
        char name[64];
        snprintf(name, sizeof name, "db_mean_%s", group_suffix((group_t)g));
        write_three_cols_f(&rf, name, db_r[g], db_w[g], db_e[g]);
    }

    // Per-group possible chips (based on db_mean_* for each op)
    // Per-group possible chips (based on db_mean_* for each op)
// One row per size: read/write/erase each get their own column.
for (int g = 0; g < G_COUNT; ++g)
{
    char name[64];
    snprintf(name, sizeof name, "possible_chips_%s", group_suffix((group_t)g));

    // read  column  -> poss_read[g]
    // write column  -> poss_write[g]
    // erase column  -> poss_erase[g]
    write_three_cols(&rf, name, poss_read[g], poss_write[g], poss_erase[g]);
}


    // Conclusion across groups based on ERASE db_mean_* matches (or change to poss_read if you prefer)
   // Conclusions across groups for each op (read / write / erase)
char concl_read[1024], concl_write[1024], concl_erase[1024];

conclude_possible_chips_across_groups(poss_read,  concl_read,  sizeof concl_read);
conclude_possible_chips_across_groups(poss_write, concl_write, sizeof concl_write);
conclude_possible_chips_across_groups(poss_erase, concl_erase, sizeof concl_erase);

// One row, one title, three columns
write_three_cols(&rf, "conclusion_possible_chips",
                 concl_read, concl_write, concl_erase);

    // Notes
    write_three_cols(&rf, "notes",
                     "read: MB/s; db_mean_* = closest READ@SCK to measured mean per size; NA if no read data",
                     "write: ms/op; db_mean_* = typ_page_ms * ceil(bytes/256) closest to measured mean; NA if no write data",
                     "erase: ms/op; db_mean_* = typ_4K/32K/64K closest to measured mean; NA if no erase data");

    // Blank spacer row
    const char *sp = "\n";
    f_write(&rf, sp, strlen(sp), &bw);

    // ==== REVERTED CONCLUSION FORMAT (old style): its own 4-col header + one values row ====
    const char *conc_h = "final_guess_jedec,final_guess_model,final_guess_company,final_score\n";
    f_write(&rf, conc_h, strlen(conc_h), &bw);

    char row[512];
    snprintf(row, sizeof row, "%s,%s,%s,%s\n", final_j, final_m, final_c, fscore);
    f_write(&rf, row, strlen(row), &bw);
    // =============================================================================

    f_close(&rf);
    printf("📄 report.csv written (transposed + old-style conclusion).\n");
}

/* --------------------------------- PUBLIC -------------------------------- */
void report_generate_csv(void)
{
    // 1) Load DB
    db_row_t rows[MAX_DB_ROWS];
    FIL dbf;
    int n_rows = load_database(&dbf, rows, MAX_DB_ROWS);

    // 2) Detect JEDEC & match DB
    char jedec_text[24] = {0};
    flash_get_jedec_str(jedec_text, sizeof jedec_text);
    char jedec_norm6[7] = {0};
    normalize_jedec(jedec_text, jedec_norm6);

    const db_row_t *match_row = NULL;
    if (jedec_norm6[0] && n_rows > 0)
    {
        for (int i = 0; i < n_rows; i++)
        {
            if (rows[i].jedec_norm[0] && strcmp(rows[i].jedec_norm, jedec_norm6) == 0)
            {
                match_row = &rows[i];
                break;
            }
        }
    }

    // Capacity in bytes (for WHOLE and write pages)
    uint32_t capacity_bytes = 0;
    if (match_row && match_row->capacity_mbit > 0)
    {
        capacity_bytes = (uint32_t)((match_row->capacity_mbit / 8.0f) * 1024.0f * 1024.0f);
    }

    // 3) Aggregate RESULTS.CSV (needs capacity for WHOLE classification)
    agg_t A;
    collect_aggregates(&A, capacity_bytes);

    // 4) Emit report
    write_report_csv(rows, n_rows, &A, match_row, jedec_norm6, capacity_bytes);
}
