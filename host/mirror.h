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
};
