# SPDX-License-Identifier: MIT

# foo_input_3sf: the foobar2000 component, a DLL built with clang-cl or MSVC on Windows and a .component bundle on
# macOS. Added by the top-level CMakeLists.txt when FOOBAR2000_SDK_DIR is set. See README.md. xsflib builds it from the
# SDK's parts (xsflib_add_foobar2000_component).

# On macOS the SDK has Objective-C++ files. CMake enables a language at file scope only, not inside xsflib's function.
if (APPLE)
    enable_language(OBJCXX)
endif()

xsflib_add_foobar2000_component(foo_input_3sf
    SDK ${FOOBAR2000_SDK_DIR}
    SOURCES foo_input_3sf.cpp
    LIBRARIES threesf_core
    VERSION ${threesf_VERSION}
    BUNDLE_ID com.foobar2000.foo-input-3sf
    OPTIONS ${THREESF_WARNINGS})
xsflib_add_version(foo_input_3sf ${THREESF_VERSION_ARGS} DESCRIPTION "3SF decoder for foobar2000")
