#include "sharedframe.h"

#include "VRShared.h"

#include <dxgi1_2.h>
#include <stdio.h>

namespace
{
	void Nothing(const char*, ...) {}
}

bool SharedFrame::Start(ID3D11Device* pDevice, void (*pfnLog)(const char*, ...))
{
	m_pfnLog = pfnLog ? pfnLog : &Nothing;
	if (!pDevice) return false;
	m_pDevice = pDevice;
	pDevice->GetImmediateContext(&m_pContext);

	m_hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, VRSHARED_NAME);
	if (!m_hMap)
	{
		m_pfnLog("shared frame: no block %s - window capture it is", VRSHARED_NAME);
		return false;
	}
	// The whole section, not sizeof(VRSharedState): the client and the
	// renderer may be older than this build, and asking for more bytes than
	// were published fails outright.
	m_pBlock = (VRSharedState*)MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, 0);
	if (!m_pBlock) { Stop(); return false; }

	if (m_pBlock->nVersion < 13)
	{
		m_pfnLog("shared frame: block is version %u; the shared texture arrived"
				 " at 13. Falling back to window capture.", m_pBlock->nVersion);
		Stop();
		return false;
	}
	if (m_pBlock->nEyeTexSerial == 0)
	{
		// NOT A VERDICT, A TIMING. The host asked this at 7.08 s in a headset
		// run and the renderer's first frame went out shortly after; the
		// answer was read as "the renderer does not publish" and the whole
		// session ran on window capture - 53 fresh frames a second into a
		// 90 Hz headset. The caller retries; this only says so once.
		static int s_nSaid = 0;
		if (s_nSaid++ == 0)
			m_pfnLog("shared frame: the renderer is not publishing yet (serial 0)."
					 " Window capture for now; will keep asking.");
		Stop();
		return false;
	}

	// The adapter has to match or the open silently yields nothing usable. The
	// host does not get to choose its adapter - the OpenXR runtime does - so
	// this is a real possibility on a machine with more than one GPU, and it
	// must produce a sentence rather than a black eye image.
	{
		IDXGIDevice*  pDxgi = nullptr;
		IDXGIAdapter* pAd   = nullptr;
		if (SUCCEEDED(pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDxgi)) &&
			SUCCEEDED(pDxgi->GetAdapter(&pAd)))
		{
			DXGI_ADAPTER_DESC d{};
			pAd->GetDesc(&d);
			const bool bMatch =
				(int32_t)d.AdapterLuid.LowPart  == m_pBlock->nAdapterLuidLo &&
				(int32_t)d.AdapterLuid.HighPart == m_pBlock->nAdapterLuidHi;
			if (!bMatch)
			{
				m_pfnLog("shared frame: the renderer is on adapter %08X:%08X and"
						 " this host is on %08X:%08X. A shared texture only opens"
						 " on the same adapter, so window capture it is.",
						 (unsigned)m_pBlock->nAdapterLuidHi,
						 (unsigned)m_pBlock->nAdapterLuidLo,
						 (unsigned)d.AdapterLuid.HighPart,
						 (unsigned)d.AdapterLuid.LowPart);
				pAd->Release(); pDxgi->Release();
				Stop();
				return false;
			}
		}
		if (pAd)   pAd->Release();
		if (pDxgi) pDxgi->Release();
	}

	ID3D11Device1* pDev1 = nullptr;
	if (FAILED(pDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&pDev1)))
	{ m_pfnLog("shared frame: no ID3D11Device1"); Stop(); return false; }

	HRESULT hr = pDev1->OpenSharedResourceByName(VRSHARED_EYETEX_NAME,
												 DXGI_SHARED_RESOURCE_READ,
												 __uuidof(ID3D11Texture2D),
												 (void**)&m_pShared);
	pDev1->Release();
	if (FAILED(hr) || !m_pShared)
	{
		m_pfnLog("shared frame: OpenSharedResourceByName hr=%08X", (unsigned)hr);
		Stop();
		return false;
	}

	D3D11_TEXTURE2D_DESC td{};
	m_pShared->GetDesc(&td);
	m_nW = td.Width; m_nH = td.Height;

	if (FAILED(m_pShared->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&m_pMutex)))
	{ m_pfnLog("shared frame: no keyed mutex"); Stop(); return false; }
	m_pSharedRing[0] = m_pShared; m_pSharedRing[0]->AddRef();
	m_pMutexRing[0]  = m_pMutex;  m_pMutexRing[0]->AddRef();
	m_nRing = 1;
	{
		ID3D11Device1* pD1r = nullptr;
		if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&pD1r)) && pD1r)
		{
			for (int k = 1; k < 3; ++k)
			{
				wchar_t wszName[128];
				swprintf_s(wszName, L"%ls_%d", VRSHARED_EYETEX_NAME, k);
				ID3D11Texture2D* pT = nullptr;
				if (FAILED(pD1r->OpenSharedResourceByName(wszName, DXGI_SHARED_RESOURCE_READ,
														  __uuidof(ID3D11Texture2D), (void**)&pT)) || !pT)
					break;
				IDXGIKeyedMutex* pM = nullptr;
				if (FAILED(pT->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&pM)) || !pM)
				{ pT->Release(); break; }
				m_pSharedRing[k] = pT; m_pMutexRing[k] = pM; m_nRing = k + 1;
			}
			pD1r->Release();
		}
		m_pfnLog("shared frame: eye texture ring of %d%s", m_nRing,
				 (m_nRing == 3) ? " - the game and the host never contend for a key"
								: " - an older game; one texture, one key");
	}

	// Our own copy, so the renderer's present thread is never waiting on us
	// while we submit to the runtime.
	D3D11_TEXTURE2D_DESC cd = td;
	cd.MiscFlags      = 0;
	cd.Usage          = D3D11_USAGE_DEFAULT;
	cd.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
	cd.CPUAccessFlags = 0;
	if (FAILED(m_pDevice->CreateTexture2D(&cd, nullptr, &m_pCopy)))
	{ m_pfnLog("shared frame: could not create the private copy"); Stop(); return false; }

	m_nLastSerial = m_pBlock->nEyeTexSerial;
	m_dwLastNew   = GetTickCount();

	// The pause overlay, optional: an older renderer has none.
	{
		ID3D11Device1* pD1 = nullptr;
		if (SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device1), (void**)&pD1)) && pD1)
		{
			HRESULT ho = pD1->OpenSharedResourceByName(VRSHARED_OVLTEX_NAME, DXGI_SHARED_RESOURCE_READ,
													   __uuidof(ID3D11Texture2D), (void**)&m_pOvl);
			pD1->Release();
			if (SUCCEEDED(ho) && m_pOvl)
			{
				D3D11_TEXTURE2D_DESC od{};
				m_pOvl->GetDesc(&od);
				m_nOvlW = od.Width; m_nOvlH = od.Height;
				D3D11_TEXTURE2D_DESC ocd = od;
				ocd.MiscFlags = 0; ocd.Usage = D3D11_USAGE_DEFAULT;
				ocd.BindFlags = D3D11_BIND_SHADER_RESOURCE; ocd.CPUAccessFlags = 0;
				if (FAILED(m_pOvl->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&m_pOvlMutex))
					|| FAILED(m_pDevice->CreateTexture2D(&ocd, nullptr, &m_pOvlCopy)))
				{
					if (m_pOvlMutex) { m_pOvlMutex->Release(); m_pOvlMutex = nullptr; }
					m_pOvl->Release(); m_pOvl = nullptr;
					m_pfnLog("shared frame: the pause overlay opened but could not be copied");
				}
				else m_pfnLog("shared frame: pause overlay %ux%u open", m_nOvlW, m_nOvlH);
			}
			else m_pfnLog("shared frame: no pause overlay published (hr=%08X) - the pause"
						  " menu will be drawn in the eyes", (unsigned)ho);
		}
	}
	m_pfnLog("shared frame: OPEN, %ux%u, eye %dx%d. The window is no longer"
			 " being captured.", m_nW, m_nH, EyeWidth(), EyeHeight());
	return true;
}

ID3D11Texture2D* SharedFrame::TryGetOverlay()
{
	if (!m_pOvl || !m_pOvlMutex || !m_pOvlCopy || !m_pBlock) return nullptr;
	const uint32_t nNow = m_pBlock->nOvlSerial;
	if (nNow != m_nOvlSerial)
	{
		if (m_pOvlMutex->AcquireSync(0, 4) == S_OK)
		{
			m_pContext->CopyResource(m_pOvlCopy, m_pOvl);
			m_pOvlMutex->ReleaseSync(0);
			m_nOvlSerial = nNow;
		}
	}
	return (m_nOvlSerial != 0) ? m_pOvlCopy : nullptr;
}

void SharedFrame::Stop()
{
	if (m_pOvlCopy)  { m_pOvlCopy->Release();  m_pOvlCopy = nullptr; }
	if (m_pOvlMutex) { m_pOvlMutex->Release(); m_pOvlMutex = nullptr; }
	if (m_pOvl)      { m_pOvl->Release();      m_pOvl = nullptr; }
	m_nOvlW = m_nOvlH = m_nOvlSerial = 0;
	if (m_pCopy)    { m_pCopy->Release();    m_pCopy = nullptr; }
	for (int k = 0; k < 3; ++k)
	{
		if (m_pMutexRing[k])  { m_pMutexRing[k]->Release();  m_pMutexRing[k] = nullptr; }
		if (m_pSharedRing[k]) { m_pSharedRing[k]->Release(); m_pSharedRing[k] = nullptr; }
	}
	m_nRing = 1;
	if (m_pMutex)   { m_pMutex->Release();   m_pMutex = nullptr; }
	if (m_pShared)  { m_pShared->Release();  m_pShared = nullptr; }
	if (m_pContext) { m_pContext->Release(); m_pContext = nullptr; }
	if (m_pBlock)   { UnmapViewOfFile(m_pBlock); m_pBlock = nullptr; }
	if (m_hMap)     { CloseHandle(m_hMap);   m_hMap = nullptr; }
	m_nW = m_nH = 0;
	m_nLastSerial = 0;
	m_bRestarted  = false;
}

ID3D11Texture2D* SharedFrame::TryGetFrame()
{
	if (!m_pShared || !m_pMutex || !m_pCopy || !m_pBlock) return nullptr;

	const uint32_t nNow = m_pBlock->nEyeTexSerial;
	if (nNow == m_nLastSerial) return nullptr;		// nothing new
	// Backwards: a new renderer counting from zero into textures we do not
	// hold. See Restarted().
	if (nNow < m_nLastSerial)
	{
		if (!m_bRestarted)
			m_pfnLog("shared frame: serial went back from %u to %u - the renderer was"
					 " replaced; these textures are the old one's", m_nLastSerial, nNow);
		m_bRestarted = true;
		return nullptr;
	}
	// The marker travels with the serial (v16): written before it, read after.
	const int nMarker = (m_pBlock->nVersion >= 16)
					  ? (int)(m_pBlock->nEyeTexMarker & 0xFFu) : -1;

	// A short wait, not zero: the renderer holds this only for the length of
	// one CopyResource, and giving up instantly would throw away frames for no
	// reason. Not infinite either - a host that blocks forever on a game that
	// has stopped is a hang rather than a fallback.
	const int k = (m_nRing > 1) ? (int)(nNow % (uint32_t)m_nRing) : 0;
	if (m_pMutexRing[k]->AcquireSync(0, 4) != S_OK) return nullptr;
	m_pContext->CopyResource(m_pCopy, m_pSharedRing[k]);
	m_pMutexRing[k]->ReleaseSync(0);

	m_nLastSerial = nNow;
	m_nLastMarker = nMarker;
	m_dwLastNew   = GetTickCount();
	return m_pCopy;
}

bool SharedFrame::IsStale(DWORD dwNowTick) const
{
	return (dwNowTick - m_dwLastNew) > 1000;
}
