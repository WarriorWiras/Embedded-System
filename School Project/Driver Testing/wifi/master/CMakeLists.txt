# ---- Build the Master (Pico A) application ----

# Create an executable named 'master' from master.c
add_executable(master
    master.c
)

# Where the compiler should look for headers included by master.c
target_include_directories(master PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}            # so master.c can find secrets.h
    ${CMAKE_CURRENT_LIST_DIR}/..         # so it can find lwipopts.h (in parent folder)
    ${CMAKE_CURRENT_LIST_DIR}/../../../pico_w/wifi  # brings in lwipopts_examples_common.h
)

# Link against the Pico SDK libs:
# - pico_stdlib: printf, time, GPIO helpers, etc.
# - pico_cyw43_arch_lwip_threadsafe_background:
#       CYW43 Wi-Fi driver + lwIP networking, with a background thread
#       safe for calling lwIP from your app (what master.c uses).
target_link_libraries(master
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
)

# Use USB for stdio (printf over USB CDC). Disable UART stdio.
pico_enable_stdio_usb(master 1)
pico_enable_stdio_uart(master 0)

# Generate extra output formats (UF2, bin, map, etc.)
pico_add_extra_outputs(master)
