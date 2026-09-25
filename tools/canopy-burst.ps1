# ---------------------------------------------------------------------------
# canopy-burst.ps1 - GRAB A RUN OF FRAMES WHILE SOMEBODY IS PLAYING.
#
# A helper. You do not run this: tools\canopy-retail.ps1 and
# tools\canopy-ours.ps1 both call it.
#
# WHY A BURST AND NOT A SCREENSHOT. A z-fight between two coincident surfaces
# shows only as a DIFFERENCE BETWEEN FRAMES - every single frame looks correct
# on its own, which is what the tester reported: the flashing does not come
# through in a single screenshot. One picture cannot answer the question no
# matter how carefully it is taken. A run of them can, and tools/flicker-scan.py
# reads it.
#
# WHY NOT VIDEO. These are lossless PNGs straight off the window. h264 moves
# every pixel a little every frame, which is the same signal the test is looking
# for, and separating the two costs tolerance and therefore sensitivity. It also
# saves installing and driving a recorder.
#
# The window is NOT moved between frames (-NoMove): somebody is playing, and
# yanking the window to 0,0 mid-shot would be both rude and a different picture.
# ---------------------------------------------------------------------------
param(
    [Parameter(Mandatory = $true)][string]$Out,     # folder to write into
    [string]$Label   = 'frame',
    [int]   $Count   = 24,        # how many frames in one burst
    [double]$EverySec = 0.35,     # gap between them
    [int]   $Width   = 0,         # the size the game was ASKED for (DPI fix)
    [int]   $Height  = 0
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force $Out | Out-Null

$shot = Join-Path $PSScriptRoot 'window-shot.ps1'
if (-not (Test-Path $shot)) { throw "window-shot.ps1 is missing - cannot capture." }

$got = 0
for ($i = 1; $i -le $Count; $i++) {
    if (-not (Get-Process lithtech -ErrorAction SilentlyContinue)) {
        Write-Host '  the game closed - stopping the burst' -ForegroundColor Yellow
        break
    }
    $path = Join-Path $Out ("{0}-{1:d3}.png" -f $Label, $i)
    $a = @{ Out = $path; NoMove = $true }
    if ($Width -gt 0)  { $a['Width']  = $Width }
    if ($Height -gt 0) { $a['Height'] = $Height }
    try {
        & $shot @a | Out-Null
        if (Test-Path $path) { $got++ }
    } catch {
        Write-Host ("  frame {0} failed: {1}" -f $i, $_.Exception.Message) -ForegroundColor Yellow
    }
    Start-Sleep -Milliseconds ([int]($EverySec * 1000))
}

Write-Host ("  {0} of {1} frames captured into {2}" -f $got, $Count, $Out) -ForegroundColor Green
# A COUNT, RETURNED, so the caller can refuse to draw a conclusion from three
# frames. Every canopy result this project has got wrong was a confident number
# computed from an input nobody checked.
$got
