param([string]$RepoRoot='')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (!$RepoRoot) {$RepoRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))}
$builder=Get-Content (Join-Path $RepoRoot 'tools/build-friend-installer.ps1') -Raw
$iss=Get-Content (Join-Path $RepoRoot 'packaging/friend-installer/NekoRoX-Friend.iss') -Raw
$qa=Get-Content (Join-Path $RepoRoot 'src/component/qa_hooks.h') -Raw
function Require([bool]$condition,[string]$message) {if (!$condition) {throw $message}}
Require ($builder -match 'CorrespondingSources' -and $builder -match 'public_release.py') 'Corresponding sources are not validated.'
Require ($builder -match '530E6C9DB829874BC250AF038C1D07F8B42FEF61C5DD99F7EDCDE6393A93EE23' -and $builder -match 'Get-AuthenticodeSignature') 'Official installer is not pinned and authenticated.'
Require ($iss -match '653CC120C146AAAE9E6DB9B6F19E5A1588407B8940BC1521F0CED739FF8924B0' -and $iss -notmatch 'DpiPatcher|DpiManifest') 'Official host is modified or unchecked.'
Require ($iss -match 'CoverPrivacyPage.Values\[0\] := True' -and $iss -match 'ALLOW_PUBLIC_COVERS' -and $iss -match 'catbox-disabled') 'Cover disclosure/silent consent contract is absent.'
Require ($iss -match 'PageID = CoverPrivacyPage.ID' -and $iss -match 'HasExistingInstallAtRoot\(WizardDirValue\(\)\)') 'Upgrade does not preserve cover preferences.'
Require ($iss -match 'TryInstallStatistics' -and $iss -match 'DownloadTemporaryFile' -and $iss -match '4E47C44B8624544EF618FC02999C1FBBC212534507888A871625B83E949E4014' -and $iss -match 'DeleteFile\(Candidate\)' -and $builder -notmatch "'user-components-x64\\foo_playcount") 'Statistics download is not optional, verified and transactional.'
Require ($iss.IndexOf('  WriteTransactionManifest;') -lt $iss.IndexOf('  if not CommitStaging then')) 'Transaction evidence is prepared after swapping.'
Require ($iss -match 'RestoreFailedCommit' -and $iss -match 'app.rollback.pending' -and $iss -match 'MoveFileExW') 'Checked rollback is absent.'
Require ($builder -match 'shaderRendererBinary' -and $builder -match 'DShaderSha256' -and $iss -match 'shaderRendererSha256' -and ([regex]::Matches($iss,"PayloadHashMatches\(ShaderPath").Count -eq 2)) 'Independent renderer is not checked before and after transaction commit.'
Require ($iss -match 'AppId=\{code:InstallerAppId\}' -and $iss -match 'CreateUninstallRegKey=not QaMode' -and $iss -match 'UsePreviousAppDir=not QaMode') 'QA installer registration is not isolated.'
Require ($iss -match 'function RecoveryBlocker' -and $iss -match 'function InitializeUninstall' -and $iss -notmatch 'Type: filesandordirs; Name: "\{app\}\\app.rollback.pending"') 'Incomplete recovery can be deleted.'
Require ($iss -match 'function GetCustomSetupExitCode' -and $iss -match 'Check: CanLaunchInstalledApp' -and $iss -match 'install-recovery.pending') 'Incomplete installation can report success or launch the player.'
Require ($qa -match '#define GODEN_ENABLE_QA_HOOKS 0' -and $qa -match 'hooksAllowed') 'Production QA boundary is absent.'
& (Join-Path $PSScriptRoot 'installer_consent_reader_tests.ps1') -RepoRoot $RepoRoot
Write-Host 'Public installer source contracts: PASS (runtime qualification remains separate)'
