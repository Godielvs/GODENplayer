param(
    [Parameter(Mandatory=$true)][string]$PayloadStaging,
    [Parameter(Mandatory=$true)][string]$FoobarInstaller,
    [Parameter(Mandatory=$true)][string]$CorrespondingSources,
    [string]$OutputDirectory=(Join-Path $PSScriptRoot '../out/release'),
    [string]$QaRoot=(Join-Path $PSScriptRoot '../artifacts/public-installer-qa'),
    [string]$SqliteExecutable='sqlite3.exe'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
& "$PSScriptRoot/build-friend-installer.ps1" -PayloadStaging $PayloadStaging -FoobarInstaller $FoobarInstaller -CorrespondingSources $CorrespondingSources -OutputDirectory $OutputDirectory -SqliteExecutable $SqliteExecutable
& "$PSScriptRoot/test-installer-transaction.ps1" -SetupPath (Join-Path $OutputDirectory 'GODENplayer-Beta-Setup-x64.exe') -OfficialInstaller $FoobarInstaller -QaRoot $QaRoot -SqliteExecutable $SqliteExecutable
Write-Host 'Installer transaction passed. Functional, DPI, Presence and performance evidence remain separate qualification gates.'
