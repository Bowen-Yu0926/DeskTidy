; DeskTidy (native / C++) product installer
; Build: scripts\build_native_installer.bat
; Version: VERSION (+ /DMyAppVersion) — bump via scripts\bump_version.py --bump

#ifndef MyAppVersion
#define MyAppVersion "4.0.47"
#endif

#define MyAppName "DeskTidy"
#define MyAppPublisher "DeskTidy"
#define MyAppURL "https://github.com/Bowen-Yu0926/DeskTidy-Releases"
#define MyAppExeName "desktidy_native.exe"
#define SourceDir ".."

[Setup]
; Keep the long-standing product AppId so upgrades replace prior DeskTidy installs.
AppId={{A7B3C9D1-8E2F-4A5B-9C0D-1E2F3A4B5C6D}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
OutputDir=..\dist
OutputBaseFilename=DeskTidy_Setup_{#MyAppVersion}
#ifndef DeskTidyCompress
#define DeskTidyCompress "lzma2/fast"
#endif
Compression={#DeskTidyCompress}
SolidCompression=no
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#MyAppExeName}
SetupIconFile=..\assets\app_icon.ico
DisableProgramGroupPage=yes
ShowLanguageDialog=no
LanguageDetectionMethod=locale
VersionInfoVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=DeskTidy
VersionInfoProductName=DeskTidy
VersionInfoProductVersion={#MyAppVersion}
CloseApplications=yes
CloseApplicationsFilter=desktidy_native.exe,deskNote.exe,DeskTidy.exe

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "desknote"; Description: "创建 DeskNote 快捷方式（桌面 + 开始菜单）"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "startup"; Description: "开机自动启动 {#MyAppName}"; GroupDescription: "启动选项:"; Flags: checkedonce

[Files]
; Native binaries + runtime assets (exclude per-user folders recreated at runtime).
Source: "{#SourceDir}\native\build\Release\desktidy_native.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\native\build\Release\deskNote.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\native\build\Release\assets\*"; DestDir: "{app}\assets"; Excludes: "笔记\*,录屏\*,纪要\*,邮件\*,logs\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\assets\fd\fd.exe"; DestDir: "{app}\assets\fd"; Flags: ignoreversion nocompression skipifsourcedoesntexist
Source: "{#SourceDir}\config\default_settings.json"; DestDir: "{app}\config"; Flags: ignoreversion onlyifdoesntexist
Source: "{#SourceDir}\docs\desktidy.html"; DestDir: "{app}\docs"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\docs\desknote.html"; DestDir: "{app}\docs"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\DeskNote"; Filename: "{app}\deskNote.exe"; IconFilename: "{app}\assets\desknote_icon.ico"; Comment: "DeskNote 记事本"; Tasks: desknote
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{autodesktop}\DeskNote"; Filename: "{app}\deskNote.exe"; IconFilename: "{app}\assets\desknote_icon.ico"; Comment: "DeskNote 记事本"; Tasks: desknote
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startup

[Registry]
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.new_fence"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.region_fence"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.refresh_fences"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.show_window"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Desktidy"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: binary; ValueName: "{#MyAppName}"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: binary; ValueName: "Desktidy"; Flags: uninsdeletevalue dontcreatekey

[Run]
; No skipifsilent: the in-app updater installs with /SILENT and then quits, so
; the postinstall entry is the ONLY thing that brings DeskTidy back. With
; skipifsilent the update finished with no DeskTidy running at all.
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall

[UninstallRun]
Filename: "{app}\{#MyAppExeName}"; Parameters: "--uninstall-cleanup"; Flags: runhidden waituntilterminated; RunOnceId: "DeskTidyUninstallCleanup"

[UninstallDelete]
Type: files; Name: "{userstartup}\{#MyAppName}.lnk"
Type: files; Name: "{userstartup}\DeskTidy Native.lnk"
Type: files; Name: "{userstartup}\Desktidy.lnk"
Type: filesandordirs; Name: "{app}\assets"
Type: filesandordirs; Name: "{app}\logs"
