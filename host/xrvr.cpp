// ----------------------------------------------------------------------- //
//
// MODULE  : xrvr.cpp   -   see xrvr.h
//
// ----------------------------------------------------------------------- //

#include "xrvr.h"
#include "hostlog.h"

#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

using HostLog::Msg;

namespace
{
	const float R2D = 57.2957795f;

	// Hamilton product. a then b applied in a's local frame.
	XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b)
	{
		XrQuaternionf r;
		r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
		r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
		r.y = a.w * b.y + a.y * b.w + a.z * b.x - a.x * b.z;
		r.z = a.w * b.z + a.z * b.w + a.x * b.y - a.y * b.x;
		return r;
	}

	// Rotation from an eye's forward axis to its optical centre.
	//
	// OpenXR view space is +X right, +Y up, -Z forward, and the fov angles are
	// signed so that right and up are positive. Turning -Z toward +X is a
	// NEGATIVE rotation about +Y, hence the sign on yaw; turning it toward +Y
	// is a positive rotation about +X.
	XrQuaternionf CentreQuat(float fYawRad, float fPitchRad)
	{
		const float sy = sinf(-fYawRad * 0.5f), cy = cosf(-fYawRad * 0.5f);
		const float sx = sinf(fPitchRad * 0.5f), cx = cosf(fPitchRad * 0.5f);

		XrQuaternionf q;			// q_yaw * q_pitch
		q.x =  cy * sx;
		q.y =  sy * cx;
		q.z = -sy * sx;
		q.w =  cy * cx;
		return q;
	}

	// Fullscreen-triangle blit. Needed because CopySubresourceRegion cannot
	// scale, and a menu drawn across the whole window has to be shrunk into an
	// eye-sized swapchain rather than cropped - cropping showed the middle of a
	// huge image with the edges cut off.
	const char* kBlitHLSL = R"(
cbuffer Src : register(b0) { float4 gRect; };   // x, y, w, h in texels
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    uint2 dim;
    gTex.GetDimensions(dim.x, dim.y);
    float2 t = (gRect.xy + i.uv * gRect.zw) / float2(dim);
    return gTex.SampleLevel(gSmp, t, 0);
}

// Catmull-Rom upscale.
//
// The runtime asks for 3072x3264 per eye and the renderer cannot produce
// anything close - past about 3 Mpix per eye the D3D7 path falls off a cliff.
// So the image is always being enlarged; the only question is by whom and how
// well. Handing the runtime a small image lets it do that with a basic filter,
// which is why low render resolutions look unsmoothed while 4K looked "so
// smooth" - at 4K there was almost nothing left to enlarge.
//
// Catmull-Rom keeps far more apparent detail than bilinear at the same source
// resolution, and its mild negative lobes counteract the softness rather than
// adding the halos a sharpening pass would. Nine bilinear taps, which is a few
// tenths of a millisecond at this size.
float3 CatmullRom(float2 texel, float2 dim)
{
    float2 tc  = floor(texel - 0.5) + 0.5;
    float2 f   = texel - tc;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 t0  = (tc - 1.0) / dim;
    float2 t3  = (tc + 2.0) / dim;
    float2 t12 = (tc + w2 / w12) / dim;

    float3 r = 0.0;
    r += gTex.SampleLevel(gSmp, float2(t0.x,  t0.y ), 0).rgb * (w0.x  * w0.y );
    r += gTex.SampleLevel(gSmp, float2(t12.x, t0.y ), 0).rgb * (w12.x * w0.y );
    r += gTex.SampleLevel(gSmp, float2(t3.x,  t0.y ), 0).rgb * (w3.x  * w0.y );
    r += gTex.SampleLevel(gSmp, float2(t0.x,  t12.y), 0).rgb * (w0.x  * w12.y);
    r += gTex.SampleLevel(gSmp, float2(t12.x, t12.y), 0).rgb * (w12.x * w12.y);
    r += gTex.SampleLevel(gSmp, float2(t3.x,  t12.y), 0).rgb * (w3.x  * w12.y);
    r += gTex.SampleLevel(gSmp, float2(t0.x,  t3.y ), 0).rgb * (w0.x  * w3.y );
    r += gTex.SampleLevel(gSmp, float2(t12.x, t3.y ), 0).rgb * (w12.x * w3.y );
    r += gTex.SampleLevel(gSmp, float2(t3.x,  t3.y ), 0).rgb * (w3.x  * w3.y );
    return r;
}

float4 PSUpscale(VSOut i) : SV_TARGET
{
    uint2 dim;
    gTex.GetDimensions(dim.x, dim.y);
    return float4(CatmullRom(gRect.xy + i.uv * gRect.zw, float2(dim)), 1.0);
}

// FXAA. A 2000-era renderer has no anti-aliasing of its own, and in a headset
// the resulting stairstepping crawls constantly because the head is never
// still. Driver-forced MSAA on D3D7 dropped the game to 4fps, so this runs on
// our side instead, costing a fraction of a millisecond.
float4 PSFxaa(VSOut i) : SV_TARGET
{
    uint2 dim;
    gTex.GetDimensions(dim.x, dim.y);
    float2 rcp = 1.0 / float2(dim);
    float2 t   = (gRect.xy + i.uv * gRect.zw) / float2(dim);

    float3 L = float3(0.299, 0.587, 0.114);

    float3 cNW = gTex.SampleLevel(gSmp, t + float2(-1,-1) * rcp, 0).rgb;
    float3 cNE = gTex.SampleLevel(gSmp, t + float2( 1,-1) * rcp, 0).rgb;
    float3 cSW = gTex.SampleLevel(gSmp, t + float2(-1, 1) * rcp, 0).rgb;
    float3 cSE = gTex.SampleLevel(gSmp, t + float2( 1, 1) * rcp, 0).rgb;
    float3 cM  = gTex.SampleLevel(gSmp, t, 0).rgb;

    float lNW = dot(cNW, L), lNE = dot(cNE, L);
    float lSW = dot(cSW, L), lSE = dot(cSE, L);
    float lM  = dot(cM,  L);

    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // Leave flat areas untouched - only work on edges.
    if ((lMax - lMin) < max(0.0312, lMax * 0.125)) return float4(cM, 1.0);

    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));

    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    float rcpMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcpMin, -8.0, 8.0) * rcp;

    float3 a = 0.5 * (gTex.SampleLevel(gSmp, t + dir * (1.0/3.0 - 0.5), 0).rgb +
                      gTex.SampleLevel(gSmp, t + dir * (2.0/3.0 - 0.5), 0).rgb);
    float3 b = a * 0.5 + 0.25 * (gTex.SampleLevel(gSmp, t + dir * -0.5, 0).rgb +
                                 gTex.SampleLevel(gSmp, t + dir *  0.5, 0).rgb);

    float lB = dot(b, L);
    return float4((lB < lMin || lB > lMax) ? a : b, 1.0);
}
)";

	void DumpTexture(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
					 ID3D11Texture2D* pTex, const char* pPath);
}

void HostDumpTexture(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
					 ID3D11Texture2D* pTex, const char* pPath)
{
	DumpTexture(pDev, pCtx, pTex, pPath);
}

namespace
{
	// Dumps a texture to a 32-bit BMP. Costs a full GPU->CPU readback, so it
	// runs only when explicitly requested.
	void DumpTexture(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
					 ID3D11Texture2D* pTex, const char* pPath)
	{
		D3D11_TEXTURE2D_DESC desc{};
		pTex->GetDesc(&desc);

		D3D11_TEXTURE2D_DESC st = desc;
		st.Usage = D3D11_USAGE_STAGING;
		st.BindFlags = 0;
		st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		st.MiscFlags = 0;

		ID3D11Texture2D* pStage = nullptr;
		if (FAILED(pDev->CreateTexture2D(&st, nullptr, &pStage)))
		{
			HostLog::Msg("dump: staging texture failed (format %d)", (int)desc.Format);
			return;
		}

		pCtx->CopyResource(pStage, pTex);

		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(pCtx->Map(pStage, 0, D3D11_MAP_READ, 0, &map))) { pStage->Release(); return; }

		const int w = (int)desc.Width, h = (int)desc.Height, rowBytes = w * 4;
		BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
		ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = -h;
		ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
		fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih);
		fh.bfSize = fh.bfOffBits + rowBytes * h;

		FILE* f = nullptr;
		fopen_s(&f, pPath, "wb");
		if (f)
		{
			fwrite(&fh, sizeof(fh), 1, f);
			fwrite(&ih, sizeof(ih), 1, f);
			for (int y = 0; y < h; ++y)
				fwrite((uint8_t*)map.pData + (size_t)y * map.RowPitch, 1, rowBytes, f);
			fclose(f);
			HostLog::Msg("dump: wrote %s (%dx%d, dxgi format %d)", pPath, w, h, (int)desc.Format);
		}
		pCtx->Unmap(pStage, 0);
		pStage->Release();
	}
}

bool XrVr::CreateInstance()
{
	// XR_FB_display_refresh_rate is optional: it lets us ASK the runtime to run
	// at 90 Hz rather than depending on a Virtual Desktop setting the player has
	// to find. Only request it if the runtime actually advertises it, because
	// naming an unsupported extension makes xrCreateInstance fail outright.
	uint32_t nExtCount = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &nExtCount, nullptr);
	std::vector<XrExtensionProperties> props(nExtCount, { XR_TYPE_EXTENSION_PROPERTIES });
	if (nExtCount) xrEnumerateInstanceExtensionProperties(nullptr, nExtCount, &nExtCount, props.data());

	m_bHaveRefreshExt = false;
	for (uint32_t i = 0; i < nExtCount; ++i)
		if (strcmp(props[i].extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0)
			m_bHaveRefreshExt = true;

	const char* exts[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
							XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME };

	XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ici.applicationInfo.applicationName, "NOLF1 VR");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = m_bHaveRefreshExt ? 2 : 1;
	ici.enabledExtensionNames = exts;
	if (!m_bHaveRefreshExt)
		Msg("note: runtime has no display-refresh-rate extension - the headset rate is whatever it is set to");

	if (XR_FAILED(xrCreateInstance(&ici, &m_Instance)))
	{
		Msg("FATAL: xrCreateInstance failed. Is the runtime installed and a headset connected?");
		m_nFail = 1;
		return false;
	}

	XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
	xrGetInstanceProperties(m_Instance, &ip);
	Msg("runtime: %s %u.%u.%u", ip.runtimeName,
		XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
		XR_VERSION_PATCH(ip.runtimeVersion));

	XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	XrResult r = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
	// SAY SO ON THE SCREEN. A player who starts the game before the headset
	// otherwise sees the menu drawn flat and stretched across the monitor for
	// a minute with nothing to say why - which reads as "it does not work".
	// A small notice, always on top, closed again the moment the headset
	// answers.
	HANDLE hNotice = nullptr;
	for (int i = 0; i < 120; ++i)
	{
		r = xrGetSystem(m_Instance, &sgi, &m_System);
		if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break;
		// A LINE EVERY TEN SECONDS, so a host that dies while waiting leaves
		// a time of death in its log rather than a log that simply stops.
		if (i > 0 && (i % 20) == 0) Msg("still waiting for a headset (%d s)", i / 2);
		if (i == 0)
		{
			Msg("no headset yet - waiting up to 60s.");
			hNotice = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
				MessageBoxW(nullptr,
					L"Waiting for your VR headset.\n\n"
					L"Put the headset on and start streaming (Virtual Desktop), or start "
					L"SteamVR or Oculus Link. The game appears in the headset as soon as "
					L"it connects - this message closes by itself.",
					L"NOLF VR - waiting for the headset",
					MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
				return 0; }, nullptr, 0, nullptr);
		}
		// KEEP IT IN FRONT. The game's full-screen window covered it at the
		// desk; the game keeps focus changes from its engine in VR mode, so
		// taking the foreground costs nothing.
		if (hNotice && (i % 4) == 1)
		{
			HWND hBox = FindWindowW(L"#32770", L"NOLF VR - waiting for the headset");
			if (hBox)
			{
				SetWindowPos(hBox, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
				SetForegroundWindow(hBox);
			}
		}
		Sleep(500);
	}
	if (hNotice)
	{
		// Close the notice whichever way this went.
		for (int k = 0; k < 20; ++k)
		{
			HWND hBox = FindWindowW(L"#32770", L"NOLF VR - waiting for the headset");
			if (!hBox) break;
			PostMessageW(hBox, WM_CLOSE, 0, 0);
			Sleep(50);
		}
		WaitForSingleObject(hNotice, 2000);
		CloseHandle(hNotice);
	}
	if (XR_FAILED(r)) { Msg("FATAL: no headset available (%d)", (int)r); m_nFail = 2; return false; }

	XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
	xrGetSystemProperties(m_Instance, m_System, &sp);
	Msg("system: %s", sp.systemName);

	XrViewConfigurationView vcv[2]{};
	for (int i = 0; i < 2; ++i) vcv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	uint32_t got = 0;
	xrEnumerateViewConfigurationViews(m_Instance, m_System,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &got, vcv);
	m_nRecommendedW = (int)vcv[0].recommendedImageRectWidth;
	m_nRecommendedH = (int)vcv[0].recommendedImageRectHeight;
	Msg("runtime recommends %dx%d per eye", m_nRecommendedW, m_nRecommendedH);

	PFN_xrGetD3D11GraphicsRequirementsKHR pfn = nullptr;
	xrGetInstanceProcAddr(m_Instance, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&pfn);
	if (!pfn) { Msg("FATAL: no D3D11 graphics requirements entry point"); return false; }

	XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	if (XR_FAILED(pfn(m_Instance, m_System, &req))) { Msg("FATAL: graphics requirements"); return false; }
	m_AdapterLuid = req.adapterLuid;
	Msg("runtime requires adapter LUID %08X:%08X",
		(unsigned)m_AdapterLuid.HighPart, (unsigned)m_AdapterLuid.LowPart);

	for (int i = 0; i < 2; ++i) m_Views[i].type = XR_TYPE_VIEW;
	return true;
}

bool XrVr::CreateSession(ID3D11Device* pDevice)
{
	XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	binding.device = pDevice;

	XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &binding;
	sci.systemId = m_System;
	if (XR_FAILED(xrCreateSession(m_Instance, &sci, &m_Session)))
	{
		Msg("FATAL: xrCreateSession failed");
		return false;
	}

	XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	if (XR_FAILED(xrCreateReferenceSpace(m_Session, &rsci, &m_Space)))
	{
		Msg("FATAL: xrCreateReferenceSpace failed");
		return false;
	}

	// The head, as a space. Only used by the head-as-mouse experiment, so its
	// absence disables that and nothing else - VIEW is required by the spec,
	// but a runtime that somehow refused it must not take the session with it.
	XrReferenceSpaceCreateInfo vsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	vsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	vsci.poseInReferenceSpace.orientation.w = 1.0f;
	if (XR_FAILED(xrCreateReferenceSpace(m_Session, &vsci, &m_ViewSpace)))
	{
		m_ViewSpace = XR_NULL_HANDLE;
		Msg("WARNING: no VIEW reference space - head-locked submission unavailable");
	}

	Msg("session created");
	return true;
}

bool XrVr::CreateSwapchains(int nWidth, int nHeight)
{
	// Enlarge on our side rather than the runtime's.
	//
	// The renderer cannot approach the runtime's recommended per-eye size, so
	// the image is always being enlarged somewhere. Handing over a small image
	// leaves that to the runtime's own filter; sizing the swapchain to what it
	// asked for lets us do it with Catmull-Rom instead. Same source pixels,
	// better reconstruction - which is the difference the player saw between
	// low resolutions looking unsmoothed and 4K looking markedly smoother.
	if (m_bUpscale && m_nRecommendedW > 0 && m_nRecommendedH > 0 &&
		(m_nRecommendedW > nWidth || m_nRecommendedH > nHeight))
	{
		// Capped at 1.5x the source, not the runtime's full request.
		//
		// Enlarging all the way to 3072x3264 was a 1.9x blow-up costing about
		// 3 ms of GPU per frame - and that GPU is shared with the game, which
		// is the side with no headroom. Measured: world render went from 7.7 to
		// 11 ms at the same render resolution, the game fell below 90 fps, and
		// fresh capture frames dropped from 60/sec to 50. Sharper pictures,
		// fewer of them.
		//
		// Most of the benefit of a better filter arrives in the first part of
		// the scale factor, so this keeps the reconstruction and gives back the
		// frames. The runtime enlarges whatever is left, as it did before.
		// 1.25, down from 1.5. At 1608x1676 per eye the 1.5x target was
		// 2412x2514 and the host's own work reached 5.66 ms - which shares a
		// GPU with the game, whose world render was 8.58 ms against an 11.1 ms
		// frame. Together they overran it, and the frame rate swung between 60
		// and 90.
		//
		// This is the second time the enlargement has been cut for the same
		// reason. The cost scales with the DESTINATION pixels, so it grows as
		// the render resolution grows - exactly when there is least room for it.
		const float kMaxScale = 1.25f;
		int nW = (int)((float)nWidth  * kMaxScale);
		int nH = (int)((float)nHeight * kMaxScale);
		if (nW > m_nRecommendedW) nW = m_nRecommendedW;
		if (nH > m_nRecommendedH) nH = m_nRecommendedH;

		Msg("swapchain %dx%d, enlarging %dx%d with Catmull-Rom (%.2fx; runtime wanted %dx%d)",
			nW, nH, nWidth, nHeight, (float)nW / (float)nWidth,
			m_nRecommendedW, m_nRecommendedH);
		m_nWidth  = nW;
		m_nHeight = nH;
	}
	else
	{
		m_nWidth  = nWidth;
		m_nHeight = nHeight;
	}

	// Ask the runtime what it supports rather than assuming. Submitting plain
	// UNORM where the runtime expects sRGB makes it treat our already-encoded
	// pixels as linear and convert again - washed out, blown highlights.
	uint32_t nFmts = 0;
	xrEnumerateSwapchainFormats(m_Session, 0, &nFmts, nullptr);
	std::vector<int64_t> fmts(nFmts);
	xrEnumerateSwapchainFormats(m_Session, nFmts, &nFmts, fmts.data());

	int64_t chosen = 0;
	for (uint32_t i = 0; i < nFmts; ++i)
	{
		Msg("  runtime swapchain format[%u] = %lld%s", i, (long long)fmts[i],
			(fmts[i] == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? "  (BGRA8 sRGB)" :
			(fmts[i] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ? "  (RGBA8 sRGB)" :
			(fmts[i] == DXGI_FORMAT_B8G8R8A8_UNORM)      ? "  (BGRA8 linear)" :
			(fmts[i] == DXGI_FORMAT_R8G8B8A8_UNORM)      ? "  (RGBA8 linear)" : "");
	}

	// The capture is BGRA8. Prefer the sRGB view of it - that is what the
	// runtime expects for content that is already display-encoded.
	const int64_t prefs[] = {
		DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
		DXGI_FORMAT_B8G8R8A8_UNORM,
		DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
		DXGI_FORMAT_R8G8B8A8_UNORM
	};
	for (int p = 0; p < 4 && !chosen; ++p)
		for (uint32_t i = 0; i < nFmts; ++i)
			if (fmts[i] == prefs[p]) { chosen = prefs[p]; break; }

	if (!chosen && nFmts) chosen = fmts[0];
	if (!chosen) { Msg("FATAL: runtime offered no swapchain formats"); return false; }
	Msg("using swapchain format %lld", (long long)chosen);
	m_nSwapFormat = chosen;
	m_bLoggedBlitWorld = false;
	m_bLoggedBlitPanel = false;
	m_bLoggedVpWorld   = false;
	m_bLoggedVpPanel   = false;

	// Sized to the game's per-eye image rather than the runtime's recommended
	// size. The engine cannot render 3072x3264 per eye, so the runtime upscales
	// instead - softer, but it removes any need to scale during the copy.
	for (int eye = 0; eye < 2; ++eye)
	{
		XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		sci.format      = chosen;
		sci.sampleCount = 1;
		sci.width       = m_nWidth;			// may exceed the source - see above
		sci.height      = m_nHeight;
		sci.faceCount   = 1;
		sci.arraySize   = 1;
		sci.mipCount    = 1;

		if (XR_FAILED(xrCreateSwapchain(m_Session, &sci, &m_Eyes[eye].handle)))
		{
			Msg("FATAL: xrCreateSwapchain eye %d at %dx%d", eye, nWidth, nHeight);
			return false;
		}

		uint32_t count = 0;
		xrEnumerateSwapchainImages(m_Eyes[eye].handle, 0, &count, nullptr);
		m_Eyes[eye].images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		xrEnumerateSwapchainImages(m_Eyes[eye].handle, count, &count,
			(XrSwapchainImageBaseHeader*)m_Eyes[eye].images.data());
	}

	// THE PAUSE OVERLAY'S CHAIN, at the source eye size exactly: SubmitEye
	// copies when sizes match and shades when they do not, and the shader
	// writes alpha 1 - which would make the quad an opaque card over the
	// world. The overlay is one eye wide, the same as an eye source.
	{
		XrSwapchainCreateInfo sci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		sci.format      = chosen;
		sci.sampleCount = 1;
		sci.width       = nWidth;
		sci.height      = nHeight;
		sci.faceCount   = 1;
		sci.arraySize   = 1;
		sci.mipCount    = 1;
		if (XR_FAILED(xrCreateSwapchain(m_Session, &sci, &m_Ovl.handle)))
		{
			Msg("WARNING: no swapchain for the pause overlay at %dx%d - the pause menu"
				" will not be a quad", nWidth, nHeight);
			m_Ovl.handle = XR_NULL_HANDLE;
		}
		else
		{
			uint32_t count = 0;
			xrEnumerateSwapchainImages(m_Ovl.handle, 0, &count, nullptr);
			m_Ovl.images.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
			xrEnumerateSwapchainImages(m_Ovl.handle, count, &count,
				(XrSwapchainImageBaseHeader*)m_Ovl.images.data());
			m_nOvlW = nWidth; m_nOvlH = nHeight;
		}
	}
	Msg("swapchains %dx%d, %u images each", nWidth, nHeight,
		(unsigned)m_Eyes[0].images.size());
	m_bLoggedSubRect = false;		// re-log the mapping after any resize
	return true;
}

bool XrVr::CreateBlitter(ID3D11Device* pDevice)
{
	m_pDevice = pDevice;

	ID3DBlob* pVSb = nullptr; ID3DBlob* pPSb = nullptr; ID3DBlob* pErr = nullptr;

	if (FAILED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), nullptr, nullptr, nullptr,
		"VSMain", "vs_4_0", 0, 0, &pVSb, &pErr)))
	{
		Msg("blitter: vertex shader failed: %s", pErr ? (const char*)pErr->GetBufferPointer() : "?");
		return false;
	}
	if (FAILED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), nullptr, nullptr, nullptr,
		"PSMain", "ps_4_0", 0, 0, &pPSb, &pErr)))
	{
		Msg("blitter: pixel shader failed: %s", pErr ? (const char*)pErr->GetBufferPointer() : "?");
		return false;
	}

	pDevice->CreateVertexShader(pVSb->GetBufferPointer(), pVSb->GetBufferSize(), nullptr, &m_pVS);
	pDevice->CreatePixelShader(pPSb->GetBufferPointer(), pPSb->GetBufferSize(), nullptr, &m_pPS);
	pVSb->Release(); pPSb->Release();

	ID3DBlob* pFXb = nullptr;
	if (SUCCEEDED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), nullptr, nullptr, nullptr,
		"PSFxaa", "ps_4_0", 0, 0, &pFXb, &pErr)))
	{
		pDevice->CreatePixelShader(pFXb->GetBufferPointer(), pFXb->GetBufferSize(), nullptr, &m_pPSFxaa);
		pFXb->Release();
	}
	else
	{
		Msg("blitter: FXAA shader failed: %s", pErr ? (const char*)pErr->GetBufferPointer() : "?");
	}

	ID3DBlob* pUPb = nullptr;
	if (SUCCEEDED(D3DCompile(kBlitHLSL, strlen(kBlitHLSL), nullptr, nullptr, nullptr,
		"PSUpscale", "ps_4_0", 0, 0, &pUPb, &pErr)))
	{
		pDevice->CreatePixelShader(pUPb->GetBufferPointer(), pUPb->GetBufferSize(), nullptr, &m_pPSUpscale);
		pUPb->Release();
	}
	else
	{
		Msg("blitter: upscale shader failed: %s", pErr ? (const char*)pErr->GetBufferPointer() : "?");
	}

	D3D11_SAMPLER_DESC sd{};
	sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD   = D3D11_FLOAT32_MAX;
	pDevice->CreateSamplerState(&sd, &m_pSampler);

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth      = 16;
	bd.Usage          = D3D11_USAGE_DYNAMIC;
	bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	pDevice->CreateBuffer(&bd, nullptr, &m_pCB);

	const bool ok = m_pVS && m_pPS && m_pSampler && m_pCB;
	Msg("blitter: %s", ok ? "ready (scaled blits available)" : "FAILED - menus will be cropped");
	return ok;
}

void XrVr::DestroySwapchains()
{
	if (m_Ovl.handle) { xrDestroySwapchain(m_Ovl.handle); m_Ovl.handle = XR_NULL_HANDLE; }
	m_Ovl.images.clear(); m_Ovl.submitted = false;
	for (int i = 0; i < 2; ++i)
	{
		if (m_Eyes[i].handle)
		{
			xrDestroySwapchain(m_Eyes[i].handle);
			m_Eyes[i].handle = XR_NULL_HANDLE;
		}
		m_Eyes[i].images.clear();
		m_Eyes[i].submitted = false;
	}
}

bool XrVr::PumpEvents()
{
	XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(m_Instance, &ev) == XR_SUCCESS)
	{
		if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			// THE RUNTIME RECENTRED (the headset's own gesture). LOCAL now has a
			// new origin under the head, so our own recenter offsets, which were
			// measured in the old LOCAL, are void: drop them and rebuild. The
			// client is told through nRecenterGen so it re-references height.
			auto* rc = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
			if (rc->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
			{
				m_fRecYaw = 0.0f;
				m_RecPos  = XrVector3f{ 0.0f, 0.0f, 0.0f };
				m_bRecDirty = true;
				m_bSpaceChanged = true;
				Msg("runtime recentred its LOCAL space - recenter offsets reset");
				if (m_bHaveRec) SetBodyYaw(m_fWantYaw, m_nYawMode);
			}
		}
		else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			auto* ss = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
			switch (ss->state)
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
				bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XR_SUCCEEDED(xrBeginSession(m_Session, &bi)))
				{
					m_bRunning = true;
					Msg("session running");
				}
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				xrEndSession(m_Session);
				m_bRunning = false;
				Msg("session stopping");
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				Msg("session exiting");
				return false;
			default:
				break;
			}
		}
		ev = { XR_TYPE_EVENT_DATA_BUFFER };
	}
	return true;
}

bool XrVr::BeginFrame(bool& bShouldRender)
{
	bShouldRender = false;
	if (!m_bRunning) return true;

	XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
	m_FrameState = { XR_TYPE_FRAME_STATE };
	if (XR_FAILED(xrWaitFrame(m_Session, &fwi, &m_FrameState))) return false;

	XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
	{
		const XrResult rb = xrBeginFrame(m_Session, &fbi);
		if (XR_FAILED(rb) && m_nBeginFail++ == 0)
			Msg("ERROR: xrBeginFrame failed (%d) - first of possibly many, counted from here", (int)rb);
	}

	m_bViewsValid = false;
	if (m_FrameState.shouldRender)
	{
		XrViewState vs{ XR_TYPE_VIEW_STATE };
		XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime = m_FrameState.predictedDisplayTime;
		// Poses are declared in the yaw-carrying space when it is live, so
			// they describe the image in the frame it was rendered in.
			vli.space = (m_GameSpace != XR_NULL_HANDLE) ? m_GameSpace : m_Space;

		uint32_t got = 0;
		if (XR_SUCCEEDED(xrLocateViews(m_Session, &vli, &vs, 2, &got, m_Views)))
		{
			m_bViewsValid =
				(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
				(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

			// The runtime's ACTUAL per-eye frustum, once. The engine can only
			// render a symmetric frustum, so an off-centre eye costs us the
			// widest edge in every direction - and how much it costs cannot be
			// worked out from the one eye we happen to publish downstream.
			if (m_bViewsValid && !m_bLoggedEyeFov)
			{
				m_bLoggedEyeFov = true;
				for (int e = 0; e < 2; ++e)
				{
					const XrFovf& f = m_Views[e].fov;
					const float l = f.angleLeft * R2D, r = f.angleRight * R2D;
					const float u = f.angleUp   * R2D, d = f.angleDown  * R2D;
					Msg("eye %d frustum: L%.1f R%.1f U%.1f D%.1f  -> span %.1f x %.1f, centre (%.1f, %.1f)",
						e, l, r, u, d, r - l, u - d, (l + r) * 0.5f, (u + d) * 0.5f);
					Msg("    symmetric cover needs %.1f x %.1f (we waste %.0f%% of the pixels)",
						2.0f * ((fabsf(l) > fabsf(r)) ? fabsf(l) : fabsf(r)),
						2.0f * ((fabsf(u) > fabsf(d)) ? fabsf(u) : fabsf(d)),
						100.0f * (1.0f - ((r - l) * (u - d)) /
							(4.0f * ((fabsf(l) > fabsf(r)) ? fabsf(l) : fabsf(r))
								  * ((fabsf(u) > fabsf(d)) ? fabsf(u) : fabsf(d)))));
				}
			}

			// The same eyes measured against the head. Located every frame
			// rather than only while head-locked, because the client can turn
			// the experiment on between our BeginFrame and our EndFrame and
			// this must already be in hand when it does.
			m_bViewsHeadValid = false;
			if (m_bViewsValid && m_ViewSpace)
			{
				XrViewState hvs{ XR_TYPE_VIEW_STATE };
				XrViewLocateInfo hvli{ XR_TYPE_VIEW_LOCATE_INFO };
				hvli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				hvli.displayTime = m_FrameState.predictedDisplayTime;
				hvli.space = m_ViewSpace;

				uint32_t hgot = 0;
				for (int e = 0; e < 2; ++e) m_ViewsHead[e] = { XR_TYPE_VIEW };
				if (XR_SUCCEEDED(xrLocateViews(m_Session, &hvli, &hvs, 2, &hgot, m_ViewsHead)))
				{
					m_bViewsHeadValid =
						(hvs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
						(hvs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
				}
			}

			if (m_bViewsValid)
			{
				PoseSample& slot = m_PoseRing[m_nPoseWrite];
				slot.fTimeMs = HostLog::NowMs();
				slot.nFrame  = m_nPoseFrame;
				slot.pose[0] = m_Views[0].pose;
				slot.pose[1] = m_Views[1].pose;
				m_nPoseWrite = (m_nPoseWrite + 1) % kPoseRing;
			}
		}
		bShouldRender = m_bViewsValid;
	}
	return true;
}

bool XrVr::SubmitEye(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pSrc,
					 int nEye, int nSrcX, int nSrcY, int nWidth, int nHeight,
					 bool bPanel)
{
	EyeChain& eye = (nEye >= 2) ? m_Ovl : m_Eyes[nEye];
	eye.submitted = false;
	if (!eye.handle || eye.images.empty()) return false;

	XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if (XR_FAILED(xrAcquireSwapchainImage(eye.handle, &ai, &eye.acquired))) return false;

	XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wi.timeout = XR_INFINITE_DURATION;
	if (XR_FAILED(xrWaitSwapchainImage(eye.handle, &wi))) return false;

	// With FXAA on, everything goes through the shader path - even a same-size
	// copy, since the plain copy cannot filter.
	// The overlay never takes the shader path: it must keep its alpha.
	const bool bNeedShader = (nEye >= 2)
		? ((nWidth != m_nOvlW) || (nHeight != m_nOvlH))
		: ((nWidth != m_nWidth) || (nHeight != m_nHeight) || (m_bAntiAlias && m_pPSFxaa));
	// Say so, once per mismatch, if the overlay is about to lose its alpha:
	// that is the opaque black pause card (vrmain.cpp, the swapchain check).
	if (nEye >= 2 && bNeedShader)
	{
		static int s_nSaidW = 0, s_nSaidH = 0;
		if (s_nSaidW != m_nOvlW || s_nSaidH != m_nOvlH)
		{
			s_nSaidW = m_nOvlW; s_nSaidH = m_nOvlH;
			Msg("WARNING: pause overlay %dx%d into a %dx%d chain - scaled, so it is OPAQUE",
				nWidth, nHeight, m_nOvlW, m_nOvlH);
		}
	}

	if (!bNeedShader)
	{
		// Same size - a straight GPU copy, which is the normal in-world path.
		D3D11_BOX box{};
		box.left   = nSrcX;
		box.right  = nSrcX + nWidth;
		box.top    = nSrcY;
		box.bottom = nSrcY + nHeight;
		box.front  = 0;
		box.back   = 1;
		pCtx->CopySubresourceRegion(eye.images[eye.acquired].texture, 0, 0, 0, 0, pSrc, 0, &box);
	}
	else if (m_pVS && m_pPS)
	{
		// Different size - scale it in. Used for menus, which span the whole
		// window and must be shrunk into an eye rather than cropped.
		// A captured WGC texture is not guaranteed to allow shader binding, so
		// CreateShaderResourceView on it fails and the shader samples nothing -
		// leaving the black clear. Stage through a texture we create ourselves
		// with BIND_SHADER_RESOURCE. One extra GPU copy, only on menu frames.
		D3D11_TEXTURE2D_DESC srcDesc{};
		pSrc->GetDesc(&srcDesc);

		if (!m_pStage || m_nStageW != (int)srcDesc.Width || m_nStageH != (int)srcDesc.Height)
		{
			if (m_pStage) { m_pStage->Release(); m_pStage = nullptr; }

			D3D11_TEXTURE2D_DESC sd = srcDesc;
			sd.Usage          = D3D11_USAGE_DEFAULT;
			sd.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
			sd.CPUAccessFlags = 0;
			sd.MiscFlags      = 0;

			const HRESULT hr = m_pDevice->CreateTexture2D(&sd, nullptr, &m_pStage);
			if (FAILED(hr))
			{
				Msg("blit: staging texture %ux%u failed 0x%08X - menus will be black",
					srcDesc.Width, srcDesc.Height, hr);
			}
			m_nStageW = (int)srcDesc.Width;
			m_nStageH = (int)srcDesc.Height;
		}
		if (!m_pStage) return false;
		pCtx->CopyResource(m_pStage, pSrc);

		ID3D11RenderTargetView* pRTV = nullptr;
		ID3D11ShaderResourceView* pSRV = nullptr;
		// The runtime hands back TYPELESS textures, so a null RTV desc fails
		// with E_INVALIDARG - the format has to be named explicitly. This was
		// why menus were black even after the shader could sample its source.
		// Use the NON-sRGB view of the swapchain format.
		//
		// The captured pixels are already display-encoded. Writing them through
		// an sRGB render target makes the hardware encode them a second time -
		// bright and washed out. The plain copy path never had this problem
		// because a raw bit copy does no conversion, which is why the fault
		// reappeared the moment FXAA routed world frames through the shader.
		DXGI_FORMAT rtvFormat = (DXGI_FORMAT)m_nSwapFormat;
		switch (rtvFormat)
		{
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: rtvFormat = DXGI_FORMAT_B8G8R8A8_UNORM; break;
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
		default: break;
		}

		D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
		rtvd.Format        = rtvFormat;
		rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvd.Texture2D.MipSlice = 0;

		const HRESULT hrRTV = m_pDevice->CreateRenderTargetView(
			eye.images[eye.acquired].texture, &rtvd, &pRTV);
		const HRESULT hrSRV = m_pDevice->CreateShaderResourceView(m_pStage, nullptr, &pSRV);

		// Log the WORLD case and the PANEL case separately. One shared flag
		// fired on whichever came first - always the menu, which is up before
		// the world - so the world's own blit was never described, and the
		// panel shrink it was wrongly getting never appeared in any log.
		bool& bLogged   = bPanel ? m_bLoggedBlitPanel : m_bLoggedBlitWorld;
		bool& bLoggedVp = bPanel ? m_bLoggedVpPanel   : m_bLoggedVpWorld;
		if (!bLogged)
		{
			bLogged = true;
			Msg("blit[%s]: rtv 0x%08X srv 0x%08X, scaling %dx%d -> %dx%d",
				bPanel ? "panel" : "WORLD", hrRTV, hrSRV,
				nWidth, nHeight, m_nWidth, m_nHeight);
		}

		if (SUCCEEDED(hrRTV) && SUCCEEDED(hrSRV))
		{
			D3D11_MAPPED_SUBRESOURCE map{};
			if (SUCCEEDED(pCtx->Map(m_pCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
			{
				float* p = (float*)map.pData;
				p[0] = (float)nSrcX; p[1] = (float)nSrcY;
				p[2] = (float)nWidth; p[3] = (float)nHeight;
				pCtx->Unmap(m_pCB, 0);
			}

			// Preserve aspect - a full-window menu is much wider than an eye,
			// and stretching it to fit would squash it by 2x. Letterbox into
			// the largest correctly-proportioned area and black out the rest.
			const float fSrcAspect = (float)nWidth / (float)nHeight;
			float fW = (float)m_nWidth;
			float fH = fW / fSrcAspect;
			if (fH > (float)m_nHeight)
			{
				fH = (float)m_nHeight;
				fW = fH * fSrcAspect;
			}

			// Shrink so the whole panel sits inside what the headset actually
			// displays. We render ~108 degrees but the runtime shows ~94, so a
			// panel filling the render is clipped at the edges - the menu read
			// as still slightly zoomed in.
			//
			// PANELS ONLY. This shader path is not the menu path - it is taken
			// for ANY frame whose source size differs from the swapchain, and
			// for every frame at all once FXAA is on. Both are true in the
			// ordinary in-world case, because the swapchain is deliberately
			// larger than the eye so the Catmull-Rom upscale has somewhere to
			// go. Applying the panel shrink there put the world in a black box
			// filling 72% of each eye, and - because the declared per-eye
			// frustum is ASYMMETRIC, so its optical axis is not the middle of
			// the image - shrinking about the image centre moved each eye's
			// forward direction ~4.3 degrees the opposite way, ~8.7 degrees of
			// divergence. That was reported as a flatscreen in a box AND as
			// double vision, and it was this one line.
			if (bPanel)
			{
				fW *= m_fPanelScale;
				fH *= m_fPanelScale;
			}

			const float clearCol[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			pCtx->ClearRenderTargetView(pRTV, clearCol);

			D3D11_VIEWPORT vp{
				((float)m_nWidth  - fW) * 0.5f,
				((float)m_nHeight - fH) * 0.5f,
				fW, fH, 0.0f, 1.0f };

			if (!bLoggedVp)
			{
				bLoggedVp = true;
				Msg("  viewport %s: %.0fx%.0f at (%.0f,%.0f) of %dx%d"
					" - fills %.1f%% x %.1f%%%s",
					bPanel ? "panel" : "WORLD", fW, fH, vp.TopLeftX, vp.TopLeftY,
					m_nWidth, m_nHeight,
					fW * 100.0f / (float)m_nWidth, fH * 100.0f / (float)m_nHeight,
					(!bPanel && fW < (float)m_nWidth * 0.99f)
						? "   <- THE WORLD IS IN A BOX" : "");
			}
			pCtx->OMSetRenderTargets(1, &pRTV, nullptr);
			pCtx->RSSetViewports(1, &vp);
			pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			pCtx->IASetInputLayout(nullptr);
			pCtx->VSSetShader(m_pVS, nullptr, 0);
			// Enlarging: Catmull-Rom reconstructs better than the bilinear tap
			// FXAA would use, and at this scale factor the enlargement itself
			// smooths the stairstepping FXAA exists to hide. Running FXAA on
			// top would only soften the result twice.
			const bool bEnlarging = (m_nWidth > nWidth) || (m_nHeight > nHeight);
			ID3D11PixelShader* pPS =
				(bEnlarging && m_pPSUpscale)      ? m_pPSUpscale :
				(m_bAntiAlias && m_pPSFxaa)       ? m_pPSFxaa    : m_pPS;
			pCtx->PSSetShader(pPS, nullptr, 0);
			pCtx->PSSetConstantBuffers(0, 1, &m_pCB);
			pCtx->PSSetShaderResources(0, 1, &pSRV);
			pCtx->PSSetSamplers(0, 1, &m_pSampler);
			pCtx->Draw(3, 0);

			ID3D11ShaderResourceView* pNull = nullptr;
			pCtx->PSSetShaderResources(0, 1, &pNull);
			pCtx->OMSetRenderTargets(0, nullptr, nullptr);
		}
		if (pSRV) pSRV->Release();
		if (pRTV) pRTV->Release();
	}

	if (m_nDumpPending > 0)
	{
		--m_nDumpPending;
		ID3D11Device* pDev = nullptr;
		pCtx->GetDevice(&pDev);
		if (pDev)
		{
			char path[MAX_PATH];
			sprintf_s(path, "logs\\submitted-eye%d-%d.bmp", nEye, m_nDumpIndex);
			DumpTexture(pDev, pCtx, eye.images[eye.acquired].texture, path);
			pDev->Release();
		}
	}

	XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	{
		const XrResult rr = xrReleaseSwapchainImage(eye.handle, &ri);
		if (XR_FAILED(rr) && m_nReleaseFail++ == 0)
			Msg("ERROR: xrReleaseSwapchainImage failed for eye %d (%d)", nEye, (int)rr);
	}

	eye.submitted = true;
	return true;
}

void XrVr::AnchorPausePanel()
{
	if (!m_bViewsValid) { m_bPauseAnchored = false; return; }
	XrPosef head = m_Views[0].pose;
	head.position = HeadMidpoint();
	const XrQuaternionf& q = head.orientation;
	const float yaw = atan2f(2.0f * (q.w * q.y + q.z * q.x),
							 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	m_PausePose.orientation.x = 0.0f;
	m_PausePose.orientation.y = sinf(yaw * 0.5f);
	m_PausePose.orientation.z = 0.0f;
	m_PausePose.orientation.w = cosf(yaw * 0.5f);
	const float dist = m_fMenuDist;
	m_PausePose.position.x = head.position.x + sinf(yaw) * -dist;
	m_PausePose.position.y = head.position.y;
	m_PausePose.position.z = head.position.z + cosf(yaw) * -dist;
	m_bPauseAnchored = true;
	Msg("pause panel anchored at (%.2f, %.2f, %.2f), yaw %.1f deg - it stays there",
		m_PausePose.position.x, m_PausePose.position.y, m_PausePose.position.z, yaw * R2D);
}

void XrVr::AnchorMenuPanel()
{
	if (!m_bViewsValid) { m_bMenuAnchored = false; return; }

	// Place the panel a couple of metres ahead of the head, upright - yaw only,
	// so it does not inherit pitch or roll and end up tilted.
	XrPosef head = m_Views[0].pose;
	head.position = HeadMidpoint();			// not the left eye - see HeadMidpoint
	const XrQuaternionf& q = head.orientation;

	const float yaw = atan2f(2.0f * (q.w * q.y + q.z * q.x),
							 1.0f - 2.0f * (q.x * q.x + q.y * q.y));

	m_MenuPose.orientation.x = 0.0f;
	m_MenuPose.orientation.y = sinf(yaw * 0.5f);
	m_MenuPose.orientation.z = 0.0f;
	m_MenuPose.orientation.w = cosf(yaw * 0.5f);

	const float dist = m_fMenuDist;
	m_MenuPose.position.x = head.position.x + sinf(yaw) * -dist;
	m_MenuPose.position.y = head.position.y;
	m_MenuPose.position.z = head.position.z + cosf(yaw) * -dist;

	m_AnchorHead    = head.position;
	m_bMenuAnchored = true;
	Msg("menu panel anchored at (%.2f, %.2f, %.2f), yaw %.1f deg",
		m_MenuPose.position.x, m_MenuPose.position.y, m_MenuPose.position.z, yaw * R2D);
}

void XrVr::UpdateMenuAnchor()
{
	if (!m_bViewsValid) return;
	if (!m_bMenuAnchored) { AnchorMenuPanel(); return; }

	XrPosef head = m_Views[0].pose;
	head.position = HeadMidpoint();			// not the left eye - see HeadMidpoint

	// Re-anchor if the head has moved a long way from where the panel was
	// placed. At startup the menu flag goes true before the headset is on the
	// player's face, so the panel locks to wherever it was sitting - typically
	// on a desk, which puts it near the floor once he stands up.
	const float dx = head.position.x - m_AnchorHead.x;
	const float dy = head.position.y - m_AnchorHead.y;
	const float dz = head.position.z - m_AnchorHead.z;
	const float moved = sqrtf(dx * dx + dy * dy + dz * dz);

	// Also re-anchor if the panel has ended up far outside the view. This is
	// not gaze-following - the threshold is deliberately wide, so the panel
	// stays put for normal head movement and only recovers when it is
	// somewhere the player cannot reasonably be looking.
	const XrQuaternionf& q = head.orientation;
	const float headYaw = atan2f(2.0f * (q.w * q.y + q.z * q.x),
								 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	const float toPanel = atan2f(m_MenuPose.position.x - head.position.x,
								 m_MenuPose.position.z - head.position.z);
	float off = headYaw - (toPanel + 3.14159265f);
	while (off >  3.14159265f) off -= 6.28318531f;
	while (off < -3.14159265f) off += 6.28318531f;

	if (moved > 0.6f || fabsf(off) > 1.22f)		// 0.6m, or ~70 degrees
	{
		Msg("menu panel re-anchored (head moved %.2f m, off-axis %.0f deg)",
			moved, fabsf(off) * R2D);
		AnchorMenuPanel();
	}
}

void XrVr::EndFrame(bool bHaveImage, bool bMenu, bool bPauseQuad)
{
	if (!m_bRunning) return;

	// --- menus: a world-locked quad, not a projection layer ----------------
	// Drawn into the eyes, a menu is welded to the head and drags with every
	// movement. A quad sits in space and the head moves around it.
	if (bMenu && bHaveImage && m_bMenuAnchored && m_Eyes[0].submitted)
	{
		XrCompositionLayerQuad quad{ XR_TYPE_COMPOSITION_LAYER_QUAD };
		quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quad.space      = m_Space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = m_Eyes[0].handle;
		quad.subImage.imageRect.offset = { 0, 0 };
		quad.subImage.imageRect.extent = { m_nWidth, m_nHeight };
		quad.subImage.imageArrayIndex = 0;
		quad.pose = m_MenuPose;

		// Sized by --menu-width, and placed at --menu-dist by AnchorMenuPanel.
		// The height follows from the image's aspect so the picture is never
		// stretched.
		quad.size.width  = m_fMenuWidth;
		quad.size.height = m_fMenuWidth * (float)m_nHeight / (float)m_nWidth;

		const XrCompositionLayerBaseHeader* qLayers[1] =
			{ (const XrCompositionLayerBaseHeader*)&quad };

		XrFrameEndInfo fq{ XR_TYPE_FRAME_END_INFO };
		fq.displayTime = m_FrameState.predictedDisplayTime;
		fq.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fq.layerCount = 1;
		fq.layers = qLayers;

		// Quad layers are where runtimes differ most. VirtualDesktopXR rejects
		// per-eye eyeVisibility on them outright - the symptom is a headset
		// frozen on the last good frame while the game runs fine underneath,
		// which is indistinguishable from a hang unless the result is counted.
		// We submit EYE_VISIBILITY_BOTH for that reason; this makes sure we
		// would find out if it were refused anyway.
		const double tq0 = HostLog::NowMs();
		const XrResult rq = xrEndFrame(m_Session, &fq);
		m_fLastEndCallMs = HostLog::NowMs() - tq0;
		if (XR_FAILED(rq) && m_nEndMenuFail++ == 0)
			Msg("ERROR: the runtime rejected the menu quad layer (xrEndFrame %d)", (int)rq);
		return;
	}

	XrCompositionLayerProjectionView views[2]{};
	XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	const XrCompositionLayerBaseHeader* layers[2] = { nullptr, nullptr };
	uint32_t layerCount = 0;

	const bool bBoth = bHaveImage && m_bViewsValid &&
					   m_Eyes[0].submitted && m_Eyes[1].submitted;

	// Head-locked: the client put the head rotation into its own camera, so the
	// image is already correct for where the head is pointing and the runtime
	// must be told to leave it alone. Declaring it in VIEW space does exactly
	// that - the poses are relative to the head, so there is no difference
	// between the declared reference and the display-time head for the
	// compositor to warp away.
	//
	// The pose ring, VRPoseLag and the exact-pose marker all become irrelevant
	// here: they exist to name the world pose an image was rendered from, and a
	// head-locked image is not in the world.
	const bool bHeadLock = m_bHeadLockWanted && m_bHeadLockOk &&
						   m_ViewSpace && m_bViewsHeadValid;

	if (bBoth)
	{
		// Find the pose closest to when the displayed frame was rendered.
		// Submitting the current pose with a stale image tells the runtime the
		// image is newer than it is, so reprojection corrects nothing and head
		// roll shears the picture.
		const PoseSample* pBest = nullptr;

		// Prefer the pose the displayed image was ACTUALLY rendered from, read
		// off the client's frame marker. Measured staleness averages ~12 ms but
		// ranges from 6 to 67, so the spread exceeds the mean and no fixed lag
		// can be right - which is exactly why every sweep of VRPoseLag came
		// back "they all feel the same" and why guessing the pose once made
		// things unplayable. This is the same correction with the guess removed.
		if (m_nSubmitByte >= 0)
		{
			double fNewest = -1.0;
			for (int i = 0; i < kPoseRing; ++i)
			{
				const PoseSample& s = m_PoseRing[i];
				if (s.fTimeMs < 0.0) continue;
				if ((s.nFrame & 0xFFu) != (uint32_t)m_nSubmitByte) continue;
				if (s.fTimeMs > fNewest) { fNewest = s.fTimeMs; pBest = &s; }
			}

			// An image is not half a second old in a pipeline whose own
			// timestamp instrument measures 8 ms and whose good blocks measure
			// 12 to 30. A match that says otherwise is a decode that happened
			// to land on a real frame number, and acting on it means asking
			// the compositor to rotate the picture by every degree the head
			// has turned in that time - which is strictly worse than not
			// correcting at all. Fail open to the newest pose and say so.
			if (pBest)
			{
				const double fAge = HostLog::NowMs() - fNewest;
				if (fAge > kMaxPoseAgeMs)
				{
					pBest = nullptr;
					++m_nPoseTooOld;
					if (m_nPoseTooOld == 1 || (m_nPoseTooOld % 500) == 0)
						Msg("frame marker: match was %.0f ms old (cap %.0f) - using the"
							" newest pose instead; %u so far",
							fAge, kMaxPoseAgeMs, m_nPoseTooOld);
				}
			}
		}

		// No marker, or no match: the old time-based estimate.
		if (!pBest)
		{
			const double fWant = HostLog::NowMs() - m_fPoseLagMs;
			double fBestErr = 1e9;
			for (int i = 0; i < kPoseRing; ++i)
			{
				if (m_PoseRing[i].fTimeMs < 0.0) continue;
				const double err = fabs(m_PoseRing[i].fTimeMs - fWant);
				if (err < fBestErr) { fBestErr = err; pBest = &m_PoseRing[i]; }
			}
		}

		// The symmetric frustum the client rendered. Must match the client's
		// own calculation: the maximum absolute angle on each axis, so the
		// symmetric render always CONTAINS the runtime's asymmetric frustum.
		const XrFovf& f0 = m_Views[0].fov;
		float hx = (fabsf(f0.angleLeft) > fabsf(f0.angleRight))
				 ? fabsf(f0.angleLeft) : fabsf(f0.angleRight);
		float hy = ((fabsf(f0.angleUp) > fabsf(f0.angleDown))
				 ? fabsf(f0.angleUp) : fabsf(f0.angleDown)) * m_fFovYScale;

		// If the client has told us what it actually rendered, believe it
		// rather than reconstructing it. The client owns its own FOV and can
		// be tuned; this keeps the declaration in step automatically.
		if (m_fClientFovX > 0.0f && m_fClientFovY > 0.0f)
		{
			hx = m_fClientFovX * 0.5f;
			hy = m_fClientFovY * 0.5f;
		}

		const float tx = tanf(hx);
		const float ty = tanf(hy);

		// Print what is actually going to the runtime, every few seconds
		// rather than once at startup. The declaration is now the prime
		// suspect for the warping - the desktop image is correct and only the
		// headset is distorted - and a value logged once cannot show whether
		// it drifts or disagrees with the client over a run.
		{
			static double s_fNextFovLog = 0.0;
			const double fNow = HostLog::NowMs();
			if (fNow > s_fNextFovLog)
			{
				s_fNextFovLog = fNow + 10000.0;
				Msg("  SUBMITTING fov +/-%.2f x +/-%.2f deg (image %dx%d, aspect %.3f,"
					" tan ratio %.3f)%s",
					hx * R2D, hy * R2D, m_nWidth, m_nHeight,
					(float)m_nWidth / (float)m_nHeight, tx / ty,
					(fabsf((tx / ty) - ((float)m_nWidth / (float)m_nHeight)) > 0.05f)
						? "   <- ASPECT MISMATCH" : "");
			}
		}

		for (int eye = 0; eye < 2; ++eye)
		{
			views[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
			views[eye].pose = bHeadLock ? m_ViewsHead[eye].pose
										: (pBest ? pBest->pose[eye] : m_Views[eye].pose);

			// When the client renders about this eye's optical centre rather
			// than its forward axis, the declared pose has to carry the same
			// rotation or the two disagree - and a mismatch here is worse than
			// doing neither, which is what ProjMode 1 demonstrated.
			if (m_bAsymActive)
			{
				const XrQuaternionf qc = CentreQuat(m_fAsymYaw[eye], m_fAsymPitch[eye]);
				views[eye].pose.orientation = QuatMul(views[eye].pose.orientation, qc);
			}

			// Declare the runtime's OWN asymmetric frustum, and hand it the
			// sub-rectangle of our symmetric image that corresponds to it.
			//
			// The Quest's per-eye frustums are asymmetric and mirrored - 54
			// degrees outward, 40 inward - so their centres sit 7 degrees
			// off-axis in opposite directions. Declaring a symmetric frustum
			// for both told the runtime each image was centred when it was
			// not, giving 14 degrees of divergence between the eyes. That is
			// unfusable: it presents as two separate images side by side.
			const XrFovf& fe = m_Views[eye].fov;

			int ix = 0, iy = 0, iw = m_nWidth, ih = m_nHeight;

			if (m_bNativeFrustum)
			{
				// The renderer drew this eye's own frustum across the whole
				// target, so there is nothing to reconstruct and nothing to
				// crop. Declare it and hand the image over.
				//
				// This is the case the whole renderer rewrite exists for: every
				// other branch here is a way of approximating an asymmetric
				// frustum with a symmetric render, and each approximation is
				// exact only at the centre of the image.
				views[eye].fov = fe;
			}
			else if (m_nProjMode != 1)
			{
				// Declare the frustum the game rendered and hand over the whole
				// image; the runtime maps it into its own display frustum.
				//
				// An aspect mismatch can be corrected on either axis, and the
				// two differ in which way overall zoom moves - narrowing
				// horizontally keeps the vertical scale, widening vertically
				// makes the viewer feel gigantic.
				float ax = hx, ay = hy;

				if (m_nProjMode == 2)
				{
					// Trust the vertical; derive horizontal from the image
					// shape. Narrows the view.
					ax = atanf(tanf(hy) * (float)m_nWidth / (float)m_nHeight);
				}
				else if (m_nProjMode == 3)
				{
					// Trust the horizontal; derive vertical from the image
					// shape. Heightens the view.
					ay = atanf(tanf(hx) * (float)m_nHeight / (float)m_nWidth);
				}

				// Keep the frustum sane - past this the runtime inverts it and
				// the world appears upside down.
				if (ax > 1.45f) ax = 1.45f;
				if (ay > 1.45f) ay = 1.45f;

				views[eye].fov.angleLeft  = -ax;
				views[eye].fov.angleRight =  ax;
				views[eye].fov.angleUp    =  ay;
				views[eye].fov.angleDown  = -ay;
			}
			else
			{
				// Declare the runtime's own frustum and submit the matching
				// slice. Only correct if hx/hy exactly match what was rendered.
				views[eye].fov = fe;

				float x0 = (tanf(fe.angleLeft)  + tx) / (2.0f * tx);
				float x1 = (tanf(fe.angleRight) + tx) / (2.0f * tx);
				float y0 = (ty - tanf(fe.angleUp))   / (2.0f * ty);
				float y1 = (ty - tanf(fe.angleDown)) / (2.0f * ty);

				x0 = (x0 < 0.0f) ? 0.0f : (x0 > 1.0f ? 1.0f : x0);
				x1 = (x1 < 0.0f) ? 0.0f : (x1 > 1.0f ? 1.0f : x1);
				y0 = (y0 < 0.0f) ? 0.0f : (y0 > 1.0f ? 1.0f : y0);
				y1 = (y1 < 0.0f) ? 0.0f : (y1 > 1.0f ? 1.0f : y1);

				ix = (int)(x0 * m_nWidth  + 0.5f);
				iy = (int)(y0 * m_nHeight + 0.5f);
				iw = (int)((x1 - x0) * m_nWidth  + 0.5f);
				ih = (int)((y1 - y0) * m_nHeight + 0.5f);
				if (iw < 16) iw = 16;
				if (ih < 16) ih = 16;
				if (ix + iw > m_nWidth)  iw = m_nWidth  - ix;
				if (iy + ih > m_nHeight) ih = m_nHeight - iy;
			}

			views[eye].subImage.swapchain = m_Eyes[eye].handle;
			views[eye].subImage.imageRect.offset = { ix, iy };
			views[eye].subImage.imageRect.extent = { iw, ih };
			views[eye].subImage.imageArrayIndex = 0;

			if (!m_bLoggedSubRect)
			{
				HostLog::Msg("eye %d mode %d: declared fov L%.1f R%.1f U%.1f D%.1f, rect (%d,%d) %dx%d of %dx%d",
					eye, m_nProjMode,
					views[eye].fov.angleLeft * 57.2957795f, views[eye].fov.angleRight * 57.2957795f,
					views[eye].fov.angleUp * 57.2957795f, views[eye].fov.angleDown * 57.2957795f,
					ix, iy, iw, ih, m_nWidth, m_nHeight);
			}
		}
		m_bLoggedSubRect = true;

		// The layer must be submitted in the SAME space the poses were located
		// in, or the two describe different frames and the correction is
		// misdirected all over again.
		layer.space = bHeadLock ? m_ViewSpace
					: ((m_GameSpace != XR_NULL_HANDLE) ? m_GameSpace : m_Space);
		layer.viewCount = 2;
		layer.views = views;
		layers[0] = (const XrCompositionLayerBaseHeader*)&layer;
		layerCount = 1;

		if (bHeadLock && !m_bLoggedHeadLock)
		{
			m_bLoggedHeadLock = true;
			Msg("HEAD-LOCKED: layer in VIEW space, eye poses (%.3f, %.3f, %.3f) and (%.3f, %.3f, %.3f) m from the head"
				" - the runtime is reprojecting nothing",
				m_ViewsHead[0].pose.position.x, m_ViewsHead[0].pose.position.y,
				m_ViewsHead[0].pose.position.z,
				m_ViewsHead[1].pose.position.x, m_ViewsHead[1].pose.position.y,
				m_ViewsHead[1].pose.position.z);
		}
	}

	// THE PAUSE MENU, AS A QUAD OVER THE WORLD. The world stays a projection
	// layer - stereo, head-tracked, reprojected - and the menu sits on a flat
	// panel where the head was pointing when the pause opened. The compositor
	// holds it there; nothing in the eye images has to.
	XrCompositionLayerQuad pq{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	if (bPauseQuad && layerCount == 1 && m_Ovl.submitted && m_bPauseAnchored)
	{
		pq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		pq.space      = m_Space;
		pq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		pq.subImage.swapchain = m_Ovl.handle;
		pq.subImage.imageRect.offset = { 0, 0 };
		pq.subImage.imageRect.extent = { m_nOvlW, m_nOvlH };
		pq.subImage.imageArrayIndex = 0;
		pq.pose = m_PausePose;
		pq.size.width  = m_fMenuWidth;
		pq.size.height = m_fMenuWidth * (float)m_nOvlH / (float)(m_nOvlW ? m_nOvlW : 1);
		layers[1] = (const XrCompositionLayerBaseHeader*)&pq;
		layerCount = 2;
		if (!m_bLoggedPauseQuad)
		{
			m_bLoggedPauseQuad = true;
			Msg("pause menu: quad %.2f x %.2f m over the projection layer", pq.size.width, pq.size.height);
		}
	}

	XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
	fei.displayTime = m_FrameState.predictedDisplayTime;
	fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	fei.layerCount = layerCount;
	fei.layers = layerCount ? layers : nullptr;

	const double te0 = HostLog::NowMs();
	const XrResult res = xrEndFrame(m_Session, &fei);
	m_fLastEndCallMs = HostLog::NowMs() - te0;

	// A projection layer in VIEW space is legal, but it is an unusual thing to
	// submit and the runtime is entitled to refuse it. Fall back to the
	// world-locked path rather than leaving the headset with no layer at all
	// (a project rule) - the client keeps its head-as-mouse camera, which is
	// still half the experiment and still worth reporting.
	if (XR_FAILED(res))
	{
		if (bHeadLock && m_bHeadLockOk)
		{
			m_bHeadLockOk = false;
			Msg("ERROR: the runtime rejected the head-locked layer (xrEndFrame %d)"
				" - reverting to world-locked submission", (int)res);
		}
		else if (m_nEndFail++ == 0)
		{
			// Not the head-locked path, so there is nothing to fall back to.
			// Say so once and keep counting: if this is every frame, the headset
			// is frozen on the last good image while everything else looks normal.
			Msg("ERROR: xrEndFrame failed (%d) with %u layer(s) - first of possibly many",
				(int)res, layerCount);
		}
	}
}

void XrVr::GetHeadEuler(float& fYawDeg, float& fPitchDeg, float& fRollDeg) const
{
	const XrQuaternionf& q = m_Views[0].pose.orientation;

	const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
	fPitchDeg = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp) : asinf(sinp) * R2D;
	fYawDeg   = atan2f(2.0f * (q.w * q.y + q.z * q.x),
					   1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * R2D;
	fRollDeg  = atan2f(2.0f * (q.w * q.z + q.x * q.y),
					   1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * R2D;
}

XrVector3f XrVr::HeadPosition() const
{
	return HeadMidpoint();
}

XrVector3f XrVr::HeadMidpoint() const
{
	// The midpoint of the two eyes, not eye 0. m_Views[0] is the LEFT eye, so
	// using it directly puts everything derived from "where the head is" half
	// an IPD - about 31 mm - to the left of where the player actually is.
	//
	// Imported from the Forsaken VR port, which hit this on its menu panel and
	// found the panel sitting half an IPD left of the player's gaze. Same
	// runtime, same headset, same mistake here.
	const XrVector3f& a = m_Views[0].pose.position;
	const XrVector3f& b = m_Views[1].pose.position;
	XrVector3f mid;
	mid.x = (a.x + b.x) * 0.5f;
	mid.y = (a.y + b.y) * 0.5f;
	mid.z = (a.z + b.z) * 0.5f;
	return mid;
}

float XrVr::Ipd() const
{
	const XrVector3f& a = m_Views[0].pose.position;
	const XrVector3f& b = m_Views[1].pose.position;
	const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
	return sqrtf(dx * dx + dy * dy + dz * dz);
}

namespace
{
	// Small helpers so the action setup below reads as a list rather than as
	// forty lines of struct filling.
	XrPath Path(XrInstance inst, const char* s)
	{
		XrPath p = XR_NULL_PATH;
		xrStringToPath(inst, s, &p);
		return p;
	}
}

bool XrVr::CreateActions()
{
	if (m_Session == XR_NULL_HANDLE) return false;

	XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy_s(asci.actionSetName, "gameplay");
	strcpy_s(asci.localizedActionSetName, "Gameplay");
	if (XR_FAILED(xrCreateActionSet(m_Instance, &asci, &m_ActionSet)))
	{
		Msg("input: action set creation failed");
		return false;
	}

	m_HandPath[0] = Path(m_Instance, "/user/hand/left");
	m_HandPath[1] = Path(m_Instance, "/user/hand/right");

	struct Def { XrAction* pAct; const char* pName; XrActionType type; };
	const Def defs[] = {
		{ &m_aAim,       "aim",       XR_ACTION_TYPE_POSE_INPUT    },
		{ &m_aTrigger,   "trigger",   XR_ACTION_TYPE_FLOAT_INPUT   },
		{ &m_aGrip,      "grip",      XR_ACTION_TYPE_FLOAT_INPUT   },
		{ &m_aPrimary,   "primary",   XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_aSecondary, "secondary", XR_ACTION_TYPE_BOOLEAN_INPUT },
		{ &m_aThumb,     "thumbclick",XR_ACTION_TYPE_BOOLEAN_INPUT },
		// The menu button on the LEFT Touch controller. The right hand's is
		// the Oculus button and the runtime keeps it; only the left binds.
		{ &m_aMenu,      "menu",      XR_ACTION_TYPE_BOOLEAN_INPUT },
		// Distinct names on purpose. Two actions sharing a localizedActionName
		// makes the runtime drop the second one silently, and the symptom is a
		// control that simply never fires.
		{ &m_aStickX,    "stickx",    XR_ACTION_TYPE_FLOAT_INPUT   },
		{ &m_aStickY,    "sticky",    XR_ACTION_TYPE_FLOAT_INPUT   },
		{ &m_aHaptic,    "haptic",    XR_ACTION_TYPE_VIBRATION_OUTPUT },
	};

	for (const Def& d : defs)
	{
		XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
		strcpy_s(aci.actionName, d.pName);
		strcpy_s(aci.localizedActionName, d.pName);
		aci.actionType = d.type;
		aci.countSubactionPaths = 2;			// one action, both hands
		aci.subactionPaths = m_HandPath;
		if (XR_FAILED(xrCreateAction(m_ActionSet, &aci, d.pAct)))
		{
			Msg("input: action '%s' failed", d.pName);
			return false;
		}
	}

	// Quest 3 reports as the Oculus Touch profile. Only this one is suggested:
	// binding profiles we cannot test would be guesses, and a wrong suggestion
	// is accepted silently by the runtime and then simply never fires.
	const XrActionSuggestedBinding binds[] = {
		{ m_aAim,       Path(m_Instance, "/user/hand/left/input/aim/pose")        },
		{ m_aAim,       Path(m_Instance, "/user/hand/right/input/aim/pose")       },
		{ m_aTrigger,   Path(m_Instance, "/user/hand/left/input/trigger/value")   },
		{ m_aTrigger,   Path(m_Instance, "/user/hand/right/input/trigger/value")  },
		{ m_aGrip,      Path(m_Instance, "/user/hand/left/input/squeeze/value")   },
		{ m_aGrip,      Path(m_Instance, "/user/hand/right/input/squeeze/value")  },
		{ m_aPrimary,   Path(m_Instance, "/user/hand/left/input/x/click")         },
		{ m_aPrimary,   Path(m_Instance, "/user/hand/right/input/a/click")        },
		{ m_aSecondary, Path(m_Instance, "/user/hand/left/input/y/click")         },
		{ m_aSecondary, Path(m_Instance, "/user/hand/right/input/b/click")        },
		{ m_aThumb,     Path(m_Instance, "/user/hand/left/input/thumbstick/click")  },
		{ m_aThumb,     Path(m_Instance, "/user/hand/right/input/thumbstick/click") },
		{ m_aMenu,      Path(m_Instance, "/user/hand/left/input/menu/click")        },
		{ m_aStickX,    Path(m_Instance, "/user/hand/left/input/thumbstick/x")      },
		{ m_aStickX,    Path(m_Instance, "/user/hand/right/input/thumbstick/x")     },
		{ m_aStickY,    Path(m_Instance, "/user/hand/left/input/thumbstick/y")      },
		{ m_aStickY,    Path(m_Instance, "/user/hand/right/input/thumbstick/y")     },
		{ m_aHaptic,    Path(m_Instance, "/user/hand/left/output/haptic")           },
		{ m_aHaptic,    Path(m_Instance, "/user/hand/right/output/haptic")          },
	};

	XrInteractionProfileSuggestedBinding sug{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	sug.interactionProfile = Path(m_Instance, "/interaction_profiles/oculus/touch_controller");
	sug.suggestedBindings = binds;
	sug.countSuggestedBindings = (uint32_t)(sizeof(binds) / sizeof(binds[0]));
	if (XR_FAILED(xrSuggestInteractionProfileBindings(m_Instance, &sug)))
	{
		Msg("input: the runtime rejected the Oculus Touch bindings");
		return false;
	}

	XrSessionActionSetsAttachInfo att{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	att.countActionSets = 1;
	att.actionSets = &m_ActionSet;
	if (XR_FAILED(xrAttachSessionActionSets(m_Session, &att)))
	{
		Msg("input: attaching the action set failed");
		return false;
	}

	for (int h = 0; h < 2; ++h)
	{
		XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		sci.action = m_aAim;
		sci.subactionPath = m_HandPath[h];
		sci.poseInActionSpace.orientation.w = 1.0f;
		if (XR_FAILED(xrCreateActionSpace(m_Session, &sci, &m_AimSpace[h])))
		{
			Msg("input: aim space for hand %d failed", h);
			return false;
		}
	}

	m_bActionsReady = true;
	Msg("input: Quest 3 controllers bound (aim pose, trigger, grip, A/B/X/Y,"
		" stick click, stick X/Y)");
	return true;
}

void XrVr::Pulse(int nHand, float fAmp, float fMs)
{
	if (!m_bActionsReady || !m_bRunning || m_aHaptic == XR_NULL_HANDLE) return;
	XrHapticVibration v{ XR_TYPE_HAPTIC_VIBRATION };
	v.duration  = (XrDuration)((double)fMs * 1000000.0);
	v.frequency = XR_FREQUENCY_UNSPECIFIED;
	v.amplitude = (fAmp < 0.0f) ? 0.0f : (fAmp > 1.0f ? 1.0f : fAmp);
	XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO };
	hi.action = m_aHaptic;
	hi.subactionPath = m_HandPath[nHand ? 1 : 0];
	static int s_nSaid = 0;
	const XrResult r = xrApplyHapticFeedback(m_Session, &hi, (const XrHapticBaseHeader*)&v);
	if (s_nSaid < 3) { ++s_nSaid; Msg("haptic: hand %d amp %.2f %.0f ms -> %d", nHand ? 1 : 0, v.amplitude, fMs, (int)r); }
}

void XrVr::UpdateActions()
{
	if (!m_bActionsReady || !m_bRunning) return;

	XrActiveActionSet active{ m_ActionSet, XR_NULL_PATH };
	XrActionsSyncInfo sync{ XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (XR_FAILED(xrSyncActions(m_Session, &sync))) return;

	for (int h = 0; h < 2; ++h)
	{
		HandState& hs = m_Hands[h];
		hs.nButtons = 0;

		XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
		gi.subactionPath = m_HandPath[h];

		XrActionStatePose ps{ XR_TYPE_ACTION_STATE_POSE };
		gi.action = m_aAim;
		xrGetActionStatePose(m_Session, &gi, &ps);
		hs.bActive = (ps.isActive != XR_FALSE);

		if (hs.bActive)
		{
			XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(xrLocateSpace(m_AimSpace[h], m_Space,
					m_FrameState.predictedDisplayTime, &loc)) &&
				(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
				(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
			{
				hs.pose = loc.pose;
			}
			else hs.bActive = false;
		}

		XrActionStateFloat fs{ XR_TYPE_ACTION_STATE_FLOAT };
		gi.action = m_aTrigger;
		if (XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &fs)) && fs.isActive)
		{
			hs.fTrigger = fs.currentState;
			if (fs.currentState > 0.5f) hs.nButtons |= kBtnTrigger;
		}
		gi.action = m_aGrip;
		if (XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &fs)) && fs.isActive)
		{
			hs.fGrip = fs.currentState;
			if (fs.currentState > 0.5f) hs.nButtons |= kBtnGrip;
		}

		// Sticks. Zeroed when the action is inactive rather than left holding
		// the last value - a stale non-zero axis is a player who walks into a
		// wall forever after a controller sleeps.
		hs.fStickX = 0.0f;
		hs.fStickY = 0.0f;
		gi.action = m_aStickX;
		if (XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &fs)) && fs.isActive)
			hs.fStickX = fs.currentState;
		gi.action = m_aStickY;
		if (XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &fs)) && fs.isActive)
			hs.fStickY = fs.currentState;

		struct BtnDef { XrAction a; uint32_t bit; };
		const BtnDef btns[] = {
			{ m_aPrimary,   (uint32_t)kBtnPrimary   },
			{ m_aSecondary, (uint32_t)kBtnSecondary },
			{ m_aThumb,     (uint32_t)kBtnThumbClick },
			{ m_aMenu,      (uint32_t)kBtnMenu },
		};
		for (const BtnDef& b : btns)
		{
			XrActionStateBoolean bs{ XR_TYPE_ACTION_STATE_BOOLEAN };
			gi.action = b.a;
			if (XR_SUCCEEDED(xrGetActionStateBoolean(m_Session, &gi, &bs)) &&
				bs.isActive && bs.currentState)
			{
				hs.nButtons |= b.bit;
			}
		}
	}

	if (!m_bLoggedHands && (m_Hands[0].bActive || m_Hands[1].bActive))
	{
		m_bLoggedHands = true;
		Msg("input: controllers tracking (left %s, right %s)",
			m_Hands[0].bActive ? "yes" : "no", m_Hands[1].bActive ? "yes" : "no");
	}
}

double XrVr::AgeOfPoseByte(uint32_t nLowByte) const
{
	// Newest ring entry whose frame number ends in this byte. The marker
	// carries only 8 bits, which wraps every 256 host frames - under three
	// seconds at 90 Hz - but the ring holds 128 entries, so the newest match is
	// unambiguous by a wide margin.
	const double fNow = HostLog::NowMs();
	double fBest = -1.0;
	for (int i = 0; i < kPoseRing; ++i)
	{
		const PoseSample& s = m_PoseRing[i];
		if (s.fTimeMs < 0.0) continue;
		if ((s.nFrame & 0xFFu) != nLowByte) continue;
		const double fAge = fNow - s.fTimeMs;
		if (fAge < 0.0) continue;
		if (fBest < 0.0 || fAge < fBest) fBest = fAge;
	}
	return fBest;
}

void XrVr::SetAsymCentres(bool bActive, const float* pYaw, const float* pPitch)
{
	if (bActive != m_bAsymActive)
	{
		m_bAsymActive = bActive;
		Msg("per-eye optical centres %s (eye0 %.1f/%.1f, eye1 %.1f/%.1f deg)",
			bActive ? "ACTIVE" : "off",
			pYaw[0] * R2D, pPitch[0] * R2D, pYaw[1] * R2D, pPitch[1] * R2D);
	}
	m_fAsymYaw[0]   = pYaw[0];   m_fAsymYaw[1]   = pYaw[1];
	m_fAsymPitch[0] = pPitch[0]; m_fAsymPitch[1] = pPitch[1];
}

void XrVr::SetHeadLocked(bool b)
{
	if (b == m_bHeadLockWanted) return;
	m_bHeadLockWanted = b;

	// A refusal is remembered for the session, but asking again is a fresh
	// request - the player toggling F1 twice should get a second attempt rather
	// than a silently dead key.
	if (b) m_bHeadLockOk = true;
	m_bLoggedHeadLock = false;

	if (b && !m_ViewSpace)
		Msg("head-locked submission requested but there is no VIEW space - staying world-locked");
	else
		Msg("head-locked submission %s (client %s the head through the mouse path)",
			b ? "ON" : "off", b ? "is feeding" : "is not feeding");
}


void XrVr::Recenter()
{
	if (m_Session == XR_NULL_HANDLE || m_ViewSpace == XR_NULL_HANDLE || m_Space == XR_NULL_HANDLE) return;
	// The head as the runtime sees it in LOCAL, at this frame's time.
	XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
	if (XR_FAILED(xrLocateSpace(m_ViewSpace, m_Space, m_FrameState.predictedDisplayTime, &loc))
		|| !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		Msg("recenter: the view pose is not available this frame - ignored");
		return;
	}
	const XrQuaternionf& q = loc.pose.orientation;
	// Yaw about +Y, the same convention the game space's pose is built with.
	const float fHeadYaw = atan2f(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.y * q.y + q.z * q.z));
	// The game space turns by (body yaw + recenter yaw); the head's yaw in it
	// is its LOCAL yaw minus that. Zero it: the recenter yaw is whatever the
	// body part does not already cover.
	const float fBodyPart = (m_nYawMode == 0) ? 0.0f : m_fBodyYaw;
	m_fRecYaw = fHeadYaw - fBodyPart;
	m_RecPos  = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			  ? loc.pose.position : XrVector3f{ 0.0f, 0.0f, 0.0f };
	m_bHaveRec  = true;
	m_bRecDirty = true;
	Msg("recenter: head yaw %.1f deg, position %.2f %.2f %.2f m folded into the game space",
		fHeadYaw * R2D, m_RecPos.x, m_RecPos.y, m_RecPos.z);
	SetBodyYaw(m_fWantYaw, m_nYawMode);
}

bool XrVr::ConsumeSpaceChanged()
{
	const bool b = m_bSpaceChanged;
	m_bSpaceChanged = false;
	return b;
}

void XrVr::SetBodyYaw(float fYawRad, int nMode)
{
	m_fWantYaw = fYawRad;

	if (nMode != m_nYawMode)
	{
		m_nYawMode = nMode;
		m_bLoggedYawSpace = false;
		m_bYawRefSet = false;		// re-anchor when it is switched on
		Msg("yaw-carrying reference space: %s",
			(nMode == 0) ? "OFF - declaring in LOCAL"
						 : ((nMode == 1) ? "ON (sign +)" : "ON (sign -)"));
		if (nMode != 0)
			Msg("  NOTE: this cannot change the reprojection. The runtime resamples by"
				" P^-1 * P2, and both poses are located in whatever space the layer is"
				" submitted in - so rotating that space by T makes both T^-1 * P and T"
				" cancels exactly. It moves where the layer sits in the room and nothing"
				" else. See docs/FRAMES-AGREE.md.");
	}

	if (m_Session == XR_NULL_HANDLE) return;
	// Yaw mode 0 declares in LOCAL - unless a recenter has been applied, in
	// which case the game space carries the recenter and no body yaw.
	if (m_nYawMode == 0 && !m_bHaveRec) return;

	// RELATIVE to a reference captured once, not the absolute game yaw.
	//
	// The first version rotated the space by the absolute m_fYaw, and that is
	// meaningless: the game's yaw zero is wherever the level happens to spawn
	// the player, while OpenXR's LOCAL zero is wherever the user was facing
	// when the session started. The two have no relationship, so this was
	// rotating the declared frame by an arbitrary constant - and a constant
	// frame offset misdirects every correction the compositor makes. Head pitch
	// arrives as roll, with no mouse involved, which is exactly how it was
	// reported. Both signs were wrong because both were wrong by that offset.
	//
	// Anchoring on the first frame makes the two frames agree at that moment,
	// so only actual turning after it can separate them - which is the thing
	// this is meant to track.
	if (!m_bYawRefSet)
	{
		m_bYawRefSet = true;
		m_fRefYaw    = fYawRad;
		Msg("yaw reference anchored at %.1f deg - the two frames agree from here",
			fYawRad * R2D);
	}

	float fRel = fYawRad - m_fRefYaw;
	while (fRel >  3.14159265f) fRel -= 6.28318531f;
	while (fRel < -3.14159265f) fRel += 6.28318531f;

	// LithTech yaw is a rotation about its up axis; OpenXR's up is +Y. Whether
	// a positive game yaw is a positive or negative rotation there is a
	// convention question, hence the two modes rather than a confident guess.
	const float fYaw = (m_nYawMode == 0) ? 0.0f : ((m_nYawMode == 2) ? -fRel : fRel);

	// Rebuild only when it has actually moved. A reference space's pose cannot
	// be changed after creation, so this is a destroy and create - throttled to
	// half a degree, which is far below anything visible and keeps it to a
	// handful of calls a second rather than one per frame.
	if (m_GameSpace != XR_NULL_HANDLE && !m_bRecDirty && fabsf(fYaw - m_fBodyYaw) < 0.0087f) return;
	const float fTotal = fYaw + m_fRecYaw;

	XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.x = 0.0f;
	rsci.poseInReferenceSpace.orientation.y = sinf(fTotal * 0.5f);
	rsci.poseInReferenceSpace.orientation.z = 0.0f;
	rsci.poseInReferenceSpace.orientation.w = cosf(fTotal * 0.5f);
	rsci.poseInReferenceSpace.position      = m_RecPos;

	XrSpace nu = XR_NULL_HANDLE;
	if (XR_FAILED(xrCreateReferenceSpace(m_Session, &rsci, &nu)))
	{
		if (!m_bLoggedYawSpace)
		{
			m_bLoggedYawSpace = true;
			Msg("ERROR: could not create the yaw-carrying space - staying in LOCAL");
		}
		return;
	}

	if (m_GameSpace != XR_NULL_HANDLE) xrDestroySpace(m_GameSpace);
	m_GameSpace = nu;
	m_fBodyYaw  = fYaw;
	m_bRecDirty = false;

	if (!m_bLoggedYawSpace)
	{
		m_bLoggedYawSpace = true;
		Msg("yaw-carrying space live: body yaw %.1f deg, rebuilt on every 0.5 deg of turn",
			fYaw * R2D);
	}
}

void XrVr::RequestRefreshRate(float fMaxHz)
{
	if (!m_bHaveRefreshExt || m_Session == XR_NULL_HANDLE) return;

	PFN_xrEnumerateDisplayRefreshRatesFB pEnum = nullptr;
	PFN_xrGetDisplayRefreshRateFB        pGet  = nullptr;
	PFN_xrRequestDisplayRefreshRateFB    pReq  = nullptr;
	xrGetInstanceProcAddr(m_Instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&pEnum);
	xrGetInstanceProcAddr(m_Instance, "xrGetDisplayRefreshRateFB",        (PFN_xrVoidFunction*)&pGet);
	xrGetInstanceProcAddr(m_Instance, "xrRequestDisplayRefreshRateFB",    (PFN_xrVoidFunction*)&pReq);
	if (!pEnum || !pReq) return;

	float fCurrent = 0.0f;
	if (pGet) pGet(m_Session, &fCurrent);

	uint32_t n = 0;
	pEnum(m_Session, 0, &n, nullptr);
	if (!n) return;
	std::vector<float> rates(n, 0.0f);
	pEnum(m_Session, n, &n, rates.data());

	char szList[256] = { 0 };
	float fBest = 0.0f;
	for (uint32_t i = 0; i < n; ++i)
	{
		char szOne[24];
		sprintf_s(szOne, "%s%.0f", (i ? ", " : ""), rates[i]);
		strncat_s(szList, szOne, _TRUNCATE);
		if (rates[i] > fBest && rates[i] <= fMaxHz + 0.5f) fBest = rates[i];
	}

	Msg("display refresh: currently %.0f Hz, runtime offers %s Hz", fCurrent, szList);

	if (fBest <= 0.0f || fBest <= fCurrent + 0.5f)
	{
		Msg("  keeping %.0f Hz - nothing better available at or below %.0f", fCurrent, fMaxHz);
		return;
	}

	const XrResult r = pReq(m_Session, fBest);
	if (XR_SUCCEEDED(r)) Msg("  requested %.0f Hz - granted", fBest);
	else                 Msg("  requested %.0f Hz - REFUSED (%d), staying at %.0f", fBest, (int)r, fCurrent);
}

void XrVr::Shutdown()
{
	for (int i = 0; i < 2; ++i)
	{
		if (m_Eyes[i].handle) { xrDestroySwapchain(m_Eyes[i].handle); m_Eyes[i].handle = XR_NULL_HANDLE; }
	}
	if (m_ViewSpace){ xrDestroySpace(m_ViewSpace);   m_ViewSpace = XR_NULL_HANDLE; }
	if (m_GameSpace){ xrDestroySpace(m_GameSpace);   m_GameSpace = XR_NULL_HANDLE; }
	if (m_Space)    { xrDestroySpace(m_Space);       m_Space = XR_NULL_HANDLE; }
	if (m_Session)  { xrDestroySession(m_Session);   m_Session = XR_NULL_HANDLE; }
	if (m_Instance) { xrDestroyInstance(m_Instance); m_Instance = XR_NULL_HANDLE; }
}
