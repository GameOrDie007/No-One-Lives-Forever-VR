# Builds the x64 OpenXR tracking probe.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Src  = Join-Path $Root 'host\xrprobe.cpp'
$Out  = Join-Path $Root 'host\xrprobe.exe'
$Obj  = Join-Path $Root 'build\xrprobe'
$Xr   = Join-Path $Root 'libs\openxr'

New-Item -ItemType Directory -Force $Obj | Out-Null

if (-not (Test-Path (Join-Path $Xr 'include\openxr\openxr.h'))) {
    throw "OpenXR SDK not found at $Xr"
}

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw 'vcvars64.bat not found.' }

$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MD',
    "/I`"$Xr\include`"",
    "/I`"$Root\src\nolf1-modernizer\NOLF\ClientShellDLL`"",
    "/Fo:$Obj\", "/Fe:$Out",
    "`"$Src`"",
    '/link', "`"$Xr\native\x64\release\lib\openxr_loader.lib`"", 'user32.lib'
) -join ' '

Write-Host 'Compiling xrprobe (x64)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && cl $cl 2>&1"
$output | ForEach-Object { if ($_ -match 'error|warning C') { Write-Host $_ } }

if (-not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw 'xrprobe build failed.'
}

# The loader DLL must sit beside the exe.
Copy-Item (Join-Path $Xr 'native\x64\release\bin\openxr_loader.dll') (Split-Path $Out) -Force

"{0}  ->  {1:N0} bytes" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length | Write-Host
Write-Host 'xrprobe build complete.' -ForegroundColor Green
