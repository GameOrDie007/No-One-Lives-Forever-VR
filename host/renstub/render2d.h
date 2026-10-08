// ---------------------------------------------------------------------------
// render2d - the D3D11 half of BlitToScreen.
//
// Kept out of dllmain.cpp deliberately. That file is the ABI and the
// measurement harness; this one is drawing, and mixing them makes both
// unreadable. The seam is narrow on purpose: this module knows nothing about
// RenderStructs or slots, and dllmain.cpp knows nothing about vertex buffers.
//
// The engine's 2D surfaces are system memory that the engine writes into with
// LockSurface. This module keeps one dynamic D3D11 texture per surface,
// re-uploading only when the surface has been marked dirty, and draws a
// textured quad from a source rectangle to a destination rectangle.
// ---------------------------------------------------------------------------

#pragma once

#include <d3d11.h>

// The log is dllmain's; this module borrows it rather than opening its own, so
// a failure here lands in the same file in the right order.
typedef void (*R2D_LogFn)(const char* fmt, ...);

// Build the pipeline. Returns false and logs why on any failure - a 2D path
// that silently does not draw is the failure mode this project keeps paying
// for, so every step reports.
bool R2D_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx, R2D_LogFn pfnLog);
void R2D_Destroy();

// The render target's pixel size, which is the coordinate space the engine's
// destination rectangles are expressed in.
// The back buffer is nPixW x nPixH pixels; the engine addresses it as
// nCoordW x nCoordH (its mode). Equal unless +StubRenderScale100 is set.
void R2D_SetTarget(ID3D11RenderTargetView* pRTV, int nPixW, int nPixH, int nCoordW, int nCoordH);

// Draw one surface. pKey identifies the surface for the texture cache; it is
// the engine-visible surface handle and is never dereferenced here.
void R2D_Blit(const void* pKey,
              const void* pPixels, int nPitch, int nSrcW, int nSrcH,
              int sx0, int sy0, int sx1, int sy1,
              int dx0, int dy0, int dx1, int dy1,
              float fAlpha);

// The same blit, drawn once into EACH half of a side-by-side frame, with the
// whole 2D layout mapped into the eye and centred on the forward ray. The
// engine lays its HUD out for one screen, which in VR puts the health bar in
// the left eye and the ammo count in the right.
// bCover: this blit exists to cover everything (a fade, a screen tint - a
// tiny surface stretched over the whole screen), so it fills each eye rather
// than being fitted into it.
void R2D_BlitStereo(const void* pKey,
                    const void* pPixels, int nPitch, int nSrcW, int nSrcH,
                    int sx0, int sy0, int sx1, int sy1,
                    int dx0, int dy0, int dx1, int dy1,
                    float fAlpha, int bCover);

// Where the forward ray lands in one half, in that half's own normalised
// device coordinates. With an asymmetric per-eye frustum this is NOT the
// middle of the half - about 15 degrees off - so 2D centred on the half is
// centred on nothing. Fed from the frustum the world was drawn with.
void R2D_SetEyeCentre(int nHalf, float fNdcX, float fNdcY);

// The client is in a menu, so draw the 2D layer identically into both halves:
// the host presents a menu as a world-locked flat panel, and a flat panel is
// one picture seen from two eyes. Off means play, where each half is centred
// on its own eye's forward ray.
void R2D_SetMenu(int b);

// Write every 2D surface out as a PPM, on its first upload and its 200th.
// +StubDump2D 1.
void R2D_SetDump2D(int b);
void R2D_SetTrace(int b);
// The pause menu as its own picture: 1 (default) draws pause-menu art into
// an overlay texture instead of the eyes; 0 draws it in the eyes as before.
void R2D_SetPauseQuad(int b);
// Brightness of MASK-mode text drawn into the pause overlay, 0..1 (1 = white).
void R2D_SetPauseTextWhite(float f);
// At present: was the overlay drawn this frame? Returns it, and resets.
int  R2D_OverlayEndFrame(ID3D11Texture2D** ppTex, int* pnW, int* pnH);
long R2D_OverlayUpRun();				// frames the overlay has been up without a break
void R2D_SetOverlayTrace(int b);		// +StubDumpOverlay: log each pause's blits

// How much of the eye the 2D layer fills, as a multiple of the fit scale.
// 1.0 spans the eye's full width; smaller pulls the HUD in toward the centre,
// which is where a headset's resolution and comfort both are.
void R2D_SetStereoScale(float f);
void R2D_MapWaitLine(char* out, int n);	// the 2D layer's map-wait counters, reset on read
void R2D_SetHudDepthCm(float f);	// 0 = infinity

// EXTRA scale for the 2D layer while in a menu, on top of the fit. 0 derives
// it: NOLF's interface is authored 4:3 and the engine hides the rest of a
// wider screen behind filler bars, so fitting the WHOLE layout into an eye
// spends 28% of it on bars and shrinks the menu by 1.387.
void R2D_SetMenuScale(float f);
void R2D_SetPauseScale(float f);
void R2D_SetPauseShift(float f);
// NDC offset that cancels head movement for the pause menu, per half.
void R2D_SetWorldLock(int nHalf, float fDX, float fDY);
// Compute the lock from the head pose; call per interface scene with that
// eye's tans, and with bPause 0 from world scenes to release it.
void R2D_UpdateWorldLock(int nHalf, const float* pTan4, int bPause);
void R2D_SetMenuZoom(float f, float ax, float ay);

// The engine wrote into a surface, so its texture is stale.
void R2D_Invalidate(const void* pKey);
// A SURFACE THE CLIENT DREW, as a texture for the 3D pass (VRPRIM_F_SURFACE,
// physical play's wrist display). Uploaded again on every call: the panel is
// small and the client redraws it whenever a number changes.
ID3D11ShaderResourceView* R2D_SurfaceSRV(const void* pKey, const void* pPixels, int nPitch, int nW, int nH);
// dllmain.cpp: the bits behind a surface handle the client passed, if it is
// one of ours; false for anything else.
bool Stub_SurfaceBits(void* h, const void** ppKey, const void** ppBits, int* pnPitch, int* pnW, int* pnH);
// Copy a back-buffer rect into a GPU texture that stands in for this
// surface's bits until the next Invalidate. See the note in render2d.cpp.
bool R2D_Snapshot(const void* pKey, int sx, int sy, int nW, int nH);

// The surface is gone; drop its texture.
void R2D_Forget(const void* pKey);

// For the periodic report: how much of this is actually happening.
// Every 2D surface seen this run, with the blit index it was last drawn
// at - which is what says whether it is still being drawn NOW.
void R2D_ReportSurfaces();

void R2D_Stats(int* pnTextures, long* pnUploads, long* pnDraws, long* pnFailed);

// Re-upload every surface on every blit, ignoring the dirty flag. A diagnostic
// for surfaces the engine fills without a Lock/Unlock pair, whose texture would
// otherwise keep the pixels it had when it was first created.
void R2D_SetAlwaysUpload(int b);

// The engine's Optimized2D blend mode, straight from SetOptimized2DBlend
// (LTSURFACEBLEND_*: 0 alpha, 1 solid, 2 add, 3 multiply, 4 multiply2,
// 5 mask, 6 maskadd). The renderer used to record this and never read it, so
// every blit was drawn alpha-blended - which turned the game's ADDITIVE screen
// tint into an opaque black rectangle over the whole picture, every frame.
void R2D_SetBlend(unsigned int nMode);

// 0 = every 2D blit is alpha (pre-blend-mode behaviour, makes the game black)
// 1 = all seven modes (breaks the main menu)
// 2 = ADD and SOLID only, the default. +StubBlendModes.
void R2D_SetBlendModes(int b);

// The engine's SetOptimized2DColor, applied as a multiply on every 2D blit.
// +StubColour2D 0 ignores it, which is what the renderer did before.
void R2D_SetColour(unsigned int c);
void R2D_SetColourEnabled(int b);

// The transparent colour OptimizeSurface was given for this surface. Texels
// matching it are discarded. +StubColourKey 0 draws them, which is how to see
// where they are. This is the magenta.
void R2D_SetTransparent(const void* pSurface, unsigned int nColour);
void R2D_SetKeyMask(int b);
void R2D_SetKeyBlack(int b);
void R2D_SetMenuFit(int b);
// The menu panel's aspect as the host presents it (width / height); the 2D
// layout is fitted to that band's height. 0 = the whole eye.
void R2D_SetMenuBand(float f);
// The 3D interface zoom of the menu pass, so the 2D layer scales with its boxes.
void R2D_SetMenuZoomExtra(float f);
void R2D_SetKeySoft(float f);
void R2D_SetKeySuppress(int b);
void R2D_SetColourKeyEnabled(int b);

// The transparent colour carried by the blit request now being issued.
void R2D_SetBlitKey(unsigned int c, int bHave);
