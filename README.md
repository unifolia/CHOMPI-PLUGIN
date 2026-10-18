# CHIMPO

CHIMPO is an open-source **CHOMPI** fork and emulation plugin. The firmware code
is as released, except for a few small under-the-hood portability tweaks (`firmware/README.md`). Many thanks to the CHOMPI team for releasing the firmware - all credit goes to them, truly.

CHIMPO is an independent project, not affiliated with or endorsed by CHOMPI
Club or Chase Bliss. I mean, the names are so different. CHOMPI is a trademark of CHOMPI Club (see [License](#license)).

## Download

Ready-built plugins are on the
[Releases](https://github.com/unifolia/CHOMPI-DAW/releases) page, with install
steps: AU and VST3 for macOS 11+ (Apple Silicon and Intel), VST3 for Windows
and Linux (x64). The factory SD cards are inside the plugin. The macOS builds
aren't notarized, so they need one Terminal command before they load; building
from source avoids it. See next section.

## Build

### macOS

Clone this repository, then in Terminal, from its folder:

```bash
./build.sh
```

The script asks which formats to build and whether to include
Intel, checks for Apple's Command Line Tools and CMake 3.22+ (offering to
install them), builds, and installs for the current user in
`~/Library/Audio/Plug-Ins/`.

`./build.sh au vst3 [--universal]` skips the questions. The first build
downloads JUCE 9.0.4 and takes a few minutes; afterwards the source folder is
no longer needed, but keep it to update (`git pull`, then `./build.sh`).

### Windows and Linux

CMake 3.22+ builds the VST3. On Windows, with Visual Studio 2022+ and its
"C++ Clang tools for Windows" component (the firmware needs clang; VS 2026
needs CMake 4.2+):

```bash
cmake -S . -B build -A x64 -T ClangCL
cmake --build build --config Release
```

then copy `build/Chimpo_artefacts/Release/VST3/CHIMPO.vst3` to
`C:\Program Files\Common Files\VST3`. On Linux, with g++ or clang and JUCE's
packages (the list is in `.github/workflows/build.yml`):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

which installs it in `~/.vst3`.

### Development

The CMake build installs the plugins in the user's plug-in folders, except on
Windows (`-DCHIMPO_INSTALL_PLUGINS=ON|OFF` decides). Each bundle carries the
factory cards in `Contents/Resources/Factory Cards`. `-DCHIMPO_FORMATS=AU`
picks formats. Builds are native-arch; add
`-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for hosts running under Rosetta.
`-DFETCHCONTENT_SOURCE_DIR_JUCE=<path>` uses a local JUCE checkout.
`-DCHIMPO_BUILD_TESTS=ON` builds the tests; `ctest` in the build folder runs
them (`tests/CMakeLists.txt`).

Every push and pull request is built and checked on all three systems
(`.github/workflows/build.yml`). Pushing a tag `v<version>`, matching
`project(VERSION)` in `CMakeLists.txt`, also drafts a release with the
packages, to check and publish on GitHub.

## Map

| | |
|---|---|
| `firmware/<tape, wave, tempo>/src` | the firmwares, portable parts (see `firmware/README.md`) |
| `firmware/libs` | the subsets of libDaisy, DaisySP and coreJSON they use |
| `emulator/` | emulated Daisy Seed and one `Machine` per firmware (no JUCE dependency) |
| `plugin/` | JUCE processor, panel UI, host-rate adapter, output gain and limiter, recordings as FLAC, audio import, host automation |
| `cards/<tape, wave, tempo>` | factory SD cards |
| `build.sh` | guided build and install for non-developers |
| `cmake/` | build scripts: no two firmwares define the same name; the factory cards into each bundle |
| `.github/workflows/build.yml` | CI on macOS, Windows and Linux; packages and draft releases |
| `tests/` | DSP unit tests, firmware renders against references, the plugin as a host drives it |
| `tools/chompi_render` | headless: script the panel/MIDI/clock, render a WAV (`--firmware`) |
| `tools/chompi_harness` | `-DCHIMPO_BUILD_HARNESS=ON`: offscreen UI snapshots, state and host-rate tests |
| `tools/chompi_hostcheck` | `-DCHIMPO_BUILD_HOSTCHECK=ON`: loads the built VST3 as a host and plays each firmware |

The original release also contained the hardware design files, the bootloader
and the firmware binaries.

Thanks, and go make some magic.

## License

MIT, see [`LICENSE`](LICENSE) and [`THIRD_PARTY.md`](THIRD_PARTY.md); the
firmware and hardware design are CHOMPI Club's, released under that license.
Builds link JUCE, which has its own license terms. The CHOMPI name, logo and
character are trademarks of CHOMPI Club and not covered by the license; see
[`TRADEMARKS.md`](TRADEMARKS.md).
