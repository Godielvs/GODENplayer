param(
    [string]$LabRoot = '',
    [ValidateRange(5, 60)]
    [int]$MeasureSeconds = 15,
    [string]$ManifestPath = ''
)

# Narrow C9 A/B: the same disposable process, media and window dimensions are
# measured with the legacy A4 material and with Clovis.  Process CPU and the
# renderer's own GPU diagnostics are different metrics and remain separate.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $LabRoot) { $LabRoot = Join-Path $repo 'artifacts\c9-lab' }
$lab = [IO.Path]::GetFullPath($LabRoot).TrimEnd('\')
if (-not ($lab + '\').StartsWith($artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "C9 performance probe must stay inside artifacts: $lab"
}
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\c9-qualification\W5-performance-ab.json'
}
$manifest = [IO.Path]::GetFullPath($ManifestPath)
$appDir = Join-Path $lab 'app'
$exe = Join-Path $appDir 'foobar2000.exe'
$dll = Join-Path $appDir 'profile\user-components-x64\foo_nekorox\foo_nekorox.dll'
$probe = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
$fixture = Join-Path $lab 'music\04_regions_tagged.flac'
foreach ($required in @($exe, $dll, $probe, $fixture)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing C9 performance input: $required"
    }
}

function Invoke-Probe([string]$Action, [string]$Argument = '', [switch]$AllowFailure) {
    $arguments = @($exe, $Action)
    if ($Argument) { $arguments += $Argument }
    $prior = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & $probe @arguments 2>&1; $code = $LASTEXITCODE }
    finally { $ErrorActionPreference = $prior }
    $text = (($output | ForEach-Object { $_.ToString() }) -join "`n").Trim()
    if ($code -ne 0 -and -not $AllowFailure) {
        throw "Probe $Action failed (${code}): $text"
    }
    [pscustomobject]@{ success = $code -eq 0; output = $text }
}

function Parse-Diagnostics([string]$Text) {
    $values = [ordered]@{}
    foreach ($pair in ($Text -split '\s+')) {
        if ($pair -match '^([^=]+)=(-?\d+|true|false)$') {
            $raw = $Matches[2]
            $values[$Matches[1]] = if ($raw -eq 'true') { $true } elseif ($raw -eq 'false') { $false } else { [int64]$raw }
        }
    }
    [pscustomobject]$values
}

function Measure-Case([string]$Name, [string]$MaterialAction, [string]$Size) {
    Invoke-Probe 'window-resize' $Size | Out-Null
    # The GPU budget is only meaningful for a genuinely visible/presentable
    # window. Offscreen rendering is useful for automation but not this gate.
    Invoke-Probe 'window-visible-focus' | Out-Null
    Invoke-Probe 'shell-album' | Out-Null
    Invoke-Probe 'shell-a4-select' | Out-Null
    Invoke-Probe $MaterialAction | Out-Null
    Invoke-Probe 'drop-file' $fixture | Out-Null
    Start-Sleep -Seconds 4
    $beforeDiag = Parse-Diagnostics (Invoke-Probe 'shell-a4-r12-diagnostics').output
    $beforeWindow = (Invoke-Probe 'window-state').output
    $process.Refresh()
    $cpu0 = $process.TotalProcessorTime.TotalMilliseconds
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $samples = [Collections.Generic.List[object]]::new()
    while ($watch.Elapsed.TotalSeconds -lt $MeasureSeconds) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited -or -not $process.Responding) {
            throw "Candidate became unhealthy during $Name"
        }
        $sampleText = (Invoke-Probe 'shell-a4-r12-diagnostics').output
        $sample = Parse-Diagnostics $sampleText
        $samples.Add($sample)
    }
    $process.Refresh()
    $cpu1 = $process.TotalProcessorTime.TotalMilliseconds
    $elapsed = $watch.Elapsed.TotalMilliseconds
    $afterDiag = Parse-Diagnostics (Invoke-Probe 'shell-a4-r12-diagnostics').output
    $gpuScene = @($samples | ForEach-Object { [double]$_.gpuSceneP95Us })
    $gpuDecorative = @($samples | ForEach-Object { [double]$_.gpuDecorativeP95Us })
    [pscustomobject][ordered]@{
        name = $Name
        requestedWindow = $Size
        observedWindow = $beforeWindow
        materialAction = $MaterialAction
        measureSeconds = [Math]::Round($elapsed / 1000.0, 3)
        logicalProcessors = [Environment]::ProcessorCount
        processCpuPercentAllCores = [Math]::Round((($cpu1 - $cpu0) / $elapsed) * 100.0 / [Environment]::ProcessorCount, 4)
        processCpuPercentOneCoreScale = [Math]::Round((($cpu1 - $cpu0) / $elapsed) * 100.0, 4)
        gpuSceneP95UsMin = ($gpuScene | Measure-Object -Minimum).Minimum
        gpuSceneP95UsMax = ($gpuScene | Measure-Object -Maximum).Maximum
        gpuDecorativeP95UsMin = ($gpuDecorative | Measure-Object -Minimum).Minimum
        gpuDecorativeP95UsMax = ($gpuDecorative | Measure-Object -Maximum).Maximum
        before = $beforeDiag
        after = $afterDiag
        samples = $samples.Count
    }
}

$existing = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($exe, [StringComparison]::OrdinalIgnoreCase) })
if ($existing.Count) { throw "C9 lab is already running (PID $($existing[0].ProcessId))." }
$runningMarker = Join-Path $appDir 'profile\running'
if (Test-Path -LiteralPath $runningMarker -PathType Leaf) { Remove-Item -LiteralPath $runningMarker -Force }

$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
$env:GODEN_FLAG_QA_TRACE_V2 = '1'
$env:GODEN_FLAG_TAG_IMPORT_V1 = '0'
$process = $null
$gracefulExit = $false
$failure = $null
$cases = [Collections.Generic.List[object]]::new()
try {
    $process = Start-Process -FilePath $exe -WorkingDirectory $appDir -PassThru
    for ($attempt = 0; $attempt -lt 240; ++$attempt) {
        if ((Invoke-Probe 'window-state' -AllowFailure).success) { break }
        Start-Sleep -Milliseconds 250
    }
    Invoke-Probe 'window-offscreen-show' | Out-Null
    Invoke-Probe 'options-open' | Out-Null
    Invoke-Probe 'motion-mode' '0' | Out-Null
    Invoke-Probe 'options-close' | Out-Null
    Invoke-Probe 'volume-set' '1' | Out-Null
    foreach ($size in @('1920x1080', '2400x1350')) {
        $cases.Add((Measure-Case "legacy-$size" 'shell-a4-material-legacy' $size))
        $cases.Add((Measure-Case "clovis-$size" 'shell-a4-material-clovis' $size))
    }
}
catch {
    $failure = $_.Exception.Message
}
finally {
    Remove-Item Env:NEKOROX_QA_HOOKS, Env:NEKOROX_QA_PROFILE,
        Env:GODEN_FLAG_QA_TRACE_V2, Env:GODEN_FLAG_TAG_IMPORT_V1 -ErrorAction SilentlyContinue
    if ($process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Invoke-Probe 'app-close' -AllowFailure | Out-Null
            $gracefulExit = $process.WaitForExit(15000)
            if (-not $gracefulExit) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                $process.WaitForExit(5000) | Out-Null
            }
        } else { $gracefulExit = $true }
    }
}

$result = [pscustomobject][ordered]@{
    schema = 'goden.c9.w5-performance-ab.v1'
    generatedUtc = [DateTime]::UtcNow.ToString('o')
    status = if (-not $failure -and $cases.Count -eq 4 -and $gracefulExit) { 'measured' } else { 'failed' }
    scope = 'process CPU and renderer diagnostics; not displayed cadence or visual approval'
    processId = if ($process) { $process.Id } else { 0 }
    componentSha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
    fixture = [IO.Path]::GetFileName($fixture)
    fixtureSha256 = (Get-FileHash -LiteralPath $fixture -Algorithm SHA256).Hash
    cases = $cases
    gracefulExit = $gracefulExit
    failure = $failure
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $manifest) | Out-Null
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest -Encoding utf8
$result | Select-Object status,processId,@{n='cases';e={$_.cases.Count}},gracefulExit,failure | Format-List
if ($result.status -ne 'measured') { throw "C9 performance A/B failed; see $manifest" }
