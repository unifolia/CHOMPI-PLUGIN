# Validation

What checks a build before it ships, and what still needs a host. CI runs all
of it on a pull request or a `v*` tag — never on a merge to main, by the
Bellcast pattern Marcasite set (`docs/RELEASE.md`); branch protection requires
the PR's checks with the branch up to date. A tag also validates the universal
macOS build's x86_64 half on an Intel runner.

## Automatic, every pull request and tag

| Check | What it proves | Where |
|---|---|---|
| `ctest` | DSP unit tests, nine firmware renders against WAV/MIDI references, the processor as a host drives it (state recall, damaged state, no allocation in `processBlock`) | `tests/` |
| `chompi_hostcheck` | the built VST3 loads and plays each firmware with the source `cards/` moved aside | `tools/chompi_hostcheck.cpp` |
| Steinberg VST3 `validator` | 500+ SDK conformance tests on the VST3 bundle | `scripts/validate_formats.py` |
| pluginval, strictness 10, no GUI tests | host-contract behaviour: parameters, state, buses, fuzzing | `scripts/validate_formats.py` |
| clap-validator | CLAP conformance on the CLAP bundle (`engineering.bellcast.chimpo`) | `scripts/validate_formats.py` |
| `auval -strict` ×3 with `-stress 600`, `-oop`, `-real-time-safety` | Apple's AU conformance, out-of-process load, blocking calls on the render thread | `scripts/validate_au.py` |
| pluginval on the AU | the same host-contract behaviour for the AU | `scripts/validate_au.py` |
| `scripts/third_party.py` | every vendored licence and JUCE's AGPLv3 text is present, and the texts are collected for the installers | `scripts/third_party.py` |
| installer build + install test | each OS's installer installs what it says, byte-identically, byte-identical to the validated build, and removes cleanly | `scripts/test_*_install.*` |
| performance matrix (manual) | each firmware's real-time factor on an idle machine | `scripts/performance_matrix.py`, [PERFORMANCE.md](PERFORMANCE.md) |

Tools are pinned: the VST3 SDK by commit, pluginval and clap-validator by
version and SHA-256. A failure fails the job unless `EXCEPTIONS` in
`scripts/validate_formats.py` names it with a reason (`by design` or `known
bug`; known bugs also print a CI warning). Never add an exception to go green.

## Locally

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCHIMPO_INSTALL_PLUGINS=OFF -DCHIMPO_BUILD_TESTS=ON
cmake --build build --config Release --parallel 8
ctest --test-dir build -C Release --output-on-failure

python3 scripts/validate_formats.py       # VST3, CLAP and pluginval
python3 scripts/validate_au.py            # macOS; restores any installed CHIMPO AU afterwards

python3 scripts/third_party.py            # licences, into target/notices
CHIMPO_UNSIGNED=1 bash scripts/package_macos.sh   # or package_windows.sh / package_linux.sh
```

`target/validators/` holds the tools and logs (ignored by git). A local AU run
can be shortened: `CHIMPO_AUVAL_PASSES=1 CHIMPO_AUVAL_STRESS=60 python3
scripts/validate_au.py`. `auval -real-time-safety` needs passwordless `sudo`
(it starts dtrace); locally it is skipped without it, and CI requires it.
Don't run the AU script while a host has CHIMPO open.

Run only some checks with `--only`, e.g. `python3 scripts/validate_formats.py
--only pluginval`.

## Known real-time-safety findings (`auval-rt`)

`auval`'s dtrace pass watches the render thread and reports each blocking call.
On the 1.0.1 build it found ten, in three groups. They are narrow `KNOWN_BUG`
exceptions in `scripts/validate_formats.py`: every run prints them as CI
warnings, and they stay open until fixed or accepted.

| Where | Cause | Whose |
|---|---|---|
| `ausdk::AUMethodRender` | Apple's AudioUnitSDK locks a `recursive_mutex` in `AUBase::DoRender` on every render. Every AU built on it does this | Apple's SDK; same finding as Marcasite |
| `JuceAU::Render` | JUCE's AUv2 wrapper takes `incomingMidiLock` around MIDI delivery | JUCE, upstream |
| `emu::VirtualCard::{Exists,IsDir,OpenFile,Read}`, `ChompiProcessor::renderBlock` | **fixed in PR #7**: the card used a `std::mutex` per file operation on the render thread; it now uses `emu::SpinLock`, and the flusher waits on its own `std::mutex`. Its exceptions are gone, so a reappearance fails CI | was ours |
| `ChompiProcessor::processBlock` | a `free` on the render thread. Not reproduced in `tests/plugin_test.cpp` (its counter hooks `operator new/delete`; a `free` interposer on the same path saw none), so it follows host-side parameter traffic | ours; to be located |
| clap-validator `state-invalid-random` | the CLAP reports a corrupt state as loaded: `juce::AudioProcessor::setStateInformation` returns void, so clap-juce-extensions cannot tell the host it refused it. CHIMPO ignores the bytes and keeps running | clap-juce-extensions, upstream |
| `daisy_tape::{Engine::UpdateFileExists,FileCopier::NeedsOverwrite}` | the released CHOMPI firmware calls `sprintf` on the audio thread, and libc's `printf` frees internally | CHOMPI's firmware, which stays as released |

Shipping with any of these open needs the owner's agreement: the first two
need upstream fixes, the `processBlock` free is ours and still open, and the
last cannot change without diverging from the firmware.

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
