; Installer of Kraken Explorer: one setup.exe with what the zip holds (the exe, the embeddable
; Python, the examples). Per user, no administrator rights, with a Start menu entry and an
; uninstall entry in Windows' app list. Built by scripts\build_win_zip.ps1 when Inno Setup 6.3+
; is installed:
;   ISCC /DVersion=0.0.2 /DStage=<staged folder> /DOut=<output folder> kraken-explorer.iss

[Setup]
; Identifies the app across versions: a newer setup upgrades in place. Never change it.
AppId={{22956EC7-3B44-48EB-AA16-D0208BE3BC03}
AppName=Kraken Explorer
AppVersion={#Version}
AppVerName=Kraken Explorer {#Version}
AppPublisher=Kraken Explorer
AppPublisherURL=https://github.com/stoffej/kraken-explorer
; Properties > Details of setup.exe; code signing (SignPath) requires product name and version.
VersionInfoVersion={#Version}
VersionInfoProductName=Kraken Explorer
VersionInfoProductTextVersion={#Version}
; The folder install.bat of the zip uses.
DefaultDirName={autopf}\Kraken Explorer
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#Out}
OutputBaseFilename=kraken-explorer-{#Version}-win64-setup
UninstallDisplayIcon={app}\kraken-explorer.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Excludes: "install.bat,uninstall.bat"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Kraken Explorer"; Filename: "{app}\kraken-explorer.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\kraken-explorer.exe"; Description: "Start Kraken Explorer"; Flags: nowait postinstall skipifsilent
