# ---------------------------------------------------------------------------
# write-zip.ps1 - Write-ReleaseZip, a zip with FORWARD-SLASH entry names.
#
# Compress-Archive in Windows PowerShell 5.1 names entries with backslashes
# (game\d3dstub.ren). The zip format says '/', and while Explorer and 7-Zip
# forgive it, other extractors unpack such an archive as flat files with
# backslashes IN THEIR NAMES - a download that "does not work" through no fault
# of the person who downloaded it. Each entry keeps its file's timestamp.
#
#   . .\tools\write-zip.ps1
#   Write-ReleaseZip -Folder <staged folder> -Zip <out.zip>
# ---------------------------------------------------------------------------
function Write-ReleaseZip {
    param([Parameter(Mandatory)][string]$Folder, [Parameter(Mandatory)][string]$Zip)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $base = (Resolve-Path -LiteralPath $Folder).Path.TrimEnd('\') + '\'
    if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
    $fs = [System.IO.File]::Open($Zip, [System.IO.FileMode]::CreateNew)
    try {
        $za = New-Object System.IO.Compression.ZipArchive($fs, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Get-ChildItem -LiteralPath $Folder -Recurse -File | Sort-Object FullName | ForEach-Object {
                $name = $_.FullName.Substring($base.Length).Replace('\', '/')
                $e = $za.CreateEntry($name, [System.IO.Compression.CompressionLevel]::Optimal)
                $e.LastWriteTime = $_.LastWriteTime
                $out = $e.Open()
                try {
                    $in = [System.IO.File]::OpenRead($_.FullName)
                    try { $in.CopyTo($out) } finally { $in.Dispose() }
                } finally { $out.Dispose() }
            }
        } finally { $za.Dispose() }
    } finally { $fs.Dispose() }
    # Prove it: no entry may carry a backslash.
    $check = [System.IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $bad = @($check.Entries | Where-Object { $_.FullName.Contains('\') })
        if ($bad.Count) { throw ("zip has backslash entry names: " + ($bad.FullName -join ', ')) }
    } finally { $check.Dispose() }
}
