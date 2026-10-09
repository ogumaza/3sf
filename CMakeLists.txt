# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
# Shared version for 3sfrip, 3sfplay, the foobar2000 component, Windows file properties and macOS bundles
# (threesf_add_version below). For a release, update it here and add notes in docs/release-notes/.
project(threesf VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Reject unsupported compilers. Windows builds require clang-cl or MSVC. On 32-bit x86, GCC and Clang use x87
# floating point with extra precision, producing different samples from other builds. The supported 32-bit builds
# use clang-cl or MSVC with SSE2, for 32-bit foobar2000.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(compiler_name GCC)
else()
    set(compiler_name ${CMAKE_CXX_COMPILER_ID})
endif()
if (WIN32 AND NOT MSVC)
    message(FATAL_ERROR "3SF doesn't support ${compiler_name} on Windows, such as MinGW's GCC or the GNU-style "
                        "clang++. Build with Visual Studio's clang-cl (cmake -T ClangCL) or with MSVC; README.md has "
                        "the steps.")
endif()
if (NOT MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i.86|x86|x86_64|AMD64)$")
    message(FATAL_ERROR "3SF doesn't support 32-bit x86 builds with ${compiler_name}: their samples would differ "
                        "from other builds. Build for 64-bit instead, or on Windows use clang-cl or "
                        "MSVC, which build 32-bit x86 with SSE2.")
endif()
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
# MSVC: the static C runtime, so that the foobar2000 component needs no runtime DLLs (also in
# subprojects that declare an older minimum CMake version).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
# Teakra sets CMAKE_WARN_DEPRECATED, which CMake 4.4 and later ignore and warn about (CMP0218).
set(CMAKE_POLICY_DEFAULT_CMP0218 NEW)
# clang-cl, which the Windows builds use (-T ClangCL), takes MSVC's options but is Clang underneath. It gets Clang's
# settings below through /clang:, and ignores MSVC's /d1trimfile.
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(THREESF_CLANG_CL ON)
endif()
# Output must not depend on the platform: no fused multiply-add contraction (clang contracts by
# default where the CPU has FMA, as on Apple Silicon), so that every build computes the same
# floats (the emulated VFP, the nw::snd model, length analysis). MSVC's /fp:precise doesn't
# contract.
if (THREESF_CLANG_CL)
    add_compile_options(/clang:-ffp-contract=off)
    # clang-cl does 32-bit x86 floating point on the x87 unit, with more precision than float and double have, unless
    # it's given SSE2, which MSVC uses there by default.
    if (CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_compile_options(/arch:SSE2)
    endif()
elseif (NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()
# __FILE__ gives paths relative to the source or build directory. Teakra's assertions use it in every
# build. Debug information keeps absolute paths, so that debuggers find the sources. MSVC's
# /d1trimfile isn't documented, but Visual Studio 2019 16.4 and later have it.
if (THREESF_CLANG_CL)
    add_compile_options("/clang:-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "/clang:-fmacro-prefix-map=${CMAKE_BINARY_DIR}/=")
elseif (MSVC)
    cmake_path(NATIVE_PATH CMAKE_SOURCE_DIR source_dir)
    cmake_path(NATIVE_PATH CMAKE_BINARY_DIR binary_dir)
    add_compile_options(/d1trimfile:${source_dir} /d1trimfile:${binary_dir})
else()
    add_compile_options(-fmacro-prefix-map=${CMAKE_SOURCE_DIR}/= -fmacro-prefix-map=${CMAKE_BINARY_DIR}/=)
endif()

# Teakra (MIT): the Teak DSP emulator. externals/teakra is upstream commit 3d697a1 with 3SF's changes.
set(TEAKRA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(TEAKRA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/teakra EXCLUDE_FROM_ALL)

# The interpreter has a handler for each value of the common instructions' register operands: thousands of functions
# in one object file, which can be more sections than a Windows object file holds by default.
if (MSVC)
    target_compile_options(teakra PRIVATE /bigobj)
endif()

if (MSVC)
    set(THREESF_WARNINGS /W4)
    # fopen and freopen are standard C++, and the _s versions MSVC suggests instead aren't portable.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    set(THREESF_WARNINGS -Wall -Wextra)
endif()

# Small header-only helpers the other libraries share (byte order, ASCII, quiet stdout), and the include directory
# for all of src.
add_library(threesf_common INTERFACE
    src/common/ascii.h
    src/common/bytes.h
    src/common/common_types.h
    src/common/memory_image.h
    src/common/quiet_stdout.h
)
target_include_directories(threesf_common INTERFACE src)

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

# CTR NintendoWare sound formats (BCSAR, CSEQ, CBNK, CWAR, CWAV).
add_library(threesf_csar STATIC
    src/csar/binary.h
    src/csar/formats.cpp
    src/csar/formats.h
)
target_link_libraries(threesf_csar PUBLIC threesf_common)
target_compile_options(threesf_csar PRIVATE ${THREESF_WARNINGS})

# nw::snd model: sequence player, channels, envelopes and the voice layer above nn::snd.
add_library(threesf_nwsnd STATIC
    src/nwsnd/driver.cpp
    src/nwsnd/driver.h
    src/nwsnd/engine.cpp
    src/nwsnd/engine.h
    src/nwsnd/envelope.cpp
    src/nwsnd/envelope.h
    src/nwsnd/sequence.cpp
    src/nwsnd/sequence.h
    src/nwsnd/tables.h
    src/nwsnd/util.cpp
    src/nwsnd/util.h
)
target_link_libraries(threesf_nwsnd PUBLIC threesf_nnsnd threesf_csar)
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

# zlib for the PSF container: the system's, or fetched (e.g. for Windows builds).
find_package(ZLIB QUIET)
if (NOT ZLIB_FOUND)
    include(FetchContent)
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG v1.3.1
        GIT_SHALLOW TRUE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    # Build only the static library, which 3SF links. zlib's DLL would be built by default too, and its resource script
    # doesn't compile with clang-cl.
    set_property(DIRECTORY ${zlib_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    target_include_directories(zlibstatic PUBLIC ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# 3SF core: the file format and the player (the emulated process). Portable, with no
# platform audio or UI; used by the CLI tools and the foobar2000 plugin.
add_library(threesf_core STATIC
    src/threesf/archive_player.cpp
    src/threesf/archive_player.h
    src/threesf/format.cpp
    src/threesf/format.h
    src/threesf/playback.cpp
    src/threesf/playback.h
    src/threesf/player.cpp
    src/threesf/player.h
    src/threesf/render_ahead.cpp
    src/threesf/render_ahead.h
)
find_package(Threads REQUIRED)
target_link_libraries(threesf_core PUBLIC threesf_horizon threesf_nwsnd ZLIB::ZLIB Threads::Threads)
target_compile_options(threesf_core PRIVATE ${THREESF_WARNINGS})

# Ripper: game profiles, the guest driver (driver/driver.c, prebuilt) and length analysis.
add_library(threesf_rip STATIC
    src/rip/driver_blob.h
    src/rip/game.cpp
    src/rip/game.h
    src/rip/rip.cpp
    src/rip/rip.h
)
target_link_libraries(threesf_rip PUBLIC threesf_core threesf_nwsnd)
target_compile_options(threesf_rip PRIVATE ${THREESF_WARNINGS})

# WAV/FLAC output for the command-line tools. libFLAC comes from its CMake package (vcpkg and
# other CMake-built installs), then pkg-config (Homebrew and Linux distributions), and is fetched
# and built from source when neither has it. A universal macOS build (more than one architecture
# in CMAKE_OSX_ARCHITECTURES) always builds it, because an installed libFLAC, such as Homebrew's,
# is built for the Mac's architecture only.
add_library(threesf_audio_out STATIC src/cli/audio_out.cpp src/cli/audio_out.h)
target_link_libraries(threesf_audio_out PUBLIC threesf_common)
target_compile_options(threesf_audio_out PRIVATE ${THREESF_WARNINGS})
list(LENGTH CMAKE_OSX_ARCHITECTURES osx_architectures)
if (osx_architectures LESS 2)
    find_package(FLAC CONFIG QUIET)
    if (FLAC_FOUND)
        set(THREESF_FLAC FLAC::FLAC)
    else()
        find_package(PkgConfig QUIET)
        if (PKG_CONFIG_FOUND)
            pkg_check_modules(FLAC QUIET IMPORTED_TARGET flac)
        endif()
        if (FLAC_FOUND)
            set(THREESF_FLAC PkgConfig::FLAC)
        endif()
    endif()
endif()
if (NOT THREESF_FLAC)
    include(FetchContent)
    FetchContent_Declare(flac
        GIT_REPOSITORY https://github.com/xiph/flac.git
        GIT_TAG 1.5.0
        GIT_SHALLOW TRUE)
    foreach (option BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS INSTALL_MANPAGES
                    INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    FetchContent_MakeAvailable(flac)
    # Build only libFLAC. The helper libraries of flac's programs would be built by default too, and one of them
    # (getopt) doesn't compile with clang 22's clang-cl. flac also turns on -Wall for Clang, which clang-cl takes as
    # -Weverything, so libFLAC is built without warnings there.
    set_property(DIRECTORY ${flac_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
    if (THREESF_CLANG_CL)
        target_compile_options(FLAC PRIVATE /w)
    endif()
    set(THREESF_FLAC FLAC::FLAC)
endif()
target_link_libraries(threesf_audio_out PRIVATE ${THREESF_FLAC})

# Sets THREESF_VERSION and, on Windows, adds the version resource shown in Explorer's file properties.
function(threesf_add_version target description)
    target_compile_definitions(${target} PRIVATE THREESF_VERSION="${threesf_VERSION}")
    if (WIN32)
        get_target_property(type ${target} TYPE)
        if (type STREQUAL "EXECUTABLE")
            set(VERSION_FILE_TYPE VFT_APP)
            set(VERSION_FILE_NAME ${target}${CMAKE_EXECUTABLE_SUFFIX})
        else()
            set(VERSION_FILE_TYPE VFT_DLL)
            set(VERSION_FILE_NAME ${target}${CMAKE_SHARED_LIBRARY_SUFFIX})
        endif()
        set(VERSION_DESCRIPTION ${description})
        set(VERSION_NAME ${target})
        set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
        configure_file(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/platform/windows/version.rc.in ${rc} @ONLY)
        target_sources(${target} PRIVATE ${rc})
    endif()
endfunction()

add_executable(3sfrip src/cli/3sfrip.cpp src/cli/console.cpp src/cli/console.h)
target_link_libraries(3sfrip PRIVATE threesf_rip)
target_compile_options(3sfrip PRIVATE ${THREESF_WARNINGS})
threesf_add_version(3sfrip "3SF ripper")

add_executable(3sfplay src/cli/3sfplay.cpp src/cli/console.cpp src/cli/console.h)
target_link_libraries(3sfplay PRIVATE threesf_core threesf_audio_out)
target_compile_options(3sfplay PRIVATE ${THREESF_WARNINGS})
threesf_add_version(3sfplay "3SF renderer")

foreach (tool 3sfrip 3sfplay)
    # Windows: UTF-8 as the tools' code page, so that they can take and open paths outside the system's ANSI code page,
    # such as those of files dropped on them. The linker merges the manifest into the one it makes.
    if (MSVC)
        target_sources(${tool} PRIVATE platform/windows/utf8.manifest)
    endif()

    # macOS: a droplet app for each tool (3sfrip.app, 3sfplay.app), so that files can be dropped on it in the Finder.
    # It's an AppleScript applet with the tool inside, in Contents/Resources, and the version in its Info.plist.
    if (APPLE)
        find_program(OSACOMPILE osacompile)
        find_program(CODESIGN codesign)
        find_program(PLUTIL plutil)
        if (OSACOMPILE AND CODESIGN AND PLUTIL)
            set(DROPLET_TOOL ${tool})
            set(droplet_app ${CMAKE_BINARY_DIR}/${tool}.app)
            set(droplet_script ${CMAKE_BINARY_DIR}/${tool}.applescript)
            configure_file(platform/macos/droplet.applescript.in ${droplet_script} @ONLY)
            add_custom_command(
                OUTPUT ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CMAKE_COMMAND} -E rm -rf ${droplet_app}
                COMMAND ${OSACOMPILE} -o ${droplet_app} ${droplet_script}
                COMMAND ${PLUTIL} -replace CFBundleShortVersionString -string ${threesf_VERSION}
                        ${droplet_app}/Contents/Info.plist
                COMMAND ${PLUTIL} -replace CFBundleVersion -string ${threesf_VERSION} ${droplet_app}/Contents/Info.plist
                COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${tool}> ${droplet_app}/Contents/Resources/${tool}
                COMMAND ${CODESIGN} --force --deep --sign - ${droplet_app}
                DEPENDS ${tool} ${droplet_script}
                COMMENT "Building ${tool}.app"
                VERBATIM)
            add_custom_target(${tool}_app ALL DEPENDS ${droplet_app}/Contents/Resources/${tool})
        endif()
    endif()
endforeach()

# foobar2000 component (Windows with clang-cl or MSVC, or macOS): cmake -DFOOBAR2000_SDK_DIR=<unpacked SDK> ...
set(FOOBAR2000_SDK_DIR "" CACHE PATH "foobar2000 SDK (builds the foo_input_3sf component when set)")
if (FOOBAR2000_SDK_DIR)
    if (NOT MSVC AND NOT APPLE)
        message(FATAL_ERROR "the foobar2000 component needs clang-cl or MSVC on Windows, or macOS")
    endif()
    add_subdirectory(plugins/foobar2000)
endif()

add_executable(format_test tests/format_test.cpp)
target_link_libraries(format_test PRIVATE threesf_core)
target_compile_options(format_test PRIVATE ${THREESF_WARNINGS})

add_executable(kernel_test tests/kernel_test.cpp)
target_link_libraries(kernel_test PRIVATE threesf_horizon)
target_compile_options(kernel_test PRIVATE ${THREESF_WARNINGS})

add_executable(teakra_bounds_test tests/teakra_bounds_test.cpp)
target_link_libraries(teakra_bounds_test PRIVATE threesf_dsp)
target_compile_options(teakra_bounds_test PRIVATE ${THREESF_WARNINGS})

add_executable(teakra_dsp_test tests/teakra_dsp_test.cpp)
target_link_libraries(teakra_dsp_test PRIVATE threesf_dsp)
target_compile_options(teakra_dsp_test PRIVATE ${THREESF_WARNINGS})

add_executable(teakra_jit_test tests/teakra_jit_test.cpp tests/teakra_programs.h)
target_link_libraries(teakra_jit_test PRIVATE threesf_dsp)
target_compile_options(teakra_jit_test PRIVATE ${THREESF_WARNINGS})

add_executable(teakra_state_test tests/teakra_state_test.cpp tests/teakra_programs.h)
target_link_libraries(teakra_state_test PRIVATE threesf_dsp)
target_compile_options(teakra_state_test PRIVATE ${THREESF_WARNINGS})

add_executable(snapshot_test tests/snapshot_test.cpp)
target_link_libraries(snapshot_test PRIVATE threesf_common)
target_compile_options(snapshot_test PRIVATE ${THREESF_WARNINGS})

add_executable(render_ahead_test tests/render_ahead_test.cpp)
target_link_libraries(render_ahead_test PRIVATE threesf_core)
target_compile_options(render_ahead_test PRIVATE ${THREESF_WARNINGS})

add_executable(csar_test tests/csar_test.cpp)
target_link_libraries(csar_test PRIVATE threesf_csar)
target_compile_options(csar_test PRIVATE ${THREESF_WARNINGS})

add_executable(file_names_test tests/file_names_test.cpp)
target_link_libraries(file_names_test PRIVATE threesf_rip)
target_compile_options(file_names_test PRIVATE ${THREESF_WARNINGS})

add_executable(rip_test tests/rip_test.cpp)
target_link_libraries(rip_test PRIVATE threesf_rip)
target_compile_options(rip_test PRIVATE ${THREESF_WARNINGS})

enable_testing()
add_test(NAME format COMMAND format_test)
add_test(NAME kernel COMMAND kernel_test)
add_test(NAME teakra_bounds COMMAND teakra_bounds_test)
add_test(NAME teakra_dsp COMMAND teakra_dsp_test)
add_test(NAME teakra_jit COMMAND teakra_jit_test)
add_test(NAME teakra_state COMMAND teakra_state_test)
add_test(NAME snapshot COMMAND snapshot_test)
add_test(NAME render_ahead COMMAND render_ahead_test)
add_test(NAME csar COMMAND csar_test)
add_test(NAME file_names COMMAND file_names_test)
add_test(NAME rip COMMAND rip_test)

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
file(GLOB local_tests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/*/local_tests.cmake)
foreach (file IN LISTS local_tests)
    include(${file})
endforeach()
