#!/usr/bin/env python3
"""Install the unsigned macOS package on a CI runner and check it.

CI builds dist/CHIMPO-<version>-macOS-unsigned.pkg with
`CHIMPO_UNSIGNED=1 bash scripts/package_macos.sh`: the release package's
script, layout and choices, but without the Developer ID signatures, so its
bundles are byte-identical to the build the validators ran on. This test:

- checks Installer offers the AU, VST3 and CLAP, all ticked, and always
  installs the licences;
- installs the CLAP alone, then all three formats over it;
- checks each install: the chosen bundles match the build, the others are
  absent, the licences are the MIT licence and the MIT + AGPL notice, and the
  receipts carry the plugin's version;
- has auval (strict) load the AU from /Library/Audio/Plug-Ins/Components;
- removes what it installed and forgets the receipts.

It installs system-wide through sudo, so it needs sudo without a password
(CI has it; elsewhere run `sudo -v` first). It stops if CHIMPO is already
installed in /Library: run it only on a disposable machine.
Usage: python3 scripts/test_macos_install.py
"""
import plistlib
import re
import subprocess
import tempfile
from pathlib import Path

import validate_au as va
import validate_formats as vf

ROOT = vf.ROOT
ARTEFACTS = ROOT / "build" / "Chimpo_artefacts" / "Release"
PLUGINS = Path("/Library/Audio/Plug-Ins")
VENDOR = Path("/Library/Application Support/Bellcast Engineering")
LICENCES = VENDOR / "CHIMPO"
BUNDLES = {  # Installer choice: (installed, built)
    "component": (PLUGINS / "Components/CHIMPO.component", ARTEFACTS / "AU/CHIMPO.component"),
    "vst3": (PLUGINS / "VST3/CHIMPO.vst3", ARTEFACTS / "VST3/CHIMPO.vst3"),
    "clap": (PLUGINS / "CLAP/CHIMPO.clap", ARTEFACTS / "CLAP/CHIMPO.clap"),
}
RECEIPT = "engineering.bellcast.chimpo.pkg."
_match = re.search(r"^project\(CHIMPO VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text(), re.M)
assert _match is not None, "project(CHIMPO VERSION x.y.z) not found in CMakeLists.txt"
VERSION = _match.group(1)


def receipts() -> dict:
    """CHIMPO's package receipts on this machine: {id: version}."""
    ids = subprocess.run(["pkgutil", "--pkgs=" + re.escape(RECEIPT) + ".*"],
                         capture_output=True, text=True).stdout.split()
    return {i: plistlib.loads(subprocess.run(["pkgutil", "--pkg-info-plist", i],
                                             capture_output=True, check=True).stdout)["pkg-version"]
            for i in ids}


def installed() -> list:
    """Everything of CHIMPO's the package installs that is present now."""
    return ([str(p) for p, _ in BUNDLES.values() if p.exists()]
            + ([str(LICENCES)] if LICENCES.exists() else []) + list(receipts()))


def same(a: Path, b: Path) -> None:
    result = subprocess.run(["diff", "-rq", str(a), str(b)], capture_output=True, text=True)
    if result.returncode != 0:
        vf.fail(f"installed {a} differs from {b}:\n{result.stdout}{result.stderr}")


def check_choices(pkg: Path) -> None:
    listing = plistlib.loads(subprocess.run(["installer", "-showChoicesXML", "-pkg", str(pkg),
                                             "-target", "/"], capture_output=True, check=True).stdout)
    found, todo = {}, list(listing)
    while todo:
        choice = todo.pop()
        found[choice["choiceIdentifier"]] = choice
        todo += choice.get("childItems", [])
    for choice in BUNDLES:
        c = found.get(choice)
        if not c or not (c["choiceIsVisible"] and c["choiceIsEnabled"] and c["choiceIsSelected"]):
            vf.fail(f"Installer does not offer {choice}, ticked by default: {c}")
    c = found.get("licenses")
    if not c or not c["choiceIsSelected"] or c["choiceIsEnabled"] or c["choiceIsVisible"]:
        vf.fail(f"the licences are not a fixed, hidden choice: {c}")
    print("Choices OK: " + ", ".join(BUNDLES) + ", all ticked; licences always installed")


def install(pkg: Path, untick: tuple = ()) -> None:
    with tempfile.NamedTemporaryFile(suffix=".plist") as changes:
        plistlib.dump([{"choiceIdentifier": c, "choiceAttribute": "selected",
                        "attributeSetting": 0} for c in untick], changes)
        changes.flush()
        cmd = ["sudo", "-n", "installer", "-pkg", str(pkg), "-target", "/"]
        if untick:
            cmd += ["-applyChoiceChangesXML", changes.name]
        print("$", " ".join(cmd), flush=True)
        subprocess.run(cmd, check=True)


def check_installed(formats: tuple) -> None:
    for choice, (path, built) in BUNDLES.items():
        if choice in formats:
            same(path, built)
        elif path.exists():
            vf.fail(f"{path} is installed, but {choice} was not chosen")
    with tempfile.TemporaryDirectory() as tmp:
        notice = Path(tmp) / "NOTICE.txt"
        subprocess.run(["python3", str(ROOT / "scripts/licence_page.py"), str(notice)], check=True)
        same(LICENCES / "NOTICE.txt", notice)
    same(LICENCES / "LICENSE", ROOT / "LICENSE")
    same(LICENCES / "THIRD_PARTY.md", ROOT / "THIRD_PARTY.md")
    same(LICENCES / "notices", ROOT / "target/notices")
    # CHIMPO's own licence is the MIT text; the notice must name the AGPLv3
    # (these builds link JUCE under it) and where the source is.
    licence = (LICENCES / "LICENSE").read_text(encoding="utf-8")
    # The installed notice is wrapped: compare on one line.
    notice_text = " ".join((LICENCES / "NOTICE.txt").read_text(encoding="utf-8").split())
    if "MIT License" not in licence:
        vf.fail(f"{LICENCES / 'LICENSE'} is not the MIT licence")
    for mark in ("GNU Affero General Public License", "github.com/unifolia/CHIMPO-PLUGIN"):
        if mark not in notice_text:
            vf.fail(f"the installed NOTICE.txt does not mention {mark!r}")
    want = {RECEIPT + c: VERSION for c in (*formats, "licenses")}
    if receipts() != want:
        vf.fail(f"receipts {receipts()}, expected {want}")
    print(f"Installed OK: {', '.join(formats)} and the licences, version {VERSION}")


def check_au() -> None:
    va.restart_registrar()
    va.check_registration(BUNDLES["component"][0])
    vf.judge({"auval-installed": lambda: va.auval("auval-installed", [])}, ["auval-installed"])


def uninstall(had_vendor: bool) -> None:
    for path in [*(p for p, _ in BUNDLES.values()), LICENCES]:
        subprocess.run(["sudo", "-n", "rm", "-rf", str(path)])
    if not had_vendor:
        subprocess.run(["sudo", "-n", "rmdir", str(VENDOR)], stderr=subprocess.DEVNULL)
    for receipt in receipts():
        subprocess.run(["sudo", "-n", "pkgutil", "--forget", receipt], stdout=subprocess.DEVNULL)
    va.restart_registrar()


def main() -> None:
    pkgs = sorted((ROOT / "dist").glob("CHIMPO-*-macOS-unsigned.pkg"))
    if len(pkgs) != 1:
        vf.fail("expected one unsigned package in dist: run "
                "`CHIMPO_UNSIGNED=1 bash scripts/package_macos.sh` first")
    present = installed()
    if present:
        vf.fail("CHIMPO is already installed, and this test would replace and then "
                "delete it: " + ", ".join(present) + ". Run it on a disposable machine.")
    if subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode != 0:
        vf.fail("this installs into /Library and needs sudo without a password (`sudo -v` first)")
    had_vendor = VENDOR.exists()
    try:
        check_choices(pkgs[0])
        install(pkgs[0], untick=("component", "vst3"))
        check_installed(("clap",))
        install(pkgs[0])
        check_installed(tuple(BUNDLES))
        check_au()
    finally:
        uninstall(had_vendor)
    left = installed()
    if left:
        vf.fail("left behind: " + ", ".join(left))
    print("Uninstalled cleanly")


if __name__ == "__main__":
    main()
