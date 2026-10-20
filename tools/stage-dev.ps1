param(
    [Parameter(Mandatory=$true)][string]$FoobarInstaller,
    [Parameter(Mandatory=$true)][string]$PayloadStaging,
    [Parameter(Mandatory=$true)][string]$DevRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($DevRoot)
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$root.StartsWith((Join-Path $repo 'out')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {throw 'Development candidate must be inside this repository out directory.'}
if (Test-Path $root) {throw 'Refusing to overwrite an existing development candidate.'}
if ((Get-FileHash $FoobarInstaller).Hash -ne '530E6C9DB829874BC250AF038C1D07F8B42FEF61C5DD99F7EDCDE6393A93EE23') {throw 'Official installer checksum mismatch.'}
$sig=Get-AuthenticodeSignature $FoobarInstaller
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Illustrate Ltd') {throw 'Official installer signature mismatch.'}
$app=Join-Path $root 'app'
New-Item -ItemType Directory -Force $app | Out-Null
$process=Start-Process -FilePath $FoobarInstaller -ArgumentList @('/S',"/D=$app") -Wait -PassThru -WindowStyle Hidden
if ($process.ExitCode -ne 0 -or (Get-FileHash (Join-Path $app 'foobar2000.exe')).Hash -ne '653CC120C146AAAE9E6DB9B6F19E5A1588407B8940BC1521F0CED739FF8924B0') {throw 'Official host extraction failed integrity check.'}
Copy-Item (Join-Path $PayloadStaging 'profile') $app -Recurse
New-Item -ItemType File (Join-Path $app 'portable_mode_enabled') | Out-Null
Write-Host 'Disposable local candidate prepared; host untouched, no personal profile read.'
