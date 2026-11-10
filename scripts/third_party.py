#!/usr/bin/env python3
"""Check the third-party inventory and collect every licence text it needs.

CHIMPO ships MIT firmware and code, but its plugin builds link JUCE 9 under
the AGPLv3 (see THIRD_PARTY.md), so a binary may only go out with the notices
that say so. This gate:

- requires a licence file for every vendored component (libDaisy, DaisySP,
  coreJSON) and the Mutable Instruments notice in each firmware's reverb.h;
- requires THIRD_PARTY.md to name every component, and the repository's own
  LICENSE to be the MIT text;
- requires the fetched JUCE tree's licence to be the AGPLv3 one, and the
  AGPLv3 text to be vendored in licenses/;
- collects the texts into target/notices/ for the installers, and fails the
  packaging step if anything is missing.

Run after the CMake configure (it fetches JUCE), before packaging:
    python3 scripts/third_party.py [--juce <path to JUCE source>]
The JUCE tree is looked up in $JUCE_SOURCE, build/_deps/juce-src and any
build*/_deps/juce-src.
"""
import argparse
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "target" / "notices"
VENDORED = [  # (name in THIRD_PARTY.md, licence file, output name)
    ("libDaisy", "firmware/libs/libDaisy/LICENSE", "libDaisy"),
    ("DaisySP", "firmware/libs/DaisySP/LICENSE", "DaisySP"),
    ("coreJSON", "firmware/libs/coreJSON/LICENSE", "coreJSON"),
]
FIRMWARES = ["tape", "wave", "tempo"]
problems: list = []


def fail(message: str) -> None:
    problems.append(message)


def juce_source(argument: str = "") -> Path:
    candidates = [argument, os.environ.get("JUCE_SOURCE", "")]
    candidates += [str(ROOT / "build/_deps/juce-src")]
    candidates += [str(p) for p in sorted(ROOT.glob("build*/_deps/juce-src"))]
    for candidate in candidates:
        if candidate and (Path(candidate) / "modules").is_dir():
            return Path(candidate)
    raise SystemExit("No JUCE source found: configure the build first, or pass --juce <path>")


def main() -> None:
    parser = argparse.ArgumentParser(description="Check and collect third-party notices.")
    parser.add_argument("--juce", default="", help="path to the fetched JUCE source")
    args = parser.parse_args()

    third_party = (ROOT / "THIRD_PARTY.md").read_text(encoding="utf-8")
    licence = (ROOT / "LICENSE").read_text(encoding="utf-8")
    if "MIT License" not in licence:
        fail("LICENSE is no longer the MIT text")
    agpl = ROOT / "licenses" / "AGPL-3.0.txt"
    if not agpl.is_file() or "GNU AFFERO GENERAL PUBLIC LICENSE" not in agpl.read_text(encoding="utf-8"):
        fail("licenses/AGPL-3.0.txt is missing: the JUCE licence ships with every build")

    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)

    for name, relative, out_name in VENDORED:
        if name not in third_party:
            fail(f"THIRD_PARTY.md does not list {name}")
        path = ROOT / relative
        if not path.is_file():
            fail(f"{relative} is missing: {name} has no licence text")
            continue
        (OUT / f"{out_name}.txt").write_text(
            f"{name}\nLicence: {path.read_text(encoding='utf-8', errors='replace')}", encoding="utf-8")

    # The firmware (CHOMPI Club, MIT) and the Mutable Instruments notices it
    # carries inside its sources.
    for firmware in FIRMWARES:
        reverb = ROOT / "firmware" / firmware / "src" / "reverb.h"
        if not reverb.is_file():
            fail(f"firmware/{firmware}/src/reverb.h is missing")
        elif "MIT" not in reverb.read_text(encoding="utf-8", errors="replace"):
            fail(f"firmware/{firmware}/src/reverb.h no longer carries the Mutable Instruments MIT notice")
    if "Mutable Instruments" not in third_party or "Electrosmith" not in third_party:
        fail("THIRD_PARTY.md no longer credits Mutable Instruments and Electrosmith")

    juce = juce_source(args.juce)
    juce_licence = juce / "LICENSE.md"
    if not juce_licence.is_file():
        fail(f"{juce_licence} is missing")
    elif "AGPL" not in juce_licence.read_text(encoding="utf-8", errors="replace"):
        fail(f"{juce_licence} is not the AGPLv3-or-commercial JUCE licence")
    else:
        (OUT / "JUCE-LICENSE.md").write_text(juce_licence.read_text(encoding="utf-8"), encoding="utf-8")
    if "JUCE" not in third_party:
        fail("THIRD_PARTY.md does not list JUCE")

    (OUT / "AGPL-3.0.txt").write_text(agpl.read_text(encoding="utf-8"), encoding="utf-8")
    (OUT / "LICENSE-MIT.txt").write_text(licence, encoding="utf-8")
    (OUT / "README.txt").write_text(
        "Licence texts for CHIMPO's builds. CHIMPO and its firmware are MIT (LICENSE-MIT.txt).\n"
        "The plug-ins link JUCE under the AGPLv3 (JUCE-LICENSE.md, AGPL-3.0.txt); the\n"
        "corresponding source is https://github.com/unifolia/CHOMPI-PLUGIN at the release\n"
        "tag's commit. The vendored components are libDaisy, DaisySP and coreJSON.\n",
        encoding="utf-8")

    if problems:
        raise SystemExit("Third-party notices are incomplete:\n  " + "\n  ".join(problems))
    print(f"Notices OK: {len(list(OUT.iterdir()))} files in {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
