// ----------------------------------------------------------------------- //
//
// MODULE  : capture.cpp   -   see capture.h
//
// ----------------------------------------------------------------------- //

#include "capture.h"
#include "hostlog.h"

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

using HostLog::Msg;
using winrt::com_ptr;
using winrt::check_hresult;

namespace
{
	// Match on window CLASS, not title. The engine creates its window titled
	// "NOLF" and applies -windowtitle later in startup, so a title match is a
	// race that usually loses. The class is set at creation.
	HWND FindGameWindow()
	{
		HWND h = FindWindowW(L"LithTech", nullptr);
		return (h && IsWindow(h) && IsWindowVisible(h)) ? h : nullptr;
	}

	template <typename T>
	com_ptr<T> GetDXGIInterface(winrt::Windows::Foundation::IInspectable const& obj)
	{
		auto access = obj.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		com_ptr<T> out;
		check_hresult(access->GetInterface(winrt::guid_of<T>(), out.put_void()));
		return out;
	}
}

bool WindowCapture::Start(ID3D11Device* pDevice, int nWaitSeconds)
{
	// DPI awareness belongs HERE, not in some caller's main(). Every window
	// measurement below is meaningless without it, and relying on a caller to
	// remember has now failed three times: M0's 2560x1440 reading, M4's
	// 1290x730 client rect, and again when this module was split out of
	// vrhost.cpp. Setting it at the point of use makes it impossible to forget.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	if (!wgc::GraphicsCaptureSession::IsSupported())
	{
		Msg("FATAL: Windows Graphics Capture is not supported here.");
		return false;
	}

	Msg("waiting for a window of class \"LithTech\" ...");
	for (int i = 0; i < nWaitSeconds * 2 && !m_hWnd; ++i)
	{
		if (!GameAlive()) { Msg("the game has quit - no window will come"); return false; }
		m_hWnd = FindGameWindow();
		if (!m_hWnd) Sleep(500);
	}
	if (!m_hWnd) { Msg("FATAL: no LithTech window appeared."); return false; }
	if (!m_hGameProc)
	{
		DWORD nPid = 0;
		GetWindowThreadProcessId(m_hWnd, &nPid);
		if (nPid) m_hGameProc = OpenProcess(SYNCHRONIZE, FALSE, nPid);
		Msg("game process %lu - %s", (unsigned long)nPid,
			m_hGameProc ? "watched, the host leaves when it does" : "could not be opened for watching");
	}

	// The engine creates its window at 314x160 and resizes during startup. The
	// frame pool is sized once at session start, so capturing too early locks
	// the whole run to that tiny frame.
	int lastW = -1, lastH = -1, nStable = 0;
	for (int i = 0; i < 300; ++i)
	{
		if (!GameAlive()) { Msg("the game has quit while its window was settling"); return false; }
		RECT rc{};
		GetClientRect(m_hWnd, &rc);
		const int w = rc.right - rc.left, h = rc.bottom - rc.top;
		if (w >= 640 && h >= 400 && w == lastW && h == lastH)
		{
			if (++nStable >= 6) break;
		}
		else nStable = 0;
		lastW = w; lastH = h;
		Sleep(100);
	}

	RECT rcClient{};
	GetClientRect(m_hWnd, &rcClient);
	POINT ptClient{ 0, 0 };
	ClientToScreen(m_hWnd, &ptClient);

	RECT rcFrame{};
	if (FAILED(DwmGetWindowAttribute(m_hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcFrame, sizeof(rcFrame))))
	{
		GetWindowRect(m_hWnd, &rcFrame);
	}

	const int clientW = rcClient.right - rcClient.left;
	const int clientH = rcClient.bottom - rcClient.top;

	// The captured texture covers the whole window, chrome included, so the
	// client area has to be located inside it or the split would slice the
	// title bar instead of the stereo seam.
	m_nCropX   = ptClient.x - rcFrame.left;
	m_nCropY   = ptClient.y - rcFrame.top;
	m_nClientW = clientW;
	m_nClientH = clientH;
	m_nEyeW    = clientW / 2;
	m_nEyeH    = clientH;

	Msg("game window %p, client %dx%d, crop offset (%d,%d), eye %dx%d",
		(void*)m_hWnd, clientW, clientH, m_nCropX, m_nCropY, m_nEyeW, m_nEyeH);

	// Which monitor, and at what scaling. The game renders at one size and the
	// window ends up another; this is the missing piece of that relationship.
	{
		HMONITOR hMon = MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
		MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
		GetMonitorInfoW(hMon, &mi);

		UINT dpiX = 96, dpiY = 96;
		GetDpiForMonitor(hMon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);

		Msg("  monitor %ls: %dx%d at (%d,%d), dpi %u (%u%% scaling)",
			mi.szDevice,
			mi.rcMonitor.right - mi.rcMonitor.left,
			mi.rcMonitor.bottom - mi.rcMonitor.top,
			mi.rcMonitor.left, mi.rcMonitor.top,
			dpiX, (unsigned)(dpiX * 100 / 96));
		Msg("  window at screen (%d,%d)", ptClient.x, ptClient.y);
	}

	// A negative offset means the client rect and the DWM frame came from
	// different coordinate spaces - the signature of a DPI-unaware process.
	// Refuse rather than silently capture the wrong region.
	if (m_nCropX < 0 || m_nCropY < 0 || m_nEyeW <= 0 || m_nEyeH <= 0)
	{
		Msg("FATAL: implausible crop geometry - DPI awareness did not take effect.");
		return false;
	}

	com_ptr<IDXGIDevice> dxgi;
	if (FAILED(pDevice->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void())))
	{
		Msg("FATAL: device does not expose IDXGIDevice");
		return false;
	}

	winrt::com_ptr<::IInspectable> inspectable;
	check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
	auto rtDevice = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();

	auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
	check_hresult(interop->CreateForWindow(m_hWnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
		winrt::put_abi(m_Item)));

	m_Pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
		rtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m_Item.Size());
	m_Session = m_Pool.CreateCaptureSession(m_Item);
	m_Session.StartCapture();

	Msg("capture started, item %dx%d", m_Item.Size().Width, m_Item.Size().Height);
	return true;
}

bool WindowCapture::ApplyGameScreenSize(int nGameW, int nGameH)
{
	if (nGameW <= 0 || nGameH <= 0) return false;
	if (nGameW > m_nClientW || nGameH > m_nClientH) return false;

	const int newEyeW = nGameW / 2;
	const int newEyeH = nGameH;
	if (newEyeW == m_nEyeW && newEyeH == m_nEyeH) return false;

	// The render surface sits centred in the client, so the margin splits
	// evenly on each side.
	m_nCropX += (m_nClientW - nGameW) / 2;
	m_nCropY += (m_nClientH - nGameH) / 2;
	m_nEyeW   = newEyeW;
	m_nEyeH   = newEyeH;

	Msg("game reports render surface %dx%d (client %dx%d) - crop now (%d,%d), eye %dx%d",
		nGameW, nGameH, m_nClientW, m_nClientH, m_nCropX, m_nCropY, m_nEyeW, m_nEyeH);
	return true;
}

bool WindowCapture::HasResized() const
{
	if (!m_hWnd || !IsWindow(m_hWnd)) return false;
	if (m_nClientW <= 0) return false;
	// A MINIMIZED WINDOW IS NOT A RESIZE. Alt-tabbing can minimize the game;
	// Windows parks it at (-32000,-32000). Taking that for a resize rebuilt
	// the capture against it, the geometry came out impossible, and the host
	// quit - which closed the game.
	if (IsIconic(m_hWnd)) return false;

	RECT rc{};
	GetClientRect(m_hWnd, &rc);
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w < 640 || h < 400) return false;			// mid-transition, ignore

	return (w != m_nClientW) || (h != m_nClientH);
}

bool WindowCapture::Restart(ID3D11Device* pDevice)
{
	Msg("game window resized - rebuilding capture");
	Stop();
	m_hWnd = nullptr;
	return Start(pDevice, 30);
}

winrt::com_ptr<ID3D11Texture2D> WindowCapture::TryGetFrame()
{
	if (!m_Pool) return nullptr;

	// Drain to the newest frame. Showing the freshest image matters more than
	// showing every one, and a backlog only adds latency.
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{ nullptr };
	for (;;)
	{
		auto next = m_Pool.TryGetNextFrame();
		if (!next) break;
		frame = next;
	}
	if (!frame) return nullptr;

	// How stale this image already is when it reaches us.
	//
	// Every attempt to set the pose lag by feel has failed, and it was never a
	// fair question: the difference between 10 and 20 ms is well below what
	// anyone can judge through a headset. But the frame carries the time the
	// compositor produced it, and that is the larger, measurable part of the
	// staleness - the rest is the client's own render, which it knows.
	//
	// SystemRelativeTime counts 100ns units on the same clock as QPC, so it is
	// converted through QPC rather than assumed to match the log's clock.
	{
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		const int64_t nowUnits = (freq.QuadPart > 0)
			? (int64_t)((double)now.QuadPart * 10000000.0 / (double)freq.QuadPart) : 0;
		const int64_t frameUnits = frame.SystemRelativeTime().count();
		if (nowUnits > 0 && frameUnits > 0)
			m_fLastAgeMs = (double)(nowUnits - frameUnits) / 10000.0;
	}

	// Only now release the previous frame - the caller has finished with it.
	if (m_Frame) m_Frame.Close();
	m_Frame = frame;

	return GetDXGIInterface<ID3D11Texture2D>(m_Frame.Surface());
}

// Decode the 12-block marker at (cx,cy) from a mapped BGRA image. Returns the
// frame byte, or -1. Same rules as ReadFrameMarker: contrast, sync 1 0 1 0,
// and every block sitting on one of the two levels.
static int DecodeMarkerAt(const uint8_t* pBase, UINT nPitch, UINT nW, UINT nH,
						  int cx, int cy)
{
	const int kBlock = 8, kBits = 8, kSync = 4, kAll = kBits + kSync;
	if (cx < 0 || cy < 0) return -1;
	if ((UINT)(cx + kBlock * kAll) > nW || (UINT)(cy + kBlock) > nH) return -1;

	int lum[kBits + kSync];
	int lo = 255, hi = 0;
	const uint8_t* pRow = pBase + (size_t)(cy + kBlock / 2) * nPitch;
	for (int b = 0; b < kAll; ++b)
	{
		const uint8_t* px = pRow + (size_t)(cx + b * kBlock + kBlock / 2) * 4;
		lum[b] = (px[0] + px[1] + px[2]) / 3;
		if (lum[b] < lo) lo = lum[b];
		if (lum[b] > hi) hi = lum[b];
	}
	if (hi - lo < 48) return -1;
	const int mid = (lo + hi) / 2;
	for (int s = 0; s < kSync; ++s)
		if ((lum[kBits + s] > mid) != ((s % 2) == 0)) return -1;
	const int band = (hi - lo) / 4;
	for (int b = 0; b < kAll; ++b)
		if (lum[b] > lo + band && lum[b] < hi - band) return -1;
	int nValue = 0;
	for (int b = 0; b < kBits; ++b)
		if (lum[b] > mid) nValue |= (1 << b);
	return nValue;
}

bool WindowCapture::CalibrateCropFromMarker(ID3D11Device* pDev,
											ID3D11DeviceContext* pCtx,
											ID3D11Texture2D* pSrc)
{
	if (!pDev || !pCtx || !pSrc) return false;

	const UINT kRange = 64;				// how far to look for the origin
	const UINT kW = kRange + 8 * 12;	// the marker is 96 wide
	const UINT kH = kRange + 8;

	D3D11_TEXTURE2D_DESC srcDesc{};
	pSrc->GetDesc(&srcDesc);
	if (srcDesc.Width < kW || srcDesc.Height < kH) return false;

	winrt::com_ptr<ID3D11Texture2D> stage;
	D3D11_TEXTURE2D_DESC sd{};
	sd.Width = kW; sd.Height = kH;
	sd.MipLevels = 1; sd.ArraySize = 1;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(pDev->CreateTexture2D(&sd, nullptr, stage.put()))) return false;

	D3D11_BOX box{ 0, 0, 0, kW, kH, 1 };
	pCtx->CopySubresourceRegion(stage.get(), 0, 0, 0, 0, pSrc, 0, &box);

	D3D11_MAPPED_SUBRESOURCE map{};
	if (FAILED(pCtx->Map(stage.get(), 0, D3D11_MAP_READ, 0, &map))) return false;

	int nBestX = -1, nBestY = -1, nValue = -1, nHits = 0;
	for (UINT cy = 0; cy < kRange; ++cy)
		for (UINT cx = 0; cx < kRange; ++cx)
		{
			const int v = DecodeMarkerAt((const uint8_t*)map.pData,
										 map.RowPitch, kW, kH, (int)cx, (int)cy);
			if (v < 0) continue;
			++nHits;
			if (nBestX < 0 || (int)cx < nBestX) nBestX = (int)cx;
			if (nBestY < 0 || (int)cy < nBestY) nBestY = (int)cy;
			nValue = v;
		}
	pCtx->Unmap(stage.get(), 0);

	if (nBestX < 0)
	{
		Msg("crop calibration: no frame marker in the top-left %ux%u"
			" - keeping the window-derived crop (%d,%d)", kRange, kRange,
			m_nCropX, m_nCropY);
		return false;
	}

	if (nBestX == m_nCropX && nBestY == m_nCropY)
	{
		Msg("crop calibration: marker at (%d,%d), frame byte %d"
			" - agrees with the window-derived crop", nBestX, nBestY, nValue);
		return true;
	}

	Msg("crop calibration: marker at (%d,%d) but window metrics said (%d,%d)"
		" - TAKING THE MARKER (%d decodes, frame byte %d)",
		nBestX, nBestY, m_nCropX, m_nCropY, nHits, nValue);
	Msg("  the old crop put the marker %d rows and %d columns out of reach,"
		" and ran the eye rect off the picture by the same amount",
		m_nCropY - nBestY, m_nCropX - nBestX);
	m_nCropX = nBestX;
	m_nCropY = nBestY;
	return true;
}

int WindowCapture::ReadFrameMarker(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
								   ID3D11Texture2D* pSrc, int nCropX, int nCropY)
{
	if (!pDev || !pCtx || !pSrc) return -1;

	// Eight data bits then a four-block sync, 1 0 1 0. The sync is what makes
	// a bad read detectable: without it a patch of wall decodes as a frame
	// number and the caller has no way to know.
	const UINT kBlock = 8, kBits = 8, kSync = 4;
	const UINT kAll = kBits + kSync;
	const UINT kW = kBlock * kAll, kH = kBlock;

	if (!m_MarkerStage)
	{
		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = kW; sd.Height = kH;
		sd.MipLevels = 1; sd.ArraySize = 1;
		sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(pDev->CreateTexture2D(&sd, nullptr, m_MarkerStage.put())))
		{
			Msg("frame marker: staging texture failed");
			return -1;
		}
	}

	// The marker sits at the very top-left of the client area - of THIS frame,
	// which is not necessarily one this object captured.
	D3D11_BOX box{};
	box.left = (UINT)nCropX;
	box.top  = (UINT)nCropY;
	box.right  = box.left + kW;
	box.bottom = box.top  + kH;
	box.front = 0; box.back = 1;
	pCtx->CopySubresourceRegion(m_MarkerStage.get(), 0, 0, 0, 0, pSrc, 0, &box);

	D3D11_MAPPED_SUBRESOURCE map{};
	if (FAILED(pCtx->Map(m_MarkerStage.get(), 0, D3D11_MAP_READ, 0, &map))) return -1;

	// Sample the centre of each block rather than a corner - the enlargement
	// and the renderer's own filtering both soften block edges.
	const uint8_t* pRow = (const uint8_t*)map.pData + (kBlock / 2) * map.RowPitch;

	int lum[16];
	int lo = 255, hi = 0;
	for (UINT b = 0; b < kAll; ++b)
	{
		const uint8_t* px = pRow + (b * kBlock + kBlock / 2) * 4;
		lum[b] = (px[0] + px[1] + px[2]) / 3;
		if (lum[b] < lo) lo = lum[b];
		if (lum[b] > hi) hi = lum[b];
	}
	pCtx->Unmap(m_MarkerStage.get(), 0);

	// Threshold against the marker's OWN black and white, not a constant. The
	// white swatch measures 170 on the desktop, not 255, and there is no
	// reason to assume the capture path leaves even that alone. The sync
	// guarantees at least one of each is present, so lo and hi are the
	// marker's two levels whenever this really is a marker.
	if (hi - lo < 48) return -1;			// no contrast: not a marker
	const int mid = (lo + hi) / 2;

	// The sync must read exactly 1 0 1 0.
	for (UINT s = 0; s < kSync; ++s)
	{
		const bool bOn = lum[kBits + s] > mid;
		if (bOn != ((s % 2) == 0)) return -1;
	}

	// Every block has to be near one level or the other. A gradient across the
	// corner can pass a contrast test and a sync test by luck; it cannot also
	// have every sample sitting on a level.
	const int band = (hi - lo) / 4;
	for (UINT b = 0; b < kAll; ++b)
	{
		if (lum[b] > lo + band && lum[b] < hi - band) return -1;
	}

	int nValue = 0;
	for (UINT b = 0; b < kBits; ++b)
	{
		if (lum[b] > mid) nValue |= (1 << b);
	}
	return nValue;
}

int WindowCapture::MeasureHalfShift(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
									ID3D11Texture2D* pSrc)
{
	// Every failure below says so. The first version of this returned -1 from
	// four places without a word, and a run that produced no output at all was
	// indistinguishable from one where the code never ran.
	if (!pDev || !pCtx || !pSrc || m_nEyeW < 64)
	{
		Msg("calibration: bad inputs (dev %d ctx %d src %d eyeW %d)",
			pDev ? 1 : 0, pCtx ? 1 : 0, pSrc ? 1 : 0, m_nEyeW);
		return -1;
	}

	// One row band spanning the FULL client width, so both halves come back in
	// a single readback and are automatically aligned in x.
	const UINT kRows = 4;
	const UINT kW = (UINT)(m_nEyeW * 2);
	const UINT kY = (UINT)(m_nCropY + m_nEyeH / 2);

	winrt::com_ptr<ID3D11Texture2D> stage;
	D3D11_TEXTURE2D_DESC sd{};
	sd.Width = kW; sd.Height = kRows;
	sd.MipLevels = 1; sd.ArraySize = 1;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	if (FAILED(pDev->CreateTexture2D(&sd, nullptr, stage.put())))
	{
		Msg("calibration: staging %ux%u failed", kW, kRows);
		return -1;
	}

	D3D11_BOX box{};
	box.left = (UINT)m_nCropX; box.right = box.left + kW;
	box.top = kY; box.bottom = kY + kRows;
	box.front = 0; box.back = 1;
	pCtx->CopySubresourceRegion(stage.get(), 0, 0, 0, 0, pSrc, 0, &box);

	D3D11_MAPPED_SUBRESOURCE map{};
	if (FAILED(pCtx->Map(stage.get(), 0, D3D11_MAP_READ, 0, &map)))
	{
		Msg("calibration: map failed");
		return -1;
	}
	Msg("calibration: reading %ux%u band at y=%u, eye %dx%d", kW, kRows, kY, m_nEyeW, m_nEyeH);

	// Luminance, averaged down the band to suppress noise. Comparing packed
	// RGB as integers - which the first attempt at this did - is not a
	// similarity measure at all: it is dominated by the red channel.
	std::vector<float> lum(kW, 0.0f);
	for (UINT r = 0; r < kRows; ++r)
	{
		const uint8_t* row = (const uint8_t*)map.pData + r * map.RowPitch;
		for (UINT x = 0; x < kW; ++x)
		{
			const uint8_t* p = row + x * 4;
			lum[x] += (0.114f * p[0] + 0.587f * p[1] + 0.299f * p[2]);
		}
	}
	pCtx->Unmap(stage.get(), 0);
	for (UINT x = 0; x < kW; ++x) lum[x] /= (float)kRows;

	const int nHalf = m_nEyeW;
	const int nMaxShift = nHalf / 3;

	// A positive yaw swings the view right, so the same feature sits FURTHER
	// LEFT in the right-hand image. Search that direction only.
	double fBest = 0.0;
	int    nBestShift = -1;
	double fSecond = 0.0;

	// From zero, so two identical halves report a shift of 0 and are obviously
	// wrong. Starting at 4 let an unyawed pair score 0.877 at the first shift
	// tried, which passed a correlation threshold and looked like a real
	// measurement.
	for (int s = 0; s < nMaxShift; ++s)
	{
		double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
		int n = 0;
		for (int x = s; x < nHalf; ++x)
		{
			const double a = lum[x];					// left half
			const double b = lum[nHalf + x - s];		// right half, shifted
			sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b;
			++n;
		}
		if (n < 64) break;

		// Normalised cross-correlation, so overall brightness differences
		// between the halves cannot masquerade as a match.
		const double num = n * sab - sa * sb;
		const double den = sqrt((n * saa - sa * sa) * (n * sbb - sb * sb));
		if (den <= 1e-6) continue;
		const double r = num / den;

		if (r > fBest) { fSecond = fBest; fBest = r; nBestShift = s; }
		else if (r > fSecond) { fSecond = r; }
	}

	// Reject a weak or ambiguous peak rather than reporting a number that
	// looks authoritative and is not.
	if (nBestShift < 0 || fBest < 0.5)
	{
		Msg("calibration: no clear match (best correlation %.2f) - needs a scene with detail", fBest);
		return -1;
	}

	// A 5 degree yaw must move the image tens of pixels. Anything tiny means
	// the two halves are the same picture, i.e. the yaw never reached the
	// render - which is a different failure from a weak correlation and has to
	// be reported as such rather than returned as a number.
	if (nBestShift < 8)
	{
		Msg("calibration: halves are IDENTICAL (best shift %d px, correlation %.3f)"
			" - the calibration yaw is not reaching the render",
			nBestShift, fBest);
		return -1;
	}
	Msg("calibration: shift %d px, correlation %.3f (next best %.3f)", nBestShift, fBest, fSecond);
	return nBestShift;
}

bool WindowCapture::GameAlive() const
{
	if (!m_hGameProc) return true;				// nothing to watch yet: assume alive
	return WaitForSingleObject(m_hGameProc, 0) == WAIT_TIMEOUT;
}

WindowCapture::~WindowCapture()
{
	if (m_hGameProc) { CloseHandle(m_hGameProc); m_hGameProc = nullptr; }
}

void WindowCapture::Stop()
{
	if (m_Frame)   { m_Frame.Close();   m_Frame = nullptr; }
	if (m_Session) { m_Session.Close(); m_Session = nullptr; }
	if (m_Pool)    { m_Pool.Close();    m_Pool = nullptr; }
	m_Item = nullptr;
}
