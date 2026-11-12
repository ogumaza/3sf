// SPDX-License-Identifier: MIT

// The games' effects on the aux buses (see effects.h).

#include "rip/effects.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ntdlib/ctr_fx/effects.h"
#include "ntdlib/sar/formats.h"

#include "nnsnd/aux_effects.h"
#include "rip/game.h"

namespace threesf::rip
{
namespace
{

using nnsnd::AuxEffect;
using ntdlib::ctr_fx::DelayParam;
using ntdlib::ctr_fx::ReverbParam;

// The key of a game's entry: its program ID. Keys of another kind, such as the CRC-32 of each build's code, would
// change this function and the keys alone.
uint64_t KeyOf(const GameFiles& game)
{
    return game.ProgramId();
}

struct Entry
{
    std::vector<uint64_t> keys; // the releases whose code was read
    GameEffects effects;
};

// A reverb of kind `kind`: kNwSndReverb, kNnSndReverb, kNnSndReverb2011, kNwSndReverb2011 or kFirmwareReverb. The
// firmware's reverb has no surround.
AuxEffect ReverbEffect(AuxEffect::Kind kind, const ReverbParam& param)
{
    AuxEffect effect;
    effect.kind = kind;
    effect.reverb = param;

    return effect;
}

// A delay of kind `kind`: kDelay, or kFirmwareDelay for the delay that nn::snd's DSP delay sets up in the DSP firmware.
AuxEffect DelayEffect(AuxEffect::Kind kind, const DelayParam& param)
{
    AuxEffect effect;
    effect.kind = kind;
    effect.delay = param;

    return effect;
}

// nn::snd's FxDelay with one of the 18 presets of Ocarina of Time 3D and Majora's Mask 3D: a time and a feedback, with
// a damping of 0.2 and all four channels.
AuxEffect DelayPreset(uint32_t time, float feedback)
{
    return DelayEffect(AuxEffect::Kind::kDelay,
                       {.delay_time = time, .feedback = feedback, .damping = 0.2f, .enable_surround = true});
}

const std::vector<Entry>& Entries()
{
    static const std::vector<Entry> kEntries = {
        // Pokémon X: the reverb that the field module starts on aux bus A, with the parameters that game mode's driver
        // passes (the game's profile in rip.cpp). The game runs it while the player is in the field. A rip runs it for
        // every sound.
        {{0x0004000000055d00},
         {"Pokémon X", {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb, kPokemonXFieldReverb)}, {}}}}},

        // WarioWare Gold: a reverb on each bus, for the whole game. Every region's release has this program ID. The
        // game sets the reverbs up at 0x105a74, through 0x7082c0 and 0x3cb18c (0x7084f0 and 0x3cb44c in the Rev 1
        // build). One of its modes also puts a pitch shifter on bus A. The rips leave it out.
        {{0x00040000001d1c00},
         {"WarioWare Gold",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb, {.early_reflection_time = 60,
                                                          .fused_time = 800,
                                                          .pre_delay_time = 100,
                                                          .coloration = 0.5f,
                                                          .damping = 0.4f,
                                                          .filter_sizes = {3040, 3680, 2080},
                                                          .early_gain = 0.6f,
                                                          .fused_gain = 0.4f})},
            {ReverbEffect(AuxEffect::Kind::kNwSndReverb, {.early_reflection_time = 40,
                                                          .fused_time = 4000,
                                                          .pre_delay_time = 150,
                                                          .coloration = 0.25f,
                                                          .damping = 0.4f,
                                                          .filter_sizes = {2720, 3680, 2080},
                                                          .early_gain = 0.6f,
                                                          .fused_gain = 0.4f})}}}}},

        // The Legend of Zelda: Ocarina of Time 3D, the North American, European and Japanese releases. 0x465b34 puts
        // nn::snd's FxReverb on bus A for good. The reverb has nn::snd's defaults. A scene picks bus B's delay from 18
        // presets (0x32c5dc), with the byte at +1 of its scene command 0x15 (0x273108). The rips take preset 5 for
        // every sound. More scenes pick it in normal play than any other preset. Two of the game's set-ups pick preset
        // 10 (0x450bc8, 0x458eec), and only cutscenes pick it after them. The game multiplies the time by 4. The
        // European build and the first Japanese build hold the same code. The European build's set-up lies at 0x465b54,
        // and the two set-ups' calls lie at 0x450be8 and 0x458f0c. In the Japanese build, they lie at 0x465b0c,
        // 0x450ba0 and 0x458ec4.
        {{0x0004000000033500, 0x0004000000033600, 0x0004000000033400},
         {"The Legend of Zelda: Ocarina of Time 3D",
          {{{ReverbEffect(AuxEffect::Kind::kNnSndReverb2011, {})}, {DelayPreset(96, 0.2475f)}}}}},

        // The Legend of Zelda: Majora's Mask 3D, the North American release, both European releases and the first
        // Japanese release. The North American release and the European Rev 1 run the same code. The Japanese build
        // holds the same code at other addresses. The first European release shares its program ID with Rev 1 and runs
        // the Japanese build's code. No effect on bus A. A scene picks bus B's delay from the same 18 presets
        // (0x179b9c, 0x179cf8 in the Japanese build), with the byte at +1 of its scene command 0x15 (0x48ae98). The
        // rips take preset 1 for every sound. More scenes pick it in normal play than any other preset. The game picks
        // preset 7 at boot (0x15d198, 0x15d2e0), and only cutscenes pick it after that.
        {{0x0004000000125500, 0x0004000000125600, 0x00040000000d6e00},
         {"The Legend of Zelda: Majora's Mask 3D", {{{}, {DelayPreset(24, 0.01875f)}}}}},

        // Mario Party: Island Tour, the North American release. Each scene's record picks the effects (0x2c7c7c). Bus A
        // gets nw::snd's FxDelay or FxReverb, with the parameters of a file of the game's RomFS (0x2c81b8). 119 of the
        // 120 records pick the delay. Bus B gets the DSP's delay in 3 records and nothing in the others. The rips take
        // the delay on bus A and nothing on bus B.
        {{0x00040000000f8100},
         {"Mario Party: Island Tour",
          {{{DelayEffect(AuxEffect::Kind::kDelay,
                         {.delay_time = 165, .feedback = 0.45f, .damping = 0.45f, .enable_surround = true})},
            {}}}}},

        // Mario Party: Star Rush, the North American release. The game reads its effects from a workbook of presets in
        // a pack of its RomFS (0x2a75e0). Every preset there is nw::snd's FxDelay. The sound set-up asks for
        // EFFECT_A_COMMON on bus A (0x1bed18), but in an Azahar run that call put nothing there. A scene of another
        // workbook in the same pack can put a preset on bus B (0x130ef0, 0x130f14). 15 of the 38 scenes that name a
        // preset pick EFFECT_B_01. The groups of those scenes hold 1,054 sounds, and the other presets' scenes hold 268
        // sounds alone. The rips take EFFECT_B_01 for every sound.
        {{0x000400000019bd00},
         {"Mario Party: Star Rush",
          {{{}, {DelayEffect(AuxEffect::Kind::kDelay, {.delay_time = 70, .feedback = 0.2f, .damping = 0.8f})}}}}},
    };

    return kEntries;
}

struct ArchiveEntry
{
    std::string_view sha1;
    ArchiveEffects effects;
};

// The SHA-1 of Ultimate Angler's sound archive.
constexpr std::string_view kUltimateAnglerSha1 = "a98dd3c22da8e4bed8327e4fc98196e102ce53ea";

// nw::snd's FxReverb of two add-on games of StreetPass Mii Plaza, Feed Mii (0x6a9ec0) and Ninja Launcher (0x8516e4).
// Feed Mii's data holds a file of effects with a pre-delay of 5 ms, but no code reads that file.
constexpr ReverbParam kLauncherReverb = {.early_reflection_time = 60,
                                         .fused_time = 2500,
                                         .pre_delay_time = 50,
                                         .coloration = 0.5f,
                                         .damping = 0.6f,
                                         .early_gain = 0.5f,
                                         .fused_gain = 0.3f};

// The DSP reverb of Slot Car Rivals, Market Crashers and Battleground Z.
constexpr ReverbParam kDspReverb = {.early_reflection_time = 25,
                                    .fused_time = 2000,
                                    .pre_delay_time = 5,
                                    .coloration = 0.6f,
                                    .damping = 0.4f,
                                    .early_gain = 0.4f,
                                    .fused_gain = 0.4f};

// The archives whose effects 3sfrip finds by their SHA-1: those of StreetPass Mii Plaza and its add-on games, Super
// Mario 3D Land, Animal Crossing: New Leaf and Happy Home Designer (see their entries).
//
// The Plaza's update v14.6.0 holds the code of the Plaza and of all its add-on games. The add-on content, version 0.2.0
// (USA), holds the add-ons' sound archives. The European add-on content holds the same files. Each game puts its
// effects on as it starts. The sound teardown at each game's exit takes them off (0x81d49c, through 0x120374). A bus
// without an effect stays off. Every reverb has the filter sizes 3040, 3680 and 2080.
//
// The Plaza links an earlier version of nw::snd's FxReverb: kind 6. Its damping filter takes the damping times the sum
// of the input and the last output from the input (0x33bf80 in v14.6.0). The system title's class matches word for
// word, apart from branch targets and two addresses. With the later filter, the Plaza's reverb would be too loud in its
// lows.
//
// Flower Town and five other games use nn::snd's DSP reverb. Four of them use its DSP delay too. The DSP's firmware
// runs both effects. The rips give them kinds 7 and 5. The five read their effects from a text file of the game's data
// (0x2c22c4, parsed at 0x2c1994): Warrior's Way (0x51f92c), Slot Car Rivals (0x5ea000), Mii Force (0xae9950), Market
// Crashers (0xb27d04) and Battleground Z (0xbdfe98). Each file gives a DspReverb with a bus. Each file but Warrior's
// Way's also gives a DspDelay with a bus. The parser compares the address of the bus's text with that of the literal
// "AUX_BUS_A" (0x2c19f0 and 0x2c20b0), not the text. The two never match. Every effect of the files therefore goes on
// bus B. The bus that a file names makes no difference. The parser passes on the delay's surround flag and the reverb's
// high-pass damping. The files set the surround flag and leave the high-pass damping off.
const std::vector<ArchiveEntry>& ArchiveEntries()
{
    // Happy Home Designer's two effects for a room, by the effects manager's numbers for them: the reverb that the
    // ambient sound "ringing" starts and the delay that "echoes" starts. The values are those that the manager's
    // constructor stores (0x10ec14). The reverb's filters are 3040, 3680 and 2080 samples (0x750dbc).
    static const std::vector<SequenceEffect> kRoomEffects = {
        {0,
         {{{ReverbEffect(AuxEffect::Kind::kFirmwareReverb, {.early_reflection_time = 50,
                                                            .fused_time = 8000,
                                                            .pre_delay_time = 60,
                                                            .coloration = 0.85f,
                                                            .damping = 0.91f,
                                                            .early_gain = 0.75f,
                                                            .fused_gain = 1.0f})},
           {}}}},
        {1,
         {{{DelayEffect(AuxEffect::Kind::kFirmwareDelay,
                        {.delay_time = 285, .feedback = 0.68f, .damping = 0.15f, .enable_surround = true})},
           {}}}},
    };

    static const std::vector<ArchiveEntry> kEntries = {
        // The Plaza: nw::snd's FxReverb on bus A, set up again each time the Plaza changes its set of music (0x2c1800,
        // through 0x2c1390).
        {"33b514ee80f6a6df3034bc8774235e40dfa52d1c",
         {"StreetPass Mii Plaza",
          "the Plaza's update v14.6.0",
          "BGM_PEACE_MAIN",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb2011, {.damping = 0.55f})}, {}}}}},

        // The Plaza's European system title plays without the update. Its sound system's set-up appends the same
        // FxReverb on bus A once (0x10c964 to 0x10c9bc). Only the teardown takes it off (0x107038). The archive holds
        // no sound that the update's archive lacks. Another build thus gets the update's entry first.
        {"c2550ce04d4c8c44e92c2d72682038616fbec658",
         {"StreetPass Mii Plaza",
          "the Plaza's European system title",
          "BGM_PEACE_MAIN",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb2011, {.damping = 0.55f})}, {}}}}},

        // Ultimate Angler: nw::snd's FxReverb on bus A with nw::snd's defaults, for the whole game. Its sound set-up
        // plays the event EVENT_REVERB_SETTING_1 of the archive's user parameters (0xd5084c, read at 0xd40e50).
        {kUltimateAnglerSha1,
         {"StreetPass Mii Plaza: Ultimate Angler",
          "add-on content v0.2.0",
          "SEQ_BGM_OP_INTRO",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb2011, {})}, {}}}}},

        // Flower Town: the DSP reverb on bus A, with nn::snd's defaults (0x71633c, through 0x2eec74 and 0x81d18c).
        {"331b572b09819830436fe8a09673b22fc6f7bdae",
         {"StreetPass Mii Plaza: Flower Town",
          "add-on content v0.2.0",
          "BGM_FLOWER_TOWN",
          {{{ReverbEffect(AuxEffect::Kind::kFirmwareReverb, {})}, {}}}}},

        // Monster Manor: nw::snd's FxReverb on bus A, with a fused time of 2000 ms (0x890f80).
        {"132e22e118ed9cb162fa2117d636d433920328a4",
         {"StreetPass Mii Plaza: Monster Manor",
          "add-on content v0.2.0",
          "BGM01_TITLE",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb2011, {.fused_time = 2000})}, {}}}}},

        {"c6a2a213c9780f9cbcd886d507ba72beb858b4a1",
         {"StreetPass Mii Plaza: Feed Mii",
          "add-on content v0.2.0",
          "SEQ_BGM_COOK_KOTEI",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb2011, kLauncherReverb)}, {}}}}},
        {"ddb93158672258c7d8ea2b5edb8662bda34234e6",
         {"StreetPass Mii Plaza: Ninja Launcher",
          "add-on content v0.2.0",
          "SEQ_BGM_KIROKU",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb2011, kLauncherReverb)}, {}}}}},

        // Warrior's Way: the DSP reverb on bus B.
        {"7967baa712b4badc354a88e550847e1713195c74",
         {"StreetPass Mii Plaza: Warrior's Way",
          "add-on content v0.2.0",
          "MGBTL_BGM_TITLE",
          {{{},
            {ReverbEffect(AuxEffect::Kind::kFirmwareReverb, {.early_reflection_time = 5,
                                                             .fused_time = 1300,
                                                             .pre_delay_time = 5,
                                                             .coloration = 0.6f,
                                                             .damping = 0.5f,
                                                             .early_gain = 0.0f,
                                                             .fused_gain = 0.3f})}}}}},

        // Mii Force: the DSP reverb and the DSP delay, both on bus B. Its file gives the reverb bus A.
        {"e566c28135170a936f1f6e4538a15d745f3b2a9e",
         {"StreetPass Mii Plaza: Mii Force",
          "add-on content v0.2.0",
          "SD_ST1BOSSOUT01",
          {{{},
            {ReverbEffect(AuxEffect::Kind::kFirmwareReverb, {.early_reflection_time = 25,
                                                             .fused_time = 1400,
                                                             .pre_delay_time = 5,
                                                             .coloration = 0.6f,
                                                             .damping = 0.4f,
                                                             .early_gain = 0.3f,
                                                             .fused_gain = 0.3f}),
             DelayEffect(AuxEffect::Kind::kFirmwareDelay,
                         {.delay_time = 170, .feedback = 0.4f, .damping = 0.75f, .enable_surround = true})}}}}},

        // Slot Car Rivals, Market Crashers and Battleground Z: the DSP reverb and the DSP delay, both on bus B. Their
        // files give the reverb bus A.
        {"1a7cdf18ce5dace373d4eac78733083987c9a964",
         {"StreetPass Mii Plaza: Slot Car Rivals",
          "add-on content v0.2.0",
          "SD_CAR_SELECT01",
          {{{},
            {ReverbEffect(AuxEffect::Kind::kFirmwareReverb, kDspReverb),
             DelayEffect(AuxEffect::Kind::kFirmwareDelay,
                         {.delay_time = 170, .feedback = 0.4f, .damping = 0.5f, .enable_surround = true})}}}}},
        {"e640022555aa9fce1c4516a7398f960b56614477",
         {"StreetPass Mii Plaza: Market Crashers",
          "add-on content v0.2.0",
          "SD_TRADE_ENDBGM01",
          {{{},
            {ReverbEffect(AuxEffect::Kind::kFirmwareReverb, kDspReverb),
             DelayEffect(AuxEffect::Kind::kFirmwareDelay,
                         {.delay_time = 170, .feedback = 0.4f, .damping = 0.75f, .enable_surround = true})}}}}},
        {"1a3bd13bd513803e50475dfd3b2ca427d1b68bc0",
         {"StreetPass Mii Plaza: Battleground Z",
          "add-on content v0.2.0",
          "SD_RUN_BOSS01",
          {{{},
            {ReverbEffect(AuxEffect::Kind::kFirmwareReverb, kDspReverb),
             DelayEffect(AuxEffect::Kind::kFirmwareDelay,
                         {.delay_time = 170, .feedback = 0.4f, .damping = 0.75f, .enable_surround = true})}}}}},

        // Mii Trek: no effect. Its sound set-up loads the archive and puts nothing on the buses (0x4b6410).
        {"d14849826e1df1dd543938dc9313afdbb9d82147",
         {"StreetPass Mii Plaza: Mii Trek", "add-on content v0.2.0", "STRM_SD_SE_EV_ED01_UFO_APPEAR", {}}},

        // Luigi's Mansion (2018), the USA release: the firmware's delay on bus A for the whole game. The sound system's
        // set-up (0x382448) calls 0x3a0930. That function posts the delay with nw::snd's command 0x37, whose case
        // (0x2eaa00) sets it up through nn::snd's DSP delay. It also gives bus A an aux callback that does nothing
        // (0x37c5a4). Bus B gets no effect.
        {"03cdddad0cb59b6f90dd7cc6727873701f4300b9",
         {"Luigi's Mansion",
          "the USA release",
          "JA_SE_LUI_GET_HEART",
          {{{DelayEffect(AuxEffect::Kind::kFirmwareDelay,
                         {.delay_time = 270, .feedback = 0.4f, .damping = 0.9f, .enable_surround = false})},
            {}}}}},

        // Six of the 3DS's system titles, the European builds: an FxReverb on bus A as the title starts. System
        // Settings (0x10a938), a second System Settings title (0x1ffc2c), Mii Maker (0x10a5c4) and the Activity Log
        // (0x1a24d4) take nw::snd's defaults. Their nw::snd computes the reverb as nn::snd's FxReverb of 2011 does.
        {"ff5231c919ad81331321e6e29e24d84bbf2127cc",
         {"System Settings",
          "the European build",
          "BGM_CTR_MSET",
          {{{ReverbEffect(AuxEffect::Kind::kNnSndReverb2011, {})}, {}}}}},
        {"fd187b38953c162695779f26c66140859a63e74b",
         {"System Settings",
          "the European build of the second title",
          "BGM_CTR_MSET",
          {{{ReverbEffect(AuxEffect::Kind::kNnSndReverb2011, {})}, {}}}}},
        {"0cc99a6732a7dbc0d6e8887449233ab25396672c",
         {"Mii Maker",
          "the European build",
          "MEDIT_BGM_FACE",
          {{{ReverbEffect(AuxEffect::Kind::kNnSndReverb2011, {})}, {}}}}},
        {"ea55099f2b92479898b4eda7956d645021dac2d3",
         {"Activity Log",
          "the European build",
          "BGM_CTR_PLOG_MAIN",
          {{{ReverbEffect(AuxEffect::Kind::kNnSndReverb2011, {})}, {}}}}},

        // Nintendo eShop: nw::snd's FxReverb with a fused time of 2500 ms (0x1a3808).
        {"32528650ee18385fdab7701dce1aae01bbd71e79",
         {"Nintendo eShop",
          "the European build",
          "SEQ_TIGER_BANNER",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb, {.fused_time = 2500})}, {}}}}},

        // Swapnote: nw::snd's FxReverb with its defaults (0x11862c). The title turns the reverb off at its start
        // (0x10e5d4) and on as each piece of music starts (0x20b9ac). It turns it off while it changes the music
        // (0x29401c, 0x2940dc) and in three scenes (0x20eb58).
        {"cad032a7a698c87af8e73aa78e6865afe77a67fb",
         {"Swapnote",
          "the European build",
          "SEQ_S_PHOTO_WAIT_END1",
          {{{ReverbEffect(AuxEffect::Kind::kNwSndReverb, {})}, {}}}}},

        // Super Mario 3D Land: nn::snd's FxDelay on bus A, in every stage. Each stage's sound set-up (0x1b6e6c) calls
        // 0x1deef4 with the stage's name. That function looks the stage up in the game's table of audio effects, a file
        // of its RomFS, and puts the delay on bus A at 0x1df3c0. The table names 19 stages. Most of them take its
        // DelayTunnel: 180 ms, with a damping of 0.6. Every other stage takes the row "Default". The title screen's
        // stage is one of them. The rips take that row for every sound. Bus B gets a reverb only for a stage with a
        // ReverbName, and no stage has one.
        {"bfb2742fc5e8f2869c9ce781710ba393f675fbc1",
         {"Super Mario 3D Land",
          "the USA release's Rev 1",
          "STM_BGM_MUTEKISTAR",
          {{{DelayEffect(AuxEffect::Kind::kDelay,
                         {.delay_time = 200, .feedback = 0.4f, .damping = 0.5f, .enable_surround = false})},
            {}}}}},

        // Animal Crossing: New Leaf (USA): no effect for the archive. The effects manager (0x1b29d4) builds ten
        // effects. The scene change (0x1b3070) starts one for each scene, and most scenes start none. Effect 0 is
        // nn::snd's FxReverb, with the parameters that the manager's constructor stores (0x1b2ae4 reads them). The club
        // CLUB444_DJKK (scene 0x46) starts it. DJ K.K.'s 188 sequences play only there, with a send of 0.4 on bus A
        // (ntdlib::ctr::snd::StartingSequenceTracks).
        {"54f0fbc801c85ea2035219923aa4cd955e6f4cc7",
         {"Animal Crossing: New Leaf",
          "the USA release",
          "SEQ_BGM_KAPPEI_GO_RANDOMMATCHING",
          {},
          {{0,
            {{{ReverbEffect(AuxEffect::Kind::kNnSndReverb, {.early_reflection_time = 10,
                                                            .fused_time = 1200,
                                                            .pre_delay_time = 40,
                                                            .coloration = 0.1f,
                                                            .damping = 0.6f,
                                                            .early_gain = 0.5f,
                                                            .fused_gain = 0.4f})},
              {}}}}}}},

        // Happy Home Designer (USA): no effect for the archive. A room's ambient sound picks the effects manager's
        // effect (ntdlib::ctr::snd::SequenceArrangements). The manager's constructor (0x10ec14) and set-up (0x15c8d0)
        // build effect 0 as nn::snd's DSP reverb and effect 1 as its DSP delay. Starting either queues nn::snd's
        // command 56 or 55 (0x445338). The command's dispatch (0x3265bc) runs the DSP reverb (0x32be60) or the DSP
        // delay (0x32bd90) on bus A. Their records for the mixer are those of FirmwareReverb and FirmwareDelay
        // (format_test). The code read is revision 1's. The other release's archive holds the same music and the same
        // ambient loops, at other IDs.
        {"8c07814daf28befb015cc97dd2c66699254035be",
         {"Animal Crossing: Happy Home Designer",
          "the USA release's revision 1",
          "SEQ_BGM_MUSS01_SANBIKA",
          {},
          kRoomEffects}},
        {"30c98d66881296eda5e676964918e7003b403c75",
         {"Animal Crossing: Happy Home Designer", "another USA release", "SEQ_BGM_MUSS01_SANBIKA", {}, kRoomEffects}},
    };

    return kEntries;
}

// The effect's name in 3sfrip's output.
const char* NameOf(const AuxEffect& effect)
{
    switch (effect.kind)
    {
    case AuxEffect::Kind::kNwSndReverb:
        return "nw::snd's FxReverb";
    case AuxEffect::Kind::kNnSndReverb:
        return "nn::snd's FxReverb";
    case AuxEffect::Kind::kNnSndReverb2011:
        return "nn::snd's FxReverb of 2011";
    case AuxEffect::Kind::kDelay:
        return "FxDelay";
    case AuxEffect::Kind::kFirmwareDelay:
        return "the firmware's delay";
    case AuxEffect::Kind::kNwSndReverb2011:
        return "nw::snd's FxReverb of 2011";
    case AuxEffect::Kind::kFirmwareReverb:
        return "the firmware's reverb";
    }

    return "an unknown effect";
}

} // namespace

const GameEffects* FindGameEffects(const GameFiles& game)
{
    const uint64_t key = KeyOf(game);
    for (const Entry& entry : Entries())
    {
        if (std::ranges::find(entry.keys, key) != entry.keys.end())
        {
            return &entry.effects;
        }
    }

    return nullptr;
}

const ArchiveEffects* FindArchiveEffects(std::string_view archive_sha1)
{
    for (const ArchiveEntry& entry : ArchiveEntries())
    {
        if (entry.sha1 == archive_sha1)
        {
            return &entry.effects;
        }
    }

    return nullptr;
}

uint32_t EffectSetOf(const ArchiveEffects* effects, uint8_t game_effect)
{
    if (!effects)
    {
        return 0;
    }

    for (std::size_t i = 0; i < effects->game_effects.size(); i++)
    {
        if (effects->game_effects[i].number == game_effect)
        {
            return static_cast<uint32_t>(i + 1);
        }
    }

    return 0;
}

const ArchiveEffects* FindOtherBuild(const ntdlib::sar::SoundArchive& archive, std::string_view archive_sha1)
{
    if (FindArchiveEffects(archive_sha1))
    {
        return nullptr;
    }

    for (const ArchiveEntry& entry : ArchiveEntries())
    {
        for (const auto& sound : archive.Sounds())
        {
            if (sound.name == entry.effects.label)
            {
                return &entry.effects;
            }
        }
    }

    return nullptr;
}

std::string DescribeEffects(const nnsnd::AuxEffectLists& effects)
{
    std::string text;
    for (std::size_t bus = 0; bus < effects.size(); bus++)
    {
        text += bus == 0 ? "bus A " : ", bus B ";
        if (effects[bus].empty())
        {
            text += "none";
        }

        for (std::size_t i = 0; i < effects[bus].size(); i++)
        {
            text += (i > 0 ? " then " : "") + std::string(NameOf(effects[bus][i]));
        }
    }

    return text;
}

const char* SoundComment(std::string_view archive_sha1, std::string_view sound)
{
    // Ultimate Angler's two effects that draw a random number as they start (ntdlib's starting tracks give their
    // seeds).
    struct Comment
    {
        std::string_view archive_sha1;
        std::string_view sound;
        const char* text;
    };

    static constexpr std::array<Comment, 2> kComments = {{
        {kUltimateAnglerSha1, "SE_CATCHED_092",
         "The game plays this sound on about one draw of its random numbers in five, a quieter one on another one in "
         "five, and nothing on the rest. The rip takes a draw that plays this sound."},
        {kUltimateAnglerSha1, "SE_CATCHED_120",
         "The game plays this sound on about one draw of its random numbers in three, a quieter one on one in six, "
         "and nothing on the rest. The rip takes a draw that plays this sound."},
    }};
    for (const Comment& comment : kComments)
    {
        if (comment.archive_sha1 == archive_sha1 && comment.sound == sound)
        {
            return comment.text;
        }
    }

    return nullptr;
}

} // namespace threesf::rip
