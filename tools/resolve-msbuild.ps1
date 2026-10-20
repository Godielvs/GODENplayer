Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$programFiles = [Environment]::GetFolderPath('ProgramFiles')
$candidates = [System.Collections.Generic.List[string]]::new()

$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
    $installations = & $vswhere -products * -requires Microsoft.Component.MSBuild `
        -property installationPath
    foreach ($installation in $installations) {
        if (-not [string]::IsNullOrWhiteSpace($installation)) {
            $candidates.Add((Join-Path $installation 'MSBuild\Current\Bin\amd64\MSBuild.exe'))
            $candidates.Add((Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'))
        }
    }
}

$candidates.Add((Join-Path $programFilesX86 'Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'))
$candidates.Add((Join-Path $programFilesX86 'Microsoft Visual Studio\2019\BuildTools\MSBuild\Current\Bin\MSBuild.exe'))
$candidates.Add((Join-Path $programFiles 'Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'))

$resolved = $candidates | Where-Object {
    if (-not (Test-Path -LiteralPath $_)) { return $false }
    # A newer, unrelated VS installation may have MSBuild but no v142 C++
    # tools. Select an installation that can build this repository's ABI.
    $marker = $_.IndexOf('\MSBuild\', [StringComparison]::OrdinalIgnoreCase)
    if ($marker -lt 0) { return $false }
    $root = $_.Substring(0, $marker)
    $toolsets = Get-ChildItem -LiteralPath (Join-Path $root 'VC\Tools\MSVC') `
        -Directory -Filter '14.29.*' -ErrorAction SilentlyContinue
    return @($toolsets | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\cl.exe')
    }).Count -gt 0
} | Select-Object -First 1
if (-not $resolved) {
    throw 'MSBuild with the C++ workload was not found.'
}
$resolved
