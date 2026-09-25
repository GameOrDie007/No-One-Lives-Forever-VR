# ---------------------------------------------------------------------------
# sound-ab.ps1 - the sound-reset A/B, PAIRED, one mission at a time.
#
# WHY THIS SHAPE. The first comparison ran all ten missions of one arm, then
# all ten of the other, and the two halves of the night were not the same
# machine: the first arm lost window focus 45 times to my own progress polling
# and the second lost it none. A focus loss reloads the render DLL and is where
# DirectSound loses its buffers, so the arms differed by more than the switch
# under test and the 3-versus-0 result had to be withdrawn.
#
# Alternating the arms mission by mission does not remove that interference -
# nothing at this desk can - but it spreads it evenly over both arms, which is
# what makes the comparison survive it. Each mission contributes one run to
# each arm, minutes apart, under whatever conditions that minute had.
#
# Every run's renderer restarts are reported, so the reader can check that the
# spreading actually worked rather than take it on trust.
#
#   .\tools\sound-ab.ps1
#   .\tools\sound-ab.ps1 -Missions "M01;M02" -Repeats 2
# ---------------------------------------------------------------------------
param(
    [string]$Missions = 'M16;M01;M02;M03;M04;M05;M06;M07;M08;M09',
    [int]   $Repeats  = 1,
    [string]$OutRoot  = 'logs\ab'
)

$Root = Split-Path $PSScriptRoot -Parent
$list = @($Missions -split '[;, ]' | Where-Object { $_ })
$arms = @(
    @{ name = 'base'; set = 'VRSoundReset=0' },
    @{ name = 'fix';  set = 'VRSoundReset=1' })

$rows = @()
for ($rep = 1; $rep -le $Repeats; $rep++) {
    foreach ($mission in $list) {
        # THE ARM ORDER FLIPS EACH MISSION. Whatever happens to run first gets
        # the machine in whatever state the previous mission left it - a
        # warmer file cache, a driver that has just been reloaded - and fixing
        # the order would hand that advantage to one arm every time.
        $order = if ((($list.IndexOf($mission)) + $rep) % 2) { $arms } else { $arms[1], $arms[0] }
        foreach ($arm in $order) {
            $out = Join-Path $OutRoot ("{0}-r{1}" -f $arm.name, $rep)
            Write-Host ("--- {0}  arm {1}  repeat {2} ---" -f $mission, $arm.name, $rep) -ForegroundColor Magenta
            & (Join-Path $PSScriptRoot 'chain-levels.ps1') `
                -Missions $mission -OutDir $out -Set $arm.set | Out-Host
            $csv = Join-Path $Root (Join-Path $out 'summary.csv')
            if (Test-Path $csv) {
                $r = Import-Csv $csv | Select-Object -Last 1
                $rows += [pscustomobject]@{ mission = $mission; arm = $arm.name; rep = $rep
                                            loads = $r.loads; exits = $r.exits
                                            restarts = $r.restarts; crash = $r.crash
                                            counted = $r.counted; note = $r.note }
            }
        }
    }
}

$all = Join-Path $Root (Join-Path $OutRoot 'paired.csv')
New-Item -ItemType Directory -Force (Split-Path $all) | Out-Null
$rows | Export-Csv $all -NoTypeInformation
Write-Host ''
Write-Host 'mission  arm   loads exits restarts crash counted' -ForegroundColor Cyan
foreach ($r in $rows) {
    Write-Host ("{0,-8} {1,-5} {2,5} {3,5} {4,8} {5,5} {6,7}" -f
        $r.mission, $r.arm, $r.loads, $r.exits, $r.restarts, $r.crash, $r.counted)
}
foreach ($a in @('base','fix')) {
    $sel = @($rows | Where-Object { $_.arm -eq $a -and $_.counted -eq 'True' })
    $cr  = @($sel | Where-Object { $_.crash -eq 'True' }).Count
    $tr  = ($sel | Measure-Object -Property exits -Sum).Sum
    $re  = ($sel | Measure-Object -Property restarts -Sum).Sum
    Write-Host ("{0,-5}  {1} runs counted, {2} transitions, {3} renderer restarts, {4} crashes" -f
        $a, $sel.Count, $tr, $re, $cr) -ForegroundColor Green
}
