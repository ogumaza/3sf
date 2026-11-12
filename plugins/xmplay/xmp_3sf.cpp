// SPDX-License-Identifier: MIT

// xmp-3sf: XMPlay input plugin for 3SF (.3sf and .mini3sf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. plugin_traits.h gives 3SF's traits.
//
// The player core (src/threesf) runs the game's DSP firmware. In game mode, the game's sound code drives it on an
// emulated ARM11. In archive mode, 3SF's model of the SDK sound player drives it.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include "xsflib/xmplay/input.h"

#include "../plugin_traits.h"

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<threesf::PluginTraits>::GetInterface(face, faceproc);
}
