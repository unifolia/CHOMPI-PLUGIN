# Releasing

CI runs on a pull request or a `v*` tag, never on a merge to main: branch
protection requires the PR's checks with the branch up to date, so a merge
lands exactly what CI tested.

1. **Bump the version** in `project(CHIMPO VERSION x.y.z)` in
   `CMakeLists.txt`, in a PR. Its CI runs `scripts/check_release_tag.sh` on the
   tag later, and that script fails unless the tag is `v` plus this version.
2. **Tag** the merge commit once its PR checks are green:
   `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. **Read the tag run** (`.github/workflows/build.yml`). It must be green on
   macOS, Windows and Linux, plus `macos-universal-intel`, which validates the
   universal build's x86_64 half. The jobs print the pass lines to look for:
   "Result: 537 tests passed", "clap-validator: ... tests", "auval: 3/3 passes
   succeeded", "Installed OK", "Format choice OK", "Uninstalled cleanly", and
   "Signatures OK" when Windows signing is configured.
4. **Build the macOS package** from the tag run's validated universal build,
   not a fresh local build:

   ```bash
   gh run download <tag-run-id> -n chimpo-macos-universal
   mkdir -p build/Chimpo_artefacts/Release
   tar -xzf chimpo-macos-universal.tar.gz -C build/Chimpo_artefacts/Release
   python3 scripts/third_party.py --juce <checkout>/build/_deps/juce-src
   CHIMPO_SIGN_IDENTITY="Developer ID Application: ..." \
   CHIMPO_INSTALLER_IDENTITY="Developer ID Installer: ..." \
   CHIMPO_NOTARY_PROFILE=bellcast-notary bash scripts/package_macos.sh
   shasum -a 256 dist/CHIMPO-*-macOS.pkg > dist/CHIMPO-*-macOS.pkg.sha256
   gh release upload vX.Y.Z dist/CHIMPO-*-macOS.pkg dist/CHIMPO-*-macOS.pkg.sha256
   ```

   The script refuses a non-universal bundle, signs the three bundles and the
   package, notarizes, staples and assesses it, and prints the package's
   SHA-256.
5. **Check the draft release**: the Windows installer (signed when the Azure
   secrets are set) and the Linux archive from the tag run, the macOS package
   just uploaded, every `.sha256`, and the notes. Publish it.
6. **Tell the website**: bump `version` in the site's plugin content file so
   the download page matches the release.

The Windows signing needs, once per repository: the `signing` environment
allowing `v*` tags, a federated credential for this repo, and the
`AZURE_CLIENT_ID`, `AZURE_TENANT_ID` and `AZURE_SUBSCRIPTION_ID` secrets. Until
they exist a tag ships an unsigned installer and the run says so.
