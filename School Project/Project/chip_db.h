#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Look up capacity (bytes) by JEDEC string like "BF 26 41" from CSV on SD card.
// Returns true if found; false if not found or on error.
bool chipdb_lookup_capacity_bytes(const char *csv_path,
                                  const char *jedec_id_in,
                                  size_t *out_bytes);

// Optional: read back last cached lookup (returns false if empty).
bool chipdb_get_cached_capacity(size_t *out_bytes, const char **out_jedec);

/**
 * Look up capacity (in BYTES) for a given JEDEC ID using a CSV on the microSD.
 * - csv_filename: e.g. "datasheet.csv"
 * - jedec_str: e.g. "BF 26 41" (any punctuation/case will be normalized)
 * - out_bytes: set to capacity in bytes on success
 * Returns: true if found and parsed, false otherwise.
 */
bool chipdb_lookup_capacity_bytes(const char *csv_filename,
                                  const char *jedec_str,
                                  size_t *out_bytes);

#ifdef __cplusplus
}
#endif
