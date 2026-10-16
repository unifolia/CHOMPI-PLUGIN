# CHIMPO

CHIMPO is an open-source plugin that emulates **CHOMPI**. The firmware code
is as released, except for a few 64-bit portability fixes (`firmware/README.md`).

CHIMPO is an independent project, not affiliated with or endorsed by CHOMPI
Club or Chase Bliss. I mean, the names are so different. CHOMPI is a trademark of CHOMPI Club (see
[License](#license)).

## Build

Runs on macOS 11+. Clone this repository, then in Terminal, from its folder:

```bash
./build.sh
```

The script asks which formats to build and whether to include
Intel, checks for Apple's Command Line Tools and CMake 3.22+ (offering to
install them), builds, and installs for the current user:

AU, VST3 | `~/Library/Audio/Plug-Ins/`
Factory cards | `~/Library/Application Support/CHIMPO/Factory Cards/<TAPE, WAVE, TEMPO>`

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

I am working on getting this on Windows/Linux as well.

## Layout

| | |
|---|---|
| `firmware/<tape, wave, tempo>/src` | the firmwares, portable parts (see `firmware/README.md`) |
| `firmware/libs` | the subsets of libDaisy, DaisySP and coreJSON they use |
| `emulator/` | emulated Daisy Seed and one `Machine` per firmware (no JUCE dependency) |
| `plugin/` | JUCE processor, panel UI, host-rate adapter, output gain and limiter, recordings as FLAC, audio import |
| `cards/<tape, wave, tempo>` | factory SD cards |
| `build.sh` | guided build and install for non-developers |
| `tools/chompi_render` | headless: script the panel/MIDI/clock, render a WAV (`--firmware`) |
| `tools/chompi_harness` | `-DCHIMPO_BUILD_HARNESS=ON`: offscreen UI snapshots, state and host-rate tests |

The original release also contained the hardware design files, the bootloader
and the firmware binaries.

## License

MIT, see [`LICENSE`](LICENSE) and [`THIRD_PARTY.md`](THIRD_PARTY.md); the
firmware and hardware design are CHOMPI Club's, released under that license.
Builds link JUCE, which has its own license terms. The CHOMPI name, logo and
character are trademarks of CHOMPI Club and not covered by the license; see
[`TRADEMARKS.md`](TRADEMARKS.md).
