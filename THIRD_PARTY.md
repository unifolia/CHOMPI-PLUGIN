# Authorship and Third-Party Notices

## Authorship

CHOMPI Club commissioned Electrosmith to engineer a production version of CHOMPI (2022-2023).
Electrosmith is credited for the hardware, designed around Electrosmith's Daisy Seed2 DFM; the
core firmware that ties it to the Daisy, including the TAPE firmware through version 1.0.9; and
the original bootloader. TAPE 2.0 was then written at Chase Bliss on top of that platform, after
CHOMPI Club became part of Chase Bliss.

CHOMPI Club, now part of Chase Bliss, owns this work and released it under the MIT license in
`LICENSE`. Electrosmith is credited as the designer of the hardware and as the author of the
original firmware and platform, including the DSP and hardware-support source they brought to the
project.

The emulator and plugin (`emulator/`, `plugin/`, `tools/`) are built on that release under the
same license.

## Third-party components

These notices, and the copyright attributions in the source files, must be preserved in any
redistribution.

| Component | Copyright | License | Where |
|---|---|---|---|
| **libDaisy** (subset) | © Electrosmith | MIT | `firmware/libs/libDaisy/`, with its `LICENSE` |
| **DaisySP** | © Electrosmith, Corp. | MIT | `firmware/libs/DaisySP/`, with its `LICENSE` |
| **coreJSON** | FreeRTOS / Amazon | MIT | `firmware/libs/coreJSON/`, with its `LICENSE` |
| **JUCE** | © Raw Material Software | AGPLv3 or the commercial JUCE licence | fetched at build time; not in this repository |

libDaisy's hardware `System` class is replaced by the emulator's `emulator/shim/sys/system.h`,
which is not part of the vendored library.

### JUCE

Plugin builds link JUCE 9, which is dual-licensed under the AGPLv3 and the commercial JUCE
licence. Distributing a build means complying with one of them; building for your own use does
not change this repository's MIT license.

## libDaisy is Electrosmith's CHOMPI adaptation

libDaisy is vendored as Electrosmith's CHOMPI adaptation of v5.4.0. Every file kept here
(including `hid/midi_parser`) is as in that adaptation, and the same in the TAPE, WAVE and TEMPO
releases.

## DSP derived from Mutable Instruments

Portions of the FX engine derive from Émilie Gillet's open-source work (Mutable Instruments),
released under the MIT license. In each of `firmware/<tape, wave, tempo>/src/`:

- `reverb.h`: carries the full original MIT notice. Do not remove it.
- `fx_engine.h`: ported from `pichenettes/eurorack` (`plaits/dsp/fx/fx_engine.h`), original
  code by Émilie Gillet, 2014.
- `limiter.h`: extracted from `pichenettes/stmlib`.

The firmware's encoder driver, influenced by the Mutable Instruments encoder classes, is not
included; `emulator/shim/emu_hardware.h` emulates its timing.

## A note on the vendored libraries

Daisy is an open-source hardware and software platform by Electrosmith; libDaisy and DaisySP are
published by Electrosmith under the MIT license. The copies here keep their original copyright
headers, license files and author attributions. DaisySP is as shipped with the CHOMPI firmware
(it carries no version tag), and coreJSON is v3.2.0 as published.
