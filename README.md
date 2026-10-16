# CHIMPO

CHIMPO is an open-source AU / VST3 plugin that emulates **CHOMPI**, running
its own firmwares compiled against an emulated Daisy Seed: **TAPE 2.0**, the sampler and looper;
**WAVE 1.0**, the wavetable synth; and **TEMPO 1.0**, the dual-engine pattern
generator, which can follow the DAW's tempo and transport. The firmware code
is as released, but for a few 64-bit portability fixes (`firmware/README.md`),
so the panel, sounds and effects behave as on the hardware. Pick the firmware
in the top-left of the panel.

CHIMPO is an independent project, not affiliated with or endorsed by CHOMPI
Club or Chase Bliss. CHOMPI is a trademark of CHOMPI Club (see
[License](#license)).

## Build

Runs on macOS 11+. Clone this repository, then in Terminal, from its folder:

```bash
./build.sh
```

The script asks which formats to build (AU for Logic Pro, GarageBand and
MainStage; VST3 for most other DAWs) and whether to include
Intel, checks for Apple's Command Line Tools and CMake 3.22+ (offering to
install them), builds, and installs for the current user:

| | |
|---|---|
| AU, VST3 | `~/Library/Audio/Plug-Ins/` |
| Factory cards | `~/Library/Application Support/CHIMPO/Factory Cards/<TAPE, WAVE, TEMPO>` |

`./build.sh au vst3 [--universal]` skips the questions. The first build
downloads JUCE 9.0.4 and takes a few minutes; afterwards the source folder is
no longer needed, but keep it to update (`git pull`, then `./build.sh`).

For development, CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

This builds both formats and installs them and the factory cards as above
(`-DCHIMPO_INSTALL_PLUGINS=OFF` installs nothing; `-DCHIMPO_FORMATS=AU`
picks formats). Builds are native-arch; add
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for hosts running under Rosetta.
`-DFETCHCONTENT_SOURCE_DIR_JUCE=<path>` uses a local JUCE checkout.

## The SD card

The firmware reads and writes a microSD card; here that is a folder. Each
firmware has its own card layout, so each has its own default folder in
`~/Music/CHIMPO`: `TAPE SD Card`, `WAVE SD Card` and `TEMPO SD Card`. Switching firmware
restarts the unit on that firmware's default card. The first time a firmware
runs (for TAPE, a host's plugin scan counts) its folder is filled from its
factory card, which the build installs from `cards/` (above; a build that
skipped installing reads `cards/` in place). A folder you choose instead is
used as found: an empty one is a blank card, and **Restore Factory Samples**
fills it. Edit it as you would the real card:

- TAPE: samples are 48 kHz 16-bit stereo WAVs named
  `<jammi|cubbi>_<bank a-e><slot 1-14>.wav`; the firmware builds the `_double`
  variants itself at boot. `options.json` holds MIDI channels, record latch,
  tape slew, monitoring, pitch quantization and split delay.
- WAVE: up to seven wavetables, the first `.wav` files in name order, each 33
  frames of 2048 32-bit float samples (the Serum layout). Presets remember a
  table by position, so renaming or reordering them changes saved presets.
  `options.json` holds MIDI channels, clock out, and CC in and out.
- TEMPO: 48 kHz 16-bit stereo WAVs of up to 10 seconds, in folders:
  `Chromatic/chroma_a<1-14>.wav` (played chromatically), `Slice/slice_a<1-14>.wav`
  (sixteen slices each) and `Buffer/buffer.wav` (the recording). `options.json`
  holds MIDI channels (one out per engine), clock out, CC in and out, and
  *Midi Start-Stop Message Behavior* (1: send and follow, 2: send only,
  3: follow only, 4: neither).

Only `.wav` and `.json` files are read, in the folder and one level of
subfolders. The card is loaded into memory when the unit powers on, so changes
made on disk need **Reload SD Card** (the ••• menu), which power-cycles the
unit like reinserting the card. Writes made by the firmware (presets, saved
recordings) are mirrored to the folder shortly after they happen.
`CHIMPO_TAPE_CARD=<folder>`, `CHIMPO_WAVE_CARD=` and `CHIMPO_TEMPO_CARD=` in
the environment change the default folders.

## In a DAW

- Instrument plugin. MIDI, as each firmware reads it:
  - TAPE: notes 24-72 play the keyboard (C4 = original pitch); CC 20-25 set
    the six knobs on their current page (not while the shift layer is held;
    the looper knob only while the looper plays); CC 26/27 press play/loop.
  - WAVE: notes 36-84 (the keys are 48-72); CC 20-25 set the knobs but the
    purple one; CC 14/15 press the CHIMPO and loop keys. With clock out on in
    `options.json`, it sends MIDI clock while the sequencer plays, at 12 PPQN
    like the hardware (half the usual 24).
  - TEMPO: notes 24-72 play the engine on show (or feed its pattern); CC
    20-25 set the knobs, CC 14/15 press the CHIMPO and loop keys. Each engine
    sends its notes on its own channel.

  By default MIDI on any channel is accepted (menu: *Respond to MIDI on All
  Channels*); turn that off to honor `options.json`'s channel. All Notes Off
  and All Sound Off (CC 123/120, sent by a host's panic) release the notes MIDI
  is holding; the firmwares themselves ignore them.
- TEMPO follows the host (••• menu: *Follow Host Tempo and Transport*, on by
  default): the plugin puts it in its MIDI-clock sync mode and sends it clock
  on the host's beat grid, Start when the transport starts (at beat 0 after a
  count-in, and again on a cycle or locate, so patterns restart with the host),
  and Stop when it stops. While the transport is stopped the clock keeps
  running at the host's tempo, so patterns played live keep the project tempo.
  Steps then fall on TEMPO's own division of the beat (eighth notes by
  default). Hosts that report no tempo leave TEMPO on its own clock. With the option off, MIDI clock and start/stop sent to the plugin
  reach TEMPO instead.
- The plugin's audio input is the line input, so a side-chain source can be
  sampled into the CHIMPO buffer or looper. The mic input hears the same
  signal in mono.
- Main output is the line out. An optional second stereo output carries the
  headphone mix (which alone includes input monitoring in the default
  `Monitor Position`). Both are raised to a DAW level (below) and never
  exceed -1 dBFS. The plugin reports an infinite tail: it can sound with no
  MIDI coming in (the looper, TEMPO's patterns, delay feedback).
- The project remembers the firmware, card folder and mode switch, and the
  front panel's state. TAPE: selected mode/bank/slot and that slot's settings,
  effect and volume knobs, shift-layer settings; other slots' settings are on
  the card (`presets.json`), as on the hardware. WAVE: the sound as it stands
  (saved to a preset or not), octave, LFOs, tempo and division, and the
  sequence, which comes back stopped. TEMPO: both engines' A and B states
  (knobs, samples, patterns, sequences, divisions), which engine and state are
  showing, tempo and clock mode; patterns come back stopped.
  A project on the default card uses each Mac's own default folder. If a
  project's card folder is missing (another Mac, an unplugged drive), the unit
  runs on the default card and says so above the panel; the project keeps
  asking for its folder, so reconnect it and choose **Reload SD Card**.
  Recordings are RAM, as on the hardware: TAPE's CHIMPO buffer and looper are
  empty at power-on, and TEMPO's buffer reloads `Buffer/buffer.wav`. Save a
  recording to a slot (shift + black key 10, a white key, then CHIMPO) to keep
  it. WAVE stays silent through its boot animation, as on the hardware.

## Playing the panel

| Control | Mouse |
|---|---|
| Keys | click and hold |
| Knobs (endless encoders) | drag up/right or scroll to turn; click to push |
| CHIMPO key (red, with the face) held: shift layer | hold **Shift** with the pointer over the panel, or Option-click to latch |
| Any key combination | Option-click (or right-click) latches a key down until clicked again |
| Mode switch | click: up = play, down = record |

Hover any control for its normal and shift functions. The firmware's own
timing applies: keys are debounced over ~8 ms and knobs deliver at most one
step per millisecond; turning faster than that drops steps beyond a 64-step
backlog, as a real encoder would.

## How it works

`emulator/` replaces the Daisy Seed: a virtual clock, the CD4021 switch chain
and encoders, the LED chains, MIDI ports, timers and FatFs backed by the
in-memory card. Each firmware's `chompi_main.cpp` is ported to a `Machine`
(`emulator/src/<firmware>/`): each 24-sample block runs the audio interrupt,
every second block the 1 kHz SD interrupt, then one pass of the main loop. The
firmwares define the same names differently, so each is compiled and
prelinked on its own, exposing only its factory (`CMakeLists.txt`). Firmware time advances with the audio the host renders, not the wall
clock, so an offline bounce plays out as it would in realtime. (While the host
isn't rendering and the panel is open, the wall clock drives it instead, to
keep the panel alive.) The firmware always runs at 48 kHz; other host rates are
resampled with a windowed-sinc filter (flat to 20 kHz, aliases kept above it,
about 0.7 ms each way at 44.1 kHz), and the latency is reported to the host.

The firmwares scale their outputs for CHOMPI's analog output stage, which
leaves them 12-24 dB quieter than a typical instrument plugin. The plugin
adds a fixed gain per firmware (TAPE +18 dB, WAVE +24 dB, TEMPO +12 dB), so
a full-velocity chord at the default volume peaks near -4 dBFS, then a
limiter that only acts on peaks above -1 dBFS (1 ms lookahead, included in
the latency). `chompi_render` writes the firmware's own output, without it.

Differences from the hardware: no battery/USB power management, no hardware
test mode or shipping mode, no USB/DIN MIDI distinction, and the input level
is digital full scale rather than the codec's analog level.

## Layout

| | |
|---|---|
| `firmware/<tape, wave, tempo>/src` | the firmwares, portable parts (see `firmware/README.md`) |
| `firmware/libs` | the subsets of libDaisy, DaisySP and coreJSON they use |
| `emulator/` | emulated Daisy Seed and one `Machine` per firmware (no JUCE dependency) |
| `plugin/` | JUCE processor, panel UI, host-rate adapter, output gain and limiter |
| `cards/<tape, wave, tempo>` | factory SD cards |
| `build.sh` | guided build and install for non-developers |
| `tools/chompi_render` | headless: script the panel/MIDI/clock, render a WAV (`--firmware`) |
| `tools/chompi_harness` | `-DCHIMPO_BUILD_HARNESS=ON`: offscreen UI snapshots, state and host-rate tests |

The original release also contained the hardware design files, the bootloader
and the firmware binaries; they remain in the git history (commit `a73d732`).

## License

MIT, see [`LICENSE`](LICENSE) and [`THIRD_PARTY.md`](THIRD_PARTY.md); the
firmware and hardware design are CHOMPI Club's, released under that license.
Builds link JUCE, which has its own license terms. The CHOMPI name, logo and
character are trademarks of CHOMPI Club and not covered by the license; see
[`TRADEMARKS.md`](TRADEMARKS.md).
