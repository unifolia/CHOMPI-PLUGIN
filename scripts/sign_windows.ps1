# Sign Windows binaries with Azure Artifact Signing (account `bellcast`,
# certificate profile `bellcast-engineering`), then verify each signature.
#
#   pwsh scripts/sign_windows.ps1 <file> [<file> ...]
#
# Needs an Azure CLI login holding the Certificate Profile Signer role on the
# profile. In CI that is azure/login (OIDC) inside the `signing` environment,
# which only `main`, listed branches and `v*` tags may use. The first call fetches
# SignTool and the Artifact Signing dlib from NuGet into target\signing.
# Inno Setup also calls this script to sign the installer and uninstaller.
param([Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Files)
$ErrorActionPreference = 'Stop'

$endpoint = 'https://eus.codesigning.azure.net/'
$account = 'bellcast'
$profileName = 'bellcast-engineering'
$timestamp = 'http://timestamp.acs.microsoft.com'

$tools = Join-Path (Split-Path $PSScriptRoot -Parent) 'target\signing'
New-Item -ItemType Directory -Force $tools | Out-Null

function Find-Tool($name) {
    Get-ChildItem $tools -Recurse -Filter $name -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object FullName -Descending | Select-Object -First 1
}

$signtool = Find-Tool 'signtool.exe'
$dlib = Find-Tool 'Azure.CodeSigning.Dlib.dll'
if (-not $signtool -or -not $dlib) {
    $nuget = (Get-Command nuget -ErrorAction SilentlyContinue).Source
    if (-not $nuget) {
        $nuget = Join-Path $tools 'nuget.exe'
        Invoke-WebRequest https://dist.nuget.org/win-x86-commandline/latest/nuget.exe -OutFile $nuget
    }
    foreach ($package in 'Microsoft.Windows.SDK.BuildTools', 'Microsoft.ArtifactSigning.Client') {
        & $nuget install $package -ExcludeVersion -OutputDirectory $tools -NonInteractive -Verbosity quiet
        if ($LASTEXITCODE) { throw "nuget install $package failed" }
    }
    $signtool = Find-Tool 'signtool.exe'
    $dlib = Find-Tool 'Azure.CodeSigning.Dlib.dll'
    if (-not $signtool -or -not $dlib) { throw 'SignTool or the Artifact Signing dlib was not found after install' }
    Write-Host "SignTool: $($signtool.FullName)"
    Write-Host "Dlib: $($dlib.FullName)"
}

# Authenticate only through the Azure CLI login; skipping the other
# credential types avoids slow, confusing fallbacks.
$metadata = Join-Path $tools 'metadata.json'
@{
    Endpoint = $endpoint
    CodeSigningAccountName = $account
    CertificateProfileName = $profileName
    ExcludeCredentials = @(
        'EnvironmentCredential', 'WorkloadIdentityCredential', 'ManagedIdentityCredential',
        'SharedTokenCacheCredential', 'VisualStudioCredential', 'VisualStudioCodeCredential',
        'AzurePowerShellCredential', 'AzureDeveloperCliCredential', 'InteractiveBrowserCredential'
    )
} | ConvertTo-Json | Set-Content -Encoding utf8 $metadata

foreach ($file in $Files) {
    $path = (Resolve-Path -LiteralPath $file).Path
    & $signtool.FullName sign /fd SHA256 /tr $timestamp /td SHA256 /dlib $dlib.FullName /dmdf $metadata $path
    if ($LASTEXITCODE) { throw "Signing failed: $path" }
    & $signtool.FullName verify /pa /tw $path
    if ($LASTEXITCODE) { throw "Signature does not verify: $path" }
    $signature = Get-AuthenticodeSignature -LiteralPath $path
    if ($signature.Status -ne 'Valid') { throw "Signature status $($signature.Status): $path" }
    Write-Host "Signed $path by $($signature.SignerCertificate.Subject)"
}
