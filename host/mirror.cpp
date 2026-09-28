// ----------------------------------------------------------------------- //
//
// MODULE  : mirror.cpp   -   see mirror.h
//
// ----------------------------------------------------------------------- //

#include "mirror.h"
#include "hostlog.h"

#include <dxgi1_5.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")

using HostLog::Msg;
using winrt::com_ptr;

namespace
{
	const wchar_t* kClass = L"NOLFVRMirror";

	// The pause overlay over the preview: a full-viewport triangle sampling a
	// rectangle of the overlay. The overlay's colour is premultiplied (the
	// headset composites it with SOURCE_ALPHA and no UNPREMULTIPLIED bit), so
	// the blend is ONE / INV_SRC_ALPHA.
	const char* kOvlHLSL = R"(
cbuffer Src : register(b0) { float4 gRect; };   // x, y, w, h in overlay texels
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
    return gTex.SampleLevel(gSmp, (gRect.xy + i.uv * gRect.zw) / float2(dim), 0);
}
)";

	// The spectator view: the output rectangle maps onto a ROTATED rectangle of
	// the eye - centre gC, full right edge gR, full down edge gD, all in eye
	// texels - so one draw both follows the smoothed head and levels the
	// horizon. The vertex shader is the overlay's.
	const char* kSpecHLSL = R"(
cbuffer Spec : register(b0) { float4 gC; float4 gR; float4 gD; };
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(VSOut i) : SV_TARGET
{
    uint2 dim;
    gTex.GetDimensions(dim.x, dim.y);
    float2 p = gC.xy + (i.uv.x - 0.5) * gR.xy + (i.uv.y - 0.5) * gD.xy;
    return float4(gTex.SampleLevel(gSmp, p / float2(dim), 0).rgb, 1.0);
}
)";

	// Quaternion (x,y,z,w) applied to a vector, and its conjugate.
	void QRot(const float q[4], const float v[3], float o[3])
	{
		const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
		const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
		const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
		o[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
		o[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
		o[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
	}
	void QRotInv(const float q[4], const float v[3], float o[3])
	{
		const float c[4] = { -q[0], -q[1], -q[2], q[3] };
		QRot(c, v, o);
	}

	// The game window the preview covers, so keys that land here go there.
	// A preview with a do-nothing handler swallows every keystroke the
	// moment it has focus - the console, chat, every bind (see the
	// vr-mirror-window-eats-the-keyboard note; it cost PreyVR weeks).
	HWND g_hMirrorGame = nullptr;

	// WHO ACTUALLY OWNS THE KEYBOARD, because nobody has ever measured it.
	//
	// T now opens "Say:" but nothing can be
	// typed into it. Those are two different paths and only one was fixed. The
	// bind arrives through DirectInput, which the proxy now acquires in
	// BACKGROUND so it no longer needs focus. The TEXT arrives as WM_CHAR
	// (GameClientShell.cpp: HANDLE_MSG(hWnd, WM_CHAR, OnChar)), and a window
	// message only reaches a window that has keyboard FOCUS.
	//
	// SetFocus below is a prime suspect: it silently does nothing when the
	// target belongs to another thread's input queue, and the host and
	// lithtech.exe are different PROCESSES. If it has never worked, focus is
	// sitting on some third window and neither the game nor this preview sees a
	// character at all - which would also explain why forwarding never helped.
	//
	// So: name the window that holds focus, and say whether anything is even
	// arriving here. Capped, because this is a diagnostic and not a feature.
	static void SayWho(const char* when)
	{
		HWND fg = GetForegroundWindow();
		HWND fo = GetFocus();                      // this thread's focus only
		wchar_t cls[64] = {0}, ttl[96] = {0};
		DWORD pid = 0;
		if (fg) { GetClassNameW(fg, cls, 63); GetWindowTextW(fg, ttl, 95);
		          GetWindowThreadProcessId(fg, &pid); }
		Msg("MIRROR KEYS [%s]: foreground=%p class='%ls' title='%ls' pid=%lu"
		    " | this thread's focus=%p | game=%p",
		    when, fg, cls, ttl, pid, fo, g_hMirrorGame);
	}

	LRESULT CALLBACK MirrorProc(HWND h, UINT msg, WPARAM w, LPARAM l)
	{
		switch (msg)
		{
		case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
		case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
		{
			static int s_nSaid = 0;
			const bool bFwd = (g_hMirrorGame && IsWindow(g_hMirrorGame));
			if (s_nSaid < 16)
			{
				++s_nSaid;
				Msg("MIRROR KEY: msg=0x%04X wParam=0x%02X ('%c') -> %s",
				    msg, (unsigned)w,
				    (w >= 32 && w < 127) ? (char)w : '.',
				    bFwd ? "POSTED to the game window" : "DROPPED, no game window");
				if (s_nSaid == 1) SayWho("first key seen here");
			}
			if (bFwd) { PostMessageW(g_hMirrorGame, msg, w, l); return 0; }
			break;
		}
		case WM_SETFOCUS:
			// Never keep focus: hand it straight to the game.
			//
			// AND SAY WHETHER IT WORKED. SetFocus returns the window that
			// previously had focus, or NULL on failure - and it fails silently
			// across processes unless the input queues are attached.
			if (g_hMirrorGame && IsWindow(g_hMirrorGame))
			{
				SetLastError(0);
				HWND prev = SetFocus(g_hMirrorGame);
				Msg("MIRROR: SetFocus(game %p) returned %p, err %lu%s",
				    g_hMirrorGame, prev, GetLastError(),
				    prev ? "" : "  <- FAILED: cross-process SetFocus does"
				                " nothing without AttachThreadInput, so the"
				                " game never gets WM_CHAR and typing is dead");
				SayWho("after handing focus to the game");
			}
			return 0;
		}
		// NO CURSOR. The class cursor was the arrow, so a mouse left resting
		// on the preview drew an arrow stuck on the desktop picture (headset
		// tester, 24 September, RC5). The game hides its own.
		if (msg == WM_SETCURSOR) { SetCursor(nullptr); return TRUE; }
		if (msg == WM_CLOSE) { ShowWindow(h, SW_HIDE); return 0; }
		if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
		return DefWindowProcW(h, msg, w, l);
	}
}

bool MirrorWindow::Create(ID3D11Device* pDevice, int nEyeW, int nEyeH, bool bRightEye)
{
	// The swap chain buffer is exactly one eye, so the copy out of the capture
	// is 1:1 and never crops. DXGI then stretches that buffer to the window,
	// which costs nothing and keeps the preview at whatever size we like.
	m_nWidth    = nEyeW;
	m_nHeight   = nEyeH;
	m_bRightEye = bRightEye;
	m_pDevice   = pDevice;
	m_nEyeW     = nEyeW;		// kept: cover mode overwrites m_nWidth/m_nHeight
	m_nEyeH     = nEyeH;

	// 1080 tall, width following the eye's own aspect so the preview is not
	// squashed. An eye is roughly square in VR, so this is not 16:9.
	const int nWinH = 1080;
	const int nWinW = (nEyeH > 0) ? (int)((float)nEyeW * ((float)nWinH / (float)nEyeH) + 0.5f)
								  : 1080;

	WNDCLASSEXW wc{};
	wc.cbSize        = sizeof(wc);
	wc.lpfnWndProc   = MirrorProc;
	wc.hInstance     = GetModuleHandleW(nullptr);
	wc.lpszClassName = kClass;
	wc.hCursor       = nullptr;			// see WM_SETCURSOR: no cursor over the preview
	RegisterClassExW(&wc);

	RECT rc{ 0, 0, nWinW, nWinH };

	// Topmost so it stays visible over the borderless game window, but
	// NOACTIVATE so it never takes keyboard focus. Without that the preview
	// steals focus from the game and every click has to go back to the game
	// window first.
	// TRANSPARENT as well as NOACTIVATE: without it the preview sits over the
	// borderless game window and eats mouse clicks meant for the game's menus.
	m_hWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
		kClass, L"No One Lives Forever VR - right eye",
		WS_POPUP, 40, 40,
		rc.right - rc.left, rc.bottom - rc.top,
		nullptr, nullptr, wc.hInstance, nullptr);
	if (!m_hWnd) { Msg("mirror: CreateWindow failed (%lu)", GetLastError()); return false; }

	com_ptr<IDXGIDevice> dxgi;
	if (FAILED(pDevice->QueryInterface(__uuidof(IDXGIDevice), dxgi.put_void()))) return false;
	com_ptr<IDXGIAdapter> adapter;
	dxgi->GetAdapter(adapter.put());
	com_ptr<IDXGIFactory2> factory;
	adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void());

	DXGI_SWAP_CHAIN_DESC1 scd{};
	scd.Width       = nEyeW;
	scd.Height      = nEyeH;
	scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	// 3 buffers, not 2. A flip-model Present blocks once every buffer is queued
	// for display, even with vsync off - so a 2-buffer chain on a 60 Hz monitor
	// injects 60 Hz pacing into whatever thread presents it. Here that thread
	// is the one feeding the headset.
	scd.BufferCount = 3;
	scd.SampleDesc.Count = 1;
	scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scd.Scaling     = DXGI_SCALING_STRETCH;		// buffer is one eye, window is not

	// Tearing support has to be declared at creation to be usable at present.
	{
		com_ptr<IDXGIFactory5> f5;
		BOOL bTear = FALSE;
		if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), f5.put_void())) &&
			SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &bTear, sizeof(bTear))) &&
			bTear)
		{
			m_bAllowTearing = true;
			scd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		}
	}

	if (FAILED(factory->CreateSwapChainForHwnd(pDevice, m_hWnd, &scd, nullptr, nullptr,
		m_SwapChain.put())))
	{
		Msg("mirror: swap chain creation failed");
		return false;
	}

	// No alt-enter fullscreen - this is a preview, not the game.
	factory->MakeWindowAssociation(m_hWnd, DXGI_MWA_NO_ALT_ENTER);

	// SW_SHOWNOACTIVATE, so showing it does not pull focus off the game.
	ShowWindow(m_hWnd, SW_SHOWNOACTIVATE);
	SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	m_bOpen = true;
	Msg("mirror window %dx%d showing a %dx%d %s eye", nWinW, nWinH, nEyeW, nEyeH,
		bRightEye ? "right" : "left");
	return true;
}

void MirrorWindow::Present(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pSrc,
						   int nSrcX, int nSrcY, int nSrcW, int nSrcH)
{
	if (!m_bOpen || !m_SwapChain || !pSrc) return;

	// A desktop preview does not need headset cadence. Presenting it every
	// frame puts a 60 Hz monitor in the critical path of a 90 Hz headset, so
	// cap it at ~30/s and let the eyes have the rest of the budget.
	const double tNow = HostLog::NowMs();
	if (tNow - m_fLastPresentMs < 33.0) return;
	m_fLastPresentMs = tNow;

	// The eye as handed in, before the crop below: the pause overlay is one
	// eye in size, so the crop's place inside the eye is its place in the overlay.
	const int nEyeX0 = nSrcX, nEyeY0 = nSrcY, nEyeW0 = nSrcW, nEyeH0 = nSrcH;

	// When covering the game window the buffer must have that window's aspect,
	// or DXGI stretches one eye across the full width and the picture is twice
	// as wide as it should be. It used to be the window's SIZE with the eye
	// letterboxed inside it; see below. Otherwise the buffer is just the eye.
	// ...NO: CROP THE EYE TO THE SCREEN'S SHAPE AND FILL IT. Letterboxed 1:1,
	// the eye (taller than wide, ~120 degrees high) read on a 16:9 monitor as
	// a picture stretched vertically between two black bars (headset tester,
	// 24 September, RC4). The buffer is now the middle band of the eye at the
	// covered window's aspect - full width, cropped top and bottom - and DXGI
	// scales it to the window (DXGI_SCALING_STRETCH), so it fills the screen
	// with square pixels, like a flat game.
	if (m_bCover && m_nCoverW > 0 && m_nCoverH > 0 && nSrcW > 0 && nSrcH > 0)
	{
		int nCropW = nSrcW;
		int nCropH = (int)((double)nSrcW * (double)m_nCoverH / (double)m_nCoverW + 0.5);
		if (nCropH > nSrcH)
		{
			nCropH = nSrcH;
			nCropW = (int)((double)nSrcH * (double)m_nCoverW / (double)m_nCoverH + 0.5);
			if (nCropW > nSrcW) nCropW = nSrcW;
		}
		nSrcX += (nSrcW - nCropW) / 2;
		nSrcY += (nSrcH - nCropH) / 2;
		nSrcW = nCropW;
		nSrcH = nCropH;
	}
	const int nWantW = nSrcW;
	const int nWantH = nSrcH;

	// A resolution change resizes the eye under us. Match the buffer to it, or
	// the copy silently crops and the preview shows the middle of the eye.
	if (nWantW != m_nWidth || nWantH != m_nHeight)
	{
		// Keep the tearing flag: ResizeBuffers replaces the flags wholesale, and
		// dropping it here would silently restore the blocking present.
		const UINT nFlags = m_bAllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
		if (FAILED(m_SwapChain->ResizeBuffers(0, nWantW, nWantH, DXGI_FORMAT_UNKNOWN, nFlags)))
			return;
		m_nWidth  = nWantW;
		m_nHeight = nWantH;
		Msg("mirror: buffer resized to %dx%d", nWantW, nWantH);
	}

	com_ptr<ID3D11Texture2D> back;
	if (FAILED(m_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void()))) return;

	// The eye is narrower than the window it is covering, so the rest has to be
	// painted out every frame - FLIP_DISCARD leaves buffer contents undefined
	// after a present, and without this the uncovered strip shows stale pixels.
	const int nDstX = (m_nWidth  - nSrcW) / 2;
	const int nDstY = (m_nHeight - nSrcH) / 2;
	if (m_bCover && (nDstX > 0 || nDstY > 0))
	{
		com_ptr<ID3D11RenderTargetView> rtv;
		if (m_pDevice && SUCCEEDED(m_pDevice->CreateRenderTargetView(back.get(), nullptr, rtv.put())))
		{
			const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			pCtx->ClearRenderTargetView(rtv.get(), black);
		}
	}

	// Straight 1:1 copy of the whole eye - no scaling, so nothing is softened.
	D3D11_BOX box{};
	box.left   = nSrcX;
	box.top    = nSrcY;
	box.right  = nSrcX + nSrcW;
	box.bottom = nSrcY + nSrcH;
	box.front  = 0;
	box.back   = 1;

	// THE SPECTATOR VIEW, when it is on and this is the world; otherwise (a
	// menu, a pause, a failed draw) the plain copy, exactly as before.
	const bool bSpecDrawn = m_bSpec && m_bSpecWorld && !m_Ovl && nDstX == 0 && nDstY == 0 &&
		DrawSpectator(pCtx, back.get(), pSrc, nEyeX0, nEyeY0, nEyeW0, nEyeH0);
	if (!bSpecDrawn)
	{
		if (m_bSpec) m_bSpecFwdValid = false;	// start from the head when it comes back
		pCtx->CopySubresourceRegion(back.get(), 0,
			(nDstX > 0) ? nDstX : 0, (nDstY > 0) ? nDstY : 0, 0, pSrc, 0, &box);
	}

	// THE PAUSE MENU, over the same crop of the eye (see SetPauseOverlay).
	if (m_Ovl && nEyeW0 > 0 && nEyeH0 > 0)
	{
		D3D11_TEXTURE2D_DESC od{};
		m_Ovl->GetDesc(&od);
		const float sx = (float)od.Width  / (float)nEyeW0;
		const float sy = (float)od.Height / (float)nEyeH0;
		DrawOverlay(pCtx, back.get(),
					(float)(nSrcX - nEyeX0) * sx, (float)(nSrcY - nEyeY0) * sy,
					(float)nSrcW * sx, (float)nSrcH * sy,
					(nDstX > 0) ? nDstX : 0, (nDstY > 0) ? nDstY : 0, nSrcW, nSrcH);
	}

	// No vsync, and allow tearing so DXGI never blocks us waiting on the
	// monitor. The preview may tear; the headset must not stutter.
	m_SwapChain->Present(0, m_bAllowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);

	const double tCost = HostLog::NowMs() - tNow;
	m_fPresentMs += tCost;
	if (++m_nPresents >= 300)
	{
		Msg("mirror: present avg %.2f ms over %u frames", m_fPresentMs / m_nPresents, m_nPresents);
		m_fPresentMs = 0.0;
		m_nPresents  = 0;
	}
}

void MirrorWindow::CoverWindow(HWND hGame)
{
	if (!m_bOpen || !m_hWnd || !hGame || !IsWindow(hGame)) return;
	g_hMirrorGame = hGame;

	RECT rc;
	if (!GetWindowRect(hGame, &rc)) return;
	const int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w < 64 || h < 64) return;

	// Only cover the game window if it is on the display the player is actually
	// looking at. Once the game moves to a high-refresh virtual display it is
	// off-screen, and following it there would put the preview somewhere it can
	// never be seen - covering a window nobody can look at, while the real
	// monitor shows nothing at all.
	const POINT ptOrigin{ 0, 0 };
	HMONITOR hPrimary = MonitorFromPoint(ptOrigin, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFO miPrim{};
	miPrim.cbSize = sizeof(miPrim);
	GetMonitorInfoW(hPrimary, &miPrim);

	RECT rcHit;
	const bool bOnPrimary = IntersectRect(&rcHit, &rc, &miPrim.rcMonitor) != 0;

	if (bOnPrimary)
	{
		// THE WHOLE MONITOR, NOT JUST THE GAME'S WINDOW. The game's window
		// stops short of the taskbar (3840x2076 on a 3840x2160 screen), so
		// the preview did too, with the taskbar showing under it. Borderless
		// fullscreen was asked for (24 September, RC5); the crop in Present
		// follows whatever shape this is.
		RECT rcMon = rc;
		{
			MONITORINFO mi{}; mi.cbSize = sizeof(mi);
			if (GetMonitorInfoW(MonitorFromWindow(hGame, MONITOR_DEFAULTTOPRIMARY), &mi))
				rcMon = mi.rcMonitor;
		}
		const int mw = rcMon.right - rcMon.left, mh = rcMon.bottom - rcMon.top;
		m_bCover  = true;
		m_nCoverW = mw;
		m_nCoverH = mh;
		SetWindowPos(m_hWnd, HWND_TOP, rcMon.left, rcMon.top, mw, mh,
			SWP_NOACTIVATE | SWP_SHOWWINDOW);
		// Just above the GAME, not above everything: put the game behind us.
		// ASYNC: see FollowForeground - a synchronous move of the game's
		// window waits on the game's thread, which may be waiting on us.
		SetWindowPos(hGame, m_hWnd, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
		m_bVisible = true;
		Msg("mirror: covering the monitor at (%d,%d) %dx%d (the game window is %dx%d)",
			rcMon.left, rcMon.top, mw, mh, w, h);
		return;
	}

	// Game is on another display - show a normal preview on the primary.
	m_bCover = false;
	const int nWinH = 1080;
	const int nWinW = (m_nEyeH > 0) ? (int)((float)m_nEyeW * ((float)nWinH / (float)m_nEyeH) + 0.5f)
									: 1080;
	SetWindowPos(m_hWnd, HWND_TOPMOST,
		miPrim.rcMonitor.left + 40, miPrim.rcMonitor.top + 40, nWinW, nWinH,
		SWP_NOACTIVATE | SWP_SHOWWINDOW);
	Msg("mirror: game is off the primary display - preview %dx%d on the primary instead",
		nWinW, nWinH);
}

void MirrorWindow::FollowForeground(HWND hGame)
{
	if (!m_bOpen || !m_hWnd) return;

	// The preview is topmost so it covers the game's side-by-side window. That
	// also means it sits over everything else on the desktop, including
	// whatever the player alt-tabs to. Hide it whenever the game is not in the
	// foreground, so alt-tab behaves normally.
	// ...AND ACTUALLY DO IT. This only ever tested that the game was not
	// minimized, so an alt-tab left the preview over whatever was switched
	// to. Now that it covers the whole monitor, taskbar included, that would
	// hide the desktop entirely. Shown while the game (or the preview itself)
	// is in front, hidden for anything else - after half a second, so a
	// passing flicker of focus does not blink it.
	const HWND hFg = GetForegroundWindow();
	const bool bGameUp = hGame && IsWindow(hGame) && !IsIconic(hGame);
	static DWORD s_dwOtherSince = 0;
	bool bOther = bGameUp && hFg && hFg != hGame && hFg != m_hWnd;
	if (bOther) { if (!s_dwOtherSince) s_dwOtherSince = GetTickCount(); }
	else s_dwOtherSince = 0;
	const bool bWant = bGameUp && !(bOther && GetTickCount() - s_dwOtherSince >= 500);
	if (bWant != m_bVisible)
	{
		m_bVisible = bWant;
		ShowWindow(m_hWnd, bWant ? SW_SHOWNOACTIVATE : SW_HIDE);
		static int s_nSaid = 0;
		if (++s_nSaid <= 20)
			Msg("mirror: %s", bWant ? "shown - the game is in front again"
									: "hidden - another program is in front");
	}
	// Re-assert the order ONLY when it has slipped. SetWindowPos every frame
	// is a DWM round trip each time: 25 fps in the menu, 25-45 in game
	// (19 Sep) against the usual 90.
	// ...and ONLY when the foreground window changes. SetWindowPos is a DWM
	// round trip of ~20 ms; even once a second it silenced the host past
	// the renderer's 16.7 ms sync timeout - 21 timed-out frames in one
	// session, a visible drop every second (19 Sep). Foreground changes
	// are the only time the order can have slipped, and they are rare.
	static HWND s_hLastFore = nullptr;
	const HWND hFore = GetForegroundWindow();
	if (bWant && m_bCover && hFore != s_hLastFore)
	{
		s_hLastFore = hFore;
		if (GetWindow(m_hWnd, GW_HWNDNEXT) != hGame)
			SetWindowPos(hGame, m_hWnd, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
	}

	// ...AND THE GAME CAN CLIMB BACK OVER US WITHOUT THE FOREGROUND CHANGING.
	// The game raises its own window as it activates, which can land just
	// AFTER the check above ran - and from then on the foreground never
	// changes again, so the desktop showed the game window's last present
	// (the splash, side by side) for the whole session while the mirror drew
	// the right eye underneath it.
	//
	// Twice a second, walk UP from the game (GetWindow, no DWM round trip)
	// and only if the mirror is not above it, raise the mirror - one
	// SetWindowPos, on a slip, not on a timer.
	static int s_nTick = 0;
	static long s_nFixes = 0;
	if (bWant && m_bCover && hGame && hFore == hGame && ++s_nTick >= 45)
	{
		s_nTick = 0;
		bool bAbove = false;
		int nSteps = 0;
		for (HWND h = GetWindow(hGame, GW_HWNDPREV); h && nSteps < 256; h = GetWindow(h, GW_HWNDPREV), ++nSteps)
			if (h == m_hWnd) { bAbove = true; break; }
		if (!bAbove)
		{
			// NEVER A SYNCHRONOUS CALL ON THE GAME'S WINDOW. Moving another
			// process's window waits for that process's thread to handle the
			// move - and the game's thread spends its frames waiting for OUR
			// tick, so each correction stalled this loop 40-70 ms. RC3 in the
			// headset: 55 long frames against RC2's 13, and the frames felt
			// late while the runtime still reported 90 (24 September). Our
			// own window is ours; the game's move is posted, not waited on.
			LARGE_INTEGER q0, q1, qf; QueryPerformanceCounter(&q0);
			SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
			SetWindowPos(hGame, m_hWnd, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
			QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
			const double fMs = qf.QuadPart ? 1000.0 * (double)(q1.QuadPart - q0.QuadPart) / (double)qf.QuadPart : 0.0;
			++s_nFixes;
			if (s_nFixes <= 10 || (s_nFixes % 50) == 0)
				Msg("mirror: the game window was above the mirror - raised it again (%ld times, this one %.1f ms)",
					s_nFixes, fMs);
		}
	}
}

bool MirrorWindow::Pump()
{
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		if (msg.message == WM_QUIT) return false;
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return true;
}

void MirrorWindow::Destroy()
{
	m_SwapChain = nullptr;
	if (m_hWnd) { DestroyWindow(m_hWnd); m_hWnd = nullptr; }
	m_bOpen = false;
}

bool MirrorWindow::DrawOverlay(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pBack,
							   float fU, float fV, float fW, float fH,
							   int nDstX, int nDstY, int nDstW, int nDstH)
{
	if (!m_pDevice || !pCtx || !pBack || !m_Ovl) return false;
	EnsureShaders();
	if (!m_pOvlVS || !m_pOvlPS || !m_pOvlSmp || !m_pOvlBlend || !m_pOvlCB)
	{
		if (!m_bOvlSaid)
		{
			m_bOvlSaid = true;
			Msg("mirror: pause-menu blend objects missing (vs %d ps %d smp %d blend %d cb %d)",
				m_pOvlVS ? 1 : 0, m_pOvlPS ? 1 : 0, m_pOvlSmp ? 1 : 0, m_pOvlBlend ? 1 : 0, m_pOvlCB ? 1 : 0);
		}
		return false;
	}

	com_ptr<ID3D11ShaderResourceView> srv;
	com_ptr<ID3D11RenderTargetView> rtv;
	const HRESULT hrS = m_pDevice->CreateShaderResourceView(m_Ovl.get(), nullptr, srv.put());
	const HRESULT hrR = FAILED(hrS) ? E_FAIL : m_pDevice->CreateRenderTargetView(pBack, nullptr, rtv.put());
	if (FAILED(hrS) || FAILED(hrR))
	{
		if (!m_bOvlSaid) { m_bOvlSaid = true; Msg("mirror: pause-menu blend views failed (srv 0x%08X rtv 0x%08X)", hrS, hrR); }
		return false;
	}

	D3D11_MAPPED_SUBRESOURCE ms{};
	if (FAILED(pCtx->Map(m_pOvlCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
	float* p = (float*)ms.pData;
	p[0] = fU; p[1] = fV; p[2] = fW; p[3] = fH;
	pCtx->Unmap(m_pOvlCB.get(), 0);

	const D3D11_VIEWPORT vp = { (float)nDstX, (float)nDstY, (float)nDstW, (float)nDstH, 0.0f, 1.0f };
	ID3D11RenderTargetView* pRTV = rtv.get();
	ID3D11ShaderResourceView* pSRV = srv.get();
	ID3D11SamplerState* pSmp = m_pOvlSmp.get();
	ID3D11Buffer* pCB = m_pOvlCB.get();
	const float kFactor[4] = { 0, 0, 0, 0 };
	pCtx->OMSetRenderTargets(1, &pRTV, nullptr);
	pCtx->OMSetBlendState(m_pOvlBlend.get(), kFactor, 0xFFFFFFFF);
	pCtx->RSSetViewports(1, &vp);
	pCtx->IASetInputLayout(nullptr);
	pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pCtx->VSSetShader(m_pOvlVS.get(), nullptr, 0);
	pCtx->PSSetShader(m_pOvlPS.get(), nullptr, 0);
	pCtx->PSSetShaderResources(0, 1, &pSRV);
	pCtx->PSSetSamplers(0, 1, &pSmp);
	pCtx->PSSetConstantBuffers(0, 1, &pCB);
	pCtx->Draw(3, 0);

	// Leave the context as the headset path expects it: its blitter sets its
	// own shaders and targets but not a blend state.
	ID3D11ShaderResourceView* pNullSRV = nullptr;
	ID3D11RenderTargetView* pNullRTV = nullptr;
	pCtx->PSSetShaderResources(0, 1, &pNullSRV);
	pCtx->OMSetRenderTargets(1, &pNullRTV, nullptr);
	pCtx->OMSetBlendState(nullptr, kFactor, 0xFFFFFFFF);

	if (!m_bOvlSaid)
	{
		m_bOvlSaid = true;
		Msg("mirror: the pause menu is drawn over the preview (overlay rect %.0f,%.0f %.0fx%.0f)", fU, fV, fW, fH);
	}
	return true;
}

// Everything both preview draws need, made on first use: the full-viewport
// triangle, the pause overlay's blend and the spectator view's crop. A part
// that fails is said once and its draw falls back to what it did before.
bool MirrorWindow::EnsureShaders()
{
	if (m_bOvlTried) return m_pOvlVS != nullptr;
	m_bOvlTried = true;
	if (!m_pDevice) return false;

	com_ptr<ID3DBlob> vs, ps, sps, err;
	const size_t n = strlen(kOvlHLSL);
	if (FAILED(D3DCompile(kOvlHLSL, n, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, vs.put(), err.put()))
		|| FAILED(m_pDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, m_pOvlVS.put())))
	{
		Msg("mirror: the preview's shaders could not be built - plain preview, no pause menu on it");
		m_pOvlVS = nullptr;
		return false;
	}
	if (FAILED(D3DCompile(kOvlHLSL, n, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, ps.put(), err.put()))
		|| FAILED(m_pDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, m_pOvlPS.put())))
	{
		Msg("mirror: the pause-menu blend could not be built - the preview goes on without the menu");
		m_pOvlPS = nullptr;
	}
	err = nullptr;
	if (FAILED(D3DCompile(kSpecHLSL, strlen(kSpecHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, sps.put(), err.put()))
		|| FAILED(m_pDevice->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, m_pSpecPS.put())))
	{
		Msg("mirror: the spectator view could not be built (%s) - plain preview",
			err ? (const char*)err->GetBufferPointer() : "no compiler message");
		m_pSpecPS = nullptr;
	}

	D3D11_SAMPLER_DESC sd{};
	sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD   = D3D11_FLOAT32_MAX;
	m_pDevice->CreateSamplerState(&sd, m_pOvlSmp.put());

	D3D11_BLEND_DESC bd{};
	bd.RenderTarget[0].BlendEnable    = TRUE;
	bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	m_pDevice->CreateBlendState(&bd, m_pOvlBlend.put());

	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = 16;
	cb.Usage     = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	m_pDevice->CreateBuffer(&cb, nullptr, m_pOvlCB.put());
	cb.ByteWidth = 48;
	m_pDevice->CreateBuffer(&cb, nullptr, m_pSpecCB.put());
	return true;
}

void MirrorWindow::SetSpectatorView(const float q[4], float fTanL, float fTanR, float fTanU, float fTanD,
									float fRx, float fRy, float fRw, float fRh, bool bWorld)
{
	for (int i = 0; i < 4; ++i) m_fSpecQ[i] = q[i];
	m_fSpecTan[0] = fTanL; m_fSpecTan[1] = fTanR; m_fSpecTan[2] = fTanU; m_fSpecTan[3] = fTanD;
	m_fSpecRect[0] = fRx; m_fSpecRect[1] = fRy; m_fSpecRect[2] = fRw; m_fSpecRect[3] = fRh;
	// A frustum with no width, or a quaternion that is not one, is not a view.
	const float qq = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
	m_bSpecWorld = bWorld && fTanR > fTanL && fTanU > fTanD && fRw > 0.0f && fRh > 0.0f &&
				   qq > 0.9f && qq < 1.1f;
	m_bSpecViewSet = true;
}

// THE SPECTATOR VIEW. Where the head points, smoothed; projected into this
// eye's picture through the frustum it was declared with; a cut-out of the
// eye around that point, turned so the world's up is the screen's up.
//
// Everything is in the eye image's own terms (the frustum's tangents and the
// fraction of the picture it covers), so this reads nothing of the game and
// is the same code for any port whose host declares a projection layer.
bool MirrorWindow::DrawSpectator(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pBack, ID3D11Texture2D* pSrc,
								 int nEyeX, int nEyeY, int nEyeW, int nEyeH)
{
	if (!m_bSpecViewSet || nEyeW < 16 || nEyeH < 16 || m_nWidth < 16 || m_nHeight < 16) return false;
	if (!EnsureShaders() || !m_pSpecPS || !m_pSpecCB || !m_pOvlSmp) return false;

	float fCx, fCy, fRx, fRy, fDx, fDy, fW, fH;
	if (!SpectatorCrop(nEyeX, nEyeY, nEyeW, nEyeH, fCx, fCy, fRx, fRy, fDx, fDy, fW, fH))
		return false;

	// The source's view, kept while the texture is the same one.
	if (pSrc != m_pSpecSrcKey || !m_pSpecSRV)
	{
		m_pSpecSRV = nullptr;
		m_pSpecSrcKey = pSrc;
		const HRESULT hr = m_pDevice->CreateShaderResourceView(pSrc, nullptr, m_pSpecSRV.put());
		if (FAILED(hr))
		{
			m_pSpecSRV = nullptr;
			if (!m_bSpecFailSaid) { m_bSpecFailSaid = true; Msg("mirror: spectator view cannot read the eye (0x%08X) - plain preview", hr); }
			return false;
		}
	}
	com_ptr<ID3D11RenderTargetView> rtv;
	if (FAILED(m_pDevice->CreateRenderTargetView(pBack, nullptr, rtv.put()))) return false;

	D3D11_MAPPED_SUBRESOURCE ms{};
	if (FAILED(pCtx->Map(m_pSpecCB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
	float* p = (float*)ms.pData;
	p[0] = fCx;       p[1] = fCy;       p[2]  = 0; p[3]  = 0;
	p[4] = fRx * fW;  p[5] = fRy * fW;  p[6]  = 0; p[7]  = 0;
	p[8] = fDx * fH;  p[9] = fDy * fH;  p[10] = 0; p[11] = 0;
	pCtx->Unmap(m_pSpecCB.get(), 0);

	const D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_nWidth, (float)m_nHeight, 0.0f, 1.0f };
	ID3D11RenderTargetView* pRTV = rtv.get();
	ID3D11ShaderResourceView* pSRV = m_pSpecSRV.get();
	ID3D11SamplerState* pSmp = m_pOvlSmp.get();
	ID3D11Buffer* pCB = m_pSpecCB.get();
	const float kFactor[4] = { 0, 0, 0, 0 };
	pCtx->OMSetRenderTargets(1, &pRTV, nullptr);
	pCtx->OMSetBlendState(nullptr, kFactor, 0xFFFFFFFF);
	pCtx->RSSetViewports(1, &vp);
	pCtx->IASetInputLayout(nullptr);
	pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	pCtx->VSSetShader(m_pOvlVS.get(), nullptr, 0);
	pCtx->PSSetShader(m_pSpecPS.get(), nullptr, 0);
	pCtx->PSSetShaderResources(0, 1, &pSRV);
	pCtx->PSSetSamplers(0, 1, &pSmp);
	pCtx->PSSetConstantBuffers(0, 1, &pCB);
	pCtx->Draw(3, 0);

	ID3D11ShaderResourceView* pNullSRV = nullptr;
	ID3D11RenderTargetView* pNullRTV = nullptr;
	pCtx->PSSetShaderResources(0, 1, &pNullSRV);
	pCtx->OMSetRenderTargets(1, &pNullRTV, nullptr);

	if (!m_bSpecSaid)
	{
		m_bSpecSaid = true;
		Msg("mirror: spectator view on - a %.0fx%.0f cut-out of the %dx%d eye, following the head over %.2f s",
			fW, fH, nEyeW, nEyeH, m_fSpecSmooth);
	}
	return true;
}

// The cut-out itself, apart from any drawing so a harness can check it.
// Out: the centre, the unit right and down directions (eye pixels, y down)
// and the width and height, all in the eye texture's pixels.
bool MirrorWindow::SpectatorCrop(int nEyeX, int nEyeY, int nEyeW, int nEyeH,
								 float& cx, float& cy, float& fRx, float& fRy,
								 float& fDx, float& fDy, float& W, float& H)
{
	const float* q = m_fSpecQ;
	const float kFwd[3] = { 0.0f, 0.0f, -1.0f };
	float fHead[3];
	QRot(q, kFwd, fHead);						// where the head points, world

	// Follow it with a filter that adapts to speed (the "one-euro" filter):
	// at rest it is heavy, so a wobble is not followed; turning it lightens,
	// so a turn is not dragged behind. A plain time constant heavy enough to
	// still a wobble lagged a 20 degree/s turn by 7 degrees, and the eye has
	// little more room than that at the screen's shape - the cut-out hit the
	// edge and the wobble came straight through. A jump of more than 45
	// degrees (a recenter, a teleport, the first frame) is taken at once.
	const double tNow = m_fSpecClockMs >= 0.0 ? m_fSpecClockMs : HostLog::NowMs();
	const float dt = m_bSpecFwdValid ? (float)((tNow - m_fSpecLastMs) / 1000.0) : 0.0f;
	m_fSpecLastMs = tNow;
	const float fDot = m_fSpecFwd[0] * fHead[0] + m_fSpecFwd[1] * fHead[1] + m_fSpecFwd[2] * fHead[2];
	if (!m_bSpecFwdValid || fDot < 0.7071f || dt > 0.5f || dt <= 0.0f)
	{
		for (int i = 0; i < 3; ++i) { m_fSpecFwd[i] = m_fSpecPrevHead[i] = fHead[i]; m_fSpecRate[i] = 0.0f; }
		m_fSpecSpeed = 0.0f;
		m_bSpecFwdValid = true;
	}
	else
	{
		// The head's turn rate as a VECTOR, smoothed (1 Hz), and only then its
		// size. A wobble's rate swings both ways and averages out; its SIZE is
		// always positive, and filtering that read a 2 degree, 6 Hz wobble as a
		// steady 48 degree/s turn and opened the filter to it.
		const float* p0 = m_fSpecPrevHead;
		const float w[3] = { p0[1] * fHead[2] - p0[2] * fHead[1],
							 p0[2] * fHead[0] - p0[0] * fHead[2],
							 p0[0] * fHead[1] - p0[1] * fHead[0] };		// axis x sin(angle)
		for (int i = 0; i < 3; ++i) m_fSpecPrevHead[i] = fHead[i];
		const float kTwoPi = 6.2831853f;
		const float aS = 1.0f / (1.0f + 1.0f / (kTwoPi * 1.0f * dt));
		for (int i = 0; i < 3; ++i) m_fSpecRate[i] += (w[i] * 57.29578f / dt - m_fSpecRate[i]) * aS;
		m_fSpecSpeed = sqrtf(m_fSpecRate[0] * m_fSpecRate[0] + m_fSpecRate[1] * m_fSpecRate[1] +
							 m_fSpecRate[2] * m_fSpecRate[2]);			// degrees per second
		// At rest: the time constant asked for. Every 10 degrees/s of turn
		// adds 0.5 Hz, so a 60 degree/s turn is followed within ~0.05 s.
		const float fMinHz = (m_fSpecSmooth > 0.001f) ? 1.0f / (kTwoPi * m_fSpecSmooth) : 1000.0f;
		const float fHz = fMinHz + 0.05f * m_fSpecSpeed;
		const float a = 1.0f / (1.0f + 1.0f / (kTwoPi * fHz * dt));
		float l = 0.0f;
		for (int i = 0; i < 3; ++i) { m_fSpecFwd[i] += (fHead[i] - m_fSpecFwd[i]) * a; l += m_fSpecFwd[i] * m_fSpecFwd[i]; }
		l = sqrtf(l);
		if (l < 1e-4f) { for (int i = 0; i < 3; ++i) m_fSpecFwd[i] = fHead[i]; }
		else for (int i = 0; i < 3; ++i) m_fSpecFwd[i] /= l;
	}

	// Into the eye's frame, then onto its picture plane (tangents, y up).
	float v[3];
	QRotInv(q, m_fSpecFwd, v);
	if (v[2] > -0.2f) { m_bSpecFwdValid = false; return false; }	// behind or at the edge
	const float tx = v[0] / -v[2], ty = v[1] / -v[2];

	// Tangents to eye pixels. The frustum covers rect fractions of the image.
	const float fTanL = m_fSpecTan[0], fTanR = m_fSpecTan[1], fTanU = m_fSpecTan[2], fTanD = m_fSpecTan[3];
	const float sx = (float)nEyeW * m_fSpecRect[2] / (fTanR - fTanL);	// px per tangent
	const float sy = (float)nEyeH * m_fSpecRect[3] / (fTanU - fTanD);
	const float ox = (float)nEyeX + (float)nEyeW * m_fSpecRect[0] - fTanL * sx;	// where tangent 0 is
	const float oy = (float)nEyeY + (float)nEyeH * m_fSpecRect[1] + fTanU * sy;
	cx = ox + tx * sx;
	cy = oy - ty * sy;

	// THE HORIZON LEVEL: the world's horizontal through that point, as the
	// picture shows it, becomes the screen's horizontal. That is how the point
	// moves on the image when nudged along the horizontal - exact anywhere in
	// the picture. (Levelling the world's UP instead is not the same thing
	// off-centre: the two image 2-3 degrees apart there.) Looking straight up
	// or down there is no horizon, and the cut-out stays upright.
	auto Level = [&](float fTx, float fTy, const float* pDir)
	{
		float h[3] = { -pDir[2], 0.0f, pDir[0] };			// forward x up
		const float hl = sqrtf(h[0] * h[0] + h[2] * h[2]);
		fRx = 1.0f; fRy = 0.0f;
		if (hl > 0.05f)
		{
			h[0] /= hl; h[2] /= hl;
			float hv[3];
			QRotInv(q, h, hv);
			const float rx = (hv[0] + fTx * hv[2]) * sx, ry = -(hv[1] + fTy * hv[2]) * sy;
			const float rl = sqrtf(rx * rx + ry * ry);
			if (rl > 1e-4f) { fRx = rx / rl; fRy = ry / rl; }
		}
		fDx = -fRy; fDy = fRx;					// down, a quarter turn from right
	};
	Level(tx, ty, m_fSpecFwd);

	// m_fSpecZoom of the eye's width at the preview's shape, shrunk if the
	// turn would take a corner outside the eye, then kept inside it.
	W = m_fSpecZoom * (float)nEyeW;
	H = W * (float)m_nHeight / (float)m_nWidth;
	if (H > m_fSpecZoom * (float)nEyeH) { H = m_fSpecZoom * (float)nEyeH; W = H * (float)m_nWidth / (float)m_nHeight; }
	float hx = 0.5f * (fabsf(fRx) * W + fabsf(fDx) * H);
	float hy = 0.5f * (fabsf(fRy) * W + fabsf(fDy) * H);
	float k = 1.0f;
	if (hx > 0.5f * (float)nEyeW) k = (0.5f * (float)nEyeW) / hx;
	if (hy * k > 0.5f * (float)nEyeH) k = (0.5f * (float)nEyeH) / hy;
	W *= k; H *= k; hx *= k; hy *= k;
	const float x0 = (float)nEyeX + hx, x1 = (float)(nEyeX + nEyeW) - hx;
	const float y0 = (float)nEyeY + hy, y1 = (float)(nEyeY + nEyeH) - hy;
	cx = (x0 < x1) ? ((cx < x0) ? x0 : (cx > x1) ? x1 : cx) : 0.5f * (x0 + x1);
	cy = (y0 < y1) ? ((cy < y0) ? y0 : (cy > y1) ? y1 : cy) : 0.5f * (y0 + y1);

	// Held at the edge, the followed direction becomes the one drawn: left
	// lagging beyond it, it would keep the view pinned to the edge after the
	// head stopped, instead of settling from there at once.
	{
		float e[3] = { (cx - ox) / sx, -(cy - oy) / sy, -1.0f }, w[3];
		const float el = sqrtf(e[0] * e[0] + e[1] * e[1] + 1.0f);
		for (int i = 0; i < 3; ++i) e[i] /= el;
		QRot(q, e, w);
		for (int i = 0; i < 3; ++i) m_fSpecFwd[i] = w[i];
		Level(e[0] / -e[2], e[1] / -e[2], w);		// and level it where it now is
	}
	return true;
}
