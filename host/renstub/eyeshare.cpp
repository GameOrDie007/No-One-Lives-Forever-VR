#include "eyeshare.h"

#include "VRShared.h"			// for VRSHARED_EYETEX_NAME - one copy, as it insists

#include <dxgi1_2.h>
#include <stdio.h>

namespace
{
	ID3D11Device*        g_pDev = nullptr;
	ID3D11DeviceContext* g_pCtx = nullptr;
	ES_LogFn             Log    = nullptr;

	// A RING OF THREE. One shared texture with one keyed mutex made the two
	// processes hand each other a GPU lock every frame: the game's copy in
	// waited for the host's release, the host's copy out waited for the
	// game's, and each side's GPU queue sat behind the other's. A host
	// log: xrEndFrame blocking 90 ms every two to three seconds, the game's
	// watchdog catching driver waits on the same cadence. With three the
	// game writes texture N while the host reads N-1, and a key is only
	// ever taken when nobody else wants it. Index = eye-texture serial % 3.
	const int            kEyeRing = 3;
	ID3D11Texture2D*     g_pSharedRing[3] = { nullptr, nullptr, nullptr };
	IDXGIKeyedMutex*     g_pMutexRing[3]  = { nullptr, nullptr, nullptr };
	HANDLE               g_hSharedRing[3] = { nullptr, nullptr, nullptr };
	ID3D11Texture2D*     g_pShared = nullptr;	// ring[0], for the describers
	IDXGIKeyedMutex*     g_pMutex  = nullptr;
	HANDLE               g_hShared = nullptr;

	uint32_t g_nW = 0, g_nH = 0;
	uint32_t g_nFormat = 0;
	int32_t  g_nLuidLo = 0, g_nLuidHi = 0;

	uint32_t g_nSerial    = 0;
	long     g_nPublished = 0;
	long     g_nDropped   = 0;
	// the pause overlay
	ID3D11Texture2D* g_pOvl      = nullptr;
	IDXGIKeyedMutex* g_pOvlMutex = nullptr;
	HANDLE           g_hOvl      = nullptr;
	uint32_t g_nOvlW = 0, g_nOvlH = 0, g_nOvlSerial = 0;
	long     g_nOvlDropped = 0;

	// ONE key, taken and released by both sides.
	//
	// The first version used the textbook handoff - writer takes 0 and
	// releases 1, reader takes 1 and releases 0 - and it starved itself
	// immediately: with no reader running, nothing ever releases key 0 again,
	// so the writer published exactly one frame and then failed every acquire
	// for the rest of the run. The probe measured "serial 1 -> 1 over 15 s".
	//
	// A handoff protocol requires the other side to exist. This one does not:
	// a single key still gives mutual exclusion, and a reader that is not
	// there simply never takes it.
	const UINT64 kKey = 0;
}

bool ES_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
			   uint32_t nW, uint32_t nH, ES_LogFn pfnLog)
{
	Log = pfnLog;
	g_pDev = pDev; g_pCtx = pCtx;
	if (!pDev || !pCtx || nW == 0 || nH == 0) return false;

	// The adapter, so a mismatch between the two processes is a number rather
	// than an empty texture nobody can explain.
	{
		IDXGIDevice*  pDxgi = nullptr;
		IDXGIAdapter* pAd   = nullptr;
		if (SUCCEEDED(pDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgi)) &&
			SUCCEEDED(pDxgi->GetAdapter(&pAd)))
		{
			DXGI_ADAPTER_DESC d{};
			if (SUCCEEDED(pAd->GetDesc(&d)))
			{
				g_nLuidLo = (int32_t)d.AdapterLuid.LowPart;
				g_nLuidHi = (int32_t)d.AdapterLuid.HighPart;
				char szName[160]{};
				WideCharToMultiByte(CP_ACP, 0, d.Description, -1, szName,
									sizeof(szName) - 1, nullptr, nullptr);
				if (Log) Log("  ES: adapter '%s' luid %08X:%08X", szName,
							 (unsigned)g_nLuidHi, (unsigned)g_nLuidLo);
			}
		}
		if (pAd)   pAd->Release();
		if (pDxgi) pDxgi->Release();
	}

	// A named shared texture needs the NTHANDLE flag and D3D11.1. Without
	// NTHANDLE the older GetSharedHandle path gives a value that is not a real
	// handle and cannot be opened by name at all.
	D3D11_TEXTURE2D_DESC td{};
	td.Width            = nW;
	td.Height           = nH;
	td.MipLevels        = 1;
	td.ArraySize        = 1;
	td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage            = D3D11_USAGE_DEFAULT;
	td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED_NTHANDLE
						| D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	HRESULT hr = S_OK;
	for (int k = 0; k < kEyeRing; ++k)
	{
		hr = pDev->CreateTexture2D(&td, nullptr, &g_pSharedRing[k]);
		if (Log) Log("  ES: CreateTexture2D %ux%u [%d] hr=%08X", nW, nH, k, (unsigned)hr);
		if (FAILED(hr) || !g_pSharedRing[k]) { ES_Destroy(); return false; }

		wchar_t wszName[128];
		if (k == 0) wcscpy_s(wszName, VRSHARED_EYETEX_NAME);
		else swprintf_s(wszName, L"%ls_%d", VRSHARED_EYETEX_NAME, k);

		IDXGIResource1* pRes = nullptr;
		hr = g_pSharedRing[k]->QueryInterface(__uuidof(IDXGIResource1), (void**)&pRes);
		if (SUCCEEDED(hr) && pRes)
		{
			hr = pRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ
											   | DXGI_SHARED_RESOURCE_WRITE,
										  wszName, &g_hSharedRing[k]);
			if (Log) Log("  ES: CreateSharedHandle [%d] %ls hr=%08X", k, wszName, (unsigned)hr);
			pRes->Release();
		}
		else if (Log) Log("  ES: no IDXGIResource1 (hr=%08X) - D3D11.1 is required",
						  (unsigned)hr);
		if (!g_hSharedRing[k]) { ES_Destroy(); return false; }

		hr = g_pSharedRing[k]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_pMutexRing[k]);
		if (FAILED(hr) || !g_pMutexRing[k]) { ES_Destroy(); return false; }
	}
	g_pShared = g_pSharedRing[0]; g_pMutex = g_pMutexRing[0]; g_hShared = g_hSharedRing[0];
	if (Log) Log("  ES: keyed mutexes on all %d ring textures", kEyeRing);

	g_nW = nW; g_nH = nH;
	g_nFormat = (uint32_t)td.Format;
	if (Log) Log("  ES: publishing %ux%u as %ls", nW, nH, VRSHARED_EYETEX_NAME);

	// The overlay, one eye wide. Its absence is not fatal: the host falls
	// back to drawing the pause menu in the eyes.
	{
		D3D11_TEXTURE2D_DESC od = td;
		od.Width = nW / 2; od.Height = nH;
		HRESULT ho = pDev->CreateTexture2D(&od, nullptr, &g_pOvl);
		IDXGIResource1* pR = nullptr;
		if (SUCCEEDED(ho) && g_pOvl
			&& SUCCEEDED(g_pOvl->QueryInterface(__uuidof(IDXGIResource1), (void**)&pR)) && pR)
		{
			ho = pR->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
										VRSHARED_OVLTEX_NAME, &g_hOvl);
			pR->Release();
		}
		if (SUCCEEDED(ho) && g_hOvl)
			ho = g_pOvl->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_pOvlMutex);
		if (FAILED(ho) || !g_pOvl || !g_hOvl || !g_pOvlMutex)
		{
			if (Log) Log("  ES: the pause overlay texture did not come up (hr=%08X)", (unsigned)ho);
			if (g_pOvlMutex) { g_pOvlMutex->Release(); g_pOvlMutex = nullptr; }
			if (g_pOvl)      { g_pOvl->Release();      g_pOvl = nullptr; }
			if (g_hOvl)      { CloseHandle(g_hOvl);    g_hOvl = nullptr; }
		}
		else
		{
			g_nOvlW = od.Width; g_nOvlH = od.Height;
			if (Log) Log("  ES: pause overlay %ux%u as %ls", g_nOvlW, g_nOvlH, VRSHARED_OVLTEX_NAME);
		}
	}
	return true;
}

void ES_PublishOverlay(ID3D11Texture2D* pTex)
{
	if (!g_pOvl || !g_pOvlMutex || !g_pCtx || !pTex) return;
	if (g_pOvlMutex->AcquireSync(kKey, 0) != S_OK) { ++g_nOvlDropped; return; }
	g_pCtx->CopyResource(g_pOvl, pTex);
	++g_nOvlSerial;
	g_pOvlMutex->ReleaseSync(kKey);
}
uint32_t ES_OverlaySerial() { return g_nOvlSerial; }
bool ES_OverlaySize(uint32_t* pnW, uint32_t* pnH)
{
	if (!g_pOvl) return false;
	if (pnW) *pnW = g_nOvlW;
	if (pnH) *pnH = g_nOvlH;
	return true;
}

void ES_Destroy()
{
	if (g_pOvlMutex) { g_pOvlMutex->Release(); g_pOvlMutex = nullptr; }
	if (g_pOvl)      { g_pOvl->Release();      g_pOvl = nullptr; }
	if (g_hOvl)      { CloseHandle(g_hOvl);    g_hOvl = nullptr; }
	g_nOvlW = g_nOvlH = 0;
	for (int k = 0; k < kEyeRing; ++k)
	{
		if (g_pMutexRing[k])  { g_pMutexRing[k]->Release();  g_pMutexRing[k]  = nullptr; }
		if (g_pSharedRing[k]) { g_pSharedRing[k]->Release(); g_pSharedRing[k] = nullptr; }
		if (g_hSharedRing[k]) { CloseHandle(g_hSharedRing[k]); g_hSharedRing[k] = nullptr; }
	}
	g_pMutex = nullptr; g_pShared = nullptr; g_hShared = nullptr;
	g_nW = g_nH = 0;
	g_pDev = nullptr; g_pCtx = nullptr;
}

void ES_Publish(ID3D11Texture2D* pBackBuffer)
{
	if (!g_pShared || !g_pMutex || !g_pCtx || !pBackBuffer) return;

	// Zero wait. The game's present thread must never block on the host: if the
	// reader still holds it, this frame is skipped and the next one goes. A
	// dropped frame is invisible; a stalled present is not.
	// The NEXT serial's slot. The host reads slot (serial % 3) for the serial
	// it sees, so the game is always writing the one it read two frames ago.
	const int k = (int)((g_nSerial + 1u) % (uint32_t)kEyeRing);
	const HRESULT hr = g_pMutexRing[k]->AcquireSync(kKey, 0);
	if (hr != S_OK) { ++g_nDropped; return; }

	g_pCtx->CopyResource(g_pSharedRing[k], pBackBuffer);

	++g_nSerial;
	++g_nPublished;
	g_pMutexRing[k]->ReleaseSync(kKey);
}

void ES_Stats(long* pnPublished, long* pnDropped, uint32_t* pnSerial)
{
	if (pnPublished) *pnPublished = g_nPublished;
	if (pnDropped)   *pnDropped   = g_nDropped;
	if (pnSerial)    *pnSerial    = g_nSerial;
}

bool ES_Describe(uint32_t* pnW, uint32_t* pnH, uint32_t* pnFormat,
				 int32_t* pnLuidLo, int32_t* pnLuidHi)
{
	if (!g_pShared) return false;
	if (pnW)      *pnW      = g_nW;
	if (pnH)      *pnH      = g_nH;
	if (pnFormat) *pnFormat = g_nFormat;
	if (pnLuidLo) *pnLuidLo = g_nLuidLo;
	if (pnLuidHi) *pnLuidHi = g_nLuidHi;
	return true;
}
