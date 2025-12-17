#ifndef SD_CARD_H          // This stops the file from being included twice by accident
#define SD_CARD_H

#include <stdbool.h>       // Lets us use 'bool' (true/false)

//
// Real SD card interface using FatFs
// Think of this as a tiny toolbox for talking to the SD card:
// - "init" = get the wires ready
// - "mount" = open the card so we can see files
// - "write/append" = put text into a file
// - "exists" = check if a file is there
// - "unmount" = close the card safely
// - "count_csv_rows" = count lines in a CSV file
//

/**
 * sd_card_init
 * Get the SD card hardware ready (SPI pins, etc.).
 * Call this once before using other SD card functions.
 * @return true if OK, false if something went wrong.
 */
bool sd_card_init(void);

/**
 * sd_mount
 * Open (mount) the SD card’s file system so we can read/write files.
 * Call after sd_card_init().
 * @return true if the card was mounted, false if mount failed.
 */
bool sd_mount(void);

/**
 * sd_write_file
 * Create or overwrite a file and write the given text.
 * Good for writing a brand-new file from scratch.
 * @param filename  e.g., "RESULTS.CSV"
 * @param content   the text to put in the file (will replace old contents)
 * @return true if write succeeded, false otherwise.
 */
bool sd_write_file(const char *filename, const char *content);

/**
 * sd_append_to_file
 * Open a file and add text to the END of it (keeps old data).
 * Useful for logging more results on new lines.
 * @param filename  e.g., "RESULTS.CSV"
 * @param content   the text to add (you include the "\n" if you want a new line)
 * @return true if append succeeded, false otherwise.
 */
bool sd_append_to_file(const char *filename, const char *content);

/**
 * sd_file_exists
 * Check if a file is already on the SD card.
 * @param filename  name to look for
 * @return true if found, false if not found (or error).
 */
bool sd_file_exists(const char *filename);

/**
 * sd_unmount
 * Close (unmount) the SD card’s file system safely.
 * Call this before shutting down or removing the card.
 */
void sd_unmount(void);

/**
 * sd_count_csv_rows
 * Count how many lines are in a CSV file.
 * Also returns how many are "data rows" (usually total minus the header line).
 * @param filename          e.g., "RESULTS.CSV"
 * @param out_total_lines   where to store total number of lines (can be NULL if not needed)
 * @param out_data_rows     where to store number of data lines (can be NULL if not needed)
 * @return number of data rows (same as *out_data_rows), or -1 on error.
 */
int sd_count_csv_rows(const char *filename, int *out_total_lines, int *out_data_rows);

// Compute average temperature (temp_C column) over all data rows in a CSV.
// Returns true on success. On success, *out_avg gets the average and *out_count
// gets the number of temperature samples found.
bool sd_compute_avg_temp(const char *filename, double *out_avg, int *out_count);

bool sd_print_file(const char *filename);


#endif // SD_CARD_H
