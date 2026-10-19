# foo_input_3sf

foobar2000 input component for 3SF rips (`.3sf`, `.mini3sf`). It's built on the same player
core as `3sfplay`, which runs the game's DSP firmware in Teakra, driven either by the game's
sound code on an emulated ARM11 (game mode) or by 3SF's model of the SDK sound player (archive
mode).

**Status:** the component builds with clang-cl for 32-bit and 64-bit foobar2000 on Windows, and
with Apple Clang for foobar2000 on Apple Silicon Macs. CI builds it on every push to `main` that
changes more than documentation: in the Windows x64 and x86 jobs with clang-cl, and in the macOS
ARM64 job. Releases include a package for 64-bit and 32-bit foobar2000 on Windows
(`.github/workflows/release.yml`). `ctest` covers the player core. See "Performance" for how fast
each build plays.

## Features

- Tags are shown as metadata, as in kode54's PSF-family components: `game` as album, `year` as
  date, and `replaygain_*` as ReplayGain. Tags can be edited: the tag area is rewritten, and
  `_lib`, `length`, `fade`, `volume`, `utf8` and `3sf_var` are kept.
- Length and fade come from the `length` and `fade` tags. Files without a length tag play
  for the default length and fade and stop early when the sound ends by itself.
- Advanced preferences › Decoding › 3SF decoder: default length and fade, endless playback, and
  rendering ahead. Endless playback and rendering ahead apply to playback only, never to
  conversion or scanning.
- During playback, the component renders ahead on a separate thread, as fast as the CPU allows,
  and buffers up to 10 minutes and 10 seconds of audio (about 76 MiB), enough for the longest
  track `3sfrip`'s length analysis produces. Seeking within the buffer is instant. Seeking further
  ahead waits for rendering to reach the requested position. Rendering ahead keeps one CPU core
  busy until it reaches the end of the track or the buffer limit. Longer tracks also save the
  emulation state every 2 seconds. Seeking back before the buffered audio restores the latest
  state before the target and renders from there. At most 128 states are kept, with longer
  intervals between them as the track continues. With rendering ahead turned off, every track
  saves states, and seeking past what has played renders up to the target.
- Output is 16-bit stereo at 32728 Hz, the DSP's native rate, and isn't resampled.

## Building

**On GitHub (no Windows machine or Mac needed):** the `foobar2000 component` workflow
(`.github/workflows/foobar2000.yml`) builds the 32-bit and 64-bit DLLs and the Mac bundle, and
uploads one `.fb2k-component` with all three. Start it from the repository's Actions tab (Run
workflow). GitHub lists a manual workflow only once it's on the default branch (`main`). The
workflow requires GitHub Actions to be enabled for the repository. The workflow downloads the SDK
from foobar2000.org with `externals/xsflib/ci/foobar2000-sdk`. That action pins the SDK's version.
CI builds with the same version.

**On Windows:** you need Visual Studio 2022 or later with the C++ workload and the "C++ Clang
tools for Windows" component, plus CMake 3.20 or later. Download and unpack the foobar2000 SDK
from <https://www.foobar2000.org/SDK>. Use the folder containing `pfc` and `foobar2000` as the
SDK path below. CI builds with the version that `externals/xsflib/ci/foobar2000-sdk` pins. A
newer SDK may require changes to xsflib's `cmake/XsflibApps.cmake`.

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
`Compress-Archive` writes the folder's name with a backslash. The zip format doesn't allow that.
foobar2000 didn't recognize a package made that way. Omitting `-T ClangCL` selects MSVC. MSVC
builds aren't supported.

**On macOS:** you need the Xcode command line tools (`xcode-select --install`), CMake 3.20 or
later, and the foobar2000 SDK unpacked somewhere (macOS's `tar -xf SDK-<date>.7z` unpacks it).

```
cmake -S . -B build-fb2k -DCMAKE_BUILD_TYPE=Release -DFOOBAR2000_SDK_DIR=/path/to/SDK \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-fb2k --target foo_input_3sf
```

This produces `build-fb2k/plugins/foobar2000/foo_input_3sf.component`, a bundle for Apple
Silicon Macs that runs on macOS 11 or later. That's the same minimum as the SDK's projects.
Without the two `CMAKE_OSX_` options, the bundle is for the Mac that built it only. For
a `.fb2k-component`, put the bundle in a `mac` folder inside the zip. The bundle is signed ad hoc,
not with a Developer ID, and it isn't notarized.

The build compiles the SDK's `pfc`, `foobar2000/SDK` and
`foobar2000/foobar2000_component_client` sources directly. On Windows it links
`foobar2000/shared/shared-<arch>.lib` (or `shared.lib`), and on macOS it compiles the parts of
`foobar2000/shared` and the Objective-C++ files that the SDK's Xcode projects build.

On Windows everything is built with the static C runtime (`/MT`), so the component needs no
runtime DLLs. On macOS it uses only the system's libraries.

## Performance

Playback runs on one CPU core. Most of the time is spent emulating the DSP in Teakra; game mode
also emulates the ARM11. On x86-64, where the system allows it, Teakra uses a JIT to translate the
DSP firmware into native code. The 32-bit and Apple Silicon components use the interpreter.
MSVC builds run the interpreter more slowly than clang-cl builds, and a 32-bit MSVC build can
stutter. Use 64-bit foobar2000 and the 64-bit component if your other components support them.

## Licence

The component is MIT-licensed, like the rest of 3SF, and uses xsflib (MIT), ntdlib (MIT), Teakra (MIT) and zlib. Ship
`LICENSE` and `THIRD-PARTY-NOTICES.md` (or their text) with a build. Nothing in it is GPL, so it
can be distributed as a foobar2000 component.
