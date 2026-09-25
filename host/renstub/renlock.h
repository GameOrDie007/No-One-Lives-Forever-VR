#pragma once
// ---------------------------------------------------------------------------
// ONE LOCK AROUND THE RENDERER.
//
// The D3D11 immediate context is not thread-safe, and this renderer is
// called from TWO threads: the engine's main thread, and the loading screen's
// own thread (NOLF's CLoadingScreen::RunThread - Start3D, StartOptimized2D,
// End3D, FlipScreen in a loop) which runs for the whole of every level load
// while the main thread is inside the loader calling BindTexture,
// RebindLightmaps and DeleteSurface - and through them our texture-cache
// flush, LM_Destroy and the rest.
//
// Two threads on one context is undefined behaviour in the driver, and it
// presented exactly as undefined behaviour does: an access violation on an
// NVIDIA worker thread, jumping through a garbage pointer, at the HQ elevator
// on the third world of the session, after a hundred clean second loads two
// days earlier. Intermittent, transition-only, nowhere near our code in the
// stack.
//
// So every entry point the engine can call (the SLOT macro, dllmain.cpp) and
// every exported R3D_* the client can call takes this lock. It is recursive
// (a slot calling a slot is fine), and uncontended for 99% of a run because
// the loading thread only exists during a load. The report thread TRIES it
// and skips a cycle if it is busy, because Term holds it while stopping that
// thread. The watchdog never takes it: it suspends the main thread, and the
// main thread may be holding this.
// ---------------------------------------------------------------------------
#include <windows.h>

extern CRITICAL_SECTION g_csRender;

struct RenderGuard
{
	RenderGuard()  { EnterCriticalSection(&g_csRender); }
	~RenderGuard() { LeaveCriticalSection(&g_csRender); }
	RenderGuard(const RenderGuard&) = delete;
	RenderGuard& operator=(const RenderGuard&) = delete;
};
