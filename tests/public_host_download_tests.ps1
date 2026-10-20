# All acquisition responses and publisher checks below are local fakes. Real
# checksum validation, atomic publication and cleanup are production code.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../tools/public-host-download.ps1')
$sandbox=Join-Path ([IO.Path]::GetTempPath()) ('goden-host-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
$script:body=[byte[]](1,2,3,4)
$validHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($script:body))
$script:calls=0;$script:transferThrows=$false;$script:identityThrows=$false;$script:concurrent=$false
function Receive-PublicHost([Uri]$Uri,[string]$Temporary,[long]$MaximumBytes) {
    ++$script:calls
    [IO.File]::WriteAllBytes($Temporary,$script:body)
    if ($script:concurrent) {[IO.File]::WriteAllText($script:destination,'concurrent')}
    if ($script:transferThrows) {throw 'injected transfer failure'}
}
function Assert-PublicHostIdentity([string]$Path) {if ($script:identityThrows) {throw 'injected publisher failure'}}
function Check([bool]$value,[string]$message) {if (!$value) {throw $message}}
function Acquire {Get-VerifiedPublicHost $script:destination 'https://www.foobar2000.org/test' $validHash 16}
function Refused {try {Acquire;throw 'UNEXPECTED SUCCESS'} catch {if ($_.Exception.Message -eq 'UNEXPECTED SUCCESS') {throw}}}
try {
    $script:destination=Join-Path $sandbox 'official.exe'
    $script:body=[byte[]](9);Refused
    Check (!(Test-Path $script:destination)) 'Invalid response published.'
    Check (@(Get-ChildItem $sandbox -Filter '*.download-*').Count -eq 0) 'Failed response leaked temporary.'
    $script:body=[byte[]](1,2,3,4);Acquire
    Check ((Get-FileHash $script:destination).Hash -eq $validHash) 'Valid retry did not publish.'
    $prior=$script:calls;Acquire;Check ($script:calls -eq $prior) 'Valid cache downloaded again.'
    [IO.File]::WriteAllText($script:destination,'invalid-cache');Refused
    Check ([IO.File]::ReadAllText($script:destination) -eq 'invalid-cache') 'Invalid existing cache was overwritten.'
    Remove-Item -LiteralPath $script:destination
    $script:transferThrows=$true;Refused;$script:transferThrows=$false
    $script:identityThrows=$true;Refused;$script:identityThrows=$false
    Check (!(Test-Path $script:destination)) 'Transfer/publisher failure published final file.'
    $script:concurrent=$true;Refused
    Check ([IO.File]::ReadAllText($script:destination) -eq 'concurrent') 'Concurrent cache was overwritten.'
    Check (@(Get-ChildItem $sandbox -Filter '*.download-*').Count -eq 0) 'Temporary files remain.'
    'Host acquisition offline fixtures: PASS (7 cases).'
} finally {
    # The exact fresh temporary sandbox was created above, under the OS temp root.
    $resolved=[IO.Path]::GetFullPath($sandbox)
    $prefix=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if (!$resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {throw 'Unsafe fixture cleanup path.'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
