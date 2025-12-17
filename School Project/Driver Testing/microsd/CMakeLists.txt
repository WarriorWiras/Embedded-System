# ============================================================
# SPI Flash Performance Evaluation & Forensic Analysis Project
# Build config for Raspberry Pi Pico W + SD (FatFs) + SPI flash
# ============================================================

# Minimum CMake version required by the Pico SDK
cmake_minimum_required(VERSION 3.13)

# -----------------------------
# Board selection
# Must be set BEFORE project()
# Options: pico, pico_w, etc.
# -----------------------------
set(PICO_BOARD pico_w)

# -----------------------------
# Project declaration
# (names the project and enables C/C++/ASM)
# -----------------------------
project(microsd C CXX ASM)

# -----------------------------
# Executable + source files
# - main.c              : app entry / orchestration
# - flash_benchmark.c   : SPI flash read/program/erase + timers
# - sd_card.c           : FatFs wrappers (mount, write, append)
# - fatfs/ff.c          : FatFs core
# - fatfs/diskio.c      : FatFs low-level disk glue (SPI SD)
# -----------------------------
add_executable(microsd
    main.c
    flash_benchmark.c
    sd_card.c
    fatfs/ff.c
    fatfs/diskio.c
)

# -----------------------------
# Header search paths
# - current dir          : for sd_card.h, flash_benchmark.h, etc.
# - fatfs                : for ff.h, diskio.h
# -----------------------------
target_include_directories(microsd PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/fatfs
)

# -----------------------------
# Link against Pico SDK libs
# - pico_stdlib : stdio, clocks, basic runtime
# - hardware_gpio/adc/spi : peripherals used
#   (Add others like hardware_timer if needed)
# -----------------------------
target_link_libraries(microsd
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_spi
)

# -----------------------------
# StdIO over USB/UART
# Enable one or both. USB is handy for logs;
# UART helps if USB serial is not available.
# -----------------------------
pico_enable_stdio_usb(microsd 1)
pico_enable_stdio_uart(microsd 1)

# -----------------------------
# Produce UF2, bin, map, etc.
# -----------------------------
pico_add_extra_outputs(microsd)
