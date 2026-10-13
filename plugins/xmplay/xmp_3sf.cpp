// SPDX-License-Identifier: MIT

// xmp-3sf: XMPlay input plugin for 3SF (.3sf and .mini3sf), from xsflib's plugin template (xsflib/xmplay/input.h). The
// template's comment says what the plugin does. This file gives 3SF's traits: its player, names, extensions and limits.
//
// The player core (src/threesf) runs the game's DSP firmware. In game mode, the game's sound code drives it on an
// emulated ARM11. In archive mode, 3SF's model of the SDK sound player drives it.
//
// Build: see plugins/xmplay/README.md (clang-cl for 32-bit Windows, with the XMPlay plugin SDK).

#include <windows.h>
#include <xmpin.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"
#include "xsflib/xmplay/input.h"

#include "threesf/format.h"
#include "threesf/player.h"

namespace
{

struct Traits
{
    using Player = threesf::Player;
    using Set = threesf::LoadedSet;

    static constexpr const char* kName = "3SF";
    static constexpr std::array<const char*, 2> kExtensions = {"3sf", "mini3sf"};
    static constexpr uint8_t kPsfVersion = threesf::kPsfVersion3sf;
    static constexpr uint64_t kMaxFileSize = 256u << 20; // largest file read into memory

    // The sequence's variables, which a rip of an archive may set.
    static constexpr std::array<const char*, 1> kPlaybackTags = {"3sf_var"};

    static constexpr const char* kVersion = THREESF_VERSION;
    static constexpr const char* kAbout =
        "Plays 3SF (Nintendo 3DS sequenced music) rips by emulating the game's DSP firmware, driven by the game's "
        "sound code or by 3SF's model of the SDK sound player.\n\n"
        "MIT licence. Uses xsflib (MIT), ntdlib (MIT), Teakra (MIT, Copyright (c) 2018 Weiyi Wang) and zlib.";

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return threesf::LoadSet(path, reader, out, separators);
    }
};

} // namespace

// XMPlay looks the plugin up by this name (the SDK's xmpin.def exports it).
XMPIN* WINAPI XMPIN_GetInterface(DWORD face, InterfaceProc faceproc)
{
    return xsflib::xmplay::Plugin<Traits>::GetInterface(face, faceproc);
}
