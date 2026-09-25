# Builds the x64 capture host. Separate from build.ps1 because this is a
# different architecture and toolchain path - the game client is x86.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $Root 'host\vrhost.cpp'
$Out  = Join-Path $Root 'host\vrhost.exe'
$Obj  = Join-Path $Root 'build\host'

New-Item -ItemType Directory -Force $Obj | Out-Null

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found - is VS2019 Build Tools installed?" }

# C++/WinRT needs C++17. windowsapp.lib carries the WinRT activation entry
# points used by Windows.Graphics.Capture.
$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MD',
    '/DUNICODE', '/D_UNICODE',
    "/Fo:$Obj\", "/Fe:$Out",
    "`"$Src`"",
    '/link', 'windowsapp.lib', 'd3d11.lib', 'dxgi.lib', 'user32.lib', 'dwmapi.lib'
) -join ' '

Write-Host 'Compiling host (x64)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && cl $cl 2>&1"
$output | ForEach-Object { if ($_ -match 'error|warning C') { Write-Host $_ } }

if (-not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'Host build failed.'
}

"{0}  ->  {1:N0} bytes" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length | Write-Host
Write-Host 'Host build complete.' -ForegroundColor Green
