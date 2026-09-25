# ---------------------------------------------------------------------------
# version-res.ps1 - compile the version resource for one binary.
#
# Dot-source it from a build script, then call New-VersionRes; it returns the
# path of a .res to hand to cl alongside the sources (cl passes .res files to
# the linker). Needs rc.exe on PATH, which the vcvars environment provides, so
# it is run inside the same cmd /c as cl.
# ---------------------------------------------------------------------------
function New-VersionResCommand {
    param([string]$ObjDir, [string]$FileName, [string]$Description, [ValidateSet('app','dll')][string]$Type)
    $resDir = Join-Path (Split-Path $PSScriptRoot -Parent) 'host\res'
    $rcIn   = Join-Path $resDir 'version.rc.in'
    $base   = [IO.Path]::GetFileNameWithoutExtension($FileName) -replace '[^A-Za-z0-9_]', '_'
    $rc     = Join-Path $ObjDir ($base + '.version.rc')
    $res    = Join-Path $ObjDir ($base + '.version.res')
    $text = [IO.File]::ReadAllText($rcIn)
    $text = $text.Replace('@FILE_NAME@', $FileName).Replace('@FILE_DESC@', $Description)
    $text = $text.Replace('@FILE_TYPE@', $(if ($Type -eq 'app') { 'VFT_APP' } else { 'VFT_DLL' }))
    New-Item -ItemType Directory -Force $ObjDir | Out-Null
    [IO.File]::WriteAllText($rc, $text, (New-Object Text.ASCIIEncoding))
    # The command to run inside the vcvars environment, and the .res it makes.
    return @{ Cmd = "rc /nologo /i `"$resDir`" /fo `"$res`" `"$rc`""; Res = $res }
}
