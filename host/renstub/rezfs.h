// ---------------------------------------------------------------------------
// rezfs - open one of the game's own files by name.
//
// Stage 0 of docs/PLAN-FILES-NOT-HEAP.md. Everything this renderer draws is
// currently reverse-engineered out of lithtech.exe's heap, and every rule we
// have not re-derived is a defect waiting to be found in a headset. The art is
// all on disk - 3403 .DTX, 238 .ABC, 71 .DAT - and reading it from there is
// the only way to stop guessing. This is the door to it.
//
// The reference implementation and the format write-up are in tools/rez.py,
// which is verified against Monolith's own lithrez extraction: 4871 files,
// 0 missing, 0 extra, 0 size mismatches, 203 content samples byte-identical.
//
// THE MOUNT ORDER IS READ FROM THE ENGINE'S OWN COMMAND LINE. Later archives
// override earlier ones, and getting that order wrong means quietly serving the
// pre-patch version of a file - exactly the class of silent art bug this whole
// plan exists to end. lithtech.exe is launched with an explicit `-rez A -rez B`
// list (tools/run.ps1 owns it), so the command line is the same source of truth
// the engine used and cannot drift from it.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "render3d.h"		// R3D_LogFn

// Mount every archive named by the engine's command line, in order, plus the
// game directory itself. Safe to call more than once; the second call is a
// no-op. Returns the number of distinct file names reachable.
uint32_t RezFS_Init(R3D_LogFn pfnLog);

// Is a file there? Cheap - a hash lookup, no I/O.
bool RezFS_Exists(const char* pszPath);

// Read a whole file. Returns a buffer the CALLER frees with RezFS_Free, and
// sets *pnSize. Null if the name is not found or the read fails.
//
// Path separators may be '/' or '\\' and case does not matter, which is what
// the engine accepts. A leading slash is ignored.
uint8_t* RezFS_Read(const char* pszPath, uint32_t* pnSize);
void     RezFS_Free(uint8_t* p);

// Read at most the first nMax bytes. Same ownership rules as RezFS_Read, and
// *pnSize is what was actually read, which may be less if the file is smaller.
//
// For identifying a world without reading it. The 103 .DAT files are 240 MB
// between them and everything needed to tell one from another sits in the
// first few hundred bytes, so reading whole files to compare headers would be
// a thousand times the I/O for the same answer.
uint8_t* RezFS_ReadHead(const char* pszPath, uint32_t nMax, uint32_t* pnSize);

// How many files, and how many archives they came from. For the log line that
// says whether this is working at all.
uint32_t RezFS_FileCount();
uint32_t RezFS_ArchiveCount();

// Walk every known name whose path ends with this suffix (case-insensitive),
// e.g. ".DTX". Returns how many matched; pfn may be null to just count.
uint32_t RezFS_ForEach(const char* pszSuffix,
                       void (*pfn)(const char* pszPath, uint32_t nSize, void* pUser),
                       void* pUser);
