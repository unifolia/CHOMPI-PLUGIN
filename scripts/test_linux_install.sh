#!/bin/bash
# Unpack the built Linux archive and exercise install.sh: per-user install,
# reinstall over it, uninstall, format choice, and (when passwordless sudo is
# available, as on CI) a system install and uninstall. Checks the installed
# payload is byte-identical to the build outputs, that the CLAP's factory-card
# folder went with it, and that the licences shipped are the MIT licence and
# the MIT + AGPL notice.
set -euo pipefail
cd "$(dirname "$0")/.."
shopt -s nullglob
archives=(dist/CHIMPO-*-Linux-x86_64.tar.gz)
test ${#archives[@]} = 1 || { echo "Expected one Linux archive in dist" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
tar -xzf "${archives[0]}" -C "$tmp"
pkg=$(echo "$tmp"/CHIMPO-*)
test -x "$pkg/install.sh"

same() { cmp -s "$1" "$2" || { echo "$1 differs from $2" >&2; exit 1; }; }
# CHIMPO's own licence is the MIT text; the notice says the builds link JUCE
# under the AGPLv3 and where the source is.
python3 scripts/licence_page.py "$tmp/NOTICE.expected"
check_licences() {  # dir
    same "$1/LICENSE" LICENSE
    same "$1/NOTICE.txt" "$tmp/NOTICE.expected"
    grep -q 'MIT License' "$1/LICENSE"
    # The installed notice is wrapped, so phrases can cross a line break.
    notice=$(tr '\n' ' ' < "$1/NOTICE.txt")
    printf '%s' "$notice" | grep -q 'GNU Affero General Public License'
    printf '%s' "$notice" | grep -q 'github.com/unifolia/CHOMPI-PLUGIN' 
    test -f "$1/THIRD_PARTY.md"
    test -n "$(ls -A "$1/notices")"
    test -f "$1/notices/AGPL-3.0.txt"
    test -f "$1/notices/JUCE-LICENSE.md"
}
check_licences "$pkg"
check_installed() {  # clap-dir vst3-dir doc-dir
    same "$1/CHIMPO.clap" build/Chimpo_artefacts/Release/CLAP/CHIMPO.clap
    test -d "$1/CHIMPO.clap.resources/Factory Cards"
    same "$2/CHIMPO.vst3/Contents/x86_64-linux/CHIMPO.so" \
        build/Chimpo_artefacts/Release/VST3/CHIMPO.vst3/Contents/x86_64-linux/CHIMPO.so
    check_licences "$3"
}
check_removed() {
    for path in "$@"; do
        if [ -e "$path" ]; then echo "Left behind: $path" >&2; exit 1; fi
    done
}

home="$tmp/home"
mkdir -p "$home"
export HOME="$home"
unset XDG_DATA_HOME
"$pkg/install.sh"
"$pkg/install.sh"  # reinstalling over an existing install must work
check_installed "$home/.clap" "$home/.vst3" "$home/.local/share/doc/chimpo"
mkdir -p "$home/Music/CHIMPO"  # cards CHIMPO already copied must survive
"$pkg/install.sh" --uninstall
check_removed "$home/.clap/CHIMPO.clap" "$home/.clap/CHIMPO.clap.resources" \
    "$home/.vst3/CHIMPO.vst3" "$home/.local/share/doc/chimpo"
test -d "$home/Music/CHIMPO"
if "$pkg/install.sh" --bogus 2>/dev/null; then echo "Accepted an unknown option" >&2; exit 1; fi
echo "Per-user install OK"

# Format choice: install one format, add the other, then remove one.
"$pkg/install.sh" --clap-only
same "$home/.clap/CHIMPO.clap" build/Chimpo_artefacts/Release/CLAP/CHIMPO.clap
check_removed "$home/.vst3/CHIMPO.vst3"
check_licences "$home/.local/share/doc/chimpo"
"$pkg/install.sh" --vst3-only
check_installed "$home/.clap" "$home/.vst3" "$home/.local/share/doc/chimpo"
"$pkg/install.sh" --uninstall --clap-only
check_removed "$home/.clap/CHIMPO.clap" "$home/.clap/CHIMPO.clap.resources"
test -f "$home/.vst3/CHIMPO.vst3/Contents/x86_64-linux/CHIMPO.so"
check_licences "$home/.local/share/doc/chimpo"  # kept while a format remains
"$pkg/install.sh" --uninstall
check_removed "$home/.clap/CHIMPO.clap" "$home/.vst3/CHIMPO.vst3" "$home/.local/share/doc/chimpo"
if "$pkg/install.sh" --clap-only --vst3-only 2>/dev/null; then echo "Accepted both -only options" >&2; exit 1; fi
echo "Format choice OK"

if sudo -n true 2>/dev/null; then
    sudo "$pkg/install.sh" --system
    check_installed /usr/lib/clap /usr/lib/vst3 /usr/share/doc/chimpo
    if sudo "$pkg/install.sh" 2>/dev/null; then echo "Root per-user install was not refused" >&2; exit 1; fi
    sudo "$pkg/install.sh" --system --uninstall
    check_removed /usr/lib/clap/CHIMPO.clap /usr/lib/vst3/CHIMPO.vst3 /usr/share/doc/chimpo
    echo "System install OK"
else
    echo "No passwordless sudo: system install not tested"
fi
