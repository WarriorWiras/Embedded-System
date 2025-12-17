#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "pico/time.h"
#include "fatfs/ff.h"

#include "sd_card.h"
#include "flash_benchmark.h"
#include "bench_read.h"
#include "bench_write.h"
#include "bench_erase.h"
#include "report.h"
#include "web/http_server.h"
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "dhcpserver/dhcpserver.h"
#include "config/config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ======================== Hardware / App Constants ======================== */
/* Maker Pi Pico W — SD slot on GP10..GP15 (your low-level driver already uses these) */
#define SD_CS_PIN 15   // GP15 CSn
#define SD_SCK_PIN 10  // GP10 SCK
#define SD_MOSI_PIN 11 // GP11 MOSI
#define SD_MISO_PIN 12 // GP12 MISO

/* Button */
/* Buttons */
#define BUTTON_PIN 20         // GP20 - run analysis
#define RESTORE_BUTTON_PIN 21 // GP21 - restore from backup


/* ADC (internal temp + VSYS/3 on ADC3 via GPIO29) */
#define ADC_TEMP_CHANNEL 4
#define ADC_VSYS_PIN 29
#define ADC_VSYS_CHANNEL 3
#define ADC_CONV (3.3f / (1 << 12))
#define ADC_VSYS_DIV 3.0f

/* CSV / logging */
#define CSV_FILENAME "RESULTS.CSV"
#define TARGET_ROWS 1000
#define MAX_TESTS_PER_PRESS 20
#define DEBOUNCE_DELAY_MS 50
#define HEARTBEAT_MS 30000

/* ============================== Global State ============================== */
static bool sd_card_initialized = false;
static bool sd_card_mounted = false;
static bool csv_initialized = false;
static bool flash_chip_ready = false;

static int data_row_count = 0;

static bool last_button_state_gp20 = true; // pull-up => idle high
static bool last_button_state_gp21 = true; // pull-up => idle high
static uint32_t last_button_time_gp20 = 0;
static uint32_t last_button_time_gp21 = 0;

// File list used by HTTP server
static sd_file_info_t sd_files[MAX_FILES_TO_LIST];
static int sd_file_count = 0;
static bool file_list_needs_refresh = true;

// Adapter used by the HTTP server implementation
int http_get_file_list(sd_file_info_t *files, int max_files)
{
    return sd_get_file_list(files, max_files);
}


// Forward declarations for internal helpers (they are defined later in this file)
static inline float get_internal_temperature(void);
static inline float get_supply_voltage(void);

// Adapters used by the HTTP server (forward to existing functions)
float http_get_temperature(void)
{
    return get_internal_temperature();
}

float http_get_voltage(void)
{
    return get_supply_voltage();
}

bool http_get_sd_mounted(void)
{
    return sd_is_mounted();
}

/* ============================ Resource Checks ============================= */
static inline void print_menu_banner(void)
{
    printf("\n================= ANALYSIS MENU =================\n");
    printf("Type one of these commands then press Enter:\n");
    printf("   safe         - Safe analysis (read-only)\n");
    printf("   destructive  - Destructive analysis (read + write/erase)\n");
    printf("   exit         - Exit and generate report\n");
    printf("=================================================\n");
}
/* Trim, lowercase utility (used by simple line reader variants) */
static void strtolower_trim(char *s)
{
    // ltrim
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        ++i;
    if (i)
        memmove(s, s + i, strlen(s + i) + 1);

    // rtrim
    size_t len = strlen(s);
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';

    // to lower
    for (size_t j = 0; j < len; ++j)
    {
        if (s[j] >= 'A' && s[j] <= 'Z')
            s[j] = (char)(s[j] - 'A' + 'a');
    }
}

static bool read_command_gap_terminated(char *out, size_t out_n)
{
    size_t i = 0;
    bool got_any = false;
    absolute_time_t deadline = make_timeout_time_ms(200);

    for (;;)
    {
        int ch = getchar_timeout_us(2000); // ~2ms poll
        if (ch >= 0)
        {
            got_any = true;
            deadline = make_timeout_time_ms(200);

            if (ch == '\r' || ch == '\n')
                break; // explicit end
            if (ch == 0x08 || ch == 0x7F)
            {
                if (i)
                    --i;
                continue;
            } // backspace
            if (ch < 32)
                continue;

            if (i < out_n - 1)
                out[i++] = (char)ch;
        }
        else
        {
            if (got_any && absolute_time_diff_us(deadline, get_absolute_time()) <= 0)
                break; // idle end
        }
    }
    out[i] = '\0';
    strtolower_trim(out);
    return (out[0] != '\0');
}

static void input_flush(void)
{
    while (getchar_timeout_us(0) >= 0)
    { /* discard */
    }
}

/* Blocking yes/no prompt: returns true for yes, false for no */
static bool prompt_yes_no(const char *question)
{
    input_flush();
    for (;;)
    {
        printf("%s (y/n): ", question);
        fflush(stdout);
        char raw[8] = {0};

        // reuse your gap-terminated reader (lowercases + trims)
        if (!read_command_gap_terminated(raw, sizeof raw))
        {
            // no token yet -> just loop until we get y/n
            continue;
        }

        if (raw[0] == 'y')
        {
            printf("y\n");
            return true;
        }
        if (raw[0] == 'n')
        {
            printf("n\n");
            return false;
        }

        printf("Please type 'y' or 'n'.\n");
    }
}

static bool ensure_sd_ready(void)
{
    // Initialize controller once
    if (!sd_card_initialized)
    {
        if (!sd_card_init())
        {
            printf("⛔ SD init failed (card missing or wiring?).\n");
            return false;
        }
        sd_card_initialized = true;
    }
    // Mount filesystem
    if (!sd_card_mounted)
    {
        if (!sd_mount())
        {
            printf("⛔ SD mount failed (not FAT32 or I/O error).\n");
            return false;
        }
        sd_card_mounted = true;
    }
    return true;
}

/* Quick live probe: return true only if JEDEC reads as a non-0x00/0xFF mfr */
static bool flash_is_live_now(char *out, size_t n)
{
    uint8_t m = 0, d1 = 0, d2 = 0;
    if (!flash_read_jedec_id(&m, &d1, &d2))
    {
        if (out && n)
            snprintf(out, n, "No / Unknown_Flash");
        return false;
    }
    if (m == 0x00 || m == 0xFF)
    {
        if (out && n)
            snprintf(out, n, "No / Unknown_Flash");
        return false;
    }
    if (out && n)
        snprintf(out, n, "%02X %02X %02X", m, d1, d2);
    return true;
}

/* Return true only if flash reports a valid JEDEC ID */
static bool flash_has_valid_jedec(char *out, size_t n)
{
    char id[24] = {0};
    flash_get_jedec_str(id, sizeof id);

    if (out && n)
    {
        snprintf(out, n, "%s", id);
        out[n - 1] = '\0';
    }

    if (!strcmp(id, "No / Unknown_Flash") || id[0] == '\0')
    {
        printf("⛔ No valid JEDEC ID detected. Check wiring/power/CS.\n");
        return false;
    }
    return true;
}

static bool ensure_flash_ready(void)
{
    // If main() didn’t manage to bring it up for some reason, try once here
    if (!flash_chip_ready)
    {
        printf("ℹ️  Flash not marked ready — attempting soft re-probe…\n");
        if (flash_benchmark_init())
        {
            flash_chip_ready = true;
        }
        else
        {
            printf("⛔ Flash init failed — no chip detected.\n");
            return false;
        }
    }

    char jedec[24];
    flash_get_jedec_str(jedec, sizeof jedec);
    if (!strcmp(jedec, "No / Unknown_Flash"))
    {
        printf("⛔ Flash JEDEC unknown — is the chip wired/powered?\n");
        return false;
    }
    printf("✅ Flash present: JEDEC %s\n", jedec);
    return true;
}
/* ============================== CSV Handling ============================== */
static bool initialize_csv_file(void)
{
    printf("\n📋 CSV FILE INITIALIZATION 📋\n");
    printf("================================\n");
    printf("🔍 Checking CSV file status: %s\n", CSV_FILENAME);

    bool exists = sd_file_exists(CSV_FILENAME);
    if (!exists)
    {
        printf("📝 CSV not found — creating with header…\n");
        if (!sd_write_file(CSV_FILENAME, NULL))
        {
            printf("❌ CRITICAL: Failed to create CSV. Check SD & FAT32.\n");
            return false;
        }
        if (!sd_file_exists(CSV_FILENAME))
        {
            printf("❌ VERIFICATION FAILED: CSV missing after create.\n");
            return false;
        }
        printf("✅ CSV created + header written\n");
    }
    else
    {
        printf("✅ CSV exists — will append\n");
    }

    printf("\n🔧 SD CARD VALIDATION (32GB FAT32)\n");
    printf("----------------------------------\n");
    printf("✅ SD: initialized + mounted\n");
    printf("✅ FS: FAT32\n");
    printf("✅ Capacity: 32GB supported\n");
    printf("✅ CSV File: %s\n", exists ? "EXISTS" : "CREATED");

    csv_initialized = true;
    printf("================================\n");
    printf("📋 CSV INITIALIZATION COMPLETE 📋\n\n");
    return true;
}

static bool ensure_csv_ready(void)
{
    if (csv_initialized)
        return true;
    if (!initialize_csv_file())
    {
        printf("⛔ CSV init failed.\n");
        return false;
    }
    csv_initialized = true;
    int total = 0, data = 0;
    if (sd_count_csv_rows(CSV_FILENAME, &total, &data) == 0)
    {
        data_row_count = data;
        printf("📊 Existing rows: %d (next #%d)\n", data_row_count, data_row_count + 1);
    }
    return true;
}

/* ============================== Small Helpers ============================= */
static inline float get_internal_temperature(void)
{
    adc_select_input(ADC_TEMP_CHANNEL);
    uint16_t raw = adc_read();
    float v = raw * ADC_CONV;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

static inline float get_supply_voltage(void)
{
    adc_select_input(ADC_VSYS_CHANNEL); // <-- select ADC3 (GPIO29, VSYS/3)
    uint16_t raw = adc_read();
    return raw * ADC_CONV * ADC_VSYS_DIV; // ADC_CONV = 3.3/4096, ADC_VSYS_DIV = 3.0
}

/* Uptime-based timestamp: 0000-00-00 HH:MM:SS (no RTC on Pico by default) */
static inline void create_timestamp(char *buf, size_t n)
{
    uint64_t us = to_us_since_boot(get_absolute_time());
    uint32_t s = (uint32_t)(us / 1000000ULL);
    uint32_t hh = s / 3600;
    uint32_t mm = (s % 3600) / 60;
    uint32_t ss = s % 60;
    /* Keep the same format you were using for compatibility of CSV parsing */
    snprintf(buf, n, "2025-09-28 %02lu:%02lu:%02lu",
             (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
}

/* Normalize one-letter aliases */
static const char *normalize_cmd(const char *cmd)
{
    if (!strcmp(cmd, "safe") || !strcmp(cmd, "s"))
        return "safe";
    if (!strcmp(cmd, "destructive") || !strcmp(cmd, "d"))
        return "destructive";

    if (!strcmp(cmd, "write") || !strcmp(cmd, "w"))
        return "write";
    if (!strcmp(cmd, "erase") || !strcmp(cmd, "e"))
        return "erase";

    if (!strcmp(cmd, "exit") || !strcmp(cmd, "quit") || !strcmp(cmd, "q"))
        return "exit";

    return cmd;
}

/* Gap-terminated command reader: newline or 200 ms idle ends input */

/* ============================ ONE TEST + LOG ============================== */
/* Define BEFORE the menu so there is no implicit declaration */
static void run_one_test_and_log(const char *operation)
{
    /* SD must remain live (no auto-mount) */
    if (!sd_is_mounted())
    {
        printf("⛔ microSD not live (not mounted). Cannot run %s.\n", operation);
        return;
    }

    /* Flash must be live right now */
    char jedec[24];
    if (!flash_is_live_now(jedec, sizeof jedec))
    {
        printf("⛔ Microchip is not live. %s aborted. (Chip disconnected.)\n", operation);
        return;
    }

    /* Warn + confirm for WRITE and ERASE */
    if (!strcmp(operation, "write") || !strcmp(operation, "erase"))
    {
        printf("⚠️  This will MODIFY the microchip. Proceed? (yes/no): ");
        char answer[8] = {0};
        if (!read_command_gap_terminated(answer, sizeof answer))
        {
            printf("❌ No response. Aborting.\n");
            return;
        }
        if (strncmp(answer, "y", 1) != 0)
        {
            printf("↩️  Cancelled. Back to menu.\n");
            return;
        }

        /* Re-check live again right before the destructive action */
        if (!flash_is_live_now(jedec, sizeof jedec))
        {
            printf("⛔ Microchip is not live anymore. %s aborted.\n", operation);
            return;
        }
    }

    /* Prepare test params */
    const uint32_t addr = 0x0000;
    const uint32_t size = 4096;
    const char *pattern = "incremental";

    float temp = get_internal_temperature();
    float voltage = get_supply_voltage();

    // --- measure wall time around the operation ---
    uint64_t t0 = time_us_64();
    uint64_t ret_us = 0;

    if (strcmp(operation, "read") == 0)
    {
        ret_us = benchmark_flash_read(addr, size, pattern);
    }
    else if (strcmp(operation, "write") == 0)
    {
        ret_us = benchmark_flash_program(addr, size, pattern);
    }
    else if (strcmp(operation, "erase") == 0)
    {
        ret_us = benchmark_flash_erase(addr, size);
    }
    else
    {
        printf("❓ Unknown op: \"%s\"\n", operation);
        return;
    }

    uint64_t wall_us = time_us_64() - t0;

    // If the benchmark returns 0, fall back to the measured wall time
    uint64_t elapsed_us = (ret_us > 0) ? ret_us : wall_us;

    // --- compute throughput for read/write ---
    float throughput_MBps = 0.0f;
    if (elapsed_us > 0 && strcmp(operation, "erase") != 0)
    {
        float secs = (float)elapsed_us / 1e6f;
        float mb = (float)size / (1024.0f * 1024.0f);
        throughput_MBps = (secs > 0.0f) ? (mb / secs) : 0.0f;
    }

    char ts[32];
    create_timestamp(ts, sizeof ts);

    char row[256];
    printf("🧾 Using live JEDEC for CSV: [%s]\n", jedec);
    snprintf(row, sizeof(row),
             "%s,%s,%u,0x%06X,%llu,%.3f,%d,%.2f,%.2f,%s,%s,%s",
             jedec, operation, size, addr,
             (unsigned long long)elapsed_us, throughput_MBps,
             ++data_row_count, temp, voltage,
             (strcmp(operation, "write") == 0 ? pattern : "n/a"),
             ts, "menu_cmd");

    if (sd_append_to_file(CSV_FILENAME, row))
    {
        printf("✅ Logged to %s\n", CSV_FILENAME);
    }
    else
    {
        printf("❌ Failed to append to %s\n", CSV_FILENAME);
        --data_row_count;
    }
}

static void show_sd_menu_and_handle(void)
{
    // Optional: hard block if SD not mounted (since you re-enabled error handling)
    if (!sd_is_mounted())
    {
        printf("⛔ SD not live (not mounted). Analysis menu disabled.\n");
        return;
    }

    for (;;)
    {
        print_menu_banner();

        printf("> ");
        fflush(stdout);

        char raw[64] = {0};
        if (!read_command_gap_terminated(raw, sizeof raw))
        {
            sleep_ms(40);
            continue;
        }

        const char *cmd = normalize_cmd(raw);

        // ======================= Scenario A: SAFE =======================
        if (!strcmp(cmd, "safe"))
        {
            printf("\n🛡️ SAFE ANALYSIS selected.\n");
            printf("➡️ Performing READ benchmark...\n\n");

            bench_read_run_100(/*confirm_whole_chip=*/true);

            if (bench_read_has_data())
            {
                bench_read_print_summary();
            }
            else
            {
                printf("(no READ benchmark data)\n");
            }

            // After safe analysis: ask whether to exit
            if (prompt_yes_no("\nDo you want to exit?"))
            {
                printf("\n📑 Generating report.csv before exit…\n");
                report_generate_csv();
                printf("✅ report.csv written to SD (if mounted).\n");
                return;
            }
            else
            {
                // Back to 1.) — top-level menu again
                continue;
            }
        }

        // ================= Scenario B: DESTRUCTIVE =====================
        if (!strcmp(cmd, "destructive"))
        {
            printf("\n⚠️ DESTRUCTIVE ANALYSIS selected.\n");
            printf("➡️ Performing READ benchmark first...\n\n");

            bench_read_run_100(/*confirm_whole_chip=*/true);

            if (bench_read_has_data())
            {
                bench_read_print_summary();
            }
            else
            {
                printf("(no READ benchmark data)\n");
            }

            // Now ask for initial destructive action: write or erase
            char action_raw[64] = {0};
            const char *action = NULL;

            for (;;)
            {
                printf("\nChoose destructive action (write / erase): ");
                fflush(stdout);

                memset(action_raw, 0, sizeof action_raw);
                if (!read_command_gap_terminated(action_raw, sizeof action_raw))
                {
                    sleep_ms(40);
                    continue;
                }

                const char *a = normalize_cmd(action_raw);

                if (!strcmp(a, "write"))
                {
                    action = "write";
                    break;
                }
                if (!strcmp(a, "erase"))
                {
                    action = "erase";
                    break;
                }

                printf("Please type 'write' or 'erase'.\n");
            }

            // ---------------- Scenario B1: WRITE first ----------------
            if (!strcmp(action, "write"))
            {
                printf("\n✍️ WRITE selected.\n");
                printf("➡️ Performing WRITE benchmark...\n\n");

                bench_write_run_100(/*confirm_whole_chip=*/true, "incremental");

                if (bench_write_has_data())
                {
                    bench_write_print_summary();
                }
                else
                {
                    printf("(no WRITE benchmark data)\n");
                }

                // Ask if user wants to ERASE next (B1a/B1b)
                if (prompt_yes_no("\nDo you want to ERASE data next?"))
                {
                    // ===== Scenario B1a: yes -> erase + summary + exit-or-loop =====
                    printf("\n🧨 Performing ERASE benchmark...\n\n");
                    bench_erase_run_100(/*confirm_whole_chip=*/true);

                    if (bench_erase_has_data())
                    {
                        bench_erase_print_summary();
                    }
                    else
                    {
                        printf("(no ERASE benchmark data)\n");
                    }

                    // After erase summary, ask whether to exit or go back to menu
                    if (prompt_yes_no("\nDo you want to exit?"))
                    {
                        printf("\n📑 Generating report.csv before exit…\n");
                        report_generate_csv();
                        printf("✅ report.csv written to SD (if mounted).\n");
                        return;
                    }
                    else
                    {
                        // Back to 1.) — top-level menu again
                        continue;
                    }
                }
                else
                {
                    // ===== Scenario B1b: no -> auto exit =====
                    printf("⏭️  Erase skipped. Auto-exiting.\n");
                    printf("\n📑 Generating report.csv before exit…\n");
                    report_generate_csv();
                    printf("✅ report.csv written to SD (if mounted).\n");
                    return;
                }
            }

            // ---------------- Scenario B2: ERASE first ----------------
            if (!strcmp(action, "erase"))
            {
                printf("\n🧨 ERASE selected.\n");
                printf("➡️ Performing ERASE benchmark...\n\n");

                bench_erase_run_100(/*confirm_whole_chip=*/true);

                if (bench_erase_has_data())
                {
                    bench_erase_print_summary();
                }
                else
                {
                    printf("(no ERASE benchmark data)\n");
                }

                // Ask if user wants to WRITE next (B2a/B2b)
                if (prompt_yes_no("\nDo you want to WRITE data next?"))
                {
                    // ===== Scenario B2a: yes -> write + summary + exit-or-loop =====
                    printf("\n✍️ Performing WRITE benchmark...\n\n");
                    bench_write_run_100(/*confirm_whole_chip=*/true, "incremental");

                    if (bench_write_has_data())
                    {
                        bench_write_print_summary();
                    }
                    else
                    {
                        printf("(no WRITE benchmark data)\n");
                    }

                    // After write summary, ask whether to exit or go back to menu
                    if (prompt_yes_no("\nDo you want to exit?"))
                    {
                        printf("\n📑 Generating report.csv before exit…\n");
                        report_generate_csv();
                        printf("✅ report.csv written to SD (if mounted).\n");
                        return;
                    }
                    else
                    {
                        // Back to 1.) — top-level menu again
                        continue;
                    }
                }
                else
                {
                    // ===== Scenario B2b: no -> auto exit =====
                    printf("✋ WRITE skipped. Auto-exiting.\n");
                    printf("\n📑 Generating report.csv before exit…\n");
                    report_generate_csv();
                    printf("✅ report.csv written to SD (if mounted).\n");
                    return;
                }
            }

            // defensive: continue loop even if somehow no action matched
            continue;
        }

        // ============================ EXIT ============================
        if (!strcmp(cmd, "exit"))
        {
            printf("👋 Exiting analysis menu.\n");
            printf("\n📑 Generating report.csv before exit…\n");
            report_generate_csv();
            printf("✅ report.csv written to SD (if mounted).\n");
            return;
        }

        // Fallback: unknown top-level command
        printf("❓ Unknown command: %s (use safe | destructive | exit)\n", raw);
    }
}


/* ================ (Optional) Matrix Forensics Driver ================= */
void perform_forensic_analysis_and_log(void)
{
    if (!sd_card_initialized || !sd_card_mounted)
    {
        printf("⛔ SD card not ready (not detected or not mounted). Skipping forensics.\n");
        return;
    }
    if (!flash_chip_ready)
    {
        printf("⛔ Flash chip not detected/ready. Skipping forensics.\n");
        return;
    }
    if (data_row_count >= TARGET_ROWS)
    {
        printf("ℹ️ Target of %d entries already reached. Skipping logging.\n", TARGET_ROWS);
        return;
    }

    int logged_this_press = 0;

    printf("\n🔍 STARTING COMPREHENSIVE FORENSIC ANALYSIS 🔍\n");
    printf("===========================================\n");

    char timestamp_str[32];
    create_timestamp(timestamp_str, sizeof(timestamp_str));
    float temp = get_internal_temperature();
    float voltage = get_supply_voltage();

    char chip_id[24];
    flash_get_jedec_str(chip_id, sizeof(chip_id));
    if (!strcmp(chip_id, "No / Unknown_Flash"))
    {
        printf("⛔ Flash JEDEC unknown. Aborting forensics.\n");
        return;
    }

    printf("📊 System Status:\n");
    printf("   Temperature: %.2f°C\n", temp);
    printf("   Voltage: %.2fV\n", voltage);
    printf("   Flash Chip: %s\n", chip_id);
    printf("   Timestamp: %s\n", timestamp_str);

    const char *test_patterns[] = {"0xFF", "0x00", "0x55", "random", "incremental"};
    const uint32_t test_sizes[] = {256, 512, 1024, 4096};
    const uint32_t test_addresses[] = {0x0000, 0x1000, 0x10000, 0x100000};
    const char *operations[] = {"read", "program", "erase"};
    int total_tests = (int)(sizeof(test_patterns) / sizeof(test_patterns[0]) *
                            sizeof(test_sizes) / sizeof(test_sizes[0]) *
                            sizeof(operations) / sizeof(operations[0]));
    printf("\n🧪 Performing %d forensic tests...\n", total_tests);

    for (int op = 0; op < 3; op++)
    {
        for (int p = 0; p < 5; p++)
        {
            for (int s = 0; s < 4; s++)
            {
                for (int a = 0; a < 4; a++)
                {
                    data_row_count++;

                    uint64_t elapsed_us = 0;
                    float throughput_MBps = 0.0f;
                    char notes[64];

                    printf("🔬 Test %d: %s %s pattern, %d bytes at 0x%06X\n",
                           data_row_count, operations[op], test_patterns[p],
                           test_sizes[s], test_addresses[a]);

                    switch (op)
                    {
                    case 0:
                        elapsed_us = benchmark_flash_read(test_addresses[a], test_sizes[s], test_patterns[p]);
                        snprintf(notes, sizeof(notes), "Flash_Read_Test_%d", data_row_count);
                        break;
                    case 1:
                        elapsed_us = benchmark_flash_program(test_addresses[a], test_sizes[s], test_patterns[p]);
                        snprintf(notes, sizeof(notes), "Flash_Program_Test_%d", data_row_count);
                        break;
                    case 2:
                        elapsed_us = benchmark_flash_erase(test_addresses[a], test_sizes[s]);
                        snprintf(notes, sizeof(notes), "Flash_Erase_Test_%d", data_row_count);
                        break;
                    }

                    if (elapsed_us > 0)
                    {
                        float time_seconds = elapsed_us / 1e6f;
                        float size_MB = test_sizes[s] / (1024.0f * 1024.0f);
                        throughput_MBps = (time_seconds > 0) ? (size_MB / time_seconds) : 0.0f;
                    }

                    char csv_row[512];
                    printf("🧾 Using JEDEC for CSV: [%s]\n", chip_id);
                    int len = snprintf(csv_row, sizeof(csv_row),
                                       "%s,%s,%u,0x%06X,%llu,%.3f,%d,%.2f,%.2f,%s,%s,%s",
                                       chip_id, operations[op], test_sizes[s], test_addresses[a],
                                       (unsigned long long)elapsed_us, throughput_MBps, data_row_count,
                                       temp, voltage, test_patterns[p], timestamp_str, notes);

                    if (len > 0 && len < (int)sizeof(csv_row))
                    {
                        if (sd_append_to_file(CSV_FILENAME, csv_row))
                        {
                            printf("✅ Test %d logged: %.2f MB/s\n", data_row_count, throughput_MBps);
                            logged_this_press++;
                        }
                        else
                        {
                            printf("❌ Failed to log test %d\n", data_row_count);
                            data_row_count--;
                        }
                    }
                    else
                    {
                        printf("❌ CSV formatting error for test %d\n", data_row_count);
                        data_row_count--;
                    }

                    sleep_ms(100);

                    if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
                        break;
                }
                if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
                    break;
            }
            if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
                break;
        }
        if (logged_this_press >= MAX_TESTS_PER_PRESS || data_row_count >= TARGET_ROWS)
            break;
    }

    printf("\n📈 Progress Report:\n");
    printf("   Total entries: %d\n", data_row_count);
    printf("   Target: %d entries\n", TARGET_ROWS);
    printf("   Progress: %.1f%% complete\n", (data_row_count * 100.0f) / TARGET_ROWS);

    if (data_row_count >= TARGET_ROWS)
        printf("🎉 Target of %d entries reached! System continues logging...\n", TARGET_ROWS);
    else
        printf("🔄 Press GP20 again for more forensic analysis\n");

    printf("===========================================\n");
    printf("🔍 FORENSIC ANALYSIS COMPLETE 🔍\n\n");
}

/* ============================== Button Action ============================= */
static void handle_gp20_button_press(void)
{
    printf("\n🚀 GP20 pressed — starting checks…\n");

    // 1) (Re)mount SD every press
    if (!sd_mount())
    {
        printf("⛔ microSD not present or mount failed. Insert the card and press GP20 again.\n");
        return;
    }
    printf("✅ microSD mounted\n");

    // 2) Flash must be live right now (quick JEDEC; no cache)
    char jedec[24];
    if (!flash_is_live_now(jedec, sizeof jedec))
    {
        printf("⛔ Microchip (SPI flash) not live. Check wiring/power/CS and press GP20 again.\n");
        return;
    }
    printf("✅ Flash live: JEDEC %s\n", jedec);

    // 3) CSV ready (SD already mounted)
    if (!ensure_csv_ready())
    {
        printf("⛔ CSV logger not ready. Fix SD and press GP20 again.\n");
        return;
    }

    // 4) Ask for backup (SAFE mode), then run processes
    if (prompt_yes_no("\n💾 Would you like to perform a full microchip backup (Safe Mode)?"))
    {
        printf("📀 Starting SAFE microchip backup...\n");
        if (!sd_backup_flash_safe("SPI_Backup", "microchip_backup_safe.bin"))
        {
            printf("❌ SAFE backup failed. You can still use the menu.\n");
        }
        else
        {
            printf("✅ SAFE backup complete! File: SPI_Backup/microchip_backup_safe.bin\n");
        }
    }
    else
    {
        printf("⏭️  Backup skipped by user.\n");
    }

    // 5) Your normal processes
    //    Step-1 menu (read | write | erase | quit)
    show_sd_menu_and_handle();

    //    Optional: run your forensic matrix after the menu
    // perform_forensic_analysis_and_log();

    printf("✅ Done. Back at main. Press GP20 to run again.\n");
}

static void handle_gp21_restore_press(void)
{
    printf("\n🧿 GP21 pressed — RESTORE MODE\n");

    // 1) (Re)mount SD
    bool sd_ok = false;
    if (sd_mount()) {
        sd_ok = true;
        printf("✅ microSD mounted\n");
    } else {
        printf("⚠️  microSD not mounted (will still allow webserver).\n");
    }

    // 2) Flash quick JEDEC probe (don't abort; allow web even if flash absent)
    char jedec[24];
    bool flash_ok = false;
    if (flash_is_live_now(jedec, sizeof jedec)) {
        flash_ok = true;
        printf("✅ Flash live: JEDEC %s\n", jedec);
    } else {
        printf("⚠️  Microchip (SPI flash) not live — restore via serial will be unavailable.\n");
    }

    // 3) Check that backup file exists
    const char *dir = "SPI_Backup";
    const char *fname = "microchip_backup_safe.bin";

    char fullpath[64];
    snprintf(fullpath, sizeof fullpath, "%s/%s", dir, fname);

    if (!sd_ok) {
        printf("⚠️  Cannot check file existence - microSD not mounted. Web mode will still start (no files available).\n");
    } else {
        if (!sd_file_exists(fullpath)) {
            printf("⚠️  Backup file '%s' not found on SD. Web mode will still start (no files available).\n", fullpath);
        }
    }

    // 4) Ask user how they want to proceed: serial 'restore' or start 'web'server to download backup
    printf("\n⚠️  RESTORE WARNING\n");
    printf("    This will overwrite the ENTIRE microchip with data from:\n");
    printf("      %s\n", fullpath);
    printf("    Type 'restore' to proceed via serial, 'web' to start HTTP server for downloading the backup, or 'no' to cancel.\n");

    char raw[16] = {0};
    for (;;)
    {
        printf("> ");
        fflush(stdout);

        if (!read_command_gap_terminated(raw, sizeof raw))
        {
            sleep_ms(40);
            continue;
        }

        if (!strcmp(raw, "restore") || !strcmp(raw, "rest"))
        {
            // user confirmed restore via serial — require SD + flash
            if (!sd_ok) {
                printf("❌ Cannot restore: microSD not mounted.\n");
                continue;
            }
            if (!flash_ok) {
                printf("❌ Cannot restore: flash chip not detected.\n");
                continue;
            }

            printf("\n🔁 Starting RESTORE from backup...\n");
            if (!sd_restore_flash_safe(dir, fname))
            {
                printf("❌ RESTORE failed. Microchip contents may be partially updated.\n");
            }
            else
            {
                printf("✅ RESTORE complete. Microchip contents now match backup.\n");
            }
            printf("🎯 Restore mode finished. You can press GP20 for analysis again.\n");
            return;
        }

        if (!strcmp(raw, "web"))
        {
            printf("[*] Starting webserver mode to download backup file...\n");

            // Ensure SD file list is populated
            sd_file_count = sd_get_file_list(sd_files, MAX_FILES_TO_LIST);
            file_list_needs_refresh = false;

            // Init WiFi (Pico W)
            if (cyw43_arch_init()) {
                printf("[!] Failed to initialize WiFi hardware\n");
                return;
            }

            // Enable AP mode (no return value) — print diagnostics before/after
            printf("[i] Enabling AP mode: %s (WPA2)\n", AP_SSID);
            cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
            printf("[i] cyw43_arch_enable_ap_mode() completed\n");

            // wait a short moment for AP to come up
            for (int i = 0; i < 40; i++) { cyw43_arch_poll(); sleep_ms(50); }

            // Diagnostic: print netif_default status
            if (netif_default) {
                ip4_addr_t addr = netif_default->ip_addr;
                printf("[i] netif_default present - IP: %s\n", ip4addr_ntoa(&addr));
            } else {
                printf("[!] netif_default is NULL after AP enable - network interface not created\n");
            }

            // configure IP and DHCP
            dhcp_server_t dhcp_server;
            bool dhcp_started = false;
            if (netif_default) {
                ip4_addr_t ipaddr, netmask, gw;
                IP4_ADDR(ip_2_ip4(&ipaddr), 192,168,4,1);
                IP4_ADDR(ip_2_ip4(&netmask), 255,255,255,0);
                IP4_ADDR(ip_2_ip4(&gw), 192,168,4,1);
                netif_set_addr(netif_default, ip_2_ip4(&ipaddr), ip_2_ip4(&netmask), ip_2_ip4(&gw));
                netif_set_up(netif_default);

                dhcp_server_init(&dhcp_server, &ipaddr, &netmask);
                dhcp_started = true;
                printf("[+] DHCP server started\n");
            } else {
                printf("[!] netif_default is NULL - cannot configure IP/DHCP\n");
            }

            // Hook up HTTP server file list and start HTTP server
            http_server_set_file_list(sd_files, &sd_file_count, &file_list_needs_refresh);
            if (!http_server_init()) {
                printf("[!] HTTP server failed to start\n");
            } else {
                printf("[+] HTTP server running. Connect to AP '%s' and open http://192.168.4.1\n", AP_SSID);
            }

            // Wait here while serving; pressing GP21 again will exit web mode
            printf("[i] Press GP21 again to stop webserver and return.\n");
            bool last_state = gpio_get(RESTORE_BUTTON_PIN);
            for (;;)
            {
                cyw43_arch_poll();
                bool cur = gpio_get(RESTORE_BUTTON_PIN);
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (last_state && !cur && (now - last_button_time_gp21) > DEBOUNCE_DELAY_MS) {
                    // falling edge -> exit
                    last_button_time_gp21 = now;
                    break;
                }
                last_state = cur;
                sleep_ms(50);
            }

            // Stop services
            if (dhcp_started) dhcp_server_deinit(&dhcp_server);
            cyw43_arch_deinit();
            printf("[i] Webserver stopped. Returning to main.\n");
            return;
        }

        if (!strcmp(raw, "no") || !strcmp(raw, "n"))
        {
            printf("↩️  Restore cancelled. No changes made.\n");
            return;
        }

        printf("Please type 'restore', 'web' or 'no'.\n");
    }
}


/* ================================== main ================================== */
int main(void)
{
    stdio_init_all();
    setvbuf(stdin, NULL, _IONBF, 0); // make getchar() immediate
    sleep_ms(7000);                  // allow USB CDC to enumerate

    /* Flash bring-up (once) */
    if (!flash_benchmark_init())
    {
        puts("Flash init failed.");
    }
    else
    {
        // Initial data peek
        flash_dump(0x000000, 64);
        flash_chip_ready = true; // mark ready after successful init
    }

    /* Banner */
    printf("\n");
    printf("████████████████████████████████████████████████████████████\n");
    printf("█  MAKER PI PICO W - FLASH MEMORY FORENSIC ANALYSIS SYSTEM  █\n");
    printf("████████████████████████████████████████████████████████████\n");
    printf("█ Version: 2.0 - Comprehensive Analysis & CSV Logging       █\n");
    printf("█ Hardware: Raspberry Pi Pico W + 32GB FAT32 SD Card        █\n");
    printf("█ Target: Real flash chip forensic benchmarking             █\n");
    printf("████████████████████████████████████████████████████████████\n\n");

    /* ADC */
    printf("🔧 SYSTEM INITIALIZATION\n");
    printf("========================\n");
    printf("⚡ Initializing ADC for environmental monitoring...\n");
    adc_init();
    adc_gpio_init(ADC_VSYS_PIN);
    adc_set_temp_sensor_enabled(true);
    printf("✅ ADC initialized - temperature and voltage monitoring ready\n");

        /* Buttons */
    printf("🔘 Configuring GP20 button interface...\n");
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);
    printf("✅ GP20 button configured with pull-up resistor\n");
    printf("   Press GP20 to trigger forensic analysis\n");
    printf("\n");
    printf("🔘 Configuring GP21 RESTORE button interface...\n");
    gpio_init(RESTORE_BUTTON_PIN);
    gpio_set_dir(RESTORE_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(RESTORE_BUTTON_PIN);
    printf("✅ GP21 button configured with pull-up resistor\n");
    printf("   Press GP21 to RESTORE microchip from backup\n");

    /* Flash status */
    printf("\n⚡ FLASH MEMORY FORENSIC SYSTEM\n");
    printf("===============================\n");
    if (!flash_chip_ready)
    {
        printf("⚠️  No external flash chip detected — simulation mode\n");
    }
    else
    {
        char chip_id[24];
        flash_get_jedec_str(chip_id, sizeof chip_id);
        printf("🎯 JEDEC ID: %s\n", chip_id);
        if (!strcmp(chip_id, "No / Unknown_Flash"))
        {
            printf("⚠️  Flash chip identity unknown - using generic analysis\n");
        }
    }

    /* Environment snapshot */
    printf("\n🌡️ ENVIRONMENTAL MONITORING\n");
    printf("============================\n");
    float temp = get_internal_temperature();
    float v = get_supply_voltage();
    printf("🌡️  Current temperature: %.2f°C\n", temp);
    printf("⚡ Supply voltage: %.2fV\n", v);
    printf("%s\n", (temp >= -10 && temp <= 85) ? "✅ Temperature within operating range (-10°C to 85°C)"
                                               : "⚠️  Temperature outside recommended range");
    printf("%s\n", (v >= 2.7 && v <= 5.5) ? "✅ Voltage within operating range (2.7V to 5.5V)"
                                          : "⚠️  Voltage outside recommended range");

    /* CSV system intro */
    printf("\n📊 CSV FORENSIC DATA LOGGING SYSTEM\n");
    printf("===================================\n");
    printf("📁 Target file: %s\n", CSV_FILENAME);
    printf("📋 Format: CSV with comprehensive forensic data\n");
    printf("💾 Storage: 32GB FAT32 SD Card (Windows compatible)\n");
    printf("🎯 Target: %d forensic analysis entries\n", TARGET_ROWS);
    printf("🔄 Mode: Append to existing file or create new\n");

    /* Instructions */
    printf("\n📌 SD CARD REQUIREMENTS\n");
    printf("======================\n");
    printf("💿 Capacity: 32GB (confirmed compatible)\n");
    printf("📂 Format: FAT32 (Windows/Mac/Linux readable)\n");
    printf("🔌 Connection: Maker Pi Pico W SD card slot (GP10-GP15)\n");
    printf("⚠️  IMPORTANT: Insert formatted 32GB FAT32 SD card before testing\n");

    printf("\n🚀 SYSTEM STATUS: READY FOR OPERATION\n");
    printf("=====================================\n");
    printf("✅ Hardware: Maker Pi Pico W initialized\n");
    printf("✅ Button: GP20 configured and ready\n");
    printf("✅ Flash: %s\n", flash_chip_ready ? "Ready for real analysis" : "Simulation mode ready");
    printf("✅ Environmental: Temperature and voltage monitoring active\n");
    printf("⏳ SD Card: Will be initialized on first GP20 press\n");

    printf("\n🔬 OPERATION INSTRUCTIONS\n");
    printf("=========================\n");
    printf("1️⃣  Insert 32GB FAT32 formatted SD card\n");
    printf("2️⃣  Press GP20 button to start forensic analysis\n");
    printf("3️⃣  Use menu: read | write | erase | quit\n");
    printf("4️⃣  Results automatically saved to RESULTS.CSV\n");
    printf("5️⃣  File is Windows-compatible; remove SD to view on PC\n");

    printf("\n🎯 Waiting for GP20 button press to begin forensic analysis...\n\n");

    srand(to_ms_since_boot(get_absolute_time())); // for any simulated/random patterns

    /* ============================ Main Loop ============================ */
    printf("🔄 System entering main operational loop\n");
    printf("   Monitoring GP20 for forensic analysis trigger...\n\n");

       uint32_t last_hb = 0;
    for (;;)
    {
        bool curr20 = gpio_get(BUTTON_PIN);
        bool curr21 = gpio_get(RESTORE_BUTTON_PIN);
        uint32_t now = to_ms_since_boot(get_absolute_time());

        /* Debounced falling edge for GP20 (forensics) */
        if (last_button_state_gp20 && !curr20 && (now - last_button_time_gp20) > DEBOUNCE_DELAY_MS)
        {
            char ts[32];
            create_timestamp(ts, sizeof ts);
            printf("\n🔘 GP20 BUTTON PRESS DETECTED! 🔘\n⏰ Timestamp: %s\n", ts);

            handle_gp20_button_press();

            printf("\n✅ Forensic analysis complete - system ready for next press\n");
            printf("🎯 Current progress: %d entries logged\n", data_row_count);
            if (data_row_count > 0)
            {
                printf("📈 Target completion: %.1f%% (%d entries target)\n",
                       (data_row_count * 100.0f) / TARGET_ROWS, TARGET_ROWS);
            }
            printf("🔄 Press GP20 again to continue…\n\n");

            last_button_time_gp20 = now;
        }
        last_button_state_gp20 = curr20;

        /* Debounced falling edge for GP21 (restore) */
        if (last_button_state_gp21 && !curr21 && (now - last_button_time_gp21) > DEBOUNCE_DELAY_MS)
        {
            char ts[32];
            create_timestamp(ts, sizeof ts);
            printf("\n🧿 GP21 RESTORE BUTTON PRESS DETECTED! 🧿\n⏰ Timestamp: %s\n", ts);

            handle_gp21_restore_press();

            last_button_time_gp21 = now;
        }
        last_button_state_gp21 = curr21;

        /* Heartbeat */
        if ((now - last_hb) > HEARTBEAT_MS)
        {
            float t = get_internal_temperature();
            float vv = get_supply_voltage();
            printf("💓 System heartbeat - GP20/GP21 monitoring active (entries: %d)\n", data_row_count);
            printf("   Temperature: %.1f°C | Voltage: %.2fV\n", t, vv);
            last_hb = now;
        }

        sleep_ms(10);
    }


    // not reached
    // return 0;
}
