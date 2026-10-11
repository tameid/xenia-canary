# Verifies the whole NUI pipeline in one run: the provider publishes depth, the
# guest reads frames off disk, and the XUsbcam* device answers guest calls.
#
# Exists because every part of this has been debugged separately, and each
# failure so far looked like success from one angle and failure from another.
# One command that asserts all of them beats another log to read.
#
#   .\smoke.ps1 [-Seconds 12] [-Store <dir>]
#
# Builds nothing. Run guest\build.ps1 first; this reports a missing xex rather
# than silently testing a stale one.

param(
  [int]$Seconds = 12,
  [string]$Store = "$env:TEMP\nuismoke"
)

$ErrorActionPreference = 'Stop'
# $PSScriptRoot is reliable under -File and dot-sourcing alike; $MyInvocation.Path
# is null in some hosts. Fall back to the invocation record, then to the cwd.
$here = $PSScriptRoot
if (-not $here) { $here = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $here) { $here = (Get-Location).Path }
$plugin = Join-Path $here 'plugin'
$xex = Join-Path $plugin 'guest\nui-plugin.xex'
$xenia = Join-Path $here '..\..\build\bin\Windows\Release\xenia_canary.exe'
$provider = 'C:\Users\sparky\Repos\nui-provider\build\bin\nui-provider.exe'

foreach ($p in @($xex, $xenia, $provider)) {
  if (-not (Test-Path $p)) { Write-Host ('missing: ' + $p) -ForegroundColor Red; exit 1 }
}

Remove-Item $Store -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $Store -Force | Out-Null
Copy-Item $xex $Store -Force
Copy-Item $xenia $Store -Force

$log = Join-Path $Store 'run.log'
$prov = Start-Process $provider -ArgumentList ('--dir "' + $Store + '" --depth --interval-ms 33') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
$emu = Start-Process (Join-Path $Store 'xenia_canary.exe') -ArgumentList ('--enable_console --log_level 3 --log_file "' + $log + '" --storage_root "' + $Store + '" "' + (Join-Path $Store 'nui-plugin.xex') + '"') -PassThru -WindowStyle Hidden
Start-Sleep -Seconds $Seconds
foreach ($p in @($emu, $prov)) { $p | Stop-Process -Force -ErrorAction SilentlyContinue }

function Has([string]$pattern) {
  if (-not (Test-Path $log)) { return $false }
  return [bool](Select-String -Path $log -Pattern $pattern -Quiet -ErrorAction SilentlyContinue)
}

$frameFile = Join-Path $Store 'frame.0.bin'
$frameBytes = 0
if (Test-Path $frameFile) { $frameBytes = (Get-Item $frameFile).Length }

$results = @()
$results += ,@('provider published depth frames', $frameBytes -gt 100000)
$results += ,@('camera probe ran to completion', (Has 'camera end'))
$results += ,@('XUsbcamReadFrame answered guest code', (Has 'cam_readframe_rc='))
$results += ,@('device state flag took effect', (Has 'status_after=0'))
$results += ,@('guest decoded a live frame', (Has 'frame_id='))

$fail = 0
Write-Host ''
foreach ($r in $results) {
  if ($r[1]) { Write-Host ('  PASS  ' + $r[0]) }
  else { Write-Host ('  FAIL  ' + $r[0]) -ForegroundColor Red; $fail++ }
}

Write-Host ''
Write-Host ('log: ' + $log)
if ($fail -eq 0) { Write-Host 'all checks passed' -ForegroundColor Green; exit 0 }
Write-Host ($fail.ToString() + ' check(s) failed') -ForegroundColor Red
exit 1