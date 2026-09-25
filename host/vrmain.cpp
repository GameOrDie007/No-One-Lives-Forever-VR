// ----------------------------------------------------------------------- //
//
// MODULE  : vrmain.cpp
//
// PURPOSE : M5. The VR host: reads head pose from OpenXR, publishes it for the
//           game to pose its camera with, captures the resulting side-by-side
//           window, and submits it to the headset.
//
//           Startup order matters. The runtime dictates which GPU adapter to
//           use, so the OpenXR instance has to exist before the D3D11 device.
//           Capture then binds to that same device, so the eye copy never
//           crosses adapters.
//
// ----------------------------------------------------------------------- //

#include "xrvr.h"
#include "capture.h"
#include "sharedframe.h"
#include "mirror.h"
#include "hostlog.h"
#include "VRShared.h"

#include <dxgi1_2.h>
#include <winrt/base.h>
#include <algorithm>
#include <string.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

using HostLog::Msg;
using winrt::com_ptr;

namespace
{
	HANDLE			g_hMap    = nullptr;
	VRSharedState*	g_pShared = nullptr;

	// Wire version to advertise; 0 = ours. Set from argv[8].
	uint32_t		g_nWireVersion = 0;

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
		// Normally our own version. Overridable so an OLDER client can be run
		// against this host for a bisect: the contract only ever appends, so a
		// version-9 client reads a version-11 block correctly - every field it
		// knows about is at the same offset, and it simply never looks at the
		// tail. Only the equality check in VRShared::Attach stands in the way.
		//
		// This exists because the alternative - building and running the OLD
		// host too - cost two headset sessions to two different silent failures.
		g_pShared->nVersion = (g_nWireVersion > 0) ? g_nWireVersion : VRSHARED_VERSION;
		// So the game can tell when this process is gone (VRShared.h, v19).
		g_pShared->nHostPid = GetCurrentProcessId();
		Msg("shared block published, %u bytes, host pid %lu", (unsigned)sizeof(VRSharedState),
			(unsigned long)g_pShared->nHostPid);
		return true;
	}

	// Seqlock: odd while writing, even when consistent. The client retries
	// rather than blocking, so a slow game never stalls the headset.
	void PublishPose(const XrVr& xr, uint32_t nFrame)
	{
		if (!g_pShared) return;

		float yaw, pitch, roll;
		xr.GetHeadEuler(yaw, pitch, roll);
		const XrVector3f pos = xr.HeadPosition();
		const XrFovf     fov = xr.EyeFov(0);

		++g_pShared->nSequence;
		_ReadWriteBarrier();

		g_pShared->fHeadYawDeg    = yaw;
		g_pShared->fHeadPitchDeg  = pitch;
		g_pShared->fHeadRollDeg   = roll;

		// Controllers. Euler here as well as the head, for the same reason:
		// the client can correct a wrong axis with a console variable rather
		// than a rebuild. The head's quaternion is carried separately because
		// composing three angles proved lossy for it; a weapon aim ray only
		// needs a direction, so it does not have the same problem.
		for (int h = 0; h < 2; ++h)
		{
			const XrVr::HandState& hs = xr.Hand(h);
			VRHandState& out = g_pShared->Hands[h];
			out.nActive = hs.bActive ? 1u : 0u;
			if (!hs.bActive) continue;

			out.fPosX = hs.pose.position.x;
			out.fPosY = hs.pose.position.y;
			out.fPosZ = hs.pose.position.z;

			const XrQuaternionf& q = hs.pose.orientation;
			const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
			out.fPitchDeg = (fabsf(sinp) >= 1.0f) ? copysignf(90.0f, sinp)
												  : asinf(sinp) * 57.2957795f;
			out.fYawDeg   = atan2f(2.0f * (q.w * q.y + q.z * q.x),
								   1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * 57.2957795f;
			out.fRollDeg  = atan2f(2.0f * (q.w * q.z + q.x * q.y),
								   1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * 57.2957795f;

			out.fTrigger = hs.fTrigger;
			out.fGrip    = hs.fGrip;
			out.fStickX  = hs.fStickX;
			out.fStickY  = hs.fStickY;
			out.nButtons = hs.nButtons;
		}

		// The rotation whole, so the client does not have to rebuild it from
		// three angles. Still in OpenXR's frame - the handedness conversion
		// belongs on the client side, where the engine's convention is known.
		const XrQuaternionf q = xr.HeadOrientation();
		g_pShared->fHeadQuatX     = q.x;
		g_pShared->fHeadQuatY     = q.y;
		g_pShared->fHeadQuatZ     = q.z;
		g_pShared->fHeadQuatW     = q.w;

		g_pShared->fHeadPosX      = pos.x;
		g_pShared->fHeadPosY      = pos.y;
		g_pShared->fHeadPosZ      = pos.z;
		g_pShared->fIpdMeters     = xr.Ipd();
		g_pShared->fFovLeftRad    = fov.angleLeft;
		g_pShared->fFovRightRad   = fov.angleRight;
		g_pShared->fFovUpRad      = fov.angleUp;
		g_pShared->fFovDownRad    = fov.angleDown;

		// Per-eye optical centre. Taken per eye, not from eye 0 for both -
		// the horizontal centres are mirrored (+/-7 degrees) and using one for
		// both would cant the whole world sideways rather than correct it.
		for (int e = 0; e < 2; ++e)
		{
			const XrFovf f = xr.EyeFov(e);
			g_pShared->fEyeCentreYawRad[e]   = (f.angleLeft + f.angleRight) * 0.5f;
			g_pShared->fEyeCentrePitchRad[e] = (f.angleUp   + f.angleDown ) * 0.5f;

			// And the frustum whole, which is what a renderer that can build
			// an asymmetric projection actually wants. The centres above are
			// this same information halved and thrown away.
			g_pShared->fEyeFovLeftRad[e]  = f.angleLeft;
			g_pShared->fEyeFovRightRad[e] = f.angleRight;
			g_pShared->fEyeFovUpRad[e]    = f.angleUp;
			g_pShared->fEyeFovDownRad[e]  = f.angleDown;
		}
		g_pShared->nFrameCounter  = nFrame;
		g_pShared->nHostAliveTick = GetTickCount();

		_ReadWriteBarrier();
		++g_pShared->nSequence;
	}

	void ReleaseSharedBlock()
	{
		if (g_pShared) { UnmapViewOfFile(g_pShared); g_pShared = nullptr; }
		if (g_hMap)    { CloseHandle(g_hMap);        g_hMap = nullptr; }
	}

	com_ptr<ID3D11Device> CreateDeviceOnAdapter(LUID luid, com_ptr<ID3D11DeviceContext>& ctxOut)
	{
		com_ptr<IDXGIFactory1> factory;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) return nullptr;

		com_ptr<IDXGIAdapter1> chosen;
		for (UINT i = 0; ; ++i)
		{
			com_ptr<IDXGIAdapter1> a;
			if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
			DXGI_ADAPTER_DESC1 ad{};
			a->GetDesc1(&ad);
			if (memcmp(&ad.AdapterLuid, &luid, sizeof(LUID)) == 0)
			{
				Msg("using adapter: %ls", ad.Description);
				chosen = a;
				break;
			}
		}
		// A RUNTIME THAT NAMES NO ADAPTER (LUID 0 - SteamVR's null driver, the
		// headless test runtime) gets the first hardware adapter. A real
		// headset's runtime always names one, so this never changes its choice.
		if (!chosen && luid.LowPart == 0 && luid.HighPart == 0)
		{
			for (UINT i = 0; ; ++i)
			{
				com_ptr<IDXGIAdapter1> a;
				if (factory->EnumAdapters1(i, a.put()) == DXGI_ERROR_NOT_FOUND) break;
				DXGI_ADAPTER_DESC1 ad{};
				a->GetDesc1(&ad);
				if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
				Msg("the runtime names no adapter (LUID 0) - using the first hardware one: %ls", ad.Description);
				chosen = a;
				break;
			}
		}
		if (!chosen) { Msg("FATAL: no adapter matches the runtime's LUID"); return nullptr; }

		com_ptr<ID3D11Device> dev;
		D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		if (FAILED(D3D11CreateDevice(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
			D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
			dev.put(), nullptr, ctxOut.put())))
		{
			Msg("FATAL: D3D11CreateDevice failed");
			return nullptr;
		}
		return dev;
	}
}

// THE HOST GAVE UP BEFORE THE HEADSET CAME UP: say why, in words a player can
// act on, and close the game rather than leave its menu drawn flat and
// stretched on the monitor with no explanation.
static void TellPlayerAndCloseGame(int nReason)
{
	const wchar_t* pszText =
		(nReason == 2)
		? L"NOLF VR could not find a VR headset.\n\n"
		  L"Connect the headset first: start streaming in Virtual Desktop, or start "
		  L"SteamVR or Oculus Link, and make sure the headset is awake and on your head.\n\n"
		  L"Then double-click Play NOLF VR.bat again. The game will close now."
		: (nReason == 1)
		? L"NOLF VR could not start OpenXR, the standard PC VR interface.\n\n"
		  L"Install or start your headset's PC software - Virtual Desktop (with its "
		  L"OpenXR runtime), SteamVR, or the Meta Quest Link app - and make it the "
		  L"active OpenXR runtime.\n\n"
		  L"Then double-click Play NOLF VR.bat again. The game will close now."
		: L"NOLF VR could not start the headset session. Collect report.bat saves the "
		  L"logs for a bug report.\n\nThe game will close now.";
	// The game first, then the message: the box is modal and waits for OK.
	// Every visible window of the engine's class, not the first FindWindow
	// returns - one close aimed at the wrong one left the menu up.
	EnumWindows([](HWND h, LPARAM) -> BOOL {
		wchar_t cls[64] = { 0 };
		GetClassNameW(h, cls, 63);
		if (wcscmp(cls, L"LithTech") == 0 && IsWindowVisible(h))
		{
			const BOOL bOk = PostMessageW(h, WM_CLOSE, 0, 0);
			Msg("closing the game window %p - the headset never came up (posted: %s)",
				(void*)h, bOk ? "yes" : "NO");
		}
		return TRUE; }, 0);
	MessageBoxW(nullptr, pszText, L"NOLF VR", MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
	// And again once the player has read it, in case the first did not land.
	EnumWindows([](HWND h, LPARAM) -> BOOL {
		wchar_t cls[64] = { 0 };
		GetClassNameW(h, cls, 63);
		if (wcscmp(cls, L"LithTech") == 0) PostMessageW(h, WM_CLOSE, 0, 0);
		return TRUE; }, 0);
}

int main(int argc, char** argv)
{
	HostLog::Open("vrhost");
	Msg("=== NOLF1 VR host - M5 ===");
	// SAY SO IF THE CONSOLE IS CLOSED. Closing this window ends the process
	// with no crash record and nothing in the log - which is exactly how one
	// session's host vanished while waiting for a headset, unexplained.
	SetConsoleCtrlHandler([](DWORD nEvent) -> BOOL {
		Msg("console event %lu (%s) - the host is being ended from outside", (unsigned long)nEvent,
			nEvent == CTRL_CLOSE_EVENT ? "its window was closed" :
			nEvent == CTRL_LOGOFF_EVENT ? "logoff" : nEvent == CTRL_SHUTDOWN_EVENT ? "shutdown" :
			nEvent == CTRL_C_EVENT ? "Ctrl+C" : nEvent == CTRL_BREAK_EVENT ? "Ctrl+Break" : "other");
		return FALSE; }, TRUE);

	const int nMaxMinutes = (argc > 1) ? atoi(argv[1]) : 60;

	// Pose lag in ms - how far behind the head the displayed frame is, end to
	// end. An estimate the player tunes by eye; too low and head roll shears,
	// too high and the world over-corrects and swims.
	const float fPoseLagMs = (argc > 2) ? (float)atof(argv[2]) : 45.0f;

	// Vertical FOV tuning knob - see XrVr::SetFovYScale.
	const float fFovYScale = (argc > 3) ? (float)atof(argv[3]) : 1.0f;

	// 0 = symmetric declared FOV, whole image. 1 = asymmetric FOV + sub-rect.
	const int nProjMode = (argc > 4) ? atoi(argv[4]) : 0;

	// Diagnostic: copy the SAME half of the window into both eyes. Not "the
	// same because IPD is zero" - the identical source region, twice. If this
	// still shows two images then the fault is in the OpenXR submission and
	// nothing to do with capture, the split, or the game.
	const bool bSameEye = (argc > 5) ? (atoi(argv[5]) != 0) : false;

	// Desktop right-eye preview. Off unless asked for - see below.
	const bool bWantMirror = (argc > 6) ? (atoi(argv[6]) != 0) : false;

	// FXAA on the submitted eyes. On by default - the renderer has no AA of its
	// own and the stairstepping crawls badly in a headset.
	const bool bAntiAlias = (argc > 7) ? (atoi(argv[7]) != 0) : true;

	// Advertise an older wire version, so a client built before the current
	// contract will attach. For bisecting only - see CreateSharedBlock.
	g_nWireVersion = (argc > 8) ? (uint32_t)atoi(argv[8]) : 0;

	// argv[9]: take the frame from the renderer's shared texture instead of
	// capturing the game window.
	//
	// DEFAULT OFF, deliberately. The whole path is desk-tested - 90 fps into a
	// separate 64-bit process, docs/EYE-TEXTURE-SHARED.md - but no part of it
	// has been through a headset, and window capture is what every headset
	// session so far has run on. Off by default makes the first headset test
	// an A/B between two things that both exist, rather than a change nobody
	// can undo from inside the session.
	const bool bUseShared = (argc > 9) ? (atoi(argv[9]) != 0) : false;

	// argv[10]: how a menu is fed to the panel. 1 (the default) gives each eye
	// its own half, exactly as in play. 0 is the old behaviour - the WHOLE
	// side-by-side window sent to both eyes.
	//
	// That old behaviour existed because the client drew its 2D once across
	// the pair, so a half was half of one flat picture and unreadable. Since
	// the renderer draws the 2D layer into both halves (docs/PER-EYE-2D.md)
	// each half is a complete menu, and sending both halves to one eye shows
	// two menus side by side, which cannot fuse.
	//
	// Kept as a switch rather than deleted: this is the one change in that
	// document that could not be tested at the desk, because it lives in the
	// 64-bit host and needs a runtime.
	const bool bMenuStereo = (argc > 10) ? (atoi(argv[10]) != 0) : true;

	// NAMED options, scanned anywhere in the line. The positional list above
	// is already ten deep and every addition to it is another place a script
	// can silently pass the wrong argument in the wrong slot.
	float fMenuWidth = 0.0f, fMenuDist = 0.0f;
	// THE MENU PANEL IS A 16:9 BAND. The eye is nearly square, and a panel
	// of that shape read as a tall card with lime bars top and bottom;
	// a wide one is what was preferred. --menu-aspect 0 presents the whole eye.
	// 3:2 SINCE 13 SEPTEMBER: at 16:9 the band cropped the top and bottom
	// of the authored menu by six per cent each (the 100-degree cap makes
	// the 3D core 1232 rows tall; a 16:9 band of a 1920-wide eye is 1080).
	// 3:2 is 1280 rows and shows the whole card with a little blue above
	// and below. The tester preferred wide and a little taller, judged by eye.
	float fMenuAspect = 1.5f;
	for (int i = 1; i + 1 < argc; ++i)
	{
		if (!strcmp(argv[i], "--menu-width"))  fMenuWidth  = (float)atof(argv[i + 1]);
		if (!strcmp(argv[i], "--menu-dist"))   fMenuDist   = (float)atof(argv[i + 1]);
		if (!strcmp(argv[i], "--menu-aspect")) fMenuAspect = (float)atof(argv[i + 1]);
	}
	// A wider panel keeps the band about as tall as the old panel was.
	if (fMenuWidth <= 0.0f && fMenuAspect > 1.0f) fMenuWidth = 3.2f;

	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	CreateSharedBlock();
	if (g_nWireVersion)
		Msg("WIRE VERSION FORCED to %u (ours is %u) - running an older client on purpose",
			g_nWireVersion, (unsigned)VRSHARED_VERSION);

	XrVr xr;
	xr.SetPoseLagMs(fPoseLagMs);
	xr.SetFovYScale(fFovYScale);
	xr.SetProjMode(nProjMode);
	xr.SetAntiAlias(bAntiAlias);
	xr.SetMenuPanel(fMenuWidth, fMenuDist);
	Msg("anti-aliasing: %s", bAntiAlias ? "FXAA" : "off");
	Msg("pose lag %.0f ms, vertical FOV scale %.2f, projection mode %d%s",
		fPoseLagMs, fFovYScale, nProjMode,
		bSameEye ? ", SAME-EYE DIAGNOSTIC (both eyes get the left image)" : "");
	if (!xr.CreateInstance()) { ReleaseSharedBlock(); TellPlayerAndCloseGame(xr.FailReason()); return 1; }

	com_ptr<ID3D11DeviceContext> context;
	com_ptr<ID3D11Device> device = CreateDeviceOnAdapter(xr.RequiredAdapter(), context);
	if (!device) { ReleaseSharedBlock(); TellPlayerAndCloseGame(0); return 1; }

	if (!xr.CreateSession(device.get())) { ReleaseSharedBlock(); TellPlayerAndCloseGame(0); return 1; }

	// Needed to scale a full-window menu into an eye-sized swapchain.
	xr.CreateBlitter(device.get());

	// The game must be running before capture can size the swapchains.
	Msg("start the game now - waiting for its window");
	WindowCapture capture;
	if (!capture.Start(device.get())) { xr.Shutdown(); ReleaseSharedBlock(); return 1; }

	if (!xr.CreateSwapchains(capture.EyeWidth(), capture.EyeHeight()))
	{
		xr.Shutdown(); ReleaseSharedBlock(); return 1;
	}
	Msg("rendering %dx%d per eye; runtime would prefer %dx%d (it will upscale)",
		capture.EyeWidth(), capture.EyeHeight(), xr.RecommendedWidth(), xr.RecommendedHeight());

	// The renderer's own texture, if it is publishing one and we were asked to
	// use it. Every refusal inside Start is a sentence naming a normal
	// condition, and every one of them means "carry on with window capture".
	SharedFrame sharedFrame;
	if (bUseShared)
	{
		if (sharedFrame.Start(device.get(), &Msg))
			Msg("FRAME SOURCE: the renderer's shared texture (window capture is idle)");
		else
			Msg("FRAME SOURCE: window capture - the shared texture was refused above");
	}
	else
	{
		Msg("FRAME SOURCE: window capture (pass argv[9]=1 for the shared texture)");
	}

	// Desktop preview of a single eye. OFF by default: it sits over the
	// borderless game window, so it both duplicates the picture on screen and
	// swallows mouse clicks meant for the game's menus. Opt in with -Mirror.
	MirrorWindow mirror;
	const bool bMarkerPixels = (getenv("NOLFVR_MARKER_PIXELS") != nullptr);
	const bool bMirror = bWantMirror &&
		mirror.Create(device.get(), capture.EyeWidth(), capture.EyeHeight(), true);
	if (bMirror) mirror.CoverWindow(capture.Window());

	// --- main loop --------------------------------------------------------
	std::vector<double> submitMs;
	submitMs.reserve(4096);

	uint32_t nFrame = 0, nPresented = 0, nNoImage = 0, nRebuilds = 0;
	double   fLastRebuildMs = -1e9;
	// The eye size the swapchains were last made for. See the check at the top
	// of the loop: they must follow the FRAME, not the window.
	int      nChainSrcW = capture.EyeWidth(), nChainSrcH = capture.EyeHeight();
	double   fWaitMs = 0.0, fLoopMs = 0.0, tPrevLoop = -1.0;
	// Where the frame drops are: loop periods over 16 and 22 ms per window,
	// and the worst. In the headset it was not a solid 90; the average said 89.
	uint32_t nLong16 = 0, nLong22 = 0; double fLoopWorst = 0.0;
	// WHERE A LONG LOOP GOES. A run: 105-144 ms loop periods, 5-9 per
	// ten seconds, and the game waiting on a silent host as often. Each loop
	// is stamped at its steps; any loop over 40 ms is logged with the split,
	// and the window line counts long WAITS apart from long WORK.
	uint32_t nLongWait = 0, nLongWork = 0; double fWaitWorst = 0.0, fWorkWorst = 0.0;
	double tStepCopy = 0.0, tStepMarker = 0.0, tStepSubmit = 0.0, tStepEnd = 0.0;
	uint32_t nStallLogged = 0;
	uint32_t nLoops = 0;
	uint32_t nFreshFrames = 0, nReusedFrames = 0;
	// When the game last handed us a new picture, and whether the stall has
	// been reported. See the no-fresh-frame line in the loop.
	double tLastFresh = 0.0; bool bStaleSaid = false;
	bool bSharedStaleSaid = false;		// the shared frame's stale state as last logged
	double   fAgeSum = 0.0, fAgeMax = 0.0;
	uint32_t nAgeSamples = 0;
	double   fTrueSum = 0.0, fTrueMax = 0.0;
	uint32_t nTrueSamples = 0, nMarkMiss = 0, nMarkFail = 0;
	int      nHeldMarker = -1;		// marker of the image currently on screen
	bool     bCalibDone  = false;	// one measurement per arming
	bool     bCalibSeen  = false;
	bool     bGeometryLocked = false;	// applied the game's surface size once
	bool     bDumped = false;
	bool     bAskedRefresh = false;
	bool     bWasMenu = false;
	bool     bWasPauseQuad = false;
	// Armed when the world first appears, not at startup. A fixed 75 s from
	// launch never fired in the 4 September headset run, which lasted 64.9 s -
	// so the one picture of what the runtime actually received was missing
	// from exactly the session that needed explaining. Level load time varies
	// with resolution; entry into the world does not.
	double   fDumpAtMs = 1e18;
	com_ptr<ID3D11Texture2D> lastFrame;

	// Which source lastFrame came from. It travels WITH the frame rather than
	// with the loop iteration: a frame is submitted more than once when no new
	// one has arrived, and the two sources need different crops, so deciding
	// this per iteration would use the wrong crop on every repeat.
	bool bFrameShared = false;
	const double tEnd = HostLog::NowMs() + nMaxMinutes * 60000.0;
	double tNextStat = HostLog::NowMs() + 10000.0;

	while (HostLog::NowMs() < tEnd)
	{
		if (!xr.PumpEvents()) break;
		if (bMirror && !mirror.Pump()) { Msg("mirror window closed"); break; }
		if (bMirror) mirror.FollowForeground(capture.Window());
		if (!capture.IsWindowAlive()) { Msg("game window gone - shutting down"); break; }
		if (!capture.GameAlive())     { Msg("game process gone - shutting down"); break; }

		// Resolution changes resize the window under us. Everything sized from
		// the client rect - crop offsets, eye size, swapchains - goes stale,
		// which showed up as an off-centre split view rather than an obvious
		// failure. Rebuild instead of quietly cropping the wrong region.
		// Rate-limited and counted. A rebuild blocks for up to a second without
		// calling xrWaitFrame/xrEndFrame, which stalls the session - so if the
		// detector ever misfires, thrashing must be visible and bounded rather
		// than presenting as a black headset at 1fps.
		// NOT WHILE THE RENDERER'S FRAME IS THE PICTURE. Then the window is not
		// being read at all, and a rebuild only puts at risk what already works:
		// it stalled the headset while it waited for the window, and after an
		// alt-tab that minimized the game it waited 30 seconds, measured the
		// minimized window, called it impossible and quit - closing the game
		//. Note the new size and move the mirror.
		if (sharedFrame.IsOpen() && capture.HasResized())
		{
			capture.AcceptCurrentSize();
			Msg("game window is now %dx%d - the renderer's frame is the picture, so the capture is not rebuilt",
				capture.ClientW(), capture.ClientH());
			if (bMirror) mirror.CoverWindow(capture.Window());
		}
		else if (capture.HasResized() && HostLog::NowMs() - fLastRebuildMs > 2000.0)
		{
			fLastRebuildMs = HostLog::NowMs();
			bGeometryLocked = false;		// re-apply the game surface after rebuild
			if (++nRebuilds > 20)
			{
				Msg("FATAL: %u capture rebuilds - the resize detector is misfiring, giving up", nRebuilds);
				break;
			}
			Msg("resize detected (rebuild %u)", nRebuilds);

			lastFrame = nullptr;
			xr.DestroySwapchains();
			if (!capture.Restart(device.get()))
			{
				Msg(capture.GameAlive() ? "FATAL: capture restart failed"
										: "the game quit during the rebuild - shutting down");
				break;
			}
			// The renderer's frame when it is open (see below): the window's
			// size is only the frame's size when the window is the source.
			const bool bShared = sharedFrame.IsOpen() && sharedFrame.EyeWidth() > 0 && sharedFrame.EyeHeight() > 0;
			const int nNewW = bShared ? sharedFrame.EyeWidth()  : capture.EyeWidth();
			const int nNewH = bShared ? sharedFrame.EyeHeight() : capture.EyeHeight();
			if (!xr.CreateSwapchains(nNewW, nNewH))
			{
				Msg("FATAL: swapchain rebuild failed");
				break;
			}
			nChainSrcW = nNewW; nChainSrcH = nNewH;
			Msg("rebuilt for %dx%d per eye (%s; window %dx%d per eye)", nNewW, nNewH,
				bShared ? "the renderer's frame" : "the window", capture.EyeWidth(), capture.EyeHeight());
			if (bMirror) mirror.CoverWindow(capture.Window());	// it moved or resized
		}

		// THE SWAPCHAINS FOLLOW THE FRAME, NOT THE WINDOW. A rebuild sizes them
		// from the window's client area, and the game's window changes height
		// during play (3840x2076 and 3840x2160 both occur) while the renderer's
		// shared frame stays 1920x2076 per eye. Mismatched, every eye went
		// through the scaler - the world stretched about 4% taller than the
		// field it was declared with - and so did the pause overlay, whose
		// scaler writes alpha 1: the see-through pause quad became an opaque
		// black card. Reported from the headset after a load, 24 September; the
		// log showed swapchains of 1920x2160 from rebuild 3 until the quit.
		// Same safe point, same rate limit, as the rebuild above.
		if (sharedFrame.IsOpen() && sharedFrame.EyeWidth() > 0 && sharedFrame.EyeHeight() > 0
			&& (sharedFrame.EyeWidth() != nChainSrcW || sharedFrame.EyeHeight() != nChainSrcH)
			&& HostLog::NowMs() - fLastRebuildMs > 2000.0)
		{
			fLastRebuildMs = HostLog::NowMs();
			Msg("swapchains were made for %dx%d per eye, the renderer's frames are %dx%d - remaking them to match",
				nChainSrcW, nChainSrcH, sharedFrame.EyeWidth(), sharedFrame.EyeHeight());
			lastFrame = nullptr;
			xr.DestroySwapchains();
			if (!xr.CreateSwapchains(sharedFrame.EyeWidth(), sharedFrame.EyeHeight()))
			{
				Msg("FATAL: swapchain remake failed");
				break;
			}
			nChainSrcW = sharedFrame.EyeWidth(); nChainSrcH = sharedFrame.EyeHeight();
		}

		// Time the runtime wait separately from our own work. The two have
		// completely different fixes and they are indistinguishable from the
		// frame count alone: if xrWaitFrame is where the time goes, the runtime
		// is pacing us and the headset refresh rate is the thing to change; if
		// our work is, the bottleneck is here in the host.
		const double tBeforeWait = HostLog::NowMs();
		bool bShouldRender = false;
		// Tag the pose BEFORE locating it. BeginFrame is what writes the ring
		// entry, so setting the frame number after it stamped every entry with
		// the PREVIOUS iteration's number - and the client draws the newest
		// published number, for which no entry existed yet. That alone makes
		// the freshest, most correct marker reads miss.
		xr.NotePoseFrame(nFrame + 1);
		// RECENTER, BEFORE this frame's poses are located, so the first pose
		// published in the new space travels with the new generation number.
		// Placed after it, the client saw the new number beside the LAST
		// frame's pose - the old space - and referenced itself to that.
		if (g_pShared)
		{
			static uint32_t s_nReqSeen = 0;
			if (g_pShared->nRecenterReq != s_nReqSeen)
			{
				s_nReqSeen = g_pShared->nRecenterReq;
				xr.Recenter();
				++g_pShared->nRecenterGen;
			}
			if (xr.ConsumeSpaceChanged()) ++g_pShared->nRecenterGen;
		}
		if (!xr.BeginFrame(bShouldRender)) break;
		const double tAfterWait = HostLog::NowMs();
		fWaitMs += tAfterWait - tBeforeWait;
		if (tPrevLoop > 0.0)
		{
			const double fP = tAfterWait - tPrevLoop;
			fLoopMs += fP;
			if (fP > 16.0) ++nLong16;
			if (fP > 22.0) ++nLong22;
			if (fP > fLoopWorst) fLoopWorst = fP;
		}
		{
			const double fW = tAfterWait - tBeforeWait;
			if (fW > fWaitWorst) fWaitWorst = fW;
			if (fW > 16.0) ++nLongWait;
			// The previous loop's work, from its wait's end to this wait's start.
			if (tStepEnd > 0.0 && tPrevLoop > 0.0)
			{
				const double fWork = tBeforeWait - tPrevLoop;
				if (fWork > fWorkWorst) fWorkWorst = fWork;
				if (fWork > 16.0) ++nLongWork;
				if ((fWork > 40.0 || fW > 40.0) && nStallLogged < 40)
				{
					++nStallLogged;
					Msg("HOST STALL: wait %.1f ms | work %.1f = copy %.1f + marker %.1f + submit %.1f + endframe %.1f (xrEndFrame itself %.1f) + rest %.1f",
						fW, fWork, tStepCopy - tPrevLoop, tStepMarker - tStepCopy,
						tStepSubmit - tStepMarker, tStepEnd - tStepSubmit, xr.LastEndCallMs(), tBeforeWait - tStepEnd);
				}
			}
		}
		tPrevLoop = tAfterWait;
		++nLoops;

		if (!xr.IsRunning()) { Sleep(5); continue; }

		// Once, as soon as the session is live: ask for 90 Hz. The client
		// renders ~89 fps, so anything the runtime gives below that is the
		// binding constraint on the whole chain.
		if (!bAskedRefresh)
		{
			bAskedRefresh = true;
			xr.RequestRefreshRate(90.0f);
			// Actions must be attached to a live session, so this cannot go
			// alongside the swapchains.
			xr.CreateActions();
		}

		xr.UpdateActions();

		// The client's haptic requests (block v17): one pulse per new serial.
		if (g_pShared && g_pShared->nVersion >= 17)
		{
			static uint32_t s_nHapticSeen = 0;
			const uint32_t nS = g_pShared->nHapticSerial;
			if (nS != s_nHapticSeen)
			{
				s_nHapticSeen = nS;
				xr.Pulse((int)g_pShared->nHapticHand, g_pShared->fHapticAmp, g_pShared->fHapticMs);
			}
		}

		++nFrame;
		PublishPose(xr, nFrame);		// the ring entry for nFrame already exists

		// Take the client's rendered FOV so the declaration always matches.
		if (g_pShared) xr.SetClientFov(g_pShared->fGameFovXRad, g_pShared->fGameFovYRad);
		// Written by the renderer, not the client. Read every frame rather
		// than latched, so a renderer that loses the block mid-run - the
		// host restarting, say - takes the declaration back with it.
		if (g_pShared) xr.SetNativeFrustum(g_pShared->nNativeFrustum != 0);

		// Follow the client on the optical centres - it decides, we declare.
		// Declare exactly what the client applied, not what we would have
		// derived. Two ends deriving the same value independently is how the
		// eyes end up disagreeing, and a disagreement here is unfusable rather
		// than merely wrong.
		if (g_pShared)
			xr.SetAsymCentres(g_pShared->nAsymActive != 0,
				g_pShared->fAppliedYawRad, g_pShared->fAppliedPitchRad);

		// Same rule for the head: the client decides whether the rotation is in
		// its camera, we obey. Under VRHeadAsMouse it is, so the layer goes up
		// head-locked and the runtime reprojects nothing.
		if (g_pShared) xr.SetHeadLocked(g_pShared->nHeadLocked != 0);

		// The body yaw, and whether to carry it into the declared frame. This
		// is the frame the client actually rendered in; the host has never
		// known it, and that gap is the world bending.
		if (g_pShared)
			xr.SetBodyYaw(g_pShared->fBodyYawRad, (int)g_pShared->nYawSpaceMode);

		// Pose lag is swept from the game console, so it can change mid-run.
		// Ignore an unset value - an older client leaves it zero.
		if (g_pShared && g_pShared->nPoseLagValid &&
			g_pShared->fPoseLagMs != xr.PoseLagMs())
		{
			xr.SetPoseLagMs(g_pShared->fPoseLagMs);
			Msg("pose lag now %.1f ms (set from the game console)", g_pShared->fPoseLagMs);
		}

		// NOTE: an earlier version re-cropped to the size the game reported for
		// its render surface, assuming the surface sat centred inside a
		// slightly larger client with a border. That is wrong whenever Windows
		// scales the window - the game's picture then fills the whole client,
		// and cropping a smaller centred region straddles the stereo seam, so
		// each eye received parts of BOTH halves. That was the double image.
		//
		// Splitting the client at its midpoint is correct either way: with a
		// symmetric border the seam is still the centre, and with scaling the
		// picture fills the client. It costs a few border pixels at the outer
		// edges and assumes nothing.
		if (g_pShared && g_pShared->nGameScreenW > 0 && !bGeometryLocked)
		{
			bGeometryLocked = true;
			Msg("game render surface %ux%u, window client %dx%d (ratio %.3f) - splitting the client at its midpoint",
				g_pShared->nGameScreenW, g_pShared->nGameScreenH,
				capture.EyeWidth() * 2, capture.EyeHeight(),
				(double)(capture.EyeWidth() * 2) / (double)g_pShared->nGameScreenW);
		}

		bool bHaveImage = false;
		if (bShouldRender)
		{
			// Reuse the previous frame if capture has not delivered a new one.
			// Repeating is better than dropping the layer, but it is NOT free:
			// a repeated frame is visible judder, so how often this happens is
			// the difference between a smooth image and a stuttering one. The
			// game's own frame rate does not settle it - Windows Graphics
			// Capture delivers frames when the compositor presents the game
			// window, which happens at the refresh rate of the display that
			// window is on, not the rate the game renders at.
			// Prefer the renderer's own texture when it is open. Window capture
			// publishes at the monitor's refresh, so a third of what reaches a
			// 90 Hz headset is duplicated; this arrives at whatever rate the
			// game draws. Falls straight back if the source ever goes stale -
			// a level reload, or the renderer being replaced.
			// KEEP ASKING FOR THE SHARED TEXTURE. The renderer creates its
			// device after the game window appears and publishes its first
			// frame a little after that; a single check at startup lands
			// before either and condemns the run to window capture. Once a
			// second for the first minute, then never again.
			if (bUseShared && !sharedFrame.IsOpen())
			{
				static DWORD s_dwSharedFirst = 0, s_dwSharedTry = 0;
				const DWORD dwNowS = GetTickCount();
				if (!s_dwSharedFirst) s_dwSharedFirst = dwNowS;
				if (dwNowS - s_dwSharedFirst < 60000
					&& dwNowS - s_dwSharedTry >= 1000)
				{
					s_dwSharedTry = dwNowS;
					if (sharedFrame.Start(device.get(), &Msg))
						Msg("FRAME SOURCE: the renderer's shared texture, opened"
							" after %u ms of asking (window capture is idle)",
							(unsigned)(dwNowS - s_dwSharedFirst));
				}
			}
			// THE RENDERER WAS REPLACED (an alt-tab reloads it): drop the old
			// textures and open the new ones by name - see
			// SharedFrame::Restarted. Retried four times a second with no time
			// limit, unlike the startup ask above, because a focus change can
			// come at any point in a session. Window capture covers the gap.
			{
				static bool  s_bReopen = false;
				static DWORD s_dwReopenTry = 0;
				static DWORD s_dwReopenFrom = 0;
				if (bUseShared && sharedFrame.IsOpen() && sharedFrame.Restarted())
				{
					sharedFrame.Stop();
					s_bReopen = true;
					s_dwReopenFrom = GetTickCount();
					Msg("FRAME SOURCE: the renderer restarted - reopening its shared texture");
				}
				if (bUseShared && s_bReopen && !sharedFrame.IsOpen()
					&& GetTickCount() - s_dwReopenTry >= 250)
				{
					s_dwReopenTry = GetTickCount();
					if (sharedFrame.Start(device.get(), &Msg))
					{
						s_bReopen = false;
						bSharedStaleSaid = false;
						Msg("FRAME SOURCE: the new renderer's shared texture, opened %u ms"
							" after the restart", (unsigned)(GetTickCount() - s_dwReopenFrom));
					}
				}
			}
			bool bThisShared = false;
			winrt::com_ptr<ID3D11Texture2D> tex;
			// STALE IS NOT FOREVER. The shared frame is polled on EVERY pass,
			// stale or not. It used to be read only while fresh, so once it
			// went stale nothing ever refreshed its clock and the run was
			// latched onto window capture for good. A startup resize blocks this
			// loop ~0.7 s while the renderer resets, and when the two added up
			// past the 1 s stale limit the headset showed a capture of our own
			// mirror window: a frozen, doubled picture while the game ran on.
			if (bUseShared && sharedFrame.IsOpen())
			{
				ID3D11Texture2D* pS = sharedFrame.TryGetFrame();
				const bool bStale = sharedFrame.IsStale(GetTickCount());
				if (bStale != bSharedStaleSaid)
				{
					bSharedStaleSaid = bStale;
					Msg(bStale ? "FRAME SOURCE: shared texture stale for 1 s - window capture meanwhile"
							   : "FRAME SOURCE: back on the renderer's shared texture");
				}
				// WAIT A MOMENT FOR THE GAME'S NEXT FRAME BEFORE REPEATING THE
				// LAST ONE. The renderer presents the instant our tick lands
				// (xrWaitFrame), and this read comes a moment later in the same
				// pass - so a frame arriving a fraction of a millisecond after
				// the read was repeated even though it existed. And in combat the
				// game's own frame took ~10.6 of its 11.1 ms, so its frames
				// landed just after this read for seconds at a time: bursts of
				// 106-212 repeats per 10 s with the game itself at a flat 90
				// (a headset session, 24 September). Up to NOLFVR_FRESH_WAIT_MS
				// of the ~9 ms this loop spends waiting on the runtime anyway.
				// OFF BY DEFAULT: at the desk it cost 0.01 ms a loop and changed
				// nothing, because a desk run has no bursts to catch (0.6
				// repeats per 10 s with it on and off) - whether it helps is a
				// headset question. play-vr -FreshWaitMs 3 turns it on.
				static double s_fFreshWaitMs = -1.0;
				static long   s_nFreshCaught = 0, s_nFreshMissed = 0, s_nFreshLoops = 0;
				static double s_fFreshSpent = 0.0;
				if (s_fFreshWaitMs < 0.0)
				{
					const char* pszW = getenv("NOLFVR_FRESH_WAIT_MS");
					s_fFreshWaitMs = pszW ? atof(pszW) : 0.0;
					if (s_fFreshWaitMs < 0.0) s_fFreshWaitMs = 0.0;
					if (s_fFreshWaitMs > 6.0) s_fFreshWaitMs = 6.0;
					Msg("fresh-frame wait: up to %.1f ms for the game's next frame before repeating one"
						" (NOLFVR_FRESH_WAIT_MS)", s_fFreshWaitMs);
				}
				if (!bStale && !pS && s_fFreshWaitMs > 0.0)
				{
					const double t0 = HostLog::NowMs();
					for (;;)
					{
						const double tNow = HostLog::NowMs();
						if (tNow - t0 >= s_fFreshWaitMs) { ++s_nFreshMissed; break; }
						YieldProcessor();
						pS = sharedFrame.TryGetFrame();
						if (pS) { ++s_nFreshCaught; break; }
					}
					s_fFreshSpent += HostLog::NowMs() - t0;
				}
				if (++s_nFreshLoops >= 900)
				{
					if (s_fFreshWaitMs > 0.0)
						Msg("  fresh-frame wait: %ld frames caught by waiting (would have repeated), %ld still"
							" repeated after %.1f ms; %.2f ms waited per loop on average",
							s_nFreshCaught, s_nFreshMissed, s_fFreshWaitMs, s_fFreshSpent / (double)s_nFreshLoops);
					s_nFreshCaught = s_nFreshMissed = s_nFreshLoops = 0; s_fFreshSpent = 0.0;
				}
				if (!bStale)
				{
					if (pS)
					{
						tex.copy_from(pS);
						bThisShared = true;
					}
					// No new shared frame this iteration is NOT a reason to fall
					// back to capture for one frame: mixing the two sources would
					// alternate between two different crops.
				}
				else
				{
					tex = capture.TryGetFrame();
				}
			}
			else
			{
				tex = capture.TryGetFrame();
			}

			// THE GAME STOPPED PRESENTING while its process and window live on.
			// On 21 September the client's log stopped 2.6 s into a level, this
			// loop repeated the last picture for ~15 s, and the host then died on
			// a fail-fast with no line between - so nothing said whether the game
			// had hung, died or merely stopped drawing. Say it once, with the
			// three facts that tell those apart.
			if (!tex && tLastFresh > 0.0 && !bStaleSaid && HostLog::NowMs() - tLastFresh > 3000.0)
			{
				bStaleSaid = true;
				Msg("no fresh frame from the game for %.1f s - process alive %d, window alive %d, window responding %d",
					(HostLog::NowMs() - tLastFresh) / 1000.0, capture.GameAlive() ? 1 : 0,
					capture.IsWindowAlive() ? 1 : 0,
					(capture.Window() && !IsHungAppWindow(capture.Window())) ? 1 : 0);
				// ...AND WHERE IT IS STUCK. The same stall came back on 24 September,
				// 43 s into M05S01 while firing, and again nothing inside the game
				// said a word. Every thread's stack is taken from the outside by
				// tools/crashtools/stackdump32.exe into this run's folder. Not waited
				// on: this loop keeps presenting the last picture meanwhile.
				DWORD dwPid = 0;
				if (capture.Window()) GetWindowThreadProcessId(capture.Window(), &dwPid);
				if (dwPid)
				{
					char szOut[MAX_PATH], szCmd[512];
					sprintf_s(szOut, "%s\\stall-stacks.txt", HostLog::Dir());
					sprintf_s(szCmd, "\"tools\\crashtools\\stackdump32.exe\" %lu", (unsigned long)dwPid);
					SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
					HANDLE hOut = CreateFileA(szOut, GENERIC_WRITE, FILE_SHARE_READ, &sa,
											  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
					STARTUPINFOA si{}; si.cb = sizeof(si);
					si.dwFlags = STARTF_USESTDHANDLES;
					si.hStdOutput = hOut; si.hStdError = hOut; si.hStdInput = nullptr;
					PROCESS_INFORMATION pi{};
					if (hOut != INVALID_HANDLE_VALUE
						&& CreateProcessA(nullptr, szCmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
										  nullptr, nullptr, &si, &pi))
					{
						Msg("stall: every thread's stack of pid %lu going to %s", (unsigned long)dwPid, szOut);
						CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
					}
					else
						Msg("stall: could not run the stack dump (error %lu)", (unsigned long)GetLastError());
					if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
				}
			}
			if (tex)
			{
				lastFrame = tex;
				tStepCopy = HostLog::NowMs();
				bFrameShared = bThisShared;
				++nFreshFrames;
				tLastFresh = HostLog::NowMs(); bStaleSaid = false;
				// Field calibration: the two halves of this frame are the same
				// scene from angles differing by a known yaw, so the shift
				// between them gives the renderer's focal length in pixels and
				// hence the field it actually produced.
				// Log the moment the flag is seen, so "no output" can be told
				// apart from "the host never saw the request".
				if (g_pShared && g_pShared->nCalibActive && !bCalibSeen)
				{
					bCalibSeen = true;
					Msg("calibration: request seen from the client (yaw %.2f deg)",
						g_pShared->fCalibYawRad * 57.2957795f);
				}

				if (g_pShared && g_pShared->nCalibActive && !bCalibDone)
				{
					const int nShift = capture.MeasureHalfShift(device.get(), context.get(), lastFrame.get());
					if (nShift > 0)
					{
						const double fYaw    = g_pShared->fCalibYawRad;
						const double fFocal  = (double)nShift / tan(fYaw);
						const double fHalf   = atan((double)(capture.EyeWidth() / 2) / fFocal);
						const double fAsked  = g_pShared->fGameFovXRad * 0.5;
						const double fRatio  = (fAsked > 0.0) ? tan(fHalf) / tan(fAsked) : 0.0;

						Msg("FIELD MEASURED: yaw %.1f deg -> %d px, focal %.0f px",
							fYaw * 57.2957795, nShift, fFocal);
						Msg("  published half-fovX %.2f deg, renderer produced %.2f deg (tan ratio %.3f)",
							fAsked * 57.2957795, fHalf * 57.2957795, fRatio);
						if (fRatio > 0.05)
							Msg("  -> VRFovXTest should be multiplied by %.3f", 1.0 / fRatio);
						bCalibDone = true;
					}
				}
				if (g_pShared && !g_pShared->nCalibActive) { bCalibDone = false; bCalibSeen = false; }

				// The true staleness, from the client's own stamp. Measured,
				// not guessed - the two previous attempts (a fixed lag, and
				// the compositor's timestamp) were both wrong.
				// The crop must match the SOURCE of lastFrame, not the window
				// capture. The shared texture is the render surface itself and
				// has no chrome to skip; the captured window has both a border
				// and a title bar. Using the window's crop on a shared frame read
				// 37 rows below the marker, and the log said so for a whole
				// headset session: "897 unreadable, 0 unmatched".
				// Take the crop from the marker itself before reading it.
				//
				// Window metrics put the client area at (2,37) inside the
				// captured texture; the picture actually starts at (0,0), so
				// every read landed 37 rows below an 8-row marker and the log
				// said "563 unreadable, 0 unmatched" for the whole run. The
				// same 37 rows ran the eye rect off the bottom of the picture.
				//
				// Retried rather than done once: the renderer does not draw the
				// marker outside GS_PLAYING, so the first frames carry none.
				// Shared frames are the render surface itself and are (0,0) by
				// construction, so they are left alone.
				static bool   s_bCropCal   = false;
				static double s_fNextCalMs = 0.0;
				if (!s_bCropCal && !bFrameShared)
				{
					const double fNowCal = HostLog::NowMs();
					if (fNowCal >= s_fNextCalMs)
					{
						s_fNextCalMs = fNowCal + 500.0;
						s_bCropCal = capture.CalibrateCropFromMarker(
							device.get(), context.get(), lastFrame.get());
					}
				}

				const int nMarkX = bFrameShared ? sharedFrame.CropX() : capture.CropX();
				const int nMarkY = bFrameShared ? sharedFrame.CropY() : capture.CropY();
				// FROM MEMORY WHEN THE BLOCK CARRIES IT (v16). The pixel read is a
				// GPU readback that waited behind the shared-texture copy, which
				// waited on the game's mutex release, which waited behind us:
				// 82-148 ms every two to three seconds, every one in this step.
				// NOLFVR_MARKER_PIXELS=1 in the environment keeps reading pixels.
				const int nMark = (bFrameShared && !bMarkerPixels && sharedFrame.Marker() >= 0)
					? sharedFrame.Marker()
					: capture.ReadFrameMarker(device.get(), context.get(),
											  lastFrame.get(), nMarkX, nMarkY);
				if (nMark >= 0)
				{
					const double fTrue = xr.AgeOfPoseByte((uint32_t)nMark);
					if (fTrue >= 0.0)
					{
						fTrueSum += fTrue;
						if (fTrue > fTrueMax) fTrueMax = fTrue;
						++nTrueSamples;
					}
					else ++nMarkMiss;

					// Hold this marker until a new image arrives, so repeats
					// of the same picture keep the pose it was drawn from and
					// the runtime warps each one forward by the right amount.
					nHeldMarker = (g_pShared && g_pShared->nExactPose) ? nMark : -1;
				}
				else { ++nMarkFail; nHeldMarker = -1; }

				const double a = capture.LastFrameAgeMs();
				fAgeSum += a;
				// Track the largest magnitude, not the largest signed value -
				// seeded at zero it reported "max 0.0" for a run in which every
				// sample was negative, which reads as "no variation at all".
				if (nAgeSamples == 0 || fabs(a) > fabs(fAgeMax)) fAgeMax = a;
				++nAgeSamples;
			}
			else ++nReusedFrames;

			if (lastFrame)
			{
				// A menu is still presented as a world-locked flat panel - that
				// part is right and stays. What changed is what FEEDS it: the
				// renderer now draws the 2D layer into both halves, so each
				// half is a complete menu and the panel takes an ordinary eye
				// pair. See bMenuStereo above.
				const bool bMenu = g_pShared && g_pShared->nInMenu != 0;

				// In a menu, send the WHOLE window to both eyes and let the
				// blitter scale it down with letterboxing. Cropping a
				// half-width region showed the middle of a huge picture with
				// the edges cut off.
				// The shared texture IS the render surface, so it has no window
				// chrome to skip. Window capture has to crop past it. This is
				// the ONLY geometry that differs between the two sources - the
				// eye size is the game's screen either way.
				const int nCropX = bFrameShared ? sharedFrame.CropX() : capture.CropX();
				const int nCropY = bFrameShared ? sharedFrame.CropY() : capture.CropY();

				// THE EYE SIZE OF THE FRAME BEING SUBMITTED. It was always the
				// window's, and the window can be 3840x2160 while the renderer's
				// frame is 3840x2076: the eyes then read 2160 rows from a
				// 2076-row picture and the world came out squashed ("THE WORLD
				// IS IN A BOX" in the log). Same fix as the swapchains above.
				const int nEyeW = bFrameShared ? sharedFrame.EyeWidth()  : capture.EyeWidth();
				const int nEyeH = bFrameShared ? sharedFrame.EyeHeight() : capture.EyeHeight();
				int nLx = nCropX;
				int nRx = nCropX + (bSameEye ? 0 : nEyeW);
				int nSw = nEyeW;
				int nSh = nEyeH;
				if (bMenu && !bMenuStereo)
				{
					nLx = nRx = nCropX;
					nSw = nEyeW * 2;
				}
				// The band: centred rows of the eye at the panel's aspect. The
				// renderer fits the layout to the same band (R2D_SetMenuBand),
				// so what is cropped here is only the backdrop's margin.
				int nSy = nCropY;
				if (bMenu && fMenuAspect > 0.0f)
				{
					const int nBand = (int)((float)nSw / fMenuAspect);
					if (nBand > 0 && nBand < nSh)
					{
						nSy = nCropY + (nSh - nBand) / 2;
						nSh = nBand;
					}
				}

				const double t0 = HostLog::NowMs();
				// bMenu decides the panel shrink inside the blit. Without it the
				// blitter cannot tell a menu panel from the world and shrank both.
				tStepMarker = HostLog::NowMs();
				const bool a = xr.SubmitEye(context.get(), lastFrame.get(), 0,
					nLx, nSy, nSw, nSh, bMenu);
				const bool b = xr.SubmitEye(context.get(), lastFrame.get(), 1,
					nRx, nSy, nSw, nSh, bMenu);
				submitMs.push_back(HostLog::NowMs() - t0);
				tStepSubmit = HostLog::NowMs();

				if (bMenu != bWasMenu)
				{
					bWasMenu = bMenu;
					Msg(bMenu ? (bMenuStereo
									? "menu: world-locked panel, one half per eye"
									: "menu: world-locked panel, whole window to both eyes")
							  : "world: stereo split resumed");
					if (bMenu)
						Msg("menu: band %d x %d from row %d of %d (aspect %.2f)",
							nSw, nSh, nSy - nCropY, capture.EyeHeight(), fMenuAspect);
					// Re-anchor each time a menu opens, so it appears in front
					// of wherever the player is looking.
					if (bMenu) xr.AnchorMenuPanel();

					// The world is up: take the picture shortly after, while the
					// player is still in it.
					// A DESK TOOL, OFF IN PLAY: set NOLFVR_DUMP=1 to enable. It
					// writes three full-size pictures (~80 MB) into logs\ on every
					// launch and costs one ~45 ms frame while it does.
					static const bool s_bAutoDumpOn = [] {
						char v[8] = { 0 };
						return GetEnvironmentVariableA("NOLFVR_DUMP", v, sizeof(v)) > 0 && v[0] == '1';
					}();
					if (s_bAutoDumpOn && !bMenu && !bDumped && fDumpAtMs > 1e17)
					{
						fDumpAtMs = HostLog::NowMs() + 5000.0;
						Msg("dump: armed for 5 s from now (the world is up)");
					}
				}
				if (bMenu) xr.UpdateMenuAnchor();

				// Desktop preview - the right eye, or the whole menu.
				if (bMirror)
				{
					mirror.Present(context.get(), lastFrame.get(),
						nRx, nSy, nSw, nSh);
				}
				bHaveImage = a && b;
				if (bHaveImage) ++nPresented;

				// Dump what the runtime actually received, once, well after
				// startup so there is time to load a level first.
				// BOTH THUMBSTICKS CLICKED: dump both eyes NOW, with a suffix,
				// two seconds apart at most. Headset testing sees a one-eye
				// shimmer on Cate's hair and the reception's security camera
				// that the desk cannot reach; this captures the exact moment.
				{
					static double s_fLastChordMs = 0.0;
					static int    s_nChordDumps = 0;
					// A DESK TOOL, OFF IN PLAY: set NOLFVR_DUMP_CHORD=1 to enable.
					// The same chord was the game's recenter, so every recenter
					// wrote three full-size pictures (~70 MB) and hitched.
					static const bool s_bChordOn = [] {
						char v[8] = { 0 };
						return GetEnvironmentVariableA("NOLFVR_DUMP_CHORD", v, sizeof(v)) > 0 && v[0] == '1';
					}();
					const bool bChord = s_bChordOn && g_pShared
						&& (g_pShared->Hands[0].nButtons & VRBTN_THUMBCLICK)
						&& (g_pShared->Hands[1].nButtons & VRBTN_THUMBCLICK);
					if (bChord && HostLog::NowMs() - s_fLastChordMs > 2000.0)
					{
						s_fLastChordMs = HostLog::NowMs();
						++s_nChordDumps;
						bDumped = false;
						fDumpAtMs = 0.0;
						Msg("dump: both thumbsticks clicked - dumping both eyes (chord %d)", s_nChordDumps);
					}
				}
				if (!bDumped && HostLog::NowMs() > fDumpAtMs)
				{
					bDumped = true;
					xr.RequestDump();

					char szWin[64];
					static int s_nWinDump = 0;
					sprintf_s(szWin, "logs\\captured-window-%d.bmp", ++s_nWinDump);
					HostDumpTexture(device.get(), context.get(), lastFrame.get(), szWin);
					Msg("dump: crop (%d,%d), eye %dx%d - left eye from x=%d, right eye from x=%d",
						capture.CropX(), capture.CropY(),
						capture.EyeWidth(), capture.EyeHeight(),
						capture.CropX(), capture.CropX() + capture.EyeWidth());
				}
			}
			else ++nNoImage;
		}

		// Menus are a flat panel and never reprojected, so the marker is only
		// meaningful for the world.
		const bool bInMenu = (g_pShared && g_pShared->nInMenu != 0);
		xr.SetSubmitPoseByte(bInMenu ? -1 : nHeldMarker);


		// THE PAUSE MENU'S OWN PICTURE, as a quad anchored where the head was
		// when the pause opened. The renderer publishes it beside the eyes;
		// if it is not there (an older renderer, +StubPauseQuad 0) the menu is
		// in the eyes as before and nothing here runs.
		bool bPauseQuad = false;
		if (!bInMenu && g_pShared && g_pShared->nPauseQuad != 0 && sharedFrame.IsOpen())
		{
			if (ID3D11Texture2D* pO = sharedFrame.TryGetOverlay())
			{
				if (!bWasPauseQuad) xr.AnchorPausePanel();
				bPauseQuad = xr.SubmitEye(context.get(), pO, 2, 0, 0,
										  sharedFrame.OverlayW(), sharedFrame.OverlayH(), false);
				// And on the desktop: the preview copies an eye, which never
				// contains the pause menu, so it blends this over its picture.
				// The next preview present picks it up (it runs at ~30 Hz).
				if (bMirror) mirror.SetPauseOverlay(bPauseQuad ? pO : nullptr);
				{
					static bool s_bSaid = false;
					if (bMirror && bPauseQuad && !s_bSaid) { s_bSaid = true; Msg("mirror: pause overlay handed to the preview"); }
				}
			}
		}
		if (bMirror && !bPauseQuad) mirror.SetPauseOverlay(nullptr);
		if (bPauseQuad != bWasPauseQuad)
		{
			bWasPauseQuad = bPauseQuad;
			Msg(bPauseQuad ? "pause menu: up, as a world-anchored quad over the stereo world"
						   : "pause menu: down");
		}
		xr.EndFrame(bHaveImage, bInMenu, bPauseQuad);
		tStepEnd = HostLog::NowMs();
		if (tStepCopy < tPrevLoop) tStepCopy = tPrevLoop;
		if (tStepMarker < tStepCopy) tStepMarker = tStepCopy;
		if (tStepSubmit < tStepMarker) tStepSubmit = tStepMarker;

		if (HostLog::NowMs() > tNextStat && !submitMs.empty())
		{
			tNextStat = HostLog::NowMs() + 10000.0;
			double sum = 0.0;
			for (double v : submitMs) sum += v;
			const double fLoopAvg = (nLoops > 1) ? fLoopMs / (nLoops - 1) : 0.0;
			const double fWaitAvg = (nLoops > 0) ? fWaitMs / nLoops : 0.0;
			Msg("frames %u, presented %u, no-image %u, eye copy avg %.3f ms",
				nFrame, nPresented, nNoImage, sum / submitMs.size());
			Msg("  loop %.2f ms (%.1f fps) = runtime wait %.2f + our work %.2f  -> %s",
				fLoopAvg, (fLoopAvg > 0.0) ? 1000.0 / fLoopAvg : 0.0,
				fWaitAvg, fLoopAvg - fWaitAvg,
				(fWaitAvg > (fLoopAvg - fWaitAvg)) ? "RUNTIME is pacing us (headset refresh rate)"
												   : "OUR WORK is the bottleneck");
			Msg("  long loop periods: %u over 16 ms, %u over 22 ms, worst %.1f ms (of %u)",
				nLong16, nLong22, fLoopWorst, nLoops);
			Msg("  long periods by side: %u long WAITS (worst %.1f ms) | %u long WORK loops (worst %.1f ms)",
				nLongWait, fWaitWorst, nLongWork, fWorkWorst);
			nLongWait = nLongWork = 0; fWaitWorst = fWorkWorst = 0.0;
			nLong16 = nLong22 = 0; fLoopWorst = 0.0;
			const uint32_t nTot = nFreshFrames + nReusedFrames;
			if (nTot)
			{
				const double fPerSec = nFreshFrames / 10.0;	// stats window is 10 s
				Msg("  capture: %u fresh, %u repeated (%.1f fresh/sec, %.0f%% repeated)%s",
					nFreshFrames, nReusedFrames, fPerSec,
					100.0 * nReusedFrames / nTot,
					(fPerSec < 80.0) ? "  <- REPEATS ARE THE STUTTER" : "");
			}

			if (nAgeSamples)
			{
				// NOT the age of the content. Measured at about -10 ms: the
				// timestamp sits in the FUTURE relative to when we receive the
				// frame, so SystemRelativeTime is the compositor's scheduled
				// PRESENT time, not the moment the content was captured. It
				// cannot answer what the pose lag should be.
				//
				// Kept because it is a real and stable quantity - it says we
				// receive each frame roughly 10 ms before its nominal present -
				// and because deleting it invites the same wrong assumption
				// being made again.
				Msg("  frame stamp vs receipt: avg %+.1f ms, worst %+.1f ms over %u frames"
					" (negative = stamp is ahead; this is a present time, not a capture time)",
					fAgeSum / nAgeSamples, fAgeMax, nAgeSamples);
			}

			if (nTrueSamples)
			{
				Msg("  TRUE image staleness: avg %.1f ms, worst %.1f ms over %u frames"
					"  (marker unmatched %u, unreadable %u, rejected as too old %u)%s",
					fTrueSum / nTrueSamples, fTrueMax, nTrueSamples, nMarkMiss, nMarkFail,
					xr.PoseTooOldCount(),
					(xr.PoseTooOldCount() > 0)
						? "   <- the marker is still decoding wrong; those frames fell back"
						: "");
			}
			else if (nMarkFail || nMarkMiss)
			{
				// Expected on a menu, where the client deliberately does not draw
				// one. Not expected in the world, and the host says which it was
				// so the two cannot be confused.
				Msg("  frame marker not usable: %u unreadable, %u unmatched%s",
					nMarkFail, nMarkMiss,
					(g_pShared && g_pShared->nInMenu)
						? "  (in a menu - the client draws no marker there)"
						: "  IN THE WORLD - the marker should be readable");
			}

			// What the client rendered against what we declare. The player sees
			// the distortion in the headset but NOT on the desktop, which puts
			// the fault after the render - so these two numbers disagreeing is
			// the most likely cause, and it has never been printed together.
			if (g_pShared)
			{
				const float R = 57.2957795f;
				Msg("  DECLARED vs RENDERED: client rendered %.2f x %.2f deg,"
					" asym %u, applied centres (%.2f,%.2f) (%.2f,%.2f) deg",
					g_pShared->fGameFovXRad * R, g_pShared->fGameFovYRad * R,
					g_pShared->nAsymActive,
					g_pShared->fAppliedYawRad[0] * R, g_pShared->fAppliedPitchRad[0] * R,
					g_pShared->fAppliedYawRad[1] * R, g_pShared->fAppliedPitchRad[1] * R);

				// Which side of the pipeline is currently responsible for head
				// rotation. A run judged in the headset is worthless if the log
				// cannot say which arm of the experiment produced it.
				Msg("  HEAD ROTATION: %s",
					g_pShared->nHeadLocked
						? (xr.HeadLocked()
							? "client camera (mouse path), layer head-locked, no reprojection"
							: "client camera (mouse path), but the layer is NOT head-locked - see the error above")
						: "composed at render time, layer world-locked, runtime reprojects");

				// Per-frame calls that used to fail in silence. Zero here is the
				// only reading that means "submission is working" - the absence of
				// an error message never did.
				uint32_t nFB = 0, nFR = 0, nFE = 0, nFM = 0;
				xr.SubmitFailures(nFB, nFR, nFE, nFM);
				if (nFB | nFR | nFE | nFM)
					Msg("  SUBMISSION FAILURES: beginFrame %u, releaseImage %u,"
						" endFrame %u, endFrame(menu quad) %u  <- THE HEADSET IS NOT GETTING THESE FRAMES",
						nFB, nFR, nFE, nFM);
			}

			fWaitMs = fLoopMs = 0.0;
			nLoops  = 0;
			fAgeSum = fAgeMax = 0.0;
			nAgeSamples = 0;
			fTrueSum = fTrueMax = 0.0;
			nTrueSamples = nMarkMiss = nMarkFail = 0;
			nFreshFrames = nReusedFrames = 0;
			tPrevLoop = -1.0;
		}
	}

	Msg("");
	Msg("=== results ===");
	Msg("frames        : %u", nFrame);
	Msg("presented     : %u", nPresented);
	Msg("no image      : %u", nNoImage);
	if (!submitMs.empty())
	{
		std::sort(submitMs.begin(), submitMs.end());
		double sum = 0.0;
		for (double v : submitMs) sum += v;
		Msg("eye copy      : avg %.3f ms  median %.3f  p99 %.3f  max %.3f",
			sum / submitMs.size(), submitMs[submitMs.size() / 2],
			submitMs[(size_t)(submitMs.size() * 0.99)], submitMs.back());
	}
	Msg("=== end ===");

	mirror.Destroy();
	capture.Stop();
	xr.Shutdown();
	ReleaseSharedBlock();
	HostLog::Close();
	return 0;
}
