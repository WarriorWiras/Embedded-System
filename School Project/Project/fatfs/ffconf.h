/* ffconf.h – FatFs configuration for RP2040 + SD card (SPI) */
#define FFCONF_DEF 80386   /* must match FF_DEFINED in ff.h */


/*----------------------------------------------------------*/
/* Functions                                                */
/*----------------------------------------------------------*/
#define FF_FS_READONLY     0
#define FF_FS_MINIMIZE     0
#define FF_USE_FIND        0
#define FF_USE_MKFS        1
#define FF_USE_FASTSEEK    0
#define FF_USE_EXPAND      0
#define FF_USE_CHMOD       0
#define FF_USE_LABEL       0
#define FF_USE_FORWARD     0

/*----------------------------------------------------------*/
/* Long File Name                                           */
/*----------------------------------------------------------*/
#define FF_USE_LFN         2   /* dynamic working buffer */
#define FF_MAX_LFN         255
#define FF_LFN_UNICODE     0   /* ASCII/ANSI */

#define FF_SFN_BUF         12
#define FF_LFN_BUF         255

/*----------------------------------------------------------*/
/* Character encoding / Unicode                             */
/*----------------------------------------------------------*/
#define FF_CODE_PAGE       437
#define FF_USE_STRFUNC     0

/*----------------------------------------------------------*/
/* Sector / Buffer size                                     */
/*----------------------------------------------------------*/
#define FF_FS_TINY         0
#define FF_MIN_SS          512
#define FF_MAX_SS          512

/*----------------------------------------------------------*/
/* Volume / Filesystem                                      */
/*----------------------------------------------------------*/
#define FF_MULTI_PARTITION 0
#define FF_USE_TRIM        0
#define FF_FS_NOFSINFO     0
#define FF_FS_EXFAT        0
#define FF_FS_RPATH        0
#define FF_VOLUMES         1

/*----------------------------------------------------------*/
/* Synchronization (not used on bare metal)                 */
/*----------------------------------------------------------*/
#define FF_FS_REENTRANT    0
#define FF_FS_TIMEOUT      1000
#define FF_SYNC_t          void*

/*----------------------------------------------------------*/
/* Misc                                                     */
/*----------------------------------------------------------*/
#define FF_STR_VOLUME_ID   0
#define FF_FS_NORTC        1   /* no RTC on Pico */
#define FF_NORTC_YEAR      2025  /* Any valid year >= 1980 */
#define FF_NORTC_MON       1     /* 1 = January */
#define FF_NORTC_MDAY      1     /* 1 = 1st of the month */
#define FF_FS_LOCK         0
#define FF_STRF_ENCODE     0
#define FF_LBA64           0    /* Use 0 for standard 32-bit LBA (good for SD cards) */

#if FF_LBA64 && !FF_FS_EXFAT
#error "exFAT must be enabled when using 64-bit LBA"
#endif


