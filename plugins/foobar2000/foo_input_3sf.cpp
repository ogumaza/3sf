// SPDX-License-Identifier: MIT

// foo_input_3sf: foobar2000 input component for 3SF (.3sf and .mini3sf), from xsflib's input template
// (xsflib/foobar2000/input.h). The template's comment says what the component does. This file gives 3SF's traits: its
// player, names, extensions, GUIDs and limits.
//
// The player core (src/threesf) runs the game's DSP firmware. In game mode, the game's sound code drives it on an
// emulated ARM11. In archive mode, 3SF's model of the SDK sound player drives it.
//
// Build: see plugins/foobar2000/README.md (clang-cl or MSVC on Windows, or Apple Clang on macOS, with the foobar2000
// SDK).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xsflib/foobar2000/input.h"
#include "xsflib/psf/psf.h"
#include <SDK/foobar2000.h>

#include "threesf/format.h"
#include "threesf/player.h"

namespace
{

struct Traits
{
    using Player = threesf::Player;
    using Set = threesf::LoadedSet;

    static constexpr const char* kName = "3SF";
    static constexpr const char* kDecoderName = "3SF decoder";
    static constexpr std::array<const char*, 2> kExtensions = {"3sf", "mini3sf"};
    static constexpr uint8_t kPsfVersion = threesf::kPsfVersion3sf;
    static constexpr t_filesize kMaxFileSize = 256u << 20; // largest file read into memory

    // A sequence that waits for its variables would go silent if removing the tags dropped 3sf_var.
    static constexpr const char* kVariablesTag = "3sf_var";

    // {A335A9E1-B807-48DD-A589-4D6511BCCE2B}
    static constexpr GUID kInputGuid = {0xa335a9e1, 0xb807, 0x48dd, {0xa5, 0x89, 0x4d, 0x65, 0x11, 0xbc, 0xce, 0x2b}};

    // Advanced preferences entries, under Decoding > 3SF decoder.
    static constexpr GUID kBranchGuid = {0x83236141, 0xed54, 0x4ab0, {0xab, 0xe8, 0x2d, 0xe1, 0x59, 0x1a, 0x7e, 0x6d}};
    static constexpr GUID kLengthGuid = {0x39864782, 0x5638, 0x436d, {0x9f, 0x8b, 0x5a, 0xe6, 0x63, 0x38, 0xd9, 0xcd}};
    static constexpr GUID kFadeGuid = {0x832017b3, 0x784b, 0x40d0, {0x8b, 0x01, 0x58, 0xd2, 0x29, 0xd3, 0xcb, 0x5c}};
    static constexpr GUID kEndlessGuid = {0x13458d58, 0x4bcb, 0x48da, {0xb5, 0x59, 0xca, 0x5b, 0xff, 0xb9, 0xbe, 0x63}};
    static constexpr GUID kRenderAheadGuid = {
        0x4451eb09, 0x317c, 0x48e0, {0x86, 0x08, 0x00, 0x6d, 0x10, 0xb3, 0x8d, 0x52}};

    static std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, Set& out,
                                              std::string_view separators)
    {
        return threesf::LoadSet(path, reader, out, separators);
    }
};

xsflib::foobar2000::Settings<Traits> g_settings;
input_singletrack_factory_t<xsflib::foobar2000::Input<Traits>> g_input_3sf_factory;

} // namespace

DECLARE_FILE_TYPE("3SF files", "*.3SF;*.MINI3SF");

DECLARE_COMPONENT_VERSION("3SF Decoder", THREESF_VERSION,
                          "Plays 3SF (Nintendo 3DS sequenced music) rips by emulating the game's DSP firmware, "
                          "driven by the game's sound code or by 3SF's model of the SDK sound player.\n\n"
                          "MIT licence. Uses xsflib (MIT), ntdlib (MIT), Teakra (MIT, Copyright (c) 2018 Weiyi Wang) "
                          "and zlib.");

VALIDATE_COMPONENT_FILENAME("foo_input_3sf.dll");
