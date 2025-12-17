#pragma once
#include <stdbool.h>

// Runs 100 iterations per size; asks about whole-chip (double-confirm inside)
void bench_write_run_100(bool confirm_whole_chip, const char *pattern);

// Prints summary (µs and MB/s) for the most recent in-memory write results
void bench_write_print_summary(void);

bool bench_write_has_data(void);

