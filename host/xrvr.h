// ----------------------------------------------------------------------- //
//
// MODULE  : xrvr.h
//
// PURPOSE : OpenXR session, swapchains and frame submission.
//
//           Split from capture so neither file grows into the 300 KB
//           single-file trap the project brief warns about.
//
// ----------------------------------------------------------------------- //

#pragma once

#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32

#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <vector>

// Writes a texture to a 32-bit BMP. Costs a full GPU->CPU readback, so it is
// for diagnostics only.
void HostDumpTexture(ID3D11Device* pDev, ID3D11DeviceContext* pCtx,
					 ID3D11Texture2D* pTex, const char* pPath);

class XrVr
{
public:
	// Step 1: instance and system. Must run before the D3D device exists,
	// because the runtime dictates which adapter to use.
	bool	CreateInstance();
	// Why CreateInstance failed, for the message the player sees:
	// 1 no OpenXR runtime would start, 2 a runtime but no headset.
	int		FailReason() const { return m_nFail; }
	LUID	RequiredAdapter() const { return m_AdapterLuid; }

	// Step 2: session on the device created for that adapter.
	bool	CreateSession(ID3D11Device* pDevice);

	// Step 3: one swapchain per eye, sized to the game's per-eye image.
	bool	CreateSwapchains(int nWidth, int nHeight);
	void	DestroySwapchains();

	// Pumps events. Returns false when the runtime wants us gone.
	bool	PumpEvents();
	bool	IsRunning() const { return m_bRunning; }

	// Begins a frame and reports the predicted view poses.
	bool	BeginFrame(bool& bShouldRender);

	// Copies one eye region of pSrc into that eye's swapchain image.
	// When the source region is not the same size as the swapchain - a menu
	// spanning the full window, say - it is SCALED to fit rather than cropped,
	// which needs a shader rather than CopySubresourceRegion.
	bool	SubmitEye(ID3D11DeviceContext* pCtx, ID3D11Texture2D* pSrc,
					  int nEye, int nSrcX, int nSrcY, int nWidth, int nHeight,
					  bool bPanel = false);

	// Must be called once before SubmitEye can scale.
	bool	CreateBlitter(ID3D11Device* pDevice);

	// Ends the frame, presenting whatever eyes were submitted.
	// bMenu presents eye 0's image as a world-locked quad instead of a
	// projection layer, so a menu floats in space and the head can move around
	// it rather than dragging it along.
	void	EndFrame(bool bHaveImage, bool bMenu = false, bool bPauseQuad = false);
	// The pause menu's quad: anchored where the head is looking when the pause
	// opens, then left alone. Eye index 2 in SubmitEye feeds its swapchain.
	void	AnchorPausePanel();

	// Anchors the menu quad straight ahead of wherever the head is now. Called
	// on entering a menu so it appears in front of the player.
	void	AnchorMenuPanel();

	// Called every frame while a menu is up. Recovers from an anchor placed
	// before the headset was on the player's face, without following the gaze.
	void	UpdateMenuAnchor();

	// Asks the runtime for the highest display refresh rate it offers, up to
	// fMaxHz. Logs what was available and what was granted. No-op when the
	// runtime does not support XR_FB_display_refresh_rate.
	void	RequestRefreshRate(float fMaxHz);

	// --- motion controllers ------------------------------------------------
	//
	// Created after the session. Aim pose rather than grip pose: OpenXR's aim
	// is the pointing ray the runtime guarantees is consistent across
	// controllers, which is what a gun barrel should follow.
	bool	CreateActions();

	// Syncs input and locates both hands. Call once per frame after BeginFrame.
	void	UpdateActions();
	// One haptic pulse on a hand: amplitude 0..1, duration in ms.
	void	Pulse(int nHand, float fAmp, float fMs);

	struct HandState
	{
		bool		bActive = false;
		XrPosef		pose{};
		float		fTrigger = 0.0f;
		float		fGrip    = 0.0f;
		float		fStickX  = 0.0f;	// -1..+1, right positive
		float		fStickY  = 0.0f;	// -1..+1, up positive
		uint32_t	nButtons = 0;		// see kBtn* below
		// Game Or Die Hands: the palm (grip pose) in this hand's aim space, and
		// the touch sensors. bTouchKnown false on a controller without them.
		XrPosef		aimToGrip{};
		bool		bGripValid    = false;
		bool		bTriggerTouch = false;
		bool		bThumbTouch   = false;
		bool		bTouchKnown   = false;
	};
	const HandState& Hand(int i) const { return m_Hands[i]; }

	// Button bits, packed into VRHandState::nButtons.
	enum {
		kBtnTrigger   = 1 << 0,
		kBtnGrip      = 1 << 1,
		kBtnPrimary   = 1 << 2,		// A on the right hand, X on the left
		kBtnSecondary = 1 << 3,		// B / Y
		kBtnThumbClick= 1 << 4,
		kBtnMenu      = 1 << 5,
		// THE STEAM FRAME'S OWN BUTTONS, set only under its native profile.
		// The Frame is a split gamepad: the right hand carries A/B/X/Y and a
		// menu button, the left a D-pad and a View button, and each has a
		// shoulder. Primary/secondary/menu above are ALSO set on the Frame,
		// laid out as SteamVR's own Touch remap lays them, so a client that
		// ignores these bits plays exactly as it did before.
		kBtnPadX      = 1 << 6,		// right X
		kBtnPadY      = 1 << 7,		// right Y
		kBtnDpadUp    = 1 << 8,		// left D-pad
		kBtnDpadDown  = 1 << 9,
		kBtnDpadLeft  = 1 << 10,
		kBtnDpadRight = 1 << 11,
		kBtnShoulder  = 1 << 12,	// either hand's shoulder, on that hand
		kBtnView      = 1 << 13,	// left View
		kBtnPadMenu   = 1 << 14,	// right Menu
		kBtnFrame     = 1 << 15		// on both hands: the Frame profile is the one bound
	};

	void	Shutdown();

	// Head pose for the shared block, in degrees, plus the symmetric FOV the
	// game should render.
	void	GetHeadEuler(float& fYawDeg, float& fPitchDeg, float& fRollDeg) const;
	XrVector3f	HeadPosition() const;

	// Midpoint of the two eyes. Anything meaning "where the head is" must use
	// this rather than m_Views[0], which is the left eye.
	XrVector3f	HeadMidpoint() const;

	// Head rotation as the runtime gave it, undecomposed.
	XrQuaternionf HeadOrientation() const
	{
		return m_bViewsValid ? m_Views[0].pose.orientation
							 : XrQuaternionf{ 0.0f, 0.0f, 0.0f, 1.0f };
	}

	XrFovf		EyeFov(int nEye) const { return m_Views[nEye].fov; }
	float		Ipd() const;

	// What was last SUBMITTED for an eye: the declared frustum, the orientation
	// the picture was drawn from, and the part of the image the declaration
	// covers, as fractions. The desktop spectator view projects through this.
	struct DeclaredView
	{
		XrFovf			fov{};
		XrQuaternionf	q{ 0.0f, 0.0f, 0.0f, 1.0f };
		float			rx = 0.0f, ry = 0.0f, rw = 1.0f, rh = 1.0f;
		bool			bValid = false;
	};
	const DeclaredView&	Declared(int nEye) const { return m_Decl[nEye & 1]; }

	int		RecommendedWidth()  const { return m_nRecommendedW; }
	int		RecommendedHeight() const { return m_nRecommendedH; }

	// How far behind the current pose the displayed image actually is. The
	// submitted layer must carry the pose the frame was RENDERED with, not the
	// latest one, or the runtime reprojects against the wrong reference and
	// head roll shears the image.
	void	SetPoseLagMs(float fMs) { m_fPoseLagMs = fMs; }
	float	PoseLagMs() const { return m_fPoseLagMs; }

	// Multiplies the vertical half-angle we declare to the runtime. A tuning
	// knob, not a model: three attempts to derive the correct value from first
	// principles were wrong, so the working value is being measured instead.
	// >1 stretches the world vertically, <1 squashes it.
	void	SetFovYScale(float f) { m_fFovYScale = f; }

	// Records the pose published for a host frame number, so a captured image
	// carrying that number can later be matched to the pose it was drawn from.
	void	NotePoseFrame(uint32_t nFrame) { m_nPoseFrame = nFrame; }

	// Age, in ms, of the pose whose low byte is nLowByte - i.e. how stale the
	// image carrying that marker actually is. Negative if no match is found.
	double	AgeOfPoseByte(uint32_t nLowByte) const;

	// Markers that decoded and matched a pose, but named an image far older
	// than this pipeline can produce - so the match was a coincidence and the
	// newest pose was used instead. Nonzero means the marker is still wrong.
	uint32_t	PoseTooOldCount() const { return m_nPoseTooOld; }

	// Submit the next frame with the pose whose frame number ends in this
	// byte - the one the displayed image was actually rendered from. Pass -1
	// to fall back to the newest pose.
	void	SetSubmitPoseByte(int nLowByte) { m_nSubmitByte = nLowByte; }

	// Writes the next submitted eye images to BMP. What the runtime actually
	// receives, rather than what we believe we sent.
	void	RequestDump() { m_nDumpPending = 2; ++m_nDumpIndex; }
	double	LastEndCallMs() const { return m_fLastEndCallMs; }

	// 0 = declare the symmetric frustum the game rendered and submit the whole
	//     image. The runtime maps it into its own display frustum. This is what
	//     most OpenXR apps do and needs no assumptions beyond the FOV.
	// 1 = declare the runtime's asymmetric frustum and submit the matching
	//     sub-rectangle. Correct only if our belief about the rendered FOV is
	//     exactly right; wrong by any margin and the eyes disagree by a fixed
	//     angle, which is unfusable.
	// Tells the submission side that the client is rendering about the per-eye
	// optical centres, and what those centres are. Both must be in step: a
	// rotated render with an unrotated declaration (or the reverse) is worse
	// than neither.
	void	SetAsymCentres(bool bActive, const float* pYaw, const float* pPitch);

	// Submit the projection layer in VIEW space with head-relative eye poses,
	// which is OpenXR's way of nailing an image to the display: the compositor
	// then reprojects nothing, for rotation or position.
	//
	// Only ever correct when the CLIENT has put the head rotation into the
	// camera itself, which it does under VRHeadAsMouse and tells us about. A
	// head-locked layer drawn from a camera that did not turn with the head
	// would leave the world welded to the face.
	void	SetHeadLocked(bool b);

	// Carry the game's body yaw into the reference space that poses are
	// declared in and the layer is submitted in.
	//
	// The client renders with the head composed onto the body's aim, in the
	// BODY's frame; the host has always declared the head pose in the runtime's
	// ROOM frame. Those differ by exactly this angle, so the compositor's
	// correction for head motion is turned about axes rotated away from the
	// ones the image was drawn with - which is the world bending.
	//
	// nMode: 0 off, 1 on, 2 on with the opposite sign.
	void	SetBodyYaw(float fYawRad, int nMode);
	// Fold the head's current yaw and position into the declared space, so
	// the player looks down the game's forward from the game's eye height
	// wherever they are sitting or standing. ConsumeSpaceChanged reports a
	// recenter the RUNTIME did (the headset's own gesture), once.
	void	Recenter();
	bool	ConsumeSpaceChanged();
	bool	HeadLocked() const { return m_bHeadLockWanted && m_bHeadLockOk; }

	// Per-frame OpenXR calls whose result used to be thrown away. A runtime that
	// refuses every submission looks EXACTLY like one that accepts every
	// submission unless somebody counts - the PreyVR port lost a session to
	// xrEndFrame failing on 2036 frames out of 2036 in silence.
	void	SubmitFailures(uint32_t& nBegin, uint32_t& nRelease,
						uint32_t& nEnd, uint32_t& nEndMenu) const
	{
		nBegin   = m_nBeginFail;   nRelease = m_nReleaseFail;
		nEnd     = m_nEndFail;     nEndMenu = m_nEndMenuFail;
	}

	void	SetProjMode(int n) { m_nProjMode = n; }

	// The renderer is building each eye's true asymmetric projection, so
	// the image IS that eye's frustum and the runtime should be told
	// exactly that, over the whole image. Set from the shared block, which
	// the renderer writes - see VRShared.h, nNativeFrustum.
	void	SetNativeFrustum(bool b)
	{
		if (b != m_bNativeFrustum) m_bLoggedSubRect = false;
		m_bNativeFrustum = b;
	}
	// HOW BIG THE MENU PANEL IS, and how far away. A panel 2 m wide at 2 m
	// subtends 53 degrees of a headset's ~110, which is why headset testing
	// found the main menu small and the options hard to read. Width and
	// distance are separate because they are not the same complaint: making
	// it wider makes the text bigger, bringing it closer also makes it
	// harder to take in at a glance.
	void	SetMenuPanel(float fWidth, float fDist)
	{
		if (fWidth > 0.1f) m_fMenuWidth = fWidth;
		if (fDist  > 0.1f) m_fMenuDist  = fDist;
	}

	void	SetAntiAlias(bool b) { m_bAntiAlias = b; }

	// Size the swapchains to what the runtime asks for and enlarge into them
	// with Catmull-Rom, instead of handing over a small image for the runtime
	// to enlarge with a basic filter. Off returns to eye-sized swapchains.
	void	SetUpscale(bool b) { m_bUpscale = b; }

	// The FOV the client says it rendered, full angles in radians. When set,
	// it is declared verbatim instead of being reconstructed.
	void	SetClientFov(float fFovX, float fFovY)
	{
		if (fFovX != m_fClientFovX || fFovY != m_fClientFovY) m_bLoggedSubRect = false;
		m_fClientFovX = fFovX;
		m_fClientFovY = fFovY;
	}

private:
	struct EyeChain
	{
		XrSwapchain						handle = XR_NULL_HANDLE;
		std::vector<XrSwapchainImageD3D11KHR>	images;
		uint32_t						acquired = 0;
		bool							submitted = false;
	};

	XrInstance		m_Instance	= XR_NULL_HANDLE;
	XrSession		m_Session	= XR_NULL_HANDLE;
	XrSpace			m_Space		= XR_NULL_HANDLE;

	// The head itself, as a reference space. A layer submitted in this space is
	// head-locked, which is how the head-as-mouse experiment removes the
	// runtime's reprojection from the picture entirely.
	XrSpace			m_ViewSpace	= XR_NULL_HANDLE;

	// LOCAL, rotated by the game's body yaw. Poses are declared in this and the
	// projection layer is submitted in it, so the frame the image was rendered
	// in and the frame the compositor corrects in are the same one.
	XrSpace			m_GameSpace	= XR_NULL_HANDLE;
	float			m_fBodyYaw   = 0.0f;		// what m_GameSpace was built for
	float			m_fWantYaw   = 0.0f;		// what the client last published
	int				m_nYawMode   = 0;
	bool			m_bLoggedYawSpace = false;
	float			m_fRefYaw    = 0.0f;		// the game yaw the frames were anchored at
	bool			m_bYawRefSet = false;
	float			m_fRecYaw    = 0.0f;		// recenter: yaw folded into the game space
	XrVector3f		m_RecPos     { 0.0f, 0.0f, 0.0f };	// recenter: origin, in LOCAL
	bool			m_bHaveRec   = false;		// a recenter has been applied
	bool			m_bRecDirty  = false;		// rebuild the space whatever the throttle says
	bool			m_bSpaceChanged = false;	// the runtime moved LOCAL; reported once
	XrSystemId		m_System	= XR_NULL_SYSTEM_ID;
	LUID			m_AdapterLuid{};

	EyeChain		m_Eyes[2];
	EyeChain		m_Ovl;						// the pause overlay, one eye wide
	int				m_nOvlW = 0, m_nOvlH = 0;	// its swapchain size
	XrPosef			m_PausePose{};
	bool			m_bPauseAnchored = false;
	bool			m_bLoggedPauseQuad = false;
	XrView			m_Views[2]{};

	// The same two eyes located against the HEAD rather than the world: what
	// the head-locked layer has to declare. Essentially half an IPD either way.
	XrView			m_ViewsHead[2]{};
	bool			m_bViewsHeadValid = false;
	bool			m_bHeadLockWanted = false;
	bool			m_bHeadLockOk     = true;	// cleared if the runtime refuses
	bool			m_bLoggedHeadLock = false;

	// Failure counts for the per-frame calls. Reported in the periodic block so
	// a total failure is visible in the log rather than only in the headset.
	uint32_t		m_nBeginFail   = 0;
	uint32_t		m_nReleaseFail = 0;
	uint32_t		m_nEndFail     = 0;
	uint32_t		m_nEndMenuFail = 0;
	XrFrameState	m_FrameState{ XR_TYPE_FRAME_STATE };
	bool			m_bRunning	= false;
	bool			m_bViewsValid = false;
	bool			m_bHaveRefreshExt = false;
	bool			m_bHaveFrameExt = false;	// XR_VALVE_frame_controller_interaction listed
	bool			m_bFrameProfile = false;	// and it is the profile the runtime bound
	int				m_nFail = 0;
	bool			m_bLoggedEyeFov = false;

	XrActionSet		m_ActionSet = XR_NULL_HANDLE;
	XrAction		m_aAim = XR_NULL_HANDLE, m_aTrigger = XR_NULL_HANDLE;
	XrAction		m_aGrip = XR_NULL_HANDLE, m_aPrimary = XR_NULL_HANDLE;
	XrAction		m_aSecondary = XR_NULL_HANDLE, m_aThumb = XR_NULL_HANDLE;
	XrAction		m_aMenu = XR_NULL_HANDLE;
	XrAction		m_aStickX = XR_NULL_HANDLE, m_aStickY = XR_NULL_HANDLE;
	XrAction		m_aHaptic = XR_NULL_HANDLE;
	// Steam Frame only; bound on no other profile.
	XrAction		m_aPadX = XR_NULL_HANDLE, m_aPadY = XR_NULL_HANDLE;
	XrAction		m_aDpadUp = XR_NULL_HANDLE, m_aDpadDown = XR_NULL_HANDLE;
	XrAction		m_aDpadLeft = XR_NULL_HANDLE, m_aDpadRight = XR_NULL_HANDLE;
	XrAction		m_aShoulder = XR_NULL_HANDLE, m_aView = XR_NULL_HANDLE;
	XrAction		m_aPadMenu = XR_NULL_HANDLE;
	// Game Or Die Hands: the palm and the touch sensors.
	XrAction		m_aGripPose = XR_NULL_HANDLE;
	XrAction		m_aTriggerTouch = XR_NULL_HANDLE, m_aThumbTouch = XR_NULL_HANDLE;
	XrSpace			m_GripSpace[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	bool			SuggestFrameBindings(bool bOldShoulderName, bool bHands = true);
	void			NoteInteractionProfile();
	XrPath			m_HandPath[2]{};
	XrSpace			m_AimSpace[2]{ XR_NULL_HANDLE, XR_NULL_HANDLE };
	HandState		m_Hands[2];
	bool			m_bActionsReady = false;
	bool			m_bLoggedHands = false;
	bool			m_bAsymActive = false;
	float			m_fAsymYaw[2]   = { 0.0f, 0.0f };
	float			m_fAsymPitch[2] = { 0.0f, 0.0f };

	// Ring of recent poses, so a frame can be submitted with the pose that
	// produced it rather than the pose current at submission time.
	struct PoseSample
	{
		double		fTimeMs = -1.0;
		uint32_t	nFrame  = 0;		// host frame the client was given
		XrPosef		pose[2]{};
	};
	uint32_t			m_nPoseFrame = 0;
	int					m_nSubmitByte = -1;		// marker of the image being shown
	uint32_t			m_nPoseTooOld = 0;		// matches rejected as implausibly stale

	// The oldest image we will believe a marker about. Chosen against the
	// measurement, not by feel: the good blocks in this project's own logs
	// run 10-31 ms average with a worst case near 97, and the bad ones run
	// 600-760 with a worst case over 1500. There is no overlap, so anywhere
	// in between separates them - 150 ms is comfortably clear of every real
	// sample ever recorded and rejects every bad one.
	static constexpr double kMaxPoseAgeMs = 150.0;
	static const int	kPoseRing = 128;
	PoseSample			m_PoseRing[kPoseRing];
	int					m_nPoseWrite = 0;
	float				m_fPoseLagMs = 45.0f;
	float				m_fFovYScale = 1.0f;
	bool				m_bLoggedSubRect = false;
	int					m_nDumpPending = 0;
	double				m_fLastEndCallMs = 0.0;	// the last xrEndFrame call alone, wall time
	int					m_nDumpIndex = 0;		// suffix, so a second dump keeps the first
	int					m_nProjMode = 0;
	bool				m_bNativeFrustum = false;
	float				m_fClientFovX = 0.0f;
	float				m_fClientFovY = 0.0f;

	// Scaling blit resources.
	ID3D11Device*			m_pDevice   = nullptr;
	ID3D11VertexShader*		m_pVS       = nullptr;
	ID3D11PixelShader*		m_pPS       = nullptr;
	ID3D11PixelShader*		m_pPSFxaa   = nullptr;
	ID3D11PixelShader*		m_pPSUpscale = nullptr;	// Catmull-Rom, for enlarging
	bool					m_bUpscale  = true;		// swapchains at runtime size
	ID3D11SamplerState*		m_pSampler  = nullptr;
	ID3D11Buffer*			m_pCB       = nullptr;
	ID3D11Texture2D*		m_pStage    = nullptr;	// shader-readable copy of the capture
	int						m_nStageW   = 0;
	int						m_nStageH   = 0;
	bool					m_bLoggedBlitWorld = false;	// blit described once per case
	bool					m_bLoggedBlitPanel = false;
	bool					m_bLoggedVpWorld   = false;
	bool					m_bLoggedVpPanel   = false;
	int64_t					m_nSwapFormat = 0;	// needed to view a TYPELESS swapchain image
	float					m_fPanelScale = 0.72f;	// menu panel size within the eye
	bool					m_bAntiAlias = true;
	// 2.6 m wide at 1.6 m away subtends about 78 degrees, against 53 for the
	// 2-at-2 it replaced. Bigger and closer, which is what was asked for,
	// without filling the whole field.
	// THE MENU PANEL, IN METRES. Angular width is 2*atan(w/2 / d).
	//
	// 2.6 at 1.6 is 78 degrees, and in headset testing that was near
	// impossible to navigate: the tester had to close one eye and look almost
	// off screen to reach the edge. It got that big because 2.0 at 2.0 - 53
	// degrees - had read as too small to read, so the panel was grown to
	// make the text bigger. That is treating the symptom with the wrong lever.
	//
	// The text is not small relative to the menu: measured against a d3d.ren
	// control, a menu line is 0.0076 of the menu's width in retail and 0.0078
	// in ours. It is small in DEGREES, because a headset has about a tenth the
	// angular resolution of the monitor the layout was drawn for. Growing the
	// panel buys legibility and spends it on having to scan - and at 78 the
	// scanning got worse than the reading.
	//
	// 2.1 at 1.7 is 63 degrees: a middle setting, and A JUDGEMENT CALL THAT
	// NEEDS A HEADSET - it cannot be measured from a frame capture. The real
	// fix is to scale the menu's own layout up for VR rather than the panel,
	// which is the same task as enlarging the menu font and is not done.
	// -MenuWidth / -MenuDist override both.
	float					m_fMenuWidth = 2.1f;
	float					m_fMenuDist  = 1.7f;
	XrPosef					m_MenuPose{};			// world-locked menu anchor
	XrVector3f				m_AnchorHead{};			// head position when anchored
	bool					m_bMenuAnchored = false;

	int				m_nWidth  = 0;
	int				m_nHeight = 0;
	DeclaredView	m_Decl[2];
	int				m_nRecommendedW = 0;
	int				m_nRecommendedH = 0;
};
