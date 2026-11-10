#!/bin/sh
# Install or remove CHIMPO's CLAP and VST3 plug-ins.
#
#   ./install.sh                     for your user (~/.clap, ~/.vst3)
#   sudo ./install.sh --system       for every user (/usr/lib/clap, /usr/lib/vst3)
#   ./install.sh --uninstall         remove (add --system for a system install)
#   --clap-only / --vst3-only        install or remove just one format
#
# The CLAP's factory cards live in CHIMPO.clap.resources, which installs beside
# it. Cards CHIMPO has already copied to your Music folder are never touched.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
scope=user
action=install
clap=1
vst3=1

usage() { sed -n '2,9s/^# \{0,1\}//p' "$0"; }
for arg in "$@"; do
    case $arg in
        --system) scope=system ;;
        --uninstall) action=uninstall ;;
        --clap-only) vst3=0 ;;
        --vst3-only) clap=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done
if [ "$clap" = 0 ] && [ "$vst3" = 0 ]; then
    echo "--clap-only and --vst3-only exclude each other" >&2
    exit 2
fi

if [ "$scope" = system ]; then
    if [ "$(id -u)" != 0 ]; then
        echo "A system install needs root: sudo $0 $*" >&2
        exit 1
    fi
    clap_dir=/usr/lib/clap
    vst3_dir=/usr/lib/vst3
    doc_dir=/usr/share/doc/chimpo
else
    if [ "$(id -u)" = 0 ]; then
        echo "Running as root would install into root's home only. Use --system for every user." >&2
        exit 1
    fi
    clap_dir=$HOME/.clap
    vst3_dir=$HOME/.vst3
    doc_dir=${XDG_DATA_HOME:-$HOME/.local/share}/doc/chimpo
fi

if [ "$action" = uninstall ]; then
    if [ "$clap" = 1 ] && [ -e "$clap_dir/CHIMPO.clap" ]; then
        rm -f "$clap_dir/CHIMPO.clap"
        rm -rf "$clap_dir/CHIMPO.clap.resources"
        echo "Removed CLAP from $clap_dir"
    fi
    if [ "$vst3" = 1 ] && [ -e "$vst3_dir/CHIMPO.vst3" ]; then
        rm -rf "$vst3_dir/CHIMPO.vst3"
        echo "Removed VST3 from $vst3_dir"
    fi
    # Keep the licences while either format remains installed.
    if [ ! -e "$clap_dir/CHIMPO.clap" ] && [ ! -e "$vst3_dir/CHIMPO.vst3" ]; then
        rm -rf "$doc_dir"
    fi
    exit 0
fi

# Stage beside the target, then swap, so a host never sees a half-copied plug-in.
if [ "$clap" = 1 ]; then
    mkdir -p "$clap_dir"
    cp "$here/CHIMPO.clap" "$clap_dir/.CHIMPO.clap.new"
    chmod 755 "$clap_dir/.CHIMPO.clap.new"
    mv -f "$clap_dir/.CHIMPO.clap.new" "$clap_dir/CHIMPO.clap"
    rm -rf "$clap_dir/.CHIMPO.clap.resources.new"
    cp -R "$here/CHIMPO.clap.resources" "$clap_dir/.CHIMPO.clap.resources.new"
    rm -rf "$clap_dir/CHIMPO.clap.resources"
    mv "$clap_dir/.CHIMPO.clap.resources.new" "$clap_dir/CHIMPO.clap.resources"
    echo "CLAP: $clap_dir/CHIMPO.clap (cards in CHIMPO.clap.resources)"
fi
if [ "$vst3" = 1 ]; then
    mkdir -p "$vst3_dir"
    rm -rf "$vst3_dir/.CHIMPO.vst3.new"
    cp -R "$here/CHIMPO.vst3" "$vst3_dir/.CHIMPO.vst3.new"
    chmod -R u=rwX,go=rX "$vst3_dir/.CHIMPO.vst3.new"
    rm -rf "$vst3_dir/CHIMPO.vst3"
    mv "$vst3_dir/.CHIMPO.vst3.new" "$vst3_dir/CHIMPO.vst3"
    echo "VST3: $vst3_dir/CHIMPO.vst3"
fi
rm -rf "$doc_dir"
mkdir -p "$doc_dir"
cp -R "$here/LICENSE" "$here/NOTICE.txt" "$here/THIRD_PARTY.md" "$here/notices" "$doc_dir/"
chmod -R u=rwX,go=rX "$doc_dir"
echo "Licences: $doc_dir"
echo "CHIMPO is MIT-licensed; these builds link JUCE under the AGPLv3: $doc_dir/NOTICE.txt"
echo "Rescan plug-ins in your host if CHIMPO does not appear."
