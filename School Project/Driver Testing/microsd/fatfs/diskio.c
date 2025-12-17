/*-----------------------------------------------------------------------*/
/* Low level disk I/O module for FatFs (glue)                            */
/* Target: Raspberry Pi Pico / Maker Pi Pico W with SPI SD card          */
/*                                                                       */
/* What this file does (big picture):                                    */
/* - Brings an SD card up in SPI mode and identifies its type (SDSC/SDHC)*/
/* - Implements the 5 functions FatFs calls to talk to a "disk":         */
/*     disk_initialize / disk_status / disk_read / disk_write / ioctl    */
/* - Translates sector reads/writes into SPI commands (CMD17/CMD24, etc) */
/*                                                                       */
/* Notes:                                                                */
/* - This uses the classic SD card SPI command set.                      */
/* - SDHC/SDXC cards use block (512B) addressing; SDSC uses byte addr.   */
/* - Keep SPI slow during init (400 kHz), then speed up for transfers.   */
/*-----------------------------------------------------------------------*/

#include "ff.h"     /* Obtains integer types + FatFs types */
#include "diskio.h" /* Declarations of disk functions */
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <stdio.h>

/* --------------------------------------------------------------------
 * SD Card pinout on Maker Pi Pico W (hardware SPI1)
 * -------------------------------------------------------------------- */
#define SD_SPI_PORT spi1
#define SD_PIN_MISO 12
#define SD_PIN_CS   15
#define SD_PIN_SCK  10
#define SD_PIN_MOSI 11

/* --------------------------------------------------------------------
 * SD Card SPI commands (CMDn, ACMDn = CMD55 followed by CMDn)
 * Only the ones we use are listed.
 * -------------------------------------------------------------------- */
#define CMD0   (0)            /* GO_IDLE_STATE (reset, enter SPI mode)        */
#define CMD1   (1)            /* SEND_OP_COND (MMC, rarely used for SD)       */
#define ACMD41 (0x80 + 41)    /* SD SEND_OP_COND (init)                        */
#define CMD8   (8)            /* SEND_IF_COND (voltage check, v2.0+)          */
#define CMD9   (9)            /* SEND_CSD                                      */
#define CMD10  (10)           /* SEND_CID                                      */
#define CMD12  (12)           /* STOP_TRANSMISSION                             */
#define ACMD13 (0x80 + 13)    /* SD_STATUS                                     */
#define CMD16  (16)           /* SET_BLOCKLEN (legacy, 512 by default)        */
#define CMD17  (17)           /* READ_SINGLE_BLOCK                             */
#define CMD18  (18)           /* READ_MULTIPLE_BLOCK                           */
#define CMD23  (23)           /* SET_BLOCK_COUNT (MMC)                         */
#define ACMD23 (0x80 + 23)    /* SET_WR_BLK_ERASE_COUNT (SDC)                 */
#define CMD24  (24)           /* WRITE_BLOCK                                   */
#define CMD25  (25)           /* WRITE_MULTIPLE_BLOCK                          */
#define CMD32  (32)           /* ERASE_ER_BLK_START                            */
#define CMD33  (33)           /* ERASE_ER_BLK_END                              */
#define CMD38  (38)           /* ERASE                                         */
#define CMD55  (55)           /* APP_CMD (prefix for ACMDs)                    */
#define CMD58  (58)           /* READ_OCR (read operating conditions register) */

/* Global state: is the card initialized and what type is it? */
static bool sd_card_ready = false;  /* becomes true after successful init */
static bool is_sdhc_card  = false;  /* true for SDHC/SDXC (block addressing) */

/* ----------------- Chip Select helpers (active-low) ------------------ */
static void sd_cs_select(void)   { gpio_put(SD_PIN_CS, 0); sleep_us(1); }
static void sd_cs_deselect(void) { sleep_us(1); gpio_put(SD_PIN_CS, 1); sleep_us(1); }

/* Exchange a single byte over SPI and get the response back */
static uint8_t sd_spi_write_read(uint8_t data)
{
    uint8_t result;
    spi_write_read_blocking(SD_SPI_PORT, &data, &result, 1);
    return result;
}

/* --------------------------------------------------------------------
 * Send an SD command (CMDxx/ACMDxx) and return its R1 response byte.
 * - Handles waiting for the card to be ready (not busy).
 * - Adds a CRC only for CMD0 and CMD8 (required by spec in SPI mode).
 * - Returns the first "not busy" response byte (R1: bit7=0).
 * -------------------------------------------------------------------- */
static uint8_t sd_send_command(uint8_t cmd, uint32_t arg)
{
    uint8_t response;

    /* Wait until the card returns 0xFF (not busy on MISO) */
    for (int i = 0; i < 500; i++) {
        if (sd_spi_write_read(0xFF) == 0xFF) break;
        sleep_us(10);
    }

    /* Command frame: 0x40|cmd + 4-byte arg + CRC */
    sd_spi_write_read(0x40 | cmd);
    sd_spi_write_read((uint8_t)(arg >> 24));
    sd_spi_write_read((uint8_t)(arg >> 16));
    sd_spi_write_read((uint8_t)(arg >> 8));
    sd_spi_write_read((uint8_t)arg);

    /* Valid CRCs in SPI mode are only required for CMD0 and CMD8 */
    if (cmd == CMD0)      sd_spi_write_read(0x95);
    else if (cmd == CMD8) sd_spi_write_read(0x87);
    else                  sd_spi_write_read(0x01); /* dummy CRC */

    /* Read up to ~50 bytes until we get an R1 response (bit7 cleared) */
    for (int i = 0; i < 50; i++) {
        response = sd_spi_write_read(0xFF);
        if ((response & 0x80) == 0) return response;
        sleep_us(10);
    }
    return 0xFF; /* timeout / invalid */
}

/* --------------------------------------------------------------------
 * sd_init()
 * Bring the SD card up in SPI mode, verify voltage, complete ACMD41 init,
 * detect SDHC, and then bump SPI speed for data transfers.
 * Returns true on success, false on failure.
 * -------------------------------------------------------------------- */
static bool sd_init(void)
{
    printf("# Initializing 32GB FAT32 SD Card hardware...\n");
    printf("   CS  GP%-2d | SCK GP%-2d | MOSI GP%-2d | MISO GP%-2d\n",
           SD_PIN_CS, SD_PIN_SCK, SD_PIN_MOSI, SD_PIN_MISO);

    /* 1) SPI at 400 kHz for safe init (per spec) */
    printf("🔌 SPI init at 400 kHz (safe)\n");
    spi_init(SD_SPI_PORT, 400 * 1000);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1);
    sd_cs_deselect();

    /* Give card power-up time */
    sleep_ms(500);

    /* 2) Send 80+ clocks with CS high (here: 25 bytes = 200 clocks) */
    for (int i = 0; i < 25; i++) sd_spi_write_read(0xFF);

    /* 3) CMD0: go idle, enter SPI mode (expect R1=0x01) */
    printf("# CMD0 (GO_IDLE_STATE)\n");
    uint8_t response = 0xFF;
    bool got_idle = false;
    for (int tries = 0; tries < 10; tries++) {
        sd_cs_select();
        response = sd_send_command(CMD0, 0);
        sd_cs_deselect();
        printf("   attempt %d: R1=0x%02X\n", tries + 1, response);
        if (response == 0x01) { got_idle = true; break; }
        sleep_ms(50);
    }
    if (!got_idle) {
        printf("### CMD0 failed, card did not enter idle\n");
        return false;
    }

    /* 4) CMD8: check voltage range (v2.0+ cards) */
    printf("# CMD8 (SEND_IF_COND)\n");
    sd_cs_select();
    response = sd_send_command(CMD8, 0x1AA);  /* VHS=2.7-3.6V, check pattern=0xAA */
    bool v2_card = false;
    if (response == 0x01) {
        /* Read trailing R7 (32 bits) */
        uint32_t r7 = 0;
        for (int i = 0; i < 4; i++) r7 = (r7 << 8) | sd_spi_write_read(0xFF);
        if ((r7 & 0xFF) != 0xAA) { sd_cs_deselect(); return false; }
        v2_card = true;
        printf("# v2.0+ card, 3.3V OK\n");
    } else if (response == 0x05) {
        /* Illegal command => v1.x SDSC */
        v2_card = false;
        printf("###  v1.x SDSC (CMD8 unsupported)\n");
    } else {
        sd_cs_deselect();
        printf("### CMD8 unexpected R1=0x%02X\n", response);
        return false;
    }
    sd_cs_deselect();

    /* 5) ACMD41 loop: finish initialization (HCS bit for v2.0+) */
    printf("# ACMD41 init loop\n");
    int timeout = 1000;
    do {
        sd_cs_select();
        uint8_t r55 = sd_send_command(CMD55, 0);  /* APP_CMD prefix */
        if (r55 > 1) { sd_cs_deselect(); sleep_ms(10); continue; }
        uint32_t arg = v2_card ? 0x40000000 : 0x00000000; /* HCS */
        response = sd_send_command(41, arg); /* ACMD41 */
        sd_cs_deselect();
        if (response == 0x00) break;          /* Ready */
        if (response != 0x01) {               /* Still idle? */
            printf("### ACMD41 failed: R1=0x%02X\n", response);
            return false;
        }
        sleep_ms(10);
    } while (--timeout > 0);
    if (timeout == 0) {
        printf("### ACMD41 timeout\n");
        return false;
    }

    /* 6) CMD58: read OCR, detect SDHC/SDXC via CCS bit */
    if (v2_card) {
        sd_cs_select();
        response = sd_send_command(CMD58, 0);
        if (response == 0x00) {
            uint32_t ocr = 0;
            for (int i = 0; i < 4; i++) ocr = (ocr << 8) | sd_spi_write_read(0xFF);
            is_sdhc_card = (ocr & 0x40000000) != 0; /* CCS bit */
            printf("# Card type: %s\n", is_sdhc_card ? "SDHC/SDXC" : "SDSC (v2)");
        } else {
            printf("###  CMD58 failed, assume SDSC\n");
            is_sdhc_card = false;
        }
        sd_cs_deselect();
    } else {
        is_sdhc_card = false; /* v1.x -> SDSC */
    }

    /* 7) Speed up SPI for data transfers (after init completes) */
    printf("# SPI speed -> 10 MHz\n");
    spi_set_baudrate(SD_SPI_PORT, 10000000);

    sd_card_ready = true;
    printf("# SD init complete | ready=%d | SDHC=%d\n", sd_card_ready, is_sdhc_card);
    return true;
}

/*-----------------------------------------------------------------------*/
/* disk_status: report whether drive 0 is initialized                    */
/*-----------------------------------------------------------------------*/
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return sd_card_ready ? 0 : STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* disk_initialize: bring drive 0 online                                 */
/*-----------------------------------------------------------------------*/
DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return sd_init() ? 0 : STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* disk_read: read one or more 512-byte sectors                           */
/* - For SDHC: CMD17 arg is block address (sector number).               */
/* - For SDSC: CMD17 arg is byte address (sector * 512).                 */
/* - Waits for start token 0xFE, then reads 512 bytes and 2 CRC bytes.   */
/*-----------------------------------------------------------------------*/
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sd_card_ready) {
        printf("### SD not ready for read\n");
        return RES_NOTRDY;
    }

    printf("# Read %u sector(s) from LBA %lu (SDHC=%d)\n",
           count, (unsigned long)sector, is_sdhc_card);

    for (UINT i = 0; i < count; i++) {
        LBA_t current = sector + i;

        sd_cs_select();

        /* Address: block (SDHC) vs byte (SDSC) */
        uint32_t arg = is_sdhc_card ? (uint32_t)current : (uint32_t)(current * 512);

        /* Single-block read */
        uint8_t r1 = sd_send_command(CMD17, arg);
        if (r1 != 0x00) {
            sd_cs_deselect();
            printf("### CMD17 fail @ LBA %lu: R1=0x%02X\n", (unsigned long)current, r1);
            return RES_ERROR;
        }

        /* Wait for data token 0xFE */
        int timeout = 8000;
        uint8_t token;
        do { token = sd_spi_write_read(0xFF); } while (token != 0xFE && --timeout > 0);
        if (!timeout) {
            sd_cs_deselect();
            printf("### Data token timeout @ LBA %lu\n", (unsigned long)current);
            return RES_ERROR;
        }

        /* Read 512 bytes into caller's buffer */
        for (int j = 0; j < 512; j++) buff[i * 512 + j] = sd_spi_write_read(0xFF);

        /* Discard CRC (2 bytes) — or capture if you want to validate */
        uint8_t crc1 = sd_spi_write_read(0xFF);
        uint8_t crc2 = sd_spi_write_read(0xFF);
        (void)crc1; (void)crc2;

        sd_cs_deselect();

        if (i == 0) {
            /* Optional: show first 16 bytes for debug */
            printf("# First 16B of LBA %lu: ", (unsigned long)current);
            for (int k = 0; k < 16; k++) printf("%02X ", buff[k]);
            printf("\n");
        }
    }

    printf("# Read OK (%u sector(s))\n", count);
    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* disk_write: write one or more 512-byte sectors                         */
/* - Uses CMD24 (single block) per sector for simplicity.                */
/* - For SDHC use block address; for SDSC use byte address.              */
/*-----------------------------------------------------------------------*/
#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sd_card_ready) return RES_NOTRDY;

    printf("# Write %u sector(s) to LBA %lu (SDHC=%d)\n",
           count, (unsigned long)sector, is_sdhc_card);

    for (UINT i = 0; i < count; i++) {
        sd_cs_select();

        uint32_t arg = is_sdhc_card ? (uint32_t)(sector + i)
                                    : (uint32_t)((sector + i) * 512);

        uint8_t r1 = sd_send_command(CMD24, arg);
        if (r1 != 0x00) {
            sd_cs_deselect();
            printf("### CMD24 fail @ LBA %lu: R1=0x%02X\n", (unsigned long)(sector + i), r1);
            return RES_ERROR;
        }

        /* Start token for single-block write */
        sd_spi_write_read(0xFE);

        /* Send 512 bytes of data */
        for (int j = 0; j < 512; j++) sd_spi_write_read(buff[i * 512 + j]);

        /* Dummy CRC */
        sd_spi_write_read(0xFF);
        sd_spi_write_read(0xFF);

        /* Check data response (xxx00101b = 0x05 is "data accepted") */
        uint8_t resp = sd_spi_write_read(0xFF);
        if ((resp & 0x1F) != 0x05) {
            sd_cs_deselect();
            printf("### Data reject @ LBA %lu: resp=0x%02X\n", (unsigned long)(sector + i), resp);
            return RES_ERROR;
        }

        /* Wait until the card finishes internal programming (busy=0x00) */
        int timeout = 1000;
        while (sd_spi_write_read(0xFF) == 0x00 && --timeout > 0) { /* spin */ }

        sd_cs_deselect();
        if (!timeout) {
            printf("### Write busy-timeout @ LBA %lu\n", (unsigned long)(sector + i));
            return RES_ERROR;
        }
    }

    return RES_OK;
}
#endif

/*-----------------------------------------------------------------------*/
/* disk_ioctl: misc control (sync, geometry, block size, etc.)           */
/*-----------------------------------------------------------------------*/
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) return RES_PARERR;

    switch (cmd) {
    case CTRL_SYNC:
        /* Ensure media is up to date (no write cache in this driver) */
        printf("# CTRL_SYNC\n");
        return RES_OK;

    case GET_SECTOR_COUNT:
        /* Return an estimate: adjust if you can query CSD/CID for real value */
        if (is_sdhc_card) {
            *(LBA_t *)buff = 67108864u;  /* ~32GB / 512B = 67,108,864 */
            printf("# Sector count: %lu (SDHC ~32GB)\n", (unsigned long)67108864u);
        } else {
            *(LBA_t *)buff = 2048000u;   /* ~1GB example */
            printf("# Sector count: %lu (SDSC ~1GB)\n", (unsigned long)2048000u);
        }
        return RES_OK;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;             /* Sector size is 512 bytes */
        printf("📏 Sector size: 512\n");
        return RES_OK;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1;              /* Erase block size in sectors (dummy) */
        printf("#  Erase block size: 1 sector\n");
        return RES_OK;

    default:
        return RES_PARERR;
    }
}
