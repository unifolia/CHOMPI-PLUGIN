#!/usr/bin/env python3
"""Measure the DSP cost of each firmware, as a matrix for the release record.

Runs the headless `chompi_render` (the emulator and the firmwares, the same
code the plug-in runs, without a host) on a copy of each factory card and
times a fixed script: boot, a note, and silence. Reports

- the real-time factor: audio seconds rendered per wall-clock second;
- the CPU share a 48 kHz host would need at that factor (1/factor);
- the peak resident memory of the render process.

A factor of 100+ leaves the plug-in a few per cent of a core; below ~20 a
busy project would struggle on that machine. Numbers move with the CPU,
thermal state and other work: run it on an idle machine, and record the model
and OS beside the table.

    python3 scripts/performance_matrix.py [--binary build/chompi_render]
        [--seconds 10] [--json out.json]

The document it feeds is docs/PERFORMANCE.md.
"""
import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARES = ["tape", "wave", "tempo"]
SCRIPT = "wait 500; note 60 110; wait 4000; off 60; wait 5500"


def find_binary(argument: str) -> Path:
    candidates = [argument, str(ROOT / "build" / "chompi_render")]
    candidates += [str(p) for p in sorted(ROOT.glob("build*/chompi_render"))]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    raise SystemExit("chompi_render not found: build with -DCHIMPO_BUILD_TOOLS=ON first")


def measure(binary: Path, seconds: float) -> dict:
    results = {}
    for firmware in FIRMWARES:
        with tempfile.TemporaryDirectory(prefix=f"chimpo-perf-{firmware}-") as tmp:
            card = Path(tmp) / "card"
            shutil.copytree(ROOT / "cards" / firmware, card)
            out = Path(tmp) / "out.wav"
            start = time.perf_counter()
            proc = subprocess.Popen([str(binary), "--firmware", firmware, str(card), str(out), SCRIPT],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            # ru_maxrss for RUSAGE_CHILDREN is the largest any child has used,
            # not this one's: sample the process instead.
            peak = 0
            while proc.poll() is None:
                rss = subprocess.run(["ps", "-o", "rss=", "-p", str(proc.pid)],
                                     capture_output=True, text=True).stdout.strip()
                if rss:
                    peak = max(peak, int(rss) * 1024)
                time.sleep(0.02)
            wall = time.perf_counter() - start
            stdout, stderr = proc.communicate()
            if proc.returncode != 0:
                raise SystemExit(f"{firmware}: chompi_render failed\n{stdout}{stderr}")
            # The wall time covers the rendered audio plus process start-up and
            # card loading.
            audio = seconds
            results[firmware] = {
                "audio_seconds": round(audio, 2),
                "wall_seconds": round(wall, 3),
                "realtime_factor": round(audio / wall, 1),
                "cpu_share_percent": round(100.0 / (audio / wall), 2),
                "peak_rss_bytes": peak,
                "wav_bytes": out.stat().st_size if out.exists() else 0,
            }
            print(f"{firmware}: {audio / wall:.1f}x real time "
                  f"(wall {wall:.2f} s), peak RSS {peak / 1e6:.0f} MB")
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description="Measure each firmware's DSP cost.")
    parser.add_argument("--binary", default="", help="path to chompi_render")
    parser.add_argument("--seconds", type=float, default=10.0, help="audio seconds per firmware")
    parser.add_argument("--json", default="", help="write the results here as JSON")
    args = parser.parse_args()

    binary = find_binary(args.binary)
    results = measure(binary, args.seconds)
    document = {
        "machine": {
            "model": subprocess.run(["sysctl", "-n", "hw.model"], capture_output=True, text=True).stdout.strip()
            if platform.system() == "Darwin" else platform.processor(),
            "os": f"{platform.system()} {platform.release()}",
            "python": platform.python_version(),
        },
        "binary": str(binary.relative_to(ROOT)) if ROOT in binary.parents else str(binary),
        "firmwares": results,
    }
    if args.json:
        Path(args.json).write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {args.json}")
    print("\n| Firmware | Real-time factor | CPU share at 48 kHz | Peak RSS |")
    print("|---|---|---|---|")
    for firmware, r in results.items():
        print(f"| {firmware} | {r['realtime_factor']}x | {r['cpu_share_percent']}% | "
              f"{r['peak_rss_bytes'] / 1e6:.0f} MB |")


if __name__ == "__main__":
    main()
