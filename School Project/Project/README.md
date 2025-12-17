# SPI Flash Performance Evaluation & Forensic Analysis (RP2040 + SD Card + Wi-Fi) 💾📊

Firmware for the **Raspberry Pi Pico W** (Maker Pi Pico W) that:

- Talks to an external **SPI NOR flash** chip
- Runs **read / program / erase** performance benchmarks
- Logs results to **CSV files** on a microSD card (via **FatFs**)
- Compares measurements against a **datasheet database** to guess the chip
- Supports **safe full-chip backup & restore** to/from SD
- Hosts a **Wi-Fi AP + HTTP server** so you can download the CSV/backup files from a browser

---

## About the Project

This embedded system turns the Pico W into a small **flash forensics and benchmarking tool**.

Main goals:

- Measure **latency and throughput** of an external SPI flash at multiple sizes  
- Generate `RESULTS.CSV` (raw data) and `report.csv` (summary & chip guess)  
- Allow **non-destructive Safe analysis** (read-only) and **Destructive analysis** (program/erase)  
- Provide **backup/restore** to preserve original contents  
- Expose a simple **web dashboard** to monitor status and download SD card files

---

## Features

- 🔎 **JEDEC ID auto-detection** of the flash chip  
- 📈 Benchmarks for **read**, **program (write)**, and **erase**  
- 💽 Logging to `RESULTS.CSV` on a **FAT32 microSD card**  
- 📊 `report.csv` generated from `RESULTS.CSV` + `datasheet.csv`  
- 🛡️ **Safe analysis** (no flash modifications)  
- 💣 **Destructive analysis** (write/erase after read)  
- 🧯 **Full-chip backup & restore** using SD card files  
- 🌐 **Wi-Fi AP + HTTP server** to view status and download CSV/backup files  

---

## Hardware Overview

- **Board**: Maker Pi Pico W (Raspberry Pi Pico W)
- **External SPI Flash**: wired to Pico SPI (per project schematic)
- **microSD Card**: FAT32, connected via SPI with FatFs
- **Buttons** (as used in `main.c`):
  - `GP20` – trigger backup + analysis menu
  - `GP21` – trigger restore / web-server mode
- **Sensors (via ADC)**:
  - Internal temperature sensor
  - VSYS/3 (supply voltage monitor)

---

## Project Structure

### Top-Level Source Files

| File              | Description |
|-------------------|-------------|
| `main.c`          | **Entry point & controller.** Initialises the board, mounts the SD card, probes the SPI flash, handles button logic (analysis vs restore/web), and coordinates benchmarks, backup/restore, and report generation. |
| `flash_benchmark.c` | **Core flash benchmarking layer.** Provides low-level SPI flash access (JEDEC ID read, read/program/erase primitives) and timing helpers used by the benchmark modules. |
| `bench_read.c`    | **Read benchmark module.** Runs repeated read tests at various sizes (e.g. 1 byte, page, sector), logs each sample to `RESULTS.CSV`, and prints summary statistics. |
| `bench_write.c`   | **Program (write) benchmark module.** Performs flash program operations for Destructive analysis, times them, logs to `RESULTS.CSV`, and prints write summary statistics. |
| `bench_erase.c`   | **Erase benchmark module.** Performs sector/block erase operations, measures erase times, logs to `RESULTS.CSV`, and prints erase summary statistics. |
| `chip_db.c`       | **Chip database utilities.** Helper routines for interpreting `datasheet.csv` entries and mapping JEDEC IDs / timing profiles to possible chip models and vendors. |
| `report.c`        | **Report generator.** Reads `RESULTS.CSV` and `datasheet.csv`, aggregates stats per size/operation, compares them, builds candidate chip lists, selects a best guess, and writes everything into `report.csv`. |
| `sd_card.c`       | **SD card + FatFs wrapper.** Initialises and mounts the SD card, provides helper functions for opening/writing/reading files, and implements safe full-chip **backup** and **restore** of the SPI flash to/from binary files on SD. |
| `dhcpserver.c`    | **Minimal DHCP server.** Lets the Pico act as a DHCP server when running as a Wi-Fi AP, assigning IP addresses to clients that connect to the Pico’s hotspot. |
| `http_server.c`   | **HTTP server.** Implements a small web server (using lwIP’s raw API) that serves a status/dashboard page and provides endpoints to **list and download SD card files** (e.g. `RESULTS.CSV`, `report.csv`, backups). |
| `lwipopts.h`      | **lwIP configuration.** Configures the lwIP TCP/IP stack (enabling required features such as DHCP and HTTP while trimming unused ones). |

> Each `*.h` file (e.g. `flash_benchmark.h`, `bench_read.h`, `sd_card.h`, `report.h`, etc.) declares the functions, data structures, and constants used by the corresponding `*.c` file.

### Library / Support Folders

| Folder    | Description |
|-----------|-------------|
| `fatfs/`  | **FatFs library** sources, including `ff.c`, `diskio.c`, `ffsystem.c`, `ffunicode.c`, and headers. Provides the file system APIs (`f_mount`, `f_open`, `f_read`, `f_write`, etc.) used by `sd_card.c`. |
| `build/` *(generated)* | Out-of-source build directory created by CMake. Contains intermediate object files and the final `.elf` / `.uf2` firmware. You can delete and recreate this folder. |

> Your repository may also include additional Pico SDK or lwIP support files depending on your template.

### Files on the SD Card

These files live on the **microSD card** at runtime:

| File / Folder                       | Created by / Purpose |
|-------------------------------------|----------------------|
| `datasheet.csv`                     | **Provided by user.** Database of known flash chips and their datasheet timings. Used by `report.c` to match measurement profiles to candidate chips. |
| `RESULTS.CSV`                       | **Generated by benchmark modules.** Raw per-run measurements for all read/program/erase tests. |
| `report.csv`                        | **Generated by `report.c`.** Summary and chip-guess report derived from `RESULTS.CSV` + `datasheet.csv`. |
| `SPI_Backup/microchip_backup_safe.bin` | **Generated by `sd_card.c`.** Full-chip backup image captured before destructive tests, used for safe **restore** later. |

---

## How to Compile

### Prerequisites

- [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) installed
- `PICO_SDK_PATH` environment variable set correctly
- **ARM GCC toolchain** (e.g. `arm-none-eabi-gcc`, `arm-none-eabi-g++`)
- **CMake** (3.13+)
- A build tool such as `ninja` or `make`
- A 32 GB microSD card formatted as **FAT32**

### Build Steps

From the project root (where your `CMakeLists.txt` is):

```bash
# 1. Create and enter the build directory
mkdir -p build
cd build

# 2. Configure the CMake project
cmake ..

# 3. Build the firmware
cmake --build . -- -j4
```

On success, the build directory should contain a `.uf2` file (name depends on your CMake target), e.g.:

- `project.uf2`

---

## How to Run

### 1. Prepare the microSD Card

1. Format the card as **FAT32**.
2. Copy your **`datasheet.csv`** file to the **root** of the SD card.
3. (Optional) Create the folder `SPI_Backup/` if you want backup files stored in a specific place (the program can also create it).
4. Insert the SD card into the Maker Pi Pico W SD slot.

### 2. Flash the Firmware onto Pico W

1. Connect the Pico W to your PC **while holding BOOTSEL**.
2. A mass-storage drive named `RPI-RP2` will appear.
3. Drag and drop the built `.uf2` file (e.g. `project.uf2`) from `build/` onto `RPI-RP2`.
4. The board will reboot and start running the firmware.

### 3. Open a Serial Console

Use any serial terminal (PuTTY, screen, minicom, VS Code Serial Monitor) with:

- **Baud rate**: `115200`
- **Data bits**: `8`
- **Parity**: `None`
- **Stop bits**: `1`

You should see boot messages indicating:

- SD card mount status
- Detected flash JEDEC ID
- Basic instructions for buttons/menu

### 4. Start an Analysis Session

1. **Press the GP20 button** (check your board’s silkscreen for BTN mapping).  
2. The firmware will:
   - Ensure SD is mounted
   - Ensure flash is detected
   - Optionally ask whether to perform a **full backup** to `SPI_Backup/microchip_backup_safe.bin`
3. After that, a simple **text menu** appears on the serial console, for example:

   ```text
   ================= ANALYSIS MENU =================
   safe         - Safe analysis (read-only)
   destructive  - Destructive analysis (read + write/erase)
   exit         - Exit and generate report.csv
   =================================================
   >
   ```

4. Type one of the commands (`safe`, `destructive`, or `exit`) and press **Enter**.

The program will log results to `RESULTS.CSV` on the SD card and print summaries to the console.

### 5. Using Backup & Restore

- **Backup**  
  - When prompted after pressing GP20, choose to perform a backup.
  - The firmware will read the entire flash chip and write it to:  
    `SPI_Backup/microchip_backup_safe.bin` on the SD card.

- **Restore**  
  - Press **GP21** to enter restore/web mode.
  - Follow the serial prompts:
    - Type `restore` to restore the flash from `SPI_Backup/microchip_backup_safe.bin`.
    - Type `web` to start the Wi-Fi AP and HTTP server instead (see next section).
    - Type `no` to cancel.

> ⚠ **Warning:** Restore will completely overwrite the flash contents with the backup image.

### 6. Web Dashboard (Wi-Fi AP + HTTP Server)

1. From the GP21 prompt, choose **`web`**.
2. The Pico W will:
   - Enable Wi-Fi AP using SSID/password configured in your project (e.g. in `config.h` / `lwipopts.h`).
   - Start the DHCP server and HTTP server.
3. On your laptop/phone:
   - Connect to the Pico’s Wi-Fi network.
   - Open a browser and go to (for example): `http://192.168.4.1/`
4. The web page will:
   - Show SD card status, temperature, voltage, etc.
   - List files on the SD card with **Download** links (e.g. `RESULTS.CSV`, `report.csv`, backup binaries).

Press GP21 again (or follow on-screen instructions) to stop the server and return to normal mode.

---

## How to Test

A simple end-to-end test flow:

1. **Build & Flash**
   - Run the CMake build steps.
   - Flash the `.uf2` to the Pico W.
   - Confirm you see a boot banner on the serial console.

2. **SD & Flash Detection**
   - Insert a prepared SD card with `datasheet.csv`.
   - Ensure the serial console shows:
     - SD card mounted successfully.
     - A valid JEDEC ID for the attached SPI flash.

3. **Safe Analysis Test**
   - Press GP20.
   - When menu appears, type `safe` and press Enter.
   - Let the read benchmarks complete.
   - Type `exit` to generate `report.csv`.
   - Remove SD and check:
     - `RESULTS.CSV` created with read entries.
     - `report.csv` created with summary and candidate chip list.

4. **Destructive Analysis Test**
   - Reinsert SD, reconnect, and open serial console.
   - Press GP20 → choose `destructive`.
   - Follow prompts to run write and/or erase benchmarks.
   - After completion, confirm `RESULTS.CSV` has write/erase rows and `report.csv` updated.

5. **Backup & Restore Test**
   - Perform a **backup** before destructive analysis.
   - After changing the flash contents via destructive tests, use GP21 → `restore`.
   - Optionally verify (using external tools or known patterns) that the flash contents match the backup.

6. **Web Interface Test**
   - Use GP21 → `web` to start the HTTP server.
   - Connect via Wi-Fi and download `RESULTS.CSV` and `report.csv`.
   - Confirm the downloaded files match those on the SD card.

If all of the above work, the project is compiled, running, and tested successfully. 🎉
