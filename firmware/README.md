# The firmwares (emulated)

`tape/src`, `wave/src` and `tempo/src` are CHOMPI's TAPE 2.0 sampler, WAVE 1.0
wavetable synth and TEMPO 1.0 pattern generator as released in commit
`a73d732` (`firmware/chompi-<name>/code/src`), minus the files that only drive
the Daisy Seed, and with the changes listed below. Replacements for the
removed files are in `emulator/`.

| Removed | Replaced by |
|---|---|
| `hardware.h`, `encoder.h`, `encoder.cpp` | `emulator/shim/emu_hardware.h` and `emulator/shim/<name>/hardware.h` (each firmware's MIDI interface) |
| `temp_led_stuff.h` (LED PWM/DMA driver) | `emulator/shim/temp_led_stuff.h` |
| `chompi_main.cpp` | `emulator/src/<name>/<Name>Machine.cpp` (line-for-line port as a class) |
| `chompi_sram.lds`, `Makefile`, `.vscode/` | the CMake build |

WAVE and TEMPO also drive libDaisy's MIDI UART/USB handlers, a hardware timer
(their clock) and Cortex-M cache and interrupt calls directly; those are
`emulator/shim/emu_peripherals.h`. The bootloader, battery-management test and
ARM toolchain notes were dropped; they are in the git history.

## Changes to shipped code

- `tape/src/Warble.h`: `SetDelay(10u)` → `SetDelay(size_t(10))`. On the 32-bit
  Daisy `size_t` is `unsigned int`, so this is the overload the hardware calls;
  with a 64-bit `size_t` the original is ambiguous.
- `tempo/src/SampleManager.h`, `tempo/src/SliceEngine.h`: arithmetic on
  `void*` (a GCC extension, rejected by clang in C++) is done on `char*`, which
  is what GCC does. Six expressions; no change in behaviour.

WAVE compiles unchanged. TEMPO's own release notes ask for GCC 13.3 on ARM;
here it builds with the host's clang as C++17.

## Vendored libraries

`libs/` keeps only what the firmwares use, and is shared: these files are
byte-identical in the TAPE, WAVE and TEMPO releases (whose libDaisy and
DaisySP otherwise differ). libDaisy's UI framework, FIFO, WAV header, MIDI
event types and MIDI parser (the CHOMPI adaptation of v5.4.0); DaisySP's
`Source/` (each firmware compiles the modules it uses, `CMakeLists.txt`); coreJSON v3.2.0's source. Each firmware
still compiles its own copy. `UNIT_TEST` is defined to select libDaisy's
host-side `ScopedIrqBlocker`. libDaisy's `sys/system.h` is replaced by an
emulator stand-in (virtual clock), `emulator/shim/sys/system.h`.

## Portability notes

The firmwares assume 32-bit ARM. Known differences on a 64-bit host:

- TAPE's `SampleReader` computes `val * (f_size - sizeof(WAV header))` in
  `size_t`. With no file open this wraps at 2^64 instead of 2^32 before the
  float to `uint32_t` conversion. Both saturate to the same value for the 0 and
  1 the firmware passes at init; other values with no file open can differ.
- `OptionsManager` passes `size_t` to `%d`; arm64 and x86-64 read the low 32
  bits, so the queries are the same.
- TAPE's `Warble` random seed is a function `static`, shared (unsynchronised)
  by every TAPE unit in a process. It only shapes the lo-fi wobble.
- TEMPO's `ArpeggiatorSequencer` reads `notes[i][curIdx[i]]` in places
  without checking for an empty vector (`checkNotePlaying` then discards it).
  On the STM32 that reads near address 0, which is mapped; on a Mac it faults. The port reserves the
  vectors' storage at boot so the read stays in owned memory
  (`emulator/src/tempo/TempoMachine.cpp`).
- TEMPO's clock manager subtracts `System::GetTick()` values in `size_t`,
  which wraps correctly only where `size_t` is 32 bits; the emulator's tick is
  64-bit.
