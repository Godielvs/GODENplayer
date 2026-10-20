[CmdletBinding()]
param(
    [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
$msbuild = & (Join-Path $repository 'tools\resolve-msbuild.ps1')
$solution = Join-Path $repository 'NekoRoX.sln'
$objectDirectory = Join-Path $repository 'out\obj\foo_nekorox\Release\crt-shaders'
$source = Join-Path $repository 'src\component\shaders\crt_xy.hlsl'
$stamp = Join-Path $objectDirectory 'crt-inputs.stamp'
$fxc = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe'
$outputs = @(
    'crt_fullscreen_vs.h',
    'crt_decay_ps.h',
    'crt_beam_vs.h',
    'crt_beam_ps.h',
    'crt_composite_ps.h'
) | ForEach-Object { Join-Path $objectDirectory $_ }

function Invoke-ComponentBuild([string]$Name) {
    $arguments = @(
        $solution,
        '/m',
        '/nodeReuse:false',
        '/t:foo_nekorox',
        '/p:Configuration=Release',
        '/p:Platform=x64',
        '/p:PlatformToolset=v142',
        '/p:WindowsTargetPlatformVersion=10.0.19041.0',
        '/v:normal',
        '/nologo'
    )
    $lines = @(& $msbuild @arguments 2>&1 |
        ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0) {
        throw "$Name build failed:`n$($lines -join "`n")"
    }
    [ordered]@{
        name = $Name
        fxc_output_writes = @($lines | Where-Object {
            $_ -match 'compilation header save succeeded'
        }).Count
        target_skipped = [bool]($lines | Where-Object {
            $_ -match '(Ignorando o destino|Skipping target).*CompileCrtShaders'
        })
    }
}

function Get-OutputSnapshot {
    $snapshot = [ordered]@{}
    foreach ($path in $outputs) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $snapshot[[IO.Path]::GetFileName($path)] = $null
            continue
        }
        $item = Get-Item -LiteralPath $path
        $snapshot[$item.Name] = [ordered]@{
            last_write_ticks = $item.LastWriteTimeUtc.Ticks
            bytes = $item.Length
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash
        }
    }
    $snapshot
}

function Assert-SameSnapshot($Before, $After, [string]$Message) {
    foreach ($path in $outputs) {
        $name = [IO.Path]::GetFileName($path)
        if ($null -eq $Before[$name] -or $null -eq $After[$name] -or
            $Before[$name].last_write_ticks -ne $After[$name].last_write_ticks -or
            $Before[$name].sha256 -ne $After[$name].sha256) {
            throw "$Message ($name)"
        }
    }
}

function Assert-AllRebuilt($Before, $After, [string]$Message) {
    foreach ($path in $outputs) {
        $name = [IO.Path]::GetFileName($path)
        if ($null -eq $After[$name] -or
            $After[$name].last_write_ticks -le $Before[$name].last_write_ticks) {
            throw "$Message ($name)"
        }
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $timestamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')
    $OutputDirectory = Join-Path $repository "artifacts\r3\$timestamp\crt-incremental"
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null

$sourceItem = Get-Item -LiteralPath $source
$originalSourceWriteUtc = $sourceItem.LastWriteTimeUtc
$missingPath = $outputs[1]
$backupPath = "$missingPath.r3-backup"
$results = @()

try {
    $results += Invoke-ComponentBuild 'prime'
    $prime = Get-OutputSnapshot

    $results += Invoke-ComponentBuild 'no-op'
    $noOp = Get-OutputSnapshot
    Assert-SameSnapshot $prime $noOp 'BUILD01 no-op rewrote a shader output'
    if ($results[-1].fxc_output_writes -ne 0 -or -not $results[-1].target_skipped) {
        throw 'BUILD01 no-op did not skip fxc.'
    }

    if (Test-Path -LiteralPath $backupPath) {
        throw "Refusing to overwrite prior probe backup: $backupPath"
    }
    Move-Item -LiteralPath $missingPath -Destination $backupPath
    $missingHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $backupPath).Hash
    $results += Invoke-ComponentBuild 'missing-output'
    if (-not (Test-Path -LiteralPath $missingPath -PathType Leaf) -or
        (Get-FileHash -Algorithm SHA256 -LiteralPath $missingPath).Hash -ne $missingHash) {
        throw 'BUILD01 missing shader output was not recreated exactly.'
    }
    Remove-Item -LiteralPath $backupPath

    $beforeSourceTouch = Get-OutputSnapshot
    (Get-Item -LiteralPath $source).LastWriteTimeUtc = [DateTime]::UtcNow
    Start-Sleep -Milliseconds 20
    $results += Invoke-ComponentBuild 'shader-input-timestamp'
    $afterSourceTouch = Get-OutputSnapshot
    Assert-AllRebuilt $beforeSourceTouch $afterSourceTouch `
        'BUILD01 shader input did not rebuild every dependent output'

    $beforeStampChange = Get-OutputSnapshot
    Set-Content -LiteralPath $stamp -Value 'R3 deliberate stamp invalidation' -Encoding utf8
    Start-Sleep -Milliseconds 20
    $results += Invoke-ComponentBuild 'compiler-flags-stamp'
    $afterStampChange = Get-OutputSnapshot
    Assert-AllRebuilt $beforeStampChange $afterStampChange `
        'BUILD01 compiler/flags stamp did not rebuild dependent outputs'

    $results += Invoke-ComponentBuild 'final-no-op'
    $finalNoOp = Get-OutputSnapshot
    Assert-SameSnapshot $afterStampChange $finalNoOp `
        'BUILD01 final no-op rewrote a shader output'
    if ($results[-1].fxc_output_writes -ne 0 -or -not $results[-1].target_skipped) {
        throw 'BUILD01 final no-op did not skip fxc.'
    }

    $report = [ordered]@{
        schema = 'goden.r3.crt-incremental.v1'
        status = 'pass'
        captured_utc = [DateTime]::UtcNow.ToString('o')
        source = $source
        compiler = (Get-Item -LiteralPath $fxc).FullName
        runs = $results
        final_outputs = $finalNoOp
    }
    $reportPath = Join-Path $output 'crt-incremental.json'
    $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Output $reportPath
} finally {
    (Get-Item -LiteralPath $source).LastWriteTimeUtc = $originalSourceWriteUtc
    if (Test-Path -LiteralPath $backupPath -PathType Leaf) {
        if (Test-Path -LiteralPath $missingPath -PathType Leaf) {
            Remove-Item -LiteralPath $backupPath
        } else {
            Move-Item -LiteralPath $backupPath -Destination $missingPath
        }
    }
}
