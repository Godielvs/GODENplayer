param([Parameter(Mandatory=$true)][string]$DevRoot,[string]$OutputPath='')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$root=[IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
if(!($root+'\').StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Disposable QA only'}
$app=Join-Path $root 'app\foobar2000.exe';$probe=Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if(!$OutputPath){$OutputPath=Join-Path $root 'glyphflow-visible.json'}
function Probe([string[]]$a){
 if($a[0] -eq 'window-native-maximize'){$output=& $probe $app @a 2>&1}
 else{$output=& $probe $app @a --background 2>&1}
 $r=($output|ForEach-Object {$_.ToString()})-join "`n"
 if($LASTEXITCODE){throw (($a -join ' ')+': '+$r)};return $r
}
function Number([string]$text,[string]$field){return [double][regex]::Match($text,$field+'=([0-9.]+)').Groups[1].Value}
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class GlyphVisibleInput {
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]static extern uint SendInput(uint n,Input[] v,int size);
 [StructLayout(LayoutKind.Explicit,Size=40)]struct Input{[FieldOffset(0)]public uint type;[FieldOffset(8)]public ushort key;[FieldOffset(12)]public uint flags;}
 public static bool Modifier(){return SendInput(2,new[]{new Input{type=1,key=0x11},new Input{type=1,key=0x11,flags=2}},40)==2;}
}
'@
$env:NEKOROX_QA_HOOKS='1';$env:NEKOROX_QA_PROFILE='1'
try{$player=Start-Process $app -WindowStyle Hidden -PassThru}finally{Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE -ErrorAction SilentlyContinue}
$rows=[Collections.Generic.List[object]]::new();$passed=$true
try {
 Start-Sleep -Seconds 3
 Probe @('window-visible-focus')|Out-Null;Probe @('window-native-maximize')|Out-Null
 Probe @('volume-set','1')|Out-Null
 $media=Get-Content (Join-Path $repo 'artifacts\six-visual-refinement-20261001\media.json') -Raw|ConvertFrom-Json
 Probe @('drop-file',$media[0].source)|Out-Null;Start-Sleep -Seconds 1
 Probe @('shell-album')|Out-Null;Probe @('shell-collection-select','12')|Out-Null
 Start-Sleep -Seconds 1
 $order=@(1,2,3,0,1,2)
 foreach($index in $order){
  $before=Probe @('shell-album-transition-state');$generation=Number $before 'generation'
  Probe @('drop-file',$media[$index].source)|Out-Null
  $player.Refresh();$window=$player.MainWindowHandle
  $samples=[Collections.Generic.List[string]]::new();$watch=[Diagnostics.Stopwatch]::StartNew();$observed=$false;$settled=$null;$inputSent=$false
  while($watch.ElapsedMilliseconds -lt 3000){
   if([GlyphVisibleInput]::GetForegroundWindow() -ne $window){throw 'Focus changed during GlyphFlow test'}
   $state=Probe @('shell-album-transition-state');$samples.Add($state)
   # Reproduce one cover click after the new raster arrives. Constant modifier
   # bursts stress accelerator routing and defer decorative shaders for the
   # whole animation, which is a different workload from a cover change.
   if(!$inputSent -and (Number $state 'generation') -gt $generation){
    if(![GlyphVisibleInput]::Modifier()){throw 'Hardware-equivalent key input failed'};$inputSent=$true
   }
   if((Number $state 'generation') -gt $generation){$observed=$true;if($state -match 'active=false'){$settled=$state;break}}
   Start-Sleep -Milliseconds 20
  }
  $ok=$observed -and $settled -and (Number $settled 'glyphFrames') -ge 20 -and
   (Number $settled 'acceptedFrames') -ge 20 -and (Number $settled 'fallbackFrames') -eq 0 -and
   (Number $settled 'p95IntervalMs') -le 33.34
  $passed=$passed -and $ok
  $rows.Add([ordered]@{track=$media[$index].title;pass=$ok;before=$before;settled=$settled;samples=$samples;inputCount=[int]$inputSent;window=(Probe @('window-state'));shader=(Probe @('shell-collection-render-state'))})
 }
 [ordered]@{passed=$passed;app=$app;presentation='Visible maximized hardware surface;unmuted playback;Radiance active;real harmless input during transition';coreSha256=(Get-FileHash -LiteralPath (Join-Path $root 'app\profile\user-components-x64\foo_nekorox\foo_nekorox.dll')).Hash;rows=$rows}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $OutputPath -Encoding UTF8
 if(!$passed){throw 'GlyphFlow visible cadence qualification failed; retained measurements show each failure'}
 $rows|ForEach-Object {Write-Output ($_.track+': '+$_.settled)}
}finally {Start-Process $app -ArgumentList /exit -WindowStyle Hidden -Wait;if(!$player.WaitForExit(10000)){throw 'Fixture failed to close normally'}}
