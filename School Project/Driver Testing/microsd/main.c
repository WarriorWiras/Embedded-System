#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include "sd_card.h"
#include "flash_benchmark.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// -----------------------------------------------------------
// Maker Pi Pico W SD Card Pin Configuration (GP10–GP15)
// (Wiring info for the built-in SD slot; low-level handled in diskio.c)
// -----------------------------------------------------------
#define SD_CS_PIN 15   // GP15 - CS (chip select)
#define SD_SCK_PIN 10  // GP10 - SCK (clock)
#define SD_MOSI_PIN 11 // GP11 - MOSI (controller -> card)
#define SD_MISO_PIN 12 // GP12 - MISO (card -> controller)

// -----------------------------------------------------------
// Button configuration (GP20)
// We'll use this button to trigger a batch of tests/logging.
// -----------------------------------------------------------
#define BUTTON_PIN 20 // GP20 - push button

// -----------------------------------------------------------
// CSV configuration
// - CSV_FILENAME: where we log results
// - DEBOUNCE_DELAY_MS: basic debounce for button press
// -----------------------------------------------------------
#define CSV_FILENAME "RESULTS.CSV"
#define DEBOUNCE_DELAY_MS 50

// -----------------------------------------------------------
// Global state flags/counters
// These track one-time setup status and logging progress.
// -----------------------------------------------------------
static bool sd_card_initialized = false;
static bool sd_card_mounted = false;
static bool csv_initialized = false;
static bool flash_chip_ready = false;
static int data_row_count = 0;        // how many data rows already in the CSV
static bool last_button_state = true; // button is pull-up -> true means not pressed
static uint32_t last_button_time = 0; // last time we saw a press (for debounce)

// -----------------------------------------------------------
// ADC configuration for temperature & supply voltage
// - Internal temperature sensor is on ADC channel 4
// - VSYS (board supply) is readable at ADC3 via a /3 divider
// -----------------------------------------------------------
#define ADC_TEMP_CHANNEL 4
#define ADC_VSYS_PIN 29
#define ADC_CONVERSION_FACTOR (3.3f / (1 << 12)) // 12-bit ADC -> volts
#define ADC_VOLTAGE_DIVIDER 3.0f                 // VSYS goes through /3 divider

// -----------------------------------------------------------
// Logging limits
// - TARGET_ROWS: how many total rows we aim to collect
// - MAX_TESTS_PER_PRESS: cap the number of test rows per button press
// -----------------------------------------------------------
#define TARGET_ROWS 1000
#define MAX_TESTS_PER_PRESS 20

// -----------------------------------------------------------
// Read internal temperature (°C) from ADC
// Formula from RP2040 datasheet; good enough for telemetry.
// -----------------------------------------------------------
float get_internal_temperature()
{
    adc_select_input(ADC_TEMP_CHANNEL);
    uint16_t raw = adc_read();
    const float conversion_factor = 3.3f / (1 << 12);
    float voltage = raw * conversion_factor;
    float temp = 27 - (voltage - 0.706f) / 0.001721f;
    return temp;
}

// -----------------------------------------------------------
// Read supply voltage (V):
// - ADC3 reads VSYS/3, so multiply back by 3
// -----------------------------------------------------------
float get_supply_voltage()
{
    adc_select_input(3); // VSYS/3 on ADC3
    uint16_t raw = adc_read();
    float voltage = raw * ADC_CONVERSION_FACTOR * ADC_VOLTAGE_DIVIDER;
    return voltage;
}

// -----------------------------------------------------------
// Create a simple timestamp string (HH:MM:SS since boot)
// NOTE: Date part is hardcoded to 2025-09-28 here as a label.
// -----------------------------------------------------------
void create_timestamp(char *buffer, size_t buffer_size)
{
    uint64_t time_us = to_us_since_boot(get_absolute_time());
    uint32_t seconds = (uint32_t)(time_us / 1000000ULL);
    uint32_t hours = seconds / 3600;
    uint32_t minutes = (seconds % 3600) / 60;
    uint32_t secs = seconds % 60;

    snprintf(buffer, buffer_size, "2025-09-28 %02lu:%02lu:%02lu",
             (unsigned long)hours, (unsigned long)minutes, (unsigned long)secs);
}

// -----------------------------------------------------------
// Ensure RESULTS.CSV exists and has the correct header.
// If file is missing, create it; if present, we just keep appending.
// -----------------------------------------------------------
bool initialize_csv_file()
{
    printf("\n# CSV FILE INITIALIZATION #\n");
    printf("================================\n");

    printf("# Checking CSV file status: %s\n", CSV_FILENAME);

    // Check if file exists already
    bool file_exists = sd_file_exists(CSV_FILENAME);

    if (!file_exists)
    {
        printf("# CSV file not found - creating new file\n");
        printf("# Creating forensic analysis CSV with comprehensive header\n");

        // sd_write_file(NULL) will create file and write header
        if (!sd_write_file(CSV_FILENAME, NULL))
        {
            printf("### CRITICAL ERROR: Failed to create CSV file!\n");
            printf("   Check SD card connection and FAT32 format\n");
            return false;
        }

        printf("# CSV file created successfully with header\n");

        // Verify creation
        if (!sd_file_exists(CSV_FILENAME))
        {
            printf("### VERIFICATION FAILED: CSV file not found after creation!\n");
            return false;
        }

        printf("# CSV file creation verified\n");
    }
    else
    {
        printf("# CSV file already exists - ready for appending\n");
        printf("# Will append new forensic data to existing file\n");
    }

    // Friendly banner confirming SD/FS expectations
    printf("\n# SD CARD VALIDATION (32GB FAT32)\n");
    printf("----------------------------------\n");
    printf("# SD Card: Initialized and mounted\n");
    printf("# File System: FAT32 compatible\n");
    printf("# Capacity: 32GB supported\n");
    printf("# CSV File: %s ready\n", file_exists ? "EXISTS" : "CREATED");

    csv_initialized = true;
    printf("================================\n");
    printf("# CSV INITIALIZATION COMPLETE #\n\n");

    return true;
}

// -----------------------------------------------------------
// Run a batch of tests (read/program/erase) across patterns,
// sizes, and addresses; log results to CSV.
// Stops early if we hit per-press cap or total target.
// -----------------------------------------------------------
void perform_forensic_analysis_and_log()
{
    // If we already finished the big target, don't add more rows.
    if (data_row_count >= TARGET_ROWS)
    {
        printf("# Target of %d entries already reached. Skipping logging.\n", TARGET_ROWS);
        return;
    }
    int logged_this_press = 0;

    printf("\n# STARTING COMPREHENSIVE FORENSIC ANALYSIS #\n");
    printf("===========================================\n");

    // Gather environment once per batch
    char timestamp_str[32];
    create_timestamp(timestamp_str, sizeof(timestamp_str));
    float temp = get_internal_temperature();
    float voltage = get_supply_voltage();

    // Try to identify flash chip; fall back to "No_Flash"
    char chip_id[64];
    if (flash_chip_ready)
    {
        if (!flash_identify_chip(chip_id, sizeof(chip_id)))
        {
            strcpy(chip_id, "Unknown_Flash");
        }
    }
    else
    {
        strcpy(chip_id, "Wafi");
    }

    printf("# System Status:\n");
    printf("   Temperature: %.2f°C\n", temp);
    printf("   Voltage: %.2fV\n", voltage);
    printf("   Flash Chip: %s\n", chip_id);
    printf("   Timestamp: %s\n", timestamp_str);

    // Test matrices: patterns × sizes × addresses × ops
    const char *test_patterns[] = {"0xFF", "0x00", "0x55", "random", "incremental"};
    const uint32_t test_sizes[] = {256, 512, 1024, 4096};
    const uint32_t test_addresses[] = {0x0000, 0x1000, 0x10000, 0x100000};
    const char *operations[] = {"read", "program", "erase"};

    int total_tests =
        (int)(sizeof(test_patterns) / sizeof(test_patterns[0])) *
        (int)(sizeof(test_sizes) / sizeof(test_sizes[0])) *
        (int)(sizeof(operations) / sizeof(operations[0]));

    printf("\n# Performing %d forensic tests...\n", total_tests);

    // Nested loops over operations, patterns, sizes, and addresses
    for (int op = 0; op < 3; op++) // 3 operations
    {
        for (int p = 0; p < 5; p++) // 5 patterns
        {
            for (int s = 0; s < 4; s++) // 4 sizes
            {
                for (int a = 0; a < 4; a++) // 4 addresses
                {
                    data_row_count++;

                    uint64_t elapsed_us = 0;
                    float throughput_MBps = 0.0f;
                    char notes[64];

                    printf("# Test %d: %s %s pattern, %d bytes at 0x%06X\n",
                           data_row_count, operations[op], test_patterns[p],
                           test_sizes[s], test_addresses[a]);

                    // If we have a real flash chip, call real benchmarks.
                    // Otherwise, simulate a plausible timing.
                    if (flash_chip_ready && strcmp(chip_id, "No_Flash") != 0)
                    {
                        switch (op)
                        {
                        case 0: // READ
                            elapsed_us = benchmark_flash_read(test_addresses[a], test_sizes[s], test_patterns[p]);
                            snprintf(notes, sizeof(notes), "Flash_Read_Test_%d", data_row_count);
                            break;
                        case 1: // PROGRAM (write)
                            elapsed_us = benchmark_flash_program(test_addresses[a], test_sizes[s], test_patterns[p]);
                            snprintf(notes, sizeof(notes), "Flash_Program_Test_%d", data_row_count);
                            break;
                        case 2: // ERASE
                            elapsed_us = benchmark_flash_erase(test_addresses[a], test_sizes[s]);
                            snprintf(notes, sizeof(notes), "Flash_Erase_Test_%d", data_row_count);
                            break;
                        }

                        // Calculate throughput in MB/s if time > 0
                        if (elapsed_us > 0)
                        {
                            float time_seconds = elapsed_us / 1000000.0f;
                            float size_MB = test_sizes[s] / (1024.0f * 1024.0f);
                            throughput_MBps = size_MB / time_seconds;
                        }
                    }
                    else
                    {
                        // Simulation path for when no external flash is attached
                        elapsed_us = (rand() % 10000) + 1000; // ~1–11 ms
                        throughput_MBps = (test_sizes[s] / 1024.0f / 1024.0f) / (elapsed_us / 1000000.0f);
                        snprintf(notes, sizeof(notes), "Simulated_%s_Test_%d", operations[op], data_row_count);
                    }

                    // Build CSV row (must match header order)
                    // chip_id,operation,block_size,address,elapsed_us,throughput_MBps,run,temp_C,voltage_V,pattern,timestamp,notes
                    char csv_row[512];
                    int len = snprintf(csv_row, sizeof(csv_row),
                                       "%s,%s,%u,0x%06X,%llu,%.3f,%d,%.2f,%.2f,%s,%s,%s",
                                       chip_id,
                                       operations[op],
                                       test_sizes[s],
                                       test_addresses[a],
                                       (unsigned long long)elapsed_us,
                                       throughput_MBps,
                                       data_row_count,
                                       temp,
                                       voltage,
                                       test_patterns[p],
                                       timestamp_str,
                                       notes);

                    // Only write if formatting succeeded and fits buffer
                    if (len > 0 && len < (int)sizeof(csv_row))
                    {
                        if (sd_append_to_file(CSV_FILENAME, csv_row))
                        {
                            printf("# Test %d logged: %.2f MB/s\n", data_row_count, throughput_MBps);
                            logged_this_press++;
                        }
                        else
                        {
                            printf("### Failed to log test %d\n", data_row_count);
                            data_row_count--; // roll back counter if append failed
                        }
                    }
                    else
                    {
                        printf("### CSV formatting error for test %d\n", data_row_count);
                        data_row_count--; // roll back counter if snprintf failed
                    }

                    // Small pause to avoid hammering the card/console
                    sleep_ms(100);

                    // Stop early if we hit caps
                    if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
                    {
                        break;
                    }
                }
                if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
                    break; // after 'a' loop
            }
            if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
                break; // after 's' loop
        }
        if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
            break; // after 'p' loop
    }

    // Summary for this button press
    printf("\n# Progress Report:\n");
    printf("   Total entries: %d\n", data_row_count);
    printf("   Target: %d entries\n", TARGET_ROWS);
    printf("   Progress: %.1f%% complete\n", (data_row_count * 100.0f) / TARGET_ROWS);

    if (data_row_count >= TARGET_ROWS)
    {
        printf("# Target of %d entries reached! System continues logging...\n", TARGET_ROWS);
    }
    else
    {
        printf("# Press GP20 again for more forensic analysis\n");
    }

    printf("===========================================\n");
    printf("# FORENSIC ANALYSIS COMPLETE #\n\n");
}

// -----------------------------------------------------------
// Handle a *single* GP20 press:
// - Ensure SD is initialized/mounted
// - Ensure CSV exists/checked
// - Check flash & environment
// - Run the batch of tests
// - Print summary
// -----------------------------------------------------------
void handle_gp20_button_press()
{
    printf("\n# GP20 BUTTON PRESSED - STARTING FORENSIC SEQUENCE #\n");
    printf("======================================================\n");
    printf("# Button press detected at: ");
    char timestamp[32];
    create_timestamp(timestamp, sizeof(timestamp));
    printf("%s\n", timestamp);

    printf("\n# SYSTEM VALIDATION PHASE\n");
    printf("--------------------------\n");

    // --- STEP 1: SD Card ready? If not, set it up.
    printf("# STEP 1: SD CARD SYSTEM CHECK\n");

    if (!sd_card_initialized || !sd_card_mounted)
    {
        printf("###  SD card not ready - performing full initialization\n");

        // Low-level "hello" (prints only; real init happens during mount)
        if (!sd_card_initialized)
        {
            printf("🔌 Initializing 32GB FAT32 SD card hardware...\n");
            if (!sd_card_init())
            {
                printf("### CRITICAL ERROR: SD card hardware initialization failed!\n");
                printf("   Solutions:\n");
                printf("   - Check SD card is properly inserted\n");
                printf("   - Verify SD card is FAT32 formatted\n");
                printf("   - Ensure 32GB capacity is supported\n");
                printf("   - Check Maker Pi Pico W SD card connections\n");
                return;
            }
            sd_card_initialized = true;
            printf("# SD card hardware initialized successfully\n");
        }

        // Mount the filesystem so we can use files
        if (!sd_card_mounted)
        {
            printf("# Mounting 32GB FAT32 filesystem...\n");
            if (!sd_mount())
            {
                printf("### CRITICAL ERROR: Filesystem mount failed!\n");
                printf("   Solutions:\n");
                printf("   - Format SD card as FAT32 on PC\n");
                printf("   - Check for file system corruption\n");
                printf("   - Try different SD card\n");
                return;
            }
            sd_card_mounted = true;
            printf("# 32GB FAT32 filesystem mounted successfully\n");

            // Refresh our row counter from existing CSV, if any
            int total_lines = 0, data_rows = 0;
            if (sd_count_csv_rows(CSV_FILENAME, &total_lines, &data_rows) == 0)
            {
                data_row_count = data_rows; // continue numbering after existing rows
                printf("# Existing CSV rows: total=%d, data=%d (next run=%d)\n",
                       total_lines, data_rows, data_row_count + 1);
            }
            else
            {
                printf("###  Could not count rows; continuing with data_row_count=%d\n",
                       data_row_count);
            }
        }
    }
    else
    {
        printf("# SD card already initialized and mounted\n");
    }

    // --- STEP 2: CSV file present and ready?
    printf("\n# STEP 2: CSV FILE SYSTEM CHECK\n");

    if (!csv_initialized)
    {
        printf("# Initializing CSV file system...\n");
        if (!initialize_csv_file())
        {
            printf("### CRITICAL ERROR: CSV file system initialization failed!\n");
            printf("   The forensic analysis cannot proceed without CSV logging\n");
            return;
        }
    }
    else
    {
        printf("# CSV file system already ready\n");

        // Double-check the file still exists; recreate if needed
        if (!sd_file_exists(CSV_FILENAME))
        {
            printf("###  CSV file missing - recreating...\n");
            csv_initialized = false; // Force re-init
            if (!initialize_csv_file())
            {
                printf("### CRITICAL ERROR: Failed to recreate CSV file!\n");
                return;
            }
        }
        else
        {
            printf("# CSV file verified: %s exists\n", CSV_FILENAME);
        }
    }

    // Re-count rows to keep run numbers consistent after card reinsert
    {
        int total_lines = 0, data_rows = 0;
        if (sd_count_csv_rows(CSV_FILENAME, &total_lines, &data_rows) == 0)
        {
            printf("# Existing entries in %s: %d data rows (total lines: %d)\n",
                   CSV_FILENAME, data_rows, total_lines);
            if (data_rows > data_row_count)
            {
                data_row_count = data_rows;
            }
        }
        else
        {
            printf("###  Could not count rows in %s (will continue with current counter: %d)\n",
                   CSV_FILENAME, data_row_count);
        }
    }

    // --- STEP 3: Flash present? Identify chip if possible.
    printf("\n# STEP 3: FLASH FORENSIC SYSTEM CHECK\n");

    if (flash_chip_ready)
    {
        char chip_id[64];
        if (flash_identify_chip(chip_id, sizeof(chip_id)))
        {
            printf("# Flash chip identified: %s\n", chip_id);
            printf("# Real flash forensic analysis will be performed\n");
        }
        else
        {
            printf("###  Flash chip identity unknown - using generic analysis\n");
        }
    }
    else
    {
        printf("###  No flash chip detected - simulated forensic analysis will be used\n");
        printf("   This is normal if external flash is not connected\n");
    }

    // --- STEP 4: Check environment sensors (temp/voltage) for sanity.
    printf("\n# STEP 4: ENVIRONMENTAL MONITORING CHECK\n");
    float temp = get_internal_temperature();
    float voltage = get_supply_voltage();
    printf("# Temperature sensor: %.2f°C\n", temp);
    printf("# Voltage monitor: %.2fV\n", voltage);

    if (temp < -10 || temp > 85)
    {
        printf("###  Temperature outside normal range (-10°C to 85°C)\n");
    }
    if (voltage < 2.7 || voltage > 5.5)
    {
        printf("###  Voltage outside normal range (2.7V to 5.5V)\n");
    }

    // --- STEP 5: Do the actual batch of tests and log results.
    printf("\n======================================================\n");
    printf("# STARTING FORENSIC DATA COLLECTION #\n");
    printf("======================================================\n");

    perform_forensic_analysis_and_log();

    // STEP 5: Execute Forensic Analysis
    perform_forensic_analysis_and_log();

    // --- Print overall average temperature from RESULTS.CSV ---
    {
        double avg_temp = 0.0;
        int n = 0;
        if (sd_compute_avg_temp(CSV_FILENAME, &avg_temp, &n))
        {
            if (n > 0)
            {
                printf("#  Average temperature so far (over %d entries): %.2f°C\n", n, avg_temp);
            }
            else
            {
                printf("#  No temperature samples yet in %s\n", CSV_FILENAME);
            }
        }
        else
        {
            printf("### Could not compute average temperature from %s\n", CSV_FILENAME);
        }
    }

    // --- STEP 6: Post-check & summary
    printf("\n# FINAL VALIDATION & SUMMARY\n");
    printf("-----------------------------\n");

    if (sd_file_exists(CSV_FILENAME))
    {
        printf("# CSV file verified after analysis\n");
        printf("# Total forensic entries logged: %d\n", data_row_count);
        printf("# Target progress: %.1f%% (%d entries target)\n",
               (data_row_count * 100.0f) / TARGET_ROWS, TARGET_ROWS);
        if (data_row_count >= TARGET_ROWS)
        {
            printf("# MILESTONE: %d+ forensic entries completed!\n", TARGET_ROWS);
        }
    }
    else
    {
        printf("### ERROR: CSV file missing after analysis!\n");
    }

    printf("\n======================================================\n");
    printf("# FORENSIC SEQUENCE COMPLETE - GP20 READY FOR NEXT PRESS #\n");
    printf("======================================================\n\n");
}

int main()
{
    // -------------------------------------------------------
    // Bring up stdio (USB/UART) and give host a moment to attach
    // -------------------------------------------------------
    stdio_init_all();
    sleep_ms(2000);

    // Banner
    printf("\n");
    printf("████████████████████████████████████████████████████████████\n");
    printf("█  MAKER PI PICO W - FLASH MEMORY FORENSIC ANALYSIS SYSTEM  █\n");
    printf("████████████████████████████████████████████████████████████\n");
    printf("█ Version: 2.0 - Comprehensive Analysis & CSV Logging       █\n");
    printf("█ Hardware: Raspberry Pi Pico W + 32GB FAT32 SD Card        █\n");
    printf("█ Target: Real flash chip forensic benchmarking             █\n");
    printf("████████████████████████████████████████████████████████████\n\n");

    // -------------------------------------------------------
    // ADC: temperature & voltage monitoring setup
    // -------------------------------------------------------
    printf("# SYSTEM INITIALIZATION\n");
    printf("========================\n");

    printf("# Initializing ADC for environmental monitoring...\n");
    adc_init();
    adc_gpio_init(ADC_VSYS_PIN);       // prepare VSYS/3 pin
    adc_set_temp_sensor_enabled(true); // enable internal temp sensor
    printf("# ADC initialized - temperature and voltage monitoring ready\n");

    // -------------------------------------------------------
    // Button: configure input with pull-up (active low)
    // -------------------------------------------------------
    printf("# Configuring GP20 button interface...\n");
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    printf("# GP20 button configured with pull-up resistor\n");
    printf("# Press GP20 to trigger forensic analysis\n");

    // -------------------------------------------------------
    // Flash: initialize interface; if not present, we simulate.
    // -------------------------------------------------------
    printf("\n# FLASH MEMORY FORENSIC SYSTEM\n");
    printf("===============================\n");

    printf("# Initializing external flash chip interface...\n");
    if (flash_benchmark_init())
    {
        flash_chip_ready = true;
        printf("# Flash chip hardware interface ready\n");

        // Try to identify chip
        char chip_id[64];
        if (flash_identify_chip(chip_id, sizeof(chip_id)))
        {
            printf("# Flash chip identified: %s\n", chip_id);
            printf("# Real flash forensic analysis will be performed\n");
        }
        else
        {
            printf("### Flash chip identity unknown - using generic analysis\n");
        }
    }
    else
    {
        printf("### No external flash chip detected\n");
        printf("   Forensic analysis will use simulation mode\n");
        printf("   This is normal if external flash is not connected\n");
    }

    // -------------------------------------------------------
    // Show initial environment status
    // -------------------------------------------------------
    printf("\n# ENVIRONMENTAL MONITORING\n");
    printf("============================\n");

    float temp = get_internal_temperature();
    float voltage = get_supply_voltage();
    printf("#  Current temperature: %.2f°C\n", temp);
    printf("# Supply voltage: %.2fV\n", voltage);

    if (temp >= -10 && temp <= 85)
    {
        printf("# Temperature within operating range (-10°C to 85°C)\n");
    }
    else
    {
        printf("###  Temperature outside recommended range\n");
    }

    if (voltage >= 2.7 && voltage <= 5.5)
    {
        printf("# Voltage within operating range (2.7V to 5.5V)\n");
    }
    else
    {
        printf("### Voltage outside recommended range\n");
    }

    // -------------------------------------------------------
    // CSV logging expectations & instructions
    // -------------------------------------------------------
    printf("\n# CSV FORENSIC DATA LOGGING SYSTEM\n");
    printf("===================================\n");
    printf("# Target file: %s\n", CSV_FILENAME);
    printf("# Format: CSV with comprehensive forensic data\n");
    printf("# Storage: 32GB FAT32 SD Card (Windows compatible)\n");
    printf("# Target: %d forensic analysis entries\n", TARGET_ROWS);
    printf("# Mode: Append to existing file or create new\n");

    printf("\n# SD CARD REQUIREMENTS\n");
    printf("======================\n");
    printf("# Capacity: 32GB (confirmed compatible)\n");
    printf("# Format: FAT32 (Windows/Mac/Linux readable)\n");
    printf("# Connection: Maker Pi Pico W SD card slot (GP10-GP15)\n");
    printf("#  IMPORTANT: Insert formatted 32GB FAT32 SD card before testing\n");

    printf("\n# SYSTEM STATUS: READY FOR OPERATION\n");
    printf("=====================================\n");
    printf("# Hardware: Maker Pi Pico W initialized\n");
    printf("# Button: GP20 configured and ready\n");
    printf("# Flash: %s\n", flash_chip_ready ? "Ready for real analysis" : "Simulation mode ready");
    printf("# Environmental: Temperature and voltage monitoring active\n");
    printf("# SD Card: Will be initialized on first GP20 press\n");

    printf("\n# OPERATION INSTRUCTIONS\n");
    printf("=========================\n");
    printf("1 Insert 32GB FAT32 formatted SD card\n");
    printf("2 Press GP20 button to start forensic analysis\n");
    printf("3 Each press performs comprehensive flash testing\n");
    printf("4 Results automatically saved to RESULTS.CSV\n");
    printf("5 File is Windows compatible - remove SD card to view on PC\n");
    printf("6 System handles file creation, existence checks, and appending\n");

    printf("\n# Waiting for GP20 button press to begin forensic analysis...\n\n");

    // Random seed used for simulation mode timings
    srand(to_ms_since_boot(get_absolute_time()));

    // -------------------------------------------------------
    // Main loop: watch the button with debounce,
    // run the analysis when pressed, print heartbeat every 30s.
    // -------------------------------------------------------
    printf("# System entering main operational loop\n");
    printf("   Monitoring GP20 for forensic analysis trigger...\n\n");

    while (1)
    {
        bool current_button_state = gpio_get(BUTTON_PIN);
        uint32_t current_time = to_ms_since_boot(get_absolute_time());

        // Detect falling edge (pressed) with debounce window
        if (last_button_state && !current_button_state &&
            (current_time - last_button_time) > DEBOUNCE_DELAY_MS)
        {
            printf("\n# GP20 BUTTON PRESS DETECTED! #\n");
            printf("# Timestamp: ");
            char press_time[32];
            create_timestamp(press_time, sizeof(press_time));
            printf("%s\n", press_time);

            // Run full sequence (checks + tests + summary)
            handle_gp20_button_press();

            // Print the entire CSV to the serial monitor
            printf("\n# Dumping %s to serial...\n", CSV_FILENAME);
            if (!sd_print_file(CSV_FILENAME))
            {
                printf("### Could not dump %s\n", CSV_FILENAME);
            }

            printf("\n# Forensic analysis complete - system ready for next press\n");
            printf("# Current progress: %d entries logged\n", data_row_count);
            if (data_row_count > 0)
            {
                printf("# Target completion: %.1f%% (%d entries target)\n",
                       (data_row_count * 100.0f) / TARGET_ROWS, TARGET_ROWS);
            }
            printf("# Press GP20 again to continue forensic analysis...\n\n");

            last_button_time = current_time;
        }

        // Remember state for edge detection next iteration
        last_button_state = current_button_state;

        // Heartbeat/status print every ~30 seconds
        static uint32_t last_heartbeat = 0;
        if ((current_time - last_heartbeat) > 30000) // 30 s
        {
            printf("# System heartbeat - GP20 monitoring active (entries: %d)\n", data_row_count);

            // Show current environmental snapshot
            float current_temp = get_internal_temperature();
            float current_voltage = get_supply_voltage();
            printf("   Temperature: %.1f°C | Voltage: %.2fV\n", current_temp, current_voltage);

            last_heartbeat = current_time;
        }

        // Small sleep keeps CPU usage low without missing button presses
        sleep_ms(10);
    }

    return 0;
}
