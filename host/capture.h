// ----------------------------------------------------------------------- //
//
// MODULE  : capture.h
//
// PURPOSE : Windows Graphics Capture of the game window, landing as a D3D11
//           texture on the GPU. Nothing touches the game process.
//
//           Reports where the client area sits inside the captured frame, so
//           callers can copy an eye region without window chrome.
//
// ----------------------------------------------------------------------- //

#pragma once

#include <windows.h>
#include <d3d11.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

class WindowCapture
{
public:
	// Waits for a LithTech window to appear and settle at a usable size, then
	// starts capturing. Blocks for up to nWaitSeconds.
	bool	Start(ID3D11Device* pDevice, int nWaitSeconds = 300);
	void	Stop();

	// Latest frame, or empty if none arrived since the last call.
	winrt::com_ptr<ID3D11Texture2D>	TryGetFrame();

	int		EyeWidth()  const { return m_nEyeW; }
	int		EyeHeight() const { return m_nEyeH; }
	int		CropX()     const { return m_nCropX; }
	int		CropY()     const { return m_nCropY; }
	HWND	Window()    const { return m_hWnd; }
	// Age of the last fresh frame at the moment it reached us, in ms. The
	// measurable part of how stale the displayed image is.
	double	LastFrameAgeMs() const { return m_fLastAgeMs; }

	// Reads the client's frame marker out of a captured image: eight 8x8
	// blocks along the top-left edge encoding the low byte of the host frame
	// the camera was posed from. Returns -1 if it cannot be read.
	//
	// This is the only way to know WHICH of the client's frames an image is.
	// Without it the host can only guess how stale the picture is, and every
	// guess tried so far has been wrong in a way that made things worse.
	//
	// nCropX/nCropY are the top-left of the client area WITHIN pSrc, and must
	// come from whichever source produced pSrc. They are not this object's own
	// crop: with -SharedFrame the image is the renderer's own surface and has
	// no window chrome, so its crop is (0,0) while a captured window's is not.
	// Reading the window crop out of a shared frame missed the marker by 37
	// rows and made it unreadable for whole runs.
	int		ReadFrameMarker(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
							ID3D11Texture2D* pSrc, int nCropX, int nCropY);

	// Find the marker and take the crop FROM it, instead of deriving the crop
	// from window metrics and hoping the marker is underneath it.
	//
	// The renderer draws the marker at the top-left of its own render surface,
	// so wherever the marker is, that IS the top-left of the picture. Deriving
	// it instead from ClientToScreen against the DWM extended frame bounds has
	// now been wrong twice. It returned (2,37) for a capture whose picture
	// starts at (0,0), which put the 8-row marker 37 rows above every read -
	// 563 of 563 unreadable across a whole headset session - and ran the eye
	// rect 37 rows off the bottom of the picture into black.
	//
	// Searches a bounded region once and keeps the SMALLEST offset that
	// decodes: shifting right or down within an 8-pixel block still samples the
	// right blocks, but shifting left of the true origin samples outside the
	// marker and fails - so the smallest valid offset is the origin.
	// Returns false and changes nothing if no marker is found.
	bool	CalibrateCropFromMarker(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
									ID3D11Texture2D* pSrc);

	// Correlates a band across the middle of the two eye halves and returns
	// the horizontal shift in pixels between them, or -1 if no clear match.
	// Only meaningful while the client is rendering a calibration frame, where
	// the halves differ solely by a known yaw.
	int		MeasureHalfShift(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
							 ID3D11Texture2D* pSrc);

	int		ClientW()   const { return m_nClientW; }
	int		ClientH()   const { return m_nClientH; }

	// True when the game's client area no longer matches what we are set up
	// for - the player changed resolution. Everything sized from it is stale.
	bool	HasResized() const;
	// Take the window's current client size as the new normal WITHOUT
	// rebuilding the capture - for when the renderer's shared frame is the
	// picture and the window is not being read at all.
	void	AcceptCurrentSize()
	{
		RECT rc{};
		if (m_hWnd && GetClientRect(m_hWnd, &rc) && rc.right > 0 && rc.bottom > 0)
		{
			m_nClientW = rc.right - rc.left;
			m_nClientH = rc.bottom - rc.top;
		}
	}

	// Re-measures and restarts capture on the same window. The caller must
	// also recreate its swapchains.
	bool	Restart(ID3D11Device* pDevice);

	// The game reports its true render surface, which is smaller than the
	// window client. Applying it re-centres the crop so the split lands on the
	// real stereo seam. Returns true if the geometry changed.
	bool	ApplyGameScreenSize(int nGameW, int nGameH);

	// False once the game exits, so the host can shut down cleanly instead of
	// spinning on a dead window.
	bool	IsWindowAlive() const { return m_hWnd && IsWindow(m_hWnd); }
	// False once the game PROCESS has exited. The window test above is not
	// enough on the way out: the engine shrinks its window as it quits, which
	// reads as a resize, and the rebuild then waited up to thirty seconds for
	// a window that would never come while the OpenXR session starved - the
	// host crashed eleven seconds into that wait at every quit.
	bool	GameAlive() const;
	~WindowCapture();

private:
	HWND	m_hWnd     = nullptr;
	HANDLE	m_hGameProc = nullptr;		// SYNCHRONIZE handle on the game, see GameAlive
	int		m_nEyeW    = 0;
	int		m_nEyeH    = 0;
	int		m_nCropX   = 0;
	int		m_nCropY   = 0;

	// The client size we are actually configured for. Compared directly rather
	// than reconstructed from m_nEyeW * 2, which can never match an odd width
	// because of integer division - that made HasResized() true forever and
	// rebuilt capture every frame.
	double	m_fLastAgeMs = 0.0;
	winrt::com_ptr<ID3D11Texture2D>	m_MarkerStage;	// 96x8 readback target
	int		m_nClientW = 0;
	int		m_nClientH = 0;

	winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool	m_Pool{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureSession		m_Session{ nullptr };
	winrt::Windows::Graphics::Capture::GraphicsCaptureItem			m_Item{ nullptr };

	// The captured surface is only valid while its frame object lives. Held
	// here and closed at the START of the next TryGetFrame, so the texture
	// stays alive for as long as the caller is using it. Releasing the frame
	// before returning left the pool free to recycle the surface, and every
	// eye image submitted to the runtime came out black.
	winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame		m_Frame{ nullptr };
};
