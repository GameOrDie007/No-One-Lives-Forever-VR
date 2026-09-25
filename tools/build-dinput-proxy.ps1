# Builds the proxy DINPUT.dll and stages it beside lithtech.exe.
#
# The startup fault is inside lithtech.exe's DirectInput init, before any of our
# code exists, so it cannot be fixed from CShell.dll. lithtech.exe imports
# DINPUT.dll by name and Windows searches the executable's directory before
# System32, so a DLL of that name in game\ is loaded instead of the system one.
#
# See host\dinputproxy\dinput_proxy.cpp for the modes. Default is FILTER, which
# skips the joystick enumeration that hangs and leaves keyboard and mouse alone.
# NOLFVR_DINPUT=pass forwards everything unchanged and is the control.
#
# Reverting is deleting game\DINPUT.dll. Nothing else is touched.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $Root 'host\dinputproxy'
$Obj  = Join-Path $Root 'build\dinputproxy'
$Out  = Join-Path $Obj  'DINPUT.dll'
$Game = Join-Path $Root 'game'

New-Item -ItemType Directory -Force $Obj | Out-Null

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path $vcvars)) { throw 'vcvarsall.bat not found.' }

# 32-bit, because lithtech.exe is. A 64-bit build would simply not load and the
# game would fall back to the system DINPUT without saying so.
# The version resource (see host\res\version.h), linked in with the source.
. (Join-Path $PSScriptRoot 'version-res.ps1')
$ver = New-VersionResCommand -ObjDir $Obj -FileName 'DINPUT.dll' -Description 'NOLF1 VR - DirectInput wrapper (keyboard input while the headset software has focus)' -Type dll

$cl = @(
    '/nologo', '/O2', '/EHsc', '/W3', '/MT', '/LD',
    '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN',
    "/Fo:$Obj\", "/Fe:$Out",
    "`"$Src\dinput_proxy.cpp`"",
    "`"$($ver.Res)`"",
    '/link', "/DEF:`"$Src\dinput_proxy.def`"",
    'user32.lib', 'ole32.lib'
) -join ' '

Write-Host 'Compiling proxy DINPUT.dll (x86)...' -ForegroundColor Cyan

# DELETE THE OLD ONE FIRST, and this is not tidiness.
#
# The only success test used to be Test-Path $Out, which a DLL left by an
# EARLIER build satisfies. So a compile that failed outright still printed
# "Proxy build complete" and staged a stale binary. It did exactly that on
# 19 September over an error C2065, and the only reason it was caught is that
# the byte count had not changed. A build script that reports success on
# failure is worse than no build script.
if (Test-Path $Out) { Remove-Item $Out -Force }

$output = cmd /c "`"$vcvars`" x86 >nul 2>&1 && $($ver.Cmd) && cl $cl 2>&1"
$clExit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }

# The compiler's own verdict, not an inference from the file system.
if ($clExit -ne 0) {
    $output | ForEach-Object { Write-Host $_ }
    throw "proxy build failed (cl exit $clExit)."
}
if (-not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'proxy build failed: cl reported success but produced no DLL.'
}

# Refuse to ship a 64-bit build by accident: it would not load, and the game
# would quietly use the system DINPUT while the log said the proxy was in force.
$fs = [IO.File]::OpenRead($Out)
$br = New-Object IO.BinaryReader($fs)
$fs.Position = 0x3c
$pe = $br.ReadInt32()
$fs.Position = $pe + 4
$machine = $br.ReadUInt16()
$br.Close(); $fs.Close()
if ($machine -ne 0x14c) {
    throw ("built DINPUT.dll is machine 0x{0:X} - must be 0x14C (x86) to load into lithtech.exe" -f $machine)
}
Write-Host ("  machine 0x{0:X} (x86) - correct" -f $machine) -ForegroundColor DarkGray

Copy-Item $Out $Game -Force
"{0}  ->  {1:N0} bytes, staged in game\" -f 'DINPUT.dll', (Get-Item $Out).Length | Write-Host
Write-Host 'Proxy build complete. Default mode is FILTER:' -ForegroundColor Green
Write-Host '  the DirectInput joystick sweep is skipped; keyboard and mouse are not.' -ForegroundColor Green
Write-Host 'NOLFVR_DINPUT=pass  forwards everything unchanged (the control).' -ForegroundColor DarkGray
Write-Host 'Delete game\DINPUT.dll to remove the proxy entirely.' -ForegroundColor DarkGray
