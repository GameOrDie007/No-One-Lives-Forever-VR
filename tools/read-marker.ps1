# Reads the client's frame marker off the screen and decodes it exactly the way
# the host does - but from the desktop, so it can be checked with no headset.
#
# The marker is twelve 8x8 swatches along the very top-left of the game's render
# surface: eight data bits, least significant first, carrying the low byte of
# the host frame counter the image was drawn from, then a fixed 1 0 1 0 sync.
#
# The host uses the byte to find the pose an image was rendered from. Before the
# sync existed it could not tell a marker from a wall, so any patch of picture
# decoded as a valid frame number, matched a pose at random out of a 1.4-second
# ring about half the time, and the compositor was handed that pose as the truth.
# The host's own logs carry it: blocks reading "avg 700 ms, worst 1558 ms,
# unmatched 244" beside a timestamp instrument in the same block saying 8 ms.
#
# So this answers one narrow question exactly: are the swatches there, do they
# carry an incrementing number, and does the sync hold.
#
# The game runs borderless at (0,0), so screen (0,0) is client (0,0). If that
# stops being true this reads the wrong pixels - which is precisely the failure
# it is looking for, so the raw luminances are printed either way.

param([int]$Samples = 12, [int]$IntervalMs = 250, [int]$X = 0, [int]$Y = 0)

Add-Type -AssemblyName System.Drawing

$block = 8
$bits  = 8
$sync  = 4
$all   = $bits + $sync
$w = $block * $all

$bmp = New-Object System.Drawing.Bitmap($w, $block)
$g   = [System.Drawing.Graphics]::FromImage($bmp)

$good = 0
$prev = -1
$deltas = @()

for ($s = 0; $s -lt $Samples; $s++) {
    $g.CopyFromScreen($X, $Y, 0, 0, (New-Object System.Drawing.Size($w, $block)))

    $lum = @()
    for ($b = 0; $b -lt $all; $b++) {
        $px = $bmp.GetPixel($b * $block + [int]($block / 2), [int]($block / 2))
        $lum += [int](($px.R + $px.G + $px.B) / 3)
    }

    $lo = ($lum | Measure-Object -Minimum).Minimum
    $hi = ($lum | Measure-Object -Maximum).Maximum
    $mid = [int](($lo + $hi) / 2)
    $band = [int](($hi - $lo) / 4)

    $why = ''
    if (($hi - $lo) -lt 48) { $why = 'no contrast' }

    if (-not $why) {
        for ($k = 0; $k -lt $sync; $k++) {
            $on = $lum[$bits + $k] -gt $mid
            if ($on -ne (($k % 2) -eq 0)) { $why = "sync block $k wrong"; break }
        }
    }
    if (-not $why) {
        for ($b = 0; $b -lt $all; $b++) {
            if ($lum[$b] -gt ($lo + $band) -and $lum[$b] -lt ($hi - $band)) {
                $why = "block $b sits between levels"; break
            }
        }
    }

    $value = -1
    if (-not $why) {
        $value = 0
        for ($b = 0; $b -lt $bits; $b++) {
            if ($lum[$b] -gt $mid) { $value = $value -bor (1 -shl $b) }
        }
        $good++
        if ($prev -ge 0) { $deltas += ((($value - $prev) + 256) % 256) }
        $prev = $value
    }

    "{0,3}  [{1}]  lo {2,3} hi {3,4}  -> {4}" -f `
        $s, ($lum -join ' '), $lo, $hi,
        $(if ($why) { "REJECTED ($why)" } else { "byte $value" })

    Start-Sleep -Milliseconds $IntervalMs
}

$g.Dispose()
$bmp.Dispose()

""
"decoded {0} of {1}" -f $good, $Samples
if ($deltas.Count) {
    $avg = ($deltas | Measure-Object -Average).Average
    "counter steps between samples: {0}  (avg {1:N1} - at {2} ms apart that is {3:N0} host fps)" -f `
        ($deltas -join ' '), $avg, $IntervalMs, ($avg * 1000.0 / $IntervalMs)
}
