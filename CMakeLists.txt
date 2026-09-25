cmake_minimum_required(VERSION 3.16)
project(MagicHat C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(MRC_STATIC_DEPENDENCIES "Link SDL2, libslirp and compiler runtimes statically (OS libraries remain shared)" OFF)
if(MRC_STATIC_DEPENDENCIES AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libgcc $<$<LINK_LANGUAGE:CXX>:-static-libstdc++>)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# The interpreter is the whole program's hot loop; boot is ~800M
# instructions and experiments run into the billions.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<NOT:$<CONFIG:Debug>>:-O3>)
    # The bus and the interpreter are separate translation units, so without
    # link-time optimisation the memory path cannot be inlined into the
    # instruction loop — which is where nearly all the time goes.
    if(NOT ANDROID)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT MRC_HAVE_IPO OUTPUT MRC_IPO_WHY)
        if(MRC_HAVE_IPO AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(STATUS "link-time optimisation enabled")
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(STATUS "no link-time optimisation: ${MRC_IPO_WHY}")
        endif()
    endif()
endif()

# CPU, chip, board and host dependencies are separate targets. A 68k build
# reuses common services without pulling in the DataRover or MIPS frontend.
add_library(mrc_common STATIC src/core/bus/bus.c)
target_include_directories(mrc_common PUBLIC src)
add_library(mrc_cpu_mips STATIC src/cpu/mips/r3900.c
    src/jit/mips_ir.c src/jit/aarch64.c src/jit/x86_64.c src/jit/cache.c)
target_link_libraries(mrc_cpu_mips PUBLIC mrc_common)
add_library(mrc_tx39 STATIC
    src/soc/tx39/tx39.c src/soc/tx39/tx39_icu.c
    src/soc/tx39/tx39_uart.c src/soc/tx39/tx39_timer.c
    src/soc/tx39/tx39_video.c src/soc/tx39/tx39_sib.c
    src/soc/tx39/tx39_mbus.c src/soc/tx39/tx39_power.c
    src/soc/tx39/ucb1100.c src/soc/tx39/ucb1100_audio.c)
add_library(mrc_devices STATIC
    src/devices/glacier/glacier.c src/devices/pccard/pccard.c
    src/devices/ne2000/ne2000.c src/devices/pccard/ne2000_card.c src/devices/pccard/modem_card.c
    src/devices/pccard/sram_card.c
    src/devices/magicbus/keyboard.c
    src/devices/magicbus/host_keys.c
    src/devices/unknown_dev/unknown_dev.c)
target_link_libraries(mrc_devices PUBLIC mrc_common)
add_library(mrc_host STATIC src/host/console.c src/host/serial.c
    src/host/power.c src/host/wav.c src/host/network.c src/host/ppp.c src/host/card_image.c
    src/host/pclink.c)
target_link_libraries(mrc_host PUBLIC mrc_common)
if(NOT ANDROID)
    find_package(PkgConfig QUIET)
endif()
if(NOT ANDROID AND PkgConfig_FOUND)
    pkg_check_modules(SLIRP QUIET IMPORTED_TARGET slirp>=4.0)
endif()
if(SLIRP_FOUND)
    target_compile_definitions(mrc_host PRIVATE MRC_HAVE_SLIRP)
    if(MRC_STATIC_DEPENDENCIES)
        find_library(MRC_SLIRP_ARCHIVE NAMES libslirp.a HINTS ${SLIRP_LIBRARY_DIRS})
        if(NOT MRC_SLIRP_ARCHIVE)
            message(FATAL_ERROR "Static libslirp archive required; build libslirp with -Ddefault_library=static")
        endif()
        pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
        target_include_directories(mrc_host PRIVATE ${SLIRP_INCLUDE_DIRS})
        target_link_libraries(mrc_host PUBLIC ${MRC_SLIRP_ARCHIVE} PkgConfig::GLIB)
    else()
        target_link_libraries(mrc_host PUBLIC PkgConfig::SLIRP)
    endif()
    message(STATUS "libslirp found: userspace NAT available (--net user)")
else()
    message(STATUS "libslirp not found: --net user unavailable")
endif()
target_link_libraries(mrc_tx39 PUBLIC mrc_cpu_mips mrc_host m)
add_library(mrc_datarover840 STATIC src/machines/datarover840/machine.c
    src/machines/datarover840/runtime.c src/machines/datarover840/snapshot.c
    src/machines/datarover840/power.c src/machines/datarover840/suspend.c
    src/machines/datarover840/keyboard.c)
target_link_libraries(mrc_datarover840 PUBLIC mrc_tx39 mrc_devices mrc_host)
add_library(mrc_sdl_datarover STATIC src/frontend/sdl/datarover_gui_sdl.c
    src/frontend/sdl/chooser.c)
target_link_libraries(mrc_sdl_datarover PUBLIC mrc_datarover840)
# Compatibility aggregation for existing test targets and downstream users.
add_library(mcapcore INTERFACE)
target_link_libraries(mcapcore INTERFACE mrc_sdl_datarover)

if(NOT TARGET SDL2)
    find_package(SDL2 QUIET)
else()
    set(SDL2_FOUND TRUE)
endif()
if(SDL2_FOUND)
    if(MRC_STATIC_DEPENDENCIES)
        if(NOT TARGET SDL2::SDL2-static)
            message(FATAL_ERROR "SDL2 package must provide SDL2::SDL2-static")
        endif()
        set(MRC_SDL_LIBRARY SDL2::SDL2-static)
    else()
        set(MRC_SDL_LIBRARY SDL2)
    endif()
    message(STATUS "SDL2 found: building the GUI (--gui)")
    add_library(mrc_sdl_display STATIC src/frontend/sdl/picker.c
        src/frontend/sdl/card_panel.c
        src/frontend/sdl/devices_panel.c
        src/frontend/sdl/settings_panel.c
        src/frontend/sdl/install_panel.c
        src/frontend/sdl/shell.c
        src/frontend/sdl/display.c
                                       src/frontend/sdl/lcd.c
                                       src/frontend/sdl/display_panel.c
                                       src/frontend/sdl/ui.c
                                       src/frontend/sdl/ui_assets.c)
    target_include_directories(mrc_sdl_display PUBLIC src ${SDL2_INCLUDE_DIRS})
    target_link_libraries(mrc_sdl_display PUBLIC ${MRC_SDL_LIBRARY} mrc_state)
    target_link_libraries(mrc_sdl_datarover PUBLIC mrc_sdl_display)
    foreach(t mrc_host mrc_sdl_datarover)
        target_compile_definitions(${t} PRIVATE MRC_HAVE_SDL)
        target_include_directories(${t} PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(${t} PUBLIC ${MRC_SDL_LIBRARY})
    endforeach()
else()
    message(STATUS "SDL2 not found: --gui will not be available")
endif()
add_library(mrc_state STATIC src/host/state.c src/host/devices.c)
target_include_directories(mrc_state PUBLIC src)
target_link_libraries(mrc_state PUBLIC mrc_rom)
add_library(mrc_rom STATIC src/rom/identify.c)
target_include_directories(mrc_rom PUBLIC src)
if(ANDROID)
    add_library(mcap SHARED src/frontend/cli/launch.c src/frontend/cli/main.c
        src/frontend/android/entry.c)
    set_source_files_properties(src/frontend/cli/launch.c PROPERTIES COMPILE_DEFINITIONS "main=mrc_launch_main")
    target_link_libraries(mcap PRIVATE log)
else()
    add_executable(mcap src/frontend/cli/launch.c src/frontend/cli/main.c)
endif()
target_link_libraries(mcap PRIVATE mrc_rom mrc_state)
target_link_libraries(mcap PRIVATE mcapcore)

#
# The Magic Cap 1.x machines use the project-owned CPU32 core. The old
# reference implementation remains in the source tree for historical
# comparison, but it is not part of any configured build.
option(MRC_BUILD_M68K_MACHINES "Build the Magic Cap 68k machines" ON)
if(MRC_BUILD_M68K_MACHINES)
    enable_language(CXX)
    add_library(mrc_cpu32 INTERFACE)
    target_include_directories(mrc_cpu32 INTERFACE src)
    target_link_libraries(mrc_cpu32 INTERFACE mrc_common)
    add_library(mrc_mc68349 STATIC src/soc/mc68349/sim.cpp
                                      src/soc/mc68349/duart.cpp)
    target_link_libraries(mrc_mc68349 PUBLIC mrc_common)
    # The 68k core of our own. Not a test artefact, whatever it is
    # currently used for: it has to be built and linked wherever the 68k
    # machines are, or the machine will not link once it calls into it.
    add_library(mrc_m68k_core STATIC src/cpu/m68k/core/m68k_decode.c
                                     src/cpu/m68k/core/m68k.c
                                     src/cpu/m68k/core/m68k_block.c
                                     src/cpu/m68k/core/m68k_emit_x86.c
                                     src/cpu/m68k/core/m68k_emit_a64.c
                                     src/cpu/m68k/core/m68k_emit_none.c
                                     src/jit/code_arena.c)
    target_include_directories(mrc_m68k_core PUBLIC src)

    add_library(mrc_pic2000 STATIC src/machines/pic2000/machine68k.cpp
        src/machines/pic2000/registers.cpp src/machines/pic2000/runtime.c
        src/machines/pic2000/audio.cpp src/machines/pic2000/snapshot.cpp
        src/machines/pic2000/magicbus.cpp)
    set_target_properties(mrc_pic2000 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(mrc_pic2000 PUBLIC mrc_cpu32 mrc_mc68349 mrc_rom
                                             mrc_m68k_core mrc_host mrc_devices)
    add_library(mrc_sdl_pic2000 STATIC src/frontend/sdl/gui68k_sdl.c)
    target_link_libraries(mrc_sdl_pic2000 PUBLIC mrc_pic2000 mrc_host)
    if(SDL2_FOUND)
        target_compile_definitions(mrc_sdl_pic2000 PRIVATE MRC_HAVE_SDL)
        target_include_directories(mrc_sdl_pic2000 PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(mrc_sdl_pic2000 PUBLIC ${MRC_SDL_LIBRARY} mrc_sdl_display)
    endif()
    add_library(mrc_cli_pic2000 STATIC src/frontend/cli/main68k.c)
    target_link_libraries(mrc_cli_pic2000 PUBLIC mrc_sdl_pic2000 mrc_state mrc_host)

    target_compile_definitions(mcap PRIVATE MRC_HAVE_PIC2000)
    target_link_libraries(mcap PRIVATE mrc_cli_pic2000)
    message(STATUS "68k machines available on the project-owned CPU32 core")
else()
    message(STATUS "68k machines disabled")
endif()

if(ANDROID)
    return()
endif()

enable_testing()

#
# The machine, devices and CPU32 core are built and tested without a
# reference implementation.
#
if(TARGET mrc_pic2000)
    add_executable(test_pic_magicbus tests/runtime/test_pic_magicbus.cpp)
    target_link_libraries(test_pic_magicbus PRIVATE mrc_pic2000)
    add_test(NAME pic_magicbus COMMAND test_pic_magicbus)
    add_executable(test_pic2000_runtime tests/runtime/test_pic2000.cpp)
    add_executable(test_econoram tests/dev/test_econoram.cpp)
    target_include_directories(test_econoram PRIVATE src)
    set_target_properties(test_econoram PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_test(NAME econoram COMMAND test_econoram)
    target_link_libraries(test_pic2000_runtime PRIVATE mrc_pic2000)
    add_executable(test_m68k_state tests/runtime/test_m68k_state.cpp)
    target_link_libraries(test_m68k_state PRIVATE mrc_pic2000)
    set_target_properties(test_m68k_state PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_test(NAME m68k_state COMMAND test_m68k_state)
    add_executable(test_pic2000_audio tests/runtime/test_pic2000_audio.cpp)
    target_link_libraries(test_pic2000_audio PRIVATE mrc_pic2000)
    add_test(NAME pic2000_audio COMMAND test_pic2000_audio)
    set_target_properties(test_pic2000_runtime PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_test(NAME pic2000_runtime COMMAND test_pic2000_runtime)

    # The emitter against the interpreter: not a comparison with the
    # reference, so it needs nothing but the core itself.
    add_executable(test_m68k_emit tests/cpu/test_m68k_emit.cpp)
    target_include_directories(test_m68k_emit PRIVATE src)
    target_link_libraries(test_m68k_emit PRIVATE mrc_m68k_core)
    set_target_properties(test_m68k_emit PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    add_test(NAME m68k_emit COMMAND test_m68k_emit)
endif()

# CPU tests below exercise the project-owned engines directly.
add_executable(test_r3900 tests/cpu/test_r3900.c)
target_link_libraries(test_r3900 PRIVATE mrc_cpu_mips)
add_test(NAME r3900 COMMAND test_r3900)

add_executable(test_uart tests/dev/test_uart.c)
target_link_libraries(test_uart PRIVATE mrc_tx39)
add_test(NAME uart COMMAND test_uart)

add_executable(test_pen_queue tests/frontend/test_pen_queue.c)
target_include_directories(test_pen_queue PRIVATE src)
add_test(NAME pen_queue COMMAND test_pen_queue)
if(SDL2_FOUND)
    add_executable(test_display tests/frontend/test_display.c)
    target_link_libraries(test_display PRIVATE mrc_sdl_display)
    add_test(NAME sdl_display COMMAND test_display)

    add_executable(test_lcd tests/frontend/test_lcd.c)
    target_link_libraries(test_lcd PRIVATE mrc_sdl_display)
    add_test(NAME sdl_lcd COMMAND test_lcd)

    add_executable(test_ui tests/frontend/test_ui.c)
    target_link_libraries(test_ui PRIVATE mrc_sdl_display)
    add_executable(test_install_panel tests/frontend/test_install_panel.c)
    target_link_libraries(test_install_panel PRIVATE mrc_sdl_display mrc_host)
    add_test(NAME install_panel COMMAND test_install_panel)
    add_executable(test_card_panel tests/frontend/test_card_panel.c)
    target_link_libraries(test_card_panel PRIVATE mrc_sdl_display mrc_host)
    add_test(NAME card_panel COMMAND test_card_panel)
    add_executable(test_devices tests/frontend/test_devices.c)
    target_link_libraries(test_devices PRIVATE mrc_sdl_display)
    add_test(NAME devices COMMAND test_devices)
    add_test(NAME sdl_ui COMMAND test_ui)
endif()

add_executable(test_region tests/bus/test_region.c)
target_link_libraries(test_region PRIVATE mrc_common)
add_test(NAME region COMMAND test_region)

add_executable(test_mbus tests/dev/test_mbus.c)
target_link_libraries(test_mbus PRIVATE mrc_tx39)
add_test(NAME mbus COMMAND test_mbus)

add_executable(test_glacier tests/dev/test_glacier.c)
target_link_libraries(test_glacier PRIVATE mrc_devices)
add_test(NAME glacier COMMAND test_glacier)

add_executable(test_ne2000 tests/dev/test_ne2000.c)
target_link_libraries(test_ne2000 PRIVATE mrc_devices)
add_test(NAME ne2000 COMMAND test_ne2000)

add_executable(test_sib tests/dev/test_sib.c)
target_link_libraries(test_sib PRIVATE mrc_tx39)
add_test(NAME sib COMMAND test_sib)

add_executable(test_ucb_audio tests/dev/test_ucb_audio.c)
target_link_libraries(test_ucb_audio PRIVATE mrc_tx39)
add_test(NAME ucb_audio COMMAND test_ucb_audio)

add_executable(test_datarover_boot tests/dev/test_datarover_boot.c)
target_link_libraries(test_datarover_boot PRIVATE mrc_datarover840)
add_test(NAME datarover_boot COMMAND test_datarover_boot)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME datarover_ram COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_datarover_ram.py $<TARGET_FILE:mcap>)
    add_test(NAME rom_patch COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_rom_patch.py)
    add_test(NAME pclink COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_pclink.py)
    add_test(NAME modem_probe COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_modem.py)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME pclink_native COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_pclink_native.py)
    endif()
    if(TARGET mrc_pic2000)
        add_test(NAME rom_detection COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_rom_detection.py $<TARGET_FILE:mcap>)
        add_test(NAME m68k_cli COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/host/test_m68k_cli.py $<TARGET_FILE:mcap>)
    endif()
endif()

if(SLIRP_FOUND)
    add_executable(test_network tests/host/test_network.c)
    target_link_libraries(test_network PRIVATE mrc_host)
    add_test(NAME network COMMAND test_network)
endif()

add_executable(test_card_image tests/host/test_card_image.c)
target_link_libraries(test_card_image PRIVATE mrc_host mrc_devices)
add_test(NAME card_image COMMAND test_card_image)

add_executable(test_magicbus_keyboard tests/dev/test_magicbus_keyboard.c)
target_link_libraries(test_magicbus_keyboard PRIVATE mrc_devices)
add_test(NAME magicbus_keyboard COMMAND test_magicbus_keyboard)
add_executable(test_mbus_transport tests/dev/test_mbus_transport.c)
target_link_libraries(test_mbus_transport PRIVATE mrc_tx39)
add_test(NAME mbus_transport COMMAND test_mbus_transport)
add_executable(test_datarover_keyboard tests/dev/test_datarover_keyboard.c)
target_link_libraries(test_datarover_keyboard PRIVATE mrc_datarover840)
add_test(NAME datarover_keyboard COMMAND test_datarover_keyboard)
