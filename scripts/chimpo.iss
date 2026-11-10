; Inno Setup 6 script for the Windows installer
; dist\CHIMPO-<version>-Windows-Setup.exe. Build it with
; scripts/package_windows.sh, which passes /DAppVersion from CMakeLists.txt.
;
; Installs the VST3 bundle to Common Files\VST3 and the CLAP with its factory
; cards to Common Files\CLAP (the standard system-wide locations hosts scan),
; and the licences beside the uninstaller. The card folders CHIMPO copies to
; the user's Music folder are never touched.
;
; The licence page carries the MIT licence and the AGPLv3 notice for JUCE,
; which package_windows.sh generates into target\windows-licences.

#ifndef AppVersion
  #error Pass /DAppVersion=<version> (scripts/package_windows.sh does)
#endif

[Setup]
; Never change AppId: Windows identifies upgrades and the uninstaller by it.
AppId={{D8F93A6E-7BB0-49D4-88EC-4F0881D7C9BB}
AppName=CHIMPO
AppVersion={#AppVersion}
AppVerName=CHIMPO {#AppVersion}
AppPublisher=Bellcast Engineering
AppPublisherURL=https://bellcast.engineering
AppSupportURL=https://github.com/unifolia/CHIMPO-PLUGIN
VersionInfoVersion={#AppVersion}
VersionInfoProductName=CHIMPO
; Paths below are relative to the repository root.
SourceDir=..
DefaultDirName={commonpf64}\Bellcast Engineering\CHIMPO
DisableDirPage=yes
DisableProgramGroupPage=yes
LicenseFile=target\windows-licences\licence-page.txt
OutputDir=dist
OutputBaseFilename=CHIMPO-{#AppVersion}-Windows-Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=CHIMPO
#ifdef Sign
; scripts/package_windows.sh defines the `azure` sign tool (sign_windows.ps1)
; when CHIMPO_SIGN=1. Signs the uninstaller inside, then the installer.
SignTool=azure
SignedUninstaller=yes
#endif

; Users pick formats on the Select Components page. A reinstall shows the
; previous choice, and unticking a format there removes it.
[Types]
Name: "full"; Description: "VST3 and CLAP"
Name: "custom"; Description: "Choose formats"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 (Ableton Live, Cubase, FL Studio, Reaper and others)"; Types: full custom; Flags: disablenouninstallwarning
Name: "clap"; Description: "CLAP (Bitwig Studio, Reaper and others)"; Types: full custom; Flags: disablenouninstallwarning

[InstallDelete]
; Replace the whole bundle so no file from an older version lingers in it,
; and remove either format if it is no longer ticked. The CLAP's cards folder
; goes with it.
Type: filesandordirs; Name: "{commoncf64}\VST3\CHIMPO.vst3"
Type: files; Name: "{commoncf64}\CLAP\CHIMPO.clap"; Components: not clap
Type: filesandordirs; Name: "{commoncf64}\CLAP\CHIMPO.clap.resources"; Components: not clap

[Files]
Source: "build\Chimpo_artefacts\Release\VST3\CHIMPO.vst3\*"; DestDir: "{commoncf64}\VST3\CHIMPO.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\Chimpo_artefacts\Release\CLAP\CHIMPO.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
Source: "build\Chimpo_artefacts\Release\CLAP\CHIMPO.clap.resources\*"; DestDir: "{commoncf64}\CLAP\CHIMPO.clap.resources"; Components: clap; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "target\windows-licences\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "target\notices\*"; DestDir: "{app}\notices"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
// Installing no format would install only the licences. A silent install
// with no format (/COMPONENTS="") exits before installing anything.
function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectComponents) and not WizardIsComponentSelected('vst3')
    and not WizardIsComponentSelected('clap') then
  begin
    SuppressibleMsgBox('Choose at least one plug-in format to install.', mbError, MB_OK, IDOK);
    Result := False;
  end;
end;
