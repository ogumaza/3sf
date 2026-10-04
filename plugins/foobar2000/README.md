# foo_input_3sf

foobar2000 input component for 3SF rips (`.3sf`, `.mini3sf`). It's built on the same player
core as `3sfplay`, which runs the game's DSP firmware in Teakra, driven either by the game's
sound code on an emulated ARM11 (game mode) or by 3SF's model of the SDK sound player (archive
mode).

**Status:** it builds with clang-cl (or MSVC) for 32-bit and 64-bit foobar2000 on Windows, and
with Apple Clang for foobar2000 on macOS, as one bundle for Apple Silicon and Intel. CI builds it
on every push to `main` that changes more than documentation: in the Windows jobs (clang-cl for
x64 and x86, and MSVC for x64), and for Apple Silicon only in the macOS ARM64 job. The bundle with
both Mac architectures comes from the manual workflow (see "Building"). Releases include a
package for 64-bit and 32-bit foobar2000 on Windows (`.github/workflows/release.yml`). `ctest`
covers the player core. Both clang-cl and MSVC builds play in foobar2000 on Windows; see
"Performance" for details.

## Features

- Tags are shown as metadata, as in kode54's PSF-family components: `game` as album, `year` as
  date, and `replaygain_*` as ReplayGain. Tags can be edited: the tag area is rewritten, and
  `_lib`, `length`, `fade` and `volume` are kept.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play
  for the default length and fade and stop early when the sound ends by itself.
- Advanced preferences › Decoding › 3SF decoder: default length and fade, and endless playback.
  Endless playback applies to playback only, never to conversion or scanning.
- Seeking within the part of a track already played is quick. The component saves the emulation
  state every 2 seconds during playback. Seeking restores the nearest earlier state and renders
  from there. Seeking further ahead requires rendering up to the target. At most 128 states are
  kept, with longer intervals between them as the track continues. Conversion and scanning do
  not save states.
- Output is 16-bit stereo at 32728 Hz, the DSP's native rate, and isn't resampled.

## Building

**On GitHub (no Windows machine or Mac needed):** the `foobar2000 component` workflow
(`.github/workflows/foobar2000.yml`) builds the 32-bit and 64-bit DLLs and the Mac bundle, and
uploads one `.fb2k-component` with all three. Start it from the repository's Actions tab (Run
workflow). GitHub lists a manual workflow only once it's on the default branch (`main`). It needs
GitHub Actions enabled for the repository. The workflow downloads the SDK version specified in
`.github/actions/foobar2000-sdk/action.yml` from foobar2000.org. CI uses the same version.

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang
tools for Windows" component, plus CMake 3.20 or later. Download and unpack the foobar2000 SDK
from <https://www.foobar2000.org/SDK>. Use the folder containing `pfc` and `foobar2000` as the
SDK path below. CI builds with the version that `.github/actions/foobar2000-sdk/action.yml` names,
and a newer one may need changes to `plugins/foobar2000/CMakeLists.txt`.

```
cmake -S . -B build-fb2k -A x64 -T ClangCL -DFOOBAR2000_SDK_DIR=C:/path/to/SDK
cmake --build build-fb2k --config Release --target foo_input_3sf
```

This produces `build-fb2k/plugins/foobar2000/Release/foo_input_3sf.dll`. For a
`.fb2k-component`, zip the DLL, and for 64-bit foobar2000 v2 put it in an `x64` folder inside
the zip. For 32-bit foobar2000, configure with `-A Win32 -T ClangCL` and put that DLL at the top
level of the zip. With the 64-bit DLL in `pkg\x64`, Windows' own `tar` makes the zip:
`tar -a -c -f foo_input_3sf.zip -C pkg x64` (add `foo_input_3sf.dll` after `x64` for a 32-bit DLL
in `pkg` itself), then rename it to `foo_input_3sf.fb2k-component`. Windows PowerShell's
`Compress-Archive` writes the folder's name with a backslash, which the zip format doesn't allow,
and foobar2000 didn't recognize a package made that way. Omitting `-T ClangCL` selects MSVC,
which produces the same audio but runs more slowly. MSVC builds made with Visual Studio 2022
versions before 17.7 are slower still.

**On macOS:** you need the Xcode command line tools (`xcode-select --install`), CMake 3.20 or
later, and the foobar2000 SDK unpacked somewhere (macOS's `tar -xf SDK-<date>.7z` unpacks it).

```
cmake -S . -B build-fb2k -DCMAKE_BUILD_TYPE=Release -DFOOBAR2000_SDK_DIR=/path/to/SDK \
  "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-fb2k --target foo_input_3sf
```

This produces `build-fb2k/plugins/foobar2000/foo_input_3sf.component`, a bundle for Apple
Silicon and Intel Macs that runs on macOS 11 or later, the same minimum as the SDK's
projects. Without the two `CMAKE_OSX_` options, the bundle is for the Mac that built it only. For
a `.fb2k-component`, put the bundle in a `mac` folder inside the zip. The bundle is signed ad hoc,
not with a Developer ID, and it isn't notarized.

The build compiles the SDK's `pfc`, `foobar2000/SDK` and
`foobar2000/foobar2000_component_client` sources directly. On Windows it links
`foobar2000/shared/shared-<arch>.lib` (or `shared.lib`), and on macOS it compiles the parts of
`foobar2000/shared` and the Objective-C++ files that the SDK's Xcode projects build. If the SDK
layout has changed, adjust
`plugins/foobar2000/CMakeLists.txt`. Another option is to copy the SDK's `foo_sample` project,
replace its sources with `foo_input_3sf.cpp`, and add the 3SF core (`src/threesf`,
`src/horizon`, `src/arm`, `src/dsp`, `src/nwsnd`, `src/nnsnd`, `src/csar`, Teakra in
`externals/teakra`, and zlib) as a static library, with `src` on the include path for the
headers in `src/common`, and `THREESF_VERSION` defined as the version in quotes, such as
`"0.9.0"` (the one in `CMakeLists.txt`'s `project()`).

On Windows everything is built with the static C runtime (`/MT`), so the component needs no
runtime DLLs. On macOS it uses only the system's libraries.

## Performance

Playback runs on one CPU core. Most of the time is spent emulating the DSP in Teakra; game mode
also emulates the ARM11. On x86-64, where the system allows it, Teakra uses a JIT to translate the
DSP firmware into native code. The 32-bit and Apple Silicon components use the interpreter.
MSVC builds run the interpreter more slowly than clang-cl builds, and a 32-bit MSVC build can
stutter. Use 64-bit foobar2000 and the 64-bit component if your other components support them.

## Licence

The component is MIT-licensed, like the rest of 3SF, and uses Teakra (MIT) and zlib. Ship
`LICENSE` and `THIRD-PARTY-NOTICES.md` (or their text) with a build. Nothing in it is GPL, so it
can be distributed as a foobar2000 component.
