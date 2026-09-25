// rezfs - open one of the game's own files by name. See rezfs.h.
//
// The format is written up in tools/rez.py, which is the reference reading and
// is verified against lithrez's own extraction. Two things in it are worth
// repeating here because both are silent when wrong:
//
//   - A resource entry ends with a name AND a comment, both null-terminated.
//     The comment is almost always empty, which is one zero byte. Miss it and
//     every entry after the first is one byte out; the tell is that the next
//     entry's position lands outside the file.
//
//   - The extension is four characters stored REVERSED, so an .ABC model reads
//     "CBA" in file order. Strip the padding before reversing: reverse first
//     and the padding ends up at the front, which names every file ".\0ABC"
//     and makes all 4754 of them miss.

#include "rezfs.h"

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct RezEntry
{
	int      nArchive;		// index into g_Archives
	uint32_t nPos;
	uint32_t nSize;
};

struct Archive
{
	std::string sPath;
	HANDLE      hFile;
	uint32_t    nLength;
};

std::vector<Archive> g_Archives;
std::unordered_map<std::string, RezEntry> g_Files;
R3D_LogFn g_pfnLog = nullptr;
bool g_bInit = false;

void Log(const char* fmt, ...)
{
	if (!g_pfnLog) return;
	char buf[1024];
	va_list ap; va_start(ap, fmt);
	_vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
	va_end(ap);
	g_pfnLog("%s", buf);
}

// '/' and '\\' are the same separator and case does not matter - that is what
// the engine accepts, and the archives store everything uppercase anyway.
std::string Norm(const char* p)
{
	std::string s;
	if (!p) return s;
	while (*p == '/' || *p == '\\') ++p;
	s.reserve(strlen(p) + 1);
	for (; *p; ++p)
	{
		char c = *p;
		if (c == '\\') c = '/';
		else if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		s.push_back(c);
	}
	return s;
}

bool ReadAt(Archive& a, uint32_t nPos, void* pDst, uint32_t nBytes)
{
	if (!nBytes) return true;
	if ((uint64_t)nPos + nBytes > a.nLength) return false;

	// A POSITIONED READ, NOT A SEEK AND THEN A READ.
	//
	// SetFilePointerEx followed by ReadFile is two operations against one
	// shared file pointer. Any other read of the same archive between them -
	// and the texture rescue reads .dtx files out of these same archives -
	// moves the pointer, and this call then returns the RIGHT NUMBER OF BYTES
	// FROM THE WRONG OFFSET. It succeeds, so nothing upstream can tell.
	//
	// That is what produced "WORLDS/M01S02.DAT is version 0, expected 66" on
	// the second world load of a session: the level file parsed as zeros, the
	// file-texture rescue was therefore off, and 17521 of the level's 26904
	// polygons were dropped as untextured. It is the black HQ and the empty
	// blue Morocco, and it is transient, which is why a direct +runworld of
	// the same level always looked perfect.
	//
	// Passing an OVERLAPPED offset to a synchronous handle reads from that
	// offset without consulting the file pointer, so there is no window.
	uint8_t* pOut = (uint8_t*)pDst;
	uint32_t nDone = 0;
	while (nDone < nBytes)
	{
		OVERLAPPED ov;
		memset(&ov, 0, sizeof ov);
		ov.Offset     = (DWORD)(nPos + nDone);
		ov.OffsetHigh = 0;
		DWORD nGot = 0;
		if (!ReadFile(a.hFile, pOut + nDone, nBytes - nDone, &nGot, &ov))
			return false;
		if (!nGot) return false;			// short of the end: not our file
		nDone += nGot;
	}
	return true;
}

// A directory block is a flat run of entries; recursion is on the
// subdirectories it names, not on the bytes. Depth is bounded because a
// malformed archive must not take the game down with a stack overflow.
bool ReadDir(int nArch, uint32_t nPos, uint32_t nSize,
			 const std::string& sPrefix, int nDepth, uint32_t* pnAdded)
{
	if (nDepth > 24 || !nSize || nSize > (1u << 24)) return false;
	Archive& a = g_Archives[nArch];

	std::vector<uint8_t> blk(nSize);
	if (!ReadAt(a, nPos, blk.data(), nSize)) return false;

	struct Sub { uint32_t nPos, nSize; std::string sPrefix; };
	std::vector<Sub> subs;

	uint32_t o = 0;
	while (o < nSize)
	{
		if (o + 4 > nSize) return false;
		uint32_t nKind = *(const uint32_t*)(blk.data() + o); o += 4;

		// The name, and for a file the comment after it, are null-terminated
		// and must not be allowed to run off the end of the block.
		auto CStr = [&](std::string& sOut) -> bool
		{
			uint32_t nStart = o;
			while (o < nSize && blk[o]) ++o;
			if (o >= nSize) return false;		// unterminated
			sOut.assign((const char*)blk.data() + nStart, o - nStart);
			++o;
			return true;
		};

		if (nKind == 1)
		{
			if (o + 12 > nSize) return false;
			const uint32_t dPos  = *(const uint32_t*)(blk.data() + o + 0);
			const uint32_t dSize = *(const uint32_t*)(blk.data() + o + 4);
			o += 12;						// pos, size, time
			std::string sName;
			if (!CStr(sName)) return false;
			subs.push_back({ dPos, dSize, sPrefix + Norm(sName.c_str()) + "/" });
		}
		else if (nKind == 0)
		{
			if (o + 24 > nSize) return false;
			const uint32_t fPos  = *(const uint32_t*)(blk.data() + o + 0);
			const uint32_t fSize = *(const uint32_t*)(blk.data() + o + 4);
			const uint32_t nExt  = *(const uint32_t*)(blk.data() + o + 16);
			const uint32_t nKeys = *(const uint32_t*)(blk.data() + o + 20);
			o += 24;						// pos, size, time, id, ext, numKeys
			std::string sName, sComment;
			if (!CStr(sName)) return false;
			if (!CStr(sComment)) return false;
			// Keys would be a trailing block this reading does not decode.
			// NOLF has none; refuse rather than walk off into it.
			if (nKeys) return false;
			if ((uint64_t)fPos + fSize > a.nLength) return false;

			char szExt[5] = { 0, 0, 0, 0, 0 };
			{
				const uint8_t raw[4] = { (uint8_t)(nExt & 0xFF),
										 (uint8_t)((nExt >> 8) & 0xFF),
										 (uint8_t)((nExt >> 16) & 0xFF),
										 (uint8_t)((nExt >> 24) & 0xFF) };
				int n = 4;
				while (n > 0 && raw[n - 1] == 0) --n;		// strip THEN reverse
				for (int i = 0; i < n; ++i) szExt[i] = (char)raw[n - 1 - i];
			}

			std::string sFull = sPrefix + Norm(sName.c_str());
			if (szExt[0]) { sFull += "."; sFull += Norm(szExt); }

			// LATER ARCHIVES OVERRIDE EARLIER ONES. operator[] replaces, which
			// is the whole point of the mount order.
			RezEntry e{}; e.nArchive = nArch; e.nPos = fPos; e.nSize = fSize;
			g_Files[sFull] = e;
			++*pnAdded;
		}
		else return false;
	}
	if (o != nSize) return false;

	for (size_t i = 0; i < subs.size(); ++i)
		if (!ReadDir(nArch, subs[i].nPos, subs[i].nSize, subs[i].sPrefix,
					 nDepth + 1, pnAdded))
			return false;
	return true;
}

bool MountArchive(const char* pszPath)
{
	HANDLE h = CreateFileA(pszPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
						   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;

	LARGE_INTEGER li{};
	if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0x8B || li.QuadPart > 0x7FFFFFFF)
		{ CloseHandle(h); return false; }

	Archive a; a.sPath = pszPath; a.hFile = h; a.nLength = (uint32_t)li.QuadPart;
	g_Archives.push_back(a);
	const int nArch = (int)g_Archives.size() - 1;

	uint32_t hdr[3] = { 0, 0, 0 };			// version, rootDirPos, rootDirSize
	uint32_t nAdded = 0;
	if (!ReadAt(g_Archives[nArch], 0x7F, hdr, sizeof(hdr)) || hdr[0] != 1
		|| !ReadDir(nArch, hdr[1], hdr[2], "", 0, &nAdded))
	{
		Log("  REZ: %s is not a readable rez (version %u) - skipped",
			pszPath, hdr[0]);
		CloseHandle(h);
		g_Archives.pop_back();
		return false;
	}
	Log("  REZ: mounted %s - %u files", pszPath, nAdded);
	return true;
}

// The engine's own `-rez <name>` list, in order. Not a copy of it: the same
// bytes the engine was started with.
void MountFromCommandLine(const char* pszGameDir)
{
	const char* pszCmd = GetCommandLineA();
	if (!pszCmd) return;

	for (const char* p = pszCmd; *p; ++p)
	{
		if ((*p != '-' && *p != '/') || _strnicmp(p + 1, "rez", 3) != 0) continue;
		if (p[4] != ' ' && p[4] != '\t') continue;
		const char* q = p + 4;
		while (*q == ' ' || *q == '\t') ++q;
		bool bQuote = (*q == '"');
		if (bQuote) ++q;
		const char* e = q;
		while (*e && (bQuote ? *e != '"' : (*e != ' ' && *e != '\t'))) ++e;
		if (e == q) continue;

		char szName[MAX_PATH];
		size_t n = (size_t)(e - q);
		if (n >= sizeof(szName)) n = sizeof(szName) - 1;
		memcpy(szName, q, n); szName[n] = 0;

		// `-rez custom` names a DIRECTORY, not an archive - the engine allows
		// loose files. Not mounted here yet; loose overrides are a later
		// concern and saying so beats pretending it worked.
		char szFull[MAX_PATH * 2];
		_snprintf_s(szFull, sizeof(szFull), _TRUNCATE, "%s\\%s", pszGameDir, szName);
		const DWORD nAttr = GetFileAttributesA(szFull);
		if (nAttr != INVALID_FILE_ATTRIBUTES && (nAttr & FILE_ATTRIBUTE_DIRECTORY))
		{
			Log("  REZ: -rez %s is a directory - loose files are not mounted yet",
				szName);
			continue;
		}
		if (nAttr == INVALID_FILE_ATTRIBUTES)
		{
			Log("  REZ: -rez %s is not present - skipped", szName);
			continue;
		}
		MountArchive(szFull);
		p = e;
	}
}

}  // namespace

uint32_t RezFS_Init(R3D_LogFn pfnLog)
{
	if (g_bInit) return (uint32_t)g_Files.size();
	g_bInit = true;
	g_pfnLog = pfnLog;

	// The archives are named relative to the game directory, which is where
	// lithtech.exe lives - not necessarily the process's current directory.
	char szExe[MAX_PATH] = { 0 };
	GetModuleFileNameA(nullptr, szExe, MAX_PATH);
	char* pSlash = strrchr(szExe, '\\');
	if (pSlash) *pSlash = 0;

	MountFromCommandLine(szExe);

	Log("  REZ: %u archives, %u files reachable (game dir %s)",
		(uint32_t)g_Archives.size(), (uint32_t)g_Files.size(), szExe);
	return (uint32_t)g_Files.size();
}

bool RezFS_Exists(const char* pszPath)
{
	if (!g_bInit || !pszPath) return false;
	return g_Files.find(Norm(pszPath)) != g_Files.end();
}

uint8_t* RezFS_Read(const char* pszPath, uint32_t* pnSize)
{
	if (pnSize) *pnSize = 0;
	if (!g_bInit || !pszPath) return nullptr;

	auto it = g_Files.find(Norm(pszPath));
	if (it == g_Files.end()) return nullptr;

	const RezEntry& e = it->second;
	if (e.nArchive < 0 || (size_t)e.nArchive >= g_Archives.size()) return nullptr;

	uint8_t* p = (uint8_t*)malloc(e.nSize ? e.nSize : 1);
	if (!p) return nullptr;
	if (!ReadAt(g_Archives[e.nArchive], e.nPos, p, e.nSize)) { free(p); return nullptr; }

	if (pnSize) *pnSize = e.nSize;
	return p;
}

uint8_t* RezFS_ReadHead(const char* pszPath, uint32_t nMax, uint32_t* pnSize)
{
	if (pnSize) *pnSize = 0;
	if (!g_bInit || !pszPath || !nMax) return nullptr;

	auto it = g_Files.find(Norm(pszPath));
	if (it == g_Files.end()) return nullptr;

	const RezEntry& e = it->second;
	if (e.nArchive < 0 || (size_t)e.nArchive >= g_Archives.size()) return nullptr;

	const uint32_t n = e.nSize < nMax ? e.nSize : nMax;
	uint8_t* p = (uint8_t*)malloc(n ? n : 1);
	if (!p) return nullptr;
	if (!ReadAt(g_Archives[e.nArchive], e.nPos, p, n)) { free(p); return nullptr; }

	if (pnSize) *pnSize = n;
	return p;
}

void RezFS_Free(uint8_t* p) { free(p); }

uint32_t RezFS_FileCount()    { return (uint32_t)g_Files.size(); }
uint32_t RezFS_ArchiveCount() { return (uint32_t)g_Archives.size(); }

uint32_t RezFS_ForEach(const char* pszSuffix,
					   void (*pfn)(const char*, uint32_t, void*), void* pUser)
{
	if (!g_bInit) return 0;
	const std::string sSuf = Norm(pszSuffix ? pszSuffix : "");
	uint32_t n = 0;
	for (auto it = g_Files.begin(); it != g_Files.end(); ++it)
	{
		const std::string& s = it->first;
		if (sSuf.size() > s.size()) continue;
		if (!sSuf.empty() && s.compare(s.size() - sSuf.size(), sSuf.size(), sSuf) != 0)
			continue;
		++n;
		if (pfn) pfn(s.c_str(), it->second.nSize, pUser);
	}
	return n;
}
