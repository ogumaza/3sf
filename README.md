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
  works for any archive.

The format is described in [`docs/3sf.md`](docs/3sf.md). Game mode needs a small driver profile
per game (`src/rip/rip.cpp`), and so far only Pokemon X (Japan) has one. Archive mode takes any
sound archive, with a DSP firmware file.

## Tools

| Tool | Description |
|---|---|
| `3sfrip <input>...` | Rips each input into a `.3sflib` and one `.mini3sf` per sound, in a folder next to the input that's named after it with `_3sf` added (`-o DIR` puts them in `DIR`). An input is a decrypted `.3ds`/`.cci`, `.cxi` or `.cia` (encrypted images are rejected), an extracted game directory (`exefs/code.bin`, `exefs/exheader.bin`, `romfs/`), or a `.bcsar`. Games with a driver profile rip in game mode, everything else in archive mode (`--mode` chooses). A `.bcsar` needs a DSP firmware file, any game's `dspaudio.cdc` or a console's `dspfirm.cdc`: give it along with the archive, put it in the archive's folder, or name it with `--firmware`. `--list` shows the sound archives, `--bgm` or `--only REGEX` picks sounds, and `--game`/`--artist`/`--year`/`--copyright`/`--by` set tags. |
| `3sfplay <file.mini3sf>...` | Renders each sound to a WAV file next to it, at the native 32728 Hz, with the tagged length, fade and volume. `-o FILE` names the output file for a single input, as WAV or FLAC by its extension. `--length` and `--fade` override the tags, `--default` gives the length of a file without one, and `--info` shows the tags. |
| `plugins/foobar2000` | foobar2000 input component: `foo_input_3sf.dll` for Windows, built with clang-cl, and `foo_input_3sf.component` for macOS. Both are built with the foobar2000 SDK; see its [README](plugins/foobar2000/README.md). |

Both tools accept multiple inputs and continue if one fails. A nonexistent input path stops the
command before it writes any files. For example,
`3sfrip game.3ds` writes `game_3sf/`, and `3sfplay game_3sf/*.mini3sf` renders every sound in it.
`--version` shows a tool's version.

### Downloads

The Releases page has `3sfrip` and `3sfplay` for 64-bit and 32-bit Windows, and the foobar2000
component for 64-bit and 32-bit foobar2000 on Windows. On other systems, build 3SF from source
(see "Building").

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

You need Git, CMake 3.20 or later and a C++20 compiler. zlib and libFLAC are downloaded and
built along with 3SF when they aren't installed. Python 3 is only needed for the scripts in
`tools/` and `driver/`.

- **Windows:** Visual Studio 2022 or later with the "Desktop development with C++" workload,
  which includes CMake, and its "C++ Clang tools for Windows" component, which adds clang-cl.
  Configure with `cmake -S . -B build -T ClangCL` in place of the configure command below.
  clang-cl builds play faster than MSVC builds, with the same output.
  Without `-T ClangCL`, MSVC builds it, and versions of Visual Studio 2022 before 17.7 build a
  slower Teakra that way. Run the commands in a Developer PowerShell or Developer Command Prompt.
- **macOS:** the Xcode command line tools (`xcode-select --install`) and CMake
  (`brew install cmake`). `brew install flac pkg-config` saves building libFLAC, except in a
  universal build (two architectures in `CMAKE_OSX_ARCHITECTURES`), which builds its own.
- **Linux:** GCC or Clang, CMake and Git from your distribution. The zlib and libFLAC
  development packages and pkg-config (on Debian and Ubuntu, `zlib1g-dev libflac-dev
  pkg-config`) save building those two.

MinGW, GCC and GNU-style Clang aren't supported on Windows. GCC and Clang also aren't supported
for 32-bit x86: their floating-point arithmetic produces different samples from the other
builds. CMake rejects these configurations. Use clang-cl or MSVC for 32-bit foobar2000.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

`CMakePresets.json` has `release` and `debug` presets for the same steps, such as
`cmake --preset release`, `cmake --build --preset release` and `ctest --preset release`. The
presets need CMake 3.21 or later, and on Windows they build with MSVC.

CI (`.github/workflows/ci.yml`) builds 3SF and runs `ctest` on every push to `main` that changes
more than documentation: on Windows with clang-cl (x64, and x86 for 32-bit foobar2000) and
MSVC (x64), on macOS with Apple Clang (Apple Silicon and Intel), and on Linux with GCC and Clang
(x64).

`externals/teakra` is [Teakra](https://github.com/wwylele/teakra) at upstream commit 3d697a1 with
3SF's changes, excluding upstream's hardware tests, Git LFS setup and CI. Compare against that
commit to see the changes. This version has a faster interpreter and a JIT that translates DSP
firmware into x86-64 code as it runs. It bounds firmware accesses to DSP memory, fixes delayed
interrupts when the DSP is idle, and initializes the interrupt controller and every register to
a known state. Emulation failures throw exceptions instead of aborting, and 32-bit x86 builds
are supported. Samples match upstream Teakra's, except that 3SF drops the zeros upstream inserts
when the DSP's output runs dry for a slot.

The test of the DSP glue (`src/dsp/teakra_dsp.*`) runs small firmwares built into the test, and
checks how the glue loads firmware, waits for the DSP, moves bytes through the firmware's pipes
and passes on its interrupts (`tests/teakra_dsp_test.cpp`).

Emulating the DSP takes most of the time. On x86-64, where the system allows it, Teakra
translates the DSP firmware into machine code as it runs, which renders the same samples faster
than interpreting it. On Apple Silicon and in 32-bit builds Teakra interprets the firmware.
Set `TEAKRA_JIT=0` in the environment to disable the JIT. A test runs random programs through
both the JIT and interpreter and compares the results (`tests/teakra_jit_test.cpp`).

## Layout

```
docs/3sf.md          the format
driver/              the guest driver (ARM code, prebuilt into src/rip/driver_blob.h)
src/arm/             ARM11 interpreter (ARMv6K, Thumb, VFPv2)
src/horizon/         Horizon kernel HLE, services, RomFS, the emulated system
src/dsp/             the DSP: Teakra running the game's firmware, and FCRAM
src/threesf/         3SF format and player core, both modes (no platform audio or UI)
src/rip/             game loading (images, RomFS, BLZ), profiles, rip building
src/cli/             3sfrip, 3sfplay
src/csar, nnsnd, nwsnd/   CSAR parsing and the nw::snd/nn::snd model (archive mode)
src/common/          small helpers shared by the rest (byte order, ASCII, quiet stdout)
plugins/foobar2000/  foobar2000 component
platform/            the Windows manifest and the macOS droplet apps of 3sfrip and 3sfplay
tests/               ctest suites
```

## Licence

3SF is under the MIT licence (`LICENSE`). Teakra is MIT too; `THIRD-PARTY-NOTICES.md` lists
everything 3SF uses and what a binary distribution must include.

Game data isn't covered: a `.3sflib` contains a game's code, sound archive and DSP firmware,
and rips are for personal use.
