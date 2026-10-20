param([string]$Destination = (Join-Path $PSScriptRoot '../out/downloads/foobar2000-x64_v2.25.10.exe'))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'public-host-download.ps1')
Get-VerifiedPublicHost -Destination $Destination `
    -Uri 'https://www.foobar2000.org/downloads/foobar2000-x64_v2.25.10.exe' `
    -ExpectedSha256 '530E6C9DB829874BC250AF038C1D07F8B42FEF61C5DD99F7EDCDE6393A93EE23'
Write-Host 'Verified intact official foobar2000 2.25.10 x64 installer.'
