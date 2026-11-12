// SPDX-License-Identifier: MIT

// The traits that the foobar2000 component and the XMPlay plugin share, for xsflib's plugin templates
// (xsflib/foobar2000/input.h and xsflib/xmplay/input.h): 3SF's player, names, extensions, limits, version and about
// text. Each plugin's traits derive from PluginTraits. The plugin's CMake target defines THREESF_VERSION.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/psf/psf.h"

#include "threesf/format.h"
#include "threesf/player.h"

namespace threesf
{

struct PluginTraits
{
    using Player = threesf::Player;
    using Set = threesf::LoadedSet;

    static constexpr const char* kName = "3SF";
    static constexpr std::array<const char*, 2> kExtensions = {"3sf", "mini3sf"};
    static constexpr uint8_t kPsfVersion = threesf::kPsfVersion3sf;
    static constexpr uint64_t kMaxFileSize = 256u << 20; // largest file read into memory

    // The tags that set how a set plays. A rip of an archive may hold them. foobar2000 keeps them when the user removes
    // the tags. A sequence that waits for its variables would otherwise go silent. XMPlay's Message info window leaves
    // them out.
    static constexpr std::array<const char*, threesf::kPlaybackTags.size()> kPlaybackTags = threesf::kPlaybackTags;

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

} // namespace threesf
