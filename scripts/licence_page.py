#!/usr/bin/env python3
"""Write the licence page the installers show, and the copy they install.

CHIMPO has no EULA: it is MIT, and the plug-ins link JUCE under the AGPLv3.
The page says both, points at the corresponding source, and carries the MIT
warranty disclaimer. Installers show one line per paragraph (Installer and
Inno Setup wrap it to their own width); the installed copy is ordinary prose.

    python3 scripts/licence_page.py <out.txt> [--width 0] [--crlf] [--bom]

--width 0 joins each paragraph into one line (what the licence screens take);
the default wraps at 80 columns for the installed copy. --crlf and --bom are
for Inno Setup, which reads a .txt licence as UTF-8 only with a BOM.
"""
import argparse
import re
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = "https://github.com/unifolia/CHOMPI-PLUGIN"


def version() -> str:
    match = re.search(r"^project\(CHIMPO VERSION ([0-9.]+)", (ROOT / "CMakeLists.txt").read_text(), re.M)
    if match is None:
        raise SystemExit("project(CHIMPO VERSION x.y.z) not found in CMakeLists.txt")
    return match.group(1)


def paragraphs() -> list:
    v = version()
    return [
        f"CHIMPO {v}",
        "Copyright (c) 2026 CHOMPI Club. Distributed by Bellcast Engineering.",
        "CHIMPO and its firmware are free software under the MIT License: you may use, copy, "
        "modify, merge, publish, distribute, sublicense and sell copies, provided the copyright "
        "notice and this permission notice are included. The licence is installed as LICENSE "
        f"and is at {REPOSITORY}/blob/main/LICENSE.",
        "These plug-ins link JUCE 9, which is licensed under the GNU Affero General Public "
        "License version 3. The complete corresponding source for these builds, including the "
        f"pinned JUCE revision and the build scripts, is published at {REPOSITORY}. The AGPLv3 "
        "text is installed as notices/AGPL-3.0.txt, beside the third-party notices in "
        "THIRD_PARTY_NOTICES.md and notices/.",
        "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
        "INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A "
        "PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT "
        "HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF "
        "CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE "
        "OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.",
        "CHOMPI is a product of CHOMPI Club. CHIMPO is an independent project, not affiliated "
        "with or endorsed by CHOMPI Club or Chase Bliss. See TRADEMARKS.md.",
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description="Write the licence page and the installed copy.")
    parser.add_argument("out")
    parser.add_argument("--width", type=int, default=80)
    parser.add_argument("--crlf", action="store_true")
    parser.add_argument("--bom", action="store_true")
    args = parser.parse_args()

    lines = []
    for paragraph in paragraphs():
        if args.width > 0:
            lines += textwrap.wrap(paragraph, width=args.width, break_long_words=False, break_on_hyphens=False)
        else:
            lines.append(" ".join(paragraph.split()))
        lines.append("")
    text = "\n".join(lines).rstrip() + "\n"
    if args.crlf:
        text = text.replace("\n", "\r\n")
    data = text.encode("utf-8")
    if args.bom:
        data = b"\xef\xbb\xbf" + data
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_bytes(data)
    print(f"{args.out}: {len(data)} bytes")


if __name__ == "__main__":
    main()
