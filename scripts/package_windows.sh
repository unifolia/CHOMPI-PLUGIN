#!/bin/bash
# Build the unsigned Windows installer dist/CHIMPO-<version>-Windows-Setup.exe
# with Inno Setup 6.
#
# Run in Git Bash after the CMake build on Windows and
# `python3 scripts/third_party.py`. Set ISCC to the compiler's path if Inno
# Setup is not in its default location. Signing is a separate, earlier step
# for the plug-ins (scripts/sign_windows.ps1) and part of this build for the
# installer when CHIMPO_SIGN=1.
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(sed -n 's/^project(CHIMPO VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
test -n "$version"
test -d target/notices || { echo "Run scripts/third_party.py first" >&2; exit 1; }
artefacts=build/Chimpo_artefacts/Release
for f in "$artefacts/VST3/CHIMPO.vst3/Contents/x86_64-win/CHIMPO.vst3" \
    "$artefacts/CLAP/CHIMPO.clap"; do
    test -f "$f" || { echo "Missing $f: build on Windows first" >&2; exit 1; }
done
test -d "$artefacts/CLAP/CHIMPO.clap.resources/Factory Cards" ||
    { echo "Missing the CLAP's factory cards: build on Windows first" >&2; exit 1; }

iscc=${ISCC:-}
if [ -z "$iscc" ]; then
    for candidate in "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" "/c/Program Files/Inno Setup 6/ISCC.exe"; do
        if [ -f "$candidate" ]; then iscc=$candidate; break; fi
    done
fi
test -n "$iscc" || { echo "Inno Setup 6 not found: install it or set ISCC" >&2; exit 1; }

# The licence page and the installed notice, CRLF and UTF-8 with a BOM (the
# only way Inno Setup reads a .txt licence as UTF-8). The page has one line
# per paragraph, which the page wraps to its own width.
python3 scripts/licence_page.py --crlf --bom target/windows-licences/NOTICE.txt
python3 scripts/licence_page.py --crlf --bom --width 0 target/windows-licences/licence-page.txt

setup="dist/CHIMPO-$version-Windows-Setup.exe"
rm -f "$setup"
args=(/Q "/DAppVersion=$version")
if [ "${CHIMPO_SIGN:-0}" = 1 ]; then
    # The plug-ins must already be signed (CI signs them before packaging).
    for f in "$artefacts/CLAP/CHIMPO.clap" "$artefacts/VST3/CHIMPO.vst3/Contents/x86_64-win/CHIMPO.vst3"; do
        pwsh -NoProfile -Command "if ((Get-AuthenticodeSignature -LiteralPath '$f').Status -ne 'Valid') { exit 1 }" ||
            { echo "$f is not signed" >&2; exit 1; }
    done
    signer=$(cygpath -w "$PWD/scripts/sign_windows.ps1")
    # Inno replaces $q with a quote and $f with the quoted file to sign.
    args+=(/DSign "/Sazure=pwsh -NoProfile -File \$q$signer\$q \$f")
fi
# Git Bash would rewrite /Q and /D... as paths.
MSYS_NO_PATHCONV=1 "$iscc" "${args[@]}" scripts/chimpo.iss
test -f "$setup"
sha256sum "$setup"
