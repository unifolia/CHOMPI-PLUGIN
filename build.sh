#!/bin/bash
# Builds the CHIMPO plugin and installs it for the current user.
#
#   ./build.sh                     asks which formats to build
#   ./build.sh au vst3             builds those without asking
#   ./build.sh au --universal      also for Intel Macs / DAWs under Rosetta
#
# BUILD_DIR (default build-release) picks the build folder.
# Runs on macOS's bash 3.2: no associative arrays, no empty arrays under set -u.
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${BUILD_DIR:-build-release}"
LOG="$BUILD_DIR/build.log"

if [ -t 1 ]; then bold=$'\e[1m'; dim=$'\e[2m'; red=$'\e[31m'; reset=$'\e[0m'; else bold=; dim=; red=; reset=; fi
info() { printf '%s\n' "$*"; }
fail() { printf '\n%s%s%s\n' "$red" "$*" "$reset" >&2; exit 1; }
yes_no() { # yes_no <question> <default y|n>
    local answer
    read -r -p "$1 " answer || answer=
    answer=$(printf '%s' "${answer:-$2}" | tr '[:upper:]' '[:lower:]')
    [ "${answer:0:1}" = y ]
}

usage() { sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; }

# ---- Arguments -----------------------------------------------------------

formats=
universal=
for arg in "$@"; do
    case "$(printf '%s' "$arg" | tr '[:upper:]' '[:lower:]')" in
        au | component) formats="$formats;AU" ;;
        vst3 | vst) formats="$formats;VST3" ;;
        --universal) universal=1 ;;
        -h | --help) usage; exit 0 ;;
        *) usage; fail "Unknown option: $arg" ;;
    esac
done

info "${bold}CHIMPO build${reset}"
info

[ "$(uname -s)" = Darwin ] || fail "build.sh is for macOS. On Windows and Linux, download a release or build with CMake (README.md)."

# ---- Tools -----------------------------------------------------------------

if ! xcode-select -p >/dev/null 2>&1; then
    info "Apple's Command Line Tools (the compiler) aren't installed."
    info "A window will open to install them. When it has finished, run ./build.sh again."
    xcode-select --install >/dev/null 2>&1 || true
    exit 1
fi

cmake_bin=$(command -v cmake || true)
if [ -z "$cmake_bin" ] && [ -x /Applications/CMake.app/Contents/bin/cmake ]; then
    cmake_bin=/Applications/CMake.app/Contents/bin/cmake
fi
if [ -z "$cmake_bin" ]; then
    if command -v brew >/dev/null 2>&1 && yes_no "CMake is needed to build. Install it with Homebrew now? [Y/n]" y; then
        brew install cmake
        cmake_bin=$(command -v cmake)
    else
        fail "CMake is needed: download the macOS .dmg from https://cmake.org/download/, drag CMake to Applications, then run ./build.sh again."
    fi
fi
cmake_version=$("$cmake_bin" --version | awk 'NR == 1 { print $3 }')
IFS=. read -r cmake_major cmake_minor _ <<< "$cmake_version"
if [ "$cmake_major" -lt 3 ] || { [ "$cmake_major" -eq 3 ] && [ "$cmake_minor" -lt 22 ]; }; then
    fail "CMake $cmake_version is too old; 3.22 or newer is needed (brew upgrade cmake, or a new download from cmake.org)."
fi

# ---- What to build -------------------------------------------------------

# The Mac's own chip, even when this Terminal runs under Rosetta.
if [ "$(uname -m)" = arm64 ] || [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || true)" = 1 ]; then
    chip=arm64
else
    chip=x86_64
fi

if [ -z "$formats" ]; then
    info "Which versions of the plugin do you want?"
    info "  1) AU           for Logic Pro, GarageBand and MainStage (most other Mac DAWs load it too)"
    info "  2) VST3         for Ableton Live, Bitwig, Cubase, Reaper, Studio One, FL Studio"
    while [ -z "$formats" ]; do
        read -r -p "Type the numbers (e.g. 1), or press Return for both: " picks || picks=
        [ -n "$picks" ] || picks="1 2"
        bad=
        for pick in $(printf '%s' "$picks" | tr ',' ' '); do
            case "$pick" in
                1) formats="$formats;AU" ;;
                2) formats="$formats;VST3" ;;
                *) bad=1 ;;
            esac
        done
        if [ -n "$bad" ]; then
            info "Please use only 1 and 2."
            formats=
        fi
    done
    info

    if [ "$chip" = arm64 ] && [ -z "$universal" ]; then
        info "Does your DAW run under Rosetta, or will you copy the plugin to an Intel Mac?"
        info "${dim}(If you don't know, the answer is no. Saying yes doubles the build time.)${reset}"
        if yes_no "Also build for Intel? [y/N]" n; then universal=1; fi
        info
    fi
fi

formats=$(printf '%s' "${formats#;}" | tr ';' '\n' | awk '!seen[$0]++' | paste -sd ';' -)
if [ -n "$universal" ]; then archs="arm64;x86_64"; else archs="$chip"; fi

# ---- Build -----------------------------------------------------------------

arch_label="Apple Silicon"
[ "$chip" = x86_64 ] && arch_label="Intel"
[ -n "$universal" ] && arch_label="Apple Silicon + Intel"
info "Building ${bold}$(printf '%s' "$formats" | sed 's/;/ + /g')${reset} for $arch_label."
info "The first build downloads JUCE and takes a few minutes. Details go to $LOG."
mkdir -p "$BUILD_DIR"

if ! "$cmake_bin" -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$archs" \
    -DCHIMPO_FORMATS="$formats" \
    -DCHIMPO_BUILD_TOOLS=OFF \
    -DCHIMPO_BUILD_HARNESS=OFF \
    -DCHIMPO_INSTALL_PLUGINS=ON >"$LOG" 2>&1; then
    tail -n 25 "$LOG" >&2
    fail "Setting up the build failed (full log: $LOG)."
fi

# Show the build's own percentage instead of compiler output.
if ! "$cmake_bin" --build "$BUILD_DIR" --config Release -j "$(sysctl -n hw.ncpu)" 2>&1 | tee -a "$LOG" |
    while IFS= read -r line; do
        case "$line" in
            "["*"%]"*)
                percent=${line#[}
                printf '\r  %s%%' "${percent%%%]*}"
                ;;
        esac
    done; then
    info
    tail -n 25 "$LOG" >&2
    fail "The build failed (full log: $LOG)."
fi
printf '\r  done   \n\n'

# ---- Install ---------------------------------------------------------------

info "${bold}Installed${reset}"
case ";$formats;" in *";AU;"*)
    info "  AU          ~/Library/Audio/Plug-Ins/Components/CHIMPO.component"
    # Logic and GarageBand only see a new or rebuilt AU once macOS rescans.
    killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true ;;
esac
case ";$formats;" in *";VST3;"*)
    info "  VST3        ~/Library/Audio/Plug-Ins/VST3/CHIMPO.vst3" ;;
esac
info "  The factory SD cards are inside the plugin."

# Earlier builds installed the factory cards here; the plugin no longer reads them.
old_cards="$HOME/Library/Application Support/CHIMPO/Factory Cards"
if [ -d "$old_cards" ]; then
    rm -rf "$old_cards"
    rmdir "$HOME/Library/Application Support/CHIMPO" 2>/dev/null || true
    info "  Removed the old copy in ~/Library/Application Support/CHIMPO."
fi
info
info "${bold}Next${reset}"
info "  Restart your DAW and load CHIMPO as an instrument (some DAWs need a"
info "  plug-in rescan in their settings)."
info "  Pick TAPE, WAVE or TEMPO in the top-left of the panel. Each has its own SD card"
info "  folder in ~/Music/CHIMPO, filled with its factory files the first time;"
info "  edit it there like the real card."
info "  To update later: git pull, then ./build.sh again."
