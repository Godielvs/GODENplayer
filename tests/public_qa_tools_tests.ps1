# Offline runner/AST fixtures. Never launches the player, UI probes, or real DSP.
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$scripts=[ordered]@{
    'c9-w3-counters.ps1'=@('FixtureAlbum','FixtureArtist','FixtureTitle')
    'run-r5-installed-soak.ps1'=@('MediaRoot')
    'run-r6-ambient-qualification.ps1'=@('MediaRoot')
    'run-r6-ambient-soak.ps1'=@('MediaRoot')
    'test-album-glyphflow-transition.ps1'=@('FirstTrack','SecondTrack')
    'test-album-scroll.ps1'=@('AppRoot','FixtureAlbum','FixtureArtist','FixtureTitle')
    'test-library-scroll-resources.ps1'=@('Track')
    'test-scroll-during-artwork-transition.ps1'=@('FirstTrack','SecondTrack')
}
foreach ($name in $scripts.Keys) {
    $tokens=$null; $parseErrors=$null
    $ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo "tools\$name"), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) {throw "QA script parse failed: $name"}
    foreach ($parameter in $scripts[$name]) {
        $entry=@($ast.ParamBlock.Parameters | Where-Object {$_.Name.VariablePath.UserPath -eq $parameter})
        if ($entry.Count -ne 1 -or $null -ne $entry[0].DefaultValue -or
                $entry[0].Extent.Text -notmatch 'Mandatory\s*=\s*\$true' -or
                $entry[0].Extent.Text -notmatch 'ValidateNotNullOrEmpty') {
            throw "QA media/app input must be explicit and nonempty: $name / $parameter"
        }
    }
}
$sandbox=Join-Path ([IO.Path]::GetTempPath()) ('goden-qa-runner-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $sandbox | Out-Null
function Check([bool]$Condition, [string]$Message) {if (-not $Condition) {throw $Message}}
function Invoke-Fixture([string]$Report, [int]$Runs=2, [switch]$Paired) {
    $arguments=@('-NoProfile','-NonInteractive','-File', (Join-Path $repo 'tools\test-master-dsp-performance.ps1'),
        '-CandidateExecutable', (Join-Path $sandbox 'candidate.ps1'), '-ManifestPath', $Report, '-Runs', $Runs)
    if ($Paired) {$arguments+=@('-BaselineExecutable', (Join-Path $sandbox 'baseline.ps1'))}
    & (Join-Path $PSHOME 'pwsh.exe') @arguments *> (Join-Path $sandbox 'runner.log')
    return $LASTEXITCODE
}
try {
    $candidate=@'
$counter=Join-Path $PSScriptRoot 'counter.txt'
$value=if (Test-Path $counter) {[int](Get-Content $counter -Raw)} else {0}
Set-Content $counter ($value+1)
$timing=if ((Test-Path (Join-Path $PSScriptRoot 'fail')) -and $value -eq 0) {'0.5'} else {'0.2'}
if (Test-Path (Join-Path $PSScriptRoot 'huge')) {$timing='9'*1000}
if (Test-Path (Join-Path $PSScriptRoot 'malformed')) {'INVALID TIMINGS'} else {
    "Common audiophile chain p99: $timing"
    'Creative 12-module chain p99: 0.6'
}
$global:LASTEXITCODE=0
'@
    Set-Content (Join-Path $sandbox 'candidate.ps1') $candidate -Encoding UTF8
    Set-Content (Join-Path $sandbox 'baseline.ps1') @'
'Common audiophile chain p99: 0.3'
'Creative 12-module chain p99: 0.7'
$global:LASTEXITCODE=0
'@ -Encoding UTF8
    $paired=Join-Path $sandbox 'paired.json'
    Check ((Invoke-Fixture $paired -Paired) -eq 0) 'Paired fixture failed.'
    $report=Get-Content $paired -Raw | ConvertFrom-Json
    Check ($report.schema -eq 'goden.master-dsp-performance.v2' -and $report.passed -eq $true) 'Wrong runner schema/outcome.'
    Check (($report.samples.role -join ',') -eq 'baseline,candidate,candidate,baseline') 'Paired order is biased or incomplete.'
    Check ($report.samples.Count -eq 4 -and $report.summary.candidate.samples -eq 2) 'Planned samples missing.'
    Check ($report.executableSha256.candidate -eq (Get-FileHash (Join-Path $sandbox 'candidate.ps1')).Hash.ToLowerInvariant()) 'Executable identity mismatch.'
    Check ($report.wholePlayerPerformanceMeasured -eq $false) 'Fixture falsely claims player measurement.'
    # Verify a failure is retained even when the later planned sample passes.
    Remove-Item -LiteralPath (Join-Path $sandbox 'counter.txt')
    Set-Content (Join-Path $sandbox 'fail') 'fixture'
    $failed=Join-Path $sandbox 'failed.json'
    Check ((Invoke-Fixture $failed) -ne 0) 'Timing failure was accepted.'
    $report=Get-Content $failed -Raw | ConvertFrom-Json
    Check ($report.passed -eq $false -and $report.samples.Count -eq 2) 'Failed report or planned sample lost.'
    Check ($report.samples[0].passed -eq $false -and $report.samples[1].passed -eq $true) 'Runner discarded a failed sample.'
    $originalHash=(Get-FileHash $failed).Hash
    Check ((Invoke-Fixture $failed) -ne 0) 'Existing measurement was overwritten.'
    Check ((Get-FileHash $failed).Hash -eq $originalHash) 'Failed report changed on rerun.'
    Set-Content (Join-Path $sandbox 'malformed') 'fixture'
    $malformed=Join-Path $sandbox 'malformed.json'
    Check ((Invoke-Fixture $malformed -Runs 1) -ne 0) 'Malformed output was accepted.'
    $report=Get-Content $malformed -Raw | ConvertFrom-Json
    Check ($report.samples[0].parsed -eq $false -and $null -eq $report.samples[0].commonP99Ms) 'Missing metrics became a passing zero.'
    Remove-Item -LiteralPath (Join-Path $sandbox 'malformed')
    Set-Content (Join-Path $sandbox 'huge') 'fixture'
    $huge=Join-Path $sandbox 'huge.json'
    Check ((Invoke-Fixture $huge -Runs 1) -ne 0) 'Nonfinite timing was accepted.'
    $report=Get-Content $huge -Raw | ConvertFrom-Json
    Check ($report.samples[0].parsed -eq $false -and $null -eq $report.samples[0].commonP99Ms -and $report.samples[0].creativeP99Ms -eq 0.6) 'Overflow lost the report or the other valid metric.'
    Write-Host 'Public QA tooling fixtures: PASS (8 AST contracts; paired, failed, immutable, malformed and overflow runner cases).'
} finally {
    $absolute=[IO.Path]::GetFullPath($sandbox)
    $parent=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if (-not $absolute.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($absolute) -notlike 'goden-qa-runner-*') {throw 'Unsafe fixture cleanup path.'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
