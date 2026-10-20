param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$Target = 'Build',
    [switch]$VerifyDeterminism,
    [switch]$EnableQaHooks,
    [string]$PythonExecutable = 'python.exe',
    [ValidateRange(1, 1)]
    [int]$Workers = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$msbuild = & "$PSScriptRoot\resolve-msbuild.ps1"
$python = (Get-Command $PythonExecutable -ErrorAction Stop).Source
& $python -B "$PSScriptRoot\verify-build-inputs.py"
if ($LASTEXITCODE) {throw 'Pinned build input verification failed before compilation.'}
$bindNative = $Configuration -eq 'Release' -and !$EnableQaHooks -and $Target -in @('Build','Rebuild')
$sourceSnapshot = Join-Path $repo 'out/Release/native-build-before.json'
if ($bindNative) {
    & $python -B "$PSScriptRoot\public_release.py" snapshot-build $sourceSnapshot
    if ($LASTEXITCODE) {throw 'Native pre-build snapshot failed.'}
}

if ($Target -in @('Build', 'Rebuild')) {
    & "$PSScriptRoot\build-zstd.ps1" -Configuration $Configuration -Workers $Workers -PythonExecutable $python
    & "$PSScriptRoot\build-libsonare.ps1" -Configuration $Configuration -Workers $Workers -PythonExecutable $python
    if ($Configuration -eq 'Release') {
        & "$PSScriptRoot\build-spectrum-analyzer.ps1" `
            -Configuration Release -Workers $Workers -VerifyDeterminism:$VerifyDeterminism -PythonExecutable $python
    }
}

& $msbuild "$repo\NekoRoX.sln" `
    /m:1 `
    /nodeReuse:false `
    /t:$Target `
    /p:Configuration=$Configuration `
    /p:Platform=x64 `
    /p:GodenEnableQaHooks=$([int][bool]$EnableQaHooks) `
    "/p:C4Python=$python" `
    /p:PlatformToolset=v142 `
    /p:CL_MPCount=$Workers `
    /p:MultiProcMaxCount=$Workers `
    /p:NekoBuildWorkers=$Workers `
    /p:WindowsTargetPlatformVersion=10.0.19041.0 `
    /v:minimal `
    /nologo
if ($LASTEXITCODE -ne 0) {
    throw "NekoRoX build failed with exit code $LASTEXITCODE."
}
if ($bindNative) {
    & $python -B "$PSScriptRoot\public_release.py" seal-native $sourceSnapshot
    if ($LASTEXITCODE) {throw 'Native source/binary binding failed.'}
}
