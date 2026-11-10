#!/usr/bin/env python3
"""Validate the built Audio Unit (macOS).

Installs the AU bundle in ~/Library/Audio/Plug-Ins/Components for the run,
killing AudioComponentRegistrar so the system sees it, and restores any prior
installation afterwards. Don't run it while a host has CHIMPO loaded. It stops
if another CHIMPO AU is installed elsewhere: the system would load whichever
has the highest version, not necessarily the build. It runs:

- auval: Apple's validator, strict, three passes, each ending in its
  multi-thread stress test (600 s of audio);
- auval-oop: strict, with the AU loaded out of process;
- auval-rt: strict, with auval's real-time-safety test, which watches the
  render thread with dtrace for calls that could block. auval starts dtrace
  through sudo and waits for it forever, so this runs only where sudo needs
  no password: CI requires it, elsewhere it is skipped;
- pluginval-au: pluginval as validate_formats.py runs it on the VST3.

auval exits 0 after a warning; here any WARNING fails like an error. Failures
go through validate_formats.EXCEPTIONS, and the same rules apply to them.
Logs are in target/validators. Pass counts and the stress length can be
lowered for a local run: CHIMPO_AUVAL_PASSES=1 CHIMPO_AUVAL_STRESS=60.
Usage: python3 scripts/validate_au.py [--only auval,auval-oop,auval-rt,pluginval-au]
"""
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import validate_formats as vf
from validate_formats import Failure

SOURCE = vf.AU
COMPONENTS = Path.home() / "Library" / "Audio" / "Plug-Ins" / "Components"
TARGET = COMPONENTS / "CHIMPO.component"
AUVAL = ["auval", "-v", "aumu", "Tap2", "Cdaw", "-strict"]
PASSES = int(os.environ.get("CHIMPO_AUVAL_PASSES", "3"))
STRESS_SECONDS = os.environ.get("CHIMPO_AUVAL_STRESS", "600")
BINARY = "CHIMPO"


def restart_registrar() -> None:
    subprocess.run(["killall", "-9", "AudioComponentRegistrar"], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


REGISTRATION_WAIT_S = 30


def check_registration(target: Path = TARGET) -> None:
    """The installed build must be the only CHIMPO AU the system knows: of
    several, hosts and validators get the highest version, wherever it is.
    Waits for the restarted registrar, which is slow on a fresh machine."""
    deadline = time.monotonic() + REGISTRATION_WAIT_S
    while True:
        listing = subprocess.run(["auval", "-l"], capture_output=True, text=True,
                                 encoding="utf-8", errors="replace").stdout
        found = [Path(url.removeprefix("file://").rstrip("/"))
                 for url in re.findall(r"^aumu Tap2 Cdaw .*\((file://[^)]*)\)", listing, re.M)]
        others = [str(p) for p in found if p != target]
        if others:
            vf.fail("another CHIMPO AU is installed, so this run would not test the build: "
                    + ", ".join(others) + ". Move it aside for the run.")
        if target in found:
            return
        if time.monotonic() > deadline:
            listed = [line for line in listing.splitlines() if "Tap2" in line] or ["no CHIMPO AU"]
            signature = subprocess.run(["codesign", "--verify", "--deep", str(target)],
                                       capture_output=True, text=True, errors="replace")
            vf.fail(f"the system did not register {target} within {REGISTRATION_WAIT_S} s.\n"
                    "auval -l lists: " + "; ".join(listed) + "\ncodesign --verify: "
                    + (signature.stderr.strip() or "valid"))
        time.sleep(1)


FRAME = re.compile(r"\s*([^`\s]+)`(.+?)(?:\+0x[0-9a-f]+)?\s*$")
SYSTEM = re.compile(r"(libsystem_|libc\+\+|libdyld|dyld$|AudioToolbox)")


def demangle(symbols: list) -> dict:
    """Names as dtrace prints them, demangled with c++filt (Mach-O prefixes _)."""
    wanted = [s for s in symbols if s.startswith("_Z")]
    if not wanted:
        return {}
    out = subprocess.run(["c++filt"], input="\n".join(s[1:] for s in wanted), text=True,
                         capture_output=True).stdout.splitlines()
    return {s: name for s, name in zip(wanted, out)}


def rt_violations(tool: str, out: str) -> list:
    """One failure per distinct stack auval's real-time-safety test reports,
    named after the blocking call and the first frame outside the system
    libraries. A stack with no frame in the plug-in is auval's own (its logging
    on the watched thread): printed, not a failure."""
    stacks = []
    for block in re.findall(r"Realtime-safety violation:\n((?:[ \t]+\S.*\n?)+)", out):
        stack = []
        for line in block.splitlines():
            match = FRAME.match(line)
            if match:
                stack.append(match.groups())
        stacks.append(stack)
    symbols = sorted({s for stack in stacks for _, s in stack if s.startswith("_Z")})
    names = demangle(symbols)
    found, auvals_own = {}, 0
    for stack in stacks:
        frames = [(module.removesuffix(".dylib"), names.get(s, s)) for module, s in stack]
        if not frames:
            continue
        if all(module != BINARY for module, _ in frames):
            auvals_own += 1
            continue
        origin = next((f for f in frames if not SYSTEM.match(f[0])), frames[-1])
        where = re.sub(r"\(.*\)$", "", origin[1]) if origin[0] == BINARY else origin[0]
        test = f"{frames[0][1]} from {where}"
        count, _ = found.get(test, (0, None))
        found[test] = (count + 1, "\n".join(f"{m}`{s}" for m, s in frames))
    if auvals_own:
        print(f"{tool}: ignored {auvals_own} violation(s) with no frame in the plug-in (auval's own)")
    return [Failure(tool, test, f"{count} time(s):\n{stack}")
            for test, (count, stack) in found.items()]


def auval(tool: str, args: list, passes: int = 1, timeout: int = 1800) -> list:
    cmd = AUVAL + args
    print("$", " ".join(cmd), flush=True)
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                                errors="replace", timeout=timeout)
        code, out = str(result.returncode), result.stdout + result.stderr
    except subprocess.TimeoutExpired as e:  # its output is bytes whatever text= says
        code = f"none: killed after {timeout} s"
        out = b"".join(s for s in (e.stdout, e.stderr) if s).decode("utf-8", "replace")
    (vf.TOOLS / f"{tool}.log").write_text(out, encoding="utf-8")
    lines = out.splitlines()
    warnings = {}
    for i, line in enumerate(lines):
        if "WARNING" in line:
            warnings.setdefault(line.strip(), "\n".join(lines[max(0, i - 4):i + 1]))
    succeeded = out.count("AU VALIDATION SUCCEEDED")
    violations = rt_violations(tool, out)
    print(f"{tool}: exit {code}, {succeeded}/{passes} passes succeeded, {len(warnings)} warning(s)"
          + (f", {len(violations)} distinct real-time violation(s)" if violations else ""))
    failures = [Failure(tool, w, context) for w, context in warnings.items()] + violations
    # When the real-time test is the only section that failed, the violations
    # above are the whole failure.
    failed_sections = len(re.findall(r"^\s*\* \* FAIL", out, re.M))
    only_violations = (violations and failed_sections == 1
                       and "Real-time Safe Test FAILED" in out)
    if (code != "0" or succeeded != passes) and not only_violations:
        failures.append(Failure(tool, "(run)", f"exit {code}\n{out[-3000:]}"))
    return failures


def check_auval() -> list:
    return auval("auval", ["-r", str(PASSES), "-stress", STRESS_SECONDS], passes=PASSES)


def check_auval_oop() -> list:
    return auval("auval-oop", ["-oop"])


def check_auval_rt() -> list:
    if subprocess.run(["sudo", "-n", "true"], capture_output=True).returncode != 0:
        if os.environ.get("GITHUB_ACTIONS") == "true":
            return [Failure("auval-rt", "(run)", "sudo asks for a password, so auval can't start dtrace")]
        print("auval-rt: skipped: sudo asks for a password here (CI runs it)")
        return []
    return auval("auval-rt", ["-real-time-safety"], timeout=600)


def check_pluginval_au() -> list:
    return vf.check_pluginval(TARGET, "pluginval-au")


def main() -> None:
    checks = {"auval": check_auval, "auval-oop": check_auval_oop,
              "auval-rt": check_auval_rt, "pluginval-au": check_pluginval_au}
    chosen = vf.choose(checks, "Validate the built Audio Unit (macOS).")
    if not SOURCE.is_dir():
        vf.fail(f"{SOURCE} is missing: build the Audio Unit first")
    if "pluginval-au" in chosen:
        vf.fetch("pluginval")  # before installing, to keep the install window short
    COMPONENTS.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="chimpo-auval-", dir=COMPONENTS.parent) as staging:
        backup = Path(staging) / "previous.component"
        previous = TARGET.exists()
        if previous:
            TARGET.rename(backup)
        try:
            shutil.copytree(SOURCE, TARGET)
            restart_registrar()
            check_registration()
            vf.judge(checks, chosen)
        finally:
            if TARGET.exists():
                shutil.rmtree(TARGET)
            if previous:
                backup.rename(TARGET)
            restart_registrar()


if __name__ == "__main__":
    main()
