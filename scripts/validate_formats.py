#!/usr/bin/env python3
"""Run the format validators on the built CHIMPO plug-ins.

- Steinberg's VST3 validator, built from a pinned VST3 SDK, on the VST3
  bundle (extensive tests).
- pluginval (Tracktion), strictness 10 without GUI tests, on the same VST3.
- clap-validator (free-audio) on the CLAP bundle, once CHIMPO ships one.

CHIMPO is an instrument, so the VST3 bypass requirement for effects does not
apply; the SDK validator's own bypass test skips instruments.

pluginval, clap-validator and the VST3 SDK are pinned below. pluginval and
clap-validator downloads must match their SHA-256; the SDK is pinned by commit
and its submodules. Any failure fails the run unless EXCEPTIONS lists it with a
reason. "by design" means CHIMPO behaves as intended. "known bug" means it must
be fixed; CI reports it as a warning on every run.

The AU's validators (auval, pluginval) are in scripts/validate_au.py, which
shares EXCEPTIONS and the helpers here.

Run it after the CMake build (CI runs it on every platform). Bundles are read
from $CHIMPO_BUILD/Chimpo_artefacts/Release (default build/). Tools live in
target/validators; delete that to fetch them again.
Usage: python3 scripts/validate_formats.py [--only vst3-validator,pluginval,clap-validator]
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, NoReturn, Optional

ROOT = Path(__file__).resolve().parent.parent
ARTEFACTS = ROOT / os.environ.get("CHIMPO_BUILD", "build") / "Chimpo_artefacts" / "Release"
VST3 = ARTEFACTS / "VST3" / "CHIMPO.vst3"
AU = ARTEFACTS / "AU" / "CHIMPO.component"
CLAP = ARTEFACTS / "CLAP" / "CHIMPO.clap"
TOOLS = ROOT / "target" / "validators"
SDK = TOOLS / "vst3sdk"
VALIDATOR_BUILD = TOOLS / "vst3sdk-build"
SYSTEM = platform.system()  # Darwin, Windows or Linux
EXE = ".exe" if SYSTEM == "Windows" else ""
PLUGINVAL_STRICTNESS = "10"

# Pinned: the validator's verdict must not change under us between runs.
VST3_SDK = "https://github.com/steinbergmedia/vst3sdk.git"
VST3_SDK_COMMIT = "9fad9770f2ae8542ab1a548a68c1ad1ac690abe0"
VST3_SDK_SUBMODULES = ["base", "pluginterfaces", "public.sdk", "cmake"]

PLUGINVAL = "https://github.com/Tracktion/pluginval/releases/download/v1.0.4/"
CLAP_VALIDATOR = (
    "https://github.com/free-audio/clap-validator/releases/download/0.4.1/"
    "clap-validator-0.4.1-127-g152b982-"
)
DOWNLOADS = {
    "pluginval": {
        "Darwin": (PLUGINVAL + "pluginval_macOS.zip",
                   "3c4c533bda0c5059eea3ddaea752d757ee2025041f0f47e6bcb0e87f6082b29f"),
        "Windows": (PLUGINVAL + "pluginval_Windows.zip",
                    "c08e61ce3b96db41636f8ec7e76f4c7e2c13ebdac7fa1b5a1f52b4f32ec715ab"),
        "Linux": (PLUGINVAL + "pluginval_Linux.zip",
                  "c01c49d8063965c4c2dea8324468336768f5c9139e0b1caebde14c2400b55352"),
    },
    "clap-validator": {
        "Darwin": (CLAP_VALIDATOR + "macos-universal.zip",
                   "bbec8cd7d18274e549d5d8c12ece3cec54be966129388dd2e742b9957f2ba9f1"),
        "Windows": (CLAP_VALIDATOR + "windows.zip",
                    "d935c3af0a45c3911ea2e900f4aa5d6709dac82bb485f0c4ce28648ab2cd0c10"),
        "Linux": (CLAP_VALIDATOR + "ubuntu-22.04.zip",
                  "49edadcfb407ea0dd946ce418300e853fbd2660fa4b0d00e4f19ff8eef24ad90"),
    },
}

BY_DESIGN, KNOWN_BUG = "by design", "known bug"


@dataclass
class Failure:
    tool: str
    test: str
    details: str


@dataclass
class Allowed:
    tool: str
    test: str  # a regular expression matched against the whole test name
    kind: str
    reason: str
    matches: Callable[[Failure], bool] = lambda f: True


EXCEPTIONS: list = [
    # --- Real-time safety (auval-rt) -------------------------------------
    # Apple's AudioUnitSDK locks a std::recursive_mutex in AUBase::DoRender
    # (RenderCallbackList::foreach) on every render while the host has a render
    # notification registered, as in every AU built on it.
    Allowed("auval-rt", re.escape("pthread_mutex_lock from ausdk::AUMethodRender"), KNOWN_BUG,
            "Apple's AudioUnitSDK locks a recursive_mutex in AUBase::DoRender on every "
            "render. Every AU built on it does this. To be reported to Apple"),
    # JUCE's AUv2 wrapper takes incomingMidiLock (a CriticalSection) in
    # JuceAU::Render around its MIDI handling, as every JUCE-built AU does.
    Allowed("auval-rt", re.escape("pthread_mutex_lock from JuceAU::Render"), KNOWN_BUG,
            "JUCE's AUv2 wrapper locks incomingMidiLock in JuceAU::Render around MIDI "
            "delivery. Every JUCE AU does this; an upstream JUCE fix is needed"),
    # The emulated SD card used to keep a std::mutex over every file operation;
    # it now uses a spinlock (emu::SpinLock) with the flusher waiting on its own
    # std::mutex, so the firmware's thread takes no pthread mutex. Removing that
    # lock's exceptions is what proves it: if auval-rt reports VirtualCard or
    # renderBlock again, CI fails.
    Allowed("auval-rt", re.escape("free from ChompiProcessor::processBlock"), KNOWN_BUG,
            "A free on the render thread inside processBlock. Not reproduced by "
            "tests/plugin_test.cpp (its allocation counter hooks operator "
            "new/delete; a free interposer on the same path saw none either), so "
            "it is tied to host-side parameter traffic. To be located and removed"),
    # CHOMPI's released firmware calls sprintf on the audio thread (its SD-card
    # file bookkeeping), and libc's printf frees internally. The firmware stays
    # as released (CLAUDE.MD invariant), so this cannot be fixed in this repo.
    Allowed("auval-rt", re.escape("free from daisy_tape::") + r"[\w:]+", KNOWN_BUG,
            "CHOMPI's released firmware formats file names with sprintf on the audio "
            "thread, and libc's printf frees internally. The firmware stays as "
            "released; a CHOMPI firmware fix would be needed upstream"),
]


def fail(message: str) -> NoReturn:
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(cmd: list, timeout: int = 1800, cwd: Optional[Path] = None) -> str:
    print("$", " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=timeout, cwd=cwd)
    return result.stdout + result.stderr


def fetch(tool: str) -> Path:
    """Download, check and unpack a pinned tool; return its executable."""
    url, want = DOWNLOADS[tool][SYSTEM]
    home = TOOLS / tool
    archive = TOOLS / url.rsplit("/", 1)[1]
    if not archive.exists() or sha256(archive) != want:
        TOOLS.mkdir(parents=True, exist_ok=True)
        # curl, not urllib: some Python installs lack a CA bundle.
        subprocess.run(["curl", "-fsSL", "--retry", "3", "-o", str(archive), url], check=True)
    got = sha256(archive)
    if got != want:
        fail(f"{archive.name}: SHA-256 {got}, expected {want}")
    shutil.rmtree(home, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(home)
    for inner in list(home.rglob("*.tar.gz")):
        with tarfile.open(inner) as t:
            try:
                t.extractall(home, filter="data")
            except TypeError:  # Python without extraction filters
                t.extractall(home)
    name = "pluginval" if tool == "pluginval" else "clap-validator"
    for exe in home.rglob(name + EXE):
        if exe.is_file():
            exe.chmod(0o755)  # zipfile drops the executable bit
            return exe
    fail(f"no {name}{EXE} in {archive.name}")


def sdk() -> Path:
    """Clone the pinned VST3 SDK (with the submodules the validator needs)."""
    if not (SDK / "CMakeLists.txt").exists():
        shutil.rmtree(SDK, ignore_errors=True)
        run(["git", "clone", "--filter=blob:none", "--no-checkout", VST3_SDK, SDK], timeout=1800)
        run(["git", "-C", SDK, "checkout", VST3_SDK_COMMIT])
        run(["git", "-C", SDK, "submodule", "update", "--init", "--depth", "1",
             *VST3_SDK_SUBMODULES], timeout=1800)
    return SDK


def vst3_validator() -> Path:
    exe = VALIDATOR_BUILD / "bin" / "Release" / ("validator" + EXE)
    if exe.exists():
        return exe
    source = sdk()
    env = dict(os.environ)
    args = ["cmake", "-S", str(source), "-B", str(VALIDATOR_BUILD), "-DCMAKE_BUILD_TYPE=Release",
            "-DSMTG_ENABLE_VSTGUI_SUPPORT=OFF", "-DSMTG_ENABLE_VST3_PLUGIN_EXAMPLES=OFF",
            "-DSMTG_ENABLE_VST3_HOSTING_EXAMPLES=OFF", "-DSMTG_RUN_VST_VALIDATOR=OFF",
            "-DSMTG_CREATE_PLUGIN_LINK=OFF"]
    if SYSTEM == "Darwin" and subprocess.run(["xcodebuild", "-version"],
                                             capture_output=True).returncode != 0:
        # Command Line Tools only: the SDK asks xcodebuild for a version.
        env["XCODE_VERSION"] = "16"
        args += ["-G", "Unix Makefiles", "-DXCODE_VERSION=16"]
    subprocess.run(args, check=True, env=env)
    subprocess.run(["cmake", "--build", str(VALIDATOR_BUILD), "--config", "Release",
                    "--target", "validator", "--parallel", "4"], check=True, env=env)
    if not exe.exists():
        fail(f"the SDK build did not produce {exe}")
    return exe


def check_vst3_validator() -> list:
    out = run([vst3_validator(), "-e", VST3])
    (TOOLS / "vst3-validator.log").write_text(out, encoding="utf-8")
    summary = re.search(r"Result: \d+ tests passed, \d+ tests failed", out)
    if not summary:
        return [Failure("vst3-validator", "(run)", out[-2000:])]
    print(f"vst3-validator: {summary.group(0)}")
    failures, test, block = [], "", []
    for line in out.splitlines():
        header = re.fullmatch(r"\[(.+)\]", line.strip())
        if header and header.group(1) == "XXXXXXX Failed":
            failures.append(Failure("vst3-validator", test, "\n".join(block)))
        elif header and header.group(1) != "Succeeded":
            test, block = header.group(1), []
        else:
            block.append(line)
    return failures


def check_pluginval(plugin: Path = VST3, tool: str = "pluginval") -> list:
    """pluginval on one plug-in; scripts/validate_au.py runs it on the AU as pluginval-au."""
    logs = TOOLS / f"{tool}-logs"
    shutil.rmtree(logs, ignore_errors=True)
    logs.mkdir(parents=True)
    cmd = [fetch("pluginval"), "--strictness-level", PLUGINVAL_STRICTNESS, "--skip-gui-tests",
           "--timeout-ms", "300000", "--output-dir", logs, "--validate", plugin]
    print("$", " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=3600)
    log = "\n".join(p.read_text(encoding="utf-8", errors="replace")
                    for p in sorted(logs.glob("*")) if p.is_file())
    out = (result.stdout + result.stderr).strip() or log
    (TOOLS / f"{tool}.log").write_text(out, encoding="utf-8")
    groups = out.count("Completed tests in")
    print(f"{tool}: exit {result.returncode}, {groups} test groups completed")
    if result.returncode == 0 and "FAILED" not in out:
        if groups == 0:
            return [Failure(tool, "(run)", "pluginval ran no tests\n" + out[-2000:])]
        return []
    failed = re.findall(r"Starting tests in: pluginval / (.+?)\\.\\.\\.\n(?:(?!Starting tests in).)*?"
                        r"!!! Test \d+ failed", out, re.S)
    started = re.findall(r"Starting tests in: pluginval / (.+?)\\.\\.\\.", out)
    if not failed and started and "exiting immediately" in out:  # the plug-in crashed
        return [Failure(tool, started[-1], "crashed\n" + out[-3000:])]
    return [Failure(tool, t, out[-3000:]) for t in failed] or [
        Failure(tool, "(run)", f"exit {result.returncode}\n{out[-3000:]}")]


def check_clap_validator() -> list:
    cmd = [fetch("clap-validator"), "validate", "--json", "--hide-output", CLAP]
    print("$", " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                            encoding="utf-8", errors="replace", timeout=1800)
    out = result.stdout  # the JSON; progress logging goes to stderr
    (TOOLS / "clap-validator.json").write_text(out, encoding="utf-8")
    (TOOLS / "clap-validator.log").write_text(result.stderr, encoding="utf-8")
    try:
        results = json.loads(out[out.find("{"):])["results"]
    except (ValueError, KeyError):
        return [Failure("clap-validator", "(run)", (out + result.stderr)[-2000:])]
    summary = {}
    for r in results:
        summary[r["status"]["code"]] = summary.get(r["status"]["code"], 0) + 1
    print(f"clap-validator: {len(results)} tests, "
          + ", ".join(f"{n} {code}" for code, n in sorted(summary.items())))
    if not results:
        return [Failure("clap-validator", "(run)", "clap-validator ran no tests")]
    failures = []
    for r in results:
        code = r["status"]["code"]
        if code in ("success", "skipped"):
            continue
        test = next(iter(r["test"].values()))["test"]
        failures.append(Failure("clap-validator", test,
                                f"{code}: {r['status'].get('details') or ''}"))
    return failures


def allowed(f: Failure) -> Optional[Allowed]:
    for a in EXCEPTIONS:
        if a.tool == f.tool and re.fullmatch(a.test, f.test) and a.matches(f):
            return a
    return None


def choose(checks: dict, description: str) -> list:
    """The checks named by --only (default: all of them)."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--only", default=",".join(checks),
                        help="comma-separated subset of " + ", ".join(checks))
    chosen = parser.parse_args().only.split(",")
    for name in chosen:
        if name not in checks:
            fail(f"unknown validator {name!r}")
    return chosen


def judge(checks: dict, chosen: list) -> None:
    """Run the chosen checks; exit with an error on any failure EXCEPTIONS doesn't allow."""
    TOOLS.mkdir(parents=True, exist_ok=True)
    in_ci = os.environ.get("GITHUB_ACTIONS") == "true"
    unexpected = []
    for name in chosen:
        failures = checks[name]()
        print(f"\n{name}: {len(failures)} failure(s)")
        for f in failures:
            a = allowed(f)
            if a is None:
                unexpected.append(f)
                print(f"  FAIL  {f.test}\n" + "\n".join("        " + l for l in f.details.splitlines()[-20:]))
            elif a.kind == KNOWN_BUG:
                print(f"  known bug  {f.test}: {a.reason}")
                if in_ci:
                    print(f"::warning title={name}: {f.test} (known bug)::{a.reason}")
            else:
                print(f"  by design  {f.test}: {a.reason}")
    if unexpected:
        fail(f"{len(unexpected)} unexpected validator failure(s); logs in {TOOLS}")
    print("\nAll validators passed (with the listed exceptions).")


def main() -> None:
    checks = {"vst3-validator": check_vst3_validator, "pluginval": check_pluginval}
    if CLAP.exists():  # only once CHIMPO ships a CLAP build
        checks["clap-validator"] = check_clap_validator
    chosen = choose(checks, "Run the format validators on the built plug-ins.")
    if not VST3.exists():
        fail(f"{VST3} is missing: build the plug-in first (cmake --build)")
    judge(checks, chosen)


if __name__ == "__main__":
    main()
