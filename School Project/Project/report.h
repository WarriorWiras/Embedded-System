// report.h
#ifndef REPORT_H
#define REPORT_H

#ifdef __cplusplus
extern "C" {
#endif

// Generates / overwrites report.csv from datasheet.csv + RESULTS.CSV
void report_generate_csv(void);

// Optional gates you can override in another .c (non-weak there):
// Return 1 to include that section, 0 to skip.
int report_enable_erase(void);
int report_enable_prog(void);
int report_enable_read(void);

#ifdef __cplusplus
}
#endif

#endif // REPORT_H
