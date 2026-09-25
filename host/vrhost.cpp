// ----------------------------------------------------------------------- //
//
// MODULE  : vrhost.cpp
//
// PURPOSE : M4 transport probe. Captures the NOLF window with Windows Graphics
//           Capture, lands it as a D3D11 texture on the GPU, splits it into two
//           eye textures, and measures what that costs.
//
//           x64, because the OpenXR runtime it will eventually feed is x64 and
//           the 32-bit game cannot host it. Nothing here touches the game
//           process - capture is entirely external, so the D3D7 renderer is
//           left alone.
//
//           M4 only proves the pixels arrive and measures the cost. OpenXR
//           submission and head tracking are M5.
//
// ----------------------------------------------------------------------- //

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>

#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
using winrt::com_ptr;
using winrt::check_hresult;

// --------------------------------------------------------------------------
// Logging - same one-line-per-event, flush-always discipline as the client.
// --------------------------------------------------------------------------
namespace
{
	FILE*			g_pLog = nullptr;
	LARGE_INTEGER	g_Freq{}, g_Start{};

	double NowMs()
	{
		LARGE_INTEGER n;
		QueryPerformanceCounter(&n);
		return (double)(n.QuadPart - g_Start.QuadPart) * 1000.0 / (double)g_Freq.QuadPart;
	}

	void Log(const char* fmt, ...)
	{
		char buf[1024];
		va_list a;
		va_start(a, fmt);
		vsnprintf(buf, sizeof(buf), fmt, a);
		va_end(a);

		printf("[%9.3f] %s\n", NowMs(), buf);
		if (g_pLog)
		{
			fprintf(g_pLog, "[%9.3f] %s\n", NowMs(), buf);
			fflush(g_pLog);
		}
	}

	void OpenLog()
	{
		QueryPerformanceFrequency(&g_Freq);
		QueryPerformanceCounter(&g_Start);

		SYSTEMTIME st;
		GetLocalTime(&st);
		char dir[MAX_PATH], path[MAX_PATH];
		CreateDirectoryA("logs", nullptr);
		sprintf_s(dir, "logs\\host-%04d%02d%02d-%02d%02d%02d",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		CreateDirectoryA(dir, nullptr);
		sprintf_s(path, "%s\\host.log", dir);
		fopen_s(&g_pLog, path, "w");
		Log("log: %s", path);
	}

	template <typename T>
	com_ptr<T> GetDXGIInterface(winrt::Windows::Foundation::IInspectable const& obj)
	{
		auto access = obj.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
		com_ptr<T> out;
		check_hresult(access->GetInterface(winrt::guid_of<T>(), out.put_void()));
		return out;
	}

	// Match on window CLASS, not title. The engine creates its window titled
	// "NOLF" and only applies -windowtitle later in startup, so a title match
	// is a race that usually loses. The class "LithTech" is set at creation.
	HWND FindGameWindow()
	{
		HWND hwnd = FindWindowW(L"LithTech", nullptr);
		if (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) return hwnd;
		return nullptr;
	}

	// One-off proof that the pixels are real. Costs a full GPU->CPU readback,
	// which is exactly the expensive operation this design exists to avoid, so
	// it runs once and is timed separately.
	bool SaveTextureBmp(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
		ID3D11Texture2D* pTex, const char* pPath, double* pOutMs)
	{
		D3D11_TEXTURE2D_DESC desc{};
		pTex->GetDesc(&desc);

		D3D11_TEXTURE2D_DESC staging = desc;
		staging.Usage = D3D11_USAGE_STAGING;
		staging.BindFlags = 0;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		staging.MiscFlags = 0;

		com_ptr<ID3D11Texture2D> stage;
		if (FAILED(pDev->CreateTexture2D(&staging, nullptr, stage.put()))) return false;

		const double t0 = NowMs();
		pCtx->CopyResource(stage.get(), pTex);

		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(pCtx->Map(stage.get(), 0, D3D11_MAP_READ, 0, &map))) return false;
		if (pOutMs) *pOutMs = NowMs() - t0;

		const int w = (int)desc.Width, h = (int)desc.Height;
		const int rowBytes = w * 4;

		BITMAPFILEHEADER fh{};
		BITMAPINFOHEADER ih{};
		ih.biSize = sizeof(ih);
		ih.biWidth = w;
		ih.biHeight = -h;			// top-down
		ih.biPlanes = 1;
		ih.biBitCount = 32;
		ih.biCompression = BI_RGB;
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + rowBytes * h;

		FILE* f = nullptr;
		fopen_s(&f, pPath, "wb");
		if (!f) { pCtx->Unmap(stage.get(), 0); return false; }

		fwrite(&fh, sizeof(fh), 1, f);
		fwrite(&ih, sizeof(ih), 1, f);
		for (int y = 0; y < h; ++y)
		{
			fwrite((uint8_t*)map.pData + (size_t)y * map.RowPitch, 1, rowBytes, f);
		}
		fclose(f);
		pCtx->Unmap(stage.get(), 0);
		return true;
	}
}

int wmain(int argc, wchar_t** argv)
{
	// MUST come before any window measurement. A DPI-unaware process has every
	// coordinate Windows returns silently divided by the scaling factor - the
	// same mistake that produced a bogus 2560x1440 reading in M0, and again
	// here as a 1290x730 client rect for a 1920x1080 window.
	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	OpenLog();
	Log("=== NOLF1 VR host - M4 transport probe (x64) ===");

	const int nFrameTarget = (argc > 1) ? _wtoi(argv[1]) : 600;

	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	Log("winrt apartment ready");

	if (!wgc::GraphicsCaptureSession::IsSupported())
	{
		Log("FATAL: Windows Graphics Capture is not supported on this machine.");
		return 1;
	}
	Log("graphics capture supported");

	// --- D3D11 device -----------------------------------------------------
	// Set up before looking for the game, so the host can be started first and
	// simply wait. Costs ~140ms total.
	// Pick the adapter explicitly rather than letting D3D11CreateDevice choose.
	// This machine has three display adapters; the largest dedicated-VRAM one
	// is the GPU the headset will be on, which is what M5 needs.
	com_ptr<IDXGIFactory1> factory;
	check_hresult(CreateDXGIFactory1(winrt::guid_of<IDXGIFactory1>(), factory.put_void()));

	com_ptr<IDXGIAdapter1> chosen;
	SIZE_T bestVram = 0;
	for (UINT i = 0; ; ++i)
	{
		com_ptr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;

		DXGI_ADAPTER_DESC1 ad{};
		adapter->GetDesc1(&ad);
		const bool bSoftware = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
		Log("  adapter %u: %ls  vram %llu MB%s", i, ad.Description,
			(unsigned long long)(ad.DedicatedVideoMemory / (1024 * 1024)),
			bSoftware ? "  [software]" : "");

		if (!bSoftware && ad.DedicatedVideoMemory > bestVram)
		{
			bestVram = ad.DedicatedVideoMemory;
			chosen = adapter;
		}
	}
	if (!chosen) { Log("FATAL: no hardware adapter found"); return 1; }

	const double tDev = NowMs();
	com_ptr<ID3D11Device> device;
	com_ptr<ID3D11DeviceContext> context;
	check_hresult(D3D11CreateDevice(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
		device.put(), nullptr, context.put()));

	{
		DXGI_ADAPTER_DESC1 ad{};
		chosen->GetDesc1(&ad);
		Log("d3d11 device on %ls  (creation took %.0f ms)", ad.Description, NowMs() - tDev);
	}
	auto dxgiDevice = device.as<IDXGIDevice>();

	// --- now wait for the game --------------------------------------------
	HWND hwnd = nullptr;
	Log("waiting for a window of class \"LithTech\" ... (start the game now)");
	for (int i = 0; i < 600 && !hwnd; ++i)
	{
		hwnd = FindGameWindow();
		if (!hwnd) Sleep(500);
	}
	if (!hwnd)
	{
		Log("FATAL: no LithTech window found. Is the game running?");
		return 1;
	}

	// The engine creates its window small and resizes it during startup. The
	// frame pool is sized once at capture start, so grabbing the window too
	// early locks the whole session to a 314x160 frame. Wait for the client
	// area to reach a sensible size and hold it.
	{
		int lastW = -1, lastH = -1, nStable = 0;
		for (int i = 0; i < 300; ++i)
		{
			RECT rc{};
			GetClientRect(hwnd, &rc);
			const int w = rc.right - rc.left, h = rc.bottom - rc.top;

			if (w >= 640 && h >= 400 && w == lastW && h == lastH)
			{
				if (++nStable >= 6) break;		// ~600ms unchanged
			}
			else
			{
				nStable = 0;
			}
			lastW = w; lastH = h;
			Sleep(100);
		}
		Log("window settled at %dx%d", lastW, lastH);
	}

	wchar_t szTitle[256] = { 0 };
	GetWindowTextW(hwnd, szTitle, 256);

	// The captured texture covers the whole window, chrome included. Splitting
	// it down the middle would slice the title bar and borders, not the game's
	// eye seam. Locate the client area inside the DWM's frame bounds.
	RECT rcClient{};
	GetClientRect(hwnd, &rcClient);
	POINT ptClient{ 0, 0 };
	ClientToScreen(hwnd, &ptClient);

	RECT rcFrame{};
	if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rcFrame, sizeof(rcFrame))))
	{
		GetWindowRect(hwnd, &rcFrame);
	}

	const int clientW = rcClient.right - rcClient.left;
	const int clientH = rcClient.bottom - rcClient.top;
	const int cropX   = ptClient.x - rcFrame.left;
	const int cropY   = ptClient.y - rcFrame.top;

	Log("found window %p \"%ls\"", (void*)hwnd, szTitle);
	Log("  client %dx%d at screen (%d,%d)", clientW, clientH, ptClient.x, ptClient.y);
	Log("  dwm frame (%d,%d)-(%d,%d) = %dx%d",
		rcFrame.left, rcFrame.top, rcFrame.right, rcFrame.bottom,
		rcFrame.right - rcFrame.left, rcFrame.bottom - rcFrame.top);
	Log("  client offset inside capture: (%d,%d)", cropX, cropY);

	winrt::com_ptr<::IInspectable> inspectable;
	check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
	auto rtDevice = inspectable.as<wgdx::Direct3D11::IDirect3DDevice>();

	// --- capture item for the game window ---------------------------------
	auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
	wgc::GraphicsCaptureItem item{ nullptr };
	check_hresult(interop->CreateForWindow(hwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
		winrt::put_abi(item)));

	auto size = item.Size();
	Log("capture item size %dx%d", size.Width, size.Height);

	auto pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
		rtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
	auto session = pool.CreateCaptureSession(item);
	session.StartCapture();
	Log("capture started, collecting %d frames", nFrameTarget);

	// --- eye textures -----------------------------------------------------
	// Sized from the CLIENT area, not the capture item, so window chrome is
	// excluded and the split lands exactly on the stereo seam.
	const UINT eyeW = (UINT)clientW / 2;
	const UINT eyeH = (UINT)clientH;

	D3D11_TEXTURE2D_DESC eyeDesc{};
	eyeDesc.Width = eyeW;
	eyeDesc.Height = eyeH;
	eyeDesc.MipLevels = 1;
	eyeDesc.ArraySize = 1;
	eyeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	eyeDesc.SampleDesc.Count = 1;
	eyeDesc.Usage = D3D11_USAGE_DEFAULT;
	eyeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	// Shared so an OpenXR submission path can consume these at M5 without a
	// further copy. Proving the flag is accepted is part of the M4 gate.
	eyeDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

	com_ptr<ID3D11Texture2D> eyeTex[2];
	for (int i = 0; i < 2; ++i)
	{
		HRESULT hr = device->CreateTexture2D(&eyeDesc, nullptr, eyeTex[i].put());
		if (FAILED(hr))
		{
			Log("FATAL: eye texture %d creation failed 0x%08X", i, hr);
			return 1;
		}
	}
	Log("eye textures %ux%u created, D3D11_RESOURCE_MISC_SHARED accepted", eyeW, eyeH);

	// --- capture loop -----------------------------------------------------
	std::vector<double> splitMs;
	splitMs.reserve(nFrameTarget);

	int nFrames = 0, nEmpty = 0;
	bool bSaved = false;
	const double tLoopStart = NowMs();

	while (nFrames < nFrameTarget)
	{
		auto frame = pool.TryGetNextFrame();
		if (!frame)
		{
			++nEmpty;
			Sleep(1);
			if (NowMs() - tLoopStart > 60000.0) { Log("timed out waiting for frames"); break; }
			continue;
		}

		auto surface = frame.Surface();
		auto tex = GetDXGIInterface<ID3D11Texture2D>(surface);

		const double t0 = NowMs();

		// Split into per-eye textures. Entirely GPU-side - no readback.
		for (int i = 0; i < 2; ++i)
		{
			D3D11_BOX box{};
			box.left   = cropX + eyeW * i;
			box.right  = cropX + eyeW * (i + 1);
			box.top    = cropY;
			box.bottom = cropY + eyeH;
			box.front  = 0;
			box.back   = 1;
			context->CopySubresourceRegion(eyeTex[i].get(), 0, 0, 0, 0, tex.get(), 0, &box);
		}
		context->Flush();

		splitMs.push_back(NowMs() - t0);
		++nFrames;

		if (!bSaved && nFrames == 60)
		{
			double readbackMs = 0.0;
			if (SaveTextureBmp(device.get(), context.get(), eyeTex[0].get(), "logs\\eye-left.bmp", &readbackMs))
			{
				Log("wrote logs\\eye-left.bmp  (full GPU->CPU readback took %.3f ms)", readbackMs);
			}
			if (SaveTextureBmp(device.get(), context.get(), eyeTex[1].get(), "logs\\eye-right.bmp", &readbackMs))
			{
				Log("wrote logs\\eye-right.bmp (full GPU->CPU readback took %.3f ms)", readbackMs);
			}
			bSaved = true;
		}

		if (nFrames % 120 == 0) Log("  %d frames captured", nFrames);
	}

	session.Close();
	pool.Close();

	// --- results ----------------------------------------------------------
	const double elapsed = (NowMs() - tLoopStart) / 1000.0;
	if (!splitMs.empty())
	{
		std::sort(splitMs.begin(), splitMs.end());
		double sum = 0.0;
		for (double v : splitMs) sum += v;

		Log("");
		Log("=== results ===");
		Log("frames captured : %d in %.1fs  (%.1f fps)", nFrames, elapsed, nFrames / elapsed);
		Log("split to 2 eyes : avg %.3f ms  median %.3f ms  p99 %.3f ms  max %.3f ms",
			sum / splitMs.size(),
			splitMs[splitMs.size() / 2],
			splitMs[(size_t)(splitMs.size() * 0.99)],
			splitMs.back());
		Log("empty polls     : %d", nEmpty);
		Log("=== end ===");
	}
	else
	{
		Log("no frames captured");
	}

	if (g_pLog) fclose(g_pLog);
	return 0;
}
