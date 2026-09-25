# Builds the x64 VR host: OpenXR session + window capture + eye submission.

$ErrorActionPreference = 'Stop'

$Root = Split-Path $PSScriptRoot -Parent
$Out  = Join-Path $Root 'host\nolfvr.exe'
$Obj  = Join-Path $Root 'build\vrhost'
$Xr   = Join-Path $Root 'libs\openxr'
$Shell = Join-Path $Root 'src\nolf1-modernizer\NOLF\ClientShellDLL'

New-Item -ItemType Directory -Force $Obj | Out-Null

# Remove the previous binary FIRST. Without this the success check
# below passes on last build's exe: a compile that failed with hard
# errors still printed "VR host build complete", and every test after
# it silently ran the old host.
if (Test-Path $Out) { Remove-Item $Out -Force }

if (-not (Test-Path (Join-Path $Xr 'include\openxr\openxr.h'))) { throw "OpenXR SDK not found at $Xr" }

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw 'vcvars64.bat not found.' }

$sources = @('vrmain.cpp', 'xrvr.cpp', 'capture.cpp', 'mirror.cpp', 'sharedframe.cpp') |
           ForEach-Object { "`"$(Join-Path $Root "host\$_")`"" }
# The version resource (see host\res\version.h), linked in with the sources.
. (Join-Path $PSScriptRoot 'version-res.ps1')
$ver = New-VersionResCommand -ObjDir $Obj -FileName 'nolfvr.exe' -Description 'NOLF1 VR - OpenXR host (headset, mirror)' -Type app
$sources += "`"$($ver.Res)`""

$cl = @(
    '/nologo', '/std:c++17', '/EHsc', '/O2', '/W3', '/MD',
    '/DUNICODE', '/D_UNICODE', '/DNOMINMAX',
    "/I`"$Xr\include`"",
    "/I`"$Shell`"",
    "/Fo:$Obj\", "/Fe:$Out",
    ($sources -join ' '),
    '/link', "`"$Xr\native\x64\release\lib\openxr_loader.lib`"",
    'windowsapp.lib', 'user32.lib', 'shcore.lib'
) -join ' '

Write-Host 'Compiling VR host (x64)...' -ForegroundColor Cyan
$output = cmd /c "`"$vcvars`" >nul 2>&1 && $($ver.Cmd) && cl $cl 2>&1"
$clExit = $LASTEXITCODE
$output | ForEach-Object { if ($_ -match 'error|fatal') { Write-Host $_ -ForegroundColor Red } }

if ($clExit -ne 0 -or -not (Test-Path $Out)) {
    $output | ForEach-Object { Write-Host $_ }
    throw "VR host build failed (cl exit $clExit)."
}

Copy-Item (Join-Path $Xr 'native\x64\release\bin\openxr_loader.dll') (Split-Path $Out) -Force

"{0}  ->  {1:N0} bytes" -f (Split-Path $Out -Leaf), (Get-Item $Out).Length | Write-Host
Write-Host 'VR host build complete.' -ForegroundColor Green
