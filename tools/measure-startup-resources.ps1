param(
    [Parameter(Mandatory = $true)]
    [string]$App,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [ValidateRange(3, 30)]
    [int]$Cycles = 8,
    [string]$ManifestPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$appFull = [IO.Path]::GetFullPath($App)
$probe = Join-Path $repo "out\$Configuration\nekorox_ui_probe.exe"
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\performance-study\startup-resources.json'
}
foreach ($required in @($appFull, $probe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Startup measurement input is missing: $required"
    }
}

function Get-TargetProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $appFull, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) { throw 'Multiple startup targets are running.' }
    if ($matches.Count -eq 0) { return $null }
    try { return Get-Process -Id $matches[0].ProcessId -ErrorAction Stop }
    catch [Microsoft.PowerShell.Commands.ProcessCommandException] { return $null }
}

function Stop-Target {
    if ($null -eq (Get-TargetProcess)) { return }
    & $appFull /exit | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        $process = Get-TargetProcess
    } while ($null -ne $process -and [DateTime]::UtcNow -lt $deadline)
    if ($null -ne $process) { throw 'Startup target did not close.' }
}

function Percentile([double[]]$Values, [double]$Amount) {
    $sorted = @($Values | Sort-Object)
    $index = [Math]::Max(0, [Math]::Ceiling($Amount * $sorted.Count) - 1)
    return [double]$sorted[$index]
}

function Stats([double[]]$Values) {
    return [ordered]@{
        average = [double]($Values | Measure-Object -Average).Average
        median = Percentile $Values 0.5
        p95 = Percentile $Values 0.95
        maximum = [double]($Values | Measure-Object -Maximum).Maximum
    }
}

Stop-Target
$results = @()
try {
    for ($cycle = 0; $cycle -lt $Cycles; ++$cycle) {
        $clock = [Diagnostics.Stopwatch]::StartNew()
        Start-Process -FilePath $appFull -WindowStyle Hidden
        $processSeenMs = $null
        $windowSeenMs = $null
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            Start-Sleep -Milliseconds 25
            $process = Get-TargetProcess
            if ($null -ne $process -and $null -eq $processSeenMs) {
                $processSeenMs = $clock.Elapsed.TotalMilliseconds
            }
            if ($null -ne $process -and $null -eq $windowSeenMs) {
                & $probe $appFull window-state --background > $null 2>&1
                if ($LASTEXITCODE -eq 0) {
                    $windowSeenMs = $clock.Elapsed.TotalMilliseconds
                    & $probe $appFull window-visible-focus no-focus --background > $null 2>&1
                    if ($LASTEXITCODE -ne 0) { throw 'Startup window could not be shown.' }
                }
            }
            $ready = $false
            if ($null -ne $windowSeenMs) {
                $output = @(& $probe $appFull workspace-state --background 2>&1)
                $ready = $LASTEXITCODE -eq 0
            }
        } while (-not $ready -and [DateTime]::UtcNow -lt $deadline)
        if (-not $ready -or $null -eq $process) {
            throw "Startup cycle $cycle did not become UI-ready."
        }
        $readyMs = $clock.Elapsed.TotalMilliseconds
        Start-Sleep -Milliseconds 500
        $process = Get-Process -Id $process.Id -ErrorAction Stop
        $cim = Get-CimInstance Win32_Process -Filter "ProcessId=$($process.Id)"
        $results += [pscustomobject][ordered]@{
            cycle = $cycle
            processSeenMs = [double]$processSeenMs
            windowSeenMs = [double]$windowSeenMs
            workspaceReadyMs = [double]$readyMs
            cpuMsAtReadyPlus500 =
                [double]$process.TotalProcessorTime.TotalMilliseconds
            workingSetMb = [double]($process.WorkingSet64 / 1MB)
            privateMb = [double]($process.PrivateMemorySize64 / 1MB)
            handles = [int]$process.HandleCount
            threads = [int]$process.Threads.Count
            pageFaults = [uint64]$cim.PageFaults
            readBytes = [uint64]$cim.ReadTransferCount
            writeBytes = [uint64]$cim.WriteTransferCount
        }
        Stop-Target
    }
}
finally {
    Stop-Target
}

$manifest = [ordered]@{
    schema = 'nekorox.startup-resources.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    app = $appFull
    cycles = $Cycles
    summary = [ordered]@{
        processSeenMs = Stats ([double[]]$results.processSeenMs)
        windowSeenMs = Stats ([double[]]$results.windowSeenMs)
        workspaceReadyMs = Stats ([double[]]$results.workspaceReadyMs)
        cpuMsAtReadyPlus500 = Stats ([double[]]$results.cpuMsAtReadyPlus500)
        workingSetMb = Stats ([double[]]$results.workingSetMb)
        privateMb = Stats ([double[]]$results.privateMb)
        handles = Stats ([double[]]$results.handles)
        threads = Stats ([double[]]$results.threads)
        pageFaults = Stats ([double[]]$results.pageFaults)
        readBytes = Stats ([double[]]$results.readBytes)
        writeBytes = Stats ([double[]]$results.writeBytes)
    }
    results = $results
}

$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $manifestFull)) | Out-Null
[IO.File]::WriteAllText(
    $manifestFull,
    ($manifest | ConvertTo-Json -Depth 8),
    [Text.UTF8Encoding]::new($false))
$manifest | ConvertTo-Json -Depth 8
