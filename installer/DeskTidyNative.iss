; DeskTidy Native (C++ greenfield) standalone installer
; Build: scripts\build_native_installer.bat

#ifndef MyAppVersion
#define MyAppVersion "4.0.52"
#endif

#define MyAppName "DeskTidy Native"
#define MyAppPublisher "DeskTidy"
#define MyAppExeName "desktidy_native.exe"
#define SourceDir ".."

[Setup]
AppId={{B8C4D0E2-9F3A-4B6C-8D1E-2F3A4B5C6D7E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\DeskTidyNative
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\dist
OutputBaseFilename=DeskTidyNative_Setup_{#MyAppVersion}
Compression=lzma2/fast
SolidCompression=no
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
DisableProgramGroupPage=yes
ShowLanguageDialog=no
LanguageDetectionMethod=locale
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=DeskTidy Native
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
CloseApplications=yes
CloseApplicationsFilter=desktidy_native.exe

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "startup"; Description: "开机自动启动 {#MyAppName}"; GroupDescription: "启动选项:"; Flags: checked

[InstallDelete]
Type: files; Name: "{app}\desktidy_dev.marker"

[Files]
Source: "{#SourceDir}\native\build\Release\desktidy_native.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\native\README.md"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\docs\cpp-greenfield.md"; DestDir: "{app}\docs"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startup

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; Headless cleanup: stop other instances, restore desktop/namespace icons,
; remove autostart + shell verbs + Run values, purge config/cache/logs while
; preserving user 笔记/录屏/纪要 document folders.
Filename: "{app}\{#MyAppExeName}"; Parameters: "--uninstall-cleanup"; Flags: runhidden waituntilterminated; RunOnceId: "DeskTidyNativeUninstallCleanup"

[UninstallDelete]
Type: files; Name: "{userstartup}\{#MyAppName}.lnk"
Type: files; Name: "{userstartup}\DeskTidy Native.lnk"
; Runtime caches beside the exe. Never list 笔记/录屏/纪要 (user documents).
Type: filesandordirs; Name: "{app}\assets"
Type: filesandordirs; Name: "{app}\logs"
