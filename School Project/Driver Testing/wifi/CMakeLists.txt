cmake_minimum_required(VERSION 3.13)


project(DriverDemo C CXX ASM)
pico_sdk_init()

add_subdirectory(master)
add_subdirectory(slave)
