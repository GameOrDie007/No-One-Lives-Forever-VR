// ---------------------------------------------------------------------------
// eyeshare - hand the finished frame to the host as a shared D3D11 texture.
//
// The host's picture currently comes from Windows Graphics Capture of the game
// WINDOW. That publishes at the monitor's refresh, so about a third of the
// frames reaching a 90 Hz headset are duplicates, and the image can never be
// larger than the desktop can show.
//
// Both processes are D3D11 now. This copies the back buffer into a texture
// created with a NAME, which the 64-bit host opens with
// OpenSharedResourceByName - no handle marshalling, and the 32/64 boundary
// does not arise.
//
// Access is arbitrated by a keyed mutex rather than by hoping. Two processes
// writing and reading one texture with no lock produces tearing, and tearing
// in a stereo image is exactly the kind of artefact that gets attributed to
// the projection.
// ---------------------------------------------------------------------------

#pragma once

#include <d3d11.h>
#include <stdint.h>

typedef void (*ES_LogFn)(const char* fmt, ...);

// Create the shared texture at the back buffer's size. Returns false and logs
// the reason on any failure; the caller carries on without it, and the host
// falls back to window capture, so this is never fatal.
bool ES_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
               uint32_t nW, uint32_t nH, ES_LogFn pfnLog);

void ES_Destroy();

// Copy the back buffer in and bump the serial. Called from SwapBuffers, before
// Present, so the host is offered the same image the monitor gets.
void ES_Publish(ID3D11Texture2D* pBackBuffer);
// The pause overlay: a second shared texture, one eye wide. Created
// beside the eye texture; published when the renderer drew into it.
void ES_PublishOverlay(ID3D11Texture2D* pTex);
uint32_t ES_OverlaySerial();
bool ES_OverlaySize(uint32_t* pnW, uint32_t* pnH);

// For the periodic report: publishes, and how many were dropped because the
// keyed mutex was still held by the reader.
void ES_Stats(long* pnPublished, long* pnDropped, uint32_t* pnSerial);

// What to write into the shared block, so vrblock owns the block and this
// module owns the texture.
bool ES_Describe(uint32_t* pnW, uint32_t* pnH, uint32_t* pnFormat,
                 int32_t* pnLuidLo, int32_t* pnLuidHi);
