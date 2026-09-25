#include <cstring>
#include <algorithm>
#include "lightmap.h"

#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <unordered_map>
#include <vector>

namespace
{
	// The engine's structures. Every one of these has a call site behind it,
	// listed in lightmap.h; none is a guess and none is written to.
	const uint32_t kLightAnims     = 0x08;	// world -> array of 0x60-byte records
	const uint32_t kLightAnimCount = 0x0C;
	const uint32_t kTexelSize      = 0xF8;	// world -> float, world units per texel
	const uint32_t kWorldModels    = 0x18C;
	const uint32_t kWorldModelCount= 0x190;
	const uint32_t kPolyArray      = 0xA0;
	const uint32_t kPolyCount      = 0xA4;
	const uint32_t kAnimFrames     = 0x24;	// anim -> array of frame pointers
	const uint32_t kAnimFrameCount = 0x28;
	const uint32_t kAnimEntries    = 0x30;
	const uint32_t kAnimCurFrame   = 0x34;
	const uint32_t kPolyRefs       = 0x0C;	// poly -> {uint16 anim, uint16 data}[]
	const uint32_t kPolyRefCount   = 0x10;
	const uint32_t kPolyLMOrigin   = 0x34;	// poly -> float3
	const uint32_t kPolyLMWidth    = 0x48;	// poly -> uint8
	const uint32_t kPolyLMHeight   = 0x49;	// poly -> uint8
	const uint32_t kPolySurface    = 0x28;
	const uint32_t kSurfacePlane   = 0x34;	// surface -> float3 normal, float dist
	const uint32_t kSurfaceFlags   = 0x38;	// bit 0x80 lightmapped, top 3 bits axis
	const uint32_t kAxisTableRVA   = 0xB3E84;	// lithtech.exe + this

	// The renderer's own budget: 1024 texels, so no lightmap is larger than
	// 32 x 32. d3d.ren + 0x6A7A8 loads 0x400 and refuses anything that
	// overruns it.
	const uint32_t kMaxTexels = 1024;
	const int      kAtlas     = 2048;

	bool Readable(uint32_t p, size_t n)
	{
		return p && !IsBadReadPtr((const void*)(uintptr_t)p, n);
	}
	uint32_t Word(uint32_t p, uint32_t off)
	{
		return Readable(p + off, 4) ? *(const uint32_t*)(uintptr_t)(p + off) : 0;
	}

	uint32_t g_pAxis   = 0;
	int      g_bAxisOK = 0;

	ID3D11Texture2D*          g_pTex = nullptr;
	ID3D11ShaderResourceView* g_pSRV = nullptr;
	uint32_t g_pBuiltFrom = 0;

	std::unordered_map<uint32_t, LMPoly> g_Map;
	std::vector<uint32_t> g_Pixels;	// the atlas, kept for readback

	// What the packer has to place, gathered before anything is decoded so the
	// tall ones can go down first.
	struct Rect { uint32_t pPoly, pData, nLen; uint16_t nW, nH; };
}

int LM_Decode(uint32_t pSrc, uint32_t nLen, uint16_t* pOut, uint32_t nMax,
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
		if (n + c > nMax) return 0;
		for (uint32_t k = 0; k < c; ++k) pOut[n++] = v;
	}
	*pnOut = n;
	return 1;
}

int LM_AxisTable(LM_LogFn Log)
{
	if (g_bAxisOK) return 1;
	const HMODULE h = GetModuleHandleA(nullptr);
	if (!h) return 0;
	g_pAxis = (uint32_t)(uintptr_t)h + kAxisTableRVA;
	if (!Readable(g_pAxis, 8 * 36)) return 0;

	// Six entries, one per dominant axis of a plane normal. The three bits
	// that index the table can hold eight and the last two are zero, so the
	// check is six - asking for eight refuses a table that is correct.
	int nUnit = 0;
	for (int i = 0; i < 6; ++i)
	{
		const float* v = (const float*)(uintptr_t)(g_pAxis + i * 36);
		const float m = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
		if (m > 0.999f && m < 1.001f) ++nUnit;
	}
	g_bAxisOK = (nUnit == 6);
	if (Log) Log("  LM: basis table at %08X, %d of 6 unit vectors  %s",
				 g_pAxis, nUnit, g_bAxisOK ? "" : "<- NOT the table");
	return g_bAxisOK;
}

int LM_Basis(uint32_t sf, float* pP, float* pQ)
{
	if (!g_bAxisOK || !Readable(sf, kSurfaceFlags + 4)) return 0;
	const uint32_t nPlane = Word(sf, kSurfacePlane);
	if (!Readable(nPlane, 12)) return 0;
	const uint32_t idx = Word(sf, kSurfaceFlags) >> 29;
	if (idx > 5) return 0;
	const float* V = (const float*)(uintptr_t)(g_pAxis + idx * 36);
	const float* n = (const float*)(uintptr_t)nPlane;
	pP[0] = V[1]*n[2] - V[2]*n[1];
	pP[1] = V[2]*n[0] - V[0]*n[2];
	pP[2] = V[0]*n[1] - V[1]*n[0];
	pQ[0] = n[1]*pP[2] - n[2]*pP[1];
	pQ[1] = n[2]*pP[0] - n[0]*pP[2];
	pQ[2] = n[0]*pP[1] - n[1]*pP[0];
	return 1;
}

ID3D11ShaderResourceView* LM_Atlas() { return g_pSRV; }
float LM_AtlasSize() { return (float)kAtlas; }

uint32_t LM_Texel(int nX, int nY)
{
	if (g_Pixels.empty() || nX < 0 || nY < 0 || nX >= kAtlas || nY >= kAtlas)
		return 0;
	return g_Pixels[(size_t)nY * kAtlas + nX];
}

int LM_ForPoly(uint32_t pPoly, LMPoly* pOut)
{
	const auto it = g_Map.find(pPoly);
	if (it == g_Map.end()) return 0;
	*pOut = it->second;
	return 1;
}

void LM_Destroy()
{
	if (g_pSRV) { g_pSRV->Release(); g_pSRV = nullptr; }
	if (g_pTex) { g_pTex->Release(); g_pTex = nullptr; }
	g_Map.clear();
	g_Pixels.clear();
	g_pBuiltFrom = 0;
}

// A 24-bit BMP of part of the atlas. There is no statistic over an image
// that beats looking at it, and this project has four wrong answers on record
// that came from trying.
static void LMWriteBMP(const char* pszPath, int nX0, int nY0, int nW, int nH,
					   LM_LogFn Log)
{
	if (g_Pixels.empty() || nW <= 0 || nH <= 0) return;
	const int nRow = (nW * 3 + 3) & ~3;

	BITMAPFILEHEADER fh{};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
	fh.bfSize = fh.bfOffBits + (DWORD)nRow * nH;

	BITMAPINFOHEADER ih{};
	ih.biSize = sizeof(ih);
	ih.biWidth = nW; ih.biHeight = nH;		// positive: bottom-up
	ih.biPlanes = 1; ih.biBitCount = 24;
	ih.biSizeImage = (DWORD)nRow * nH;

	FILE* f = nullptr;
	fopen_s(&f, pszPath, "wb");
	if (!f) { if (Log) Log("  LM: could not write %s", pszPath); return; }
	fwrite(&fh, sizeof fh, 1, f);
	fwrite(&ih, sizeof ih, 1, f);

	std::vector<uint8_t> row((size_t)nRow, 0);
	for (int y = nH - 1; y >= 0; --y)
	{
		for (int x = 0; x < nW; ++x)
		{
			const uint32_t v = g_Pixels[(size_t)(nY0 + y) * kAtlas + (nX0 + x)];
			row[x * 3 + 0] = (uint8_t)(v & 0xFF);			// B
			row[x * 3 + 1] = (uint8_t)((v >> 8) & 0xFF);		// G
			row[x * 3 + 2] = (uint8_t)((v >> 16) & 0xFF);	// R
		}
		fwrite(row.data(), nRow, 1, f);
	}
	fclose(f);
	if (Log) Log("  LM: wrote %s (%dx%d of the atlas)", pszPath, nW, nH);
}

int LM_Build(uint32_t pWorld, ID3D11Device* pDev, LM_LogFn Log,
			 const char* pszDump)
{
	if (!pDev || !pWorld) return 0;
	if (pWorld == g_pBuiltFrom) return g_pSRV != nullptr;
	LM_Destroy();
	g_pBuiltFrom = pWorld;
	if (!LM_AxisTable(Log)) { Log("  LM: no basis table, so no lightmaps"); return 0; }

	float fS = 0.0f;
	if (Readable(pWorld + kTexelSize, 4))
		memcpy(&fS, (const void*)(uintptr_t)(pWorld + kTexelSize), 4);
	if (!(fS > 0.0f)) { Log("  LM: world+0xF8 is not a texel size"); return 0; }

	// The one light animation. A level with none has no static lighting and
	// there is nothing here to do; a level with several would need the ref's
	// first uint16 to choose, which is why that field is read rather than
	// assumed to be zero.
	const uint32_t pLA = Word(pWorld, kLightAnims), nLA = Word(pWorld, kLightAnimCount);
	if (!nLA || nLA > 4096 || !Readable(pLA, 0x60))
	{ Log("  LM: no light animations"); return 0; }

	// Gather. Nothing is decoded yet - the packer wants the sizes first.
	const uint32_t pList = Word(pWorld, kWorldModels);
	const uint32_t nList = Word(pWorld, kWorldModelCount);
	if (!Readable(pList, 4) || !nList) return 0;

	std::vector<Rect> rects;
	rects.reserve(20000);
	uint32_t nFlagged = 0, nNoSize = 0, nNoRef = 0, nNoRecord = 0;
	uint32_t nZeroSize = 0, nOverSize = 0, nMaxW = 0, nMaxH = 0, nNoSizeWithRef = 0;
	int nNoSizeSaid = 0;
	// WHICH MODELS THE LIGHTMAPS BELONG TO.
	//
	// T01S01 packs 3888 lightmaps and only 98 of the 1118 polygons this
	// renderer draws ever finds one, so the night intro renders as flat
	// daylight. Both walks use the same model list and the same polygon
	// pointers, so the lightmaps cannot be missing - they must belong to
	// polygons we do not draw. A count by model NAME says which, and that is a
	// different question from "how many are there".
	struct LMKind { char sz[32]; uint32_t n; uint32_t nDropped; };
	std::vector<LMKind> kinds;

	for (uint32_t i = 0; i < nList; ++i)
	{
		const uint32_t sub = Word(Word(pList, i * 4), 4);
		if (!Readable(sub, 0xB0)) continue;
		const uint32_t pPolys = Word(sub, kPolyArray), nPolys = Word(sub, kPolyCount);
		if (!Readable(pPolys, 4) || !nPolys) continue;
		char szKind[32] = { 0 };
		{
			const char* q = (const char*)(uintptr_t)(sub + 0x04);
			int c = 0;
			while (c < 31 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126)
			{ szKind[c] = q[c]; ++c; }
			szKind[c] = 0;
			// Strip the trailing instance number so "AIVolume174" and
			// "AIVolume3" are one kind.
			while (c > 0 && szKind[c - 1] >= '0' && szKind[c - 1] <= '9')
				szKind[--c] = 0;
		}
		size_t nKind = 0;
		for (; nKind < kinds.size(); ++nKind)
			if (strcmp(kinds[nKind].sz, szKind) == 0) break;
		if (nKind == kinds.size() && kinds.size() < 64)
		{ LMKind k{}; strncpy(k.sz, szKind, 31); kinds.push_back(k); }

		for (uint32_t j = 0; j < nPolys; ++j)
		{
			const uint32_t po = Word(pPolys, j * 4);
			if (!Readable(po, 0x54)) continue;
			const uint32_t sf = Word(po, kPolySurface);
			if (!Readable(sf, kSurfaceFlags + 4)) continue;
			if (!(Word(sf, kSurfaceFlags) & 0x80)) continue;
			++nFlagged;

			const uint32_t lw = *(const uint8_t*)(uintptr_t)(po + kPolyLMWidth);
			const uint32_t lh = *(const uint8_t*)(uintptr_t)(po + kPolyLMHeight);
			if (!lw || !lh || lw * lh > kMaxTexels)
			{
				// WHAT "NO SIZE" IS. The harbour drops 3882 of 11478 flagged
				// surfaces here and the aeroplane 941 of 7181, and every one
				// draws from the light grid instead - the harbour's 1056 grey
				// polygons, the cabin's rainbow seats. Say whether the byte
				// pair is zero or too big, and whether the polygon still has
				// a resolvable record, so the next reader knows where to look.
				if (!lw || !lh) ++nZeroSize; else ++nOverSize;
				if (lw > nMaxW) nMaxW = lw;
				if (lh > nMaxH) nMaxH = lh;
				const uint32_t pRef0 = Word(po, kPolyRefs), nRef0 = Word(po, kPolyRefCount);
				if (nRef0 && nRef0 <= 256 && Readable(pRef0, nRef0 * 4)) ++nNoSizeWithRef;
				if (nNoSizeSaid < 4)
				{
					++nNoSizeSaid;
					Log("  LM: no-size sample: %s poly %08X  w %u h %u  refs %u  surface flags %08X",
						szKind, po, lw, lh, nRef0, Word(sf, kSurfaceFlags));
				}
				if (nKind < kinds.size()) ++kinds[nKind].nDropped;
				++nNoSize; continue;
			}

			const uint32_t pRef = Word(po, kPolyRefs), nRef = Word(po, kPolyRefCount);
			if (!nRef || nRef > 256 || !Readable(pRef, nRef * 4)) { ++nNoRef; continue; }

			// Resolve the ref through the animation it names, the way
			// d3d.ren + 0x6BB30 does, rather than assuming animation zero.
			uint32_t at = 0, len = 0;
			for (uint32_t k = 0; k < nRef; ++k)
			{
				const uint16_t a = *(const uint16_t*)(uintptr_t)(pRef + k * 4);
				const uint16_t d = *(const uint16_t*)(uintptr_t)(pRef + k * 4 + 2);
				if (a >= nLA) continue;
				const uint32_t la = pLA + (uint32_t)a * 0x60;
				if (!Readable(la, 0x60)) continue;
				if (d >= Word(la, kAnimEntries)) continue;
				const uint32_t pFrames = Word(la, kAnimFrames);
				const uint32_t nFrame  = Word(la, kAnimCurFrame);
				if (nFrame >= Word(la, kAnimFrameCount)) continue;
				const uint32_t pEnt = Word(pFrames, nFrame * 4);
				if (!Readable(pEnt + (uint32_t)d * 8, 8)) continue;
				at  = Word(pEnt, (uint32_t)d * 8);
				len = Word(pEnt, (uint32_t)d * 8 + 4);
				break;					// the first animation that resolves
			}
			if (!at || !len) { ++nNoRecord; continue; }

			if (nKind < kinds.size()) ++kinds[nKind].n;
			Rect r{};
			r.pPoly = po; r.pData = at; r.nLen = len;
			r.nW = (uint16_t)lw; r.nH = (uint16_t)lh;
			rects.push_back(r);
		}
	}

	if (rects.empty()) { Log("  LM: nothing to pack"); return 0; }

	// Pack. Bucketing by height means every shelf holds one height exactly, so
	// there is no vertical waste at all and the packing cannot depend on the
	// order the world walk happened to produce.
	std::vector<uint32_t> order(rects.size());
	{
		std::vector<uint32_t> nCount(33, 0), nAt(33, 0);
		for (size_t i = 0; i < rects.size(); ++i) ++nCount[rects[i].nH];
		uint32_t nRun = 0;
		for (int h = 32; h >= 1; --h) { nAt[h] = nRun; nRun += nCount[h]; }
		for (size_t i = 0; i < rects.size(); ++i) order[nAt[rects[i].nH]++] = (uint32_t)i;
	}

	std::vector<uint32_t> atlas((size_t)kAtlas * kAtlas, 0);
	std::vector<uint16_t> tex(kMaxTexels);

	int nX = 0, nY = 0, nShelf = 0, nPlaced = 0, nDropped = 0, nBadDecode = 0;
	uint64_t nTexels = 0;
	uint32_t nMaxChan = 0;			// the largest 5-bit channel value seen
	uint64_t nSumChan = 0, nChans = 0;

	for (size_t k = 0; k < order.size(); ++k)
	{
		const Rect& r = rects[order[k]];

		uint32_t nOut = 0;
		if (!LM_Decode(r.pData, r.nLen, tex.data(), kMaxTexels, &nOut)
			|| nOut != (uint32_t)r.nW * r.nH)
		{ ++nBadDecode; continue; }

		// One texel of slack between rectangles. The mapping puts every vertex
		// between the first and last texel CENTRE - measured, 19626 of 19626 -
		// so bilinear never reaches outside a rectangle and this is belt and
		// braces rather than a requirement.
		if (nX + r.nW > kAtlas) { nX = 0; nY += nShelf + 1; nShelf = 0; }
		if (nY + r.nH > kAtlas) { ++nDropped; continue; }
		if (r.nH > nShelf) nShelf = r.nH;

		for (int y = 0; y < r.nH; ++y)
			for (int x = 0; x < r.nW; ++x)
			{
				const uint32_t v = tex[(size_t)y * r.nW + x];
				const uint32_t cr = (v >> 10) & 31, cg = (v >> 5) & 31, cb = v & 31;
				if (cr > nMaxChan) nMaxChan = cr;
				if (cg > nMaxChan) nMaxChan = cg;
				if (cb > nMaxChan) nMaxChan = cb;
				nSumChan += cr + cg + cb; nChans += 3;
				atlas[(size_t)(nY + y) * kAtlas + (nX + x)] =
					0xFF000000u | ((cr * 255 / 31) << 16)
								| ((cg * 255 / 31) << 8) | (cb * 255 / 31);
			}

		LMPoly e{};
		e.fX = (float)nX; e.fY = (float)nY;
		e.nW = r.nW; e.nH = r.nH; e.fS = fS;
		const float* O = (const float*)(uintptr_t)(r.pPoly + kPolyLMOrigin);
		e.O[0] = O[0]; e.O[1] = O[1]; e.O[2] = O[2];
		if (!LM_Basis(Word(r.pPoly, kPolySurface), e.P, e.Q)) { ++nDropped; continue; }
		g_Map[r.pPoly] = e;

		nX += r.nW + 1;
		nTexels += (uint64_t)r.nW * r.nH;
		++nPlaced;
	}

	D3D11_TEXTURE2D_DESC td{};
	td.Width = kAtlas; td.Height = kAtlas; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd{};
	sd.pSysMem = atlas.data(); sd.SysMemPitch = kAtlas * 4;
	HRESULT hr = pDev->CreateTexture2D(&td, &sd, &g_pTex);
	if (SUCCEEDED(hr)) hr = pDev->CreateShaderResourceView(g_pTex, nullptr, &g_pSRV);

	Log("  LM: %u surfaces flagged; dropped %u with no size, %u with no ref,"
		" %u with no record", nFlagged, nNoSize, nNoRef, nNoRecord);
	Log("  LM: of the no-size drops, %u have a zero byte and %u are over %u texels"
		" (largest seen %u x %u); %u of them still carry a ref array",
		nZeroSize, nOverSize, kMaxTexels, nMaxW, nMaxH, nNoSizeWithRef);
	{
		std::string sD;
		for (size_t k = 0; k < kinds.size(); ++k)
			if (kinds[k].nDropped)
			{
				char sz[64]; sprintf_s(sz, "%s %u  ", kinds[k].sz, kinds[k].nDropped);
				sD += sz;
			}
		if (!sD.empty()) Log("  LM: the no-size drops by model kind: %s", sD.c_str());
	}
	{
		// Worst first: the top one or two answer the question on their own.
		std::sort(kinds.begin(), kinds.end(),
			[](const LMKind& a, const LMKind& b){ return a.n > b.n; });
		Log("  LM: which models they belong to, worst first -");
		for (size_t k = 0; k < kinds.size() && k < 8; ++k)
			if (kinds[k].n)
				Log("        %-28s %6u", kinds[k].sz, kinds[k].n);
	}
	Log("  LM: %d lightmaps packed into %dx%d, %llu texels, %d shelves deep,"
		" %d dropped, %d failed to decode",
		nPlaced, kAtlas, kAtlas, nTexels, nY + nShelf, nDropped, nBadDecode);
	Log("  LM: channel values across the whole level: largest %u of 31,"
		" mean %.2f of 31 (%.3f as a multiplier)", nMaxChan,
		nChans ? (double)nSumChan / (double)nChans : 0.0,
		nChans ? (double)nSumChan / (double)nChans / 31.0 : 0.0);
	Log("  LM: atlas hr=%08X", (unsigned)hr);
	g_Pixels.swap(atlas);
	if (pszDump && *pszDump) LMWriteBMP(pszDump, 0, 0, kAtlas,
									   (nY + nShelf + 16 < kAtlas)
									   ? nY + nShelf + 16 : kAtlas, Log);
	return g_pSRV != nullptr;
}
