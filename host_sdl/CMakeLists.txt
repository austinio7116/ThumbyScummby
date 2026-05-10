cmake_minimum_required(VERSION 3.16)

find_package(SDL2 REQUIRED)

add_executable(thumbyscummby
    main.cpp
    platform_sdl.cpp
    save_backend.cpp
    config_backend.cpp
    telemetry.cpp
)

target_link_libraries(thumbyscummby
    PRIVATE thumbyscummby_engine
            ${SDL2_LIBRARIES}
)
target_include_directories(thumbyscummby PRIVATE ${SDL2_INCLUDE_DIRS})

target_compile_features(thumbyscummby PUBLIC cxx_std_17)
target_compile_options(thumbyscummby PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fno-exceptions -fno-rtti
    -g -O2
)

# --gc-sections drops unused functions/data based on the
# -ffunction-sections / -fdata-sections compile flags set on
# thumbyscummby_engine.  This is critical for fitting on RP2350 — the
# transcribed scummvm code drags in v6/v7/v8/HE/Mac/Towns/NES/C64 paths
# that are never reached from our v4/v5 entry point, but their symbols
# would otherwise need link-stubs.
target_link_options(thumbyscummby PRIVATE
    -Wl,--gc-sections
    -Wl,--print-gc-sections    # log what got stripped (turn off later)
)
