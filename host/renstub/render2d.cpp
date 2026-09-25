// ---------------------------------------------------------------------------
// render2d - see render2d.h.
//
// Everything here rests on two things that were measured rather than assumed:
//
//   - the engine's surfaces are XRGB8888, B,G,R,X in memory order. Not taken
//     from GetScreenFormat's masks alone - a 640x480 surface was written out
//     as a BMP and looked at, and it is the NOLF title screen in the right
//     colours. DXGI_FORMAT_B8G8R8A8_UNORM is that layout.
//
//   - the blit request is 24 bytes: source surface, a flag, a colour, a
//     source LTRect*, a destination LTRect*, and a float alpha. The two rects
//     were told apart by evidence: the first tracks each surface's own size
//     (640x480 for the menu background, 2x2 for the dimming overlay) while the
//     second is constant at the screen rectangle.
//
// The swap chain is created at the ENGINE's mode size, not the window's client
// size, so destination rectangles map one-to-one onto back-buffer pixels and
// DXGI stretches to whatever size the window happens to be. That removes the
// resize problem rather than handling it.
// ---------------------------------------------------------------------------

#include "render2d.h"
#include "render3d.h"
#include "vrblock.h"
#include <math.h>
#include <math.h>

#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

namespace
{
	ID3D11Device*           g_pDev = nullptr;
	ID3D11DeviceContext*    g_pCtx = nullptr;
	R2D_LogFn               g_pfnLog = nullptr;

	ID3D11VertexShader*     g_pVS = nullptr;
	ID3D11PixelShader*      g_pPS = nullptr;
	ID3D11InputLayout*      g_pLayout = nullptr;
	ID3D11Buffer*           g_pVB = nullptr;
	ID3D11Buffer*           g_pCB = nullptr;
	ID3D11SamplerState*     g_pSampler = nullptr;
	ID3D11BlendState*       g_pBlend = nullptr;	// LTSURFACEBLEND_ALPHA
	ID3D11BlendState*       g_pBlendMode[7] = {};	// one per LTSURFACEBLEND_*
	ID3D11BlendState*       g_pBlendOver = nullptr;	// the overlay: over, alpha accumulates
	unsigned int            g_n2DBlendMode = 0;
	// What the engine asked for on the CURRENT blit, unclamped.
	unsigned int            g_n2DBlendRaw  = 0;
	// Which of the engine's 2D blend modes to honour.
	//
	//   0  none - every blit is ALPHA, the behaviour before any of this
	//   1  all seven
	//   2  ADD and SOLID only, everything else ALPHA   <- the default
	//
	// 2, because 1 broke the main menu and 0 makes the game black.
	//
	// ADD has to be honoured: NOLF's screen tint is an 8x8 surface stretched
	// over the whole frame with LTSURFACEBLEND_ADD, and adding black is a
	// no-op. Drawn as alpha it is an opaque black rectangle over the game,
	// which is exactly what the player saw.
	//
	// MASK must NOT be, and that is the other half of the same coin. The
	// engine draws masked text as a pair - a MASK pass that punches the glyph
	// out of the destination, then an ADD pass that puts its colour in. On a
	// black menu the MASK pass is a no-op either way, but honoured in the
	// wrong order it ERASES the glyph the ADD pass just drew. Measured: with
	// all seven modes the main menu drew one item out of five; with this it
	// draws Single player, Continue game, Multiplayer, Options, Quit and the
	// help line.
	//
	// Skipping MASK entirely was the alternative and it is equivalent here -
	// (1-src)*dest against a black destination is the destination - but alpha
	// is the mode that is known to produce the right menu, and this is not the
	// place to be clever.
	int                     g_bBlendModes  = 2;

	// SetOptimized2DColor - a colour filter the engine applies to 2D blits.
	// Recorded by the slot and never used, exactly as the blend mode was, so
	// every tint the game asked for was silently dropped: the selected menu
	// item looked identical to the unselected ones, which makes a menu
	// unusable in a headset even with every word legible.
	// HLTCOLOR is 0x00RRGGBB, bit 31 being COLOR_TRANSPARENCY_MASK.
	float                   g_f2DColour[3] = { 1.0f, 1.0f, 1.0f };

	// The TRANSPARENT COLOUR of an optimized surface.
	//
	// OptimizeSurface(HSURFACE, HLTCOLOR hTransparentColor) is slot 28, and
	// the renderer took the handle and threw the colour away - the parameter
	// was not even named. Every menu sprite with a keyed background therefore
	// drew its key colour as if it were art. That is the headset report of
	// magenta spots: magenta is the classic key, and the Phase 1 Init paint
	// that was blamed for it had already been changed to black on 3 September.
	struct KeyEntry { const void* pKey; unsigned int nColour; };
	// 512, not 64. A menu run optimizes more than 64 surfaces, and the old
	// table simply stopped recording at that point - so a surface's
	// transparent colour was kept or lost depending on how late in the run the
	// engine happened to optimize it. Nothing said so.
	KeyEntry                g_Keys[512] = {};
	long                    g_nKeysLost = 0;
	int                     g_nKeys    = 0;
	int                     g_bColourKey = 1;
	// Whether a surface's transparent colour is believed only when bit 31 is
	// set. +StubKeyMask 0 believes it either way - see the note at the test.
	int                     g_bKeyMask   = 0;
	// +StubKeyBlack: key pure black out of 2D blits that carry no key of their
	// own. The menu's font sheet is never optimized and its request colour is
	// 00FFFFFF, so neither key path can reach it - yet retail shows no black
	// box behind the text, which means black is IMPLICIT for this blit type.
	// This is the arm that tests that against a d3d.ren control.
	int                     g_bKeyBlack  = 0;
	// Set per blit for the ones that must never be keyed - the frame marker,
	// whose swatches the host reads back by pixel position to match a frame to
	// a pose. Keying black out of it turned the yellow swatch into the scene
	// behind and would have broken pose matching silently.
	int                     g_bKeySuppress = 0;
	// The transparent colour carried by the CURRENT blit request, if any.
	unsigned int            g_nBlitKey   = 0;
	int                     g_bBlitKey   = 0;
	int                     g_bColour2D    = 1;

	ID3D11RenderTargetView* g_pRTV = nullptr;
	int                     g_nTargetW = 0, g_nTargetH = 0;

	long g_nUploads = 0, g_nDraws = 0, g_nFailed = 0;
	// Re-upload every surface on every blit, ignoring the dirty flag.
	//
	// A diagnostic. UnlockSurface is the only notification the engine gives
	// that a surface changed, so a surface filled by any other route keeps
	// whatever pixels it had when its texture was first made - which for the
	// main menu's 640x480 panel would mean a black texture forever, while the
	// surface itself holds the title art. This is how that is told apart from
	// the blit being wrong.
	int  g_bAlwaysUpload = 0;

	void Log(const char* fmt, ...)
	{
		// Deliberately not variadic-forwarding: the log function is dllmain's
		// and takes a format. Only fixed strings are passed from here, with the
		// numbers baked in by the caller, so there is nothing to forward.
		if (g_pfnLog) g_pfnLog("%s", fmt);
	}

	void LogF(const char* fmt, unsigned a, unsigned b = 0)
	{
		if (!g_pfnLog) return;
		char sz[256];
		sprintf_s(sz, fmt, a, b);
		g_pfnLog("%s", sz);
	}

	struct Vert { float x, y, u, v; };

	// One texture per engine surface. A fixed table rather than a map: the
	// engine created 93 surfaces in a menu run and this is called 27 times a
	// frame, so a linear scan over a small array is both simpler and faster
	// than anything with an allocator in it.
	const int kMaxTex = 512;
	// Write each 2D surface out as a picture. Declared here rather than beside
	// the other switches because Upload() - well above them - is what uses it.
	int   g_bDump2D = 0;
	int   g_bTrace2D = 0;		// set per blit by the caller; see StubTrace2D

	// THE PAUSE OVERLAY. Pause-menu art (the menu-fit 2D layer drawn over a
	// loaded world) goes into this one-eye-wide target instead of the eyes,
	// so the host can present it as a quad anchored in the world. In the
	// fourth headset test the pause menu still followed head movement and
	// gaze instead of staying put. Cleared to transparent at the first pause
	// blit of a frame; published at present if anything was drawn.
	int                     g_bPauseQuad = 1;
	float                   g_fPauseTextWhite = 1.0f;	// +StubPauseTextWhite100
	ID3D11Texture2D*        g_pOvlTex = nullptr;
	ID3D11RenderTargetView* g_pOvlRTV = nullptr;
	int   g_nOvlW = 0, g_nOvlH = 0;
	int   g_bOvlDrawn = 0, g_bOvlCleared = 0;
	long  g_nOvlBlits = 0, g_nOvlFrames = 0;
	// Frames the overlay has been up without a break, and the per-pause trace
	// (+StubDumpOverlay): every blit of the 30th frame of each pause, for eight
	// pauses - so a pause that goes wrong after a load is caught, not only the first.
	long  g_nOvlUpRun = 0;
	int   g_bOvlTrace = 0, g_nOvlTracedPauses = 0;
	int   g_nDumpSaid = 0;
	// THE FIRST FORTY QUADS ARE ALL FROM THE TITLE SCREEN. Capping the
	// quad log at the first N calls answers a question nobody asked -
	// what the 2D layer did before the menu existed - and says nothing
	// about the frame that was photographed. So count every blit and
	// print a short BURST every so often, which reaches the end of the
	// run as well as the start.
	long  g_nBlitSeen = 0;
	// EVERY DISTINCT SURFACE, ON FIRST SIGHT. A burst of six blits every
	// four thousand is a SAMPLE, and a backdrop blitted once a frame
	// among thousands of glyph blits can hide from a sample indefinitely
	// - which is exactly the mistake that sent a search for the menu's
	// panel into the interface scene, where it is not. This list is
	// bounded by the number of SURFACES, which is single digits, so it
	// can be complete instead of representative.
	struct SeenKey { const void* p; int nW, nH; long nBlits; long nLast; };
	SeenKey g_Seen[64];
	int     g_nSeen = 0;

	struct TexEntry
	{
		const void*               pKey;
		ID3D11Texture2D*          pTex;
		ID3D11ShaderResourceView* pSRV;
		int                       nW, nH;
		bool                      bDirty;
			bool bSaid;	// its contents have been described once
		int                       nUploads;	// how many times it has been pushed
		bool                      bKeySaid;
		int                       nDumped;	// how many times it has been written out
		// A GPU-SIDE SNAPSHOT of the back buffer, standing in for the bits.
		// See R2D_Snapshot. While bSnapValid the system-memory bits are stale
		// and never uploaded; the next Invalidate drops it.
		ID3D11Texture2D*          pSnapTex;
		ID3D11ShaderResourceView* pSnapSRV;
		int                       nSnapW, nSnapH;
		bool                      bSnapValid;
	};
	TexEntry g_Tex[kMaxTex];
	int      g_nTex = 0;

	TexEntry* Find(const void* pKey)
	{
		for (int i = 0; i < g_nTex; ++i)
			if (g_Tex[i].pKey == pKey) return &g_Tex[i];
		return nullptr;
	}

	void ReleaseEntry(TexEntry* e)
	{
		if (e->pSRV) { e->pSRV->Release(); e->pSRV = nullptr; }
		if (e->pTex) { e->pTex->Release(); e->pTex = nullptr; }
		if (e->pSnapSRV) { e->pSnapSRV->Release(); e->pSnapSRV = nullptr; }
		if (e->pSnapTex) { e->pSnapTex->Release(); e->pSnapTex = nullptr; }
		e->bSnapValid = false; e->nSnapW = e->nSnapH = 0;
		e->nW = e->nH = 0;
	}

	// A SURFACE, WRITTEN OUT AS A PICTURE. A mean cannot tell a black backdrop
	// from a dark one with a bright corner, and the menu's 640x480 backdrop
	// measures 98.5 while the screen shows black - so the only honest next step
	// is to open the bytes. PPM because it needs no library and every viewer
	// reads it.
	//
	// +StubDump2D 1. Written on the first upload and again on the 200th, so a
	// surface whose contents change - an animated menu backdrop - is caught
	// changing rather than assumed static.
	void DumpSurface(const void* pKey, const void* pPixels, int nPitch,
					 int nW, int nH, int nSeq)
	{
		char szPath[256];
		sprintf_s(szPath, "logs\\2d-%08X-%dx%d-%d.ppm",
			(unsigned)(uintptr_t)pKey, nW, nH, nSeq);
		FILE* f = nullptr;
		if (fopen_s(&f, szPath, "wb") || !f) return;
		fprintf(f, "P6\n%d %d\n255\n", nW, nH);
		const BYTE* pS = (const BYTE*)pPixels;
		for (int y = 0; y < nH; ++y)
		{
			const BYTE* q = pS + (size_t)y * nPitch;
			for (int x = 0; x < nW; ++x, q += 4)
			{
				// The engine's surfaces are BGRA; PPM wants RGB.
				fputc(q[2], f); fputc(q[1], f); fputc(q[0], f);
			}
		}
		fclose(f);
		// THE MEAN, BESIDE THE FILENAME. A path cannot be compared
		// across uploads; a number can, and it is the whole difference
		// between "the engine cleared this surface" and "we are
		// drawing it wrong".
		double dSum = 0.0;
		for (int y = 0; y < nH; ++y)
		{
			const BYTE* q = pS + (size_t)y * nPitch;
			for (int x = 0; x < nW; ++x, q += 4)
				dSum += (double)q[0] + q[1] + q[2];
		}
		if (g_pfnLog) g_pfnLog("  2D DUMP: %s  mean %.1f", szPath,
					dSum / (3.0 * nW * nH));
	}

	// Make (or remake) the texture for a surface and push the pixels in.
	static ID3D11ShaderResourceView* SrvOf(const TexEntry* e)
	{
		return (e->bSnapValid && e->pSnapSRV) ? e->pSnapSRV : e->pSRV;
	}

	TexEntry* Upload(const void* pKey, const void* pPixels, int nPitch, int nW, int nH)
	{
		TexEntry* e = Find(pKey);
		// The snapshot IS the picture; the bits behind it were never read.
		if (e && e->bSnapValid && e->pSnapSRV) return e;
		if (!e)
		{
			if (g_nTex >= kMaxTex) { ++g_nFailed; return nullptr; }
			e = &g_Tex[g_nTex++];
			memset(e, 0, sizeof(*e));
			e->pKey   = pKey;
			e->bDirty = true;
		}

		// A surface that changed size is a different texture.
		if (e->pTex && (e->nW != nW || e->nH != nH)) ReleaseEntry(e);

		if (!e->pTex)
		{
			D3D11_TEXTURE2D_DESC td{};
			td.Width          = (UINT)nW;
			td.Height         = (UINT)nH;
			td.MipLevels      = 1;
			td.ArraySize      = 1;
			td.Format         = DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage          = D3D11_USAGE_DYNAMIC;
			td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

			if (FAILED(g_pDev->CreateTexture2D(&td, nullptr, &e->pTex)))
			{ ++g_nFailed; return nullptr; }
			if (FAILED(g_pDev->CreateShaderResourceView(e->pTex, nullptr, &e->pSRV)))
			{ ReleaseEntry(e); ++g_nFailed; return nullptr; }

			e->nW = nW; e->nH = nH;
			e->bDirty = true;
		}

		if (e->bDirty || g_bAlwaysUpload)
		{
			D3D11_MAPPED_SUBRESOURCE m{};
			if (FAILED(g_pCtx->Map(e->pTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
			{ ++g_nFailed; return nullptr; }

			// The engine's surfaces have their own pitch and the texture has
			// its own; copying row by row is the only correct way and the rows
			// are 4-byte pixels either way.
			const BYTE* pSrc = (const BYTE*)pPixels;
			BYTE*       pDst = (BYTE*)m.pData;
			const int   nRow = nW * 4;
			for (int y = 0; y < nH; ++y)
				memcpy(pDst + (size_t)y * m.RowPitch, pSrc + (size_t)y * nPitch, nRow);

			// WHAT IS IN THIS SURFACE? Say it once per surface, with its mean
			// colour. The menu draws black and the 2D layer only ever sees four
			// textures, so the question "which four, and are any of them the
			// menu" has to be answered before anything can be built on it.
			// A pointer and a size cannot answer it; a mean colour can.
			if (!e->bSaid && g_pfnLog)
			{
				e->bSaid = true;
				unsigned long long nSum = 0; int nCnt = 0;
				const BYTE* pS2 = (const BYTE*)pPixels;
				// AND THE ALPHA, because the shader throws it away and the
				// whole question is whether it was ever worth anything. Also
				// the corner pixel, which on a text surface is background: if
				// the background is a colour the engine named transparent we
				// key it, and if it is transparent by ALPHA we cannot, because
				// PSMain returns gParams.x and never c.a.
				unsigned nAMin = 255, nAMax = 0;
				unsigned nBG = 0;
				for (int y = 0; y < nH; y += 4)
					for (int x = 0; x < nW; x += 4)
					{
						const BYTE* q = pS2 + (size_t)y * nPitch + (size_t)x * 4;
						nSum += (unsigned)q[0] + q[1] + q[2];
						if (q[3] < nAMin) nAMin = q[3];
						if (q[3] > nAMax) nAMax = q[3];
						++nCnt;
					}
				{
					const BYTE* q0 = pS2;
					nBG = ((unsigned)q0[3] << 24) | ((unsigned)q0[2] << 16)
						| ((unsigned)q0[1] << 8) | (unsigned)q0[0];
				}
				char szD[256];
				sprintf_s(szD, "  2D SURFACE %08X  %dx%d  mean %.1f  alpha %u..%u"
					"  corner ARGB %08X%s%s",
					(unsigned)(uintptr_t)pKey, nW, nH,
					nCnt ? (double)nSum / (double)(nCnt * 3) : 0.0,
					nAMin, nAMax, nBG,
					(nAMin == nAMax) ? "   <- alpha is CONSTANT, carries nothing" : "",
					(nCnt && nSum == 0) ? "   <- ENTIRELY BLACK" : "");
				g_pfnLog("%s", szD);
			}

			// EVERY UPLOAD, not the first and the 200th. The 640x480 menu
			// surface holds the title art on upload 0 and draws BLACK, so
			// the question is whether a LATER upload replaces it with
			// black - and a dump that stops after two cannot see that. A
			// whole run is 8 uploads; dumping all of them costs nothing.
			if (g_bDump2D && e->nDumped < 16)
			{
				++e->nDumped;
				DumpSurface(pKey, pPixels, nPitch, nW, nH, e->nUploads);
			}
			++e->nUploads;

			g_pCtx->Unmap(e->pTex, 0);
			e->bDirty = false;
			++g_nUploads;
			if (g_bTrace2D && g_pfnLog)
			{
				// the mean of the middle row, so the log can say WHICH art
				long nSum = 0; const BYTE* q = (const BYTE*)pPixels + (size_t)(nH / 2) * nPitch;
				for (int x = 0; x < nW; ++x, q += 4) nSum += q[0] + q[1] + q[2];
				g_pfnLog("  T2D UPLOAD %p %dx%d upload #%ld mid-row mean %.1f",
						 pKey, nW, nH, e->nUploads, nW ? (double)nSum / (3.0 * nW) : 0.0);
			}
		}
		return e;
	}

	const char* kShader =
		"cbuffer C : register(b0) { float4 gParams; float4 gKey; float4 gSoft; }\n"
		"Texture2D    gTex : register(t0);\n"
		"SamplerState gSmp : register(s0);\n"
		"struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
		"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
		"VSOut VSMain(VSIn i) {\n"
		"  VSOut o; o.pos = float4(i.pos, 0.0f, 1.0f); o.uv = i.uv; return o;\n"
		"}\n"
		"float4 PSMain(VSOut i) : SV_Target {\n"
		// DOES THIS QUAD LAND AT ALL? gKey.w == 2 paints it with its own UVs
		// and never touches the texture, which separates "the draw is being
		// overwritten" from "the draw happens and the texture is black". The
		// menu backdrop reports a correct quad, a correct tint and an intact
		// source surface, and comes out black - so one of those is a lie.
		"  if (gKey.w > 1.5f) return float4(i.uv.x, i.uv.y, 0.0f, 1.0f);\n"
		"  float4 c = gTex.Sample(gSmp, i.uv);\n"
		// A BINARY COLOUR KEY CANNOT SURVIVE FILTERING, AND THAT IS THE BOX.
		//
		// The font sheet is 1096x28 and each glyph lands in a destination a
		// fraction of that, so the sampler mixes white glyph with black
		// background and returns every value in between. An exact test keeps
		// all of them, the tint darkens them, and the result is the dark fringe
		// round every word - measured at (33,12,0), which is WHITE times the
		// shadow tint, not black at all. Widening the tolerance would eat the
		// glyph instead.
		//
		// So the key sets ALPHA rather than deciding a discard: distance from
		// the key, scaled steeply, is 0 exactly on the key and 1 a few per cent
		// away. Fringe texels come out proportionally transparent, which is
		// what antialiased text IS, and art further than 1/gSoft.x from the key
		// is untouched. +StubKeySoft 0 restores the old hard test.
		"  float a = 1.0f;\n"
		"  if (gKey.w > 0.5f) {\n"
		"    float d = length(c.rgb - gKey.rgb);\n"
		"    if (gSoft.x > 0.5f) a = saturate(d * gSoft.x);\n"
		"    else if (all(abs(c.rgb - gKey.rgb) < 0.002f)) discard;\n"
		"    if (a <= 0.004f) discard;\n"
		"  }\n"
		// THE OVERLAY HAS NO WORLD UNDER IT. The pause menu's art is drawn into
		// a transparent texture the compositor lays over the world, so a blit
		// that expects to modulate the destination - MASK, (1-src)*dest, the
		// text sheets - would paint an opaque black box. gSoft.y carries the
		// engine's blend mode when the target is the overlay: MASK becomes
		// black with coverage = luminance (a white glyph darkens fully, the
		// black sheet not at all), ADD the colour with coverage = luminance.
		// Composited over the world with source alpha, each comes out as the
		// mode meant.
		// THE SELECTED ITEM IS THE ADD PASS. Menu text is a pair: a MASK pass
		// that punches the glyph out (black on the page), then an ADD pass
		// that puts the item's colour in - white for the selected item, and
		// nothing to see for the rest. In the overlay the MASK pass painted
		// gSoft.z, which was 1.0, so the ADD had nothing left to brighten and
		// every item read the same. In the headset the pause menu was all
		// white, with no way to tell which item was selected. gSoft.z is now
		// 0.55 (+StubPauseTextWhite100 55): the unselected are that grey, and
		// the selected item's ADD pass lifts it to white.
		"  if (gSoft.y > 4.5f && gSoft.y < 5.5f) {\n"
		"    float l = dot(c.rgb, float3(0.299f, 0.587f, 0.114f));\n"
		"    return float4(gSoft.z, gSoft.z, gSoft.z, gParams.x * a * saturate(l));\n"
		"  }\n"
		// ADD contributes by what it ADDS: its coverage is the luminance of
		// the COLOURED glyph, so an add of black - the unselected item's
		// colour - leaves the grey MASK glyph alone instead of painting over
		// it with black.
		"  if (gSoft.y > 1.5f && gSoft.y < 2.5f) {\n"
		"    float3 o = c.rgb * gParams.yzw;\n"
		"    float l = dot(o, float3(0.299f, 0.587f, 0.114f));\n"
		"    return float4(o, gParams.x * a * saturate(l * 1.5f));\n"
		"  }\n"
		"  return float4(c.rgb * gParams.yzw, gParams.x * a);\n"
		"}\n";

	bool Compile(const char* pszEntry, const char* pszTarget, ID3DBlob** ppOut)
	{
		ID3DBlob* pErr = nullptr;
		const HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr,
									  nullptr, pszEntry, pszTarget, 0, 0, ppOut, &pErr);
		if (FAILED(hr))
		{
			if (g_pfnLog)
			{
				g_pfnLog("  R2D: %s (%s) failed to compile, hr=%08X",
						 pszEntry, pszTarget, (unsigned)hr);
				if (pErr) g_pfnLog("  R2D: %.400s", (const char*)pErr->GetBufferPointer());
			}
			if (pErr) pErr->Release();
			return false;
		}
		if (pErr) pErr->Release();
		return true;
	}
	// Where the forward ray lands in each half, in that half's own NDC, and
	// how much of the eye the 2D layer is allowed to fill. Both are set from
	// outside; the defaults are the middle of the half at the fit scale, which
	// is what a symmetric frustum would give.
	float g_fEyeCentreX[2] = { 0.0f, 0.0f };
	float g_fEyeCentreY[2] = { 0.0f, 0.0f };
	float g_fStereoScale   = 1.0f;
	// WHERE THE 2D LAYER SITS IN DEPTH. Placed on each eye's forward ray it
	// is at infinity - the same direction from both eyes - and a caption at
	// infinity drawn over a wall two metres away cannot be fused with that
	// wall. Headset testing found them hard to read, set at infinity and
	// nearly cross-eyed to fuse. A convergence shift of (IPD/2)/depth in tangent
	// units, inward per eye, puts the whole layer at this distance instead.
	// Centimetres; 0 puts it back at infinity. +StubHudDepthCm.
	float g_fHudDepthCm    = 250.0f;
	float g_fIpdM          = 0.063f;

	// The inward NDC shift for one eye at the HUD depth: positive moves
	// content right. The left eye's content moves right, the right eye's
	// left, and the two rays meet at the depth.
	static float ConvergeNdc(int nEye)
	{
		if (g_fHudDepthCm <= 0.0f) return 0.0f;
		VRBFrustum fr{};
		if (!VRB_EyeFrustum(nEye, &fr)) return 0.0f;
		const float fW = fr.fTanR - fr.fTanL;
		if (fW <= 0.01f) return 0.0f;
		const float fTan = (g_fIpdM * 0.5f) / (g_fHudDepthCm * 0.01f);
		const float fNdc = fTan * 2.0f / fW;
		return (nEye == 0) ? fNdc : -fNdc;
	}
	// EXTRA scale for the 2D layer while in a MENU, so that the part of the
	// layout the menu actually uses spans the eye rather than the part the
	// engine hides. See the fit below. 0 derives it from the layout's shape.
	float g_fMenuScale     = 0.0f;
	float g_fPauseScale    = 0.0f;	// the pause menu over a live world
	float g_fPauseShiftX   = 0.0f;	// and its sideways shift, layout NDC
	// WORLD-LOCK for the pause menu: an NDC offset per eye that cancels the
	// head's movement since the menu opened, so the panel stays where it
	// was in the world while the eye buffer turns. Set per frame by the
	// caller from the head pose and the eye's frustum tans.
	float g_fLockDX[2] = { 0.0f, 0.0f };
	float g_fLockDY[2] = { 0.0f, 0.0f };
	// THE MENU ZOOM, and the anchor it zooms about.
	//
	// Small menu text and an over-wide menu panel are ONE problem: a
	// menu line is 0.0076 of the menu's width in retail and 0.0078 in ours, so
	// the layout is faithful and therefore small in DEGREES. The panel is 63
	// degrees wide and the interactive card uses only its left third, so most
	// of what the eye scans is decoration - the logo, the backdrop, Cate.
	//
	// A plain scale cannot fix that, and it is why +StubMenuScale100 was never
	// a usable lever: the NDC below is cx + fL*fS, so multiplying fS zooms
	// about the SCREEN CENTRE, and the card is left of centre - it grows and
	// slides off the left edge at the same time. The zoom needs an ANCHOR: the
	// point that ends up in the middle of the eye.
	//
	//   ndc' = (ndc - anchor) * zoom
	//
	// Zoom 1 and anchor 0 is the identity and is the default, so this changes
	// nothing until it is asked for. The 3D interface scene takes the SAME
	// zoom and anchor through its frustum tangents - see R3D_SetMenuZoom - or
	// the card and the words on it would part company, which is exactly what
	// +StubMenuScale100 did.
	// MEASURED, on two folders that share nothing else: the card spans eye-NDC
	// x -0.93..-0.21 and y -0.40..+0.47 on BOTH the main menu and Options, so
	// NOLF authors every folder's card at the same place and one anchor serves
	// all 54. Its centre is (-0.57, +0.04).
	//
	// WHY THE PAUSE MENU IS LEFT ALONE. It is a projection layer over a live
	// world and inherits that world's per-eye centres, so magnifying about an
	// anchor would magnify the disparity with it and change the panel's
	// apparent DEPTH as well as its size. At the main menu both centres are
	// zero and the two halves come out pixel identical, so a zoom there cannot
	// introduce disparity at all. Only the second case is done here; the first
	// is a question for a headset, not for a screenshot.
	float g_fMenuZoom      = 1.0f;
	float g_fMenuAnchorX   = 0.0f;
	float g_fMenuAnchorY   = 0.0f;
	int   g_bMenu          = 0;	// the client is not playing
	// THE MENU PANEL IS A 16:9 BAND OF THE EYE, not the whole eye. The host
	// submits a centred band of that aspect as the panel (see vrmain.cpp),
	// so the 2D layout is fitted to the band's HEIGHT and the 3D interface
	// scene - the blue backdrop, Cate, the logo - shows either side of the
	// 4:3 card, so the menu reads as a wide screen. 0 = the whole eye,
	// the pre-13 September fit. +StubMenuBand100 178 is 16:9.
	float g_fMenuBand      = 16.0f / 9.0f;
	// The 3D interface zoom of the menu pass (render3d.cpp: the band spans
	// the authored height, capped at 100 degrees wide) - what the scene's
	// 90 degrees spans of the eye's width. The 2D layer's 4:3 core, fitted
	// to the eye's width, is scaled by the same number so text stays inside
	// its 3D boxes. 1 when no menu pass has said otherwise.
	float g_fMenuZoomExtra = 1.0f;
	// FIT AND FLATTEN ARE TWO DIFFERENT DECISIONS AND g_bMenu WAS BOTH.
	//
	//   FIT      - scale the 2D layout so the part the menu uses spans the eye
	//              instead of the part the engine hides. A folder's art is a
	//              menu layout wherever it appears, so this is true for the
	//              PAUSE menu too.
	//   FLATTEN  - zero the per-eye centres so both halves are identical. That
	//              is right only when the host presents a QUAD, and the pause
	//              menu is a projection layer because the world behind it must
	//              stay in stereo.
	//
	// Tying them together is what made the pause menu unusable either way:
	// with g_bMenu off the layout stayed full-frame and the per-eye centring
	// then pushed part of it across the seam, so EACH EYE SAW A DIFFERENT
	// PIECE - rivalry, which is why closing one eye helped. With g_bMenu on
	// the halves matched but sat 15 degrees apart in the world. The layout
	// needed fitting; it never needed flattening.
	int   g_bMenuFit       = 0;
	// Slope turning distance-from-the-key into alpha. 32 means a texel more
	// than about 3% from the key is fully opaque. 0 = the old hard test.
	float g_fKeySoft       = 32.0f;
	long  g_nBarsSkipped   = 0;	// 4:3 filler bars not drawn in a fitted menu

}

bool R2D_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx, R2D_LogFn pfnLog)
{
	g_pDev = pDev; g_pCtx = pCtx; g_pfnLog = pfnLog;
	if (!pDev || !pCtx) return false;

	ID3DBlob* pVSb = nullptr;
	ID3DBlob* pPSb = nullptr;
	if (!Compile("VSMain", "vs_4_0", &pVSb)) return false;
	if (!Compile("PSMain", "ps_4_0", &pPSb)) { pVSb->Release(); return false; }

	HRESULT hr = pDev->CreateVertexShader(pVSb->GetBufferPointer(),
										  pVSb->GetBufferSize(), nullptr, &g_pVS);
	LogF("  R2D: CreateVertexShader hr=%08X", (unsigned)hr);
	hr = pDev->CreatePixelShader(pPSb->GetBufferPointer(),
								 pPSb->GetBufferSize(), nullptr, &g_pPS);
	LogF("  R2D: CreatePixelShader hr=%08X", (unsigned)hr);

	const D3D11_INPUT_ELEMENT_DESC kLayout[2] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = pDev->CreateInputLayout(kLayout, 2, pVSb->GetBufferPointer(),
								 pVSb->GetBufferSize(), &g_pLayout);
	LogF("  R2D: CreateInputLayout hr=%08X", (unsigned)hr);
	pVSb->Release();
	pPSb->Release();

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth      = sizeof(Vert) * 4;
	bd.Usage          = D3D11_USAGE_DYNAMIC;
	bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = pDev->CreateBuffer(&bd, nullptr, &g_pVB);
	LogF("  R2D: vertex buffer hr=%08X", (unsigned)hr);

	D3D11_BUFFER_DESC cd{};
	cd.ByteWidth      = 48;					// gParams, gKey, and gSoft
	cd.Usage          = D3D11_USAGE_DYNAMIC;
	cd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
	cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = pDev->CreateBuffer(&cd, nullptr, &g_pCB);
	LogF("  R2D: constant buffer hr=%08X", (unsigned)hr);

	D3D11_SAMPLER_DESC sd{};
	sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD   = D3D11_FLOAT32_MAX;
	hr = pDev->CreateSamplerState(&sd, &g_pSampler);
	LogF("  R2D: sampler hr=%08X", (unsigned)hr);

	D3D11_BLEND_DESC bl{};
	bl.RenderTarget[0].BlendEnable    = TRUE;
	bl.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
	bl.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
	bl.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bl.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = pDev->CreateBlendState(&bl, &g_pBlend);
	{
		D3D11_BLEND_DESC ov = bl;
		ov.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
		ov.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		pDev->CreateBlendState(&ov, &g_pBlendOver);
	}
	LogF("  R2D: blend state hr=%08X", (unsigned)hr);

	// The other six the engine can ask for. All of them are expressible, and
	// building them all costs the same as building the one that was missing:
	// a mode we silently treat as alpha is a bug nobody can see until it paints
	// something out.
	//
	//   0 ALPHA      lerp(src, dest, a)      SRC_ALPHA / INV_SRC_ALPHA
	//   1 SOLID      src                     ONE / ZERO
	//   2 ADD        src + dest              ONE / ONE
	//   3 MULTIPLY   src * dest              ZERO / SRC_COLOR
	//   4 MULTIPLY2  src * dest * 2          DEST_COLOR / SRC_COLOR
	//   5 MASK       (1 - src) * dest        ZERO / INV_SRC_COLOR
	//   6 MASKADD    src + (1 - src) * dest  ONE / INV_SRC_COLOR
	struct ModeDef { D3D11_BLEND src, dst; const char* pszName; };
	const ModeDef kModes[7] = {
		{ D3D11_BLEND_SRC_ALPHA,  D3D11_BLEND_INV_SRC_ALPHA, "alpha"     },
		{ D3D11_BLEND_ONE,        D3D11_BLEND_ZERO,          "solid"     },
		{ D3D11_BLEND_ONE,        D3D11_BLEND_ONE,           "add"       },
		{ D3D11_BLEND_ZERO,       D3D11_BLEND_SRC_COLOR,     "multiply"  },
		{ D3D11_BLEND_DEST_COLOR, D3D11_BLEND_SRC_COLOR,     "multiply2" },
		{ D3D11_BLEND_ZERO,       D3D11_BLEND_INV_SRC_COLOR, "mask"      },
		{ D3D11_BLEND_ONE,        D3D11_BLEND_INV_SRC_COLOR, "maskadd"   },
	};
	int nModesOk = 0;
	for (int i = 0; i < 7; ++i)
	{
		D3D11_BLEND_DESC m{};
		m.RenderTarget[0].BlendEnable    = TRUE;
		m.RenderTarget[0].SrcBlend       = kModes[i].src;
		m.RenderTarget[0].DestBlend      = kModes[i].dst;
		m.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
		m.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
		m.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		m.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
		m.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (SUCCEEDED(pDev->CreateBlendState(&m, &g_pBlendMode[i]))) ++nModesOk;
		else
		{
			char szF[128];
			sprintf_s(szF, "  R2D: blend mode %d (%s) FAILED", i, kModes[i].pszName);
			Log(szF);
		}
	}
	LogF("  R2D: %d of 7 blend modes ready", nModesOk);

	const bool bOk = g_pVS && g_pPS && g_pLayout && g_pVB && g_pCB &&
					 g_pSampler && g_pBlend;
	Log(bOk ? "  R2D: 2D pipeline ready" : "  R2D: PIPELINE INCOMPLETE - nothing will draw");
	return bOk;
}

void R2D_Destroy()
{
	if (g_pOvlRTV) { g_pOvlRTV->Release(); g_pOvlRTV = nullptr; }
	if (g_pOvlTex) { g_pOvlTex->Release(); g_pOvlTex = nullptr; }
	g_nOvlW = g_nOvlH = 0;
	for (int i = 0; i < g_nTex; ++i) ReleaseEntry(&g_Tex[i]);
	g_nTex = 0;
	if (g_pBlend)   { g_pBlend->Release();   g_pBlend = nullptr; }
	if (g_pBlendOver) { g_pBlendOver->Release(); g_pBlendOver = nullptr; }
	for (int i = 0; i < 7; ++i)
		if (g_pBlendMode[i]) { g_pBlendMode[i]->Release(); g_pBlendMode[i] = nullptr; }
	g_n2DBlendMode = 0;
	if (g_pSampler) { g_pSampler->Release(); g_pSampler = nullptr; }
	if (g_pCB)      { g_pCB->Release();      g_pCB = nullptr; }
	if (g_pVB)      { g_pVB->Release();      g_pVB = nullptr; }
	if (g_pLayout)  { g_pLayout->Release();  g_pLayout = nullptr; }
	if (g_pPS)      { g_pPS->Release();      g_pPS = nullptr; }
	if (g_pVS)      { g_pVS->Release();      g_pVS = nullptr; }
	g_pDev = nullptr; g_pCtx = nullptr;
}

void R2D_SetTarget(ID3D11RenderTargetView* pRTV, int nW, int nH)
{
	g_pRTV = pRTV; g_nTargetW = nW; g_nTargetH = nH;
}

void R2D_Invalidate(const void* pKey)
{
	TexEntry* e = Find(pKey);
	if (e) { e->bDirty = true; e->bSnapValid = false; }
}

// THE EYE COPY ON THE GPU. The client moves the first eye across the frame
// with DrawSurfaceToSurface(stash, screen) then (screen, stash): the engine
// asks us to read the back buffer into the stash's bits (BlitFromScreen) and
// later to draw them back. Reading it back meant a full GPU stall and a
// 16 MB copy each way, every frame - 3.3 ms in the screen lock alone, and the
// pipeline drained. This keeps the rect on the GPU: one CopySubresourceRegion
// into a texture that then stands in for the surface's bits.
bool R2D_Snapshot(const void* pKey, int sx, int sy, int nW, int nH)
{
	if (!g_pCtx || !g_pDev || !g_pRTV || nW <= 0 || nH <= 0) return false;
	if (sx < 0 || sy < 0 || sx + nW > g_nTargetW || sy + nH > g_nTargetH) return false;
	TexEntry* e = Find(pKey);
	if (!e)
	{
		if (g_nTex >= kMaxTex) return false;
		e = &g_Tex[g_nTex++];
		memset(e, 0, sizeof(*e));
		e->pKey   = pKey;
		e->bDirty = true;
	}
	if (e->pSnapTex && (e->nSnapW != nW || e->nSnapH != nH))
	{
		if (e->pSnapSRV) { e->pSnapSRV->Release(); e->pSnapSRV = nullptr; }
		if (e->pSnapTex) { e->pSnapTex->Release(); e->pSnapTex = nullptr; }
	}
	if (!e->pSnapTex)
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = (UINT)nW; td.Height = (UINT)nH;
		td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(g_pDev->CreateTexture2D(&td, nullptr, &e->pSnapTex))) return false;
		if (FAILED(g_pDev->CreateShaderResourceView(e->pSnapTex, nullptr, &e->pSnapSRV)))
		{ e->pSnapTex->Release(); e->pSnapTex = nullptr; return false; }
		e->nSnapW = nW; e->nSnapH = nH;
	}
	ID3D11Resource* pBack = nullptr;
	g_pRTV->GetResource(&pBack);
	if (!pBack) return false;
	D3D11_BOX box = { (UINT)sx, (UINT)sy, 0, (UINT)(sx + nW), (UINT)(sy + nH), 1 };
	g_pCtx->CopySubresourceRegion(e->pSnapTex, 0, 0, 0, 0, pBack, 0, &box);
	// THE FIRST THREE, READ BACK: the mean colour of the source rect's
	// centre and of the snapshot's, so "the copy failed" and "the copy took
	// the wrong picture" are told apart in the log rather than argued.
	static int s_nSnapProbe = 0;
	if (s_nSnapProbe < 3 && g_pfnLog)
	{
		++s_nSnapProbe;
		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = 64; sd.Height = 64; sd.MipLevels = 1; sd.ArraySize = 1;
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D* pSt = nullptr;
		if (SUCCEEDED(g_pDev->CreateTexture2D(&sd, nullptr, &pSt)))
		{
			double fm[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };
			for (int k = 0; k < 2; ++k)
			{
				D3D11_BOX b2 = { (UINT)(sx + nW / 2 - 32), (UINT)(sy + nH / 2 - 32), 0,
								 (UINT)(sx + nW / 2 + 32), (UINT)(sy + nH / 2 + 32), 1 };
				if (k == 1) { b2.left = nW / 2 - 32; b2.top = nH / 2 - 32; b2.right = nW / 2 + 32; b2.bottom = nH / 2 + 32; }
				g_pCtx->CopySubresourceRegion(pSt, 0, 0, 0, 0, k ? (ID3D11Resource*)e->pSnapTex : pBack, 0, &b2);
				D3D11_MAPPED_SUBRESOURCE mm{};
				if (SUCCEEDED(g_pCtx->Map(pSt, 0, D3D11_MAP_READ, 0, &mm)))
				{
					for (int y = 0; y < 64; ++y)
						for (int x = 0; x < 64; ++x)
						{
							const BYTE* q = (const BYTE*)mm.pData + y * mm.RowPitch + x * 4;
							fm[k][0] += q[2]; fm[k][1] += q[1]; fm[k][2] += q[0];
						}
					g_pCtx->Unmap(pSt, 0);
				}
			}
			pSt->Release();
			g_pfnLog("  R2D SNAPSHOT %d: key %p rect %d,%d %dx%d target %dx%d | source centre mean %.0f %.0f %.0f | snapshot centre mean %.0f %.0f %.0f",
				s_nSnapProbe, pKey, sx, sy, nW, nH, g_nTargetW, g_nTargetH,
				fm[0][0] / 4096, fm[0][1] / 4096, fm[0][2] / 4096,
				fm[1][0] / 4096, fm[1][1] / 4096, fm[1][2] / 4096);
		}
	}
	pBack->Release();
	e->bSnapValid = true;
	e->bDirty = false;
	return true;
}

void R2D_Forget(const void* pKey)
{
	// DROP THE COLOUR KEY TOO. A KEY OUTLIVING ITS SURFACE IS A KEY ON THE
	// NEXT SURFACE.
	//
	// g_Keys is addressed by the surface pointer and the engine recycles those
	// addresses, so an entry left behind here is inherited whole by whatever
	// is allocated at the same address next - and a surface that should have
	// no key then has one, or has the wrong colour. That is the black box
	// still behind the menu's help line after every other one had gone: the
	// strip had a stale key, so the "no key, assume black" path never ran for
	// it. Same mistake as the recycled texture objects; see the sprite
	// texture-by-name rule.
	for (int i = 0; i < g_nKeys; ++i)
		if (g_Keys[i].pKey == pKey)
		{
			g_Keys[i] = g_Keys[--g_nKeys];
			break;
		}

	TexEntry* e = Find(pKey);
	if (!e) return;
	ReleaseEntry(e);
	*e = g_Tex[--g_nTex];			// order does not matter; the key is the identity
}

// EVERY SURFACE, WITH ITS LAST BLIT. "First seen" says a surface existed;
// it cannot say whether it is still being drawn in the frame that was
// photographed, and for a backdrop that is the entire question. nLast is the
// blit index it was last drawn at, so a surface that stopped shows a number
// far below the total.
void R2D_ReportSurfaces()
{
	if (!g_pfnLog) return;
	g_pfnLog("   2D: %ld filler-bar blits skipped in a fitted menu", g_nBarsSkipped);
	g_pfnLog("   2D: pause overlay - %ld blits over %ld frames (%s)", g_nOvlBlits,
			 g_nOvlFrames, g_bPauseQuad ? "on: the host presents it as a quad" : "off");
	g_pfnLog("   2D: %d surfaces carry a transparent colour%s", g_nKeys,
		g_nKeysLost ? "  <- AND THE TABLE OVERFLOWED, see g_Keys" : "");
	g_pfnLog("   2D SURFACES SEEN THIS RUN (%d of them, %ld blits total):",
		g_nSeen, g_nBlitSeen);
	for (int s = 0; s < g_nSeen; ++s)
		g_pfnLog("     %08X %5dx%-5d  %8ld blits, last at %ld of %ld%s",
			(unsigned)(uintptr_t)g_Seen[s].p, g_Seen[s].nW, g_Seen[s].nH,
			g_Seen[s].nBlits, g_Seen[s].nLast, g_nBlitSeen,
			(g_nBlitSeen && g_Seen[s].nLast < g_nBlitSeen - 200)
				? "   <- STOPPED" : "");
}

void R2D_Stats(int* pnTextures, long* pnUploads, long* pnDraws, long* pnFailed)
{
	if (pnTextures) *pnTextures = g_nTex;
	if (pnUploads)  *pnUploads  = g_nUploads;
	if (pnDraws)    *pnDraws    = g_nDraws;
	if (pnFailed)   *pnFailed   = g_nFailed;
}

// The draw itself, in normalised device coordinates and into a given
// viewport. Split out of R2D_Blit so the stereo path can call it twice with
// two viewports and two quads, and so there is exactly ONE piece of code that
// sets render state - a second copy of this is how a per-eye path ends up
// subtly different from the mono one.
static bool EnsureOverlay()
{
	const int nW = g_nTargetW / 2, nH = g_nTargetH;
	if (nW <= 0 || nH <= 0 || !g_pDev) return false;
	if (g_pOvlTex && g_nOvlW == nW && g_nOvlH == nH) return true;
	if (g_pOvlRTV) { g_pOvlRTV->Release(); g_pOvlRTV = nullptr; }
	if (g_pOvlTex) { g_pOvlTex->Release(); g_pOvlTex = nullptr; }
	D3D11_TEXTURE2D_DESC td{};
	td.Width = (UINT)nW; td.Height = (UINT)nH;
	td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(g_pDev->CreateTexture2D(&td, nullptr, &g_pOvlTex))) return false;
	if (FAILED(g_pDev->CreateRenderTargetView(g_pOvlTex, nullptr, &g_pOvlRTV)))
	{ g_pOvlTex->Release(); g_pOvlTex = nullptr; return false; }
	g_nOvlW = nW; g_nOvlH = nH;
	if (g_pfnLog) g_pfnLog("  R2D: pause overlay target %dx%d", nW, nH);
	return true;
}


// See TimedMap in render3d.cpp: the 2D layer's two per-quad maps, timed.
static double g_fMap2DMs[2] = { 0, 0 }, g_fMap2DWorst[2] = { 0, 0 };
static long   g_nMap2DCalls[2] = { 0, 0 }, g_nMap2DSlow[2] = { 0, 0 };
static HRESULT TimedMap2D(ID3D11Resource* pRes, D3D11_MAPPED_SUBRESOURCE* pOut, int nSite)
{
	LARGE_INTEGER a, b, f;
	QueryPerformanceCounter(&a);
	const HRESULT hr = g_pCtx->Map(pRes, 0, D3D11_MAP_WRITE_DISCARD, 0, pOut);
	QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
	const double ms = f.QuadPart ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
	++g_nMap2DCalls[nSite]; g_fMap2DMs[nSite] += ms;
	if (ms > g_fMap2DWorst[nSite]) g_fMap2DWorst[nSite] = ms;
	if (ms > 2.0) ++g_nMap2DSlow[nSite];
	return hr;
}
static void DrawQuad(TexEntry* e, const D3D11_VIEWPORT& vp,
					 float x0, float y0, float x1, float y1,
					 float u0, float v0, float u1, float v1, float fAlpha)
{
	D3D11_MAPPED_SUBRESOURCE m{};
	if (FAILED(TimedMap2D(g_pVB, &m, 0))) { ++g_nFailed; return; }
	Vert* v = (Vert*)m.pData;
	v[0] = { x0, y0, u0, v0 };			// triangle strip, top-left first
	v[1] = { x1, y0, u1, v0 };
	v[2] = { x0, y1, u0, v1 };
	v[3] = { x1, y1, u1, v1 };
	g_pCtx->Unmap(g_pVB, 0);

	if (SUCCEEDED(TimedMap2D(g_pCB, &m, 1)))
	{
		float* p = (float*)m.pData;
		p[0] = fAlpha;
		p[1] = g_f2DColour[0]; p[2] = g_f2DColour[1]; p[3] = g_f2DColour[2];

		// The colour key for THIS surface, if it was optimized with one.
		p[4] = p[5] = p[6] = 0.0f; p[7] = 0.0f;

		// +StubDump2D 2: paint the BIG surfaces with their own UVs instead of
		// their texture. Only the big ones, so the text and the HUD still draw
		// and the picture stays readable while the backdrop answers for itself.
		if (g_bDump2D >= 2 && e && e->nW >= 500 && e->nH >= 400)
			p[7] = 2.0f;

		// The blit's own key first - it is per draw and beats the surface's.
		if (g_bColourKey && g_bBlitKey)
		{
			p[4] = (float)((g_nBlitKey >> 16) & 0xFF) / 255.0f;
			p[5] = (float)((g_nBlitKey >>  8) & 0xFF) / 255.0f;
			p[6] = (float)( g_nBlitKey        & 0xFF) / 255.0f;
			p[7] = 1.0f;
		}
		else if (g_bColourKey && e)
			for (int i = 0; i < g_nKeys; ++i)
			{
				// FFFFFFFF IS THE "NO TRANSPARENCY" SENTINEL, NOT A QUALIFIER.
				//
				// This used to require bit 31 - "COLOR_TRANSPARENCY_MASK" -
				// before believing a surface's transparent colour. The census
				// says what the engine actually passes: 00000000, FFFFFFFF and
				// 00FF00FF. Only FFFFFFFF has bit 31, and it has it because
				// every bit is set, not because it was flagged. So the test
				// believed the one value that means "none" and threw away both
				// real keys.
				//
				// MAGENTA IS THE PROOF. The blit-side key exists solely
				// because 00FF00FF is this game's transparent colour and
				// nothing in its art is pure magenta - and the surface side
				// was discarding exactly that value. Black is the same
				// mistake, and black discarded is an opaque box behind white
				// text: the menu items, the help line, the cutscene caption.
				//
				// +StubKeyMask 1 restores the old test for comparison.
				// WHITE IS THE SENTINEL, WHATEVER ITS ALPHA BYTE.
				//
				// FFFFFFFF was the first one found. The help line's strip is
				// optimized with 00FFFFFF instead, and treating that as a real
				// key keys out the WHITE TEXT and leaves the black background
				// standing - which is the one box that survived every other
				// fix here. Both are white; the alpha byte is not carrying a
				// decision. So the test is on the colour, not the word.
				//
				// Black and magenta remain real keys, and a surface with no
				// usable key falls through to the implicit black below, which
				// is what the text needs.
				const bool bHasKey = g_bKeyMask
					? ((g_Keys[i].nColour & 0x80000000u) != 0)
					: ((g_Keys[i].nColour & 0x00FFFFFFu) != 0x00FFFFFFu);
				if (g_Keys[i].pKey == e->pKey && bHasKey)
				{
					const unsigned int c = g_Keys[i].nColour;
					p[4] = (float)((c >> 16) & 0xFF) / 255.0f;
					p[5] = (float)((c >>  8) & 0xFF) / 255.0f;
					p[6] = (float)( c        & 0xFF) / 255.0f;
					p[7] = 1.0f;
					break;
				}
			}
		// Nothing named a key for this surface. If black is implicit for
		// untagged blits, this is where it applies - and p[7] is still 0 here,
		// so it cannot override a key that was found above.
		// +StubKeyBlack 2 forces black over ANY key already chosen. It is a
		// bisector, not a setting: if a black box survives it, that box is not
		// coming from a keyable 2D blit at all and the search moves elsewhere.
		const bool bHadKey = (p[7] > 0.5f);
		if (g_bKeyBlack && !g_bKeySuppress
			&& (p[7] < 0.5f || g_bKeyBlack >= 2))
		{
			p[4] = p[5] = p[6] = 0.0f;
			p[7] = 1.0f;
		}

		// WHICH KEY WON, ONCE PER SURFACE. Forcing black emptied the help
		// line's box, so a key was being chosen for it and was not black -
		// this says which, and where it came from.
		if (e && !e->bKeySaid && g_pfnLog)
		{
			e->bKeySaid = true;
			g_pfnLog("   2D KEY: surface %08X %dx%d -> %s  rgb %.2f %.2f %.2f",
				(unsigned)(uintptr_t)e->pKey, e->nW, e->nH,
				!bHadKey ? (g_bKeyBlack ? "implicit BLACK" : "none")
					: (g_bBlitKey ? "the BLIT's key" : "the SURFACE's key"),
				p[4], p[5], p[6]);
		}
		p[8] = g_fKeySoft;
		p[9] = p[10] = p[11] = 0.0f;
		if (g_pRTV == g_pOvlRTV && g_pOvlRTV)
			p[9] = (float)g_n2DBlendMode;		// the overlay: see the shader
		// the MASK text's brightness in the overlay: 0 black as the mode meant,
		// 1 white - over a dimmed world, white reads. The tester asked for a colour.
		p[10] = g_fPauseTextWhite;
		g_pCtx->Unmap(g_pCB, 0);
	}

	g_pCtx->RSSetViewports(1, &vp);
	g_pCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);

	const UINT nStride = sizeof(Vert), nOffset = 0;
	g_pCtx->IASetInputLayout(g_pLayout);
	g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStride, &nOffset);
	g_pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	g_pCtx->VSSetShader(g_pVS, nullptr, 0);
	g_pCtx->PSSetShader(g_pPS, nullptr, 0);
	g_pCtx->PSSetConstantBuffers(0, 1, &g_pCB);
	ID3D11ShaderResourceView* pSrvUse = SrvOf(e);
	g_pCtx->PSSetShaderResources(0, 1, &pSrvUse);
	g_pCtx->PSSetSamplers(0, 1, &g_pSampler);

	const float kBlendFactor[4] = { 0, 0, 0, 0 };
	ID3D11BlendState* pBS = (g_n2DBlendMode < 7 && g_pBlendMode[g_n2DBlendMode])
						  ? g_pBlendMode[g_n2DBlendMode] : g_pBlend;
	// A SNAPSHOT IS COPIED, NOT BLENDED. The eye copy used to go through the
	// default ALPHA blend, and the world writes its texture alpha into the
	// back buffer - hair strands, glass, the shiny parts of a skin - so those
	// pixels came out darkened over black in the copied eye alone. In the
	// headset Cate's hair had a shimmer that closing one eye revealed, and the
	// security camera was see-through in one eye and flat matte in the other.
	if (e->bSnapValid && g_pBlendMode[1]) pBS = g_pBlendMode[1];
	// Into the overlay everything composites OVER a transparent target and
	// accumulates coverage in alpha; the shader turned the mode into
	// colour-plus-coverage.
	if (g_pRTV == g_pOvlRTV && g_pOvlRTV && g_pBlendOver) pBS = g_pBlendOver;
	g_pCtx->OMSetBlendState(pBS, kBlendFactor, 0xFFFFFFFF);
	g_pCtx->Draw(4, 0);
	++g_nDraws;
}

void R2D_Blit(const void* pKey,
			  const void* pPixels, int nPitch, int nSrcW, int nSrcH,
			  int sx0, int sy0, int sx1, int sy1,
			  int dx0, int dy0, int dx1, int dy1,
			  float fAlpha)
{
	if (!g_pCtx || !g_pRTV || !g_pVS || !g_nTargetW || !g_nTargetH) return;
	if (nSrcW <= 0 || nSrcH <= 0 || dx1 <= dx0 || dy1 <= dy0) return;

	TexEntry* e = Upload(pKey, pPixels, nPitch, nSrcW, nSrcH);
	if (!e || !SrvOf(e)) return;	// a snapshot-backed entry has no dynamic texture

	// Destination pixels to normalised device coordinates. The back buffer is
	// the engine's mode size, so these are the engine's own coordinates.
	const float x0 = (float)dx0 / g_nTargetW * 2.0f - 1.0f;
	const float x1 = (float)dx1 / g_nTargetW * 2.0f - 1.0f;
	const float y0 = 1.0f - (float)dy0 / g_nTargetH * 2.0f;
	const float y1 = 1.0f - (float)dy1 / g_nTargetH * 2.0f;

	const float u0 = (float)sx0 / nSrcW, u1 = (float)sx1 / nSrcW;
	const float v0 = (float)sy0 / nSrcH, v1 = (float)sy1 / nSrcH;

	const D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)g_nTargetW, (float)g_nTargetH, 0.0f, 1.0f };
	DrawQuad(e, vp, x0, y0, x1, y1, u0, v0, u1, v1, fAlpha);
}

// ---------------------------------------------------------------------------
// The same blit, into BOTH eyes.
//
// The engine lays its 2D out for one screen, and in VR that screen is the
// side-by-side pair - so the health bar lands in the left eye only, the ammo
// count in the right eye only, and the crosshair straddles the seam. In a
// headset that is not a HUD at all.
//
// This is the choke point every 2D primitive already passes through, which is
// the whole reason the fix lives here: the alternative is threading an offset
// through every DrawSurfaceToSurface call site in the client, where missing one
// reads as mostly fine rather than as a bug.
//
// The layout is mapped into each eye WHOLE and with its aspect kept: the frame
// is twice as wide as one eye, so the fit scale is 0.5, and the 2:1 layout ends
// up spanning the eye's full width and the middle half of its height.
//
// It is centred on the FORWARD RAY, not on the middle of the half. With an
// asymmetric per-eye frustum those are about 15 degrees apart, which is the
// finding the per-eye crosshair already had to be moved for (commit c44d49e).
// R2D_SetEyeCentre is fed from the frustum the world was drawn with.
// ---------------------------------------------------------------------------
void R2D_BlitStereo(const void* pKey,
					const void* pPixels, int nPitch, int nSrcW, int nSrcH,
					int sx0, int sy0, int sx1, int sy1,
					int dx0, int dy0, int dx1, int dy1,
					float fAlpha, int bCover)
{
	if (!g_pCtx || !g_pRTV || !g_pVS || !g_nTargetW || !g_nTargetH) return;
	if (nSrcW <= 0 || nSrcH <= 0 || dx1 <= dx0 || dy1 <= dy0) return;

	TexEntry* e = Upload(pKey, pPixels, nPitch, nSrcW, nSrcH);
	if (!e || !SrvOf(e)) return;	// a snapshot-backed entry has no dynamic texture

	const int   nEyeW = g_nTargetW / 2;
	const float fLx0 = (float)dx0 / g_nTargetW * 2.0f - 1.0f;
	const float fLx1 = (float)dx1 / g_nTargetW * 2.0f - 1.0f;
	const float fLy0 = 1.0f - (float)dy0 / g_nTargetH * 2.0f;
	const float fLy1 = 1.0f - (float)dy1 / g_nTargetH * 2.0f;

	const float u0 = (float)sx0 / nSrcW, u1 = (float)sx1 / nSrcW;
	const float v0 = (float)sy0 / nSrcH, v1 = (float)sy1 / nSrcH;

	// Fit the whole layout inside one eye, keeping its aspect, AROUND THE
	// FORWARD RAY - which is not the middle of the eye.
	//
	// The first version fitted to the eye's full width and then shifted the
	// result onto the forward ray, and the shift pushed one side of the layout
	// outside the viewport, where it was clipped away. The desk caught it: the
	// ammo counter drew in the right eye and was simply absent from the left,
	// which reads like the second draw never happening rather than like a
	// scale being wrong by 25%.
	//
	// So the offset is part of the fit. With the layout centred at cx, the
	// room either side is 1-|cx|, and the scale is whichever axis runs out
	// first. Both eyes must use the SAME scale or the two images differ in
	// size and cannot fuse, so the tighter of the two centres decides it.
	const float fWl = (float)g_nTargetW, fHl = (float)g_nTargetH;
	const float fWv = (float)(nEyeW ? nEyeW : 1), fHv = (float)g_nTargetH;
	float fRoomX = 1.0f, fRoomY = 1.0f;
	for (int i = 0; i < 2; ++i)
	{
		const float rx = 1.0f - fabsf(g_fEyeCentreX[i]);
		const float ry = 1.0f - fabsf(g_fEyeCentreY[i]);
		if (rx < fRoomX) fRoomX = rx;
		if (ry < fRoomY) fRoomY = ry;
	}
	if (fRoomX < 0.05f) fRoomX = 0.05f;
	if (fRoomY < 0.05f) fRoomY = 0.05f;

	float fKx = fRoomX * fWv / fWl;		// scale allowed by width
	float fKy = fRoomY * fHv / fHl;		// scale allowed by height
	// In a menu the panel is the centred band, the layout is centred on the
	// eye (cx = cy = 0 below), and the band's height is the limit.
	const bool bBand = (g_bMenu && g_fMenuBand > 0.0f);
	if (bBand)
	{
		const float fHb = (fWv / g_fMenuBand < fHv) ? fWv / g_fMenuBand : fHv;
		fKx = fWv / fWl;
		fKy = fHb / fHl;
		// A NEAR-WHOLE-SCREEN BLIT - the splash - IS A PICTURE, NOT A LAYOUT.
		// The band fit above scales width and height by different amounts
		// (the layout's core zoom makes that come out right for the menu);
		// on the splash it is a stretch, and at 3:2 it read as cropped and
		// zoomed (reported twice, 13 September). One uniform scale, the
		// smaller of the two, so the whole picture sits inside the band.
		if ((float)(dx1 - dx0) >= fWl * 0.9f && (float)(dy1 - dy0) >= fHl * 0.9f)
		{
			const float k = (fKx < fKy) ? fKx : fKy;
			fKx = k; fKy = k;
		}
	}
	// A MENU DOES NOT USE THE WHOLE LAYOUT, AND FITTING THE WHOLE LAYOUT
	// THEREFORE MAKES IT SMALLER THAN IT NEEDS TO BE.
	//
	// NOLF's interface is authored 4:3. On a 2560x1384 screen the engine
	// draws it in the centred 1846-wide 4:3 region and covers the 357 px
	// either side with two filler bars whose whole job is to hide the part of
	// the screen the menu does not reach - measured in a retail capture, the
	// left bar is x 0..356 and the picture runs 357..2202. Fitting all 2560
	// into the eye therefore spends 28% of the eye's width on those bars, and
	// shrinks the menu by the same 1.387 - which is the reported complaint that the
	// menu is too small to read, arriving through the 2D layer rather than
	// through the panel size.
	//
	// So in a menu the 4:3 CORE spans the eye and the bars run off the edges,
	// where they cost nothing: there is nothing behind them to hide, because
	// the 3D interface scene is fitted to the same core (InterfaceMgr.cpp).
	// +StubMenuScale100 overrides the multiplier; 100 is the old behaviour.
	//
	// NOT for a blit that already covers the whole screen. A loading screen
	// is a full-screen image and is NOT a "cover" blit by the test above -
	// that one only catches a tiny surface stretched over everything - so
	// without this it would be scaled up like the menu and lose 28% of its
	// width. The menu's own pieces are all sub-rectangles of the screen; the
	// filler bars are full HEIGHT but not full width, which is why the test
	// is both axes and not either.
	const bool bWholeScreen =
		((float)(dx1 - dx0) >= fWl * 0.98f && (float)(dy1 - dy0) >= fHl * 0.98f);
	// THE FILLER BARS ARE NOT ART, AND IN A FITTED MENU THEY ARE IN THE WAY.
	//
	// The note above says the 4:3 core spans the eye and the bars "run off the
	// edges, where they cost nothing". That holds when the eye centre is zero,
	// as it is at the main menu. Over a LIVE WORLD it is not: the fit is
	// limited to the room left after centring - 1-|cx|, about 0.59 here - so
	// everything shrinks and the bars come back INSIDE the eye, one at each
	// edge, in a different place in each eye because the centres differ.
	// That is the olive band seen in the periphery in headset testing and
	// reported as double vision; it is two 357x1384 strips of nothing.
	//
	// They exist to hide the part of a 4:3 screen the menu does not reach, and
	// in a headset there is nothing behind them to hide. Full HEIGHT, not full
	// width, entirely outside the centred 4:3 core - which is exactly how the
	// note above characterises them - so that is the test.
	if ((g_bMenu || g_bMenuFit) && !bWholeScreen && fHl > 0.0f)
	{
		const float fCoreW = fHl * 4.0f / 3.0f;
		if (fCoreW < fWl)
		{
			const float fBarL = (fWl - fCoreW) * 0.5f;
			const float fBarR = fWl - fBarL;
			const bool  bFullH = ((float)(dy1 - dy0) >= fHl * 0.98f);
			if (bFullH && ((float)dx1 <= fBarL + 1.0f
						|| (float)dx0 >= fBarR - 1.0f))
			{
				++g_nBarsSkipped;
				if (g_bTrace2D && g_pfnLog)
				{
					static long s_nSaidB = 0;
					if (s_nSaidB++ < 12)
						g_pfnLog("  T2D BAR skipped: dst %d,%d..%d,%d src %dx%d", dx0, dy0, dx1, dy1, nSrcW, nSrcH);
				}
				return;
			}
			// AND THE BARS BESIDE THE SPLASH, which the client draws against
			// the splash's own 16:9 edges rather than the 4:3 core's: a
			// full-height blit under a quarter of the width, touching the
			// left or right edge of the logical screen (13 September evening
			// headset test: lime strips either side of the splash).
			if (bFullH && (float)(dx1 - dx0) < fWl * 0.25f
				&& (dx0 <= 1 || (float)dx1 >= fWl - 1.0f))
			{
				++g_nBarsSkipped;
				return;
			}
			// WHAT THE BAR RULES SKIP, once per distinct rectangle, so a text
			// block mistaken for a bar can be seen (13 September: the briefing
			// paragraph vanished after the splash-bar rule).
			if (g_bTrace2D && g_pfnLog && bFullH)
			{
				static long s_nSaid = 0;
				if (s_nSaid++ < 12)
					g_pfnLog("  T2D FULL-HEIGHT blit kept: dst %d,%d..%d,%d src %dx%d (bars %.0f..%.0f)",
							 dx0, dy0, dx1, dy1, nSrcW, nSrcH, fBarL, fBarR);
			}
		}
	}

	// A NEAR-WHOLE-SCREEN BLIT keeps the plain band fit: the splash is fitted
	// to the whole logical screen at 16:9 (0.96 of its width) and is meant to
	// fill the band, not to be a piece of the 4:3 layout.
	const bool bNearWhole =
		((float)(dx1 - dx0) >= fWl * 0.9f && (float)(dy1 - dy0) >= fHl * 0.9f);
	float fMenu = 1.0f;
	if ((g_bMenu || g_bMenuFit) && !bWholeScreen && !bNearWhole)
	{
		// THE AUTO-FIT FIRST, THEN THE LEVER ON TOP OF IT.
		//
		// +StubMenuScale100 used to REPLACE this, which made it useless as a
		// lever: the fit is already about 1.387 here, so asking for 150 was
		// asking for 1.50 - eight percent - and every attempt to zoom the menu
		// with it looked like it had done nothing. A multiplier means 150
		// is half again as big as whatever the fit worked out, on any
		// resolution, which is what a caller asking for "150%" means.
		// The core zoom fills the eye's width with the 4:3 core; in the band
		// the height is what fits, and the backdrop is meant to show beside
		// the card, so the zoom stays off there.
		// ...AND IN THE BAND TOO. 13 September: the interface camera keeps
		// its authored 90 degrees across the EYE'S width whatever the panel
		// crops, so the 3D scene's 4:3 core is the eye wide (1920 px at the tested
		// resolution) while the band fit alone put the logical 4:3 core at
		// 1384 - the menu text sat right of centre in its lime box and the
		// help line above its blue one. The core zoom is that ratio.
		if (fHl > 0.0f)
		{
			const float fCore = fWl / (fHl * 4.0f / 3.0f);
			if (fCore > 1.0f) fMenu = fCore;
		}
		if (g_fMenuScale > 0.0f) fMenu *= g_fMenuScale;
		// ...times what the 3D scene's 90 degrees actually spans (see
		// g_fMenuZoomExtra); 0.84 in the Quest 3's band.
		if (bBand && g_fMenuZoomExtra > 0.0f) fMenu *= g_fMenuZoomExtra;

		// THE PAUSE MENU IS NOT THE MAIN MENU, AND THE ZOOM ABOVE IS FOR THE
		// MAIN MENU. Over a live world the layout is centred on the forward
		// ray, which sits 0.24 of the eye off-centre with the native per-eye
		// frustum - and the core zoom then fits the 4:3 layout to the whole
		// eye anyway, so it runs off the far edge and its LEFT-ALIGNED text
		// column lands a third of the way across. Desk-measured at 3840x2076:
		// "Resume game" at 31% of the eye. In the headset the pause menu was
		// still far off to the left instead of at the centre of the screen.
		//
		// So over a world the zoom comes off and a smaller scale goes on: a
		// menu in front of your face wants to be a panel you can read at a
		// glance, not a wall. +StubPauseScale100 is the lever.
		if (!g_bMenu && g_fPauseScale > 0.0f)
			fMenu = g_fPauseScale;
	}
	const float fK  = g_fStereoScale * fMenu * ((fKx < fKy) ? fKx : fKy);
	const float fSx = fK * fWl / fWv;
	const float fSy = fK * fHl / fHv;

	// PAUSE-MENU ART GOES TO THE OVERLAY, not the eyes. Same fit as the eye
	// path but no eye centre, no sideways shift and no head-lock offset: the
	// picture is a flat panel and the HOST decides where it sits in the
	// world. DrawQuad binds g_pRTV itself, so the target is swapped around it.
	{
		const bool bPauseArtAll = (g_bMenuFit && !g_bMenu && !bWholeScreen
								   && g_fPauseScale > 0.0f && !bCover);
		// ONLY OVER A LOADED WORLD, AND ONLY WITH A LIVE HOST. The main menu is
		// menu-fit too, and when the client is not talking to the block (a
		// version mismatch at the desk did it) g_bMenu is false and every
		// main-menu blit went to the overlay - an empty green card, no text,
		// no title. The pause menu is the one menu drawn over a world, and a
		// quad needs a host to present it.
		if (bPauseArtAll && g_bPauseQuad && R3D_HaveWorld() && VRB_Live() && EnsureOverlay())
		{
			if (!g_bOvlCleared)
			{
				const float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				g_pCtx->ClearRenderTargetView(g_pOvlRTV, c);
				g_bOvlCleared = 1;
			}
			if (g_bOvlTrace && g_nOvlUpRun == 29 && g_nOvlTracedPauses < 8 && g_pfnLog)
				g_pfnLog("   OVL pause %d blit: surface %08X %dx%d src %d,%d..%d,%d dst %d,%d..%d,%d "
						 "-> overlay ndc %.2f,%.2f..%.2f,%.2f  mode %d alpha %.2f snap %d",
						 g_nOvlTracedPauses + 1, (unsigned)(uintptr_t)pKey, nSrcW, nSrcH,
						 sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1,
						 fLx0 * fSx, fLy0 * fSy, fLx1 * fSx, fLy1 * fSy,
						 g_n2DBlendMode, fAlpha, e->bSnapValid ? 1 : 0);
			ID3D11RenderTargetView* pEye = g_pRTV;
			g_pRTV = g_pOvlRTV;
			const D3D11_VIEWPORT vpo = { 0.0f, 0.0f, (float)g_nOvlW, (float)g_nOvlH, 0.0f, 1.0f };
			DrawQuad(e, vpo, fLx0 * fSx, fLy0 * fSy, fLx1 * fSx, fLy1 * fSy,
					 u0, v0, u1, v1, fAlpha);
			g_pRTV = pEye;
			g_pCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
			g_bOvlDrawn = 1; ++g_nOvlBlits;
			return;
		}
	}

	for (int nEye = 0; nEye < 2; ++nEye)
	{
		const D3D11_VIEWPORT vp = { (float)(nEye * nEyeW), 0.0f,
									(float)nEyeW, (float)g_nTargetH, 0.0f, 1.0f };
		// A COVER blit means it: a fade or a tint is a tiny surface stretched
		// over the whole screen to hide everything behind it, and hiding 76%
		// of the eye is not a fade, it is a rectangle. Those fill the eye
		// instead of being fitted into it. Everything else - the HUD, the
		// menu, the loading screen - keeps its layout and its aspect.
		if (bCover)
		{
			DrawQuad(e, vp, -1.0f, 1.0f, 1.0f, -1.0f, u0, v0, u1, v1, fAlpha);
			continue;
		}
		// IN A MENU THE TWO HALVES MUST MATCH. The host presents a menu as a
		// world-locked flat panel, and a flat panel is one picture seen from
		// two eyes - so the per-eye forward-ray offset, which is exactly right
		// for a HUD living in the eye buffer, would put the two copies at a
		// horizontal disparity and float the panel at the wrong depth.
		//
		// It does not show at the main menu, where no world has been drawn and
		// both centres are still zero: the halves there come out pixel
		// identical, measured. It would show on the PAUSE menu, which inherits
		// the centres of the world drawn a moment earlier.
		const float cx = g_bMenu ? 0.0f : g_fEyeCentreX[nEye];
		const float cy = g_bMenu ? 0.0f : g_fEyeCentreY[nEye];
		// WHERE THIS PRIMITIVE ACTUALLY LANDS, and what it is multiplied by.
		// "the surface holds the title screen and the screen is black" cannot
		// be taken further by looking at either end of the pipe; the answer is
		// in the quad and the tint, and neither was ever printed.
		// COMPUTED BEFORE THE DUMPS READ IT, deliberately. The dumps below
		// used to print `cx + fLx0*fSx` inline while DrawQuad was handed the
		// ZOOMED value - an instrument sitting below the substitution it is
		// meant to be watching, which is the same shape as the blend-mode log
		// that reported our own clamp back to us for weeks. One expression,
		// computed once, read by the dump and by the draw.
		// THE ONE PLACE EVERY 2D PRIMITIVE'S NDC IS DECIDED, which is why the
		// zoom is applied here and not at sixty call sites. A cover blit is
		// exempt for the same reason it is exempt from the fit: it is a fade,
		// and a fade that does not cover the eye is not a fade.
		// THE PAUSE MENU'S TEXT COLUMN, ONTO THE RAY. NOLF's menus are
		// left-aligned inside their 4:3 core: the item list sits at about
		// 0.29 of the core's width, which is -0.30 in the layout's NDC. With
		// the layout centred on the forward ray that column lands a hand's
		// width to the left of straight ahead, in both eyes, which fuse into
		// a pause menu off to the left. Shifting the whole layout by
		// that much puts the column where you are looking. Pause only: the
		// main menu is presented as a quad and centres itself.
		// MENU ART ONLY. The first version of this applied to every 2D blit
		// outside the main menu, which is the HUD during play as well - and
		// slid the ammo counter sideways. g_bMenuFit is the folder-art flag.
		const bool bPauseArt = (g_bMenuFit && !g_bMenu && !bWholeScreen
								&& g_fPauseScale > 0.0f && !bCover);
		const float fShift = bPauseArt ? g_fPauseShiftX * fSx : 0.0f;
		// THE PAUSE MENU STAYS WHERE IT WAS. Headset testing wanted the pause
		// menu static, not following gaze or head movement. The 2D layer
		// lives in the eye buffer and so turns with the head; this cancels
		// that turn for menu art by the head's yaw and pitch since the menu
		// opened, projected through the eye's own frustum. Small-angle: a
		// panel is a flat thing in front of the face, and past about 40
		// degrees it leaves the eye anyway, which is what a fixed panel does.
		const float fLockX = bPauseArt ? g_fLockDX[nEye] : 0.0f;
		const float fLockY = bPauseArt ? g_fLockDY[nEye] : 0.0f;
		// Not for the menu (flat by design) nor the pause art (its own quad).
		const float fConv = (g_bMenu || bPauseArt) ? 0.0f : ConvergeNdc(nEye);
		float qx0 = cx + fShift + fLockX + fConv + fLx0 * fSx, qy0 = cy + fLockY + fLy0 * fSy;
		float qx1 = cx + fShift + fLockX + fConv + fLx1 * fSx, qy1 = cy + fLockY + fLy1 * fSy;
		// g_bMenu, NOT g_bMenuFit, AND THE DIFFERENCE IS THE WHOLE SAFETY
		// ARGUMENT. g_bMenuFit is true for any folder's art wherever it is
		// shown, INCLUDING the pause menu over a live world. The 3D half of
		// this zoom is a change to the frustum the scene is drawn with, and
		// the pause menu's scene contains the WORLD - so zooming on the fit
		// flag would magnify the level behind the pause menu, which is a far
		// worse bug than the one being fixed.
		//
		// g_bMenu is the host-presents-a-quad case: the main menu and the
		// folder tree, where no world is drawn and both eye centres are zero.
		// That is the case the headset complaint was about (the menu too wide)
		// and it is the one where a zoom is unambiguously safe. The pause menu
		// keeps its current size until that can be judged in a headset.
		if (g_bMenu && !bCover && !bNearWhole && g_fMenuZoom > 0.0f)	// the splash keeps its fit
		{
			qx0 = (qx0 - g_fMenuAnchorX) * g_fMenuZoom;
			qx1 = (qx1 - g_fMenuAnchorX) * g_fMenuZoom;
			qy0 = (qy0 - g_fMenuAnchorY) * g_fMenuZoom;
			qy1 = (qy1 - g_fMenuAnchorY) * g_fMenuZoom;

			// SAID ON FIRST USE, not when the switch was parsed. A line
			// printed at startup says the value was read; this one says the
			// transform actually reached a quad, which is the thing a switch
			// that changes nothing fails to do.
			//
			// And whether it pulls the art off an edge. A zoom about an
			// off-centre anchor moves the menu's own backdrop with everything
			// else, so where the source's edge lands INSIDE the eye there is
			// nothing drawn - a black band, in a headset, that nobody was
			// warned about. The source edge at ndc -1 maps to
			// (-1 - ax) * zoom, so the gap closes when ax >= 1/zoom - 1. Only
			// the LEFT edge can constrain it, because this game's menu card is
			// left of centre and the anchor follows it.
			//
			// REPORTED, NOT CLAMPED: silently moving the anchor would answer a
			// different question from the one asked, and the band is a fair
			// choice - it crops a panel that is too wide, which is the
			// complaint being fixed.
			static int s_bSaidZoom = 0;
			if (!s_bSaidZoom && g_pfnLog
				&& (g_fMenuZoom != 1.0f || g_fMenuAnchorX != 0.0f))
			{
				s_bSaidZoom = 1;
				const float fEdge = (-1.0f - g_fMenuAnchorX) * g_fMenuZoom;
				g_pfnLog("  R2D MENU ZOOM: applied - %.2fx about ndc"
						 " (%.3f, %.3f)%s", g_fMenuZoom, g_fMenuAnchorX,
						 g_fMenuAnchorY,
						 (fEdge > -1.0f)
							? "   <- LEAVES A GAP at the left edge; anchor x"
							  " must be >= 1/zoom - 1 to close it" : "");
			}
		}
		if (nEye == 0) ++g_nBlitSeen;
		if (g_bDump2D && nEye == 0)
		{
			int nS = -1;
			for (int s = 0; s < g_nSeen; ++s)
				if (g_Seen[s].p == pKey && g_Seen[s].nW == nSrcW
					&& g_Seen[s].nH == nSrcH) { nS = s; break; }
			if (nS < 0 && g_nSeen < 64)
			{
				nS = g_nSeen++;
				g_Seen[nS].p = pKey; g_Seen[nS].nW = nSrcW;
				g_Seen[nS].nH = nSrcH; g_Seen[nS].nBlits = 0;
				g_Seen[nS].nLast = 0;
				if (g_pfnLog)
					g_pfnLog("  2D SURFACE FIRST SEEN @%ld: %08X %dx%d"
						" -> NDC %.3f,%.3f .. %.3f,%.3f  alpha %.2f"
						"  tint %.2f %.2f %.2f  blend %d  cover %d",
						g_nBlitSeen, (unsigned)(uintptr_t)pKey, nSrcW, nSrcH,
						qx0, qy0, qx1, qy1, fAlpha,
						g_f2DColour[0], g_f2DColour[1], g_f2DColour[2],
						g_n2DBlendMode, bCover);
			}
			if (nS >= 0) { ++g_Seen[nS].nBlits;
				  g_Seen[nS].nLast = g_nBlitSeen; }
		}
		// A burst of six every 4000 blits: enough to see one frame's
		// whole list, spread across the run rather than piled at its
		// start. 4000 is about thirteen bursts over a menu capture.
		if (g_bDump2D && nEye == 0 && (g_nBlitSeen % 4000) < 6)
		{
			++g_nDumpSaid;
			if (g_pfnLog)
				g_pfnLog("  2D QUAD @%ld: %08X %dx%d -> NDC %.3f,%.3f .. %.3f,%.3f"
					"  uv %.2f,%.2f..%.2f,%.2f  alpha %.2f  tint %.2f %.2f %.2f"
					"  blend %d (asked %u)  cover %d",
					g_nBlitSeen, (unsigned)(uintptr_t)pKey, nSrcW, nSrcH,
					qx0, qy0, qx1, qy1,
					u0, v0, u1, v1, fAlpha,
					g_f2DColour[0], g_f2DColour[1], g_f2DColour[2],
					g_n2DBlendMode, g_n2DBlendRaw, bCover);
		}
		DrawQuad(e, vp, qx0, qy0, qx1, qy1, u0, v0, u1, v1, fAlpha);
	}
}

void R2D_SetStereoScale(float f) { g_fStereoScale = (f > 0.0f) ? f : 1.0f; }
void R2D_MapWaitLine(char* out, int n)
{
	sprintf_s(out, (size_t)n, "2D vb %ld/%ld worst %.1f, cb %ld/%ld worst %.1f",
		g_nMap2DCalls[0], g_nMap2DSlow[0], g_fMap2DWorst[0],
		g_nMap2DCalls[1], g_nMap2DSlow[1], g_fMap2DWorst[1]);
	for (int i = 0; i < 2; ++i) { g_nMap2DCalls[i] = g_nMap2DSlow[i] = 0; g_fMap2DMs[i] = g_fMap2DWorst[i] = 0.0; }
}
void R2D_SetHudDepthCm(float f) { g_fHudDepthCm = f; }
void R2D_SetMenuScale(float f) { g_fMenuScale = (f > 0.0f) ? f : 0.0f; }
void R2D_SetPauseScale(float f) { g_fPauseScale = (f > 0.0f) ? f : 0.0f; }
void R2D_SetPauseShift(float f) { g_fPauseShiftX = f; }
void R2D_SetWorldLock(int nHalf, float fDX, float fDY)
{
	if (nHalf < 0 || nHalf > 1) return;
	g_fLockDX[nHalf] = fDX; g_fLockDY[nHalf] = fDY;
}

// WORLD-LOCK THE PAUSE MENU. Called by the renderer for every interface scene
// drawn over a loaded world, with that eye's frustum tans (left, right, up,
// down), and with bPause false from every world scene, which resets it.
//
// The first version of this lived in dllmain at the eye-centre site and never
// ran: the interface scene does not arrive there with a native-frustum pTan -
// its frustum is the REMEMBERED world one, held inside R3D_DrawWorld. In the
// headset the pause menu still followed the head. So it is called from there.
//
// Records the head's yaw and pitch the moment the menu opens and cancels the
// movement since, in NDC, as tan(d) over half the eye's tangent span. Turning
// the head right moves the world left in the eye, so the panel moves left
// with it. Small-angle by design: a panel is a flat thing in front of the
// face, and past about forty degrees it leaves the eye, as a fixed panel does.
void R2D_UpdateWorldLock(int nHalf, const float* pTan4, int bPause)
{
	static bool  s_bLocked = false;
	static float s_fYaw0 = 0.0f, s_fPitch0 = 0.0f;
	static long  s_nSaid = 0;
	if (!bPause)
	{
		if (s_bLocked && g_pfnLog && s_nSaid < 4)
		{ ++s_nSaid; g_pfnLog("  R2D: pause menu world-lock released"); }
		s_bLocked = false;
		g_fLockDX[0] = g_fLockDX[1] = g_fLockDY[0] = g_fLockDY[1] = 0.0f;
		return;
	}
	float fYaw = 0.0f, fPitch = 0.0f;
	if (nHalf < 0 || nHalf > 1 || !pTan4 || !VRB_HeadYawPitch(&fYaw, &fPitch))
		return;
	const float tl = pTan4[0], tr = pTan4[1], tu = pTan4[2], td = pTan4[3];
	if (!(tr > tl && tu > td)) return;
	if (!s_bLocked)
	{
		s_bLocked = true; s_fYaw0 = fYaw; s_fPitch0 = fPitch;
		if (g_pfnLog && s_nSaid < 4)
		{
			++s_nSaid;
			g_pfnLog("  R2D: pause menu world-locked at head yaw %.1f pitch %.1f",
					 fYaw, fPitch);
		}
	}
	const float kD2R = 0.01745329f;
	float dYaw = fYaw - s_fYaw0;
	while (dYaw > 180.0f)  dYaw -= 360.0f;
	while (dYaw < -180.0f) dYaw += 360.0f;
	const float dPitch = fPitch - s_fPitch0;
	g_fLockDX[nHalf] = -tanf(dYaw   * kD2R) * 2.0f / (tr - tl);
	g_fLockDY[nHalf] = -tanf(dPitch * kD2R) * 2.0f / (tu - td);
}
void R2D_SetMenuZoom(float f, float ax, float ay)
{
	g_fMenuZoom = (f > 0.0f) ? f : 1.0f;
	g_fMenuAnchorX = ax;
	g_fMenuAnchorY = ay;
	// Whether it does anything is reported from the DRAW, not here - see the
	// note at the transform. A value being read is not a value being used.
}

void R2D_SetMenu(int b) { g_bMenu = b; }
void R2D_SetMenuBand(float f) { g_fMenuBand = f; }
void R2D_SetMenuZoomExtra(float f) { g_fMenuZoomExtra = (f > 0.0f) ? f : 1.0f; }
void R2D_SetMenuFit(int b) { g_bMenuFit = b; }
void R2D_SetKeySoft(float f) { g_fKeySoft = (f > 0.0f) ? f : 0.0f; }
void R2D_SetDump2D(int b) { g_bDump2D = b; }
void R2D_SetTrace(int b) { g_bTrace2D = b; }
void R2D_SetPauseQuad(int b) { g_bPauseQuad = b; }
void R2D_SetPauseTextWhite(float f) { g_fPauseTextWhite = (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f); }
int  R2D_OverlayEndFrame(ID3D11Texture2D** ppTex, int* pnW, int* pnH)
{
	const int bDrawn = g_bOvlDrawn;
	if (ppTex) *ppTex = bDrawn ? g_pOvlTex : nullptr;
	if (pnW) *pnW = g_nOvlW;
	if (pnH) *pnH = g_nOvlH;
	if (bDrawn) ++g_nOvlFrames;
	g_nOvlUpRun = bDrawn ? g_nOvlUpRun + 1 : 0;
	if (g_bOvlTrace && g_nOvlUpRun == 30 && g_nOvlTracedPauses < 8) ++g_nOvlTracedPauses;
	g_bOvlDrawn = 0; g_bOvlCleared = 0;
	return bDrawn;
}
long R2D_OverlayUpRun() { return g_nOvlUpRun; }
void R2D_SetOverlayTrace(int b) { g_bOvlTrace = b; }

void R2D_SetEyeCentre(int nHalf, float fNdcX, float fNdcY)
{
	if (nHalf < 0 || nHalf > 1) return;
	const bool bChanged = (g_fEyeCentreX[nHalf] != fNdcX
						|| g_fEyeCentreY[nHalf] != fNdcY);
	g_fEyeCentreX[nHalf] = fNdcX;
	g_fEyeCentreY[nHalf] = fNdcY;
	// Said when it changes, at most a handful of times a run. Where straight
	// ahead is in each half decides where the whole 2D layer lands, and a
	// still frame cannot tell a centre that is wrong from a HUD the game
	// happened to draw over there.
	static int s_nSaid = 0;
	if (bChanged && g_pfnLog && s_nSaid < 4)
	{
		++s_nSaid;
		g_pfnLog("  R2D: forward ray in half %d is at ndc (%.3f, %.3f)"
				 " - the 2D layer is centred there, not on the half",
				 nHalf, fNdcX, fNdcY);
	}
}

void R2D_SetAlwaysUpload(int b) { g_bAlwaysUpload = b; }

void R2D_SetBlendModes(int b) { g_bBlendModes = b; g_n2DBlendMode = 0; }
void R2D_SetColourKeyEnabled(int b) { g_bColourKey = b; }
void R2D_SetKeyMask(int b) { g_bKeyMask = b; }
void R2D_SetKeyBlack(int b) { g_bKeyBlack = b; }
void R2D_SetKeySuppress(int b) { g_bKeySuppress = b; }
void R2D_SetBlitKey(unsigned int c, int bHave)
{ g_nBlitKey = c; g_bBlitKey = bHave; }

void R2D_SetTransparent(const void* pSurface, unsigned int nColour)
{
	if (!pSurface) return;
	for (int i = 0; i < g_nKeys; ++i)
		if (g_Keys[i].pKey == pSurface) { g_Keys[i].nColour = nColour; return; }
	if (g_nKeys < 512) { g_Keys[g_nKeys].pKey = pSurface;
						g_Keys[g_nKeys].nColour = nColour; ++g_nKeys; }
	else ++g_nKeysLost;			// reported, never silent

	static int s_nSaid = 0;
	if (s_nSaid++ < 6)
	{
		char sz[112];
		sprintf_s(sz, "  R2D: surface %p optimized with transparent colour %08X"
			" (RGB %u %u %u)", pSurface, nColour,
			(nColour >> 16) & 0xFF, (nColour >> 8) & 0xFF, nColour & 0xFF);
		Log(sz);
	}
}

void R2D_SetColourEnabled(int b)
{
	g_bColour2D = b;
	if (!b) g_f2DColour[0] = g_f2DColour[1] = g_f2DColour[2] = 1.0f;
}

void R2D_SetColour(unsigned int c)
{
	if (!g_bColour2D) return;
	g_f2DColour[0] = (float)((c >> 16) & 0xFF) / 255.0f;
	g_f2DColour[1] = (float)((c >>  8) & 0xFF) / 255.0f;
	g_f2DColour[2] = (float)( c        & 0xFF) / 255.0f;

	// The first few DISTINCT colours the engine asks for, once each. If this
	// ever blacks something out, the log says whether the game asked for
	// black or whether we mis-unpacked it.
	static unsigned int s_seen[8]; static int s_nSeen = 0;
	for (int i = 0; i < s_nSeen; ++i) if (s_seen[i] == c) return;
	if (s_nSeen < 8)
	{
		s_seen[s_nSeen++] = c;
		char sz[96];
		sprintf_s(sz, "  R2D: 2D colour %08X -> RGB %3.0f %3.0f %3.0f", c,
			g_f2DColour[0]*255.0f, g_f2DColour[1]*255.0f, g_f2DColour[2]*255.0f);
		Log(sz);
	}
}

void R2D_SetBlend(unsigned int nMode)
{
	// WHAT THE ENGINE ASKED FOR, BEFORE WE CLAMP IT.
	//
	// The report below was written to catch exactly this - "nothing recorded
	// that the engine had asked for a mode we did not honour" - and then the
	// clamp was added ABOVE it, so for every run since it has been reporting
	// our own substitution back to us. An instrument downstream of the thing
	// it is meant to observe cannot see it.
	g_n2DBlendRaw = nMode;
	{
		static unsigned int s_nRaw = 0;
		if (nMode < 32 && !(s_nRaw & (1u << nMode)))
		{
			s_nRaw |= (1u << nMode);
			static const char* const kRawNames[7] =
				{ "alpha", "solid", "add", "multiply", "multiply2",
				  "mask", "maskadd" };
			char szR[192];
			sprintf_s(szR, "  R2D: engine REQUESTED 2D blend %u (%s)", nMode,
				(nMode < 7) ? kRawNames[nMode] : "UNKNOWN");
			Log(szR);
		}
	}
	if (!g_bBlendModes) { g_n2DBlendMode = 0; return; }
	// MODE 3: SOLID, ADD **AND MASK**. The default, and here is why.
	//
	// MASK is (1 - src) * dest, and NOLF's font sheet is WHITE GLYPHS ON
	// BLACK - so a MASK blit paints the glyph BLACK and leaves the background
	// untouched. That is how this game draws every unselected menu item, and
	// the engine asks for it: 50 of 77 font quads on the main menu request
	// blend 5, and mode 2 was turning all fifty into ALPHA, which draws the
	// sheet's white instead. Retail's unselected items are BLACK; ours were
	// WHITE with what looked like a hard black drop shadow beside them.
	//
	// The shadow was never the defect. Retail draws the same shadow pass, and
	// a black shadow under a black glyph is invisible - it only became visible
	// once the glyph above it turned white. Two sessions were spent on colour
	// keys and a soft-key shader chasing that shadow as if it were a fringe.
	//
	// Mode 2 exists because honouring ALL SEVEN (mode 1) drew one menu item
	// out of five, and mode 2 was the retreat from that. But the note it left
	// behind - "(1-src)*dest against a black destination is the destination,
	// so skipping MASK is equivalent" - is only true where the destination IS
	// black. The main menu's destination is an olive card.
	if (g_bBlendModes == 2 && nMode != 1 && nMode != 2) nMode = 0;
	if (g_bBlendModes == 3 && nMode != 1 && nMode != 2 && nMode != 5) nMode = 0;
	if (nMode == g_n2DBlendMode) return;
	g_n2DBlendMode = nMode;

	// Say each mode the first time we ACT on it.
	static unsigned int s_nSeen = 0;
	if (nMode < 32 && !(s_nSeen & (1u << nMode)))
	{
		s_nSeen |= (1u << nMode);
		static const char* const kNames[7] =
			{ "alpha", "solid", "add", "multiply", "multiply2", "mask", "maskadd" };
		char szB[192];
		sprintf_s(szB, "  R2D: engine asked for 2D blend %u (%s)%s", nMode,
			(nMode < 7) ? kNames[nMode] : "UNKNOWN",
			(nMode < 7 && g_pBlendMode[nMode]) ? "" : "  <- NOT HONOURED, drawing alpha");
		Log(szB);
	}
}
