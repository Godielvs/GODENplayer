param(
    [Parameter(Mandatory = $true)]
    [string]$ProfileRoot,
    [string]$PolicyPath,
    [string]$ManifestPath,
    [switch]$Apply,
    [Parameter(Mandatory=$true)][string]$DevRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($PolicyPath)) {
    $PolicyPath = Join-Path $repo 'config\migration\profile-policy-v1.json'
}
$profile = [IO.Path]::GetFullPath($ProfileRoot).TrimEnd('\')
$devRootFull = [IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
$artifactRoot = [IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\')

function Test-Descendant([string]$Path, [string]$Parent) {
    $fullPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $fullParent = [IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    return $fullPath.StartsWith($fullParent, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-ProfileBoundary([string]$Path) {
    $allowedDev = Test-Descendant $Path $devRootFull
    $allowedFixture = Test-Descendant $Path $artifactRoot
    if (-not $allowedDev -and -not $allowedFixture) {
        throw "Refusing to normalize a profile outside the V3 dev or test boundary: $Path"
    }
    if ((Split-Path -Leaf $Path) -ne 'profile') {
        throw "ProfileRoot must end in a profile directory: $Path"
    }
}

function Assert-ChildPath([string]$Path) {
    if (-not (Test-Descendant $Path $profile)) {
        throw "Refusing profile operation outside ${profile}: $Path"
    }
}

function Measure-Tree([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return [ordered]@{ files = 0; bytes = 0 }
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (-not $item.PSIsContainer) {
        return [ordered]@{ files = 1; bytes = [int64]$item.Length }
    }
    $files = @(Get-ChildItem -LiteralPath $Path -File -Recurse -Force `
        -ErrorAction SilentlyContinue)
    [int64]$bytes = 0
    foreach ($file in $files) { $bytes += [int64]$file.Length }
    return [ordered]@{ files = $files.Count; bytes = [int64]$bytes }
}

function New-Operation([string]$Kind, [string]$Path) {
    Assert-ChildPath $Path
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $measurement = Measure-Tree $Path
    $relative = $Path.Substring($profile.Length + 1).Replace('\', '/')
    $script:operations.Add([ordered]@{
        kind = $Kind
        path = $relative
        files = $measurement.files
        bytes = $measurement.bytes
        result = if ($Apply) { 'removed' } else { 'planned' }
    })
    if ($Apply) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
}

Assert-ProfileBoundary $profile
if (-not (Test-Path -LiteralPath $profile -PathType Container)) {
    throw "Profile does not exist: $profile"
}
if (-not (Test-Path -LiteralPath $PolicyPath -PathType Leaf)) {
    throw "Profile policy does not exist: $PolicyPath"
}

$policy = Get-Content -LiteralPath $PolicyPath -Raw | ConvertFrom-Json
if ($policy.schema -ne 'nekorox.profile-policy.v1') {
    throw "Unsupported profile policy schema: $($policy.schema)"
}

$before = Measure-Tree $profile
$operations = [Collections.Generic.List[object]]::new()

foreach ($name in $policy.retiredProfileDirectories) {
    New-Operation 'retired-profile-directory' (Join-Path $profile $name)
}
foreach ($name in $policy.retiredProfileFiles) {
    New-Operation 'retired-profile-file' (Join-Path $profile $name)
}

$configuration = Join-Path $profile 'configuration'
foreach ($name in $policy.retiredConfigurationFiles) {
    New-Operation 'retired-component-configuration' (Join-Path $configuration $name)
}

$components = Join-Path $profile 'user-components-x64'
if (Test-Path -LiteralPath $components) {
    $allowed = @{}
    foreach ($name in $policy.allowedUserComponents) {
        $allowed[$name.ToLowerInvariant()] = $true
    }
    foreach ($directory in Get-ChildItem -LiteralPath $components -Directory -Force) {
        if (-not $allowed.ContainsKey($directory.Name.ToLowerInvariant())) {
            New-Operation 'non-product-component' $directory.FullName
        }
    }
}

$yttm = Join-Path $profile 'yttm'
if (Test-Path -LiteralPath $yttm) {
    $allowedYttm = @{}
    foreach ($name in $policy.yttmAllowedChildren) {
        $allowedYttm[$name.ToLowerInvariant()] = $true
    }
    foreach ($child in Get-ChildItem -LiteralPath $yttm -Force) {
        if (-not $allowedYttm.ContainsKey($child.Name.ToLowerInvariant())) {
            New-Operation 'non-canonical-yttm-content' $child.FullName
        }
    }
}

$after = Measure-Tree $profile
[int64]$reclaimableFiles = 0
[int64]$reclaimableBytes = 0
foreach ($operation in $operations) {
    $reclaimableFiles += [int64]$operation.files
    $reclaimableBytes += [int64]$operation.bytes
}
$manifest = [ordered]@{
    schema = 'nekorox.profile-normalization.v1'
    createdUtc = [DateTime]::UtcNow.ToString('o')
    mode = if ($Apply) { 'apply' } else { 'dry-run' }
    profile = $profile
    policy = [IO.Path]::GetFullPath($PolicyPath)
    before = $before
    after = $after
    reclaimed = [ordered]@{
        files = [int64]$before.files - [int64]$after.files
        bytes = [int64]$before.bytes - [int64]$after.bytes
    }
    reclaimable = [ordered]@{
        files = $reclaimableFiles
        bytes = $reclaimableBytes
    }
    operations = $operations
}

if ([string]::IsNullOrWhiteSpace($ManifestPath)) {
    $ManifestPath = Join-Path $repo 'artifacts\profile-normalization.json'
}
$manifestFull = [IO.Path]::GetFullPath($ManifestPath)
$manifestParent = Split-Path -Parent $manifestFull
New-Item -ItemType Directory -Force -Path $manifestParent | Out-Null
$manifest | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $manifestFull `
    -Encoding UTF8

$reportedBytes = if ($Apply) { $manifest.reclaimed.bytes } else { $reclaimableBytes }
$verb = if ($Apply) { 'reclaimed' } else { 'reclaimable' }
Write-Host ("Profile normalization {0}: {1} operation(s), {2:N0} bytes {3}." -f `
    $manifest.mode, $operations.Count, $reportedBytes, $verb)
Write-Host "Manifest: $manifestFull"
