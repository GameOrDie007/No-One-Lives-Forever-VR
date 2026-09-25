# ---------------------------------------------------------------------------
# symbolize.ps1 - TURN "cshell.dll+0016952B" INTO A FILE AND A LINE NUMBER.
#
# Every crash this project records prints module-relative addresses:
#
#     === CRASH ===
#       code C0000005 at 6609952B  cshell.dll+0016952B
#       access violation WRITING address 00000019  small
#       frames (via ebp, each labelled by the module it lands in):
#         [ 0] return 6601B59A  cshell.dll+000EB59A
#
# Those are exact and they have been unreadable, so a crash in our own client
# has been left unresolved beyond the module for weeks. The .pdb beside the
# build answers it in one call, and dbghelp.dll - which ships with Windows -
# is the thing that reads a .pdb.
#
#   .\tools\symbolize.ps1 0016952B
#   .\tools\symbolize.ps1 0016952B,000EB59A,000F47D4,000F4EDB
#   .\tools\symbolize.ps1 -Module renstub 0001A2B3
#
# THE PDB MUST MATCH THE BINARY THAT CRASHED. A rebuild moves everything, so
# resolving yesterday's address against today's symbols produces a confident
# and completely wrong answer. This refuses if the .pdb is newer than the log
# it is being asked about, when given one with -Log.
# ---------------------------------------------------------------------------
param(
    # One or more module-relative addresses, hex, with or without 0x.
    [Parameter(Mandatory = $true, Position = 0)][string[]]$Rva,
    # Which of our modules: cshell (the client) or renstub (the renderer).
    [ValidateSet('cshell', 'renstub')]
    [string]$Module = 'cshell',
    # Optional: the crash log these addresses came from, so the pdb's age can
    # be checked against it.
    [string]$Log = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path $PSScriptRoot -Parent

if ($Module -eq 'cshell') {
    $dir = Join-Path $Root 'src\nolf1-modernizer\NOLF\ClientShellDLL\Final_Release'
    $img = Join-Path $dir 'CShell.dll'
    $pdb = Join-Path $dir 'ClientShellDLL.pdb'
} else {
    $dir = Join-Path $Root 'host\renstub'
    $img = Join-Path $Root 'game\d3dstub.ren'
    $pdb = Join-Path $dir 'd3dstub.pdb'
}

foreach ($f in @($img, $pdb)) {
    if (-not (Test-Path -LiteralPath $f)) { throw "not found: $f" }
}

Write-Host ''
Write-Host ("  module : {0}" -f $img) -ForegroundColor DarkGray
Write-Host ("  symbols: {0}  ({1})" -f $pdb, (Get-Item $pdb).LastWriteTime) -ForegroundColor DarkGray

# THE STALENESS CHECK. Symbols newer than the crash describe a different
# binary, and the answer they give will look perfectly reasonable.
if ($Log -and (Test-Path -LiteralPath $Log)) {
    $lt = (Get-Item $Log).LastWriteTime
    $pt = (Get-Item $pdb).LastWriteTime
    if ($pt -gt $lt) {
        Write-Host ''
        Write-Host '  REFUSING: the .pdb is NEWER than that crash log, so it describes' -ForegroundColor Red
        Write-Host ('  a different build. pdb {0} vs log {1}.' -f $pt, $lt) -ForegroundColor Red
        Write-Host '  Resolving against it would give a confident wrong answer.' -ForegroundColor Red
        exit 1
    }
}

Add-Type -Namespace Sym -Name Dbg -MemberDefinition @'
[DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern bool SymInitialize(IntPtr h, string path, bool invade);
[DllImport("dbghelp.dll", SetLastError=true)]
public static extern bool SymCleanup(IntPtr h);
[DllImport("dbghelp.dll", SetLastError=true)]
public static extern uint SymSetOptions(uint o);
[DllImport("dbghelp.dll", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern ulong SymLoadModuleEx(IntPtr h, IntPtr file, string img,
    string mod, ulong baseAddr, uint size, IntPtr data, uint flags);
[DllImport("dbghelp.dll", SetLastError=true)]
public static extern bool SymFromAddr(IntPtr h, ulong addr, out ulong disp, byte[] si);
[DllImport("dbghelp.dll", SetLastError=true)]
public static extern bool SymGetLineFromAddr64(IntPtr h, ulong addr, out uint disp, byte[] line);
'@

$SYMOPT_LOAD_LINES = 0x10
$SYMOPT_UNDNAME    = 0x02
$null = [Sym.Dbg]::SymSetOptions($SYMOPT_LOAD_LINES -bor $SYMOPT_UNDNAME)

# A made-up base. Everything below is base + rva, so the value only has to be
# free and consistent.
$base = [uint64]0x10000000
$hProc = [System.Diagnostics.Process]::GetCurrentProcess().Handle

if (-not [Sym.Dbg]::SymInitialize($hProc, $dir, $false)) {
    throw ("SymInitialize failed ({0})" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
}
try {
    $loaded = [Sym.Dbg]::SymLoadModuleEx($hProc, [IntPtr]::Zero, $img, $null, $base, 0, [IntPtr]::Zero, 0)
    if ($loaded -eq 0) {
        throw ("SymLoadModuleEx failed ({0}) - is the .pdb beside the .dll?" -f
               [Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }

    Write-Host ''
    foreach ($r in ($Rva | ForEach-Object { $_ -split '[,; ]' } | Where-Object { $_ })) {
        $clean = $r -replace '^0x', '' -replace '^\+', ''
        $val = 0
        if (-not [uint32]::TryParse($clean, [Globalization.NumberStyles]::HexNumber,
                                    [Globalization.CultureInfo]::InvariantCulture, [ref]$val)) {
            Write-Host ("  {0,-12} not a hex address" -f $r) -ForegroundColor Yellow
            continue
        }
        $addr = $base + [uint64]$val

        # SYMBOL_INFO on x64: the header is 88 bytes with tail padding, but
        # Name[] starts at offset 84 - SizeOfStruct(4) TypeIndex(4)
        # Reserved[2](16) Index(4) Size(4) ModBase(8) Flags(4) pad(4) Value(8)
        # Address(8) Register(4) Scope(4) Tag(4) NameLen(4) MaxNameLen(4).
        # Reading from 88 silently eats the first FOUR characters, which
        # turned CButeMgr::GetVector into "eMgr::GetVector" and looked like a
        # mangling quirk rather than an arithmetic mistake.
        $si = New-Object byte[] (88 + 512)
        [BitConverter]::GetBytes([uint32]88).CopyTo($si, 0)     # SizeOfStruct
        [BitConverter]::GetBytes([uint32]500).CopyTo($si, 80)   # MaxNameLen
        $disp = [uint64]0
        $name = '(no symbol)'
        if ([Sym.Dbg]::SymFromAddr($hProc, $addr, [ref]$disp, $si)) {
            $len = [BitConverter]::ToUInt32($si, 76)
            if ($len -gt 0 -and $len -lt 500) {
                $name = [Text.Encoding]::ASCII.GetString($si, 84, $len)
            }
        }

        # IMAGEHLP_LINE64: 40 bytes. LineNumber at +16, FileName pointer at +24.
        $ln = New-Object byte[] 40
        [BitConverter]::GetBytes([uint32]40).CopyTo($ln, 0)
        $ldisp = [uint32]0
        $where = ''
        if ([Sym.Dbg]::SymGetLineFromAddr64($hProc, $addr, [ref]$ldisp, $ln)) {
            $lineNo = [BitConverter]::ToUInt32($ln, 16)
            $pFile  = [IntPtr][BitConverter]::ToInt64($ln, 24)
            $file   = if ($pFile -ne [IntPtr]::Zero) {
                          [Runtime.InteropServices.Marshal]::PtrToStringAnsi($pFile)
                      } else { '' }
            if ($file) { $where = ('{0}:{1}' -f (Split-Path $file -Leaf), $lineNo) }
        }

        Write-Host ("  +{0}" -f $clean.PadRight(10)) -NoNewline -ForegroundColor Cyan
        Write-Host ("{0}" -f $name) -NoNewline
        if ($disp -gt 0) { Write-Host (" +{0}" -f $disp) -NoNewline -ForegroundColor DarkGray }
        if ($where) { Write-Host ("   {0}" -f $where) -ForegroundColor Green }
        else { Write-Host '' }
    }
} finally {
    $null = [Sym.Dbg]::SymCleanup($hProc)
}
Write-Host ''
