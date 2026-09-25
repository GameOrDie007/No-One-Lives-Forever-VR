# Builds tools/reload-probe.exe - load, free, load d3dstub.ren in one process,
# which is the sequence LithTech performs mid-session and which no desk game run
# reproduces. See tools/reload-probe.cpp.
#
# 32-bit, because the renderer is. Invocation copied from build-renstub.ps1:
# vcvars32 prints a vswhere warning that kills a naive `cmd /c` chain, so it is
# redirected the same way that build does it.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $PSScriptRoot 'reload-probe.cpp'
$Obj  = Join-Path $Root 'build\reloadprobe'
$Out  = Join-Path $Obj 'reload-probe.exe'

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
if (-not (Test-Path $vcvars)) { throw 'No 32-bit VC environment found.' }

New-Item -ItemType Directory -Force $Obj | Out-Null
if (Test-Path $Out) { Remove-Item $Out -Force }

$cl = @('/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MT',
        '/DWIN32', '/D_CONSOLE', "/Fo:$Obj\", "`"$Src`"",
        '/link', "/OUT:`"$Out`"", 'kernel32.lib', 'user32.lib') -join ' '

Write-Host 'Compiling the reload probe (x86)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && cl $cl 2>&1"
$clExit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }

if ($clExit -ne 0 -or -not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw "reload-probe build failed (cl exit $clExit)."
}

Write-Host "reload-probe.exe  ->  $Out" -ForegroundColor Green
