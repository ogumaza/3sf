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
- WarioWare Gold (North America, the first release and Rev 1): a reverb on each bus.
- The Legend of Zelda: Ocarina of Time 3D (North America): a reverb on bus A and a delay on bus B.
- The Legend of Zelda: Majora's Mask 3D (North America): a delay on bus B.

Ocarina of Time 3D and Majora's Mask 3D pick bus B's delay from 18 presets for each scene. A rip
plays every sound with the preset that the game picks when it starts. In one of its modes,
WarioWare Gold also puts a pitch shifter on bus A. The rips leave it out. `3sfrip` finds a game
by its program ID. Other regions' releases of WarioWare Gold, Ocarina of Time 3D and Majora's
Mask 3D have other IDs. Their rips get no effects until someone reads their code. Rips of a loose
`.bcsar` or of any other game get none. Some games use effects that the DSP runs. Archive mode
doesn't play those. Rips with effects require a player newer than 3SF 1.3.3. 3SF 1.3.3 refuses
them. `docs/3sf.md` describes the effects under "Effects on the aux buses".

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

`externals/teakra` is [Teakra](https://github.com/wwylele/teakra) at upstream commit 3d697a1 with
3SF's changes, excluding upstream's hardware tests, Git LFS setup and CI. Compare against that
commit to see the changes. This version has a faster interpreter and a JIT that translates DSP
firmware into x86-64 code as it runs, and it can save and load its state. It bounds firmware
accesses to DSP memory, fixes delayed interrupts when the DSP is idle, and initializes the
interrupt controller and every register to a known state. Emulation failures throw exceptions
instead of aborting, and 32-bit x86 builds are supported. Samples match upstream Teakra's, except
that 3SF drops the zeros upstream inserts when the DSP's output runs dry for a slot.

The DSP glue tests (`tests/teakra_dsp_test.cpp`) use small firmware programs to check loading,
timeouts, pipe transfers and interrupts in `src/dsp/teakra_dsp.*`.

Emulating the DSP takes most of the time. On x86-64, where the system allows it, Teakra
translates the DSP firmware into machine code as it runs, which renders the same samples faster
than interpreting it. On Apple Silicon and in 32-bit builds Teakra interprets the firmware.
Set `TEAKRA_JIT=0` in the environment to disable the JIT. A test runs random programs through
both the JIT and interpreter and compares the results (`tests/teakra_jit_test.cpp`).

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
restoring the state reproduces the same results (`tests/teakra_state_test.cpp`).

## Layout

```
docs/3sf.md          the format
externals/           the libraries xsflib and ntdlib, and Teakra
driver/              the guest driver (ARM code, prebuilt into src/rip/driver_blob.h)
src/arm/             ARM11 interpreter (ARMv6K, Thumb, VFPv2)
src/horizon/         Horizon kernel HLE, services, RomFS, the emulated system
src/dsp/             the DSP: Teakra running the game's firmware, and FCRAM
src/threesf/         3SF format and player core, both modes (no platform audio or UI)
src/rip/             game loading (images, RomFS, BLZ), profiles, rip building, lengths, --bgm
src/cli/             3sfrip, 3sfplay
src/nnsnd, nwsnd/    the nw::snd/nn::snd model (archive mode)
src/common/          small helpers shared by the rest (quiet stdout, how 3SF reads sound archives)
plugins/foobar2000/  foobar2000 component
plugins/xmplay/      XMPlay plugin
tests/               ctest suites
```

## Licence

3SF is under the MIT licence (`LICENSE`). xsflib, ntdlib and Teakra are MIT too.
`THIRD-PARTY-NOTICES.md` lists everything 3SF uses and what a binary distribution must include.

Game data is not covered by this licence. A `.3sflib` contains a game's code, sound archive and
DSP firmware. Rips are for personal use.
