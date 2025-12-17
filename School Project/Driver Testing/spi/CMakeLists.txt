if (NOT PICO_NO_HARDWARE)
    add_subdirectory(master_spi)
    add_subdirectory(slave_spi)
endif ()
