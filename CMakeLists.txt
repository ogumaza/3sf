# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for 3sfrip, 3sfplay, the foobar2000 component, the XMPlay plugin, Windows file properties and macOS
# bundles (THREESF_VERSION_ARGS below). For a release, update it here and add notes in docs/release-notes/.
project(threesf VERSION 1.3.1 LANGUAGES CXX)

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
# Teakra sets CMAKE_WARN_DEPRECATED, which CMake 4.4 and later ignore and warn about (CMP0218).
set(CMAKE_POLICY_DEFAULT_CMP0218 NEW)

# xsflib also provides zlib for the PSF container and libFLAC for FLAC output. Their targets are xsflib::zlib and
# xsflib::flac. ntdlib, a library for the hardware and formats of Nintendo's consoles, builds on it.
set(XSFLIB_PSF ON)
set(XSFLIB_CLI ON)
xsflib_add_libraries(xsflib ntdlib)

# Teakra (MIT): the Teak DSP emulator. externals/teakra is upstream commit 3d697a1 with 3SF's changes.
set(TEAKRA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(TEAKRA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/teakra EXCLUDE_FROM_ALL)

# The interpreter has a handler for each value of the common instructions' register operands: thousands of functions
# in one object file, which can be more sections than a Windows object file holds by default.
if (MSVC)
    target_compile_options(teakra PRIVATE /bigobj)
endif()

# What every 3SF target builds on: the include directory for all of src, and the common code of xsflib and ntdlib (byte
# order, ASCII, memory images). Its headers hold the options for reading sound archives, and quiet stdout for the tools
# and tests.
add_library(threesf_common INTERFACE
    src/common/quiet_stdout.h
    src/common/sar_options.h
)
target_include_directories(threesf_common INTERFACE src)
target_link_libraries(threesf_common INTERFACE xsflib::common ntdlib::common)

# The DSP: Teakra running the game's audio firmware.
add_library(threesf_dsp STATIC
    src/dsp/dsp.h
    src/dsp/fcram.h
    src/dsp/teakra_dsp.cpp
    src/dsp/teakra_dsp.h
)
target_link_libraries(threesf_dsp PUBLIC threesf_common teakra)
target_compile_options(threesf_dsp PRIVATE ${THREESF_WARNINGS})

# nn::snd model: the ARM11 side of the DSP protocol.
add_library(threesf_nnsnd STATIC
    src/nnsnd/dsp_abi.h
    src/nnsnd/dsp_link.cpp
    src/nnsnd/dsp_link.h
    src/nnsnd/sound_system.cpp
    src/nnsnd/sound_system.h
    src/nnsnd/voice.cpp
    src/nnsnd/voice.h
)
target_link_libraries(threesf_nnsnd PUBLIC threesf_dsp)
target_compile_options(threesf_nnsnd PRIVATE ${THREESF_WARNINGS})

# nw::snd model: ntdlib's sequence player, channels and envelopes with the 3DS's rules, and the engine and the voice
# layer above nn::snd.
add_library(threesf_nwsnd STATIC
    src/nwsnd/engine.cpp
    src/nwsnd/engine.h
    src/nwsnd/sequence.cpp
    src/nwsnd/sequence.h
    src/nwsnd/tables.h
    src/nwsnd/util.cpp
    src/nwsnd/util.h
    src/nwsnd/voice.cpp
    src/nwsnd/voice.h
)
target_link_libraries(threesf_nwsnd PUBLIC threesf_nnsnd ntdlib::nwsnd ntdlib::sar)
target_compile_options(threesf_nwsnd PRIVATE ${THREESF_WARNINGS})

# ARM11 (ARMv6K + VFPv2) user-mode interpreter that runs the game's sound code.
add_library(threesf_arm STATIC
    src/arm/cpu.cpp
    src/arm/cpu.h
    src/arm/media.cpp
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
    src/horizon/romfs.cpp
    src/horizon/romfs.h
    src/horizon/services.cpp
    src/horizon/services.h
    src/horizon/system.cpp
    src/horizon/system.h
)
target_link_libraries(threesf_horizon PUBLIC threesf_arm threesf_dsp)
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
target_link_libraries(threesf_core PUBLIC threesf_horizon threesf_nwsnd xsflib::player)
target_compile_options(threesf_core PRIVATE ${THREESF_WARNINGS})

# Ripper: game profiles, the guest driver (driver/driver.c, prebuilt) and length analysis.
add_library(threesf_rip STATIC
    src/rip/driver_blob.h
    src/rip/game.cpp
    src/rip/game.h
    src/rip/length.cpp
    src/rip/length.h
    src/rip/music.cpp
    src/rip/music.h
    src/rip/rip.cpp
    src/rip/rip.h
)
target_link_libraries(threesf_rip PUBLIC threesf_core threesf_nwsnd ntdlib::rip_archive xsflib::rip
                      PRIVATE xsflib::zlib)
target_compile_options(threesf_rip PRIVATE ${THREESF_WARNINGS})

# The version of each program, as THREESF_VERSION, and on Windows the version resource that Explorer shows in the file's
# properties (xsflib_add_version).
set(THREESF_VERSION_ARGS VERSION ${threesf_VERSION} DEFINE THREESF_VERSION PRODUCT 3SF
    COPYRIGHT "Copyright (c) 2026 the 3SF authors. MIT licence.")

add_executable(3sfrip src/cli/3sfrip.cpp)
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
    xsflib_add_project_test(threesf kernel threesf_horizon)
    xsflib_add_project_test(threesf teakra_bounds threesf_dsp)
    xsflib_add_project_test(threesf teakra_dsp threesf_dsp)
    xsflib_add_project_test(threesf teakra_jit threesf_dsp)
    xsflib_add_project_test(threesf teakra_state threesf_dsp)
    xsflib_add_project_test(threesf rip threesf_rip)

    # The tools report the version they were built as.
    string(REPLACE "." "\\." version_pattern ${threesf_VERSION})
    foreach (tool 3sfrip 3sfplay)
        add_test(NAME version_${tool} COMMAND ${tool} --version)
        set_tests_properties(version_${tool} PROPERTIES PASS_REGULAR_EXPRESSION "^${tool} ${version_pattern}[\r\n]*$")
    endforeach()

    # --var takes N=V with N from 0 to 31, and anything else is an error before an input is read.
    add_test(NAME var_out_of_range COMMAND 3sfrip --var 32=1 no-such-archive.bcsar)
    set_tests_properties(var_out_of_range PROPERTIES PASS_REGULAR_EXPRESSION "error: --var takes N=V")

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
