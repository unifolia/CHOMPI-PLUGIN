#!/bin/bash
# Build the signed, notarized, stapled macOS installer
# dist/CHIMPO-<version>-macOS.pkg from build/Chimpo_artefacts/Release and
# target/notices.
#
# For a release, those are the tag run's validated universal build, unpacked
# from its `chimpo-macos-universal` artifact. Otherwise run after the CMake
# build and `python3 scripts/third_party.py`. Needs the owner's keychain
# identities; creates no credentials and publishes nothing.
#
# CHIMPO_UNSIGNED=1 instead builds dist/CHIMPO-<version>-macOS-unsigned.pkg for
# CI's install test (scripts/test_macos_install.py): the same package, but its
# bundles keep the build's ad-hoc signatures, the package is unsigned and
# nothing is notarized. Never ship it.
set -euo pipefail
unsigned=${CHIMPO_UNSIGNED:-0}
if [ "$unsigned" != 1 ]; then
    : "${CHIMPO_SIGN_IDENTITY:?Set a Developer ID Application identity}"
    : "${CHIMPO_INSTALLER_IDENTITY:?Set a Developer ID Installer identity}"
    : "${CHIMPO_NOTARY_PROFILE:?Set an existing notarytool keychain profile}"
fi

cd "$(dirname "$0")/.."
version=$(sed -n 's/^project(CHIMPO VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
test -n "$version"
test -d target/notices || { echo "Run scripts/third_party.py first" >&2; exit 1; }

artefacts=build/Chimpo_artefacts/Release
work=target/pkg
rm -rf "$work"
mkdir -p "$work/payload" "$work/components" "$work/resources" dist
pkg="dist/CHIMPO-$version-macOS.pkg"
if [ "$unsigned" = 1 ]; then pkg="dist/CHIMPO-$version-macOS-unsigned.pkg"; fi
rm -f "$pkg"

# Payload copies, so signing never touches the build tree. Drop extended
# attributes: pkgbuild would otherwise ship them as ._* AppleDouble files
# beside and inside the signed bundles.
copy() { ditto --norsrc --noextattr --noqtn --noacl "$1" "$2"; }
copy "$artefacts/CLAP/CHIMPO.clap" "$work/payload/clap/CHIMPO.clap"
copy "$artefacts/VST3/CHIMPO.vst3" "$work/payload/vst3/CHIMPO.vst3"
copy "$artefacts/AU/CHIMPO.component" "$work/payload/component/CHIMPO.component"
# Licences: the MIT licence, the MIT + AGPL notice, the third-party notices
# and their texts.
python3 scripts/licence_page.py "$work/payload/licenses/NOTICE.txt"
copy LICENSE "$work/payload/licenses/LICENSE"
copy THIRD_PARTY.md "$work/payload/licenses/THIRD_PARTY.md"
copy target/notices "$work/payload/licenses/notices"
xattr -cr "$work/payload"
export COPYFILE_DISABLE=1

p="$work/payload"
# One installer serves Apple silicon and Intel: every binary must be universal
# (-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"). CHIMPO_ALLOW_THIN=1 permits a
# single-architecture test package, offered only where its binaries run.
archs=$(lipo -archs "$p/clap/CHIMPO.clap/Contents/MacOS/CHIMPO" | tr ' ' ',')
while IFS= read -r binary; do
    found=$(lipo -archs "$binary" | tr ' ' '\n' | sort | tr '\n' ' ')
    if [ "$found" != "arm64 x86_64 " ] && [ "${CHIMPO_ALLOW_THIN:-0}" != 1 ]; then
        echo "$binary is not universal (has: $found)" >&2
        exit 1
    fi
done < <(find "$p" -path '*/Contents/MacOS/*' -type f)

if [ "$unsigned" != 1 ]; then
    for bundle in "$p/clap/CHIMPO.clap" "$p/vst3/CHIMPO.vst3" "$p/component/CHIMPO.component"; do
        codesign --force --options runtime --timestamp --sign "$CHIMPO_SIGN_IDENTITY" "$bundle"
    done
fi
for bundle in "$p/clap/CHIMPO.clap" "$p/vst3/CHIMPO.vst3" "$p/component/CHIMPO.component"; do
    codesign --verify --deep --strict --verbose=2 "$bundle"
done

# One component package per format. Bundles must not be "relocatable":
# otherwise Installer upgrades any same-ID bundle wherever it finds one
# (e.g. a developer build) instead of installing to the plug-in folder.
# They are not version-checked either, so an older version still replaces an
# installed bundle instead of being skipped.
component() {  # id-suffix payload-dir install-location
    local plist="$work/$1.plist"
    pkgbuild --analyze --root "$p/$1" "$plist" >/dev/null
    python3 - "$plist" <<'EOF'
import plistlib, sys
with open(sys.argv[1], "rb") as f:
    entries = plistlib.load(f)
for entry in entries:
    entry["BundleIsRelocatable"] = False
    entry["BundleOverwriteAction"] = "upgrade"
    entry["BundleIsVersionChecked"] = False
with open(sys.argv[1], "wb") as f:
    plistlib.dump(entries, f)
EOF
    pkgbuild --quiet --root "$p/$1" --component-plist "$plist" \
        --identifier "engineering.bellcast.chimpo.pkg.$1" --version "$version" \
        --install-location "$2" "$work/components/chimpo-$1.pkg"
}
component clap "/Library/Audio/Plug-Ins/CLAP"
component vst3 "/Library/Audio/Plug-Ins/VST3"
component component "/Library/Audio/Plug-Ins/Components"
pkgbuild --quiet --root "$p/licenses" \
    --identifier engineering.bellcast.chimpo.pkg.licenses --version "$version" \
    --install-location "/Library/Application Support/Bellcast Engineering/CHIMPO" \
    "$work/components/chimpo-licenses.pkg"

# The licence Installer asks the user to accept: the MIT terms plus the AGPL
# notice for JUCE, one line per paragraph so Installer wraps it to its width.
python3 scripts/licence_page.py --width 0 "$work/resources/NOTICE.txt"
ref() { echo "<pkg-ref id=\"engineering.bellcast.chimpo.pkg.$1\" version=\"$version\">chimpo-$1.pkg</pkg-ref>"; }
# customize="always": Installation Type opens on the format list (all ticked)
# rather than hiding it behind a Customize button.
cat > "$work/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>CHIMPO $version</title>
    <license file="NOTICE.txt"/>
    <options customize="always" require-scripts="false" hostArchitectures="$archs"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <volume-check><allowed-os-versions><os-version min="11.0"/></allowed-os-versions></volume-check>
    <choices-outline>
        <line choice="component"/>
        <line choice="vst3"/>
        <line choice="clap"/>
        <line choice="licenses"/>
    </choices-outline>
    <choice id="component" title="Audio Unit" description="For Logic Pro, GarageBand and other AU hosts. Installs to /Library/Audio/Plug-Ins/Components.">$(ref component)</choice>
    <choice id="vst3" title="VST3" description="For VST3 hosts such as Ableton Live, Cubase and Reaper. Installs to /Library/Audio/Plug-Ins/VST3.">$(ref vst3)</choice>
    <choice id="clap" title="CLAP" description="For CLAP hosts such as Bitwig Studio and Reaper. Installs to /Library/Audio/Plug-Ins/CLAP.">$(ref clap)</choice>
    <choice id="licenses" title="Licences" visible="false" enabled="false" selected="true">$(ref licenses)</choice>
</installer-gui-script>
EOF
if [ "$unsigned" = 1 ]; then
    productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
        --package-path "$work/components" "$pkg"
else
    productbuild --distribution "$work/distribution.xml" --resources "$work/resources" \
        --package-path "$work/components" --sign "$CHIMPO_INSTALLER_IDENTITY" --timestamp "$pkg"
fi

# No metadata may reach the installed plug-in folders. Installer applies
# AppleDouble (._*) entries as extended attributes, so inspect the fully
# expanded payload. Only com.apple.provenance is tolerated: the kernel adds
# it to files created by provenance-tracked apps and it cannot be removed.
rm -rf "$work/expanded"
# Expand outside the payload tree: pkgutil sets payload modes as it writes, and
# expanding inside target/pkg makes it fail to write into its own directories.
expanded=$(mktemp -d "${TMPDIR:-/tmp}/chimpo-pkg.XXXXXX")/expanded
trap 'rm -rf "$(dirname "$expanded")"' EXIT
pkgutil --expand-full "$pkg" "$expanded"
grep -q '<license file="NOTICE.txt"/>' "$expanded/Distribution"
cmp "$expanded/Resources/NOTICE.txt" "$work/resources/NOTICE.txt"
for f in NOTICE.txt LICENSE THIRD_PARTY.md; do
    cmp "$expanded/chimpo-licenses.pkg/Payload/$f" "$work/payload/licenses/$f"
done
python3 - "$expanded" <<'PY'
import os, subprocess, sys
bad = []
for root, dirs, files in os.walk(sys.argv[1]):
    for name in dirs + files:
        path = os.path.join(root, name)
        if "/Payload" not in path:
            continue
        if name == ".DS_Store" or name.startswith("._"):
            bad.append(path)
# macOS Python has no os.listxattr; `xattr -r` prints "path: name" lines.
listing = subprocess.run(["xattr", "-r", sys.argv[1]], capture_output=True, text=True,
                         check=True).stdout
bad += [line for line in listing.splitlines()
        if "/Payload" in line and not line.endswith(": com.apple.provenance")]
if bad:
    sys.exit("Metadata in installer payload:\n" + "\n".join(bad[:20]))
PY
if [ "$unsigned" = 1 ]; then
    echo "Built $pkg: unsigned and not notarized, for testing only"
    shasum -a 256 "$pkg"
    exit 0
fi
# Notarize, fail unless Apple accepts, then staple the ticket for offline installs.
result=$(xcrun notarytool submit "$pkg" --keychain-profile "$CHIMPO_NOTARY_PROFILE" \
    --wait --output-format json)
echo "$result"
status=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["status"])' "$result")
if [ "$status" != "Accepted" ]; then
    id=$(python3 -c 'import json,sys; print(json.loads(sys.argv[1])["id"])' "$result")
    xcrun notarytool log "$id" --keychain-profile "$CHIMPO_NOTARY_PROFILE" >&2
    exit 1
fi
xcrun stapler staple "$pkg"
xcrun stapler validate "$pkg"
pkgutil --check-signature "$pkg"
spctl --assess --type install --verbose=2 "$pkg"
shasum -a 256 "$pkg"
