param(
    [Parameter(Mandatory=$true)][string]$DevRoot,
    [string]$LayoutPath = '',
    [string]$ManifestPath = '',
    [string]$ProbePath = '',
    [int]$TimeoutMs = 15000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
$app = Join-Path $devRootFull 'app\foobar2000.exe'
if (-not $LayoutPath) {
    $LayoutPath = Join-Path $repo 'config\layout\nekorox-v3.fcl'
}
$layoutFull = [IO.Path]::GetFullPath($LayoutPath)
if (-not $ManifestPath) {
    $ManifestPath = Join-Path $repo 'artifacts\columns-runtime-layout.json'
}
if (-not $ProbePath) {
    $ProbePath = Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
}
$probeFull = [IO.Path]::GetFullPath($ProbePath)

if ($devRootFull -match 'foobar2000-64bit-lab' -or
        $devRootFull -match 'backup-2026-06-03') {
    throw "Refusing layout installation against a protected installation: $devRootFull"
}
foreach ($required in @($app, $layoutFull, $probeFull,
        (Join-Path $PSScriptRoot 'verify-runtime-layout.js'))) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Runtime layout input is missing: $required"
    }
}

function Get-AppProcess {
    $matches = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" |
        Where-Object {
            $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath).Equals(
                $app, [StringComparison]::OrdinalIgnoreCase)
        })
    if ($matches.Count -gt 1) {
        throw 'Multiple V3 development processes are running.'
    }
    if ($matches.Count -eq 0) { return $null }
    return Get-Process -Id $matches[0].ProcessId -ErrorAction SilentlyContinue
}

function Wait-App([bool]$Present) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-AppProcess
        if ($Present -and $null -ne $process) {
            $previousPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                & $probeFull $app 'window-state' '--background' 2>$null | Out-Null
                $probeExitCode = $LASTEXITCODE
            }
            finally {
                $ErrorActionPreference = $previousPreference
            }
            if ($probeExitCode -eq 0) { return $process }
        }
        if (-not $Present -and $null -eq $process) { return $null }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    $expectation = if ($Present) { 'start' } else { 'exit' }
    throw "Development foobar2000 did not $expectation within ${TimeoutMs}ms."
}

function Wait-ImportHost {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    do {
        $process = Get-AppProcess
        if ($null -ne $process -and $process.MainWindowHandle -ne 0) {
            return $process
        }
        Start-Sleep -Milliseconds 50
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Columns UI import host did not become ready within ${TimeoutMs}ms."
}

function Stop-App {
    $process = Get-AppProcess
    if ($null -eq $process) { return }
    & $app /exit | Out-Null
    if (-not $process.WaitForExit($TimeoutMs)) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        [void]$process.WaitForExit(3000)
    }
    Wait-App $false | Out-Null
    Start-Sleep -Milliseconds 100
}

function Start-Import {
    $arguments = "/columnsui:import-quiet `"$layoutFull`""
    Start-Process -FilePath $app -ArgumentList $arguments `
        -WindowStyle Minimized | Out-Null
    Wait-ImportHost | Out-Null
    Start-Sleep -Milliseconds 750
}

function Export-And-Verify([string]$OutputPath) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $lastVerification = 'Columns UI did not publish a layout dataset.'
    do {
        Remove-Item -LiteralPath $OutputPath -Force -ErrorAction SilentlyContinue
        & $app /columnsui:export-quiet $OutputPath | Out-Null

        $fileWatch = [Diagnostics.Stopwatch]::StartNew()
        do {
            if ((Test-Path -LiteralPath $OutputPath) -and
                    (Get-Item -LiteralPath $OutputPath).Length -gt 0) {
                break
            }
            Start-Sleep -Milliseconds 25
        } while ($fileWatch.ElapsedMilliseconds -lt 1000 -and
            $watch.ElapsedMilliseconds -lt $TimeoutMs)

        if ((Test-Path -LiteralPath $OutputPath) -and
                (Get-Item -LiteralPath $OutputPath).Length -gt 0) {
            $previousPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            try {
                $verification = & node `
                    (Join-Path $PSScriptRoot 'verify-runtime-layout.js') `
                    $layoutFull $OutputPath 2>&1
                $verificationExitCode = $LASTEXITCODE
            }
            finally {
                $ErrorActionPreference = $previousPreference
            }
            if ($verificationExitCode -eq 0) {
                return (($verification | ForEach-Object { $_.ToString() }) -join "`n") |
                    ConvertFrom-Json
            }
            $lastVerification = ($verification |
                ForEach-Object { $_.ToString() }) -join "`n"
        }
        Start-Sleep -Milliseconds 100
    } while ($watch.ElapsedMilliseconds -lt $TimeoutMs)
    throw "Columns UI did not publish the expected runtime layout within " +
        "${TimeoutMs}ms: $lastVerification"
}

if ($null -ne (Get-AppProcess)) {
    throw 'Close the V3 development portable before installing its layout.'
}

$artifactDirectory = Join-Path $repo 'artifacts\columns-runtime-layout'
New-Item -ItemType Directory -Force -Path $artifactDirectory | Out-Null
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$importExport = Join-Path $artifactDirectory "accepted-$stamp.fcl"
$restartExport = Join-Path $artifactDirectory "restart-$stamp.fcl"

try {
    Start-Import
    Stop-App

    Start-Process -FilePath $app -WindowStyle Minimized
    Wait-App $true | Out-Null
    $accepted = Export-And-Verify $importExport
    Stop-App

    Start-Process -FilePath $app -WindowStyle Minimized
    Wait-App $true | Out-Null
    $restarted = Export-And-Verify $restartExport
    Stop-App
}
catch {
    try { Stop-App } catch {}
    throw
}

$manifest = [ordered]@{
    schema = 'nekorox.columns-runtime-acceptance.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    executable = $app
    expectedLayout = $layoutFull
    accepted = $accepted
    restarted = $restarted
}
$manifestDirectory = Split-Path -Parent $ManifestPath
New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
$manifest | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath $ManifestPath -Encoding UTF8

Write-Host "Columns UI accepted and persisted the NekoRoX runtime layout."
Write-Host "Manifest: $ManifestPath"
