; DeskTidy Inno Setup Script
; Build: scripts\build_installer.bat
; Version source: VERSION (auto-bumped by scripts\bump_version.py)

#ifndef MyAppVersion
#define MyAppVersion "4.0.24"
#endif

#define MyAppName "DeskTidy"
#define MyAppPublisher "DeskTidy"
#define MyAppURL "https://github.com"
#define MyAppExeName "DeskTidy.exe"
#define SourceDir ".."

[Setup]
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
; Default lzma2/fast + SolidCompression=no: wizard paints without decompressing
; the whole archive. Set DESKTIDY_INSTALLER_MAX=1 for lzma2/max (smaller setup).
; Cold double-click delay is dominated by AV scanning this unsigned EXE size
; (InitializeSetup no longer stops DeskTidy before UI). build.bat prunes
; WebEngine *.debug.* / DevTools / unused locales to keep the setup smaller.
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
; Close / terminate DeskTidy before replacing or removing files.
CloseApplications=yes
CloseApplicationsFilter=DeskTidy.exe,deskNote.exe

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "desknote"; Description: "创建 DeskNote 快捷方式（桌面 + 开始菜单）"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce
Name: "startup"; Description: "开机自动启动 {#MyAppName}"; GroupDescription: "启动选项:"; Flags: unchecked

[Files]
; Onedir payload (DeskTidy.exe + deskNote.exe + Qt runtime).
; Excludes: runtime logs must not ship inside the setup.
Source: "{#SourceDir}\dist\DeskTidy\*"; DestDir: "{app}"; Excludes: "logs\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\assets\fd\fd.exe"; DestDir: "{app}\assets\fd"; Flags: ignoreversion nocompression
Source: "{#SourceDir}\config\default_settings.json"; DestDir: "{app}\config"; Flags: ignoreversion onlyifdoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\DeskNote"; Filename: "{app}\deskNote.exe"; IconFilename: "{app}\deskNote.exe"; Comment: "DeskNote 记事本"; Tasks: desknote
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{autodesktop}\DeskNote"; Filename: "{app}\deskNote.exe"; IconFilename: "{app}\deskNote.exe"; Comment: "DeskNote 记事本"; Tasks: desknote
; 使用启动文件夹快捷方式，避免写入 Run 注册表时被安全软件拦截（错误码 5）
Name: "{userstartup}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: startup

[Registry]
; Explorer desktop background menu (runtime-registered; purge on uninstall).
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.new_fence"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.region_fence"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.refresh_fences"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\DeskTidy.show_window"; Flags: uninsdeletekey dontcreatekey
; Legacy Run-key autostart (current builds prefer Startup folder shortcuts).
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#MyAppName}"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Desktidy"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "桌面整理（私人版）"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: binary; ValueName: "{#MyAppName}"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: binary; ValueName: "Desktidy"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: binary; ValueName: "桌面整理（私人版）"; Flags: uninsdeletevalue dontcreatekey

[UninstallDelete]
; Runtime-created Startup shortcuts (installer task + legacy desktop-guard cleanup).
Type: files; Name: "{userstartup}\{#MyAppName}.lnk"
Type: files; Name: "{userstartup}\{#MyAppName}-桌面守护.lnk"
Type: files; Name: "{userstartup}\Desktidy.lnk"
Type: files; Name: "{userstartup}\Desktidy-桌面守护.lnk"
; Legacy Chinese product name shortcuts from older installs.
Type: files; Name: "{userstartup}\桌面整理（私人版）.lnk"
Type: files; Name: "{userstartup}\桌面整理（私人版）-桌面守护.lnk"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]
var
  GPurgeUserData: Boolean;

function StopDeskTidyAt(const WorkDir: String; const PurgeUserData: Boolean): Boolean;
var
  ResultCode: Integer;
  ExePath: String;
  Args: String;
begin
  Result := True;
  ExePath := WorkDir + '\{#MyAppExeName}';
  if FileExists(ExePath) then
  begin
    if PurgeUserData then
      Args := '--uninstall-cleanup --purge-userdata'
    else
      Args := '--uninstall-cleanup';
    Exec(ExePath, Args, WorkDir, SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
  end;
end;

function StopDeskTidyProcesses(const UseAppDir: Boolean; const PurgeUserData: Boolean): Boolean;
var
  ResultCode: Integer;
  WorkDir: String;
  LegacyDir: String;
begin
  Result := True;
  // {app} is not initialized during InitializeSetup — only use it on uninstall.
  if UseAppDir then
  begin
    StopDeskTidyAt(ExpandConstant('{app}'), PurgeUserData);
  end
  else
  begin
    WorkDir := ExpandConstant('{localappdata}\Programs\{#MyAppName}');
    StopDeskTidyAt(WorkDir, PurgeUserData);
    // Previous product folder names (Desktidy / Chinese).
    LegacyDir := ExpandConstant('{localappdata}\Programs\Desktidy');
    if LegacyDir <> WorkDir then
      StopDeskTidyAt(LegacyDir, PurgeUserData);
    LegacyDir := ExpandConstant('{localappdata}\Programs\桌面整理（私人版）');
    if LegacyDir <> WorkDir then
      StopDeskTidyAt(LegacyDir, PurgeUserData);
  end;
  // Belt-and-suspenders if a process is still holding files.
  Exec('taskkill.exe', '/F /IM DeskTidy.exe /T', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Exec('taskkill.exe', '/F /IM deskNote.exe /T', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Sleep(400);
end;

procedure DeleteStartupShortcuts();
var
  StartupDir: String;
begin
  // Always purge Startup links — even when DeskTidy.exe is already gone
  // (manual delete / failed cleanup) so login does not relaunch the app.
  StartupDir := ExpandConstant('{userstartup}');
  DeleteFile(StartupDir + '\{#MyAppName}.lnk');
  DeleteFile(StartupDir + '\{#MyAppName}-桌面守护.lnk');
  DeleteFile(StartupDir + '\Desktidy.lnk');
  DeleteFile(StartupDir + '\Desktidy-桌面守护.lnk');
  DeleteFile(StartupDir + '\桌面整理（私人版）.lnk');
  DeleteFile(StartupDir + '\桌面整理（私人版）-桌面守护.lnk');
end;

procedure DeleteUserDataFolders();
var
  DataDir: String;
  AppDir: String;
begin
  // Belt-and-suspenders after {app} is removed — Python may have already wiped these.
  DataDir := GetEnv('USERPROFILE') + '\.desktidy';
  if DirExists(DataDir) then
    DelTree(DataDir, True, True, True);

  AppDir := ExpandConstant('{app}');
  if DirExists(AppDir + '\笔记') then
    DelTree(AppDir + '\笔记', True, True, True);
  if DirExists(AppDir + '\录屏') then
    DelTree(AppDir + '\录屏', True, True, True);
  if DirExists(AppDir + '\logs') then
    DelTree(AppDir + '\logs', True, True, True);

  // Desktop shortcuts (installer-created icons are usually removed by Inno).
  DeleteFile(ExpandConstant('{autodesktop}\{#MyAppName}.lnk'));
  DeleteFile(ExpandConstant('{userdesktop}\{#MyAppName}.lnk'));
  DeleteFile(ExpandConstant('{userdesktop}\Desktidy.lnk'));
  DeleteFile(ExpandConstant('{userdesktop}\桌面整理（私人版）.lnk'));
  DeleteFile(ExpandConstant('{autodesktop}\DeskNote.lnk'));
  DeleteFile(ExpandConstant('{userdesktop}\DeskNote.lnk'));
  DeleteFile(ExpandConstant('{autodesktop}\deskNote.lnk'));
  DeleteFile(ExpandConstant('{userdesktop}\deskNote.lnk'));
  // Start Menu DeskNote (group may already be gone; still try).
  DeleteFile(ExpandConstant('{group}\DeskNote.lnk'));
  DeleteFile(ExpandConstant('{userprograms}\{#MyAppName}\DeskNote.lnk'));
  DeleteFile(ExpandConstant('{group}\deskNote.lnk'));
  DeleteFile(ExpandConstant('{userprograms}\{#MyAppName}\deskNote.lnk'));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  // Market pattern: do not block the wizard with a pre-UI stop.
  // Older builds launched the installed DeskTidy for shell cleanup before any
  // page painted (PyInstaller+Qt cold start, and up to ~20s waiting on a live
  // instance). CloseApplications=yes still uses Restart Manager; this path is
  // a fast hard stop so file replace cannot fail. Full shell cleanup stays on
  // uninstall only.
  NeedsRestart := False;
  Result := '';
  Exec('taskkill.exe', '/F /IM DeskTidy.exe /T', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Exec('taskkill.exe', '/F /IM deskNote.exe /T', '', SW_HIDE,
       ewWaitUntilTerminated, ResultCode);
  Sleep(400);
end;

function InitializeUninstall(): Boolean;
begin
  GPurgeUserData :=
    MsgBox(
      '是否清除 DeskTidy 的本地数据？' + #13#10 + #13#10 +
      '确认后将删除：' + #13#10 +
      '• 用户配置目录 %USERPROFILE%\.desktidy' + #13#10 +
      '  （设置、分区存储、布局快照、壁纸缓存、笔记草稿等）' + #13#10 +
      '• 安装目录下的「笔记」「录屏」文件夹（默认路径）' + #13#10 + #13#10 +
      '自定义的笔记 / 录屏 / 会议纪要目录不会自动删除，' + #13#10 +
      '清除完成后会提示路径，请自行决定是否手动删除。' + #13#10 + #13#10 +
      '选择「否」可保留全部数据，以便重装后恢复。',
      mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES;

  StopDeskTidyProcesses(True, GPurgeUserData);
  DeleteStartupShortcuts();
  if GPurgeUserData then
    DeleteUserDataFolders();
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    DeleteStartupShortcuts();
    if GPurgeUserData then
      DeleteUserDataFolders();
    // Files are gone; still try to clear leftover processes.
    Exec('taskkill.exe', '/F /IM DeskTidy.exe /T', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
    Exec('taskkill.exe', '/F /IM deskNote.exe /T', '', SW_HIDE,
         ewWaitUntilTerminated, ResultCode);
  end;
end;

[Messages]
chinesesimplified.WelcomeLabel2=这将在您的计算机上安装 [name/ver]。%n%nDeskTidy 是一款 Windows 桌面整理工具，支持自动分类、桌面分区、快捷启动栏和区域截图等功能。
chinesesimplified.SetupAppTitle=安装
chinesesimplified.SetupWindowTitle=安装 - {#MyAppName}
chinesesimplified.SelectDirLabel3=安装程序将安装 [name] 到下列文件夹。
chinesesimplified.SelectTasksDesc=请选择附加任务：
chinesesimplified.FinishedHeadingLabel=安装完成
chinesesimplified.FinishedLabel=安装程序已完成 [name] 的安装。
chinesesimplified.FinishedLabelNoIcons=安装程序已完成 [name] 的安装。
chinesesimplified.ButtonNext=下一步(&N) >
chinesesimplified.ButtonInstall=安装(&I)
chinesesimplified.ButtonFinish=完成(&F)
chinesesimplified.ButtonCancel=取消
chinesesimplified.ButtonBack=< 上一步(&B)
