# Performance

The matrix records what each firmware costs, so a release has a measured
baseline and a regression has something to be measured against. It is not a
pass/fail gate: it needs an idle machine.

## Run it

```bash
cmake --build build --config Release --target chompi_render
python3 scripts/performance_matrix.py --seconds 10 --json performance.json
```

The script renders a fixed ten-second script (boot, a note, silence) on a copy
of each factory card through `chompi_render` — the emulator and the firmwares,
the same code the plug-in runs, without a host — and reports:

- **real-time factor**: audio seconds rendered per wall-clock second;
- **CPU share at 48 kHz**: what a host would spend at that factor;
- **peak RSS**: the render process's peak resident memory.

Run it on an idle machine (nothing else playing, plugged in, cool), and record
the Mac model and OS beside the table. Numbers move with thermal state and
other work; compare runs on the same machine.

## Baseline

Recorded 2026-11-10 with the release build on a MacBook Air (M2, `Mac14,2`),
macOS 26, while other work was running on the machine; treat these as a floor,
not a quiet-machine result. `chompi_render` includes process start-up and card
loading, so a longer `--seconds` converges closer to the steady-state figure.

| Firmware | Real-time factor | CPU share at 48 kHz | Peak RSS |
|---|---|---|---|
| tape | 68x | 1.46% | 242 MB |
| wave | 55x | 1.83% | 12 MB |
| tempo | 80x | 1.24% | 69 MB |

TAPE's memory is its sample buffer and the recording it keeps in RAM; WAVE and
TEMPO stay small. The JSON from a run (`--json`) records the machine and the
raw numbers: `performance.json`.

## Reading it

A factor of 100x leaves about 1% of a core, which is what a sampler should
cost; below roughly 20x a busy project on that machine would struggle. The
number covers the firmware's DSP and the emulated card reads, not the host's
buffer handling or the GUI. The GUI's cost shows up in the harness's offscreen
render tests, not here.

Re-run after a change to the default patch, the output stage or the rate
adapter, and whenever the factory cards change.
