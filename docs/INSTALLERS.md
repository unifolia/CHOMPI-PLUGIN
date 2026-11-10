# Installers

Three installers, one per OS, all with the same licence page (MIT for CHIMPO
and its firmware, the AGPLv3 notice for JUCE, and the corresponding-source
link). CI builds and install-tests each one on every pull request; the macOS
package it tests is unsigned, so its bundles stay byte-identical to the ones
the validators ran on.

| OS | Build | Installs | Test |
|---|---|---|---|
| macOS 11+ | `scripts/package_macos.sh` → `.pkg` | AU, VST3, CLAP into `/Library/Audio/Plug-Ins`; licences into `/Library/Application Support/Bellcast Engineering/CHIMPO` | `scripts/test_macos_install.py` |
| Windows 10+ | `scripts/package_windows.sh` → `CHIMPO-<version>-Windows-Setup.exe` | VST3 and CLAP into `Common Files`; licences into `Program Files\Bellcast Engineering\CHIMPO` | `scripts/test_windows_install.ps1` |
| Linux x64 | `scripts/package_linux.sh` → `.tar.gz` with `install.sh` | `~/.clap`, `~/.vst3` (or `/usr/lib` with `--system`); licences into the user's doc folder | `scripts/test_linux_install.sh` |

Run `python3 scripts/third_party.py` first: it checks every licence text is
present and collects them into `target/notices/`, which all three installers
ship. The Windows and Linux packages also carry the factory cards beside the
CLAP (`CHIMPO.clap.resources`), because a CLAP is a single file rather than a
bundle; on macOS they live inside `CHIMPO.clap/Contents/Resources`.

Each installer offers the formats as choices, all ticked, and refuses to
install the licences alone. The tests check that the installed payload is
byte-identical to the build, that a format can be unticked on reinstall
(removing it), that the licences are the MIT text and the MIT + AGPL notice,
and that an uninstall leaves nothing behind.

## Signing

- **macOS**: `package_macos.sh` signs the three bundles and the package with
  the Developer ID identities in `CHIMPO_SIGN_IDENTITY` and
  `CHIMPO_INSTALLER_IDENTITY`, notarizes with the `notarytool` keychain
  profile named by `CHIMPO_NOTARY_PROFILE`, then staples and assesses the
  result. `CHIMPO_UNSIGNED=1` builds the test package instead.
- **Windows**: `scripts/sign_windows.ps1` signs with Azure Artifact Signing
  (account `bellcast`, profile `bellcast-engineering`) through the Azure CLI's
  OIDC login. The workflow signs the plug-ins, then Inno signs the installer
  and the uninstaller. A tag signs when the `AZURE_*` secrets exist; without
  them the release is unsigned and the run says so.

The macOS release package is built from the tag run's validated universal
bundle (`chimpo-macos-universal`), not from a fresh local build, so the
shipped binaries are the ones the validators checked. Upload it to the draft
release with its `.sha256`.
