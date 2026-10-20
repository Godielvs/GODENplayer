param([string]$SevenZip = '7z.exe')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$download=Join-Path $repo 'out/downloads/SDK-2025-03-07.7z'
$destination=Join-Path $repo 'out/dependencies/foobar2000-sdk/SDK-2025-03-07'
$expected='CCDA3C5840E66E0E28A7E4FE36407C4E78581AA30C40C362A188FCBAAE799A3E'
New-Item -ItemType Directory -Force (Split-Path $download),$destination | Out-Null
if (!(Test-Path $download)) {
    Invoke-WebRequest 'https://www.foobar2000.org/downloads/SDK-2025-03-07.7z' -OutFile $download -MaximumRedirection 3
}
if ((Get-FileHash $download).Hash -ne $expected) { throw 'Official SDK archive checksum mismatch.' }
& $SevenZip x $download "-o$destination" -y | Out-Null
if ($LASTEXITCODE -ne 0) {throw 'Official SDK extraction failed.'}
foreach($file in 'foobar2000/SDK/foobar2000.h','sdk-license.txt') {
    if (!(Test-Path (Join-Path $destination $file))) {throw "SDK is incomplete: $file"}
}
Write-Host 'Pinned official SDK prepared. This does not modify the player.'
