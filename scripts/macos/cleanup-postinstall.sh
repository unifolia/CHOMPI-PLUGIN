#!/bin/bash
# Run by the CHIMPO installer's "Remove older copies" choice, as root.
#
# An earlier CHIMPO that lives outside /Library (an early test build in
# ~/Library, say) registers the same AU, VST3 and CLAP component id as this
# package, so a host sees two copies of one plugin; Logic in particular can then
# list neither. The package itself only writes /Library, so those copies are not
# its to upgrade. Move them aside instead of deleting them: the user keeps their
# build, and the host finds exactly one CHIMPO.
set -u

QUARANTINE="${CHIMPO_QUARANTINE:-/Library/Application Support/Bellcast Engineering/CHIMPO/removed installs}"
STAMP="$(date +%Y-%m-%d-%H-%M-%S)"
MOVED=0

# CHIMPO_CLEANUP_HOMES lets the install test point this at a scratch home.
for HOME_DIR in ${CHIMPO_CLEANUP_HOMES:-} /Users/*; do
    [ -n "$HOME_DIR" ] || continue
    [ -d "$HOME_DIR/Library/Audio/Plug-Ins" ] || continue
    for REL in Components/CHIMPO.component VST3/CHIMPO.vst3 CLAP/CHIMPO.clap; do
        SRC="$HOME_DIR/Library/Audio/Plug-Ins/$REL"
        [ -e "$SRC" ] || continue
        DEST="$QUARANTINE/$STAMP/$(basename "$HOME_DIR")/$REL"
        mkdir -p "$(dirname "$DEST")"
        if mv "$SRC" "$DEST"; then
            echo "CHIMPO: moved older copy $SRC to $DEST"
            MOVED=$((MOVED + 1))
        else
            echo "CHIMPO: could not move $SRC; remove it by hand if a host shows two CHIMPOs" >&2
        fi
    done
done

if [ "$MOVED" -eq 0 ]; then
    echo "CHIMPO: no older copies outside /Library"
fi
exit 0
