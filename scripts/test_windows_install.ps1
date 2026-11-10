# Install the built Windows installer silently on a CI runner, check every
# file landed where hosts look, then uninstall and check it is all gone.
# Run only on a disposable machine: it installs system-wide.
$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$setup = @(Get-ChildItem dist -Filter 'CHIMPO-*-Windows-Setup.exe')
if ($setup.Count -ne 1) { throw "Expected one installer in dist, found $($setup.Count)" }

$common = ${env:CommonProgramFiles}
$app = Join-Path $env:ProgramFiles 'Bellcast Engineering\CHIMPO'
$artefacts = 'build\Chimpo_artefacts\Release'
$expected = @(
    "$common\VST3\CHIMPO.vst3\Contents\x86_64-win\CHIMPO.vst3",
    "$common\CLAP\CHIMPO.clap",
    "$common\CLAP\CHIMPO.clap.resources\Factory Cards\TAPE",
    "$app\NOTICE.txt",
    "$app\LICENSE.txt",
    "$app\THIRD_PARTY.md"
)

function Run($exe, $arguments) {
    $p = Start-Process -FilePath $exe -ArgumentList $arguments -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$exe exited with $($p.ExitCode)" }
}

Run $setup[0].FullName '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$PWD\dist\install.log"
foreach ($f in $expected) {
    if (-not (Test-Path -LiteralPath $f)) { throw "Not installed: $f" }
}
# Installed payloads must be byte-identical to the build outputs.
$pairs = @{
    "$common\CLAP\CHIMPO.clap" = "$artefacts\CLAP\CHIMPO.clap"
    "$common\VST3\CHIMPO.vst3\Contents\x86_64-win\CHIMPO.vst3" = "$artefacts\VST3\CHIMPO.vst3\Contents\x86_64-win\CHIMPO.vst3"
    "$app\NOTICE.txt" = 'target\windows-licences\NOTICE.txt'
    "$app\LICENSE.txt" = 'LICENSE'
}
foreach ($installed in $pairs.Keys) {
    $a = (Get-FileHash -LiteralPath $installed).Hash
    $b = (Get-FileHash -LiteralPath $pairs[$installed]).Hash
    if ($a -ne $b) { throw "Installed $installed differs from the build" }
}
# The licence page is the same notice as the installed file, and it says what
# these builds are: MIT code, JUCE under the AGPLv3, with the source location.
$page = Get-Content -LiteralPath 'target\windows-licences\licence-page.txt' -Raw -Encoding UTF8
$notice = (Get-Content -LiteralPath "$app\NOTICE.txt" -Raw -Encoding UTF8) -replace '\s+', ' '
if (-not $page.Contains('GNU Affero General Public License')) { throw 'The licence page does not name the AGPLv3' }
if (-not $notice.Contains('MIT License')) { throw 'The installed NOTICE.txt has no MIT licence' }
if (-not $notice.Contains('github.com/unifolia/CHIMPO-PLUGIN')) { throw 'The notice does not name the source' }
$notices = @(Get-ChildItem "$app\notices" -File -Recurse).Count
if ($notices -eq 0) { throw 'No licence notices installed' }
if (-not (Test-Path "$app\notices\AGPL-3.0.txt")) { throw 'The AGPLv3 text is not installed' }
Write-Host "Installed OK ($notices notice files)"

# Signed builds: the installer, uninstaller and every installed plug-in
# binary must carry a valid signature from the expected publisher.
if ($env:CHIMPO_SIGN -eq '1') {
    $signer = if ($env:CHIMPO_EXPECT_SIGNER) { $env:CHIMPO_EXPECT_SIGNER } else { 'James Lewis Sotelo' }
    $signed = @($setup[0].FullName, "$app\unins000.exe", "$common\CLAP\CHIMPO.clap",
        "$common\VST3\CHIMPO.vst3\Contents\x86_64-win\CHIMPO.vst3")
    foreach ($f in $signed) {
        $s = Get-AuthenticodeSignature -LiteralPath $f
        if ($s.Status -ne 'Valid') { throw "Signature $($s.Status): $f" }
        if ($s.SignerCertificate.Subject -notmatch [regex]::Escape("CN=$signer")) {
            throw "Unexpected signer $($s.SignerCertificate.Subject): $f"
        }
        if (-not $s.TimeStamperCertificate) { throw "Not timestamped: $f" }
    }
    Write-Host "Signatures OK: $($s.SignerCertificate.Subject)"
}

# Format choice. A reinstall installs only the ticked formats and removes an
# unticked one; ticking none exits before installing anything.
$vst3 = "$common\VST3\CHIMPO.vst3"
$vst3Binary = "$vst3\Contents\x86_64-win\CHIMPO.vst3"
$clap = "$common\CLAP\CHIMPO.clap"
$clapCards = "$common\CLAP\CHIMPO.clap.resources"
$quiet = '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
Run $setup[0].FullName ($quiet + '/COMPONENTS=clap')
if ((Test-Path -LiteralPath $vst3) -or -not (Test-Path -LiteralPath $clap -PathType Leaf)) {
    throw 'A CLAP-only reinstall did not leave just the CLAP'
}
if (-not (Test-Path -LiteralPath "$clapCards\Factory Cards\TAPE")) { throw 'A CLAP-only reinstall left no factory cards' }
Run $setup[0].FullName ($quiet + '/COMPONENTS=vst3')
if ((Test-Path -LiteralPath $clap) -or (Test-Path -LiteralPath $clapCards) -or
    -not (Test-Path -LiteralPath $vst3Binary -PathType Leaf)) {
    throw 'A VST3-only reinstall did not leave just the VST3'
}
$p = Start-Process -FilePath $setup[0].FullName -ArgumentList ($quiet + '/COMPONENTS=') -Wait -PassThru
if ($p.ExitCode -eq 0) { throw 'An install with no format was not refused' }
if (-not (Test-Path -LiteralPath $vst3Binary -PathType Leaf)) { throw 'A refused install changed the existing one' }
Write-Host 'Format choice OK'

Run "$app\unins000.exe" '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
# The uninstaller runs from a temporary copy and removes its own folder
# just after the process we waited for exits: poll briefly.
$left = @()
for ($i = 0; $i -lt 30; $i++) {
    $left = @(($expected + "$common\VST3\CHIMPO.vst3", "$app\notices", $clapCards) | Where-Object { Test-Path -LiteralPath $_ })
    if ($left.Count -eq 0) { break }
    Start-Sleep -Seconds 1
}
if ($left.Count -ne 0) { throw "Left behind after uninstall: $($left -join ', ')" }
Write-Host 'Uninstalled cleanly'
