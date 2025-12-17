add_executable(slave_spi
        slave_spi.c
        )

#pico_generate_pio_header(slave ${CMAKE_CURRENT_LIST_DIR}/spi_slave.pio)

# pull in common dependencies
target_link_libraries(slave_spi pico_stdlib hardware_gpio)

# enable usb output, enable uart output
pico_enable_stdio_usb(slave_spi 1)
pico_enable_stdio_uart(slave_spi 0)

# create map/bin/hex file etc.
pico_add_extra_outputs(slave_spi)

# add url via pico_set_program_url
example_auto_set_url(slave_spi)
