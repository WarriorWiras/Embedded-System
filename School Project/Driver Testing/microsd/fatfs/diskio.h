/*-----------------------------------------------------------------------/
/  Low level disk interface module include file   (C) ChaN, 2014
/  Purpose: Defines the tiny API that FatFs uses to talk to a storage
/           device (SD card, SPI flash, etc.). You implement these
/           functions in your platform code (e.g., diskio.c).
/-----------------------------------------------------------------------*/

#ifndef _DISKIO_DEFINED
#define _DISKIO_DEFINED

#ifdef __cplusplus
extern "C" {
#endif

#include "ff.h"   /* Pulls in BYTE, UINT, LBA_t, etc. */

/* -------------------------------
 * DSTATUS: bitfield status of a drive
 * Example bits: STA_NOINIT, STA_NODISK, STA_PROTECT
 * ------------------------------- */
typedef BYTE DSTATUS;

/* -------------------------------
 * DRESULT: return codes for I/O ops
 * RES_OK     : success
 * RES_ERROR  : read/write hardware error
 * RES_WRPRT  : medium is write-protected
 * RES_NOTRDY : medium not ready/absent
 * RES_PARERR : invalid parameter(s)
 * ------------------------------- */
typedef enum {
    RES_OK = 0,   /* 0: Successful */
    RES_ERROR,    /* 1: R/W Error */
    RES_WRPRT,    /* 2: Write Protected */
    RES_NOTRDY,   /* 3: Not Ready */
    RES_PARERR    /* 4: Invalid Parameter */
} DRESULT;

/* ==========================================================
 * Low-level disk I/O API that YOU implement (see diskio.c)
 * FatFs calls these to access the physical device.
 * pdrv : physical drive number (0, 1, …) — usually 0.
 * ========================================================== */

/**
 * disk_initialize(pdrv)
 * Bring the drive online (power up, init SPI, card init, etc.).
 * Return: DSTATUS with STA_NOINIT cleared on success.
 */
DSTATUS disk_initialize(BYTE pdrv);

/**
 * disk_status(pdrv)
 * Query current status bits (initialized? no disk? write-protected?).
 */
DSTATUS disk_status(BYTE pdrv);

/**
 * disk_read(pdrv, buff, sector, count)
 * Read 'count' sectors starting at LBA 'sector' into 'buff'.
 * Each sector is typically 512 bytes (unless your media differs).
 * Return RES_OK on success.
 */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);

/**
 * disk_write(pdrv, buff, sector, count)
 * Write 'count' sectors from 'buff' to LBA 'sector'.
 * Only required when FatFs is built with write support.
 * Return RES_OK on success.
 */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);

/**
 * disk_ioctl(pdrv, cmd, buff)
 * Misc control commands (sync, get size, etc.).
 * See command codes below.
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff);

/* -------------------------------
 * DSTATUS bit flags
 * ------------------------------- */
#define STA_NOINIT   0x01   /* Drive not initialized */
#define STA_NODISK   0x02   /* No medium in the drive */
#define STA_PROTECT  0x04   /* Write protected */

/* ---------------------------------------------------------
 * disk_ioctl() command codes
 * (Some used by FatFs core; others optional for your driver)
 * --------------------------------------------------------- */

/* Generic commands (used by FatFs) */
#define CTRL_SYNC          0  /* Flush any pending writes to the medium */
#define GET_SECTOR_COUNT   1  /* Return total number of sectors (LBA count) */
#define GET_SECTOR_SIZE    2  /* Return sector size in bytes (e.g., 512) */
#define GET_BLOCK_SIZE     3  /* Erase block size in sectors (for f_mkfs) */
#define CTRL_TRIM          4  /* Advise device that a range is no longer used */

/* Generic commands (not used by FatFs core, but available) */
#define CTRL_POWER         5  /* Get/Set power status */
#define CTRL_LOCK          6  /* Lock/Unlock media removal */
#define CTRL_EJECT         7  /* Eject media */
#define CTRL_FORMAT        8  /* Low-level format (rarely implemented) */

/* MMC/SD-specific commands (optional, for extra info) */
#define MMC_GET_TYPE      10  /* Get card type */
#define MMC_GET_CSD       11  /* Get CSD register */
#define MMC_GET_CID       12  /* Get CID register */
#define MMC_GET_OCR       13  /* Get OCR register */
#define MMC_GET_SDSTAT    14  /* Get SD status */
#define ISDIO_READ        55  /* Read iSDIO register */
#define ISDIO_WRITE       56  /* Write iSDIO register */
#define ISDIO_MRITE       57  /* Masked write to iSDIO register */

#ifdef __cplusplus
}
#endif

#endif /* _DISKIO_DEFINED */
