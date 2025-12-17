/*
 * FatFs - Generic FAT Filesystem Module  R0.15
 * Header file for embedded systems
 *
 * What this header provides:
 * - Basic integer types used by FatFs
 * - Return codes (FRESULT) explaining success/error states
 * - File open flags (FA_*)
 * - Core objects: FATFS (a mounted volume), FIL (an open file), FILINFO (file info)
 * - Prototypes for the main FatFs API (f_open, f_read, f_write, etc.)
 */

#ifndef FF_H
#define FF_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------
 * Integer type definitions used by FatFs
 * (Keep them fixed-width/portable across MCUs)
 * --------------------------------------------- */
typedef unsigned char       BYTE;   // 8-bit
typedef unsigned short      WORD;   // 16-bit
typedef unsigned long       DWORD;  // 32-bit
typedef unsigned long long  QWORD;  // 64-bit
typedef WORD                WCHAR;  // 16-bit wide char (for LFN builds)
typedef DWORD               LBA_t;  // Logical Block Address (sector index)

/* ---------------------------------------------
 * FatFs return codes (function results)
 * ---------------------------------------------
 * FR_OK                   : operation succeeded
 * FR_DISK_ERR             : low-level disk I/O error
 * FR_NO_FILE / NO_PATH    : file or path not found
 * FR_DENIED / EXIST       : permissions/exists issues
 * FR_NO_FILESYSTEM        : no valid FAT volume found
 * ... (others below)
 * --------------------------------------------- */
typedef enum
{
    FR_OK = 0,              /* No error */
    FR_DISK_ERR,            /* A hard error occurred in the low level disk I/O layer */
    FR_INT_ERR,             /* Assertion failed */
    FR_NOT_READY,           /* The physical drive cannot work */
    FR_NO_FILE,             /* Could not find the file */
    FR_NO_PATH,             /* Could not find the path */
    FR_INVALID_NAME,        /* The path name format is invalid */
    FR_DENIED,              /* Access denied due to prohibited access or directory full */
    FR_EXIST,               /* Access denied due to prohibited access */
    FR_INVALID_OBJECT,      /* The file/directory object is invalid */
    FR_WRITE_PROTECTED,     /* The physical drive is write protected */
    FR_INVALID_DRIVE,       /* The logical drive number is invalid */
    FR_NOT_ENABLED,         /* The volume has no work area */
    FR_NO_FILESYSTEM,       /* There is no valid FAT volume */
    FR_MKFS_ABORTED,        /* The f_mkfs() aborted due to a parameter error */
    FR_TIMEOUT,             /* Could not get a grant to access the volume within defined period */
    FR_LOCKED,              /* The operation is rejected according to the file sharing policy */
    FR_NOT_ENOUGH_CORE,     /* LFN working buffer could not be allocated */
    FR_TOO_MANY_OPEN_FILES, /* Number of open files > _FS_LOCK */
    FR_INVALID_PARAMETER    /* Given parameter is invalid */
} FRESULT;

/* ---------------------------------------------
 * File access mode and open method flags
 * (Combine with bitwise OR when calling f_open)
 * ---------------------------------------------
 * FA_READ          : allow reading
 * FA_WRITE         : allow writing
 * FA_OPEN_EXISTING : open only if exists (default)
 * FA_CREATE_NEW    : create new, fail if exists
 * FA_CREATE_ALWAYS : create new, overwrite if exists
 * FA_OPEN_ALWAYS   : open if exists, else create
 * FA_OPEN_APPEND   : open/seek to end (write)
 * --------------------------------------------- */
#define FA_READ           0x01
#define FA_WRITE          0x02
#define FA_OPEN_EXISTING  0x00
#define FA_CREATE_NEW     0x04
#define FA_CREATE_ALWAYS  0x08
#define FA_OPEN_ALWAYS    0x10
#define FA_OPEN_APPEND    0x30

/* ---------------------------------------------
 * FATFS: Mounted filesystem (volume) object
 * One instance per mounted logical drive.
 * win[] is a 512-byte working sector buffer.
 * --------------------------------------------- */
typedef struct
{
    uint8_t  fs_type;   /* File system type (0:invalid) */
    uint8_t  pdrv;      /* Physical drive number */
    uint8_t  ldrv;      /* Logical drive number (used only when _FS_REENTRANT) */
    uint8_t  csize;     /* Cluster size [sectors] */
    uint32_t n_fats;    /* Number of FATs (1 or 2) */
    uint32_t fsize;     /* Sectors per FAT */
    uint32_t volbase;   /* Volume start sector (LBA of partition/volume) */
    uint32_t fatbase;   /* FAT start sector */
    uint32_t dirbase;   /* Root directory start sector */
    uint32_t database;  /* Data area start sector (first cluster) */
    uint32_t winsect;   /* Current sector cached in win[] */
    uint8_t  win[512];  /* Sector window buffer (directory/FAT/data) */
} FATFS;

/* ---------------------------------------------
 * FIL: Open file object
 * Tracks where we are in the file and provides
 * a private 512B buffer for I/O.
 * --------------------------------------------- */
typedef struct
{
    FATFS   *fs;         /* Pointer to the related file system object */
    uint16_t id;         /* File system mount ID of the volume */
    uint8_t  attr;       /* File attributes (e.g., read-only, hidden) */
    uint8_t  stat;       /* File status flags (internal) */
    uint32_t sclust;     /* First cluster of file */
    uint32_t clust;      /* Current cluster of file pointer */
    uint32_t sect;       /* Sector number currently in buf[] */
    uint32_t dir_sect;   /* Sector number containing the directory entry */
    uint8_t *dir_ptr;    /* Pointer to the directory entry inside FATFS.win[] */
    uint8_t  dir_index;  /* Index of directory entry within sector (0–15) */
    uint32_t fsize;      /* File size in bytes */
    uint32_t fptr;       /* Current read/write position (byte offset) */
    uint8_t  buf[512];   /* Per-file I/O buffer (one sector) */
} FIL;

/* ---------------------------------------------
 * FILINFO: Info about a file (from f_stat)
 * fname is short 8.3 name (up to 12 + NUL).
 * --------------------------------------------- */
typedef struct
{
    uint32_t fsize;   /* File size in bytes */
    uint16_t fdate;   /* Last modified date (FAT date format) */
    uint16_t ftime;   /* Last modified time (FAT time format) */
    uint8_t  fattrib; /* File attribute (read-only/hidden/system/dir/archive) */
    char     fname[13]; /* Short file name (8.3) */
} FILINFO;

/* FatFs uses UINT for byte counts/read-write counts */
typedef unsigned int UINT;

/* ---------------------------------------------
 * Core FatFs API (minimal set used in your app)
 * ---------------------------------------------
 * f_mount(fs, path, opt)      : mount/unmount a volume
 * f_open(fp, path, mode)      : open/create a file
 * f_close(fp)                 : close a file
 * f_read(fp, buff, btr, br)   : read bytes
 * f_write(fp, buff, btw, bw)  : write bytes
 * f_sync(fp)                  : flush file buffers to disk
 * f_stat(path, fno)           : get file info without opening
 * f_lseek(fp, ofs)            : move file pointer (seek)
 * f_size(fp)                  : get file size
 * --------------------------------------------- */
FRESULT f_mount(FATFS *fs, const char *path, uint8_t opt);
FRESULT f_open(FIL *fp, const char *path, uint8_t mode);
FRESULT f_close(FIL *fp);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT f_sync(FIL *fp);
FRESULT f_stat(const char *path, FILINFO *fno);
FRESULT f_lseek(FIL *fp, uint32_t ofs);
uint32_t f_size(FIL *fp);

#endif /* FF_H */
