# ---- Build the Slave (Pico B) application ----

# Create an executable named 'slave' from slave.c
add_executable(slave
    slave.c
)

# Tell the compiler where to find headers used by slave.c
target_include_directories(slave PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}            # for secrets.h (same folder as slave.c)
    ${CMAKE_CURRENT_LIST_DIR}/..         # for lwipopts.h (in parent folder)
    ${CMAKE_CURRENT_LIST_DIR}/../../../pico_w/wifi  # for lwipopts_examples_common.h
)

# Link with:
# - pico_stdlib: stdio, time, etc.
# - pico_cyw43_arch_lwip_threadsafe_background:
#     CYW43 Wi-Fi driver + lwIP with a background thread (safe for UDP callbacks)
target_link_libraries(slave
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

# Use USB CDC for printf; disable UART stdio
pico_enable_stdio_usb(slave 1)
pico_enable_stdio_uart(slave 0)

# Emit UF2/bin/elf/map outputs for flashing and debugging
pico_add_extra_outputs(slave)
