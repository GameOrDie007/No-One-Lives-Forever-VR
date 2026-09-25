// ---------------------------------------------------------------------------
// vrblock - the renderer's own view of the host's shared state.
//
// The renderer needs two things the scene description cannot carry: the four
// frustum angles of the eye it is about to draw, and a way to tell the host
// that it is drawing them itself. Both live in the block the OpenXR host
// publishes, which the client already reads - this opens a second, independent
// view of the same named mapping rather than routing anything through the
// client.
//
// Reading it here rather than adding a channel through the client is not a
// shortcut. The projection is the renderer's business now; a value that passes
// through a component that does not use it is a value that can be transformed
// on the way, and this project already has two fields in that block
// (fAppliedYawRad, nAsymActive) that exist only because both ends derived the
// same quantity and disagreed.
//
// Fails open in every direction: no host, wrong magic, wrong version or a stale
// alive tick all mean "no frustum", and the renderer falls back to the
// symmetric projection it has always used.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>

typedef void (*VRB_LogFn)(const char* fmt, ...);

// Tangents rather than angles: the projection wants tangents, and converting
// once at the boundary keeps trigonometry out of the per-frame path.
struct VRBFrustum
{
	float fTanL, fTanR, fTanU, fTanD;
};

// Attach if the host is up. Cheap to call repeatedly; retries at a low rate so
// the host may be started at any time.
bool VRB_Poll(VRB_LogFn pfnLog);

// The host has written within the last second.
bool VRB_Live();

// The host frame counter (one per xrWaitFrame); false with no block.
bool VRB_HostFrame(uint32_t* pOut);

// Eye 0 is the left. Returns false when there is no live block, so the caller
// can fall back rather than draw with a zero frustum.
bool VRB_EyeFrustum(int nEye, VRBFrustum* pOut);

// Tell the host we are building the projection ourselves, so it declares the
// runtime's own frustum over the whole image instead of a symmetric one over a
// slice. The only field the renderer writes.
void VRB_PublishNativeFrustum(int bOn);
// The pause overlay: its size, whether it carries a menu this frame, and
// the serial that says a new picture is up. Block version 15.
void VRB_PublishOverlay(uint32_t nSerial, uint32_t nW, uint32_t nH, int bActive);

// Describe the shared eye texture. The serial is written LAST, so a host that
// sees a new serial has already seen the dimensions belonging to it.
void VRB_PublishEyeTex(uint32_t nSerial, uint32_t nW, uint32_t nH,
					   uint32_t nFormat, int32_t nLuidLo, int32_t nLuidHi);

// Is the client in a menu rather than playing? The client publishes this as
// GetGameState() != GS_PLAYING. The renderer needs it because a menu is
// presented by the host as a world-locked flat PANEL, and a flat panel wants
// the two halves identical - while the HUD in play wants each half centred on
// its own eye's forward ray. Returns false when there is no live block.
bool VRB_InMenu();
// The head's yaw and pitch in degrees, as the host published them. False
// when there is no live block. Used to world-lock the pause menu.
bool VRB_HeadYawPitch(float* pYawDeg, float* pPitchDeg);

// Why the last VRB_EyeFrustum returned false. Never null.
const char* VRB_LastFail();

// How many times a torn read was answered with last frame's optics.
long VRB_ReusedCount();

void VRB_Close();
