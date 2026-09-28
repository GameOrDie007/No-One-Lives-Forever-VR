// ----------------------------------------------------------------------- //
//
// MODULE  : mirror.h
//
// PURPOSE : Desktop preview window showing ONE eye.
//
//           Without this the only thing on the monitor is the game's own
//           side-by-side window, which is why the desktop looked like a VR
//           view being mirrored into the headset. This decouples them: the
//           game window can be whatever resolution the headset wants, and the
//           monitor shows a normal single-eye picture.
//
// ----------------------------------------------------------------------- //

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

class MirrorWindow
{
public:
	// nEyeW/nEyeH are the size of ONE eye. The window is sized from that, so
	// the preview keeps the eye's aspect instead of cropping it into 16:9.
	bool	Create(ID3D11Device* pDevice, int nEyeW, int nEyeH, bool bRightEye);
	void	Destroy();

	// Copies the chosen eye region out of the captured frame and presents it.
	void	Present(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pSrc,
					int nSrcX, int nSrcY, int nSrcW, int nSrcH);

	// Parks the preview exactly over the game window and keeps it there. The
	// game renders side-by-side, so any part of its window left uncovered shows
	// a double image on the desktop. Covering it is more reliable than moving
	// the game off-screen, which depends on a display that may not be attached
	// and on the compositor still updating a window nobody can see.
	void	CoverWindow(HWND hGame);

	// THE PAUSE MENU ON THE DESKTOP TOO. In the headset the pause menu is its
	// own quad, so the eye this preview copies never contains it. While it is
	// up, pass the overlay here (premultiplied alpha, one eye in size) and
	// Present blends it over the same crop of the eye; pass null when it goes.
	void	SetPauseOverlay(ID3D11Texture2D* pOvl) { m_Ovl.copy_from(pOvl); }

	// THE SPECTATOR VIEW - a Game Or Die feature for every port. The raw eye
	// shakes with every small movement of the head, which is unwatchable on a
	// stream. With it on, the preview shows a smaller cut-out of the eye that
	// follows a SMOOTHED version of where the head points (a turn is followed,
	// a wobble is not) and is rotated to keep the horizon level. No extra
	// render: the eye is much wider than a monitor picture, so the cut-out has
	// room to move inside it. fSmoothSec is the follow time constant, fZoom the
	// cut-out's width as a fraction of the eye.
	void	SetSpectator(bool bOn, float fSmoothSec, float fZoom)
	{ m_bSpec = bOn; m_fSpecSmooth = fSmoothSec; m_fSpecZoom = fZoom; }
	bool	Spectator() const { return m_bSpec; }

	// What the eye being shown is, from the host's submission: declared
	// frustum as tangents (left and down negative), the orientation it was
	// drawn from (x,y,z,w), and the fraction of the image the frustum covers.
	// bWorld false (a menu, a pause) shows the plain view instead.
	void	SetSpectatorView(const float q[4], float fTanL, float fTanR, float fTanU, float fTanD,
							 float fRx, float fRy, float fRw, float fRh, bool bWorld);

	// The spectator cut-out on its own, for the desk harness: centre, unit
	// right and down, width and height, in eye-texture pixels. The harness
	// sets the preview's size and a clock with SpectatorTestSetup first.
	bool	SpectatorCrop(int nEyeX, int nEyeY, int nEyeW, int nEyeH,
						  float& cx, float& cy, float& fRx, float& fRy,
						  float& fDx, float& fDy, float& W, float& H);
	void	SpectatorTestSetup(int nBufW, int nBufH, double fClockMs)
	{ m_nWidth = nBufW; m_nHeight = nBufH; m_fSpecClockMs = fClockMs; }

	// Hides the preview whenever the game is not the foreground window, so a
	// topmost preview does not sit over everything the player alt-tabs to.
	void	FollowForeground(HWND hGame);

	// Pumps window messages. False once the user closes it.
	bool	Pump();

	bool	IsRightEye() const { return m_bRightEye; }
	HWND	Window() const { return m_hWnd; }

private:
	HWND							m_hWnd = nullptr;
	ID3D11Device*					m_pDevice = nullptr;	// for the letterbox clear
	winrt::com_ptr<IDXGISwapChain1>	m_SwapChain;
	int								m_nWidth = 0;
	int								m_nHeight = 0;
	bool							m_bRightEye = true;
	bool							m_bOpen = false;
	bool							m_bVisible = true;
	bool							m_bAllowTearing = false;
	bool							m_bCover = false;	// sized to cover the game window
	int								m_nCoverW = 0;
	int								m_nCoverH = 0;
	int								m_nEyeW = 0;		// as created, before cover resizing
	int								m_nEyeH = 0;
	double							m_fLastPresentMs = 0.0;
	double							m_fPresentMs = 0.0;
	unsigned						m_nPresents = 0;

	// The pause-overlay blend (see SetPauseOverlay), made on first use.
	bool	DrawOverlay(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pBack,
						float fU, float fV, float fW, float fH, int nDstX, int nDstY, int nDstW, int nDstH);
	winrt::com_ptr<ID3D11Texture2D>			m_Ovl;
	winrt::com_ptr<ID3D11VertexShader>		m_pOvlVS;
	winrt::com_ptr<ID3D11PixelShader>		m_pOvlPS;
	winrt::com_ptr<ID3D11SamplerState>		m_pOvlSmp;
	winrt::com_ptr<ID3D11BlendState>		m_pOvlBlend;
	winrt::com_ptr<ID3D11Buffer>			m_pOvlCB;
	bool									m_bOvlTried = false;
	bool									m_bOvlSaid = false;

	// The spectator view (see SetSpectator).
	bool	DrawSpectator(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pBack, ID3D11Texture2D* pSrc,
						  int nEyeX, int nEyeY, int nEyeW, int nEyeH);
	bool	EnsureShaders();
	bool									m_bSpec = false;
	float									m_fSpecSmooth = 0.35f;
	float									m_fSpecZoom = 0.78f;
	float									m_fSpecQ[4] = { 0, 0, 0, 1 };
	float									m_fSpecTan[4] = { -1, 1, 1, -1 };	// L R U D
	float									m_fSpecRect[4] = { 0, 0, 1, 1 };
	bool									m_bSpecWorld = false;
	bool									m_bSpecViewSet = false;
	float									m_fSpecFwd[3] = { 0, 0, -1 };		// smoothed, world
	bool									m_bSpecFwdValid = false;
	float									m_fSpecPrevHead[3] = { 0, 0, -1 };
	float									m_fSpecRate[3] = { 0, 0, 0 };	// turn rate vector, degrees/s, smoothed
	float									m_fSpecSpeed = 0.0f;			// its size
	double									m_fSpecLastMs = 0.0;
	double									m_fSpecClockMs = -1.0;	// >= 0: the harness's clock
	bool									m_bSpecSaid = false;
	winrt::com_ptr<ID3D11PixelShader>		m_pSpecPS;
	winrt::com_ptr<ID3D11Buffer>			m_pSpecCB;
	ID3D11Texture2D*						m_pSpecSrcKey = nullptr;	// the texture m_pSpecSRV views
	winrt::com_ptr<ID3D11ShaderResourceView> m_pSpecSRV;
	bool									m_bSpecFailSaid = false;
};
