// bench_read.h
#pragma once
#include <stdbool.h>

void bench_read_run_100(bool confirm_whole_chip);
void bench_read_print_summary(void);
bool bench_read_has_data(void);
