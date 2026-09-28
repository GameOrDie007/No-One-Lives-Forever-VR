// world - the level's own .DAT, parsed in the renderer. See world.h.
//
// Every identity enforced here is enforced by tools/dat.py over all 103 worlds
// first. Two independently written parsers reading the same bytes is the point:
// a mistake in one of them shows up as a disagreement rather than as a picture
// bug three weeks later.

#include "world.h"
#include "rezfs.h"

#include <string.h>
#include <stdio.h>			// sscanf_s, for the properties string
#include <math.h>
#include <vector>

namespace
{
	// Every read is bounds-checked. A malformed or truncated file must make
	// the parse FAIL, never make the renderer fault - this runs inside the
	// game's process on a level load.
	inline bool Rd(const uint8_t* p, uint32_t n, uint32_t o, void* pDst, uint32_t nLen)
	{
		if (o > n || nLen > n - o) return false;
		memcpy(pDst, p + o, nLen);
		return true;
	}
	inline bool RdU32(const uint8_t* p, uint32_t n, uint32_t o, uint32_t* v)
	{ return Rd(p, n, o, v, 4); }
	inline bool RdU16(const uint8_t* p, uint32_t n, uint32_t o, uint16_t* v)
	{ return Rd(p, n, o, v, 2); }

	// The record's fixed part; the rest is variable (see WorldSurface).
	const uint32_t kSurfTexture   = 36;		// u16, index into the model's list
	const uint32_t kSurfPlane     = 38;		// u32
	const uint32_t kSurfEngFlags  = 42;		// u32, the engine's SURF_ bits
	const uint32_t kSurfEffects   = 50;		// u8 count, then the strings, then the u16 type
	const uint32_t kPlaneSize     = 16;		// {float3 normal, float dist}

	// A plane normal is UNIT LENGTH. That is the whole basis of locating the
	// array, so it gets one definition.
	inline bool UnitNormal(const uint8_t* p, uint32_t n, uint32_t o)
	{
		float v[3];
		if (!Rd(p, n, o, v, 12)) return false;
		const float d = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
		return d > 0.99f && d < 1.01f;
	}
}

// ---------------------------------------------------------------------------
// LOCATING THE PLANES, which is the one thing that is not simply computed.
//
// Every model but VisBSP puts its planes immediately after the per-polygon
// size array. VisBSP does not, in any of the 103 worlds, and because the
// surfaces, the points and the polygons are ALL measured from the planes, that
// single displacement made four arrays wrong at once - which is why it was
// misdiagnosed for days as "the surface array is not after the planes".
//
// There is no formula. Solved exactly over all twelve declared counts plus a
// constant on 12 worlds, the answer fails out of sample on 78 of the other 90.
// So the array is found by the criterion that a plane normal is unit length:
// nPlanes of them in a row at stride 16 is not something other data does by
// accident. On M01S02's VisBSP exactly one offset in the model satisfies it.
// ---------------------------------------------------------------------------
static uint32_t LocatePlanes(const uint8_t* p, uint32_t nSize,
							 uint32_t nAssumed, uint32_t nPlanes, uint32_t nEnd)
{
	if (!nPlanes) return nAssumed;

	// The computed position first: right on every model but one, and one
	// check rather than a scan.
	bool bAll = true;
	for (uint32_t i = 0; i < nPlanes && bAll; ++i)
		bAll = UnitNormal(p, nSize, nAssumed + i * kPlaneSize);
	if (bAll) return nAssumed;

	// Cascade, cheapest first. One normal rejects essentially every offset;
	// checking the whole array at every byte would be nPlanes reads per byte
	// scanned over a megabyte.
	const uint32_t nLimit = (nEnd > nPlanes * kPlaneSize)
						  ? nEnd - nPlanes * kPlaneSize : 0;
	for (uint32_t o = nAssumed; o <= nLimit; ++o)
	{
		if (!UnitNormal(p, nSize, o)) continue;
		if (!UnitNormal(p, nSize, o + kPlaneSize)) continue;
		if (!UnitNormal(p, nSize, o + (nPlanes - 1) * kPlaneSize)) continue;
		bAll = true;
		for (uint32_t i = 2; i + 1 < nPlanes && bAll; ++i)
			bAll = UnitNormal(p, nSize, o + i * kPlaneSize);
		if (bAll) return o;
	}
	return 0;					// not found, and saying so beats guessing
}

// Case-insensitive "does this class name contain Light".
static bool IsLightClass(const std::string& s)
{
	for (size_t i = 0; i + 5 <= s.size(); ++i)
		if ((s[i] | 32) == 'l' && (s[i+1] | 32) == 'i' && (s[i+2] | 32) == 'g'
		 && (s[i+3] | 32) == 'h' && (s[i+4] | 32) == 't') return true;
	return false;
}

// Walk the object block and keep the lights. See the call site for the format
// and for why a partial result is thrown away.
static void ParseObjectLights(const uint8_t* b, uint32_t n, uint32_t nAt,
							  WorldFile* pOut, R3D_LogFn pfnLog)
{
	pOut->Lights.clear();
	pOut->Classes.clear();
	pOut->Sky.clear();
	pOut->bSkyCam = false;
	pOut->bSkyCamFromDims = false;
	pOut->bSkyCamFromDemo = false;
	pOut->fSkyView[0] = pOut->fSkyView[1] = pOut->fSkyView[2] = 0.0f;

	uint32_t nObj = 0;
	if (!RdU32(b, n, nAt, &nObj) || nObj == 0 || nObj > 100000) return;

	uint32_t q = nAt + 4;
	long nLightish = 0;
	for (uint32_t i = 0; i < nObj; ++i)
	{
		uint16_t nRec = 0, nNm = 0;
		if (!RdU16(b, n, q, &nRec) || nRec < 6) { pOut->Lights.clear(); return; }
		const uint32_t nNextRec = q + 2u + nRec;
		if (nNextRec > n) { pOut->Lights.clear(); return; }

		uint32_t r = q + 2;
		if (!RdU16(b, n, r, &nNm) || !nNm || nNm > 128
			|| (uint64_t)r + 2 + nNm > n) { pOut->Lights.clear(); return; }
		r += 2;
		const std::string sCls((const char*)(b + r), nNm);
		r += nNm;

		uint32_t nProp = 0;
		if (!RdU32(b, n, r, &nProp) || nProp > 1024)
			{ pOut->Lights.clear(); return; }
		r += 4;

		const bool bLight = IsLightClass(sCls);
		if (bLight) ++nLightish;
		const bool bSkyPtr = (sCls == "SkyPointer");
		const bool bSkyDemo = (sCls == "DemoSkyWorldModel");
		std::string sSkyName; float fSkyIdx = 0.0f, fSkyInner = 1.0f;
		float fSkyPos[3] = { 0, 0, 0 }; bool bSkyPos = false;
		float fSkyDims[3] = { 0, 0, 0 };
		float fSkyInn[3] = { 0, 0, 0 };

		WorldLight L;
		L.fPos[0] = L.fPos[1] = L.fPos[2] = 0.0f;
		L.fRGB[0] = L.fRGB[1] = L.fRGB[2] = 0.0f;
		L.fRadius = 0.0f;
		L.fBright = 1.0f;
		L.bObjects = true;
		L.sClass = sCls;
		bool bGotPos = false, bGotCol = false;
		std::string sObjName; int nShowSurf = -1, nVisible = -1;

		for (uint32_t k = 0; k < nProp; ++k)
		{
			uint16_t pl = 0, dl = 0;
			if (!RdU16(b, n, r, &pl) || !pl || pl > 128
				|| (uint64_t)r + 2 + pl > n) { pOut->Lights.clear(); return; }
			r += 2;
			const std::string sP((const char*)(b + r), pl);
			r += pl;
			if ((uint64_t)r + 1 + 4 + 2 > n) { pOut->Lights.clear(); return; }
			const uint8_t nType = b[r];
			r += 1 + 4;						// type, then the property flags
			if (!RdU16(b, n, r, &dl)) { pOut->Lights.clear(); return; }
			r += 2;
			if ((uint64_t)r + dl > n) { pOut->Lights.clear(); return; }
			const uint8_t* pD = b + r;
			r += dl;

			// The object's own name (a string property: u16 length, chars),
			// against its class. See WorldFile::Classes.
			if (sP == "Name" && nType == 0 && dl >= 2)
			{
				uint16_t sl = 0;
				memcpy(&sl, pD, 2);
				if (sl && (uint32_t)sl + 2 <= dl)
				{
					sObjName.assign((const char*)pD + 2, sl);
					pOut->Classes[sObjName] = sCls;
				}
			}
			// A bool property is one byte. Read for every object; only the
			// volume brushes carry it, and only the unmatched ones consult it.
			if (sP == "ShowSurface" && nType == 5 && dl == 1)
				nShowSurf = pD[0] ? 1 : 0;
			// The engine's own "is the brush drawn". See WorldFile::Visible:
			// on a liquid this is the answer, and ShowSurface is a proxy that
			// happens to agree only where the level never needed them to
			// differ.
			if (sP == "Visible" && nType == 5 && dl == 1)
				nVisible = pD[0] ? 1 : 0;
			if (sCls == "KeyFramer" && sP == "ObjectName" && nType == 0 && dl >= 2)
			{
				uint16_t sl = 0; memcpy(&sl, pD, 2);
				if (sl && (uint32_t)sl + 2 <= dl)
				{
					std::string sK((const char*)pD + 2, sl);
					for (size_t q = 0; q < sK.size(); ++q) sK[q] = (char)tolower((unsigned char)sK[q]);
					pOut->KeyframedLower.insert(sK);
				}
			}

			if (bSkyDemo)
			{
				if (sP == "Pos" && nType == 1 && dl == 12) { memcpy(fSkyPos, pD, 12); bSkyPos = true; }
				else if (sP == "SkyDims" && dl == 12) memcpy(fSkyDims, pD, 12);
				else if (sP == "InnerPercentX" && dl == 4) memcpy(&fSkyInn[0], pD, 4);
				else if (sP == "InnerPercentY" && dl == 4) memcpy(&fSkyInn[1], pD, 4);
				else if (sP == "InnerPercentZ" && dl == 4) memcpy(&fSkyInn[2], pD, 4);
			}
			if (bSkyPtr)
			{
				if (sP == "SkyObjectName" && nType == 0 && dl >= 2)
				{
					uint16_t sl = 0; memcpy(&sl, pD, 2);
					if (sl && (uint32_t)sl + 2 <= dl) sSkyName.assign((const char*)pD + 2, sl);
				}
				// Index is a REAL in the file whatever its type byte says:
				// Berlin's four came back 3, 4, 2, 1 as floats and 0 when the
				// type was insisted on.
				else if (sP == "Index" && dl == 4) memcpy(&fSkyIdx, pD, 4);
				else if (sP == "InnerPercentX" && dl == 4) { memcpy(&fSkyInner, pD, 4); fSkyInn[0] = fSkyInner; }
				else if (sP == "InnerPercentY" && dl == 4) memcpy(&fSkyInn[1], pD, 4);
				else if (sP == "InnerPercentZ" && dl == 4) memcpy(&fSkyInn[2], pD, 4);
				else if (sP == "Pos" && nType == 1 && dl == 12) { memcpy(fSkyPos, pD, 12); bSkyPos = true; }
				else if (sP == "SkyDims" && dl == 12) memcpy(fSkyDims, pD, 12);
			}

			if (!bLight) continue;

			if (sP == "Pos" && nType == 1 && dl == 12)
			{
				memcpy(L.fPos, pD, 12);
				bGotPos = true;
			}
			else if ((sP == "LightColor" || sP == "InnerColor")
					 && nType == 2 && dl == 12)
			{
				float c[3];
				memcpy(c, pD, 12);
				// The file states colour 0..255; everything downstream of the
				// grid is 0..1, the same range the lightmap texels arrive in.
				L.fRGB[0] = c[0] / 255.0f;
				L.fRGB[1] = c[1] / 255.0f;
				L.fRGB[2] = c[2] / 255.0f;
				bGotCol = true;
			}
			else if (sP == "LightRadius" && nType == 3 && dl == 4)
				memcpy(&L.fRadius, pD, 4);
			else if (sP == "BrightScale" && nType == 3 && dl == 4)
				memcpy(&L.fBright, pD, 4);
			else if (sP == "LightObjects" && nType == 5 && dl == 1)
				L.bObjects = (pD[0] != 0);
		}

		// A light with no radius cannot be splatted into a grid - the sun is
		// the case, and it is global rather than local. Dropped here rather
		// than pretended about.
		if (bLight && bGotPos && bGotCol && L.fRadius > 1.0f)
			pOut->Lights.push_back(L);
		if (!sObjName.empty() && nShowSurf >= 0)
			pOut->ShowSurface[sObjName] = nShowSurf;
		if (!sObjName.empty() && nVisible >= 0)
			pOut->Visible[sObjName] = nVisible;
		// THE SKY BOX MODEL IS THE SKY CAMERA WHEN IT CARRIES SkyDims. In the
		// engine a DemoSkyWorldModel with non-zero SkyDims calls SetSkyDef
		// itself (ltengineobjects.cpp): the box is pos +- dims and the camera
		// sits in pos +- dims*InnerPercent. Only SkyPointer objects were read
		// here, so in twenty levels built from the same template the camera
		// came from the clouds' pointer, 128 units above the box centre and in
		// the plane of a cloud sheet - whose edge-on edge flickered as a dark
		// line at the horizon of M05S05's opening cutscene (22 September).
		// It overrides every pointer.
		if (bSkyDemo && bSkyPos && fSkyDims[0] != 0.0f && fSkyDims[1] != 0.0f && fSkyDims[2] != 0.0f)
		{
			memcpy(pOut->fSkyCam, fSkyPos, 12); pOut->bSkyCam = true;
			pOut->bSkyCamFromDemo = true; pOut->bSkyCamFromDims = true;
			for (int k = 0; k < 3; ++k) pOut->fSkyView[k] = fabsf(fSkyDims[k] * fSkyInn[k]);
		}
		if (bSkyPtr && !sSkyName.empty())
		{
			WorldFile::SkyObj so; so.sName = sSkyName; so.fIndex = fSkyIdx; so.fInner = fSkyInner;
			pOut->Sky.push_back(so);
			// THE SKY CAMERA IS THE POINTER THAT DEFINES THE SKY BOX. The engine
			// sets its SkyDef - the box and the view box the camera sits in -
			// only from a pointer whose SkyDims are non-zero; the others just
			// add an object to the sky. The first pointer used to win: in the
			// GOTY beach (M16S01) that is the clouds' pointer, 288 units above
			// the box centre and 32 under its ceiling, and everything above the
			// horizon came out black in the opening cutscene (headset, 22
			// September). A dims pointer overrides any earlier choice; without
			// one, the first pointer still stands.
			const bool bDims = (fSkyDims[0] != 0.0f && fSkyDims[1] != 0.0f && fSkyDims[2] != 0.0f);
			if (bSkyPos && !pOut->bSkyCamFromDemo && (!pOut->bSkyCam || (bDims && !pOut->bSkyCamFromDims)))
			{
				memcpy(pOut->fSkyCam, fSkyPos, 12); pOut->bSkyCam = true;
				if (bDims) pOut->bSkyCamFromDims = true;
				for (int k = 0; k < 3; ++k) pOut->fSkyView[k] = bDims ? fabsf(fSkyDims[k] * fSkyInn[k]) : 0.0f;
			}
		}

		q = nNextRec;
	}

	if (pfnLog)
		pfnLog("  WORLD LIGHTS: %u objects, %ld named like a light,"
			   " %u usable (position, colour and a radius)",
			   nObj, nLightish, (unsigned)pOut->Lights.size());
	if (pfnLog && !pOut->Sky.empty())
	{
		std::string sList;
		for (size_t i = 0; i < pOut->Sky.size(); ++i)
		{
			char sz[96]; _snprintf_s(sz, sizeof sz, _TRUNCATE, "%s%s(%.0f)", i ? ", " : "",
				pOut->Sky[i].sName.c_str(), pOut->Sky[i].fIndex);
			sList += sz;
		}
		pfnLog("  WORLD SKY: %u sky pointers name %s; sky camera %s (%.0f %.0f %.0f)",
			   (unsigned)pOut->Sky.size(), sList.c_str(), pOut->bSkyCam ? "at" : "unknown",
			   pOut->fSkyCam[0], pOut->fSkyCam[1], pOut->fSkyCam[2]);
	}
}

bool World_Parse(const uint8_t* b, uint32_t n, const char* pszPath,
				 WorldFile* pOut, R3D_LogFn pfnLog)
{
	if (!b || !pOut) return false;
	pOut->sPath = pszPath ? pszPath : "";
	pOut->Models.clear();

	uint32_t nObjectDataPos = 0;
	if (!RdU32(b, n, 0, &pOut->nVersion) || !RdU32(b, n, 4, &nObjectDataPos))
		return false;
	// Version 66 is what every NOLF1 world is. A different number means a
	// different layout, and guessing at one is how you draw garbage.
	if (pOut->nVersion != 66)
	{
		if (pfnLog) pfnLog("  WORLD FILE: %s is version %u, expected 66",
						   pszPath, pOut->nVersion);
		return false;
	}

	// 3 offsets, 8 reserved dwords, then a length-prefixed properties string.
	uint32_t o = 44, nInfo = 0;
	if (!RdU32(b, n, o, &nInfo)) return false;
	// KEEP IT, rather than stepping over it. It carries the level's ambient
	// light, and this renderer has been lighting every world without it.
	pOut->sInfo.clear();
	pOut->fAmbient[0] = pOut->fAmbient[1] = pOut->fAmbient[2] = 0.0f;
	if (nInfo && nInfo < 4096 && (uint64_t)o + 4 + nInfo <= n)
	{
		pOut->sInfo.assign((const char*)(b + o + 4), nInfo);
		const char* pA = strstr(pOut->sInfo.c_str(), "AmbientLight");
		if (pA)
		{
			float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f;
			if (sscanf_s(pA + 12, " %f %f %f", &a0, &a1, &a2) == 3)
			{
				// The file states it 0..255, the shader wants 0..1.
				pOut->fAmbient[0] = a0 / 255.0f;
				pOut->fAmbient[1] = a1 / 255.0f;
				pOut->fAmbient[2] = a2 / 255.0f;
			}
		}
	}
	o += 4 + nInfo;

	o += 4;										// the tree's scalar
	float box[12];
	if (!Rd(b, n, o, box, 48)) return false;
	// TWO NESTED BOXES. The SECOND is the world's own and is what the engine
	// header carries - verified exactly, not approximately: M01S02's file says
	// min (-5248 -1856 -80) max (6528 80 9936) and so does the heap, to the
	// bit. The first box is the root node's and is 16 units tighter.
	for (int i = 0; i < 3; ++i)
	{
		pOut->fBoxMin[i] = box[6 + i];
		pOut->fBoxMax[i] = box[9 + i];
	}
	o += 48;

	uint32_t nTreeNodes = 0;
	if (!RdU32(b, n, o, &nTreeNodes)) return false;
	o += 8;										// node count + one reserved
	// ONE LAYOUT BIT PER NODE, packed. Get this wrong and everything after is
	// off by a few bytes and nothing parses - which is exactly how it was
	// found.
	o += (nTreeNodes + 7) / 8;

	if (!RdU32(b, n, o, &pOut->nModelCount)) return false;
	o += 4;

	// IDENTITY 1: the model chain is a linked list whose last link lands
	// EXACTLY on the header's objectDataPos. A wrong stride anywhere derails
	// it. IDENTITY 2: the count written before the first record equals the
	// records walked, and it is written independently of the chain.
	// ---- THE OBJECT BLOCK, for the level's own lights ---------------------
	//
	//   u32 objectCount
	//     per object:   u16 recordLen, u16 nameLen, name, u32 propCount
	//       per prop:   u16 nameLen, name, u8 type, u32 FLAGS, u16 len, data
	//
	// THE u32 FLAGS IS THE TRAP. Miss it and the walk does not fail - every
	// record runs into the next and the parse invents plausible rubbish, which
	// is exactly what the first attempt produced.
	//
	// recordLen is the safety net: walk by it, and if anything inside a record
	// does not add up, drop the WHOLE list rather than keep a partial one. The
	// world does not depend on this - geometry is already parsed above - so
	// failing quietly to no lights is the right failure.
	ParseObjectLights(b, n, nObjectDataPos, pOut, pfnLog);

	uint32_t p = o;
	while (p != nObjectDataPos)
	{
		if (p + 46 > n)
		{
			if (pfnLog) pfnLog("  WORLD FILE: %s - the model chain ran off the file",
							   pszPath);
			return false;
		}
		uint32_t nNext = 0;
		uint16_t nLen = 0;
		if (!RdU32(b, n, p, &nNext) || !RdU16(b, n, p + 44, &nLen)) return false;
		if (!nLen || nLen > 64 || p + 46 + nLen > n) return false;
		if (nNext <= p) return false;			// must advance

		WorldModelFile m;
		m.sName.assign((const char*)(b + p + 46), nLen);
		m.nStart = p;
		m.nNext = nNext;

		uint32_t e = p + 46 + nLen;
		uint32_t c[12];
		if (!Rd(b, n, e, c, 48)) return false;
		m.nPoints = c[0]; m.nPlanes = c[1]; m.nSurfaces = c[2];
		m.nPolygons = c[4]; m.nNodes = c[5]; m.nVertexRefs = c[6];
		e += 48;
		// box min (12), box max (12), then the translation the object spawns
		// at. See WorldModelFile::fTrans.
		m.bHaveTrans = Rd(b, n, e + 24, m.fTrans, 12);
		e += 36;								// box min, box max, translation

		// IDENTITY 4: the texture blob splits into exactly the declared count.
		// THESE NAMES ARE THE BRIDGE - they are what the stage 1 Dtx cache is
		// keyed by.
		uint32_t nBlob = 0, nTex = 0;
		if (!RdU32(b, n, e, &nBlob) || !RdU32(b, n, e + 4, &nTex)) return false;
		if (e + 8 + nBlob > n) return false;
		{
			const char* q = (const char*)(b + e + 8);
			uint32_t i = 0;
			while (i < nBlob)
			{
				uint32_t j = i;
				while (j < nBlob && q[j]) ++j;
				m.Textures.push_back(std::string(q + i, j - i));
				i = j + 1;
			}
			// A blob with an EMPTY name in the middle is legal - Chevelle_Body
			// has one - so only a trailing empty is dropped. Filtering every
			// empty loses it and miscounts by one.
			if (nBlob && !q[nBlob - 1] && !m.Textures.empty()
				&& m.Textures.back().empty())
				m.Textures.pop_back();
		}
		if (m.Textures.size() != nTex)
		{
			if (pfnLog) pfnLog("  WORLD FILE: %s/%s declares %u textures, the "
							   "blob splits into %u", pszPath, m.sName.c_str(),
							   nTex, (unsigned)m.Textures.size());
			return false;
		}
		e += 8 + nBlob;

		// IDENTITY 3: TWO BYTES PER POLYGON, not a u16. The low byte is the
		// vertex count, the high byte is something else, and it is zero on
		// every model this was first checked against - which made the wrong
		// reading look perfect twice and fail on 98 of the 103 worlds. The
		// vertex counts must sum to the declared reference count.
		if (e + m.nPolygons * 2 > n) return false;
		{
			uint32_t nSum = 0;
			for (uint32_t i = 0; i < m.nPolygons; ++i) nSum += b[e + i * 2];
			if (nSum != m.nVertexRefs)
			{
				if (pfnLog) pfnLog("  WORLD FILE: %s/%s polygon sizes sum to %u, "
								   "declared %u", pszPath, m.sName.c_str(),
								   nSum, m.nVertexRefs);
				return false;
			}
		}
		e += m.nPolygons * 2;

		m.nPlaneAt = LocatePlanes(b, n, e, m.nPlanes, nNext);
		m.bLocated = m.nPlaneAt != 0;
		m.nSurfaceAt = m.bLocated ? m.nPlaneAt + m.nPlanes * kPlaneSize : 0;

		pOut->Models.push_back(m);
		p = nNext;
	}

	if (pOut->nModelCount != pOut->Models.size())
	{
		if (pfnLog) pfnLog("  WORLD FILE: %s declares %u models, the chain walks %u",
						   pszPath, pOut->nModelCount, (unsigned)pOut->Models.size());
		return false;
	}
	return true;
}

bool World_Surfaces(const WorldFile&, const WorldModelFile& m,
					const uint8_t* b, uint32_t n,
					std::vector<WorldSurface>* pOut)
{
	if (!pOut) return false;
	pOut->clear();
	if (!m.bLocated || !m.nSurfaces) return false;
	pOut->reserve(m.nSurfaces);
	// VARIABLE LENGTH - see WorldSurface. The record's fixed part is 50
	// bytes; a u8 effect count follows, then the effect strings, then the
	// u16 surface type. A record whose texture or plane index is out of
	// range means the walk has lost its footing, and the whole array is
	// refused rather than half-read.
	uint32_t o = m.nSurfaceAt;
	const uint32_t nTex = (uint32_t)m.Textures.size();
	for (uint32_t i = 0; i < m.nSurfaces; ++i)
	{
		WorldSurface s;
		memset(&s, 0, sizeof s);
		if (!RdU16(b, n, o + kSurfTexture, &s.nTexture)) return false;
		if (!RdU32(b, n, o + kSurfPlane, &s.nPlane)) return false;
		if (!RdU32(b, n, o + kSurfEngFlags, &s.nEngFlags)) return false;
		if (s.nTexture >= nTex || s.nPlane >= m.nPlanes) { pOut->clear(); return false; }
		uint8_t nEff = 0;
		if (!Rd(b, n, o + kSurfEffects, &nEff, 1)) return false;
		if (nEff > 4) { pOut->clear(); return false; }
		uint32_t q = o + kSurfEffects + 1;
		for (uint8_t e = 0; e < nEff; ++e)
		{
			uint16_t nLen = 0;
			if (!RdU16(b, n, q, &nLen)) return false;
			if (q + 2 + nLen > n) return false;
			const char* pName = (const char*)(b + q + 2);
			const bool bMirror = (nLen == 6 && _strnicmp(pName, "mirror", 6) == 0);
			q += 2 + nLen;
			if (!RdU16(b, n, q, &nLen)) return false;
			if (q + 2 + nLen > n) return false;
			q += 2 + nLen;
			if (bMirror) s.bMirror = true;
		}
		if (!RdU16(b, n, q, &s.nFlags)) return false;
		q += 2;
		if (!Rd(b, n, m.nPlaneAt + s.nPlane * kPlaneSize, s.fPlane, 16)) return false;
		pOut->push_back(s);
		o = q;
	}
	return true;
}

// ---------------------------------------------------------------------------
namespace
{
	struct IdentCtx
	{
		uint32_t nModelCount;
		const float* pMin;
		const float* pMax;
		std::vector<std::string> Hits;
	};

	// Enough to reach the model count on any world: the properties string and
	// the packed tree bits are the only variable parts before it, and the
	// largest tree in the game is a few thousand nodes.
	const uint32_t kHeadBytes = 8192;

	void IdentOne(const char* pszPath, uint32_t, void* pUser)
	{
		IdentCtx* c = (IdentCtx*)pUser;
		uint32_t n = 0;
		uint8_t* b = RezFS_ReadHead(pszPath, kHeadBytes, &n);
		if (!b) return;

		uint32_t nVer = 0, nInfo = 0, o = 44;
		float box[12];
		uint32_t nNodes = 0, nModels = 0;
		if (RdU32(b, n, 0, &nVer) && nVer == 66
			&& RdU32(b, n, o, &nInfo))
		{
			o += 4 + nInfo + 4;
			if (Rd(b, n, o, box, 48))
			{
				o += 48;
				if (RdU32(b, n, o, &nNodes))
				{
					// THE HEAD IS NOT A FIXED SIZE. The model count sits after
					// the world tree's one-bit-per-node layout array, so how far
					// in it is depends on how big the tree is - and on M14S02,
					// an outdoor level with 86881 tree nodes, it is at 11049.
					// Reading a fixed 8192 bytes put it past the end, the read
					// failed, and the file was skipped SILENTLY: the level drew,
					// the account never printed, and the marker rule and the
					// file-texture rescue were both off for that world with
					// nothing saying so.
					//
					// So work out where it is and, if that is past what was
					// read, read again with enough. Adaptive rather than a
					// bigger constant, because the next level to break this
					// would break it the same silent way.
					o += 8 + (nNodes + 7) / 8;
					if (o + 4 > n)
					{
						RezFS_Free(b);
						b = RezFS_ReadHead(pszPath, o + 64, &n);
						if (!b) return;
					}
					if (RdU32(b, n, o, &nModels) && nModels == c->nModelCount)
					{
						// EXACT, not approximate. The file and the engine
						// carry the same bytes, and a tolerance here would
						// turn a decisive test into a plausible one.
						bool bSame = true;
						for (int i = 0; i < 3; ++i)
							if (box[6 + i] != c->pMin[i] || box[9 + i] != c->pMax[i])
								bSame = false;
						if (bSame) c->Hits.push_back(pszPath);
					}
				}
			}
		}
		RezFS_Free(b);
	}
}

std::string World_Identify(uint32_t nModelCount, const float* pMin,
						   const float* pMax, R3D_LogFn pfnLog)
{
	IdentCtx c;
	c.nModelCount = nModelCount;
	c.pMin = pMin;
	c.pMax = pMax;
	RezFS_ForEach(".DAT", IdentOne, &c);

	if (c.Hits.size() == 1) return c.Hits[0];
	if (pfnLog)
	{
		if (c.Hits.empty())
			pfnLog("  WORLD FILE: no .DAT has %u models and bounds "
				   "(%.0f %.0f %.0f)..(%.0f %.0f %.0f)", nModelCount,
				   pMin[0], pMin[1], pMin[2], pMax[0], pMax[1], pMax[2]);
		else
			pfnLog("  WORLD FILE: %u worlds match, which identifies nothing - "
				   "first two %s, %s", (unsigned)c.Hits.size(),
				   c.Hits[0].c_str(), c.Hits[1].c_str());
	}
	return std::string();
}

WorldFile* World_Load(uint32_t nModelCount, const float* pMin, const float* pMax,
					  R3D_LogFn pfnLog)
{
	const std::string sPath = World_Identify(nModelCount, pMin, pMax, pfnLog);
	if (sPath.empty()) return nullptr;

	uint32_t nSize = 0;
	uint8_t* b = RezFS_Read(sPath.c_str(), &nSize);
	if (!b)
	{
		if (pfnLog) pfnLog("  WORLD FILE: %s could not be read", sPath.c_str());
		return nullptr;
	}

	WorldFile* w = new WorldFile();
	const bool bOK = World_Parse(b, nSize, sPath.c_str(), w, pfnLog);
	if (!bOK) { delete w; RezFS_Free(b); return nullptr; }

	// The bytes are kept: the surfaces are read out of them on demand rather
	// than copied up front, and a level's worth of surface records is several
	// megabytes we would otherwise hold twice.
	w->Models.shrink_to_fit();
	extern void World_Keep(WorldFile*, uint8_t*, uint32_t);
	World_Keep(w, b, nSize);
	return w;
}

// The file bytes, kept alongside the parse. A map rather than a member so
// world.h stays a description of the FORMAT and not of this allocation.
#include <map>
namespace { std::map<WorldFile*, std::pair<uint8_t*, uint32_t> > g_Bytes; }

void World_Keep(WorldFile* w, uint8_t* b, uint32_t n)
{ g_Bytes[w] = std::make_pair(b, n); }

bool World_Bytes(WorldFile* w, const uint8_t** pb, uint32_t* pn)
{
	auto it = g_Bytes.find(w);
	if (it == g_Bytes.end()) return false;
	*pb = it->second.first;
	*pn = it->second.second;
	return true;
}

void World_Release(WorldFile* w)
{
	if (!w) return;
	auto it = g_Bytes.find(w);
	if (it != g_Bytes.end()) { RezFS_Free(it->second.first); g_Bytes.erase(it); }
	delete w;
}
