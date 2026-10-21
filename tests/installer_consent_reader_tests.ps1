param([string]$RepoRoot='', [string[]]$ProfilePaths=@())
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (!$RepoRoot) {$RepoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
$driver=Join-Path $RepoRoot 'tools/test-installer-transaction.ps1'
$tokens=$null
$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($driver,[ref]$tokens,[ref]$parseErrors)
if ($parseErrors.Count) {throw 'Installer QA driver has syntax errors.'}
foreach ($functionName in @('Read-PinnedComponentSettings','Assert-SilentCoverConsent','Assert-PresenceEnabledByDefault')) {
    $reader=@($ast.FindAll({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
            $node.Name -ceq $functionName
    },$true))
    if ($reader.Count -ne 1) {throw "Expected exactly one read-only function: $functionName"}
    # Load only read-only functions, never the installer transaction driver.
    . ([scriptblock]::Create($reader[0].Extent.Text))
}
function Record([string]$Guid,[byte[]]$Value) {
    return [byte[]]([Guid]::Parse($Guid).ToByteArray()+[BitConverter]::GetBytes([uint32]$Value.Length)+$Value)
}
function Envelope([byte[]]$Payload) {
    return [byte[]]([Guid]::Parse('b2223dd1-15c4-46f5-8031-96e692bc1e20').ToByteArray()+
        [byte[]](0..15)+[BitConverter]::GetBytes([uint32]$Payload.Length)+$Payload)
}
$provider=Record '9448016b-56f5-4d7f-83b2-925991b68801' ([BitConverter]::GetBytes([uint32]1))
$denied=Record '9448016b-56f5-4d7f-83b2-925991b68802' ([byte[]]@(0))
$consumed=Record '9448016b-56f5-4d7f-83b2-925991b68805' ([byte[]]@(1))
$valid=Envelope ([byte[]]($provider+$denied+$consumed))
$temporaryRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$fixture=[IO.Path]::GetFullPath((Join-Path $temporaryRoot ('goden-consent-reader-'+[Guid]::NewGuid().ToString('N'))))
if (-not $fixture.StartsWith($temporaryRoot+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture escaped the temporary directory.'
}
$configuration=Join-Path $fixture 'configuration'
[IO.Directory]::CreateDirectory($configuration) | Out-Null
$file=Join-Path $configuration 'foo_nekorox.dll.cfg'
$count=0
function Check([string]$Name,[byte[]]$Bytes,[bool]$Expected) {
    [IO.File]::WriteAllBytes($file,$Bytes)
    $accepted=$false
    try {Assert-SilentCoverConsent $fixture; $accepted=$true} catch {}
    if ($accepted -ne $Expected) {throw "Consent reader result differs: $Name"}
    if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($file),$Bytes)) {
        throw "Consent reader changed the fixture: $Name"
    }
    $script:count++
}
try {
    foreach ($case in @(
        @{name='enabled';value=[byte[]]@(1);expected=$true},
        @{name='disabled';value=[byte[]]@(0);expected=$false},
        @{name='wrong width';value=[byte[]]@(1,0);expected=$false},
        @{name='invalid bool';value=[byte[]]@(2);expected=$false},
        @{name='missing';value=$null;expected=$false}
    )) {
        $bytes=if ($null -eq $case.value) {$valid} else {Envelope ([byte[]]($provider+$denied+$consumed+
            (Record 'b7d97e82-389a-48a5-920c-eb4ea5c038c9' $case.value)))}
        [IO.File]::WriteAllBytes($file,$bytes)
        $accepted=$false
        try {$accepted=Assert-PresenceEnabledByDefault $fixture} catch {}
        if ($accepted -ne $case.expected) {throw "Presence default result differs: $($case.name)"}
        if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($file),[byte[]]$bytes)) {throw 'Presence reader changed the fixture.'}
        $count++
    }
    Check 'denied' $valid $true
    $automatic=Record '9448016b-56f5-4d7f-83b2-925991b68801' ([BitConverter]::GetBytes([uint32]6))
    Check 'automatic denied' (Envelope ([byte[]]($automatic+$denied+$consumed))) $true
    $varied=[byte[]]$valid.Clone()
    [Array]::Copy([Guid]::NewGuid().ToByteArray(),0,$varied,16,16)
    Check 'opaque header differs' $varied $true
    Check 'unrelated record' (Envelope ([byte[]]($provider+$denied+$consumed+
        (Record '00000000-0000-0000-0000-000000000001' ([byte[]]@()))))) $true
    Check 'missing provider' (Envelope ([byte[]]($denied+$consumed))) $false
    Check 'accepted instead of denied' (Envelope ([byte[]]($provider+
        (Record '9448016b-56f5-4d7f-83b2-925991b68802' ([byte[]]@(1)))+$consumed))) $false
    Check 'unconsumed' (Envelope ([byte[]]($provider+$denied+
        (Record '9448016b-56f5-4d7f-83b2-925991b68805' ([byte[]]@(0)))))) $false
    Check 'wrong provider' (Envelope ([byte[]](
        (Record '9448016b-56f5-4d7f-83b2-925991b68801' ([BitConverter]::GetBytes([uint32]0)))+
        $denied+$consumed))) $false
    Check 'duplicate' (Envelope ([byte[]]($provider+$denied+$consumed+$denied))) $false
    Check 'wrong bool width' (Envelope ([byte[]]($provider+
        (Record '9448016b-56f5-4d7f-83b2-925991b68802' ([byte[]]@(0,0)))+$consumed))) $false
    $badSignature=[byte[]]$valid.Clone(); $badSignature[0]=0
    Check 'wrong signature' $badSignature $false
    $badLength=[byte[]]$valid.Clone(); $badLength[32]=0
    Check 'wrong envelope length' $badLength $false
    $hugeRecord=[byte[]]$valid.Clone()
    [Array]::Copy([BitConverter]::GetBytes([uint32]::MaxValue),0,$hugeRecord,52,4)
    Check 'oversized record' $hugeRecord $false
    Check 'oversized file' ([byte[]]::new(1048577)) $false
    for ($length=0; $length -lt $valid.Length; $length++) {
        $truncated=[byte[]]::new($length)
        [Array]::Copy($valid,$truncated,$length)
        # Also exercise truncated SDK records after a valid envelope length.
        if ($length -ge 36) {
            [Array]::Copy([BitConverter]::GetBytes([uint32]($length-36)),0,$truncated,32,4)
        }
        Check "truncated at $length" $truncated $false
    }
    foreach ($profile in $ProfilePaths) {
        $actual=Join-Path $profile 'configuration/foo_nekorox.dll.cfg'
        $before=[IO.File]::ReadAllBytes($actual)
        Assert-SilentCoverConsent $profile
        if (-not [Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($actual),$before)) {
            throw 'Consent reader changed the actual profile.'
        }
    }
    Write-Host "Installer consent reader: PASS ($count fixtures; $($ProfilePaths.Count) actual profiles; read-only)"
} finally {
    # Delete only the owned fixture, whose absolute boundary was checked above.
    if (Test-Path -LiteralPath $file) {Remove-Item -LiteralPath $file -Force}
    [IO.Directory]::Delete($configuration,$false)
    [IO.Directory]::Delete($fixture,$false)
}
