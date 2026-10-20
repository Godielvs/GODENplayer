param(
    [Parameter(Mandatory=$true)][string]$SourceApp,
    [string]$InstallRoot = "$env:LOCALAPPDATA\Programs\Goden",
    [string]$LegacyInstallRoot = "$env:LOCALAPPDATA\Programs\NekoRoX",
    [switch]$RefreshProfile,
    [switch]$RegistrationOnly,
    [switch]$Launch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = [IO.Path]::GetFullPath($SourceApp).TrimEnd('\')
$root = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')
$legacyRoot = [IO.Path]::GetFullPath($LegacyInstallRoot).TrimEnd('\')
$legacyApp = Join-Path $legacyRoot 'app'
$programs = [IO.Path]::GetFullPath(
    (Join-Path $env:LOCALAPPDATA 'Programs')).TrimEnd('\')
$app = Join-Path $root 'app'
$next = Join-Path $root 'app.next'
$previous = Join-Path $root 'app.previous'
$rollbackPending = Join-Path $root 'app.rollback.pending'
$previousOld = Join-Path $root 'app.previous.old'
$exe = Join-Path $app 'foobar2000.exe'
$repo = Split-Path -Parent $PSScriptRoot
$brandIcon = Join-Path $repo 'packaging\brand\goden.ico'
$installedIcon = Join-Path $root 'goden.ico'
$sourceExe = Join-Path $source 'foobar2000.exe'
$sourceCore = Join-Path $source (
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll')
$sourceWorker = Join-Path $source (
    'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe')
$sourceShader = Join-Path $source (
    'profile\user-components-x64\foo_nekorox\goden-shader-render.exe')
$sourceSpectrum = Join-Path $source (
    'profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll')
$shortcutGroup = Join-Path $env:APPDATA (
    'Microsoft\Windows\Start Menu\Programs\Goden')
$shortcut = Join-Path $shortcutGroup 'GODENplayer.lnk'
$legacyShortcutAlias = Join-Path $env:APPDATA (
    'Microsoft\Windows\Start Menu\Programs\foobar.lnk')
$shortcutAppUserModelId = 'Goden.GODENplayer.x64'
$primaryShortcutAppUserModelId = $shortcutAppUserModelId
$manifestPath = Join-Path $root 'install-manifest.json'
$script:shortcutBackups = @()
$script:registrationBackup = $null

function Assert-ChildPath([string]$Path, [string]$Parent) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not ($full + '\').StartsWith(
            $parentFull, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escaped its allowed root: $full"
    }
}

function Remove-TreeLongPathSafe([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $extended = if ($Path.StartsWith('\\')) {
        '\\?\UNC\' + $Path.TrimStart('\')
    } else {
        '\\?\' + [IO.Path]::GetFullPath($Path)
    }
    [IO.Directory]::Delete($extended, $true)
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
}

function Assert-SamePayload(
    [string]$Expected,
    [string]$Actual,
    [string]$Label
) {
    if ((-not (Test-Path -LiteralPath $Expected -PathType Leaf)) -or
            (-not (Test-Path -LiteralPath $Actual -PathType Leaf))) {
        throw "Promotion payload is missing ($Label): $Expected / $Actual"
    }
    $expectedItem = Get-Item -LiteralPath $Expected
    $actualItem = Get-Item -LiteralPath $Actual
    if ($expectedItem.Length -ne $actualItem.Length -or
            (Get-Sha256 $Expected) -ne (Get-Sha256 $Actual)) {
        throw "Promotion payload mismatch ($Label)."
    }
}

function Get-ShortcutTarget([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try {
        $shell = New-Object -ComObject WScript.Shell
        return [IO.Path]::GetFullPath(
            $shell.CreateShortcut($Path).TargetPath).TrimEnd('\')
    }
    catch { return $null }
}

function Remove-OldShortcutIfOwned([string]$Path) {
    $target = Get-ShortcutTarget $Path
    if ($null -eq $target) { return }
    foreach ($ownedRoot in @($source, $app, $legacyApp)) {
        if (($target + '\').StartsWith(
                ([IO.Path]::GetFullPath($ownedRoot).TrimEnd('\') + '\'),
                [StringComparison]::OrdinalIgnoreCase)) {
            if ($null -ne $script:registrationBackup) {
                $backup = Join-Path $script:registrationBackup (
                    'shortcut-{0}.lnk' -f $script:shortcutBackups.Count)
                Copy-Item -LiteralPath $Path -Destination $backup -Force
                $script:shortcutBackups += [ordered]@{
                    original = [IO.Path]::GetFullPath($Path)
                    backup = $backup
                }
            }
            Remove-Item -LiteralPath $Path -Force
            return
        }
    }
}

function Backup-RegistryKey(
    [string]$NativeKey,
    [string]$Destination
) {
    if (Test-Path -LiteralPath $Destination) { return }
    & reg.exe export $NativeKey $Destination /y | Out-Null
    if ($LASTEXITCODE -ne 0 -and (Test-Path -LiteralPath $Destination)) {
        Remove-Item -LiteralPath $Destination -Force
    }
}

function New-ShortcutWithAppUserModelId(
    [string]$Path,
    [string]$TargetPath,
    [string]$WorkingDirectory,
    [string]$IconPath,
    [string]$Description,
    [string]$AppUserModelId
) {
    if (-not ('NekoRoXShortcutProperties' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class NekoRoXShortcutProperties {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Win32FindData {
        public uint FileAttributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME CreationTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastAccessTime;
        public System.Runtime.InteropServices.ComTypes.FILETIME LastWriteTime;
        public uint FileSizeHigh;
        public uint FileSizeLow;
        public uint Reserved0;
        public uint Reserved1;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string FileName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 14)]
        public string AlternateFileName;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    private struct PropertyKey {
        public Guid FormatId;
        public uint PropertyId;

        public PropertyKey(Guid formatId, uint propertyId) {
            FormatId = formatId;
            PropertyId = propertyId;
        }
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct PropVariant {
        [FieldOffset(0)] public ushort ValueType;
        [FieldOffset(8)] public IntPtr PointerValue;
    }

    [ComImport]
    [Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IPropertyStore {
        [PreserveSig] int GetCount(out uint propertyCount);
        [PreserveSig] int GetAt(uint propertyIndex, out PropertyKey key);
        [PreserveSig] int GetValue(ref PropertyKey key, out PropVariant value);
        [PreserveSig] int SetValue(ref PropertyKey key, ref PropVariant value);
        [PreserveSig] int Commit();
    }

    [ComImport]
    [Guid("000214F9-0000-0000-C000-000000000046")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IShellLinkW {
        [PreserveSig] int GetPath(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder file,
            int maxPath,
            out Win32FindData findData,
            uint flags);
        [PreserveSig] int GetIDList(out IntPtr itemIdList);
        [PreserveSig] int SetIDList(IntPtr itemIdList);
        [PreserveSig] int GetDescription(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder description,
            int maxName);
        [PreserveSig] int SetDescription(
            [MarshalAs(UnmanagedType.LPWStr)] string description);
        [PreserveSig] int GetWorkingDirectory(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder directory,
            int maxPath);
        [PreserveSig] int SetWorkingDirectory(
            [MarshalAs(UnmanagedType.LPWStr)] string directory);
        [PreserveSig] int GetArguments(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder arguments,
            int maxPath);
        [PreserveSig] int SetArguments(
            [MarshalAs(UnmanagedType.LPWStr)] string arguments);
        [PreserveSig] int GetHotkey(out short hotkey);
        [PreserveSig] int SetHotkey(short hotkey);
        [PreserveSig] int GetShowCmd(out int showCommand);
        [PreserveSig] int SetShowCmd(int showCommand);
        [PreserveSig] int GetIconLocation(
            [Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder iconPath,
            int iconPathLength,
            out int iconIndex);
        [PreserveSig] int SetIconLocation(
            [MarshalAs(UnmanagedType.LPWStr)] string iconPath,
            int iconIndex);
        [PreserveSig] int SetRelativePath(
            [MarshalAs(UnmanagedType.LPWStr)] string path,
            uint reserved);
        [PreserveSig] int Resolve(IntPtr windowHandle, uint flags);
        [PreserveSig] int SetPath(
            [MarshalAs(UnmanagedType.LPWStr)] string path);
    }

    [DllImport("ole32.dll")]
    private static extern int PropVariantClear(ref PropVariant value);

    private const ushort StringValueType = 31;
    private static readonly Guid ShellLinkClassId = new Guid(
        "00021401-0000-0000-C000-000000000046");
    private static readonly PropertyKey AppUserModelIdKey = new PropertyKey(
        new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), 5);

    public static void CreateShortcut(
        string shortcutPath,
        string targetPath,
        string workingDirectory,
        string iconPath,
        string description,
        string appUserModelId) {
        Type shellLinkType = Type.GetTypeFromCLSID(ShellLinkClassId, true);
        object shellLink = Activator.CreateInstance(shellLinkType);
        try {
            IShellLinkW link = (IShellLinkW)shellLink;
            Marshal.ThrowExceptionForHR(link.SetPath(targetPath));
            Marshal.ThrowExceptionForHR(
                link.SetWorkingDirectory(workingDirectory));
            Marshal.ThrowExceptionForHR(link.SetIconLocation(iconPath, 0));
            Marshal.ThrowExceptionForHR(link.SetDescription(description));

            IPropertyStore store = (IPropertyStore)shellLink;
            PropertyKey key = AppUserModelIdKey;
            PropVariant value = new PropVariant {
                ValueType = StringValueType,
                PointerValue = Marshal.StringToCoTaskMemUni(appUserModelId)
            };
            try {
                Marshal.ThrowExceptionForHR(store.SetValue(ref key, ref value));
                Marshal.ThrowExceptionForHR(store.Commit());
            }
            finally {
                PropVariantClear(ref value);
            }

            ((System.Runtime.InteropServices.ComTypes.IPersistFile)shellLink)
                .Save(shortcutPath, false);
        }
        finally {
            Marshal.FinalReleaseComObject(shellLink);
        }
    }

}
'@
    }

    [NekoRoXShortcutProperties]::CreateShortcut(
        $Path,
        $TargetPath,
        $WorkingDirectory,
        $IconPath,
        $Description,
        $AppUserModelId)
    $shell = New-Object -ComObject Shell.Application
    $folder = $shell.NameSpace((Split-Path -Parent $Path))
    $item = $folder.ParseName((Split-Path -Leaf $Path))
    $stored = $item.ExtendedProperty('System.AppUserModel.ID')
    if ($stored -cne $AppUserModelId) {
        throw "Shortcut AppUserModelID verification failed: $Path ($stored)"
    }
}

function Get-FoobarProcesses {
    return @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object { $_.ExecutablePath } |
        Select-Object ProcessId, ExecutablePath)
}

if (-not ($root + '\').StartsWith(
        $programs + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw "The user install must stay below ${programs}: $root"
}
foreach ($path in @($app, $next, $previous, $rollbackPending, $previousOld)) {
    Assert-ChildPath $path $root
}
foreach ($required in @(
        $sourceExe,
        $brandIcon,
        (Join-Path $source 'portable_mode_enabled'),
        $sourceCore,
        $sourceWorker,
        $sourceShader,
        $sourceSpectrum)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Goden promotion input is missing: $required"
    }
}

$running = @(Get-FoobarProcesses)
if ($running.Count -gt 0) {
    $details = ($running | ForEach-Object {
        "$($_.ProcessId): $($_.ExecutablePath)"
    }) -join '; '
    throw "Close every foobar2000 instance before promotion: $details"
}

New-Item -ItemType Directory -Path $root -Force | Out-Null
$existingInstall = Test-Path -LiteralPath $app
$profileSeedApp = if ($existingInstall) {
    $app
} elseif (Test-Path -LiteralPath (Join-Path $legacyApp 'foobar2000.exe')) {
    $legacyApp
} else {
    $null
}
if ($RegistrationOnly -and -not $existingInstall) {
    throw "RegistrationOnly requires an existing Goden install: $app"
}

if (-not $RegistrationOnly) {
if (Test-Path -LiteralPath $next) {
    Remove-TreeLongPathSafe $next
}
New-Item -ItemType Directory -Path $next -Force | Out-Null

$excludedDirectories = @(
    (Join-Path $source 'native-component'),
    (Join-Path $source 'workspace-tools'),
    (Join-Path $source 'profile\crash reports'))
$excludedFiles = @(
    'running',
    'nobadshutdown',
    'startup.incomplete',
    'Start-NekoRoX-Lab.ps1',
    '$R0')

function Invoke-Robocopy([string[]]$Arguments, [string]$Label) {
    & robocopy.exe @Arguments | Out-Null
    if ($LASTEXITCODE -gt 7) {
        throw "$Label failed with robocopy exit code $LASTEXITCODE."
    }
}

if ($null -ne $profileSeedApp -and -not $RefreshProfile) {
    # The installed profile becomes authoritative as soon as it is used.
    # Begin from that complete state, overlay runtime files from the lab, then
    # replace only component payloads. History, playlists, ratings, lyrics,
    # artwork and analysis caches therefore survive future code promotions.
    Invoke-Robocopy @(
        $profileSeedApp, $next,
        '/E', '/COPY:DAT', '/DCOPY:T', '/R:2', '/W:1', '/XJ',
        '/NFL', '/NDL', '/NJH', '/NJS',
        '/XD', (Join-Path $profileSeedApp 'profile\crash reports'),
        '/XF', 'running', 'nobadshutdown', 'startup.incomplete') `
        'Compatible installed-state staging'
    Invoke-Robocopy (@(
        $source, $next,
        '/E', '/COPY:DAT', '/DCOPY:T', '/R:2', '/W:1', '/XJ',
        '/NFL', '/NDL', '/NJH', '/NJS',
        '/XD') + $excludedDirectories + @(
        (Join-Path $source 'profile'),
        '/XF') + $excludedFiles) 'Runtime overlay'
    Invoke-Robocopy @(
        (Join-Path $source 'profile\user-components-x64'),
        (Join-Path $next 'profile\user-components-x64'),
        '/MIR', '/COPY:DAT', '/DCOPY:T', '/R:2', '/W:1', '/XJ',
        '/NFL', '/NDL', '/NJH', '/NJS') 'Component overlay'
} else {
    Invoke-Robocopy (@(
        $source, $next,
        '/E', '/COPY:DAT', '/DCOPY:T', '/R:2', '/W:1', '/XJ',
        '/NFL', '/NDL', '/NJH', '/NJS',
        '/XD') + $excludedDirectories + @('/XF') + $excludedFiles) `
        'Initial Goden staging'
}

# foo_nekorox is intentionally installed as a user component so its profile
# configuration and update contract remain compatible with existing installs.
# A stale copy under app\components is seen by foobar2000 as a built-in
# component with the same name and makes startup report a name clash.
$staleBuiltInComponent = Join-Path $next 'components\foo_nekorox.dll'
if (Test-Path -LiteralPath $staleBuiltInComponent) {
    Remove-Item -LiteralPath $staleBuiltInComponent -Force
}

$stagedExe = Join-Path $next 'foobar2000.exe'
$stagedComponent = Join-Path $next (
    'profile\user-components-x64\foo_nekorox\foo_nekorox.dll')
$stagedWorker = Join-Path $next (
    'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe')
$stagedShader = Join-Path $next (
    'profile\user-components-x64\foo_nekorox\goden-shader-render.exe')
$stagedSpectrum = Join-Path $next (
    'profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll')
foreach ($required in @($stagedExe, $stagedComponent, $stagedWorker, $stagedShader, $stagedSpectrum)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Staged Goden install is incomplete: $required"
    }
}
Assert-SamePayload $sourceCore $stagedComponent 'foo_nekorox.dll'
Assert-SamePayload $sourceWorker $stagedWorker 'nekorox-analysis.exe'
Assert-SamePayload $sourceShader $stagedShader 'goden-shader-render.exe'
Assert-SamePayload $sourceSpectrum $stagedSpectrum 'foo_vis_spectrum_analyzer.dll'

if (Test-Path -LiteralPath $rollbackPending) {
    throw 'A recoverable promotion transaction is already pending. ' +
        "Resolve $rollbackPending before retrying."
}

# app.previous.old belongs to the older three-copy transaction. Remove it
# before touching the live application so a failure cannot strand two full
# profile backups. All paths were constrained to the install root above.
if (Test-Path -LiteralPath $previousOld) {
    Remove-TreeLongPathSafe $previousOld
}

$hadLiveApp = Test-Path -LiteralPath $app
try {
    # The live tree is the rollback for this transaction. An older rollback
    # can be removed while live remains untouched, avoiding a third full copy.
    if (Test-Path -LiteralPath $previous) {
        Remove-TreeLongPathSafe $previous
    }
    if ($hadLiveApp) {
        Move-Item -LiteralPath $app -Destination $rollbackPending
    }
    Move-Item -LiteralPath $next -Destination $app

    Assert-SamePayload $sourceCore (Join-Path $app (
        'profile\user-components-x64\foo_nekorox\foo_nekorox.dll')) `
        'committed foo_nekorox.dll'
    Assert-SamePayload $sourceWorker (Join-Path $app (
        'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe')) `
        'committed nekorox-analysis.exe'
    Assert-SamePayload $sourceShader (Join-Path $app (
        'profile\user-components-x64\foo_nekorox\goden-shader-render.exe')) `
        'committed goden-shader-render.exe'
    Assert-SamePayload $sourceSpectrum (Join-Path $app (
        'profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll')) `
        'committed foo_vis_spectrum_analyzer.dll'

    if ($hadLiveApp) {
        Move-Item -LiteralPath $rollbackPending -Destination $previous
    }
}
catch {
    $failure = $_
    try {
        if ((Test-Path -LiteralPath $app) -and
                (-not (Test-Path -LiteralPath $next))) {
            Move-Item -LiteralPath $app -Destination $next
        }
        if ((Test-Path -LiteralPath $rollbackPending) -and
                (-not (Test-Path -LiteralPath $app))) {
            Move-Item -LiteralPath $rollbackPending -Destination $app
        }
    }
    catch {
        throw "Promotion failed and automatic recovery was incomplete. " +
            "Original error: $failure; recovery error: $_"
    }
    throw $failure
}
}

Copy-Item -LiteralPath $brandIcon -Destination $installedIcon -Force

$registrationBackup = Join-Path $root 'registration-backup'
New-Item -ItemType Directory -Path $registrationBackup -Force | Out-Null
$script:registrationBackup = $registrationBackup
Backup-RegistryKey `
    'HKCU\Software\Microsoft\Windows\CurrentVersion\App Paths\foobar2000.exe' `
    (Join-Path $registrationBackup 'app-paths.reg')
Backup-RegistryKey `
    'HKCU\Software\Classes\Applications\foobar2000.exe' `
    (Join-Path $registrationBackup 'application.reg')
Backup-RegistryKey `
    'HKCU\Software\Classes\NekoRoX.Audio.V3' `
    (Join-Path $registrationBackup 'nekorox-audio.reg')
Backup-RegistryKey `
    'HKCU\Software\Classes\foobar2000.NekoRoX' `
    (Join-Path $registrationBackup 'nekorox-foobar.reg')
New-Item -ItemType Directory -Path $shortcutGroup -Force | Out-Null
$shortcutBackup = Join-Path $registrationBackup 'foobar.user.before.lnk'
if ((Test-Path -LiteralPath $shortcut) -and
        -not (Test-Path -LiteralPath $shortcutBackup)) {
    Copy-Item -LiteralPath $shortcut -Destination $shortcutBackup
}

New-ShortcutWithAppUserModelId `
    $shortcut `
    $exe `
    $app `
    $installedIcon `
    'GODENplayer' `
    $primaryShortcutAppUserModelId
foreach ($oldShortcut in @(
        (Join-Path $shortcutGroup 'Goden foobar2000.lnk'),
        (Join-Path $shortcutGroup 'foobar2000.lnk'),
        $legacyShortcutAlias,
        (Join-Path $env:APPDATA (
            'Microsoft\Windows\Start Menu\Programs\foobar2000.lnk')),
        (Join-Path $env:APPDATA (
            'Microsoft\Windows\Start Menu\Programs\foobar2000 NekoRoX.lnk')))) {
    Remove-OldShortcutIfOwned $oldShortcut
}
$legacyShortcutGroup = Join-Path $env:APPDATA (
    'Microsoft\Windows\Start Menu\Programs\NekoRoX')
if (Test-Path -LiteralPath $legacyShortcutGroup) {
    foreach ($oldShortcut in Get-ChildItem -LiteralPath $legacyShortcutGroup `
            -Filter '*.lnk' -File) {
        Remove-OldShortcutIfOwned $oldShortcut.FullName
    }
}
if ($script:shortcutBackups.Count -gt 0) {
    $script:shortcutBackups | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath (
            Join-Path $registrationBackup 'shortcuts.json') -Encoding UTF8
}

$appPaths = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\App Paths\foobar2000.exe'
New-Item -Path $appPaths -Force | Out-Null
Set-Item -Path $appPaths -Value $exe
New-ItemProperty -Path $appPaths -Name 'Path' -Value $app `
    -PropertyType String -Force | Out-Null

$classes = 'HKCU:\Software\Classes'
$application = Join-Path $classes 'Applications\foobar2000.exe'
New-Item -Path $application -Force | Out-Null
New-ItemProperty -Path $application -Name 'FriendlyAppName' `
    -Value 'GODENplayer' -PropertyType String -Force | Out-Null
$applicationIcon = Join-Path $application 'DefaultIcon'
New-Item -Path $applicationIcon -Force | Out-Null
Set-Item -Path $applicationIcon -Value $installedIcon
$applicationCommand = Join-Path $application 'shell\open\command'
New-Item -Path $applicationCommand -Force | Out-Null
Set-Item -Path $applicationCommand `
    -Value "`"$exe`" /show /nekorox-open `"%1`""

$progId = 'Goden.Audio'
$progIdKey = Join-Path $classes $progId
New-Item -Path $progIdKey -Force | Out-Null
Set-Item -Path $progIdKey -Value 'Goden audio file'
$progIdIcon = Join-Path $progIdKey 'DefaultIcon'
New-Item -Path $progIdIcon -Force | Out-Null
Set-Item -Path $progIdIcon -Value $installedIcon
$progIdCommand = Join-Path $progIdKey 'shell\open\command'
New-Item -Path $progIdCommand -Force | Out-Null
Set-Item -Path $progIdCommand `
    -Value "`"$exe`" /show /nekorox-open `"%1`""

$extensions = @(
    '.aac', '.aiff', '.ape', '.flac', '.m4a', '.mp3', '.mp4',
    '.ogg', '.opus', '.wav', '.wma')
$supportedTypes = Join-Path $application 'SupportedTypes'
New-Item -Path $supportedTypes -Force | Out-Null
foreach ($extension in $extensions) {
    New-ItemProperty -Path $supportedTypes -Name $extension -Value '' `
        -PropertyType String -Force | Out-Null
    $openWith = Join-Path $classes "$extension\OpenWithProgids"
    New-Item -Path $openWith -Force | Out-Null
    New-ItemProperty -Path $openWith -Name $progId -Value '' `
        -PropertyType String -Force | Out-Null
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NekoRoXShellNotify {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(
        uint eventId, uint flags, IntPtr item1, IntPtr item2);
}
'@
[NekoRoXShellNotify]::SHChangeNotify(
    0x08000000, 0x0000, [IntPtr]::Zero, [IntPtr]::Zero)

$openWithManifest = Join-Path $root 'open-with-registration.json'
& (Join-Path $PSScriptRoot 'register-open-with.ps1') `
    -AppPath $exe `
    -IconPath $installedIcon `
    -ManifestPath $openWithManifest

$manifest = [ordered]@{
    schema = 'nekorox.user-install.v1'
    installedUtc = [DateTime]::UtcNow.ToString('o')
    source = $source
    installRoot = $root
    promotionMode = if ($RegistrationOnly) {
        'registration-repair'
    } elseif ($null -ne $profileSeedApp -and -not $RefreshProfile) {
        'runtime-preserve-installed-profile'
    } else {
        'full-profile'
    }
    executable = $exe
    icon = $installedIcon
    shortcut = $shortcut
    previousAvailable = Test-Path -LiteralPath $previous
    files = [ordered]@{
        foobar2000Sha256 = Get-Sha256 $exe
        fooNekoroxSha256 = Get-Sha256 (
            Join-Path $app (
                'profile\user-components-x64\foo_nekorox\foo_nekorox.dll'))
        analysisWorkerSha256 = Get-Sha256 (
            Join-Path $app (
                'profile\user-components-x64\foo_nekorox\nekorox-analysis.exe'))
        shaderRendererSha256 = Get-Sha256 (
            Join-Path $app (
                'profile\user-components-x64\foo_nekorox\goden-shader-render.exe'))
        spectrumAnalyzerSha256 = Get-Sha256 (
            Join-Path $app (
                'profile\user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll'))
    }
    registration = [ordered]@{
        appPaths = $appPaths
        application = $application
        progId = $progId
        shortcutAppUserModelId = $shortcutAppUserModelId
        primaryShortcutAppUserModelId = $primaryShortcutAppUserModelId
        shadowsLegacyMachineShortcut = $false
        openWithManifest = $openWithManifest
        defaultAssociationsChanged = $false
    }
}
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host 'GODENplayer user installation: PASS'
Write-Host "Executable: $exe"
Write-Host "Start menu: $shortcut"
Write-Host "Manifest: $manifestPath"
if ($Launch) {
    Start-Process -FilePath $exe
}
