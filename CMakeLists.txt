# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for 3sfrip, 3sfplay, the foobar2000 component, the XMPlay plugin, Windows file properties and macOS
# bundles (THREESF_VERSION_ARGS below). For a release, update it here and add notes in docs/release-notes/.
project(threesf VERSION 1.5.1 LANGUAGES CXX)

# xsflib is a library for players and rippers of the PSF family. It's a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for 3SF's targets (THREESF_WARNINGS). It refuses the compilers
# whose output would differ from other builds.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Clone with git clone --recursive, or run "
                        "git submodule update --init --recursive in a clone.")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME 3SF PREFIX THREESF)
# clang-cl's -Wmissing-designated-field-initializers asks for every field of a designated initializer. Filled-in fields
# would copy the struct's defaults into each initializer, where they could drift from the struct. The warning is off
# for 3SF's targets. The libraries keep it.
if (THREESF_CLANG_CL)
    list(APPEND THREESF_WARNINGS -Wno-missing-designated-field-initializers)
endif()

# xsflib also provides zlib for the PSF container and libFLAC for FLAC output. Their targets are xsflib::zlib and
# xsflib::flac. ntdlib, a library for the hardware and formats of Nintendo's consoles, builds on it. ntdlib::ctr, a
# module of ntdlib, contains Teakra (MIT, the Teak DSP emulator, in externals/teakra of ntdlib) for the DSP of the 3DS.
# It also contains the models of nn::snd and nw::snd that archive mode uses.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
set(NTDLIB_CTR ON CACHE BOOL "" FORCE)
xsflib_add_libraries(xsflib ntdlib)

# What every 3SF target builds on: the include directory for all of src, and the common code of xsflib and ntdlib (byte
# order, ASCII, memory images).
add_library(threesf_common INTERFACE)
target_include_directories(threesf_common INTERFACE src)
target_link_libraries(threesf_common INTERFACE xsflib::common ntdlib::common)

# The effects on the aux buses of nn::snd, on the models in ntdlib::ctr_fx, and the start of the archive-mode model with
# those effects.
add_library(threesf_nnsnd STATIC
    src/nnsnd/archive_start.cpp
    src/nnsnd/archive_start.h
    src/nnsnd/aux_effects.cpp
    src/nnsnd/aux_effects.h
)
target_link_libraries(threesf_nnsnd PUBLIC threesf_common ntdlib::ctr ntdlib::ctr_fx)
target_compile_options(threesf_nnsnd PRIVATE ${THREESF_WARNINGS})

# ARM11 (ARMv6K + VFPv2) user-mode interpreter that runs the game's sound code.
add_library(threesf_arm STATIC
    src/arm/cpu.cpp
    src/arm/cpu.h
    src/arm/media.cpp
    src/arm/memory.cpp
    src/arm/memory.h
    src/arm/thumb.cpp
    src/arm/vfp.cpp
)
target_link_libraries(threesf_arm PUBLIC threesf_common)
target_compile_options(threesf_arm PRIVATE ${THREESF_WARNINGS})
# VFP arithmetic is done with host floats, and the host rounding mode follows FPSCR at run time.
if (MSVC)
    target_compile_options(threesf_arm PRIVATE /fp:strict)
else()
    target_compile_options(threesf_arm PRIVATE -frounding-math)
endif()

# Horizon kernel HLE, services and the emulated process (kernel + Teakra DSP).
add_library(threesf_horizon STATIC
    src/horizon/ipc.h
    src/horizon/kernel.cpp
    src/horizon/kernel.h
    src/horizon/kernel_constants.h
    src/horizon/kernel_memory.cpp
    src/horizon/kernel_svc.cpp
    src/horizon/kernel_sync.cpp
    src/horizon/romfs.cpp
    src/horizon/romfs.h
    src/horizon/services.cpp
    src/horizon/services.h
    src/horizon/system.cpp
    src/horizon/system.h
)
target_link_libraries(threesf_horizon PUBLIC threesf_arm ntdlib::ctr)
target_compile_options(threesf_horizon PRIVATE ${THREESF_WARNINGS})

# 3SF core: the file format and the player (the emulated process). It's portable, with no platform audio or UI. The CLI
# tools, the foobar2000 component and the XMPlay plugin use it.
add_library(threesf_core STATIC
    src/threesf/archive_player.cpp
    src/threesf/archive_player.h
    src/threesf/format.cpp
    src/threesf/format.h
    src/threesf/game_mode.cpp
    src/threesf/game_mode.h
    src/threesf/player.cpp
    src/threesf/player.h
)
target_link_libraries(threesf_core PUBLIC threesf_horizon threesf_nnsnd xsflib::player)
target_compile_options(threesf_core PRIVATE ${THREESF_WARNINGS})

# Ripper: game profiles, the guest driver (driver/driver.c, prebuilt), the games' effects on the aux buses and length
# analysis.
add_library(threesf_rip STATIC
    src/rip/blz.cpp
    src/rip/blz.h
    src/rip/driver_blob.h
    src/rip/effects.cpp
    src/rip/effects.h
    src/rip/game.cpp
    src/rip/game.h
    src/rip/length.cpp
    src/rip/length.h
    src/rip/music.cpp
    src/rip/music.h
    src/rip/rip.cpp
    src/rip/firmware.cpp
    src/rip/archive_mode.cpp
    src/rip/rip.h
)
target_link_libraries(threesf_rip PUBLIC threesf_core ntdlib::rip_archive xsflib::rip
                      PRIVATE ntdlib::nwsnd_commands xsflib::zlib)
target_compile_options(threesf_rip PRIVATE ${THREESF_WARNINGS})

# The version of each program, as THREESF_VERSION, and on Windows the version resource that Explorer shows in the file's
# properties (xsflib_add_version).
set(THREESF_VERSION_ARGS VERSION ${threesf_VERSION} DEFINE THREESF_VERSION PRODUCT 3SF
    COPYRIGHT "Copyright (c) 2026 the 3SF authors. MIT licence.")

add_executable(3sfrip src/cli/3sfrip.cpp src/cli/rip_jobs.cpp src/cli/firmware_inputs.cpp)
target_link_libraries(3sfrip PRIVATE threesf_rip xsflib::cli)
target_compile_options(3sfrip PRIVATE ${THREESF_WARNINGS})
xsflib_add_version(3sfrip ${THREESF_VERSION_ARGS} DESCRIPTION "3SF ripper")

add_executable(3sfplay src/cli/3sfplay.cpp)
target_link_libraries(3sfplay PRIVATE threesf_core xsflib::cli)
target_compile_options(3sfplay PRIVATE ${THREESF_WARNINGS})
xsflib_add_version(3sfplay ${THREESF_VERSION_ARGS} DESCRIPTION "3SF renderer")

# Windows: UTF-8 as the tools' code page. The tools can then take and open paths outside the system's ANSI code page.
# Files dropped on them can have such paths. macOS: a droplet app for each tool (3sfrip.app, 3sfplay.app). Files can
# then be dropped on it in the Finder.
foreach (tool 3sfrip 3sfplay)
    xsflib_add_utf8_manifest(${tool})
    xsflib_add_droplet(${tool} VERSION ${threesf_VERSION})
endforeach()

# foobar2000 component (Windows with clang-cl, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_3sf component when set)")
if (FOOBAR2000_SDK_DIR)
    add_subdirectory(plugins/foobar2000)
endif()

# XMPlay plugin (32-bit Windows with clang-cl): cmake -A Win32 -T ClangCL -DXMPLAY_SDK_DIR=<unpacked SDK> ...
set(XMPLAY_SDK_DIR "" CACHE PATH "XMPlay plugin SDK (builds the xmp-3sf plugin when set)")
if (XMPLAY_SDK_DIR)
    add_subdirectory(plugins/xmplay)
endif()

# The tests. -DTHREESF_TESTS=OFF leaves them out of a build that only makes the programs and plugins, such as CI's
# macOS job.
option(THREESF_TESTS "Build 3SF's tests and register them with CTest." ON)
if (THREESF_TESTS)
    enable_testing()

    # The unit tests: threesf_<name>_test from tests/<name>_test.cpp, registered as threesf_<name>.
    xsflib_add_project_test(threesf format threesf_core)
    xsflib_add_project_test(threesf aux_effects threesf_core)
    xsflib_add_project_test(threesf kernel threesf_horizon)
    xsflib_add_project_test(threesf rip threesf_rip ntdlib::testing)

    # The tools report the version they were built as.
    foreach (tool 3sfrip 3sfplay)
        xsflib_add_version_test(${tool} VERSION ${threesf_VERSION} NAME version_${tool})
    endforeach()

    # --var takes N=V with N from 0 to 31, and anything else is an error before an input is read.
    add_test(NAME var_out_of_range COMMAND 3sfrip --var 32=1 no-such-archive.bcsar)
    set_tests_properties(var_out_of_range PROPERTIES PASS_REGULAR_EXPRESSION "error: --var takes N=V")

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
