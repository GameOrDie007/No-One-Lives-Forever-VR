// ---------------------------------------------------------------------------
// screenlock - CPU access to the back buffer, which is what LockScreen is.
//
// The VR client renders eye 0 into the left half of the screen, copies that
// half OUT of the screen into a stash surface, renders eye 1, and blits the
// stash back. The copy out is the engine's own DrawSurfaceToSurface reading
// from the screen surface, and that reaches the renderer as slot 30,
// LockScreen. Our stub returned LTFALSE with a null pointer, so the stash was
// never filled and the right eye stayed black.
//
// The contract is measured, not guessed. The real d3d.ren + 0x369D0 is a thin
// wrapper over IDirectDrawSurface7::Lock:
//
//   args 0..3   left, top, right, bottom, copied into one 16-byte RECT
//   arg 4       receives DDSURFACEDESC2.lpSurface (offset 0x24)
//   arg 5       receives DDSURFACEDESC2.lPitch    (offset 0x10)
//   returns     1 on success, 0 on failure, and 0 if already locked
//
// The DDSURFACEDESC2 is identified beyond doubt: the function stores 0x7C into
// its dwSize field, and sizeof(DDSURFACEDESC2) is 124.
//
// This is a full GPU->CPU round trip per lock and it is not fast. Correctness
// first; docs/M4-READBACK-MEASURED.md is the reason to measure it before
// building anything on top of it.
// ---------------------------------------------------------------------------

#pragma once

#include <d3d11.h>
#include <dxgi.h>

typedef void (*SL_LogFn)(const char* fmt, ...);

bool SL_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
               IDXGISwapChain* pSwap, SL_LogFn pfnLog);
void SL_Destroy();

// The back buffer changed, so the staging copy has to be rebuilt.
void SL_Invalidate();

// Returns 1 and fills ppData/pPitch, or 0. ppData points at the pixel (nLeft,
// nTop), which is what the caller asked to lock.
int  SL_Lock(int nLeft, int nTop, int nRight, int nBottom,
             void** ppData, int* pPitch);

// Copies whatever the engine wrote back to the back buffer.
void SL_Unlock();

// Copy a rectangle of the back buffer into caller memory. This is what
// BlitFromScreen is: the client renders the right eye into the left half of
// the screen, copies that half into a stash surface with this, draws the left
// eye over it, and puts the stash back on the right. With this unimplemented
// the stash was black, so the right half was black no matter what put it back.
bool SL_ReadRect(int nSrcX, int nSrcY, int nW, int nH,
                 void* pDst, int nDstPitch);

void SL_Stats(long* pnLocks, long* pnFailed, double* pfMillis);
