cmake_minimum_required(VERSION 3.16)

find_package(SDL2 REQUIRED)

add_executable(thumbyscummby
    main.cpp
    platform_sdl.cpp
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
