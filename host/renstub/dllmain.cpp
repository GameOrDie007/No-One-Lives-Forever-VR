// ---------------------------------------------------------------------------
// d3dstub.ren - the first renderer we own.
//
// It draws nothing. That is deliberate: this is the gate that proves we can BE
// the renderer rather than watch one, and a stub that also tries to bring up a
// graphics device would confuse "the engine accepted our table" with "our
// device works". One question at a time.
//
// What it does:
//
//   - exports the three symbols the engine calls, __cdecl (measured)
//   - delegates GetSupportedModes and FreeModeList to the real d3d.ren, so the
//     RMode layout - which has NOT been measured - is not needed yet
//   - fills all 37 renderer slots at offsets 108..252 with its own functions
//   - answers each one with a value taken from the real renderer where that was
//     measured, and a defensible default elsewhere
//   - logs the first call to every slot, and a count of all of them at the end
//
// Everything here rests on docs/PHASE0-RENDERSTRUCT.md and docs/SCENEDESC.md:
// the slot offsets, the calling convention, Init returning 0, and the PFormat.
// Nothing is guessed that was measurable.
//
// Selected with +RenderDll d3dstub.ren. Reverting is not passing the switch;
// no file is renamed or overwritten.
// ---------------------------------------------------------------------------

#include <share.h>		// _fsopen: a log nothing else can read is not a log
#include <windows.h>
#include <math.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>   // ID3D11Debug, for +StubD3DDebug 1
#include <dxgi.h>
#include <stdio.h>
#include <stdarg.h>
#include <intrin.h>
#include <stdint.h>
#include <string>
#include <vector>
namespace { void StartWatchdog(); void StopWatchdog(); void StopReportThread(); }

#include "render2d.h"
#include "eyeshare.h"
#include "render3d.h"
#include "renlock.h"
#include "screenlock.h"
#include "vrblock.h"
#include "rezfs.h"
#include "butes.h"
#include "dtx.h"

namespace
{
	HMODULE g_hReal    = nullptr;
	FARPROC g_pfnSetup = nullptr;

	FILE*  g_pLog     = nullptr;
	HANDLE g_hLogLock = nullptr;

	const int kFirstSlotOffset = 108;		// measured
	const int kSlots           = 37;		// measured

	volatile LONG g_nCalls[kSlots] = { 0 };
	const char*   g_szName[kSlots] = { nullptr };

	void Log(const char* fmt, ...)
	{
		if (!g_pLog) return;
		WaitForSingleObject(g_hLogLock, INFINITE);
		va_list a; va_start(a, fmt);
		vfprintf(g_pLog, fmt, a);
		va_end(a);
		fputc('\n', g_pLog);
		{
			// Timed: a flush is a WriteFile, and a lazy-writer flush on a
			// busy disk can block the caller for the disk's seek time.
			LARGE_INTEGER q0, q1, f;
			QueryPerformanceCounter(&q0);
			fflush(g_pLog);		// a renderer crash must not eat the last lines
			QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&f);
			if (f.QuadPart)
				R3D_AddLogMs((double)(q1.QuadPart - q0.QuadPart) * 1000.0
							 / (double)f.QuadPart);
		}
		ReleaseMutex(g_hLogLock);
	}

	// ------------------------------------------------------------------
	// Where does the window live?
	//
	// Init has to create a device, and a device needs an HWND. The engine
	// hands Init a RenderStructInit whose layout we have only partly mapped -
	// the renderer's own DLL filename sits at offset 8 (docs/SCENEDESC.md).
	// The window handle is NOT located, and picking an offset because AvP2's
	// header has one there is exactly the guess this project keeps paying for.
	//
	// So: dump the struct wide, and test EVERY word with IsWindow(). A window
	// handle identifies itself - it is the one field the OS will confirm. The
	// process's own top-level windows are enumerated alongside as the control:
	// a word that IsWindow() accepts AND that appears in that list is the
	// engine's window, and nothing else in the struct can accidentally be it.
	// ------------------------------------------------------------------

	struct ModRange { const char* pszName; uintptr_t lo, hi; };
	// EVERY module, not the three we happened to name.
	//
	// This list held d3dstub.ren, lithtech.exe and d3d.ren, so Classify said "?"
	// for ClientShellDLL.dll, d3dim700.dll, ddraw.dll, the CRT and every system
	// DLL in the process - and a crash report that says "at 033A6916 ?" reads
	// exactly like execution in the heap. It was costly: the 5 September
	// crash was diagnosed as a jump through freed memory on the strength of
	// nothing but that question mark.
	ModRange g_Mods[96];
	int      g_nMods = 0;
	char     g_szModNames[96][40];

	void NoteModule(const char* pszName, HMODULE h)
	{
		if (!h || g_nMods >= 96) return;
		const IMAGE_DOS_HEADER* pDos = (const IMAGE_DOS_HEADER*)h;
		if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return;
		const IMAGE_NT_HEADERS* pNt =
			(const IMAGE_NT_HEADERS*)((const BYTE*)h + pDos->e_lfanew);
		if (pNt->Signature != IMAGE_NT_SIGNATURE) return;
		g_Mods[g_nMods].pszName = pszName;
		g_Mods[g_nMods].lo = (uintptr_t)h;
		g_Mods[g_nMods].hi = (uintptr_t)h + pNt->OptionalHeader.SizeOfImage;
		++g_nMods;
	}

	// Fill the table from the process itself. Called at crash time as well as at
	// init, because a folder change loads DLLs and the interesting address is
	// usually in one of them.
	void NoteAllModules()
	{
		HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (hSnap == INVALID_HANDLE_VALUE) return;
		MODULEENTRY32 me{}; me.dwSize = sizeof(me);
		if (Module32First(hSnap, &me))
		{
			do
			{
				if (g_nMods >= 96) break;
				bool bHave = false;
				for (int i = 0; i < g_nMods; ++i)
					if (g_Mods[i].lo == (uintptr_t)me.modBaseAddr) { bHave = true; break; }
				if (bHave) continue;
				strncpy(g_szModNames[g_nMods], me.szModule,
						sizeof(g_szModNames[0]) - 1);
				g_szModNames[g_nMods][sizeof(g_szModNames[0]) - 1] = 0;
				g_Mods[g_nMods].pszName = g_szModNames[g_nMods];
				g_Mods[g_nMods].lo = (uintptr_t)me.modBaseAddr;
				g_Mods[g_nMods].hi = (uintptr_t)me.modBaseAddr + me.modBaseSize;
				++g_nMods;
			} while (Module32Next(hSnap, &me));
		}
		CloseHandle(hSnap);
	}

	// Which module, and HOW FAR INTO IT - an offset is what makes an address
	// something you can look up.
	const char* Where(uintptr_t v, char* pOut, size_t nOut)
	{
		for (int i = 0; i < g_nMods; ++i)
			if (v >= g_Mods[i].lo && v < g_Mods[i].hi)
			{
				sprintf_s(pOut, nOut, "%s+%08X", g_Mods[i].pszName,
					(unsigned)(v - g_Mods[i].lo));
				return pOut;
			}
		sprintf_s(pOut, nOut, "%s", (v == 0) ? "-" : (v < 0x10000 ? "small" : "?"));
		return pOut;
	}

	const char* Classify(uintptr_t v)
	{
		for (int i = 0; i < g_nMods; ++i)
			if (v >= g_Mods[i].lo && v < g_Mods[i].hi) return g_Mods[i].pszName;
		if (v == 0) return "-";
		if (v < 0x10000) return "small";
		return "?";
	}

	HWND g_hWndSeen[16] = { nullptr };
	int  g_nWndSeen = 0;

	BOOL CALLBACK OnWindow(HWND hWnd, LPARAM)
	{
		DWORD nPid = 0;
		GetWindowThreadProcessId(hWnd, &nPid);
		if (nPid != GetCurrentProcessId()) return TRUE;

		char szClass[128]{}, szTitle[128]{};
		GetClassNameA(hWnd, szClass, sizeof(szClass) - 1);
		GetWindowTextA(hWnd, szTitle, sizeof(szTitle) - 1);
		RECT rc{}, rcClient{};
		GetWindowRect(hWnd, &rc);
		GetClientRect(hWnd, &rcClient);

		Log("   HWND %08X  class '%s'  title '%s'  %s  window %d,%d %dx%d  client %dx%d",
			(unsigned)(uintptr_t)hWnd, szClass, szTitle,
			IsWindowVisible(hWnd) ? "visible" : "HIDDEN",
			rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
			rcClient.right, rcClient.bottom);

		if (g_nWndSeen < 16) g_hWndSeen[g_nWndSeen++] = hWnd;
		return TRUE;
	}

	void LogWindows(const char* pszWhen)
	{
		Log("");
		Log("--- top-level windows owned by this process, %s ---", pszWhen);
		g_nWndSeen = 0;
		EnumWindows(OnWindow, 0);
		if (g_nWndSeen == 0)
		{
			Log("   NONE. The engine has not created its window yet - a device");
			Log("   cannot be made here and Init is the wrong place for it.");
		}
		Log("--- end windows ---");
	}

	// Dump the RenderStructInit wide, one line per word, with every test that
	// can identify a field applied to it at once.
	void DumpInit(void* p, int nWords)
	{
		Log("");
		Log("=== RenderStructInit at %p, %d words ===", p, nWords);
		Log("  (a STACK address: the engine builds it per call, so this is read live)");
		Log("  word offset   hex        as int      as float   ascii  is");

		int nHwndHits = 0;
		int nHwndOff[16];

		for (int i = 0; i < nWords; ++i)
		{
			const BYTE* pw = (const BYTE*)p + i * 4;
			if (IsBadReadPtr(pw, 4))
			{
				Log("  ... unreadable at word %d (offset %d)", i, i * 4);
				break;
			}
			const uint32_t v = *(const uint32_t*)pw;
			float f; memcpy(&f, &v, 4);

			char szA[5]{};
			for (int b = 0; b < 4; ++b)
			{
				const char c = (char)((v >> (b * 8)) & 0xFF);
				szA[b] = (c >= 32 && c < 127) ? c : '.';
			}

			// The identification. IsWindow is the only one of these that is a
			// fact rather than a reading.
			char szIs[160]{};
			const char* pszMod = Classify(v);
			if (v && IsWindow((HWND)(uintptr_t)v))
			{
				DWORD nPid = 0;
				GetWindowThreadProcessId((HWND)(uintptr_t)v, &nPid);
				sprintf_s(szIs, "*** HWND, %s ***",
					nPid == GetCurrentProcessId() ? "OURS" : "another process");
				if (nHwndHits < 16) nHwndOff[nHwndHits] = i * 4;
				++nHwndHits;
			}
			else if (strcmp(pszMod, "?") != 0 && strcmp(pszMod, "-") != 0)
				sprintf_s(szIs, "in %s", pszMod);

			Log("  %4d %6d   %08X  %11d  %11.4f  %s  %s",
				i, i * 4, v, (int)v, f, szA, szIs);
		}

		Log("");
		if (nHwndHits == 0)
		{
			Log("  NO field in the struct is a window handle. Whatever Init is given,");
			Log("  the window is not in it - or it does not exist yet.");
		}
		else
		{
			Log("  %d field(s) the OS confirms are windows:", nHwndHits);
			for (int i = 0; i < nHwndHits && i < 16; ++i)
				Log("     offset %d", nHwndOff[i]);
		}
		Log("=== end RenderStructInit ===");
	}

	// ------------------------------------------------------------------
	// Switches, read off the engine's own command line.
	//
	// probe-run.ps1 passes console variables as "+Name Value", so the same
	// -Set that configures the client configures the stub. Nothing here is a
	// rebuild: an A and a B arm of the same experiment are two command lines.
	// Every value is logged, because a run that cannot say what it ran is not
	// a measurement.
	// ------------------------------------------------------------------

	int CmdLineInt(const char* pszName, int nDefault)
	{
		const char* pszCmd = GetCommandLineA();
		if (!pszCmd) return nDefault;
		char szTok[64];
		sprintf_s(szTok, "+%s ", pszName);
		const char* p = strstr(pszCmd, szTok);
		if (!p) return nDefault;
		p += strlen(szTok);
		while (*p == ' ') ++p;
		return atoi(p);
	}

	// The same, for a WORD rather than a number. Stops at whitespace, so
	// `+StubModelProbe lamp` yields "lamp" and nothing after it.
	void CmdLineStr(const char* pszName, char* pOut, size_t nOut)
	{
		if (nOut) pOut[0] = 0;
		const char* pszCmd = GetCommandLineA();
		if (!pszCmd) return;
		char szTok[64];
		sprintf_s(szTok, "+%s ", pszName);
		const char* p = strstr(pszCmd, szTok);
		if (!p) return;
		p += strlen(szTok);
		while (*p == ' ') ++p;
		size_t i = 0;
		while (i + 1 < nOut && p[i] && p[i] != ' ' && p[i] != '	')
		{ pOut[i] = p[i]; ++i; }
		pOut[i] = 0;
	}

	int g_bDelegateInit = 0;		// call the REAL d3d.ren's Init from ours
	int g_bWantDevice   = 1;		// create our own D3D11 device in Init
	int g_nSceneDumpAt  = 400;		// which RenderScene call to dump the lists from
	// +StubD3DDebug 1: create the device with the debug layer and, at Term,
	// make it NAME what is still holding it. OFF by default - the layer
	// costs real performance and needs the Graphics Tools feature
	// installed, which a player will not have.
	int g_bD3DDebug     = 0;

	// Bisection. Slots in [lo, hi] are handed back to the real d3d.ren and the
	// rest stay ours, so the slot responsible for a behaviour can be found by
	// halving rather than by guessing which of 37 stubs is wrong.
	//
	// Delegating anything requires the real renderer to have a device, so this
	// forces Init to be delegated too - otherwise the real slots would be
	// called against a renderer that was never initialised, and the crash would
	// be blamed on the slot rather than on the harness.
	int g_nDelegateLo   = -1;
	int g_nDelegateHi   = -1;
	int g_bTrace4       = 0;		// call the real RebindLightmaps and record it
	// 1, and that single bit is the difference between a level that loads and
	// one that never does. MEASURED by bisection, not chosen: see the A/B in
	// docs/RENDERSCENE-FIRST-LOOK.md. RebindLightmaps is LTBOOL-style, so 0 is
	// LTFALSE and the engine treats it as a failed rebind and stops.
	int g_nRebindRet    = 1;
	// 1. MEASURED: at 2456x1328 a 0 crashes the engine 12 s into a level load
	// with 0xC0000005 and 0 world frames; a 1 runs clean with 1488. See
	// docs/HIGH-RES-CRASH.md.
	int g_nWarpRet      = 1;
	int g_bEyeShare     = 1;	// publish the frame as a shared texture
	int g_bDrawInterface = 1;	// also draw scene type 2, the menu's 3D
	// The aspect, in hundredths, to PILLARBOX the INTERFACE scene to inside its
	// own viewport. OFF by default: the client fits the authored field to the
	// viewport now (InterfaceMgr.cpp), which needs no bars and crops nothing,
	// and running both would crop what the client just fitted. Kept because
	// +StubIfaceAspect 133 reproduces RETAIL exactly - retail draws the scene
	// in a centred 4:3 region and covers the rest with two filler bars - and a
	// pixel comparison against retail wants that arm available.
	int g_nIfaceAspect = 0;
	int  g_bSkipFade    = 0;		// skip full-screen blits from a tiny surface
	int  g_bOnlyBig     = 0;		// draw only blits from a large surface
	int  g_bFloorDumped = 0;
	int  g_bFloorDump   = 0;
	int  g_bTexDumpAll  = 0;
	int  g_bProbe       = 1;	// one-shot ray probe from the camera
	int  g_bProbed      = 0;
	long g_nFrameDumpAt = 0;	// present number to write the back buffer at
	long g_nFrameDumpCount = 1;	// how many consecutive presents to write
	// +StubFrameDumpOnFolder N: write N consecutive frames starting at the
	// next menu-folder change. The main menu's textures dropping out and
	// reloading on a selection is a MOMENT, and one screenshot cannot see
	// it; sixty frames either side of the change can.
	long g_nFrameDumpOnFolder = 0;
	long g_nFrameDumpEvery = 0;
	// +StubTrace2D 1: log every surface call - create, delete, lock, unlock,
	// optimize, blit, upload - with its present number, but only while a
	// frame dump window is open, so the trace lines up with the frames on
	// disk. 2: always. The menu's pop-in is a few frames of one surface
	// wearing another's pixels, and this is the sequence that says how.
	long g_nTrace2D = 0;
	float g_fProbeAz = 999.0f;	// aim the depth probe (degrees, 999 = off)
	float g_fProbeEl = 0.0f;
	long  g_nProbeSweep = 1;	// rays across the aim, to bracket a thin feature
	long  g_bModelWalk = 0;	// hunt for the mesh from a published HOBJECT
	long  g_bDumpOnModels = 0;	// dump the first frame with a model in view
	long  g_bDumpSurfaces = 0;	// write the three largest 2D surfaces 5 s in - StubDumpSurfaces
	long  g_nDumpAfter = 2000;	// but not before this present: the fade-in is black
	int   s_nDumpsDone = 0;
	long  s_nRearmAt = 0;
	long  g_nDumpMax = 1;	// armed dumps allowed in a run - see StubDumpMax
	long  g_nMaxFps = 90;	// frame CAP: NOLF's game speed is proportional to it
	int   g_bSyncHost = 1;	// pace the game to the HOST's frame tick when it is live
	float g_fProbeStep = 0.1f;	// degrees between them
	int  g_bTexDumpedAll = 0;
	volatile LONG g_nFadeBlits = 0;
	// The frame marker, held until present so nothing draws over it.
	struct HeldMarker
	{
		const void* pKey; const void* pBits; int nPitch, nW, nH;
		int src[4], dst[4]; float fAlpha; int bPending;
	};
	// The marker is MANY blits - one per swatch - so all of a frame's are held.
	HeldMarker    g_MarkerHeld[64]{};
	int           g_nMarkerHeld = 0;
	int           g_bMarkerLast = 1;		// +StubMarkerLast 0 draws it in place
	int           g_bDumpOverlay = 0;
	volatile LONG g_nMarkerDrawnLast = 0;
}
static void DrawHeldMarker();
namespace
{
	uint32_t g_nScreenW = 0, g_nScreenH = 0;
	int g_bTrace17      = 0;		// call the real RenderScene and watch the words
	int g_bWorldDump    = 1;		// follow scene word 13 into the world
	int g_bWorldSeen    = 0;		// one-shot: the first non-null world handle
	int g_bMenuDumped   = 0;		// one-shot: the same walk at the menu

	// What the scene description's word 0 and word 13 actually hold, counted
	// over every RenderScene call rather than sampled at one chosen frame. The
	// first run of this dumped at call 230 - two and a half seconds in, still
	// the menu - and reported word 13 = 1, which is not a pointer. Which frame
	// the world first appears on is the thing being measured, so it cannot also
	// be an input.
	long g_nType[8]     = {0};		// histogram of word 0, values 0..6 and other
	long g_nW13Zero     = 0;		// word 13 == 0
	long g_nW13Small    = 0;		// word 13 nonzero but below 0x10000
	long g_nW13Ptr      = 0;		// word 13 looks like a real pointer
	int  g_bPtrDumped   = 0;		// one-shot: the first word 13 that IS a pointer
	int  g_bType1Dumped = 0;		// one-shot: the first scene with word 0 == 1

	// The 16-byte object the real RebindLightmaps allocates and returns. The
	// engine treats it as opaque and hands it straight back as scene word 13,
	// so the layout only has to satisfy US - but it mirrors the real one's,
	// because a structure that matches the renderer we are replacing is one
	// fewer thing to be wrong about.
	struct LMHandle
	{
		uint32_t nPad0;			// +0
		uint32_t nPad1;			// +4
		uint32_t pWorld;		// +8   *(void**)RebindLightmaps' argument
		uint16_t nStamp;		// +0xC 0xFFFF, so the first frame wraps and resets
		uint16_t nPad2;			// +0xE
	};
	uint32_t g_pWorld      = 0;	// the engine's world, as handed to slot 4
	int      g_bRebindReal = 1;	// build a real handle instead of returning 1
	int      g_bDraw3D     = 1;	// draw the world in RenderScene
	int      g_bClearCyan  = 0;	// paint every Clear cyan, as Phase 1 did
	volatile LONG g_nFullScreenBlits = 0;
	int      g_bSkipTint = 0;
	int      g_bScreenLock = 1;	// LockScreen reads the real back buffer
	int      g_bReqDump    = 1;	// dump the WarpToScreen/BlitFromScreen requests
	int      g_bTexDump    = 1;	// dump the engine texture object once
	int      g_bLMProbe    = 1;	// walk the lightmap chain once and check it
	int      g_bNativeFrustum = 0;	// build each eye's own asymmetric frustum
	volatile LONG g_nNativeMiss = 0;	// scenes drawn symmetric despite it
	const char* g_pszLastMissWhy = "";	// so each distinct reason is said once
	int      g_bLMEnable   = 1;	// draw with lightmaps at all
	int      g_nAniso      = 16;	// sampler anisotropy; 1 = plain bilinear
	int      g_bSkyStandIn = 1;	// untextured polygons draw sky blue, not white
	int      g_bSkyBox     = 1;	// draw the level's SkyBox model as a backdrop
	int      g_nModelDims  = 1024;	// mesh path: reject a model bigger than this
	int      g_bFastReadOpt = 1;	// cache address validity per page
	int      g_nWorldBSPOpt = 1;	// 0 VisBSP, 1 PhysicsBSP, 2 both
	int      g_bBSPCoverOpt = 0;	// report uncovered VisBSP faces
	int      g_nTransAlpha  = 45;	// glass opacity, per cent
	int      g_bTransNamesOpt = 1;	// widened see-through name rule
	int      g_nWorldRebuild = 0;	// rebuild the world mesh every N ms
	int      g_bWorldProbeOpt = 0;	// find HOBJECT -> world model
	int      g_bSprProbeOpt = 0;	// find where a sprite keeps its texture
	int      g_bSpritesOpt = 1;	// draw sprites at all
	int      g_bPrimsOpt = 1;	// particles, rain, water, canvases (+StubPrims 0)
	int      g_bDynLightsOpt = 1;	// dynamic lights on world and models (+StubDynLights 0)
	int      g_nSprScaleOpt = 100;	// sprite size, per cent
	int      g_bWorldXformOpt = 1;	// apply live world model transforms
	int      g_nWorldNudge = 0;	// forced offset to prove the transform path
	int      g_bSkipTrans  = 0;	// diagnostic: drop the TranslucentWorldModels
	int      g_bDrawNoPixels = 0;	// draw batches whose texture has no pixels
	int      g_bBlendModes = 3;	// 2D blend: 0 none, 1 all, 2 add+solid, 3 +mask
	int      g_bColour2D = 1;	// honour SetOptimized2DColor
	int      g_bColourKey = 1;	// honour the optimized surface transparent colour
	int      g_bModelBoxes = 0;	// draw a box per published model instance
	int      g_bLMOnly     = 0;	// lightmap with no texture
	int      g_nLMScale100 = 100;
	volatile LONG g_nTexSeen = 0, g_nTexPow2 = 0;
	int g_bMipFound = 0;
	int g_bWideDumped = 0;
	uint32_t g_pVisBSP = 0;
	int g_bTexWritten = 0;

	// Every texture the engine has bound. Kept so that a word found inside a
	// surface record can be CHECKED against a set we already know rather than
	// guessed at: a pointer that hits one of these is the texture link, and
	// nothing else in the structure will hit it by accident.
	const int kTexKnown = 512;
	void* g_pTexKnown[kTexKnown] = { nullptr };
	volatile LONG g_nTexKnown = 0;

	bool IsKnownTexture(uint32_t v)
	{
		const LONG n = g_nTexKnown < kTexKnown ? g_nTexKnown : kTexKnown;
		for (LONG i = 0; i < n; ++i)
			if ((uint32_t)(uintptr_t)g_pTexKnown[i] == v) return true;
		return false;
	}
	bool     g_bLastBlitFullScreen = false;
	volatile LONG g_nTintSkipped = 0;
	volatile LONG g_nRightHalfBlits = 0;
	volatile LONG g_nStereo2DBlits  = 0;	// drawn into both halves
	volatile LONG g_nCompositeBlits = 0;	// left alone: the eye copy
	volatile LONG g_nScreenSnaps    = 0;	// BlitFromScreen answered on the GPU
	int           g_bGpuEyeCopy     = 1;	// +StubGpuEyeCopy 0 reads it back as before
	volatile LONG g_nMarkerBlits    = 0;	// left alone: the frame marker
	// 1 while CInterfaceMgr has a folder up, published by the client every
	// frame. See R3D_PublishFolder2D and the note at R2D_SetMenu.
	volatile LONG g_bFolder2D       = 0;
	int           g_bFolder2DUse    = 0;	// +StubFolder2D, default OFF
	volatile LONG g_nFolder2DId    = 0;	// the folder id + 1 the client last sent; 999 = the splash or the loading screen
	// THE MENU ZOOM. Defaults are the identity - zoom 1, anchor 0 - so a build
	// with these switches absent draws exactly what it drew before.
	float         g_fMenuZoomV       = 1.0f;
	float         g_fMenuAnchorXV    = 0.0f;
	float         g_fMenuAnchorYV    = 0.0f;
	int  g_bStereo2D    = 1;
	volatile LONG g_nScreenReads = 0;
	uint32_t g_nModeW = 0, g_nModeH = 0;	// the engine's mode, for "is this full screen"

	// The six words the scene description points at, read straight out of
	// lithtech.exe. Their addresses are fixed - the exe has no relocation and
	// every run has shown 004B1590..A4 - but the base is taken at runtime
	// rather than hardcoded, so a rebased image would read nothing instead of
	// reading the wrong thing.
	void LogSixWords(const char* pszWhen)
	{
		const uintptr_t nAt = (uintptr_t)GetModuleHandleA(nullptr) + 0xB1590;
		char szLine[256]{};
		for (int w = 0; w < 6; ++w)
		{
			const void* p = (const void*)(nAt + w * 4);
			char szOne[32];
			sprintf_s(szOne, "%08X ",
				IsBadReadPtr(p, 4) ? 0xDEADBEEF : *(const uint32_t*)p);
			strcat_s(szLine, szOne);
		}
		Log("  %s: %s", pszWhen, szLine);
	}

	// ------------------------------------------------------------------
	// The real renderer's own 37 slots.
	//
	// Harvested by calling the real RenderDLLSetup on the engine's struct
	// BEFORE we write ours over the top - the same order the shim used, and
	// safe to CALL rather than tail-jump now that the convention is measured
	// as __cdecl.
	//
	// This is what makes a one-variable experiment possible: the stub can hand
	// any single slot back to the renderer that is known to work, and the
	// engine's reaction names the slot that matters.
	// ------------------------------------------------------------------

	void* g_pRealSlot[kSlots] = { nullptr };
	bool  g_bHaveReal = false;

	// ------------------------------------------------------------------
	// The 15 engine callbacks at offsets 0-56.
	//
	// These are the OTHER half of the struct: functions the engine provides
	// for the renderer to call. If the engine's willingness to keep using a
	// renderer depends on something the real Init tells it, this is the only
	// channel it can tell it through. Thunked with a tail jump, so no
	// assumption is made about their conventions - the same discipline the
	// exports use.
	// ------------------------------------------------------------------

	const int kCallbacks = 15;
	void*        g_pOrigCB[kCallbacks]  = { nullptr };
	volatile LONG g_nCBCalls[kCallbacks] = { 0 };
	volatile LONG g_bInInit = 0;

	// The whole conversation the real Init has with the engine, in order, with
	// arguments. Recorded only while Init is on the stack: the same callbacks
	// are used hundreds of times later and the interesting window is this one.
	const int kTrace = 1024;
	const int kArgs  = 6;
	struct CBCall { int nWhich; uint32_t nArg[kArgs]; };
	CBCall g_Trace[kTrace];
	volatile LONG g_nTrace = 0;

	void __cdecl OnCallback(int i, const uint32_t* pArgs)
	{
		if (i < 0 || i >= kCallbacks) return;
		InterlockedIncrement(&g_nCBCalls[i]);

		if (g_bInInit)
		{
			const LONG n = InterlockedIncrement(&g_nTrace) - 1;
			if (n < kTrace)
			{
				g_Trace[n].nWhich = i;
				for (int a = 0; a < kArgs; ++a)
					g_Trace[n].nArg[a] =
						(pArgs && !IsBadReadPtr(pArgs + a, 4)) ? pArgs[a] : 0;
			}
		}
	}

#define CB_THUNK(n)                                       \
	__declspec(naked) void CB##n()                        \
	{                                                     \
		__asm { pushad }                                  \
		__asm { pushfd }                                  \
		__asm { lea eax, [esp + 40] }                     \
		__asm { push eax }                                \
		__asm { push n }                                  \
		__asm { call OnCallback }                         \
		__asm { add esp, 8 }                              \
		__asm { popfd }                                   \
		__asm { popad }                                   \
		__asm { jmp dword ptr [g_pOrigCB + n * 4] }       \
	}

	CB_THUNK(0);  CB_THUNK(1);  CB_THUNK(2);  CB_THUNK(3);  CB_THUNK(4);
	CB_THUNK(5);  CB_THUNK(6);  CB_THUNK(7);  CB_THUNK(8);  CB_THUNK(9);
	CB_THUNK(10); CB_THUNK(11); CB_THUNK(12); CB_THUNK(13); CB_THUNK(14);

	void* const g_pCBThunk[kCallbacks] = {
		CB0, CB1, CB2,  CB3,  CB4,  CB5,  CB6, CB7,
		CB8, CB9, CB10, CB11, CB12, CB13, CB14,
	};

	// ------------------------------------------------------------------
	// The device.
	//
	// Measured on 2 September: the window handle is NOT in Init's argument.
	// 640 bytes were dumped from it and tested word by word with IsWindow();
	// the engine's own window - class "LithTech", the only visible top-level
	// window this process owns - appears nowhere in the struct. It does appear
	// 536 bytes further up, next to 800/600/32, but that is the CALLER's stack
	// frame, not our argument, and reading a neighbouring frame at a fixed
	// offset is not something to build on.
	//
	// So the window is found the way the OS itself will confirm it: enumerate
	// this process's top-level windows and take the one whose class is
	// "LithTech". If there is not EXACTLY one, no device is created and the
	// stub goes on drawing nothing - a wrong window is worse than no window.
	// ------------------------------------------------------------------

	ID3D11Device*           g_pDev  = nullptr;
	ID3D11DeviceContext*    g_pCtx  = nullptr;
	IDXGISwapChain*         g_pSwap = nullptr;
	ID3D11RenderTargetView* g_pRTV  = nullptr;
	HWND                    g_hWndGame = nullptr;
	volatile LONG           g_nPresents = 0;
	volatile LONG           g_nPresentFails = 0;

	HWND g_hFound = nullptr;
	int  g_nFound = 0;

	BOOL CALLBACK OnGameWindow(HWND hWnd, LPARAM)
	{
		DWORD nPid = 0;
		GetWindowThreadProcessId(hWnd, &nPid);
		if (nPid != GetCurrentProcessId()) return TRUE;
		char szClass[64]{};
		GetClassNameA(hWnd, szClass, sizeof(szClass) - 1);
		if (strcmp(szClass, "LithTech") != 0) return TRUE;
		++g_nFound;
		if (!g_hFound) g_hFound = hWnd;
		return TRUE;
	}

	HWND FindGameWindow()
	{
		g_hFound = nullptr; g_nFound = 0;
		EnumWindows(OnGameWindow, 0);
		if (g_nFound == 1)
		{
			Log("  window: HWND %08X, class LithTech, the only one this process owns",
				(unsigned)(uintptr_t)g_hFound);
			return g_hFound;
		}
		Log("  window: %d windows of class LithTech in this process - REFUSING to",
			g_nFound);
		Log("  guess which one. No device will be created; the stub draws nothing.");
		return nullptr;
	}

	// Paint the whole back buffer one colour and put it on the screen. This is
	// the entire visual output of the stub, and it is the gate: a window that
	// changes colour is a window our device owns.
	void PaintAndPresent(float r, float g, float b)
	{
		if (!g_pRTV || !g_pCtx || !g_pSwap) return;
		const float c[4] = { r, g, b, 1.0f };
		g_pCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
		g_pCtx->ClearRenderTargetView(g_pRTV, c);
		const HRESULT hr = g_pSwap->Present(0, 0);
		if (FAILED(hr))
		{
			if (InterlockedIncrement(&g_nPresentFails) <= 3)
				Log("  Present FAILED hr=%08X", (unsigned)hr);
			return;
		}
		InterlockedIncrement(&g_nPresents);
	}

	bool CreateDevice(HWND hWnd, UINT nW, UINT nH)
	{
		RECT rcClient{};
		GetClientRect(hWnd, &rcClient);
		Log("  device: engine asks for %ux%u; the window's client area is %dx%d",
			nW, nH, rcClient.right, rcClient.bottom);

		// A renderer sizes the window to the mode. The engine has already
		// written 800x600 into the RenderStruct, so this makes the window agree
		// with what the engine believes the screen is.
		RECT rc = { 0, 0, (LONG)nW, (LONG)nH };
		const LONG nStyle = GetWindowLongA(hWnd, GWL_STYLE);
		AdjustWindowRect(&rc, (DWORD)nStyle, FALSE);
		if (!SetWindowPos(hWnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
						  SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE))
			Log("  device: SetWindowPos failed (%lu) - carrying on at the old size",
				GetLastError());
		GetClientRect(hWnd, &rcClient);
		Log("  device: client area is now %dx%d", rcClient.right, rcClient.bottom);

		// The back buffer is the ENGINE's mode size, not the window's client
		// size. The engine's destination rectangles are in mode coordinates, so
		// this makes them map one-to-one onto back-buffer pixels, and DXGI
		// stretches to whatever the window happens to be. The window was
		// observed changing size mid-run; this removes that problem rather than
		// handling it.
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount       = 2;
		sd.BufferDesc.Width  = nW;
		sd.BufferDesc.Height = nH;
		sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow      = hWnd;
		sd.SampleDesc.Count  = 1;
		sd.Windowed          = TRUE;
		sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

		// D3D11 AND DXGI STAY LOADED FOR THE LIFE OF THE PROCESS. The engine
		// unloads this DLL on every focus change and loads it again, and Term
		// has always left the device alive (2-3 refs, up to 9 in the release
		// candidates) - so the unload dropped d3d11.dll's last load count while a
		// live device still had work queued inside it, and the next call into
		// it faulted at "d3d11.dll_unloaded" (an alt-tab in the headset,
		// 24 September). Pinned, the unload leaves both where they are; the
		// reload would have mapped them straight back anyway.
		{
			static bool s_bPinned = false;
			// OFF BY DEFAULT SINCE THE LEAK WAS FOUND. Term now releases every
			// object and the device dies (DEVICE 0), so the unload has nothing
			// left to pull out from under - and a pinned d3d11 kept a leaked
			// device alive into process exit, where one desk exit hung for six
			// minutes and another froze the whole machine (24 September).
			// +StubPinD3D 1 brings the pin back.
			if (!s_bPinned && CmdLineInt("StubPinD3D", 0) == 0)
			{
				s_bPinned = true;
				Log("  device: d3d11.dll and dxgi.dll not pinned (+StubPinD3D 1 to pin)");
			}
			if (!s_bPinned)
			{
				HMODULE h = nullptr;
				const BOOL b1 = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, "d3d11.dll", &h);
				const BOOL b2 = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN, "dxgi.dll", &h);
				s_bPinned = true;
				Log("  device: d3d11.dll %s, dxgi.dll %s for the life of the process",
					b1 ? "PINNED" : "NOT pinned", b2 ? "PINNED" : "NOT pinned");
			}
		}

		D3D_FEATURE_LEVEL nLevel = (D3D_FEATURE_LEVEL)0;
		const D3D_DRIVER_TYPE kTypes[2] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };

		for (int i = 0; i < 2; ++i)
		{
			const UINT nFlags = g_bD3DDebug ? D3D11_CREATE_DEVICE_DEBUG : 0;
			const HRESULT hr = D3D11CreateDeviceAndSwapChain(
				nullptr, kTypes[i], nullptr, nFlags, nullptr, 0, D3D11_SDK_VERSION,
				&sd, &g_pSwap, &g_pDev, &nLevel, &g_pCtx);
			Log("  device: D3D11CreateDeviceAndSwapChain(%s) hr=%08X",
				i == 0 ? "HARDWARE" : "WARP", (unsigned)hr);
			if (SUCCEEDED(hr)) break;
			g_pSwap = nullptr; g_pDev = nullptr; g_pCtx = nullptr;
		}
		if (!g_pDev || !g_pSwap) { Log("  device: NOT CREATED."); return false; }
		Log("  device: created, feature level %X", (unsigned)nLevel);

		ID3D11Texture2D* pBack = nullptr;
		HRESULT hr = g_pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack);
		Log("  device: GetBuffer hr=%08X", (unsigned)hr);
		if (SUCCEEDED(hr) && pBack)
		{
			hr = g_pDev->CreateRenderTargetView(pBack, nullptr, &g_pRTV);
			Log("  device: CreateRenderTargetView hr=%08X", (unsigned)hr);
			pBack->Release();
		}
		if (!g_pRTV) { Log("  device: no render target view."); return false; }
		// THE BASELINE for Term's per-stage counts: what the device, swap chain,
		// context and RTV alone hold, before any of our subsystems exist.
		{
			g_pDev->AddRef();
			Log("  device: refs %lu with only the swap chain, context and RTV made", g_pDev->Release());
		}

		// DXGI otherwise subclasses the engine's window for Alt+Enter and
		// resize handling. The engine owns that window; we are a guest in it.
		IDXGIDevice*  pDxgiDev = nullptr;
		IDXGIAdapter* pAdapter = nullptr;
		IDXGIFactory* pFactory = nullptr;
		if (SUCCEEDED(g_pDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgiDev)) &&
			SUCCEEDED(pDxgiDev->GetParent(__uuidof(IDXGIAdapter), (void**)&pAdapter)) &&
			SUCCEEDED(pAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&pFactory)))
		{
			pFactory->MakeWindowAssociation(
				hWnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
			Log("  device: DXGI told to keep its hands off the engine's window");
		}
		if (pFactory) pFactory->Release();
		if (pAdapter) pAdapter->Release();
		if (pDxgiDev) pDxgiDev->Release();

		// The 2D pipeline. Its target is the back buffer at the engine's mode
		// size, which is the coordinate space the blit rectangles use.
		SL_Create(g_pDev, g_pCtx, g_pSwap, &Log);
		StartWatchdog();
		if (!R3D_Create(g_pDev, g_pCtx, &Log))
			Log("  the 3D path did not come up; the world will stay black.");
		if (!R2D_Create(g_pDev, g_pCtx, &Log))
			Log("  device: the 2D path did not come up - blits will do nothing");
		R2D_SetTarget(g_pRTV, (int)nW, (int)nH);

		// Hand the finished frame to the host directly instead of letting it
		// photograph our window. Never fatal: if this does not come up the
		// host falls back to window capture, which is what it does today.
		g_nScreenW = nW; g_nScreenH = nH;
		if (g_bEyeShare && !ES_Create(g_pDev, g_pCtx, nW, nH, &Log))
			Log("  device: the shared eye texture did not come up -"
				" the host will fall back to window capture");
		R3D_SetTarget(g_pRTV, (int)nW, (int)nH);

		return true;
	}

	// First call to each slot is worth a line; the rest are worth a counter.
	// One line per frame would bury the header and produce a file too large to
	// read, which is the mistake the host log already documents.
	void Note(int nSlot)
	{
		if (InterlockedIncrement(&g_nCalls[nSlot]) == 1)
			Log("  first call: slot %2d (offset %3d)  %s",
				nSlot, kFirstSlotOffset + nSlot * 4, g_szName[nSlot]);
	}

	// ------------------------------------------------------------------
	// Surfaces.
	//
	// The client creates real surfaces - the frame marker's swatches, the
	// per-eye crosshair, the eye stash - and writes pixels into them. Returning
	// null would take the engine down a path it has no reason to survive, so
	// these are backed by actual memory even though nothing is ever displayed.
	// ------------------------------------------------------------------
	struct StubSurface
	{
		uint32_t nMagic;			// so a stray handle is caught rather than followed
		uint32_t nWidth, nHeight;
		int      nPitch;
		void*    pBits;
	};
	const uint32_t kSurfMagic = 0x53545542;		// 'STUB'

	volatile LONG g_nSurfacesLive = 0;
	volatile LONG g_nOddAlpha     = 0;		// blits whose alpha was not in 0..1

	// A registry of the surfaces WE handed out. The point is identification:
	// when a word inside a structure the engine passes us turns out to be one
	// of these, that word is a surface pointer and there is nothing to infer -
	// we allocated it, so we know. Same discipline as testing a word with
	// IsWindow() rather than reading it off a header.
	StubSurface* Check(void* h);			// defined below, beside the slots

	const int kMaxSurf = 512;
	struct SurfEntry { void* p; uint32_t w, h; };
	SurfEntry g_Surf[kMaxSurf];
	volatile LONG g_nSurfKnown = 0;

	void NoteSurface(void* p, uint32_t w, uint32_t h)
	{
		const LONG n = InterlockedIncrement(&g_nSurfKnown) - 1;
		if (n < kMaxSurf) { g_Surf[n].p = p; g_Surf[n].w = w; g_Surf[n].h = h; }
	}

	// What a raw dword is, if anything we can prove. Returns "" when nothing
	// is known - an honest blank beats a plausible label.
	const char* WhatIs(uint32_t v)
	{
		static char sz[96];
		sz[0] = 0;
		if (!v) return sz;

		const LONG n = (g_nSurfKnown < kMaxSurf) ? g_nSurfKnown : kMaxSurf;
		for (LONG i = 0; i < n; ++i)
			if ((uint32_t)(uintptr_t)g_Surf[i].p == v)
			{
				sprintf_s(sz, "SURFACE %ux%u", g_Surf[i].w, g_Surf[i].h);
				return sz;
			}

		const char* pszMod = Classify(v);
		if (strcmp(pszMod, "?") != 0 && strcmp(pszMod, "-") != 0 &&
			strcmp(pszMod, "small") != 0)
			sprintf_s(sz, "in %s", pszMod);
		return sz;
	}

	// A __cdecl function's arguments start one word past its return address.
	// That gives the raw stack whatever the declared signature happens to be,
	// which matters here because the signatures are what is being tested.
	#define RAW_ARGS() ((const uint32_t*)_AddressOfReturnAddress() + 1)

	// ------------------------------------------------------------------
	// BlitToScreen, the busiest call the engine makes: 27 times a frame, and
	// the entire main menu goes through it. Before it can be implemented its
	// argument has to be mapped, and the argument is built per call, so the
	// bytes have to be copied while the call is live - the same trap the scene
	// description dump fell into once already (docs/SCENEDESC.md).
	//
	// A ring of the last few calls rather than one snapshot: consecutive blits
	// draw different things, and a field that is constant across all of them is
	// telling you something different from one that varies.
	// ------------------------------------------------------------------
	const int kBlitRing  = 10;
	const int kBlitArgs  = 8;
	const int kBlitBytes = 80;
	struct BlitSnap
	{
		uint32_t nArg[kBlitArgs];
		BYTE     Body[kBlitBytes];
		bool     bBody;
		// +12 and +16 of the request are pointers to somewhere else on the
		// stack. A blit needs a source rectangle and a destination rectangle
		// and there are exactly two of them, so these are the candidates -
		// but which is which is a measurement, not a reading.
		uint32_t nRectA[6], nRectB[6];
		bool     bRectA, bRectB;
	};
	BlitSnap g_Blit[kBlitRing];
	volatile LONG g_nBlit = 0;

	void SnapBlit(const uint32_t* pArgs)
	{
		const LONG n = InterlockedIncrement(&g_nBlit) - 1;
		BlitSnap& s = g_Blit[n % kBlitRing];
		for (int a = 0; a < kBlitArgs; ++a)
			s.nArg[a] = (pArgs && !IsBadReadPtr(pArgs + a, 4)) ? pArgs[a] : 0;

		s.bBody = s.bRectA = s.bRectB = false;
		const void* p = (const void*)(uintptr_t)s.nArg[0];
		if (p && !IsBadReadPtr(p, kBlitBytes))
		{
			memcpy(s.Body, p, kBlitBytes);
			s.bBody = true;

			const void* pA = (const void*)(uintptr_t)*(const uint32_t*)(s.Body + 12);
			const void* pB = (const void*)(uintptr_t)*(const uint32_t*)(s.Body + 16);
			if (pA && !IsBadReadPtr(pA, sizeof(s.nRectA)))
			{ memcpy(s.nRectA, pA, sizeof(s.nRectA)); s.bRectA = true; }
			if (pB && !IsBadReadPtr(pB, sizeof(s.nRectB)))
			{ memcpy(s.nRectB, pB, sizeof(s.nRectB)); s.bRectB = true; }
		}
	}

	// ------------------------------------------------------------------
	// Write a surface out as a BMP.
	//
	// We told the engine the screen is XRGB8888 and it has been writing into
	// our surfaces ever since. Whether it believed us is not something to find
	// out after 250 lines of D3D11 have been written on top of the assumption:
	// four wrong answers in this project came from a statistic over pixels
	// nobody had opened. So open them.
	//
	// If the engine's pixels are the menu background in the right colours, the
	// format is right and the surfaces really do carry the image. If the
	// channels are swapped, that is visible in one glance and costs nothing.
	// ------------------------------------------------------------------
	void WriteSurfaceBMP(const StubSurface* p, const char* pszPath)
	{
		if (!p || !p->pBits || p->nWidth == 0 || p->nHeight == 0) return;

		const int nRow = ((int)p->nWidth * 3 + 3) & ~3;		// BMP rows are DWORD-aligned
		const uint32_t nPix = (uint32_t)nRow * p->nHeight;

		BITMAPFILEHEADER fh{};
		fh.bfType    = 0x4D42;								// 'BM'
		fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
		fh.bfSize    = fh.bfOffBits + nPix;

		BITMAPINFOHEADER ih{};
		ih.biSize     = sizeof(ih);
		ih.biWidth    = (LONG)p->nWidth;
		ih.biHeight   = (LONG)p->nHeight;					// positive = bottom-up
		ih.biPlanes   = 1;
		ih.biBitCount = 24;
		ih.biSizeImage = nPix;

		FILE* f = nullptr;
		fopen_s(&f, pszPath, "wb");
		if (!f) { Log("  could not write %s", pszPath); return; }
		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&ih, sizeof(ih), 1, f);

		BYTE* pRow = (BYTE*)calloc(nRow, 1);
		for (int y = (int)p->nHeight - 1; y >= 0; --y)		// bottom-up
		{
			const BYTE* pSrc = (const BYTE*)p->pBits + (size_t)y * p->nPitch;
			for (uint32_t x = 0; x < p->nWidth; ++x)
			{
				// XRGB8888 little-endian is B,G,R,X in memory - and BMP wants
				// B,G,R. If that is wrong the picture comes out with red and
				// blue swapped, which is unmistakable.
				pRow[x * 3 + 0] = pSrc[x * 4 + 0];
				pRow[x * 3 + 1] = pSrc[x * 4 + 1];
				pRow[x * 3 + 2] = pSrc[x * 4 + 2];
			}
			fwrite(pRow, nRow, 1, f);
		}
		free(pRow);
		fclose(f);
		Log("  wrote %s  (%ux%u)", pszPath, p->nWidth, p->nHeight);
	}

	char g_szDir[MAX_PATH]{};

	void DumpBiggestSurfaces(int nHowMany)
	{
		const LONG n = (g_nSurfKnown < kMaxSurf) ? g_nSurfKnown : kMaxSurf;
		Log("");
		Log("=== the %d largest surfaces the engine has filled ===", nHowMany);

		bool bDone[kMaxSurf] = { false };
		for (int k = 0; k < nHowMany; ++k)
		{
			LONG nBest = -1;
			uint32_t nArea = 0;
			for (LONG i = 0; i < n; ++i)
			{
				if (bDone[i]) continue;
				const StubSurface* p = Check(g_Surf[i].p);
				if (!p) continue;					// deleted since
				const uint32_t a = p->nWidth * p->nHeight;
				if (a > nArea) { nArea = a; nBest = i; }
			}
			if (nBest < 0) break;
			bDone[nBest] = true;

			char szPath[MAX_PATH];
			sprintf_s(szPath, "%slogs\\surface%d-%ux%u.bmp", g_szDir, k,
				g_Surf[nBest].w, g_Surf[nBest].h);
			WriteSurfaceBMP(Check(g_Surf[nBest].p), szPath);
		}
		Log("=== end surfaces ===");
	}

	void DumpBlits()
	{
		const LONG nTotal = g_nBlit;
		if (!nTotal) { Log("  no BlitToScreen calls to dump"); return; }

		Log("");
		Log("=== BlitToScreen: the last %d of %ld calls ===",
			(nTotal < kBlitRing) ? (int)nTotal : kBlitRing, nTotal);
		Log("  arg0 is the request; the rest is whatever the stack held, so a");
		Log("  column that is constant AND meaningful is a real argument.");

		const int nShow = (nTotal < kBlitRing) ? (int)nTotal : kBlitRing;
		for (int i = 0; i < nShow; ++i)
		{
			const LONG idx = nTotal - nShow + i;
			const BlitSnap& s = g_Blit[idx % kBlitRing];

			char szArgs[160]{};
			for (int a = 0; a < kBlitArgs; ++a)
			{
				char szOne[16];
				sprintf_s(szOne, "%08X ", s.nArg[a]);
				strcat_s(szArgs, szOne);
			}
			Log("");
			Log("  call %ld  args: %s", idx, szArgs);
			if (!s.bBody) { Log("    arg0 did not point at readable memory"); continue; }

			// The request is 24 bytes; past that is the caller's own stack and
			// it differs between call sites, which is how we know where it
			// ends. Printing 32 bytes shows the boundary rather than asserting
			// it.
			for (int w = 0; w < 8; ++w)
			{
				const uint32_t v = *(const uint32_t*)(s.Body + w * 4);
				float f; memcpy(&f, &v, 4);
				Log("    +%3d  %08X  %11d  %12.4f  %s", w * 4, v, (int)v, f, WhatIs(v));
			}
			if (s.bRectA)
				Log("    [+12] -> %d %d %d %d %d %d", (int)s.nRectA[0], (int)s.nRectA[1],
					(int)s.nRectA[2], (int)s.nRectA[3], (int)s.nRectA[4], (int)s.nRectA[5]);
			if (s.bRectB)
				Log("    [+16] -> %d %d %d %d %d %d", (int)s.nRectB[0], (int)s.nRectB[1],
					(int)s.nRectB[2], (int)s.nRectB[3], (int)s.nRectB[4], (int)s.nRectB[5]);
		}
		Log("=== end BlitToScreen ===");
	}

	StubSurface* Check(void* h)
	{
		if (!h || IsBadReadPtr(h, sizeof(StubSurface))) return nullptr;
		StubSurface* p = (StubSurface*)h;
		return (p->nMagic == kSurfMagic) ? p : nullptr;
	}

	// ------------------------------------------------------------------
	// State the engine can ask about. IsIn3D and IsInOptimized2D are queried
	// tens of thousands of times a run - measured - so they have to answer
	// honestly or the engine's own guards will misbehave.
	// ------------------------------------------------------------------
	volatile LONG g_bIn3D  = 0;
	volatile LONG g_bIn2D  = 0;
	uint32_t      g_n2DColour = 0xFFFFFFFF;
	uint32_t      g_n2DBlend  = 0;

	// The RenderStruct the engine handed us, so Init can fill in the fields the
	// engine reads back out of it.
	BYTE* g_pStruct = nullptr;

	// Set when this PROCESS_ATTACH is the engine reloading us mid-session
	// rather than a fresh launch. See the log-open note in DllMain.
	int g_nReload = 0;
	// This process's creation time, stamped into the log header so a later
	// process that inherits the same pid cannot be mistaken for a reload.
	unsigned long long g_nLogStart = 0;
}

// --------------------------------------------------------------------------
// The 37 slots. LTRESULT-style calls return 0 for LT_OK; LTBOOL-style calls
// return 1 for LTTRUE. Both were taken from the real renderer where they could
// be measured - Init returning 0 above all - and are the documented convention
// elsewhere.
// --------------------------------------------------------------------------

// The order the engine calls slots in, recorded for a short window once the
// world exists. Counts say how often a slot is called and say nothing about
// when - and "the world draws and then something clears it" is entirely a
// question of when.
namespace
{
	volatile LONG g_bOrder = 0;
	LONG g_nOrder = 0;
	int  g_aOrder[400];

	// A RING of the last 80, not the first 80. The first 80 calls after a level
	// load are all slot 2 - a texture-binding burst - which says nothing about
	// the steady-state frame. Slots 9 and 12 are excluded: they are the state
	// QUERIES, called tens of thousands of times a run, and they would fill the
	// window on their own.
	void NoteOrder(int n)
	{
		if (!g_bOrder || n == 9 || n == 12) return;
		const LONG i = InterlockedIncrement(&g_nOrder) - 1;
		g_aOrder[i % 400] = n;
	}
}

// THE LOCK IS TAKEN HERE, in the one macro every engine-facing entry point
// already begins with - see renlock.h for why. A guard object at function
// scope, so the lock is held for the whole call.
CRITICAL_SECTION g_csRender;
#define SLOT(n) RenderGuard _renderGuard_##n; do { Note(n); NoteOrder(n); } while (0)

static int   __cdecl s_Init(void* pInit)          { SLOT(0);
	Log("  Init: RenderStructInit at %p", pInit);

	// Where the engine called us FROM. Everything the engine decides about a
	// renderer, it decides in the instructions after this address - and
	// lithtech.exe is on disk and can be read. That is cheaper than inferring
	// the decision from the outside, and it cannot be wrong about what the
	// code does.
	{
		const uintptr_t nRet  = (uintptr_t)_ReturnAddress();
		const uintptr_t nBase = (uintptr_t)GetModuleHandleA(nullptr);
		Log("  Init: called from %08X = lithtech.exe + %06X",
			(unsigned)nRet, (unsigned)(nRet - nBase));
	}

	// The whole point of this build: find the window, by measurement.
	LogWindows("at Init");
	if (pInit) DumpInit(pInit, 160);

	// The cookie. Read out of lithtech.exe itself, not guessed:
	//
	//   0046251F  call dword ptr [0x4b43c4]        <- Init (RenderStruct + 108)
	//   00462525  add esp, 8
	//   00462528  test eax, eax
	//   0046252A  jne  0x462741                    <- non-zero return = failure
	//   00462530  cmp  dword ptr [esp + 0xd8], 0xd5d
	//   0046253B  jne  0x462741                    <- and THIS is the other half
	//
	// [esp+0xd8] is the RenderStructInit itself - the same address the engine
	// computed with `lea eax, [esp+0xdc]` one push earlier and passed to Init.
	// So returning 0 is only half the success signal: the renderer must also
	// stamp 0xD5D into the first dword of the struct it was handed. Failing
	// that, the engine takes the path at 0x462741, which returns error 0x33 and
	// never calls Term - exactly the silent, crashless stop that was observed.
	//
	// The engine zeroes this struct before filling it, so the field starts at 0
	// and nothing but the renderer can set it.
	const uint32_t kInitCookie = 0xD5D;
	if (pInit && !IsBadWritePtr(pInit, 4))
	{
		*(uint32_t*)pInit = kInitCookie;
		Log("  Init: stamped the cookie %03X into RenderStructInit[0]", kInitCookie);
	}

	uint32_t nW = 800, nH = 600;
	if (g_pStruct)
	{
		*(uint32_t*)(g_pStruct + 68) = 1;			// m_bInitted, measured offset
		Log("  Init: m_bInitted set; width %u height %u as the engine left them",
			*(uint32_t*)(g_pStruct + 60), *(uint32_t*)(g_pStruct + 64));
		const uint32_t w = *(uint32_t*)(g_pStruct + 60);
		const uint32_t h = *(uint32_t*)(g_pStruct + 64);
		if (w >= 320 && w <= 8192 && h >= 200 && h <= 8192) { nW = w; nH = h; }
		g_nModeW = nW; g_nModeH = nH;
	}

	// The one-variable experiment. The engine made no renderer call at all
	// after our Init returned 0 - not even GetScreenFormat, which a working
	// run calls second. Either the return value is not the whole signal, or
	// the engine reacts to something the real Init DOES. Handing this one slot
	// back to the renderer that is known to work separates those two, and
	// nothing else about the stub changes.
	if (g_bDelegateInit && g_bHaveReal && g_pRealSlot[0])
	{
		Log("  Init: DELEGATING to the real d3d.ren's Init at %p", g_pRealSlot[0]);
		g_bInInit = 1;
		const int nReal = ((int(__cdecl*)(void*))g_pRealSlot[0])(pInit);
		g_bInInit = 0;
		Log("  Init: the real Init returned %d", nReal);

		// What the real Init told the engine, in order. Our Init has to say
		// whatever of this the engine is actually waiting for, and the only way
		// to know which is to see all of it first.
		const LONG nShow = (g_nTrace < kTrace) ? g_nTrace : kTrace;

		Log("");
		Log("=== the real Init's %ld calls back into the engine ===", g_nTrace);
		LONG nPer[kCallbacks] = { 0 };
		for (LONG t = 0; t < nShow; ++t) ++nPer[g_Trace[t].nWhich];
		for (int c = 0; c < kCallbacks; ++c)
			if (nPer[c]) Log("  cb %2d (offset %2d): %ld calls", c, c * 4, nPer[c]);

		// 4 and 6 are the console-variable pair - hundreds of calls reading
		// LMAnimStatic, DrawWorldModels, PSSrcBlend and the rest of the
		// renderer's own settings. They are noise for this question. Everything
		// else the real Init says to the engine is printed in full.
		Log("");
		Log("  every call that is NOT the console-variable pair (4, 6):");
		Log("  #     cb  engine fn   arg0     arg1     arg2     arg3     arg4     arg5");
		for (LONG t = 0; t < nShow; ++t)
		{
			const CBCall& c = g_Trace[t];
			if (c.nWhich == 4 || c.nWhich == 6) continue;
			char szArgs[160]{};
			for (int a = 0; a < kArgs; ++a)
			{
				char szOne[16];
				sprintf_s(szOne, "%08X ", c.nArg[a]);
				strcat_s(szArgs, szOne);
			}
			Log("  %4ld  %2d  %08X  %s", t, c.nWhich,
				(unsigned)(uintptr_t)g_pOrigCB[c.nWhich], szArgs);
			for (int a = 0; a < kArgs; ++a)
			{
				const char* s = (const char*)(uintptr_t)c.nArg[a];
				if (!s || IsBadReadPtr(s, 4)) continue;
				int nLen = 0;
				while (nLen < 95 && s[nLen] >= 32 && s[nLen] < 127) ++nLen;
				if (nLen >= 3 && s[nLen] == 0)
					Log("          arg%d = \"%.95s\"", a, s);
			}
		}

		// And the last few whatever they are: if the engine is waiting to be
		// told something, being told it is the last thing Init does.
		Log("");
		Log("  the last 12 calls of any kind:");
		for (LONG t = (nShow > 12 ? nShow - 12 : 0); t < nShow; ++t)
		{
			const CBCall& c = g_Trace[t];
			const char* s = (const char*)(uintptr_t)c.nArg[0];
			char szName[100]{};
			if (s && !IsBadReadPtr(s, 4))
			{
				int nLen = 0;
				while (nLen < 95 && s[nLen] >= 32 && s[nLen] < 127) ++nLen;
				if (nLen >= 3 && s[nLen] == 0) sprintf_s(szName, " \"%.95s\"", s);
			}
			Log("  %4ld  cb %2d  %08X %08X %08X%s", t, c.nWhich,
				c.nArg[0], c.nArg[1], c.nArg[2], szName);
		}
		Log("=== end Init's calls ===");
		return nReal;
	}

	// The device. Everything above this line is measurement; this is the first
	// thing the stub does that a renderer does.
	if (!g_bWantDevice) Log("  Init: StubDevice 0 - no device will be created");
	g_hWndGame = g_bWantDevice ? FindGameWindow() : nullptr;
	if (g_hWndGame && CreateDevice(g_hWndGame, nW, nH))
	{
		// BLACK, and it used to be magenta.
		//
		// The magenta was the Phase 1 gate: a window that turned magenta and
		// stayed magenta meant the device worked and the engine had stopped
		// calling us, and cyan meant it was driving Clear. That question was
		// answered months ago.
		//
		// It is black now because the player reports magenta spots on the
		// main menu in the headset, and this one-off paint is the ONLY magenta
		// anything in this project produces - grepped, renderer and host. It is
		// presented exactly once, before the engine's first Clear, so it can
		// only survive into a menu if something copies that first frame into a
		// buffer it never fully overwrites again.
		//
		// That makes this a measurement rather than a cosmetic change: if
		// magenta still appears, it is not coming from here, and where it IS
		// coming from becomes the question.
		PaintAndPresent(0.0f, 0.0f, 0.0f);
		Log("  Init: painted black and presented (%ld presents so far)."
			" It was magenta until 3 September; see the note here.", g_nPresents);
	}

	return 0; }										// measured: success is 0

static int   __cdecl s_Term(void)                 { SLOT(1);
	StopWatchdog();
	StopReportThread();
	// The engine now reaches this - it did not before, because a renderer it
	// has rejected is never torn down. Release in reverse order of creation.
	Log("  Term: %ld presents over the run, %ld failed", g_nPresents, g_nPresentFails);
	// UNBIND EVERYTHING BEFORE ANYTHING IS RELEASED - and note the ORDER.
	//
	// The D3D11 immediate context holds a reference to every object still
	// BOUND to the pipeline - render targets, depth stencil, shaders, vertex
	// and constant buffers, samplers, shader resource views. Dropping our own
	// reference to a bound resource does not destroy it, because the context
	// still has one; and while those live, the device cannot die either.
	//
	// This teardown had neither call. With 486 references outstanding on the
	// device at Term on 11 September, and about 120 MB of a 32-bit address
	// space lost per renderer reload, that is the shape of the remaining
	// problem: ClearState unbinds, Flush lets the deferred destruction the
	// runtime has queued actually happen, and only then is a Release the last
	// one.
	//
	// PUT THIS FIRST. The first attempt ran it AFTER ES_Destroy, R2D_Destroy,
	// SL_Destroy and R3D_Destroy had already released their resources, and
	// the game then died on the very next focus loss - 0xC0000005 after Term,
	// before the reload could finish. Unbinding has to happen while the
	// things being unbound still exist.
	if (g_pCtx) { g_pCtx->ClearState(); g_pCtx->Flush(); }
	// THE DEVICE'S COUNT AFTER EACH STAGE. Every live object made from the
	// device holds a reference on it, so the stage whose teardown leaves the
	// count higher than its share is the one leaking. The debug layer that
	// would name the survivors is not installed on the machines that matter,
	// and asking for it makes device creation fail (887A002D).
	auto DevRefs = []() -> unsigned long
	{
		if (!g_pDev) return 0;
		g_pDev->AddRef();
		return g_pDev->Release();
	};
	const unsigned long nR0 = DevRefs();
	ES_Destroy();
	const unsigned long nR1 = DevRefs();
	R2D_Destroy();
	const unsigned long nR2 = DevRefs();
	SL_Destroy();
	const unsigned long nR3 = DevRefs();
	R3D_Destroy();
	const unsigned long nR4 = DevRefs();
	Log("  Term: device refs %lu at start -> %lu after eye share -> %lu after 2D -> %lu after sprites"
		" -> %lu after 3D (the RTV, swap chain, context and our own account for the rest)",
		nR0, nR1, nR2, nR3, nR4);

	// DO NOT ADD A SECOND ClearState()/Flush() HERE. Tried 11 September, and
	// it crashes the game exactly the way MOVING the first one did.
	//
	// The reasoning was good and the measurement that motivated it is real:
	// bracketing each teardown with GlobalMemoryStatusEx gives FREED = +0.0 MB,
	// min 0, max 0, across 83 teardowns - the four Destroy calls above release
	// hundreds of resources and not one byte of address space comes back. D3D11
	// destruction is deferred, so the obvious read is that nothing is ever
	// flushed after those releases and everything stays mapped.
	//
	// It is still wrong. A second flush here died on 0xC0000005 in all three
	// missions of an identical chain arm, within 17 to 26 seconds against 300 s
	// baselines:
	//
	//     M01S01  26.1 s  GAME EXITED (exit -1073741819)
	//     M02S01  16.7 s  GAME EXITED (exit -1073741819)
	//     M03S01  16.8 s  GAME EXITED (exit -1073741819)
	//
	// ClearState touches state the destroys above have already torn down. The
	// note on the first flush says unbinding has to happen while the things
	// being unbound still exist, and that applies to a second call just as much
	// as to a moved one.
	//
	// AND FREED = 0 IS NOT THE EVIDENCE IT LOOKS LIKE. Two confounds, either of
	// which alone explains it: the CRT does not return freed heap back to the
	// OS, so a destructor cannot move this number at all; and D3D deferred
	// destruction completes when the GPU is done, which is after this function
	// has returned and the DLL has gone. Address space really does come back
	// later - one baseline mission measured -12.3 MB per reload, space being
	// RETURNED across reloads. Whatever the remaining 2 device references are,
	// this is not the instrument that will find them.

	// WHAT IS STILL HOLDING THE DEVICE? Release() returns the new reference
	// count, and it is the only free answer to "did this actually go away".
	//
	// The engine unloads and reloads this DLL on every focus loss and gain,
	// and each reload costs about 120 MB of a 32-bit address space - measured
	// over six missions, 110 to 131 MB every time, linear, until the world
	// build throws std::bad_alloc at 96% of the ceiling. Releasing the objects
	// the teardown had forgotten changed that by nothing at all, which points
	// at the device itself surviving: if one resource is still referenced the
	// device cannot be destroyed, and everything it owns stays mapped.
	//
	// A non-zero count on the last line below is the whole diagnosis.
	{
		// NAME WHAT IS STILL HOLDING THE DEVICE, rather than guessing at it.
		//
		// The count below has said DEVICE 2 on every teardown ever logged, and
		// an audit of all 78 file-scope COM pointers in this renderer found
		// every owned one released and every borrowed one a non-owning alias.
		// Guessing has run out. ReportLiveDeviceObjects prints each survivor
		// with its type and refcount into the debugger output, which is the
		// only thing that answers "which two".
		//
		// Needs +StubD3DDebug 1 AND the Windows "Graphics Tools" optional
		// feature; without the layer the QueryInterface simply fails and this
		// says so rather than pretending.
		if (g_bD3DDebug && g_pDev)
		{
			ID3D11Debug* pDbg = nullptr;
			if (SUCCEEDED(g_pDev->QueryInterface(__uuidof(ID3D11Debug),
												 (void**)&pDbg)) && pDbg)
			{
				// AND INTO THIS FILE, through the device's own message queue:
				// nobody has a debugger attached to a headset session, and the
				// survivors have now cost an alt-tab crash and, with d3d11
				// pinned, a machine that froze while the game exited
				// (24 September).
				ID3D11InfoQueue* pQ = nullptr;
				if (FAILED(g_pDev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&pQ))) pQ = nullptr;
				if (pQ) { pQ->ClearStoredMessages(); pQ->SetMessageCountLimit((UINT64)-1); }
				Log("  D3D DEBUG: live objects (also in the debugger output):");
				pDbg->ReportLiveDeviceObjects(D3D11_RLDO_DETAIL);
				pDbg->Release();
				if (pQ)
				{
					const UINT64 nMsgs = pQ->GetNumStoredMessages();
					for (UINT64 m = 0; m < nMsgs && m < 400; ++m)
					{
						SIZE_T nLen = 0;
						if (FAILED(pQ->GetMessage(m, nullptr, &nLen)) || !nLen) continue;
						D3D11_MESSAGE* pMsg = (D3D11_MESSAGE*)malloc(nLen);
						if (!pMsg) break;
						if (SUCCEEDED(pQ->GetMessage(m, pMsg, &nLen)) && pMsg->pDescription)
							Log("    LIVE: %.*s", (int)pMsg->DescriptionByteLength, pMsg->pDescription);
						free(pMsg);
					}
					Log("  D3D DEBUG: %u messages read from the info queue", (unsigned)nMsgs);
					pQ->Release();
				}
			}
			else
			{
				Log("  D3D DEBUG: asked for, but the debug layer is not"
					" available - install the Graphics Tools optional feature");
			}
		}

		unsigned long nRTV = 0, nSwap = 0, nCtx = 0, nDev = 0;
		unsigned long nD1 = 0, nD2 = 0, nD3 = 0;
		auto DevRefs2 = []() -> unsigned long
		{ if (!g_pDev) return 0; g_pDev->AddRef(); return g_pDev->Release(); };
		if (g_pRTV)  { nRTV  = g_pRTV->Release();  g_pRTV  = nullptr; }
		nD1 = DevRefs2();
		if (g_pSwap) { nSwap = g_pSwap->Release(); g_pSwap = nullptr; }
		nD2 = DevRefs2();
		if (g_pCtx)  { nCtx  = g_pCtx->Release();  g_pCtx  = nullptr; }
		nD3 = DevRefs2();
		Log("  Term: device refs %lu after the RTV, %lu after the swap chain, %lu after the context",
			nD1, nD2, nD3);
		if (g_pDev)  { nDev  = g_pDev->Release();  g_pDev  = nullptr; }
		Log("  Term: refs left after release - rtv %lu, swapchain %lu,"
			" context %lu, DEVICE %lu%s",
			nRTV, nSwap, nCtx, nDev,
			nDev ? "   <- the device SURVIVED: something still holds a resource"
				   " made from it, and everything that device owns stays mapped"
				 : "");

		// PAST THE DEVICE, which is the only place the big number can appear.
		// R3D_Destroy's own "AFTER" line fires before this block runs, so it
		// can never see what releasing the device gives back - and if the
		// device is what is pinning everything, this is the line that will say
		// so. Same call GlobalMemoryStatusEx makes in render3d.cpp; duplicated
		// rather than exported because it is four lines and one of them is the
		// units.
		MEMORYSTATUSEX ms{};
		ms.dwLength = sizeof ms;
		if (GlobalMemoryStatusEx(&ms))
		{
			const double kMB = 1.0 / (1024.0 * 1024.0);
			Log("  MEMORY (renderer teardown - PAST DEVICE): %.0f MB of address"
				" space free of %.0f MB total - %.0f MB in use, %.0f%% of the"
				" ceiling",
				(double)ms.ullAvailVirtual * kMB,
				(double)ms.ullTotalVirtual * kMB,
				(double)(ms.ullTotalVirtual - ms.ullAvailVirtual) * kMB,
				ms.ullTotalVirtual
					? 100.0 * (double)(ms.ullTotalVirtual - ms.ullAvailVirtual)
					  / (double)ms.ullTotalVirtual
					: 0.0);
		}
	}
	return 0; }
// The engine hands the renderer its textures through here, so this is where
// texturing has to start. The real one (d3d.ren + 0x39FE0) walks a chain at
// [pTex + 0xC] linked by +0x30, bracketed by engine callbacks 2 and 3 - which
// were both unidentified until now.
//
// What the argument IS, though, is a measurement, not a reading: the same dump
// that named "VisBSP" and "Door20" out of anonymous structures.
static void LogWordsAndStrings(const char* pszWhat, uint32_t p, int nBytes);

static int   __cdecl s_BindTexture(void* pTex, int nFlag)
{
	SLOT(2);
	// Distinct textures only. BindTexture fires 20 000 times a run because the
	// engine rebinds the same few every frame, and a table of the first 512
	// calls would hold a handful of textures repeated.
	if (pTex && !IsKnownTexture((uint32_t)(uintptr_t)pTex))
	{
		const LONG i = InterlockedIncrement(&g_nTexKnown) - 1;
		if (i < kTexKnown) g_pTexKnown[i] = pTex;
		else InterlockedDecrement(&g_nTexKnown);
	}

	// Retry until the texture actually EXISTS, not until it has been seen.
	// The engine binds a texture before its data is resident - the first bind
	// can carry a mip with pitch 0 - and giving up after one attempt left 176
	// textures created and 25174 of 26904 polygons white.
	// Hand the renderer the same callback, so it can fetch a model skin the
	// engine never binds rather than falling back to the wrong slot.
	if (g_pOrigCB[2]) R3D_SetTexDataFn(g_pOrigCB[2]);

	if (pTex && !R3D_HasTexture((uint32_t)(uintptr_t)pTex))
	{
		const LONG i = g_nTexKnown;

		// Take our copy here, where the real renderer takes its own. Only on
		// the first bind of each texture: the engine rebinds the same few
		// thousands of times a run, and callback 2 is not free.
		if (g_pOrigCB[2])
		{
			void* pData = ((void*(__cdecl*)(void*))g_pOrigCB[2])(pTex);
			if (pData)
				R3D_NoteTexture((uint32_t)(uintptr_t)pTex, (uint32_t)(uintptr_t)pData);

			// 176 D3D11 textures were created out of far more bound, and the
			// creation rejects anything whose mip pitch is not width*4. So
			// what ARE the others? Print the format field and the pitch ratio
			// for the first 30 distinct textures; a 2000-era game has more
			// than one texture format and the ratio names each of them.
			if (pData && i < 30 && !IsBadReadPtr(pData, 0xE0))
			{
				const uint8_t* d = (const uint8_t*)pData;
				const uint32_t w = *(const uint16_t*)(d + 0x10);
				const uint32_t h = *(const uint16_t*)(d + 0x12);
				const uint8_t* m = d + 0xCC;
				const uint32_t mw = *(const uint32_t*)(m + 0);
				const uint32_t mp = *(const uint32_t*)(m + 12);
				// How many bytes a mip occupies, from the gap to the next
				// one. That names the format without decoding any flag:
				// 4 bytes per pixel is BGRA, 1 is paletted, 0.5 is DXT1 and
				// 1.0-with-blocks is DXT3/5.
				const uint32_t p0 = *(const uint32_t*)(m + 8);
				const uint32_t p1 = *(const uint32_t*)(m + 24 + 8);
				const double fBpp = (p1 > p0 && w && h)
					? (double)(p1 - p0) / ((double)w * h) : 0.0;
				Log("  tex %2ld: %4ux%-4u flags %08X  pitch %u  mip0..mip1 gap"
					" %u = %.3f bytes/pixel  %s",
					i, w, h, *(const uint32_t*)(d + 0x18), mp,
					(p1 > p0) ? (p1 - p0) : 0, fBpp,
					(mw == w && mp == mw * 4) ? "accepted" : "REJECTED");
			}
			if (g_pOrigCB[3]) ((void(__cdecl*)(void*))g_pOrigCB[3])(pTex);
		}
	}
	// Width and height, checked across many textures rather than read off one.
	//
	// The data object's +0x10 reads as 256 x 128 in two uint16s for the first
	// texture sampled. Every texture in a 2000-era game has power-of-two
	// dimensions, so that is a property the whole set must have - and a wrong
	// offset or a wrong field width will not produce 40 powers of two in a row.
	if (g_bTexDump && g_nCalls[2] <= 40 && pTex && !IsBadReadPtr(pTex, 0x60)
		&& g_pOrigCB[2])
	{
		void* pData = ((void*(__cdecl*)(void*))g_pOrigCB[2])(pTex);
		if (pData && !IsBadReadPtr(pData, 0x60))
		{
			const uint8_t* d = (const uint8_t*)pData;
			const uint32_t w = *(const uint16_t*)(d + 0x10);
			const uint32_t h = *(const uint16_t*)(d + 0x12);
			const bool bPow2 = w && h && !(w & (w - 1)) && !(h & (h - 1))
							&& w <= 2048 && h <= 2048;
			if (bPow2) InterlockedIncrement(&g_nTexPow2);
			InterlockedIncrement(&g_nTexSeen);
			// Where are the pixels? The object says it has 4 mips, and mip k
			// must measure (w >> k) by (h >> k). So walk the object looking
			// for a stride at which those halving dimensions appear: that is
			// a pattern four levels deep in two fields at once, which random
			// memory does not produce.
			if (!g_bMipFound && w >= 128 && h >= 128)
			{
				for (int nPass = 0; nPass < 2 && !g_bMipFound; ++nPass)
				for (uint32_t off = 0x14; off + 0x100 < 0x400 && !g_bMipFound; off += 4)
				{
					for (uint32_t stride = 8; stride <= 64 && !g_bMipFound; stride += 4)
					{
						// Both widths, because a uint16 search alone found
						// nothing and the field could as easily be a uint32.
						int nGood = 0;
						const bool b32 = (nPass != 0);
						for (uint32_t k = 0; k < 4; ++k)
						{
							const uint32_t at = off + k * stride;
							if (IsBadReadPtr(d + at, 8)) break;
							const uint32_t mw = b32 ? *(const uint32_t*)(d + at)
													: *(const uint16_t*)(d + at);
							const uint32_t mh = b32 ? *(const uint32_t*)(d + at + 4)
													: *(const uint16_t*)(d + at + 2);
							if (mw == (w >> k) && mh == (h >> k)) ++nGood; else break;
						}
						if (nGood == 4)
						{
							g_bMipFound = 1;
							Log("");
							Log("  THE MIP ARRAY: offset %u, stride %u - four levels"
								" of (w>>k, h>>k) in a row", off, stride);
							for (uint32_t k = 0; k < 4; ++k)
							{
								const uint32_t at = off + k * stride;
								Log("    mip %u at +%03X: %ux%u", k, at,
									*(const uint16_t*)(d + at),
									*(const uint16_t*)(d + at + 2));
								LogWordsAndStrings("      ",
									(uint32_t)(uintptr_t)(d + at), stride);
							}
						}
					}
				}
			}

			// Write mip 0 out and look at it.
			//
			// The mip record is {uint32 w, uint32 h, pixels, pitch} at
			// texdata + 0xCC, stride 24, and pitch is width * 4 at every
			// level - which is what says the pixels are 32-bit. All of that
			// is inference from numbers, and this project has four wrong
			// answers on record that came from a statistic over pixels nobody
			// opened. So open them: if this is a NOLF wall texture in the
			// right colours, every claim above is right at once.
			if (g_bTexWritten < 4 && w >= 128 && h >= 128)
			{
				const uint8_t* m = d + 0xCC;
				const uint32_t mw = *(const uint32_t*)(m + 0);
				const uint32_t mh = *(const uint32_t*)(m + 4);
				const uint32_t pp = *(const uint32_t*)(m + 8);
				const uint32_t mp = *(const uint32_t*)(m + 12);
				if (mw == w && mh == h && mp == mw * 4
					&& pp && !IsBadReadPtr((const void*)(uintptr_t)pp, mp * mh))
				{
					const int nIdx = g_bTexWritten++;
					StubSurface t{};
					t.pBits  = (void*)(uintptr_t)pp;
					t.nWidth = mw; t.nHeight = mh; t.nPitch = mp;
					char szPath[MAX_PATH];
					// A forward slash on purpose. This line was written through
					// a scripted edit and arrived with ONE backslash, which C
					// reads as a tab - the escaping trap this project has a
					// rule about, through a layer that was not expected to
					// have it. Windows takes '/' in a path, so there is
					// nothing here to escape wrongly.
					sprintf_s(szPath, "%slogs/texture%d-%ux%u.bmp", g_szDir, nIdx, mw, mh);
					WriteSurfaceBMP(&t, szPath);
					Log("  wrote mip 0 to %s (%ux%u, pitch %u = width*%u)",
						szPath, mw, mh, mp, mp / mw);
				}
			}

			// And the object itself, wide enough to read by eye if the
			// search finds nothing.
			if (!g_bWideDumped && w >= 256 && h >= 256)
			{
				g_bWideDumped = 1;
				Log("");
				Log("  a %ux%u texture's data object, %d mips claimed:", w, h,
					*(const uint32_t*)(d + 0x14));
				LogWordsAndStrings("  td", (uint32_t)(uintptr_t)d, 0x140);
			}

			if (g_nCalls[2] <= 10)
				Log("  texture %2ld: %ux%u %s  [+14]=%u [+18]=%u [+2C]=%08X"
					" [+30]=%u [+4C]=%08X",
					g_nCalls[2], w, h, bPow2 ? "" : "<- NOT a power of two",
					*(const uint32_t*)(d + 0x14), *(const uint32_t*)(d + 0x18),
					*(const uint32_t*)(d + 0x2C), *(const uint32_t*)(d + 0x30),
					*(const uint32_t*)(d + 0x4C));
			// RELEASE ONLY WHAT WAS ACQUIRED. This call used to sit outside the
			// `if (pData ...)` above, so a callback 2 that returned nothing - it
			// loads on demand and can fail - was still answered with a callback 3.
			// An unbalanced release on a refcounted texture frees something the
			// engine still holds, and the engine then calls through it. That is
			// the shape of the 5 September crash: execution at 03326916, an
			// address in no module at all, reading FFFFFFFF, immediately after
			// this diagnostic's own acquire/release pair.
			if (g_pOrigCB[3]) ((void(__cdecl*)(void*))g_pOrigCB[3])(pTex);
		}
	}

	if (g_bTexDump && g_nCalls[2] == 30 && pTex && !IsBadReadPtr(pTex, 0x60))
	{
		Log("");
		Log("=== BindTexture, call 30: the engine's texture object at %p"
			" (flag %d) ===", pTex, nFlag);
		LogWordsAndStrings("tex ", (uint32_t)(uintptr_t)pTex, 0x60);
		const uint32_t nChain = *(const uint32_t*)((const char*)pTex + 0xC);
		Log("  the chain the real renderer walks, [pTex+0xC] = %08X"
			"  (null because nothing of ours ever created it)", nChain);

		// Engine callback 2 is how the real renderer reaches a texture's data:
		// lithtech.exe + 0x62270 returns [pTex + 8], loading it first if that
		// is null. Calling it is safe here because this is exactly where the
		// real renderer calls it from.
		//
		// The convention is measured, not assumed: that function ends in a
		// plain `ret`, so it is __cdecl and the caller cleans. Getting this
		// wrong would corrupt the stack, which is why it was read rather than
		// tried.
		if (g_pOrigCB[2])
		{
			void* pData = ((void*(__cdecl*)(void*))g_pOrigCB[2])(pTex);
			Log("  engine callback 2 returned %p", pData);
			if (pData && !IsBadReadPtr(pData, 0x60))
				LogWordsAndStrings("data", (uint32_t)(uintptr_t)pData, 0x60);
			if (g_pOrigCB[3])
			{
				((void(__cdecl*)(void*))g_pOrigCB[3])(pTex);
				Log("  engine callback 3 called to release it");
			}
		}
		Log("=== end BindTexture ===");
	}
	return 0;
}
static int   __cdecl s_UnbindTexture(void*)       { SLOT(3);  return 0; }
// ---------------------------------------------------------------------------
// RebindLightmaps - the slot that blocks a level load.
//
// Bisected on 2 September: with every slot ours the world never finishes
// loading, and handing back THIS ONE SLOT and nothing else makes it load. Not
// slot 2, not slot 3, not any range that excludes it.
//
// So the engine's loader waits on something this call does, and our version
// does nothing. `+StubDelegate4Trace 1` calls the real one and records both
// channels it could be using: what it says back to the engine through the 15
// callbacks, and what it writes into its own argument.
// ---------------------------------------------------------------------------
static void LogWorldFromHandle(uint32_t h);

static int   __cdecl s_RebindLightmaps(void* a, void* b, void* c, void* d)
{
	SLOT(4);

	// A WORLD IS BEING LOADED, and this is the only place the renderer is
	// told. Every cached texture is keyed on an engine pointer the allocator
	// is about to hand back for a different picture - see R3D_WorldLoaded.
	// Costs nothing per frame: this runs once per load.
	R3D_WorldLoaded();

	// This is the door to the world, and it is 30 instructions long. The real
	// one (d3d.ren + 0x33160) does exactly this:
	//
	//     ebx = alloc(0x10)          a 16-byte object, the renderer's own
	//     eax = *(void**)pArg        the world, out of the argument's FIRST dword
	//     [ebx + 0xC] = 0xFFFF       the frame stamp, uint16
	//     [ebx + 8]   = eax          the world
	//     return ebx
	//
	// and the engine hands that return value straight back as word 13 of every
	// scene description. Measured, not inferred: a run with
	// +StubRebindRet 305419896 produced `word 13 +52 12345678` in the scene
	// description. The engine is a courier for a pointer the RENDERER owns.
	//
	// So the world does not arrive through the scene description at all. It
	// arrives here, once per level, in the first dword of this argument - and
	// a renderer that returns a constant, as ours did, is handed its own
	// constant back every frame and can never reach the world. That is why
	// returning 1 made the level load and still drew nothing.
	//
	// 0xFFFF for the stamp is the real one's value and is deliberate: the
	// counter wraps on first use, which is what triggers the full reset walk
	// over every surface. Starting at 0 would skip it.
	if (!g_bTrace4 || !g_bHaveReal || !g_pRealSlot[4])
	{
		if (!g_bRebindReal || !a || IsBadReadPtr(a, 4)) return g_nRebindRet;

		LMHandle* pH = (LMHandle*)calloc(1, sizeof(LMHandle));
		if (!pH) return g_nRebindRet;
		pH->pWorld = *(uint32_t*)a;
		pH->nStamp = 0xFFFF;
		g_pWorld   = pH->pWorld;
		g_nOrder = 0; InterlockedExchange(&g_bOrder, 1);

		Log("");
		Log("RebindLightmaps: argument %p, its first dword is %08X  %s",
			a, pH->pWorld, Classify(pH->pWorld));
		Log("  handing back our own 16-byte handle %p; the engine should give", pH);
		Log("  it to us again as scene word 13.");
		LogWorldFromHandle((uint32_t)(uintptr_t)pH);
		return (int)(uintptr_t)pH;
	}

	Log("");
	Log("=== RebindLightmaps: args %p %p %p %p ===", a, b, c, d);

	const int kSnap = 256;
	BYTE Before[kSnap], After[kSnap];
	const bool bSnap = a && !IsBadReadPtr(a, kSnap);
	if (bSnap) memcpy(Before, a, kSnap);

	g_nTrace   = 0;
	g_bInInit  = 1;						// the same gate the callback trace uses
	const int r = ((int(__cdecl*)(void*, void*, void*, void*))g_pRealSlot[4])(a, b, c, d);
	g_bInInit  = 0;

	Log("  the real RebindLightmaps returned %d (0x%08X)", r, (unsigned)r);

	if (bSnap && !IsBadReadPtr(a, kSnap))
	{
		memcpy(After, a, kSnap);
		int nChanged = 0;
		Log("  what it wrote into its own argument:");
		for (int i = 0; i < kSnap / 4; ++i)
		{
			const uint32_t x = *(const uint32_t*)(Before + i * 4);
			const uint32_t y = *(const uint32_t*)(After  + i * 4);
			if (x == y) continue;
			float f; memcpy(&f, &y, 4);
			Log("    +%3d  %08X -> %08X  %11d  %12.3f  %s",
				i * 4, x, y, (int)y, f, WhatIs(y));
			++nChanged;
		}
		if (!nChanged) Log("    nothing - the argument is input only");
	}

	const LONG n = (g_nTrace < kTrace) ? g_nTrace : kTrace;
	Log("  it made %ld calls back into the engine:", g_nTrace);
	for (LONG t = 0; t < n; ++t)
	{
		const CBCall& e = g_Trace[t];
		char szArgs[128]{};
		for (int k = 0; k < 4; ++k)
		{
			char szOne[16];
			sprintf_s(szOne, "%08X ", e.nArg[k]);
			strcat_s(szArgs, szOne);
		}
		Log("    %3ld  cb %2d (offset %2d)  %s", t, e.nWhich, e.nWhich * 4, szArgs);
	}
	Log("=== end RebindLightmaps ===");
	return r;
}

// ---- THE STALL WATCHDOG ------------------------------------------------
//
// Headset testing: frame rate in the 80s where it needs a rock solid 90.
// The logs: a stall of about 100 ms every TWO SECONDS by the clock, seen from
// inside the world pass one time and inside the publish the next - so not a
// place in our code but something that holds the whole process. The notes
// have this signature once before ("the audio stack holding the whole
// process 100 ms, on a 2 s timer; a reboot cleared it") and the desk runs
// silent, so the desk will never see it. This names it instead: a thread
// that watches the time since the last present, and when a frame runs past
// 45 ms suspends the main thread for a moment, reads where it is, and logs
// the return addresses as module+offset. No allocation while the main thread
// is suspended - it may hold the heap lock - and the log line is written
// after it is resumed. +StubWatchdog 0 turns it off.
namespace
{
	volatile LONGLONG g_qWatchTick = 0;		// QPC at the last present
	DWORD    g_nMainThreadId = 0;
	int      g_bWatchdog = 1;
	int      g_nPresentEvery = 0;		// desktop presents while the host is live: 0 never, 1 every, N one in N
	volatile LONG g_nPresentsSkipped = 0;
	long     g_nWatchReports = 0;

	void NameAddress(DWORD addr, char* out, size_t n)
	{
		HMODULE h = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
							 | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							 (LPCSTR)(uintptr_t)addr, &h) && h)
		{
			char path[MAX_PATH] = { 0 };
			GetModuleFileNameA(h, path, MAX_PATH);
			const char* b = strrchr(path, '\\');
			b = b ? b + 1 : path;
			sprintf_s(out, n, "%s+%05X", b, (unsigned)(addr - (DWORD)(uintptr_t)h));
		}
		else sprintf_s(out, n, "%08X", (unsigned)addr);
	}

	// STOPPED BEFORE THE DLL GOES. The thread was created and its handle
	// closed, and nothing ever ended it: at quit the engine unloaded this DLL
	// with the watchdog still sleeping inside it, and the next Sleep returned
	// into unmapped code. Windows logged "d3dstub.ren_unloaded +0xe518" on
	// EVERY exit of both night sweeps - 200 access violations, one per level
	// - and on every quit of the game. Term asks it to stop and waits.
	volatile LONG g_bWatchStop = 0;
	HANDLE g_hWatchdog = nullptr;
	DWORD WINAPI WatchdogThread(void*)
	{
		HANDLE hMain = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
								| THREAD_QUERY_INFORMATION, FALSE, g_nMainThreadId);
		if (!hMain) return 0;
		LARGE_INTEGER f; QueryPerformanceFrequency(&f);
		LONGLONG qReported = 0;
		while (!g_bWatchStop)
		{
			Sleep(2);
			if (g_bWatchStop) break;
			const LONGLONG t = g_qWatchTick;
			if (!t || t == qReported || !f.QuadPart) continue;
			LARGE_INTEGER q; QueryPerformanceCounter(&q);
			const double fMs = (double)(q.QuadPart - t) * 1000.0 / (double)f.QuadPart;
			if (fMs < 45.0) continue;
			if (g_nWatchReports >= 24) continue;
			if (!R3D_HaveWorld()) continue;		// loads and menus stall by right
			qReported = t;
			++g_nWatchReports;

			DWORD addrs[14]; int nAddr = 0;
			if (SuspendThread(hMain) != (DWORD)-1)
			{
				CONTEXT ctx = {};
				ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
				if (GetThreadContext(hMain, &ctx))
				{
					addrs[nAddr++] = ctx.Eip;
					// The EBP chain, as far as it runs. Frames built without
					// a frame pointer end it early; the top address alone
					// names the module that is holding us.
					DWORD ebp = ctx.Ebp;
					for (int i = 0; i < 13 && ebp; ++i)
					{
						if (IsBadReadPtr((const void*)(uintptr_t)ebp, 8)) break;
						const DWORD ret = *(const DWORD*)(uintptr_t)(ebp + 4);
						const DWORD nxt = *(const DWORD*)(uintptr_t)ebp;
						if (!ret) break;
						addrs[nAddr++] = ret;
						if (nxt <= ebp) break;
						ebp = nxt;
					}
				}
				ResumeThread(hMain);
			}
			char line[1200]; line[0] = 0;
			for (int i = 0; i < nAddr; ++i)
			{
				char nm[128]; NameAddress(addrs[i], nm, sizeof nm);
				strcat_s(line, i ? " < " : "");
				strcat_s(line, nm);
			}
			Log("  WATCHDOG: main thread %.0f ms past its last present, in '%s' | %s",
				fMs, R3D_Phase(), nAddr ? line : "(no context)");
		}
		CloseHandle(hMain);
		return 0;
	}

	void StartWatchdog()
	{
		if (!g_bWatchdog || g_hWatchdog) return;
		g_nMainThreadId = GetCurrentThreadId();
		g_bWatchStop = 0;
		g_hWatchdog = CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr);
		Log("  watchdog: armed on thread %lu - a frame past 45 ms is named by its stack", g_nMainThreadId);
	}
	void StopWatchdog()
	{
		if (!g_hWatchdog) return;
		InterlockedExchange(&g_bWatchStop, 1);
		const DWORD r = WaitForSingleObject(g_hWatchdog, 1000);
		CloseHandle(g_hWatchdog);
		g_hWatchdog = nullptr;
		Log("  watchdog: %s", (r == WAIT_OBJECT_0) ? "stopped" : "did not stop in a second - unload may fault");
	}
}
static void* __cdecl s_Context(void*)             { SLOT(5);  return nullptr; }
// Black now that there is something to draw on top of it. It was cyan while the
// question was "is the engine calling us at all", which one screenshot then
// answered; a colour that is easy to see is a poor background for a picture.
static int   __cdecl s_Clear(void*, uint32_t, void*) { SLOT(6);
	if (g_pCtx && g_pRTV)
	{
		// Phase 1's instrument, kept: +StubClearCyan 1 paints every engine
		// Clear cyan. It separates "something paints over the whole screen
		// after we draw" from "our own draw never lands", and it needs no log
		// to interpret.
		// THE FOG COLOUR, where the level has fog. A level with no skybox
		// panorama shows the clear straight through its sky brushes, and
		// retail leaves the fog colour there - which is why the intro forest
		// has a dark blue night sky in the original and a pure black one here.
		float c[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		if (g_bClearCyan) { c[1] = 1.0f; c[2] = 1.0f; }
		else              { R3D_FogClearColour(c); }
		g_pCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
		g_pCtx->ClearRenderTargetView(g_pRTV, c);
	}
	return 0; }

static int   __cdecl s_Start3D(void)              { SLOT(7);  g_bIn3D = 1; return 1; }
static int   __cdecl s_End3D(void)                { SLOT(8);  g_bIn3D = 0; return 1; }
static int   __cdecl s_IsIn3D(void)               { SLOT(9);  return g_bIn3D; }

// The blend mode resets to ALPHA at the start of every Optimized2D block.
// iltclient.h says so - "Defaults to LTSURFACEBLEND_ALPHA" - and the frame
// trace shows why it matters: after the screen tint sets ADD, the engine runs
// TWELVE BlitToScreen calls in the next block without setting a mode at all.
// Carrying ADD into them drew the whole HUD additively, so over the bright
// street it saturated to white and vanished, while over a black screen it
// looked perfect. That is exactly the pair of symptoms the desk saw.
static int   __cdecl s_StartOpt2D(void)           { SLOT(10); g_bIn2D = 1; g_n2DBlend = 0; R2D_SetBlend(0); R2D_SetColour(0xFFFFFF); return 1; }
static int   __cdecl s_EndOpt2D(void)             { SLOT(11); g_bIn2D = 0; return 0; }
static int   __cdecl s_IsInOpt2D(void)            { SLOT(12); return g_bIn2D; }

// The mode was recorded here and never used - so the game's ADDITIVE screen
// tint was drawn alpha-blended, an opaque black surface over the whole picture
// on every frame. R2D_SetBlend is what makes it mean something.
static int   __cdecl s_SetBlend(uint32_t b)       { SLOT(13); g_n2DBlend = b; R2D_SetBlend(b); return 1; }
static int   __cdecl s_GetBlend(uint32_t* p)      { SLOT(14); if (p) *p = g_n2DBlend;  return 1; }
// Recorded and never used until now - the same defect the blend mode had,
// and the reason no menu item ever looked selected.
static int   __cdecl s_SetColour(uint32_t c)      { SLOT(15); g_n2DColour = c; R2D_SetColour(c); return 1; }
static int   __cdecl s_GetColour(uint32_t* p)     { SLOT(16); if (p) *p = g_n2DColour; return 1; }

// ---------------------------------------------------------------------------
// RenderScene.
//
// The engine does not push geometry at us - there is no DrawPolygon slot
// anywhere in the 37. It hands us the scene description, whose words 1..6 are
// six pointers into lithtech.exe's data segment, and the renderer walks the
// engine's own structures from there. So the first thing to know is what those
// six point AT, and that is a measurement rather than a reading.
//
// Dumped once, late enough that a scene is actually up, because the first
// scenes rendered are the loading screen and are not representative.
// ---------------------------------------------------------------------------
static int RealScene(void* pScene)
{
	if (!g_bHaveReal || !g_pRealSlot[17]) return 0;
	return ((int(__cdecl*)(void*))g_pRealSlot[17])(pScene);
}

// ---------------------------------------------------------------------------
// The world walk.
//
// Word 13 of the scene description (offset 52) is the world handle. That is
// not a guess and it is not from the six-pointer table that turned out to be
// nothing: it is read out of the real renderer's own code. d3d.ren imports
// nothing from lithtech.exe - only DDRAW, WINMM, KERNEL32 and USER32 - so
// every fact it has about the world arrives through the RenderStruct or
// through this pointer, and tools/ren-anatomy.py shows the RenderStruct
// carries only dimensions, counters and the global light direction.
//
// d3d.ren + 0x1EBE2 loads [scene + 0x34] and, when the stamp counter at
// [+0xC] wraps, walks everything below it to reset the per-item stamps. That
// walk is what names the fields:
//
//   world   = [handle + 8]
//   stamp   = uint16 at [handle + 0xC]        wraps at 0xFFFF, never 0
//   [world + 0x18C] = array of pointers, [world + 0x190] = how many
//     each element e:  sub = [e + 4]
//       [sub + 0x88] = array of 0x30-byte records, [sub + 0x8C] = how many
//                      each record carries a uint16 stamp at +0x2C
//       [sub + 0xA0] = array of POINTERS,          [sub + 0xA4] = how many
//                      each object carries a uint16 stamp at +0x42
//
// SELF-CHECK: the viewport rect at word 41 must read 0,0,1440,1494 - the value
// docs/SCENEDESC.md measured independently. If it does not, we are not looking
// at the scene description and every number below it is worthless. A mechanism
// that produces a known answer for a structure you already know is worth more
// than one that only produces answers for structures you do not.
// ---------------------------------------------------------------------------
static bool Readable(uint32_t v, size_t n)
{
	return v && !IsBadReadPtr((const void*)(uintptr_t)v, n);
}

static uint32_t Word(uint32_t p, uint32_t off)
{
	return Readable(p + off, 4) ? *(const uint32_t*)(uintptr_t)(p + off) : 0;
}

// A word that points at printable ASCII is almost always a name, and a name
// settles what a structure IS in one line where a field map only narrows it.
static bool LooksLikeString(uint32_t p, char* pszOut, int nOut)
{
	if (!p || IsBadReadPtr((const void*)(uintptr_t)p, 4)) return false;
	const char* q = (const char*)(uintptr_t)p;
	int i = 0;
	for (; i < nOut - 1; ++i)
	{
		if (IsBadReadPtr(q + i, 1)) return false;
		const unsigned char c = (unsigned char)q[i];
		if (!c) break;
		if (c < 32 || c > 126) return false;
		pszOut[i] = (char)c;
	}
	pszOut[i] = 0;
	return i >= 3;
}

static void LogWordsAndStrings(const char* pszWhat, uint32_t p, int nBytes)
{
	if (!Readable(p, nBytes)) { Log("      %s: unreadable at %08X", pszWhat, p); return; }
	for (int i = 0; i < nBytes; i += 4)
	{
		const uint32_t v = *(const uint32_t*)(uintptr_t)(p + i);
		char szStr[64];
		float f; memcpy(&f, &v, 4);
		if (LooksLikeString(v, szStr, sizeof szStr))
			Log("      %s +%03X  %08X   -> \"%s\"", pszWhat, i, v, szStr);
		else if (v && v < 0x10000)
			Log("      %s +%03X  %08X   %d", pszWhat, i, v, (int)v);
		else if (f > -1e7f && f < 1e7f && f != 0.0f && (v & 0x7F800000) && (v & 0x7F800000) != 0x7F800000)
			Log("      %s +%03X  %08X   %.3f%s", pszWhat, i, v, f, Classify(v));
		else
			Log("      %s +%03X  %08X   %s", pszWhat, i, v, Classify(v));
	}
}

static void LogHex(const char* pszWhat, uint32_t p, int nBytes)
{
	if (!Readable(p, nBytes)) { Log("      %s: unreadable at %08X", pszWhat, p); return; }
	for (int i = 0; i < nBytes; i += 16)
	{
		char szLine[160]{}; char szOne[24];
		sprintf_s(szOne, "      %s +%02X ", pszWhat, i);
		strcat_s(szLine, szOne);
		for (int j = 0; j < 16 && i + j < nBytes; j += 4)
		{
			sprintf_s(szOne, "%08X ", *(const uint32_t*)(uintptr_t)(p + i + j));
			strcat_s(szLine, szOne);
		}
		Log("%s", szLine);
	}
}

static void LogWorldWalk(const uint32_t* s)
{
	Log("");
	Log("=== the world, reached through scene word 13 (RenderScene call %d) ===",
		g_nCalls[17]);

	// The self-check first, so a bad read is visible before anything is read
	// INTO it. Words 41..44 are the viewport rect; SCENEDESC measured
	// 0,0,1440,1494 at this resolution.
	Log("  scene type (word 0)      = %d", s[0]);
	Log("  viewport rect (word 41)  = %d,%d,%d,%d   %s",
		s[41], s[42], s[43], s[44],
		(s[43] > 0 && s[43] <= 8192 && s[44] > 0 && s[44] <= 8192)
			? "plausible - this IS the scene description"
			: "IMPLAUSIBLE - we are not reading the scene description");

	// The whole thing, classified. Reading one word and arguing about it is
	// how the six "list heads" happened; a pointer that leads somewhere is
	// visible here whatever offset it turns out to live at.
	Log("  --- the scene description, word by word ---");
	for (int i = 0; i < 60; ++i)
	{
		if (IsBadReadPtr(&s[i], 4)) break;
		float f; memcpy(&f, &s[i], 4);
		const char* pszWhere = Classify(s[i]);
		const bool bHeap = s[i] >= 0x10000 && !*pszWhere
			&& !IsBadReadPtr((const void*)(uintptr_t)s[i], 4);
		Log("    word %2d  +%3d  %08X  %11d  %14.4f  %s%s",
			i, i * 4, s[i], (int)s[i], f, pszWhere,
			bHeap ? "  <- readable heap pointer" : "");
	}

	LogWorldFromHandle(s[13]);
	Log("=== end world walk ===");
}

static void LogWorldFromHandle(uint32_t h)
{
	Log("  world handle             = %08X  %s", h, Classify(h));
	if (!Readable(h, 16))
	{
		Log("  nothing behind it. At the menu this is expected; in a loaded");
		Log("  world it means the handle never reached us.");
		return;
	}
	LogHex("handle", h, 16);

	const uint32_t nStamp = Readable(h + 0xC, 2) ? *(const uint16_t*)(uintptr_t)(h + 0xC) : 0;
	const uint32_t w = Word(h, 8);
	Log("  frame stamp [+0xC]       = %u   (wraps at 65535, reset walk at 1)", nStamp);
	Log("  world       [+8]         = %08X  %s", w, Classify(w));
	if (!Readable(w, 0x194))
	{
		Log("  the world pointer is unreadable, so the layout below is wrong.");
		return;
	}

	// The whole world header. The 565-entry array at +0x18C turns out to be the
	// level's WorldModels - Door20, Breakable0, Garden_Water - which are brush
	// OBJECTS, not the static architecture. So the main BSP is another field
	// here, and tools/global-usage.py says the drawing subtree takes the
	// ADDRESS of +0x6C and reads +0xFC, +0x104 and +0x180.
	Log("  --- the world header ---");
	LogWordsAndStrings("world", w, 0x1A0);

	// The header is a run of {vtable, pointer, count} array descriptors and a
	// uniform spatial grid. Naming those fields is only worth anything if the
	// naming can be wrong, so each one is checked against another field that
	// must agree with it arithmetically. Three independent relations:
	//
	//   the bounding box centre must be the average of min and max
	//   the cell counts must multiply to the total cell count
	//   the reciprocal cell sizes must be 1 / the cell sizes
	//
	// A layout that is off by one field fails all three at once. A dump that
	// merely looks plausible passes none of them, which is the whole point -
	// this project has twice written down a structure that only looked right.
	{
		const float* f = (const float*)(uintptr_t)w;
		auto F = [&](uint32_t off) { return Readable(w + off, 4) ? f[off / 4] : 0.0f; };
		auto D = [&](uint32_t off) { return Word(w, off); };
		auto Near = [](float a, float b, float tol) {
			const float d = a - b; return (d < 0 ? -d : d) <= tol; };

		Log("  --- the header, checked ---");
		Log("    bounds min (%.0f %.0f %.0f)  max (%.0f %.0f %.0f)",
			F(0x90), F(0x94), F(0x98), F(0x9C), F(0xA0), F(0xA4));
		const bool bMid = Near(F(0xA8), (F(0x90) + F(0x9C)) * 0.5f, 0.5f)
					   && Near(F(0xAC), (F(0x94) + F(0xA0)) * 0.5f, 0.5f)
					   && Near(F(0xB0), (F(0x98) + F(0xA4)) * 0.5f, 0.5f);
		Log("    centre     (%.0f %.0f %.0f)   %s",
			F(0xA8), F(0xAC), F(0xB0),
			bMid ? "== (min+max)/2, so +0x90/+0x9C/+0xA8 are min, max, centre"
				 : "NOT the average of min and max - the layout is wrong");

		const uint32_t nx = D(0x104), ny = D(0x108), nz = D(0x10C);
		const bool bGrid = nx && ny && nz && (nx * ny * nz == D(0x100));
		Log("    grid       %u x %u x %u = %u cells, +0x100 holds %u   %s",
			nx, ny, nz, nx * ny * nz, D(0x100),
			bGrid ? "agree" : "DISAGREE - +0x104 is not the grid size");

		const bool bRecip = F(0x120) > 0.0f
			&& Near(F(0x12C), 1.0f / F(0x120), 1e-6f)
			&& Near(F(0x130), 1.0f / F(0x124), 1e-6f)
			&& Near(F(0x134), 1.0f / F(0x128), 1e-6f);
		Log("    cell size  (%.1f %.1f %.1f), +0x12C holds (%.6f %.6f %.6f)  %s",
			F(0x120), F(0x124), F(0x128), F(0x12C), F(0x130), F(0x134),
			bRecip ? "== 1/cell, as a renderer would cache it"
				   : "NOT the reciprocals - the layout is wrong");

		Log("    grid box   (%.0f %.0f %.0f) .. (%.0f %.0f %.0f)",
			F(0x138), F(0x13C), F(0x140), F(0x150), F(0x154), F(0x158));
		Log("    WORLD HEADER SELF-CHECK: %s",
			(bMid && bGrid && bRecip) ? "PASSED - all three relations hold"
									  : "FAILED - do not trust the field names");

		// The header is a run of 20-byte array descriptors:
		// {vtable, pointer, count, 0, 0}. That stride is not assumed - the
		// WorldModel array, whose count of 565 and contents are already known,
		// sits at +0x188 and fits the pattern exactly, so the same shape at
		// +0x18, +0x2C, +0x40 and +0x54 is the same kind of thing.
		//
		// Two of them hold 19626 entries under different vtables. Static level
		// geometry is the only thing a level has two equally-sized very large
		// arrays of, so this is where it should be - but that is a guess, and
		// the bytes below are what settles it.
		Log("  --- the array descriptors ---");
		static const uint32_t kDesc[] = { 0x18, 0x2C, 0x40, 0x54, 0x188, 0x19C };
		for (int k = 0; k < 6; ++k)
		{
			const uint32_t off = kDesc[k];
			const uint32_t vt = D(off), ptr = D(off + 4), cnt = D(off + 8);
			Log("");
			Log("    +%03X  vtable %08X %-12s  data %08X  count %u",
				off, vt, Classify(vt), ptr, cnt);
			if (D(off + 12) || D(off + 16))
				Log("          (+12/+16 are %08X %08X, not the zeros the pattern"
					" predicts)", D(off + 12), D(off + 16));
			if (cnt && Readable(ptr, 0x40)) LogHex("      data", ptr, 0x40);
		}

		// +0x054's entries are {pointer, length} pairs, and by eye the pointers
		// chain: 12622020 +8 -> 12622028 +14 -> 12622036 +20 -> 1262204A.
		// If that is really an index into the blob at +0x040, then walking all
		// 19626 must land exactly on the blob's end and the lengths must sum to
		// its count. Eight entries agreeing is a pattern; 19626 agreeing with
		// an independently stored total is a structure.
		{
			// A descriptor is {vtable, pointer, count}, so the DATA is at +4.
			// The first version of this read +0 and reported the blob as
			// 004A1AB8 - a lithtech.exe address, obviously a vtable - and the
			// check failed rather than printing a plausible sum. That is the
			// check earning its keep on its author.
			const uint32_t pIdx = D(0x58), nIdx = D(0x5C);
			const uint32_t pBlob = D(0x44), nBlob = D(0x48);
			uint64_t nSum = 0;
			uint32_t nWalk = pBlob, nBad = 0, nFirstBad = 0;
			if (Readable(pIdx, 8) && nIdx && nIdx < 1000000)
			{
				for (uint32_t i = 0; i < nIdx; ++i)
				{
					if (!Readable(pIdx + i * 8, 8)) { nBad = 1; nFirstBad = i; break; }
					const uint32_t at = *(const uint32_t*)(uintptr_t)(pIdx + i * 8);
					const uint32_t len = *(const uint32_t*)(uintptr_t)(pIdx + i * 8 + 4);
					if (at != nWalk && !nBad) { nBad = 2; nFirstBad = i; }
					nWalk = at + len;
					nSum += len;
				}
				Log("");
				Log("  --- is +0x054 an index into the blob at +0x040? ---");
				Log("    %u entries of {pointer, length}, blob %08X, its count %u",
					nIdx, pBlob, nBlob);
				Log("    lengths sum to %llu; the walk ends at %08X, blob start + %u",
					nSum, nWalk, nWalk - pBlob);
				Log("    INDEX SELF-CHECK: %s",
					(!nBad && nSum == nBlob)
						? "PASSED - contiguous, complete, and the sum equals the"
						  " stored count. +0x040 is a byte blob and +0x054 indexes it."
						: (nBad == 2
							? "FAILED - the entries are not contiguous"
							: "FAILED - see the numbers above"));
				if (nBad) Log("    first disagreement at entry %u", nFirstBad);
			}
		}

		// The four pointers at +0xC4..+0xD0 each lead to three self-linked
		// (empty) list heads followed by a bounding box, and the four boxes
		// tile the world box: each splits it at the centre in X and Z, and
		// leaves Y whole. That is a quadtree node's children, and it is why the
		// grid has 48 x 40 cells across but only 8 up. Checked rather than
		// eyeballed - a child box must match the parent's min or centre on each
		// axis.
		{
			auto FA = [&](uint32_t a, uint32_t o2) {
				float v = 0.0f;
				if (Readable(a + o2, 4)) memcpy(&v, (const void*)(uintptr_t)(a + o2), 4);
				return v; };
			auto Near = [](float a, float b) {
				const float d = a - b; return (d < 0 ? -d : d) <= 0.5f; };
			const float bx[2] = { F(0x90), F(0x9C) }, cx = F(0xA8);
			const float bz[2] = { F(0x98), F(0xA4) }, cz = F(0xB0);
			int nGood = 0;
			Log("");
			Log("  --- the four children at +0xC4..+0xD0 ---");
			for (int k = 0; k < 4; ++k)
			{
				const uint32_t ptr = D(0xC4 + k * 4);
				if (!Readable(ptr, 0x30)) { Log("    [%d] unreadable", k); continue; }
				const float x0 = FA(ptr, 0x18), z0 = FA(ptr, 0x20);
				const float x1 = FA(ptr, 0x24), z1 = FA(ptr, 0x2C);
				const bool ok =
					(Near(x0, bx[0]) || Near(x0, cx)) && (Near(x1, cx) || Near(x1, bx[1])) &&
					(Near(z0, bz[0]) || Near(z0, cz)) && (Near(z1, cz) || Near(z1, bz[1]));
				if (ok) ++nGood;
				Log("    [%d] %08X  x %.0f..%.0f  z %.0f..%.0f  %s",
					k, ptr, x0, x1, z0, z1, ok ? "a quadrant" : "NOT a quadrant");
			}
			Log("    QUADTREE SELF-CHECK: %s",
				nGood == 4 ? "PASSED - all four tile the world box in X and Z,"
							 " with Y left whole. +0x6C is the WorldTree root."
						   : "FAILED - these are not the children of that box");
		}

		// The node layout falls out of the root's own offsets. The root node is
		// INLINE in the world at +0x78, not a separate allocation, and every
		// field we already named lines up against a child node's layout when
		// you subtract 0x78:
		//
		//   +0x00  three list heads, {next, prev}, self-linked when empty
		//   +0x18  bounds min      (world +0x90)
		//   +0x24  bounds max      (world +0x9C)
		//   +0x30  centre          (world +0xA8)
		//   +0x3C  largest extent  (world +0xB4)
		//   +0x40  radius          (world +0xB8)
		//   +0x44  a count, 523 at the root
		//   +0x4C  four children   (world +0xC4)
		//
		// A child node dumped earlier has its bounding box at +0x18, which is
		// what makes this the same structure rather than a coincidence of
		// spacing. So the tree can be walked, and the question the walk exists
		// to answer is where the geometry hangs: every node has three lists and
		// the root's are all empty.
		{
			struct Walk
			{
				uint32_t nNodes = 0, nLeaves = 0, nWithItems = 0, nItems = 0;
				uint32_t nDeepest = 0, pFirstItem = 0, nFirstList = 0;
			} wk;

			// Iterative, with an explicit stack and a hard cap. A malformed
			// tree must produce a short report, not a stack overflow inside
			// the engine's render thread.
			uint32_t aStack[512]; int nSP = 0, nDepth[512];
			aStack[nSP] = w + 0x78; nDepth[nSP] = 0; ++nSP;
			while (nSP > 0 && wk.nNodes < 20000)
			{
				--nSP;
				const uint32_t nd = aStack[nSP]; const int d = nDepth[nSP];
				if (!Readable(nd, 0x60)) continue;
				++wk.nNodes;
				if ((uint32_t)d > wk.nDeepest) wk.nDeepest = d;

				bool bAny = false;
				for (int L = 0; L < 3; ++L)
				{
					const uint32_t head = nd + L * 8;
					const uint32_t first = Word(head, 0);
					if (!first || first == head) continue;	// self-linked = empty
					bAny = true;
					// Walk the ring, capped, counting members.
					uint32_t cur = first; int nGuard = 0;
					while (cur && cur != head && nGuard++ < 4096)
					{
						++wk.nItems;
						if (!wk.pFirstItem) { wk.pFirstItem = cur; wk.nFirstList = L; }
						if (!Readable(cur, 8)) break;
						cur = Word(cur, 0);
					}
				}
				if (bAny) ++wk.nWithItems;

				int nKids = 0;
				for (int k = 0; k < 4; ++k)
				{
					const uint32_t c = Word(nd, 0x4C + k * 4);
					if (Readable(c, 0x60) && nSP < 500)
					{ aStack[nSP] = c; nDepth[nSP] = d + 1; ++nSP; ++nKids; }
				}
				if (!nKids) ++wk.nLeaves;
			}

			Log("");
			Log("  --- walking the WorldTree from the root node at world+0x78 ---");
			Log("    %u nodes, %u leaves, deepest %u", wk.nNodes, wk.nLeaves, wk.nDeepest);
			Log("    %u nodes have a non-empty list; %u items in total",
				wk.nWithItems, wk.nItems);
			Log("    root's own count field (+0x44) says %u", D(0xBC));
			if (wk.pFirstItem)
			{
				Log("    the first item, off list %u:", wk.nFirstList);
				LogWordsAndStrings("item", wk.pFirstItem, 0x60);
			}
			else
				Log("    every list in the tree is empty - the geometry is not here");
		}

		// The drawing subtree takes the ADDRESS of world+0x6C, which carries
		// its own vtable - an embedded object, and the grid fields above are
		// inside it. These four pointers sit in the middle of it.
		Log("");
		Log("  --- the embedded object at +0x6C (vtable %08X %s) ---",
			D(0x6C), Classify(D(0x6C)));
		for (uint32_t off = 0xC4; off <= 0xD0; off += 4)
		{
			const uint32_t ptr = D(off);
			Log("    +%03X  %08X  %s", off, ptr, Classify(ptr));
			if (Readable(ptr, 0x30)) LogHex("      ", ptr, 0x30);
		}
	}

	const uint32_t nList = Word(w, 0x190);
	const uint32_t pList = Word(w, 0x18C);
	Log("  [world+0x190] count      = %u", nList);
	Log("  [world+0x18C] array      = %08X  %s", pList, Classify(pList));
	if (nList > 4096 || !Readable(pList, 4))
	{
		Log("  that count is not credible, so this is not the array. Stop here");
		Log("  rather than printing pointers derived from it.");
		return;
	}

	// The name settles what these are. sub+0 is a vtable into lithtech.exe and
	// sub+4 is an inline string - "TranslucentWorldModel89" for the first one -
	// so these are the engine's WorldModels, named rather than inferred.
	Log("");
	Log("  the first 16 by name (sub+0 is a vtable, sub+4 an inline string):");
	for (uint32_t i = 0; i < nList && i < 16; ++i)
	{
		const uint32_t e = Word(pList, i * 4);
		const uint32_t sub = Word(e, 4);
		if (!Readable(sub, 0x40)) { Log("    [%2u] unreadable", i); continue; }
		char szName[64]{};
		const char* q = (const char*)(uintptr_t)(sub + 4);
		int k = 0;
		for (; k < 63 && !IsBadReadPtr(q + k, 1) && q[k] >= 32 && q[k] <= 126; ++k)
			szName[k] = q[k];
		szName[k] = 0;
		Log("    [%2u] vtable %08X %-12s  \"%s\"", i, Word(sub, 0),
			Classify(Word(sub, 0)), szName);
	}

	// Is the static level architecture simply one of the 565?
	//
	// In LithTech the main world can be a WorldModel like any other, and the
	// first sixteen names are all doors and breakables only because that is
	// where the array happens to start. A door brush has about 21 of each
	// thing; the level itself would have thousands. So rank them: if one entry
	// dwarfs the rest, that is the world, and if they are all small then the
	// architecture is somewhere else entirely.
	{
		uint32_t nBest[5] = {0}, pBest[5] = {0};
		uint64_t nTotal = 0;
		for (uint32_t i = 0; i < nList; ++i)
		{
			const uint32_t sub = Word(Word(pList, i * 4), 4);
			if (!Readable(sub, 0xB0)) continue;
			const uint32_t n = Word(sub, 0xA4);		// the array walked earlier
			nTotal += n;
			for (int k = 0; k < 5; ++k)
				if (n > nBest[k])
				{
					for (int j = 4; j > k; --j) { nBest[j] = nBest[j-1]; pBest[j] = pBest[j-1]; }
					nBest[k] = n; pBest[k] = sub; break;
				}
		}
		Log("");
		Log("  --- the 565 ranked by their +0xA4 count ---");
		Log("    %llu in total, %.1f each on average", nTotal, (double)nTotal / (nList ? nList : 1));
		for (int k = 0; k < 5 && pBest[k]; ++k)
		{
			char szName[64]{};
			const char* q = (const char*)(uintptr_t)(pBest[k] + 4);
			int c = 0;
			for (; c < 63 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126; ++c)
				szName[c] = q[c];
			szName[c] = 0;
			Log("    %u  \"%s\"   (+0x5C %u, +0x6C %u, +0x74 %u, +0x7C %u, +0xAC %u)",
				nBest[k], szName, Word(pBest[k], 0x5C), Word(pBest[k], 0x6C),
				Word(pBest[k], 0x74), Word(pBest[k], 0x7C), Word(pBest[k], 0xAC));
		}
		// The render BSP, by name. Its parallel {pointer, count} pairs are the
		// level's geometry, and this is the first look at them.
		for (uint32_t i = 0; i < nList; ++i)
		{
			const uint32_t sub = Word(Word(pList, i * 4), 4);
			if (!Readable(sub, 0xB0)) continue;
			char szName[16]{};
			const char* q = (const char*)(uintptr_t)(sub + 4);
			for (int c = 0; c < 15 && !IsBadReadPtr(q + c, 1); ++c) szName[c] = q[c];
			if (strcmp(szName, "VisBSP") != 0) continue;

			Log("");
			Log("    --- VisBSP at %08X: the render BSP ---", sub);
			// Every {pointer, count} pair in its header, with the stride the
			// count implies where two arrays are adjacent in memory.
			static const uint32_t kPair[] = { 0x58, 0x68, 0x70, 0x78, 0xA0, 0xA8 };
			for (int k = 0; k < 6; ++k)
			{
				const uint32_t ptr = Word(sub, kPair[k]);
				const uint32_t cnt = Word(sub, kPair[k] + 4);
				Log("      +%02X  data %08X  count %u", kPair[k], ptr, cnt);
				if (cnt && Readable(ptr, 0x30)) LogHex("        ", ptr, 0x30);
			}
			// Three checks over the WHOLE arrays. The first four vertices form
			// an axis-aligned rectangle and the first three planes have unit
			// axis normals, which is suggestive and nothing more - four of
			// anything can look like anything. These cannot pass by accident.
			{
				auto FF = [&](uint32_t a) {
					float v = 0.0f;
					if (Readable(a, 4)) memcpy(&v, (const void*)(uintptr_t)a, 4);
					return v; };
				// The world's own bounds, re-read here rather than borrowed
				// from the header check's scope.
				auto WF = [&](uint32_t off) { return FF(w + off); };

				// Planes: 16 bytes of {normal, distance}, so every normal must
				// be unit length.
				const uint32_t pPl = Word(sub, 0x68), nPl = Word(sub, 0x6C);
				uint32_t nUnit = 0, nPlChecked = 0;
				for (uint32_t j = 0; j < nPl && Readable(pPl + j * 16, 16); ++j)
				{
					const float x = FF(pPl + j * 16), y = FF(pPl + j * 16 + 4),
								z = FF(pPl + j * 16 + 8);
					const float m = x * x + y * y + z * z;
					++nPlChecked;
					if (m > 0.999f && m < 1.001f) ++nUnit;
				}
				Log("      PLANES: %u of %u have a unit-length normal  %s",
					nUnit, nPlChecked,
					(nPlChecked && nUnit == nPlChecked)
						? "- +0x68 is an array of planes, 16 bytes each"
						: "- NOT planes, or not that stride");

				// Vertices: 12 bytes of XYZ, so every one must lie inside the
				// world's own bounding box, which was measured separately.
				const uint32_t pVx = Word(sub, 0xA8), nVx = Word(sub, 0xAC);
				uint32_t nIn = 0, nVxChecked = 0;
				for (uint32_t j = 0; j < nVx && Readable(pVx + j * 12, 12); ++j)
				{
					const float x = FF(pVx + j * 12), y = FF(pVx + j * 12 + 4),
								z = FF(pVx + j * 12 + 8);
					++nVxChecked;
					if (x >= WF(0x90) && x <= WF(0x9C) && y >= WF(0x94) && y <= WF(0xA0)
						&& z >= WF(0x98) && z <= WF(0xA4)) ++nIn;
				}
				Log("      VERTICES: %u of %u lie inside the world bounds  %s",
					nIn, nVxChecked,
					(nVxChecked && nIn == nVxChecked)
						? "- +0xA8 is an array of points, 12 bytes each"
						: "- NOT points, or not that stride");

				// Polygons: an array of pointers whose spacing should be one
				// constant object size.
				const uint32_t pPo = Word(sub, 0xA0), nPo = Word(sub, 0xA4);
				uint32_t nStride = 0, nSame = 0, nPoChecked = 0;
				for (uint32_t j = 1; j < nPo && Readable(pPo + j * 4, 4); ++j)
				{
					const uint32_t a = Word(pPo, (j - 1) * 4), b = Word(pPo, j * 4);
					if (b <= a) continue;
					++nPoChecked;
					if (!nStride) nStride = b - a;
					if (b - a == nStride) ++nSame;
				}
				Log("      POLYGONS: %u pointers, %u of %u consecutive pairs are"
					" %u bytes apart  %s", nPo, nSame, nPoChecked, nStride,
					(nPoChecked && nSame * 20 > nPoChecked * 19)
						? "- one object size, so +0xA0 indexes a flat array"
						: "- mixed sizes, so they are separately allocated");
			}
			g_pVisBSP = sub;

			// The polygon record.
			//
			// Annotating each word against the arrays we have already verified
			// is what turns a hexdump into a layout: a pointer that lands
			// exactly on vertex 1234 or plane 56 is not ambiguous, and one that
			// lands between two elements says the stride is wrong.
			{
				const uint32_t pPlanes = Word(sub, 0x68), nPlanes = Word(sub, 0x6C);
				const uint32_t pA70 = Word(sub, 0x70), nA70 = Word(sub, 0x74);
				const uint32_t pA78 = Word(sub, 0x78), nA78 = Word(sub, 0x7C);
				const uint32_t pPolys = Word(sub, 0xA0), nPolys = Word(sub, 0xA4);
				const uint32_t pVerts = Word(sub, 0xA8), nVerts = Word(sub, 0xAC);

				auto Annotate = [&](uint32_t v, char* pszOut, int nOut)
				{
					pszOut[0] = 0;
					if (v >= pPlanes && v < pPlanes + nPlanes * 16)
					{
						const uint32_t d = v - pPlanes;
						sprintf_s(pszOut, nOut, "-> plane[%u]%s", d / 16,
							(d % 16) ? " (MISALIGNED)" : "");
					}
					else if (v >= pVerts && v < pVerts + nVerts * 12)
					{
						const uint32_t d = v - pVerts;
						sprintf_s(pszOut, nOut, "-> vertex[%u]%s", d / 12,
							(d % 12) ? " (MISALIGNED)" : "");
					}
					else if (v >= pPolys && v < pPolys + nPolys * 4)
						sprintf_s(pszOut, nOut, "-> polygon pointer[%u]", (v - pPolys) / 4);
					else if (v >= pA70 && v < pA70 + nA70 * 64)
						sprintf_s(pszOut, nOut, "-> the +0x70 array, byte %u", v - pA70);
					else if (v >= pA78 && v < pA78 + nA78 * 64)
						sprintf_s(pszOut, nOut, "-> the +0x78 array, byte %u", v - pA78);
				};

				// THE CHECK.
				//
				// Reading the dump gives a layout: +0x50 is a uint16 vertex
				// count, +0x54 begins an inline list of 24-byte entries, and
				// each entry's first dword points into the vertex array. That
				// is four separate claims from one hexdump, which is exactly
				// the evidence this project has twice found insufficient.
				//
				// A polygon is planar. So if the layout is right, every vertex
				// a polygon names lies in one plane - and if any of the four
				// claims is wrong, the "vertices" are arbitrary memory and will
				// not be coplanar. 5957 polygons cannot pass that by accident.
				{
					uint32_t nOK = 0, nPlanar = 0, nBadPtr = 0, nDegen = 0;
					uint32_t nMinV = 0xFFFFFFFF, nMaxV = 0; uint64_t nSumV = 0;
					for (uint32_t j = 0; j < nPolys; ++j)
					{
						const uint32_t po = Word(pPolys, j * 4);
						if (!Readable(po, 0x54)) continue;
						const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + 0x50);
						if (nv < 3 || nv > 256) { ++nBadPtr; continue; }
						if (!Readable(po + 0x54, nv * 24)) { ++nBadPtr; continue; }
						if (nv < nMinV) nMinV = nv;
						if (nv > nMaxV) nMaxV = nv;
						nSumV += nv;

						// Every entry's first dword must be an exact vertex.
						bool bPtrs = true;
						float vx[3][3]{};
						for (uint32_t t = 0; t < nv; ++t)
						{
							const uint32_t vp = Word(po + 0x54, t * 24);
							if (vp < pVerts || vp >= pVerts + nVerts * 12
								|| ((vp - pVerts) % 12)) { bPtrs = false; break; }
							if (t < 3 && Readable(vp, 12))
								memcpy(vx[t], (const void*)(uintptr_t)vp, 12);
						}
						if (!bPtrs) { ++nBadPtr; continue; }
						++nOK;

						// Coplanarity, from the first three.
						const float ax = vx[1][0] - vx[0][0], ay = vx[1][1] - vx[0][1],
									az = vx[1][2] - vx[0][2];
						const float bx2 = vx[2][0] - vx[0][0], by = vx[2][1] - vx[0][1],
									bz = vx[2][2] - vx[0][2];
						float nx = ay * bz - az * by, ny = az * bx2 - ax * bz,
							  nz = ax * by - ay * bx2;
						const float len = sqrtf(nx * nx + ny * ny + nz * nz);
						if (len < 1e-3f) { ++nDegen; continue; }
						nx /= len; ny /= len; nz /= len;
						bool bFlat = true;
						for (uint32_t t = 3; t < nv; ++t)
						{
							const uint32_t vp = Word(po + 0x54, t * 24);
							float q[3]; memcpy(q, (const void*)(uintptr_t)vp, 12);
							const float d = nx * (q[0] - vx[0][0]) + ny * (q[1] - vx[0][1])
										  + nz * (q[2] - vx[0][2]);
							if (d > 0.5f || d < -0.5f) { bFlat = false; break; }
						}
						if (bFlat) ++nPlanar;
					}
					Log("");
					Log("      --- is +0x50/+0x54 the vertex list? ---");
					Log("        %u of %u polygons have every entry landing exactly"
						" on a vertex", nOK, nPolys);
					Log("        %u of those are planar to within half a unit;"
						" %u degenerate", nPlanar, nDegen);
					Log("        vertices per polygon: %u..%u, mean %.2f",
						nMinV, nMaxV, nOK ? (double)nSumV / nOK : 0.0);
					Log("        POLYGON LAYOUT SELF-CHECK: %s",
						(nOK == nPolys && nPlanar + nDegen == nOK && nBadPtr == 0)
							? "PASSED - +0x50 is a uint16 vertex count, +0x54 an"
							  " inline list of 24-byte entries, first dword a vertex"
							: "FAILED - see the counts above");
				}

				for (int k = 0; k < 2; ++k)
				{
					const uint32_t po = Word(pPolys, k * 4);
					if (!Readable(po, 0x90)) continue;
					Log("");
					Log("      --- polygon %d at %08X ---", k, po);
					for (int o = 0; o < 0x90; o += 4)
					{
						const uint32_t v = Word(po, o);
						float f; memcpy(&f, &v, 4);
						char szAnn[64]; Annotate(v, szAnn, sizeof szAnn);
						if (szAnn[0])
							Log("        +%02X  %08X  %s", o, v, szAnn);
						else if (f > -1e6f && f < 1e6f && f != 0.0f
								 && (v & 0x7F800000) && (v & 0x7F800000) != 0x7F800000)
							Log("        +%02X  %08X  %.3f", o, v, f);
						else
							Log("        +%02X  %08X  %u", o, v, v);
					}
				}
			}
			break;
		}

		Log("    %s", nBest[0] > nBest[4] * 20
			? "one entry dwarfs the rest - that is the level itself"
			: "they are all the same order of magnitude, so none of them is the"
			  " level architecture");
	}

	const uint32_t nShow = nList < 4 ? nList : 4;
	for (uint32_t i = 0; i < nShow; ++i)
	{
		const uint32_t e = Word(pList, i * 4);
		const uint32_t sub = Word(e, 4);
		Log("");
		Log("    [%u] element %08X -> sub %08X  %s", i, e, sub, Classify(sub));
		if (i == 0)
		{
			Log("      the element itself:");
			LogWordsAndStrings("elem", e, 0x20);
			Log("      the sub-object, looking for a name:");
			LogWordsAndStrings("sub ", sub, 0xB0);
		}
		if (!Readable(sub, 0xA8)) { Log("      sub unreadable"); continue; }

		const uint32_t nRec = Word(sub, 0x8C), pRec = Word(sub, 0x88);
		const uint32_t nPtr = Word(sub, 0xA4), pPtr = Word(sub, 0xA0);
		Log("      0x30-byte records: %u at %08X   (stamp is uint16 at +0x2C)", nRec, pRec);
		Log("      pointer array:     %u at %08X   (stamp is uint16 at +0x42)", nPtr, pPtr);
		if (nRec && Readable(pRec, 0x60)) LogHex("rec0", pRec, 0x30);
		if (nPtr && Readable(pPtr, 4))
		{
			const uint32_t o = Word(pPtr, 0);
			Log("      first pointed-to object %08X  %s", o, Classify(o));
			if (Readable(o, 0x50)) LogHex("obj0", o, 0x50);
		}
	}
}

namespace
{
	// ------------------------------------------------------------------
	// Which eye is this?
	//
	// The renderer is handed two world scenes per frame with the SAME viewport
	// - docs/BOTH-EYES.md - so the eye has to come from the order, and the
	// order has to be verified rather than read out of the client. Counted,
	// with a denominator, because an asymmetric frustum applied to the wrong
	// eye is a defect the player would report as inverted stereo after
	// a headset session, and this costs nothing to settle at the desk.
	// ------------------------------------------------------------------
	int      g_nSceneInFrame  = 0;		// world scenes since the last SwapBuffers
	long     g_nFramesSeen    = 0;		// frames that contained at least one
	long     g_nFramesTwo     = 0;		// frames that contained exactly two
	long     g_nPairsRight    = 0;		// pairs where scene 0 is on the +right side
	long     g_nPairs         = 0;
	double   g_fSepSum        = 0.0;	// |offset| along the right vector
	double   g_fSepMin        = 1e30, g_fSepMax = -1e30;
	float    g_fPrevPos[3]    = { 0, 0, 0 };
	float    g_fPrevRight[3]  = { 0, 0, 0 };
	int      g_nPrevInFrame   = -1;

	// Which OpenXR eye the FIRST world scene of a frame belongs to, or -1
	// until a frame has shown it.
	//
	// Latched from the geometry rather than read from the client's pass
	// order, because the client can swap the eyes with a console variable
	// and the renderer cannot see console variables. The eye that sits on
	// the +right side of the pair IS the right eye, whatever order the
	// client chose to draw them in - there is nothing to agree about.
	int      g_nEyeOfScene0 = -1;

	// The camera's right vector out of the scene description's quaternion. Same
	// convention as render3d.cpp's QuatBasis, which draws a correct world with
	// it - so this is a transcription, not a second derivation.
	void CamRight(const float* q, float* r)
	{
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		r[0] = 1 - 2 * (y * y + z * z);
		r[1] = 2 * (x * y + w * z);
		r[2] = 2 * (x * z - w * y);
	}

	void NoteWorldScene(const float* pPos, const float* pQuat)
	{
		float r[3];
		CamRight(pQuat, r);

		if (g_nSceneInFrame == 0) ++g_nFramesSeen;
		if (g_nSceneInFrame == 1 && g_nPrevInFrame == 0)
		{
			// Scene 1 minus scene 0, along scene 0's right vector.
			const float d[3] = { pPos[0] - g_fPrevPos[0],
								 pPos[1] - g_fPrevPos[1],
								 pPos[2] - g_fPrevPos[2] };
			const float f = d[0]*g_fPrevRight[0] + d[1]*g_fPrevRight[1]
						  + d[2]*g_fPrevRight[2];
			++g_nPairs;
			// The client puts pass 1 (the right-half eye) at +IPD/2 along the
			// right vector and pass 2 at -IPD/2, so scene 1 minus scene 0 is
			// NEGATIVE when the order is what the client says it is.
			if (f < 0.0f) ++g_nPairsRight;
			// Scene 0 is the right eye when scene 1 sits to its LEFT.
			// OpenXR numbers the left eye 0.
			if (g_nEyeOfScene0 < 0 && (f > 0.01f || f < -0.01f))
			{
				g_nEyeOfScene0 = (f < 0.0f) ? 1 : 0;
				Log("  EYE: latched - the first world scene of a frame is the"
					" %s eye (offset %+.2f units along its own right vector)",
					g_nEyeOfScene0 ? "RIGHT" : "LEFT", f);
			}
			const double a = (f < 0) ? -f : f;
			g_fSepSum += a;
			if (a < g_fSepMin) g_fSepMin = a;
			if (a > g_fSepMax) g_fSepMax = a;
		}

		g_fPrevPos[0] = pPos[0]; g_fPrevPos[1] = pPos[1]; g_fPrevPos[2] = pPos[2];
		g_fPrevRight[0] = r[0]; g_fPrevRight[1] = r[1]; g_fPrevRight[2] = r[2];
		g_nPrevInFrame = g_nSceneInFrame;

		// The first few in full, so the pattern can be read rather than
		// summarised.
		static long s_nShown = 0;
		if (s_nShown < 8)
		{
			++s_nShown;
			Log("  EYE: frame scene %d  pos (%.1f %.1f %.1f)  quat"
				" (%.4f %.4f %.4f %.4f)  right (%.3f %.3f %.3f)",
				g_nSceneInFrame, pPos[0], pPos[1], pPos[2],
				pQuat[0], pQuat[1], pQuat[2], pQuat[3], r[0], r[1], r[2]);
		}
		++g_nSceneInFrame;
	}

	void EndFrameScenes()
	{
		if (g_nSceneInFrame == 2) ++g_nFramesTwo;
		g_nSceneInFrame = 0;
		g_nPrevInFrame = -1;
	}

}


// ---------------------------------------------------------------------------
// The crash handler.
//
// An access violation in a 25-year-old engine driving a renderer we wrote is
// not something the player can debug, and it is not something a log that stops
// mid-sentence can either. This names the faulting module and walks the frame
// pointer chain, labelling every return address with the module it lands in -
// which is the difference between "it crashed" and "it crashed in OUR code, in
// the blit path".
//
// Returns EXCEPTION_CONTINUE_SEARCH so the process still dies the way it would
// have. This only writes down what happened on the way.
// ---------------------------------------------------------------------------
static LONG WINAPI StubCrashFilter(EXCEPTION_POINTERS* pEP)
{
	if (!pEP || !pEP->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;

	const EXCEPTION_RECORD* pR = pEP->ExceptionRecord;
	const uint32_t nAt = (uint32_t)(uintptr_t)pR->ExceptionAddress;

	// EVERY module, now, before anything is labelled.
	NoteAllModules();
	char szW[80];

	Log("");
	Log("=== CRASH ===");
	Log("  code %08X at %08X  %s", (unsigned)pR->ExceptionCode, nAt,
		Where(nAt, szW, sizeof szW));
	// WHAT THE RENDERER THOUGHT IT WAS DOING. The module walk below can only
	// ever say "in d3dstub" - the whole frame is one function - so this is the
	// line that says WHICH pass, and it costs one pointer store per phase.
	Log("  the renderer was: %s   (after %ld completed world passes)",
		R3D_LastPhase(), R3D_PhaseFrames());

	if (pR->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
		&& pR->NumberParameters >= 2)
	{
		const ULONG_PTR nKind = pR->ExceptionInformation[0];
		const uint32_t  nAddr = (uint32_t)pR->ExceptionInformation[1];
		Log("  access violation %s address %08X  %s",
			(nKind == 0) ? "READING" : ((nKind == 1) ? "WRITING"
										: "EXECUTING (DEP)"),
			nAddr, Classify(nAddr));
	}

	if (pEP->ContextRecord)
	{
		const CONTEXT* c = pEP->ContextRecord;
		Log("  eip %08X esp %08X ebp %08X", c->Eip, c->Esp, c->Ebp);
		Log("  eax %08X ebx %08X ecx %08X edx %08X",
			c->Eax, c->Ebx, c->Ecx, c->Edx);
		Log("  esi %08X edi %08X", c->Esi, c->Edi);

		// The frame pointer chain. Optimised frames may be missing from it,
		// which is why every entry is checked for readability rather than
		// trusted - a wrong chain would otherwise crash the crash handler.
		Log("  frames (via ebp, each labelled by the module it lands in):");
		uint32_t nBp = c->Ebp;
		for (int i = 0; i < 16; ++i)
		{
			if (!nBp || IsBadReadPtr((const void*)(uintptr_t)nBp, 8)) break;
			const uint32_t nNext = *(const uint32_t*)(uintptr_t)nBp;
			const uint32_t nRet  = *(const uint32_t*)(uintptr_t)(nBp + 4);
			if (!nRet) break;
			Log("    [%2d] return %08X  %s", i, nRet,
				Where(nRet, szW, sizeof szW));
			if (nNext <= nBp) break;			// the chain must go up the stack
			nBp = nNext;
		}
	}
	// THE BYTES AT THE FAULTING ADDRESS. An address in no module and an address
	// in an unnamed one look identical in a register dump; the bytes do not.
	if (!IsBadReadPtr((const void*)(uintptr_t)nAt, 32))
	{
		char szB[160]; szB[0] = 0;
		for (int i = 0; i < 32; ++i)
		{
			char szOne[8];
			sprintf_s(szOne, "%02X ", ((const unsigned char*)(uintptr_t)nAt)[i]);
			strcat_s(szB, szOne);
		}
		Log("  bytes at the fault: %s", szB);
	}

	// WHO CALLED. The frame chain is only as good as the frame pointers; a scan
	// of the stack for values that land inside a real module finds the callers
	// whether or not anybody kept ebp.
	if (pEP->ContextRecord)
	{
		Log("  stack scan from esp - every word that lands in a module:");
		const uint32_t nSp = pEP->ContextRecord->Esp;
		int nSaid = 0;
		for (uint32_t o = 0; o < 0x800 && nSaid < 24; o += 4)
		{
			const uint32_t a = nSp + o;
			if (IsBadReadPtr((const void*)(uintptr_t)a, 4)) break;
			const uint32_t v = *(const uint32_t*)(uintptr_t)a;
			if (v < 0x10000) continue;
			bool bIn = false;
			for (int i = 0; i < g_nMods; ++i)
				if (v >= g_Mods[i].lo && v < g_Mods[i].hi) { bIn = true; break; }
			if (!bIn) continue;
			++nSaid;
			Log("    esp+%04X  %08X  %s", o, v, Where(v, szW, sizeof szW));
		}
	}
	Log("=== END CRASH ===");
	if (g_pLog) fflush(g_pLog);
	return EXCEPTION_CONTINUE_SEARCH;
}

static int   __cdecl s_RenderScene(void* pScene)
{
	SLOT(17);

	// The world walk fires on the FIRST call whose word 13 is non-null, not at
	// a call number chosen in advance. Which frame the world appears on is the
	// thing being measured, so it cannot also be an input: the menu and the
	// loaded level are hundreds of frames apart and a fixed dump point would
	// land on whichever one the timing happened to give.
	if (g_bWorldDump && pScene && !IsBadReadPtr(pScene, 224))
	{
		const uint32_t* s = (const uint32_t*)pScene;

		g_nType[s[0] < 7 ? s[0] : 7]++;
		if (!s[13])                 g_nW13Zero++;
		else if (s[13] < 0x10000)   g_nW13Small++;
		else                        g_nW13Ptr++;

		// The real renderer takes the world path only when word 0 is 1
		// (d3d.ren + 0x1EBE5, `cmp eax, 1`), and it needs word 13 to be a
		// pointer. Both are dumped separately so a run says which of the two
		// conditions is the one we never meet.
		if (!g_bType1Dumped && s[0] == 1)
		{
			g_bType1Dumped = 1;
			Log("");
			Log("first scene with word 0 == 1 - the real renderer's world path");
			LogWorldWalk(s);
		}
		if (!g_bPtrDumped && s[13] >= 0x10000)
		{
			g_bPtrDumped = 1;
			Log("");
			Log("first scene whose word 13 is a real pointer");
			LogWorldWalk(s);
		}
		if (!g_bWorldSeen && s[13])
		{
			g_bWorldSeen = 1;
			Log("");
			Log("scene word 13 first non-null on RenderScene call %d, value %08X",
				g_nCalls[17], s[13]);
		}
		// One dump from the menu as well, so "null at the menu" is a recorded
		// measurement rather than an absence of evidence.
		if (!g_bMenuDumped && g_nCalls[17] == 60)
		{
			g_bMenuDumped = 1;
			Log("");
			Log("--- the same walk at call 60, before any level is loaded ---");
			LogWorldWalk(s);
		}
	}

	// Draw the world.
	//
	// Every input comes out of the scene description at an offset measured in
	// docs/SCENEDESC.md and re-confirmed by the word-by-word dump above:
	// position at word 48, rotation quaternion at word 51, the FULL field
	// angles at words 45 and 46, near and far at words 26 and 47, and the
	// viewport rectangle at words 41..44 - which is what makes a side-by-side
	// eye pair two calls that differ only in that rectangle.
	if (g_bDraw3D && pScene && !IsBadReadPtr(pScene, 224))
	{
		const uint32_t* sd = (const uint32_t*)pScene;
		// Scene word 0 is 1 for the world and 2 for the INTERFACE scene - the
		// live 3D behind the main menu, which NOLF draws into a 640x480 panel
		// and stretches across the middle of the screen. Skipping type 2 is why
		// that panel is black: the engine letterboxes the menu with two 261x1000
		// filler bars either side and blits the panel between them, and we drew
		// the bars and nothing in the middle.
		//
		// IT WORKS NOW, AND IT IS STILL OFF BY DEFAULT.
		//
		// The reason it used to be off - "the interface scene may not reach its
		// geometry the same way the world does" - is answered: the client
		// publishes the menu's models (VRPublishModels runs in GS_FOLDER now,
		// around the INTERFACE camera), R3D_DrawWorld no longer turns round at
		// the door when there is no world, and with +StubDrawInterface 1
		// **Cate Archer draws on the main menu** for the first time.
		//
		// It used to appear in ONE EYE, which is why this stayed off: the
		// world is rendered twice a frame by the eye loop in GameClientShell
		// and the interface scene was rendered once, by
		// CInterfaceMgr::UpdateInterfaceSFX, so only one half of the frame got
		// her. Content in a single eye is binocular rivalry - uncomfortable
		// rather than merely wrong, and worse than the black it replaced.
		//
		// That is now done at the source. UpdateInterfaceSFX renders the scene
		// into each half of the side-by-side frame, with the same camera both
		// times, so the two halves are identical - which is what a backdrop
		// presented on a flat world-locked panel wants. ON by default.
		//
		// +StubDrawInterface 0 to go back to a black menu.
		if (sd[0] == 1 || (g_bDrawInterface && sd[0] == 2))
		{
			NoteWorldScene(&((const float*)pScene)[48],
						   &((const float*)pScene)[51]);
			// Built here rather than in RebindLightmaps. That call runs before
			// a single texture has been bound, so a world built there gets no
			// textures at all - measured: 0 created, 26904 polygons white.
			R3D_BuildWorld(g_pWorld);

			// The eye's own frustum, when there is one and we have been told
			// to use it. Every one of these can fail independently and each
			// failure falls back to the symmetric projection rather than to a
			// wrong one: no host, an old host, a host that has stopped, a
			// frame before the eye order was latched, or a nonsense frustum.
			const float* pTan = nullptr;
			float aTan[4];
			// NoteWorldScene has already counted this one, so the first scene
			// of a frame leaves the counter at 1 and the second at 2. Written
			// out rather than left to be worked out: the first native draw of
			// a run is the SECOND scene of the frame that latched the order,
			// which makes the log look inverted until that is noticed.
			// WHICH HALF OF THE FINISHED FRAME THIS SCENE LANDS IN.
			//
			// For the WORLD it is not its viewport: the client renders both
			// eyes into the left half and blits the first one across
			// afterwards, so the first scene of a frame ends up on the right.
			//
			// The INTERFACE scene is not part of that eye loop and nothing
			// blits it. CInterfaceMgr renders it straight into each half in
			// turn, so for a menu the viewport IS the half - and reading it
			// the world's way put the first menu scene, treated as the right
			// eye, into the left half of the frame. That is a full eye swap
			// PLUS both optical-centre offsets applied backwards, which is
			// why the pause menu's two halves looked shifted apart by far
			// more than an IPD once the backdrop became the level itself.
			const int nHalfThis = (sd[0] == 2)
				? (((int)sd[41] >= ((int)sd[43] - (int)sd[41])) ? 1 : 0)
				: ((g_nSceneInFrame == 1) ? 1 : 0);

			// Half 1 - the right of the frame - is whichever eye scene 0 was.
			const int nEyeThis = (g_nEyeOfScene0 < 0) ? -1
				: (nHalfThis ? g_nEyeOfScene0 : 1 - g_nEyeOfScene0);
			if (g_bNativeFrustum)
			{
				VRB_Poll(Log);
				const int nEye = nEyeThis;
				VRBFrustum fr{};
				if (nEye >= 0 && VRB_EyeFrustum(nEye, &fr))
				{
					aTan[0] = fr.fTanL; aTan[1] = fr.fTanR;
					aTan[2] = fr.fTanU; aTan[3] = fr.fTanD;
					pTan = aTan;
				}
				else
				{
					// This scene is about to be drawn with the SYMMETRIC
					// projection, which is a wrong frame rather than a missing
					// offset - so it is counted, and the first few say why.
					// Measured at about one frame in a thousand; the cause was
					// never established because nothing recorded it.
					const long n = InterlockedIncrement(&g_nNativeMiss);
					const char* pszWhy = (nEye < 0)
						? "no eye latched for this scene" : VRB_LastFail();
					// The first version logged the first five and stopped, so
					// the ten misses at level load hid every later one and
					// their reasons with them. Log each DISTINCT reason once
					// instead - the same mistake the crosshair log made an
					// hour earlier, and the same fix.
					if (pszWhy != g_pszLastMissWhy)
					{
						g_pszLastMissWhy = pszWhy;
						Log("  native frustum MISS %ld: eye %d, %s", n, nEye, pszWhy);
					}
				}
				VRB_PublishNativeFrustum(pTan != nullptr);
			}

			// WHERE STRAIGHT AHEAD IS in each half of the finished frame, so
			// the 2D layer can be centred on it rather than on the middle of
			// the half - about 15 degrees apart, which is the finding the
			// per-eye crosshair already had to be moved for.
			//
			// Which half this scene ends up in is NOT a fact about its
			// viewport. The client renders BOTH eyes into the left half and
			// moves the first one across afterwards with a half-rect blit
			// (GameClientShell.cpp: pass 1, then DrawSurfaceToSurface(hScreen,
			// hStash, &rHalf, nHalfWidth, 0), then pass 2). So the FIRST scene
			// of a frame lands on the right and the second stays on the left -
			// which is also what this renderer measures independently, in the
			// line that reports scene 0 on the +right side.
			//
			// The first attempt read the half off the scene's viewport
			// rectangle and got half 0 for both eyes, every frame. It cost a
			// run, and the log said so plainly: two different frusta published
			// to the same half.
			if (pTan)
			{
				const float tl = pTan[0], tr = pTan[1], tu = pTan[2], td = pTan[3];
				if (tr > tl && tu > td)
					R2D_SetEyeCentre(nHalfThis,
									 -(tr + tl) / (tr - tl),
									 -(tu + td) / (tu - td));
			}

			const float* f = (const float*)pScene;
			// WHAT THE ENGINE ASKED FOR, for each KIND of scene separately.
			// The existing camera report is gated on the world's own sample
			// vertices, so at the main menu - where there is no world - it never
			// fires, and the interface scene's viewport and field angles have
			// never once been printed.
			{
				static long s_nSaidScene[3] = { 0, 0, 0 };
				const int nKind = (sd[0] == 2) ? 2 : 1;
				if (s_nSaidScene[nKind] < 2)
				{
					++s_nSaidScene[nKind];
					Log("  SCENE type %u (%s): viewport %d,%d..%d,%d (%dx%d)"
						"  fov %.2f x %.2f deg (aspect %.4f, tan %.4f/%.4f)"
						"  near %.2f far %.1f  pos (%.2f %.2f %.2f)"
						"  quat (%.4f %.4f %.4f %.4f)",
						sd[0], (nKind == 2) ? "THE INTERFACE" : "the world",
						(int)sd[41], (int)sd[42], (int)sd[43], (int)sd[44],
						(int)sd[43] - (int)sd[41], (int)sd[44] - (int)sd[42],
						f[45] * 57.2958f, f[46] * 57.2958f,
						(f[46] > 0.0f) ? (float)(tan(f[45] * 0.5) / tan(f[46] * 0.5))
									   : 0.0f,
						(float)tan(f[45] * 0.5), (float)tan(f[46] * 0.5),
						f[26], f[47], f[48], f[49], f[50],
						f[51], f[52], f[53], f[54]);
				}
			}
			// THE INTERFACE SCENE IS AUTHORED 4:3 AND THE ENGINE PILLARBOXES IT.
			//
			// It declares 90 x 75 degrees - tangents 1.0000 and 0.7673, aspect
			// 1.3032 - and its viewport is the WHOLE 2560x1384 frame, aspect
			// 1.8497. Drawing it there stretches the menu horizontally by 1.39,
			// which is measurable and was measured: Cate's suit is 0.4355 of the
			// frame wide here against 0.2980 in retail, while her HEIGHT and her
			// top edge match retail to three decimals. Vertical was never wrong.
			//
			// Retail does not stretch it. It draws the scene into a CENTRED 4:3
			// region and covers the rest with two filler bars, and that is not a
			// guess: in a retail capture the left bar is x 0..356 and the picture
			// runs 357..2202, which is 1846 wide against 1384 tall - 4:3 to the
			// pixel. THOSE BARS ARE ALREADY DRAWN HERE, by the 2D layer, over the
			// stretched picture - so the region has to match them exactly or a
			// sliver of scene shows past their edge. 4:3, not the scene's own
			// 1.3032, for that reason.
			//
			// +StubIfaceAspect 0 draws it stretched across the whole viewport as
			// before; the value is the aspect in hundredths.
			int vL = (int)sd[41], vT = (int)sd[42];
			int vR = (int)sd[43], vB = (int)sd[44];
			// NOT WHEN A LEVEL IS LOADED. The pillarbox exists because the MAIN
			// menu's scene is authored 4:3 and the engine covers the rest with
			// filler bars. The PAUSE menu is the same scene type over a world
			// that is about to be drawn at the headset's own aspect, and boxing
			// that would put the level in a 4:3 window inside the eye.
			if (sd[0] == 2 && g_nIfaceAspect > 0 && !R3D_HaveWorld())
			{
				const int vw = vR - vL, vh = vB - vT;
				const float fWant = (float)g_nIfaceAspect * 0.01f;
				if (vw > 0 && vh > 0 && (float)vw > (float)vh * fWant)
				{
					const int nw = (int)((float)vh * fWant + 0.5f);
					const int nl = vL + (vw - nw) / 2;
					static long s_nSaidBox = 0;
					if (s_nSaidBox < 2)
					{
						++s_nSaidBox;
						Log("  INTERFACE pillarbox: %dx%d viewport -> %d,%d..%d,%d"
							" (%dx%d, aspect %.4f), bars %d px either side",
							vw, vh, nl, vT, nl + nw, vB, nw, vh, fWant,
							(vw - nw) / 2);
					}
					vL = nl; vR = nl + nw;
				}
			}
			// THE SCOPE FIRST, once a frame, so both eyes' lens discs sample a
			// finished picture. Its pass carries this scene's near and far.
			if (sd[0] == 1 && g_nSceneInFrame == 1) R3D_DrawScopePass();
			R3D_DrawWorld(&f[48], &f[51], f[45], f[46], f[26], f[47],
						  vL, vT, vR, vB,
						  pTan, nEyeThis, (sd[0] == 2) ? 1 : 0);
		}
	}

	// Which call to dump is a switch, not a rebuild: the menu and a loaded
	// world are different scenes and the interesting one moves.
	if (g_nCalls[17] != g_nSceneDumpAt || !pScene || IsBadReadPtr(pScene, 32))
		return g_bTrace17 ? RealScene(pScene) : 0;

	// The six words stayed zero through 1861 rendered world frames, and zero
	// again while the REAL renderer drew the world and a background thread
	// watched them. Draw-list heads would not do that. Transient per-scene
	// state - written at the top of RenderScene and cleared at the bottom -
	// would, and a sampler between frames could never catch it. So read them
	// either side of the real call, from inside the call.
	if (g_bTrace17 && g_bHaveReal && g_pRealSlot[17])
	{
		Log("");
		Log("=== the six words, ACROSS the real RenderScene ===");
		LogSixWords("before");
		const int r = RealScene(pScene);
		LogSixWords("after ");
		Log("=== end ===");
		return r;
	}

	const uint32_t* s = (const uint32_t*)pScene;
	Log("");
	Log("=== the scene description's six pointers, RenderScene call %d ===",
		g_nSceneDumpAt);
	Log("  the engine hands these to every RenderScene; whatever they lead to");
	Log("  is what our renderer has to draw.");

	for (int i = 1; i <= 6; ++i)
	{
		const uint32_t nPtr = s[i];
		Log("");
		Log("  word %d (offset %d) = %08X  %s", i, i * 4, nPtr, Classify(nPtr));
		if (!nPtr || IsBadReadPtr((const void*)(uintptr_t)nPtr, 4)) { Log("    unreadable"); continue; }

		const uint32_t nHead = *(const uint32_t*)(uintptr_t)nPtr;
		Log("    -> holds %08X  %s", nHead, Classify(nHead));
		if (!nHead || IsBadReadPtr((const void*)(uintptr_t)nHead, 64))
		{
			Log("    (empty list, or not a pointer)");
			continue;
		}

		const uint32_t* t = (const uint32_t*)(uintptr_t)nHead;
		for (int w = 0; w < 16; ++w)
		{
			float f; memcpy(&f, &t[w], 4);
			Log("      +%3d  %08X  %11d  %12.3f  %s",
				w * 4, t[w], (int)t[w], f, Classify(t[w]));
		}
	}
	Log("=== end list pointers ===");
	return 0;
}
static void  __cdecl s_RenderCommand(int, char**) { SLOT(18); }
static void* __cdecl s_GetDDInterface(const char*){ SLOT(19); return nullptr; }
static void  __cdecl s_SwapBuffers(uint32_t)      { SLOT(20);
	EndFrameScenes();
	R3D_NotePresent();		// the frame boundary the FPS line counts
	{ LARGE_INTEGER qw; QueryPerformanceCounter(&qw); g_qWatchTick = qw.QuadPart; }
	// Everything from here to the end of this slot - the shared-texture
	// publish, the frame limiter, Present - is timed into the stall line's
	// "present slot" bucket. The limiter is the suspect with a Sleep() in it.
	struct PresentSlotTimer
	{
		LARGE_INTEGER q0;
		PresentSlotTimer() { QueryPerformanceCounter(&q0); }
		~PresentSlotTimer()
		{
			LARGE_INTEGER q1, f;
			QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&f);
			if (f.QuadPart)
				R3D_AddPresentMs((double)(q1.QuadPart - q0.QuadPart) * 1000.0
								 / (double)f.QuadPart);
		}
	} _presentSlotTimer;
	if (g_pSwap)
	{
		// Hand the frame over BEFORE Present, so the host is offered exactly
		// the image the monitor is about to get and never one frame behind it.
		// The held frame marker goes on last, over the fade and the menu.
		DrawHeldMarker();

		if (g_bEyeShare)
		{
			// Attach to the block here as well as in the frustum path. It used
			// to be polled ONLY under +StubNativeFrustum, so the shared
			// texture silently published into nothing whenever that switch was
			// off - the renderer's own counter said "765 published" while the
			// block still read serial 0. Two features, one hidden dependency.
			VRB_Poll(&Log);

			ID3D11Texture2D* pBack = nullptr;
			if (SUCCEEDED(g_pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D),
											 (void**)&pBack)) && pBack)
			{
				R3D_GpuTailStamp(1);
				ES_Publish(pBack);
				R3D_GpuTailStamp(2);
				// SUBMIT IT NOW. Without this the copy into the shared texture
				// sat in the command buffer through the limiter's wait and went
				// to the GPU with the Present, 6-8 ms later than it could have -
				// the host saw the frame that much later, and the GPU timer
				// counted the idle gap as "publish 6.2 ms".
				R3D_GpuFrameEnd();		// the GPU work ends here; the limiter wait is not work
				g_pCtx->Flush();
				pBack->Release();
			}
			uint32_t nW = 0, nH = 0, nFmt = 0, nSer = 0;
			int32_t nLo = 0, nHi = 0;
			if (ES_Describe(&nW, &nH, &nFmt, &nLo, &nHi))
			{
				ES_Stats(nullptr, nullptr, &nSer);
				VRB_PublishEyeTex(nSer, nW, nH, nFmt, nLo, nHi);
			}
			// THE PAUSE OVERLAY, if the 2D layer drew into it this frame.
			{
				ID3D11Texture2D* pOvl = nullptr; int nOW = 0, nOH = 0;
				const int bOvl = R2D_OverlayEndFrame(&pOvl, &nOW, &nOH);
				if (bOvl && pOvl) ES_PublishOverlay(pOvl);
				// +StubDumpOverlay 1 writes the overlay on the 30th frame of each
				// pause, for eight pauses, so the desk can see what the quad
				// carries - the black boxes lived there. Once per session was not
				// enough: a black box reported after loading a save came on a
				// later pause than the first.
				static int s_nOvlDumped = 0;
				if (bOvl && pOvl && g_bDumpOverlay && s_nOvlDumped < 8 && R2D_OverlayUpRun() == 30)
				{
					++s_nOvlDumped;
					char szP[MAX_PATH];
					sprintf_s(szP, "%slogs/overlay-%d.bmp", g_szDir, s_nOvlDumped);
					Log("  overlay dump: %s", R3D_DumpBackBuffer(pOvl, szP) ? szP : "FAILED");
					sprintf_s(szP, "%slogs/overlay-%d-alpha.bmp", g_szDir, s_nOvlDumped);
					R3D_SetDumpAlphaAsGrey(1);
					Log("  overlay alpha dump: %s", R3D_DumpBackBuffer(pOvl, szP) ? szP : "FAILED");
					R3D_SetDumpAlphaAsGrey(0);
				}
				static int s_bWasOvl = 0;
				if (bOvl != s_bWasOvl)
				{
					s_bWasOvl = bOvl;
					Log("  pause overlay %s (%dx%d)", bOvl ? "UP - the pause menu is a quad now" : "down", nOW, nOH);
				}
				VRB_PublishOverlay(ES_OverlaySerial(), (uint32_t)nOW, (uint32_t)nOH, bOvl);
			}
		}

		// The frame, as the renderer drew it, at a chosen present. The desk's
		// window capture returns black whenever the game window is occluded or
		// the desktop is rearranged - which happened mid-session and quietly
		// turned two A/B runs into two black images that agreed perfectly.
		// A RANGE of presents, not one. A single frame cannot show a flicker;
		// two frames from a frozen pose can, because anything that differs
		// between them is the renderer being non-deterministic rather than the
		// scene changing. Z-fighting, alternating draw order and uninitialised
		// state all look like this and nothing else does.
		// Wait for a model to be in front of the camera, then dump. Timing
		// this by hand cost four runs and never produced one: at the quick
		// save the nearest model is 1691 units away at az -53, just outside
		// the eye's field, and no amount of --static moved it in.
		// Re-arms, so a player driving the game gets several chances instead of
		// one. The near cut-off skips the player's own weapon and attachments,
		// which sit at the camera and were satisfying the test at 22 units
		// while the actual NPCs were nowhere in view.
		if (g_bDumpOnModels && g_nFrameDumpAt == 0 && g_nPresents >= g_nDumpAfter
			&& s_nDumpsDone < g_nDumpMax && g_nPresents >= s_nRearmAt)
		{
			float fAz = 999.0f, fDist = 1e30f;
			R3D_NearestModel(&fAz, &fDist);
			if (fAz < 25.0f && fDist > 80.0f && fDist < 2000.0f)
			{
				g_nFrameDumpAt = g_nPresents + 1;
				g_nFrameDumpCount = 1;
				++s_nDumpsDone;
				s_nRearmAt = g_nPresents + 270;		// ~3 s at 90 fps
				Log("  frame dump ARMED (%d of 8): a model is %.0f units away"
					" at az %.0f - dumping present %ld",
					s_nDumpsDone, fDist, fAz, g_nFrameDumpAt);
			}
		}
		// let the next arm happen once this dump has been written - ALL of
		// it: this used to reset one present after the start, so a count above
		// one never wrote more than the first frame.
		if (g_nFrameDumpAt > 0 && g_nPresents >= g_nFrameDumpAt + g_nFrameDumpCount)
			g_nFrameDumpAt = 0;

		// +StubFrameDumpEvery N writes every Nth present for the whole run -
		// a scripted cutscene is forty seconds of camera cuts, and one frame
		// per second on a sheet is how an unexplained glitch in the intro gets
		// a name.
		const bool bEvery = (g_nFrameDumpEvery > 0 && (g_nPresents % g_nFrameDumpEvery) == 0);
		if (bEvery || (g_nFrameDumpAt > 0 && g_nPresents >= g_nFrameDumpAt &&
			g_nPresents < g_nFrameDumpAt + g_nFrameDumpCount))
		{
			ID3D11Texture2D* pBack = nullptr;
			if (g_pSwap && SUCCEEDED(g_pSwap->GetBuffer(
					0, __uuidof(ID3D11Texture2D), (void**)&pBack)) && pBack)
			{
				char szPath[MAX_PATH];
				sprintf_s(szPath, "%slogs/frame-%ld.bmp", g_szDir, g_nPresents);
				Log("  frame dump at present %ld: %s", g_nPresents,
					R3D_DumpBackBuffer(pBack, szPath) ? szPath : "FAILED");
				pBack->Release();
			}
		}

		// One texture read back per frame while the dump is running. See
		// R3D_DumpNextTexture: several in one callback resets the device.
		// A model skin the engine never binds, fetched between frames rather
		// than inside the mesh build - doing it there killed world rendering.
		R3D_PullQueuedSkin();
		R3D_RefreshStaleTexture();

		if (g_bTexDumpAll && !g_bTexDumpedAll && R3D_FloorTexture())
		{
			char szDir[MAX_PATH];
			sprintf_s(szDir, "%stexdump/", g_szDir);
			CreateDirectoryA(szDir, nullptr);
			if (!R3D_DumpNextTexture(szDir, Log))
			{
				g_bTexDumpedAll = 1;
				Log("  R3D: texture dump complete -> %s", szDir);
			}
		}

		// Hold the frame rate. NOT a performance setting: LithTech advances a
		// FIXED timestep per rendered frame, so game time is 11 ms times the frame
		// count and the simulation runs at real speed only near 90 FPS. At 1400
		// FPS the game runs about fifteen times too fast and a handgun fires like
		// a machine gun.
		//
		// Present(0,0) never throttled - the compositor did, and only while the
		// window sat normally on one monitor. Game speed must not depend on where
		// the window happens to sit. +StubMaxFps 0 removes the cap.
		LARGE_INTEGER qB, qC, qD, qF;
		QueryPerformanceFrequency(&qF);
		QueryPerformanceCounter(&qB);		// after publish, before the limiter
		// PACED BY THE HEADSET, NOT BY A CLOCK OF OUR OWN.
		//
		// The cap below holds the game at 90.0 by this machine's counter while
		// the host presents at the headset's own 90 - two clocks, no phase
		// lock, and a beat between them. A host log: "739 fresh, 149
		// repeated (17% repeated) <- REPEATS ARE THE STUTTER" in one ten-second
		// window and 2% in the next, drifting through the beat. The client log
		// shows the game itself never dropping below 84 fps.
		//
		// So while a host is live, wait for ITS frame counter to advance -
		// written once per xrWaitFrame - and present straight after. The
		// frame this Present publishes then has a whole host period to be
		// picked up, and every host frame gets a fresh one as long as the game
		// renders inside that period. If the counter has already moved on
		// since the last wait, this frame is late: present at once, no wait.
		// A host that stops ticking is given a frame and a half, then the cap
		// below takes over as before. +StubSyncHost 0 restores the old cap.
		bool bSynced = false;
		if (g_bSyncHost && g_nMaxFps > 0 && VRB_Live())
		{
			uint32_t nHost = 0;
			if (VRB_HostFrame(&nHost))
			{
				static uint32_t s_nLastHost = 0;
				static int      s_bHaveHost = 0;
				static long     s_nWaited = 0, s_nLate = 0, s_nTimedOut = 0, s_nReport = 0;
				static double   s_fWaitMs = 0.0;
				bSynced = true;
				LARGE_INTEGER l0; QueryPerformanceCounter(&l0);
				const LONGLONG llLimit = (qF.QuadPart * 3) / (2 * g_nMaxFps);
				if (s_bHaveHost && nHost == s_nLastHost)
				{
					for (;;)
					{
						LARGE_INTEGER l1; QueryPerformanceCounter(&l1);
						if (l1.QuadPart - l0.QuadPart > llLimit) { ++s_nTimedOut; break; }
						VRB_HostFrame(&nHost);
						if (nHost != s_nLastHost)
						{
							++s_nWaited;
							s_fWaitMs += (double)(l1.QuadPart - l0.QuadPart) * 1000.0 / (double)qF.QuadPart;
							break;
						}
						Sleep(0);
					}
				}
				else if (s_bHaveHost) ++s_nLate;
				s_nLastHost = nHost;
				s_bHaveHost = 1;
				if (++s_nReport % 900 == 0)
					Log("  host sync: %ld frames waited for the host tick (avg %.2f ms),"
						" %ld were already late (no wait), %ld timed out (host silent)",
						s_nWaited, s_nWaited ? s_fWaitMs / (double)s_nWaited : 0.0,
						s_nLate, s_nTimedOut);
			}
		}
		if (!bSynced && g_nMaxFps > 0)
		{
			static LARGE_INTEGER s_liFreq = { 0 };
			static LARGE_INTEGER s_liNext = { 0 };
			if (!s_liFreq.QuadPart) QueryPerformanceFrequency(&s_liFreq);
			const LONGLONG llBudget = s_liFreq.QuadPart / g_nMaxFps;
			LARGE_INTEGER li; QueryPerformanceCounter(&li);
			if (!s_liNext.QuadPart) s_liNext.QuadPart = li.QuadPart;
			s_liNext.QuadPart += llBudget;
			// A long stall - a level load, a frame dump - must not leave us owing
			// time and then racing to catch it up.
			if (li.QuadPart > s_liNext.QuadPart) s_liNext.QuadPart = li.QuadPart;
			else for (;;)
			{
				QueryPerformanceCounter(&li);
				const LONGLONG llLeft = s_liNext.QuadPart - li.QuadPart;
				if (llLeft <= 0) break;
				// Sleep's granularity is about 15 ms, so sleeping the whole budget
				// would overshoot an 11 ms frame and halve the rate. Sleep the
				// coarse part, spin the rest.
				const LONGLONG llMs = (llLeft * 1000) / s_liFreq.QuadPart;
				if (llMs > 2) Sleep((DWORD)(llMs - 2)); else Sleep(0);
			}
		}

		QueryPerformanceCounter(&qC);		// after the limiter, before Present
		// THE DESKTOP WINDOW DOES NOT NEED EVERY FRAME. With the shared frame
		// open the host never looks at this window, yet Present() hands each
		// frame to the desktop compositor, which runs at the monitor's rate
		// and blocks us when its queue is full - the watchdog caught the main
		// thread inside Present via dxgi -> d3d11 -> nvwgf2um -> a kernel
		// wait, 46 ms at a time. While a host is live, present one frame in
		// StubPresentEvery (default 6) and Flush the rest so the published
		// copy still reaches the GPU at once. +StubPresentEvery 1 is the old way.
		// 0 = NEVER while the host is live: the tester asked for no desktop
		// mirror at all, only the headset. The window keeps its last frame.
		// The host's own -Mirror shows ONE eye if a desktop view is wanted.
		static long s_nPresentTurn = 0;
		const bool bLiveNow = VRB_Live();
		const bool bPresentThis = !bLiveNow || g_nPresentEvery == 1
							   || (g_nPresentEvery > 1 && (++s_nPresentTurn % g_nPresentEvery) == 0);
		HRESULT hr = S_OK;
		if (bPresentThis) hr = g_pSwap->Present(0, 0);
		else { g_pCtx->Flush(); InterlockedIncrement(&g_nPresentsSkipped); }
		QueryPerformanceCounter(&qD);
		// THE PRESENT SLOT, IN THREE PARTS. The stall line said the whole slot
		// was 100 ms in the menu and could not say which third; this can.
		if (qF.QuadPart)
		{
			const double k = 1000.0 / (double)qF.QuadPart;
			R3D_NotePresentParts((double)(qC.QuadPart - qB.QuadPart) * k,
								 (double)(qD.QuadPart - qC.QuadPart) * k);
		}
		if (FAILED(hr))
		{
			if (InterlockedIncrement(&g_nPresentFails) <= 3)
				Log("  SwapBuffers: Present FAILED hr=%08X", (unsigned)hr);
		}
		else InterlockedIncrement(&g_nPresents);
	} }
static int   __cdecl s_GetInfoFlags(void)         { SLOT(21); return 0; }

// Returns 3. Measured on 2 September against the real renderer, and it is the
// one place the stub's guessed contract was actually wrong: this was declared
// void, so the engine read whatever happened to be in EAX. Everything else the
// stub returns - Init 0, Start3D 1, End3D 1, StartOptimized2D 1,
// EndOptimized2D 0, OptimizeSurface 1, IsIn3D the state, the surface calls a
// pointer - matches the real renderer exactly.
//
// 3 is very likely the format type, since it is also what the real renderer
// writes into the PFormat's second word.
static int   __cdecl s_GetScreenFormat(void* pFmt)
{
	SLOT(22);
	// Exactly what the real renderer reports - measured, docs/SCENEDESC.md.
	// 32-bit XRGB8888.
	if (!pFmt || IsBadWritePtr(pFmt, 52))
	{
		Log("  GetScreenFormat: unwritable argument %p - returning 3 anyway", pFmt);
		return 3;
	}
	uint32_t* f = (uint32_t*)pFmt;
	f[1]  = 3;			// format type, as the real one reported
	f[3]  = 0x00FF0000; f[4] = 0x0000FF00; f[5] = 0x000000FF;
	f[7]  = 8; f[8] = 8; f[9] = 8;
	f[10] = 32;
	f[11] = 16; f[12] = 8;
	return 3;
}

static bool Trace2DOn()
{
	if (g_nTrace2D >= 2) return true;
	if (g_nTrace2D == 1 && g_nFrameDumpAt > 0
		&& g_nPresents >= g_nFrameDumpAt - 2
		&& g_nPresents < g_nFrameDumpAt + g_nFrameDumpCount) return true;
	return false;
}

static void* __cdecl s_CreateSurface(int nW, int nH)
{
	SLOT(23);

	// The (width, height) signature was inferred in Phase 0, never checked.
	// The raw stack says what was actually pushed; if a third word is a
	// consistent small number it is a format or a flag and the signature is
	// wrong. Logged for the first few only - 93 of these happen at load.
	if (g_nCalls[23] <= 3)
	{
		const uint32_t* a = RAW_ARGS();
		Log("  CreateSurface raw args: %08X %08X %08X %08X  (declared %d x %d)",
			a[0], a[1], a[2], a[3], nW, nH);
	}

	if (nW <= 0 || nH <= 0 || nW > 8192 || nH > 8192) return nullptr;
	StubSurface* p = (StubSurface*)calloc(1, sizeof(StubSurface));
	if (!p) return nullptr;
	p->nMagic  = kSurfMagic;
	p->nWidth  = (uint32_t)nW;
	p->nHeight = (uint32_t)nH;
	p->nPitch  = nW * 4;
	p->pBits   = calloc((size_t)nW * nH, 4);
	if (Trace2DOn()) Log("  T2D p=%ld CREATE %p %dx%d", (long)g_nPresents, p, nW, nH);
	if (!p->pBits) { free(p); return nullptr; }
	InterlockedIncrement(&g_nSurfacesLive);
	NoteSurface(p, p->nWidth, p->nHeight);
	return p;
}

static void __cdecl s_DeleteSurface(void* h)
{
	SLOT(24);
	StubSurface* p = Check(h);
	if (!p) return;
	if (Trace2DOn()) Log("  T2D p=%ld DELETE %p %ux%u", (long)g_nPresents, h, p->nWidth, p->nHeight);
	R2D_Forget(h);
	free(p->pBits);
	p->nMagic = 0;
	free(p);
	InterlockedDecrement(&g_nSurfacesLive);
}

static void __cdecl s_GetSurfaceInfo(void* h, uint32_t* pW, uint32_t* pH, int* pPitch)
{
	SLOT(25);
	StubSurface* p = Check(h);
	if (pW)     *pW     = p ? p->nWidth  : 0;
	if (pH)     *pH     = p ? p->nHeight : 0;
	if (pPitch) *pPitch = p ? p->nPitch  : 0;
}

static void* __cdecl s_LockSurface(void* h)
{
	SLOT(26); StubSurface* p = Check(h);
	if (Trace2DOn()) Log("  T2D p=%ld LOCK   %p", (long)g_nPresents, h);
	return p ? p->pBits : nullptr;
}

// The engine has just finished writing into the surface, so whatever texture
// we are holding for it is stale. This is the only notification there is -
// there is no "surface changed" callback - which is why the upload is driven
// from here rather than from the blit.
static void  __cdecl s_UnlockSurface(void* h)
{
	SLOT(27);
	if (Trace2DOn()) Log("  T2D p=%ld UNLOCK %p", (long)g_nPresents, h);
	R2D_Invalidate(h);
}

// The second parameter is the surface's TRANSPARENT COLOUR and it used to be
// unnamed and thrown away, which is where the player's magenta spots came from.
static int   __cdecl s_Optimize(void* h, uint32_t nTransparent)
{
	SLOT(28);
	if (Trace2DOn()) Log("  T2D p=%ld OPTIMIZE %p key %08X", (long)g_nPresents, h, (unsigned)nTransparent);
	// THE CENSUS. Which surfaces are optimized, how big they are, and with
	// what transparent colour - because text drawing in a black box is a claim
	// about a particular set of surfaces and this is the only place that set
	// is visible. Sized, so a 16x16 glyph sheet can be told from a backdrop.
	{
		static int s_nSaid = 0;
		if (s_nSaid < 60)
		{
			++s_nSaid;
			StubSurface* p = Check(h);
			Log("  OPTIMIZE %2d: surface %p %ux%u  transparent %08X%s",
				s_nSaid, h, p ? p->nWidth : 0, p ? p->nHeight : 0,
				nTransparent,
				(nTransparent & 0x80000000u) ? "" : "   <- bit 31 CLEAR,"
					" discarded by the mask test");
		}
	}
	R2D_SetTransparent(h, nTransparent); R2D_Invalidate(h); return 1;
}
static void  __cdecl s_Unoptimize(void* h)    { SLOT(29); R2D_Invalidate(h); }

// The contract is read out of the real renderer rather than guessed - see
// screenlock.h. Four rectangle words, a pointer out, a pitch out, LTBOOL back.
static int   __cdecl s_LockScreen(int nL, int nT, int nR, int nB,
								  void** ppData, int* pPitch)
{
	SLOT(30);
	if (!g_bScreenLock)
	{
		if (ppData) *ppData = nullptr;
		if (pPitch) *pPitch = 0;
		return 0;				// +StubScreenLock 0 restores the old behaviour
	}
	return SL_Lock(nL, nT, nR, nB, ppData, pPitch);
}
static void __cdecl s_UnlockScreen(void)      { SLOT(31); SL_Unlock(); }
// The measured request. 24 bytes, and every field here was identified against
// something already known: the surface pointer because we allocated it, the
// source rect because it tracks that surface's own size, the destination rect
// because it is constant at the screen rectangle, and the alpha because it is
// 1.0 for the opaque background and drifts around 0.65 for the fade overlay.
struct BlitRequest
{
	void*    pSurface;			// +0   one of ours
	uint32_t nFlag;				// +4   0 for the opaque blit, 1 for the fade
	uint32_t nColour;			// +8   0x00FFFFFF in everything seen so far
	int32_t* pSrcRect;			// +12  left, top, right, bottom
	int32_t* pDstRect;			// +16  left, top, right, bottom
	float    fAlpha;			// +20
};

struct BlitRequestPeek { void* pSurface; };

// Whether a menu FOLDER is up, which is not the same question as whether the
// world is being drawn. The client is the only side that knows, and it is one
// call rather than a field in the shared block, because the host has no use
// for it and the block is shared with a separate process.
extern "C" __declspec(dllexport) void __cdecl R3D_PublishFolder2D(int bFolder)
{
	RenderGuard _renderGuard;
	// A FOLDER CHANGE FREES THE OLD FOLDER'S TEXTURES. That is the other
	// moment an address stops meaning what it meant, so it opens the same
	// verify window a world load does.
	// The argument is the FOLDER ID plus one, or 0 for no folder. Any change
	// is a folder change, including menu -> submenu, which a boolean could not
	// see: both are GS_FOLDER and both free the previous folder's textures.
	static LONG s_nLastFold = -1;
	InterlockedExchange(&g_bFolder2D, bFolder ? 1 : 0);
	InterlockedExchange(&g_nFolder2DId, (LONG)bFolder);
	if (s_nLastFold != (LONG)bFolder)
	{
		const LONG nPrev = s_nLastFold;
		s_nLastFold = (LONG)bFolder;
		if (nPrev >= 0)
		{
			char szWhy[64];
			sprintf_s(szWhy, "folder %ld -> %ld", nPrev, s_nLastFold);
			R3D_TexturesMayHaveMoved(szWhy);
		}
	}
}

// Whether the folder that is up is a FULL-CARD screen - one retail draws
// through the interface camera with no world behind it. The client owns the
// folder enumeration, so it decides; the renderer only obeys.
extern "C" __declspec(dllexport) void __cdecl R3D_PublishInterfaceOnly(int bOnly)
{
	RenderGuard _renderGuard;
	R3D_SetInterfaceOnly(bOnly);
}

static void __cdecl s_BlitToScreen(void* pReq)
{
	SLOT(32);
	SnapBlit(RAW_ARGS());

	// READ THE REQUEST INSTEAD OF TRUSTING THE HEADER ABOVE IT.
	//
	// The struct's field names were measured against the big background blits,
	// and the note admits it: "0x00FFFFFF in everything seen so far". The
	// menu's TEXT blits are a different population - a 284x28 strip with a
	// black background, never optimized, so carrying no colour key of its own
	// - and retail keys that black out somehow. Whatever holds the transparent
	// colour is in these words. Dump them for the small sources and stop
	// guessing which field it is.
	{
		const BlitRequestPeek* q = (const BlitRequestPeek*)pReq;
		const StubSurface* ps = (pReq && !IsBadReadPtr(pReq, 40))
			? Check(q->pSurface) : nullptr;
		static int s_nSaid = 0;
		if (ps && s_nSaid < 3 && ps->nWidth > 4 && ps->nHeight > 4
			&& ps->nWidth <= 1200 && ps->nHeight <= 64)
		{
			++s_nSaid;
			const uint32_t* w = (const uint32_t*)pReq;
			Log("  BLIT REQUEST for a %ux%u source - ten raw words:",
				ps->nWidth, ps->nHeight);
			for (int i = 0; i < 10; ++i)
				Log("      +%02X  %08X   (as float %.3f)", i * 4, w[i],
					*(const float*)&w[i]);
		}
	}

	const BlitRequest* r = (const BlitRequest*)pReq;
	if (!r || IsBadReadPtr(r, sizeof(BlitRequest))) return;

	const StubSurface* p = Check(r->pSurface);
	if (!p || !p->pBits) return;
	if (!r->pSrcRect || IsBadReadPtr(r->pSrcRect, 16)) return;
	if (!r->pDstRect || IsBadReadPtr(r->pDstRect, 16)) return;

	// What the blit request's flag and colour ACTUALLY carry. The header here
	// says "0 for the opaque blit, 1 for the fade" and "0x00FFFFFF in
	// everything seen so far", and both were written before the menus were
	// being looked at. The SDK has DrawSurfaceToSurfaceTransparent and
	// ScaleSurfaceToSurfaceTransparent, which have to carry their transparent
	// colour somewhere, and this is the only somewhere there is.
	{
		// Keyed on the SOURCE SIZE as well, so each distinct surface is
		// named once rather than only each distinct flag/colour pair.
		static uint32_t s_pairs[24][4]; static int s_nPairs = 0;
		const uint32_t sw = p ? p->nWidth : 0, sh = p ? p->nHeight : 0;
		bool bSeen = false;
		for (int i = 0; i < s_nPairs; ++i)
			if (s_pairs[i][0] == r->nFlag && s_pairs[i][1] == r->nColour
				&& s_pairs[i][2] == sw && s_pairs[i][3] == sh)
				{ bSeen = true; break; }
		if (!bSeen && s_nPairs < 24)
		{
			s_pairs[s_nPairs][0] = r->nFlag;
			s_pairs[s_nPairs][1] = r->nColour;
			s_pairs[s_nPairs][2] = sw;
			s_pairs[s_nPairs][3] = sh;
			++s_nPairs;
			Log("  R2D BLIT: flag %u colour %08X (RGB %u %u %u), source %ux%u"
				"  -> dst %d,%d..%d,%d  req alpha %.2f",
				r->nFlag, r->nColour, (r->nColour >> 16) & 0xFF,
				(r->nColour >> 8) & 0xFF, r->nColour & 0xFF,
				p ? p->nWidth : 0, p ? p->nHeight : 0,
				r->pDstRect[0], r->pDstRect[1], r->pDstRect[2], r->pDstRect[3],
				r->fAlpha);
		}
	}

	// An alpha outside 0..1 would mean the field is not what it was read as.
	// Clamping would hide that; refusing and counting it does not.
	float fAlpha = r->fAlpha;
	if (!(fAlpha >= 0.0f && fAlpha <= 1.0f)) { InterlockedIncrement(&g_nOddAlpha); fAlpha = 1.0f; }

	// Which blit covers the whole screen?
	//
	// A cyan Clear came back black, so something paints over everything after
	// the engine clears - and the 2D layer is the only thing that draws after
	// RenderScene. A full-screen blit of a black surface would explain the
	// black world, the invisible test triangle and the covered Clear all at
	// once, so it is worth naming rather than assuming.
	{
		const int dw = r->pDstRect[2] - r->pDstRect[0];
		const int dh = r->pDstRect[3] - r->pDstRect[1];
		// Anything aimed at the RIGHT half of the screen. The client renders
		// the right eye first, copies it aside into a stash surface, draws the
		// left eye over it and puts the copy back on the right - so if the
		// right half is black, either that copy never lands here or it lands
		// carrying black. This says which.
		if (r->pDstRect[0] * 2 >= (int)g_nModeW && g_nModeW)
		{
			const LONG n = InterlockedIncrement(&g_nRightHalfBlits);
			if (n <= 4 || (n % 400) == 0)
			{
				uint64_t nSum = 0; int nS = 0;
				const uint8_t* pRow = (const uint8_t*)p->pBits
									+ (size_t)(p->nHeight / 2) * p->nPitch;
				for (uint32_t x = 0; x < p->nWidth && nS < 256; x += 4, ++nS)
					nSum += pRow[x * 4] + pRow[x * 4 + 1] + pRow[x * 4 + 2];
				Log("  RIGHT-HALF BLIT %ld: surface %p %ux%u -> %d,%d..%d,%d"
					" alpha %.3f  mean of middle row %.1f",
					n, r->pSurface, p->nWidth, p->nHeight,
					r->pDstRect[0], r->pDstRect[1], r->pDstRect[2], r->pDstRect[3],
					fAlpha, nS ? (double)nSum / (nS * 3) : -1.0);
			}
		}

		g_bLastBlitFullScreen =
			(dw * 10 >= (int)g_nModeW * 9 && dh * 10 >= (int)g_nModeH * 9);
		if (g_bLastBlitFullScreen)
		{
			const LONG n = InterlockedIncrement(&g_nFullScreenBlits);
			if (n <= 3 || (n % 500) == 0)
			{
				// What is actually in it, sampled from the middle row.
				uint64_t nSum = 0; int nS = 0;
				const uint8_t* pRow = (const uint8_t*)p->pBits
									+ (size_t)(p->nHeight / 2) * p->nPitch;
				for (uint32_t x = 0; x < p->nWidth && nS < 256; x += 4, ++nS)
					nSum += pRow[x * 4] + pRow[x * 4 + 1] + pRow[x * 4 + 2];
				Log("  FULL-SCREEN BLIT %ld: surface %p %ux%u -> %d,%d..%d,%d"
					" alpha %.3f  mean of middle row %.1f",
					n, r->pSurface, p->nWidth, p->nHeight,
					r->pDstRect[0], r->pDstRect[1], r->pDstRect[2], r->pDstRect[3],
					fAlpha, nS ? (double)nSum / (nS * 3) : -1.0);
			}
		}
	}

	// +StubSkipBlackTint 1 drops the screen tint. A DIAGNOSTIC, not a fix: the
	// tint is the game's own full-screen fade, left opaque black by the
	// quick-load path, and the right place to clear it is the client. This
	// exists so the renderer can be verified without waiting on that, and it
	// only ever drops a full-screen blit of a tiny, entirely black surface -
	// which the menu fade (2x2, alpha well below 1) and the title screen
	// (640x480, not black) both survive.
	if (g_bSkipTint && g_bLastBlitFullScreen && p->nWidth <= 8 && p->nHeight <= 8)
	{
		bool bBlack = true;
		for (uint32_t y = 0; y < p->nHeight && bBlack; ++y)
		{
			const uint8_t* pRow = (const uint8_t*)p->pBits + (size_t)y * p->nPitch;
			for (uint32_t x = 0; x < p->nWidth; ++x)
				if (pRow[x * 4] || pRow[x * 4 + 1] || pRow[x * 4 + 2]) { bBlack = false; break; }
		}
		if (bBlack) { InterlockedIncrement(&g_nTintSkipped); return; }
	}

	// Count the full-screen fades, and optionally skip them.
	//
	// The main menu blits its 640x480 panel and then a 2x2 surface stretched
	// over the WHOLE screen at alpha 0.68, repeatedly. Each application leaves
	// 32% of what was there, so a handful of them per frame drives the panel to
	// black - which is exactly what the menu looks like, while the panel
	// surface itself contains the title art intact.
	// Draw ONLY large source surfaces. Splits "the menu panel's blit never
	// draws" from "it draws and something paints over it" - the panel is
	// 640x480 and everything else in the menu is a font strip, a 2x2 fade or a
	// 261-wide filler bar.
	if (g_bOnlyBig && (p->nWidth < 320 || p->nHeight < 240)) return;

	// Hoisted above the per-eye path, which needs to know: a fade fills each
	// eye rather than being fitted into it.
	const bool bTinySrc = (p->nWidth <= 4 && p->nHeight <= 4);
	const bool bFullDst = (r->pDstRect[0] <= 0 && r->pDstRect[1] <= 0
						&& r->pDstRect[2] >= (int32_t)g_nScreenW
						&& r->pDstRect[3] >= (int32_t)g_nScreenH);
	if (bTinySrc && bFullDst)
	{
		InterlockedIncrement(&g_nFadeBlits);
		if (g_bSkipFade) return;
	}

	// A blit that exists only to COVER, as against one that carries a layout.
	// A fade and the screen tint are a solid colour stretched over the whole
	// screen: nothing in them has a position, so fitting them into 76% of the
	// eye turns a fade into a black rectangle with the world around it - which
	// is exactly what the desk showed when this test was bTinySrc's 4 pixels
	// and the tint turned out to be 8x8.
	//
	// The menu background is also stretched over the whole screen and is NOT
	// this: it is a 640x480 image whose layout has to keep step with the text
	// drawn over it, so it is fitted like everything else. The source size is
	// what tells them apart.
	const bool bCover = bFullDst && p->nWidth <= 16 && p->nHeight <= 16;

	// PER-EYE 2D. The engine lays its HUD out for one screen, and in VR that
	// screen is the side-by-side pair: the health bar lands in the left eye
	// only, the ammo count in the right eye only, and the crosshair straddles
	// the seam. Drawing each primitive into both halves is what makes it a HUD
	// again, and this is the one place every 2D primitive passes through.
	//
	// Two kinds of blit must NOT be transformed, and both are identifiable
	// here without asking the client anything:
	//
	//   The eye compositing. The client renders both eyes into the left half
	//   and moves the first one across with a blit of exactly one half-rect
	//   (GameClientShell.cpp, DrawSurfaceToSurface(hScreen, hStash, &rHalf,
	//   nHalfWidth, 0)). Nothing in the HUD is exactly half the screen.
	//
	//   The frame marker. Twelve 8x8 swatches at the top-left corner, which
	//   the HOST reads back out of the captured image by pixel position to
	//   match the frame to a pose. Move it and pose matching breaks - so the
	//   band is excluded, and the count is reported so that a marker layout
	//   that has changed shows up as a zero rather than as silence.
	// Play or menu: a HUD lives in the eye buffer and is centred per eye, a
	// menu goes onto the host's flat panel and must be identical in both.
	// A FOLDER IS A MENU FOR THE 2D LAYER EVEN WHEN THE WORLD KEEPS STEREO.
	//
	// nInMenu answers a different question - "is the world being drawn" - and
	// it is deliberately FALSE at a pause menu, because a pause keeps the world
	// and must keep the stereo pair (dialogue is the same case, and keying that
	// off GS_PLAYING once flattened every conversation in the game).
	//
	// But the 2D layer does not follow the world. A HUD is centred per eye and
	// should be; a FOLDER's art is one flat layout and drawing it per eye puts
	// the same olive card at two different places, which is the double vision
	// reported in the headset on the pause menu. So the 2D layer asks if a folder
	// is up, and the host presentation goes on asking about the world.
	// THE 2D MODE MUST FOLLOW THE PRESENTATION, NOT THE GAME STATE.
	//
	// Menu mode draws the 2D layer identically into both halves and zeroes the
	// per-eye optical centres. That is right for a QUAD - a real rectangle in
	// space, where the runtime works out the disparity - and WRONG for a
	// PROJECTION layer, where each eye is composited through its own canted
	// frustum: identical pixels then land at different world directions and
	// cannot be fused. HUD mode exists for that case and centres per eye so
	// the content lands in the same direction for both.
	//
	// nInMenu is what decides which layer the host submits, so nInMenu is what
	// this must follow. Keying it off "a folder is up" instead put menu mode
	// on folders shown over a live world - which are projection layers - and
	// the pause and cutscene menus doubled in the headset. My change, this
	// session, and the desk could not see it: the eye texture was PERFECTLY
	// mono, max channel difference 1 between the halves. A capture of the eye
	// texture cannot tell you whether something will FUSE.
	//
	// +StubFolder2D 1 restores the experiment.
	// ...AND THE SPLASH OR LOADING SCREEN (the client sends 999), whether or
	// not the host is live yet: the splash is drawn in the game's first
	// seconds, before the host's alive tick reaches this DLL, and took the
	// world's fit - stretched, and then cropped to the band once the host
	// came up (13 and 14 September: the splash reported cropped and zoomed).
	// It is a flat picture with no world behind it, so the menu fit is right
	// for it in every case.
	R2D_SetMenu((VRB_InMenu() || (g_bFolder2DUse && g_bFolder2D) || g_nFolder2DId == 999) ? 1 : 0);
	// The FIT, separately: a folder's art is a menu layout wherever it is
	// shown, including over a live world. See the note in render2d.cpp for why
	// this is not the same decision as flattening the two halves.
	R2D_SetMenuFit((VRB_InMenu() || g_bFolder2D) ? 1 : 0);
	// THE 3D HALF OF THE SAME ZOOM, off the SAME condition. The card is a
	// sprite in the interface scene and the words on it are 2D blits; if these
	// two ever disagree about whether a menu is up, the text slides off the
	// card - which is the recorded failure of +StubMenuScale100.
	// THE SAME CONDITION THE 2D HALF USES, which is R2D_SetMenu's and NOT
	// R2D_SetMenuFit's. The fit flag is true over a live world; this is a
	// change to the frustum, and applying it there would magnify the level
	// behind the pause menu. See the note in render2d.cpp.
	R3D_SetMenuZoom(g_fMenuZoomV, g_fMenuAnchorXV, g_fMenuAnchorYV,
					(VRB_InMenu() || (g_bFolder2DUse && g_bFolder2D)) ? 1 : 0);

	const int nHalfW = (int)g_nModeW / 2;
	const bool bStereo2D = (g_bStereo2D && g_nModeW > 0 && g_nModeH > 0);
	const bool bHalfRect = (r->pDstRect[3] - r->pDstRect[1] >= (int)g_nModeH)
					   && (r->pDstRect[2] - r->pDstRect[0] == nHalfW)
					   && (r->pDstRect[0] == 0 || r->pDstRect[0] == nHalfW);
	// kMarkerBlock is 8 and twelve blocks are drawn: GameClientShell.h and
	// DrawFrameMarker(). Named here rather than shared, because the renderer
	// cannot include the client's headers - hence the count in the report.
	const bool bMarker = (r->pDstRect[0] >= 0 && r->pDstRect[1] >= 0
					   && r->pDstRect[2] <= 8 * 12 && r->pDstRect[3] <= 8);

	// MAGENTA is NOLF's transparency key, and the blit request carries it.
	//
	// Measured at the menus: the request's colour word is 00FF00FF on a 24x24
	// sprite and a 1096x28 bar, and 00000000 or 00FFFFFF on the big background
	// surfaces. Those are the player's magenta spots - the key drawn as
	// if it were art.
	//
	// ONLY magenta is keyed, deliberately. The flag word does not separate a
	// transparent blit from an opaque one - flag 1 carries both 00FFFFFF (the
	// 2x2 fade) and 00FF00FF (the sprite) - so keying on whatever colour the
	// request happens to hold would punch holes in white and black artwork.
	// Nothing in this game's art is pure magenta, which is exactly why it was
	// chosen as the key. Any other colour that turns out to need keying will
	// appear as a coloured rectangle and can be added then.
	R2D_SetBlitKey(r->nColour, (r->nColour & 0x00FFFFFFu) == 0x00FF00FFu);
	// The marker is data for the host, not art. Never key it.
	R2D_SetKeySuppress(bMarker ? 1 : 0);

	if (Trace2DOn())
		Log("  T2D p=%ld BLIT %p %ux%u src %d,%d-%d,%d dst %d,%d-%d,%d",
			(long)g_nPresents, r->pSurface, p->nWidth, p->nHeight,
			r->pSrcRect[0], r->pSrcRect[1], r->pSrcRect[2], r->pSrcRect[3],
			r->pDstRect[0], r->pDstRect[1], r->pDstRect[2], r->pDstRect[3]);
	R2D_SetTrace(Trace2DOn() ? 1 : 0);
	if (bStereo2D && !bHalfRect && !bMarker)
	{
		InterlockedIncrement(&g_nStereo2DBlits);
		R2D_BlitStereo(r->pSurface, p->pBits, p->nPitch, (int)p->nWidth, (int)p->nHeight,
					   r->pSrcRect[0], r->pSrcRect[1], r->pSrcRect[2], r->pSrcRect[3],
					   r->pDstRect[0], r->pDstRect[1], r->pDstRect[2], r->pDstRect[3],
					   fAlpha, bCover ? 1 : 0);
		return;
	}
	if (bStereo2D && bHalfRect) InterlockedIncrement(&g_nCompositeBlits);
	if (bStereo2D && bMarker)   InterlockedIncrement(&g_nMarkerBlits);

	// THE MARKER IS DRAWN LAST, AT PRESENT. The client paints it before the
	// interface goes on top, and the pause menu's full-screen fade then
	// darkens the swatches - a host log: "marker unreadable 364" in
	// exactly the paused windows, so every paused frame went out against the
	// wrong pose and the world fought the head. Held here and drawn over
	// everything just before the frame is published.
	if (bMarker && g_bMarkerLast && g_nMarkerHeld < 64)
	{
		HeldMarker& m = g_MarkerHeld[g_nMarkerHeld++];
		m.pKey = r->pSurface; m.pBits = p->pBits;
		m.nPitch = p->nPitch; m.nW = (int)p->nWidth; m.nH = (int)p->nHeight;
		for (int k = 0; k < 4; ++k) { m.src[k] = r->pSrcRect[k]; m.dst[k] = r->pDstRect[k]; }
		m.fAlpha = fAlpha;
		m.bPending = 1;
		return;
	}

	// THE EYE COPY HAS NO KEY. Every blit that names no key gets BLACK as an
	// implicit one (+StubKeyBlack 1), which is right for the HUD's untagged
	// art and exactly wrong for a copy of a rendered eye: every pure-black
	// pixel of the first eye - a sign's black face, a night sky, a shadow -
	// was dropped from the copied half and showed whatever lay beneath. That
	// used to be a half cleared to black once a frame, so it hid; with the
	// per-pass clear it is stale, and the headset showed part of the outside,
	// like a portal, through the rotating Unity sign in one eye only.
	if (bStereo2D && bHalfRect) R2D_SetKeySuppress(1);
	R2D_Blit(r->pSurface, p->pBits, p->nPitch, (int)p->nWidth, (int)p->nHeight,
			 r->pSrcRect[0], r->pSrcRect[1], r->pSrcRect[2], r->pSrcRect[3],
			 r->pDstRect[0], r->pDstRect[1], r->pDstRect[2], r->pDstRect[3],
			 fAlpha);
	if (bStereo2D && bHalfRect) R2D_SetKeySuppress(0);
}

static void DrawHeldMarker()
{
	if (g_nMarkerHeld <= 0) return;
	R2D_SetKeySuppress(1);
	for (int i = 0; i < g_nMarkerHeld; ++i)
	{
		const HeldMarker& m = g_MarkerHeld[i];
		R2D_Blit(m.pKey, m.pBits, m.nPitch, m.nW, m.nH,
				 m.src[0], m.src[1], m.src[2], m.src[3],
				 m.dst[0], m.dst[1], m.dst[2], m.dst[3], m.fAlpha);
	}
	R2D_SetKeySuppress(0);
	g_nMarkerHeld = 0;
	InterlockedIncrement(&g_nMarkerDrawnLast);
}
// WarpToScreen and BlitFromScreen: the two slots the right eye needs.
//
// The real BlitFromScreen (d3d.ren + 0x36AE0) is a thin wrapper over
// IDirectDrawSurface7::Blt with the SCREEN as the source - so it copies a
// rectangle of the screen into one of our surfaces, which is exactly the eye
// stash readback the client does once per frame.
//
// The request layout is measured rather than read out of the disassembly,
// because the same method already worked for BlitToScreen in docs/PHASE1-2D.md
// and it identifies the fields against things we already know: a surface
// pointer because we allocated it, and a rectangle because it tracks that
// surface's own size while the other one does not.
static void DumpRequest(const char* pszWhat, void* pReq, int nWords)
{
	if (!pReq || IsBadReadPtr(pReq, nWords * 4)) { Log("  %s: unreadable request", pszWhat); return; }
	const uint32_t* w = (const uint32_t*)pReq;
	Log("");
	Log("=== %s request at %p ===", pszWhat, pReq);
	for (int i = 0; i < nWords; ++i)
	{
		const StubSurface* p = Check((void*)(uintptr_t)w[i]);
		char szNote[96]{};
		if (p) sprintf_s(szNote, "OUR SURFACE %ux%u", p->nWidth, p->nHeight);
		else if (w[i] && !IsBadReadPtr((const void*)(uintptr_t)w[i], 16))
		{
			const int32_t* r = (const int32_t*)(uintptr_t)w[i];
			if (r[0] >= -1 && r[1] >= -1 && r[2] > r[0] && r[3] > r[1]
				&& r[2] <= 8192 && r[3] <= 8192)
				sprintf_s(szNote, "-> a rect %d,%d..%d,%d", r[0], r[1], r[2], r[3]);
			else
				sprintf_s(szNote, "-> %08X %08X %08X %08X",
					(unsigned)r[0], (unsigned)r[1], (unsigned)r[2], (unsigned)r[3]);
		}
		float f; memcpy(&f, &w[i], 4);
		Log("  +%2d  %08X  %11d  %10.4f  %s", i * 4, w[i], (int)w[i], f, szNote);
	}
	Log("=== end %s ===", pszWhat);
}

// Returns g_nWarpRet, and that is a switch because it is a suspect.
//
// The real WarpToScreen at d3d.ren+0x36DA0 returns 1 from its success path
// (xor eax,eax / inc eax) and 0 only when its own guards fail. Ours has always
// returned 0. The engine, at lithtech.exe+0x40E8D7, does:
//
//   0040E8D7  call dword ptr [0x4b4448]     <- WarpToScreen, RenderStruct+240
//   0040E8E0  test eax, eax
//   0040E8E2  je   0x40e8f9                 <- a 0 takes a DIFFERENT path
//
// So returning 0 does not mean "nothing happens" - it sends the engine down a
// fallback it would not otherwise run. That is the same shape as the
// RebindLightmaps bug: an LTBOOL-style slot answered with the LTRESULT
// convention. Whether that fallback is what dies at 2048 pixels wide
// (docs/HIGH-RES-CRASH.md) is one desk run, and this switch is the A/B.
static int  __cdecl s_WarpToScreen(void* pReq)
{
	SLOT(33);
	if (g_bReqDump && g_nCalls[33] == 200) DumpRequest("WarpToScreen", pReq, 10);
	return g_nWarpRet;
}
static void __cdecl s_MakeScreenShot(const char*) { SLOT(34); }
static void __cdecl s_ReadConsoleVars(void)   { SLOT(35); }
static void __cdecl s_BlitFromScreen(void* pReq)
{
	SLOT(36);
	if (g_bReqDump && g_nCalls[36] == 50) DumpRequest("BlitFromScreen", pReq, 10);

	// Same 24-byte request as BlitToScreen - measured, not assumed: the dump
	// shows our own 400x600 stash surface at +0 and two rectangles at +12 and
	// +16, exactly the layout docs/PHASE1-2D.md established for the outgoing
	// direction.
	//
	// Both rectangles read 0,0..400,600 in every sample, so which is the
	// screen's and which is the surface's cannot be told apart here. They are
	// used as screen-then-surface by analogy with BlitToScreen, and the copy
	// is clamped to both, so an eventual case where they differ produces a
	// smaller copy rather than a wrong one.
	const BlitRequest* r = (const BlitRequest*)pReq;
	if (!r || IsBadReadPtr(r, sizeof(BlitRequest))) return;
	const StubSurface* p = Check(r->pSurface);
	if (!p || !p->pBits) return;
	if (!r->pSrcRect || IsBadReadPtr(r->pSrcRect, 16)) return;

	int w = r->pSrcRect[2] - r->pSrcRect[0];
	int h = r->pSrcRect[3] - r->pSrcRect[1];
	if (w > (int)p->nWidth)  w = (int)p->nWidth;
	if (h > (int)p->nHeight) h = (int)p->nHeight;
	if (w <= 0 || h <= 0) return;

	// ON THE GPU when it can be. The readback below is a full GPU stall
	// plus 16 MB each way per frame; the snapshot is one copy that never
	// leaves the card. The bits are left untouched, so anything that reads
	// them afterwards sees the previous read - nothing does in play.
	if (g_bGpuEyeCopy && R2D_Snapshot(r->pSurface, r->pSrcRect[0], r->pSrcRect[1], w, h))
	{
		InterlockedIncrement(&g_nScreenSnaps);
		return;
	}
	if (SL_ReadRect(r->pSrcRect[0], r->pSrcRect[1], w, h, p->pBits, (int)p->nPitch))
	{
		InterlockedIncrement(&g_nScreenReads);
		R2D_Invalidate(r->pSurface);	// its texture is now stale
	}
}

namespace
{
	void* const g_pSlot[kSlots] = {
		s_Init, s_Term, s_BindTexture, s_UnbindTexture, s_RebindLightmaps,
		s_Context, s_Clear, s_Start3D, s_End3D, s_IsIn3D,
		s_StartOpt2D, s_EndOpt2D, s_IsInOpt2D,
		s_SetBlend, s_GetBlend, s_SetColour, s_GetColour,
		s_RenderScene, s_RenderCommand, s_GetDDInterface, s_SwapBuffers,
		s_GetInfoFlags, s_GetScreenFormat,
		s_CreateSurface, s_DeleteSurface, s_GetSurfaceInfo,
		s_LockSurface, s_UnlockSurface, s_Optimize, s_Unoptimize,
		s_LockScreen, s_UnlockScreen, s_BlitToScreen, s_WarpToScreen,
		s_MakeScreenShot, s_ReadConsoleVars, s_BlitFromScreen,
	};

	const char* const g_szNames[kSlots] = {
		"Init", "Term", "BindTexture", "UnbindTexture", "RebindLightmaps",
		"Context(?)", "Clear", "Start3D", "End3D", "IsIn3D",
		"StartOptimized2D", "EndOptimized2D", "IsInOptimized2D",
		"SetOptimized2DBlend", "GetOptimized2DBlend",
		"SetOptimized2DColor", "GetOptimized2DColor",
		"RenderScene", "RenderCommand", "GetDirectDrawInterface", "SwapBuffers",
		"GetInfoFlags", "GetScreenFormat",
		"CreateSurface", "DeleteSurface", "GetSurfaceInfo",
		"LockSurface", "UnlockSurface", "OptimizeSurface", "UnoptimizeSurface",
		"LockScreen", "UnlockScreen", "BlitToScreen", "WarpToScreen",
		"MakeScreenShot", "ReadConsoleVariables", "BlitFromScreen",
	};

	// -----------------------------------------------------------------------
	// The lightmaps.
	//
	// Slot 4 is named RebindLightmaps and this is the thing it is named for.
	// Everything below was read out of the retail renderer before a line of it
	// was written, so each offset has a call site behind it rather than a
	// guess:
	//
	//   d3d.ren + 0x33160  the real RebindLightmaps. Calls + 0x6CC90 unless
	//                      the NoLMPages variable is set; on failure it prints
	//                      "unable to create lightmap pages" and disables them.
	//   d3d.ren + 0x6CC90  world->GetLightAnim("LightAnim_BASE", 0), then a
	//                      page for every polygon, then a fill for every one.
	//   d3d.ren + 0x6C8A8  the page allocator. Reads bit 0x80 of the surface's
	//                      +0x38 to decide a polygon HAS a lightmap, and the
	//                      two BYTES at poly + 0x48 / + 0x49 as its width and
	//                      height. Pages are 64 x 64 - the 1/64 constant at
	//                      d3d.ren + 0xAB520 and the `cmp 0x40` bound.
	//   d3d.ren + 0x6BE50  the fill. Walks poly + 0x0C, a [poly + 0x10]-long
	//                      array of 4-byte {uint16 anim, uint16 data} refs.
	//   d3d.ren + 0x6BB30  resolves one ref: frame = [lightanim + 0x34], the
	//                      frame's index array is [[la + 0x24] + frame*4], and
	//                      the record is that array's [data] entry of
	//                      {pointer, length}.
	//   d3d.ren + 0x6A790  the decompressor, and it is eleven instructions:
	//                      16-bit run-length, bit 15 of the value means "a
	//                      count byte follows", so the texel is 15 bits.
	//
	// Nothing here writes to engine memory and nothing here calls into the
	// engine. Every claim gets a denominator.
	// -----------------------------------------------------------------------

	// The decompressor, transcribed. Returns 1 only when the record is
	// consumed EXACTLY - a decoder with the wrong stride still produces
	// plausible pixels, and this is the difference between the two.
	int LMDecode(uint32_t pSrc, uint32_t nLen, uint16_t* pOut, uint32_t nMax,
				 uint32_t* pnOut)
	{
		*pnOut = 0;
		if (!nLen || !Readable(pSrc, nLen)) return 0;
		const uint8_t* p = (const uint8_t*)(uintptr_t)pSrc;
		const uint8_t* const e = p + nLen;
		uint32_t n = 0;
		while (p < e)
		{
			if (p + 2 > e) return 0;
			uint16_t v = *(const uint16_t*)p; p += 2;
			uint32_t c = 1;
			if (v & 0x8000)
			{
				if (p >= e) return 0;
				c = *p++;
				v = (uint16_t)(v & 0x7FFF);
			}
			if (n + c > nMax) return 0;			// the renderer's own 1024 budget
			for (uint32_t k = 0; k < c; ++k) pOut[n++] = v;
		}
		*pnOut = n;
		return 1;
	}

	// 15-bit texel -> XRGB8888, so a lightmap can be written out and looked at.
	// The channel split is not a guess: the decompressor masks off bit 15 as a
	// flag, so the payload is fifteen bits and the only reading is 5-5-5.
	void LM555ToBGRA(const uint16_t* pSrc, uint32_t n, uint32_t* pDst)
	{
		for (uint32_t i = 0; i < n; ++i)
		{
			const uint32_t v = pSrc[i];
			const uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
			pDst[i] = ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
		}
	}

	// The engine's lightmap basis, transcribed from lithtech.exe + 0x3CD10.
	//
	//   V = *(float3*)(g_pLMAxis + (flags >> 29) * 36)
	//   P = V x n,  Q = n x P        n = the plane at [surface + 0x34]
	//
	// The table is past the end of lithtech.exe's raw .data, so it is built at
	// startup and only exists in the running process. Its address is taken
	// from the loaded module rather than from the linker's image base, and the
	// eight entries are checked for unit length before anything is believed.
	uint32_t g_pLMAxis = 0;
	int      g_bLMAxisOK = 0;
	uint32_t g_nLMBadAxis = 0;	// surfaces whose three bits are 6 or 7

	void LMAxisTable()
	{
		const HMODULE h = GetModuleHandleA(nullptr);
		if (!h) return;
		g_pLMAxis = (uint32_t)(uintptr_t)h + 0xB3E84;
		Log("");
		Log("  --- the basis table at lithtech.exe + 0xB3E84 (%08X) ---",
			g_pLMAxis);
		if (!Readable(g_pLMAxis, 8 * 36))
		{ Log("    unreadable"); return; }
		// Six entries, one per dominant axis of a plane normal; the three
		// bits that index it can hold eight and the last two are spare. All
		// eight are printed so the shape is visible, but only the six are
		// checked and only the six are ever used.
		int nUnit = 0;
		for (int i = 0; i < 8; ++i)
		{
			const float* v = (const float*)(uintptr_t)(g_pLMAxis + i * 36);
			const float m = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
			if (i < 6 && m > 0.999f && m < 1.001f) ++nUnit;
			Log("    [%d] (%6.3f %6.3f %6.3f)  |v| = %.4f   then"
				" (%6.3f %6.3f %6.3f) (%6.3f %6.3f %6.3f)",
				i, v[0], v[1], v[2], sqrtf(m),
				v[3], v[4], v[5], v[6], v[7], v[8]);
		}
		g_bLMAxisOK = (nUnit == 6);
		Log("    BASIS TABLE SELF-CHECK: %s",
			g_bLMAxisOK ? "PASSED - 6 of 6 used entries are unit vectors, so"
						  " the address is right and the table is built"
						: "FAILED - the six are not unit vectors, so that is not"
						  " the table");
	}

	// P and Q for one surface. Returns 0 rather than a wrong basis.
	int LMBasis(uint32_t sf, float* pP, float* pQ)
	{
		if (!g_bLMAxisOK || !Readable(sf, 0x3C)) return 0;
		const uint32_t nPlane = Word(sf, 0x34);
		if (!Readable(nPlane, 12)) return 0;
		const uint32_t idx = Word(sf, 0x38) >> 29;
		if (idx > 5) { ++g_nLMBadAxis; return 0; }
		const float* V = (const float*)(uintptr_t)(g_pLMAxis + idx * 36);
		const float* n = (const float*)(uintptr_t)nPlane;
		pP[0] = V[1]*n[2] - V[2]*n[1];
		pP[1] = V[2]*n[0] - V[0]*n[2];
		pP[2] = V[0]*n[1] - V[1]*n[0];
		pQ[0] = n[1]*pP[2] - n[2]*pP[1];
		pQ[1] = n[2]*pP[0] - n[0]*pP[2];
		pQ[2] = n[0]*pP[1] - n[1]*pP[0];
		return 1;
	}

	void LogLightmaps(uint32_t w)
	{
		Log("");
		Log("=== the lightmaps ===");
		if (!Readable(w, 0x1A0)) { Log("  no world"); return; }

		// 1. The light animations. d3d.ren + 0x6BE50 treats [world + 0x08] as
		//    an array of 0x60-byte records indexed by a ref's first uint16 and
		//    [world + 0x0C] as its count. docs/WORLD-NAMED.md read + 0x08 as a
		//    pointer to the string "LightAnim_BASE" - which is the same fact
		//    if the name is inline at the start of record 0, and that is what
		//    the check below decides.
		const uint32_t pLA = Word(w, 0x08), nLA = Word(w, 0x0C);
		Log("  [world+0x08] = %08X, [world+0x0C] = %u light animations", pLA, nLA);
		if (!nLA || nLA > 4096 || !Readable(pLA, 0x60))
		{ Log("  LIGHTANIM SELF-CHECK: FAILED - that is not an array"); return; }

		auto NameOf = [&](uint32_t p, char* pszOut, int nOut)
		{
			int c = 0;
			const char* q = (const char*)(uintptr_t)p;
			for (; c < nOut - 1 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126; ++c)
				pszOut[c] = q[c];
			pszOut[c] = 0;
		};

		uint32_t pBase = 0;
		for (uint32_t i = 0; i < nLA && i < 16; ++i)
		{
			const uint32_t la = pLA + i * 0x60;
			if (!Readable(la, 0x60)) break;
			char szName[48];
			NameOf(la, szName, sizeof szName);
			Log("    [%2u] at %08X  \"%s\"  +20 %u  +24 %08X  +28 %u"
				"  +2C %08X  +30 %u  +34 %u  +38 %u",
				i, la, szName, Word(la, 0x20), Word(la, 0x24), Word(la, 0x28),
				Word(la, 0x2C), Word(la, 0x30), Word(la, 0x34), Word(la, 0x38));
			if (!pBase && strcmp(szName, "LightAnim_BASE") == 0) pBase = la;
		}
		Log("    LIGHTANIM SELF-CHECK: %s",
			pBase ? "PASSED - a record's inline name is LightAnim_BASE, so"
					" +0x08 is the array and the string found earlier was its"
					" first field"
				  : "FAILED - no record is named LightAnim_BASE");
		if (!pBase) return;

		// 2. Does the base animation's current frame index array turn out to
		//    be the 19626-entry {pointer, length} array the world header
		//    already describes at +0x54? Two structures reached by completely
		//    different routes - one from the header, one from the renderer's
		//    call sequence - landing on the same address is not a coincidence.
		const uint32_t pFrames = Word(pBase, 0x24), nFrames = Word(pBase, 0x28);
		const uint32_t nEnt    = Word(pBase, 0x30);
		const uint32_t nFrame  = Word(pBase, 0x34);
		const uint32_t pIdxHdr = Word(w, 0x58), nIdxHdr = Word(w, 0x5C);
		const uint32_t pBlob   = Word(w, 0x44), nBlob = Word(w, 0x48);
		uint32_t pIdx = 0;
		if (Readable(pFrames, 4) && nFrame <= nFrames)
			pIdx = Word(pFrames, nFrame * 4);
		Log("");
		Log("    frames %u, current frame %u, entries %u", nFrames, nFrame, nEnt);
		Log("    frame %u's index array = %08X;  the world header's +0x54"
			" data = %08X, count %u", nFrame, pIdx, pIdxHdr, nIdxHdr);
		Log("    blob %08X, %u bytes", pBlob, nBlob);
		const bool bTie = (pIdx == pIdxHdr) && (nEnt == nIdxHdr) && nEnt != 0;
		Log("    INDEX TIE SELF-CHECK: %s", bTie
			? "PASSED - the light animation's data IS the header's indexed blob"
			: "FAILED - they are different arrays, so one of the two readings"
			  " is wrong");
		if (!Readable(pIdx, 8)) { Log("    no index array to walk"); return; }

		// The lightmap texel size in world units, which the renderer divides
		// its basis by at d3d.ren + 0x6CBA9.
		float fS = 0.0f;
		if (Readable(w + 0xF8, 4)) memcpy(&fS, (const void*)(uintptr_t)(w + 0xF8), 4);
		Log("    [world+0xF8] = %.4f  (the renderer divides the lightmap"
			" basis by this)", fS);

		LMAxisTable();

		// 3. Every polygon of every WorldModel, with denominators.
		const uint32_t pList = Word(w, 0x18C), nList = Word(w, 0x190);
		if (!Readable(pList, 4) || !nList) { Log("  no WorldModel array"); return; }

		std::vector<uint8_t> seen((size_t)(nEnt / 8 + 2), 0);
		uint32_t nPoly = 0, nFlag = 0, nRefOK = 0, nWHOK = 0, nIdxOK = 0;
		uint32_t nDec = 0, nDecExact = 0, nMaxW = 0, nMaxH = 0, nDistinct = 0;
		uint32_t nSpanOK = 0, nSpanSeen = 0;
		float fWorstSpanU = 0.0f, fWorstSpanV = 0.0f;
		float fMinU = 1e30f, fMaxU = -1e30f, fMinV = 1e30f, fMaxV = -1e30f;
		uint64_t nTexels = 0, nBytes = 0;
		std::vector<uint16_t> tex(1024);
		int nWritten = 0;

		for (uint32_t i = 0; i < nList; ++i)
		{
			const uint32_t sub = Word(Word(pList, i * 4), 4);
			if (!Readable(sub, 0xB0)) continue;
			const uint32_t pPolys = Word(sub, 0xA0), nPolys = Word(sub, 0xA4);
			if (!Readable(pPolys, 4) || !nPolys) continue;

			for (uint32_t j = 0; j < nPolys; ++j)
			{
				const uint32_t po = Word(pPolys, j * 4);
				if (!Readable(po, 0x54)) continue;
				++nPoly;

				const uint32_t sf = Word(po, 0x28);
				if (!Readable(sf, 0x3C)) continue;
				if (!(Word(sf, 0x38) & 0x80)) continue;		// no lightmap
				++nFlag;

				const uint32_t lw = *(const uint8_t*)(uintptr_t)(po + 0x48);
				const uint32_t lh = *(const uint8_t*)(uintptr_t)(po + 0x49);
				if (lw && lh && lw <= 64 && lh <= 64 && lw * lh <= 1024) ++nWHOK;
				if (lw > nMaxW) nMaxW = lw;
				if (lh > nMaxH) nMaxH = lh;

				const uint32_t pRef = Word(po, 0x0C), nRef = Word(po, 0x10);
				if (!nRef || nRef > 256 || !Readable(pRef, nRef * 4)) continue;
				++nRefOK;

				// The BASE animation's ref is the one carrying the static
				// lightmap; any others are animated lights.
				uint32_t nData = 0xFFFFFFFF;
				for (uint32_t k = 0; k < nRef; ++k)
				{
					const uint16_t a = *(const uint16_t*)(uintptr_t)(pRef + k * 4);
					const uint16_t d = *(const uint16_t*)(uintptr_t)(pRef + k * 4 + 2);
					if (pLA + (uint32_t)a * 0x60 == pBase) { nData = d; break; }
				}
				if (nData >= nEnt) continue;
				++nIdxOK;
				if (!(seen[nData >> 3] & (1u << (nData & 7))))
				{ seen[nData >> 3] |= (uint8_t)(1u << (nData & 7)); ++nDistinct; }

				if (!Readable(pIdx + nData * 8, 8)) continue;
				const uint32_t at  = Word(pIdx, nData * 8);
				const uint32_t len = Word(pIdx, nData * 8 + 4);
				uint32_t nOut = 0;
				if (!LMDecode(at, len, tex.data(), 1024, &nOut)) continue;
				++nDec;
				nTexels += nOut; nBytes += len;
				if (nOut == lw * lh) ++nDecExact;

				// The lightmap's own mapping, computed the way the engine
				// computes it rather than the way it looked like it might.
				// LMBasis reproduces lithtech.exe + 0x3CD10 exactly; the only
				// question left open is whether the two offsets it is fed -
				// the plane at surface + 0x34 and the origin at poly + 0x34 -
				// are what they are taken to be, and the range check below is
				// what answers that.
				if (fS > 0.0f && lw && lh)
				{
					float Pv[3], Qv[3];
					const float* O = (const float*)(uintptr_t)(po + 0x34);
					const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + 0x50);
					if (LMBasis(sf, Pv, Qv) && nv >= 3 && nv <= 64
						&& Readable(po + 0x54, nv * 24))
					{
						float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
						bool bOK = true;
						for (uint32_t t = 0; t < nv; ++t)
						{
							const uint32_t vp = Word(po + 0x54, t * 24);
							if (!Readable(vp, 12)) { bOK = false; break; }
							const float* V = (const float*)(uintptr_t)vp;
							const float dx = V[0] - O[0], dy = V[1] - O[1],
										dz = V[2] - O[2];
							// The renderer's own expression: the dot product
							// over the texel size, plus the half-texel that
							// puts a sample at the centre of a texel.
							const float u = (dx*Pv[0] + dy*Pv[1] + dz*Pv[2]) / fS + 0.5f;
							const float v = (dx*Qv[0] + dy*Qv[1] + dz*Qv[2]) / fS + 0.5f;
							if (u < u0) u0 = u;
							if (u > u1) u1 = u;
							if (v < v0) v0 = v;
							if (v > v1) v1 = v;
						}
						if (bOK)
						{
							++nSpanSeen;
							// Not a span this time but the actual rectangle:
							// every vertex has to land inside this polygon's
							// own lightmap, 0..w and 0..h, or the origin is
							// not the origin.
							if (u0 >= -0.01f && u1 <= (float)lw + 0.01f
								&& v0 >= -0.01f && v1 <= (float)lh + 0.01f)
								++nSpanOK;
							const float eu = (u1 - (float)lw > -u0) ? u1 - (float)lw : -u0;
							const float ev = (v1 - (float)lh > -v0) ? v1 - (float)lh : -v0;
							if (eu > fWorstSpanU) fWorstSpanU = eu;
							if (ev > fWorstSpanV) fWorstSpanV = ev;
							if (u0 < fMinU) fMinU = u0;
							if (u1 > fMaxU) fMaxU = u1;
							if (v0 < fMinV) fMinV = v0;
							if (v1 > fMaxV) fMaxV = v1;
						}
					}
				}

				// Open the pixels. Four wrong answers in this project came
				// from a statistic over an image nobody looked at.
				if (nWritten < 4 && nOut == lw * lh && lw >= 8 && lh >= 8)
				{
					std::vector<uint32_t> bgra(nOut);
					LM555ToBGRA(tex.data(), nOut, bgra.data());
					StubSurface t{};
					t.pBits = bgra.data(); t.nWidth = lw; t.nHeight = lh;
					t.nPitch = lw * 4;
					char szPath[MAX_PATH];
					sprintf_s(szPath, "%slogs/lightmap%d-%ux%u.bmp",
							  g_szDir, nWritten, lw, lh);
					WriteSurfaceBMP(&t, szPath);
					Log("    lightmap %d: polygon %08X, %ux%u, record %u,"
						" %u compressed bytes -> %u texels", nWritten, po,
						lw, lh, nData, len, nOut);
					++nWritten;
				}
			}
		}

		Log("");
		Log("  --- the counts, with their denominators ---");
		Log("    %u polygons in the level", nPoly);
		Log("    %u have bit 0x80 set in their surface's +0x38", nFlag);
		Log("    %u of those have a sane lightmap size (largest %ux%u)",
			nWHOK, nMaxW, nMaxH);
		Log("    %u have a readable ref list at +0x0C", nRefOK);
		Log("    %u name a record inside the base animation's %u", nIdxOK, nEnt);
		Log("    %u distinct records referenced, of %u in the array",
			nDistinct, nEnt);
		Log("    %u decompress cleanly; %u of those to exactly width*height",
			nDec, nDecExact);
		Log("    %llu texels from %llu compressed bytes (%.2fx)",
			nTexels, nBytes,
			nBytes ? (double)nTexels * 2.0 / (double)nBytes : 0.0);
		Log("    LIGHTMAP SELF-CHECK: %s",
			(nWHOK && nDecExact == nWHOK && nDistinct == nEnt)
				? "PASSED - every lightmapped polygon resolves to a record that"
				  " decompresses to exactly its own width*height, and between"
				  " them they use every record in the array"
				: "see the numbers above - they do not all agree yet");

		Log("");
		Log("  --- the mapping, with the engine's own basis ---");
		Log("    %u of %u polygons have every vertex inside their own"
			" 0..w x 0..h lightmap", nSpanOK, nSpanSeen);
		Log("    worst escape %.3f texels across, %.3f down",
			fWorstSpanU, fWorstSpanV);
		Log("    all vertices land in u %.3f .. %.3f, v %.3f .. %.3f",
			fMinU, fMaxU, fMinV, fMaxV);
		Log("    %u surfaces asked for a table entry that does not exist",
			g_nLMBadAxis);
		Log("    MAPPING SELF-CHECK: %s", (nSpanSeen && nSpanOK == nSpanSeen)
			? "PASSED - poly+0x34 is the lightmap origin, surface+0x34 the"
			  " plane, and world+0xF8 the texel size"
			: "FAILED - one of the three is not what it is taken to be");
		Log("=== end lightmaps ===");
	}

	// THE SECOND THREAD THAT OUTLIVED THE DLL. This one logs a report every
	// four seconds for the first 48 s of a run, sleeping between - and a run
	// shorter than that (every desk capture, every sweep level) unloaded the
	// DLL with it asleep inside: "d3dstub.ren_unloaded +0x70f23" at exit,
	// found the same morning the watchdog's was fixed. It now waits on an
	// event Term signals, and Term waits for it.
	HANDLE g_hReportStop = nullptr;
	HANDLE g_hReportThread = nullptr;
	static bool ReportWait(DWORD ms)
	{
		return g_hReportStop && WaitForSingleObject(g_hReportStop, ms) == WAIT_OBJECT_0;
	}
	void StopReportThread()
	{
		if (!g_hReportThread) return;
		if (g_hReportStop) SetEvent(g_hReportStop);
		// TEN SECONDS, NOT TWO. The thread runs every R3D_*Report between
		// its waits, and the all-levels sweep of 9 September caught it mid-
		// report at Term on 2 of 103 levels: two seconds were not enough,
		// the DLL unloaded under it, and the exit faulted at
		// __ehhandler$ReportThread. If it STILL has not stopped, the module
		// is PINNED so it cannot unload - a leak at exit beats a fault.
		const DWORD r = WaitForSingleObject(g_hReportThread, 10000);
		CloseHandle(g_hReportThread); g_hReportThread = nullptr;
		if (g_hReportStop) { CloseHandle(g_hReportStop); g_hReportStop = nullptr; }
		if (r == WAIT_OBJECT_0)
			Log("  report thread: stopped");
		else
		{
			HMODULE hSelf = nullptr;
			const BOOL bPinned = GetModuleHandleExA(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				(LPCSTR)&StopReportThread, &hSelf);
			Log("  report thread: did not stop in ten seconds - the module is %s so the unload cannot fault",
				bPinned ? "PINNED" : "NOT pinned (GetModuleHandleEx failed)");
		}
	}
	DWORD WINAPI ReportThread(LPVOID)
	{
		// One early tick: the last stub run died at about four seconds, so a
		// five-second first report saw nothing at all. A report that never
		// prints is indistinguishable from a report with nothing to say.
		if (ReportWait(1200)) return 0;
		LogWindows("1.2 s after load - does a window appear later?");
		bool bLMDone = false;

		for (int i = 0; i < 12; ++i)
		{
			if (ReportWait(3800)) return 0;
			Log("");
			Log("--- %d s: which of our slots the engine has called ---", 1 + (i + 1) * 4);
			for (int s = 0; s < kSlots; ++s)
				if (g_nCalls[s])
					Log("   slot %2d %-24s %9ld", s, g_szNames[s], g_nCalls[s]);
			Log("   surfaces live: %ld", g_nSurfacesLive);
			Log("   presents: %ld  (failed %ld)  device %s",
				g_nPresents, g_nPresentFails, g_pSwap ? "up" : "NONE");
			if (g_bNativeFrustum)
				Log("   native frustum: %ld misses (each a scene drawn symmetric),"
					" %ld torn reads answered with last frame's optics",
					g_nNativeMiss, VRB_ReusedCount());
			Log("   full-screen fade blits from a tiny surface: %ld (skip %d)",
				g_nFadeBlits, g_bSkipFade);
			Log("   frame marker drawn last, at present: %ld frames", g_nMarkerDrawnLast);
			// THE SAME LOCK THE SLOTS TAKE (renlock.h), around ONLY the calls
			// that read the caches the main thread rebuilds - and TRIED, never
			// waited: Term holds the lock while it stops this thread.
			//
			// NOT around the whole iteration. The first version was, and the
			// very first report deadlocked the game at six seconds: this
			// thread held the lock and listed the process's windows, a window
			// query is a synchronous message to the thread that owns the
			// window, and that thread - the main thread - was waiting on the
			// lock. Anything that can wait on the main thread stays outside.
			if (TryEnterCriticalSection(&g_csRender))
			{
			struct LeaveAtEnd { ~LeaveAtEnd() { LeaveCriticalSection(&g_csRender); } } _leaveAtEnd;
			R3D_MissingReport(&Log);
			R3D_StaleReport(&Log);
			R3D_FlagReport(&Log);
			R3D_SurfProbeReport(&Log);
			R3D_BridgeReport(&Log);
			// The ground's texture, as we are actually drawing it. Once.
			// OFF by default. This reads a texture back through a staging
			// resource and writes a BMP, and a run at 2456x1328 died inside a
			// BMP row-copy loop in this DLL reading past the end of its
			// source. Whether that was this dump or the older surface dumper
			// was not established - both compile to the same loop - so the
			// diagnostic that has already produced its finding is the one that
			// gets switched off. +StubFloorDump 1 brings it back.
			// WHAT IS UNDER THE PLAYER. One shot, once the world exists. The floor
			// heuristic above answers a question about the LEVEL; this one
			// answers the player's question, which is about the place in the
			// picture he is looking at. It walks the world three times, so it
			// costs a visible hitch - once per run, and only once.
			if (g_bProbe && !g_bProbed && R3D_FloorTexture())
			{
				g_bProbed = 1;
				char szDir[MAX_PATH];
				sprintf_s(szDir, "%slogs/", g_szDir);
				R3D_ProbeFromCamera(szDir, &Log);

				// Aimed depth probe: every surface along ONE ray, nearest first,
				// naming any pair close enough in depth to z-fight. The grid above
				// keeps only the nearest hit, so it cannot see a fight at all.
				// The mesh hunt. One shot, after the world and the model list exist.
				if (g_bModelWalk) R3D_ModelWalk(&Log);
				if (g_bModelWalk) R3D_MeshStride(&Log);
				if (g_bModelWalk) R3D_MeshVerify(&Log);
				if (g_bModelWalk) R3D_FindFaces(&Log);

				if (g_fProbeAz < 900.0f)
				{
					// A SWEEP, not one ray. A gap is bracketed by proving the rays
					// either side of it hit the wall it is a gap in; a single ray
					// that hits nothing cannot tell a hole from a bad aim.
					for (long k = 0; k < g_nProbeSweep; ++k)
					{
						const float off = (float)(k - (g_nProbeSweep - 1) / 2) * g_fProbeStep;
						R3D_ProbeAt(g_fProbeAz + off, g_fProbeEl, &Log);
					}
				}
				R3D_ModelReport(&Log);
				R3D_SurfaceSearch(&Log);
			}
			if (g_bFloorDump && !g_bFloorDumped && R3D_FloorTexture())
			{
				g_bFloorDumped = 1;
				char szPath[MAX_PATH];
				sprintf_s(szPath, "%slogs/floor-texture.bmp", g_szDir);
				Log("   floor texture dump: %s",
					R3D_DumpTexture(R3D_FloorTexture(), szPath) ? szPath : "FAILED");
			}
			}	// the render lock
			else Log("   (renderer reports skipped: the renderer is busy on another thread)");
			// NOT under logs/: the run harness takes the newest directory
			// there as the run's own, so writing a subdirectory into it made
			// probe-run report the dump folder as the log folder.
			//
			// And not until the level's textures actually exist - the first
			// report fired with the cache empty and said "dumped 0 of 0",
			// which reads as a broken dumper rather than as too early.
			// Gated on the FLOOR being found, which only happens once world
			// geometry has been built. Gating on a texture count instead ran
			// the dump during the loading screen, where it stalled the GPU
			// hard enough that a 90-second run never finished loading - and
			// dumped four loading-screen graphics for its trouble.
			if (g_bEyeShare)
			{
				long nPub = 0, nDrop = 0; uint32_t nSer = 0;
				ES_Stats(&nPub, &nDrop, &nSer);
				Log("   shared eye texture: %ld published, %ld dropped"
					" (reader still holding it), serial %u",
					nPub, nDrop, nSer);
			}
			{
				int nTex = 0; long nUp = 0, nDraw = 0, nFail = 0;
				R2D_Stats(&nTex, &nUp, &nDraw, &nFail);
				if (g_bOrder && g_nOrder >= 400)
				{
					const LONG nAt = g_nOrder;
					std::string szO;
					for (int t = 0; t < 400; ++t)
					{
						char szOne[16];
						sprintf_s(szOne, "%d ", g_aOrder[(nAt + t) % 400]);
						szO += szOne;
					}
					Log("   the engine's last 400 calls, in order (9 and 12 omitted):");
					Log("     %s", szO.c_str());
					Log("     (6 Clear, 7 Start3D, 17 RenderScene, 8 End3D,"
						" 10/11 the 2D pair, 32 BlitToScreen, 20 SwapBuffers)");
				}
				int nP3 = 0, nT3 = 0; long nD3 = 0;
				R3D_Stats(&nP3, &nT3, &nD3);
				Log("   3D: %d polygons, %d triangles, %ld world draws", nP3, nT3, nD3);
				Log("   eyes: %ld of %ld frames contained exactly two world"
					" scenes", g_nFramesTwo, g_nFramesSeen);
				Log("         %ld of %ld pairs put scene 0 on the +right side;"
					" separation %.2f units (%.2f .. %.2f)",
					g_nPairsRight, g_nPairs,
					g_nPairs ? g_fSepSum / g_nPairs : 0.0,
					g_nPairs ? g_fSepMin : 0.0, g_nPairs ? g_fSepMax : 0.0);
				Log("         %s",
					(g_nPairs && g_nPairsRight == g_nPairs
					 && g_nFramesTwo * 20 > g_nFramesSeen * 19)
					? "EYE ORDER: settled - scene 0 is the right-half eye, every"
					  " frame, and the pair is one IPD apart"
					: "EYE ORDER: not settled - see the two lines above");
				Log("   full-screen blits: %ld, of which black tint skipped: %ld",
					g_nFullScreenBlits, g_nTintSkipped);
				{
					long nLk = 0, nLf = 0; double fMs = 0;
					SL_Stats(&nLk, &nLf, &fMs);
					Log("   screen locks: %ld ok, %ld refused, %.1f ms total"
						" (%.3f ms each)", nLk, nLf, fMs, nLk ? fMs / nLk : 0.0);
					Log("   blits aimed at the right half: %ld", g_nRightHalfBlits);
					Log("   2D: %ld blits drawn into both halves, %ld left alone"
						" as the eye copy, %ld left alone as the frame marker"
						" (stereo 2D %s)",
						g_nStereo2DBlits, g_nCompositeBlits, g_nMarkerBlits,
						g_bStereo2D ? "on" : "OFF");
					Log("   screen reads (BlitFromScreen): %ld, answered on the GPU: %ld", g_nScreenReads, g_nScreenSnaps);
					Log("   textures sized: %ld of %ld are powers of two  %s",
						g_nTexPow2, g_nTexSeen,
						(g_nTexSeen && g_nTexPow2 == g_nTexSeen)
							? "- +0x10/+0x12 are the width and height"
							: "- NOT the dimensions, or not uint16");
				}
				R2D_ReportSurfaces();
				Log("   2D: %d textures, %ld uploads, %ld draws, %ld failed,"
					" %ld blits with an alpha outside 0..1",
					nTex, nUp, nDraw, nFail, g_nOddAlpha);
			}

			std::string szCB;
			for (int c = 0; c < kCallbacks; ++c)
				if (g_nCBCalls[c])
				{
					char szOne[32];
					sprintf_s(szOne, " %d:%ld", c, g_nCBCalls[c]);
					szCB += szOne;
				}
			Log("   engine callbacks used:%s", szCB.empty() ? " (none)" : szCB.c_str());

			{
				std::string szT;
				for (int t = 0; t < 8; ++t)
					if (g_nType[t])
					{
						char szOne[32];
						sprintf_s(szOne, " %s%d:%ld", t == 7 ? ">=" : "", t, g_nType[t]);
						szT += szOne;
					}
				Log("   scene word 0 (type):%s", szT.empty() ? " (none)" : szT.c_str());
				Log("   scene word 13: %ld zero, %ld small, %ld a pointer",
					g_nW13Zero, g_nW13Small, g_nW13Ptr);
			}

			// The slots that are NEVER called are as informative as the ones
			// that are: a renderer the engine has given up on stops being
			// called at all, and that is visible here and nowhere else.
			std::string szQuiet;
			for (int s = 0; s < kSlots; ++s)
				if (!g_nCalls[s]) { szQuiet += " "; szQuiet += std::to_string(s); }
			Log("   never called:%s", szQuiet.empty() ? " (none)" : szQuiet.c_str());

			if (i == 0)
			{
				LogWindows("5 s in"); DumpBlits();
				if (g_bDumpSurfaces) DumpBiggestSurfaces(3);	// ~13 MB of BMPs; +StubDumpSurfaces 1
			}

			if (i == 3 && g_bLMProbe && g_pWorld && !bLMDone)
			{ bLMDone = true; LogLightmaps(g_pWorld); }

			// The polygon -> texture link.
			//
			// A polygon's +0x28 points into VisBSP's +0x78 array. What that
			// record IS can be settled without guessing: if one of its words
			// is a pointer the engine has handed us through BindTexture, that
			// word is the texture. Nothing else in a geometry structure hits a
			// set of 500 known heap pointers by accident.
			//
			// Done here rather than at world-build time because textures are
			// bound during the load and the set has to be populated first.
			if (i == 2 && g_pVisBSP && g_nTexKnown > 0)
			{
				const uint32_t pPolys = Word(g_pVisBSP, 0xA0);
				const uint32_t nPolys = Word(g_pVisBSP, 0xA4);
				const uint32_t pSurf  = Word(g_pVisBSP, 0x78);
				const uint32_t nSurf  = Word(g_pVisBSP, 0x7C);
				Log("");
				Log("=== the polygon -> texture link ===");
				Log("  %u polygons, %u entries in the +0x78 array at %08X,"
					" %ld textures known", nPolys, nSurf, pSurf, g_nTexKnown);

				// The stride, from the data: the smallest gap between the
				// +0x28 pointers of different polygons.
				uint32_t nMinGap = 0xFFFFFFFF, nPrev = 0;
				for (uint32_t j = 0; j < nPolys && j < 400; ++j)
				{
					const uint32_t po = Word(pPolys, j * 4);
					const uint32_t sf = Word(po, 0x28);
					if (sf < pSurf) continue;
					if (nPrev && sf != nPrev)
					{
						const uint32_t d = (sf > nPrev) ? sf - nPrev : nPrev - sf;
						if (d < nMinGap) nMinGap = d;
					}
					nPrev = sf;
				}
				Log("  smallest gap between two polygons' +0x28 targets: %u bytes",
					nMinGap == 0xFFFFFFFF ? 0 : nMinGap);

				for (uint32_t j = 0; j < 3; ++j)
				{
					const uint32_t po = Word(pPolys, j * 4);
					const uint32_t sf = Word(po, 0x28);
					Log("");
					Log("    polygon %u at %08X -> +0x28 = %08X (byte %u of the array)",
						j, po, sf, sf - pSurf);
					if (IsBadReadPtr((const void*)(uintptr_t)sf, 0x50)) { Log("      unreadable"); continue; }
					for (int o = 0; o < 0x50; o += 4)
					{
						const uint32_t v = Word(sf, o);
						float f; memcpy(&f, &v, 4);
						const char* pszMod = Classify(v);
						if (IsKnownTexture(v))
							Log("      +%02X  %08X   <== A TEXTURE the engine bound", o, v);
						else if (f > -1e6f && f < 1e6f && f != 0.0f
								 && (v & 0x7F800000) && (v & 0x7F800000) != 0x7F800000)
							Log("      +%02X  %08X   %.4f", o, v, f);
						else
							Log("      +%02X  %08X   %u %s", o, v, v, pszMod);
					}
				}
				// THE UV DERIVATION, checked on every polygon.
				//
				// The surface record carries three vectors: O at +0x04, P at
				// +0x10, Q at +0x1C. For polygon 0 they are (2560,-1024,1408),
				// (1,0,0) and (0,-1,0), and its first vertex (2096,-1540,1408)
				// carries the floats -464 and 516 - which are exactly
				// dot(v-O,P) and dot(v-O,Q).
				//
				// If that is the mapping, it holds for all 5957 polygons and
				// every vertex of each. If it is a coincidence of one wall, it
				// does not.
				{
					uint32_t nChecked = 0, nGood = 0;
					float fWorst = 0.0f;
					for (uint32_t j = 0; j < nPolys; ++j)
					{
						const uint32_t po = Word(pPolys, j * 4);
						if (IsBadReadPtr((const void*)(uintptr_t)po, 0x54)) continue;
						const uint32_t sf = Word(po, 0x28);
						if (IsBadReadPtr((const void*)(uintptr_t)sf, 0x28)) continue;
						const float* S = (const float*)(uintptr_t)sf;
						const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + 0x50);
						if (nv < 3 || nv > 64) continue;
						if (IsBadReadPtr((const void*)(uintptr_t)(po + 0x54), nv * 24)) continue;
						for (uint32_t t = 0; t < nv; ++t)
						{
							const uint32_t e = po + 0x54 + t * 24;
							const uint32_t vp = Word(e, 0);
							if (IsBadReadPtr((const void*)(uintptr_t)vp, 12)) continue;
							const float* V = (const float*)(uintptr_t)vp;
							const float* UV = (const float*)(uintptr_t)(e + 4);
							const float dx = V[0] - S[1], dy = V[1] - S[2], dz = V[2] - S[3];
							const float u = dx * S[4] + dy * S[5] + dz * S[6];
							const float v = dx * S[7] + dy * S[8] + dz * S[9];
							const float eu = u - UV[0], ev = v - UV[1];
							const float e2 = (eu < 0 ? -eu : eu) + (ev < 0 ? -ev : ev);
							++nChecked;
							if (e2 < 0.05f) ++nGood; else if (e2 > fWorst) fWorst = e2;
						}
					}
					Log("");
					Log("  UV DERIVATION: %u of %u vertices have their stored"
						" (u,v) equal to dot(v-O,P), dot(v-O,Q)", nGood, nChecked);
					Log("    worst disagreement %.4f", fWorst);
					Log("    %s", (nChecked && nGood == nChecked)
						? "PASSED - +0x04 is the texture origin, +0x10 and +0x1C"
						  " its two axes, and the per-vertex floats are texture"
						  " units. Dividing by the texture size gives 0..1."
						: "FAILED - that is not the mapping");
				}

				// WHICH offset of the 72-byte surface record holds the
				// texture? One sample said +0x30 and that produced 176
				// textures but left 25174 of 26904 polygons untextured, so one
				// sample was not enough. Count it across every surface: the
				// texture field is the offset that holds a pointer the engine
				// has bound, in most records, and no other offset will.
				{
					int nHit[72 / 4] = { 0 };
					uint32_t nSurfSeen = 0;
					for (uint32_t j = 0; j < nSurf; ++j)
					{
						const uint32_t sf = pSurf + j * 72;
						if (IsBadReadPtr((const void*)(uintptr_t)sf, 72)) continue;
						++nSurfSeen;
						for (int o = 0; o < 72; o += 4)
							if (IsKnownTexture(Word(sf, o))) ++nHit[o / 4];
					}
					Log("");
					Log("  which offset of the 72-byte surface holds a bound"
						" texture, over %u records:", nSurfSeen);
					int nBest = -1, nBestAt = 0;
					for (int o = 0; o < 72; o += 4)
						if (nHit[o / 4] > 0)
						{
							Log("    +%02X  %d records (%.1f%%)", o, nHit[o / 4],
								nSurfSeen ? 100.0 * nHit[o / 4] / nSurfSeen : 0.0);
							if (nHit[o / 4] > nBest) { nBest = nHit[o / 4]; nBestAt = o; }
						}
					Log("    best: +%02X with %d of %u", nBestAt, nBest, nSurfSeen);

					// The number that actually decides how much of the world
					// can be textured: how many DISTINCT textures the surfaces
					// reference, against how many we have managed to build.
					// "178 created" means nothing without it.
					const int kMax = 4096;
					static uint32_t aSeen[kMax];
					int nDistinct = 0, nHave = 0;
					for (uint32_t j = 0; j < nSurf; ++j)
					{
						const uint32_t sf = pSurf + j * 72;
						if (IsBadReadPtr((const void*)(uintptr_t)sf, 72)) continue;
						const uint32_t tx = Word(sf, 0x30);
						if (!tx) continue;
						bool bNew = true;
						for (int k = 0; k < nDistinct; ++k)
							if (aSeen[k] == tx) { bNew = false; break; }
						if (!bNew || nDistinct >= kMax) continue;
						aSeen[nDistinct++] = tx;
						if (R3D_HasTexture(tx)) ++nHave;
					}
					Log("    the surfaces reference %d distinct textures;"
						" we have built %d of them", nDistinct, nHave);
					Log("    (%ld distinct textures bound in total so far)",
						g_nTexKnown);
				}

				// What the shared word at +0x30 leads to. It is the same
				// pointer for different polygons, so it is a shared object -
				// and the texture has to be reachable from somewhere.
				{
					const uint32_t po = Word(pPolys, 0);
					const uint32_t sf = Word(po, 0x28);
					const uint32_t p30 = Word(sf, 0x30);
					Log("");
					Log("  following +0x30 = %08X:", p30);
					if (!IsBadReadPtr((const void*)(uintptr_t)p30, 0x60))
						for (int o = 0; o < 0x60; o += 4)
						{
							const uint32_t v = Word(p30, o);
							char szN[64]{};
							if (IsKnownTexture(v)) sprintf_s(szN, "<== A TEXTURE the engine bound");
							else if (v && !IsBadReadPtr((const void*)(uintptr_t)v, 4))
							{
								const char* q = (const char*)(uintptr_t)v;
								int k = 0;
								while (k < 40 && !IsBadReadPtr(q + k, 1)
									   && q[k] >= 32 && q[k] <= 126) ++k;
								if (k >= 4) sprintf_s(szN, "-> \"%.40s\"", q);
							}
							Log("      +%02X  %08X  %s %s", o, v, Classify(v), szN);
						}
				}
				Log("=== end link ===");
			}

			// The six words the scene description points at, read straight out
			// of lithtech.exe rather than through the scene description - so
			// they can be watched in a run where the REAL renderer is drawing
			// and our RenderScene is never called.
			//
			// They stayed zero through 1861 rendered world frames with our
			// renderer, which a set of draw-list heads would not. If they are
			// non-zero while the real renderer draws, they are counters it
			// WRITES, and the "six list heads" reading was wrong.
			{
				const uintptr_t nBase = (uintptr_t)GetModuleHandleA(nullptr);
				const uintptr_t nAt   = nBase + 0xB1590;		// 0x004B1590
				char szLine[256]{};
				for (int w = 0; w < 6; ++w)
				{
					const void* p = (const void*)(nAt + w * 4);
					char szOne[32];
					sprintf_s(szOne, "%08X ",
						IsBadReadPtr(p, 4) ? 0xDEADBEEF : *(const uint32_t*)p);
					strcat_s(szLine, szOne);
				}
				Log("   engine words at 004B1590..A4: %s", szLine);
			}
		}
		return 0;
	}
}

// --------------------------------------------------------------------------
// Exports. __cdecl, measured. The .def strips decoration.
// --------------------------------------------------------------------------

extern "C" void __cdecl RenderDLLSetup(void* pStruct)
{
	g_pStruct = (BYTE*)pStruct;
	Log("");
	Log("RenderDLLSetup: RenderStruct at %p", pStruct);

	if (!pStruct || IsBadWritePtr((BYTE*)pStruct + kFirstSlotOffset, kSlots * 4))
	{
		Log("FATAL: the struct is not writable where the slots live - not filling it");
		return;
	}

	// Harvest the real renderer's table first, so any single slot can be handed
	// back to it. It writes into the engine's struct; we overwrite immediately.
	if (g_pfnSetup)
	{
		((void(__cdecl*)(void*))g_pfnSetup)(pStruct);
		BYTE* pR = (BYTE*)pStruct + kFirstSlotOffset;
		for (int i = 0; i < kSlots; ++i) g_pRealSlot[i] = *(void**)(pR + i * 4);
		g_bHaveReal = (g_pRealSlot[0] != nullptr);
		Log("real renderer's table harvested: slot 0 (Init) at %p, slot 22 at %p",
			g_pRealSlot[0], g_pRealSlot[22]);
	}
	else
		Log("no real RenderDLLSetup - nothing can be delegated this run");

	BYTE* pBase = (BYTE*)pStruct + kFirstSlotOffset;
	for (int i = 0; i < kSlots; ++i)
		*(void**)(pBase + i * 4) = g_pSlot[i];

	// Hand a range of slots back to the renderer that works. Slot 0 is excluded
	// because Init is delegated through our own s_Init, which still has to
	// stamp the cookie and log.
	if (g_nDelegateLo >= 0 && g_bHaveReal)
	{
		int nGiven = 0;
		for (int i = 1; i < kSlots; ++i)
			if (g_bTrace17 && i == 17) continue;		// ours, so it can watch
			else if (i >= g_nDelegateLo && i <= g_nDelegateHi && g_pRealSlot[i])
			{
				*(void**)(pBase + i * 4) = g_pRealSlot[i];
				++nGiven;
			}
		Log("BISECTION: %d slots in [%d..%d] handed back to the real d3d.ren;"
			" the rest are ours", nGiven, g_nDelegateLo, g_nDelegateHi);
	}

	// And thunk the engine's own 15 callbacks, so we can see which of them the
	// renderer is expected to call - and in particular which the real Init uses.
	for (int i = 0; i < kCallbacks; ++i)
	{
		g_pOrigCB[i] = *(void**)((BYTE*)pStruct + i * 4);
		*(void**)((BYTE*)pStruct + i * 4) = g_pCBThunk[i];
	}
	Log("thunked %d engine callbacks at offsets 0..%d:", kCallbacks, (kCallbacks - 1) * 4);
	{
		const uintptr_t nBase = (uintptr_t)GetModuleHandleA(nullptr);
		for (int i = 0; i < kCallbacks; ++i)
			Log("   cb %2d  offset %2d  %08X  = lithtech.exe + %06X",
				i, i * 4, (unsigned)(uintptr_t)g_pOrigCB[i],
				(unsigned)((uintptr_t)g_pOrigCB[i] - nBase));
	}

	Log("filled %d slots at offsets %d..%d",
		kSlots, kFirstSlotOffset, kFirstSlotOffset + (kSlots - 1) * 4);
	Log("engine callbacks the struct arrived with: %08X %08X (offsets 0, 4)",
		*(uint32_t*)pStruct, *(uint32_t*)((BYTE*)pStruct + 4));
	Log("width %u height %u initted %u (offsets 60, 64, 68) as handed over",
		*(uint32_t*)((BYTE*)pStruct + 60), *(uint32_t*)((BYTE*)pStruct + 64),
		*(uint32_t*)((BYTE*)pStruct + 68));
}

// THE MODE LIST IS OURS NOW. It used to be delegated to the real d3d.ren on
// the argument that the RMode layout had not been measured. It has been:
// FreeModeList in the retail d3d.ren walks the list through offset 0x210,
// which is exactly where the SDK header's m_pNext lands (4 + 256 + 128 + 128
// + 3*4), and it returns with a plain ret, so both exports are cdecl.
//
// Why it had to change: OPTIONS -> DISPLAY crashed the game at the desk,
// every time, on a worker thread with nvwgf2um.dll on its stack and EIP in
// unmapped memory. That page calls GetRenderModes, which reached the real
// d3d.ren, which enumerates through DirectDraw - and game\DDraw.dll is
// dgVoodoo 2.87.3, which stands up a D3D11 device of its own to answer, and
// tears it down under its worker thread when the list is freed. None of that
// belongs in a process that already has its renderer. So the page never
// showed a resolution list, and nobody had opened it in a headset yet.
//
// The list: the size the engine started us at, first, so the page's "current
// mode" lookup finds it, then the usual monitor sizes. 32-bit only - the page
// throws 16-bit modes away. m_RenderDLL is our own file name, because the page
// compares it against the current mode's to decide whether picking a
// resolution means switching renderer DLLs.
static bool _stristr_ci(const char* hay, const char* needle)
{
	const size_t nl = strlen(needle);
	for (; *hay; ++hay)
		if (_strnicmp(hay, needle, nl) == 0) return true;
	return false;
}

struct StubRMode
{
	int       bHardware;
	char      szRenderDLL[256];
	char      szInternalName[128];
	char      szDescription[128];
	uint32_t  nWidth, nHeight, nBitDepth;
	StubRMode* pNext;
};
static_assert(offsetof(StubRMode, pNext) == 0x210, "RMode layout: m_pNext must sit at 0x210, as the retail d3d.ren walks it");
static_assert(sizeof(StubRMode) == 0x214, "RMode size");

static long g_nModeLists = 0;

extern "C" StubRMode* __cdecl GetSupportedModes()
{
	static const uint32_t kSizes[][2] = {
		{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 },
		{ 1280, 960 }, { 1280, 1024 }, { 1600, 900 }, { 1600, 1200 }, { 1920, 1080 },
		{ 1920, 1200 }, { 2560, 1384 }, { 2560, 1440 }, { 3440, 1440 },
		{ 3840, 2076 }, { 3840, 2160 },
	};

	// Our own file name, as the engine loaded it.
	char szSelf[MAX_PATH] = "d3dstub.ren";
	HMODULE hSelf = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
						   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   (LPCSTR)&GetSupportedModes, &hSelf) && hSelf)
	{
		char szPath[MAX_PATH];
		if (GetModuleFileNameA(hSelf, szPath, sizeof szPath))
		{
			const char* p = strrchr(szPath, '\\');
			strcpy_s(szSelf, p ? p + 1 : szPath);
		}
	}

	StubRMode* pHead = nullptr; StubRMode** ppTail = &pHead;
	int nCount = 0;
	auto Add = [&](uint32_t w, uint32_t h)
	{
		for (StubRMode* q = pHead; q; q = q->pNext)
			if (q->nWidth == w && q->nHeight == h) return;
		StubRMode* m = (StubRMode*)calloc(1, sizeof(StubRMode));
		if (!m) return;
		m->bHardware = 1;
		strcpy_s(m->szRenderDLL, szSelf);
		strcpy_s(m->szInternalName, "d3dstub");
		strcpy_s(m->szDescription, "NOLF VR (Direct3D 11)");
		m->nWidth = w; m->nHeight = h; m->nBitDepth = 32;
		*ppTail = m; ppTail = &m->pNext; ++nCount;
	};
	if (g_nScreenW >= 640 && g_nScreenH >= 480) Add(g_nScreenW, g_nScreenH);
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); ++i)
		Add(kSizes[i][0], kSizes[i][1]);

	++g_nModeLists;
	Log("GetSupportedModes: list %ld, %d modes as '%s' (first %ux%u), thread %lu",
		g_nModeLists, nCount, szSelf, pHead ? pHead->nWidth : 0u,
		pHead ? pHead->nHeight : 0u, (unsigned long)GetCurrentThreadId());

	// WHO ELSE IS LOADED RIGHT NOW. The engine answers GetRenderModes by
	// loading every renderer DLL it can find and asking each one, and a DLL
	// loaded for that and freed again takes its threads down with it - the
	// Display-page crash is a thread running at an address no module owns.
	// This is the census at the moment of the call, so the log can say
	// which DLLs were in the process to be unloaded afterwards.
	{
		HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
												GetCurrentProcessId());
		if (hSnap != INVALID_HANDLE_VALUE)
		{
			MODULEENTRY32 me{}; me.dwSize = sizeof me;
			for (BOOL ok = Module32First(hSnap, &me); ok; ok = Module32Next(hSnap, &me))
			{
				const char* n = me.szModule;
				const size_t l = strlen(n);
				const bool bRen = l > 4 && _stricmp(n + l - 4, ".ren") == 0;
				if (bRen || _stristr_ci(n, "ddraw") || _stristr_ci(n, "d3dimm")
					|| _stristr_ci(n, "dgvoodoo") || _stristr_ci(n, "d3dim700"))
					Log("  loaded now: %-16s base %08X size %06X",
						n, (unsigned)(uintptr_t)me.modBaseAddr, (unsigned)me.modBaseSize);
			}
			CloseHandle(hSnap);
		}
	}
	return pHead;
}

extern "C" void __cdecl FreeModeList(StubRMode* p)
{
	int n = 0;
	while (p) { StubRMode* q = p->pNext; free(p); p = q; ++n; }
	Log("FreeModeList: %d modes freed", n);
}

// Called by the renderer when it notices a folder change (see
// R3D_TexturesMayHaveMoved). Arms the frame dump if the switch asked for it.
void Stub_FolderChanged()
{
	if (g_nFrameDumpOnFolder > 0 && g_nFrameDumpAt == 0)
	{
		g_nFrameDumpAt = g_nPresents + 1;
		g_nFrameDumpCount = g_nFrameDumpOnFolder;
		Log("  FRAME DUMP armed on folder change: %ld frames from present %ld",
			g_nFrameDumpCount, g_nFrameDumpAt);
	}
}

BOOL APIENTRY DllMain(HMODULE hSelf, DWORD nReason, LPVOID)
{
	// The previous stub run went quiet and the process ended a few seconds
	// later with the report thread never printing once. One explanation is that
	// the engine gave up and called FreeLibrary on us, which would kill that
	// thread mid-instruction. This says so if it happens.
	if (nReason == DLL_PROCESS_DETACH)
	{
		Log("");
		Log("DLL_PROCESS_DETACH at tick %lu - %ld presents, %ld Init calls",
			GetTickCount(), g_nPresents, g_nCalls[0]);
		return TRUE;
	}
	if (nReason != DLL_PROCESS_ATTACH) return TRUE;
	DisableThreadLibraryCalls(hSelf);
	// Before anything can call a slot. Spins briefly before sleeping: the
	// hold times are a frame at most and the contention is rare.
	InitializeCriticalSectionAndSpinCount(&g_csRender, 4000);

	char szDir[MAX_PATH]{};
	GetModuleFileNameA(hSelf, szDir, MAX_PATH);
	char* pSlash = strrchr(szDir, '\\');
	if (pSlash) *(pSlash + 1) = 0;

	strcpy_s(g_szDir, szDir);
	CreateDirectoryA((std::string(szDir) + "logs").c_str(), nullptr);
	char szLog[MAX_PATH];
	sprintf_s(szLog, "%slogs\\renstub.log", szDir);

	// Append when the SAME process is re-attaching, truncate when it is a new
	// launch.
	//
	// LithTech frees and reloads the render DLL inside a single session, and
	// this used to be an unconditional "w" - so a reload silently truncated the
	// log while the game was still running. The 13:23 session did exactly
	// that: one 94.4-second client run, the file 215 KB mid-run with world
	// draws at 86-89 FPS, then 109 KB at exit with "1 Init calls" and no world
	// draws at all. The half that mattered was gone before anything could copy
	// it, and archiving the file afterwards - which run.ps1 now does - would
	// only ever have saved the remnant.
	//
	// The pid is stamped on the header line below and read back here. Nothing
	// else distinguishes "the engine reloaded me" from "a new run started",
	// because both are a plain PROCESS_ATTACH.
	{
		g_nReload = 0;
		const unsigned long nPid = (unsigned long)GetCurrentProcessId();
		// A PID IS NOT AN IDENTITY - WINDOWS REUSES THEM, AND QUICKLY.
		//
		// This test used to be the pid alone, and a harness that launches the
		// game every few minutes gets the same pid back often. When it does,
		// the new process reads its own number in the previous run's log,
		// decides the engine has merely reloaded it, and APPENDS - so the file
		// carries two runs, its world-load count is the sum of both, and a
		// crash marker from the earlier one is still sitting in it. On the
		// night of 11 September a chain harness reported a crash and sixteen
		// world loads for a mission that had done neither: both belonged to a
		// run three minutes earlier that happened to have had pid 5184.
		//
		// The process CREATION TIME settles it. A reload is the same process,
		// so it has the same creation time; a new process with a recycled pid
		// does not.
		unsigned long long nStart = 0;
		{
			FILETIME ftC{}, ftE{}, ftK{}, ftU{};
			if (GetProcessTimes(GetCurrentProcess(), &ftC, &ftE, &ftK, &ftU))
				nStart = ((unsigned long long)ftC.dwHighDateTime << 32) | ftC.dwLowDateTime;
		}
		g_nLogStart = nStart;
		FILE* pOld = nullptr;
		fopen_s(&pOld, szLog, "r");
		if (pOld)
		{
			// THE HEADER IS NOT THE FIRST LINE. The archive mounts log before
			// it - fifteen REZ lines on a full install - so reading one line
			// never found the pid, every reload was taken for a new run, and
			// the truncation this block exists to prevent went on happening:
			// the engine reloads this DLL on every focus loss and gain, and
			// each one wiped the evidence of the one before. Scan the head.
			char szFirst[512]{};
			for (int nLine = 0; nLine < 40 && fgets(szFirst, sizeof szFirst, pOld); ++nLine)
			{
				const char* pTag = strstr(szFirst, "(pid ");
				unsigned long nWas = 0;
				if (pTag && sscanf_s(pTag + 5, "%lu", &nWas) == 1)
				{
					// The start stamp is written after the pid by the header
					// below. A log from before this check existed has none, and
					// is treated as another process's - which is the safe way
					// round: at worst it costs one archived file.
					unsigned long long nWasStart = 0;
					const char* pStart = strstr(szFirst, " start ");
					if (pStart) sscanf_s(pStart + 7, "%llu", &nWasStart);
					if (nWas == nPid && nWasStart == nStart && nStart != 0)
						g_nReload = 1;
					break;
				}
			}
			fclose(pOld);
		}

		// A NEW LAUNCH USED TO DESTROY THE PREVIOUS RUN'S LOG, and archiving it
		// afterwards is a race the archiver can lose.
		//
		// It lost it on 5 September. The stage 1 A/B was two launches 94 seconds
		// apart; run.ps1's archive step did not run for either arm, and the first
		// arm's renderer log was gone before anything copied it. That arm was
		// recoverable only because it ran the compiled-in default. In stage 2 the
		// WORLD DRAWN line in THIS FILE is the measurement, and an A/B missing one
		// arm is not an A/B.
		//
		// So the renderer moves the old file aside itself, at the only moment it
		// is certainly complete and certainly not open. Best effort on purpose: a
		// failed rename leaves us exactly where we were before, never worse.
		// Nothing downstream changes - the live log keeps its name, so every tool
		// and every doc that reads game\logs\renstub.log still finds it.
		//
		// LOCAL time, not UTC, so the name sorts beside the client's own
		// logs\<YYYYMMDD-HHMMSS>\ directory and the pair can be matched by eye.
		if (!g_nReload)
		{
			WIN32_FILE_ATTRIBUTE_DATA fad{};
			if (GetFileAttributesExA(szLog, GetFileExInfoStandard, &fad))
			{
				FILETIME ftLocal{};
				SYSTEMTIME st{};
				if (FileTimeToLocalFileTime(&fad.ftLastWriteTime, &ftLocal) &&
				    FileTimeToSystemTime(&ftLocal, &st))
				{
					char szKeep[MAX_PATH];
					sprintf_s(szKeep, "%slogs\\renstub-%04u%02u%02u-%02u%02u%02u.log",
						szDir, st.wYear, st.wMonth, st.wDay,
						st.wHour, st.wMinute, st.wSecond);
					MoveFileA(szLog, szKeep);
				}
			}
		}
		// _fsopen, NOT fopen_s. fopen_s opens a file with sharing DENIED,
		// so for the whole of a run nothing else could read this log - a
		// sweep harness polling it for the ACCOUNT line got a sharing
		// violation every second and recorded 103 perfectly good levels as
		// "never printed one". _SH_DENYWR keeps the writer exclusive and
		// lets readers in, which is what a live log is for.
		g_pLog = _fsopen(szLog, g_nReload ? "a" : "w", _SH_DENYWR);
	}
	g_hLogLock = CreateMutexA(nullptr, FALSE, nullptr);

	for (int i = 0; i < kSlots; ++i) g_szName[i] = g_szNames[i];

	// Module ranges, so a word in the struct that is a pointer says where to.
	NoteModule("d3dstub.ren", hSelf);
	NoteAllModules();
	NoteModule("lithtech.exe", GetModuleHandleA(nullptr));

	g_bDelegateInit = CmdLineInt("StubDelegateInit", 0);
	g_bWantDevice   = CmdLineInt("StubDevice", 1);
	g_bD3DDebug     = CmdLineInt("StubD3DDebug", 0);
	g_nSceneDumpAt  = CmdLineInt("StubSceneDump", 400);
	g_nDelegateLo   = CmdLineInt("StubDelegateLo", -1);
	g_nDelegateHi   = CmdLineInt("StubDelegateHi", -1);
	g_bTrace4       = CmdLineInt("StubDelegate4Trace", 0);
	g_nRebindRet    = CmdLineInt("StubRebindRet", 1);
	g_nWarpRet      = CmdLineInt("StubWarpRet", 1);
	g_bEyeShare     = CmdLineInt("StubEyeShare", 1);
	g_bDrawInterface = CmdLineInt("StubDrawInterface", 1);
	g_nIfaceAspect = CmdLineInt("StubIfaceAspect", 0);
	g_bSkipFade     = CmdLineInt("StubSkipFade", 0);
	g_bOnlyBig      = CmdLineInt("StubOnlyBig", 0);
	g_bFloorDump    = CmdLineInt("StubFloorDump", 0);
	g_bTexDumpAll   = CmdLineInt("StubTexDumpAll", 0);
	g_bProbe        = CmdLineInt("StubProbe", 1);
	R3D_SetProbeDump(CmdLineInt("StubProbeDump", 0));
	R3D_SetSkipHidden(CmdLineInt("StubSkipHidden", 1));
	g_bStereo2D     = CmdLineInt("StubStereo2D", 1);
	R2D_SetStereoScale(CmdLineInt("StubHudScale100", 100) / 100.0f);
	R2D_SetHudDepthCm((float)CmdLineInt("StubHudDepthCm", 250));
	R2D_SetMenuScale(CmdLineInt("StubMenuScale100", 0) / 100.0f);
	R2D_SetMenuBand(CmdLineInt("StubMenuBand100", 150) / 100.0f);
	R3D_SetMenuBand(CmdLineInt("StubMenuBand100", 150) / 100.0f);
	// The PAUSE menu over a live world: scale of the fitted layout. See the
	// note in R2D_BlitStereo - the 4:3 core zoom that suits the main menu
	// spills a left-aligned layout off the eye when the forward ray is not
	// at the centre. 100 is the old behaviour.
	// 115: the tester asked for the pause sub-menus a bit larger. The overlay is
	// one eye wide and the fitted layout has room.
	// 135: after the aim round, the tester asked for the menu a bit bigger.
	R2D_SetPauseScale(CmdLineInt("StubPauseScale100", 135) / 100.0f);
	// 55, not 100: the MASK pass is the unselected grey and the ADD pass
	// lifts the selected item to white. See the shader's overlay note.
	R2D_SetPauseTextWhite(CmdLineInt("StubPauseTextWhite100", 55) / 100.0f);
	// And shift it right, in hundredths of the layout's half-width, so the
	// LEFT-ALIGNED text column of NOLF's menus lands on the forward ray
	// rather than a hand's width to its left. 30 is where the column sits.
	R2D_SetPauseShift(CmdLineInt("StubPauseShift100", 30) / 100.0f);
	// +StubMenuZoom100 160 makes the menu half again as big.
	//
	// THE ANCHOR IS NOT IN NDC, AND THAT IS ON PURPOSE. NDC would need a minus
	// sign - the menu card sits at x -0.57 - and a bare "-570" on the command
	// line is a token the ENGINE's own parser sees before we do, sitting in the
	// same argument list as -rez and -windowtitle. atoi would have read it
	// correctly and the game might never have got that far.
	//
	// So it is thousandths ACROSS THE EYE: 0 is the left edge, 1000 the right,
	// 500 the centre, and the same for Y from bottom to top. Always positive.
	// 500/500 maps to NDC 0,0 which is the identity, so the defaults change
	// nothing.
	//
	// MEASURED, on the main menu and on Options, which share no other layout:
	// the card spans eye-NDC x -0.93..-0.21 and y -0.40..+0.47 on both, so its
	// centre is (-0.57, +0.04) - which in this encoding is X 215, Y 520.
	R3D_SetSkipInvisible(CmdLineInt("StubSkipInvisible", 1));
	g_fMenuZoomV    = CmdLineInt("StubMenuZoom100", 100) / 100.0f;
	g_fMenuAnchorXV = CmdLineInt("StubMenuAnchorX1000", 500) / 500.0f - 1.0f;
	g_fMenuAnchorYV = CmdLineInt("StubMenuAnchorY1000", 500) / 500.0f - 1.0f;
	R2D_SetMenuZoom(g_fMenuZoomV, g_fMenuAnchorXV, g_fMenuAnchorYV);
	R3D_SetDumpMin(CmdLineInt("StubTexDumpMin", 256));
	R3D_SetFlagHist(CmdLineInt("StubFlagHist", 0));
	g_nFrameDumpAt  = CmdLineInt("StubFrameDumpAt", 0);
	// HOW MANY CONSECUTIVE FRAMES to write, for finding something that
	// only exists BETWEEN frames. A flashing texture does not appear in a
	// single screenshot - 11 September, on the Morocco sniper roof, the
	// headset still showed a flashing texture that looked like it was
	// glitching out, and it did not show up in a single
	// screenshot. tools/flicker-scan.py needs at least six frames to
	// tell a pixel that ALTERNATES from one that is merely changing, and
	// the count was hardcoded to 1 with no way to ask for more.
	g_nFrameDumpCount = CmdLineInt("StubFrameDumpCount", 1);
	g_nFrameDumpOnFolder = CmdLineInt("StubFrameDumpOnFolder", 0);
	g_nFrameDumpEvery = CmdLineInt("StubFrameDumpEvery", 0);
	g_nTrace2D = CmdLineInt("StubTrace2D", 0);
	g_nFrameDumpCount = CmdLineInt("StubFrameDumpCount", 1);
	// CENTI-degrees. A crack three pixels wide is 0.3 degrees across at this
	// field, so whole degrees cannot be aimed at it - the ray misses and the
	// probe reports the wall either side, which reads as "no gap here".
	g_fProbeAz = (float)CmdLineInt("StubProbeAz", 99900) * 0.01f;
	g_fProbeEl = (float)CmdLineInt("StubProbeEl", 0) * 0.01f;
	g_nProbeSweep = CmdLineInt("StubProbeSweep", 1);
	g_fProbeStep  = (float)CmdLineInt("StubProbeStep", 10) * 0.01f;
	g_bModelWalk  = CmdLineInt("StubModelWalk", 0);
	if (g_nFrameDumpCount < 1) g_nFrameDumpCount = 1;
	R3D_SetSkipFlags((uint32_t)CmdLineInt("StubSkipFlags", 0));
	R2D_SetAlwaysUpload(CmdLineInt("StubAlwaysUpload", 0));
	g_bTrace17      = CmdLineInt("StubTrace17", 0);
	g_bWorldDump    = CmdLineInt("StubWorldDump", 1);
	g_bRebindReal   = CmdLineInt("StubRebindReal", 1);
	g_bDraw3D       = CmdLineInt("StubDraw3D", 1);
	R3D_SetTest(CmdLineInt("StubDraw3DTest", 0));
	// 0, NOT 1 - AND THIS IS WHY THERE WAS NEVER A SKY.
	//
	// Untextured polygons used to be DRAWN, in sky blue, because before there
	// was a real skybox that stand-in was the only thing that made the sky look
	// like sky (skipping those polygons turned the sky black). The sky BRUSHES are
	// exactly those polygons: the engine never loads a texture for them because
	// it draws the sky by another path entirely.
	//
	// So the stand-in was painting flat blue over the top of the real backdrop,
	// on every level, and the fix for the sky in September made no visible
	// difference because of it. Skipping them turns the sky brushes back into
	// the HOLES they are meant to be, and the panorama shows through.
	// +StubDrawUntextured 1 puts the old stand-in back.
	R3D_SetDrawUntextured(CmdLineInt("StubDrawUntextured", 0));
	R3D_SetSprAdd(CmdLineInt("StubSprAdd", 1));
	R3D_SetSprAddMod(CmdLineInt("StubSprAddMod", 1));
	R3D_SetDrawUntexturedTrans(CmdLineInt("StubDrawUntexturedTranslucent", 0));
	R3D_SetRevZ(CmdLineInt("StubRevZ", 1));
	R3D_SetNearOverride((float)CmdLineInt("StubNear", 0));
	R3D_SetFarOverride((float)CmdLineInt("StubFar", 0));
	g_bClearCyan    = CmdLineInt("StubClearCyan", 0);
	g_bSkipTint     = CmdLineInt("StubSkipBlackTint", 0);
	g_bScreenLock   = CmdLineInt("StubScreenLock", 1);
	g_bReqDump      = CmdLineInt("StubReqDump", 1);
	// OFF BY DEFAULT. This is the only diagnostic in the renderer that calls
	// INTO THE ENGINE - callback 2 to acquire a texture's data and callback 3
	// to release it - and it did that on all of the first 40 binds plus a
	// second time on the 30th. Everything it was built to find is long since
	// found: the texture object's layout is decoded, the mip chain is located,
	// and textures are read from their own .dtx files now. What is left is a
	// default-on diagnostic that can unbalance a refcount in a 25-year-old
	// engine on a folder change, which is where the 5 September crash happened.
	// +StubTexDump 1 to bring it back.
	g_bTexDump      = CmdLineInt("StubTexDump", 0);
	g_bLMProbe      = CmdLineInt("StubLMProbe", 1);
	// 200, NOT 100. LithTech stores lightmaps at HALF scale and the renderer
	// doubles them - the note by the pixel shader guessed this from four
	// lightmaps and then talked itself out of it because the level maximum is
	// 31 of 31. The picture settles it: at 100 the Morocco street is mean RGB
	// 70,71,70 against retail's 127,127,124 from the SAME head angle; at 200 it
	// is 121,119,113. Everything has been about 45% too dark since lightmaps
	// were implemented.
	//
	// 170 rather than a clean 200 because 200 CLIPS THE HIGHLIGHTS - on the HQ
	// tutorial the tiled wall blows out to flat white where retail still has
	// grout lines, which is what the headset reported as everything too bright.
	// Swept against retail from a fixed head angle on two levels that disagree
	// about everything else, and they agree on this:
	//
	//     StubLMScale100     100     140     170     200     230
	//     HQ  (T01S02)     70.20   54.79   48.88   50.98      -
	//     Morocco (M01S02) 69.74     -     53.38   54.22   60.81
	//
	// A fitted number, and it says so - but fitted with a deterministic
	// instrument against the real renderer on two levels, not guessed.
	//
	// WHAT THE LIGHTMAP DATA ACTUALLY CONTAINS, measured 18 September off
	// the atlas this renderer dumps (logs/lmatlas.bmp, T01S02):
	//
	//   used texels   min 24   median 66   max 255
	//   the low values are 24, 32, 41, 49, 57 - steps of ~8.2, so the
	//   data is 5 bit (0..31) expanded, and it reaches the TOP of the
	//   range. Not half-scale. Whatever 1.7 is, it is not a missing x2.
	//
	// THE FLOOR IS THE LEVEL'S OWN AMBIENT, AND IT IS ALREADY BAKED IN.
	// T01S02 authors AmbientLight 26 26 28; 26 quantised to 5 bits is 3
	// of 31, which expands to exactly 24 - the measured floor, on all
	// three channels, with NOTHING below it anywhere in the level.
	//
	// So retail does not add the level ambient to a lightmapped surface
	// at run time and neither should we. That kills a candidate that
	// looked very strong for an hour: levels declare ambient over a 10x
	// range (9.3 to 96.0 luminance over the 78 that declare one) and the
	// two levels this scale was fitted against sit at 26.1 (HQ) and 48.0
	// (Morocco) - a ratio of 1.84, which is almost exactly the gap
	// between the scales they each wanted. Coincidence. The quantisation
	// is what settles it, not the ratio.
	//
	// tools/dat-objects.py reads the authored value; the atlas dump and
	// a histogram of its non-zero texels is the rest of the measurement.
	// 100 SINCE 19 SEPTEMBER. 170 was a fit; a side-by-side comparison twice said the HQ is
	// much brighter than retail, and at 100 the Unity statue lands on retail's
	// own numbers (32.8 v 25-45). Morocco may read darker: +StubLMScale100 130.
	// 200 SINCE 19 SEPTEMBER: retail's MODULATE2X. At 100 a plain HQ wall
	// measured exactly half retail (77 v 149) while the statue was still above
	// it - a gain cannot do that, but 2x lightmaps plus a MULTIPLIED (not added)
	// reflection can: wall 154, dome clips white, statue 147 x 0.19 x 2 x 0.8 =
	// 45 against retail's 43. The "200 blows out the tiled wall" seen earlier
	// was the flat +43 additive env layer, since removed, not the 2x.
	g_nLMScale100   = CmdLineInt("StubLMScale100", 170);
	g_bLMOnly       = CmdLineInt("StubLMOnly", 0);
	g_bLMEnable     = CmdLineInt("StubLM", 1);
	g_nAniso        = CmdLineInt("StubAniso", 16);
	g_bSkyStandIn   = CmdLineInt("StubSkyStandIn", 1);
	g_bSkyBox       = CmdLineInt("StubSkyBox", 1);
	g_nModelDims    = CmdLineInt("StubModelDims", 1024);
	g_bFastReadOpt  = CmdLineInt("StubFastRead", 1);
	g_nWorldBSPOpt  = CmdLineInt("StubWorldBSP", 1);
	g_bBSPCoverOpt  = CmdLineInt("StubBSPCover", 0);
	g_nTransAlpha   = CmdLineInt("StubTransAlpha", 45);
	R3D_SetCutProbe(CmdLineInt("StubCutProbe", 0));
	R3D_SetCutGuess(CmdLineInt("StubCutGuess", 1));
	R3D_SetEnvNoCut(CmdLineInt("StubEnvNoCut", 1));
	R3D_SetPieceMatIndex(CmdLineInt("StubPieceMatIndex", 1));
	R3D_SetSkinFrame(CmdLineInt("StubSkinFrame", 0));
	R3D_SetSprStrictTex(CmdLineInt("StubSprStrictTex", 1));
	R3D_SetSprNameProbe(CmdLineInt("StubSprNameProbe", 0));
	R3D_SetSprFromFile(CmdLineInt("StubSprFromFile", 1));
	R3D_SetSkinNameProbe(CmdLineInt("StubSkinNameProbe", 0));
	R3D_SetSkinFromButes(CmdLineInt("StubSkinFromButes", 1));
	R3D_SetHideViewArms(CmdLineInt("StubHideViewArms", 1));
	// THE BODY DRAWS NOW, AND SHIPS OFF. Headset testing found no in-game
	// body for the player. It drew nothing before 8 September because the player's
	// own model is flagged invisible in first person and the invisible-skip
	// rule honoured that; the rule now exempts the player when this is on, and
	// looking straight down shows legs and torso.
	//
	// OFF because the ARMS come with it. Measured from HERO_ACTION.ABC: the
	// arms are baked into the upper-body piece (Stitch69, bind y 3..37, x
	// +-18) and the legs are their own piece (Stitch51, y -53..18). With the
	// body on, a second, static pair of arms sits at chest height in front of
	// the face beside the view model's tracked hands, and VRArmIK swings them
	// wide and low rather than to the controller. Two pairs of arms is worse
	// than none.
	//
	// THE RULE THAT WOULD MAKE IT SHIPPABLE, with the number already measured:
	// when the body is on, skip any piece of the player's model whose
	// bind-pose minimum Y is above -5 - that hides the upper body, hands and
	// head and keeps the legs. The bind positions are read in the skinning
	// loop below the hide site; wiring them up to it is the piece of work.
	// "Show body" on the VR page turns this on for anyone who wants to look.
	R3D_SetDrawBody(CmdLineInt("StubBody", 0));
	R3D_SetHideHead(CmdLineInt("StubHideHead", 1));
	R3D_SetFog(CmdLineInt("StubFog", 1));
	R2D_SetDump2D(CmdLineInt("StubDump2D", 0));
	// +StubPauseQuad 0 draws the pause menu in the eyes again (head-welded).
	R2D_SetPauseQuad(CmdLineInt("StubPauseQuad", 1));
	Dtx_SetZeroAlphaRule(CmdLineInt("StubZeroAlpha", 1));
	R3D_SetSkinNamePeriod(CmdLineInt("StubSkinNamePeriod", 30));
	R3D_SetModelScale(CmdLineInt("StubModelScale", 1));
	// ON now. It was measured as "no help" (1.12 against a 5.95 floor) and
	// that measurement was true of a menu whose sprites were all HALF SIZE:
	// nothing overlapped, so nothing could be in the wrong order. At the
	// right size the panel covers THE OPERATIVE and the list order draws it
	// after - so the title vanished. Sorted back to front it comes back at
	// x 0.475..0.707, y 0.051..0.135 of the frame against retail's
	// 0.473..0.715, 0.048..0.139.
	R3D_SetSprSort(CmdLineInt("StubSprSort", 1));
	R3D_SetAddBlend(CmdLineInt("StubAddBlend", 1));
	R3D_SetMulBlend(CmdLineInt("StubMulBlend", 1));
	R3D_SetModelGlass(CmdLineInt("StubModelGlass", 1));
	R3D_SetVolumeSurface(CmdLineInt("StubVolumeSurface", 1));
	R3D_SetVertexColour(CmdLineInt("StubVertexColour", 1));
	R3D_SetSkyFogAllow(CmdLineInt("StubSkyFog", 1));
	R3D_SetLightAddAllow(CmdLineInt("StubLightAdd", 1));
	R3D_SetBatchList(CmdLineInt("StubBatchList", 0));
	// THE AUTHORED TEXTURE IS THE PICTURE; THE MAP IS A SHEEN ON IT.
	//
	// Round 5 raised this to 100 so "the streaks are the picture rather than a
	// sheen over a still texture", which makes the reflection the whole
	// waterfall - and a reflection is what the headset then showed: a looping
	// reflective shine. Retail's sheet is its own WA0010 streak texture, which
	// reads as falling water because of what is drawn on it, with the
	// environment map adding a slow shimmer at EnvScale 1.0. 30 puts the
	// authored art back in charge.
	R3D_SetWaterLook(CmdLineInt("StubWaterLight100", 80) / 100.0f, CmdLineInt("StubEnvContrast100", 30) / 100.0f);
	// The water volume's own top face, which its polygrid already draws.
	R3D_SetWaterTop(CmdLineInt("StubWaterTop", 1));
	// The environment layer tests depth. +StubEnvDepth 0 is the old way.
	R3D_SetEnvDepth(CmdLineInt("StubEnvDepth", 1));
	// Polygrids before the glass they are seen through.
	R3D_SetWaterFirst(CmdLineInt("StubWaterFirst", 1));
	// THE WATERFALL SCROLLS ITS OWN TEXTURE, at retail's measured rate.
	//
	// 0.20 Hz. A single point inside the sheet in the 13:00 clip oscillates
	// once every 5.1 seconds - measured at four separate points over 612
	// frames, three of which agree exactly and the fourth reads 3.4 s. One
	// texture repeat therefore passes a given point every five seconds, and
	// the scroll is 0.20 repeats a second. Slow: retail's sheet reads as
	// falling water because of what is PAINTED on it, not because it travels.
	//
	// The first attempt at this number said 6.18 repeats a second and the
	// headset showed it racing, which is 31x. It came from a 2D FFT of a
	// space-time image whose low bins had been zeroed to avoid reading the
	// window size as a signal - and that guard, at 0.245 Hz, deleted the
	// real 0.20 Hz fundamental and left a harmonic behind. A guard against
	// one artefact removed the answer. The single-point measurement above
	// needs no such guard and is the one to trust.
	//
	// NEGATIVE runs it DOWN: the first version was seen flowing upward, so the
	// level's V axis points up the face here.
	//
	// Repeats per second x 100. 0 stops it.
	R3D_SetWaterFlow(CmdLineInt("StubWaterFlow", -20) / 100.0f);
	R3D_SetEnvWorld((float)CmdLineInt("StubEnvRepeatU", 40), (float)CmdLineInt("StubEnvRepeatV", 240),
					(float)CmdLineInt("StubEnvFlow", 50));
	R3D_SetSkipBatch(CmdLineInt("StubSkipBatch", -1));
	R3D_SetEnvCoord(CmdLineInt("StubEnvCoord100", 400) / 100.0f);
	// THE PAN IS RETAIL'S OWN, AND WE WERE NINE TIMES FASTER.
	//
	// The loop is the one complaint that survived every version of this
	// waterfall. Round 4 was rejected as "a static texture that has a
	// reflective light looping" and round 5, with the map at full strength,
	// as a looping shine - opposite settings, same word.
	// Chasing the look while leaving the SPEED alone could not have fixed
	// either, because the speed is what makes it a loop.
	//
	// The retail renderer states it: the client logs its console variables
	// at font init and d3d.ren reports EnvPanSpeed 0.0005 - PER FRAME. At
	// the 90 fps this runs at that is 0.045 of the map a second, and a full
	// cycle takes twenty-two seconds. We were panning 0.40 a second, which
	// cycles every two and a half - about the interval at which a person
	// says "it loops".
	//
	// 5/100 = 0.05 a second, the nearest this integer switch reaches to
	// retail's 0.045. StubEnvPan100 0 stops it dead.
	R3D_SetEnvMap(CmdLineInt("StubEnvMap", 1), CmdLineInt("StubEnvScale100", 12) / 100.0f,
				  CmdLineInt("StubEnvPan100", 5) / 100.0f);
	R3D_SetMulForce(CmdLineInt("StubMulForce", 0));
	R3D_SetCrashTest(CmdLineInt("StubCrashTest", 0));
	R3D_SetSkipStill(CmdLineInt("StubSkipStill", 0));
	// DEFAULT 1 NOW, and the cap is why. VRModelCap went 192 -> 512 at the
	// tester's request, so Morocco publishes 244 instances instead of 192 and
	// drops none. Without the cache that build costs 5.9 ms and 9.7 in the
	// later part of a run; with it, 1.9. The cap asked for is only affordable
	// with this on. Desk-verified - same picture to within the harness noise
	// floor, hit rate within two points of R3D STILL's own figure - but NOT
	// yet seen in a headset: +StubSkinCache 0 is one switch away.
	R3D_SetSkinCache(CmdLineInt("StubSkinCache", 1));
	// +StubMeshPool: unchanged instances drawn from a resident GPU pool instead
	// of being copied into the per-frame ring every frame (see g_pPoolVB).
	R3D_SetMeshPool(CmdLineInt("StubMeshPool", 1));
	// +StubSkinDiag 1: the skinning path's statistics (see g_bSkinDiag).
	R3D_SetSkinDiag(CmdLineInt("StubSkinDiag", 0));
	// +StubMemKindCache: MemKind answers from verified cached regions (render3d.cpp).
	R3D_SetMemKindCache(CmdLineInt("StubMemKindCache", 1));
	R3D_SetModelLight(CmdLineInt("StubModelLight", 1));
	R3D_SetTerrainGrid(CmdLineInt("StubTerrainGrid", 1));
	R3D_SetLightScaleAllow(CmdLineInt("StubLightScale", 1));
	R3D_SetLightScaleTest(CmdLineInt("StubLightScale100", 0));
	R3D_SetPubSkins(CmdLineInt("StubPubSkins", 1));
	R3D_SetSkyOccluder(CmdLineInt("StubSkyOccluder", 1));
	R3D_SetLightObjects(CmdLineInt("StubLightObjects", 0));
	R3D_SetLightDirect(CmdLineInt("StubLightDirect", 1));
	R3D_SetLightGain(CmdLineInt("StubLightGain100", 100) / 100.0f);
	R3D_SetAddLast(CmdLineInt("StubAddLast", 1));
	R3D_SetMeshRing(CmdLineInt("StubMeshRing", 4));
	g_bTransNamesOpt= CmdLineInt("StubTransNames", 1);
	g_nWorldRebuild = CmdLineInt("StubWorldRebuild", 0);
	g_bWorldProbeOpt= CmdLineInt("StubWorldProbe", 0);
	g_bSprProbeOpt  = CmdLineInt("StubSpriteProbe", 0);
	g_bSpritesOpt   = CmdLineInt("StubSprites", 1);
	g_bPrimsOpt     = CmdLineInt("StubPrims", 1);
	g_bDynLightsOpt = CmdLineInt("StubDynLights", 1);
	g_nSprScaleOpt  = CmdLineInt("StubSpriteScale100", 100);
	g_bWorldXformOpt= CmdLineInt("StubWorldXform", 1);
	g_nWorldNudge   = CmdLineInt("StubWorldNudge", 0);
	g_bSkipTrans    = CmdLineInt("StubSkipTranslucent", 0);
	g_bDrawNoPixels = CmdLineInt("StubDrawNoPixels", 0);
	g_bBlendModes   = CmdLineInt("StubBlendModes", 3);
	g_bColour2D     = CmdLineInt("StubColour2D", 1);
	g_bColourKey    = CmdLineInt("StubColourKey", 1);
	// DEFAULT OFF. The bit-31 test believed only FFFFFFFF, which is the
	// "no transparency" value - see the note at the test. +StubKeyMask 1
	// restores it as a control. The variable's own initialiser was
	// already 0 and this line was quietly overriding it.
	const int nKeyMask  = CmdLineInt("StubKeyMask", 0);
	// Default ON. With it off, every menu item drew in an opaque black box;
	// with it on they match retail and nothing else in the frame changed.
	// +StubKeyBlack 0 is the control.
	const int nKeyBlack = CmdLineInt("StubKeyBlack", 1);
	g_bFolder2DUse      = CmdLineInt("StubFolder2D", 0);
	R2D_SetKeySoft((float)CmdLineInt("StubKeySoft", 32));
	g_bModelBoxes   = CmdLineInt("StubModelBoxes", 0);
	// Off by default. It changes what reaches the player's eyes and it has
	// never been in a headset.
	g_bNativeFrustum = CmdLineInt("StubNativeFrustum", 0);

	// Installed before anything else can fault. Off with +StubCrashLog 0,
	// because a filter that is itself broken would hide the very thing it
	// exists to show.
	if (CmdLineInt("StubCrashLog", 1)) SetUnhandledExceptionFilter(StubCrashFilter);
	R3D_SetLMScale(g_nLMScale100 / 100.0f);
	// MULTISAMPLING for the world pass.
	//
	// DEFAULT 4, AND IT COSTS NOTHING MEASURABLE. Before today this renderer
	// had no anti-aliasing of any kind - every SampleDesc.Count in the module
	// was 1 - and the only smoothing anywhere was the host's FXAA, applied
	// AFTER the image had been stretched to reach what the runtime wanted.
	//
	// Measured at 3840x2076, same scene: 84.5 fps at x1, x4 and x8 alike. The
	// game is not fill-rate bound, so edge quality here is nearly free.
	//
	// The first build of it drew sky-coloured shards through Morocco's
	// alpha-tested lattice. That was not a multisampling bug: the world pass
	// does not write every pixel and had always been inheriting the previous
	// frame's back buffer on the ones it misses. See the clear in
	// R3D_DrawWorld, which is what fixed it. StubMSAA 1 turns it off.
	R3D_SetMsaa(CmdLineInt("StubMSAA", 4));
	R3D_SetMsaaClear(CmdLineInt("StubMsaaClear", 0));
	R3D_SetLMOnly(g_bLMOnly);
	R3D_SetLMEnable(g_bLMEnable);
	R3D_SetAniso(g_nAniso);
	R3D_SetSkyStandIn(g_bSkyStandIn);
	R3D_SetSkyBox(g_bSkyBox);
	R3D_SetModelDims((float)g_nModelDims);
	R3D_SetFastRead(g_bFastReadOpt);
	R3D_SetWorldBSP(g_nWorldBSPOpt);
	R3D_SetBSPCover(g_bBSPCoverOpt);
	R3D_SetTransAlpha((float)g_nTransAlpha / 100.0f);
	R3D_SetTransNames(g_bTransNamesOpt);
	R3D_SetWorldRebuild(g_nWorldRebuild);
	R3D_SetWorldProbe(g_bWorldProbeOpt);
	R3D_SetSpriteProbe(g_bSprProbeOpt);
	R3D_SetSprites(g_bSpritesOpt);
	R3D_SetPrims(g_bPrimsOpt);
	R3D_SetDynLights(g_bDynLightsOpt);
	R3D_SetSpriteScale((float)g_nSprScaleOpt / 100.0f);
	R3D_SetWorldXform(g_bWorldXformOpt);
	R3D_SetWorldSeed(CmdLineInt("StubWorldSeed", 1));
	R3D_SetDrawHiddenWM(CmdLineInt("StubDrawHiddenWM", 0));
	// 0: an unmatched Door/Switch/TranslucentWorldModel is not drawn (removed
	// by ObjectRemover); Terrain and the volume brushes are, whatever this
	// says. 1 draws them all, the way it was. See IsVolumeClass in render3d.
	R3D_SetDrawUnmatchedWM(CmdLineInt("StubDrawUnmatchedWM", 0));
	R3D_SetWorldNudge((float)g_nWorldNudge);
	R3D_SetSkipTranslucent(g_bSkipTrans);
	R3D_SetDrawNoPixels(g_bDrawNoPixels);
	R2D_SetBlendModes(g_bBlendModes);
	R2D_SetColourEnabled(g_bColour2D);
	R2D_SetColourKeyEnabled(g_bColourKey);
	R2D_SetKeyMask(nKeyMask);
	R2D_SetKeyBlack(nKeyBlack);
	R3D_SetModelBoxes(g_bModelBoxes);
	R3D_SetModelMesh(CmdLineInt("StubModelMesh", 1));
	R3D_SetModelOnly(CmdLineInt("StubModelOnly", 0));
	// Was 1 while the models had no texture and orange was the only way to
	// tell them from the sky. The skins are found now (obj+0x1B8), so the
	// real texture is the default and orange is the diagnostic.
	R3D_SetMeshTint(CmdLineInt("StubMeshTint", 0));
	R3D_SetTexHunt(CmdLineInt("StubMeshTexHunt", 0));
	R3D_SetIdxHunt(CmdLineInt("StubMeshIdx", 0));
	R3D_SetBeamDump(CmdLineInt("StubBeamDump", 0));
	{
		char szProbe[32];
		CmdLineStr("StubModelProbe", szProbe, sizeof szProbe);
		{ char szSkySkip[64] = ""; CmdLineStr("StubSkySkip", szSkySkip, sizeof szSkySkip); R3D_SetSkySkip(szSkySkip); }
		R3D_SetWaterFlowAny(CmdLineInt("StubWaterFlowAny", 0));
		R3D_SetWaterBias(CmdLineInt("StubWaterBias", 0));
		R3D_SetVolumeVisible(CmdLineInt("StubVolumeVisible", 0));
		R3D_SetSkyInGlass(CmdLineInt("StubSkyInGlass", 0));
		R3D_SetSkyCamCentre(CmdLineInt("StubSkyCamCentre", 0));
		R3D_SetSkyTrace(CmdLineInt("StubSkyTrace", 0));
		R3D_SetSkyParallax(CmdLineInt("StubSkyParallax", 1));
		R3D_SetMipOffsetUV(CmdLineInt("StubMipOffsetUV", 1));
		R3D_SetMipOffsetSky(CmdLineInt("StubMipOffsetSky", 0));
		R3D_SetWorldCull(CmdLineInt("StubWorldCull", 2));
		R3D_SetCullFromEye(CmdLineInt("StubCullFromEye", 1));
		{ char szBW[64] = ""; CmdLineStr("StubBatchWatch", szBW, sizeof szBW); R3D_SetBatchWatch(szBW); }
		{ char szMW[64] = ""; CmdLineStr("StubModelWatch", szMW, sizeof szMW); R3D_SetModelWatch(szMW); }
		R3D_SetGlassDepth(CmdLineInt("StubGlassDepth", 1), CmdLineInt("StubGlassDepthCut100", 0));
		R3D_SetSkyChecker(CmdLineInt("StubSkyChecker", 0));
		R3D_SetModelProbe(szProbe);
	}
	R3D_SetObjScale(CmdLineInt("StubObjScale", 1));
	R3D_SetPieceBase(CmdLineInt("StubPieceBase", 1));
	R3D_SetPieceVerts(CmdLineInt("StubPieceVerts", 0));
	R3D_SetPieceVA(CmdLineInt("StubPieceVA", 1));
	R3D_SetPieceSkin(CmdLineInt("StubPieceSkin", 1));
	R3D_SetVtxNrm(CmdLineInt("StubVtxNormals", 1));
	R3D_SetNodeT(CmdLineInt("StubNodeT", 0));
	R3D_SetEntryCount(CmdLineInt("StubEntryCount", 0));
	R3D_SetWeightPre(CmdLineInt("StubWeightPre", 1));
	R3D_SetEdgeChk(CmdLineInt("StubEdgeChk", 0));
	R3D_SetFlushOnLoad(CmdLineInt("StubFlushOnLoad", 1));
	R3D_SetSkinConsist(CmdLineInt("StubSkinConsist", 0));
	R3D_SetAlphaTest(CmdLineInt("StubAlphaTest", 1));
	R3D_SetA2C(CmdLineInt("StubA2C", 1));
	R3D_SetAlphaSharp(CmdLineInt("StubAlphaSharp", 1));
	R3D_SetDrawSections(CmdLineInt("StubDrawSections", 0));
	R3D_SetCull(CmdLineInt("StubCull", 0));
	R3D_SetSkipOcclTex(CmdLineInt("StubSkipOcclTex", 1));
	R3D_SetSkinPull(CmdLineInt("StubSkinPull", 1));
	R3D_SetTexName(CmdLineInt("StubTexName", 0));
	Dtx_SetLog(Log);
	// 0 = no cap. The stock game's textures top out at 512, so this only ever
	// bites on an upscale pack - where 2048x2048 uncompressed skins exhaust a
	// 32-bit process and crash inside the display driver.
	Dtx_SetMaxDim(CmdLineInt("StubTexMaxDim", 0));
	// MODE 1 BY DEFAULT: the file's pixels for every texture the heap path
	// also accepts, which is the same set of polygons drawn either way -
	// desk-verified at 254 batches both ways, and 478 of 478 textures agreed
	// with the engine's own copy on dimensions and mean colour. Mode 2 needs
	// stage 2 first; see R3D_SetTexFromFile.
	R3D_SetTexFromFile(CmdLineInt("StubTexFromFile", 1));
	// STAGE 2. Reads the level .DAT beside the heap walk and reports the
	// two against each other. Draws nothing yet, so 0 is only for taking
	// the parse out of the picture if a level load ever misbehaves.
	R3D_SetWorldFile(CmdLineInt("StubWorldFile", 1));
	R3D_SetSurfProbe(CmdLineInt("StubSurfProbe", 0));
	// The file-derived marker rule. DEFAULT OFF as of 5 September, after it
	// deleted real scenery and the skybox on the intro level.
	//
	// The rule itself is sound - flag 202 is worn by AI.dtx, Invisible.dtx and
	// Occluder.dtx and by NOTHING else, swept over all 103 worlds. What is not
	// established is the BRIDGE from a heap polygon to a file surface, which
	// was measured on m01s02 and on m01s02 only, and then shipped as the
	// default. That is the trap this repo already had written down: two
	// agreeing samples are not a sweep, and 565 of 565 models on ONE level is
	// one sample.
	//
	// It stays off until StubBridgeCheck says the mapping is right on more
	// than one level.
	R3D_SetMarkerFlags((uint32_t)CmdLineInt("StubMarkerFlags", 0));
	R3D_SetBridgeCheck(CmdLineInt("StubBridgeCheck", 0));
	// Draw polygons the engine never textured, using the level file's own
	// texture name. Adds only; 0 restores the old drop-them behaviour.
	R3D_SetFileTex(CmdLineInt("StubFileTex", 1));
	// Hide the editor marker textures the file names. On by default: the
	// rescue above gives these polygons their real pictures back, and their
	// real pictures are the word "sky" and magenta INVISIBLE stripes.
	R3D_SetMarkerTex(CmdLineInt("StubMarkerTex", 1));
	g_bMarkerLast = CmdLineInt("StubMarkerLast", 1);
	g_bDumpOverlay = CmdLineInt("StubDumpOverlay", 0);
	R2D_SetOverlayTrace(g_bDumpOverlay);
	// STAGE 0 of docs/PLAN-FILES-NOT-HEAP.md: can this renderer open one of the
	// game's own files by name? Everything after this stage depends on it, so
	// it says so in the log on every run rather than being assumed.
	//
	// The self-test is end to end and against a value nobody here chose: the
	// first bytes of an .abc model are a length-prefixed section name, and the
	// first section of every LithTech model is called "Header". If the mount
	// order, the directory walk, the name lookup and the read are all right,
	// those six characters come back. If any one of them is wrong, they do not.
	if (CmdLineInt("StubRezFS", 1))
	{
		const uint32_t nFiles = RezFS_Init(Log);
		// Which skin belongs to which model, from the game's own attribute
		// files. Needs the archives mounted and nothing else, so it goes here.
		Butes_Load(Log);
		const uint32_t nDtx = RezFS_ForEach(".DTX", nullptr, nullptr);
		const uint32_t nAbc = RezFS_ForEach(".ABC", nullptr, nullptr);
		const uint32_t nDat = RezFS_ForEach(".DAT", nullptr, nullptr);
		Log("  REZ: %u files in %u archives - %u textures, %u models, %u worlds",
			nFiles, RezFS_ArchiveCount(), nDtx, nAbc, nDat);

		const char* pszProbe = "CHARS/MODELS/HERO_ACTION.ABC";
		uint32_t nSize = 0;
		uint8_t* pData = RezFS_Read(pszProbe, &nSize);
		if (!pData)
			Log("  REZ SELFTEST: FAILED - %s not found", pszProbe);
		else
		{
			const bool bOK = (nSize > 8 && pData[0] == 6 && pData[1] == 0
							  && !memcmp(pData + 2, "Header", 6));
			Log("  REZ SELFTEST: %s - %s, %u bytes, first section '%.*s'",
				bOK ? "the file system works" : "FAILED",
				pszProbe, nSize, (nSize > 8) ? 6 : 0,
				(nSize > 8) ? (const char*)(pData + 2) : "");
			RezFS_Free(pData);
		}

		// STAGE 1: does the .dtx parse land on the byte the header declares?
		//
		// A DTX is a 164-byte header followed by a mip chain with no padding and
		// nothing after it, so `164 + sum(mip bytes) == file size` is an identity
		// the file either satisfies or does not. Every texture in the mounted
		// archives is checked, against the SAME mount order the game will use -
		// so a different install or a changed -rez list cannot quietly invalidate
		// the offline sweep that first established it.
		//
		// A count that agrees is not a parse that works, and this is only the
		// count: the REZ reader matched lithrez on 4871 of 4871 sizes while 4754
		// of them had the wrong NAME. What proves the pixels are right is the
		// heap-versus-file comparison in R3D_NoteTexture, which needs +StubTexName.
		if (CmdLineInt("StubDtxTest", 1))
		{
			uint32_t nSeen = 0;
			Dtx_SelfTest(Log, &nSeen);
		}
	}

	g_bDumpOnModels = CmdLineInt("StubDumpOnModels", 0);
	g_bDumpSurfaces = CmdLineInt("StubDumpSurfaces", 0);
	// Not before this present. The first armed dump fired at present 252,
	// during the level fade-in, and the picture was black - a model was
	// genuinely in view and there was still nothing to see.
	g_nDumpAfter = CmdLineInt("StubDumpAfter", 2000);
	// DEFAULT ONE. Each dump reads a 14 MB back buffer back on the render
	// thread and writes 10 MB to disk, which stalls the renderer for a good
	// fraction of a second. LithTech takes one simulation step per rendered
	// frame, so that stall is handed to the engine as a single huge timestep
	// and every enemy moves, aims and fires for all of it at once. Eight of
	// them cost two driving sessions: a handgun sounded like a machine gun and
	// killed the player on spawn, while the same build with no dumps armed
	// played perfectly at 88 FPS. Use tools/drive-capture.ps1 to photograph
	// the window from outside instead; it costs the game nothing.
	g_nDumpMax = CmdLineInt("StubDumpMax", 1);
	g_nMaxFps  = CmdLineInt("StubMaxFps", 90);
	g_bSyncHost = CmdLineInt("StubSyncHost", 1);
	g_bGpuEyeCopy = CmdLineInt("StubGpuEyeCopy", 1);
	g_bWatchdog = CmdLineInt("StubWatchdog", 1);
	g_nPresentEvery = CmdLineInt("StubPresentEvery", 0);
	{
		static char s_szLMDump[MAX_PATH];
		if (CmdLineInt("StubLMDump", 0))		// a desk tool: +StubLMDump 1
		{
			sprintf_s(s_szLMDump, "%slogs/lmatlas.bmp", g_szDir);
			R3D_SetLMDump(s_szLMDump);
		}
	}
	if (g_bTrace17) g_bDelegateInit = 1;
	if (g_bTrace4) g_bDelegateInit = 1;		// the real slot needs a real device
	if (g_nDelegateLo >= 0 && g_nDelegateHi < g_nDelegateLo) g_nDelegateHi = kSlots - 1;
	if (g_nDelegateLo >= 0) g_bDelegateInit = 1;

	if (g_nReload)
	{
		Log("");
		Log("=== RELOADED by the engine, same process - appending ===");
		Log("Everything above is EARLIER IN THIS SAME RUN. The engine frees and");
		Log("reloads the render DLL mid-session; the counters below start from");
		Log("zero again, so a total here is one segment, not the whole run.");
	}
	if (g_nReload)
		Log("=== renderer RE-INITIALISED in the same process: the engine frees and"
			" reloads this DLL on every focus loss and gain (LTEVENT 6/4, then 7/5/3)."
			" Everything below is a fresh instance; the world will be rebuilt. ===");
	// THE START STAMP IS PART OF THE IDENTITY, not decoration: a pid on its own
	// is reused within minutes and makes a new run append to the last one's
	// log. See the reload test above.
	Log("=== d3dstub.ren - the first renderer we own === (pid %lu start %llu)",
		(unsigned long)GetCurrentProcessId(), g_nLogStart);
	Log("built %s %s", __DATE__, __TIME__);
	Log("switches: StubDelegateInit %d   StubDevice %d   StubSceneDump %d"
		"   StubDelegate %d..%d   (from the command line)",
		g_bDelegateInit, g_bWantDevice, g_nSceneDumpAt,
		g_nDelegateLo, g_nDelegateHi);
	Log("switches: StubDraw3D %d   StubDraw3DTest %d   StubSkipBlackTint %d"
		"   StubClearCyan %d",
		g_bDraw3D, CmdLineInt("StubDraw3DTest", 0), g_bSkipTint, g_bClearCyan);
	Log("switches: StubMSAA %d asked", CmdLineInt("StubMSAA", 4));
	Log("switches: StubLM %d   StubLMOnly %d   StubLMScale100 %d"
		"   StubLMProbe %d   StubAniso %d",
		g_bLMEnable, g_bLMOnly, g_nLMScale100, g_bLMProbe, g_nAniso);
	Log("switches: StubRevZ %d   (1 = reversed-Z depth)",
		CmdLineInt("StubRevZ", 1));
	Log("switches: StubNativeFrustum %d   StubRebindRet %d   StubWarpRet %d"
		"   StubEyeShare %d",
		g_bNativeFrustum, g_nRebindRet, g_nWarpRet, g_bEyeShare);

	// The whole command line, verbatim. A log that lists switch VALUES cannot
	// answer "which script launched this" - and a session was called a
	// regression against a run it had never shared a configuration with.
	Log("command line: %s", GetCommandLineA() ? GetCommandLineA() : "(none)");

	// The engine writes RenderDll back into autoexec.cfg on exit, so this DLL
	// loads on every launch from then on - including launches that pass none of
	// the switches it needs. Without StubNativeFrustum the world is drawn once
	// per frame with no per-eye asymmetric projection: that is the pre-2
	// September arrangement, and NO headset result in this project was obtained
	// with it. Say so where it will be read, rather than leaving it to be
	// inferred from a switch line six lines up.
	if (!g_bNativeFrustum)
	{
		Log("");
		Log("*** StubNativeFrustum is 0. This is OUR renderer WITHOUT the VR");
		Log("*** pairing: no per-eye asymmetric projection, so the client is");
		Log("*** rotating the camera onto each eye instead - the pre-2 September");
		Log("*** arrangement, which is what the world BENDING was. Every headset");
		Log("*** result in this project was obtained with StubNativeFrustum 1,");
		Log("*** VRAsymFrustum 0 and VRCrosshair 0 together. Do not compare this");
		Log("*** run against one of those. Launch with play-vr.ps1 -Native.");
		Log("*** (It still draws BOTH eyes - measured, two desk arms, 2.00 world");
		Log("*** passes per frame either way. This switch is not a mono switch.)");
		Log("");
	}
	Log("It draws nothing. The question is whether the engine accepts our table");
	Log("and keeps calling us, not whether anything appears on screen.");

	char szReal[MAX_PATH];
	sprintf_s(szReal, "%sd3d.ren", szDir);
	g_hReal = LoadLibraryA(szReal);
	if (g_hReal)
	{
		// Only RenderDLLSetup is still borrowed. The mode list is answered
		// here - see GetSupportedModes - because the real one enumerates
		// through dgVoodoo's DirectDraw and that crashed OPTIONS -> DISPLAY.
		g_pfnSetup = GetProcAddress(g_hReal, "RenderDLLSetup");
		NoteModule("d3d.ren", g_hReal);
		Log("RenderDLLSetup borrowed from %s (%p); the mode list is our own", szReal, g_pfnSetup);
	}

	g_hReportStop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	g_hReportThread = CreateThread(nullptr, 0, ReportThread, nullptr, 0, nullptr);
	return TRUE;
}
