[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory,
    [int]$LoadedProcessId = 0
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $repository '.git'))) {
    throw "Not a Git working tree: $repository"
}

function Get-TextSha256([string]$Text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
    } finally {
        $sha.Dispose()
    }
}

function Get-FileRecord([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $item = Get-Item -LiteralPath $Path
    [ordered]@{
        path = $item.FullName
        bytes = $item.Length
        last_write_utc = $item.LastWriteTimeUtc.ToString('o')
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $item.FullName).Hash
    }
}

Push-Location $repository
try {
    $head = (& git rev-parse HEAD).Trim()
    $branch = (& git branch --show-current).Trim()
    $status = @(& git status --porcelain=v1 --untracked-files=all)
    $diffText = ((& git diff --binary --no-ext-diff) -join "`n")

    $scrollCpp = Get-FileRecord (Join-Path $repository 'src\core\src\scroll_controller.cpp')
    $scrollHeader = Get-FileRecord (Join-Path $repository 'src\core\include\nekorox\scroll_controller.h')
    $expectedScrollCpp = 'EEA4F54B38706A2B7AEA8F9F803CE1AE7EFAE52DAE0BD0CE5A7FC50E4D21EBEC'
    $expectedScrollHeader = '7A0FE4E23AD4E84E52224DBBE90E4375A921F7585FD0C498527717D666268D99'
    if ($scrollCpp.sha256 -ne $expectedScrollCpp -or
        $scrollHeader.sha256 -ne $expectedScrollHeader) {
        throw 'BASE02 failed: approved ScrollController sources changed.'
    }

    $msbuild = & (Join-Path $repository 'tools\resolve-msbuild.ps1')
    $msbuildRecord = Get-FileRecord $msbuild
    $msbuildRecord['version'] = (Get-Item -LiteralPath $msbuild).VersionInfo.FileVersion

    $fxc = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe'
    $fxcRecord = Get-FileRecord $fxc
    if ($null -ne $fxcRecord) {
        $fxcRecord['version'] = (Get-Item -LiteralPath $fxc).VersionInfo.FileVersion
    }

    $builtDll = Get-FileRecord (Join-Path $repository 'out\Release\foo_nekorox.dll')
    $builtPdb = Get-FileRecord (Join-Path $repository 'out\Release\foo_nekorox.pdb')
    $loaded = @()
    if ($LoadedProcessId -gt 0) {
        $process = Get-Process -Id $LoadedProcessId -ErrorAction Stop
        foreach ($module in @($process.Modules)) {
            if ($module.ModuleName -ieq 'foo_nekorox.dll') {
                $loaded += [ordered]@{
                    process_id = $process.Id
                    process_path = $process.Path
                    module = Get-FileRecord $module.FileName
                }
            }
        }
    }

    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
        $OutputDirectory = Join-Path $repository "artifacts\r3\$timestamp\baseline"
    }
    $output = [IO.Path]::GetFullPath($OutputDirectory)
    New-Item -ItemType Directory -Path $output -Force | Out-Null

    $goldenVector = [ordered]@{
        impulse_pixels = 420.0
        initial_position = 500.0
        target_position = 920.0
        samples = @(
            [ordered]@{ seconds = 1.0 / 240.0; position = 503.417500135557 },
            [ordered]@{ seconds = 1.0 / 165.0; position = 506.947967307005 },
            [ordered]@{ seconds = 1.0 / 144.0; position = 508.954799836039 },
            [ordered]@{ seconds = 1.0 / 60.0; position = 542.199834635539 },
            [ordered]@{ seconds = 0.05; position = 699.529002349836 },
            [ordered]@{ seconds = 0.10; position = 848.095472182162 },
            [ordered]@{ seconds = 0.25; position = 918.731951266529 },
            [ordered]@{ seconds = 1.00; position = 920.0 }
        )
    }

    $manifest = [ordered]@{
        schema = 'goden.r3.baseline.v1'
        captured_utc = [DateTime]::UtcNow.ToString('o')
        repository = $repository
        git = [ordered]@{
            head = $head
            branch = $branch
            dirty = $status.Count -gt 0
            status_porcelain = $status
            unstaged_diff_utf8_sha256 = Get-TextSha256 $diffText
        }
        frozen_scroll_physics = [ordered]@{
            controller_cpp = $scrollCpp
            controller_header = $scrollHeader
            golden_qpc_vector = $goldenVector
            golden_qpc_vector_sha256 = Get-TextSha256 ($goldenVector | ConvertTo-Json -Depth 8 -Compress)
        }
        toolchain = [ordered]@{
            msbuild = $msbuildRecord
            fxc = $fxcRecord
            platform_toolset = 'v142'
            windows_sdk = '10.0.19041.0'
        }
        built_artifacts = [ordered]@{
            dll = $builtDll
            pdb = $builtPdb
        }
        loaded_artifacts = $loaded
        safety = [ordered]@{
            personal_profile_used_as_fixture = $false
            destructive_media_operations = $false
        }
    }
    $manifestPath = Join-Path $output 'baseline.json'
    $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding utf8
    Write-Output $manifestPath
} finally {
    Pop-Location
}
