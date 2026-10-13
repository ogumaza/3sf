# SPDX-License-Identifier: MIT

# xmp-3sf: the XMPlay input plugin, a 32-bit DLL built with clang-cl. XMPlay is a 32-bit program. Added by the top-level
# CMakeLists.txt when XMPLAY_SDK_DIR is set. See README.md. xsflib builds it against the SDK's headers
# (xsflib_add_xmplay_plugin).

xsflib_add_xmplay_plugin(xmp-3sf
    SDK ${XMPLAY_SDK_DIR}
    SOURCES xmp_3sf.cpp
    LIBRARIES threesf_core
    OPTIONS ${THREESF_WARNINGS})
xsflib_add_version(xmp-3sf ${THREESF_VERSION_ARGS} DESCRIPTION "3SF plugin for XMPlay")
