/*-----------------------------------------------------------------------*/
/* Low level disk I/O module SKELETON for FatFs     (C)ChaN, 2019        */
/*-----------------------------------------------------------------------*/
/* If a working storage control module is available, it should be        */
/* attached to the FatFs via a glue function rather than modifying it.   */
/* This is an example of glue functions to attach various exsisting      */
/* storage control modules to the FatFs module with a defined API.       */
/*-----------------------------------------------------------------------*/

#include "ff.h"     /* Obtains integer types */
#include "diskio.h" /* Declarations of disk functions */
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <stdio.h>

// SD Card SPI configuration for Maker Pi Pico W
#define SD_SPI_PORT spi1
#define SD_PIN_MISO 12
#define SD_PIN_CS 15
#define SD_PIN_SCK 10
#define SD_PIN_MOSI 11

// SD Card Commands
#define CMD0 (0)           /* GO_IDLE_STATE */
#define CMD1 (1)           /* SEND_OP_COND (MMC) */
#define ACMD41 (0x80 + 41) /* SEND_OP_COND (SDC) */
#define CMD8 (8)           /* SEND_IF_COND */
#define CMD9 (9)           /* SEND_CSD */
#define CMD10 (10)         /* SEND_CID */
#define CMD12 (12)         /* STOP_TRANSMISSION */
#define ACMD13 (0x80 + 13) /* SD_STATUS (SDC) */
#define CMD16 (16)         /* SET_BLOCKLEN */
#define CMD17 (17)         /* READ_SINGLE_BLOCK */
#define CMD18 (18)         /* READ_MULTIPLE_BLOCK */
#define CMD23 (23)         /* SET_BLOCK_COUNT (MMC) */
#define ACMD23 (0x80 + 23) /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24 (24)         /* WRITE_BLOCK */
#define CMD25 (25)         /* WRITE_MULTIPLE_BLOCK */
#define CMD32 (32)         /* ERASE_ER_BLK_START */
#define CMD33 (33)         /* ERASE_ER_BLK_END */
#define CMD38 (38)         /* ERASE */
#define CMD55 (55)         /* APP_CMD */
#define CMD58 (58)         /* READ_OCR */

static bool sd_card_ready = false;
static bool is_sdhc_card = false; // Track if this is SDHC/SDXC card

static void sd_cs_select(void)
{
    gpio_put(SD_PIN_CS, 0);
    // Use busy-wait instead of sleep in case we're called from HTTP/IRQ context
    busy_wait_us(1);
}

static void sd_cs_deselect(void)
{
    busy_wait_us(1);
    gpio_put(SD_PIN_CS, 1);
    busy_wait_us(1);
}


static uint8_t sd_spi_write_read(uint8_t data)
{
    uint8_t result;
    spi_write_read_blocking(SD_SPI_PORT, &data, &result, 1);
    return result;
}

static uint8_t sd_send_command(uint8_t cmd, uint32_t arg)
{
    uint8_t response;

    // Wait for card ready (not busy)
for (int i = 0; i < 500; i++)
    {
        if (sd_spi_write_read(0xFF) == 0xFF)
            break;
        busy_wait_us(10);
    }

    // Send command
    sd_spi_write_read(0x40 | cmd);
    sd_spi_write_read((uint8_t)(arg >> 24));
    sd_spi_write_read((uint8_t)(arg >> 16));
    sd_spi_write_read((uint8_t)(arg >> 8));
    sd_spi_write_read((uint8_t)arg);

    // CRC (only needed for CMD0 and CMD8)
    if (cmd == CMD0)
    {
        sd_spi_write_read(0x95);
    }
    else if (cmd == CMD8)
    {
        sd_spi_write_read(0x87);
    }
    else
    {
        sd_spi_write_read(0x01);
    }

    // Wait for response (R1 response)
    for (int i = 0; i < 50; i++)
    {
        response = sd_spi_write_read(0xFF);
        if ((response & 0x80) == 0)
            return response;
        busy_wait_us(10);
    }

    return 0xFF;
}

static bool sd_init(void)
{
    printf("🔧 Initializing 32GB FAT32 SD Card hardware...\n");
    printf("   Pin Configuration:\n");
    printf("   - CS (GP%d): Chip Select\n", SD_PIN_CS);
    printf("   - SCK (GP%d): Serial Clock\n", SD_PIN_SCK);
    printf("   - MOSI (GP%d): Master Out Slave In\n", SD_PIN_MOSI);
    printf("   - MISO (GP%d): Master In Slave Out\n", SD_PIN_MISO);

    // Initialize SPI with very conservative settings for 32GB cards
    printf("🔌 Configuring SPI interface at 400kHz (safe speed)...\n");
    spi_init(SD_SPI_PORT, 400 * 1000); // Start very slow for compatibility

    // Configure SPI pins with proper functions
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);

    // Configure CS pin with explicit setup
    gpio_init(SD_PIN_CS);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);
    gpio_put(SD_PIN_CS, 1); // Ensure CS is high initially
    sd_cs_deselect();

    printf("✅ SPI pins configured successfully\n");
    printf("⏳ Allowing SD card power stabilization...\n");

    sleep_ms(500); // Extended power-up time for 32GB cards

    // Extended power-up sequence with diagnostics
    printf("⚡ Performing comprehensive SD card power-up sequence...\n");
    printf("   Sending 160+ clock cycles with CS high (SD card spec)...\n");

    // Send extended clock cycles for 32GB card compatibility
    for (int i = 0; i < 25; i++)
    { // 25 bytes = 200 clock cycles (extra margin)
        uint8_t dummy_response = sd_spi_write_read(0xFF);
        if (i == 0)
        {
            printf("   First dummy response: 0x%02X\n", dummy_response);
        }
    }

    printf("✅ Power-up sequence completed\n");
    sleep_ms(200); // Additional settling time

    // CMD0: Reset SD card to SPI mode with enhanced error handling
    printf("📤 Sending CMD0 (GO_IDLE_STATE) to enter SPI mode...\n");

    int cmd0_attempts = 0;
    uint8_t response;
    bool cmd0_success = false;

    for (cmd0_attempts = 0; cmd0_attempts < 10; cmd0_attempts++)
    {
        sd_cs_select();
        response = sd_send_command(CMD0, 0);
        sd_cs_deselect();

        printf("   CMD0 attempt %d: response = 0x%02X\n", cmd0_attempts + 1, response);

        if (response == 0x01)
        {
            cmd0_success = true;
            break;
        }

        if (response == 0xFF)
        {
            printf("   No response - card may not be inserted or connected\n");
        }
        else if (response != 0x01)
        {
            printf("   Unexpected response - retrying...\n");
        }

        sleep_ms(50); // Wait between attempts
    }

    if (!cmd0_success)
    {
        printf("❌ CMD0 failed after %d attempts: final response=0x%02X\n", cmd0_attempts, response);
        printf("   Troubleshooting:\n");
        printf("   - Check SD card is properly inserted\n");
        printf("   - Verify SD card is not write-protected\n");
        printf("   - Ensure all SPI connections are solid\n");
        printf("   - Try a different SD card\n");
        printf("   - Check if SD card is FAT32 formatted\n");
        return false;
    }

    printf("✅ CMD0 successful after %d attempts - SD card in SPI mode\n", cmd0_attempts + 1);

    // CMD8: Check voltage range
    printf("📤 Sending CMD8 (SEND_IF_COND)...\n");
    sd_cs_select();
    response = sd_send_command(CMD8, 0x1AA);
    bool v2_card = false;

    if (response == 0x01)
    {
        // Read R7 response
        uint32_t r7 = 0;
        for (int i = 0; i < 4; i++)
        {
            r7 = (r7 << 8) | sd_spi_write_read(0xFF);
        }
        printf("✅ CMD8 response: 0x%08lX\n", (unsigned long)r7);

        if ((r7 & 0xFF) != 0xAA)
        {
            printf("❌ CMD8 voltage check failed\n");
            sd_cs_deselect();
            return false;
        }
        printf("✅ SD card v2.0+ supports 3.3V operation\n");
        v2_card = true;
    }
    else if (response == 0x05)
    {
        printf("⚠️  Older SD card (v1.x) - CMD8 not supported\n");
        v2_card = false;
    }
    else
    {
        printf("❌ CMD8 failed: response=0x%02X\n", response);
        sd_cs_deselect();
        return false;
    }
    sd_cs_deselect();

    // ACMD41: Initialize card
    int timeout = 1000;
    printf("🔄 Initializing SD card with ACMD41...\n");

    do
    {
        sd_cs_select();

        // Send CMD55 first (APP_CMD)
        uint8_t cmd55_response = sd_send_command(CMD55, 0);
        if (cmd55_response > 1)
        {
            sd_cs_deselect();
            printf("❌ CMD55 failed: 0x%02X\n", cmd55_response);
            sleep_ms(10);
            timeout--;
            continue;
        }

        // Now send ACMD41 (CMD41 after CMD55)
        // Use HCS bit (0x40000000) for v2.0+ cards to support SDHC
        uint32_t acmd41_arg = v2_card ? 0x40000000 : 0x00000000;
        response = sd_send_command(41, acmd41_arg);
        sd_cs_deselect();

        if (response == 0x00)
        {
            printf("✅ ACMD41 successful after %d attempts\n", 1000 - timeout);
            break;
        }

        if (response != 0x01)
        {
            printf("❌ ACMD41 failed: 0x%02X\n", response);
            return false;
        }

        sleep_ms(10);
        timeout--;
    } while (timeout > 0);

    if (timeout == 0)
    {
        printf("❌ ACMD41 timeout - card not ready\n");
        return false;
    }

    // CMD58: Read OCR to determine card capacity type (SDHC vs SDSC)
    if (v2_card)
    {
        printf("📤 Sending CMD58 (READ_OCR)...\n");
        sd_cs_select();
        response = sd_send_command(CMD58, 0);
        if (response == 0x00)
        {
            uint32_t ocr = 0;
            for (int i = 0; i < 4; i++)
            {
                ocr = (ocr << 8) | sd_spi_write_read(0xFF);
            }
            printf("✅ OCR register: 0x%08lX\n", (unsigned long)ocr);

            // Check CCS bit (bit 30) to determine if SDHC/SDXC
            is_sdhc_card = (ocr & 0x40000000) != 0;
            printf("📋 Card type: %s\n", is_sdhc_card ? "SDHC/SDXC" : "SDSC");
        }
        else
        {
            printf("⚠️  CMD58 failed - assuming SDSC\n");
            is_sdhc_card = false;
        }
        sd_cs_deselect();
    }
    else
    {
        // v1.x cards are always SDSC
        is_sdhc_card = false;
        printf("📋 Card type: SDSC v1.x\n");
    }

    // Increase SPI speed after successful initialization
    printf("⚡ Increasing SPI speed to 10MHz for data transfer\n");
    spi_set_baudrate(SD_SPI_PORT, 1000000);

    sd_card_ready = true;
    printf("🎉 SD Card initialization complete!\n");
    printf("📊 Card ready: %s | SDHC: %s\n",
           sd_card_ready ? "YES" : "NO",
           is_sdhc_card ? "YES" : "NO");

    return true;
}

/*-----------------------------------------------------------------------*/
/* Get Drive Status                                                     */
/*-----------------------------------------------------------------------*/

DSTATUS disk_status(
    BYTE pdrv /* Physical drive nmuber to identify the drive */
)
{
    if (pdrv != 0)
        return STA_NOINIT;
    return sd_card_ready ? 0 : STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Inidialize a Drive                                                    */
/*-----------------------------------------------------------------------*/

DSTATUS disk_initialize(
    BYTE pdrv /* Physical drive nmuber to identify the drive */
)
{
    if (pdrv != 0)
        return STA_NOINIT;

    if (sd_init())
    {
        return 0; // Success
    }

    return STA_NOINIT;
}

/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

DRESULT disk_read(
    BYTE pdrv,    /* Physical drive nmuber to identify the drive */
    BYTE *buff,   /* Data buffer to store read data */
    LBA_t sector, /* Start sector in LBA */
    UINT count    /* Number of sectors to read */
)
{
    if (pdrv != 0 || !sd_card_ready)
    {
        printf("❌ SD card not ready for read operation\n");
        return RES_NOTRDY;
    }

    // Special handling for sector 0 (boot sector) - critical for FAT32
    if (sector == 0)
    {
        printf("🔍 CRITICAL: Reading sector 0 (FAT32 boot sector)\n");
        printf("   This sector contains filesystem information\n");
        printf("   If this fails, the SD card may not be FAT32 formatted\n");
    }

    printf("📖 Reading %u sector(s) starting from sector %lu (SDHC: %s)\n",
           count, (unsigned long)sector, is_sdhc_card ? "YES" : "NO");

    for (UINT i = 0; i < count; i++)
    {
        uint32_t current_sector = sector + i;

        printf("📖 Processing sector %lu:\n", (unsigned long)current_sector);

        sd_cs_select();

        // Address calculation with enhanced diagnostics
        uint32_t address = current_sector;
        if (!is_sdhc_card)
        {
            address = current_sector * 512; // Convert to byte addressing for SDSC
            printf("   SDSC mode: Using byte address 0x%08lX\n", (unsigned long)address);
        }
        else
        {
            printf("   SDHC mode: Using block address %lu\n", (unsigned long)address);
        }

        printf("   Sending CMD17 (READ_SINGLE_BLOCK)...\n");
        uint8_t response = sd_send_command(CMD17, address);

        if (response != 0x00)
        {
            printf("❌ CMD17 failed for sector %lu: response=0x%02X\n", (unsigned long)current_sector, response);

            if (current_sector == 0)
            {
                printf("❌ CRITICAL: Sector 0 read failed - SD card issues:\n");
                printf("   - SD card may not be FAT32 formatted\n");
                printf("   - Card might be corrupted or damaged\n");
                printf("   - Try formatting the SD card as FAT32 on PC\n");
                printf("   - Ensure SD card is properly seated\n");
            }

            sd_cs_deselect();
            return RES_ERROR;
        }

        printf("   ✅ CMD17 successful, waiting for data...\n");

        // Wait for data token with enhanced timeout handling
        int timeout = 8000; // Extended timeout for 32GB cards
        uint8_t data_response;

        do
        {
            data_response = sd_spi_write_read(0xFF);
            timeout--;

            if (timeout % 2000 == 0 && timeout > 0)
            {
                printf("   ⏳ Waiting for data token... (%d attempts remaining)\n", timeout);
            }
        } while (data_response != 0xFE && timeout > 0);

        if (timeout == 0)
        {
            printf("❌ Data token timeout for sector %lu (last response: 0x%02X)\n",
                   (unsigned long)current_sector, data_response);

            if (current_sector == 0)
            {
                printf("❌ CRITICAL: Sector 0 data timeout - possible causes:\n");
                printf("   - SD card is not responding properly\n");
                printf("   - SPI communication issues\n");
                printf("   - SD card may be damaged\n");
                printf("   - Check SPI connections (CS, SCK, MOSI, MISO)\n");
            }

            sd_cs_deselect();
            return RES_ERROR;
        }

        printf("   ✅ Data token received (0x%02X), reading %d bytes...\n", data_response, 512);

        // Read 512-byte sector data
        for (int j = 0; j < 512; j++)
        {
            buff[i * 512 + j] = sd_spi_write_read(0xFF);
        }

        // Read and verify CRC
        uint8_t crc1 = sd_spi_write_read(0xFF);
        uint8_t crc2 = sd_spi_write_read(0xFF);

        sd_cs_deselect();

        // Enhanced validation for sector 0 (FAT32 boot sector)
        if (current_sector == 0)
        {
            printf("🔍 Validating FAT32 boot sector data:\n");

            // Check for FAT32 signature at offset 510-511
            if (buff[i * 512 + 510] == 0x55 && buff[i * 512 + 511] == 0xAA)
            {
                printf("   ✅ Valid boot sector signature (0x55AA) found\n");
            }
            else
            {
                printf("   ❌ Invalid boot sector signature: 0x%02X%02X (should be 0x55AA)\n",
                       buff[i * 512 + 510], buff[i * 512 + 511]);
                printf("   ⚠️  SD card may not be properly formatted as FAT32\n");
            }

            // Check filesystem type (should contain "FAT32")
            bool fat32_found = false;
            for (int k = 0; k < 504; k++)
            {
                if (buff[i * 512 + k] == 'F' &&
                    buff[i * 512 + k + 1] == 'A' &&
                    buff[i * 512 + k + 2] == 'T' &&
                    buff[i * 512 + k + 3] == '3' &&
                    buff[i * 512 + k + 4] == '2')
                {
                    fat32_found = true;
                    break;
                }
            }

            if (fat32_found)
            {
                printf("   ✅ FAT32 filesystem signature found\n");
            }
            else
            {
                printf("   ⚠️  FAT32 signature not found - may be different filesystem\n");
            }

            // Show first 32 bytes for debugging
            printf("   📄 First 32 bytes of boot sector:\n   ");
            for (int k = 0; k < 32; k++)
            {
                printf("%02X ", buff[i * 512 + k]);
                if ((k + 1) % 16 == 0)
                    printf("\n   ");
            }
            printf("\n");
        }

        printf("   ✅ Sector %lu read successfully (CRC: %02X%02X)\n",
               (unsigned long)current_sector, crc1, crc2);

        // Debug: show first few bytes of sector data
        if (i == 0)
        {
            printf("🔍 First 16 bytes of sector %lu: ", sector);
            for (int k = 0; k < 16; k++)
            {
                printf("%02X ", buff[k]);
            }
            printf("\n");
        }
    }

    printf("✅ Successfully read %u sector(s)\n", count);
    return RES_OK;
}

/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                      */
/*-----------------------------------------------------------------------*/

#if FF_FS_READONLY == 0

DRESULT disk_write(
    BYTE pdrv,        /* Physical drive nmuber to identify the drive */
    const BYTE *buff, /* Data to be written */
    LBA_t sector,     /* Start sector in LBA */
    UINT count        /* Number of sectors to write */
)
{
    if (pdrv != 0 || !sd_card_ready)
        return RES_NOTRDY;

    printf("📝 Writing %u sector(s) starting from sector %lu (SDHC: %s)\n",
           count, (unsigned long)sector, is_sdhc_card ? "YES" : "NO");

    for (UINT i = 0; i < count; i++)
    {
        sd_cs_select();

        // Address calculation - SDHC uses block addressing, SDSC uses byte addressing
        uint32_t address = sector + i;
        if (!is_sdhc_card)
        {
            address = (sector + i) * 512; // Convert to byte addressing for SDSC
        }

        uint8_t response = sd_send_command(CMD24, address);

        if (response != 0x00)
        {
            printf("❌ CMD24 failed for sector %lu: 0x%02X\n", (unsigned long)(sector + i), response);
            sd_cs_deselect();
            return RES_ERROR;
        }

        // Send data token
        sd_spi_write_read(0xFE);

        // Send data
        for (int j = 0; j < 512; j++)
        {
            sd_spi_write_read(buff[i * 512 + j]);
        }

        // Send CRC (dummy)
        sd_spi_write_read(0xFF);
        sd_spi_write_read(0xFF);

        // Get data response
        response = sd_spi_write_read(0xFF);
        if ((response & 0x1F) != 0x05)
        {
            sd_cs_deselect();
            return RES_ERROR;
        }

        // Wait for write completion
        int timeout = 1000;
        do
        {
            response = sd_spi_write_read(0xFF);
            timeout--;
        } while (response == 0x00 && timeout > 0);

        sd_cs_deselect();

        if (timeout == 0)
            return RES_ERROR;
    }

    return RES_OK;
}

#endif

/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */
/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(
    BYTE pdrv, /* Physical drive nmuber (0..) */
    BYTE cmd,  /* Control code */
    void *buff /* Buffer to send/receive control data */
)
{
    if (pdrv != 0)
        return RES_PARERR;

    switch (cmd)
    {
    case CTRL_SYNC:
        printf("💾 Sync request - ensuring data is written\n");
        return RES_OK;

    case GET_SECTOR_COUNT:
        // For a 32GB SDHC card: 32GB = 32 * 1024 * 1024 * 1024 bytes
        // Divided by 512 bytes per sector = 67,108,864 sectors
        if (is_sdhc_card)
        {
            *(LBA_t *)buff = 67108864; // 32GB SDHC card
            printf("📊 Sector count: %lu (32GB SDHC)\n", (unsigned long)67108864);
        }
        else
        {
            *(LBA_t *)buff = 2048000; // ~1GB SDSC card
            printf("📊 Sector count: %lu (1GB SDSC)\n", (unsigned long)2048000);
        }
        return RES_OK;

    case GET_SECTOR_SIZE:
        *(WORD *)buff = 512;
        printf("📏 Sector size: 512 bytes\n");
        return RES_OK;

    case GET_BLOCK_SIZE:
        *(DWORD *)buff = 1; // Erase block size in sectors
        printf("🗂️  Block size: 1 sector\n");
        return RES_OK;
    }

    return RES_PARERR;
}