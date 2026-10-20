param([Parameter(Mandatory=$true)][string]$DevRoot,[string]$OutputPath='',[Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$Track)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$root=[IO.Path]::GetFullPath($DevRoot).TrimEnd('\')
if(!($root+'\').StartsWith([IO.Path]::GetFullPath((Join-Path $repo 'artifacts')).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Disposable QA only'}
$app=Join-Path $root 'app\foobar2000.exe';$probe=Join-Path $repo 'out\Release\nekorox_ui_probe.exe'
if(!$OutputPath){$OutputPath=Join-Path $root 'scroll-resources.json'}
Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;
public static class ScrollResourceInput {
 public delegate bool EnumProc(IntPtr h,IntPtr p);
 [DllImport("user32.dll")]static extern bool EnumChildWindows(IntPtr h,EnumProc e,IntPtr p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern bool GetCursorPos(out Point p);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out Rect r);
 [DllImport("user32.dll")]static extern uint SendInput(uint n,Input[] v,int size);
 [StructLayout(LayoutKind.Sequential)]public struct Point{public int x,y;}
 [StructLayout(LayoutKind.Sequential)]public struct Rect{public int l,t,r,b;}
 [StructLayout(LayoutKind.Explicit,Size=40)]struct Input{[FieldOffset(0)]public uint type;[FieldOffset(16)]public uint data;[FieldOffset(20)]public uint flags;}
 public static IntPtr Workspace(IntPtr root){IntPtr result=IntPtr.Zero;EnumChildWindows(root,(h,p)=>{var c=new StringBuilder(128);GetClassName(h,c,128);if(c.ToString()=="NekoRoXWorkspaceV3")result=h;return true;},IntPtr.Zero);return result;}
 public static Point Position(IntPtr h){Rect r;GetWindowRect(h,out r);return new Point{x=r.l+(r.r-r.l)*3/4,y=r.t+(r.b-r.t)/2};}
 public static bool Wheel(int delta){return SendInput(1,new[]{new Input{type=0,data=unchecked((uint)delta),flags=0x800}},40)==1;}
}
'@
function Probe([string[]]$a){$r=(& $probe $app @a --background 2>&1 | ForEach-Object {$_.ToString()}) -join "`n";if($LASTEXITCODE){throw $r};return $r}
if(@(Get-CimInstance Win32_Process -Filter "Name='foobar2000.exe'" | Where-Object ExecutablePath -eq $app).Count){throw 'Fixture already running'}
$env:NEKOROX_QA_HOOKS='1';$env:NEKOROX_QA_PROFILE='1'
try{$player=Start-Process $app -WindowStyle Hidden -PassThru}finally{Remove-Item Env:NEKOROX_QA_HOOKS,Env:NEKOROX_QA_PROFILE -ErrorAction SilentlyContinue}
$rows=[Collections.Generic.List[object]]::new()
try {
 Start-Sleep -Seconds 3
 Probe @('window-visible-focus') | Out-Null;Probe @('window-resize','1600x1000') | Out-Null
 Probe @('volume-set','1') | Out-Null;Probe @('drop-file',$Track) | Out-Null
 Start-Sleep -Seconds 2;Probe @('shell-library') | Out-Null
 # Keep the identical cloned Library selections for the paired measurement.
 Probe @('window-visible-focus') | Out-Null
 $player.Refresh();$window=$player.MainWindowHandle;$workspace=[ScrollResourceInput]::Workspace($window)
 if($workspace -eq [IntPtr]::Zero){throw 'Workspace missing'}
 $position=[ScrollResourceInput]::Position($workspace)
 [ScrollResourceInput]::SetCursorPos($position.x,$position.y) | Out-Null
 Start-Sleep -Milliseconds 500
 foreach($scenario in @('stationary','forward','reverse','forward-again')){
  $before=Probe @('workspace-render-state');$player.Refresh();$cpu=$player.TotalProcessorTime.TotalSeconds
  $clock=[Diagnostics.Stopwatch]::StartNew();$inputs=0
  while($clock.ElapsedMilliseconds -lt 4000){
   if([ScrollResourceInput]::GetForegroundWindow() -ne $window){throw ('Focus changed during resource test; actual='+[ScrollResourceInput]::GetForegroundWindow()+' expected='+$window)}
   $cursor=[ScrollResourceInput+Point]::new();[ScrollResourceInput]::GetCursorPos([ref]$cursor)|Out-Null
   if($cursor.x -ne $position.x -or $cursor.y -ne $position.y){throw 'Human cursor moved; measurement interrupted'}
   if($scenario -ne 'stationary'){$delta=if($scenario -eq 'reverse'){120}else{-120};if(![ScrollResourceInput]::Wheel($delta)){throw 'SendInput failed'};$inputs++}
   Start-Sleep -Milliseconds 40
  }
  $player.Refresh();$elapsed=$clock.Elapsed.TotalSeconds;$deltaCpu=$player.TotalProcessorTime.TotalSeconds-$cpu
  $rows.Add([ordered]@{scenario=$scenario;seconds=$elapsed;wheelInputs=$inputs;cpuSeconds=$deltaCpu;machinePercent=100*$deltaCpu/$elapsed/[Environment]::ProcessorCount;privateMiB=$player.PrivateMemorySize64/1MB;before=$before;after=(Probe @('workspace-render-state'));playback=(Probe @('playback-state'));header=(Probe @('header-paint-state'))})
 }
 [ordered]@{app=$app;coreSha256=(Get-FileHash -LiteralPath (Join-Path $root 'app\profile\user-components-x64\foo_nekorox\foo_nekorox.dll')).Hash;input='Real SendInput wheel;40ms interval;focus/cursor interruption guard';logicalProcessors=[Environment]::ProcessorCount;rows=$rows;status='pass'}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $OutputPath -Encoding UTF8
 $rows | ForEach-Object {Write-Output ($_.scenario+': '+[Math]::Round($_.machinePercent,3)+'% CPU')}
}finally {Start-Process $app -ArgumentList '/exit' -WindowStyle Hidden -Wait; if(!$player.WaitForExit(10000)){throw 'Fixture failed to close normally'}}
