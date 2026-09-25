// ----------------------------------------------------------------------- //
//
// MODULE  : xrprobe.cpp
//
// PURPOSE : M5 step 1. Proves the OpenXR half works before anything depends
//           on it: opens a session against the active runtime, reports what
//           the headset wants rendered, and logs head pose every frame.
//
//           Renders nothing and submits no layers. If head orientation moves
//           sensibly when the player turns his head, tracking is solved and the
//           remaining work is plumbing.
//
// ----------------------------------------------------------------------- //

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32
#define NOMINMAX

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "hostlog.h"
#include "VRShared.h"		// the client's copy - one contract, no duplication

#include <cmath>
#include <cstring>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using HostLog::Msg;

namespace
{
	XrInstance	g_instance = XR_NULL_HANDLE;
	XrSession	g_session  = XR_NULL_HANDLE;
	XrSpace		g_space    = XR_NULL_HANDLE;
	XrSystemId	g_system   = XR_NULL_SYSTEM_ID;

	bool Check(XrResult r, const char* what)
	{
		if (XR_SUCCEEDED(r)) return true;
		char name[XR_MAX_RESULT_STRING_SIZE] = { 0 };
		if (g_instance != XR_NULL_HANDLE) xrResultToString(g_instance, r, name);
		Msg("FAILED: %s -> %s (%d)", what, name[0] ? name : "?", (int)r);
		return false;
	}

	HANDLE			g_hMap    = nullptr;
	VRSharedState*	g_pShared = nullptr;

	// Publishes the block the game client reads. Creating it is optional - the
	// probe still reports poses if this fails, it just cannot drive the game.
	bool CreateSharedBlock()
	{
		g_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, sizeof(VRSharedState), VRSHARED_NAME);
		if (!g_hMap) { Msg("WARNING: CreateFileMapping failed (%lu)", GetLastError()); return false; }

		g_pShared = (VRSharedState*)MapViewOfFile(g_hMap, FILE_MAP_ALL_ACCESS, 0, 0,
			sizeof(VRSharedState));
		if (!g_pShared) { Msg("WARNING: MapViewOfFile failed (%lu)", GetLastError()); return false; }

		ZeroMemory(g_pShared, sizeof(VRSharedState));
		g_pShared->nMagic   = VRSHARED_MAGIC;
		g_pShared->nVersion = VRSHARED_VERSION;
		Msg("shared block \"%s\" published, %u bytes", VRSHARED_NAME, (unsigned)sizeof(VRSharedState));
		return true;
	}

	// Seqlock write: odd while writing, even when consistent. The reader
	// retries rather than blocking, so a slow client never stalls the host.
	void PublishPose(float yaw, float pitch, float roll,
					 const XrVector3f& pos, float ipd, const XrFovf& fov, uint32_t frame)
	{
		if (!g_pShared) return;

		++g_pShared->nSequence;					// -> odd
		_ReadWriteBarrier();

		g_pShared->fHeadYawDeg   = yaw;
		g_pShared->fHeadPitchDeg = pitch;
		g_pShared->fHeadRollDeg  = roll;
		g_pShared->fHeadPosX     = pos.x;
		g_pShared->fHeadPosY     = pos.y;
		g_pShared->fHeadPosZ     = pos.z;
		g_pShared->fIpdMeters    = ipd;
		g_pShared->fFovLeftRad   = fov.angleLeft;
		g_pShared->fFovRightRad  = fov.angleRight;
		g_pShared->fFovUpRad     = fov.angleUp;
		g_pShared->fFovDownRad   = fov.angleDown;
		g_pShared->nFrameCounter = frame;
		g_pShared->nHostAliveTick = GetTickCount();

		_ReadWriteBarrier();
		++g_pShared->nSequence;					// -> even
	}

	// Quaternion to yaw/pitch/roll in degrees, for a log a human can read.
	void QuatToEuler(const XrQuaternionf& q, float& yaw, float& pitch, float& roll)
	{
		const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
		pitch = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp)
									  : asinf(sinp) * 57.2957795f;
		yaw  = atan2f(2.0f * (q.w * q.y + q.z * q.x),
					  1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * 57.2957795f;
		roll = atan2f(2.0f * (q.w * q.z + q.x * q.y),
					  1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * 57.2957795f;
	}
}

int main(int argc, char** argv)
{
	HostLog::Open("xrprobe");
	Msg("=== NOLF1 VR - OpenXR tracking probe (M5 step 1-2) ===");

	const int nSeconds = (argc > 1) ? atoi(argv[1]) : 20;

	CreateSharedBlock();

	// --- instance ---------------------------------------------------------
	{
		uint32_t count = 0;
		xrEnumerateApiLayerProperties(0, &count, nullptr);
		Msg("api layers available: %u", count);
	}

	const char* exts[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };

	XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
	strcpy_s(ici.applicationInfo.applicationName, "NOLF1 VR");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 1;
	ici.enabledExtensionNames = exts;

	if (!Check(xrCreateInstance(&ici, &g_instance), "xrCreateInstance"))
	{
		Msg("Is a headset connected and the runtime running? Virtual Desktop");
		Msg("Streamer must be connected to the Quest before this will succeed.");
		HostLog::Close();
		return 1;
	}

	{
		XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
		xrGetInstanceProperties(g_instance, &ip);
		Msg("runtime: %s  version %u.%u.%u", ip.runtimeName,
			XR_VERSION_MAJOR(ip.runtimeVersion),
			XR_VERSION_MINOR(ip.runtimeVersion),
			XR_VERSION_PATCH(ip.runtimeVersion));
	}

	// --- system -----------------------------------------------------------
	// XR_ERROR_FORM_FACTOR_UNAVAILABLE just means no headset is connected yet.
	// Wait rather than exit, so the probe can be started before the headset is
	// put on and Virtual Desktop has finished connecting.
	XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	XrResult sysResult = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
	for (int i = 0; i < 120; ++i)
	{
		sysResult = xrGetSystem(g_instance, &sgi, &g_system);
		if (sysResult != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break;
		if (i == 0) Msg("no headset yet - waiting up to 60s. Connect Virtual Desktop and put the headset on.");
		Sleep(500);
	}
	if (!Check(sysResult, "xrGetSystem"))
	{
		Msg("Give up: no head-mounted display became available.");
		HostLog::Close();
		return 1;
	}

	{
		XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
		xrGetSystemProperties(g_instance, g_system, &sp);
		Msg("system: %s", sp.systemName);
		Msg("  max swapchain %ux%u, %u layers",
			sp.graphicsProperties.maxSwapchainImageWidth,
			sp.graphicsProperties.maxSwapchainImageHeight,
			sp.graphicsProperties.maxLayerCount);
		Msg("  orientation tracking %d, position tracking %d",
			(int)sp.trackingProperties.orientationTracking,
			(int)sp.trackingProperties.positionTracking);
	}

	// --- what the headset wants rendered ----------------------------------
	// This is the number that decides what resolution the game should run at.
	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(g_instance, g_system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);

	XrViewConfigurationView views[2]{};
	for (uint32_t i = 0; i < 2; ++i) views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	xrEnumerateViewConfigurationViews(g_instance, g_system,
		XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, views);

	for (uint32_t i = 0; i < viewCount; ++i)
	{
		Msg("view %u: recommended %ux%u  (max %ux%u)  samples %u", i,
			views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
			views[i].maxImageRectWidth, views[i].maxImageRectHeight,
			views[i].recommendedSwapchainSampleCount);
	}
	Msg("=> side-by-side window would be %ux%u",
		views[0].recommendedImageRectWidth * 2, views[0].recommendedImageRectHeight);

	// --- D3D11 device on the adapter the runtime demands -------------------
	PFN_xrGetD3D11GraphicsRequirementsKHR pfnGetReq = nullptr;
	xrGetInstanceProcAddr(g_instance, "xrGetD3D11GraphicsRequirementsKHR",
		(PFN_xrVoidFunction*)&pfnGetReq);
	if (!pfnGetReq) { Msg("FAILED: no xrGetD3D11GraphicsRequirementsKHR"); return 1; }

	XrGraphicsRequirementsD3D11KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
	if (!Check(pfnGetReq(g_instance, g_system, &req), "xrGetD3D11GraphicsRequirements")) return 1;
	Msg("runtime requires adapter LUID %08X:%08X",
		(unsigned)req.adapterLuid.HighPart, (unsigned)req.adapterLuid.LowPart);

	IDXGIFactory1* factory = nullptr;
	CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
	IDXGIAdapter1* adapter = nullptr;
	for (UINT i = 0; ; ++i)
	{
		IDXGIAdapter1* a = nullptr;
		if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
		DXGI_ADAPTER_DESC1 ad{};
		a->GetDesc1(&ad);
		if (memcmp(&ad.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0)
		{
			adapter = a;
			Msg("matched adapter: %ls", ad.Description);
			break;
		}
		a->Release();
	}
	if (!adapter) { Msg("FAILED: no adapter matches the runtime's LUID"); return 1; }

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
		D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
		&device, nullptr, &context)))
	{
		Msg("FAILED: D3D11CreateDevice");
		return 1;
	}

	// --- session ----------------------------------------------------------
	XrGraphicsBindingD3D11KHR binding{ XR_TYPE_GRAPHICS_BINDING_D3D11_KHR };
	binding.device = device;

	XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
	sci.next = &binding;
	sci.systemId = g_system;
	if (!Check(xrCreateSession(g_instance, &sci, &g_session), "xrCreateSession")) return 1;
	Msg("session created");

	XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation.w = 1.0f;
	if (!Check(xrCreateReferenceSpace(g_session, &rsci, &g_space), "xrCreateReferenceSpace")) return 1;

	// --- frame loop -------------------------------------------------------
	Msg("running for %d seconds - put the headset on and look around", nSeconds);

	bool running = false, quit = false;
	int nPoses = 0, nInvalid = 0;
	const double tEnd = HostLog::NowMs() + nSeconds * 1000.0;
	double tNextLog = 0.0;

	while (!quit && HostLog::NowMs() < tEnd)
	{
		XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(g_instance, &ev) == XR_SUCCESS)
		{
			if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				auto* ss = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
				Msg("session state -> %d", (int)ss->state);

				if (ss->state == XR_SESSION_STATE_READY)
				{
					XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
					bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if (Check(xrBeginSession(g_session, &bi), "xrBeginSession"))
					{
						running = true;
						Msg("session running");
					}
				}
				else if (ss->state == XR_SESSION_STATE_STOPPING)
				{
					xrEndSession(g_session);
					running = false;
				}
				else if (ss->state == XR_SESSION_STATE_EXITING ||
						 ss->state == XR_SESSION_STATE_LOSS_PENDING)
				{
					quit = true;
				}
			}
			ev = { XR_TYPE_EVENT_DATA_BUFFER };
		}

		if (!running) { Sleep(10); continue; }

		XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState fs{ XR_TYPE_FRAME_STATE };
		if (!Check(xrWaitFrame(g_session, &fwi, &fs), "xrWaitFrame")) break;

		XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame(g_session, &fbi);

		if (fs.shouldRender)
		{
			XrViewState vs{ XR_TYPE_VIEW_STATE };
			XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
			vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			vli.displayTime = fs.predictedDisplayTime;
			vli.space = g_space;

			XrView xv[2]{};
			for (int i = 0; i < 2; ++i) xv[i].type = XR_TYPE_VIEW;
			uint32_t got = 0;
			xrLocateViews(g_session, &vli, &vs, 2, &got, xv);

			const bool bValid =
				(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
				(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

			if (bValid) ++nPoses; else ++nInvalid;

			if (bValid)
			{
				float y, p, r;
				QuatToEuler(xv[0].pose.orientation, y, p, r);

				const float dx = xv[1].pose.position.x - xv[0].pose.position.x;
				const float dy = xv[1].pose.position.y - xv[0].pose.position.y;
				const float dz = xv[1].pose.position.z - xv[0].pose.position.z;

				PublishPose(y, p, r, xv[0].pose.position,
					sqrtf(dx * dx + dy * dy + dz * dz), xv[0].fov, (uint32_t)nPoses);
			}

			// Log a few times a second - enough to watch by eye, not a flood.
			if (bValid && HostLog::NowMs() > tNextLog)
			{
				tNextLog = HostLog::NowMs() + 250.0;

				float yaw, pitch, roll;
				QuatToEuler(xv[0].pose.orientation, yaw, pitch, roll);

				// 3D distance between the eyes, NOT the X-axis difference.
				// Using the X component made the reported IPD collapse from
				// 62mm to 16mm as the head turned, because the eye-separation
				// vector rotates out of X. Distance is rotation-invariant.
				const float dx = xv[1].pose.position.x - xv[0].pose.position.x;
				const float dy = xv[1].pose.position.y - xv[0].pose.position.y;
				const float dz = xv[1].pose.position.z - xv[0].pose.position.z;
				const float ipd = sqrtf(dx * dx + dy * dy + dz * dz);

				// Per-eye FOV, in degrees. The game's camera FOV must match
				// this or the projection will be wrong in the headset - and
				// wrong in a way that reads as bad depth rather than bad shape.
				const XrFovf& f = xv[0].fov;
				const float fovH = (fabsf(f.angleLeft) + fabsf(f.angleRight)) * 57.2957795f;
				const float fovV = (fabsf(f.angleUp)   + fabsf(f.angleDown))  * 57.2957795f;

				Msg("yaw %+7.2f pitch %+7.2f roll %+7.2f | pos (%+.3f, %+.3f, %+.3f) m | IPD %.1f mm | eye0 fov %.1f x %.1f deg (L%.1f R%.1f U%.1f D%.1f)",
					yaw, pitch, roll,
					xv[0].pose.position.x, xv[0].pose.position.y, xv[0].pose.position.z,
					ipd * 1000.0f, fovH, fovV,
					f.angleLeft * 57.2957795f, f.angleRight * 57.2957795f,
					f.angleUp * 57.2957795f, f.angleDown * 57.2957795f);
			}
		}

		XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
		fei.displayTime = fs.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount = 0;					// nothing to show yet
		fei.layers = nullptr;
		xrEndFrame(g_session, &fei);
	}

	Msg("");
	Msg("=== results ===");
	Msg("valid poses   : %d", nPoses);
	Msg("invalid poses : %d", nInvalid);
	Msg("=== end ===");

	if (g_space)    xrDestroySpace(g_space);
	if (g_session)  xrDestroySession(g_session);
	if (g_instance) xrDestroyInstance(g_instance);
	if (context)    context->Release();
	if (device)     device->Release();
	if (adapter)    adapter->Release();
	if (factory)    factory->Release();

	HostLog::Close();
	return 0;
}
