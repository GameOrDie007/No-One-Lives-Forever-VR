# Builds the proxy DDRAW.dll and stages it beside lithtech.exe.
#
# d3d.ren imports DDRAW.dll by name (verified from its import table: DDRAW,
# WINMM, KERNEL32, USER32) and Windows searches the executable's directory
# before System32, so a DLL of that name in game\ is loaded instead of the
# system one. Same mechanism as the proxy DINPUT.dll.
#
# This is the seam the whole transport question turns on. docs/M4-TRANSPORT.md
# chose window capture because the CLIENT SDK cannot read pixels in bulk - which
# is true, and says nothing about reading them here, one layer down, inside the
# game process.
#
# Modes, via the NOLFVR_DDRAW environment variable:
#   pass    forward everything and wrap nothing. THE CONTROL - run this first.
#   watch   (default) find the back buffer and time a full readback.
#
# Reverting is deleting game\DDRAW.dll. Nothing else is touched.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $Root 'host\ddrawproxy'
$Obj  = Join-Path $Root 'build\ddrawproxy'
$Out  = Join-Path $Obj  'DDRAW.dll'
$Game = Join-Path $Root 'game'

New-Item -ItemType Directory -Force $Obj | Out-Null

# Stage ddrawreal.dll: a copy of the SYSTEM DirectDraw under a name this proxy
# can forward to.
#
# d3dim700.dll imports thirteen internal entry points from ddraw.dll
# (AcquireDDThreadLock and friends). A proxy that does not export them is fine
# until Direct3D initialises and then fails with "Couldn't set D3D Emulation
# mode". They are forwarded in ddraw_proxy.def, and a forward needs a module
# NAME that is not "ddraw" - which is this proxy.
#
# 32-bit: lithtech.exe is x86, so the copy must come from SysWOW64, NOT
# System32. Taking the 64-bit one would produce a DLL that cannot load and a
# failure that looks like a missing export.
#
# Copied on this machine at build time. It is a Windows system file and must
# never be redistributed - a release has to create it on the user's machine.
$sysDD = Join-Path $env:WINDIR 'SysWOW64\ddraw.dll'
if (-not (Test-Path $sysDD)) { throw "32-bit ddraw.dll not found at $sysDD" }

$realOut = Join-Path $Game 'ddrawreal.dll'
$needCopy = -not (Test-Path $realOut)
if (-not $needCopy) {
    $needCopy = (Get-Item $sysDD).LastWriteTime -gt (Get-Item $realOut).LastWriteTime
}
if ($needCopy) {
    Copy-Item $sysDD $realOut -Force
    Write-Host "staged ddrawreal.dll (copy of $sysDD)" -ForegroundColor DarkGray
}

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvars)) { throw 'vcvarsall.bat not found.' }

# 32-bit, because lithtech.exe is. A 64-bit build would simply not load and the
# game would fall back to the system DDRAW without saying so - which would read
# as "the proxy did nothing" rather than "the proxy was never loaded".
$cl = @(
    '/nologo', '/O2', '/EHsc', '/W3', '/MT', '/LD',
    '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
    "/Fo:$Obj\", "/Fe:$Out",
    "`"$Src\ddraw_proxy.cpp`"",
    '/link', "/DEF:`"$Src\ddraw_proxy.def`"",
    # NOT ddraw.lib. With the import library on the line, the linker treats the
    # forwarders in the .def as ALIASES to local symbols and resolves the six it
    # can find there, failing on the other seven with LNK2001. Everything real
    # here goes through LoadLibrary/GetProcAddress anyway.
    'user32.lib', 'ole32.lib', 'dxguid.lib'
) -join ' '

# Delete the previous output FIRST.
#
# Without this a failed compile leaves the old DDRAW.dll in place, the
# "if (-not (Test-Path $Out))" check below passes on the stale file, and the
# script cheerfully stages a build that does not contain the change just made.
# That happened on 1 September: a C2374 compile error still reported
# "DDRAW.dll -> 128,512 bytes, staged in game\".
Remove-Item $Out -Force -ErrorAction SilentlyContinue

Write-Host 'Compiling proxy DDRAW.dll (x86)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" x86 >nul 2>&1 && cl $cl 2>&1"
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }

if (-not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'proxy build failed.'
}

# Refuse to ship a 64-bit build by accident.
$fs = [IO.File]::OpenRead($Out)
$br = New-Object IO.BinaryReader($fs)
$fs.Position = 0x3c
$pe = $br.ReadInt32()
$fs.Position = $pe + 4
$machine = $br.ReadUInt16()
$br.Close(); $fs.Close()
if ($machine -ne 0x14c) {
    throw ("built DDRAW.dll is machine 0x{0:X} - must be 0x14C (x86) to load into lithtech.exe" -f $machine)
}
Write-Host ("  machine 0x{0:X} (x86) - correct" -f $machine) -ForegroundColor DarkGray

Copy-Item $Out $Game -Force
"{0}  ->  {1:N0} bytes, staged in game\" -f 'DDRAW.dll', (Get-Item $Out).Length | Write-Host
Write-Host 'Writes game\logs\ddraw-proxy.log.' -ForegroundColor Green
Write-Host 'NOLFVR_DDRAW=pass forwards everything unchanged (the control).' -ForegroundColor DarkGray
Write-Host 'Delete game\DDRAW.dll to remove the proxy entirely.' -ForegroundColor DarkGray
