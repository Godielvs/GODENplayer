param(
    [string]$LabRoot = '',
    [switch]$NoMedia
)

# Launch the C9 lab candidate as a disposable portable process.
#
# QA hooks are opt-in, exactly as the product requires: NEKOROX_QA_HOOKS=1 and
# NEKOROX_QA_PROFILE=1. GODEN_FLAG_TAG_IMPORT_V1 stays OFF (the product default)
# because this session must not exercise real media writes.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$artifacts = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\') + '\'
if (-not $LabRoot) { $LabRoot = Join-Path $repo 'artifacts\c9-lab' }
$lab = [IO.Path]::GetFullPath($LabRoot).TrimEnd('\')
if (-not ($lab + '\').StartsWith($artifacts, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to launch outside artifacts: $lab"
}

$app = Join-Path $lab 'app'
$exe = Join-Path $app 'foobar2000.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Lab foobar2000.exe missing: $exe" }

$existing = @(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" -ErrorAction SilentlyContinue |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.Equals($exe, [StringComparison]::OrdinalIgnoreCase) })
if ($existing.Count -gt 0) {
    Write-Host "Lab candidate already running (pid $($existing[0].ProcessId)); leaving it alone."
    return
}

$env:NEKOROX_QA_HOOKS = '1'
$env:NEKOROX_QA_PROFILE = '1'
$env:GODEN_FLAG_QA_TRACE_V2 = '1'
# Deliberately NOT set: GODEN_FLAG_TAG_IMPORT_V1. Tag import must stay off
# outside an isolated fixture.
$env:GODEN_FLAG_TAG_IMPORT_V1 = '0'

$arguments = @()
if (-not $NoMedia) {
    $music = Join-Path $lab 'music'
    if (Test-Path -LiteralPath $music) { $arguments += $music }
}

$process = Start-Process -FilePath $exe -WorkingDirectory $app -ArgumentList $arguments -PassThru
Write-Host "lab_pid=$($process.Id) exe=$exe"

# Give the shell time to create NekoRoXHeaderV3 (the probe requires it).
$deadline = (Get-Date).AddSeconds(60)
$ready = $false
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 700
    $probe = & (Join-Path $repo 'out\Release\nekorox_ui_probe.exe') $exe window-state 2>&1
    if ($LASTEXITCODE -eq 0) { $ready = $true; Write-Host $probe; break }
}
if (-not $ready) {
    Write-Warning "lab shell did not become probe-ready within 60 s"
    exit 4
}
Write-Host "lab candidate is probe-ready"
