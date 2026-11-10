#!/bin/bash
# Build the Linux download dist/CHIMPO-<version>-Linux-x86_64.tar.gz: the CLAP
# (with the factory cards beside it), the VST3 bundle, the licence page, the
# MIT licence, the third-party notices and install.sh.
#
# Run on Linux after the CMake build and `python3 scripts/third_party.py`.
# Build on the oldest supported distro (CI uses ubuntu-22.04): the plug-ins
# only load where glibc is at least as new as the build machine's, and this
# script fails if any symbol needs a glibc newer than CHIMPO_MAX_GLIBC
# (default 2.35).
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(uname -s)" = Linux || { echo "Build the Linux package on Linux" >&2; exit 1; }
version=$(sed -n 's/^project(CHIMPO VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
test -n "$version"
test -d target/notices || { echo "Run scripts/third_party.py first" >&2; exit 1; }

artefacts=build/Chimpo_artefacts/Release
clap=$artefacts/CLAP/CHIMPO.clap
clap_resources=$clap.resources
vst3=$artefacts/VST3/CHIMPO.vst3
max_glibc=${CHIMPO_MAX_GLIBC:-2.35}
for binary in "$clap" "$vst3/Contents/x86_64-linux/CHIMPO.so"; do
    test -f "$binary" || { echo "Missing $binary: build on Linux first" >&2; exit 1; }
    needed=$(objdump -T "$binary" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -n 1)
    if [ "$(printf '%s\n%s\n' "$needed" "$max_glibc" | sort -V | tail -n 1)" != "$max_glibc" ]; then
        echo "$binary needs glibc $needed; newest allowed is $max_glibc. Build on an older distro." >&2
        exit 1
    fi
    echo "$binary: needs glibc $needed"
done
test -d "$clap_resources/Factory Cards" || { echo "Missing $clap_resources/Factory Cards" >&2; exit 1; }

name="CHIMPO-$version-Linux-x86_64"
work=target/linux-pkg
stage="$work/$name"
rm -rf "$work"
mkdir -p "$stage" dist
cp "$clap" "$stage/CHIMPO.clap"
cp -R "$clap_resources" "$stage/CHIMPO.clap.resources"
cp -R "$vst3" "$stage/CHIMPO.vst3"
python3 scripts/licence_page.py "$stage/NOTICE.txt"
cp LICENSE THIRD_PARTY.md "$stage/"
cp -R target/notices "$stage/notices"
cp scripts/linux/install.sh "$stage/install.sh"
cat > "$stage/README.txt" <<EOF
CHIMPO $version for Linux x86_64 (CLAP and VST3)

CHIMPO and its firmware are MIT-licensed free software; these plug-ins link
JUCE under the AGPLv3. NOTICE.txt has the details and the source location.

Install for your user (~/.clap and ~/.vst3):
    ./install.sh
Install for every user (/usr/lib/clap and /usr/lib/vst3):
    sudo ./install.sh --system
Install one format only: add --clap-only or --vst3-only, e.g.
    ./install.sh --clap-only
Remove:
    ./install.sh --uninstall        (add --system for a system install;
                                     add --clap-only or --vst3-only to remove one format)

The CLAP needs CHIMPO.clap.resources beside it (the factory SD cards); keep
them together when installing by hand.

Needs glibc $max_glibc or newer (Ubuntu 22.04, Debian 12, Fedora 36 or later).
Licences: LICENSE (MIT), NOTICE.txt, THIRD_PARTY.md and notices/ (open-source
components). install.sh copies them to the doc folder.
EOF
chmod -R u=rwX,go=rX "$stage"
chmod 755 "$stage/install.sh" "$stage/CHIMPO.clap"

# Stable ordering, ownership and timestamps: the same commit gives the same archive.
epoch=${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}
archive="dist/$name.tar.gz"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" \
    -C "$work" -cf - "$name" | gzip -9n > "$archive"
sha256sum "$archive"
