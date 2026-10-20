param(
    [string]$App = "$env:LOCALAPPDATA\Programs\Goden\app\foobar2000.exe",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(8, 400)]
    [int]$Steps = 120,
    [ValidateRange(0, 50)]
    [int]$StepDelayMilliseconds = 6,
    [ValidateRange(0, 100)]
    [int]$CrossModeCycles = 24,
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $OutputPath) {
    $OutputPath = Join-Path $repo (
        'artifacts\geometry-study\window-geometry-' +
        [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss') + '.json')
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Geometry measurement input is missing: $required"
    }
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class GodenGeometryProbe {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after,
        int x, int y, int width, int height, uint flags);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint message,
        UIntPtr wparam, IntPtr lparam, uint flags, uint timeout,
        out UIntPtr result);
}
'@

function Invoke-Probe([string]$Action, [string]$Argument = '') {
    $arguments = @($appFull, $Action)
    if ($Argument) { $arguments += $Argument }
    $output = @(& $probe @arguments 2>&1)
    $exitCode = $LASTEXITCODE
    [pscustomobject]@{
        exitCode = $exitCode
        output = ($output -join "`n").Trim()
    }
}

function Get-TargetProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -ne 1) {
        throw "Expected one target process, found $($matches.Count)."
    }
    Get-Process -Id $matches[0].ProcessId -ErrorAction Stop
}

function Get-Rect([IntPtr]$Window) {
    $rect = [GodenGeometryProbe+Rect]::new()
    if (-not [GodenGeometryProbe]::GetWindowRect($Window, [ref]$rect)) {
        throw 'GetWindowRect failed.'
    }
    $rect
}

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return 0.0 }
    $index = [Math]::Max(0, [Math]::Ceiling($Percentile * $sorted.Count) - 1)
    [double]$sorted[$index]
}

function Get-GeometrySnapshot {
    $tree = Invoke-Probe 'window-tree'
    $render = Invoke-Probe 'workspace-render-state'
    $rows = @()
    foreach ($line in ($tree.output -split "`r?`n")) {
        if ($line -notmatch 'rect=(-?\d+),(-?\d+) (-?\d+)x(-?\d+).*class=\[([^\]]+)\].*title=\[([^\]]*)\]') {
            continue
        }
        $rows += [pscustomobject]@{
            left = [int]$Matches[1]
            top = [int]$Matches[2]
            width = [int]$Matches[3]
            height = [int]$Matches[4]
            className = $Matches[5]
            title = $Matches[6]
        }
    }
    [pscustomobject]@{
        treeExitCode = $tree.exitCode
        renderExitCode = $render.exitCode
        render = $render.output
        windows = $rows
    }
}

function Invoke-GeometrySweep(
    [string]$Name,
    [bool]$Resize,
    [GodenGeometryProbe+Rect]$Initial,
    [Diagnostics.Process]$Process
) {
    $window = $Process.MainWindowHandle
    $ignored = [UIntPtr]::Zero
    $beforeCpu = $Process.TotalProcessorTime.TotalMilliseconds
    $beforeWorkingSet = $Process.WorkingSet64
    $before = Get-GeometrySnapshot
    $durations = [Collections.Generic.List[double]]::new()
    $failures = [Collections.Generic.List[object]]::new()
    $overall = [Diagnostics.Stopwatch]::StartNew()
    $entered = [GodenGeometryProbe]::SendMessageTimeout(
        $window, 0x0231, [UIntPtr]::Zero, [IntPtr]::Zero, 3, 2000,
        [ref]$ignored) -ne [IntPtr]::Zero
    try {
        for ($index = 0; $index -lt $Steps; ++$index) {
            $phase = 2.0 * [Math]::PI * $index / [Math]::Max(1, $Steps - 1)
            $x = $Initial.Left + [int][Math]::Round(120.0 * [Math]::Sin($phase))
            $y = $Initial.Top + [int][Math]::Round(55.0 * [Math]::Sin(2.0 * $phase))
            if ($Resize) {
                $width = 1080 + [int][Math]::Round(360.0 * (0.5 + 0.5 * [Math]::Sin($phase)))
                $height = 680 + [int][Math]::Round(220.0 * (0.5 + 0.5 * [Math]::Cos($phase)))
                $flags = 0x0014 # SWP_NOZORDER | SWP_NOACTIVATE
            } else {
                $width = 0
                $height = 0
                $flags = 0x0015 # SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
            }
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $ok = [GodenGeometryProbe]::SetWindowPos(
                $window, [IntPtr]::Zero, $x, $y, $width, $height, $flags)
            $timer.Stop()
            $durations.Add($timer.Elapsed.TotalMilliseconds)
            if (-not $ok) {
                $failures.Add([pscustomobject]@{
                    step = $index
                    win32Error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
                })
            }
            if ($StepDelayMilliseconds -gt 0) {
                Start-Sleep -Milliseconds $StepDelayMilliseconds
            }
        }
    }
    finally {
        $ignored = [UIntPtr]::Zero
        $exited = [GodenGeometryProbe]::SendMessageTimeout(
            $window, 0x0232, [UIntPtr]::Zero, [IntPtr]::Zero, 3, 2000,
            [ref]$ignored) -ne [IntPtr]::Zero
    }
    $overall.Stop()
    Start-Sleep -Milliseconds 500
    $Process.Refresh()
    $after = Get-GeometrySnapshot
    $afterCpu = $Process.TotalProcessorTime.TotalMilliseconds
    [pscustomobject][ordered]@{
        scenario = $Name
        resize = $Resize
        steps = $Steps
        delayMilliseconds = $StepDelayMilliseconds
        enteredSizeMove = $entered
        exitedSizeMove = $exited
        totalMilliseconds = $overall.Elapsed.TotalMilliseconds
        setWindowPosAverageMilliseconds = [double](
            $durations | Measure-Object -Average).Average
        setWindowPosMedianMilliseconds = Get-Percentile $durations.ToArray() 0.5
        setWindowPosP95Milliseconds = Get-Percentile $durations.ToArray() 0.95
        setWindowPosMaximumMilliseconds = [double](
            $durations | Measure-Object -Maximum).Maximum
        callsOverFrameBudget6ms = @($durations | Where-Object { $_ -gt 6.06 }).Count
        callsOver16ms = @($durations | Where-Object { $_ -gt 16.0 }).Count
        callsOver33ms = @($durations | Where-Object { $_ -gt 33.0 }).Count
        processCpuMilliseconds = $afterCpu - $beforeCpu
        workingSetDeltaBytes = [int64]$Process.WorkingSet64 - [int64]$beforeWorkingSet
        respondingAfter = $Process.Responding
        failures = @($failures)
        before = $before
        after = $after
    }
}

function Invoke-CrossModeResizeMatrix(
    [GodenGeometryProbe+Rect]$Initial,
    [Diagnostics.Process]$Process
) {
    $window = $Process.MainWindowHandle
    $rows = [Collections.Generic.List[object]]::new()
    for ($index = 0; $index -lt $CrossModeCycles; ++$index) {
        $album = Invoke-Probe 'shell-album'
        $width = if (($index % 2) -eq 0) { 1040 } else { 1720 }
        $height = if (($index % 3) -eq 0) { 680 } else { 940 }
        $ignored = [UIntPtr]::Zero
        $entered = [GodenGeometryProbe]::SendMessageTimeout(
            $window, 0x0231, [UIntPtr]::Zero, [IntPtr]::Zero, 3, 2000,
            [ref]$ignored) -ne [IntPtr]::Zero
        $timer = [Diagnostics.Stopwatch]::StartNew()
        $resized = [GodenGeometryProbe]::SetWindowPos(
            $window, [IntPtr]::Zero, $Initial.Left, $Initial.Top,
            $width, $height, 0x0014)
        $ignored = [UIntPtr]::Zero
        $exited = [GodenGeometryProbe]::SendMessageTimeout(
            $window, 0x0232, [UIntPtr]::Zero, [IntPtr]::Zero, 3, 2000,
            [ref]$ignored) -ne [IntPtr]::Zero
        $library = Invoke-Probe 'shell-library'
        $timer.Stop()
        $immediate = Get-GeometrySnapshot
        Start-Sleep -Milliseconds 350
        $settled = Get-GeometrySnapshot

        $workspace = @($settled.windows |
            Where-Object className -EQ 'NekoRoXWorkspaceV3' |
            Select-Object -First 1)
        $shell = @($settled.windows |
            Where-Object className -EQ 'GodenPlayerShellV2' |
            Select-Object -First 1)
        $renderMatches = $settled.render -match
            'size=(\d+)x(\d+).*pixels=(\d+)x(\d+)'
        $renderSizeMatchesPixels = $renderMatches -and
            [int]$Matches[1] -eq [int]$Matches[3] -and
            [int]$Matches[2] -eq [int]$Matches[4]
        $workspaceMatchesShell = $workspace.Count -eq 1 -and
            $shell.Count -eq 1 -and
            $workspace[0].width -eq $shell[0].width
        $rows.Add([pscustomobject][ordered]@{
            cycle = $index
            requested = "${width}x${height}"
            albumExitCode = $album.exitCode
            enteredSizeMove = $entered
            resized = $resized
            exitedSizeMove = $exited
            libraryExitCode = $library.exitCode
            resizeAndReturnMilliseconds = $timer.Elapsed.TotalMilliseconds
            responding = $Process.Responding
            immediateRender = $immediate.render
            settledRender = $settled.render
            renderSizeMatchesPixels = $renderSizeMatchesPixels
            workspaceMatchesShellWidth = $workspaceMatchesShell
            workspaceWidth = if ($workspace.Count -eq 1) {
                $workspace[0].width
            } else { -1 }
            shellWidth = if ($shell.Count -eq 1) { $shell[0].width } else { -1 }
        })
    }
    [pscustomobject][ordered]@{
        scenario = 'album-resize-return-library'
        cycles = $CrossModeCycles
        failures = @($rows | Where-Object {
            $_.albumExitCode -ne 0 -or $_.libraryExitCode -ne 0 -or
            -not $_.enteredSizeMove -or -not $_.resized -or
            -not $_.exitedSizeMove -or -not $_.responding -or
            -not $_.renderSizeMatchesPixels -or
            -not $_.workspaceMatchesShellWidth
        }).Count
        maximumMilliseconds = [double](
            $rows.resizeAndReturnMilliseconds | Measure-Object -Maximum).Maximum
        p95Milliseconds = Get-Percentile `
            ([double[]]$rows.resizeAndReturnMilliseconds) 0.95
        rows = @($rows)
    }
}

$process = Get-TargetProcess
$window = $process.MainWindowHandle
if ($window -eq [IntPtr]::Zero) { throw 'Target has no main window.' }
$initial = Get-Rect $window
$initialTransport = (Invoke-Probe 'transport-state').output
$initialWindowState = (Invoke-Probe 'window-state').output
$initialFrameClock = (Invoke-Probe 'frame-clock-state').output
$scenarios = @()
try {
    (Invoke-Probe 'shell-library') | Out-Null
    Start-Sleep -Milliseconds 500
    $scenarios += Invoke-GeometrySweep 'library-live-resize' $true $initial $process
    $scenarios += Invoke-GeometrySweep 'library-window-move' $false $initial $process

    (Invoke-Probe 'shell-album') | Out-Null
    Start-Sleep -Seconds 2
    $scenarios += Invoke-GeometrySweep 'album-live-resize' $true $initial $process
    $scenarios += Invoke-GeometrySweep 'album-window-move' $false $initial $process
    $scenarios += Invoke-CrossModeResizeMatrix $initial $process
}
finally {
    [GodenGeometryProbe]::SetWindowPos(
        $window, [IntPtr]::Zero, $initial.Left, $initial.Top,
        $initial.Right - $initial.Left, $initial.Bottom - $initial.Top,
        0x0014) | Out-Null
}

$manifest = [ordered]@{
    schema = 'goden.window-geometry-study.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    pid = $process.Id
    steps = $Steps
    crossModeCycles = $CrossModeCycles
    initialTransport = $initialTransport
    initialWindowState = $initialWindowState
    initialFrameClock = $initialFrameClock
    initialRect = [ordered]@{
        left = $initial.Left
        top = $initial.Top
        width = $initial.Right - $initial.Left
        height = $initial.Bottom - $initial.Top
    }
    displayRefreshRatesHz = @(Get-CimInstance Win32_VideoController |
        Where-Object CurrentRefreshRate |
        Select-Object -ExpandProperty CurrentRefreshRate)
    scenarios = $scenarios
    finalWindowState = (Invoke-Probe 'window-state').output
    finalFrameClock = (Invoke-Probe 'frame-clock-state').output
    finalWorkspace = (Invoke-Probe 'workspace-render-state').output
    finalA4 = (Invoke-Probe 'shell-a4-r12-diagnostics').output
    finalC10 = (Invoke-Probe 'shell-c10-diagnostics').output
}

$outputFull = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $outputFull)) | Out-Null
[IO.File]::WriteAllText(
    $outputFull,
    ($manifest | ConvertTo-Json -Depth 10),
    [Text.UTF8Encoding]::new($false))

$manifest | ConvertTo-Json -Depth 10
