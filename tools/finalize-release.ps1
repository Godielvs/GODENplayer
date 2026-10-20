param(
    [Parameter(Mandatory=$true)][string]$InstallerDirectory,
    [Parameter(Mandatory=$true)][string]$ReleaseManifest,
    [Parameter(Mandatory=$true)][string]$TransactionEvidence,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$SourceExport = '',
    [string[]]$Observation = @()
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$arguments = @('-B', (Join-Path $PSScriptRoot 'finalize-public-delivery.py'),
    '--installer-directory', $InstallerDirectory, '--release-manifest', $ReleaseManifest,
    '--transaction-evidence', $TransactionEvidence, '--output-directory', $OutputDirectory)
if ($SourceExport) {$arguments += @('--source-export', $SourceExport)}
foreach ($item in $Observation) {$arguments += @('--observation', $item)}
& python @arguments
if ($LASTEXITCODE -ne 0) {throw 'Public delivery finalization failed. No approval or publication was performed.'}
