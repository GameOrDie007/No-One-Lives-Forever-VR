# Builds the NOLF client from source and deploys it into the staged game folder.
# Never touches the retail install at <your NOLF install>.

$ErrorActionPreference = 'Stop'

$Root    = Split-Path $PSScriptRoot -Parent
$Src     = Join-Path $Root 'src\nolf1-modernizer'
$Game    = Join-Path $Root 'game'
$Stage   = Join-Path $Root 'build\rez-stage'
$Config  = 'Final Release'

# --- locate the v142 toolchain -------------------------------------------------
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -version '[16.0,17.0)' -products * -requires Microsoft.Component.MSBuild `
                      -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'VS2019 (v142) MSBuild not found. Install Build Tools 2019.' }

# --- compile -------------------------------------------------------------------
# VRLog.cpp bakes __DATE__/__TIME__ into the log header. Without forcing a
# recompile it reports the last time VRLog.cpp itself changed, which made a log
# from a fresh build look stale. Touch it so the header is always truthful.
$vrlog = Join-Path $Src 'NOLF\ClientShellDLL\VRLog.cpp'
if (Test-Path $vrlog) { (Get-Item $vrlog).LastWriteTime = Get-Date }

Write-Host '[1/4] Compiling NOLF.sln...' -ForegroundColor Cyan
& $msbuild (Join-Path $Src 'NOLF\NOLF.sln') /t:Build `
    /p:Configuration=$Config /p:Platform=x86 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)." }

# --- stage rez contents: three DLLs flattened alongside the asset tree ---------
Write-Host '[2/4] Staging rez contents...' -ForegroundColor Cyan
if (Test-Path $Stage) { Remove-Item $Stage -Recurse -Force }
New-Item -ItemType Directory -Force $Stage | Out-Null

$artifacts = @(
    'NOLF\ClientRes\CRes.dll'
    'NOLF\ClientShellDLL\CShell.dll'
    'NOLF\ObjectDLL\Object.lto'
)
foreach ($a in $artifacts) {
    $p = Join-Path $Src $a
    if (-not (Test-Path $p)) { throw "Missing build artifact: $a" }
    Copy-Item $p $Stage
}
Copy-Item (Join-Path $Src 'ASSETS\*') $Stage -Recurse

# --- pack ----------------------------------------------------------------------
Write-Host '[3/4] Packing Modernizer.rez...' -ForegroundColor Cyan
$rez = Join-Path $Game 'Modernizer.rez'
if (Test-Path $rez) { Remove-Item $rez -Force }
& (Join-Path $Src 'TOOLS\lithrez.exe') c $rez $Stage | Out-Null
if (-not (Test-Path $rez)) { throw 'lithrez produced no output.' }

# --- deploy loose binaries ------------------------------------------------------
Write-Host '[4/4] Deploying binaries...' -ForegroundColor Cyan
Copy-Item (Join-Path $Src 'BIN\*') $Game -Force
Copy-Item (Join-Path $Src 'LIBS\SDL2-2.0.10\lib\x86\SDL2.dll') $Game -Force

# The retail repack ships a widescreen-patched CShell.dll inside WidescreenGOTY.rez.
# It occupies the same slot as ours, so it must never be loaded. Park it.
$ws = Join-Path $Game 'WidescreenGOTY.rez'
if (Test-Path $ws) { Move-Item $ws "$ws.disabled" -Force }

"{0}  ->  {1:N0} bytes" -f 'Modernizer.rez', (Get-Item $rez).Length | Write-Host
Write-Host 'Build complete.' -ForegroundColor Green
