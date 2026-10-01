# SPDX-License-Identifier: MIT

# foo_input_3sf: the foobar2000 component, a DLL built with clang-cl or MSVC on Windows and a
# .component bundle on macOS. Added by the top-level CMakeLists.txt when FOOBAR2000_SDK_DIR is
# set; see README.md.
#
# The SDK's Visual Studio and Xcode projects aren't used: the parts a component needs (pfc, the
# SDK and the component client) are compiled here. On Windows the SDK's prebuilt shared.dll import
# library is linked, and on macOS shared is compiled too, from the files its Xcode project builds.

set(SDK ${FOOBAR2000_SDK_DIR})
foreach (dir pfc foobar2000/SDK foobar2000/foobar2000_component_client foobar2000/shared)
    if (NOT EXISTS ${SDK}/${dir})
        message(FATAL_ERROR "FOOBAR2000_SDK_DIR (${SDK}) has no ${dir}: point it at the unpacked SDK")
    endif()
endforeach()

file(GLOB pfc_sources ${SDK}/pfc/*.cpp)
file(GLOB sdk_sources ${SDK}/foobar2000/SDK/*.cpp)
if (APPLE)
    # The SDK's Xcode projects also build its Objective-C++ files, with ARC, and leave out these
    # files, which are for Windows.
    enable_language(OBJCXX)
    file(GLOB pfc_objcxx_sources ${SDK}/pfc/*.mm)
    file(GLOB sdk_objcxx_sources ${SDK}/foobar2000/SDK/*.mm)
    list(APPEND pfc_sources ${pfc_objcxx_sources})
    list(APPEND sdk_sources ${sdk_objcxx_sources})
    list(REMOVE_ITEM pfc_sources ${SDK}/pfc/stdafx.cpp ${SDK}/pfc/win-objects.cpp)
    list(REMOVE_ITEM sdk_sources ${SDK}/foobar2000/SDK/stdafx.cpp ${SDK}/foobar2000/SDK/cfg_var_legacy.cpp
         ${SDK}/foobar2000/SDK/commandline.cpp ${SDK}/foobar2000/SDK/componentversion.cpp)
    set(sdk_options -w $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
    set(sdk_options /W0)
endif()

add_library(fb2k_pfc STATIC ${pfc_sources})
# SYSTEM, so that the SDK's headers don't raise warnings in the component's build.
target_include_directories(fb2k_pfc SYSTEM PUBLIC ${SDK} ${SDK}/foobar2000)
if (WIN32)
    target_compile_definitions(fb2k_pfc PUBLIC UNICODE _UNICODE NOMINMAX)
endif()
target_compile_options(fb2k_pfc PRIVATE ${sdk_options})

add_library(fb2k_sdk STATIC ${sdk_sources})
target_link_libraries(fb2k_sdk PUBLIC fb2k_pfc)
target_compile_options(fb2k_sdk PRIVATE ${sdk_options})

if (APPLE)
    # The files the SDK's shared.xcodeproj builds. The rest of foobar2000/shared is for Windows.
    add_library(fb2k_shared STATIC
        ${SDK}/foobar2000/shared/audio_math.cpp
        ${SDK}/foobar2000/shared/shared-apple.mm
        ${SDK}/foobar2000/shared/shared-nix.cpp
        ${SDK}/foobar2000/shared/utf8.cpp
    )
    target_link_libraries(fb2k_shared PUBLIC fb2k_pfc)
    target_compile_options(fb2k_shared PRIVATE ${sdk_options})

    add_library(foo_input_3sf MODULE
        foo_input_3sf.cpp
        ${SDK}/foobar2000/foobar2000_component_client/component_client.cpp
    )
    # The component client only uses some of its parameters with cfg_var_legacy, which is for Windows, so it's built
    # without warnings like the rest of the SDK.
    set_source_files_properties(${SDK}/foobar2000/foobar2000_component_client/component_client.cpp
                                PROPERTIES COMPILE_OPTIONS -w)
    # The bundle identifier follows the SDK sample's (com.foobar2000.foo-sample).
    set_target_properties(foo_input_3sf PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION component
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME foo_input_3sf
        MACOSX_BUNDLE_GUI_IDENTIFIER com.foobar2000.foo-input-3sf
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${threesf_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${threesf_VERSION}
    )
    target_link_libraries(foo_input_3sf PRIVATE fb2k_sdk fb2k_shared threesf_core "-framework Cocoa")
    target_compile_options(foo_input_3sf PRIVATE ${THREESF_WARNINGS})
    # foobar2000 loads every component into one process, and dyld can bind an exported C++ symbol, such as an inline
    # SDK function, to another component's copy. The SDK's Xcode projects build with hidden symbols for that reason, so
    # only the entry point is exported here, and code nothing uses is stripped.
    target_link_options(foo_input_3sf PRIVATE -Wl,-exported_symbol,_foobar2000_get_interface -Wl,-dead_strip)
    # The linker signs only the executable. Signing the bundle ad hoc seals its Info.plist too.
    add_custom_command(TARGET foo_input_3sf POST_BUILD
                       COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:foo_input_3sf> VERBATIM)
else()
    # The shared.dll import library for this architecture.
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(fb2k_arch x64)
    else()
        set(fb2k_arch Win32)
    endif()
    find_library(FOOBAR2000_SHARED_LIB
        NAMES shared-${fb2k_arch} shared
        PATHS ${SDK}/foobar2000/shared ${SDK}/foobar2000/shared/${fb2k_arch}
        NO_DEFAULT_PATH REQUIRED)

    add_library(foo_input_3sf SHARED
        foo_input_3sf.cpp
        ${SDK}/foobar2000/foobar2000_component_client/component_client.cpp
    )
    target_link_libraries(foo_input_3sf PRIVATE fb2k_sdk threesf_core ${FOOBAR2000_SHARED_LIB})
    # NOMINMAX comes with fb2k_pfc. The SDK's headers get by without the parts of <windows.h> this
    # leaves out, but its own sources aren't built with it.
    target_compile_definitions(foo_input_3sf PRIVATE WIN32_LEAN_AND_MEAN)
    # C4702 (unreachable code) comes from the SDK: its raw-decoding wrappers call input_stubs::decode_run_raw, which
    # only throws. The optimizer reports it in the SDK's headers, where SYSTEM (/external:W0) doesn't reach.
    target_compile_options(foo_input_3sf PRIVATE ${THREESF_WARNINGS} /wd4702)
endif()

threesf_add_version(foo_input_3sf "3SF decoder for foobar2000")
