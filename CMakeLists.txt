cmake_minimum_required(VERSION 3.16)
project(ThumbyScummby C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Debug build
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_subdirectory(engine)
add_subdirectory(host_sdl)

# device_pico/ added via separate cmake invocation with Pico SDK toolchain
