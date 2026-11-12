# 3sf

3SF is a PSF-family music rip format for Nintendo 3DS sequenced audio (CSAR/CSEQ): the 3DS
counterpart of the DS's 2SF and NCSF.

A 3SF rip includes the game's DSP firmware. The player runs it in
[Teakra](https://github.com/wwylele/teakra), which emulates the Teak DSP, so the firmware handles
mixing, filtering and limiting. The rip's mode determines what drives the DSP:

- **Game mode** (from a decrypted game with a driver profile): the rip carries the game's
  sound code, and the player runs it on an ARM11 interpreter with a high-level Horizon kernel.
  The rip plays as it does in the game.
- **Archive mode** (from a sound archive, `.bcsar`, or a game without a profile): 3SF's model
  of the SDK sound player (`nw::snd`/`nn::snd`, reverse engineered from Pokemon X) plays the archive. It
  accepts sound archives from any game.

The format is described in [`docs/3sf.md`](docs/3sf.md). Game mode needs a small driver profile
per game (`src/rip/rip.cpp`), and so far only Pokemon X (Japan) has one. Archive mode takes any
sound archive, with a DSP firmware file.

Some games keep groups of their sound data outside the sound archive, in group files (`.bcgrp`).
`3sfrip` finds each one by its group's name, in the game's RomFS or in the folder of a loose
`.bcsar` and the folders below it. The `.3sflib` holds the parts of the group files that the rips
play. Mighty Gunvolt Burst keeps its sound data that way. It also plays each piece of its music as
five sequences, one for each channel of the NES's sound chip. `3sfrip` rips each piece as one
`.mini3sf`. Like the game, the `.mini3sf` starts the five together. Rips with group files or with
such pieces require a player newer than 3SF 1.3.3. 3SF 1.3.3 refuses them. `docs/3sf.md`
describes both, under "Archive descriptor" and "Sounds that play together".

Besides the main mix, a 3DS sends sounds to two aux buses, A and B. A game puts effects on them
as it runs, such as a reverb. Its sound archive doesn't hold them. In archive mode, `3sfrip`
writes the effects of these games into their rips. The player runs them as the game's sound
library does:

- Pokémon X: a reverb on bus A. The game starts it in the field. Some of the field's sound
  effects send to it.
- WarioWare Gold (the first release and Rev 1): a reverb on each bus.
- The Legend of Zelda: Ocarina of Time 3D (North America, Europe and Japan): a reverb on bus A
  and a delay on bus B.
- The Legend of Zelda: Majora's Mask 3D (North America, Europe and Japan): a delay on bus B.
- Mario Party: Island Tour (North America): a delay on bus A.
- Mario Party: Star Rush (North America): a delay on bus B.
- StreetPass Mii Plaza and its add-on games: a reverb on bus A for the Plaza and five of the games.
  Warrior's Way puts a reverb on bus B. Mii Force, Slot Car Rivals, Market Crashers and Battleground
  Z put a reverb and a delay on bus B. Mii Trek puts no effect on either bus. The DSP's firmware
  runs the reverbs of Flower Town, Warrior's Way and those four games. The firmware also runs the
  four games' delays.
- Luigi's Mansion (2018, North America): a delay on bus A, which the DSP's firmware runs.
- Six of the 3DS's system titles, the European builds: both System Settings titles, Mii Maker,
  Nintendo eShop, the Activity Log and Swapnote. Each puts a reverb on bus A. Swapnote has it on
  while its music plays.
- Super Mario 3D Land: a delay on bus A.
- Animal Crossing: New Leaf (North America): a reverb on bus A in DJ K.K.'s club. The game sends
  his music to it. Its other sequences play with no effect. In a dream, the game puts a delay on
  bus A in the town and its shops. Their music is streamed. 3SF rips no streams. A rip plays each
  sequence as it sounds outside a dream.
- Animal Crossing: Happy Home Designer (North America): a reverb or a delay on bus A, by the room's
  ambient sound. "Ringing" gives the reverb. "Echoes" gives the delay. The DSP's firmware runs
  both. The game sends the room's music to them. A room starts with "silence". That sound puts no
  effect on the buses.

Ocarina of Time 3D and Majora's Mask 3D pick bus B's delay from 18 presets for each scene. A rip
plays every sound with the preset that normal play picks most often. Super Mario 3D Land picks
bus A's delay for each stage. A rip plays every sound with the delay that most stages take. The
stage of the title screen is one of them. Island Tour picks its effects for each scene too. A rip
plays every sound with the delay that 119 of its 120 scenes put on bus A. Many of Star Rush's
scenes pick bus B's delay from five presets, and some of them pick none. A rip plays every sound
with the preset that most of them pick. About 470 of its 4,115 sounds play in scenes that pick
another preset or none. 268 of them play only in such scenes. In one of its modes, WarioWare Gold
also puts a pitch shifter on bus A. The rips leave it out. `3sfrip` finds a game by its program ID.
WarioWare Gold has one ID in every region. Any other release of Ocarina of Time 3D, Majora's Mask
3D, Island Tour or Star Rush with another ID gets no effects until someone reads its code. `3sfrip`
finds the Plaza's sound archives by their SHA-1, from its update v14.6.0, its add-on content v0.2.0
and its European system title. It finds Luigi's Mansion's archive, the system titles' archives and
the archive of Super Mario 3D Land's USA release, Rev 1, the same way. It prints a note for another
build of one. Rips of another loose `.bcsar` or of any other game get none. Rips with effects
require a player newer than 3SF 1.3.3. 3SF 1.3.3 refuses them. The Plaza and four of its add-on
games link an earlier version of `nw::snd`'s reverb, with another damping filter. Rips with that
reverb or with the firmware's delay or reverb require a player newer than 3SF 1.4. 3SF 1.4 refuses
them. A rip of DJ K.K.'s music holds the club's reverb as an effect set, beside the effects of the
rest of the archive. Happy Home Designer's 364 pieces of music rip as they play in a room with
"silence". Each piece gets two more files, named with `_ringing` and `_echoes`. They play the piece
with the room's reverb or delay, held as effect sets. Players of 3SF 1.4 and older refuse these
rips. `docs/3sf.md` describes the effects
under "Effects on the aux buses" and "Effect sets".

Some games' code also changes how a sequence plays. It quiets some tracks as a piece starts, or
moves the piece to other groups of tracks as the game goes on. In StreetPass Mii Plaza, some sounds
wait for a variable that another sound sets. `3sfrip` covers these cases for Ocarina of Time 3D,
WarioWare Gold, Mario Party: Island Tour, Mario Party: Star Rush, Animal Crossing: New Leaf and Happy
Home Designer, Tomodachi Life, Super Mario 3D Land, Mario Kart 7, StreetPass Mii Plaza and its add-on
games, and three of the 3DS's system titles: System Settings, the Activity Log and Mii Maker. A rip plays each such sequence
as the game starts it. The game's later states get files of their own, named after the sequence and
the state, such as `SEQ_BGM_REELING_NORMAL_tension_2`. The Plaza's seven pieces that follow a new
Mii's jingle wait for the jingle's next beat. Their rips start them at once. The Plaza's European system title plays them
the same way without the update. Rips of its archive start them at once too. System Settings and the
Activity Log each play three pieces from one sequence. Each piece keeps a group of the sequence's
tracks, and its rip plays that group. Mii Maker's first screen plays tracks 0 to 7 of its music. Two
later states play other mixes of it, with some tracks at lower volumes.
Two of Ultimate Angler's catch sounds draw a random number as they start, and the game plays them
only on some draws. Their rips take a draw that plays the sound, and their comment tag says how
often the game does. Flower Town turns its watering music down step by step as the Mii walks. The
rips play it at full volume, as it starts. Ninja Launcher speeds its battle music up for the last 10
seconds of a battle. The Plaza plays its music faster and higher in its quick mode. Each of those
pieces gets one more file in that state, named with `_hurry` or `_quick`. Mii Force's game-over
music, SD_BGM_D01, loops until the game tells it to end. It gets two more files that play the
ending, named with `_ending` and `_ending_early`.
Super Mario 3D Land plays a melody as the player collects the notes of a trail, one beat for
each note. Its seven melodies, such as `SS_NOTE_OBJ_CHIKA`, rip as they sound when the player
collects every note in time: as written, at their tempo. With each note, the game also sets the
melody's tempo to between 0.4 and 2 times the written one. The rips keep the written tempo.
The game plays the melodies and the Toad House gift fanfares more quietly than the archive
says. Their rips have a `volume` tag with the game's level, such as 0.55.
Some of the game's effects change pitch with the game's state. The red coins' notes rise as
the player collects them, and the note blocks play in the key of the music. The rips play nine
of these effects at the pitch of their first play.
The game gives Luigi his voice by playing Mario's voice sequences with Luigi's banks. Each of
those sequences gets one more file with Luigi's voice, named with `_Luigi`.

Mario Kart 7 plays a beat sequence beside the streamed music of three courses: Music Park,
Airship Fortress and Waluigi Pinball. Each beat rips at the tempo of its course's stream. Music
Park's beat rips as the race starts, with its four instruments muted. Its other states, such as
`SEQ_SYNC_BEAT_MUSIC_PARK_track_0` and `_final_lap`, get files of their own. Of the game's
effects, 22 rip with a value that the game starts them with, such as `SE_KT_FALL_WATER` with the
value for the player's kart. Each driver's horn takes the kart's body. The rips play the horns of
the first body. The other 29 courses share one beat, `SEQ_SYNC_BEAT_120`. It rips at Toad Circuit's
tempo, and its other states, such as `_glide_lake` and `_glide_lake_final_lap`, take each course's
tempo. Each beat's drum loop sounds only while the player leads the race, and it grows louder as
the lead grows. The rips play the loop at full volume.

Animal Crossing: New Leaf's café job music starts with track 7 muted. Its rip plays the start.
`SEQ_BGM_IND_CAFEARBEIT_track_7` plays the state after the café unmutes the track. Each of DJ
K.K.'s songs waits at its start for a value that the game writes. The rips start each song and
interlude with the game's value. Two of the interludes, `SEQ_BGM_DJ_HS_MURAMELO` and
`SEQ_BGM_DJ_FC_MURAMELO`, play the town's tune. The game plays the tune that the player's save
holds. The rips play the first of the game's four default tunes instead, in the key of most of DJ
K.K.'s songs. New Leaf and Animal Crossing: Happy Home Designer play a town's
tune one note at a time. Each note starts one of 417 `SE_MELODY_` sounds again, such as a
villager's voice or a door's chime. The rips play each of these sounds once, at the tune's C, in
the key that the game gives the sound. The game stops each note after a length that the sound
gives. The rips hold the note for the length that the sequence gives it. `SE_TV_12_RUN` rips at
the count of the start of the TV show's run. Tomodachi Life's apartment music gets a file for
each of the Mii's other states, such as `BGM_MANSION_AND_ROOM_track_15`. A Mii's guitar
(`SE_MII_PLAY_GUITAR_S`, `_M` and `_L`) rips in the first chord of the apartment's music.
Rips that change a variable after the start, set a track's variable, set the random numbers,
change the tempo or pitch, give a sequence other banks, give it sends on the aux buses or set its
tracks' volumes require a player newer than 3SF 1.4. 3SF 1.4 refuses them.

## Tools

| Tool | Description |
|---|---|
| `3sfrip <input>...` | Rips each input into a `.3sflib` and one `.mini3sf` per sound, in a folder next to the input that's named after it with `_3sf` added (`-o DIR` puts them in `DIR`). A `.3sflib` that differs from one of the same name in the folder gets the next free name, such as `<name>_2.3sflib`. The rips already in the folder then still play. To replace earlier rips, rip into an empty folder. An input is a decrypted `.3ds`/`.cci`, `.cxi` or `.cia` (encrypted images are rejected), an extracted game directory (`exefs/code.bin`, `exefs/exheader.bin`, `romfs/`), or a `.bcsar`. Games with a driver profile rip in game mode, everything else in archive mode (`--mode` chooses). A `.bcsar` needs a DSP firmware file, any game's `dspaudio.cdc` or a console's `dspfirm.cdc`: give it along with the archive, put it in the archive's folder, or name it with `--firmware`. `--list` shows the sound archives, `--bgm` picks the music (the sounds whose labels mark them as BGM, jingles or fanfares, and the sequences with 3 tracks or more and 50 notes or more), `--only REGEX` picks sounds by their labels, and `--game`/`--artist`/`--year`/`--copyright`/`--by` set tags. Some sequences play nothing until the game sets one of their variables: in archive mode, `--var N=V` sets variable N (0 to 15 the sound's player variables, 16 to 31 the global ones) to V before each sequence starts, and writes it in the rip's `3sf_var` tag. Sounds that make no sound are listed and skipped: those at a volume of 0 in the archive, and the sequences that the length analysis finds never sound. |
| `3sfplay <file.mini3sf>...` | Renders each sound to a WAV file next to it, at the native 32728 Hz, with the tagged length, fade and volume. `-o FILE` names the output file for a single input, as WAV or FLAC by its extension. `--length` and `--fade` override the tags, `--default` gives the length of a file without one, and `--info` shows the tags. |
| `plugins/foobar2000` | foobar2000 input component: `foo_input_3sf.dll` for Windows, built with clang-cl, and `foo_input_3sf.component` for macOS. Both are built with the foobar2000 SDK; see its [README](plugins/foobar2000/README.md). |
| `plugins/xmplay` | XMPlay input plugin: `xmp-3sf.dll`, a 32-bit DLL for XMPlay on Windows, built with clang-cl and the XMPlay plugin SDK. See its [README](plugins/xmplay/README.md). |

Both tools accept multiple inputs and continue if one fails. A nonexistent input path stops the
command before it writes any files. For example,
`3sfrip game.3ds` writes `game_3sf/`, and `3sfplay game_3sf/*.mini3sf` renders every sound in it.
`--version` shows a tool's version.

### Downloads

The Releases page has `3sfrip` and `3sfplay` for 64-bit Windows, the foobar2000 component for
64-bit and 32-bit foobar2000 on Windows, and the XMPlay plugin. On macOS, build 3SF from source
(see "Building").

To install the XMPlay plugin, unzip `xmp-3sf-X.Y.Z.zip` and put `xmp-3sf.dll` in XMPlay's folder,
or in a folder inside it. Then restart XMPlay.

### Drag and drop

- **Windows:** drop files on `3sfrip.exe` or `3sfplay.exe`. A console window shows the results
  and stays open until you press Enter.
- **macOS:** the build also makes `3sfrip.app` and `3sfplay.app` in the build folder. Drop files
  on one, or open it and choose them. The app reports the results for each file and offers to open
  the output folders.

The results go next to the files, as above. Drop a sound archive together with a firmware file
(`.cdc`), unless there's one in the archive's folder already. File and folder names can use any
characters; on Windows that needs Windows 10 version 1903 or later.

## Building

You need Git, CMake 3.20 or later and a C++20 compiler. 3SF builds on two libraries: xsflib, for
players and rippers of the PSF family, and ntdlib, for the hardware and formats of Nintendo's
consoles. The repository holds them as submodules in `externals/xsflib` and `externals/ntdlib`:
clone with `git clone --recursive`, or run `git submodule update --init --recursive` in a clone.
The source code archives on the Releases page don't include the submodules. Clone the repository
instead. zlib and libFLAC are downloaded and built along with 3SF when they aren't installed.
Python 3 is only needed for the scripts in `driver/`.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload,
  which includes CMake, and its "C++ Clang tools for Windows" component, which adds clang-cl.
  Configure with `cmake -S . -B build -A x64 -T ClangCL` in place of the configure command
  below. Run the commands in a Developer PowerShell or Developer Command Prompt.
- **macOS on Apple Silicon:** the Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake`). `brew install flac pkg-config` saves building libFLAC.

Linux, Intel Macs, MSVC and 32-bit Windows aren't supported. The exceptions are the 32-bit
foobar2000 component and the XMPlay plugin. Build them with clang-cl, configured with
`-A Win32 -T ClangCL`.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps, such as
`cmake --preset release`, `cmake --build --preset release` and `ctest --preset release`. The
presets need CMake 3.23 or later. They don't select clang-cl. On Windows, use the commands
above.

CI (`.github/workflows/ci.yml`) builds 3SF on every push to `main` that changes more than
documentation: on Windows with clang-cl (x64, and x86 for 32-bit foobar2000 and XMPlay) and on
macOS with Apple Clang (Apple Silicon). The Windows jobs also run `ctest`.

`externals/ntdlib/externals/teakra` is [Teakra](https://github.com/wwylele/teakra) at upstream commit 3d697a1 with
changes, excluding upstream's hardware tests, Git LFS setup and CI. Compare against that
commit to see the changes. This version has a faster interpreter and a JIT that translates DSP
firmware into x86-64 code as it runs, and it can save and load its state. It bounds firmware
accesses to DSP memory, fixes delayed interrupts when the DSP is idle, and initializes the
interrupt controller and every register to a known state. Emulation failures throw exceptions
instead of aborting, and 32-bit x86 builds are supported. Samples match upstream Teakra's, except
that 3SF drops the zeros upstream inserts when the DSP's output runs dry for a slot.

ntdlib's DSP glue tests (`tests/ctr_teakra_dsp_test.cpp` there) use small firmware programs to check loading,
timeouts, pipe transfers and interrupts in the glue (`src/ctr/dsp/teakra_dsp.*` in ntdlib).

Emulating the DSP takes most of the time. On x86-64, where the system allows it, Teakra
translates the DSP firmware into machine code as it runs, which renders the same samples faster
than interpreting it. On Apple Silicon and in 32-bit builds Teakra interprets the firmware.
Set `TEAKRA_JIT=0` in the environment to disable the JIT. A test runs random programs through
both the JIT and interpreter and compares the results (ntdlib's `tests/ctr_teakra_jit_test.cpp`).

During playback, the foobar2000 component and the XMPlay plugin render ahead on a separate
thread as fast as the CPU allows. They buffer up to 10 minutes and 10 seconds of audio
(`externals/xsflib/src/player/render_ahead.cpp`). Seeking within the buffer is instant. Seeking further
ahead waits for rendering to reach the requested position. Longer tracks also use save
states. Every 2 seconds, the player takes a snapshot of the whole emulation
(`externals/xsflib/include/xsflib/player/playback.h`): the DSP's registers, components and memory, FCRAM, and either
the ARM11 with the kernel's objects, services and memory (game mode) or the model of the
sound player (archive mode). Unchanged 4 KiB memory pages are shared between snapshots.
Seeking back before the buffered audio restores the latest snapshot before the target and
renders from there. With rendering ahead turned off, every track uses snapshots.

The save-state test runs random programs on the DSP, saves part-way through, and checks that
restoring the state reproduces the same results (ntdlib's `tests/ctr_teakra_state_test.cpp`).

## Layout

```
docs/3sf.md          the format
externals/           the libraries xsflib and ntdlib (ntdlib holds Teakra and the DSP)
driver/              the guest driver (ARM code, prebuilt into src/rip/driver_blob.h)
src/arm/             ARM11 interpreter (ARMv6K, Thumb, VFPv2)
src/horizon/         Horizon kernel HLE, services, RomFS, the emulated system
src/threesf/         3SF format and player core, both modes (no platform audio or UI)
src/rip/             game loading (images, RomFS, BLZ), profiles, rip building, lengths, --bgm
src/cli/             3sfrip, 3sfplay
src/nnsnd/           the effects on nn::snd's aux buses (archive mode)
plugins/foobar2000/  foobar2000 component
plugins/xmplay/      XMPlay plugin
tests/               ctest suites
```

## Licence

3SF is under the MIT licence (`LICENSE`). xsflib, ntdlib and Teakra are MIT too.
`THIRD-PARTY-NOTICES.md` lists everything 3SF uses and what a binary distribution must include.

Game data is not covered by this licence. A `.3sflib` contains a game's code, sound archive and
DSP firmware. Rips are for personal use.
