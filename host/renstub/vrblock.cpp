#include "vrblock.h"

#include <windows.h>
#include <math.h>
#include <string.h>

// The contract, from the client tree. One copy, as VRShared.h's own header
// comment insists - the host includes it the same way.
#include "VRShared.h"

namespace
{
	HANDLE          g_hMap    = nullptr;
	VRSharedState*  g_pBlock  = nullptr;
	DWORD           g_dwRetry = 0;
	VRB_LogFn       Log       = nullptr;
	int             g_bLogged = 0;
	int             g_bSaidNoHost = 0;
	int             g_bSaidNoEyeTex = 0;	// "there is no block" - said once

	// Why the last VRB_EyeFrustum said no. The renderer publishes
	// nNativeFrustum 0 for a scene it could not build a frustum for, and that
	// scene is then drawn with the SYMMETRIC projection - so a rare failure is
	// a rare wrong frame, not merely a missing offset. Guessing which of four
	// exits took it is exactly the kind of thing this project measures instead.
	const char*     g_pszLastFail = "none";

	// The last frustum each eye was successfully given, so a torn read can be
	// answered with last frame's optics instead of a wrong projection.
	VRBFrustum      g_LastGood[2] = {};
	int             g_bHaveLast[2] = { 0, 0 };
	long            g_nReused = 0;

	size_t          g_nMapped  = 0;		// bytes the host actually published
	uint32_t        g_nVersion = 0;

	// A seqlock read. The host makes nSequence odd before writing and even
	// after, so a snapshot taken across an odd value, or across a change, is
	// torn and must be discarded rather than used.
	bool Snapshot(VRSharedState* pOut)
	{
		if (!g_pBlock) return false;
		for (int nTry = 0; nTry < 4; ++nTry)
		{
			const uint32_t a = g_pBlock->nSequence;
			if (a & 1) continue;
			_ReadWriteBarrier();
			// Never copy more than the host published. A shorter block is a
			// valid older host, not an error, and the fields past its end stay
			// zero rather than being whatever follows the section.
			*pOut = VRSharedState{};
			memcpy(pOut, g_pBlock,
				   (g_nMapped && g_nMapped < sizeof(VRSharedState))
				   ? g_nMapped : sizeof(VRSharedState));
			_ReadWriteBarrier();
			if (g_pBlock->nSequence == a) return true;
		}
		g_pszLastFail = "seqlock torn 4 times";
		return false;
	}
}

bool VRB_Poll(VRB_LogFn pfnLog)
{
	if (pfnLog) Log = pfnLog;
	if (g_pBlock) return true;

	const DWORD dwNow = GetTickCount();
	if (dwNow < g_dwRetry) return false;
	g_dwRetry = dwNow + 500;

	g_hMap = OpenFileMappingA(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, VRSHARED_NAME);
	if (!g_hMap)
	{
		if (!g_bSaidNoHost && Log)
		{
			g_bSaidNoHost = 1;
			Log("  VRB: no block named %s (error %lu) - no host is running, so"
				" the symmetric projection stands. Retrying every 500 ms and"
				" this is the only time it will be said.",
				VRSHARED_NAME, GetLastError());
		}
		return false;
	}

	// 0, meaning the WHOLE section, not sizeof(VRSharedState).
	//
	// The shipped host still creates a version 12 block of 288 bytes, and this
	// renderer's struct is now 312. Asking for 312 bytes of a 288-byte section
	// fails outright, so a newer renderer would simply stop attaching to the
	// host that is actually installed - and the symptom would be the native
	// frustum silently switching off. The contract only ever appends, so
	// mapping what is there and checking the version is the correct read.
	void* p = MapViewOfFile(g_hMap, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
	if (!p) { CloseHandle(g_hMap); g_hMap = nullptr; return false; }

	VRSharedState* pS = (VRSharedState*)p;
	if (pS->nMagic != VRSHARED_MAGIC)
	{
		if (Log) Log("  VRB: block found but magic is %08X, not %08X - ignoring",
					 pS->nMagic, VRSHARED_MAGIC);
		UnmapViewOfFile(p); CloseHandle(g_hMap); g_hMap = nullptr;
		return false;
	}
	// A host older than the per-eye frustum has no eight angles to give, and
	// reading them would be reading whatever is past its shorter block. Refuse
	// rather than read a plausible zero.
	if (pS->nVersion < 12)
	{
		if (Log) Log("  VRB: block is version %u; the per-eye frustum arrived at"
					 " 12, so there is nothing here to read", pS->nVersion);
		UnmapViewOfFile(p); CloseHandle(g_hMap); g_hMap = nullptr;
		return false;
	}

	// How much the host actually published, so nothing is read or written past
	// the end of a shorter block.
	MEMORY_BASIC_INFORMATION mbi{};
	g_nMapped = VirtualQuery(p, &mbi, sizeof(mbi)) ? (size_t)mbi.RegionSize : 0;
	g_nVersion = pS->nVersion;

	g_pBlock = pS;
	if (Log) Log("  VRB: attached to %s, version %u, %zu bytes mapped"
				 " (this build's struct is %zu)",
				 VRSHARED_NAME, pS->nVersion, g_nMapped, sizeof(VRSharedState));
	return true;
}

bool VRB_Live()
{
	if (!g_pBlock) return false;
	const DWORD dwTick = g_pBlock->nHostAliveTick;
	const DWORD dwNow  = GetTickCount();
	return (dwNow - dwTick) < 1000;
}

bool VRB_EyeFrustum(int nEye, VRBFrustum* pOut)
{
	if (!pOut || nEye < 0 || nEye > 1)
	{ g_pszLastFail = "bad eye index"; return false; }
	if (!VRB_Live())
	{ g_pszLastFail = "host not live (alive tick older than 1 s)"; return false; }

	// A torn read is not a reason to change the projection.
	//
	// The four angles are the headset's OPTICS. They do not vary frame to
	// frame, so when the seqlock loses a race with the host's writer the right
	// answer is the one from last frame - not a symmetric frustum, which is a
	// visibly wrong frame. Measured at about one scene in a thousand with the
	// Python fake host, whose write window is long; a real host tears less, but
	// the reasoning does not depend on the rate.
	//
	// Deliberately NOT applied when the host is gone: that check is above this
	// one and still returns false, because "the host stopped" is a real change
	// and pretending otherwise would hide it.
	VRSharedState s;
	if (!Snapshot(&s))
	{
		if (g_bHaveLast[nEye])
		{
			*pOut = g_LastGood[nEye];
			++g_nReused;
			return true;
		}
		return false;						// Snapshot set its own reason
	}

	const float l = s.fEyeFovLeftRad[nEye];
	const float r = s.fEyeFovRightRad[nEye];
	const float u = s.fEyeFovUpRad[nEye];
	const float d = s.fEyeFovDownRad[nEye];

	// A frustum has to have positive extent on both axes and stay short of a
	// right angle on every edge. All four zero - a host that never wrote them -
	// fails this, which is the case worth catching: it would otherwise divide
	// by zero and produce a matrix of infinities that draws nothing, and
	// "nothing drawn" is the hardest symptom in this project to attribute.
	const float kMax = 1.5f;			// 85.9 degrees
	if (!(r > l) || !(u > d))
	{ g_pszLastFail = "angles are not a frustum (all zero?)"; return false; }
	if (fabsf(l) > kMax || fabsf(r) > kMax
		|| fabsf(u) > kMax || fabsf(d) > kMax)
	{ g_pszLastFail = "an angle exceeds 85.9 degrees"; return false; }

	pOut->fTanL = tanf(l);
	pOut->fTanR = tanf(r);
	pOut->fTanU = tanf(u);
	pOut->fTanD = tanf(d);

	g_LastGood[nEye] = *pOut;
	g_bHaveLast[nEye] = 1;

	if (!g_bLogged && Log)
	{
		g_bLogged = 1;
		const float R2D = 57.2957795f;
		for (int e = 0; e < 2; ++e)
			Log("  VRB: eye %d frustum L%.2f R%.2f U%.2f D%.2f deg", e,
				s.fEyeFovLeftRad[e] * R2D, s.fEyeFovRightRad[e] * R2D,
				s.fEyeFovUpRad[e] * R2D, s.fEyeFovDownRad[e] * R2D);
	}
	return true;
}

// The host's frame counter, written once per xrWaitFrame. The renderer's
// frame limiter paces the game to it rather than to a clock of its own.
bool VRB_HostFrame(uint32_t* pOut)
{
	if (!pOut || !g_pBlock) return false;
	*pOut = *(volatile uint32_t*)&g_pBlock->nFrameCounter;
	return true;
}

bool VRB_InMenu()
{
	if (!VRB_Live()) return false;
	const VRSharedState* pS = (const VRSharedState*)g_pBlock;
	return pS && pS->nInMenu != 0;
}

bool VRB_HeadYawPitch(float* pYawDeg, float* pPitchDeg)
{
	if (!VRB_Live() || !g_pBlock) return false;
	const VRSharedState* pS = (const VRSharedState*)g_pBlock;
	if (pYawDeg)   *pYawDeg   = pS->fHeadYawDeg;
	if (pPitchDeg) *pPitchDeg = pS->fHeadPitchDeg;
	return true;
}

const char* VRB_LastFail() { return g_pszLastFail; }

long VRB_ReusedCount() { return g_nReused; }

// The renderer describing its own shared texture. Written straight into the
// block rather than through the seqlock: these are the RENDERER's fields, the
// host only reads them, and the serial is the last one written so a host that
// sees a new serial has already seen the size that goes with it.
void VRB_PublishEyeTex(uint32_t nSerial, uint32_t nW, uint32_t nH,
					   uint32_t nFormat, int32_t nLuidLo, int32_t nLuidHi)
{
	if (!g_pBlock) return;
	// An older host's block has no room for these. Writing them would land on
	// whatever follows the section.
	if (g_nVersion < 13 || g_nMapped < sizeof(VRSharedState))
	{
		if (!g_bSaidNoEyeTex && Log)
		{
			g_bSaidNoEyeTex = 1;
			Log("  VRB: host block is version %u / %zu bytes, so the shared eye"
				" texture cannot be advertised. The host will keep using window"
				" capture. Said once.", g_nVersion, g_nMapped);
		}
		return;
	}
	g_pBlock->nEyeTexW      = nW;
	g_pBlock->nEyeTexH      = nH;
	g_pBlock->nEyeTexFormat = nFormat;
	g_pBlock->nAdapterLuidLo = nLuidLo;
	g_pBlock->nAdapterLuidHi = nLuidHi;
	// The marker the client painted into THIS picture, for a host that reads
	// memory rather than pixels (v16). Same process, so a plain read.
	if (g_nVersion >= 16 && g_nMapped >= sizeof(VRSharedState))
		g_pBlock->nEyeTexMarker = g_pBlock->nMarkerPainted;
	_ReadWriteBarrier();
	g_pBlock->nEyeTexSerial = nSerial;		// last, so it gates the rest
}

void VRB_PublishOverlay(uint32_t nSerial, uint32_t nW, uint32_t nH, int bActive)
{
	if (!g_pBlock) return;
	if (g_nVersion < 15 || g_nMapped < sizeof(VRSharedState)) return;
	g_pBlock->nOvlW = nW;
	g_pBlock->nOvlH = nH;
	g_pBlock->nPauseQuad = bActive ? 1u : 0u;
	_ReadWriteBarrier();
	g_pBlock->nOvlSerial = nSerial;
}

void VRB_PublishNativeFrustum(int bOn)
{
	if (g_pBlock) g_pBlock->nNativeFrustum = bOn ? 1u : 0u;
}

void VRB_Close()
{
	if (g_pBlock) { VRB_PublishNativeFrustum(0); UnmapViewOfFile(g_pBlock); g_pBlock = nullptr; }
	if (g_hMap)   { CloseHandle(g_hMap); g_hMap = nullptr; }
}
