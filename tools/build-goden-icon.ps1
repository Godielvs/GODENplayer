[CmdletBinding()]
param(
    [string]$Output = '',
    [string]$PreviewOutput = '',
    [string]$AssetOutput = '',
    [string]$ManifestOutput = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $repo 'packaging/brand/goden.ico' }
$renderer = Join-Path $PSScriptRoot 'icon-assets/render.cjs'
if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'icon-assets/node_modules/sharp'))) {
    throw 'Icon tooling is not installed. Run: npm --prefix tools/icon-assets ci --ignore-scripts'
}
$node = (Get-Command node -ErrorAction Stop).Source
& $node $renderer $Output $AssetOutput $PreviewOutput $ManifestOutput
if ($LASTEXITCODE -ne 0) { throw 'Could not render the approved GODEN B icon.' }
