#include "screenlock.h"

namespace
{
	SL_LogFn Log = nullptr;

	ID3D11Device*        g_pDev  = nullptr;
	ID3D11DeviceContext* g_pCtx  = nullptr;
	IDXGISwapChain*      g_pSwap = nullptr;

	ID3D11Texture2D* g_pStage = nullptr;
	UINT g_nW = 0, g_nH = 0;

	bool g_bLocked = false;
	D3D11_MAPPED_SUBRESOURCE g_Map{};

	long   g_nLocks = 0, g_nFailed = 0;
	double g_fMillis = 0.0;
	LARGE_INTEGER g_Freq{}, g_Start{};

	// The back buffer, fetched per lock rather than cached. A cached texture
	// survives a swap-chain resize as a stale pointer, and the failure that
	// produces - reading a buffer nobody is presenting any more - looks like a
	// renderer bug rather than a lifetime bug.
	ID3D11Texture2D* BackBuffer()
	{
		if (!g_pSwap) return nullptr;
		ID3D11Texture2D* pBack = nullptr;
		if (FAILED(g_pSwap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBack)))
			return nullptr;
		return pBack;
	}

	bool EnsureStage(ID3D11Texture2D* pBack)
	{
		D3D11_TEXTURE2D_DESC bd{};
		pBack->GetDesc(&bd);
		if (g_pStage && bd.Width == g_nW && bd.Height == g_nH) return true;

		if (g_pStage) { g_pStage->Release(); g_pStage = nullptr; }
		D3D11_TEXTURE2D_DESC sd = bd;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.MiscFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
		const HRESULT hr = g_pDev->CreateTexture2D(&sd, nullptr, &g_pStage);
		g_nW = bd.Width; g_nH = bd.Height;
		if (Log) Log("  SL: staging %ux%u fmt %d hr=%08X",
					 bd.Width, bd.Height, (int)bd.Format, (unsigned)hr);
		return g_pStage != nullptr;
	}
}

bool SL_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
			   IDXGISwapChain* pSwap, SL_LogFn pfnLog)
{
	Log = pfnLog; g_pDev = pDev; g_pCtx = pCtx; g_pSwap = pSwap;
	QueryPerformanceFrequency(&g_Freq);
	if (Log) Log("  SL: screen lock %s", (pDev && pCtx && pSwap) ? "ready" : "NOT ready");
	return pDev && pCtx && pSwap;
}

void SL_Destroy()
{
	if (g_bLocked && g_pCtx && g_pStage) { g_pCtx->Unmap(g_pStage, 0); g_bLocked = false; }
	if (g_pStage) { g_pStage->Release(); g_pStage = nullptr; }
	g_nW = g_nH = 0;
}

void SL_Invalidate()
{
	if (g_bLocked && g_pCtx && g_pStage) { g_pCtx->Unmap(g_pStage, 0); g_bLocked = false; }
	if (g_pStage) { g_pStage->Release(); g_pStage = nullptr; }
	g_nW = g_nH = 0;
}

int SL_Lock(int nLeft, int nTop, int nRight, int nBottom,
			void** ppData, int* pPitch)
{
	if (ppData) *ppData = nullptr;
	if (pPitch) *pPitch = 0;

	// The real one refuses a second lock and so does this. A caller that has
	// lost track of its own lock is better off seeing a failure than a second
	// mapping of the same memory.
	if (g_bLocked) { ++g_nFailed; return 0; }
	if (!g_pDev || !g_pCtx || !g_pSwap) { ++g_nFailed; return 0; }

	ID3D11Texture2D* pBack = BackBuffer();
	if (!pBack) { ++g_nFailed; return 0; }
	if (!EnsureStage(pBack)) { pBack->Release(); ++g_nFailed; return 0; }

	QueryPerformanceCounter(&g_Start);
	g_pCtx->CopyResource(g_pStage, pBack);
	pBack->Release();

	const HRESULT hr = g_pCtx->Map(g_pStage, 0, D3D11_MAP_READ_WRITE, 0, &g_Map);
	if (FAILED(hr))
	{
		if (g_nFailed < 3 && Log) Log("  SL: Map failed hr=%08X", (unsigned)hr);
		++g_nFailed;
		return 0;
	}
	g_bLocked = true;

	// Clamp rather than trust. The rectangle comes from the engine and a lock
	// that runs off the end of the staging texture would corrupt memory in a
	// way that surfaces somewhere else entirely.
	if (nLeft < 0) nLeft = 0;
	if (nTop  < 0) nTop  = 0;
	if (nLeft > (int)g_nW) nLeft = (int)g_nW;
	if (nTop  > (int)g_nH) nTop  = (int)g_nH;
	(void)nRight; (void)nBottom;

	if (ppData) *ppData = (unsigned char*)g_Map.pData
						+ (size_t)nTop * g_Map.RowPitch + (size_t)nLeft * 4;
	if (pPitch) *pPitch = (int)g_Map.RowPitch;
	++g_nLocks;
	return 1;
}

void SL_Unlock()
{
	if (!g_bLocked || !g_pCtx || !g_pStage) return;
	g_pCtx->Unmap(g_pStage, 0);
	g_bLocked = false;

	// Whatever the engine wrote goes back. The client draws its frame marker
	// and its per-eye crosshair straight into the screen, so this direction is
	// load-bearing and not merely tidy.
	ID3D11Texture2D* pBack = BackBuffer();
	if (pBack) { g_pCtx->CopyResource(pBack, g_pStage); pBack->Release(); }

	LARGE_INTEGER nEnd; QueryPerformanceCounter(&nEnd);
	if (g_Freq.QuadPart)
		g_fMillis += 1000.0 * (double)(nEnd.QuadPart - g_Start.QuadPart)
				   / (double)g_Freq.QuadPart;
}

bool SL_ReadRect(int nSrcX, int nSrcY, int nW, int nH,
				 void* pDst, int nDstPitch)
{
	if (!pDst || nW <= 0 || nH <= 0) return false;
	if (g_bLocked) return false;			// a lock is holding the staging map
	if (!g_pDev || !g_pCtx || !g_pSwap) return false;

	ID3D11Texture2D* pBack = BackBuffer();
	if (!pBack) return false;
	if (!EnsureStage(pBack)) { pBack->Release(); return false; }

	LARGE_INTEGER nA; QueryPerformanceCounter(&nA);
	g_pCtx->CopyResource(g_pStage, pBack);
	pBack->Release();

	D3D11_MAPPED_SUBRESOURCE ms{};
	if (FAILED(g_pCtx->Map(g_pStage, 0, D3D11_MAP_READ, 0, &ms))) return false;

	// Clamp to the back buffer. The rectangle comes from the engine and a
	// read that runs past the end would be a memory bug surfacing somewhere
	// else entirely.
	if (nSrcX < 0) nSrcX = 0;
	if (nSrcY < 0) nSrcY = 0;
	if (nSrcX + nW > (int)g_nW) nW = (int)g_nW - nSrcX;
	if (nSrcY + nH > (int)g_nH) nH = (int)g_nH - nSrcY;

	if (nW > 0 && nH > 0)
		for (int y = 0; y < nH; ++y)
			memcpy((unsigned char*)pDst + (size_t)y * nDstPitch,
				   (const unsigned char*)ms.pData
					 + (size_t)(nSrcY + y) * ms.RowPitch + (size_t)nSrcX * 4,
				   (size_t)nW * 4);

	g_pCtx->Unmap(g_pStage, 0);
	++g_nLocks;
	LARGE_INTEGER nB; QueryPerformanceCounter(&nB);
	if (g_Freq.QuadPart)
		g_fMillis += 1000.0 * (double)(nB.QuadPart - nA.QuadPart) / (double)g_Freq.QuadPart;
	return nW > 0 && nH > 0;
}

void SL_Stats(long* pnLocks, long* pnFailed, double* pfMillis)
{
	if (pnLocks)  *pnLocks  = g_nLocks;
	if (pnFailed) *pnFailed = g_nFailed;
	if (pfMillis) *pfMillis = g_fMillis;
}
