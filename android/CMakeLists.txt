cmake_minimum_required(VERSION 3.22)
project(magiccap_android C CXX)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Optimized native core, including debug APKs" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_SOURCE "${CMAKE_CURRENT_LIST_DIR}/deps/SDL2-2.32.10")
add_subdirectory("${SDL_SOURCE}" sdl)
# Desktop builds include SDL2/SDL.h; SDL's source tree has bare SDL.h.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sdl-include/SDL2")
file(COPY "${SDL_SOURCE}/include/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/sdl-include/SDL2")
include_directories("${CMAKE_CURRENT_BINARY_DIR}/sdl-include")
add_subdirectory(.. core)
# CMake 3.22's IPO probe selects host gold in an NDK try-compile. Use the
# NDK's supported thin-LTO flags explicitly for our core.
foreach(target mrc_common mrc_cpu_mips mrc_tx39 mrc_devices mrc_host mrc_state
        mrc_rom mrc_datarover840 mrc_sdl_datarover mrc_sdl_display mrc_mc68349
        mrc_pic2000 mrc_sdl_pic2000 mrc_cli_pic2000 mcap)
    target_compile_options(${target} PRIVATE -flto=thin)
endforeach()
target_link_options(mcap PRIVATE -flto=thin -fuse-ld=lld)
