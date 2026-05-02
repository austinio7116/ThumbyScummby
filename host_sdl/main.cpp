// ThumbyScummby — SDL host main.
//
// Usage: thumbyscummby <path-to-mi1-data-dir>
//
// Loads game data files (decrypts on read), initializes engine, runs main
// loop until quit.

#include "engine.h"
#include "platform.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

// Forward declarations from platform_sdl.cpp
namespace tsb::platform_sdl {
    bool init(int argc, char **argv);
    bool main_loop_iter();   // returns false when quit requested
    void shutdown();
    bool load_data_dir(const char *path);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path-to-mi1-data-dir>\n", argv[0]);
        fprintf(stderr, "  expects: 000.LFL, DISK01.LEC..DISK04.LEC, 901-904.LFL\n");
        return 1;
    }

    if (!tsb::platform_sdl::init(argc, argv)) {
        fprintf(stderr, "platform init failed\n");
        return 1;
    }

    if (!tsb::platform_sdl::load_data_dir(argv[1])) {
        fprintf(stderr, "failed to load game data from %s\n", argv[1]);
        tsb::platform_sdl::shutdown();
        return 1;
    }

    if (!tsb::engine_init()) {
        fprintf(stderr, "engine init failed\n");
        tsb::platform_sdl::shutdown();
        return 1;
    }

    while (tsb::platform_sdl::main_loop_iter()) {
        if (!tsb::engine_tick()) break;
    }

    tsb::engine_shutdown();
    tsb::platform_sdl::shutdown();
    return 0;
}
