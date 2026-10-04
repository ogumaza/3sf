# Third-party notices

3SF is under the MIT licence (`LICENSE`). Its dependencies have the following licences.

| Component | Licence | Use in 3SF |
|---|---|---|
| [Teakra](https://github.com/wwylele/teakra), Copyright (c) 2018 Weiyi Wang | MIT (`externals/teakra/LICENSE`) | In `externals/teakra`, compiled into every 3SF program: upstream commit 3d697a1 with 3SF's changes, which are 3SF's work and offered under Teakra's MIT licence. |
| [Catch2](https://github.com/catchorg/Catch2) v2.13.8, Copyright (c) 2022 Two Blue Cubes Ltd | BSL-1.0 | Teakra's unit tests use it (`externals/teakra/externals/catch`). 3SF doesn't build these tests or include Catch2 in its programs. |
| [zlib](https://zlib.net), Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler | zlib | The PSF container. The system's copy is used, or v1.3.1 is downloaded when configuring. |
| [libFLAC](https://xiph.org/flac/), Copyright the Xiph.Org Foundation | BSD-3-Clause | FLAC output in `3sfplay`. The system's copy is used, or 1.5.0 is downloaded when configuring. |
| [foobar2000 SDK](https://www.foobar2000.org/SDK), Copyright Peter Pawlowski | BSD-style (the SDK's `sdk-license.txt`) | Only for `plugins/foobar2000`. Not included here; it's supplied when building the plugin. |

Binary distributions must carry the MIT notices of 3SF and Teakra, and the libFLAC notice when
libFLAC is linked in. Shipping `LICENSE` and this file does that: the licence texts are at the
end. zlib's licence, and those of the foobar2000 SDK and its pfc library, ask for no notice in
binaries. Catch2's licence asks for its text in copies of the source code, which this file
carries too.

## Reference material (no code taken)

- [Azahar](https://github.com/azahar-emu/azahar) (GPL-2.0-or-later) was the reference for how
  the 3DS kernel, its services and the DSP interface behave. An early version used a port of
  its HLE audio mixer and DSP glue, and both have been removed. The DSP glue 3SF has now
  (`src/dsp/teakra_dsp.*`) was written in a clean room: its author worked from a description
  of the behaviour it had to keep, 3dbrew and Teakra's source, and never saw Azahar's code or
  the earlier glue. Two sets of hardware facts come from Azahar: the SVC cycle costs in
  `src/horizon/kernel.cpp`, which the format spec (`docs/3sf.md`) lists too, and the
  configuration memory values of firmware 11.17.
- [3dbrew](https://www.3dbrew.org) documents the file formats, the DSP firmware format, the
  services and the kernel.

## Game data

Nothing in this repository licenses game data. Rips (`.3sflib`) contain a game's code, its
sound archive and its DSP firmware, which belong to their copyright holders.

`src/nwsnd/tables.h` holds the numeric tables nw::snd uses: powers of two, decibel levels, pan
curves, sines and cosines, envelope attack multipliers and filter cutoffs. The sound model uses
the game's exact values to reproduce its output.

## Licence texts

### Teakra

```
MIT License

Copyright (c) 2018 Weiyi Wang

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### libFLAC

```
Copyright (C) 2000-2009  Josh Coalson
Copyright (C) 2011-2025  Xiph.Org Foundation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

- Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

- Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

- Neither the name of the Xiph.Org Foundation nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### zlib

```
Copyright notice:

 (C) 1995-2022 Jean-loup Gailly and Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Jean-loup Gailly        Mark Adler
  jloup@gzip.org          madler@alumni.caltech.edu
```

### Catch2

```
Boost Software License - Version 1.0 - August 17th, 2003

Permission is hereby granted, free of charge, to any person or organization
obtaining a copy of the software and accompanying documentation covered by
this license (the "Software") to use, reproduce, display, distribute,
execute, and transmit the Software, and to prepare derivative works of the
Software, and to permit third-parties to whom the Software is furnished to
do so, all subject to the following:

The copyright notices in the Software and this entire statement, including
the above license grant, this restriction and the following disclaimer,
must be included in all copies of the Software, in whole or in part, and
all derivative works of the Software, unless such copies or derivative
works are solely in the form of machine-executable object code generated by
a source language processor.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```
