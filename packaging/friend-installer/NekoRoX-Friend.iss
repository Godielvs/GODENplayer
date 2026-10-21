#ifndef PayloadRoot
  #error PayloadRoot must be defined by the build script
#endif
#ifndef FoobarInstaller
  #error FoobarInstaller must be defined by the build script
#endif
#ifndef OutputDir
  #error OutputDir must be defined by the build script
#endif
#ifndef CoreSha256
  #error CoreSha256 must be defined by the build script
#endif
#ifndef WorkerSha256
  #error WorkerSha256 must be defined by the build script
#endif
#ifndef CrtSha256
  #error CrtSha256 must be defined by the build script
#endif
#ifndef ShaderSha256
  #error ShaderSha256 must be defined by the build script
#endif
#ifndef SpectrumSha256
  #error SpectrumSha256 must be defined by the build script
#endif
#ifndef ColumnsSha256
  #error ColumnsSha256 must be defined by the build script
#endif
#ifndef BuildVersion
  #error BuildVersion must come from the frozen release manifest
#endif
#ifndef GodenIcon
  #define GodenIcon "..\brand\goden.ico"
#endif

[Setup]
AppId={code:InstallerAppId}
CreateUninstallRegKey=not QaMode
UsePreviousAppDir=not QaMode
UsePreviousLanguage=no
AppName=GODENplayer Beta
AppVersion={#BuildVersion}
AppVerName=GODENplayer Beta {#BuildVersion}
AppPublisher=Goden Project
AppPublisherURL=https://github.com/Godie-dev/GODENplayer
AppSupportURL=https://github.com/Godie-dev/GODENplayer/issues
DefaultDirName={localappdata}\Programs\Goden
DefaultGroupName=Goden
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
WizardStyle=modern dynamic
Compression=lzma2/ultra64
CompressionThreads=1
LZMANumBlockThreads=1
SolidCompression=yes
ArchiveExtraction=full
OutputDir={#OutputDir}
OutputBaseFilename=GODENplayer-Beta-Setup-x64
LicenseFile=NOTICE.txt
ChangesAssociations=yes
CloseApplications=yes
CloseApplicationsFilter={app}\app\foobar2000.exe
SetupLogging=yes
SetupIconFile={#GodenIcon}
UninstallDisplayIcon={app}\goden.ico

[Files]
Source: "{#FoobarInstaller}"; Flags: dontcopy noencryption
; An upgrade is staged from the live installation in PrepareToInstall. This
; frozen, privacy-scanned seed fills only absent settings; managed component
; binaries are overlaid. A clean install never reads another local profile.
Source: "{#PayloadRoot}\profile\*"; DestDir: "{app}\app.next\profile"; Excludes: "user-components-x64\*"; Flags: ignoreversion recursesubdirs createallsubdirs onlyifdoesntexist
Source: "{#PayloadRoot}\profile\user-components-x64\*"; DestDir: "{app}\app.next\profile\user-components-x64"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadRoot}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#GodenIcon}"; DestDir: "{app}"; DestName: "goden.ico"; Flags: ignoreversion

[Icons]
Name: "{group}\GODENplayer"; Filename: "{app}\app\foobar2000.exe"; WorkingDir: "{app}\app"; Comment: "GODENplayer · foobar2000"; IconFilename: "{app}\goden.ico"; AppUserModelID: "Goden.GODENplayer.x64"; Check: not QaMode
Name: "{group}\Uninstall GODENplayer"; Filename: "{uninstallexe}"; Check: not QaMode
Name: "{autodesktop}\GODENplayer"; Filename: "{app}\app\foobar2000.exe"; WorkingDir: "{app}\app"; IconFilename: "{app}\goden.ico"; Tasks: desktopicon; AppUserModelID: "Goden.GODENplayer.x64"; Check: not QaMode

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

; Registry ownership is implemented in [Code]. It records an immutable
; baseline and restores/removes a value only while current == installed.
; Generic foobar App Paths, Applications and UserChoice keys are never touched.

[Run]
Filename: "{app}\app\foobar2000.exe"; Description: "Launch GODENplayer"; Flags: nowait postinstall skipifsilent; Check: CanLaunchInstalledApp

[InstallDelete]
Type: files; Name: "{userprograms}\Goden\Goden foobar2000.lnk"; Check: not QaMode
Type: files; Name: "{userprograms}\Goden\foobar2000.lnk"; Check: not QaMode
Type: files; Name: "{userprograms}\Goden\Uninstall Goden.lnk"; Check: not QaMode
Type: files; Name: "{userdesktop}\Goden foobar2000.lnk"; Check: not QaMode

[UninstallDelete]
Type: filesandordirs; Name: "{app}\app"
Type: filesandordirs; Name: "{app}\app.previous"
Type: filesandordirs; Name: "{app}\app.next"
Type: filesandordirs; Name: "{app}\app.previous.old"
Type: filesandordirs; Name: "{app}\licenses"
Type: dirifempty; Name: "{app}"

[Code]
var
  MusicFolderPage: TInputQueryWizardPage;
  HeaderIdentityPage: TInputOptionWizardPage;
  SignatureColorPage: TInputQueryWizardPage;
  CoverPrivacyPage, StatisticsPage: TInputOptionWizardPage;
  ExistingInstall: Boolean;
  RepairedBrokenSeed: Boolean;
  StagingApp: String;
  RegistryStatePath: String;
  TransactionCommitted: Boolean;

function MoveFileExW(
  lpExistingFileName, lpNewFileName: String;
  dwFlags: Cardinal): Boolean;
  external 'MoveFileExW@kernel32.dll stdcall';

const
  MOVEFILE_WRITE_THROUGH = $8;

function QaMode: Boolean;
begin
  Result := ExpandConstant('{param:QA_MODE|0}') = '1';
end;

function CanLaunchInstalledApp: Boolean;
begin
  Result := TransactionCommitted and not QaMode;
end;

function GetCustomSetupExitCode: Integer;
begin
  // Exceptions from ssPostInstall are reported but do not change Inno's
  // default success exit code. An incomplete app swap must be observable.
  Result := 0;
  if not TransactionCommitted then Result := 12;
end;

// QA remains uninstallable but cannot reuse production uninstall logs or
// write the personal installation's Add/Remove Programs registration.
function InstallerAppId(Param: String): String;
begin
  Result := '{F057FE11-3642-4D2C-A872-71CC72DB81AA}';
  if QaMode then
    Result := Result + '-QA-' + Copy(GetSHA256OfString(
      Lowercase(ExpandConstant('{param:DIR|isolated}'))), 1, 16);
end;

function RecoveryBlocker(const Root: String): String;
begin
  Result := '';
  if FileExists(AddBackslash(Root) + 'install-recovery.pending') or
     DirExists(AddBackslash(Root) + 'app.rollback.pending') or
     (not FileExists(AddBackslash(Root) + 'app\foobar2000.exe') and
       (DirExists(AddBackslash(Root) + 'app\profile') or
        DirExists(AddBackslash(Root) + 'app.previous') or
        DirExists(AddBackslash(Root) + 'app.previous.old'))) then
    Result := 'A recoverable installation transaction is pending. ' +
      'Restore the retained application before installing or uninstalling. ' +
      'No recovery directory was removed.';
end;

function Quote(const Value: String): String;
begin
  Result := '"' + Value + '"';
end;

function RemoveTreeRobust(const Target: String): Boolean;
var
  EmptyTree, Arguments: String;
  ResultCode: Integer;
begin
  Result := True;
  if not DirExists(Target) then exit;
  if DelTree(Target, True, True, True) then exit;

  // Inno's recursive deletion can still encounter legacy MAX_PATH trees.
  // Robocopy is Unicode/long-path aware and is available on every supported
  // Windows version. Mirroring a known-empty temp directory empties only the
  // exact transaction directory passed by this installer.
  EmptyTree := ExpandConstant('{tmp}\goden-empty-tree');
  if DirExists(EmptyTree) then DelTree(EmptyTree, True, True, True);
  if not ForceDirectories(EmptyTree) then
  begin
    Result := False;
    exit;
  end;
  Arguments := Quote(EmptyTree) + ' ' + Quote(Target) +
    ' /MIR /R:2 /W:1 /XJ /NFL /NDL /NJH /NJS';
  ResultCode := 16;
  if not Exec(ExpandConstant('{sys}\robocopy.exe'), Arguments, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) or (ResultCode > 7) then
  begin
    Result := False;
    exit;
  end;
  RemoveDir(Target);
  Result := not DirExists(Target);
end;

function BooleanJson(const Value: Boolean): String;
begin
  if Value then Result := 'true' else Result := 'false';
end;

function PowerShellSingleQuoted(const Value: String): String;
begin
  Result := Value;
  StringChangeEx(Result, '''', '''''', True);
  Result := '''' + Result + '''';
end;

function IsProcessRunningAtPath(
  const Target, ProcessName: String): Boolean;
var
  PowerShell, CommandLine, Script: String;
  ResultCode: Integer;
begin
  PowerShell := ExpandConstant(
    '{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Script := '$p = @(Get-Process -Name ' +
    PowerShellSingleQuoted(ProcessName) + ' -ErrorAction ' +
    'SilentlyContinue | Where-Object { $_.Path -and $_.Path.Equals(' +
    PowerShellSingleQuoted(Target) +
    ', [StringComparison]::OrdinalIgnoreCase) }); ' +
    'if ($p.Count -gt 0) { exit 1 }; exit 0';
  CommandLine := '-NoLogo -NoProfile -NonInteractive -Command ' + Quote(Script);
  ResultCode := 2;
  if not Exec(PowerShell, CommandLine, '', SW_HIDE,
      ewWaitUntilTerminated, ResultCode) then
  begin
    Result := True;
    exit;
  end;
  Result := ResultCode <> 0;
end;

function CloseLiveApplication: Boolean;
var
  LiveExe, WorkerExe: String;
  Attempt, ResultCode: Integer;
begin
  LiveExe := ExpandConstant('{app}\app\foobar2000.exe');
  WorkerExe := ExpandConstant(
    '{app}\app\profile\user-components-x64\foo_nekorox\nekorox-analysis.exe');

  Attempt := 1;
  while FileExists(LiveExe) and
        IsProcessRunningAtPath(LiveExe, 'foobar2000') and
        (Attempt <= 5) do
  begin
    ResultCode := -1;
    Exec(LiveExe, '/exit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(400 * Attempt);
    Attempt := Attempt + 1;
  end;
  if FileExists(LiveExe) and
     IsProcessRunningAtPath(LiveExe, 'foobar2000') then
  begin
    Result := False;
    exit;
  end;

  // The analysis worker normally exits with the host. Wait for that bounded
  // shutdown rather than moving a still-mapped executable into rollback.
  Attempt := 1;
  while FileExists(WorkerExe) and
        IsProcessRunningAtPath(WorkerExe, 'nekorox-analysis') and
        (Attempt <= 20) do
  begin
    Sleep(250);
    Attempt := Attempt + 1;
  end;
  Result := not (FileExists(WorkerExe) and
    IsProcessRunningAtPath(WorkerExe, 'nekorox-analysis'));
end;

function InstallRootsOverlap(const First, Second: String): Boolean;
var
  A, B: String;
begin
  Result := False;
  if (First = '') or (Second = '') then exit;
  A := AddBackslash(ExpandFileName(First));
  B := AddBackslash(ExpandFileName(Second));
  Result := (CompareText(Copy(A, 1, Length(B)), B) = 0) or
    (CompareText(Copy(B, 1, Length(A)), A) = 0);
end;

function QaTargetAllowed(const Target, DefaultRoot, RegisteredRoot: String): Boolean;
begin
  Result := (Target <> '') and not InstallRootsOverlap(Target, DefaultRoot) and
    not InstallRootsOverlap(Target, RegisteredRoot);
  if Result then
    Result := not FileExists(AddBackslash(Target) + 'registry-ownership.ini');
end;

function InitializeSetup: Boolean;
var
  Target, RegisteredRoot: String;
begin
  Result := True;
  if not QaMode then exit;
  Target := ExpandConstant('{param:DIR|}');
  RegisteredRoot := '';
  RegQueryStringValue(HKCU,
    'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
    '{F057FE11-3642-4D2C-A872-71CC72DB81AA}_is1',
    'InstallLocation', RegisteredRoot);
  Result := QaTargetAllowed(Target,
    ExpandConstant('{localappdata}\Programs\Goden'), RegisteredRoot);
  if not Result then
    Log('QA requires an explicit disposable /DIR outside the default or registered personal installation and their parent/child folders.');
end;

procedure SnapshotRegistryValue(
  const Id, Subkey, ValueName: String);
var
  Value: String;
begin
  if RegQueryStringValue(HKCU, Subkey, ValueName, Value) then
  begin
    SetIniString('baseline-exists', Id, '1', RegistryStatePath);
    SetIniString('baseline-value', Id, Value, RegistryStatePath);
  end
  else
    SetIniString('baseline-exists', Id, '0', RegistryStatePath);
end;

procedure InstallOwnedRegistryValue(
  const Id, Subkey, ValueName, Value: String;
  const CaptureBaseline: Boolean);
begin
  if CaptureBaseline then SnapshotRegistryValue(Id, Subkey, ValueName);
  RegWriteStringValue(HKCU, Subkey, ValueName, Value);
  SetIniString('installed-value', Id, Value, RegistryStatePath);
end;

procedure InstallOwnedRegistry;
var
  AppModelKey, ProgIdKey, ExePath, IconPath, Command: String;
  CaptureBaseline: Boolean;
begin
  if QaMode then exit;
  RegistryStatePath := ExpandConstant('{app}\registry-ownership.ini');
  CaptureBaseline := not FileExists(RegistryStatePath);
  AppModelKey := 'Software\Classes\AppUserModelId\Goden.GODENplayer.x64';
  ProgIdKey := 'Software\Classes\Goden.Audio';
  ExePath := ExpandConstant('{app}\app\foobar2000.exe');
  IconPath := ExpandConstant('{app}\goden.ico');
  Command := Quote(ExePath) + ' /show /nekorox-open "%1"';
  InstallOwnedRegistryValue('aumid-display', AppModelKey, 'DisplayName',
    'GODENplayer', CaptureBaseline);
  InstallOwnedRegistryValue('aumid-icon', AppModelKey, 'IconUri', IconPath,
    CaptureBaseline);
  InstallOwnedRegistryValue('aumid-command', AppModelKey, 'RelaunchCommand',
    Quote(ExePath), CaptureBaseline);
  InstallOwnedRegistryValue('aumid-relaunch-name', AppModelKey,
    'RelaunchDisplayNameResource', 'GODENplayer', CaptureBaseline);
  InstallOwnedRegistryValue('aumid-relaunch-icon', AppModelKey,
    'RelaunchIconResource', IconPath, CaptureBaseline);
  InstallOwnedRegistryValue('progid-name', ProgIdKey, '',
    'GODENplayer audio file', CaptureBaseline);
  InstallOwnedRegistryValue('progid-icon', ProgIdKey + '\DefaultIcon', '',
    IconPath, CaptureBaseline);
  InstallOwnedRegistryValue('progid-command',
    ProgIdKey + '\shell\open\command', '', Command, CaptureBaseline);
  SetIniString('ownership', 'owner', 'Goden.GODENplayer.x64',
    RegistryStatePath);
end;

procedure RestoreOwnedRegistryValue(
  const Id, Subkey, ValueName: String);
var
  Current, Installed, Baseline: String;
begin
  Installed := GetIniString('installed-value', Id, '', RegistryStatePath);
  if (Installed = '') or
     not RegQueryStringValue(HKCU, Subkey, ValueName, Current) or
     (CompareText(Current, Installed) <> 0) then
    exit;
  if GetIniString('baseline-exists', Id, '0', RegistryStatePath) = '1' then
  begin
    Baseline := GetIniString('baseline-value', Id, '', RegistryStatePath);
    RegWriteStringValue(HKCU, Subkey, ValueName, Baseline);
  end
  else
    RegDeleteValue(HKCU, Subkey, ValueName);
end;

procedure RestoreOwnedRegistry;
var
  AppModelKey, ProgIdKey: String;
begin
  if QaMode then exit;
  RegistryStatePath := ExpandConstant('{app}\registry-ownership.ini');
  if not FileExists(RegistryStatePath) then exit;
  AppModelKey := 'Software\Classes\AppUserModelId\Goden.GODENplayer.x64';
  ProgIdKey := 'Software\Classes\Goden.Audio';
  RestoreOwnedRegistryValue('aumid-display', AppModelKey, 'DisplayName');
  RestoreOwnedRegistryValue('aumid-icon', AppModelKey, 'IconUri');
  RestoreOwnedRegistryValue('aumid-command', AppModelKey, 'RelaunchCommand');
  RestoreOwnedRegistryValue('aumid-relaunch-name', AppModelKey,
    'RelaunchDisplayNameResource');
  RestoreOwnedRegistryValue('aumid-relaunch-icon', AppModelKey,
    'RelaunchIconResource');
  RestoreOwnedRegistryValue('progid-name', ProgIdKey, '');
  RestoreOwnedRegistryValue('progid-icon', ProgIdKey + '\DefaultIcon', '');
  RestoreOwnedRegistryValue('progid-command',
    ProgIdKey + '\shell\open\command', '');
  RegDeleteKeyIfEmpty(HKCU, ProgIdKey + '\shell\open\command');
  RegDeleteKeyIfEmpty(HKCU, ProgIdKey + '\shell\open');
  RegDeleteKeyIfEmpty(HKCU, ProgIdKey + '\shell');
  RegDeleteKeyIfEmpty(HKCU, ProgIdKey + '\DefaultIcon');
  RegDeleteKeyIfEmpty(HKCU, ProgIdKey);
  RegDeleteKeyIfEmpty(HKCU, AppModelKey);
  DeleteFile(RegistryStatePath);
end;

function SeedStagingFromLive(const LiveApp, NextApp: String;
  var Failure: String): Boolean;
var
  ResultCode: Integer;
  Started: Boolean;
begin
  Failure := '';
  Result := True;
  if not DirExists(LiveApp) then exit;
  ResultCode := -1;
  Started := Exec(ExpandConstant('{sys}\robocopy.exe'),
    Quote(LiveApp) + ' ' + Quote(NextApp) +
    ' /E /COPY:DAT /DCOPY:T /R:2 /W:1 /XJ /NFL /NDL /NJH /NJS' +
    ' /XD ' + Quote(AddBackslash(LiveApp) + 'profile\crash reports') +
    ' /XF running nobadshutdown startup.incomplete',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := Started and (ResultCode >= 0) and (ResultCode <= 7);
  if not Result then
  begin
    if Started then Failure := 'Robocopy exit code ' + IntToStr(ResultCode)
    else Failure := 'Robocopy could not start (Windows error ' +
      IntToStr(ResultCode) + ')';
    Log('Staging copy failed: ' + Failure);
    Log('Staging source: ' + LiveApp + '; destination: ' + NextApp);
  end;
end;

function InitializeUninstall: Boolean;
var
  Blocker, Profile, Destination, Partial, Stamp: String;
  KeepProfile: Boolean;
  Suffix, CopyCode: Integer;
begin
  Blocker := RecoveryBlocker(ExpandConstant('{app}'));
  if (Blocker = '') and not CloseLiveApplication then
    Blocker := 'GODENplayer is still running. Close the player before uninstalling; ' +
      'the profile and installation were not changed.';
  if Blocker = '' then
  begin
    Profile := ExpandConstant('{app}\app\profile');
    if DirExists(Profile) then
    begin
      if QaMode then KeepProfile := True
      else KeepProfile := SuppressibleMsgBox(
        'Keep your GODENplayer profile, playlists, database and DSP settings?' + #13#10 +
        'A recoverable backup is created either way.',
        mbConfirmation, MB_YESNO, IDYES) = IDYES;
      Stamp := GetDateTimeString('yyyymmdd-hhnnss', '-', ':');
      if KeepProfile then Destination := ExpandConstant('{app}\profile-data')
      else Destination := ExpandConstant('{app}\profile-backups\profile-') + Stamp;
      Suffix := 0;
      while DirExists(Destination) do
      begin
        Suffix := Suffix + 1;
        Destination := ExpandConstant('{app}\profile-backups\profile-') +
          Stamp + '-' + IntToStr(Suffix);
      end;
      Partial := Destination + '.partial';
      while DirExists(Partial) do
      begin
        Suffix := Suffix + 1;
        Partial := Destination + '.partial-' + IntToStr(Suffix);
      end;
      // Copy before allowing Inno to delete anything. A late uninstall event
      // exception is not a reliable veto; cancelling later only leaves a backup.
      if not ForceDirectories(Partial) or not Exec(
          ExpandConstant('{sys}\robocopy.exe'),
          Quote(Profile) + ' ' + Quote(Partial) +
            ' /E /COPY:DAT /DCOPY:T /R:1 /W:1 /XJ /NFL /NDL /NJH /NJS',
          '', SW_HIDE, ewWaitUntilTerminated, CopyCode) or (CopyCode > 7) or
          not RenameFile(Partial, Destination) then
        Blocker := 'The profile could not be copied to its recoverable backup. ' +
          'Uninstall was stopped before deleting application files.';
    end;
  end;
  Result := Blocker = '';
  if not Result then
  begin
    Log(Blocker);
    SuppressibleMsgBox(Blocker, mbError, MB_OK, IDOK);
  end;
end;

function PayloadHashMatches(const Path, ExpectedHash: String): Boolean;
begin
  Result := FileExists(Path) and
    (CompareText(GetSHA256OfFile(Path), ExpectedHash) = 0);
end;

procedure RestoreFailedCommit(const LiveApp, PendingApp: String);
begin
  if DirExists(LiveApp) and not DirExists(StagingApp) then
    if not MoveFileExW(LiveApp, StagingApp, MOVEFILE_WRITE_THROUGH) then
      RaiseException('Recovery could not move the candidate. Previous application remains at: ' + PendingApp);
  if DirExists(PendingApp) and not DirExists(LiveApp) then
    if not MoveFileExW(PendingApp, LiveApp, MOVEFILE_WRITE_THROUGH) then
      RaiseException('Recovery could not restore the previous application. Retained backup: ' + PendingApp);
  if DirExists(PendingApp) then
    RaiseException('Recovery destination is occupied. Original application remains retained at: ' + PendingApp);
  // Reached only after all required restore renames succeeded. If a rename
  // throws, the durable marker prevents a retry/uninstall deleting the backup.
  if FileExists(ExpandConstant('{app}\install-recovery.pending')) and
     not DeleteFile(ExpandConstant('{app}\install-recovery.pending')) then
    RaiseException('Recovery completed but its marker could not be cleared. Retained installation requires inspection.');
end;

function CommitStaging: Boolean;
var
  LiveApp, PreviousApp, PendingApp: String;
  CorePath, WorkerPath, CrtPath, ShaderPath, SpectrumPath, ColumnsPath: String;
  HadLive: Boolean;
begin
  LiveApp := ExpandConstant('{app}\app');
  PreviousApp := ExpandConstant('{app}\app.previous');
  PendingApp := ExpandConstant('{app}\app.rollback.pending');
  Result := False;
  if DirExists(PendingApp) then
    exit;

  CorePath := AddBackslash(StagingApp) +
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll';
  WorkerPath := AddBackslash(StagingApp) +
    'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe';
  CrtPath := AddBackslash(StagingApp) +
    'profile\user-components-x64\foo_nekorox\goden-crt-render.exe';
  ShaderPath := AddBackslash(StagingApp) +
    'profile\user-components-x64\foo_nekorox\goden-shader-render.exe';
  SpectrumPath := AddBackslash(StagingApp) +
    'profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll';
  ColumnsPath := AddBackslash(StagingApp) +
    'profile\user-components-x64\foo_ui_columns\foo_ui_columns.dll';
  if not FileExists(AddBackslash(StagingApp) + 'portable_mode_enabled') or
     not PayloadHashMatches(AddBackslash(StagingApp) + 'foobar2000.exe',
      '653CC120C146AAAE9E6DB9B6F19E5A1588407B8940BC1521F0CED739FF8924B0') or
     not PayloadHashMatches(CorePath, '{#CoreSha256}') or
     not PayloadHashMatches(WorkerPath, '{#WorkerSha256}') or
     not PayloadHashMatches(CrtPath, '{#CrtSha256}') or
     not PayloadHashMatches(ShaderPath, '{#ShaderSha256}') or
     not PayloadHashMatches(SpectrumPath, '{#SpectrumSha256}') or
     not PayloadHashMatches(ColumnsPath, '{#ColumnsSha256}') then
    exit;

  HadLive := DirExists(LiveApp);
  // The current live tree is still untouched here and becomes the new
  // rollback. Removing an older rollback before the swap avoids retaining two
  // full profile copies while preserving a recoverable live application.
  if DirExists(PreviousApp) and
     not RemoveTreeRobust(PreviousApp) then
    exit;
  if not SaveStringToFile(ExpandConstant('{app}\install-recovery.pending'),
      'Application swap in progress. Retain all recovery directories.', False) then
    RaiseException('Could not record recovery state; live application remains untouched.');
  try
  if HadLive and
     not MoveFileExW(LiveApp, PendingApp, MOVEFILE_WRITE_THROUGH) then
  begin
    // No live rename occurred; the original application is still in place.
    if not DeleteFile(ExpandConstant('{app}\install-recovery.pending')) then
      RaiseException('Live application retained, but the recovery marker could not be cleared.');
    exit;
  end;
  if not MoveFileExW(StagingApp, LiveApp, MOVEFILE_WRITE_THROUGH) then
  begin
    RestoreFailedCommit(LiveApp, PendingApp);
    exit;
  end;

  CorePath := AddBackslash(LiveApp) +
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll';
  WorkerPath := AddBackslash(LiveApp) +
    'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe';
  CrtPath := AddBackslash(LiveApp) +
    'profile\user-components-x64\foo_nekorox\goden-crt-render.exe';
  ShaderPath := AddBackslash(LiveApp) +
    'profile\user-components-x64\foo_nekorox\goden-shader-render.exe';
  SpectrumPath := AddBackslash(LiveApp) +
    'profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll';
  ColumnsPath := AddBackslash(LiveApp) +
    'profile\user-components-x64\foo_ui_columns\foo_ui_columns.dll';
  if not FileExists(AddBackslash(LiveApp) + 'portable_mode_enabled') or
     not PayloadHashMatches(AddBackslash(LiveApp) + 'foobar2000.exe',
      '653CC120C146AAAE9E6DB9B6F19E5A1588407B8940BC1521F0CED739FF8924B0') or
     not PayloadHashMatches(CorePath, '{#CoreSha256}') or
     not PayloadHashMatches(WorkerPath, '{#WorkerSha256}') or
     not PayloadHashMatches(CrtPath, '{#CrtSha256}') or
     not PayloadHashMatches(ShaderPath, '{#ShaderSha256}') or
     not PayloadHashMatches(SpectrumPath, '{#SpectrumSha256}') or
     not PayloadHashMatches(ColumnsPath, '{#ColumnsSha256}') then
  begin
    RestoreFailedCommit(LiveApp, PendingApp);
    exit;
  end;

  if HadLive then
  begin
    if not MoveFileExW(PendingApp, PreviousApp, MOVEFILE_WRITE_THROUGH) then
    begin
      RestoreFailedCommit(LiveApp, PendingApp);
      exit;
    end;
  end;
  Result := True;
  except
    RestoreFailedCommit(LiveApp, PendingApp);
    RaiseException('Application transaction failed; previous application was restored or retained. ' + GetExceptionMessage);
  end;
end;

procedure WriteTransactionManifest;
var
  ExePath, ComponentPath, WorkerPath, CrtPath, ShaderPath, SpectrumPath, ColumnsPath, Manifest: String;
begin
  ExePath := AddBackslash(StagingApp) + 'foobar2000.exe';
  ComponentPath := ExpandConstant(
    '{app}\app.next\profile\user-components-x64\foo_nekorox\foo_nekorox.dll');
  WorkerPath := ExpandConstant(
    '{app}\app.next\profile\user-components-x64\foo_nekorox\nekorox-analysis.exe');
  CrtPath := ExpandConstant(
    '{app}\app.next\profile\user-components-x64\foo_nekorox\goden-crt-render.exe');
  ShaderPath := ExpandConstant(
    '{app}\app.next\profile\user-components-x64\foo_nekorox\goden-shader-render.exe');
  SpectrumPath := ExpandConstant(
    '{app}\app.next\profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll');
  ColumnsPath := ExpandConstant(
    '{app}\app.next\profile\user-components-x64\foo_ui_columns\foo_ui_columns.dll');
  Manifest := '{' + #13#10 +
    '  "schema": "goden.install-transaction.v2",' + #13#10 +
    '  "buildVersion": "{#BuildVersion}",' + #13#10 +
    '  "installerSeedRevision": 2,' + #13#10 +
    '  "repairedLegacyBrokenSeed": ' + BooleanJson(RepairedBrokenSeed) + ',' + #13#10 +
    '  "profilePreserved": ' + BooleanJson(ExistingInstall) + ',' + #13#10 +
    '  "staging": "app.next",' + #13#10 +
    '  "rollback": "app.previous",' + #13#10 +
    '  "foobarSha256": "' + GetSHA256OfFile(ExePath) + '",' + #13#10 +
    '  "componentSha256": "' + GetSHA256OfFile(ComponentPath) + '",' + #13#10 +
    '  "analysisWorkerSha256": "' + GetSHA256OfFile(WorkerPath) + '",' + #13#10 +
    '  "crtRendererSha256": "' + GetSHA256OfFile(CrtPath) + '",' + #13#10 +
    '  "shaderRendererSha256": "' + GetSHA256OfFile(ShaderPath) + '",' + #13#10 +
    '  "spectrumAnalyzerSha256": "' + GetSHA256OfFile(SpectrumPath) + '",' + #13#10 +
    '  "columnsUiSha256": "' + GetSHA256OfFile(ColumnsPath) + '",' + #13#10 +
    '  "validated": true' + #13#10 + '}' + #13#10;
  if not SaveStringToFile(ExpandConstant('{app}\install-transaction.next.json'),
      Manifest, False) then RaiseException('Could not prepare transaction evidence; live application remains untouched.');
end;

procedure AppendUInt32LE(var Data: AnsiString; Value: Cardinal);
begin
  Data := Data + AnsiString(Chr(Integer(Value and $FF))) +
    AnsiString(Chr(Integer((Value shr 8) and $FF))) +
    AnsiString(Chr(Integer((Value shr 16) and $FF))) +
    AnsiString(Chr(Integer((Value shr 24) and $FF)));
end;

procedure AppendByte(var Data: AnsiString; Value: Byte);
begin
  Data := Data + AnsiString(Chr(Integer(Value)));
end;

procedure WriteLibraryFolders(const MusicFolder: String);
var
  Data, EncodedPath: AnsiString;
  Target, FilterTarget, Normalized: String;
begin
  Normalized := AddBackslash(RemoveQuotes(MusicFolder));
  EncodedPath := UTF8Encode(Normalized);
  Data := '';
  AppendUInt32LE(Data, 1);
  AppendUInt32LE(Data, Length(EncodedPath));
  Data := Data + EncodedPath;
  AppendUInt32LE(Data, 0);
  Target := AddBackslash(StagingApp) + 'profile\library-v2.0\folders';
  ForceDirectories(ExtractFileDir(Target));
  if not SaveStringToFile(Target, Data, False) then
    RaiseException('Could not write the foobar2000 media-library root file.');

  { foobar2000 loads folders and filters as one media-library settings pair. }
  Data := '';
  AppendByte(Data, 1);
  AppendByte(Data, 0);
  AppendByte(Data, 0);
  AppendByte(Data, 0);
  AppendByte(Data, 42);
  AppendByte(Data, 5);
  AppendByte(Data, 0);
  AppendByte(Data, 0);
  AppendByte(Data, 0);
  AppendByte(Data, 42);
  AppendByte(Data, 46);
  AppendByte(Data, 67);
  AppendByte(Data, 85);
  AppendByte(Data, 69);
  FilterTarget := AddBackslash(StagingApp) + 'profile\library-v2.0\filters';
  if not SaveStringToFile(FilterTarget, Data, False) then
    RaiseException('Could not write the foobar2000 media-library filter file.');
end;

procedure WriteHeaderWordmarkChoice(const ShowWordmark: Boolean);
var
  Choice, Target: String;
begin
  if ShowWordmark then
    Choice := 'show'
  else
    Choice := 'hide';
  Target := ExpandConstant(
    '{app}\app.next\profile\nekorox\header-wordmark.install-choice');
  ForceDirectories(ExtractFileDir(Target));
  if not SaveStringToFile(Target, Choice + #13#10, False) then
    RaiseException('Could not save the Goden header identity choice.');
end;

function ParseRgbValue(Value: String; var Red, Green, Blue: Integer): Boolean;
var
  FirstComma, SecondComma: Integer;
begin
  StringChangeEx(Value, ' ', '', True);
  FirstComma := Pos(',', Value);
  SecondComma := 0;
  if FirstComma > 0 then
    SecondComma := Pos(',', Copy(Value, FirstComma + 1, Length(Value))) +
      FirstComma;
  Result := (FirstComma > 1) and (SecondComma > FirstComma + 1) and
    (SecondComma < Length(Value));
  if not Result then exit;
  Red := StrToIntDef(Copy(Value, 1, FirstComma - 1), -1);
  Green := StrToIntDef(
    Copy(Value, FirstComma + 1, SecondComma - FirstComma - 1), -1);
  Blue := StrToIntDef(Copy(Value, SecondComma + 1, Length(Value)), -1);
  Result := (Red >= 0) and (Red <= 255) and
    (Green >= 0) and (Green <= 255) and
    (Blue >= 0) and (Blue <= 255);
end;

procedure WriteSignatureColorChoice(const Value: String);
var
  Red, Green, Blue: Integer;
  Target, Normalized: String;
begin
  if not ParseRgbValue(Value, Red, Green, Blue) then
    RaiseException('The fallback signature colour is not a valid RGB value.');
  Normalized := IntToStr(Red) + ',' + IntToStr(Green) + ',' + IntToStr(Blue);
  Target := ExpandConstant(
    '{app}\app.next\profile\nekorox\fallback-accent.install-choice');
  ForceDirectories(ExtractFileDir(Target));
  if not SaveStringToFile(Target, Normalized + #13#10, False) then
    RaiseException('Could not save the Goden fallback signature colour.');
end;

function StatisticsProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := (Progress <= 4194304) and (ProgressMax <= 4194304);
end;

procedure TryInstallStatistics;
var
  ArchivePath, ExtractPath, DllPath, Target, Candidate: String;
begin
  Target := AddBackslash(StagingApp) + 'profile\user-components-x64\foo_playcount\foo_playcount.dll';
  Candidate := Target + '.next';
  // Keep an existing component and its statistics intact during upgrades.
  if FileExists(Target) or not StatisticsPage.Values[0] or
      (ExpandConstant('{param:STATISTICS_OFFLINE|0}') = '1') then exit;
  try
    DownloadTemporaryFile(
      'https://www.foobar2000.org/getcomponent/31048989d7d7873a52cb371510188ac4/foo_playcount.fb2k-component',
      'foo_playcount-3.1.11.zip',
      '4E47C44B8624544EF618FC02999C1FBBC212534507888A871625B83E949E4014',
      @StatisticsProgress);
    ArchivePath := ExpandConstant('{tmp}\foo_playcount-3.1.11.zip');
    ExtractPath := ExpandConstant('{tmp}\playcount-3.1.11');
    // Exact archive bytes are pinned; the inspected ZIP has three DLL entries,
    // no links or traversal paths. Only the separately pinned AMD64 file ships.
    ExtractArchive(ArchivePath, ExtractPath, '', True, nil);
    DllPath := AddBackslash(ExtractPath) + 'x64\foo_playcount.dll';
    if not PayloadHashMatches(DllPath,
      '68718DBF463E8DC1402261F11B0CDC39BD6E5291971DEAA8B91ECD4AF9CD7466') then
      RaiseException('Playback Statistics x64 checksum mismatch.');
    ForceDirectories(ExtractFileDir(Target));
    if not FileCopy(DllPath, Candidate, False) then
      RaiseException('Playback Statistics could not be copied.');
    if not PayloadHashMatches(Candidate,
      '68718DBF463E8DC1402261F11B0CDC39BD6E5291971DEAA8B91ECD4AF9CD7466') then
      RaiseException('Copied Playback Statistics checksum mismatch.');
    if not RenameFile(Candidate, Target) then
      RaiseException('Playback Statistics could not be committed.');
  except
    DeleteFile(Candidate);
    Log('Optional Playback Statistics unavailable: ' + GetExceptionMessage);
    SuppressibleMsgBox('Playback Statistics could not be installed. GODENplayer will still work. ' +
      'Retry this installer later or install Playback Statistics from foobar2000.org/components/view/foo_playcount. ' +
      'Existing statistics were preserved.', mbInformation, MB_OK, IDOK);
  end;
end;

procedure WriteCoverPrivacyChoice;
var
  Choice, Target: String;
begin
  Choice := 'automatic-v1-disabled';
  if CoverPrivacyPage.Values[0] and
      (not WizardSilent or (ExpandConstant('{param:ALLOW_PUBLIC_COVERS|0}') = '1')) then
    Choice := 'automatic-v1-accepted';
  Target := AddBackslash(StagingApp) + 'profile\nekorox\cover-provider.install-choice';
  ForceDirectories(ExtractFileDir(Target));
  if not SaveStringToFile(Target, Choice + #13#10, False) then
    RaiseException('Could not save the cover privacy choice.');
end;

procedure InitializeWizard;
begin
  MusicFolderPage := CreateInputQueryPage(wpSelectDir,
    'Choose a music folder (optional)',
    'GODENplayer can start with an empty library.',
    'Choose a local music root, or leave this empty and add folders later in Preferences.');
  MusicFolderPage.Add('Music folder (optional):', False);
  MusicFolderPage.Values[0] := '';

  HeaderIdentityPage := CreateInputOptionPage(MusicFolderPage.ID,
    'Choose the header identity',
    'Decide how prominently Goden appears in the player.',
    'The full identity uses the hand-drawn pixel wordmark and artwork colors. You can change this later from the Goden menu.',
    True, True);
  HeaderIdentityPage.Add('Show GODENplayer in the header');
  HeaderIdentityPage.Add('Keep the header minimal');
  HeaderIdentityPage.Values[0] := True;

  SignatureColorPage := CreateInputQueryPage(HeaderIdentityPage.ID,
    'Choose the fallback signature colour',
    'Used when a track has no usable artwork palette.',
    'Enter RGB values from 0 to 255. Goden cyan is 0, 216, 208; burnt sienna is 233, 116, 81. You can change this later from the Goden menu.');
  SignatureColorPage.Add('RGB (R, G, B):', False);
  SignatureColorPage.Values[0] := '0, 216, 208';
  CoverPrivacyPage := CreateInputOptionPage(SignatureColorPage.ID,
    'Discord cover privacy', 'Choose before the first upload.',
    'Automatic cover sharing uses Uguu first (public thumbnails expire after 3 hours), ' +
    'then Catbox (public, persistent upload) and MusicBrainz / Cover Art Archive lookup (artist and album). ' +
    'Only a re-encoded thumbnail is uploaded; image delivery also uses wsrv.nl. ' +
    'Turning this off stops new uploads and lookups; it does not delete previous uploads. Text Presence works independently.',
    False, False);
  CoverPrivacyPage.Add('Show album covers automatically in Discord (external services)');
  CoverPrivacyPage.Values[0] := True;
  StatisticsPage := CreateInputOptionPage(CoverPrivacyPage.ID,
    'Optional Playback Statistics', 'Download from the official component site.',
    'Install Playback Statistics 3.1.11 x64 if it is missing. Internet is needed; failure does not block installation. Existing statistics are kept.',
    False, False);
  StatisticsPage.Add('Download Playback Statistics from foobar2000.org');
  StatisticsPage.Values[0] := True;
end;

function HasExistingInstallAtRoot(const Root: String): Boolean;
begin
  Result := DirExists(AddBackslash(Root) + 'app') and
    FileExists(AddBackslash(Root) + 'app\foobar2000.exe');
end;

function ProductionRootConflict(const SelectedRoot, RegisteredRoot: String): String;
begin
  Result := '';
  if (RegisteredRoot = '') or
     (CompareText(RemoveBackslashUnlessRoot(ExpandFileName(SelectedRoot)),
       RemoveBackslashUnlessRoot(ExpandFileName(RegisteredRoot))) = 0) then exit;
  if HasExistingInstallAtRoot(RegisteredRoot) or
     FileExists(AddBackslash(RegisteredRoot) + 'unins000.exe') then
    Result := 'GODENplayer is already installed for this Windows account in ' +
      RegisteredRoot + '. Update that installation, or uninstall it with a ' +
      'profile backup before choosing a different folder. Other foobar2000 ' +
      'installations are independent and do not need to be removed.';
end;

function ManagedRootConflict(const SelectedRoot: String): String;
var
  RegisteredRoot: String;
begin
  Result := '';
  if QaMode then exit;
  if RegQueryStringValue(HKCU,
      'Software\Microsoft\Windows\CurrentVersion\Uninstall\' +
      '{F057FE11-3642-4D2C-A872-71CC72DB81AA}_is1',
      'InstallLocation', RegisteredRoot) then
    Result := ProductionRootConflict(SelectedRoot, RegisteredRoot);
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := ((PageID = MusicFolderPage.ID) or
    (PageID = HeaderIdentityPage.ID) or
    (PageID = SignatureColorPage.ID) or (PageID = CoverPrivacyPage.ID)) and
    HasExistingInstallAtRoot(WizardDirValue());
end;

procedure AppendReadySection(var Memo: String; const Section, NewLine: String);
begin
  if Section = '' then exit;
  if Memo <> '' then Memo := Memo + NewLine + NewLine;
  Memo := Memo + Section;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
var
  Choices: String;
begin
  Result := '';
  AppendReadySection(Result, MemoUserInfoInfo, NewLine);
  AppendReadySection(Result, MemoDirInfo, NewLine);
  AppendReadySection(Result, MemoTypeInfo, NewLine);
  AppendReadySection(Result, MemoComponentsInfo, NewLine);
  AppendReadySection(Result, MemoGroupInfo, NewLine);
  AppendReadySection(Result, MemoTasksInfo, NewLine);
  if HasExistingInstallAtRoot(WizardDirValue()) then
    Choices := 'Your library folders and appearance settings will be kept.'
  else
  begin
    Choices := 'Player preferences:' + NewLine + Space;
    if MusicFolderPage.Values[0] = '' then
      Choices := Choices + 'Music folders: add later in Preferences'
    else
      Choices := Choices + 'Music folder: ' + MusicFolderPage.Values[0];
    Choices := Choices + NewLine + Space + 'Header: ';
    if HeaderIdentityPage.Values[0] then
      Choices := Choices + 'GODENplayer identity'
    else
      Choices := Choices + 'Minimal';
    Choices := Choices + NewLine + Space + 'Fallback colour: ' +
      SignatureColorPage.Values[0];
  end;
  AppendReadySection(Result, Choices, NewLine);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Red, Green, Blue: Integer;
  Conflict: String;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    Conflict := ManagedRootConflict(WizardDirValue());
    if Conflict <> '' then
    begin
      SuppressibleMsgBox(Conflict, mbError, MB_OK, IDOK);
      Result := False;
      exit;
    end;
  end;
  if HasExistingInstallAtRoot(WizardDirValue()) then exit;
  if (CurPageID = MusicFolderPage.ID) and
     (MusicFolderPage.Values[0] <> '') and
     not DirExists(MusicFolderPage.Values[0]) then
  begin
    SuppressibleMsgBox('Choose an existing music folder or leave it empty.',
      mbError, MB_OK, IDOK);
    Result := False;
  end;
  if CurPageID = SignatureColorPage.ID then
  begin
    if not ParseRgbValue(
        SignatureColorPage.Values[0], Red, Green, Blue) then
    begin
      SuppressibleMsgBox(
        'Enter three values from 0 to 255, for example 233, 116, 81.',
        mbError, MB_OK, IDOK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
  Installer, InstallRoot: String;
begin
  Result := ManagedRootConflict(ExpandConstant('{app}'));
  if Result <> '' then exit;
  ExistingInstall := HasExistingInstallAtRoot(ExpandConstant('{app}'));
  StagingApp := ExpandConstant('{app}\app.next');
  Result := RecoveryBlocker(ExpandConstant('{app}'));
  if Result <> '' then exit;
  if ExistingInstall and not CloseLiveApplication then
  begin
    Result := 'GODENplayer is still running and could not be closed safely. ' +
      'Close the player and retry; the existing installation was not changed.';
    exit;
  end;
  if DirExists(ExpandConstant('{app}\app.previous.old')) and
     not RemoveTreeRobust(ExpandConstant('{app}\app.previous.old')) then
  begin
    Result := 'An obsolete rollback backup could not be removed safely. ' +
      'The existing application was not changed.';
    exit;
  end;
  if DirExists(StagingApp) and not RemoveTreeRobust(StagingApp) then
  begin
    Result := 'The previous staging directory could not be cleared safely. ' +
      'The existing application was not changed.';
    exit;
  end;
  ForceDirectories(StagingApp);
  if ExistingInstall and not SeedStagingFromLive(
      ExpandConstant('{app}\app'), StagingApp, Result) then
  begin
    Result := 'The existing installation could not be copied safely to staging. ' +
      Result + '. Check free space and folder permissions, then retry. ' +
      'The existing application and profile were not changed.';
    exit;
  end;
  Result := '';
  ExtractTemporaryFile(ExtractFileName('{#FoobarInstaller}'));
  Installer := ExpandConstant('{tmp}\') + ExtractFileName('{#FoobarInstaller}');
  if not PayloadHashMatches(Installer,
      '530E6C9DB829874BC250AF038C1D07F8B42FEF61C5DD99F7EDCDE6393A93EE23') then
  begin Result := 'Official installer checksum mismatch.'; exit; end;
  InstallRoot := StagingApp;
  ForceDirectories(InstallRoot);
  // /P is the pinned official installer's portable-mode switch. Merely
  // writing its marker after a standard install is too late to prevent shell
  // registration/shortcuts. NSIS /D must remain last and unquoted, even with
  // spaces; /P also overrides old standard-mode installer.ini on upgrades.
  if not Exec(Installer, '/S /P /D=' + InstallRoot, '', SW_HIDE,
    ewWaitUntilTerminated, ResultCode) then
  begin
    Result := 'The official foobar2000 installer could not be started.';
    exit;
  end;
  if (ResultCode <> 0) or not FileExists(InstallRoot + '\foobar2000.exe') then
  begin
    Result := 'The official foobar2000 installation did not complete successfully.';
    exit;
  end;
  if not PayloadHashMatches(InstallRoot + '\foobar2000.exe',
      '653CC120C146AAAE9E6DB9B6F19E5A1588407B8940BC1521F0CED739FF8924B0') then
  begin Result := 'Installed official foobar2000 executable does not match its pin.'; exit; end;
  if not FileExists(AddBackslash(InstallRoot) + 'portable_mode_enabled') then
    Result := 'The official host did not enter portable mode. The existing installation was not replaced.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  PortableMarker: String;
begin
  if CurStep <> ssPostInstall then exit;
  // Older candidates copied the official standard-mode uninstaller. It does
  // not own GODEN's profile-backup/rollback transaction and must not survive
  // an upgrade as a second, unsafe uninstall route.
  if FileExists(AddBackslash(StagingApp) + 'uninstall.exe') and
     not DeleteFile(AddBackslash(StagingApp) + 'uninstall.exe') then
    RaiseException('Could not retire a legacy host uninstaller; existing application remains untouched.');
  DeleteFile(AddBackslash(StagingApp) + 'components\foo_out_upnp.dll');
  DeleteFile(AddBackslash(StagingApp) + 'components\foo_freedb2.dll');
  PortableMarker := AddBackslash(StagingApp) + 'portable_mode_enabled';
  if not FileExists(PortableMarker) and not SaveStringToFile(PortableMarker, '', False) then
    RaiseException('Could not enable the local portable profile; existing application remains untouched.');
  if not ExistingInstall then
  begin
    if MusicFolderPage.Values[0] <> '' then
      WriteLibraryFolders(MusicFolderPage.Values[0]);
    WriteHeaderWordmarkChoice(HeaderIdentityPage.Values[0]);
    WriteSignatureColorChoice(SignatureColorPage.Values[0]);
    WriteCoverPrivacyChoice;
  end;
  TryInstallStatistics;
  WriteTransactionManifest;
  if not CommitStaging then
    RaiseException('The candidate could not be committed. Existing installation and recovery directories were retained.');
  if not MoveFileExW(ExpandConstant('{app}\install-transaction.next.json'),
      ExpandConstant('{app}\install-transaction.json'), MOVEFILE_WRITE_THROUGH or 1) then
  begin
    RestoreFailedCommit(ExpandConstant('{app}\app'), ExpandConstant('{app}\app.previous'));
    RaiseException('Could not commit transaction evidence; previous application restored or retained.');
  end;
  if not DeleteFile(ExpandConstant('{app}\install-recovery.pending')) then
    RaiseException('Application committed but recovery marker could not be cleared. Retained backup requires inspection.');
  InstallOwnedRegistry;
  TransactionCommitted := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RestoreOwnedRegistry;
  if CurUninstallStep = usPostUninstall then
  begin
    // Handle long legacy paths that Inno's initial recursive removal may leave.
    if not RemoveTreeRobust(ExpandConstant('{app}\app')) or
       not RemoveTreeRobust(ExpandConstant('{app}\app.previous')) or
       not RemoveTreeRobust(ExpandConstant('{app}\app.previous.old')) or
       not RemoveTreeRobust(ExpandConstant('{app}\app.next')) then
      Log('Some application files could not be removed. The profile backup is retained.');
  end;
end;
