# 3SF: 3DS Sound Format

Version 1. 3SF stores sequenced music from Nintendo 3DS games so that it can be played back
as the game plays it. Every 3SF rip carries the game's DSP audio firmware, and every player runs
it on an emulated Teak DSP. A rip is in one of two modes:

- **Game mode**, like 2SF for the DS: the rip carries the game's sound code and the player
  runs it. This is exact, and needs a driver profile for the game (see "Ripping").
- **Archive mode**, like NCSF: the rip carries the game's sound archive, and the player plays it
  with its own model of the SDK's sound player (NintendoWare `nw::snd` and `nn::snd`). This works
  for any sound archive. See "Archive mode".

In game mode, a 3SF player emulates only the parts of a 3DS that a game's sound code uses:

- the ARM11 application core (ARMv6K with VFPv2), user mode only;
- a high-level model of the Horizon kernel and of the system services the sound code talks to
  (`srv:`, `fs:USER`, `dsp::DSP`, `cfg:u`, `APT:U` and others as stubs);
- the Teak DSP, at the instruction level, running the game's DSP firmware
  (`dspaudio.cdc`).

3SF runs the game's DSP firmware in an emulated DSP ("LLE") rather than reimplementing the
firmware's mixer ("HLE"): the firmware is exact by construction, and a reimplementation that was
tried differed from it in eight measurable ways.

A rip boots the game's startup code (`crt0`, `nninitStartUp`, static initializers). The
game's `nnMain` is patched to jump to a small **driver**, stored in the rip, that initializes
the game's sound library, loads one sound's data and plays it, and calls the game's per-frame
sound update forever. The player produces the DSP's audio output.

## Files

| Extension | Contents |
|---|---|
| `.3sf` | A self-contained rip (rarely used). |
| `.3sflib` | Shared data for one game. Game mode: code segments, driver, RomFS files. Archive mode: the sound archive and the DSP firmware. |
| `.mini3sf` | One sound: a small chunk that selects the sound, plus tags; references a `.3sflib` with `_lib`. |

All three use the PSF container.

## PSF container

```
0x00  3 bytes  "PSF"
0x03  u8       version: 0x26 for 3SF (see "Version byte" below)
0x04  u32      R: size of the reserved area
0x08  u32      N: size of the compressed program
0x0C  u32      CRC-32 of the compressed program
0x10  R bytes  reserved area (uncompressed)
      N bytes  program, zlib-compressed
      "[TAG]" followed by tags (optional)
```

All integers are little-endian. The CRC-32 is the one used by zlib.

### Version byte

3SF uses **0x26**. Known PSF version bytes are 0x01 (PSF1), 0x02 (PSF2), 0x11 (SSF), 0x12
(DSF), 0x13 (Sega Genesis), 0x21 (USF), 0x22 (GSF), 0x23 (SNSF), 0x24 (2SF), 0x25 (NCSF) and
0x41 (QSF). The high nibble 2 marks Nintendo systems, and 0x26 is the next free value after the
DS's two formats.

Zophar's Domain has a "Nintendo 3DS (3SF)" music section, but it only holds MP3 recordings, and
there wasn't a 3SF format before this one. None of the known PSF-family formats uses 0x26
either. The value is defined once, in `src/threesf/format.h`.

## Program: chunks

The decompressed program is a sequence of chunks. Each chunk is an 8-byte header followed by
its payload, padded with zeros to a multiple of 4 bytes:

```
u32  type (four ASCII characters, stored as bytes in order)
u32  size of the payload in bytes (excluding padding)
...  payload
```

| Type | Payload | Meaning |
|---|---|---|
| `MEM ` | `u32 address`, then data | Load the data into the emulated address space at `address`. Pages that a `MEM` chunk touches are mapped read/write and zero-filled first. |
| `FILE` | `u16 path length`, the UTF-8 path (no leading `/`, `/` separators), zeros to a multiple of 4, then the file data | A file in the game's RomFS, as seen through `rom:/` (for example `sound/xy_sound.bcsar`). |
| `SND ` | `u32 sound id` | Archive mode: the sound to play, as a `nw::snd` item id (`0x01000000 \| index` in the archive's sound table). The last one in loading order takes effect. |

Unknown chunk types are skipped. Chunks are applied in order. For the same address or path, later
data replaces earlier data.

### Loading order

As in other PSF formats, `_lib` is loaded recursively before the file that references it. Its
path is relative to that file. After the referencing file, `_lib2`, `_lib3` and so on are loaded
in numeric order, stopping at the first missing number. An empty `_lib` or `_libN` names no
file. Chunks are applied in this loading order. A game-mode `.mini3sf` usually contains one
`MEM` chunk that patches the driver's parameter block; an archive-mode file contains one `SND `
chunk.

## Reserved area: descriptors

The reserved area may contain a process descriptor (game mode) or an archive descriptor
(archive mode). At least one loaded file must have a descriptor. The last descriptor in loading
order takes effect and determines the mode.

### Process descriptor (game mode)

```
0x00  u32  magic "3SFP"
0x04  u32  descriptor version (1)
0x08  u32  entry point of the main thread (the game's crt0; for example 0x00100000)
0x0C  u32  main thread stack size in bytes (from the exheader)
0x10  u32  main thread priority (from the exheader)
0x14  u32  application memory size in bytes (config memory APPMEMALLOC, commit limit)
0x18  u32  address of the driver's status word (0 if none)
0x1C  u32  flags: bit 0 = the DSP is required; version 1 has no other flags
```

The main thread's stack ends at 0x10000000, as on the 3DS. A version 1 player rejects process
descriptors with a different version, flags other than 1 (only bit 0 set), or an application
memory size of 0 or greater than 0x0B200000 (178 MiB, the maximum available to a 3DS application).

### Archive descriptor (archive mode)

```
0x00  u32  magic "3SFA"
0x04  u32  descriptor version (1)
0x08  u32  flags: bit 0 = the DSP is required; version 1 has no other flags
0x0C  u32  output mode: 1 stereo (version 1 plays stereo only)
0x10  u16  length of the sound archive's path, then the path (UTF-8)
      u16  length of the DSP firmware's path, then the path (UTF-8)
      zeros to a multiple of 4
```

The paths identify `FILE` chunks containing the sound archive (`.bcsar`) and DSP firmware (a
DSP1 image). A version 1 player rejects archive descriptors with a different version, flags
other than 1, or an output mode other than 1. Output modes 0 (mono) and 2 (surround) are reserved
for a later version. Reproducing the console's surround output requires configuration data that
rips don't contain.

## The emulated system (game mode)

A version 1 player provides:

- **Memory.** `MEM` chunks, the main thread stack, TLS at 0x1FF82000, the configuration memory
  page at 0x1FF80000 and the shared page at 0x1FF81000. DSP RAM (512 KiB) is at 0x1FF00000.
  `svcControlMemory` provides heap at 0x08000000 and linear (device) memory at 0x14000000.
  Linear memory maps to FCRAM at physical 0x20000000, where the DSP reads it. FCRAM is twice the
  process descriptor's application memory size: 128 MiB for a rip with 64 MiB, as `3sfrip` makes
  them.
- **Time.** An ARM11 clock of 268,111,856 Hz, one cycle per ARM instruction, plus a fixed cost
  per SVC (see "SVC costs" below). The DSP clock is half the ARM11 clock and runs in lock-step
  with it. A player may run them in turns, the ARM11 for up to 5,000 cycles and then the DSP to
  the same point, as 3SF's player does, so that an interrupt from the DSP takes effect at the end
  of the ARM11's turn. Shorter turns give the same output, but longer ones delay the DSP's
  interrupts and change it.
  When a `dsp::DSP` request has to wait for the DSP, for a reply that hasn't come yet or for a
  full command register to empty, the DSP runs on during the request, and the ARM11's clock then
  moves on by twice the DSP cycles that passed.
- **Kernel.** Threads with strict priority scheduling on one core, events, mutexes, semaphores,
  timers, address arbiters, handles and `svcSendSyncRequest` to the services.
- **Services.** `fs:USER` serves the `FILE` chunks as the game's RomFS: a level-3 RomFS image
  (archive 3, `OpenFileDirectly`) built from them. `dsp::DSP` drives the emulated DSP. `cfg:u`
  reports a Japanese console with stereo output. Unknown services answer every request with
  success.
- **Output.** The DSP's output at 268,111,856 / 8192 ≈ 32,728.498 Hz, 16-bit stereo, not
  resampled. Players report 32,728 Hz.

### SVC costs

Each SVC adds a fixed number of ARM11 cycles to the clock, as measured on hardware:

| SVC | Cycles |
|---|---|
| 0x08 `CreateThread` | 5,214 |
| 0x0A `SleepThread` | 946 |
| 0x0B `GetThreadPriority` | 616 |
| 0x0C `SetThreadPriority` | 1,812 |
| 0x14 `ReleaseMutex` | 1,324 |
| 0x16 `ReleaseSemaphore` | 2,713 |
| 0x17 `CreateEvent` | 4,329 |
| 0x18 `SignalEvent` | 3,285 |
| 0x19 `ClearEvent` | 1,389 |
| 0x1B `SetTimer` | 5,163 |
| 0x22 `ArbitrateAddress` | 5,664 |
| 0x23 `CloseHandle` | 2,937 |
| 0x24 `WaitSynchronization1` | 4,005 |
| 0x25 `WaitSynchronizationN` | 6,918 |
| 0x28 `GetSystemTick` | 340 |
| 0x32 `SendSyncRequest` | 5,825 |
| 0x37 `GetThreadId` | 677 |

Every other SVC costs 1,000 cycles.

## Archive mode

An archive-mode player runs a model of the SDK's sound library on the DSP firmware instead of
the game's code:

- **The DSP** is the same as in game mode: the firmware from the `FILE` chunk the descriptor
  names, on an emulated Teak DSP, with 64 MiB of FCRAM at physical 0x20000000 for wave data.
- **`nn::snd`**: the ARM11 side of the DSP protocol. It initialises the firmware through the
  audio pipe, writes each frame's voice parameters to the shared-memory structures and reads the
  voices' status back, one frame per 160 samples. The limiter is on (clipping mode 1), as
  `nn::snd` sets it.
- **`nw::snd`**: the sequence player (sequence commands, tracks, variables, random numbers),
  bank lookup and wave loading, envelopes, LFO, sweep, portamento, pitch-to-rate conversion,
  volume and pan curves, the track low-pass filter, the biquad filter presets (low-pass,
  high-pass, and band-pass at 512, 1,024 and 2,048 Hz, from the game's coefficient tables),
  front bypass, and voice allocation with priorities. `nw::snd` has 23 voices, the number
  Pokemon X gives it in place of the library's 24, and drops the lowest-priority one when a note
  needs another; `nn::snd` drops DSP voices by its own priorities when a stereo voice needs more
  of its 24 than are free. The random number generator (x = x × 0x19660D + 0x3C6EF35F, from
  0x12345678) steps once a frame, after the channels' updates, and takes 17 steps before the
  sound's first frame, matching its state when the sequence first runs in game mode.

3SF's implementation (`src/nwsnd`, `src/nnsnd`) was written from a study of Pokemon X's
`code.bin` (NintendoWare for CTR as linked into that game). Other games may use other versions of the
library, or another number of voices, so archive-mode rips of them are close but not guaranteed
exact. When a game has a driver profile, game mode is preferred.

Version 1 plays sequence sounds only. An archive stores files directly in its FILE block or
inside a group file (CGRP). A group's INFO block lists each embedded file's id, offset and size;
some files exist only inside groups. External banks and wave archives aren't supported, so the
ripper skips sequences that need them.
The output starts with the first frame after the sound starts, and the sound has finished when
the sequence has ended and no voice is sounding.

## Driver (game mode)

Game-mode rips carry 3SF's driver, `driver/driver.c`. It's loaded at 0x0E000000 and its
parameter block is at **0x0E000010**:

```
0x00  u32  magic "3SFD"
0x04  u32  version (1)
0x08  u32  status (written by the driver): 0 booting, 1 playing, 2 finished,
           0x80000000 | step on error
0x0C  u32  sound id: the nw::snd item id of the sound (0x01000000 | index)
0x10  u32  output mode: 1 stereo (version 1 plays stereo only)
0x14  u32  sound handle slot of the game's sound system
0x18  u32  size of the device heap used for the sound's data
0x1C  u32  period of the per-frame update in nanoseconds (16,713,680: 59.83 Hz)
0x20  ...  game profile: addresses of the game's functions and data (see driver.c)
```

A `.mini3sf` patches only the sound id (a 4-byte `MEM` chunk at 0x0E00001C). After applying all
`MEM` chunks, a version 1 player checks the driver block. If the block has the expected magic,
the player rejects versions other than 1 and output modes other than 1 (stereo).

Version 1's game profile is for games whose sound code is Game Freak's `gfl::snd` on top of
`nw::snd`, such as Pokemon X: the driver calls gfl's functions to load and play a sound. A game
that drives `nw::snd` itself needs a later version of the block.

After loading the sound, the driver repeatedly sleeps for the period at 0x1C (`svcSleepThread`)
and runs the game's sound update. The interval between updates is therefore the sleep period
plus the update's execution time. The driver sets status to 2 when the game's sound handle is
free again.

The descriptor's status address points at the status word (0x0E000018). A player uses it at both
ends of the sound:

- **Start.** The player runs the rip from boot and throws away the DSP's output until the status
  first reads 1 or 2, so the boot and the loading of the sound don't count towards its length.
  It stops with an error if the status has bit 31 set (the low byte is the step that failed), or
  if it hasn't reached 1 after 10 emulated seconds (2,681,118,560 ARM11 cycles). With a status
  address of 0 there's no status word, and the output starts at boot.
- **End.** A player plays for the tagged length plus fade, like any PSF player. Without a length
  tag it plays until the driver reports that the sound has finished (status 2), plus a tail of
  16,364 frames (half a second), or for a default length and fade, whichever comes first. The
  player checks the status after every millisecond of emulated time (268,111 ARM11 cycles), and
  the sound ends at the frame the DSP had reached at the first check that saw status 2.

## Tags

The standard PSF tags apply: `title`, `artist`, `game`, `year`, `genre`, `comment`,
`copyright`, `length`, `fade`, `volume`, `_lib`, `_libN`. `3sfby` credits the ripper. The ripper
also writes `3sf_sound` with the sound's label from the sound archive (for example
`SEQ_BGM_TITLE`) and `3sf_mode`: `game` or `archive`, so that players and listeners can
tell the modes apart.

`length` and `fade` count time at 32,728 Hz, the rate players report, not at the DSP's exact
rate: a player plays `length` × 32,728 stereo samples before the fade, and the ripper writes a
length of N samples as N / 32,728 seconds, rounded to the millisecond.

When there's no `length` tag, a player should play until the driver reports that the sound
finished, or to a default length.

`3sf_var` sets sequence variables before a sequence starts. Some sequences play nothing until
the game sets one of their variables. The value is one or more assignments `N=V`, separated by
commas or white space. Line breaks count as white space. A tag on several lines therefore works
too. N is 0 to 15 for the sound's player variables, and 16 to 31 for the global variables
(global variable N − 16). V is −32,768 to 32,767. Each number is decimal, or hex with a `0x`
prefix, and may have a minus sign. A later assignment to the same variable wins. In archive
mode, a player sets the variables after it sets up the sequence and before the sequence's first
tick. A player ignores the whole tag if any part of it is malformed or out of range. Game mode
ignores the tag: version 1's driver can't set variables. `3sfrip --var N=V` writes the tag,
with the assignments in increasing order of N, in decimal, separated by ", ":
`3sf_var=0=1, 17=-1`.

## Ripping

`3sfrip` builds a set from a decrypted ROM image (`.3ds`/`.cci`, `.cxi` or `.cia`), from an
extracted game (`exefs/code.bin`, `exefs/exheader.bin` and `romfs/`), or from a sound archive
(`.bcsar`). Encrypted images are rejected: an NCCH must have the NoCrypto flag, and a CIA's
content must not be encrypted. `3sfrip --list` shows every sound archive (`.bcsar`) in the
RomFS. A game with a driver profile is ripped in game mode, anything else in archive mode;
`--mode` overrides. Each input's set goes in a directory next to it, named after it with `_3sf`
added, unless `-o` names a directory for them all.

### Game mode

The `.3sflib` contains:

- the game's text, rodata and data segments (from `code.bin` and the exheader), with `nnMain`
  patched to branch to the driver;
- the driver and its parameter block with the game profile;
- the sound archive (`.bcsar`) and the DSP component (`dspaudio.cdc`) as `FILE` chunks;
- a process descriptor.

Each `.mini3sf` selects one sequence or wave sound. A sequence's length comes from playing it
on 3SF's model of the sound player (see "Lengths").

Game profiles (function addresses) are specific to each game and version, and are matched by
program ID and the CRC-32 of the decompressed `code.bin`. Version 1 knows Pokemon X (Japan,
`0004000000055D00`). A profile names one sound archive; other archives in the same game are
reported, and `--mode archive` rips them.

### Archive mode

The `.3sflib` holds the sound archive and a DSP firmware as `FILE` chunks, with an archive
descriptor. Each `.mini3sf` selects one sequence with a `SND ` chunk. Lengths come from the same
analysis as in game mode. `--var` sets variables for every sequence ripped, in the analysis and
in the `3sf_var` tag.

A sound archive doesn't contain the firmware that plays it. For a game, the ripper uses the
game's: a DSP1 `.cdc` file in the RomFS, or else a DSP1 image linked into the game's code,
as some games have instead of a file. For a loose `.bcsar` it takes the firmware file
that `--firmware` names or that's given along with the archive, or else one in the archive's
directory: any 3DS game's `dspaudio.cdc`, or the `dspfirm.cdc` that homebrew dumps from a
console. The ripper prints a notice that archive mode uses 3SF's model of
the SDK player and that the decrypted game gives exact rips. Sequences whose data isn't all in
the archive (external files, or a truncated archive) are listed and skipped, and wave sounds
aren't ripped.

Tags written by `3sfrip`: `title` and `3sf_sound` (the sound's label), `game`, `length` and
`fade` (sequences only), `3sf_mode`, `3sfby`, `utf8=1`, and optionally `3sf_var`, `artist`,
`year` and `copyright`.

### Lengths

`3sfrip` works out a sequence's length by playing it on 3SF's model of the sound player (see
"Archive mode") with the rip's DSP firmware. A looping sequence gets two loops and a
10-second fade. A sequence that ends plays until half a second after its last sound. Any
silence before the sequence ends is cut. A sequence still playing at 600 seconds gets a length
of 600 seconds and the fade. Sound means a sample louder than 8 of 32,767 (−72 dB). In either
mode, the ripper lists and skips the sounds that make no sound: a sequence that the analysis
finds never sounds, and a sound whose volume in the archive is 0. The sound library multiplies
everything a sound plays by that volume.

A note of 9,600 ticks or more (200 beats at the default timebase) is a held note. A sound that
plays until the game stops it gives its notes such lengths. The game then stops the sound
first.

A sequence loops when its main track jumps back to a command it has already played, or reaches
the end of a loop that repeats forever. A jump back inside a subroutine doesn't count, with two
exceptions. One is an unconditional jump whose pass ran no conditional command: nothing can
leave that loop. The other is an unconditional jump in a subroutine that the track can't leave
while it holds a held note or a note of length 0 on a looping wave at its sustain level. A
track's pass is what the track plays from the command it jumps back to until the jump. A pass
changes the sound when the track plays a note, changes a setting of the track or the player, or
opens a track. A tie or a monophonic note that continues the newest note at the same key and
volume, with no sweep, changes nothing. A pass that changes nothing is idle. A track that only
polls or counts a variable makes idle passes. An idle pass is never a loop. Any other pass of the
first track is a loop. A pass of a later track is a loop if the track played a note during it
and is the main track. A pass that played no note is a loop too while the track holds a held
note or a note of length 0 on a looping wave at its sustain level and every other track still
playing rests. A track rests when it waits forever, or when its last pass was idle and it has
changed nothing since. A track is the main track when no earlier track still playing has loops
that would count. The first track's loops count unless its last pass was idle or it waits
forever. A later track's loops count only if its last pass played a note and wasn't idle, and
the track doesn't wait forever. Two loops count together only if they come from the same track
and go back to the same command. A loop of another track, or one that goes back elsewhere,
starts the count again. Loops before the first sound don't count. A loop that completed without
a sound ends the sequence at its last sound instead.

A track waits forever when it waits a negative number of ticks, waits out a held note, or waits
for its notes to end while one of them has no length and plays a looping wave. A length or a
wait taken from a variable left at its default of −1 makes a negative wait. A track can't
change the sound while its notes sound in three cases. It may wait for its notes of length 0
to end. It may wait forever. Or it may make still passes while none of its notes has length
left to run out. A held note and a note under the damper don't count there. A still pass is
idle and draws no random number. It also leaves every variable that a track reads with the
value that the pass before left. When no track can change the sound, or the tempo or timebase
is 0, the sequence becomes steady once nothing else changes. No note's envelope is in its
attack, hold, decay or release, no track's volume, pan, surround pan or pitch bend is moving,
and no voice starts or stops. A steady sequence loops when every voice that plays has gone back
to its wave's loop start twice since the sound last changed. If it has made no sound since
then, it ended at its last sound instead. A sequence whose tracks will never change the sound
again has ended once no note sounds. A track that makes still passes with notes in them can
still change the sound. Once its notes end, its next pass starts a new one.

A sequence that stays busy without a sound for 180 seconds ended where it fell silent. An
example is one whose music has ended while a track that plays no notes runs on.

## Copyright

A `.3sflib` contains copyrighted game code and data, including the DSP firmware in both modes.
Rips are for personal use.
