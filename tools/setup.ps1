<#
    No One Lives Forever VR - setup. Run once, by double-clicking Setup.bat.

    Point it at what you have and it builds the VR game inside this folder:

      * an installed NOLF GOTY folder  -> copied into .\game (your original is never touched)
      * the two GOTY discs or .isos     -> a fresh copy is built from them
      * a .zip of an installed folder   -> expanded and used
      * nothing at all                  -> it goes looking on its own

    Drag any of those onto Setup.bat, or name one:
        Setup.bat -Install "D:\Games\No One Lives Forever"

    No game data ships with the VR port and nothing is downloaded. Everything
    comes from your own install or your own discs.
#>

[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Install,                 # an installed game folder (or a .zip of one)
    [string]$Cd1,                     # disc 1 (Data\NOLF2.REZ + the patches)
    [string]$Cd2,                     # disc 2 (Data\NOLF.REZ + Game\)
    [switch]$NoFontSeed,              # skip the one retail launch that builds FontData.fnt
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Dropped                # whatever was dragged onto Setup.bat
)

$ErrorActionPreference = "Stop"

$here = Split-Path -Parent $MyInvocation.MyCommand.Path   # ...\tools
$pkg  = Split-Path -Parent $here                          # the folder Setup.bat lives in
$game = [System.IO.Path]::Combine($pkg, "game")

# setup.log beside Setup.bat, written LINE BY LINE from the start, so a run
# that stops half way still says where. A tester's folder with no log at all
# is a failure nobody can read.
$logFile = [System.IO.Path]::Combine($pkg, "setup.log")
try { Set-Content -LiteralPath $logFile -Value ("No One Lives Forever VR setup, " + (Get-Date -Format "yyyy-MM-dd HH:mm:ss")) -Encoding ASCII } catch { }
function LogLine($t) { try { Add-Content -LiteralPath $logFile -Value $t -Encoding ASCII } catch { } }
function Line($t) { Write-Host $t; LogLine $t }
function Ok($t)   { Write-Host "  [ok]   $t" -ForegroundColor Green; LogLine "  [ok]   $t" }
function Warn($t) { Write-Host "  [ ! ]  $t" -ForegroundColor Yellow; LogLine "  [ ! ]  $t" }
function Info($t) { Write-Host "  [--]   $t" -ForegroundColor DarkGray; LogLine "  [--]   $t" }
function Bad($t)  { Write-Host "  [XX]   $t" -ForegroundColor Red; LogLine "  [XX]   $t" }
trap { LogLine ("  [XX]   stopped: " + $_.Exception.Message); Write-Host ("  [XX]   stopped: " + $_.Exception.Message) -ForegroundColor Red; exit 1 }

$mountedIsos = New-Object System.Collections.Generic.List[string]
function Unmount-All {
    foreach ($iso in $mountedIsos) {
        try { Dismount-DiskImage -ImagePath $iso -ErrorAction SilentlyContinue | Out-Null } catch { }
    }
}
function Die($t) {
    Line ""
    Bad $t
    Line ""
    Unmount-All
    exit 1
}

Line ""
Line "No One Lives Forever VR setup"
Line "============================="
Line ""

# THE VR FILES MUST ALREADY BE HERE. They ship in the package; setup only adds
# the game around them. A partial extract is the usual cause of a missing one.
foreach ($rel in @("game\Modernizer.rez", "game\d3dstub.ren", "game\DINPUT.dll", "game\SDL2.dll",
                   "host\nolfvr.exe", "host\openxr_loader.dll", "tools\play-vr.ps1", "tools\run.ps1")) {
    if (-not (Test-Path -LiteralPath ([System.IO.Path]::Combine($pkg, $rel)))) {
        Die "$rel is missing - the download is incomplete. Extract the whole archive again."
    }
}

# THIS FOLDER HAS TO BE WRITABLE. The game is copied into it, and inside
# Program Files (or a read-only or synced folder) that fails part-way with an
# access error nobody can act on. Say what to do instead, before copying.
try {
    $probe = [System.IO.Path]::Combine($pkg, 'game', '.setup-write-test')
    [System.IO.File]::WriteAllText($probe, 'ok')
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
} catch {
    Die ("Setup cannot write to this folder:`n         $pkg`n" +
         "         Move the whole NOLF VR folder somewhere you own - your Desktop, or a`n" +
         "         games folder - not inside Program Files, and run Setup.bat again.")
}

# ------------------------------------------------------------------ identities
# Each source is recognised by a file only it has, at the size only it has.
$ID_NOLF_REZ  = 618254258      # Data\NOLF.REZ   - disc 2
$ID_NOLF2_REZ = 300703727      # Data\NOLF2.REZ  - disc 1

function Size-Of($path) {
    if (-not (Test-Path -LiteralPath $path)) { return -1 }
    return (Get-Item -LiteralPath $path).Length
}
function Has-File($root, $rel, $expectSize) {
    $s = Size-Of ([System.IO.Path]::Combine($root, $rel))
    if ($s -lt 0) { return $false }
    if ($expectSize -gt 0 -and $s -ne $expectSize) { return $false }
    return $true
}

# Turn whatever we were handed into a readable folder.
function Resolve-Source($path) {
    if ([string]::IsNullOrWhiteSpace($path)) { return $null }
    $path = $path.Trim('"').TrimEnd('\')
    if (-not (Test-Path -LiteralPath $path)) { Warn "not found: $path"; return $null }
    $item = Get-Item -LiteralPath $path
    if ($item.PSIsContainer) { return $item.FullName }
    switch ($item.Extension.ToLower()) {
        ".iso" {
            Info "mounting $($item.Name)"
            try {
                $img = Mount-DiskImage -ImagePath $item.FullName -PassThru -ErrorAction Stop
                $mountedIsos.Add($item.FullName)
                Start-Sleep -Milliseconds 400
                $vol = $img | Get-Volume
                if (-not $vol.DriveLetter) { Warn "mounted but got no drive letter: $($item.Name)"; return $null }
                return ($vol.DriveLetter + ":\")
            } catch {
                Warn "could not mount $($item.Name): $($_.Exception.Message)"
                return $null
            }
        }
        ".zip" {
            $tmp = [System.IO.Path]::Combine($env:TEMP, "nolfvr_" + [Guid]::NewGuid().ToString("N").Substring(0,8))
            [System.IO.Directory]::CreateDirectory($tmp) | Out-Null
            Info "expanding $($item.Name)"
            Expand-Archive -LiteralPath $item.FullName -DestinationPath $tmp -Force
            # A zip of a folder usually holds the folder itself one level down.
            $inner = Get-ChildItem -LiteralPath $tmp -Directory
            if (-not (Test-Path -LiteralPath ([System.IO.Path]::Combine($tmp, "lithtech.exe"))) -and $inner.Count -eq 1) {
                return $inner[0].FullName
            }
            return $tmp
        }
    }
    Warn "don't know what to do with $($item.Name)"
    return $null
}

# What is this folder? The 590 MB NOLF.REZ marks a real install; the disc
# layouts are told apart by where their archives sit.
function Classify($root) {
    if (-not $root) { return $null }
    if ([string]::Equals((Resolve-Path -LiteralPath $root).ProviderPath.TrimEnd('\'), $game.TrimEnd('\'), 'OrdinalIgnoreCase')) { return $null }
    if (Test-Path -LiteralPath ([System.IO.Path]::Combine($root, "NOLF.rez"))) { return "Install" }
    if ((Has-File $root "Data\NOLF.REZ" $ID_NOLF_REZ) -or
        (Test-Path -LiteralPath ([System.IO.Path]::Combine($root, "Game\lithtech.exe")))) { return "Disc2" }
    if (Has-File $root "Data\NOLF2.REZ" $ID_NOLF2_REZ) { return "Disc1" }
    if (Test-Path -LiteralPath ([System.IO.Path]::Combine($root, "nolf_goty_cd1.iso"))) { return "IsoFolder" }
    return $null
}

$sources = @{}
function Offer($path) {
    $r = Resolve-Source $path
    if (-not $r) { return }
    $kind = Classify $r
    if (-not $kind) { Warn "not a NOLF install or disc: $path"; return }
    if ($kind -eq "IsoFolder") {
        foreach ($iso in (Get-ChildItem -LiteralPath $r -Filter "*.iso" -File)) { Offer $iso.FullName }
        return
    }
    if (-not $sources.ContainsKey($kind)) {
        $sources[$kind] = $r
        Ok "$kind  <-  $path"
    }
}

try {

# ------------------------------------------------------- 1. what were we given?
foreach ($p in @($Install, $Cd1, $Cd2)) { if ($p) { Offer $p } }
foreach ($p in $Dropped) { if ($p -and -not $p.StartsWith("-")) { Offer $p } }

# AN EARLIER RUN ALREADY BUILT IT. Running setup again with nothing named just
# re-checks the game folder and redoes the steps after the copy.
$already = (Has-File $game "NOLF.rez" 0) -and (Has-File $game "lithtech.exe" 0)

if ($sources.Count -eq 0 -and -not $already) {
    Line "Nothing named, so looking around..."
    $guesses = New-Object System.Collections.Generic.List[string]
    $guesses.Add((Split-Path -Parent $pkg))
    foreach ($g in @("$env:ProgramFiles\GOG Galaxy\Games\No One Lives Forever",
                     "${env:ProgramFiles(x86)}\GOG Galaxy\Games\No One Lives Forever",
                     "${env:ProgramFiles(x86)}\Fox\No One Lives Forever",
                     "$env:ProgramFiles\Fox\No One Lives Forever")) { $guesses.Add($g) }
    foreach ($d in (Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Used -ne $null })) {
        foreach ($sub in @("Games\No One Lives Forever", "GOG Games\No One Lives Forever",
                           "Program Files (x86)\Fox\No One Lives Forever",
                           "Downloadable Games\No One Lives Forever", "NOLF", "Games\NOLF")) {
            $guesses.Add([System.IO.Path]::Combine($d.Root, $sub))
        }
    }
    foreach ($g in $guesses) {
        if ($g -and (Test-Path -LiteralPath ([System.IO.Path]::Combine($g, "lithtech.exe"))) -and
            (Test-Path -LiteralPath ([System.IO.Path]::Combine($g, "NOLF.rez")))) { Offer $g; break }
    }
}

if ($sources.Count -eq 0 -and -not $already) {
    Line ""
    Bad "Could not find No One Lives Forever."
    Line ""
    Line "  Drag your installed game folder onto Setup.bat,"
    Line "  or drag your two NOLF GOTY discs (the .iso files) onto it together,"
    Line "  or name it directly:"
    Line ""
    Line '      Setup.bat -Install "D:\Games\No One Lives Forever"'
    Line ""
    exit 1
}
Line ""

# ------------------------------------------------------- 2. the game files, into .\game
[System.IO.Directory]::CreateDirectory($game) | Out-Null

# NEVER OVERWRITE THE VR FILES, AND BRING NOTHING THAT WOULD FIGHT THEM. A flat
# install may carry the Modernizer (Custom\MODERNIZER.REZ - and the launcher
# mounts the custom folder), another dinput.dll, a widescreen patch with its own
# client DLL, or a flat launcher. None of that belongs in the VR folder.
$ourFiles = @("Modernizer.rez", "d3dstub.ren", "DINPUT.dll", "SDL2.dll", "vrtune.cfg", "vrweapons.cfg")
$skipFiles = $ourFiles + @("ditest.exe", "Play NOLF.bat", "pick-resolution.ps1", "READ ME FIRST.txt",
                           "nolf-stabilizer.log", "WidescreenGOTY.rez")
$skipDirs  = @("Custom", "_original launcher (not used)", "logs")

if ($sources.ContainsKey("Install")) {
    $src = $sources["Install"]
    Line "Copying your game"
    Line "-----------------"
    Info "from $src"
    Info "into $game  (this takes a minute; your original is not changed)"
    $rcArgs = @($src, $game, "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NP", "/XF") + $skipFiles + @("/XD") +
              ($skipDirs | ForEach-Object { [System.IO.Path]::Combine($src, $_) })
    $null = & robocopy @rcArgs
    if ($LASTEXITCODE -ge 8) { Die "copying failed (robocopy $LASTEXITCODE)." }
    Ok "game files copied"
} elseif ($sources.ContainsKey("Disc1") -or $sources.ContainsKey("Disc2")) {
    if (-not ($sources.ContainsKey("Disc1") -and $sources.ContainsKey("Disc2"))) {
        Die ("Need both discs. Got only " + (($sources.Keys | Where-Object { $_ -like "Disc*" }) -join ", ") +
             ". Drag both .iso files onto Setup.bat at once.")
    }
    Line "Building the game from your discs"
    Line "---------------------------------"
    function CopyInto($srcDir, $dstDir, $filter) {
        if (-not (Test-Path -LiteralPath $srcDir)) { return 0 }
        [System.IO.Directory]::CreateDirectory($dstDir) | Out-Null
        $n = 0
        foreach ($f in (Get-ChildItem -LiteralPath $srcDir -File -Filter $filter)) {
            if ($ourFiles -contains $f.Name) { continue }
            Copy-Item -LiteralPath $f.FullName -Destination ([System.IO.Path]::Combine($dstDir, $f.Name)) -Force
            $n++
        }
        return $n
    }
    $d2 = $sources["Disc2"]; $d1 = $sources["Disc1"]
    $copied = 0
    $copied += CopyInto ([System.IO.Path]::Combine($d2, "Game"))   $game "*"
    $copied += CopyInto ([System.IO.Path]::Combine($d2, "Movies")) ([System.IO.Path]::Combine($game, "Movies")) "*"
    $copied += CopyInto ([System.IO.Path]::Combine($d2, "Data"))   $game "*.rez"
    $copied += CopyInto ([System.IO.Path]::Combine($d1, "Data"))   $game "*.rez"
    if ($copied -eq 0) { Die "Copied nothing from the discs. Are they really the NOLF GOTY discs?" }
    Ok "$copied files copied from the discs"
    if (-not (Has-File $game "NOLF.REZ" $ID_NOLF_REZ))  { Die "NOLF.REZ did not arrive, or is the wrong size." }
    if (-not (Has-File $game "NOLF2.REZ" $ID_NOLF2_REZ)) { Die "NOLF2.REZ did not arrive, or is the wrong size." }
    Ok "disc data verified by size"
} else {
    Info "the game is already in $game - re-checking it"
}
Unmount-All

# Files copied off a CD or a mounted .iso keep the read-only attribute, and the
# game then cannot write autoexec.cfg, its font cache or your saves.
$ro = 0
foreach ($f in (Get-ChildItem -LiteralPath $game -Recurse -File)) {
    if ($f.IsReadOnly) { $f.IsReadOnly = $false; $ro++ }
}
if ($ro -gt 0) { Ok "cleared the read-only attribute on $ro files" }

# The launcher mounts .\game\custom; it must exist and be empty of flat mods.
[System.IO.Directory]::CreateDirectory([System.IO.Path]::Combine($game, "custom")) | Out-Null

# ------------------------------------------------------- 3. the edition
# The VR client is built on NOLF Modernizer, which needs the Game of the Year
# edition (v1.004): NOLFGOTY.REZ, and the 1.003 update's nolfu003.rez.
if (-not (Has-File $game "NOLFGOTY.REZ" 0) -or -not (Has-File $game "nolfu003.rez" 0)) {
    Die ("This is not the Game of the Year edition (v1.004): NOLFGOTY.REZ or nolfu003.rez is missing. " +
         "The VR port needs the GOTY edition.")
}
if (-not (Has-File $game "lithtech.exe" 0)) { Die "lithtech.exe is missing from the game folder." }
Ok "Game of the Year edition found"

# ------------------------------------------------------- 4. the font cache
# FontData.fnt is a font-metrics cache the game writes beside itself: a float
# version, a uint32 record count, then 1012 bytes per font. The Modernizer-based
# client cannot build it from nothing - it stops at 8 fonts and fails with
# "Could not initialize InterfaceResMgr" - but the RETAIL game builds all 9. So
# the retail game is run once, windowed, on the retail archives only. The cache
# is generated on this machine from your own game data.
function Get-FontRecordCount($path) {
    if (-not (Test-Path -LiteralPath $path)) { return 0 }
    $len = (Get-Item -LiteralPath $path).Length
    if ($len -lt 1020) { return 0 }
    return [int](($len - 8) / 1012)
}
$fd = [System.IO.Path]::Combine($game, "FontData.fnt")
$have = Get-FontRecordCount $fd
if ($have -ge 9) {
    Ok "font cache already has $have records"
} elseif ($NoFontSeed) {
    Warn "font cache has $have records and -NoFontSeed was given - the game may fail to start"
} else {
    if ($have -gt 0) { Warn "font cache has only $have records - rebuilding it"; Remove-Item -LiteralPath $fd -Force }
    $rez = New-Object System.Collections.Generic.List[string]
    foreach ($r in @("NOLF.rez", "NOLF2.rez", "nolfu003.rez", "nolfcres003.rez", "NolfGoty.rez")) {
        if (Has-File $game $r 0) { $rez.Add("-rez $r") }
    }
    Info "running the original game once to build its font cache (a window will flash up)"
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName         = [System.IO.Path]::Combine($game, "lithtech.exe")
    $psi.Arguments        = '-windowtitle "NOLF - building font cache" ' + ($rez -join " ") + ' +multiplayer 0 +windowed 1'
    $psi.WorkingDirectory = $game
    $psi.UseShellExecute  = $false
    $proc = [System.Diagnostics.Process]::Start($psi)
    for ($i = 0; $i -lt 45; $i++) {
        Start-Sleep -Seconds 1
        if ((Get-FontRecordCount $fd) -ge 9) { break }
        if ($proc.HasExited) { break }
    }
    if (-not $proc.HasExited) {
        try { $proc.CloseMainWindow() | Out-Null; $proc.WaitForExit(4000) | Out-Null } catch { }
        if (-not $proc.HasExited) { try { $proc.Kill() } catch { } }
    }
    Start-Sleep -Milliseconds 800
    $got = Get-FontRecordCount $fd
    if ($got -ge 9) { Ok "font cache built - $got records" }
    else { Warn "font cache came out with $got records; the game may fail to start" }
}

# ------------------------------------------------------- 5. settings
# The game reads an absent console variable as ZERO, and an install whose
# Options pages were never visited has none - gore, water animation and the
# sky all off. Retail's own values are written where nothing is set.
$repair = [System.IO.Path]::Combine($here, "repair-settings.ps1")
$cfg    = [System.IO.Path]::Combine($game, "autoexec.cfg")
if ((Test-Path -LiteralPath $repair) -and (Test-Path -LiteralPath $cfg)) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $repair -Config $cfg | Out-Null
    Ok "game settings checked"
}

Line ""
Ok "Done."
Line ""
Line "  To play: put your headset on, start Virtual Desktop / SteamVR / Oculus Link,"
Line "  then double-click  Play NOLF VR.bat"
Line ""
exit 0

} finally {
    Unmount-All
}
