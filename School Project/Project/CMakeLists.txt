# SPI Flash Performance Evaluation & Forensic Analysis Project
cmake_minimum_required(VERSION 3.13)

# Set board type to Pico W before project declaration
set(PICO_BOARD pico_w)

project(project C CXX ASM)

add_executable(project
    main.c
    flash_benchmark.c
    sd_card.c
    chip_db.c
    bench_read.c
    bench_write.c
    bench_erase.c
    report.c
    fatfs/ff.c
    fatfs/diskio.c
    fatfs/ffsystem.c
    fatfs/ffunicode.c
    dhcpserver/dhcpserver.c
    web/http_server.c

)

# Include directories
target_include_directories(project PRIVATE 
    ${CMAKE_CURRENT_LIST_DIR}                              # Current directory (for sd_card.h and main.c)
    ${CMAKE_CURRENT_LIST_DIR}/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${CMAKE_CURRENT_LIST_DIR}/web
    ${CMAKE_CURRENT_LIST_DIR}/dhcpserver
)

# Link libraries
target_link_libraries(project
    pico_stdlib
    hardware_gpio
    hardware_adc
    hardware_spi
    hardware_timer
    pico_cyw43_arch_lwip_threadsafe_background  # WiFi support with background processing
)

# Enable USB and UART for stdio
pico_enable_stdio_usb(project 1)
pico_enable_stdio_uart(project 1)
pico_add_extra_outputs(project)
