# ---------------------------------------------------------------------------
# canopy-scan.ps1 - FIND A TEXTURE THAT FLASHES, which no screenshot can show.
#
# the canopy in the first
# part of the Morocco mission, on the roof of the sniper section, still had a
# flashing texture that looked like it was glitching out, and the tester did
# not think it would show in a single screenshot.
#
# It cannot: a still frame shows one of the two states and
# looks fine. What separates a flashing surface from a moving one is that its
# pixels ALTERNATE between two values rather than travelling through a range,
# and tools/flicker-scan.py measures exactly that over a run of frames.
#
# WHAT THE CANOPY IS. M01S01's texture table has a Translucent category whose
# top entries are TrFa0015, TrFa0016 and Fa0162 - translucent FABRIC. Our
# renderer draws NOLF's TranslucentWorldModels OPAQUE (see g_bSkipTranslucent in
# render3d.cpp), which is a known deviation and the leading suspect: an opaque
# surface sitting where a translucent one belongs is a z-fight waiting to
# happen, and a z-fight alternates.
#
# So this takes two arms. If the flashing is gone in the second, the translucent
# path owns it and the fix goes there rather than into the depth buffer.
#
#   .\tools\canopy-scan.ps1                  both arms
#   .\tools\canopy-scan.ps1 -At "x,y,z"      stand somewhere specific
# ---------------------------------------------------------------------------
#
# TWO THINGS THIS GOT WRONG FOR WEEKS, both of which produce a clean, confident,
# meaningless zero - see:
#
#   A FROZEN HEAD. look-shot passes --static to the fake host, so every capture
#   this script ever took was from a fixed orientation. A z-fight between two
#   coincident surfaces is STABLE when the camera does not move: the depth
# comparison comes out the same way every frame. It was visible in the headset
#   because the head was moving. It now sweeps, which makes the fight flip.
#
#   A CINEMATIC CAMERA. Every +runworld level sat in its opening camera, so the
#   captures were third-person CHASE. That is fixed in the client now
#   (VRDebugEndCinematic actually ends it), and look-shot ends it by default.
#
# THE ARMS. The fix under test hides world models the engine marked invisible
# and our renderer was drawing anyway - M01S01 has Canopy_a and Canopy_b with
# byte-identical bounding boxes (-592,256,-320)..(-432,320,0) and the same
# texture Fa0034.dtx, which is a z-fight by construction. +StubDrawHiddenWM 1
# restores the OLD behaviour, so that arm is the positive control: it must
# flicker. If neither arm flickers the instrument is not pointed at the canopy
# and no conclusion may be drawn from either.
#
#   .\tools\canopy-scan.ps1                  both arms
#   .\tools\canopy-scan.ps1 -At "x,y,z"      stand somewhere specific
param(
    [string]$World  = 'Worlds\M01S01',
    [string]$Look   = '0,0,0',
    # The midpoint of Canopy_a/Canopy_b, backed off along -z and a little below
    # so the pair fills a good part of the frame.
    [string]$At     = '-512,272,-620',
    [int]   $Frames = 40,
    [int]   $DumpAt = 1200,     # presents, ~13 s in: after the cinematic ends
    [int]   $Wait   = 34,
    [string]$Out    = 'logs\canopy'
)

$ErrorActionPreference = 'Continue'
$Root = Split-Path $PSScriptRoot -Parent
$OutAbs = Join-Path $Root $Out
New-Item -ItemType Directory -Force $OutAbs | Out-Null

foreach ($arm in @(
    @{ name = 'fixed';        set = "StubFrameDumpAt=$DumpAt;StubFrameDumpCount=$Frames"
       expect = 'the hidden-brush fix IS in force - this is the arm that should be CLEAN' },
    @{ name = 'hidden-drawn'; set = "StubFrameDumpAt=$DumpAt;StubFrameDumpCount=$Frames;StubDrawHiddenWM=1"
       expect = 'the old behaviour restored - THE POSITIVE CONTROL, this one must flicker' }))
{
    Write-Host ("=== {0} ===" -f $arm.name) -ForegroundColor Magenta
    # The dumps land in game\logs as frame-<present>.bmp; clear them first so a
    # previous arm's frames cannot be scanned as this one's.
    Get-ChildItem (Join-Path $Root 'game\logs') -Filter 'frame-*.bmp' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    Write-Host ("    {0}" -f $arm.expect) -ForegroundColor DarkGray
    # A HASHTABLE SPLAT, not an array. -Sweep is a [switch], and a bare
    # '-Sweep' inside an array splat shifts every argument after it by one -
    # the first run of this failed with 'Cannot convert value "-Wait" to type
    # System.Int32', which reads like a bad value and is really a bad shape.
    # A hashtable binds by name and cannot slip.
    #
    # -Sweep rather than -Look: the head must MOVE or a z-fight holds still.
    $a = @{ Sweep = $true; World = $World; Wait = $Wait
            Set = $arm.set; Out = "$Out\$($arm.name).png" }
    if ($At) { $a['At'] = $At }
    & (Join-Path $PSScriptRoot 'look-shot.ps1') @a | Out-Host

    $dst = Join-Path $OutAbs $arm.name
    New-Item -ItemType Directory -Force $dst | Out-Null
    $got = Get-ChildItem (Join-Path $Root 'game\logs') -Filter 'frame-*.bmp' -ErrorAction SilentlyContinue
    Write-Host ("   {0} frames dumped" -f $got.Count) -ForegroundColor DarkGray
    $got | Move-Item -Destination $dst -Force -ErrorAction SilentlyContinue

    if ($got.Count -ge 6) {
        & python (Join-Path $PSScriptRoot 'flicker-scan.py') $dst `
            --out (Join-Path $OutAbs "$($arm.name)-flicker.png") | ForEach-Object { '   ' + $_ }
    } else {
        Write-Host '   not enough frames to scan - raise -Frames or lower -DumpAt' -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host 'HOW TO READ IT' -ForegroundColor Cyan
Write-Host '  hidden-drawn is the POSITIVE CONTROL and must show a clear alternating'
Write-Host '  fraction. If it does not, the camera is not looking at the canopy and'
Write-Host '  NEITHER arm means anything - move -At and run it again before believing'
Write-Host '  a clean result from the other arm.'
Write-Host '  With that control showing, a fixed arm near zero is the fix working.'
