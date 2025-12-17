#include "chip_db.h"
#include "fatfs/ff.h"      // FatFs
#include "sd_card.h"       // sd_is_mounted()
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* --- small line reader: compatible with FF_USE_STRFUNC==0 ------------------ */
/* Reads a single line (up to n-1 chars) from a FIL using f_read().
 * Stops at '\n' or buffer full. Strips nothing; caller can trim.
 * Returns true if any bytes were read (line may be partial at EOF). */
static bool ff_gets_compat(FIL *fp, char *buf, size_t n) {
    if (!buf || n == 0) return false;
    size_t i = 0;
    for (;;) {
        UINT br = 0;
        unsigned char ch;
        FRESULT fr = f_read(fp, &ch, 1, &br);
        if (fr != FR_OK) {            // I/O error
            buf[i] = '\0';
            return (i > 0);
        }
        if (br == 0) {                // EOF
            buf[i] = '\0';
            return (i > 0);
        }
        if (i + 1 < n) {
            buf[i++] = (char)ch;
        }
        if (ch == '\n') {             // end of line
            break;
        }
    }
    buf[i] = '\0';
    return true;
}

/* --- helpers -------------------------------------------------------------- */

static void trim(char *s) {
    // left trim
    size_t i = 0; while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s + i, strlen(s + i) + 1);
    // right trim
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) s[--n] = '\0';
}

// keep only hex chars, uppercase -> "BF2641" form for robust matching
static void normalize_jedec(const char *in, char *out, size_t out_n) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < out_n; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isxdigit(c)) {
            out[j++] = (char)toupper(c);
        }
    }
    out[j] = '\0';
}

static int split_commas(char *line, char *fields[], int max_fields) {
    int count = 0;
    char *p = line;
    while (*p && count < max_fields) {
        fields[count++] = p;
        char *comma = strchr(p, ',');
        if (!comma) break;
        *comma = '\0';
        p = comma + 1;
    }
    return count;
}

/* --- main API ------------------------------------------------------------- */

bool chipdb_lookup_capacity_bytes(const char *csv_filename,
                                  const char *jedec_str,
                                  size_t *out_bytes)
{
    if (!out_bytes || !jedec_str || !*jedec_str) return false;
    if (!sd_is_mounted()) {
        // Caller should mount before calling; we fail closed.
        return false;
    }

    FIL f;
    FRESULT fr = f_open(&f, csv_filename, FA_READ);
    if (fr != FR_OK) {
        return false;
    }

    // Normalize target JEDEC to compact hex (e.g., "BF2641")
    char want_hex[16] = {0};
    normalize_jedec(jedec_str, want_hex, sizeof want_hex);
    if (!want_hex[0]) { f_close(&f); return false; }

    // Read header to figure out column indexes
    char line[256];
    if (!ff_gets_compat(&f, line, sizeof line)) { f_close(&f); return false; }
    trim(line);

    // Tokenize header
    char *hdr = line;
    char *cols[32] = {0};
    int ncols = split_commas(hdr, cols, 32);

    int idx_jedec = -1;
    int idx_mbit  = -1;
    for (int i = 0; i < ncols; i++) {
        char h[64]; strncpy(h, cols[i], sizeof h - 1); h[sizeof h - 1] = 0;
        trim(h);
        for (char *p = h; *p; ++p) *p = (char)tolower(*p);
        if (!strcmp(h, "jedec id")) idx_jedec = i;
        if (!strcmp(h, "capacity (mbit)")) idx_mbit = i;
    }
    if (idx_jedec < 0 || idx_mbit < 0) { f_close(&f); return false; }

    // Scan rows
    bool found = false;
    while (ff_gets_compat(&f, line, sizeof line)) {
        trim(line);
        if (!line[0]) continue;

        char *fields[32] = {0};
        int nf = split_commas(line, fields, 32);
        if (nf <= idx_mbit || nf <= idx_jedec) continue;

        char *jedec_csv = fields[idx_jedec]; trim(jedec_csv);
        char csv_hex[16] = {0};
        normalize_jedec(jedec_csv, csv_hex, sizeof csv_hex);
        if (!csv_hex[0]) continue;

        if (strcmp(csv_hex, want_hex) == 0) {
            char *mbit_s = fields[idx_mbit]; trim(mbit_s);
            double mbit = strtod(mbit_s, NULL);       // allow decimals
            if (mbit > 0) {
                // 1 Mbit = 131072 bytes (1,048,576 bits)
                unsigned long long bytes = (unsigned long long)(mbit * 131072.0);
                if (bytes > 0) {
                    *out_bytes = (size_t)bytes;
                    found = true;
                    break;
                }
            }
        }
    }

    f_close(&f);
    return found;
}
