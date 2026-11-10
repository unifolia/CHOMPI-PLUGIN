# Validation

What checks a build before it ships, and what still needs a host. CI runs all
of it on macOS, Windows and Linux (`.github/workflows/build.yml`); a `v*` tag
also validates the universal macOS build's x86_64 half on an Intel runner.

## Automatic, every push and pull request

| Check | What it proves | Where |
|---|---|---|
| `ctest` | DSP unit tests, nine firmware renders against WAV/MIDI references, the processor as a host drives it (state recall, damaged state, no allocation in `processBlock`) | `tests/` |
| `chompi_hostcheck` | the built VST3 loads and plays each firmware with the source `cards/` moved aside | `tools/chompi_hostcheck.cpp` |
| Steinberg VST3 `validator` | 500+ SDK conformance tests on the VST3 bundle | `scripts/validate_formats.py` |
| pluginval, strictness 10, no GUI tests | host-contract behaviour: parameters, state, buses, fuzzing | `scripts/validate_formats.py` |
| `auval -strict` ×3 with `-stress 600`, `-oop`, `-real-time-safety` | Apple's AU conformance, out-of-process load, blocking calls on the render thread | `scripts/validate_au.py` |
| pluginval on the AU | the same host-contract behaviour for the AU | `scripts/validate_au.py` |
| CLAP validator | only once CHIMPO ships a CLAP build; the script picks it up automatically | `scripts/validate_formats.py` |

Tools are pinned: the VST3 SDK by commit, pluginval and clap-validator by
version and SHA-256. A failure fails the job unless `EXCEPTIONS` in
`scripts/validate_formats.py` names it with a reason (`by design` or `known
bug`; known bugs also print a CI warning). Never add an exception to go green.

## Locally

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHIMPO_INSTALL_PLUGINS=OFF -DCHIMPO_BUILD_TESTS=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure

python3 scripts/validate_formats.py
python3 scripts/validate_au.py            # macOS; restores any installed CHIMPO AU afterwards
```

`target/validators/` holds the tools and logs (ignored by git). A local AU run
can be shortened: `CHIMPO_AUVAL_PASSES=1 CHIMPO_AUVAL_STRESS=60 python3
scripts/validate_au.py`. `auval -real-time-safety` needs passwordless `sudo`
(it starts dtrace); locally it is skipped without it, and CI requires it.
Don't run the AU script while a host has CHIMPO open.

Run only some checks with `--only`, e.g. `python3 scripts/validate_formats.py
--only pluginval`.

## What no validator covers

CHIMPO is an instrument, so the Bellcast effect requirements for a host bypass
parameter and a latency-compensated dry path do not apply. It also has no text
fields, so the keyboard-focus fix does not apply. The rest of the host
contract (state recall across versions, damaged state, removal mid-playback,
parameter text round-trip) is tested in `tests/plugin_test.cpp` and by
pluginval.

These still need a real host, on the builds that ship:

- insert, play, remove while playing;
- automation, preset and state recall, offline export or freeze;
- buffer-size and sample-rate changes, two instances, GUI open/close;
- silence and tails, rapid firmware switches;
- macOS: Logic (AU), a VST3 host, a CLAP host once there is one;
- Windows: Cubase and Reaper; Linux: Reaper or Bitwig.

The 600 s auval stress passes do not replace listening on real hardware; they
catch crashes and contract violations, not a firmware regression.
