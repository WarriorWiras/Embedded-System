#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include <stdint.h>

// File information structure used by the HTTP server to list files
typedef struct {
	char filename[32];
	uint32_t size;
} sd_file_info_t;

// Real SD card interface using FatFs
bool sd_card_init(void);
bool sd_mount(void);
bool sd_write_file(const char *filename, const char *content);
bool sd_append_to_file(const char *filename, const char *content);
bool sd_file_exists(const char *filename);
void sd_unmount(void);
int sd_count_csv_rows(const char *filename, int *out_total_lines, int *out_data_rows);
// NOTE: saved in root as "Flash_Backup.bin" for now (simple FAT layer has no subdirs yet).
bool sd_backup_flash_full(const char *dir, const char *filename);
bool sd_is_mounted(void);
bool sd_backup_flash_safe(const char *dir, const char *filename);
bool sd_restore_flash_safe(const char *dir, const char *filename);

// Get simple file list from root directory (fills up to max_files entries)
int sd_get_file_list(sd_file_info_t *files, int max_files);


#endif // SD_CARD_H