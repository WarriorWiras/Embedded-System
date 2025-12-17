add_executable(master_spi master_spi.c)

# pull in common dependencies
target_link_libraries(master_spi pico_stdlib hardware_spi hardware_gpio)

# enable usb output, enable uart output
pico_enable_stdio_usb(master_spi 1)
pico_enable_stdio_uart(master_spi 0)

# create map/bin/hex file etc.
pico_add_extra_outputs(master_spi)

# add url via pico_set_program_url
example_auto_set_url(master_spi)