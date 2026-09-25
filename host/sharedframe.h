// ----------------------------------------------------------------------- //
//
// MODULE  : sharedframe.h
//
// PURPOSE : The game's frame, taken from the shared D3D11 texture the renderer
//           publishes, instead of by capturing its window.
//
//           Window capture publishes at the monitor's refresh, so about a
//           third of the frames reaching a 90 Hz headset are duplicates, and
//           the image can never beat what the desktop can show. This is the
//           renderer's own back buffer, at whatever rate the game draws.
//
//           Presents the same shape as WindowCapture on purpose - TryGetFrame,
//           EyeWidth, CropX - so the host can prefer it and fall back without
//           the rest of the frame path knowing which one it got.
//
//           Used by BOTH the host and host/eyeprobe, deliberately: the module
//           that ships is then the module that was desk-tested, rather than a
//           second implementation of the same idea.
//
// ----------------------------------------------------------------------- //

#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>

struct VRSharedState;

class SharedFrame
{
public:
	// Attach to the block and open the texture. Returns false, with a reason
	// logged, if there is no block, the block is too old, the renderer is not
	// publishing, the adapter does not match, or the open fails. Every one of
	// those is a normal condition that means "use window capture", not an
	// error.
	bool	Start(ID3D11Device* pDevice, void (*pfnLog)(const char*, ...));
	void	Stop();

	bool	IsOpen() const { return m_pShared != nullptr; }

	// A private copy of the latest frame, or null if the serial has not moved
	// since the last call. The copy exists so the caller can hold the texture
	// for as long as it likes without keeping the renderer's present thread
	// waiting on a keyed mutex.
	ID3D11Texture2D* TryGetFrame();

	// Geometry, in the same terms WindowCapture reports it. The shared texture
	// IS the render surface, so there is no window chrome to crop: the offsets
	// are zero and the eye is exactly half the width. That is the arithmetic
	// window capture had to reconstruct, and getting it wrong handed each eye
	// a sliver of the other.
	int		EyeWidth()  const { return (int)m_nW / 2; }
	int		EyeHeight() const { return (int)m_nH; }
	int		CropX()     const { return 0; }
	int		CropY()     const { return 0; }
	int		FullWidth() const { return (int)m_nW; }

	// The renderer's frame number for the image last returned. This is what
	// the frame marker exists to recover by decoding swatches out of the
	// pixels; here it is simply a number the renderer wrote down.
	uint32_t Serial() const { return m_nLastSerial; }
	// The marker byte the client painted into the frame last returned, from the
	// block (v16), or -1 when the block does not carry it.
	int      Marker() const { return m_nLastMarker; }
	// The pause overlay, when the renderer publishes one: the latest copy, or
	// null if there has never been one. OverlayW/H are its size.
	ID3D11Texture2D* TryGetOverlay();
	int  OverlayW() const { return (int)m_nOvlW; }
	int  OverlayH() const { return (int)m_nOvlH; }
	bool HasOverlay() const { return m_pOvl != nullptr; }

	// The renderer has stopped publishing - it went away, or the level is
	// reloading. The caller should fall back rather than submit a stale image.
	bool	IsStale(DWORD dwNowTick) const;

	// THE RENDERER WAS REPLACED. The engine unloads and reloads its renderer
	// on every focus change; the new one re-creates the textures under the
	// same names and counts its serial from zero again, while the textures
	// opened here are the OLD ones, kept alive by this very reference. Serials
	// kept "moving", so the frame never went stale and the headset and mirror
	// showed the last picture from before the alt-tab while the game ran on
	//. A serial that goes BACKWARDS is the sign;
	// the caller reopens by name.
	bool	Restarted() const { return m_bRestarted; }

private:
	ID3D11Device*        m_pDevice = nullptr;
	ID3D11DeviceContext* m_pContext = nullptr;
	ID3D11Texture2D*     m_pShared = nullptr;	// ring[0]
	IDXGIKeyedMutex*     m_pMutex  = nullptr;
	ID3D11Texture2D*     m_pCopy   = nullptr;
	// The game's ring of three (see eyeshare.cpp); m_nRing is how many
	// opened, 1 for a game that publishes only the first.
	ID3D11Texture2D*     m_pSharedRing[3] = { nullptr, nullptr, nullptr };
	IDXGIKeyedMutex*     m_pMutexRing[3]  = { nullptr, nullptr, nullptr };
	int                  m_nRing = 1;

	HANDLE          m_hMap   = nullptr;
	VRSharedState*  m_pBlock = nullptr;

	uint32_t m_nW = 0, m_nH = 0;
	uint32_t m_nLastSerial = 0;
	int      m_nLastMarker = -1;
	ID3D11Texture2D* m_pOvl      = nullptr;
	IDXGIKeyedMutex* m_pOvlMutex = nullptr;
	ID3D11Texture2D* m_pOvlCopy  = nullptr;
	uint32_t m_nOvlW = 0, m_nOvlH = 0, m_nOvlSerial = 0;
	DWORD    m_dwLastNew   = 0;
	bool     m_bRestarted  = false;
	void   (*m_pfnLog)(const char*, ...) = nullptr;
};
