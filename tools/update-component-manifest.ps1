param([ValidateSet('Release')][string]$Configuration='Release')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
& python (Join-Path $PSScriptRoot 'public_release.py') manifest
if ($LASTEXITCODE -ne 0) {throw 'Public release manifest generation failed.'}
