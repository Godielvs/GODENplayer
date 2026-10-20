param(
    [Parameter(Mandatory)]
    [string]$SetupPath,
    [Parameter(Mandatory)]
    [string]$OfficialInstaller,
    [string]$ReleaseManifest = '',
    [string]$QaRoot = '',
    # Retained for existing callers; consent verification no longer invokes SQLite.
    [string]$SqliteExecutable = 'sqlite3.exe',
    [string]$StatisticsPackage = '',
    [int]$ExpectedCollectionPreset = -1,
    [string]$ExpectedCollectionPresetId = '',
    [switch]$SkipRuntimeUi
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if ($ExpectedCollectionPresetId) {
    $catalogue = @(Get-Content -LiteralPath (Join-Path $repo 'src\component\collection\catalogue.json') -Raw | ConvertFrom-Json)
    $resolved = -1
    for ($index = 0; $index -lt $catalogue.Count; ++$index) {
        if ($catalogue[$index].id -ceq $ExpectedCollectionPresetId) { $resolved = $index; break }
    }
    if ($resolved -lt 0 -or ($ExpectedCollectionPreset -ge 0 -and $ExpectedCollectionPreset -ne $resolved)) {
        throw 'Expected visual ID is absent or conflicts with the requested current catalogue index.'
    }
    $ExpectedCollectionPreset = $resolved
}
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\')
if (-not $QaRoot) {
    $QaRoot = Join-Path $artifacts 'installer-transaction-qa'
}
$root = [IO.Path]::GetFullPath($QaRoot).TrimEnd('\')
if ($root.Equals($artifacts, [StringComparison]::OrdinalIgnoreCase) -or
        -not (($root + '\').StartsWith($artifacts + '\',
        [StringComparison]::OrdinalIgnoreCase))) {
    throw "Installer QA root must stay inside artifacts: $root"
}
$setup = [IO.Path]::GetFullPath($SetupPath)
if (-not (Test-Path -LiteralPath $setup -PathType Leaf)) {
    throw "Installer is missing: $setup"
}
$officialInstaller = [IO.Path]::GetFullPath($OfficialInstaller)
if (-not (Test-Path -LiteralPath $officialInstaller -PathType Leaf)) {
    throw "Official foobar2000 installer is missing: $officialInstaller"
}
$releaseManifestPath = if ($ReleaseManifest) {
    [IO.Path]::GetFullPath($ReleaseManifest)
} else {
    Join-Path $repo 'packaging\component-manifest.json'
}
if (-not (Test-Path -LiteralPath $releaseManifestPath -PathType Leaf)) {
    throw "Release manifest is missing: $releaseManifestPath"
}
$releaseData = Get-Content -LiteralPath $releaseManifestPath -Raw |
    ConvertFrom-Json
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if (!$SkipRuntimeUi -and -not (Test-Path -LiteralPath $probe -PathType Leaf)) {
    throw "Installer first-boot probe is missing: $probe"
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class GodenInstallerWindowProbe {
    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder value, int capacity);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder value, int capacity);

    public static string[] DescribeVisible(uint targetProcessId) {
        var result = new List<string>();
        EnumWindows((window, parameter) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            if (processId == targetProcessId && IsWindowVisible(window)) {
                var className = new StringBuilder(256);
                var title = new StringBuilder(512);
                GetClassName(window, className, className.Capacity);
                GetWindowText(window, title, title.Capacity);
                result.Add(className + "|" + title);
            }
            return true;
        }, IntPtr.Zero);
        return result.ToArray();
    }
}

public static class GodenInstallerFileFault {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFileW(
        string path, uint access, uint share, IntPtr security, uint creation,
        uint flags, IntPtr template);
    public static Microsoft.Win32.SafeHandles.SafeFileHandle LockDirectory(string path) {
        var handle = CreateFileW(path, 1, 3, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (handle.IsInvalid) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        return handle;
    }
}
'@

function Assert-NoConfigurationDialog([int]$ProcessId) {
    $windows = @([GodenInstallerWindowProbe]::DescribeVisible(
        [uint32]$ProcessId))
    $dialogs = @($windows | Where-Object {
        $_ -match '^#32770\|' -or
        $_ -match '\|(Information|Error|Quick setup(?: -| –) Columns UI)$'
    })
    if ($dialogs.Count -gt 0) {
        throw 'Clean first boot opened a configuration dialog: ' +
            ($dialogs -join ', ')
    }
}

# A QA transaction must never alter the production uninstall registration.
$productionUninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{F057FE11-3642-4D2C-A872-71CC72DB81AA}_is1'
function Get-ProductionRegistration {
    if (!(Test-Path -LiteralPath $productionUninstallKey)) {return '<absent>'}
    $key=Get-Item -LiteralPath $productionUninstallKey
    $values=foreach($name in ($key.GetValueNames() | Sort-Object)) {
        [ordered]@{name=$name;kind=$key.GetValueKind($name).ToString();value=$key.GetValue($name)}
    }
    return ConvertTo-Json -InputObject @($values) -Depth 5 -Compress
}
$productionRegistrationBefore=Get-ProductionRegistration
. (Join-Path $PSScriptRoot 'installer-coexistence.ps1')
$foobarCoexistenceBefore=Get-FoobarCoexistenceState
function Assert-ProductionRegistration {
    if ((Get-ProductionRegistration) -cne $productionRegistrationBefore) {throw 'QA altered the production uninstall registration.'}
    if ((Get-FoobarCoexistenceState).sha256 -cne $foobarCoexistenceBefore.sha256) {
        throw 'QA altered unrelated foobar2000 registration or shortcuts. Stop and inspect; no automatic registry rewrite was attempted.'
    }
}

function Invoke-Setup([string[]]$Arguments) {
    $process = Start-Process -FilePath $setup -ArgumentList $Arguments `
        -Wait -PassThru -WindowStyle Hidden
    Assert-ProductionRegistration
    if ($process.ExitCode -ne 0) {
        throw "Installer exited with $($process.ExitCode)."
    }
    Assert-OfficialRuntime (Join-Path $root 'app')
}

function Assert-OfficialRuntime([string]$Application) {
    if (Test-Path -LiteralPath (Join-Path $Application 'uninstall.exe')) {
        throw 'Nested standard-mode foobar uninstaller survived in a portable GODEN installation.'
    }
    if (!(Test-Path -LiteralPath (Join-Path $Application 'portable_mode_enabled'))) {
        throw 'Official host did not select portable mode.'
    }
    $runtimeFiles = @(Get-ChildItem -LiteralPath $officialReferenceRoot -File -Filter '*.dll') +
        @(Get-ChildItem -LiteralPath (Join-Path $officialReferenceRoot 'runtime') -Recurse -File)
    if ($runtimeFiles.Count -lt 10 -or !(Test-Path -LiteralPath (
            Join-Path $officialReferenceRoot 'runtime/runtime.manifest'))) {
        throw 'Official app-local runtime reference is incomplete.'
    }
    foreach ($file in $runtimeFiles) {
        $relative = [IO.Path]::GetRelativePath($officialReferenceRoot, $file.FullName)
        $installed = Join-Path $Application $relative
        if (!(Test-Path -LiteralPath $installed -PathType Leaf) -or
                (Get-FileHash -LiteralPath $installed).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
            throw "Official app-local runtime changed or is absent: $relative"
        }
    }
}

function Assert-SetupFailurePreservesApp([string]$LockedPath, [string[]]$Arguments) {
    $beforeApp = Get-TreeHash (Join-Path $root 'app')
    $beforeEvidence = (Get-FileHash (Join-Path $root 'install-transaction.json')).Hash
    Write-Output ('Fault case: ' + (Split-Path -Leaf $LockedPath))
    $handle = if (Test-Path -LiteralPath $LockedPath -PathType Container) {
        [GodenInstallerFileFault]::LockDirectory($LockedPath)
    } else {
        [IO.File]::Open($LockedPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    }
    try {
        $process = Start-Process -FilePath $setup -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
        if ($process.ExitCode -eq 0) {throw 'Installer unexpectedly succeeded despite a locked transaction target.'}
    } finally {$handle.Dispose(); Assert-ProductionRegistration}
    if ((Get-TreeHash (Join-Path $root 'app')) -ne $beforeApp -or
            (Get-FileHash (Join-Path $root 'install-transaction.json')).Hash -ne $beforeEvidence) {
        throw 'Failed installation changed the live application/profile or committed evidence.'
    }
    if (Test-Path (Join-Path $root 'app.rollback.pending')) {throw 'Failed transaction left the previous application pending instead of restoring it.'}
    if (Test-Path (Join-Path $root 'install-recovery.pending')) {throw 'Successful recovery left an unresolved marker.'}
}

function Get-TreeHash([string]$Path) {
    $records = foreach ($file in Get-ChildItem -LiteralPath $Path -File -Recurse |
            Sort-Object FullName) {
        $relative = $file.FullName.Substring($Path.Length).TrimStart('\')
        "$relative|$($file.Length)|$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($records -join "`n"))
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    }
    finally { $sha.Dispose() }
}

function Assert-ManifestPayload(
    [string]$Path,
    $ManifestEntry,
    [string]$Label
) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Installed $Label is missing: $Path"
    }
    $item = Get-Item -LiteralPath $Path
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if ($item.Length -ne [long]$ManifestEntry.bytes -or
            $hash -ne [string]$ManifestEntry.sha256) {
        throw "Installed $Label does not match the frozen release manifest."
    }
    return $hash
}

function Read-PinnedComponentSettings([string]$ProfilePath) {
    # This component targets the legacy cfg_var API: the host stores these
    # values as GUID/uint32-length/payload records, not configInts rows.
    # Read only. Never repair or rewrite a user configuration to satisfy QA.
    $path = Join-Path $ProfilePath 'configuration\foo_nekorox.dll.cfg'
    $file = Get-Item -LiteralPath $path -ErrorAction Stop
    if ($file.Length -gt 1048576) { throw 'Component settings exceed the consent probe bound.' }
    $data = [IO.File]::ReadAllBytes($path)
    # The pinned 2.25.10 host precedes this component's SDK stream with a
    # 16-byte file signature, 16 opaque bytes and a uint32 payload length.
    # The opaque bytes vary across valid saves; do not pin a captured value.
    # This observed host envelope is not a general configuration format API.
    if ($data.Length -lt 36 -or
            [Guid]::new([byte[]]$data[0..15]) -ne [Guid]'b2223dd1-15c4-46f5-8031-96e692bc1e20' -or
            [BitConverter]::ToUInt32($data, 32) -ne $data.Length - 36) {
        throw 'Unexpected pinned-host component settings envelope.'
    }
    $values = @{}
    $offset = 36
    while ($offset -lt $data.Length) {
        if ($data.Length - $offset -lt 20) { throw 'Truncated component settings record.' }
        $guidBytes = [byte[]]::new(16)
        [Array]::Copy($data, $offset, $guidBytes, 0, 16)
        $name = ([Guid]::new($guidBytes)).ToString()
        $length = [BitConverter]::ToUInt32($data, $offset + 16)
        $offset += 20
        if ($length -gt $data.Length - $offset) { throw 'Truncated component settings payload.' }
        if ($values.ContainsKey($name)) { throw 'Duplicate component settings record.' }
        $payload = [byte[]]::new([int]$length)
        [Array]::Copy($data, $offset, $payload, 0, [int]$length)
        $values[$name] = $payload
        $offset += [int]$length
    }
    return $values
}

function Assert-SilentCoverConsent([string]$ProfilePath) {
    $values = Read-PinnedComponentSettings $ProfilePath
    $provider = $values['9448016b-56f5-4d7f-83b2-925991b68801']
    $disclosure = $values['9448016b-56f5-4d7f-83b2-925991b68802']
    $consumed = $values['9448016b-56f5-4d7f-83b2-925991b68805']
    if ($null -eq $provider -or $provider.Length -ne 4 -or
            [BitConverter]::ToUInt32($provider, 0) -ne 1 -or
            $null -eq $disclosure -or $disclosure.Length -ne 1 -or $disclosure[0] -ne 0 -or
            $null -eq $consumed -or $consumed.Length -ne 1 -or $consumed[0] -ne 1) {
        throw 'Silent cover denial was not consumed and persisted after boot.'
    }
}

function Assert-PresenceEnabledByDefault([string]$ProfilePath) {
    $values = Read-PinnedComponentSettings $ProfilePath
    $enabled = $values['b7d97e82-389a-48a5-920c-eb4ea5c038c9']
    if ($null -eq $enabled -or $enabled.Length -ne 1 -or $enabled[0] -ne 1) {
        throw 'Clean first boot did not persist enabled text Presence.'
    }
    return $true
}

function Assert-CleanFirstBoot([string]$App,
    [ValidateSet('first-boot','rollback-boot')][string]$Phase) {
    # Collection diagnostics require both opt-in switches in the child process.
    # Keep them scoped to this disposable host, never the caller or installer.
    $priorHooks = [Environment]::GetEnvironmentVariable('NEKOROX_QA_HOOKS', 'Process')
    $priorProfile = [Environment]::GetEnvironmentVariable('NEKOROX_QA_PROFILE', 'Process')
    try {
        $env:NEKOROX_QA_HOOKS = '1'
        $env:NEKOROX_QA_PROFILE = '1'
        # The pinned official host advertises /show. An unsupported /new
        # argument can itself open Error and invalidate first-boot observation.
        # Portable copies already have independent application/profile roots.
        $process = Start-Process -FilePath $App `
            -ArgumentList '/show' `
            -WorkingDirectory (Split-Path -Parent $App) -PassThru -WindowStyle Normal
    } finally {
        [Environment]::SetEnvironmentVariable('NEKOROX_QA_HOOKS', $priorHooks, 'Process')
        [Environment]::SetEnvironmentVariable('NEKOROX_QA_PROFILE', $priorProfile, 'Process')
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $lastProbe = ''
    try {
        do {
            Start-Sleep -Milliseconds 200
            $process.Refresh()
            if ($process.HasExited) {
                throw 'Clean-installed GODENplayer exited during first boot.'
            }
            Assert-NoConfigurationDialog $process.Id
            if ($process.MainWindowTitle -match '^(Information|Error)$') {
                throw "Clean first boot opened an error dialog: $($process.MainWindowTitle)"
            }
            $previousPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $lastProbe = (& $probe $App 'window-state' '--background' 2>&1 |
                    ForEach-Object { $_.ToString() }) -join "`n"
                $probeExit = $LASTEXITCODE
            }
            finally { $ErrorActionPreference = $previousPreference }
            if ($probeExit -eq 0 -and $lastProbe -match 'Main visible=true iconic=false') { break }
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($probeExit -ne 0 -or $lastProbe -notmatch 'Main visible=true iconic=false') {
            throw "Clean first boot never reached a visible GODENplayer workspace: $lastProbe"
        }
        Assert-NoConfigurationDialog $process.Id
        $lastProbe | Set-Content -LiteralPath (Join-Path $root ($Phase+'-window.txt'))

        if ($ExpectedCollectionPreset -ge 0) {
            $selection = (& $probe $App 'shell-collection-state' '--background' 2>&1 |
                ForEach-Object { $_.ToString() }) -join "`n"
            if ($LASTEXITCODE -ne 0 -or $selection -notmatch
                    "selected=1 preset=$ExpectedCollectionPreset style=4(?:\s|$)") {
                throw "Clean first boot has the wrong visual: $selection"
            }
            $selection | Set-Content -LiteralPath (Join-Path $root ($Phase+'-visual.txt'))
        }

        $dpiState = (& $probe $App 'window-dpi-state' '--background' 2>&1 |
            ForEach-Object { $_.ToString() }) -join "`n"
        if ($LASTEXITCODE -ne 0 -or
                $dpiState -notmatch 'pmv2=1') {
            throw "Clean first boot is not Per-Monitor V2 aware: $dpiState"
        }
        $dpiState | Set-Content -LiteralPath (Join-Path $root ($Phase+'-dpi.txt'))

        $importStatusPath = Join-Path (Split-Path -Parent $App) 'profile/nekorox/legacy-import-status.txt'
        $importDeadline = [DateTime]::UtcNow.AddSeconds(20)
        $importStatus = ''
        do {
            if (Test-Path -LiteralPath $importStatusPath -PathType Leaf) {
                $importStatus = Get-Content -LiteralPath $importStatusPath -Raw
                if ($importStatus -match '(?m)^phase=error\r?$') {
                    throw "Clean startup import failed: $importStatus"
                }
                if ($importStatus -match '(?m)^complete=1\r?$') { break }
            }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $importDeadline)
        if ($importStatus -notmatch '(?m)^complete=1\r?$') {
            throw 'Clean startup never completed its database/import initialization.'
        }
        $importStatus | Set-Content -LiteralPath (Join-Path $root ($Phase+'-import.txt'))

        # Production must reject the QA-only Options opener. Options is tested
        # through normal UI input separately; persisted defaults are verified
        # from the clean seed/consent choice without enabling QA in this DLL.

    }
    finally {
        if (-not $process.HasExited) {
            & $App /exit | Out-Null
            if (-not $process.WaitForExit(10000)) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                [void]$process.WaitForExit(3000)
            }
        }
    }
    Start-Sleep -Milliseconds 250
}

if (Test-Path -LiteralPath $root) {
    # The path has been resolved and constrained above.
    Remove-Item -LiteralPath $root -Recurse -Force
}
New-Item -ItemType Directory -Path $root | Out-Null
$officialReferenceRoot = Join-Path $root 'official-host-reference'
$officialProcess = Start-Process -FilePath $officialInstaller -ArgumentList @(
    '/S', '/P', "/D=$officialReferenceRoot") -Wait -PassThru -WindowStyle Hidden
if ($officialProcess.ExitCode -ne 0 -or
        -not (Test-Path -LiteralPath (
            Join-Path $officialReferenceRoot 'foobar2000.exe') -PathType Leaf)) {
    throw 'Could not create an official-host reference installation.'
}
Assert-ProductionRegistration
Assert-OfficialRuntime $officialReferenceRoot
$officialHostHash = (Get-FileHash -LiteralPath (
    Join-Path $officialReferenceRoot 'foobar2000.exe') -Algorithm SHA256).Hash
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
    '/QA_MODE=1', '/STATISTICS_OFFLINE=1', "/DIR=`"$root`"", '/TASKS=')
Invoke-Setup $arguments
$profile = Join-Path $root 'app\profile'
$installedHost = Join-Path $root 'app\foobar2000.exe'
$component = Join-Path $profile 'user-components-x64\foo_nekorox\foo_nekorox.dll'
$worker = Join-Path $profile 'user-components-x64\foo_nekorox\nekorox-analysis.exe'
$crt = Join-Path $profile 'user-components-x64\foo_nekorox\goden-crt-render.exe'
$shader = Join-Path $profile 'user-components-x64\foo_nekorox\goden-shader-render.exe'
$spectrum = Join-Path $profile (
    'user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll')
$columns = Join-Path $profile 'user-components-x64\foo_ui_columns\foo_ui_columns.dll'
$transactionPath = Join-Path $root 'install-transaction.json'
foreach ($required in @($profile, $component, $worker, $crt, $shader, $spectrum,
        (Join-Path $root 'install-transaction.json'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Clean install omitted required state: $required"
    }
}
$componentHash = Assert-ManifestPayload $component `
    $releaseData.coreBinary 'foo_nekorox.dll'
$workerHash = Assert-ManifestPayload $worker `
    $releaseData.analysisWorkerBinary 'nekorox-analysis.exe'
$crtHash = Assert-ManifestPayload $crt `
    $releaseData.crtRendererBinary 'goden-crt-render.exe'
$shaderHash = Assert-ManifestPayload $shader `
    $releaseData.shaderRendererBinary 'goden-shader-render.exe'
$spectrumHash = Assert-ManifestPayload $spectrum `
    $releaseData.spectrumAnalyzerBinary 'foo_vis_spectrum_analyzer.dll'
$columnsHash = Assert-ManifestPayload $columns $releaseData.columnsUiBinary 'foo_ui_columns.dll'
$transaction = Get-Content -LiteralPath $transactionPath -Raw | ConvertFrom-Json
if (-not $transaction.validated -or
        $transaction.componentSha256 -ne $componentHash -or
        $transaction.analysisWorkerSha256 -ne $workerHash -or
        $transaction.crtRendererSha256 -ne $crtHash -or
        $transaction.shaderRendererSha256 -ne $shaderHash -or
        $transaction.spectrumAnalyzerSha256 -ne $spectrumHash -or
        $transaction.columnsUiSha256 -ne $columnsHash) {
    throw 'Install transaction manifest did not validate the complete payload.'
}
$installedHostHash = (Get-FileHash -LiteralPath $installedHost `
    -Algorithm SHA256).Hash
if ($installedHostHash -ne $officialHostHash) {
    throw 'Installed host differs from the unchanged official foobar2000 host.'
}
if (Test-Path -LiteralPath (Join-Path $profile `
        'user-components-x64\foo_openlyrics\foo_openlyrics.dll')) {
    throw 'Clean install unexpectedly shipped OpenLyrics.'
}
$coverChoice=Join-Path $profile 'nekorox/cover-provider.install-choice'
if (!(Test-Path $coverChoice) -or (Get-Content $coverChoice -Raw).Trim() -ne 'catbox-disabled') {throw 'Silent install granted cover upload consent.'}
if (Test-Path (Join-Path $profile 'user-components-x64/foo_playcount/foo_playcount.dll')) {throw 'Offline install unexpectedly contains Playback Statistics.'}
$presenceEnabledByDefault = $null
if (!$SkipRuntimeUi) {
Assert-CleanFirstBoot $installedHost 'first-boot'
Assert-SilentCoverConsent $profile
$presenceEnabledByDefault = Assert-PresenceEnabledByDefault $profile
}
if (!(Test-Path (Join-Path $root 'app/portable_mode_enabled') -PathType Leaf)) {throw 'Portable profile marker is missing.'}


# An old standard-mode nested uninstaller must be retired on upgrade, while
# the original profile/rollback remain intact. Never execute this sentinel.
[IO.File]::WriteAllText((Join-Path $root 'app/uninstall.exe'), 'legacy uninstaller fixture - do not execute')

# An old transaction marker alone must never authorize a profile reset.
$legacyTransaction = @{ schema='goden.install-transaction.v2';profilePreserved=$false;validated=$true } | ConvertTo-Json
[IO.File]::WriteAllText($transactionPath,$legacyTransaction)
$sentinel = Join-Path $profile 'nekorox\installer-qa-user-state.txt'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sentinel) | Out-Null
[IO.File]::WriteAllText($sentinel, 'must-survive-upgrade-and-uninstall')
# These preferences differ from the fresh-install defaults. The full tree hash
# below must preserve them even though the install wizard has its own defaults.
[IO.File]::WriteAllText((Join-Path $profile 'nekorox\header-wordmark.install-choice'), "hide`r`n")
[IO.File]::WriteAllText((Join-Path $profile 'nekorox\fallback-accent.install-choice'), "233,116,81`r`n")
$statisticsPreserved = $null
if ($StatisticsPackage) {
    if ((Get-FileHash $StatisticsPackage).Hash -ne '4E47C44B8624544EF618FC02999C1FBBC212534507888A871625B83E949E4014') {throw 'Statistics preservation fixture is not the official pinned package.'}
    $zip = [IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($StatisticsPackage))
    try {
        $target=Join-Path $profile 'user-components-x64/foo_playcount/foo_playcount.dll'
        [IO.Directory]::CreateDirectory((Split-Path -Parent $target)) | Out-Null
        $entry=$zip.GetEntry('x64/foo_playcount.dll')
        if ($null -eq $entry -or $entry.Length -ne 279040) {throw 'Statistics fixture has unexpected architecture/length.'}
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$target,$true)
    } finally {$zip.Dispose()}
    if ((Get-FileHash $target).Hash -ne '68718DBF463E8DC1402261F11B0CDC39BD6E5291971DEAA8B91ECD4AF9CD7466') {throw 'Statistics fixture DLL mismatch.'}
    [IO.File]::WriteAllText((Join-Path $profile 'statistics-preservation-test.txt'),'synthetic statistics sentinel')
    $statisticsPreserved=$true
}
$before = Get-TreeHash $profile

Invoke-Setup $arguments
$after = Get-TreeHash $profile
if ($before -ne $after -or -not (Test-Path -LiteralPath $sentinel)) {
    throw 'Upgrade changed user profile content.'
}
if (-not (Test-Path -LiteralPath (Join-Path $root 'app.previous\foobar2000.exe'))) {
    throw 'Upgrade did not retain app.previous rollback.'
}
$previousProfile = Join-Path $root 'app.previous\profile'
Assert-ManifestPayload (Join-Path $previousProfile (
    'user-components-x64\foo_nekorox\foo_nekorox.dll')) `
    $releaseData.coreBinary 'rollback foo_nekorox.dll' | Out-Null
Assert-ManifestPayload (Join-Path $previousProfile (
    'user-components-x64\foo_nekorox\nekorox-analysis.exe')) `
    $releaseData.analysisWorkerBinary 'rollback nekorox-analysis.exe' | Out-Null
Assert-ManifestPayload (Join-Path $previousProfile (
    'user-components-x64\foo_nekorox\goden-crt-render.exe')) `
    $releaseData.crtRendererBinary 'rollback goden-crt-render.exe' | Out-Null
Assert-ManifestPayload (Join-Path $previousProfile (
    'user-components-x64\foo_nekorox\goden-shader-render.exe')) `
    $releaseData.shaderRendererBinary 'rollback goden-shader-render.exe' | Out-Null
Assert-ManifestPayload (Join-Path $previousProfile (
    'user-components-x64\foo_vis_spectrum_analyzer\foo_vis_spectrum_analyzer.dll')) `
    $releaseData.spectrumAnalyzerBinary 'rollback foo_vis_spectrum_analyzer.dll' | Out-Null

# Restore a byte-identical copy of the backup into a disposable checkout and
# actually launch it. The original rollback remains available for the upgrade.
$rollbackSmoke = Join-Path $root 'rollback-smoke\app'
New-Item -ItemType Directory -Path (Split-Path -Parent $rollbackSmoke) | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'app.previous') -Destination $rollbackSmoke -Recurse
if ((Get-TreeHash (Join-Path $root 'app.previous')) -ne (Get-TreeHash $rollbackSmoke)) {
    throw 'Rollback restoration copy differs from the retained backup.'
}
if (!$SkipRuntimeUi) {Assert-CleanFirstBoot (Join-Path $rollbackSmoke 'foobar2000.exe') 'rollback-boot'}
if ([IO.File]::ReadAllText((Join-Path $rollbackSmoke 'profile\nekorox\installer-qa-user-state.txt')) -ne
        'must-survive-upgrade-and-uninstall') {
    throw 'Restored rollback lost the user-state sentinel.'
}

# Real filesystem failure injection. No application launch, QA binary or input
# automation is needed: deny rename of the live host and then of the evidence.
if ($SkipRuntimeUi) {
    Assert-SetupFailurePreservesApp (Join-Path $root 'app') $arguments
    Assert-SetupFailurePreservesApp $transactionPath $arguments
    # A failed upgrade consumes its rollback when restoring it to live.
    # Reestablish the byte-verified original for the independent guard cases.
    if (!(Test-Path (Join-Path $root 'app.previous'))) {
        Copy-Item -LiteralPath $rollbackSmoke -Destination (Join-Path $root 'app.previous') -Recurse
    }
}

# Incomplete recovery must fail closed before any installer or uninstaller
# deletion, including when the only recoverable profile is no longer live.
if ($SkipRuntimeUi) {
    $uninstallerForGuard=Join-Path $root 'unins000.exe'
    $liveApp=Join-Path $root 'app'
    $heldApp=Join-Path $root 'app.qa-held'
    $previous=Join-Path $root 'app.previous'
    $old=Join-Path $root 'app.previous.old'
    foreach($shape in @('pending','previous','previous.old','unusable','marker')) {
        $recoverable=if($shape -eq 'marker'){Join-Path $root 'install-recovery.pending'} elseif($shape -eq 'pending'){Join-Path $root 'app.rollback.pending'} elseif($shape -eq 'previous.old'){$old} else {$previous}
        if($shape -eq 'marker') {
            [IO.File]::WriteAllText($recoverable,'synthetic incomplete recovery')
        } elseif($shape -eq 'unusable') {
            Move-Item -LiteralPath $installedHost -Destination ($installedHost+'.qa-held')
        } elseif($shape -eq 'pending') {
            New-Item -ItemType Directory -Path $recoverable | Out-Null
            [IO.File]::WriteAllText((Join-Path $recoverable 'recovery-sentinel.txt'),'retain me')
        } else {
            Move-Item -LiteralPath $liveApp -Destination $heldApp
            if($shape -eq 'previous.old'){Move-Item -LiteralPath $previous -Destination $old}
        }
        try {
            $recoveryBefore=Get-TreeHash $recoverable
            $liveBefore=if(Test-Path $liveApp){Get-TreeHash $liveApp}else{'<absent>'}
            $blocked=Start-Process -FilePath $setup -ArgumentList $arguments -Wait -PassThru -WindowStyle Hidden
            if($blocked.ExitCode -eq 0){throw "Installer accepted recovery state $shape."}
            $blocked=Start-Process -FilePath $uninstallerForGuard -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/QA_MODE=1') -Wait -PassThru -WindowStyle Hidden
            if($blocked.ExitCode -eq 0){throw "Uninstaller accepted recovery state $shape."}
            Assert-ProductionRegistration
            $liveAfter=if(Test-Path $liveApp){Get-TreeHash $liveApp}else{'<absent>'}
            if((Get-TreeHash $recoverable) -ne $recoveryBefore -or $liveAfter -ne $liveBefore){throw "Recovery state $shape was modified."}
        } finally {
            if($shape -eq 'marker'){
                Remove-Item -LiteralPath (Join-Path $root 'install-recovery.pending')
            } elseif($shape -eq 'unusable'){
                Move-Item -LiteralPath ($installedHost+'.qa-held') -Destination $installedHost
            } elseif($shape -eq 'pending'){
                # Fixed, resolved test-only path beneath the previously verified root.
                $pendingFixture=[IO.Path]::GetFullPath((Join-Path $root 'app.rollback.pending'))
                if ($recoverable -ne $pendingFixture -or !$pendingFixture.StartsWith($artifacts+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Recovery fixture escaped QA root.'}
                Remove-Item -LiteralPath $pendingFixture -Recurse -Force
            } else {
                if($shape -eq 'previous.old'){Move-Item -LiteralPath $old -Destination $previous}
                Move-Item -LiteralPath $heldApp -Destination $liveApp
            }
        }
    }
}

# Run a separate active-upgrade pass. A healthy shutdown legitimately persists
# runtime state, so the byte-identical profile gate above remains an inactive
# upgrade; this pass instead verifies exact-process closure and state survival.
$legacyOld = Join-Path $root 'app.previous.old'
$longLegacyDirectory = Join-Path $legacyOld 'profile\nekorox\assets\legacy-yttm'
while ((Join-Path $longLegacyDirectory 'legacy-cover.jpg').Length -lt 300) {
    $longLegacyDirectory = Join-Path $longLegacyDirectory (
        'long-album-identifier-0123456789abcdef')
}
[IO.Directory]::CreateDirectory($longLegacyDirectory) | Out-Null
[IO.File]::WriteAllBytes(
    (Join-Path $longLegacyDirectory 'legacy-cover.jpg'),
    [byte[]](1, 2, 3, 4))
if (!$SkipRuntimeUi) {
$activeBeforeUpgrade = Start-Process -FilePath $installedHost -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if ($activeBeforeUpgrade.HasExited) {
    throw 'Installed host exited before active-upgrade QA could run.'
}
try {
    Invoke-Setup $arguments
    $activeBeforeUpgrade.Refresh()
    if (-not $activeBeforeUpgrade.HasExited) {
        throw 'Upgrade did not close the exact live GODENplayer process.'
    }
} finally {
    if (-not $activeBeforeUpgrade.HasExited) {
        Stop-Process -Id $activeBeforeUpgrade.Id -Force -ErrorAction SilentlyContinue
    }
}
if (-not (Test-Path -LiteralPath $sentinel) -or
        [IO.File]::ReadAllText($sentinel) -ne
            'must-survive-upgrade-and-uninstall') {
    throw 'Active upgrade did not preserve the user-state sentinel.'
}
if (Test-Path -LiteralPath $legacyOld) {
    throw 'Active upgrade did not remove the legacy long-path rollback.'
}
}

$uninstaller = Join-Path $root 'unins000.exe'
if (-not (Test-Path -LiteralPath $uninstaller)) {
    throw 'Uninstaller was not created.'
}
if ($SkipRuntimeUi) {
    $beforeUninstallFault=Get-TreeHash (Join-Path $root 'app')
    $lockedProfile=[IO.File]::Open($sentinel,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::None)
    try {
        $blockedUninstall=Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/QA_MODE=1') -Wait -PassThru -WindowStyle Hidden
        if($blockedUninstall.ExitCode -eq 0){throw 'Uninstall accepted a profile that could not be backed up.'}
    } finally {$lockedProfile.Dispose(); Assert-ProductionRegistration}
    if((Get-TreeHash (Join-Path $root 'app')) -ne $beforeUninstallFault){throw 'Failed profile backup changed the live application.'}
    if(Test-Path (Join-Path $root 'profile-data')){throw 'Failed uninstall exposed a partial backup as a complete profile.'}
    if(!(Test-Path (Join-Path $root 'profile-data.partial'))){throw 'Failed backup was not retained with an explicit partial name.'}
}
$uninstallProfileBefore=Get-TreeHash $profile
$uninstall = Start-Process -FilePath $uninstaller -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/QA_MODE=1') `
    -Wait -PassThru `
    -WindowStyle Hidden
Assert-ProductionRegistration
if ($uninstall.ExitCode -ne 0) {
    throw "Uninstaller exited with $($uninstall.ExitCode)."
}
$preserved = Join-Path $root 'profile-data\nekorox\installer-qa-user-state.txt'
if (-not (Test-Path -LiteralPath $preserved) -or
        [IO.File]::ReadAllText($preserved) -ne
            'must-survive-upgrade-and-uninstall') {
    throw 'Uninstall did not preserve the recoverable profile.'
}
if((Get-TreeHash (Join-Path $root 'profile-data')) -ne $uninstallProfileBefore){throw 'Uninstall backup is not byte-identical to the full profile.'}
foreach($removed in @('app','app.next','app.previous','app.previous.old')){
    if(Test-Path (Join-Path $root $removed)){throw "Uninstall retained application directory $removed."}
}

$result = [ordered]@{
    schema = 'goden.installer-transaction-qa.v2'
    passed = $true
    setupSha256 = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash
    cleanInstall = $true
    runtimeUiSkipped = [bool]$SkipRuntimeUi
    runtimeObservationScope = 'Visible non-minimized workspace HWND, no configuration dialog, completed startup import, persisted defaults and actual PMv2 context; pixel appearance is a separate observation.'
    cleanFirstBoot = if ($SkipRuntimeUi) {$null} else {$true}
    expectedCollectionPreset = $ExpectedCollectionPreset
    expectedCollectionPresetId = $ExpectedCollectionPresetId
    noConfigurationDialogs = if ($SkipRuntimeUi) {$null} else {$true}
    legacyMarkerDidNotResetProfile = $true
    presenceEnabledByDefault = $presenceEnabledByDefault
    silentCoverConsentNotGranted = $true
    upgradeProfileIdentical = $true
    rollbackRetained = $true
    rollbackRestorationBooted = if ($SkipRuntimeUi) {$null} else {$true}
    rollbackRestorationByteIdentical = $true
    completePayloadVerified = $true
    activeUpgradeClosed = if ($SkipRuntimeUi) {$null} else {$true}
    legacyLongPathRollbackRemoved = if ($SkipRuntimeUi) {$null} else {$true}
    uninstallProfilePreserved = $true
    failedProfileBackupRefusedUninstall = if ($SkipRuntimeUi) {$true} else {$null}
    uninstallApplicationTreesRemoved = $true
    officialInstallerSourcePreserved = $true
    officialHostSourceSha256 = $officialHostHash
    runtimeWindowPerMonitorV2 = if ($SkipRuntimeUi) {$null} else {$true}
    officialHostUnmodified = ($installedHostHash -eq $officialHostHash)
    optionalStatisticsOffline = $true
    existingStatisticsProfilePreserved = $statisticsPreserved
    productionUninstallRegistrationUnchanged = $true
    observedFoobarStateUnchanged = $true
    unrelatedFoobarStateSha256 = $foobarCoexistenceBefore.sha256
    unrelatedFoobarObservedEntries = $foobarCoexistenceBefore.observedEntries
    officialRuntimePreserved = $true
    legacyHostUninstallerAbsent = $true
    incompleteRecoveryRefused = if ($SkipRuntimeUi) {$true} else {$null}
    failedPromotionPreservedLive = if ($SkipRuntimeUi) {$true} else {$null}
    failedEvidenceCommitRestoredLive = if ($SkipRuntimeUi) {$true} else {$null}
    installedHostSha256 = $officialHostHash
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (
    Join-Path $root 'installer-qa-result.json') -Encoding UTF8
Write-Host 'GODENplayer installer transaction QA: PASS'
