# build.ps1 - build nui-plugin.xex (guest) from tools/nui-spike/plugin/guest.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File guest\build.ps1
#
# No admin, no MSYS2, no WSL. Requires the prebuilt xenon-toolchain release
# unpacked at <repo>\..\xenon-toolchain-work\xenon, with the MSYS2 runtime DLLs
# dropped into its bin. See docs/kinect_spike_runbook.md 12 for the recipe.
#
# Produces tools/nui-spike/plugin/guest/nui-plugin.xex, which xenia loads
# directly when passed on the command line.

$ErrorActionPreference = "Stop"

$Guest  = Split-Path -Parent $MyInvocation.MyCommand.Path
$Plugin = Split-Path -Parent $Guest
$Repo   = Split-Path (Split-Path (Split-Path $Plugin))
$Out    = Join-Path $Guest "out"

# The toolchain lives outside the repo on purpose: it is a multi-hundred-MB
# unpacked toolchain plus vendored fork copies, and it is not source.
$ToolchainRoot = Join-Path (Split-Path $Repo) "xenon-toolchain-work"
$Bin  = Join-Path $ToolchainRoot "xenon\bin"
$Py   = "C:\Users\sparky\.venvs\xenon\Scripts\python.exe"

foreach ($p in @($Bin, $Py)) {
  if (-not (Test-Path $p)) { throw "missing $p - see docs/kinect_spike_runbook.md 12" }
}

$env:PATH = "$Bin;$env:PATH"
New-Item -ItemType Directory -Force $Out | Out-Null

$Includes = @("$Plugin\include", "$Plugin\src")
$CFLAGS = @(
  "-c","-m32","-mcpu=cell","-mtune=cell","-maltivec","-fno-pic",
  "-O2","-fno-common","-fno-builtin","-nostdlib","-ffreestanding","-Wall"
) + ($Includes | ForEach-Object { "-I$_" })

$sources = @(
  @("$Plugin\src\nui_decode.c",     "nui_decode.o"),
  @("$Plugin\src\nui_frame.c",      "nui_frame.o"),
  @("$Plugin\src\nui_ipc_logic.c",  "nui_ipc_logic.o"),
  @("$Plugin\src\nui_ipc.c",        "nui_ipc.o"),
  @("$Guest\nui_plugin.c",          "nui_plugin.o")
)

Write-Host "== compiling guests =="
$objects = @()
foreach ($s in $sources) {
  $obj = Join-Path $Out $s[1]
  & xenon-gcc.exe @CFLAGS $s[0] -o $obj
  if ($LASTEXITCODE -ne 0) { throw "compile failed: $($s[0])" }
  $objects += $obj
}

Write-Host "== assembling thunks.S =="
& xenon-gcc.exe -c -m32 -mcpu=cell -maltivec -fno-pic -x assembler-with-cpp `
    "$Guest\thunks.S" -o "$Out\thunks.o"
if ($LASTEXITCODE -ne 0) { throw "asm failed" }

Write-Host "== linking flat Xenon image =="
& xenon-ld.exe -m elf32lppc -T "$Guest\plugin.ld" -o "$Out\plugin.elf" @objects "$Out\thunks.o"
if ($LASTEXITCODE -ne 0) { throw "link failed" }

Write-Host "== objcopy to flat binary =="
& xenon-objcopy.exe -O binary "$Out\plugin.elf" "$Out\flat.bin"
if ($LASTEXITCODE -ne 0) { throw "objcopy failed" }

$entryLine = & xenon-nm.exe "$Out\plugin.elf" | Where-Object { $_ -match " T _start$" }
if (-not $entryLine) { throw "no _start symbol" }
$entry = [Convert]::ToUInt32(($entryLine -split "\s+")[0], 16)
Write-Host ("   _start = 0x{0:X8}" -f $entry)

Write-Host "== packing XEX2 =="
& $Py "$Guest\mkxex.py" --flat "$Out\flat.bin" --flat-rva 0x1000 `
    --base 0x82000000 --entry ("0x{0:X8}" -f $entry) `
    --stack-size 0x40000 --out "$Guest\nui-plugin.xex"
if ($LASTEXITCODE -ne 0) { throw "mkxex failed" }

Write-Host ""
Write-Host "OK -> $Guest\nui-plugin.xex"
& xenon-objdump.exe -h "$Out\plugin.elf"