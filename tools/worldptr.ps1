# Does the engine hand our renderer a scene description with a world in it?
#
# Word 13 of the scene description (offset 52) is the world pointer - read out
# of the real renderer's own code, which loads [scene + 0x34] and walks it
# (docs/RENDERSCENE-FIRST-LOOK.md). The engine does not always fill it in:
#
#   every slot delegated to the real d3d.ren  ->  2494 pointers, 0 small
#   every slot ours                           ->  0 pointers, 3399 small (== 1)
#
# So something the renderer does decides whether the engine populates it, and
# that is a bisection, not a theory. This runs ONE arm and prints one line.
#
#   .\tools\worldptr.ps1 -Lo 1 -Hi 36        # everything real: the control
#   .\tools\worldptr.ps1                     # everything ours: the other control
#   .\tools\worldptr.ps1 -Lo 1 -Hi 18
#
# Slot 17 always stays OURS (+StubTrace17 1) and calls the real one, because it
# is the probe: it is where the scene description is read. Delegating it would
# remove the instrument.
#
# The verdict is a count, not an impression. There is nothing to judge: the two
# arms differ by thousands, not by a few.

param(
    [int]$Lo = -1,
    [int]$Hi = -1,
    [int]$Seconds = 26
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

$set = @('RenderDll=d3dstub.ren', 'ScreenWidth=800', 'ScreenHeight=600',
         'VRAutoQuickLoad=1', 'musicenable=0', 'StubTrace17=1', 'StubDevice=0')
if ($Lo -ge 0) { $set += @("StubDelegateLo=$Lo", "StubDelegateHi=$Hi") }

& (Join-Path $PSScriptRoot 'probe-run.ps1') -Seconds $Seconds -FakeHost -Set $set | Out-Null

$log = Join-Path $Root 'game\logs\renstub.log'
if (-not (Test-Path $log)) { "NO STUB LOG - the run did not start"; exit 1 }

# Prove what actually ran rather than repeating what was asked for.
$sw = (Select-String -Path $log -Pattern '^switches:' | Select-Object -First 1).Line

$last = Select-String -Path $log -Pattern 'scene word 13: (\d+) zero, (\d+) small, (\d+) a pointer' |
        Select-Object -Last 1
if (-not $last) { "NO WORLD-POINTER LINE - the stub never saw a RenderScene"; exit 1 }
$zero, $small, $ptr = [int]$last.Matches[0].Groups[1].Value,
                      [int]$last.Matches[0].Groups[2].Value,
                      [int]$last.Matches[0].Groups[3].Value

$verdict = if ($ptr -gt 100) { 'WORLD' } elseif ($ptr -gt 0) { 'PARTIAL' } else { 'NO-WORLD' }
"{0,-9} delegated={1,-8} word13: {2} pointers, {3} small, {4} zero" -f `
    $verdict, $(if ($Lo -ge 0) { "$Lo..$Hi" } else { 'none' }), $ptr, $small, $zero
"          ran: $sw"
