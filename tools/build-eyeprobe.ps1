# Builds eyeprobe.exe - 64-BIT, deliberately.
#
# The point of the tool is that a 64-bit process can read a texture created by
# the 32-bit game, because that is what the host will have to do. Building it
# 32-bit would test nothing the renderer has not already proved to itself.
#
# Checks the exit code, not whether the output file exists: a failed build that
# leaves yesterday's exe in place has already cost this project a run
# (the development notes, "never pipe a build through tail").

$ErrorActionPreference = 'Stop'

$Root  = Split-Path $PSScriptRoot -Parent
$Src   = Join-Path $Root 'host\eyeprobe'
$Shell = Join-Path $Root 'src\nolf1-modernizer\NOLF\ClientShellDLL'
$Obj   = Join-Path $Root 'build\eyeprobe'
$Out   = Join-Path $Root 'host\eyeprobe.exe'

New-Item -ItemType Directory -Force $Obj | Out-Null
if (Test-Path $Out) { Remove-Item $Out -Force }

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw 'No 64-bit VC environment found.' }

$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MT',
    '/DWIN32', '/D_CONSOLE', '/DNOMINMAX',
    "/I`"$Shell`"",
    "/I`"$(Join-Path $Root 'host')`"",
    "/Fo:$Obj\",
    "`"$(Join-Path $Src 'eyeprobe.cpp')`"",
    "`"$(Join-Path $Root 'host\sharedframe.cpp')`"",
    '/link',
    "/OUT:`"$Out`"",
    'kernel32.lib', 'user32.lib', 'd3d11.lib', 'dxgi.lib'
) -join ' '

Write-Host 'Compiling eyeprobe (x64)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && cl $cl 2>&1"
$exit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }

if ($exit -ne 0) {
    $output | ForEach-Object { Write-Host $_ }
    throw "eyeprobe build failed (cl exit $exit)."
}
"{0}  ->  {1:N0} bytes" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length | Write-Host
Write-Host 'eyeprobe build complete.' -ForegroundColor Green
