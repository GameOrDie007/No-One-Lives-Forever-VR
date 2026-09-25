#include <cctype>
#include <cstddef>		// offsetof
#include "render3d.h"
#include "renlock.h"
#include <d3d11_1.h>
// A dump that writes the ALPHA channel as grey instead of the colour - the
// only way to see what the pause overlay's coverage looks like in a BMP.
// See R3D_SetDumpAlphaAsGrey beside R3D_DumpBackBuffer.
static int g_bDumpAlphaAsGrey = 0;
#include "lightmap.h"
#include "dtx.h"
#include "rezfs.h"
#include "spranim.h"
namespace { void PrimReleaseAll(); }
// A picture's size by name, where the name may be a SPRITE: the world walk
// needs the width and height to place a polygon's texture coordinates, and
// a .spr answers with its first frame. The HQ waterfall was dropped as
// untextured here before this.
static bool LoadInfoAny(const char* pszName, DtxInfo* pOut)
{
	if (SprAnim_IsSpr(pszName))
	{
		char szFrame[128];
		if (!SprAnim_FirstFrame(pszName, szFrame, sizeof szFrame)) return false;
		return Dtx_LoadInfo(szFrame, pOut);
	}
	return Dtx_LoadInfo(pszName, pOut);
}
#include "world.h"
#include "butes.h"
#include "render2d.h"
#include <map>
#include <set>
#include <unordered_map>
#include <string>

#include <d3dcompiler.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <new>			// std::bad_alloc - see R3D_BuildWorld
#include <utility>
#include <algorithm>

namespace
{
	R3D_LogFn Log = nullptr;

	ID3D11Device*           g_pDev = nullptr;
	ID3D11DeviceContext*    g_pCtx = nullptr;
	ID3D11RenderTargetView* g_pRTV = nullptr;

	ID3D11VertexShader*      g_pVS  = nullptr;
	ID3D11PixelShader*       g_pPS  = nullptr;
	ID3D11InputLayout*       g_pIL  = nullptr;
	ID3D11Buffer*            g_pCB  = nullptr;
	// ---- FOG -------------------------------------------------------------
	//
	// NOLF's levels carry fog, and the client already publishes it: a level's
	// SpecialFX sets FogEnable, FogR/G/B, FogNearZ and FogFarZ as ENGINE
	// CONSOLE VARIABLES, which the retail d3d.ren reads and we never have.
	// Without it the intro's night forest has a pure black sky where the
	// original has a dark blue haze, and distance carries no depth cue at all.
	//
	// Its own constant buffer at b1, written when the values change rather
	// than folded into b0. b0 is mapped from six different places - the alpha
	// threshold, the sky's pre-translated matrix, a door's own transform - and
	// every one of them would have had to learn about fog for a WRITE_DISCARD
	// not to leave it undefined.
	ID3D11Buffer*            g_pCBFog = nullptr;
	// b2: the light a model instance stands in. Its own buffer rather than a
	// wider b0 so that adding it cannot disturb the five places that write
	// the matrix and the lightmap parameters.
	ID3D11Buffer*            g_pCBMdl = nullptr;
	float                    g_fMdlSet[4] = { -1, -1, -1, -1 };
	int    g_bFog      = 0;
	int    g_bFogAllow = 1;			// +StubFog 0 turns the whole thing off
	float  g_fFogRGB[3] = { 0, 0, 0 };
	float  g_fFogNear  = 100.0f, g_fFogFar = 5000.0f;
	// SKY FOG. LithTech fogs the sky separately: WorldProperties carries
	// SkyFogEnable / SkyFogNearZ / SkyFogFarZ, the client mirrors them into
	// the console, and the retail renderer draws the sky pass with fog on at
	// those distances (measured from the sky camera, in the sky's own units)
	// in the level's fog colour. Ours excluded the sky from fog outright, so
	// the stormy harbour's purple cloud sheet drew at its own colour where
	// retail's is half sunk in blue-grey fog: (53,52,68) against (40,54,68),
	// reported on 12 September as pink and too bright. +StubSkyFog 0 reverts.
	int    g_bSkyFog = 0, g_bSkyFogAllow = 1;
	int    g_bWaterFlowAny = 0;		// +StubWaterFlowAny 1: scroll every water face, tall or not
	// +StubWaterBias <n>: a depth bias on the water rasterizer state, in
	// 24-bit depth units (negative pulls the water towards the camera), with
	// a slope-scaled term of n/250. The Dive cutscene's sea shows wedge-shaped
	// patches at its far edge that move with the head (the clip and the desk,
	// 22 September) - depth fighting between the water surface and what lies
	// just under it where both converge at the horizon. 0 = none.
	int    g_nWaterBias = 0;
	int    g_bSkyChecker = 0;		// +StubSkyChecker 1: every sky face wears a checker instead of its texture
	char   g_szSkySkip[64] = "";		// +StubSkySkip <layer name>: draw the sky without it
	float  g_fSkyFogNear = 100.0f, g_fSkyFogFar = 1000.0f;
	// THE CAMERA LIGHT ADD: a colour the engine adds to every pixel of the
	// camera's picture. CScreenTintMgr feeds it the maximum of its tints - a
	// water container's LightAdd, the damage flash, the poison's green - and
	// the retail renderer drew it as a full-screen additive sheet after the
	// world. Ours never read it, so the underwater picture kept only the
	// container's (0, 0.5, 0.7) light SCALE and came out near black where
	// retail's is a readable blue (finding 6), and the poison tinted nothing
	// (finding 6c). Published by the client each frame from
	// GetCameraLightAdd; +StubLightAdd 0 reverts.
	float  g_fLightAdd[3] = { 0, 0, 0 };
	long   g_nEnvDraws = 0;
	int    g_bLightAddAllow = 1;
	// ENVIRONMENT MAPS ON WORLD POLYGONS. The retail renderer (d3d.ren carries
	// EnvMapEnable, EnvMapWorld, EnvMapPolyGrids, EnvPanSpeed, EnvScale) draws a
	// texture's declared environment map over the surface, its coordinates
	// from the reflected view direction and panned over time. The HQ
	// waterfall (Door57 and Water0 wearing WA0010, "EnvMap Tex\EnvMap\
	// EnvMap011.dtx") shimmers in retail with a still camera and stood frozen
	// in ours. +StubEnvMap 0 reverts; +StubEnvScale100 and +StubEnvPan100 tune.
	int    g_bBatchList = 0;
	// WATER'S LIGHT AND ITS REFLECTION, fitted to a retail clip of the HQ
	// waterfall (13 September). Retail draws the sheet at about 0.8 of its
	// texture, neutral, over the dark wall behind; ours lit it by its warm
	// lightmap times the 1.7 scale and it came out white. And retail's
	// reflection map MODULATES the sheet (bright and dark streaks about the
	// same mean) where ours ADDED a faint grey. +StubWaterLight100 (0 keeps
	// the lightmap), +StubEnvContrast100.
	float  g_fWaterLight  = 0.8f;
	// The world-anchored map: units per repeat across and up a surface, and
	// water's flow down it in units per second. The vertical repeat is long
	// so the map's blobs draw out into streaks. +StubEnvRepeatU/V, +StubEnvFlow.
	float  g_fEnvRepeatU  = 40.0f;
	float  g_fEnvRepeatV  = 240.0f;
	float  g_fEnvFlow     = 50.0f;
	float  g_fEnvContrast = 0.7f;
	// A water volume's TOP face is drawn by its polygrid; see the build loop.
	int    g_bWaterTop = 1;
	long   g_nWaterTopSkipped = 0;
	long   g_nWaterTopGate = 0, g_nWaterTopNamed = 0, g_nWaterTopFlat = 0;
	// Per model: flat water faces skipped, and what the level file says about
	// that model's surface (-1 no ShowSurface entry, 0/1 its value). Printed
	// once per build: which brushes the skip actually took.
	std::map<std::string, std::pair<long, int>> g_WaterTopByModel;
	float  g_fFlatLight   = 0.0f;		// > 0: the draws that follow take this flat light
	int    g_nSkipBatch = -1;		// +StubSkipBatch N leaves batch N out: what is behind it?
	// THE MAP COORDINATE SCALE: what fraction of the map a full swing of
	// the reflection vector covers (0.5 = the whole map once). Retail
	// shows about half as many streaks across the HQ waterfall as 0.5
	// gave (13 September, a clip against the desk frame).
	float  g_fEnvCoord = 0.25f;
	int    g_bEnvMap = 1;
	float  g_fEnvScale = 0.5f;
	float  g_fEnvPan = 0.05f;
	ID3D11Buffer* g_pCBEnv = nullptr;		// b6: camera right, up, position, pan/scale/flag
	// b7: how far a WATER face's own texture has scrolled down, in repeats.
	ID3D11Buffer* g_pCBWater = nullptr;
	float g_fWaterFlow = 0.0f;		// texture repeats per second, downward
	std::map<std::string, ID3D11ShaderResourceView*> g_EnvByName;
	// The environment map for a texture NAME, from its file's command string;
	// cached, nullptr when the file names none.
	// A water texture by its path: NOLF keeps them under TEX\WATER\.
	bool IsWaterName(const char* psz)
	{
		if (!psz) return false;
		char sz[96]; strncpy_s(sz, psz, _TRUNCATE);
		for (char* c = sz; *c; ++c) { *c = (char)toupper((unsigned char)*c); if (*c == '/') *c = '\\'; }
		return strstr(sz, "\\WATER\\") != nullptr;
	}
	ID3D11ShaderResourceView* EnvSRVForName(const char* pszTex)
	{
		if (!g_pDev || !pszTex || !*pszTex) return nullptr;
		std::map<std::string, ID3D11ShaderResourceView*>::iterator it = g_EnvByName.find(pszTex);
		if (it != g_EnvByName.end()) return it->second;
		ID3D11ShaderResourceView* pEnv = nullptr;
		DtxInfo fi{};
		if (Dtx_LoadInfo(pszTex, &fi) && fi.szEnvMap[0])
		{
			DtxInfo ei{};
			pEnv = Dtx_Get(g_pDev, fi.szEnvMap, &ei);
			Log("  R3D ENVMAP: %s names %s -> %s", pszTex, fi.szEnvMap, pEnv ? "loaded" : "NOT FOUND");
		}
		g_EnvByName[pszTex] = pEnv;
		return pEnv;
	}
	int    g_bFogDirty = 1;
	ID3D11Buffer*            g_pVB  = nullptr;
	ID3D11DepthStencilView*  g_pDSV = nullptr;
	ID3D11Texture2D*         g_pDS  = nullptr;
	ID3D11DepthStencilState* g_pDSS = nullptr;
	ID3D11DepthStencilState* g_pDSSRev = nullptr;	// GREATER, for reversed-Z
	// Has this client ever published VRMODEL_F_OWNED? The player's view
	// weapon carries it every frame in the field, so one frame decides. Until
	// then the camera-inside rule keeps its old, wider reach.
	bool g_bOwnedFlagSeen = false;
	int                      g_bRevZ = 1;
	// Override the scene description's near plane, in world units. 0 = use what
	// the engine gave us (0.44). Only a measuring instrument: raising it buys
	// depth precision that reversed-Z buys for free, so a run with a high near
	// plane is a REFERENCE the two mappings can be compared against, rather
	// than something to ship. 1 unit is about 17 mm, so 50 is 0.85 m.
	float                    g_fNearOverride = 0.0f;

	// Override the FAR plane. This is the depth-precision instrument that the
	// near plane could not be: moving far from 100000 to 100100 changes every
	// depth value slightly while clipping NOTHING - the level's geometry is
	// nowhere near either value - so any pixel that changes did so because its
	// depth ordering was decided by precision. Raising the near plane looked
	// like the same experiment and was not: at 50 units it clips the alleyway
	// walls beside the camera, and the image change that produced was read as
	// Z-fighting for one wrong measurement.
	float                    g_fFarOverride = 0.0f;
	ID3D11RasterizerState*   g_pRS  = nullptr;
	ID3D11RasterizerState*   g_pRSWater = nullptr;	// cull back; see the water draw
	ID3D11RasterizerState*   g_pRSWorldCull = nullptr;	// cull back, no bias; the opaque world loop
	// +StubWorldCull 1: the opaque world loop culls back faces, as retail's
	// one-sided world polygons are drawn. A thin brush - the Morocco car's
	// trunk lid, its engine bay under the fender - has a top and an underside
	// a hair apart; drawing both made them fight: the lid flickered to its
	// dark inside and a black wedge of engine sat on the fender (headset,
	// 23 September). Water has culled this way since 14 September, which is
	// what says our winding matches retail's. Default 2 (world models only):
	// mode 1 took the M01S02 rooftop, whose main-world polygons wind the other way.
	int                      g_bWorldCull = 2;	// 2 by default: world MODELS only; desk-checked on 5 levels
	ID3D11BlendState*        g_pBlendOpaque = nullptr;
	// ALPHA TO COVERAGE FOR CUT-OUTS. A leaf or a canopy edge is alpha
	// TESTED: every pixel is wholly in or wholly out, and under MSAA at a
	// distance - through the sniper scope above all - those edges sparkle
	// from frame to frame. Run 12 in the headset: the canopy across the street
	// flickered, and so did the plant at the far left.
	// With alpha-to-coverage the texture's alpha decides how many of the
	// four samples a pixel covers, so an edge is a gradient and stays put.
	// The shader already outputs the texture's alpha. +StubA2C 0 reverts.
	ID3D11BlendState*        g_pBlendA2C = nullptr;
	int                      g_bA2C = 1;
	// ALPHA SHARPENING FOR CUT-OUTS. Under alpha-to-coverage a leaf's edge
	// is as soft as the texture's alpha ramp, and a MINIFIED leaf - a palm
	// across the street through the scope - has a ramp several texels wide
	// that flickers as it moves. Rescaling the alpha by its screen-space
	// derivative (fwidth) puts the edge at half coverage exactly one pixel
	// wide wherever it is: crisp up close, a steady soft line far away.
	// Runs 12-13 in the headset: the palm tree leaves also shimmered.
	// Carried to the shader in fogp.z. +StubAlphaSharp 0 reverts.
	int                      g_bAlphaSharp = 1;
	bool                     g_bA2CBound = false;	// what the world pass has bound now

	// The client's model list for this frame. Copied, not referenced: the
	// client hands over a pointer to its own storage and we are called from
	// its thread, but the draw happens later in the frame.
	VRModelFrame             g_Models = {};
	int                      g_bHaveModels = 0;
	int                      g_bModelBoxes = 0;
	ID3D11Buffer*            g_pBoxVB = nullptr;
	ID3D11Buffer*    g_pMeshVB = nullptr;
	// Consecutive failures to allocate it; see the creation site.
	int              s_nMeshVBFailed = 0;
	UINT             g_nMeshRegion = 0;	// which ring region holds this frame's mesh
	int              g_nMeshRing = 4;	// regions; 1 = the old discard-every-frame buffer
	ID3D11ShaderResourceView* g_pBoxTex = nullptr;

	int  g_nTargetW = 0, g_nTargetH = 0;
	uint32_t g_pBuiltFrom = 0;		// the world this vertex buffer was built from
	// STAGE 2. The level's own .DAT, parsed beside the heap walk. It draws
	// nothing yet: this exists so the file parse can be CHECKED against the
	// engine's own numbers before anything is drawn from it.
	WorldFile* g_pWorldFile = nullptr;
	int  g_bWorldFile = 1;			// +StubWorldFile
	long g_nWorldFileOK = 0, g_nWorldFileFail = 0;
	int  g_bSurfProbe = 0;			// +StubSurfProbe, the correlation probe
	long g_nProbeModels = 0, g_nProbeAligned = 0, g_nProbeDense = 0;
	long g_nProbeSpanEqFile = 0, g_nProbePolyEq = 0, g_nProbeNamed = 0;
	int  g_nProbeSaid = 0;

	// THE MARKER RULE, and it comes out of the FILE. +StubMarkerFlags, 0 to
	// disable. 202 is worn by AI.dtx and Invisible.dtx and by NOTHING else -
	// measured over m01s02's 19752 surfaces - and 363 of those surfaces sit
	// inside ordinary and translucent world models, which is exactly the HQ
	// red-lettered-blocks case a per-model name list can never reach.
	//
	// The heap's own flags CANNOT do this: one value there covers 14751
	// marker polygons and 5898 ordinary ones. See docs and the 5 September
	// flag histogram.
	uint32_t g_nMarkerFlags = 202;
	// +StubBridgeCheck. THE INSTRUMENT THAT SHOULD HAVE EXISTED FIRST.
	//
	// The heap->file surface bridge was measured on m01s02 and on m01s02
	// only, then shipped as the default, and it deleted real scenery and
	// the skybox on another level. 565 of 565 models on ONE level is one
	// sample, and this repo already had the lesson written down.
	//
	// The check needs no headset and no eyes: the FILE says which texture a
	// surface uses, and stage 1 means the HEAP can name the texture it
	// actually bound for the same polygon. If the bridge is right those two
	// names agree. If it is off by anything at all they disagree wholesale,
	// and a wrong mapping cannot hide - which is exactly what a rule that
	// deletes geometry has to be held to.
	int  g_bBridgeCheck = 0;
	// +StubFileTex. Texture polygons the ENGINE never bound a texture for,
	// using the level file's own name for the surface. ON by default: it
	// only ever ADDS a picture to a polygon that was being dropped, and
	// dropping them is what was costing the tester world items.
	int  g_bFileTex = 1;

	// +StubMarkerTex. THE MARKER RULE, BY THE FILE'S OWN TEXTURE NAME.
	//
	// These are the editor's markers, and they are markers wherever they
	// appear. AI volumes, invisible brushes, occluders, hull makers, sky
	// brushes, 'nothing' - and SOUND FILTERS. The retail renderer never
	// shows any of them; a sky brush shows the skybox through a portal we
	// do not implement, so leaving it out is what the old dropped-polygon
	// behaviour already did.
	//
	// THE CLAIM THAT USED TO BE HERE - "counted over all 103 worlds,
	// nothing else is spelled like them" - WAS WRONG, and the tester found it
	// by looking at the game rather than at a log. M02S01 draws the words
	// SOUND FILTER, in orange, tiled across a wall of the street.
	//
	// So the list was rebuilt from the level files instead of extended by
	// one. Every texture name in every world, and the markers turn out to
	// live at the ROOT of tex\ where real art lives in a category folder
	// (tex\stone\st03\st0139.dtx). Thirteen names sit at that root:
	//
	//   invisible 102 worlds   sky 87   ai 73   nothing 64   hullmaker 51
	//   SOUND 35 <- was drawn   chrome 23   occluder 16
	//   skypan2/3/4 12 between them   blackfade 8   arjan 1
	//
	// The three that are NOT markers were checked by dumping the pictures:
	// CHROME is marble, SKYPAN* are sky panoramas and BLACKFADE is solid
	// black. SOUND.DTX is 64x64 and reads SOUND FILTER in letters, which
	// is not something a 2000 game paints on a wall on purpose.
	//
	// 35 of 103 worlds carry sound-filter brushes: 175 surfaces in all, so
	// this hides lettering rather than deleting scenery.
	//
	// THIS IS WHY THE FILE-TEXTURE RESCUE NEEDED IT. Those polygons used
	// to be dropped for want of a texture, which HID the markers by
	// accident. Giving them their real pictures back put 'sky' lettering
	// across the intro and magenta INVISIBLE stripes through the level.
	// Rescuing the scenery and hiding the markers are the same job.
	//
	// A NAME beats the flags rule that came before it: flag 202 misses
	// Hullmaker (flag 0, shared with real geometry) and cannot be trusted
	// without the flag being exclusive, whereas these names are what the
	// editor itself calls them.
	int  g_bMarkerTex = 1;
	long g_nMarkerTexSkipped = 0;
	bool IsMarkerTexture(const char* pszBase)
	{
		static const char* const kMark[] = {
			"AI.DTX", "INVISIBLE.DTX", "OCCLUDER.DTX",
			"HULLMAKER.DTX", "SKY.DTX", "NOTHING.DTX",
			"SOUND.DTX" };
		for (int i = 0; i < (int)(sizeof kMark / sizeof kMark[0]); ++i)
			if (_stricmp(pszBase, kMark[i]) == 0) return true;
		return false;
	}
	long g_nBridgeAgree = 0, g_nBridgeDisagree = 0, g_nBridgeNoName = 0;
	long g_nBridgeVerified = 0, g_nBridgeRejected = 0;
	int  g_nBridgeSaid = 0;
	long g_nMarkerSkipped = 0;		// polygons dropped by the rule
	long g_nFileMapped = 0, g_nFileUnmapped = 0;	// models, by whether the
										// heap<->file bridge validated
	// Name -> index into g_pWorldFile->Models, built once per world. A
	// linear search per model is 565 x 565 string compares on every build.
	std::map<std::string, size_t> g_FileByName;
	// VERTICES THE WORLD ACTUALLY DREW. A reload that leaves the world
	// switched off is invisible to every other counter in the report,
	// because they are all about models - which is exactly how one
	// shipped to the tester.
	long long g_nWorldDrawn = 0;
	long long g_nCutDrawn   = 0;	// of those, alpha tested
	size_t   g_nBuiltWithTex = 0;	// how many textures existed when it was built
	size_t   g_nResolvedWithTex = (size_t)-1;	// when batches were last resolved
	// 1. These polygons are the SKY - measured, by drawing the frame with and
	// without them: skipping leaves a black hole where the sky is. The engine
	// never loads their texture because it draws the sky by another path
	// entirely, and our white 1x1 stand-in happens to read as overcast, which
	// is why 1899 untextured polygons went unnoticed for so long.
	//
	// Kept as a switch because "what are those polygons" is a question the
	// picture should be able to answer: +StubDrawUntextured 0 removes them.
	int      g_bSkipHidden = 1;	// leave the collision hull and the volumes out
	int      g_bDrawUntextured = 0;
	// +StubSprAdd 0 draws additive sprites as alpha again, which is the arm
	// that produced the black boxes round the coronas.
	int      g_bSprAdd = 1;
	// Frames until which EVERY texture re-checks its name, ignoring the
	// stagger. Opened by a world load or a folder change - the only two
	// moments the engine frees texture objects and recycles their addresses.
	long     g_nVerifyUntil = 0;
	long     g_nVerifyForced = 0;
	// Recycled addresses answered from the file (or refused) instead of from
	// the previous owner's cached picture. See ResolveTexObj.
	long     g_nRecycledRefused = 0;
	long     g_nSprAdditive = 0;
	// Sprites drawn with FLAG2_MULTIPLY. Counted beside the additive ones
	// because a blend mode that is never selected looks exactly like one
	// that is selected and does nothing.
	long     g_nSprMultiply = 0;
	// Untextured polygons of a TRANSLUCENT world model specifically. The sky is
	// untextured too and the stand-in colour is doing its job there, so this is
	// split by class rather than by flipping the switch above.
	// +StubDrawUntexturedTranslucent 1 puts the blue quads back.
	int      g_bDrawUntexturedTrans = 0;
	long     g_nBuilds = 0;

	// The largest upward-facing polygon in the level, and what it is textured
	// with. See the note in the build loop.
	// The last camera the world was drawn from, kept so a probe can be fired
	// along the direction the player is actually looking without the caller
	// having to know anything about the scene description.
	float    g_fLastPos[3]  = { 0, 0, 0 };
	float    g_fLastQuat[4] = { 0, 0, 0, 1 };
	// THE LAST EYE CAMERA, which a scope pass does not overwrite. The model
	// list is built once per frame by whichever pass comes first, and the
	// scope's picture comes first when a scoped gun is held - so the model
	// culls ran against the SCOPE's camera, 40 units out at the gun, and a
	// fresh shell casing a few units behind it was culled as "behind the
	// camera" for the eyes too. The AK47 and the Hampton lost their casing
	// flight (the headset, 22 September; the silenced SMG, no scope, kept it).
	// Culls use this; anything behind the eye is behind the scope as well.
	float    g_fEyeCamPos[3] = { 0, 0, 0 };
	float    g_fEyeCamQuat[4] = { 0, 0, 0, 1 };
	int      g_bHaveEyeCam = 0;
	int      g_bCullFromEye = 1;		// +StubCullFromEye 0: cull from the pass's own camera, as before
	int      g_bHaveCam = 0;
	int      g_bProbeDump = 0;	// the probe reads a texture back

	float    g_fFloorArea = 0.0f;
	uint32_t g_pFloorTex = 0, g_pFloorSurf = 0;
	float    g_fFloorTW = 0, g_fFloorTH = 0;
	int      g_nFloorVerts = 0;
	const char* g_szFloorDump = "x";

	// The textures the last build wanted and could not get. Kept so the
	// periodic report can ask whether they have ARRIVED since - which is the
	// difference between "the engine never gives us these" and "we built the
	// world too early and never rebuilt".
	std::vector<std::pair<uint32_t, int>> g_MissingAtBuild;
	// What the last build could not texture, and how many rebuilds have been
	// spent chasing it. g_MissingAtBuild cannot answer this on its own: it is
	// keyed by texture, and the case that matters has the key 00000000 - a
	// surface whose texture pointer the engine has not filled in yet - which
	// no lookup will ever resolve.
	int      g_nNoTexAtBuild = 0;
	int      g_nNoTexPrev    = 0;	// what it was one rebuild ago
	int      g_nRetexStalls  = 0;	// rebuilds in a row that improved nothing
	long     g_nBridgeByPlane = 0;	// models vouched for by their planes, no texture bound yet
	int      g_nRetexSeen    = -1;	// which build result the stall count has judged
	bool     g_bRetexGaveUp  = false;
	uint32_t g_pRetexWorld   = 0;
	long     g_nRetexLoad    = -1;	// the world LOAD the counters belong to
	int      g_nRetexBuilds  = 0;
	// Below this, a rebuild costs more than it recovers. M01S02 settles at ONE
	// polygon and would otherwise rebuild until the cap chasing it.
	const int kRetexFloor    = 16;
	size_t   g_nBuiltWithTexReport = 0;
	int      g_bBuiltLM = -1;	// the lightmap enable this buffer was built with
	int  g_nPolys = 0, g_nTris = 0;
	long g_nDraws = 0;

	// A CPU-side sample of the world, kept only so the matrix can be checked
	// without a screenshot.
	ID3D11Buffer* g_pTestVB = nullptr;	// three clip-space vertices
	int g_bTest = 0;
	float g_fLMScale = 1.7f;	// see the note by the pixel shader

	// ---- MULTISAMPLING ------------------------------------------------
	//
	// This renderer had NO anti-aliasing of any kind: every SampleDesc.Count
	// in the module was 1, and the only smoothing anywhere was the host's
	// FXAA, applied AFTER the image had been stretched 2.4x to reach what the
	// runtime asked for. In headset testing on 8 September there was far too much
	// aliasing and the picture felt low-resolution.
	//
	// It is very cheap here. The game is not fill-rate bound at all - going
	// from 2560x1384 to 3840x2076, 2.25x the pixels, cost 0.5 fps - so the
	// budget for edge quality is enormous by 2026 standards on a 2000 engine.
	//
	// Both eyes render into ONE target at different viewports, so there is one
	// MSAA texture and one resolve, not two of each.
	int  g_nMsaa = 4;
	ID3D11Texture2D*        g_pMsaaTex = nullptr;
	ID3D11RenderTargetView* g_pMsaaRTV = nullptr;
	// THE SCOPE PASS. A third world pass a frame, from the scope's objective
	// lens along its axis with the zoom level's field, into a square texture
	// of its own; the eyepiece disc the client publishes samples it. While
	// g_bScopePass is set the world pass draws into this target, keeps its
	// hands off the per-eye stash, and does no resolve or eye bookkeeping.
	int g_bScopePass = 0;
	int g_nScopeSize = 768;
	ID3D11Texture2D*          g_pScopeTex = nullptr;
	ID3D11RenderTargetView*   g_pScopeRTV = nullptr;
	ID3D11ShaderResourceView* g_pScopeSRV = nullptr;
	ID3D11Texture2D*          g_pScopeDS  = nullptr;
	ID3D11DepthStencilView*   g_pScopeDSV = nullptr;
	VRScopeFrame g_Scope = {};
	int g_bHaveScope = 0;
	long g_nScopePasses = 0;
	int  g_nMsaaMade = 0;		// what we actually got, after support checks
	// +StubMsaaClear 1 clears the multisampled target to MAGENTA at the first
	// world pass of each frame. Nothing in this game is magenta, so whatever
	// comes back magenta is precisely what the world pass never writes - which
	// is the one question the shard artifact turns on.
	int  g_bMsaaClear = 0;
	int  g_bMsaaFresh = 1;		// a new frame has begun; clear on next pass
	// The last matrix written to the constant buffer. Rewriting only the
	// alpha threshold still has to supply the mvp, so it is kept here.
	float g_fLastMVP[16] = { 0 };
	int   g_bAlphaTest = 1;
	// What the constant buffer currently holds. Mapping it for every run
	// cost 8-10 fps; almost every run wants the same value as the last.
	float g_fAlphaCutSet = 0.0f;
	int   g_bLMOnly = 0;		// draw the lightmap without the texture
	int   g_bCutProbe = 0;		// name every batch's cut-out measurement
	// +StubCutGuess 0: honour ONLY the artist's own alpharef; never guess a
	// cut-out from the pixels.
	//
	// The guess deletes 89% of the Morocco hotel lamp shade. LAMP_05B.DTX is a
	// sheet that is 88.9% transparent with the shade's art in the other 11%,
	// and it declares NO alpharef - because it is an OPAQUE PROP the game
	// never alpha tested. Guessing "cut-out" and discarding everything under
	// 0.5 leaves a fragment in the air, which is exactly the reported missing ceiling
	// lamp: just a floating bulb.
	//
	// The same guess fires on four of the five bullet-hole textures, which are
	// soft-edged decals wanting a blend rather than a hard 0.5 cut.
	//
	// Default 1: that is the arm that has always shipped, and turning it off
	// changes every texture in the game that declares nothing, so it wants
	// measuring across levels before it becomes the default.
	int   g_bCutGuess = 1;
	// A model with FLAG_ENVIRONMENTMAP never takes the guessed cut: its skin
	// alpha is the reflection mask. See the piece loop.
	int   g_bEnvNoCut = 1;
	int   g_bPieceMatIndex = 1;	// a piece's texture slot from its material index (+StubPieceMatIndex 0: by name)
	long  g_nEnvNoCutPieces = 0;
	// WHICH FRAME to name every model piece's skin on. 0 is off.
	// A cap of "the first N" reports the STARTUP: the first entries out of the
	// piece log are from before the throttled name check has corrected a single
	// skin, so they are the STALE names, and a whole theory about additive
	// blending was built on them. Naming ONE LATE FRAME reports the picture that
	// is actually on the screen, and reports ALL of it rather than the first 24.
	int   g_nSkinFrame = 0;
	// A SPRITE'S TEXTURE MUST AGREE BY NAME, EVERY FRAME.
	// SpriteTexture resolves by ADDRESS and the engine recycles texture
	// objects, so a stale address resolves confidently to whatever now lives
	// there - on M05S01 that is the MAIN MENU'S olive card, and the night alley
	// is covered in opaque olive squares that retail draws as two small yellow
	// points. The skin path already learned this; the sprite path only
	// re-checked one frame in 30, so 29 frames in 30 it trusted the address.
	// +StubSprStrictTex 0 goes back to trusting it.
	int   g_bSprStrictTex = 1;

// lmp.z is the alpha-test threshold: 0 means draw every texel.
// lmp.w, the pixel shader's OUTPUT ALPHA. 1 for everything opaque; the glass
// pass sets it and puts it back. It was an unused zero in the constant
// buffer, which is why nothing else had to change to make room for it.
static float g_fAlphaOut = 1.0f;

// WHAT THE RENDERER WAS DOING WHEN IT DIED.
//
// The crash filter can already name the module and walk the frame chain, and
// on this renderer that answers "in d3dstub" and stops - one huge per-frame
// function, so every crash lands in the same place and says nothing about
// WHICH pass. Five of 484 archived runs carry a crash block and three of them
// sit right after the world build, which is as far as the evidence goes.
//
// A const-char* store per phase, which is one instruction and no formatting,
// turns that into a sentence. Written straight through so a crash mid-frame
// cannot leave it stale, and read by StubCrashFilter.
static const char* volatile g_pszPhase = "(before the first frame)";
static long        volatile g_nPhaseFrames = 0;
// The accessors live at the bottom of the file, OUTSIDE the anonymous
// namespace this state sits in - defined here they would be internal and the
// crash filter could not link to them.
#define R3D_PHASE(sz) (g_pszPhase = (sz))
extern "C" const char* R3D_Phase() { return g_pszPhase; }

// +StubCrashTest 1: FAULT ON PURPOSE, once, inside a known phase.
//
// The breadcrumb above is only ever read during a crash, so without this it
// ships unverified - and a diagnostic nobody has seen produce output is a
// diagnostic that does not work. This makes the whole path testable in one
// run: the filter fires, the phase reads "the sprite pass", and the frame
// count is non-zero. Default 0, and a +Stub switch, so it cannot persist.
int  g_bCrashTest = 0;

// +StubSkipStill 1: DRAW NOTHING for an instance whose skeleton did not move.
//
// A MEASUREMENT, not a feature - it produces a wrong picture on purpose.
// R3D STILL says 82% of skinned instance-frames have an unchanged skeleton,
// and the obvious response is a per-object vertex cache. That is a large
// change to the hottest loop in this renderer, so the question to answer
// FIRST is how much time such a cache could possibly save: this skips exactly
// the work the cache would skip, so the cost it leaves behind is the floor.
// If the build barely gets cheaper, the cache is not the fix and the cost is
// somewhere else entirely.
int  g_bSkipStill = 0;
long g_nSkippedStill = 0;

// Set lmp.w, the shader's output alpha, for the draws that follow.
//
// A NEGATIVE value means MULTIPLY: the shader fades toward WHITE instead of
// toward transparent and returns before the fog block, because DEST_COLOR x
// ZERO cannot see alpha at all. See the shader.
//
// Writing lmp.w means writing the whole constant buffer, and the alpha-test
// threshold shares it - so the threshold in force is preserved and re-sent
// rather than reset to zero, or a cut-out run would stop being cut out the
// moment something changed the alpha beside it.
static void SetAlphaCut(float f);

// WHICH MAP WAITS. The watchdog shows the main thread inside the driver's
// kernel wait under a d3d11 call, in whichever phase happens to be running;
// a dynamic-buffer Map with DISCARD is the call that blocks when the
// driver's rename pool is spent. Every such Map goes through here, timed,
// and the phase report says per site how many took over 2 ms and the worst.
static double s_fMapMs[3] = { 0, 0, 0 }, s_fMapWorst[3] = { 0, 0, 0 };
static long   s_nMapCalls[3] = { 0, 0, 0 }, s_nMapSlow[3] = { 0, 0, 0 };
// Maps refused because the renderer had no context - see TimedMap.
static unsigned long s_nMapRefused = 0;

static HRESULT TimedMap(ID3D11Resource* pRes, D3D11_MAP mode,
						D3D11_MAPPED_SUBRESOURCE* pOut, int nSite)
{
	// A TORN-DOWN RENDERER HAS NO CONTEXT AND NO BUFFERS.
	//
	// The engine frees and reloads this DLL on every focus loss and gain, and
	// a draw can be in flight across that - the loading screen renders from
	// its own thread, and the main thread can reach here between a teardown
	// and the rebuild. Then g_pCtx or the buffer is null and D3D11 reads
	// through it: crash at "movsx eax,[ecx+0x69]" with ecx zero, which is
	// exactly what three of six missions did on 11 September once focus was
	// being stolen every forty seconds. Every caller already checks the
	// HRESULT, so refusing is a skipped update rather than a crash.
	if (!g_pCtx || !pRes)
	{
		++s_nMapRefused;
		return E_POINTER;
	}
	LARGE_INTEGER a, b, f;
	QueryPerformanceCounter(&a);
	const HRESULT hr = g_pCtx->Map(pRes, 0, mode, 0, pOut);
	QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
	const double ms = f.QuadPart ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
	if (nSite >= 0 && nSite < 3)
	{
		++s_nMapCalls[nSite]; s_fMapMs[nSite] += ms;
		if (ms > s_fMapWorst[nSite]) s_fMapWorst[nSite] = ms;
		if (ms > 2.0) ++s_nMapSlow[nSite];
	}
	return hr;
}

// The light for the run about to be drawn. w = 0 restores the fixed
// directional stand-in, which is what everything that is not a model wants.
static void SetModelLight(float r, float g, float b, float w)
{
	if (!g_pCBMdl) return;
	if (g_fMdlSet[0] == r && g_fMdlSet[1] == g
		&& g_fMdlSet[2] == b && g_fMdlSet[3] == w) return;
	D3D11_MAPPED_SUBRESOURCE ms{};
	if (FAILED(g_pCtx->Map(g_pCBMdl, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
		return;
	const float v[4] = { r, g, b, w };
	memcpy(ms.pData, v, sizeof v);
	g_pCtx->Unmap(g_pCBMdl, 0);
	g_fMdlSet[0] = r; g_fMdlSet[1] = g; g_fMdlSet[2] = b; g_fMdlSet[3] = w;
}
static void SetOutAlpha(float f)
{
	if (f == g_fAlphaOut) return;
	const float fCutHeld = (g_fAlphaCutSet < 0.0f) ? 0.0f : g_fAlphaCutSet;
	g_fAlphaOut = f;
	g_fAlphaCutSet = -1.0f;		// force the write; the value changed
	SetAlphaCut(fCutHeld);
}

static void SetAlphaCut(float f)
{
	if (f == g_fAlphaCutSet) return;
	D3D11_MAPPED_SUBRESOURCE ms{};
	// Record it only once the write is certain, or a failed map would
	// leave the tracker claiming a threshold the buffer does not hold.
	if (FAILED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &ms, 0)))
		return;
	g_fAlphaCutSet = f;
	memcpy(ms.pData, g_fLastMVP, sizeof g_fLastMVP);
	const float lmp[4] = { g_fLMScale, (float)g_bLMOnly, f, g_fAlphaOut };
	memcpy((char*)ms.pData + sizeof g_fLastMVP, lmp, sizeof lmp);
	g_pCtx->Unmap(g_pCB, 0);
}
	int   g_bLMEnable = 1;		// use lightmaps at all
	int   g_nAniso    = 16;		// sampler anisotropy; 0 or 1 = plain bilinear

	// Colour of the stand-in for a polygon with no texture. Most of those are
	// the sky (docs/UNTEXTURED-IS-SKY.md), and pure white blew the top half of
	// every outdoor frame to 255 and washed the rest of the picture with it.
	//
	// Not a guess: sampled off a retail d3d.ren frame of the same quick save -
	// about 700 sky pixels in each of eight bands - averaging R176 G207 B252.
	// Still a STAND-IN: the real sky is a 36-polygon SkyBox drawn through the
	// sky-portal path we do not implement. But a measured flat blue reads as
	// sky, and white reads as a bug.
	//
	// +StubSkyStandIn 0 puts the white back - use that when the question is
	// which surfaces have no texture, not how this looks.
	int   g_bSkyStandIn = 1;

	// What to do with a batch whose texture object exists but whose PIXELS
	// never arrived - ten textures over 1161 polygons, 10.4% of this level.
	// The engine hands pixels over once through BindTexture and frees its own
	// copy; for these it never binds at all, because the retail renderer never
	// draws those surfaces. We draw the whole level, so we meet them, and we
	// were binding a white 1x1 - which is where the slabs of blown-out white
	// across the left wall and the middle of the street came from.
	//
	// This is NOT the same set as the untextured sky: those have no texture
	// object at all, and +StubDrawUntextured 0 leaves these untouched. That
	// was measured - the sky went black and every slab stayed.
	//
	// 0 (default) skips them: a surface we can never texture correctly is
	// better absent than painted flat white over the picture.
	// 1 restores the white, which is what to use to see where they are.
	int   g_bDrawNoPixels = 0;
	const char* g_szLMDump = "";

	std::vector<float> g_Sample;
	const float* g_pSampleVerts = nullptr;
	int          g_nSampleVerts = 0;

	// The second pair is the lightmap, in atlas 0..1. A NEGATIVE u means
	// this polygon has no lightmap - 7278 of the level's 26904 do not -
	// and the pixel shader falls back to the stand-in normal shade for it.
	struct Vtx { float x, y, z; float nx, ny, nz; float u, v; float lu, lv; };

	// One draw per texture. Sorting 47 000 triangles by texture turns the
	// frame into a few hundred draws instead of one per polygon.
	// A batch names the ENGINE's texture object, not a D3D11 view.
	//
	// It used to hold the view, baked at build time, which meant a texture
	// arriving later could only be picked up by rebuilding the whole vertex
	// buffer - 47 000 triangles and 4.4 MB released and recreated. Standing
	// still that never happens; walking through a level the engine streams
	// textures in continuously, so it happened over and over, and a frame that
	// lands while the buffer is gone draws nothing. That is the reported
	// flickering and flashing of all the textures.
	//
	// Keyed by the engine pointer, the geometry never has to be rebuilt: the
	// batch simply starts resolving to a real texture instead of the white
	// stand-in the moment one exists.
	// pTexKey is the ENGINE's texture object. szFile is the level file's
	// own name for the texture, used when the engine never bound one -
	// which is the whole reason parts of the world were missing.
	// bCut: this batch's texture measured as a CUT-OUT - a bimodal alpha
	// channel, mostly fully-clear or fully-opaque texels. Its clear texels are
	// holes and must be discarded, not painted. The model path has consumed
	// this since foliage skins were first alpha tested; the world path never
	// did, which is why every tree in the game was a flat green rectangle.
	struct Batch { uint32_t pTexKey; char szFile[80];
	               ID3D11ShaderResourceView* pSRV;
				   UINT nStart, nCount; bool bSky; bool bTrans;
				   float fCutRef; bool bCut;
				   bool bGraded;		// see TexEntry::bGraded
				   uint32_t pSub;
				   // A SPRITE ON A POLYGON. 1-based SprAnim handle, 0 for a
				   // plain texture. The HQ waterfall is Spr\Spr0028.spr on
				   // the PhysicsBSP, and until this it drew as a white slab.
				   uint32_t nAnim1;
				   // The environment map the texture's command string names,
				   // drawn over the batch after it (see EnvDraw). See DtxInfo.
				   ID3D11ShaderResourceView* pEnvSRV;
				   // ...AND WHETHER IT MOVES. Only WATER pans its map (the HQ
				   // waterfall, the pools); a door or a sign keeps a still
				   // reflection. 13 September, headset testing: all reflective surfaces
				   // flashed when every map panned at the waterfall's rate.
				   bool bEnvPan;
				   // SKY OBJECTS: the pointer's Index decides the order, and
				   // anything that is not the SkyBox itself blends its alpha
				   // (clouds over the box, stars over the clouds).
				   float fSkyOrder; bool bSkyBlend;
				   // ADDITIVE ART IN THE SKY. See DtxInfo::bZeroAlpha: a sky
				   // object whose texture has no alpha and a dark picture is
				   // a glow to be ADDED. Alpha-blended it is invisible;
				   // rewritten opaque, as the zero-alpha rule does, it is a
				   // black square over the stars.
				   bool bSkyAdd;
				   // A SKY BRUSH DRAWN DEPTH-ONLY. See g_bSkyOccluder.
				   bool bOcc; };
	std::vector<Batch> g_Batches;
	// THE WORLD ON THE CPU, FOR A RAY. The client's IntersectSegment reaches
	// world models by their BOX only - the intro's whole street is one
	// Terrain object, 3320 x 800 x 2624 units, and the aim ray "hit" it at
	// its full range with no polygon, so nothing across the street could
	// carry the dot. Every polygon is right here, so R3D_RayCast answers the
	// ray itself: the same vertices the GPU draws, kept after upload, with a
	// box per batch so a ray tests a few hundred triangles, not 47 000.
	std::vector<Vtx>   g_WorldVertsCpu;
	struct BatchBox { float mn[3], mx[3]; };
	std::vector<BatchBox> g_BatchBox;

	// ---- THE SKY --------------------------------------------------------
	//
	// EVERY level has a world model called SkyBox: 36 polygons wearing the
	// panorama - BlueSky_UP/FR/LF/RT/BK on Morocco, Terr1_Day_* at HQ - and
	// until now it was drawn as ordinary geometry, which means as a small
	// closed box sitting somewhere off in the void where nothing can see
	// inside it. That is why the sky reads as flat colour or black in every
	// capture this project has ever taken.
	//
	// A skybox is not geometry in the level, it is the BACKDROP: it draws
	// first, with the camera at its centre so it can never be approached, and
	// with no depth so everything else covers it. The level's sky brushes are
	// holes - their surfaces wear Sky.dtx and the marker rule already drops
	// them - and the backdrop is what shows through the holes.
	//
	// Its polygons are bucketed separately so they cannot mix with world
	// geometry that happens to share a texture, and they are still counted as
	// DRAWN, so the account stays closed.
	int    g_bSkyBox    = 1;            // +StubSkyBox 0 draws it as it was
	bool   g_bHaveSky   = false;
	float  g_fSkyCentre[3] = { 0, 0, 0 };
	int    g_nSkyPolys  = 0;
	// The mesh path's size limit, and how many rejects it has named.
	float  g_fModelDimsMax = 1024.0f;
	long   g_nDimsSaid = 0;
	int    g_nSkyDump = 0;
	long   g_nSkyDrawn  = 0;
	ID3D11DepthStencilState* g_pDSSNoDepth = nullptr;

	// ---- GLASS ----------------------------------------------------------
	//
	// A `TranslucentWorldModel` is what NOLF builds a window, a glass partition
	// or a fenced gate out of, and this renderer has always drawn them OPAQUE.
	// In the HQ lobby that puts a solid slab across the glass double doors where
	// retail shows the receptionist through them; in Morocco it turns a barred
	// gate into a wall.
	//
	// They are drawn LAST, after the opaque world and after the models, with
	// alpha blending on and depth WRITE off - the standard ordering, so what is
	// behind the glass has already been drawn and shows through it.
	//
	// The alpha is a constant for now. NOLF authors it per object and that
	// value lives on the entity, not
	// on the texture - the glass here wears ordinary sign and metal textures, so
	// there is no alpha channel to read. +StubTransAlpha <0-100> until the real
	// per-object value is bridged across from the client.
	ID3D11BlendState*        g_pBlendAlpha = nullptr;
	// ONE + ONE. An additive overlay adds its light to what is already
	// there and never darkens it, which is why dark art is the right
	// art for one - and why drawing it with alpha makes a black blob.
	ID3D11BlendState*        g_pBlendAdd = nullptr;
	ID3D11BlendState*        g_pBlendMod2x = nullptr;
	// Additive scaled by the source alpha, for SPRITES. See the note where it
	// is created. +StubSprAddMod 0 uses the flat ONE/ONE state instead.
	ID3D11BlendState*        g_pBlendAddMod = nullptr;
	// DEST x SRC, for FLAG2_MULTIPLY. The mirror image of additive: additive
	// art is a glow on BLACK and adds nothing where it is black; multiply art
	// is a mask on WHITE and changes nothing where it is white. Bullet marks
	// and blood splats are all of this kind, and their textures carry no alpha
	// at all - BLOODL3.DTX is alpha 0 on all 4096 texels - so alpha blending
	// cannot rescue them. Drawn any other way they are white squares.
	ID3D11BlendState*        g_pBlendMul = nullptr;
	// +StubMulBlend 0 draws multiply objects the old way: alpha-blended for
	// SPRITES, opaque for MODELS - which is what each of them was before its
	// own fix, and so is the control arm for both.
	int                      g_bMulBlend = 1;
	// +StubMulForce 1: draw EVERY model run multiply, whatever it is flagged.
	//
	// A DIAGNOSTIC, and the only way to test this path without a headset and a
	// firefight. Multiply on models fires around DEBRIS - seven FX records in
	// the whole game - so an ordinary capture contains none of it, and a switch
	// that cannot change the picture proves nothing about the plumbing. Forced,
	// every model in the scene multiplies into what is behind it: characters go
	// dark, the world through them does not move, and the state, the negative
	// output alpha and the pass ordering are all exercised at once.
	int                      g_bMulForce = 0;
	// RUNS ACTUALLY DRAWN MULTIPLY THIS FRAME, as against instances FLAGGED
	// for it. The two answer different questions and only the second was ever
	// counted: a flag can be published, counted, and still never reach a draw
	// call - which is precisely the state this path was in until now.
	long                     g_nMeshMulDrawn = 0;
	int                      g_bSprAddMod = 1;
	ID3D11DepthStencilState* g_pDSSNoWrite = nullptr;
	// For a SECOND pass over geometry already drawn: test, do not write, and
	// let EQUAL through. g_pDSSNoWrite is strict LESS, which a second pass
	// over its own surface fails outright. See EnvDraw.
	ID3D11DepthStencilState* g_pDSSEnv = nullptr;
	int g_bEnvDepth = 1;
	// Polygrids drawn before the glass rather than after it. See the glass pass.
	int g_bWaterFirst = 1;
	float g_fTransAlpha = 0.45f;
	int   g_bTransNames = 1;	// the widened see-through name rule
	int   g_nWorldRebuildMs = 0;	// rebuild the world mesh on a timer

	// ---- WORLD MODELS THAT MOVE ----------------------------------------
	//
	// The client publishes every OT_WORLDMODEL's live position and rotation.
	// The renderer knows world models as heap structs. Joining the two needs an
	// offset inside the HOBJECT that points at the WorldModel - the same shape
	// of question as the model pointer at +0x1DC, and answered the same way: by
	// counting, over every published object, how often each offset lands on a
	// pointer this renderer already knows to be a world model.
	VRWorldFrame g_World;
	int  g_bHaveWorldObjs = 0;
	int  g_bWorldProbe = 0;			// +StubWorldProbe 1
	std::vector<uint32_t> g_SubPtrs;	// every world model struct, this build
	long g_aWObjHit[256] = { 0 };	// offset -> how many objects pointed at one
	long g_nWObjSeen = 0;
	int  g_bWObjSaid = 0;

	// ---- SPRITES -------------------------------------------------------
	//
	// Every effect in the game, and the whole main menu background. A sprite is
	// a camera-facing textured quad; the only unknown is where the object keeps
	// its texture, and that is found the same way everything else here was -
	// walk the offsets over every sprite object and count how often the word
	// there resolves to a texture this renderer already knows.
	VRSpriteFrame g_Spr;
	// The effects channel - particles, rain, water, canvases. See VRShared.h.
	VRPrimFrame g_Prim;
	// The dynamic lights, this frame. See VRLightFrame.
	VRLightFrame g_DynL;
	int  g_bHaveDynL = 0;
	int  g_bDynLights = 1;			// +StubDynLights 0
	ID3D11Buffer* g_pCBDyn = nullptr;	// b4: the eight nearest, per pass
	long g_nDynWorldLit = 0;
	int  g_bHavePrims = 0;
	int  g_bPrims = 1;				// +StubPrims 0
	double g_fNowSec = 0.0;			// the pass's clock, for animated textures
	long g_nPrimRunsDrawn = 0, g_nPrimVertsDrawn = 0, g_nPrimNoTex = 0;
	int  g_bHaveSprites = 0;
	int  g_bSpriteProbe = 0;
	long g_aSprHit[256][2] = { { 0 } };
	long g_nSprSeen = 0;
	int  g_bSprSaid = 0;
	int  g_bSprites = 1;			// +StubSprites 0
	float g_fSpriteScale = 1.0f;	// +StubSpriteScale100
	ID3D11Buffer* g_pSprVB = nullptr;
	long g_nSprDrawn = 0;


	// WHERE EACH WORLD MODEL WAS WHEN THE LEVEL LOADED.
	//
	// The mesh is built from vertices authored in world space, so it is correct
	// for exactly one transform: the one the object had at load. To draw a door
	// that has since swung, its vertices have to be taken back into that frame
	// and forward into the current one:
	//
	//     v' = R1 * (R0^T * (v - P0)) + P1
	//
	// P0/R0 is the baseline. Nothing has moved at load, so the FIRST transform
	// the client publishes for an object is the authored one - the same reason
	// the client's own VRWorldMove counter uses first-seen as its origin.
	// HOBJECT + 0x1A8 is the WorldModel struct; measured on 280 of 280 objects.
	const uint32_t kObjToWorldModel = 0x1A8;
	// nFlags is the engine's own flag word for the brush, carried since
	// world publish version 2. Until then this renderer had no way to know
	// the engine had stopped drawing something.
	struct WMBase { uint32_t pSub; float p[3]; float r[9];
					float lp[3]; float lr[9]; int bMoved; float fAlpha;
					uint32_t nFlags;
					// Publishes in a row this model was NOT in. See the sweep at
					// the end of R3D_PublishWorldModels: a world model the engine
					// has destroyed stops being published, and until this it was
					// drawn at its last pose forever - the aeroplane door, blown
					// out and hanging in the sky.
					int nMissing; int bGone; };
	// The client's VRWORLD_F_INVISIBLE, spelled locally so this file does
	// not have to include the client header.
	const uint32_t VRWORLD_F_INVIS = (1u << 0);
	// The client's VRWORLD_F_FOGDISABLE: the object's FLAG_FOGDISABLE, a
	// TranslucentWorldModel's FogDisable property. The retail renderer draws a
	// sky object that carries it with fog OFF (its d3d_DrawSkyObjects tests
	// FLAG_FOGDISABLE per object), which is how a level keeps its cloud sheet
	// and sun clear while the sky fog sinks the panorama walls.
	const uint32_t VRWORLD_F_FOGOFF = (1u << 1);
	// A WORLD MODEL THE ENGINE HAS HIDDEN IS NOT DRAWN. The client publishes
	// FLAG_VISIBLE for every world model and the renderer only ever COUNTED
	// the clear ones ("HIDDEN AND WE STILL DRAW: 10 of 80" in a run 8:
	// TranslucentWorldModel14..19, mapholethis). Those are the shattered
	// glass panes and the blast holes - NOLF hides a breakable brush when it
	// breaks - and every one stayed on screen. +StubDrawHiddenWM 1 puts them
	// back for an A/B; the per-window report counts what was left out.
	int  g_bDrawHiddenWM = 0;
	long g_nHiddenWMSkipped = 0;
	// A WORLD MODEL WITH NO ENGINE OBJECT IS NOT DRAWN EITHER - unless it is
	// a volume brush. The level file declares far more world models than the
	// client has objects for, and the renderer drew every one "straight from
	// the file", which put back the objects ObjectRemover had deleted at
	// level start (charges, barrels, switches, chemicals - see
	// WorldFile::Classes). Volume brushes (Water, Lava, Ladder...) exist as
	// containers the sphere search cannot see, so they keep drawing until
	// retail says otherwise. +StubDrawUnmatchedWM 1 draws them all again.
	int  g_bDrawUnmatchedWM = 0;
	long g_nUnmatchedWMSkipped = 0;
	std::map<uint32_t, int> g_UnmatchedKind;		// pSub -> 1 volume (draw), 2 removed (skip)
	// THE CLASSES THE CLIENT CANNOT SEE BUT RETAIL DRAWS. tools/unmatched-
	// classes.py over the night sweep of 9 September: every unmatched world
	// model in the game is a volume brush (Water, Weather, SafteyNet, Ladder,
	// VolumeBrush, EndlessFall, CorrosiveFluid, FreezingWater, Electricity,
	// Filter - 138 of them), an engine TERRAIN object (31: the cliffs of
	// 07HYDRO_AM, Canopy, Dome, Chevelle_Body, EggChair, grave), or a Door,
	// Switch, TranslucentWorldModel or RotatingWorldModel (52) - and those
	// last are proper game objects the client always has when they exist,
	// so their absence is ObjectRemover. Terrain and the volumes are drawn;
	// the rest are not. The desk proved the Terrain half: with it on the
	// skip side the hydro dam lost both its cliffs.
	bool IsVolumeClass(const std::string& s)
	{
		static const char* kVol[] = { "Water", "Ice", "CorrosiveFluid", "FreezingWater",
			"PoisonGas", "Electricity", "EndlessFall", "Wind", "ColdAir", "Filter",
			"SafteyNet", "SafetyNet", "Ladder", "Weather", "VolumeBrush", "Terrain", 0 };
		for (int i = 0; kVol[i]; ++i) if (s == kVol[i]) return true;
		return false;
	}
	// THE VOLUMES THAT SHOW BY DEFAULT. ObjectDLL/VolumeBrushTypes.cpp: the
	// base VolumeBrush and the four liquids leave ShowSurface at its TRUE
	// default; every other volume class hides the property and sets it
	// FALSE. Used only when an object's record did not carry the property.
	bool IsShownVolumeClass(const std::string& s)
	{
		static const char* kShown[] = { "Water", "Ice", "CorrosiveFluid",
			"FreezingWater", "VolumeBrush", "Terrain", 0 };
		for (int i = 0; kShown[i]; ++i) if (s == kShown[i]) return true;
		return false;
	}
	// +StubVolumeSurface 0 draws every volume class as before (the 9-12
	// September rule); 1 asks each object's ShowSurface.
	int  g_bVolumeSurface = 1;
	// +StubVolumeVisible 0: a water volume with a surface object draws its brush
	// too, as before 22 September. See UnmatchedKindOf.
	int   g_nSkyTrace = 0; int g_nSkyTraceSaid = 0;
	char  g_szBatchWatch[64] = ""; int g_nBatchWatchSaid = 0;
	char  g_szModelWatch[64] = ""; int g_nModelWatchSaid = 0; int g_nModelHeadSaid = 0;
	int   g_bGlassDepth = 1;		// +StubGlassDepth 0: see-through world surfaces write no depth at all, as before 22 September
	float g_fGlassDepthCut = 0.9f;	// texel alpha from which a see-through surface also writes depth (+StubGlassDepthCut100)
	long  g_nGlassDepthDraws = 0;
	int   g_bSkyParallax = 1;	// +StubSkyParallax 0: the sky camera stays at the view box centre
	int   g_bWorldExt = 0; float g_fWorldExtMin[3], g_fWorldExtMax[3];	// the level's merged BSP extents
	int  g_bSkyCamCentre = 0;	// +StubSkyCamCentre 1: the sky camera at the sky box's centre, not the level's sky pointer
	int  g_bVolumeVisible = 0;	// 0 by default; 2 keyframed volumes only; 1 all. See the rule.
	// SKY BATCHES STAY OUT OF THE SEE-THROUGH PASS (default 0 since 24 Sep).
	// The backdrop pass already draws every sky batch, graded ones included,
	// so a sky batch here is a duplicate at the sky model's place in the
	// world. On T08S01 that is a translucent blue slab across the canyon
	// (off removes it, desk dumps). M05S05, T01S02, M12S01 A/B: nothing but
	// steam, snow and subtitles differ. It did NOT cure the M05S05 horizon
	// strip (22 Sep). +StubSkyInGlass 1 draws them again.
	int  g_bSkyInGlass = 0;
	long g_nSkyGlassSkipped = 0;
	// A WORLD POLYGON WITH NO LIGHTMAP IS LIT BY ITS VERTEX COLOUR, the way
	// retail lit it. The 24-byte vertex entry carries a 32-bit colour at +20
	// that the level processor filled in: white on a BRIGHT (0x20) surface,
	// the gouraud light on a GOURAUDSHADE (0x1000) one - the aeroplane's
	// glass carries FFEBE8EF / FFFDF3F4 on one polygon. The light grid was
	// a stand-in invented before that byte was read, and its coarse coloured
	// cells are the aeroplane cabin's rainbow, the Alps' snow and the
	// chateau's rubble. +StubVertexColour 0 goes back to the grid.
	int  g_bVertexColour = 1;
	long g_nUnmDrawn = 0, g_nUnmInvisible = 0, g_nUnmRemoved = 0;
	std::string g_sUnmInvisible;	// the invisible volumes this world, by name
	// 1 = a volume brush, drawn; 2 = no object and not a volume, skipped.
	int UnmatchedKindOf(uint32_t pSub);
	std::vector<WMBase> g_WMBase;
	int  g_bWorldXform = 1;			// +StubWorldXform 0 freezes them again
	long g_nWMMoved = 0;			// how many are away from their baseline
	// +StubWorldSeed 0 puts the first-seen baseline back. See the note at
	// WorldModelFile::fTrans.
	int  g_bWorldSeed = 1;
	long g_nWMSeededAway = 0;		// first seen away from the authored place
	bool g_bDtxFlushWanted = false;	// set by R3D_WorldLoaded, done by the walk
	bool g_bMeshInvalidate = false;	// set by the walk: the model runs are the old world's
	long g_nWMDrawn = 0;
	float g_fWorldNudge = 0.0f;	// forced offset, to prove the transform

	// A batch is glass if its world model says so. No name, no texture guess:
	// the number the level author set.
	//
	// EXACTLY 1.0 IS THE TEST, and the measurement is why. On the HQ level the
	// only world models whose alpha is not 1.0 are Glass4 .. Glass16, and every
	// one of them reads 0.988. That is not a strength - real glass would be 0.3
	// to 0.6 - it is a MARK: the author touched the alpha, and the engine takes
	// it as "render this translucent". Vending machines, benches and drinking
	// fountains are all exactly 1.0, which is why they have to go back to being
	// solid. Guessing from the model NAME made a drinking fountain see-through
	// because one of them is called Water_Fountain.
	//
	// So the alpha CLASSIFIES and StubTransAlpha decides how thin it looks -
	// unless the author set something genuinely low, in which case theirs wins.
	const float kOpaqueAlpha = 1.0f;
	float GlassAlphaOf(float fAuthored)
	{
		return (fAuthored < 0.9f) ? fAuthored : g_fTransAlpha;
	}


	// Defined further down; needed by HiddenByName below.
	void ModelName(uint32_t sub, char* pszOut, int nOut);

	const WMBase* WMBaseFor(uint32_t pSub)
	{
		for (size_t i = 0; i < g_WMBase.size(); ++i)
			if (g_WMBase[i].pSub == pSub) return &g_WMBase[i];
		return nullptr;
	}

	// IS A BRUSH WE COULD NOT MATCH NAMED THE SAME AS ONE THE ENGINE HID?
	//
	// The hidden test in the draw loop needs a matched engine object, and when
	// the match fails the "no engine object -> terrain, draw it" rule takes
	// over and puts the brush back. For Morocco's canopy that is not a
	// cosmetic difference: the level carries TWO brushes, Canopy_a and
	// Canopy_b, with byte-identical bounds (-592 256 -320 .. -432 320 0) and
	// the same texture, and the client publishes BOTH as hidden. Drawing them
	// puts two coincident opaque surfaces in the same place, which z-fights -
	// and a z-fight flips with the camera, so it flashes in motion and looks
	// perfectly fine in a screenshot. Headset testing, 11 September, on the
	// sniper roof: the texture still flashed and looked like it was glitching
	// out, and a single screenshot could not capture it.
	//
	// Matching by NAME is the fallback the pointer match lacks. It is only
	// consulted for brushes that failed the pointer match, so nothing that
	// matches properly changes behaviour.
	bool HiddenByName(uint32_t pSub)
	{
		char szWant[64] = "";
		ModelName(pSub, szWant, sizeof szWant);
		if (!szWant[0]) return false;
		// A HIDDEN SECTION IS NOT A HIDDEN PARENT, and reading it as one
		// DELETED THE MOROCCO CANOPY. Retail draws a green and cream striped
		// barrel-vault awning over the hotel terrace; with the prefix-stripping
		// version of this loop we drew its wooden posts and beams and no fabric
		// at all. a side-by-side screenshots, 12 September, are what
		// caught it - no flicker measurement ever could have, because a missing
		// surface does not flicker.
		//
		// What went wrong: the engine mangles a world model's SECTIONS as
		// "#$#0 <name>", and this used to compare only the part after the last
		// space so that "#$#0 Canopy_b" matched "Canopy_b". But the sections
		// are a different thing from the parent, and this file already knew it
		// - see SkippedModel, which drops every "#$#" name and whose own note
		// reads "The parent is drawn, whatever the client says; the sections
		// never are." The client publishes the SECTIONS hidden, as it always
		// has; the parent is what gets drawn.
		//
		// In M01S01 the hidden list is: #$#0 Dome1, #$#0 Canopy_b,
		// #$#0 Canopy_a, powerline3, Blocker10. The first three are sections
		// and must not hide anything; the last two are genuinely invisible
		// objects named plainly, and an exact compare still keeps them out -
		// which is the whole reason this rule exists.
		static int s_nSaid = 0;
		for (size_t i = 0; i < g_WMBase.size(); ++i)
		{
			if (!(g_WMBase[i].nFlags & VRWORLD_F_INVIS)) continue;
			char szHave[64] = "";
			ModelName(g_WMBase[i].pSub, szHave, sizeof szHave);
			if (!szHave[0]) continue;
			// EXACT. No prefix stripping - that is the bug above.
			if (strcmp(szHave, szWant) != 0) continue;
			if (s_nSaid < 8)
			{
				++s_nSaid;
				Log("  HIDDEN BRUSH KEPT OUT: '%s' was published hidden as '%s'"
					" but its batch matched no engine object, so the"
					" terrain rule was about to draw it", szWant, szHave);
			}
			return true;
		}
		return false;
	}
	// lmp.w, the pixel shader's output alpha. 1 for everything opaque; the glass
	// pass sets it and puts it back. It was an unused zero in the constant
	// buffer, which is why nothing else had to change.
	int   g_nTransPolys = 0;
	long  g_nTransDrawn = 0;

	// pData and the dimensions are recorded so a RECYCLED texture object can be
	// noticed. The cache is keyed on the engine's texture-object pointer; when
	// the engine frees a texture and its allocator hands the same address back
	// for a different one - which is what walking through a level does - a
	// cache keyed on the pointer alone keeps serving the OLD pixels, and the
	// surfaces using it change image as the churn continues.
	// mr/mg/mb is the texture's mean colour, taken at UPLOAD time while we
	// still have the engine's pixels. Reading it back off the GPU instead
	// waits for the device on the render thread, which froze the game once
	// already (+StubProbeDump). A surface drawing as a flat pale slab is
	// either a pale texture or a broken mapping, and nothing in the renderer
	// could tell those apart.
	// fCut is the fraction of sampled texels that are TRANSPARENT, and bCut
	// says the texture is a cut-out worth alpha-testing. Plenty of opaque
	// textures store alpha 0 everywhere because nothing reads it, so an
	// unconditional alpha test would erase them - hence a band, not > 0.
	// pPix and nSig are the IDENTITY OF THE IMAGE, not of the allocation.
	// Keying on pTex alone is what put the monkey seller in Cate's clothes:
	// a reload frees every texture and the allocator hands the same addresses
	// back for different pictures. pData was the first attempt at telling
	// them apart and it is not enough on its own, because the data struct is
	// recycled too. pPix is the mip-0 pixel pointer and nSig a cheap hash of
	// the first bytes at it, so the check is about the picture.
	// szName is the FILE this picture came from, when the name probe could
	// recover it. Empty means the route has not been found for this texture,
	// not that it has none.
	struct TexEntry { uint32_t pTex; uint32_t pData; ID3D11ShaderResourceView* pSRV;
					  float fW, fH; float mr, mg, mb; float fCut; float fBi; uint32_t nUVShift;
					  // fCutRef is the alpha threshold to DRAW with, 0..1, and
					  // 0 means no test. The file's own "alpharef 96;" when it
					  // declares one; the measured fallback's 0.5 when it does
					  // not. bCut is just (fCutRef > 0), kept for the logs.
					  float fCutRef;
					  int bCut; int bOccl;
					  // The alpha channel is a GRADED FILM, not a cut-out: glass
					  // and water. Drawn blended with its own alpha, last.
					  int bGraded;
					  // The file's alpha channel was entirely zero before the
					  // zero-alpha rule rewrote it opaque. With a dark picture
					  // that means additive art - see DtxInfo::bZeroAlpha.
					  uint32_t bZeroAlpha;
					  uint32_t pPix; uint32_t nSig; long nLoad; char szName[80];
					  // A SPRITE-NAMED ENGINE TEXTURE: 1-based SprAnim handle,
					  // 0 for a plain .dtx. See TexNameOf.
					  uint32_t nAnim1; };
	// What the per-frame skin check found. Split by WHICH field moved, because
	// that is the whole question: if only nSig moves, the engine reuses the
	// object and its data and merely refills the pixels, and no pointer
	// comparison could ever have caught it.
	long g_nSkinIdChecked = 0, g_nSkinIdData = 0, g_nSkinIdPix = 0, g_nSkinIdSig = 0;
	// The DENOMINATORS. "0 checked" could mean the slot is empty, the cache
	// has no entry for it, or its data cannot be read, and one number cannot
	// tell those apart - which is the mistake that made the first run of this
	// instrument useless.
	long g_nSkinIdSlots = 0, g_nSkinIdCached = 0, g_nSkinIdLive = 0;
	long g_nSkinIdOld = 0;
	int  g_nSkinIdSaid = 0;
	long g_aSkinOff[64] = { 0 };
	long g_nSkinScan = 0;
	int  g_nSkinOffSaid = 0;
	long g_nTexRecycled = 0;
	int  g_nDumpNext = 0;	// how far R3D_DumpNextTexture has walked
	int  g_nDumpMin  = 256;	// smallest edge the dump bothers with
	std::vector<TexEntry> g_Tex;
	// Bumped on EVERY change to g_Tex (clear, erase, push_back), so an index
	// built from it knows when it is stale - including an erase followed by a
	// push_back, which leaves the size unchanged and shifts every index.
	uint32_t g_nTexListGen = 1;
	// pSRV -> index into g_Tex, first match, as the linear scans returned it.
	// The mesh build asked this once per PIECE by scanning the whole list; on
	// a busy level that was a large share of each rebuilt instance's cost.
	int TexIndexOfSRV(ID3D11ShaderResourceView* p)
	{
		static std::unordered_map<ID3D11ShaderResourceView*, int> s_Idx;
		static uint32_t s_nGen = 0;
		if (s_nGen != g_nTexListGen)
		{
			s_Idx.clear();
			for (size_t i = 0; i < g_Tex.size(); ++i)
				if (g_Tex[i].pSRV) s_Idx.emplace(g_Tex[i].pSRV, (int)i);
			s_nGen = g_nTexListGen;
		}
		if (!p) return -1;
		std::unordered_map<ID3D11ShaderResourceView*, int>::const_iterator it = s_Idx.find(p);
		return (it == s_Idx.end()) ? -1 : it->second;
	}

	// Big polygons and how magnified their texture is - the slab hunt.
	struct BigPoly { uint32_t tex; float tw, th, diag, du, dv, tpu, cx, cy, cz; };
	std::vector<BigPoly> g_BigPolys;
	ID3D11SamplerState* g_pSamp = nullptr;
	ID3D11ShaderResourceView* g_pWhite = nullptr;

	// The engine's structures. Offsets from docs/GEOMETRY-FOUND.md; nothing
	// here is a guess and nothing here is written to.
	// The engine's own bounds, and they are EXACTLY the file's second box -
	// M01S02 reads min (-5248 -1856 -80) max (6528 80 9936) in both, to the
	// bit. That is what makes identifying the world file a decisive test
	// rather than a plausible one. docs/WORLD-NAMED.md checked these three
	// against each other: centre must be the average of min and max.
	const uint32_t kWorldBoxMin     = 0x90;		// float3
	const uint32_t kWorldBoxMax     = 0x9C;		// float3
	const uint32_t kWorldModels     = 0x18C;	// -> array of element pointers
	const uint32_t kWorldModelCount = 0x190;
	const uint32_t kSubFromElement  = 0x04;		// element -> the WorldModel
	const uint32_t kNameInSub       = 0x04;		// inline string
	const uint32_t kPolyArray       = 0xA0;
	const uint32_t kPolyCount       = 0xA4;
	const uint32_t kVertArray       = 0xA8;
	const uint32_t kVertCount       = 0xAC;
	const uint32_t kPolyVertCount   = 0x50;		// uint16
	const uint32_t kPolyVertList    = 0x54;		// entries of 24 bytes
	const uint32_t kPolyVertStride  = 24;
	// polygon + 0x28 -> a 72-byte surface record; surface + 0x30 -> the very
	// texture object the engine hands to BindTexture. Both measured.
	const uint32_t kPolySurface     = 0x28;
	const uint32_t kSurfaceTexture  = 0x30;
	const uint32_t kSurfaceFlags2   = 0x38;	// see lightmap.cpp; top 3 bits axis

	// One row per distinct surface-flags value seen while building.
	// A flag value plus THE TEXTURES THAT WEAR IT. The size alone could not
	// answer the question: 24453 of m01s02's polygons share one value and
	// several sizes, so "several texture sizes" is where every interesting
	// row landed. A NAME is the identity stage 1 gave us, and AI.dtx,
	// Occluder.dtx, INVISIBLE and HULLMAKER name themselves - so a value
	// worn only by those is a marker value, read rather than guessed.
	//
	// Four names, because the question is whether the set is small and
	// marker-only; a value worn by five different textures has answered it.
	// THE SPLIT IS THE POINT. A flag value is only usable as a do-not-draw
	// rule if it appears on marker models AND NOWHERE ELSE, and the only
	// way to say that is to count both sides of the same value.
	//
	// Marker means what SkippedModel means: PhysicsBSP, AIVolume*,
	// blocker*. Those names are the CURRENT rule, so this measures a
	// candidate flag rule against the rule it would replace.
	struct FlagRow { uint32_t nFlags; int nPolys; float fW, fH;
					 char szTex[4][64]; int nNames;
				 int nMarker, nTrans, nOther; };
	std::vector<FlagRow> g_FlagHist;
	int  g_bFlagHist = 0;
	uint32_t g_nSkipFlags = 0;	// polygons ANDing with this are not drawn
	const uint32_t kTexWidth        = 0x10;		// uint16
	const uint32_t kTexHeight       = 0x12;		// uint16
	const uint32_t kTexMip0         = 0xCC;		// {w, h, pixels, pitch}

	// A texture's size is in the engine's own object and is readable whether or
	// not we have made a D3D11 texture from it yet. Taking the UV scale from
	// here rather than from our cache is what lets a polygon be bucketed with
	// its true texture on the very first build.
	bool TexDims(uint32_t pTex, float* pW, float* pH);


	// The name a WorldModel carries, as a plain string.
	void ModelName(uint32_t sub, char* pszOut, int nOut)
	{
		const char* q = (const char*)(uintptr_t)(sub + kNameInSub);
		int c = 0;
		while (c < nOut - 1 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126)
		{ pszOut[c] = q[c]; ++c; }
		pszOut[c] = 0;
	}

	// Geometry the engine never shows: the collision twin of the level, and
	// the AI volumes DEdit writes into the world. See the long note at the
	// call site in R3D_BuildWorld for how these were found and why the test is
	// a name rather than a flag.
	//
	// ONE rule in ONE place, because the ray probe has to apply exactly the
	// same one. The first version of this skipped them in the build only, and
	// the probe - which walks the world itself - went on naming the polygon
	// under the player's feet as an AI volume that was no longer being drawn.
	// An instrument that disagrees with the renderer cannot verify it.
	// A DIAGNOSTIC arm, not a fix: drops the 96 TranslucentWorldModels (4030
	// polygons). They are windows, light shafts and glass, and we draw them
	// opaque - which is the leading suspect for the white slabs washing out
	// the left wall and the middle of the street. +StubSkipTranslucent 1.
	int  g_bSkipTranslucent = 0;
	int  g_bModelMesh = 1;		// draw the real mesh, not the stand-in box
	// +StubModelOnly 1: suppress the WORLD and draw only the model meshes, so
	// that "the models produce no pixels" and "the models are hidden behind
	// something" stop looking identical. A diagnostic, never a default.
	int  g_bModelOnly = 0;
	// AN INTERFACE-ONLY FOLDER IS UP. The mission status, summary, briefing,
	// objectives, inventory and awards screens are full-card screens: retail
	// draws them through the interface camera at the origin, card art and
	// all, and never shows the world behind them. The pause menu is the same
	// engine state but keeps the world, by this port's own choice. The client
	// says which is which, per frame, through R3D_PublishInterfaceOnly.
	int  g_bInterfaceOnly = 0;
	int  g_bMipOffsetUV = 1;	// +StubMipOffsetUV 0: world UVs by the header size, as before
	// THE SKY BOXES ARE LEFT AS THEY WERE until seen against retail: 30+ levels
	// carry offset sky faces (TRAIN1, TERR1_DAY, BLUESKY, FLIGHT1, SNOW_HAZY),
	// among them skies already confirmed in the headset. +StubMipOffsetSky 1.
	int  g_bMipOffsetSky = 0;
	long g_nMipOffsetUV = 0;	// world polygons whose UVs took a mipmap offset
	// Draw the untextured mesh in the BOX colour, not the untextured fallback.
	// The fallback is the sky stand-in, so a character drew as sky-coloured
	// against sky. +StubMeshTint 0 goes back to it.
	// The mesh is drawn in runs that share a texture. Kept across the two eye
	// passes with the vertex buffer, because the second eye redraws what the
	// first built.
	// bCut: this run's skin is a cut-out, so its clear texels must be
	// discarded rather than drawn. Foliage was drawing its mask as solid
	// colour - the black wedges around the bushes.
	// nBlend: 0 opaque, 1 ADDITIVE, 2 MULTIPLY - the same three the sprite
	// pass uses, spelled the same way. It was a bAdd bool, and a bool is why
	// FLAG2_MULTIPLY sat published-and-unread on this path for as long as it
	// did: there was nowhere to put a third answer.
	struct MeshRun { UINT nStart, nCount; ID3D11ShaderResourceView* pSRV;
					 float fCutRef; int nBlend;
					 float fA;		// the object's alpha, for nBlend 3
					 // THE SCOPE NEVER SEES THE GUN IT SITS ON. Its pass looks out
					 // from the objective lens, inside the scope's own tube: drawn,
					 // the tube walls filled the picture and the eyepiece showed black.
					 // The runs are built ONCE a frame and reused by every pass, so the
					 // gun cannot be left out of the build (the eyes lost it, headset
					 // 21 September); each run says whose it is and the scope pass
					 // skips the view model's at draw time.
					 int nView;
					 // The light this instance stands in. Part of the coalesce
					 // key: two models sharing a skin but standing in different
					 // rooms are not one run.
					 float fLight[3];
					 // WHICH BUFFER the run's nStart indexes: 0 the per-frame
					 // ring (g_pMeshVB, at this frame's region), 1 the resident
					 // pool (g_pPoolVB) that unchanged instances are drawn from.
					 int nVB; };
	std::vector<MeshRun> g_MeshRuns;

	// ---- THE MODEL LIGHT GRID ------------------------------------------
	//
	// WHY THIS EXISTS. The world is lit by its own lightmaps and always has
	// been. MODELS are not lit by anything: the shader gives every one of them
	// a fixed directional stand-in, 0.55 to 1.0 of grey by surface angle, so a
	// character is exactly as bright in a dark cellar as in a sunlit street and
	// takes no colour from either. Retail's Morocco lobby lights the man in the
	// white suit warm; ours draws him the same white he would be anywhere. That
	// is most of the reported impression that the levels feel bright and unlit.
	//
	// NOLF's own lighting is not available to ask: 0 dynamic lights among 343
	// objects, because the level's light is baked. So it is read back OUT of
	// the bake. Every world polygon that has a lightmap contributes its average
	// colour to a coarse voxel grid at its centroid, once, at world build; a
	// model then looks up the cell it stands in. That is an irradiance grid,
	// and it is what the engine itself does - the level's own properties string
	// says `LMGridSize 24`.
	//
	// Coarse on purpose. This is the light in a ROOM, not on a surface: a model
	// wants the room's colour and level, and sampling it finely would only make
	// a character flicker as it walks over a lightmap seam.
	struct LightGrid
	{
		float    fMin[3];
		int      nDim[3];
		float    fCell;
		std::vector<float>    acc;	// 3 per cell, summed
		std::vector<uint32_t> cnt;
		bool     bBuilt;
	};
	LightGrid g_LGrid = {};
	// TERRAIN TAKES THE LEVEL'S LIGHT. 12 September: headset screenshots of
	// the Siberian intro and the Moroccan desert at night were two to three
	// times brighter than retail's. Fog was right and the lightmaps were
	// dark; what was bright was the GROUND. A Terrain world model is drawn
	// from the level file and carries no lightmap - the desert's has 2453
	// polygons, the intro's snowfield is 30 huge ones - so it took the
	// stand-in shade of 0.55..1.0, which is daylight in a level whose light
	// averages 0.2. The same grid that lights models is uploaded as a small
	// 3D texture and sampled PER PIXEL for every world polygon with no
	// lightmap, so a 4000-unit polygon is lit across its face and not from
	// its corners. +StubTerrainGrid 0 puts the stand-in back.
	int   g_bTerrainGrid = 1;

	// SKY BRUSHES ARE OCCLUDERS. 12 September: headset screenshots of the
	// aeroplane exterior showed the cabin interior hanging in the sky beside
	// the plane, and two characters standing in mid-air. Retail never draws
	// that: its visibility system treats a sky-textured wall as solid, and
	// the sky is painted INTO those polygons, opaque. We drop sky brushes as
	// holes, paint the backdrop with no depth, and draw every polygon in the
	// world - so whatever the level keeps behind a sky wall shows through.
	// Now the sky-textured polygons are kept and drawn right after the
	// backdrop with depth WRITTEN and colour MASKED: the picture is still the
	// backdrop, but anything behind the wall fails the depth test, which is
	// retail's behaviour without its PVS. +StubSkyOccluder 0 reverts.
	int   g_bSkyOccluder = 1;
	long  g_nSkyOccPolys = 0;
	ID3D11BlendState* g_pBlendNoColour = nullptr;
	static void PushOccluderFan(std::vector<Vtx>& bk, float (*p)[3], uint32_t nv)
	{
		if (nv < 3 || nv > 256) return;
		float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
		float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
		float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
		const float len = sqrtf(nx * nx + ny * ny + nz * nz);
		if (len < 1e-4f) return;
		nx /= len; ny /= len; nz /= len;
		for (uint32_t t = 1; t + 1 < nv; ++t)
		{
			const uint32_t idx[3] = { 0, t, t + 1 };
			for (int k = 0; k < 3; ++k)
			{
				Vtx v;
				v.x = p[idx[k]][0]; v.y = p[idx[k]][1]; v.z = p[idx[k]][2];
				v.nx = nx; v.ny = ny; v.nz = nz;
				v.u = v.v = 0.0f;
				v.lu = -2.0f; v.lv = -1.0f;		// unlit; the colour is masked anyway
				bk.push_back(v);
			}
		}
		++g_nSkyOccPolys;
	}

	// SKINS BY NAME, FROM THE CLIENT THAT CREATED THE OBJECT. 12 September:
	// headset screenshots showed the menus in the wrong colours - a green
	// backdrop, a cyan-to-yellow gradient where the logo belongs - and the
	// desk log said why: "cube18 -> DRAWS GUNS\SKINS_HH\WALTHER_HH.DTX". A
	// menu cube wearing a pistol. Skins were resolved from the engine's
	// texture HEAP ADDRESSES, and the menu creates and frees so many small
	// models that those addresses are recycled between them within a frame
	// (heap-address-is-not-identity, again). The client knows every skin it
	// asks for at CreateObject; it publishes the names here, and a name
	// resolved through the .dtx files is the picture the artist chose. The
	// entry is only honoured while the engine's model filename for that
	// object still agrees, so a recycled HOBJECT cannot serve a stranger.
	struct PubSkin { char szModel[80]; char szSkin[2][80]; long nFrame; };	// nFrame: when it was published
	std::unordered_map<uint32_t, PubSkin> g_PubSkins;
	int  g_bPubSkins = 1;					// +StubPubSkins 0 goes back to the heap
	long g_nPubSkinHits = 0, g_nPubSkinStale = 0, g_nPubSkinSaid = 0;
	static bool SameFileName(const char* a, const char* b)
	{
		if (!a || !b) return false;
		for (;; ++a, ++b)
		{
			char ca = *a, cb = *b;
			if (ca == '/') ca = '\\';
			if (cb == '/') cb = '\\';
			if (ca >= 'a' && ca <= 'z') ca -= 32;
			if (cb >= 'a' && cb <= 'z') cb -= 32;
			if (ca != cb) return false;
			if (!ca) return true;
		}
	}
	ID3D11Texture3D*          g_pLGridTex = nullptr;
	ID3D11ShaderResourceView* g_pLGridSRV = nullptr;
	ID3D11Buffer*             g_pCBGrid   = nullptr;	// b5: origin, inverse extent, light scale
	float g_fGridCB[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0 };
	int   g_bGridDirty = 1;
	// THE ENGINE'S GLOBAL LIGHT SCALE, published by the client every frame.
	// Retail multiplied every pixel by it - containers, damage, the interface
	// fade, and the level's time of day, which is what makes the Siberian
	// intro a night. +StubLightScale 0 ignores it.
	float g_fLightScale[3] = { 1.0f, 1.0f, 1.0f };
	int   g_bLightScaleAllow = 1;
	float g_fLightScaleTest = 0.0f;		// +StubLightScale100, the positive control
	long  g_nLightScaleSaid = 0;
	int   g_bModelLight = 1;		// +StubModelLight 0 puts the stand-in back
	// +StubLightObjects 1 fills the model light grid from the level's own
	// light objects as well as from lightmap texels. See the splat below and
	// docs/THE-LEVELS-OWN-LIGHTS.md.
	//
	// DEFAULT OFF, AND THE REASON IS A MEASUREMENT THAT DID NOT LAND.
	//
	// The plumbing works: M01S02 uses all 522 of its lights and M16S04 all
	// 218, and grid coverage goes from 5.3% to 69.8% on M16S04. But grid
	// coverage is a PROXY. The number that matters is what a model actually
	// receives, and that did not move:
	//
	//   M16S04  darkest instance 0.136 before, 0.136 after, 0 of 47 dark both
	//   M01S02  darkest instance 0.327 before, 0.320 after
	//
	// and the two M16S04 captures are pixel-identical (mean |diff| 0.01). The
	// new cells are mostly empty air; where models stand, the texels were
	// already there. It can also make a cell slightly DARKER, because light
	// samples join the same average the texels feed.
	//
	// So this ships switchable and off until there is a metric that shows a
	// benefit - the mean instance light and the count of instances whose light
	// moved, not the fraction of the box that is covered.
	int   g_bLightObjects = 0;
	long  g_nLightsUsed = 0, g_nLightCells = 0;
	long  g_nLGridCells = 0, g_nLGridFilled = 0;
	float g_fWorldAmbient[3] = { 0.0f, 0.0f, 0.0f };
	// A FLOOR UNDER THE LEVEL'S FLOOR, found by sweeping all 103 worlds.
	//
	// The level's own AmbientLight is the right answer where a level declares
	// one. Some do not: WORLDS/GOTY/M16S04 declares none, its grid covers 2306
	// cells of 296061, and 18 of its 47 models came out at or under 0.12 with
	// the darkest at 0.020 - which is black. Retail draws that level playably,
	// so whatever it does provides light there; we cannot ask it what.
	//
	// So this is a SAFETY NET rather than an art decision, and it is set low
	// enough to keep a genuinely dark room dark: 0.08 before the lightmap
	// scale, about 0.14 after. A character in an unlit corner should be dim
	// and hard to see. It should not be invisible.
	float g_fModelLightMin = 0.08f;
	// DIRECT LIGHT ON THE MODEL, THE WAY THE ENGINE LIGHTS ONE. Retail does
	// not light a model from the lightmaps at all: it sums the level's light
	// objects that reach it - colour times BrightScale, falling off linearly
	// to LightRadius - on top of the level's ambient, and clamps. The grid
	// above is the lightmap AVERAGE of the room, which is the light on the
	// walls, not the light on a man standing under a lamp. Measured on the
	// nightclub (logs/compare/Worlds_M04S02): our characters at 0.22-0.48 of
	// retail's while the walls sit at 0.7-0.8. So the direct sum is taken as
	// well, per channel, and the model gets whichever is brighter - never
	// darker than today, brighter where a light actually reaches it.
	// +StubLightDirect 0 turns it off; +StubLightGain100 scales the sum.
	int   g_bLightDirect = 1;
	float g_fLightGain = 1.0f;
	long  g_nLightDirectHits = 0, g_nLightDirectInst = 0;
	void LightDirectAt(const float* pPos, float* pInOut)
	{
		if (!g_bLightDirect || !g_pWorldFile) return;
		float d3[3] = { g_fWorldAmbient[0], g_fWorldAmbient[1], g_fWorldAmbient[2] };
		bool bAny = false;
		const std::vector<WorldLight>& L = g_pWorldFile->Lights;
		for (size_t i = 0; i < L.size(); ++i)
		{
			const WorldLight& w = L[i];
			if (!w.bObjects || w.fRadius <= 1.0f) continue;
			const float dx = pPos[0] - w.fPos[0], dy = pPos[1] - w.fPos[1], dz = pPos[2] - w.fPos[2];
			const float d2 = dx*dx + dy*dy + dz*dz;
			if (d2 >= w.fRadius * w.fRadius) continue;
			const float f = (1.0f - sqrtf(d2) / w.fRadius) * w.fBright * g_fLightGain;
			d3[0] += w.fRGB[0] * f; d3[1] += w.fRGB[1] * f; d3[2] += w.fRGB[2] * f;
			bAny = true;
		}
		// THE DYNAMIC LIGHTS, the same way. A muzzle flash lights the man it
		// is pointed at; a flickering lamp lights whoever stands under it.
		if (g_bDynLights && g_bHaveDynL)
		{
			for (uint32_t i = 0; i < g_DynL.nCount; ++i)
			{
				const VRLightInst& w = g_DynL.lights[i];
				if (!(w.nFlags & VRLIGHT_F_OBJECTS)) continue;
				const float dx = pPos[0] - w.fPos[0], dy = pPos[1] - w.fPos[1], dz = pPos[2] - w.fPos[2];
				const float d2 = dx*dx + dy*dy + dz*dz;
				if (d2 >= w.fRadius * w.fRadius) continue;
				const float f = (1.0f - sqrtf(d2) / w.fRadius) * g_fLightGain;
				d3[0] += w.fColour[0] * f; d3[1] += w.fColour[1] * f; d3[2] += w.fColour[2] * f;
				bAny = true;
			}
		}
		++g_nLightDirectInst;
		if (!bAny) return;
		++g_nLightDirectHits;
		for (int k = 0; k < 3; ++k)
		{
			if (d3[k] > 1.0f) d3[k] = 1.0f;
			if (d3[k] > pInOut[k]) pInOut[k] = d3[k];
		}
	}

	void LGridBegin(const float* pMin, const float* pMax)
	{
		g_LGrid.acc.clear(); g_LGrid.cnt.clear(); g_LGrid.bBuilt = false;
		// THE CELL GROWS ON A BIG WORLD RATHER THAN THE ALLOCATION.
		//
		// 128 units is the size this wants to be. M01S04's box needs 2152332
		// cells at that size - 34 MB in a 32-bit process, for a level whose
		// light varies slowly because most of that box is outdoors. So the
		// cell doubles until the grid fits a budget, and a level that would
		// have been refused outright gets a coarser grid instead of none.
		const double kMaxCells = 600000.0;
		g_LGrid.fCell = 128.0f;
		double nTot = 0.0;
		for (int nTry = 0; nTry < 8; ++nTry)
		{
			nTot = 1.0;
			for (int i = 0; i < 3; ++i)
			{
				g_LGrid.fMin[i] = pMin[i] - g_LGrid.fCell;
				const float fSpan = (pMax[i] - pMin[i]) + 2.0f * g_LGrid.fCell;
				g_LGrid.nDim[i] = (int)(fSpan / g_LGrid.fCell) + 1;
				if (g_LGrid.nDim[i] < 1) g_LGrid.nDim[i] = 1;
				nTot *= (double)g_LGrid.nDim[i];
			}
			if (nTot <= kMaxCells) break;
			g_LGrid.fCell *= 2.0f;
		}
		if (nTot > kMaxCells) { g_LGrid.nDim[0] = 0; return; }
		const size_t n = (size_t)g_LGrid.nDim[0] * g_LGrid.nDim[1] * g_LGrid.nDim[2];
		g_LGrid.acc.assign(n * 3, 0.0f);
		g_LGrid.cnt.assign(n, 0);
		g_nLGridCells = (long)n;
	}

	inline int LGridIndex(int x, int y, int z)
	{
		if (x < 0 || y < 0 || z < 0
			|| x >= g_LGrid.nDim[0] || y >= g_LGrid.nDim[1]
			|| z >= g_LGrid.nDim[2]) return -1;
		return (z * g_LGrid.nDim[1] + y) * g_LGrid.nDim[0] + x;
	}

	void LGridAdd(const float* pPos, const float* pRGB)
	{
		if (g_LGrid.cnt.empty()) return;
		const int x = (int)((pPos[0] - g_LGrid.fMin[0]) / g_LGrid.fCell);
		const int y = (int)((pPos[1] - g_LGrid.fMin[1]) / g_LGrid.fCell);
		const int z = (int)((pPos[2] - g_LGrid.fMin[2]) / g_LGrid.fCell);
		const int i = LGridIndex(x, y, z);
		if (i < 0) return;
		g_LGrid.acc[(size_t)i * 3 + 0] += pRGB[0];
		g_LGrid.acc[(size_t)i * 3 + 1] += pRGB[1];
		g_LGrid.acc[(size_t)i * 3 + 2] += pRGB[2];
		++g_LGrid.cnt[i];
	}

	void LGridEnd()
	{
		// ONE DILATION PASS. Only the cells a lightmapped polygon's centroid
		// landed in carry anything, and a level's worth of those is a thin
		// shell through a mostly empty box - M16S04 filled 2306 of 296061.
		// A model standing more than the search radius from any of them gets
		// the floor and nothing else, which is how that level ended up with
		// 18 black characters.
		//
		// Spreading each filled cell into its empty neighbours once, from a
		// SNAPSHOT so the pass cannot feed on its own output and smear across
		// the level, roughly triples the reach for one cheap sweep at load.
		if (!g_LGrid.cnt.empty())
		{
			const std::vector<uint32_t> was = g_LGrid.cnt;
			const std::vector<float>    wasA = g_LGrid.acc;
			for (int z = 0; z < g_LGrid.nDim[2]; ++z)
			for (int y = 0; y < g_LGrid.nDim[1]; ++y)
			for (int x = 0; x < g_LGrid.nDim[0]; ++x)
			{
				const int i = LGridIndex(x, y, z);
				if (i < 0 || was[i]) continue;		// already lit
				float sr = 0, sg = 0, sb = 0; uint32_t sn = 0;
				for (int dz = -1; dz <= 1; ++dz)
				for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int j = LGridIndex(x + dx, y + dy, z + dz);
					if (j < 0 || !was[j]) continue;
					const float inv = 1.0f / (float)was[j];
					sr += wasA[(size_t)j * 3 + 0] * inv;
					sg += wasA[(size_t)j * 3 + 1] * inv;
					sb += wasA[(size_t)j * 3 + 2] * inv;
					++sn;
				}
				if (!sn) continue;
				const float inv = 1.0f / (float)sn;
				g_LGrid.acc[(size_t)i * 3 + 0] = sr * inv;
				g_LGrid.acc[(size_t)i * 3 + 1] = sg * inv;
				g_LGrid.acc[(size_t)i * 3 + 2] = sb * inv;
				g_LGrid.cnt[i] = 1;
			}
		}
		g_nLGridFilled = 0;
		for (size_t i = 0; i < g_LGrid.cnt.size(); ++i)
			if (g_LGrid.cnt[i]) ++g_nLGridFilled;
		g_LGrid.bBuilt = !g_LGrid.cnt.empty();
	}

	// The light where a model is standing. Searches outward a little, because
	// a model stands in the air above a floor and the floor is what was
	// sampled; falls back on the level's own declared ambient, which is the
	// honest answer for a cell nothing lit.
	// The floor a level's own ambient is measured against, so a declared
	// ambient that is BRIGHTER than the safety net still wins.
	inline float LGridFloor(int i)
	{
		return g_fWorldAmbient[i] > g_fModelLightMin
			 ? g_fWorldAmbient[i] : g_fModelLightMin;
	}

	// THE GRID AS A TEXTURE, for the terrain. One RGBA8 texel per cell: the
	// cell's average where a lightmapped polygon landed in it, and for the
	// empty cells three more dilation passes from their lit neighbours, then
	// the level's floor. Rebuilt with every world; released with the device.
	void LGridUpload()
	{
		if (g_pLGridSRV) { g_pLGridSRV->Release(); g_pLGridSRV = nullptr; }
		if (g_pLGridTex) { g_pLGridTex->Release(); g_pLGridTex = nullptr; }
		g_fGridCB[7] = 0.0f; g_bGridDirty = 1;
		if (!g_bTerrainGrid || !g_LGrid.bBuilt || !g_pDev) return;
		const int nx = g_LGrid.nDim[0], ny = g_LGrid.nDim[1], nz = g_LGrid.nDim[2];
		if (nx < 1 || ny < 1 || nz < 1) return;
		std::vector<float>    acc = g_LGrid.acc;
		std::vector<uint32_t> cnt = g_LGrid.cnt;
		long nFilled0 = 0;
		for (size_t i = 0; i < cnt.size(); ++i) if (cnt[i]) ++nFilled0;
		for (int pass = 0; pass < 3; ++pass)
		{
			const std::vector<uint32_t> was = cnt;
			const std::vector<float>    wasA = acc;
			for (int z = 0; z < nz; ++z)
			for (int y = 0; y < ny; ++y)
			for (int x = 0; x < nx; ++x)
			{
				const int i = LGridIndex(x, y, z);
				if (i < 0 || was[i]) continue;
				float sr = 0, sg = 0, sb = 0; uint32_t sn = 0;
				for (int dz = -1; dz <= 1; ++dz)
				for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const int j = LGridIndex(x + dx, y + dy, z + dz);
					if (j < 0 || !was[j]) continue;
					const float inv = 1.0f / (float)was[j];
					sr += wasA[(size_t)j * 3 + 0] * inv;
					sg += wasA[(size_t)j * 3 + 1] * inv;
					sb += wasA[(size_t)j * 3 + 2] * inv;
					++sn;
				}
				if (!sn) continue;
				const float inv = 1.0f / (float)sn;
				acc[(size_t)i * 3 + 0] = sr * inv;
				acc[(size_t)i * 3 + 1] = sg * inv;
				acc[(size_t)i * 3 + 2] = sb * inv;
				cnt[i] = 1;
			}
		}
		long nFilled = 0;
		std::vector<uint8_t> px((size_t)nx * ny * nz * 4);
		for (size_t i = 0; i < cnt.size(); ++i)
		{
			float c[3];
			if (cnt[i])
			{
				++nFilled;
				const float inv = 1.0f / (float)cnt[i];
				for (int k = 0; k < 3; ++k)
				{
					c[k] = acc[i * 3 + k] * inv;
					// The level's ambient is a floor here as it is for models.
					if (c[k] < LGridFloor(k)) c[k] = LGridFloor(k);
				}
			}
			else for (int k = 0; k < 3; ++k) c[k] = LGridFloor(k);
			for (int k = 0; k < 3; ++k)
			{
				const float v = c[k] < 0.0f ? 0.0f : (c[k] > 1.0f ? 1.0f : c[k]);
				px[i * 4 + k] = (uint8_t)(v * 255.0f + 0.5f);
			}
			px[i * 4 + 3] = 255;
		}
		D3D11_TEXTURE3D_DESC td{};
		td.Width = (UINT)nx; td.Height = (UINT)ny; td.Depth = (UINT)nz;
		td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA sd{};
		sd.pSysMem = px.data(); sd.SysMemPitch = (UINT)nx * 4; sd.SysMemSlicePitch = (UINT)nx * ny * 4;
		HRESULT hr = g_pDev->CreateTexture3D(&td, &sd, &g_pLGridTex);
		if (SUCCEEDED(hr)) hr = g_pDev->CreateShaderResourceView(g_pLGridTex, nullptr, &g_pLGridSRV);
		if (FAILED(hr))
		{
			Log("  R3D LGRID TEX: FAILED hr=%08X - terrain keeps the stand-in shade", (unsigned)hr);
			if (g_pLGridSRV) { g_pLGridSRV->Release(); g_pLGridSRV = nullptr; }
			if (g_pLGridTex) { g_pLGridTex->Release(); g_pLGridTex = nullptr; }
			return;
		}
		g_fGridCB[0] = g_LGrid.fMin[0]; g_fGridCB[1] = g_LGrid.fMin[1]; g_fGridCB[2] = g_LGrid.fMin[2];
		g_fGridCB[3] = 0.0f;
		g_fGridCB[4] = 1.0f / ((float)nx * g_LGrid.fCell);
		g_fGridCB[5] = 1.0f / ((float)ny * g_LGrid.fCell);
		g_fGridCB[6] = 1.0f / ((float)nz * g_LGrid.fCell);
		g_fGridCB[7] = 1.0f;
		Log("  R3D LGRID TEX: %d x %d x %d cells of %.0f units, %ld lit, %ld after dilation, %ld at the floor %.3f %.3f %.3f"
			" - world polygons with no lightmap are lit from it",
			nx, ny, nz, g_LGrid.fCell, nFilled0, nFilled, (long)cnt.size() - nFilled,
			LGridFloor(0), LGridFloor(1), LGridFloor(2));
	}

	void LGridAt(const float* pPos, float* pOut)
	{
		pOut[0] = LGridFloor(0);
		pOut[1] = LGridFloor(1);
		pOut[2] = LGridFloor(2);
		if (!g_LGrid.bBuilt) return;
		const int cx = (int)((pPos[0] - g_LGrid.fMin[0]) / g_LGrid.fCell);
		const int cy = (int)((pPos[1] - g_LGrid.fMin[1]) / g_LGrid.fCell);
		const int cz = (int)((pPos[2] - g_LGrid.fMin[2]) / g_LGrid.fCell);
		for (int r = 0; r <= 3; ++r)
		{
			float sr = 0.0f, sg = 0.0f, sb = 0.0f; uint32_t sn = 0;
			for (int dz = -r; dz <= r; ++dz)
			for (int dy = -r; dy <= r; ++dy)
			for (int dx = -r; dx <= r; ++dx)
			{
				// Only the newly reached shell, or the centre is re-summed
				// at every radius and near cells dominate for no reason.
				if (r && abs(dx) != r && abs(dy) != r && abs(dz) != r) continue;
				const int i = LGridIndex(cx + dx, cy + dy, cz + dz);
				if (i < 0 || !g_LGrid.cnt[i]) continue;
				const float inv = 1.0f / (float)g_LGrid.cnt[i];
				sr += g_LGrid.acc[(size_t)i * 3 + 0] * inv;
				sg += g_LGrid.acc[(size_t)i * 3 + 1] * inv;
				sb += g_LGrid.acc[(size_t)i * 3 + 2] * inv;
				++sn;
			}
			if (sn)
			{
				const float inv = 1.0f / (float)sn;
				// The level's ambient is a FLOOR, not an addition: a room the
				// bake left dark is dark, and adding to it would wash out
				// exactly the contrast this is meant to restore.
				pOut[0] = sr * inv > LGridFloor(0) ? sr * inv : LGridFloor(0);
				pOut[1] = sg * inv > LGridFloor(1) ? sg * inv : LGridFloor(1);
				pOut[2] = sb * inv > LGridFloor(2) ? sb * inv : LGridFloor(2);
				return;
			}
		}
	}

	// ---- THE STILL-INSTANCE VERTEX CACHE -------------------------------
	//
	// +StubSkinCache 1. R3D STILL says 82% of skinned instance-frames have an
	// UNCHANGED skeleton, and +StubSkipStill measured what that is worth:
	// skipping those instances took the Morocco build from 6.43 ms to 1.56 ms
	// and from 10.37 to 2.39 in the same run - 76% of the cost, and 81% of the
	// heap reads. This keeps their vertices instead of throwing them away.
	//
	// A CACHE KEYED ON AN ENGINE POINTER IS THE TRAP THIS PROJECT HAS FALLEN
	// INTO BEFORE: the allocator recycles an HOBJECT and the entry serves the
	// previous owner's mesh, with "already cached" as the gate that hides it.
	// So the key is the object address and the entry carries THREE things that
	// must all still agree - the model pointer behind the object, a hash of
	// everything that decides a vertex, and the texture generation - and any
	// disagreement is a miss, not a repair.
	//
	// The hash covers the node matrices AND the object scale AND the flag word,
	// because the flags decide the lighting value written into every vertex and
	// the scale decides where the mesh sits inside its own nodes. A skeleton
	// that has not moved is not the same claim as a vertex that has not changed.
	int  g_bSkinCache = 0;
	// +StubSkinDiag 1: the per-vertex and per-triangle statistics the skinning
	// path grew while it was being debugged (bone distances, rigid pairs, weight
	// sums, record ranges, edge lengths). None of them changes a vertex; on a
	// busy level they were a large share of the ~50 ns each skinned vertex cost.
	int  g_bSkinDiag = 0;
	// Where a REBUILT instance's time goes, in QPC ticks, reported with R3D
	// MESH COST: the vertex skinning loop, and everything else on the miss
	// path (piece setup, skins, triangle emission, the cache store).
	long long g_qSkinLoop = 0, g_qMissPath = 0; long g_nMissInst = 0;
	long long g_qEmit = 0, g_qStore = 0, g_qPre = 0, g_qSlots = 0, g_qPieceSetup = 0, g_qPieceLoop = 0;
	long g_nSkinHit = 0, g_nSkinMiss = 0;
	// Bumped wherever a texture SRV is RELEASED. A cached run holds an SRV
	// pointer, and a released one that D3D reallocates at the same address is
	// the same trap in a different costume.
	uint32_t g_nTexGen = 1;
	struct CachedInst
	{
		uint32_t pModel, nHash, nTexGen;
		uint32_t nPieces, nTris;
		std::vector<Vtx>     v;
		std::vector<MeshRun> runs;	// nStart relative to the block
		// RESIDENT IN THE POOL when nPoolGen == g_nPoolGen: its vertices sit
		// at nPoolStart in g_pPoolVB and a hit draws them from there.
		UINT     nPoolStart = 0;
		uint32_t nPoolGen   = 0;
		// AND ONLY WHILE IT IS STILL THE SAME POSE. The skeleton hash the pool
		// copy was uploaded under. Residency used to be the generation alone,
		// and a miss replaced v without touching it - so an instance that was
		// still, moved, and was still again drew the pose from the FIRST time it
		// stood still: a shot guard standing on as a ghost, his fez and glasses
		// floating at head height, a body frozen mid-fall, a door handle where
		// the closed door had been.
		uint32_t nPoolHash  = 0;
	};
	std::map<uint32_t, CachedInst> g_SkinCache;
	size_t g_nSkinBytes = 0;
	// Whole-cache eviction, not per-entry. A frame's worth of vertices is about
	// 4 MB here, so this holds several and a level change flushes it anyway;
	// picking victims would be more code than the problem deserves.
	const size_t kSkinCacheMax = (size_t)24u << 20;
	// THE RESIDENT POOL (+StubMeshPool, default 1). A cache hit used to COPY
	// its vertices into the per-frame ring and upload them again every frame:
	// in M01S02's market that was ~250 000 vertices, 10 MB a frame, 3-7 ms of
	// the frame and a full ring (BUFFER FULL), so the instances that came last
	// in the order were not drawn at all - palms missing until the player turned
	// round and the order changed. A hit now uploads
	// its vertices ONCE, into this DEFAULT buffer, and every later frame draws
	// them where they are. Only instances that moved still go through the ring.
	// Reset only at the start of a build (a frame boundary): a slot written
	// mid-frame could otherwise overwrite one a run of this same frame points at.
	ID3D11Buffer* g_pPoolVB = nullptr;
	const UINT    kPoolV = 1000000u;			// 40 MB
	UINT          g_nPoolUsed = 0;
	uint32_t      g_nPoolGen = 1;
	bool          g_bPoolResetPending = false;
	int           g_bMeshPool = 1;
	long          g_nPoolHit = 0, g_nPoolUp = 0, g_nPoolNoRoom = 0, g_nPoolResets = 0;
	// Misses on an instance that was resident: each one is a pool copy that
	// the old test would have gone on drawing after the model changed.
	long          g_nPoolStale = 0;
	void FlushSkinCache() { g_SkinCache.clear(); g_nSkinBytes = 0; g_bPoolResetPending = true; }
	// Bind the buffer a run indexes, only when it changes. nRingOff is this
	// frame's region of the ring; the pool is always from 0.
	void BindRunVB(int nVB, int& nBound, UINT nRingOff)
	{
		if (nVB == nBound) return;
		nBound = nVB;
		const UINT nStride = sizeof(Vtx);
		const UINT nZero = 0;
		if (nVB == 1 && g_pPoolVB) g_pCtx->IASetVertexBuffers(0, 1, &g_pPoolVB, &nStride, &nZero);
		else g_pCtx->IASetVertexBuffers(0, 1, &g_pMeshVB, &nStride, &nRingOff);
	}

	// Draw the mesh in the box's orange instead of its real skin. Now that the
	// skins are found this is a diagnostic, not the normal path.
	int  g_bMeshTint = 0;
	// +StubMeshTexHunt 1: rank every offset in the piece record by how often it
	// resolves to a texture the engine has already bound. See the report below.
	int  g_bTexHunt = 0;
	int  g_bIdxHunt = 0;		// +StubMeshIdx 1: piece-local or global indices?
	int  g_bBeamDump = 0;		// +StubBeamDump 1: dump every ADDITIVE instance
	// +StubModelProbe <substring>: dump the same bind-box-beside-world-box for
	// any instance whose MODEL FILENAME contains this, instead of only for
	// additive ones. The beam dump answered "did the mesh arrive intact, and
	// is the transform what stretched it" for the intro's light beams; it is
	// the same question for a lamp that draws as a small lump where the file
	// says a shade on a stem, and there was no way to ask it of a named model.
	char g_szModelProbe[32] = "";
	long g_nBeamSaid = 0;
	// +StubObjScale 0 puts the scale back in bone space, which is the arm
	// every measurement before 6 September was made in.
	int  g_bObjScaleSpace = 1;
	long g_nScaleNU = 0;
	int  g_nIdxSaid = 0;
	// Every emitted vertex against the engine's own box for its object. The
	// mesh format was verified this way in the first place (98 of 98 inside),
	// so the same test says whether the piece bases are right.
	long g_nInBox = 0, g_nOutBox = 0;
	// Multi-piece models ONLY. A one-piece model has base 0 and is identical
	// under both readings, and one-piece props are the overwhelming majority -
	// so the aggregate above cannot see this change at all. 76.2% against
	// 75.4% was a measurement taken where the change could not act.
	long g_nInBoxM = 0, g_nOutBoxM = 0, g_nBadIdxM = 0, g_nFacesM = 0;
	// +StubPieceBase 0 restores the old global indexing, for one A/B.
	// 0: index a piece's faces into the model's whole array, from zero.
	//
	// This is not believed to be correct - the faces are demonstrably
	// piece-local - but it is the best of the three readings actually measured,
	// on multi-piece models only: global 83.4%, piece-slice-from-zero 78.9%,
	// piece-owns-its-records 84.1% but with 126900 pieces running short of
	// their vertex count, so that number is mostly this same fallback.
	// The default follows the measurement until a reading beats it.
	int  g_bPieceBase = 0;
	// +StubPieceVerts 1: skin the PIECE's own record array (piece+04) instead
	// of a slice of the model's. Wins over both other readings or it is wrong.
	int  g_bPieceVerts = 0;
	// The real one: each piece vertex entry points at its own weight records.
	int  g_bPieceVA = 1;
	// Apply the instance's object scale to the mesh offsets it skins.
	// +StubModelScale 0 restores the old behaviour, which drew every
	// scaled model with its joints pulled apart.
	int  g_bModelScale = 1;
	// Draw sprites farthest first. +StubSprSort 0 restores published order,
	// which is the engine's interface list and is not sorted by anything.
	int  g_bSprSort = 1;
	// Honour FLAG2_ADDITIVE on models. +StubAddBlend 0 draws them opaque,
	// which is what they looked like before anyone read the flag.
	int  g_bAddBlend = 1;
	// AN ADDITIVE DRAW ADDS TO WHAT IS ALREADY IN THE TARGET, so it has to come
	// after the thing it is supposed to brighten. The menu's motifs are MODELS
	// and the panel they sit on is a SPRITE, and the model pass runs first - so
	// they were adding to a cleared BLACK target, which returns the texture's
	// own colour, and then writing depth that rejected the panel and the logo
	// behind them. Three near-black quads over the logo, and a whole day's
	// reasoning that additive "fixed nothing" - because over black, additive and
	// opaque are the same picture, so the measurement was true and meaningless.
	// +StubAddLast 0 draws them in the mesh pass as before.
	int  g_bAddLast = 1;
	// +StubModelGlass 0 draws graded-alpha model skins opaque again.
	int  g_bModelGlass = 1;
	long g_nMeshGlassPieces = 0;
	long g_nPiecesHidden = 0;		// pieces skipped because the game hid them (nHideMask)
	int  g_nMeshGlassSaid = 0;
	static const float kNoFactorM[4] = { 0, 0, 0, 0 };
	// +StubPieceSkin 0: one skin for the whole model, the old behaviour.
	// A piece called Head_zTex1 uses skin ONE. The suffix is the model
	// format's own convention and the game's client source addresses
	// pieces by that literal name - GetPiece(hObj, "Head_zTex1").
	int  g_bPieceSkin = 1;
	// +StubVtxNormals 0: flat-shade from the face, the old behaviour.
	// The vertex ENTRY carries its own normal and we were throwing it
	// away, so a 505-vertex character drew every facet as a hard edge.
	// +StubNodeT 1 transposes the 3x3 of every node transform. The existing
	// self-check is "p = (0,0,0) must give the object's origin", which only
	// ever validated the TRANSLATION - m[3], m[7], m[11]. A transposed basis
	// leaves every bone origin exactly right, keeps the character inside its
	// own box, keeps every weight summing to one and every index in range,
	// and rotates each vertex the wrong way about its bone. That is a warped
	// character that passes every instrument this project owns.
	int  g_bNodeT = 0;
	int  g_bVtxNrm = 1;
	// +StubWeightPre 0: multiply the weight record's position by the bias
	// again, which is what shipped and what deformed every character.
	//
	// A WEIGHT'S POSITION IS ALREADY MULTIPLIED BY ITS BIAS. Ground truth,
	// from the game's own .abc files: for every multi-weight vertex in
	// HERO_ACTION, BARON_ACTION, GERMAN_ACTION, DOG_ACTION and BASEMODEL,
	//
	//     record.position  ==  bias * (InverseBindOf(node) . vertex.position)
	//
	// exactly - the ratio came back 1.25 for a bias of 0.80 and 5.00 for a
	// bias of 0.20, to four decimals, on every record. So the world position
	// is the ROTATION applied to the stored offset, plus the bias times the
	// node's translation:
	//
	//     world = sum_i ( R_i . s_i  +  w_i * t_i )
	//
	// and NOT sum_i w_i * (M_i . s_i), which scales the rotated offset by
	// the bias a second time and drags every blended vertex toward its own
	// joint by that factor. A one-record vertex has bias 1.0 and w == w*w,
	// so props were always right and only characters deformed - and the
	// rigid-pair test that pronounced the skinning correct only ever
	// sampled one-record vertices, the exact set the bug cannot touch.
	//
	// Reconstructing each model's own stored bind position from its weights:
	// this reading lands within 0.0001 units, the old one misses by a mean
	// of 2.5 and a worst case of 17.3 on a character 106 units tall.
	int  g_bWeightPre = 1;
	// +StubEdgeChk 1: the BIND vs WORLD EDGES instrument. Off by default; it
	// is per-triangle work in the hot path and it has already answered the
	// question it was built for.
	int  g_bEdgeChk = 0;
	// +StubFlushOnLoad 0: keep the texture cache across a world load.
	int  g_bFlushOnLoad = 1;
	long g_nWorldLoads = 0;
	// +StubSkinConsist 1: does a piece keep the same picture across a reload?
	int  g_bSkinConsist = 0;
	struct SkinRef { char name[20]; float r, g, b; long nLoad; };
	std::vector<SkinRef> g_SkinRef;
	long g_nConsistSeen = 0, g_nConsistBad = 0;
	int  g_nConsistSaid = 0;
	// +StubEntryCount: how many weight records a vertex entry owns.
	//   0 = the +04 field, but overridden by the pointer GAP when that looks
	//       plausible. This is what shipped, and 71% of vertices a frame end
	//       up with weights that do not sum to one - which is not a rounding
	//       error, it is three quarters of every character placed by a
	//       partial sum. That is the deformation.
	//   1 = the +04 low uint16 alone, never the gap.
	//   2 = the gap alone.
	int  g_nEntryCount = 0;
	int  g_bSkipOcclTex = 1;
	long g_nOcclPolys = 0;
	// The mesh-build cost, accumulated between reports.
	double    g_fMeshMsSum = 0.0, g_fMeshMsMax = 0.0;
	long      g_nMeshMsCnt = 0;
	long long g_nMeshReads = 0;
	// Engine callback 2 turns a texture OBJECT into its data. BindTexture
	// uses it, which is how every texture we own arrived - but the engine
	// only binds what IT draws, and it draws models through a path we do
	// not implement. So a model skin can exist, be named by a piece, and
	// never once be handed to us. The weapon's grip is exactly that: its
	// piece asks for slot 1, the object HAS a slot 1 pointer, and the slot
	// resolves to nothing - so it fell back to slot 0 and drew the barrel's
	// texture on the grip. Pull it ourselves instead of waiting.
	void* (__cdecl *g_pfnTexData)(void*) = nullptr;
	uint32_t g_aTexTried[256] = { 0 };
	int      g_nTexTried = 0;
	long     g_nTexPulled = 0;
	std::vector<uint32_t> g_SkinPullQ;
	// A RECYCLED texture object. The cache is keyed on the engine's pointer,
	// and after a quick-load the engine frees every texture and its allocator
	// hands the same addresses back for different images. The world's own
	// textures recover because the engine rebinds them as it draws; MODEL
	// SKINS never do, because it draws models through a path we do not
	// implement - so the cache keeps serving the old pixels and an NPC ends
	// up wearing another character's skin. Found in headset testing: reload the save
	// and the monkey seller is wearing Cate Archer.
	//
	// Detectable exactly, now that texobj+0x08 is known to hold the data
	// pointer: if the object's data no longer matches what the entry was
	// built from, that entry is for a texture that no longer exists.
	std::vector<uint32_t> g_TexRefreshQ;
	long g_nTexStale = 0;
	// Every texture we own arrived as a PAIR: the engine's texture object
	// and its data, the second fetched with callback 2. Calling that
	// callback ourselves kills world rendering. But if the object simply
	// STORES its data pointer, we can read it and never call anything -
	// so rank every offset in the object by how often it equals the data
	// pointer we were handed. 64 dwords, over every bound texture.
	long g_aDataOff[64] = { 0 };
	long g_nDataPairs = 0;
	int  g_nDataOffSaid = 0;
	// +StubSkinPull 1. Default OFF until it is proven not to be what killed
	// world rendering in three consecutive runs.
	int  g_bSkinPull = 1;
	// Is the float3 at entry+0x14 really a normal? Unit length says yes.
	// THE MENU ZOOM, 3D HALF. The same zoom and anchor the 2D layer applies to
	// its NDC, expressed as frustum tangents - because the menu CARD is a
	// sprite in the interface scene and the words on it are 2D blits, so a
	// zoom that moves one and not the other slides the text off the card.
	// That is exactly what +StubMenuScale100 did, and why it was recorded as
	// "not a usable lever".
	//
	// ndc maps linearly from [t0,t1] onto [-1,1], so asking for
	// ndc' = (ndc - a) * Z is asking for the frustum whose edges sit at the
	// OLD ndc a +/- 1/Z. Nothing here knows about menus; g_bMenuZoomOn is set
	// from the same condition that drives R2D_SetMenu - the host-presents-a-
	// quad case, NOT R2D_SetMenuFit, which is also true over a live world.
	// This changes the frustum the whole scene is drawn with, so on the fit
	// flag it would magnify the LEVEL behind the pause menu.
	float g_fMenuZoom3D = 1.0f, g_fMenuAnchorX3D = 0.0f, g_fMenuAnchorY3D = 0.0f;
	int   g_bMenuZoomOn = 0;
	// THE MENU PANEL IS A 16:9 BAND OF THE EYE (host --menu-aspect, 2D
	// g_fMenuBand). The interface scene - Cate, the logo, the card's flower -
	// is authored for the interface camera's 75-degree vertical view, and the
	// band must show exactly that: the pass is zoomed OUT until the band's
	// rows span the scene's own angle. 0 = the whole eye, no band zoom.
	float g_fMenuBand3D = 16.0f / 9.0f;
	// The backdrop SHEETS the band widens and draws opaque, by texture
	// name: the main menu's blue (MAINMENU*), the mission cards' orange
	// (*BACKGROUND*), the loading screen's (LOADING3*). Not FOLDERBACK: that
	// fill is fully transparent in retail and draws nothing.
	static bool IsFolderBackName(const char* pN)
	{
		if (!pN || !pN[0]) return false;
		const char* pB = strrchr(pN, '\\'); pB = pB ? pB + 1 : pN;
		return _strnicmp(pB, "FOLDERBACK", 10) == 0;
	}
	static bool IsMenuSheetName(const char* pN)
	{
		if (!pN || !pN[0]) return false;
		const char* pB = strrchr(pN, '\\'); pB = pB ? pB + 1 : pN;
		char szU[80]; strncpy_s(szU, pB, _TRUNCATE); _strupr_s(szU);
		return strstr(szU, "MAINMENU") || strstr(szU, "BACKGROUND")
			|| strstr(szU, "LOADING3");
	}

	long g_nNrmUnit = 0, g_nNrmSeen = 0;
	// HOW MANY PIECES DREW WITH NO TEXTURE AT ALL, uncapped and per level.
	// The R3D SKIN report answers this piece by piece and stops after 400
	// lines, so beyond that cap a level could draw a thousand untextured
	// pieces and say nothing. A count has no cap, and this is the number a
	// 103-level sweep can actually be judged on. See make-the-parts-add-up.
	long g_nPieceDrawn = 0, g_nPieceWhite = 0, g_nPieceUnnamed = 0;
	int  g_nWhiteSaid = 0;
	// +StubSkinNameProbe. See the note at the white-piece report.
	int  g_bSkinNameProbe = 0;
	// +StubSkinFromButes. The model's filename is at model+0x04 and the game's
	// attribute files say which skin goes with it. Default ON.
	int  g_bSkinFromButes = 1;
	// +StubHideViewArms. The view model's baked-in arms, which cannot be
	// right once its origin is at the controller.
	int  g_bHideViewArms = 1;
	// +StubBody / +StubHideHead. Draw the player's own model, minus the
	// head you are looking out of.
	int  g_bDrawBody = 0;
	int  g_bHideHead = 1;
	long g_nHeadHidden = 0;
	long g_nViewArmsHidden = 0;
	int  g_nButeTryThisFrame = 0, g_nButeSaid = 0;
	long g_nSkinFromButes = 0;
	// HOW MANY PUBLISHED INSTANCES DID NOT MOVE SINCE LAST FRAME.
	//
	// Six levels hit the 192 publish cap and M01S03 drops 143 models at it.
	// Raising the cap to 512 was tried and reverted because CPU skinning cost
	// 3-5 ms, and the standing note says fix the COST before raising the cap.
	// The cheapest possible saving is not to skin an instance whose skeleton
	// has not changed - but "most props are static" is a guess until it is a
	// number, and this is the number.
	//
	// A HASH OF THE NODE TRANSFORMS, keyed by HOBJECT. The key can be recycled
	// by the allocator, which would report one frame of a new object as
	// unchanged; that is acceptable for a REPORT and would not be for a cache,
	// which is exactly why this is only a report. See heap-address-is-not-identity.
	std::map<uint32_t, uint32_t> g_NodeHash;
	long g_nInstStill = 0, g_nInstMoved = 0;
	int  g_nSkinProbeSaid = 0;
	// HOW MANY INSTANCES THE ENGINE IS NOT DRAWING, and how many of the
	// untextured pieces belong to one. Counted before anything skips anything:
	// a rule that deletes has to be able to say what it deleted and why, and
	// "these are invisible" is a hypothesis until there is a number.
	long g_nInvisInst = 0, g_nInvisWhite = 0, g_nSkipInvis = 0;
	long g_nInvisNamed = 0;
	// Which objects have already been named, so a rare one is not crowded
	// out by the player's own body being skipped every single frame.
	uint32_t g_aInvisSeen[64] = {};
	int      g_nInvisSeen = 0;
	// SPRITES THAT FAIL TO RESOLVE A TEXTURE **AND ARE IN FRONT OF THE
	// CAMERA**. "362 published, 33 resolved" is alarming and does not say
	// whether anybody can see the other 329 - most sprites in a level are
	// behind you. This is the number that decides whether a missing corona is
	// a missing corona or a fact about somewhere you are not looking.
	long g_nSprFront = 0, g_nSprFrontNoTex = 0;
	int  g_nSprNameSaid = 0;
	int  g_nSprFileThisFrame = 0;
	// +StubSprFromFile. Default ON: it is the same principle as the model
	// skins' file route, which is already the default, and the alternative is
	// 198 missing effects on a level where it is raining. Its own switch so it
	// can be A/B'd without turning off every other file read.
	int  g_bSprFromFile = 1;
	long g_nSprFromFile = 0;
	// +StubSprNameProbe. Deliberately NOT folded into the existing sprite
	// probe: that one is gated on g_bCutProbe, and +StubCutProbe 1 kills the
	// game at startup - a recorded, uninvestigated fault. A diagnostic reached
	// only through a crash is not a diagnostic.
	int  g_bSprNameProbe = 0;
	int  g_nSprProbeSaid = 0;
	// +StubSkipInvisible. DEFAULT 1, and the evidence is in the commit:
	// three of the four levels that drew models untextured go to zero, no
	// level regresses across a 103-level sweep of both arms, the player's
	// weapon survives (photographed), and both arms differ by 0.01-0.04 mean
	// - below the harness's own noise floor. `+StubSkipInvisible 0` is the
	// one-switch revert if anything in a headset says otherwise.
	int  g_bSkipInvisible = 1;
	// Does the vertex normal actually shade differently from the face
	// normal it replaced? The shader's term is 0.55 + 0.45*|dot(n,d)|,
	// so compare THAT, not the angle - it is what reaches the screen.
	double g_fNrmLumSum = 0.0; long g_nNrmLumCnt = 0;
	// Do the per-vertex normals actually REACH a triangle? The measurement
	// so far only compared them where they were already being used.
	long g_nTriSmooth = 0, g_nTriFlat = 0;
	double g_fNrmLumMax = 0.0;

	// A piece name carries its texture slot as a zTex<N> suffix -
	// Head_zTex1, Eyelash_zTex2_1, Hat_zTex1. Case varies in the shipped
	// models (head_ztex1_1 exists), so match case-insensitively. No
	// suffix means slot 0, which is the body.
	// The first 24 PIECES were four characters, and the weapon - which is
	// what draws an orange slab where retail has a wooden grip - never got
	// a line. Let each MODEL through once instead, all of its pieces.
	uint32_t g_aSkinSeen[64] = { 0 };
	int      g_nSkinSeen = 0;
	int      g_nLodSaid = 0;
	static bool SkinModelNew(uint32_t pModel, uint32_t k)
	{
		for (int z = 0; z < g_nSkinSeen; ++z)
			if (g_aSkinSeen[z] == pModel) return k > 0;
		if (g_nSkinSeen >= 64) return false;
		g_aSkinSeen[g_nSkinSeen++] = pModel;
		return true;
	}

	// WHICH PICTURE DID THIS PIECE ACTUALLY GET? The skin report has always
	// said which SLOT a piece chose and whether that slot resolved, and that
	// is a different question from what is on the triangles. The weapon's
	// grip resolves, reports "slot 0 texture", and draws WOOD PANELLING - a
	// level texture - so every field in that line was true and none of them
	// named the fault. The shader resource view is the last thing in the
	// chain, so ask the cache what it is filed as.
	//
	// A linear scan, inside a capped diagnostic that stops after 400 lines.
	static const char* SkinSrvName(ID3D11ShaderResourceView* pSRV)
	{
		if (!pSRV) return "-";
		for (size_t i = 0; i < g_Tex.size(); ++i)
			if (g_Tex[i].pSRV == pSRV)
				return g_Tex[i].szName[0] ? g_Tex[i].szName : "<unnamed cache entry>";
		return "<not a cache entry>";
	}

	// NEVER pull from inside the frame. The first version called the engine
	// callback and created a D3D texture in the middle of the mesh build, and
	// the world stopped rendering after its first scene - 1 world scene over a
	// whole run against 2777 in the run before it. Two runs, both dead the same
	// way, so not the intermittent startup fault. Queue the pointer here and
	// let the frame boundary do the work, which is where every other texture
	// this renderer owns was created.
	// Queued, never done here: creating a texture inside the mesh build is
	// what the frame boundary exists for.
	static void QueueTexRefresh(uint32_t pTex)
	{
		if (!pTex) return;
		for (size_t z = 0; z < g_TexRefreshQ.size(); ++z)
			if (g_TexRefreshQ[z] == pTex) return;
		// 64 was too small: a reload invalidates every model skin in the
		// level at once and the overflow was silently dropped, so whatever
		// did not fit kept the wrong picture for the rest of the session.
		if (g_TexRefreshQ.size() >= 256) return;
		g_TexRefreshQ.push_back(pTex);
	}

	static void QueueSkinPull(uint32_t pTex)
	{
		if (!pTex || !g_bSkinPull) return;
		for (int z = 0; z < g_nTexTried; ++z)
			if (g_aTexTried[z] == pTex) return;
		if (g_nTexTried >= 256) return;
		g_aTexTried[g_nTexTried++] = pTex;
		g_SkinPullQ.push_back(pTex);
		if (g_nTexTried <= 8)
			Log("  R3D SKIN QUEUE: %08X queued for pull", pTex);
	}

static int PieceSkinSlot(const char* s)
	{
		if (!s) return 0;
		// s[i+4] is read below, so stop far enough short of the 32 bytes
		// the caller validated. Reading four bytes past a checked span is
		// a fault whenever it lands on a page boundary.
		for (int i = 0; i + 4 < 32 && s[i]; ++i)
		{
			if ((s[i] | 32) != 'z') continue;
			if ((s[i+1] | 32) != 't' || (s[i+2] | 32) != 'e'
				|| (s[i+3] | 32) != 'x') continue;
			const char c = s[i+4];
			if (c < '0' || c > '9') continue;
			const int n = c - '0';
			// A MODEL HAS EXACTLY TWO SKIN SLOTS, and the name is only a hint.
			// Counted over every .abc in NOLF.REZ: 1202 pieces, and their
			// materialIndex field is 0 on 881 and 1 on 321. Nothing uses 2.
			// Sixty-five pieces are NAMED zTex2 while the file says material
			// 1 - Cate's two eyelashes in every HERO_*, and a helicopter
			// cylinder - so the name overshoots by one and those pieces fell
			// back to the body skin. Slot 1 is the head, which is what they
			// want. (Slots 2 and 3 of the object read 000000FF and 19E3CE92:
			// garbage, not skins.)
			return (n >= 1) ? 1 : 0;
		}
		return 0;
	}
	int  g_nSkinSaid = 0;
	long g_nVAOK = 0, g_nVABad = 0;
	long g_nCulledBehind = 0;	// instances skinned nowhere near the view
	long g_nOriginFaces = 0;	// faces dropped for naming an unresolved vertex
	long g_nUnposed = 0;		// vertices bound to a node the engine never posed
	long g_nBadWeight = 0;		// records whose weight is not a fraction
	long g_nBadWSum = 0;		// vertices whose weights do not sum to one
	// The DENOMINATOR. Without it the bad-sum count could not be compared
	// between two arms - a bare total says nothing about a population that
	// also changed. Reset together with it, below.
	long g_nWSeen = 0;
	// HOW FAR IS A VERTEX FROM THE BONE IT CLAIMS? Every check we own asks
	// whether a vertex is inside the object's box, and a vertex bound to the
	// WRONG bone of the same character is still inside that box. This one can
	// see it: a vertex belongs near the bones that own it, so a skin bound to
	// the wrong node shows up as a large distance even when the weights sum
	// to 1 and every index is in range.
	double g_fBoneDistSum = 0.0;
	long   g_nBoneDist = 0, g_nBoneFar = 0;
	float  g_fBoneDistMax = 0.0f;
	// The bone distance measures |p|, the record's own bone-space position,
	// because rotating p and adding the bone's translation leaves the
	// distance back to that translation equal to |p|. So it does NOT say the
	// vertex is bound to the wrong bone - it says the RECORD is implausible.
	// A 106-unit character has no bone-space offset of 141 units. Measure the
	// records themselves, and DUMP the bad ones raw rather than reasoning
	// about them - reading these as floats has hidden a struct before.
	long g_nRecSeen = 0, g_nRecHuge = 0, g_nRecNaN = 0;
	int  g_nRecDumped = 0;
	// The first cut of this measured object SIZE, not error: the records it
	// flagged were well formed - weight exactly 1.0, valid node, position 90
	// units - and simply belonged to something big. A 60-unit bone offset is
	// only implausible on a CHARACTER, so the population has to be characters.
	// 25 nodes is a character here, 3-14 a prop; 20 is a safe line.
	long g_nChrRec = 0, g_nChrHuge = 0;
	double g_fChrSum = 0.0;
	float  g_fChrMax = 0.0f;
	// A node matrix the client published as NaN. Nothing downstream tests for
	// it, because every test is a comparison and NaN fails all of them.
	long g_nNodeNaN = 0;
	int g_nRunDumped = 0;
	int g_nHideDumped = 0;
	// THE SHAPE TEST. Two vertices rigidly bound to the SAME bone - one
	// record each, weight 1.0 - cannot change their distance apart, because a
	// bone transform is a rotation and a translation. So the distance between
	// them in BIND space must equal the distance after skinning, exactly.
	// This is the first check here that measures SHAPE rather than position,
	// and unlike a bounding box or a distance-from-bone it cannot be fooled
	// by the size of the object.
	//
	// Bind space is the entry's own float3 at +0x08, the field sitting beside
	// the normal at +0x14 that nothing has ever read.
	double g_fRigidErr = 0.0, g_fRigidRel = 0.0;
	long   g_nRigidPairs = 0, g_nRigidBad = 0;
	float  g_fRigidWorst = 0.0f;
	// BIND EDGE AGAINST WORLD EDGE, and it carries its own control.
	//
	// The entry's +0x08 is the vertex's own bind position, so every triangle
	// has a known bind size. A correct skin near the bind pose reproduces it;
	// a pose only bends the model at its joints, so the ratio stays close to
	// one. The buggy weighting drags every BLENDED vertex toward its joint,
	// which shortens exactly the edges that touch one.
	//
	// Split by whether the triangle touches a vertex with more than one
	// weight. The one-record triangles are the control: the weighting cannot
	// affect them, so they must read 1.00 in both arms. If they do not, the
	// instrument is wrong and not the renderer.
	double g_fEdgeBindR = 0.0, g_fEdgeWorldR = 0.0;
	double g_fEdgeBindB = 0.0, g_fEdgeWorldB = 0.0;
	long   g_nEdgeR = 0, g_nEdgeB = 0;
	// The rigid test measures POSITIONS and says they are right to 0.13%.
	// It cannot see CONNECTIVITY: every vertex can be in exactly the right
	// place while the faces join the wrong ones, and the result is a
	// recognisable character with panels stretched across it - static,
	// pose-independent, and passing every check written so far including
	// "0 bad index", which only ever tested RANGE. A character's triangle
	// edges are a few units long; a scrambled one spans the whole body.
	double g_fEdgeSum = 0.0; long g_nEdges = 0, g_nEdgeLong = 0;
	float  g_fEdgeMax = 0.0f;
	// WHERE do the bad weight sums fall? If they are the LAST entry of each
	// piece, the cause is structural: that entry has no next pointer, so the
	// gap cannot be used and the +04 field stands alone. 1.3% of vertices
	// misplaced is about the right size to make the 1.1% of stretched edges.
	long g_nBadLast = 0, g_nBadMid = 0, g_nLastSeen = 0;
	// THE LINK. Are the stretched triangles attached to the vertices whose
	// weights did not sum to one? If they are, one defect explains the
	// picture and there is a single thing to fix. If they are not, the
	// stretched edges have their own cause and the weight sums are a
	// separate 1.3% that happens to be harmless.
	long g_nLongBad = 0, g_nLongClean = 0;
	// The previous rigid vertex on each node, so pairs share a bone.
	uint32_t g_nPrevNode = 0xFFFFFFFFu;
	float g_fPrevBind[3] = { 0, 0, 0 }, g_fPrevWorld[3] = { 0, 0, 0 };
	// Instances whose drawn geometry does not fit their own box, named once each.
	uint32_t g_aNamed[64] = { 0 };
	int      g_nNamed = 0;
	// Smallest span seen for each MODEL, so an instance can be judged against
	// its own kind rather than against a collision box.
	struct ModelSpan { uint32_t pModel; float fMin; };
	ModelSpan g_aSpan[256];
	int       g_nSpans = 0;
	// The biggest things drawn this frame, by the diagonal actually emitted.
	struct BigInst { uint32_t pObj, pModel; float fDiag; float fPos[3];
					 float fDims[3]; uint32_t nNodes, nPieces; };
	BigInst g_aBig[10];
	int     g_nBig = 0;
	int     g_bBigSaid = 0;
	int     g_nNodeDumps = 0;	// instances whose nodes have been printed
	long g_nPieceShort = 0;		// pieces whose records ran out before nPV
	// [offset/4][0] = direct hits, [1] = indirect hits, over all pieces seen.
	long g_aTexHit[64][2] = { { 0 } };
	// The OBJECT (256 dwords) and the MODEL (128 dwords). The piece scan came
	// back 0 of 288 for every offset, so whatever names a skin is not in the
	// piece - and the object is where a LithTech game sets one.
	long g_aObjHit[256][2] = { { 0 } };
	long g_aModHit[128][2] = { { 0 } };
	long g_nTexHuntPieces = 0, g_nTexHuntObjs = 0;
	int  g_bTexHuntSaid = 0;

	// Occlusion queries around the mesh draw: samples that passed depth. Two,
	// used alternately, and the older is read with DONOTFLUSH so waiting never
	// happens - a readback on the render thread is what froze this game before.
	ID3D11Query* g_pOcc[2] = { nullptr, nullptr };
	int          g_nOccCur = 0;
	int          g_bOccArmed[2] = { 0, 0 };
	long         g_nOccReports = 0;
	int  g_nMeshCap = 0;		// vertices dropped for want of buffer
	float g_fNearAz = 999.0f;	// bearing to the nearest model, degrees
	float g_fNearDist = 1e30f;

	// Frames, counted at the engine's own frame boundary. See R3D_NotePresent.
	volatile long g_nFrames = 0;

	// WHICH OF THE TWO WORLD BSPs TO DRAW.
	//
	// This project has always drawn VisBSP and skipped PhysicsBSP as "the
	// collision twin the engine never draws". The lightmap breakdown says
	// otherwise: on T01S01, 3790 of 3888 packed lightmaps belong to PhysicsBSP
	// and VisBSP has none, which is why the night intro renders as flat
	// daylight - 98 of 1118 drawn polygons find a lightmap. And PhysicsBSP is
	// textured with the same real wall, wood and stone textures as VisBSP, in
	// the same families.
	//
	// Both cannot be drawn at once: they are coplanar and z-fight, which is
	// what "the collision hull drawn over the street" was. So it is a choice,
	// and +StubWorldBSP makes it testable in one run against retail:
	//   0  VisBSP     (skip PhysicsBSP)   - what this renderer used to do
	//   1  PhysicsBSP (skip VisBSP)       - THE DEFAULT NOW
	//   2  both                           - the z-fighting control
	//
	// Measured on M01S02, same level, same start point, one run each:
	//
	//        mode 0   10304 polygons drawn,  4446 lit (43%)
	//        mode 1   18273 polygons drawn, 17609 lit (96%)
	//
	// so mode 1 draws 77% MORE geometry and nearly all of it is lit. On T01S01
	// it is 1118/98 against 3962/3882. The pictures say the same thing: mode 0
	// is flat and uniformly bright, mode 1 has the wall gradients, the warm
	// glow under a candle sconce and the dark night exterior that retail has.
	//
	// Across every world, PhysicsBSP uses a strict superset of VisBSP's
	// textures apart from marker textures - so this is not a trade of lighting
	// against geometry.
	//
	// UNVERIFIED IN THE HEADSET. +StubWorldBSP 0 restores the old behaviour
	// exactly, and that is the A/B to run if anything looks wrong.
	int g_nWorldBSP = 1;

	// TERRAIN SECTIONS ARE NOT DRAWN. A Terrain object ("Canopy_a", "Dome1",
	// the hydro dam's "Terrain0") carries its polygons in the level file, and
	// at load the engine splits it into "#$#N <name>" section objects that
	// are NOT in the file - they are the same polygons again, sectioned for
	// the engine's own visibility. The renderer walked both the parent and
	// the sections out of the heap and drew both: two coincident surfaces,
	// and the sections' FLAG_VISIBLE follows the engine's culling, so once
	// the hidden rule honoured that flag the second copy came and went as
	// the tester moved - the canopy top still flickered (runs 12 and 13). The
	// parent is drawn, whatever the client says; the sections never are.
	// The dam's cliffs proved the parent alone is the whole picture.
	// +StubDrawSections 1 puts them back for an A/B.
	int  g_bDrawSections = 0;
	// +StubCull: 0 none (default), 1 back faces, 2 front faces.
	int  g_nCullMode = 0;
	long g_nSectionsSkipped = 0;
	bool SkippedModel(uint32_t sub)
	{
		char szName[32];
		ModelName(sub, szName, sizeof szName);
		if (!g_bDrawSections && strncmp(szName, "#$#", 3) == 0)
		{ ++g_nSectionsSkipped; return true; }
		if (g_bSkipTranslucent
			&& strncmp(szName, "TranslucentWorldModel", 21) == 0) return true;
		if (!g_bSkipHidden) return false;
		if (g_nWorldBSP == 1) return strcmp(szName, "VisBSP") == 0
			|| strncmp(szName, "AIVolume", 8) == 0
			|| strncmp(szName, "blocker", 7) == 0;
		if (g_nWorldBSP == 2) return strncmp(szName, "AIVolume", 8) == 0
			|| strncmp(szName, "blocker", 7) == 0;
		return strcmp(szName, "PhysicsBSP") == 0
			|| strncmp(szName, "AIVolume", 8) == 0
			// blocker (42) and blockerX (6): occluder brushes, tiled with a
			// texture that reads "OCCLUDER" in letters. One hangs over the
			// street in the 4 Sept headset screenshot as a green panel across the
			// sky. Same class as the two above - geometry the engine has but
			// never draws - and found the same way, by name.
			|| strncmp(szName, "blocker", 7) == 0;
	}

	// The last path component, so a file's "TEX\\AI.DTX" and the engine's
	// own spelling of the same texture can be compared.
	const char* BaseName(const char* psz)
	{
		if (!psz) return "";
		const char* p1 = strrchr(psz, '\\');
		const char* p2 = strrchr(psz, '/');
		const char* p = p1 > p2 ? p1 : p2;
		return p ? p + 1 : psz;
	}

	// HOW OFTEN this is called, because it is not free and every Word() below
	// makes one. IsBadReadPtr probes the page through the exception machinery;
	// at a few hundred nanoseconds a call it is invisible at ten thousand a
	// frame and is the whole frame at a million. Nobody had ever counted, and
	// "every extra model instance is skinned on the CPU" was the standing
	// explanation for why the publish cap could not be raised.
	long long g_nReadCalls = 0;

	// ---- A PAGE CACHE IN FRONT OF IsBadReadPtr -------------------------
	//
	// The answer to "is this address readable" is a property of the 4 KB PAGE,
	// not of the address, and it does not change while the page stays mapped.
	// IsBadReadPtr re-derives it every single time, through the exception
	// machinery, and Word() makes one call per read.
	//
	// So: one VirtualQuery per page, remembered. Direct-mapped, 8192 entries,
	// no allocation, no locking - the renderer's model build is single
	// threaded and a stale entry in a racing thread would only cost a re-probe.
	//
	// FLUSHED whenever the world is rebuilt - which is when the engine frees and
	// reallocates everything this renderer reads - and again every kFlushFrames
	// published frames, so a stale entry can only live a few seconds.
	//
	// ON BY DEFAULT, on this A/B over the same 45-second quick-save run:
	//
	//                    frame      world render   mesh build
	//     off            14.103 ms    12.212 ms     10.051 ms
	//     on             12.456 ms     9.600 ms      7.342 ms
	//
	// with WORLD DRAWN identical to the vertex (82074600 from 292 batches) and
	// zero anomalies in both arms, over 3515 frames. That is faster than the
	// 12.918 ms this renderer measured BEFORE it started drawing 2.35x the world
	// geometry, so the lighting fix costs nothing net.
	//
	// The residual risk is a page decommitted while cached as readable. It is
	// small - the skinning loop already dereferences raw pointers that Readable
	// validated a moment earlier, so the window is not materially wider than it
	// was - but +StubFastRead 0 is the control if anything ever crashes here.
	int g_bFastRead = 1;
	const uint32_t kFlushFrames = 300;
	long long g_nPageProbes = 0;
	// Reads the page cache called safe that were not. Zero is the claim that
	// the cache is keeping up; anything else says a load freed memory under us.
	long long g_nPageStale  = 0;
	struct PageEnt { uint32_t tag; uint8_t state; };   // 0 unknown, 1 ok, 2 bad
	const uint32_t kPageBits = 13;
	PageEnt g_aPage[1u << kPageBits] = {};

	void FlushPageCache()
	{
		memset(g_aPage, 0, sizeof g_aPage);
	}

	// Bounded staleness. Called once per published frame, not per read.
	void AgePageCache(uint32_t nFrame)
	{
		static uint32_t s_nFlushedAt = 0;
		if (nFrame - s_nFlushedAt >= kFlushFrames)
		{ s_nFlushedAt = nFrame; FlushPageCache(); }
	}

	inline bool PageReadable(uint32_t nPage)
	{
		PageEnt& e = g_aPage[(nPage * 2654435761u) >> (32 - kPageBits)];
		if (e.state && e.tag == nPage) return e.state == 1;
		++g_nPageProbes;
		MEMORY_BASIC_INFORMATION mbi{};
		const void* pv = (const void*)(uintptr_t)((uint64_t)nPage << 12);
		const bool ok = VirtualQuery(pv, &mbi, sizeof mbi) == sizeof mbi
			&& mbi.State == MEM_COMMIT
			&& !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
		e.tag = nPage; e.state = ok ? 1 : 2;
		return ok;
	}

	// TOUCH THE RANGE FOR REAL, UNDER SEH.
	//
	// The page cache above answers from a VirtualQuery taken up to
	// kFlushFrames ago, and AN ADDRESS IS NOT AN IDENTITY: the engine
	// decommits the menu's texture pages while a level loads, and a cached
	// "readable" then sends us into freed memory. That is what killed the
	// first successful load out of the menu - Readable(pData, 0xDC) returned
	// true and the very next instruction, movzx eax,[esi+0x10], took an access
	// violation at d3dstub.ren+000243DE.
	//
	// So the cache no longer decides whether a read is safe. It only saves the
	// VirtualQuery. One byte per page, plus the last byte, is the whole cost.
	// A page that faults is marked UNKNOWN rather than bad, so the next call
	// asks VirtualQuery again and gets the truth rather than inheriting a
	// verdict of ours.
	//
	// No C++ objects live in here: MSVC will not allow __try in a frame that
	// needs unwinding.
	bool TouchRange(uint32_t p, size_t n)
	{
		const uint64_t nEnd = (uint64_t)p + n - 1;
		__try
		{
			for (uint64_t a = ((uint64_t)p + 0xFFF) & ~0xFFFull; a <= nEnd; a += 0x1000)
			{
				const volatile uint8_t* r = (const volatile uint8_t*)(uintptr_t)(uint32_t)a;
				(void)*r;
			}
			{
				const volatile uint8_t* r0 = (const volatile uint8_t*)(uintptr_t)p;
				const volatile uint8_t* r1 = (const volatile uint8_t*)(uintptr_t)(uint32_t)nEnd;
				(void)*r0; (void)*r1;
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void ForgetPages(uint32_t p, uint64_t nEnd)
	{
		for (uint32_t g = p >> 12; g <= (uint32_t)(nEnd >> 12); ++g)
		{
			PageEnt& e = g_aPage[(g * 2654435761u) >> (32 - kPageBits)];
			if (e.tag == g) e.state = 0;
		}
	}

	bool Readable(uint32_t p, size_t n)
	{
		++g_nReadCalls;
		if (!p) return false;
		if (!g_bFastRead) return !IsBadReadPtr((const void*)(uintptr_t)p, n);
		if (!n) return true;
		const uint64_t nEnd = (uint64_t)p + n - 1;
		if (nEnd > 0xFFFFFFFFull) return false;		// wrapped
		for (uint32_t g = p >> 12; g <= (uint32_t)(nEnd >> 12); ++g)
			if (!PageReadable(g)) return false;
		if (!TouchRange(p, n))
		{
			++g_nPageStale;
			ForgetPages(p, nEnd);
			return false;
		}
		return true;
	}
	uint32_t Word(uint32_t p, uint32_t off)
	{
		return Readable(p + off, 4) ? *(const uint32_t*)(uintptr_t)(p + off) : 0;
	}

	// Defined further down, beside the texture cache it reads.
	bool TexNameOf(uint32_t pTexObj, char* pOut, size_t nOut);

	// ---- IS ANY VisBSP GEOMETRY ACTUALLY MISSING? ----------------------
	//
	// Drawing PhysicsBSP instead of VisBSP is right by every measure taken -
	// the lightmaps, the textures, the pixel comparison against retail - but
	// one weak proxy says 10.2% of VisBSP's surfaces game-wide are not matched
	// by PhysicsBSP's per-texture counts. A texture count cannot tell "the two
	// models subdivide the same wall differently" from "this wall only exists
	// in one of them", and only the second is a missing-geometry bug.
	//
	// So ask it geometrically. For every VisBSP polygon, is there a PhysicsBSP
	// polygon COPLANAR with it and overlapping it in space? If yes it is
	// covered, however differently the two carve it up. If no, it is a real
	// candidate for something we stopped drawing, and it is named.
	//
	// +StubBSPCover 1. A one-shot diagnostic, off by default: it walks both
	// models and costs a second at world build.
	int g_bBSPCover = 0;

	void BSPCoverReport(uint32_t pWorld)
	{
		if (!g_bBSPCover) return;
		const uint32_t pList = Word(pWorld, kWorldModels);
		const uint32_t nList = Word(pWorld, kWorldModelCount);
		if (!Readable(pList, 4) || !nList) return;

		struct Face { float c[3], n[3], d, r; uint32_t po; };
		std::vector<Face> phys, vis;

		for (int nPass = 0; nPass < 2; ++nPass)
		{
			for (uint32_t i = 0; i < nList; ++i)
			{
				const uint32_t sub = Word(Word(pList, i * 4), kSubFromElement);
				if (!Readable(sub, 0xB0)) continue;
				char szName[32];
				ModelName(sub, szName, sizeof szName);
				const bool bWant = (nPass == 0)
					? (strcmp(szName, "PhysicsBSP") == 0)
					: (strcmp(szName, "VisBSP") == 0);
				if (!bWant) continue;

				const uint32_t pPolys = Word(sub, kPolyArray);
				const uint32_t nPolys = Word(sub, kPolyCount);
				const uint32_t pVerts = Word(sub, kVertArray);
				const uint32_t nVerts = Word(sub, kVertCount);
				if (!Readable(pPolys, 4) || !nPolys || !Readable(pVerts, 12) || !nVerts)
					continue;

				for (uint32_t j = 0; j < nPolys; ++j)
				{
					const uint32_t po = Word(pPolys, j * 4);
					if (!Readable(po, kPolyVertList)) continue;
					const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + kPolyVertCount);
					if (nv < 3 || nv > 256) continue;
					if (!Readable(po + kPolyVertList, nv * kPolyVertStride)) continue;

					float p[256][3];
					bool bOK = true;
					for (uint32_t t = 0; t < nv; ++t)
					{
						const uint32_t vp = Word(po + kPolyVertList, t * kPolyVertStride);
						if (vp < pVerts || vp >= pVerts + nVerts * 12 || ((vp - pVerts) % 12))
						{ bOK = false; break; }
						memcpy(p[t], (const void*)(uintptr_t)vp, 12);
					}
					if (!bOK) continue;

					Face f{};
					for (uint32_t t = 0; t < nv; ++t)
						for (int k = 0; k < 3; ++k) f.c[k] += p[t][k];
					for (int k = 0; k < 3; ++k) f.c[k] /= (float)nv;

					const float ax = p[1][0]-p[0][0], ay = p[1][1]-p[0][1], az = p[1][2]-p[0][2];
					const float bx = p[2][0]-p[0][0], by = p[2][1]-p[0][1], bz = p[2][2]-p[0][2];
					float nx = ay*bz - az*by, ny = az*bx - ax*bz, nz = ax*by - ay*bx;
					const float len = sqrtf(nx*nx + ny*ny + nz*nz);
					if (len < 1e-4f) continue;
					f.n[0] = nx/len; f.n[1] = ny/len; f.n[2] = nz/len;
					f.d = f.n[0]*f.c[0] + f.n[1]*f.c[1] + f.n[2]*f.c[2];
					for (uint32_t t = 0; t < nv; ++t)
					{
						const float dx = p[t][0]-f.c[0], dy = p[t][1]-f.c[1], dz = p[t][2]-f.c[2];
						const float r = sqrtf(dx*dx + dy*dy + dz*dz);
						if (r > f.r) f.r = r;
					}
					f.po = po;
					(nPass == 0 ? phys : vis).push_back(f);
				}
			}
		}

		if (phys.empty() || vis.empty())
		{
			Log("  R3D BSP COVER: PhysicsBSP %u faces, VisBSP %u faces -"
				" nothing to compare", (unsigned)phys.size(), (unsigned)vis.size());
			return;
		}

		// A coarse grid on the centroid, so this is not 14000 x 6000 tests.
		const float kCell = 256.0f;
		std::map<long long, std::vector<uint32_t> > grid;
		for (uint32_t i = 0; i < phys.size(); ++i)
		{
			const long long gx = (long long)floorf(phys[i].c[0] / kCell);
			const long long gy = (long long)floorf(phys[i].c[1] / kCell);
			const long long gz = (long long)floorf(phys[i].c[2] / kCell);
			grid[(gx * 73856093LL) ^ (gy * 19349663LL) ^ (gz * 83492791LL)].push_back(i);
		}

		std::vector<char> bCovered(vis.size(), 0);
		uint32_t nCovered = 0, nBare = 0;
		double fBareArea = 0.0;
		float  fWorst[8][3] = {{0}};
		int    nWorst = 0;
		for (uint32_t v = 0; v < vis.size(); ++v)
		{
			const Face& fv = vis[v];
			bool bHit = false;
			const int nSpan = 1 + (int)(fv.r / kCell);
			for (int dx = -nSpan; dx <= nSpan && !bHit; ++dx)
			for (int dy = -nSpan; dy <= nSpan && !bHit; ++dy)
			for (int dz = -nSpan; dz <= nSpan && !bHit; ++dz)
			{
				const long long gx = (long long)floorf(fv.c[0]/kCell) + dx;
				const long long gy = (long long)floorf(fv.c[1]/kCell) + dy;
				const long long gz = (long long)floorf(fv.c[2]/kCell) + dz;
				auto it = grid.find((gx*73856093LL) ^ (gy*19349663LL) ^ (gz*83492791LL));
				if (it == grid.end()) continue;
				for (uint32_t k : it->second)
				{
					const Face& fp = phys[k];
					// Coplanar: same plane, either facing.
					const float dot = fv.n[0]*fp.n[0] + fv.n[1]*fp.n[1] + fv.n[2]*fp.n[2];
					if (fabsf(dot) < 0.99f) continue;
					if (fabsf(fabsf(fp.d) - fabsf(fv.d)) > 2.0f) continue;
					const float ex = fp.c[0]-fv.c[0], ey = fp.c[1]-fv.c[1], ez = fp.c[2]-fv.c[2];
					if (sqrtf(ex*ex + ey*ey + ez*ez) > fv.r + fp.r + 16.0f) continue;
					bHit = true; break;
				}
			}
			bCovered[v] = bHit ? 1 : 0;
			if (bHit) ++nCovered;
			else
			{
				++nBare;
				fBareArea += (double)fv.r * (double)fv.r;
				if (nWorst < 8)
				{
					fWorst[nWorst][0] = fv.c[0]; fWorst[nWorst][1] = fv.c[1];
					fWorst[nWorst][2] = fv.c[2]; ++nWorst;
				}
			}
		}

		Log("  R3D BSP COVER: PhysicsBSP %u faces, VisBSP %u faces",
			(unsigned)phys.size(), (unsigned)vis.size());
		Log("      of the VisBSP faces, %u have a COPLANAR OVERLAPPING PhysicsBSP"
			" face (%.1f%%) and %u do NOT", nCovered,
			100.0 * nCovered / (double)vis.size(), nBare);
		if (nBare)
		{
			// WHAT ARE THEY? A count of uncovered faces cannot say whether they
			// are scenery the switch to PhysicsBSP lost, or editor markers the
			// engine never drew either. The texture NAME says it outright, and
			// this project already has the six marker names.
			struct Kind { char sz[80]; uint32_t n; };
			std::vector<Kind> kinds;
			uint32_t nMarker = 0, nNoName = 0;
			for (uint32_t v = 0; v < vis.size(); ++v)
			{
				if (!vis[v].po) continue;
				if (bCovered[v]) continue;
				const uint32_t sf = Word(vis[v].po, kPolySurface);
				const uint32_t tx = Readable(sf, kSurfaceTexture + 4)
								  ? Word(sf, kSurfaceTexture) : 0;
				char szN[80] = { 0 };
				if (!tx || !TexNameOf(tx, szN, sizeof szN)) { ++nNoName; continue; }
				if (IsMarkerTexture(BaseName(szN))) { ++nMarker; continue; }
				size_t k = 0;
				for (; k < kinds.size(); ++k)
					if (_stricmp(kinds[k].sz, szN) == 0) break;
				if (k == kinds.size() && kinds.size() < 64)
				{ Kind e{}; strncpy(e.sz, szN, 79); kinds.push_back(e); }
				if (k < kinds.size()) ++kinds[k].n;
			}
			std::sort(kinds.begin(), kinds.end(),
				[](const Kind& a, const Kind& b){ return a.n > b.n; });
			Log("      of the uncovered: %u wear an editor MARKER texture (the"
				" engine never drew them either), %u could not be named, and the"
				" rest are:", nMarker, nNoName);
			for (size_t k = 0; k < kinds.size() && k < 10; ++k)
				Log("        %-40s %u", kinds[k].sz, kinds[k].n);
			Log("      first few by centroid:");
			for (int k = 0; k < nWorst; ++k)
				Log("        (%.0f %.0f %.0f)", fWorst[k][0], fWorst[k][1], fWorst[k][2]);
		}
	}


	// Defined further down, beside the model walk that needed it first.
	// 0 = not readable, 1 = a loaded module's image, 2 = private (heap).
	int MemKind(uint32_t p, size_t n);
	long long g_nMemKindCalls = 0, g_qMemKind = 0;

	// THE LIVE IDENTITY OF AN IMAGE, not of its allocation. Cheap enough to
	// run on every model skin slot every frame: two reads and eight words of
	// hash, no syscall and no allocation. See the note on TexEntry.
	bool TexLiveId(uint32_t pTex, uint32_t* pOutData,
				   uint32_t* pOutPix, uint32_t* pOutSig)
	{
		*pOutData = 0; *pOutPix = 0; *pOutSig = 0;
		if (!pTex || !Readable(pTex, 16)) return false;
		const uint32_t pData = Word(pTex, 0x08);
		if (!pData || !Readable(pData, kTexMip0 + 16)) return false;
		*pOutData = pData;
		const uint32_t pPix = Word(pData, kTexMip0 + 8);
		*pOutPix = pPix;
		if (pPix && Readable(pPix, 32))
		{
			uint32_t s = 0x811C9DC5u;
			for (int z = 0; z < 8; ++z)
				s = (s ^ Word(pPix, z * 4)) * 16777619u;
			*pOutSig = s ? s : 1u;
		}
		return true;
	}

	bool TexDims(uint32_t pTex, float* pW, float* pH)
	{
		if (!pTex) return false;

		// Out of the engine's own data object. kTexWidth is an
		// offset into THAT, not into the texture object - the texture object
		// holds a pointer to it at +8, which is exactly what engine callback 2
		// returns (lithtech.exe + 0x62270: mov eax, [ecx+8]).
		//
		// Reading +0x10 of the texture object itself instead put every one of
		// the 26904 polygons in the untextured bucket, which is how this was
		// caught rather than shipped.
		const uint32_t pData = Word(pTex, 8);
		if (!Readable(pData, kTexHeight + 2)) return false;
		const uint8_t* d = (const uint8_t*)(uintptr_t)pData;
		const uint32_t w = *(const uint16_t*)(d + kTexWidth);
		const uint32_t h = *(const uint16_t*)(d + kTexHeight);
		if (!w || !h || w > 4096 || h > 4096) return false;
		*pW = (float)w; *pH = (float)h;
		return true;
	}

	// ---- a little matrix maths, row-vector convention -----------------------
	// v' = v * M, which is what the shader below does with mul(float4, matrix).

	void QuatBasis(const float* q, float* r, float* u, float* f)
	{
		// q is {x, y, z, w}. Identified in docs/SCENEDESC.md and confirmed by
		// the quick save spawning at body yaw 270, which is the -90 degree Y
		// rotation (0, -0.7071, 0, 0.7071) the scene description carries.
		const float x = q[0], y = q[1], z = q[2], w = q[3];
		const float xx = x * x, yy = y * y, zz = z * z;
		const float xy = x * y, xz = x * z, yz = y * z;
		const float wx = w * x, wy = w * y, wz = w * z;
		r[0] = 1 - 2 * (yy + zz); r[1] = 2 * (xy + wz);     r[2] = 2 * (xz - wy);
		u[0] = 2 * (xy - wz);     u[1] = 1 - 2 * (xx + zz); u[2] = 2 * (yz + wx);
		f[0] = 2 * (xz + wy);     f[1] = 2 * (yz - wx);     f[2] = 1 - 2 * (xx + yy);
	}

	// The projection, from four tangents.
	//
	// Row-vector convention throughout: clip = view * P, which is what the
	// shader's mul(float4, matrix) does. For an off-centre frustum,
	//
	//   ndc.x = (2*vx/vz - (tR+tL)) / (tR-tL)
	//
	// so clip.x = sx*vx - ox*vz with sx = 2/(tR-tL) and ox = (tR+tL)/(tR-tL),
	// and the offset lives in the z ROW because vz multiplies row 2.
	//
	// The symmetric case falls straight out: tR = -tL = t gives sx = 1/t and
	// ox = 0, which is the expression this replaced. So the two paths are one
	// path, and there is no separate symmetric code to drift out of step.
	void Frustum(float tl, float tr, float tu, float td,
				 float zn, float zf, float* pr)
	{
		const float sx = 2.0f / (tr - tl);
		const float sy = 2.0f / (tu - td);
		const float ox = (tr + tl) / (tr - tl);
		const float oy = (tu + td) / (tu - td);

		pr[0]  = sx;   pr[1]  = 0;    pr[2]  = 0;                     pr[3]  = 0;
		pr[4]  = 0;    pr[5]  = sy;   pr[6]  = 0;                     pr[7]  = 0;
		pr[8]  = -ox;  pr[9]  = -oy;  pr[10] = zf / (zf - zn);        pr[11] = 1;
		pr[12] = 0;    pr[13] = 0;    pr[14] = -zn * zf / (zf - zn);  pr[15] = 0;
	}

	void ViewProj(const float* p, const float* q,
				  float fovX, float fovY, float zn, float zf, float* m,
				  const float* pTan4)
	{
		float r[3], u[3], f[3];
		QuatBasis(q, r, u, f);

		// LithTech is left-handed with Y up, so this is a look-to LH view.
		float v[16] = {
			r[0], u[0], f[0], 0,
			r[1], u[1], f[1], 0,
			r[2], u[2], f[2], 0,
			-(p[0]*r[0] + p[1]*r[1] + p[2]*r[2]),
			-(p[0]*u[0] + p[1]*u[1] + p[2]*u[2]),
			-(p[0]*f[0] + p[1]*f[1] + p[2]*f[2]), 1
		};

		// Reversed-Z is the SAME matrix with the two planes exchanged: the
		// depth row is zf/(zf-zn) and -zn*zf/(zf-zn), and swapping the
		// arguments turns that into zn/(zn-zf) and -zf*zn/(zn-zf), which is
		// exactly the reversed mapping. Writing it as an argument swap rather
		// than a second matrix means there is only one place for a sign to be
		// wrong.
		const float za = g_bRevZ ? zf : zn;
		const float zb = g_bRevZ ? zn : zf;

		float pr[16];
		if (pTan4)
			Frustum(pTan4[0], pTan4[1], pTan4[2], pTan4[3], za, zb, pr);
		else
		{
			// The scene description carries FULL angles (docs/SCENEDESC.md:
			// 2.43349 radians == 139.43 degrees), so the half-angle is what
			// goes in here.
			const float tx = tanf(fovX * 0.5f);
			const float ty = tanf(fovY * 0.5f);
			Frustum(-tx, tx, ty, -ty, za, zb, pr);
		}

		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
			{
				float s = 0;
				for (int k = 0; k < 4; ++k) s += v[i * 4 + k] * pr[k * 4 + j];
				m[i * 4 + j] = s;
			}
	}

	// Does this projection actually send the frustum's edges to the edges of
	// the screen?
	//
	// The four edge rays are (tL,0,1), (tR,0,1), (0,tU,1) and (0,tD,1) in view
	// space, and a correct projection sends them to ndc -1, +1, +1 and -1. The
	// answer is known in advance, so this needs no picture and no headset -
	// which matters, because the failure it catches is a flipped offset sign,
	// and the project's own notes record that as reading like ordinary double
	// vision rather than like a bug.
	void CheckFrustum(float tl, float tr, float tu, float td,
					  float zn, float zf, const char* pszWhat)
	{
		float pr[16];
		Frustum(tl, tr, tu, td, zn, zf, pr);

		// Row-vector: clip = view * P, with view.z = 1 for every edge ray.
		auto NdcX = [&](float vx, float vy) {
			const float cx = vx * pr[0] + vy * pr[4] + 1.0f * pr[8];
			const float cw = vx * pr[3] + vy * pr[7] + 1.0f * pr[11];
			return cw != 0.0f ? cx / cw : 0.0f; };
		auto NdcY = [&](float vx, float vy) {
			const float cy = vx * pr[1] + vy * pr[5] + 1.0f * pr[9];
			const float cw = vx * pr[3] + vy * pr[7] + 1.0f * pr[11];
			return cw != 0.0f ? cy / cw : 0.0f; };

		const float xl = NdcX(tl, 0.0f), xr = NdcX(tr, 0.0f);
		const float yu = NdcY(0.0f, tu), yd = NdcY(0.0f, td);
		const float e = fabsf(xl + 1.0f) + fabsf(xr - 1.0f)
					  + fabsf(yu - 1.0f) + fabsf(yd + 1.0f);

		Log("  R3D: %s  tan L%+.4f R%+.4f U%+.4f D%+.4f", pszWhat, tl, tr, tu, td);
		Log("       the four edge rays land at ndc x %+.5f %+.5f, y %+.5f %+.5f",
			xl, xr, yd, yu);
		Log("       FRUSTUM SELF-CHECK: %s (total error %.6f)",
			(e < 1e-4f) ? "PASSED - the frustum's edges are the screen's edges"
						: "FAILED - this projection does not show that frustum",
			e);
	}

	const char* kShader =
		"cbuffer CB : register(b0) { row_major float4x4 mvp; float4 lmp; };\n"
		// fogc.rgb is the colour, fogc.w is 0 or 1. fogp.x/y are the near and
		// far distances the level author set, in world units.
		"cbuffer FOG : register(b1) { float4 fogc; float4 fogp; };\n"
		// mlight.rgb is the light this MODEL stands in, read out of the
		// level's own lightmaps at build time; mlight.w is 0 for everything
		// that is not a model.
		"cbuffer MDL : register(b2) { float4 mlight; };\n"
		// The eight nearest dynamic lights of this pass: dlp.xyz position,
		// dlp.w radius, dlc.rgb colour, dln.x how many.
		"cbuffer DYN : register(b4) { float4 dlp[8]; float4 dlc[8]; float4 dln; };\n"
		"Texture2D    tex0 : register(t0);\n"
		"Texture2D    lmap : register(t1);\n"
		// The level's light grid: gmin.xyz is its origin, ginv.xyz one over
		// its extent, ginv.w 1 when there is one. Sampled per pixel for world
		// polygons with no lightmap (luv.y = -3): terrain and water.
		// lscale.rgb is the engine's global light scale, applied to every
		// lit pixel the way the retail renderer applied it.
		"cbuffer GRID : register(b5) { float4 gmin; float4 ginv; float4 lscale; };\n"
		// wat.x: how far this face's own texture has scrolled DOWN, in repeats.
		// Non-zero only for the vertical faces of a water volume - the
		// waterfall. See the glass pass.
		"cbuffer WAT : register(b7) { float4 wat; };\n"
		// The environment pass: envr/envu the camera's right and up, envc its
		// position, envp.xy the pan, envp.z the scale, envp.w 1 during the pass.
		"cbuffer ENV : register(b6) { float4 envr; float4 envu; float4 envc; float4 envp; };\n"
		"Texture3D    lgrid : register(t2);\n"
		"SamplerState samp : register(s0);\n"
		"struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL;"
		"               float2 uv : TEXCOORD0; float2 luv : TEXCOORD1; };\n"
		"struct VSOut { float4 pos : SV_POSITION; float3 nrm : NORMAL;"
		"               float2 uv : TEXCOORD0; float2 luv : TEXCOORD1;"
		"               float fogz : TEXCOORD2; float3 wp : TEXCOORD3; };\n"
		"VSOut VSMain(VSIn i) {\n"
		"  VSOut o; o.pos = mul(float4(i.pos, 1.0f), mvp); o.nrm = i.nrm;\n"
		"  o.uv = i.uv + float2(0.0f, wat.x); o.luv = i.luv; o.wp = i.pos;\n"
		// DISTANCE FROM THE EYE, which a perspective matrix has already put in
		// w. No second transform and no extra matrix: this is the same number
		// the hardware divides by.
		"  o.fogz = o.pos.w;\n"
		"  return o;\n"
		"}\n"
		// The lightmap where there is one, and the old stand-in normal shade
		// where there is not. Keeping the fallback visible matters: an unlit
		// polygon drawn black is indistinguishable from a hole, and there are
		// 7278 of them - sky, water and brush objects among them.
		//
		// lmp.x is the lightmap scale, and it is 1. Four lightmaps opened by
		// hand all topped out at 15 of 31, which reads as half-scale data
		// meant to be doubled. Counted over the whole level the largest
		// channel value is 31 of 31, so the four were not representative.
		// +StubLMScale100 moves it without a rebuild.
		// THE SURFACE'S OWN LIGHT, lifted out of PSMain so the ENVIRONMENT
		// PASS can multiply by the very number the base pass used rather than
		// a second copy of it that drifts. Nothing about the computation
		// changed in the lifting; it is the same branches in the same order.
		"float3 SurfaceLight(VSOut i) {\n"
		"  float3 l;\n"
		// -2 IS THE SKY AND SPRITES: no lighting at all. Six faces of a
		// skybox have six normals, so lighting them puts a hard edge across
		// the sky. -1 is the old "no lightmap, use the stand-in shade".
		// -4 IS A STORED VERTEX COLOUR, riding in the normal slot: the light
		// the level processor computed for a polygon with no lightmap. See
		// g_bVertexColour.
		// lmp.y below zero: ONE FLAT LIGHT for this draw, lmp.x. Water.
		"  if (lmp.y < -0.5f) {\n"
		"    l = lmp.x;\n"
		// -5 is the SKY's vertex colour: a photograph, never scaled. -4 is a
		// world polygon's baked gouraud colour, written by the same compiler
		// at the same half scale as the lightmaps, so it takes the same 2x:
		// the HQ dome lights are tinted facets at 1x and retail's flat white
		// at 2x (19 Sep).
		"  } else if (i.luv.x < -4.5f) {\n"
		"    l = i.nrm;\n"
		"  } else if (i.luv.x < -3.5f) {\n"
		"    l = i.nrm * lmp.x;\n"
		"  } else if (i.luv.x < -1.5f) {\n"
		"    l = 1.0f;\n"
		// A WORLD POLYGON WITH NO LIGHTMAP takes the level's light where it
		// stands, per pixel, scaled like a lightmap so it matches the brush
		// next to it. Terrain by night is dark because the level is.
		"  } else if (i.luv.x < 0.0f && i.luv.y < -2.5f && ginv.w > 0.5f) {\n"
		"    float3 gc = saturate((i.wp - gmin.xyz) * ginv.xyz);\n"
		"    l = lgrid.Sample(samp, gc).rgb * lmp.x;\n"
		"  } else if (i.luv.x < 0.0f) {\n"
		"    float3 d = normalize(float3(0.35f, 0.85f, 0.40f));\n"
		// THE MODEL'S OWN LIGHT, when there is one. The directional term stays
		// - weaker, 0.65 to 1.0 - because a model lit by one flat colour loses
		// its form and reads as a cardboard cut-out. What changes is that the
		// LEVEL now decides the level and the colour, so a character in a warm
		// lobby is warm and one in a cellar is dark.
		"    if (mlight.w > 0.5f) {\n"
		"      l = mlight.rgb * lmp.x"
		"          * (0.65f + 0.35f * saturate(dot(normalize(i.nrm), d)));\n"
		"    } else {\n"
				"    // NO abs(). It made brightness rise on BOTH sides of\n"
		"    // perpendicular, so a smoothly curving surface got a hard V\n"
		"    // crease wherever it turned past the light - which reads as\n"
		"    // faceting on a low-poly character however good its normals\n"
		"    // are. 99.94% of triangles were already using real per-vertex\n"
		"    // normals when this was found.\n"
		"    l = 0.55f + 0.45f * saturate(dot(normalize(i.nrm), d));\n"
		"    }\n"
		"  } else {\n"
		"    l = lmap.Sample(samp, i.luv).rgb * lmp.x;\n"
		"  }\n"
		"  return l;\n"
		"}\n"
		"float4 PSMain(VSOut i) : SV_TARGET {\n"
		// THE ENVIRONMENT PASS. The face normal from the position's
		// derivatives (a vertex-coloured polygon carries a colour in its
		// normal slot), the view reflected in it, and that direction's x and
		// y in the camera's frame as the map's coordinate, panned.
		"  if (envp.w > 0.5f) {\n"
		// ANCHORED TO THE WORLD, NOT THE EYE. The map used to be looked up by
		// the reflected view direction, so it slid across every surface as the
		// head turned - a mirror reflection that moved with the head
		// - where retail's EnvMapWorld holds still. The
		// surface's own frame from its normal: t along it, b up it; the map is
		// a planar texture in that frame, envr.w and envu.w repeats per unit,
		// and envp.y runs it down the face.
		"    float3 n = normalize(cross(ddx(i.wp), ddy(i.wp)));\n"
		// ONLY A STANDING FACE - the waterfall. On the pool's flat top the same
		// map drew as bright stripes running away from the eye and hid the
		// grid's own wave. The flat water keeps its
		// texture and its grid; the sheet above it keeps the flowing map.
		// WATER (envc.w = 1): the planar, world-anchored, flowing map, on a
		// standing face only. EVERYTHING ELSE: the soft sheen retail shows on
		// the HQ's black marble statue, its reception glass and its camera
		// housings (a 14 September screenshots) - a sphere map looked up by
		// the reflected view direction, added quietly, never scrolled. It
		// moves with the head the way any reflection does; the reported
		// weird movement was last night's scrolling, which is gone.
		"    float2 euv;\n"
		// ONE COPY OF THE MAP ACROSS THE WHOLE SURFACE, looked up by the
		// reflected view direction - retail's way, and the reason its streaks
		// are broad and never repeat. The planar tiling that stood here drew
		// a repeated, looping image with the same streaks in a row (headset
		// testing, 14 September). Water (envc.w = 1) pans the lookup DOWN the face on
		// top of it; everything else is still.
		"    float3 v = normalize(i.wp - envc.xyz);\n"
		"    float3 rr = reflect(v, n);\n"
		"    euv = float2(dot(rr, envr.xyz), -dot(rr, envu.xyz)) * 0.5f + 0.5f;\n"
		// envp.x: 1 for WATER only - the flowing pan, and standing faces only.
		"    if (envp.x > 0.5f) {\n"
		// A FLAT FACE IS FLAT BY ITS OWN HEIGHT, NOT BY A DERIVED NORMAL. The
		// normal above comes from the screen-space derivatives of the world
		// position, and at the far edge of the Dive's sea - a 15000-unit face
		// seen at a grazing angle from the boat - those derivatives are
		// hundreds of units per pixel and the cross product they give is
		// noise: |n.y| drops under 0.5 on patches of the flat top and the
		// flowing map, meant for standing water only, was drawn on them:
		// wedge-shaped patches at the horizon that moved with the head (a headset
		// clip and the desk, 22 September; +StubEnvMap 0 removed them,
		// +StubWaterBias -500 did not). A flat face has NO change of world
		// height across the screen whatever its distance, so that is the
		// test: the height derivative against the horizontal ones.
		"      float dyy = abs(ddx(i.wp.y)) + abs(ddy(i.wp.y));\n"
		"      float dhh = abs(ddx(i.wp.x)) + abs(ddx(i.wp.z)) + abs(ddy(i.wp.x)) + abs(ddy(i.wp.z));\n"
		"      if (abs(n.y) > 0.5f || dyy < 0.05f * dhh) discard;\n"
		"      euv.y += envp.y;\n"
		"    }\n"
		// envc.w: 0 adds the map at envp.z; 1 MODULATES - the pass is drawn
		// with 2*src*dest, so 0.5 leaves the sheet alone and envp.z is how
		// far the map's light and dark swing from it.
		// WATER SHIMMERS IN PLACE. A close retail clip (14 September, 60 fps):
		// the sheet changes fast - half its eventual difference in 0.17 s -
		// and yet its pattern does not travel (best vertical shift a few
		// pixels either way). Two copies of the map, one running down and one
		// up at a different scale, averaged: the streaks flicker and churn
		// without the whole sheet sliding.
		"    float3 e = tex0.Sample(samp, euv).rgb;\n"
		"    if (envc.w > 0.5f) {\n"
		// ONE COPY, ONE DIRECTION. This sampled the map twice - one running
		// down and one up at a different scale, averaged - on a 14 September
		// reading that the retail sheet "changes fast and yet its pattern does
		// not travel". That came from frame-to-frame cross-correlation, and a
		// waterfall is long vertical streaks: slide it vertically and it
		// correlates with itself at every offset, so the instrument reports no
		// travel for a sheet that plainly travels - a space-time image of the
		// same clip shows the diagonals outright. The churn was reproducing an
		// artefact of the measurement, and headset testing named it on 17 September:
		// the waterfall had a sheen or reflection going in different
		// directions. Two directions, because there were two copies.
		"      return float4(lerp(float3(0.5f, 0.5f, 0.5f), e, envp.z), 1.0f);\n"
		"    }\n"
		// A REFLECTION IS LIT BY THE ROOM, NOT ADDED TO IT.
		//
		// This returned e * envp.z into an ADDITIVE blend with no reference to
		// the surface's lighting at all, so every shiny face in the level got
		// the same flat lift whether it stood in a spotlight or in shadow. On a
		// dark surface that lift is the whole picture: the HQ lobby's Unity
		// statue (Tex\Shiny\ShMt01\ShMt0035, which names envmap014) measured
		// 88 here against retail's 25-45, and switching the layer off alone
		// took it to 46 - the added term was bigger than retail's entire
		// statue. Across the whole frame it was worth 16 levels of 88, which is
		// what headset testing reported on 17 September: the entire headquarters
		// was very bright in ours, and the marble Unity statue was much darker
		// in retail than in ours.
		//
		// Multiplied by the surface light, a reflection brightens the glass and
		// the camera housings that stand in the light - the sheen confirmed in the headset
		// on 14 September - and leaves a statue in shadow dark. Saturated
		// because lmp.x can carry the lightmap past one: full light is as much
		// reflection as the layer ever had, never more.
		//
		// WATER IS NOT THIS BRANCH. It returns above, through the modulate-2x
		// path, and is untouched.
		"    return float4(e * envp.z * saturate(SurfaceLight(i)), 1.0f);\n"
		"  }\n"
		"  if (lmp.y > 1.5f && lmp.y < 2.5f)\n"
		"    return float4(frac(i.luv.x * 8.0f), frac(i.luv.y * 8.0f), 0, 1);\n"
		// +StubLMOnly 4: THE ROUTE, as a colour. Magenta = a vertex colour
		// (-4/-5), cyan = unlit (-2), yellow = the stand-in (-1), green = the
		// grid (-1,-3), white = a lightmap. What the PIXEL sees, not the CPU.
		"  if (lmp.y > 3.5f) {\n"
		"    if (i.luv.x < -3.5f) return float4(1,0,1,1);\n"
		"    if (i.luv.x < -1.5f) return float4(0,1,1,1);\n"
		"    if (i.luv.x < 0.0f && i.luv.y < -2.5f) return float4(0,1,0,1);\n"
		"    if (i.luv.x < 0.0f) return float4(1,1,0,1);\n"
		"    return float4(1,1,1,1);\n"
		"  }\n"
		"  if (lmp.y > 2.5f)\n"
		"    return float4(lmap.Sample(samp, i.uv).rgb, 1);\n"
		"  float4 t0 = tex0.Sample(samp, i.uv);\n"
		// A CUT-OUT IS CUT OUT WHEREVER IT IS DRAWN.
		//
		// This test used to sit inside two of the three lighting branches, and
		// world geometry takes the third - the lightmapped one. So the engine
		// measured NOLF's tree as a cut-out, the renderer agreed, and then drew
		// it as a flat green rectangle with a branch painted on it, because the
		// only branch that could have discarded the clear texels was one the
		// polygon never reached. Same fault, same frame: the intro's power cable
		// as a grey slab across the sky.
		//
		// lmp.z is zero unless the batch about to be drawn named a cut-out texture,
		// so solid geometry is not affected and pays one compare.
		"  if (lmp.z > 0.0f) {\n"
		"    if (fogp.z > 0.5f) {\n"
		"      float a2 = (t0.a - lmp.z) / max(fwidth(t0.a), 0.0001f) + 0.5f;\n"
		"      if (a2 <= 0.0f) discard;\n"
		"      t0.a = saturate(a2);\n"
		"    } else if (t0.a < lmp.z) discard;\n"
		"  }\n"
		"  float3 c = (lmp.y > 0.5f) ? float3(1,1,1) : t0.rgb;\n"
		"  float3 l = SurfaceLight(i);\n"
		// THE TEXTURE'S OWN ALPHA, multiplied into the pass alpha. A pane
		// of glass is not a uniform film of milk: its texture says where it
		// is clear and where it is dirty, and ignoring that made the HQ
		// reception window opaque white with the receptionist barely
		// visible behind it. Only the blended pass looks at alpha at all,
		// so this costs nothing anywhere else.
		"  float3 outc = c * l * lscale.rgb;\n"
		// DYNAMIC LIGHTS ON THE WORLD. Lightmapped polygons only (luv >= 0):
		// a model's share is summed per instance into mlight, and the sky
		// and sprites take no light at all. Linear falloff, as the level's
		// own lights are summed for models, and a half-Lambert normal term
		// so a lamp does not light the floor and ceiling alike.
		// ...and on the grid-lit terrain too: a muzzle flash lights the sand.
		// NOT on a vertex-coloured polygon (-4): retail added dynamic lights
		// into the lightmap pages, so a gouraud-lit brush never saw one. The
		// aeroplane cabin (13 September, headset testing): the three projector lamps
		// threw huge hard-edged colour blotches over walls retail leaves white.
		"  if (dln.x > 0.5f && (i.luv.x >= 0.0f || (i.luv.y < -2.5f && i.luv.x > -3.5f))) {\n"
		"    float3 dl = float3(0,0,0);\n"
		"    [unroll] for (int k = 0; k < 8; ++k) {\n"
		"      if (k < (int)dln.x) {\n"
		"        float3 dv = dlp[k].xyz - i.wp; float d = length(dv);\n"
		"        float f = saturate(1.0f - d / dlp[k].w);\n"
		"        float nd = (i.luv.x < -3.5f) ? 0.75f\n"
		"                 : 0.5f + 0.5f * saturate(dot(normalize(i.nrm), dv / max(d, 1e-3f)));\n"
		"        dl += dlc[k].rgb * f * nd;\n"
		"      }\n"
		"    }\n"
		"    outc += c * dl * lmp.x;\n"
		"  }\n"
		// MULTIPLY DECALS, FLAGGED BY A NEGATIVE lmp.w.
		//
		// lmp.w is an output alpha and has only ever held 0..1, so its sign
		// was free - the same trick lmp.z and luv.x already play in this
		// shader. The blend state for these is DEST_COLOR x ZERO, which
		// ignores alpha completely, so a splat the game is fading would
		// otherwise sit at full strength and then pop off. WHITE is this
		// blend's "nothing", so the fade is a lerp TOWARDS WHITE, not
		// towards transparent. Returns before the fog block on purpose:
		// BaseScaleFX clears bFog for exactly these objects.
		"  if (lmp.w < 0.0f) {\n"
		"    outc = lerp(float3(1,1,1), outc, saturate(-lmp.w));\n"
		"    return float4(outc, 1.0f);\n"
		"  }\n"
		// LINEAR FOG, the same curve LithTech asked D3D7 for. Full colour at
		// the near distance, fully fogged at the far one. The SKYBOX is
		// excluded (luv.x < -1.5) because it is a backdrop painted at infinity:
		// fogging it would flatten the whole panorama to one colour.
		// fogp.w is 1 during the sky pass when the level asks for sky fog.
		"  if (fogc.w > 0.5f && (i.luv.x > -1.5f || (i.luv.x < -3.5f && i.luv.x > -4.5f) || fogp.w > 0.5f)) {\n"
		"    float fz = saturate((fogp.y - i.fogz)"
		"                        / max(fogp.y - fogp.x, 1.0f));\n"
		"    outc = lerp(fogc.rgb, outc, fz);\n"
		"  }\n"
		"  return float4(outc, lmp.w * t0.a);\n"
		"}\n";

	bool Compile(const char* pszEntry, const char* pszModel, ID3DBlob** ppOut)
	{
		ID3DBlob* pErr = nullptr;
		const HRESULT hr = D3DCompile(kShader, strlen(kShader), nullptr, nullptr,
									  nullptr, pszEntry, pszModel, 0, 0, ppOut, &pErr);
		if (FAILED(hr))
		{
			Log("  R3D: %s failed to compile hr=%08X: %s", pszEntry, (unsigned)hr,
				pErr ? (const char*)pErr->GetBufferPointer() : "(no message)");
			if (pErr) pErr->Release();
			return false;
		}
		if (pErr) pErr->Release();
		return true;
	}
}

// Defined further down, beside the world-load logging it was written for.
static void LogAddressSpace(const char* pszWhen);

bool R3D_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx, R3D_LogFn pfnLog)
{
	Log = pfnLog; g_pDev = pDev; g_pCtx = pCtx;
	if (!pDev || !pCtx) { if (Log) Log("  R3D: no device"); return false; }
	// The other end of the bracket in R3D_Destroy. Reload N's "AFTER" and
	// reload N+1's "renderer created" should be the same number; a gap between
	// them is address space the process lost while no renderer existed at all,
	// which would put the leak outside this file.
	LogAddressSpace("renderer created");

	ID3DBlob *pVS = nullptr, *pPS = nullptr;
	if (!Compile("VSMain", "vs_4_0", &pVS)) return false;
	if (!Compile("PSMain", "ps_4_0", &pPS)) { pVS->Release(); return false; }

	HRESULT hr = pDev->CreateVertexShader(pVS->GetBufferPointer(),
										  pVS->GetBufferSize(), nullptr, &g_pVS);
	Log("  R3D: CreateVertexShader hr=%08X", (unsigned)hr);
	hr = pDev->CreatePixelShader(pPS->GetBufferPointer(),
								 pPS->GetBufferSize(), nullptr, &g_pPS);
	Log("  R3D: CreatePixelShader hr=%08X", (unsigned)hr);

	const D3D11_INPUT_ELEMENT_DESC kIL[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	hr = pDev->CreateInputLayout(kIL, 4, pVS->GetBufferPointer(),
								 pVS->GetBufferSize(), &g_pIL);
	Log("  R3D: CreateInputLayout hr=%08X", (unsigned)hr);
	pVS->Release(); pPS->Release();

	D3D11_BUFFER_DESC cb{};
	cb.ByteWidth = 80; cb.Usage = D3D11_USAGE_DYNAMIC;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = pDev->CreateBuffer(&cb, nullptr, &g_pCB);
	Log("  R3D: constant buffer hr=%08X", (unsigned)hr);

	D3D11_BUFFER_DESC cbf{};
	cbf.ByteWidth = 32; cbf.Usage = D3D11_USAGE_DYNAMIC;
	cbf.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbf.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = pDev->CreateBuffer(&cbf, nullptr, &g_pCBFog);
	g_bFogDirty = 1;
	Log("  R3D: fog constant buffer hr=%08X", (unsigned)hr);

	D3D11_BUFFER_DESC cbm{};
	cbm.ByteWidth = 16; cbm.Usage = D3D11_USAGE_DYNAMIC;
	cbm.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbm.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = pDev->CreateBuffer(&cbm, nullptr, &g_pCBMdl);
	g_fMdlSet[0] = -1.0f;
	{
		D3D11_BUFFER_DESC cbd{};
		cbd.ByteWidth = 16 * 17; cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = pDev->CreateBuffer(&cbd, nullptr, &g_pCBDyn);
		Log("  R3D: dynamic-light constant buffer hr=%08X", (unsigned)hr);
	}
	{
		D3D11_BUFFER_DESC cbg{};
		cbg.ByteWidth = 48; cbg.Usage = D3D11_USAGE_DYNAMIC;
		cbg.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbg.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = pDev->CreateBuffer(&cbg, nullptr, &g_pCBGrid);
		g_bGridDirty = 1;
		Log("  R3D: light-grid constant buffer hr=%08X", (unsigned)hr);
	}
	{
		D3D11_BUFFER_DESC cbe{};
		cbe.ByteWidth = 64; cbe.Usage = D3D11_USAGE_DYNAMIC;
		cbe.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbe.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = pDev->CreateBuffer(&cbe, nullptr, &g_pCBEnv);
		Log("  R3D: environment-map constant buffer hr=%08X", (unsigned)hr);
	}
	{
		D3D11_BUFFER_DESC cbw{};
		cbw.ByteWidth = 16; cbw.Usage = D3D11_USAGE_DYNAMIC;
		cbw.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbw.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = pDev->CreateBuffer(&cbw, nullptr, &g_pCBWater);
		Log("  R3D: water-flow constant buffer hr=%08X", (unsigned)hr);
	}
	Log("  R3D: model-light constant buffer hr=%08X", (unsigned)hr);

	// Two depth states, so reversed-Z is a switch and not a rebuild.
	//
	// The scene description hands us near 0.44 and far 100000 - a 225,000:1
	// range. On a conventional near->0 mapping almost the whole of a float's
	// precision is spent within a few units of the eye, and the geometry that
	// is actually being looked at gets what is left. That is the textbook
	// cause of the streaks, triangles and Venetian-blind patterns the tester
	// reported, which are what Z-fighting looks like and are NOT what a
	// texture fault looks like.
	//
	// Reversed-Z maps near->1 and far->0. Because floats are dense near 0 and
	// the far plane is now there, the precision lands where the geometry is.
	// It costs one comparison flag and one clear value.
	D3D11_DEPTH_STENCIL_DESC ds{};
	ds.DepthEnable = TRUE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	ds.DepthFunc = D3D11_COMPARISON_LESS;
	hr = pDev->CreateDepthStencilState(&ds, &g_pDSS);
	Log("  R3D: depth state (LESS) hr=%08X", (unsigned)hr);

	ds.DepthFunc = D3D11_COMPARISON_GREATER;
	hr = pDev->CreateDepthStencilState(&ds, &g_pDSSRev);
	Log("  R3D: depth state (GREATER, reversed-Z) hr=%08X", (unsigned)hr);

	// The backdrop's state: no test, no write. It is drawn first and must lose
	// to everything, including to itself - a skybox is a closed box and its far
	// faces are behind its near ones, so a depth test with nothing in the buffer
	// would keep whichever half happened to be submitted first.
	{
		D3D11_DEPTH_STENCIL_DESC dn{};
		dn.DepthEnable = FALSE;
		dn.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dn.DepthFunc = D3D11_COMPARISON_ALWAYS;
		hr = pDev->CreateDepthStencilState(&dn, &g_pDSSNoDepth);
		Log("  R3D: depth state (none, for the skybox) hr=%08X", (unsigned)hr);
	}
	{
		// Glass: test against the world, do not WRITE. Two panes of glass then
		// both show, and nothing behind a pane is rejected by it.
		D3D11_DEPTH_STENCIL_DESC dw{};
		dw.DepthEnable = TRUE;
		dw.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		dw.DepthFunc = g_bRevZ ? D3D11_COMPARISON_GREATER : D3D11_COMPARISON_LESS;
		hr = pDev->CreateDepthStencilState(&dw, &g_pDSSNoWrite);
		dw.DepthFunc = g_bRevZ ? D3D11_COMPARISON_GREATER_EQUAL
							   : D3D11_COMPARISON_LESS_EQUAL;
		pDev->CreateDepthStencilState(&dw, &g_pDSSEnv);
		dw.DepthFunc = g_bRevZ ? D3D11_COMPARISON_GREATER : D3D11_COMPARISON_LESS;
		Log("  R3D: depth state (test, no write - for glass) hr=%08X", (unsigned)hr);

		D3D11_BLEND_DESC ba{};
		ba.RenderTarget[0].BlendEnable = TRUE;
		ba.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
		ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		ba.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
		ba.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
		ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		ba.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
		ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		hr = pDev->CreateBlendState(&ba, &g_pBlendAlpha);
		Log("  R3D: blend state (alpha, for glass) hr=%08X", (unsigned)hr);

		// Additive, for FLAG2_ADDITIVE models: source and destination both
		// at full weight, so the result can only get brighter.
		ba.RenderTarget[0].SrcBlend  = D3D11_BLEND_ONE;
		ba.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		hr = pDev->CreateBlendState(&ba, &g_pBlendAdd);
		// Modulate 2x, for water's reflection map: 2 * src * dest.
		ba.RenderTarget[0].SrcBlend  = D3D11_BLEND_DEST_COLOR;
		ba.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
		hr = pDev->CreateBlendState(&ba, &g_pBlendMod2x);
		Log("  R3D: blend state (modulate 2x, for water) hr=%08X", (unsigned)hr);
		Log("  R3D: blend state (additive, for the menu motifs) hr=%08X", (unsigned)hr);

		// NO COLOUR AT ALL: depth-only, for the sky brushes. See g_bSkyOccluder.
		{
			D3D11_BLEND_DESC bn{};
			bn.RenderTarget[0].BlendEnable = FALSE;
			bn.RenderTarget[0].RenderTargetWriteMask = 0;
			hr = pDev->CreateBlendState(&bn, &g_pBlendNoColour);
			Log("  R3D: blend state (no colour, for the sky occluders) hr=%08X", (unsigned)hr);
		}

		// ADDITIVE, SCALED BY THE SPRITE'S OWN ALPHA.
		//
		// ONE/ONE adds the texture at full strength no matter what the object
		// says, and a sprite's alpha is exactly how NOLF dims a glow: the van's
		// headlights are a faint corona the author set low, and added flat they
		// come out as the big light bursts reported in the headset on the snow cutscene.
		// The menu motifs were tuned against the flat state and keep it; only
		// the sprite pass uses this one.
		ba.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
		ba.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
		hr = pDev->CreateBlendState(&ba, &g_pBlendAddMod);
		Log("  R3D: blend state (additive x alpha, for sprites) hr=%08X", (unsigned)hr);

		// MULTIPLY, for FLAG2_MULTIPLY. DEST_COLOR x SRC_COLOR: white leaves
		// the scene alone, dark stains it. Alpha is left at ONE/ZERO like the
		// others; these textures have no meaningful alpha channel.
		ba.RenderTarget[0].SrcBlend  = D3D11_BLEND_DEST_COLOR;
		ba.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
		hr = pDev->CreateBlendState(&ba, &g_pBlendMul);
		Log("  R3D: blend state (multiply, for marks and blood) hr=%08X", (unsigned)hr);
	}

	// CULLING IS THE REAL FIX FOR THE COINCIDENT-BRUSH FLICKER, and it is
	// still off by default because the winding order has never been measured.
	//
	// M01S01's Canopy_a and Canopy_b share all 26 vertex positions with
	// exactly negated normals - one two-sided awning built as two back-to-back
	// brushes. Retail culls backfaces, so exactly one of them faces the camera
	// and draws. We cull nothing, so BOTH draw, coplanar, and fight for every
	// pixel: that is the flashing canopy reported in the headset. Hiding the brushes
	// was treating the symptom, and it deleted the awning outright.
	//
	// +StubCull 1 culls back faces, 2 culls front. Guessing this wrong removes
	// half the world in a way that looks exactly like a broken matrix, so it
	// is a switch to be measured across levels before it becomes a default.
	D3D11_RASTERIZER_DESC rs{};
	rs.FillMode = D3D11_FILL_SOLID;
	rs.CullMode = (g_nCullMode == 1) ? D3D11_CULL_BACK
				: (g_nCullMode == 2) ? D3D11_CULL_FRONT
									 : D3D11_CULL_NONE;
	rs.DepthClipEnable = TRUE;
	// WITHOUT THIS, MSAA SMOOTHS NOTHING ON A POLYGON EDGE. The samples exist
	// and get resolved, but the rasteriser fills whole pixels, so the only
	// thing multisampling buys is a slower path to the same staircase.
	rs.MultisampleEnable = TRUE;
	hr = pDev->CreateRasterizerState(&rs, &g_pRS);
	// WATER CULLS ITS BACK FACES. Retail's world polygons are one-sided, so the
	// far side and the FLOOR of a water box are never drawn; ours draws every
	// face twice over, and the pool's floor - 14 units under its surface
	// grid - read as a flat piece like glass with a texture on top (headset
	// testing, 14 September) with the grid's wave lost against it.
	{
		D3D11_RASTERIZER_DESC rw = rs;
		rw.CullMode = D3D11_CULL_BACK;
		rw.DepthBias = g_nWaterBias;
		rw.SlopeScaledDepthBias = (float)g_nWaterBias / 250.0f;
		rw.DepthBiasClamp = 0.0f;
		hr = pDev->CreateRasterizerState(&rw, &g_pRSWater);
		D3D11_RASTERIZER_DESC rc = rs;
		rc.CullMode = D3D11_CULL_BACK;
		pDev->CreateRasterizerState(&rc, &g_pRSWorldCull);
		Log("  R3D: rasterizer (cull back, for water, depth bias %d slope %.3f) hr=%08X",
			g_nWaterBias, (float)g_nWaterBias / 250.0f, (unsigned)hr);
	}
	Log("  R3D: rasterizer hr=%08X", (unsigned)hr);

	// The world is OPAQUE, and it has to say so.
	//
	// This module never set a blend state at all: it inherited whatever the
	// 2D layer had last bound. That was harmless only while every 2D blit was
	// alpha-blended with alpha 1. The moment SetOptimized2DBlend started being
	// honoured, the state left behind could be ADD - and the world was then
	// drawn additively over what was already in the buffer, saturating whole
	// walls to white. Six other explanations were measured and eliminated
	// first, because the geometry, the textures and the depth were all fine
	// and it was the pipeline state between them that was wrong.
	D3D11_BLEND_DESC bo{};
	bo.RenderTarget[0].BlendEnable = FALSE;
	bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	hr = pDev->CreateBlendState(&bo, &g_pBlendOpaque);
	Log("  R3D: opaque blend state hr=%08X", (unsigned)hr);
	{
		D3D11_BLEND_DESC ba = bo;
		ba.AlphaToCoverageEnable = TRUE;
		hr = pDev->CreateBlendState(&ba, &g_pBlendA2C);
		Log("  R3D: alpha-to-coverage blend state hr=%08X (cut-outs under MSAA)", (unsigned)hr);
	}

	{
		// ANISOTROPIC, and this is the fix for the third reported symptom:
		//
		//   Flickering - not whole buildings but streaks: triangles,
		//    Venetian-blind patterns, lines in different patterns,
		//    flickering constantly.
		//
		// Every texture this renderer creates has ONE mip level - the world
		// textures and the lightmap atlas alike. On a surface seen at a
		// grazing angle, hundreds of texels fall inside one screen pixel, and
		// a bilinear sampler with no mip chain simply picks two of them. The
		// picture then beats against the texel grid, which is a Venetian
		// blind of vertical lines that gets finer with distance, and whose
		// phase moves whenever the camera does - so it flickers.
		//
		// Measured on the alleyway wall, vertical-stripe energy in that
		// region: 1359 with lightmaps on, 25 with lightmaps off. The
		// lightmap is the visible offender because it carries hard shadow
		// edges, but the mechanism is the sampler and it applies to both.
		//
		// Anisotropy rather than a mip chain, deliberately: the atlas packs
		// lightmap cells one texel apart, so a mip chain would average
		// neighbouring cells together and bleed light across surfaces that
		// are nowhere near each other. Anisotropy takes its taps along the
		// major axis of the pixel's footprint at the top level, so it fixes
		// the grazing case without ever leaving the cell.
		//
		// StubAniso 0 restores the old bilinear sampler for an A/B.
		D3D11_SAMPLER_DESC sm{};
		if (g_nAniso > 1)
		{
			sm.Filter = D3D11_FILTER_ANISOTROPIC;
			sm.MaxAnisotropy = (g_nAniso > 16) ? 16 : (UINT)g_nAniso;
		}
		else
		{
			sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		}
		sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sm.MaxLOD = D3D11_FLOAT32_MAX;
		hr = pDev->CreateSamplerState(&sm, &g_pSamp);
		Log("  R3D: sampler hr=%08X, filter %s (aniso %d)", (unsigned)hr,
			(g_nAniso > 1) ? "ANISOTROPIC" : "bilinear", g_nAniso);

		// A white 1x1 for polygons with no texture. They draw shaded instead
		// of vanishing, so "this surface has no texture" is visible as a plain
		// white face rather than as a hole nobody can attribute.
		// BGRA in memory, so the uint32 is 0xAARRGGBB: R176 G207 B252 is
		// 0xFFB0CFFC. Written 0xFFFCCFB0 first, which swaps R and B and
		// gives a warm peach that reads as white against a bright street -
		// indistinguishable from the bug it was meant to fix.
		const uint32_t nWhite = g_bSkyStandIn ? 0xFFB0CFFCu : 0xFFFFFFFFu;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = &nWhite; sd.SysMemPitch = 4;
		ID3D11Texture2D* pW = nullptr;
		if (SUCCEEDED(pDev->CreateTexture2D(&td, &sd, &pW)))
		{
			pDev->CreateShaderResourceView(pW, nullptr, &g_pWhite);
			pW->Release();
		}

		// Orange, for the model stand-in boxes. A stand-in that shares a colour
		// with the sky is a stand-in nobody can pick out of the picture.
		const uint32_t nOrange = 0xFFFF8000u;		// BGRA: B00 G80 RFF
		sd.pSysMem = &nOrange;
		ID3D11Texture2D* pO = nullptr;
		if (SUCCEEDED(pDev->CreateTexture2D(&td, &sd, &pO)))
		{
			pDev->CreateShaderResourceView(pO, nullptr, &g_pBoxTex);
			pO->Release();
		}
	}

	// A triangle already in clip space, for the one-variable question "does
	// anything this module draws reach the screen at all?". It shares the
	// shaders, the input layout, the render target and the present path with
	// the world, and differs from it in the geometry and the matrix - so if
	// this appears and the world does not, the pipeline is fine and the fault
	// is in the world data or the camera. If neither appears, nothing this
	// module draws survives to the screen and the world is not the problem.
	{
		const Vtx kTri[3] = {
			{ -0.9f, -0.9f, 0.5f,  0, 1, 0,  0, 0,  -1, -1 },
			{  0.0f,  0.9f, 0.5f,  0, 1, 0,  0, 0,  -1, -1 },
			{  0.9f, -0.9f, 0.5f,  0, 1, 0,  0, 0,  -1, -1 },
		};
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof kTri; bd.Usage = D3D11_USAGE_IMMUTABLE;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = kTri;
		hr = pDev->CreateBuffer(&bd, &sd, &g_pTestVB);
		Log("  R3D: test triangle hr=%08X", (unsigned)hr);
	}

	return g_pVS && g_pPS && g_pIL && g_pCB && g_pDSS && g_pRS;
}

// A WORLD LOADED. Drop every cached texture and let the engine repopulate.
//
// This is the only honest fix available. The cache is keyed on the engine's
// texture-object pointer; a reload frees every texture and the allocator hands
// the same addresses back for different pictures, so a stale entry looks
// exactly like a live one. Two attempts to tell them apart by reading the
// object have now failed for the same reason: texobj+0x08 is the data pointer
// only while the engine has the pixels in hand, and it is NULL on every one of
// 66690 model skin slots. Ranking all 64 words of the object found nothing
// else that validates as texture data either, on 200 skins. There is no
// identity to compare, so comparison is the wrong shape of fix.
//
// The old objection to flushing was that nothing would ever rebind a model
// skin and they would stay blank. That objection is testable now, because
// +VRAutoReloadSecs reproduces the reload at the desk: whatever puts model
// skins in the cache on the FIRST load runs on every load, and the first load
// is always correct. +StubFlushOnLoad 0 restores the old behaviour.
// HOW MUCH ADDRESS SPACE IS LEFT, in a process that only has 2 GB of it.
//
// lithtech.exe is 32-bit and not large-address-aware, so the ceiling is 2 GB
// and this project has already been stopped by it once - the upscale pack
// crashes because two halves of CHARS fit individually and not together.
// Raising the render resolution to 3840x2076 with 4x multisampling adds a
// quarter of a gigabyte of buffers, and "is that close to the edge" was not a
// question anything here could answer.
//
// One line per world load. GlobalMemoryStatusEx reports the CALLING PROCESS's
// virtual address space, which is the number that matters, not the machine's
// RAM.
static void LogAddressSpace(const char* pszWhen)
{
	MEMORYSTATUSEX ms{};
	ms.dwLength = sizeof ms;
	if (!GlobalMemoryStatusEx(&ms)) return;
	const double kMB = 1.0 / (1024.0 * 1024.0);
	const double fTot  = (double)ms.ullTotalVirtual * kMB;
	const double fFree = (double)ms.ullAvailVirtual * kMB;
	Log("  MEMORY (%s): %.0f MB of address space free of %.0f MB total"
		" - %.0f MB in use, %.0f%% of the ceiling",
		pszWhen, fFree, fTot, fTot - fFree,
		(fTot > 0.0) ? (100.0 * (fTot - fFree) / fTot) : 0.0);
}

void R3D_WorldLoaded()
{
	// A new world frees the old one's textures, so an allocation that
	// failed under pressure deserves another try. Without this the model
	// mesh path would stay off for the rest of the session after one bad
	// moment.
	s_nMeshVBFailed = 0;

	++g_nWorldLoads;
	LogAddressSpace("world loaded");

	// EVERY CACHED PAGE VERDICT IS NOW SUSPECT. A load is the one moment the
	// engine hands whole regions back, so nothing measured before it may be
	// carried across. TouchRange makes a stale entry survivable; this stops it
	// being asked in the first place.
	FlushPageCache();
	// Every cached mesh belonged to the world being replaced.
	FlushSkinCache();
	// The file-texture cache is flushed at the START OF THE WORLD WALK, not
	// here: this hook can fire mid-frame while the old world's batches are
	// still to be drawn, and they hold their views without a reference. The
	// walk drops those batches first. See Dtx_Get for why not from a lookup.
	g_bDtxFlushWanted = true;
	// AND SO DID THE LIGHTMAP ATLAS. THIS IS WHY THE SECOND LEVEL OF A
	// SESSION IS UNLIT.
	//
	// LM_Build gates on `pWorld == g_pBuiltFrom` and returns silently when
	// it matches. pWorld is a HEAP POINTER: the engine frees one world and
	// allocates the next, and the allocator hands back the same address.
	// The log has it twice, once per level, byte for byte:
	//
	//     RebindLightmaps: argument 001AFCB0, its first dword is 02768B98
	//
	// So on every world after the first, LM_Build decided it had already
	// built this world, kept the PREVIOUS level's atlas and its previous
	// polygon pointers, and logged nothing at all. Both halves of the
	// picture then go wrong at once, which is exactly what was reported:
	//
	//   THE WORLD GOES BRIGHT. LM_ForPoly misses, the polygon is marked
	//   unlit (luv.x = -1) and the shader falls back to the 0.55..1.0 grey
	//   stand-in. A baked level lit by a flat term is too bright again.
	//
	//   THE PEOPLE GO DIM. The model light grid is filled from lightmap
	//   texels during the same walk, so it fills with nothing - Morocco
	//   went from 3117 of 138510 cells (docs/MODEL-LIGHTING.md) to 27 -
	//   and every model drops to the level's ambient floor, 0.188 there.
	//   Uniform, colourless, and darker than the wall behind it.
	//
	// The 27 are not survivors, they are collisions: recycled polygon
	// addresses that happened to hit entries left in the map by the
	// previous level, carrying that level's light.
	//
	// This runs BEFORE the g_bFlushOnLoad guard on purpose. That flag is a
	// texture-cache policy; a stale atlas is wrong under either setting.
	//
	// Never caught by the 103-level sweep because a sweep gives every level
	// a fresh process, so every level in it is a FIRST world. It only
	// appears when you play - or load a save, which goes via the menu.
	LM_Destroy();
	// And the world mesh, for the same reason and by the same key - see
	// the long note further down, which is where this line used to sit.
	g_pBuiltFrom = 0;
	R3D_TexturesMayHaveMoved("world load");
	if (!g_bFlushOnLoad) return;
	const size_t nHad = g_Tex.size();
	for (size_t i = 0; i < g_Tex.size(); ++i)
		if (g_Tex[i].pSRV) { g_Tex[i].pSRV->Release(); ++g_nTexGen; }
	g_Tex.clear(); ++g_nTexListGen;
	g_Batches.clear();
	// AND THE WORLD HAS TO BE REBUILT, or clearing the batches turns it off.
	//
	// R3D_BuildWorld returns early when the world pointer has not changed, and
	// a quick-load of the SAME level hands back the same pointer - so the
	// batches cleared just above were never rebuilt and the world simply
	// stopped drawing. In the headset the world was missing, just black, with
	// every enemy and NPC in the level visible - no world geometry,
	// so nothing to occlude them either. The models were unaffected because
	// they do not come from these batches.
	//
	// Invalidating it here is also right on its own terms: the engine has
	// reloaded the level, so the polygons behind that pointer are new data
	// even when the address is old. BuildWorld is NOT called from here - at
	// this point not one texture has been bound and a world built now gets
	// none of them - it is called from the world scene, which is where the
	// existing call already lives.
	//
	// THE ASSIGNMENT ITSELF NOW HAPPENS ABOVE THE g_bFlushOnLoad GUARD,
	// with the atlas. Everything this comment says is true whatever the
	// texture-cache policy is, and +StubFlushOnLoad 0 is a control arm
	// somebody will run again one day (docs/RELOAD-SCRAMBLES-SKINS.md).
	g_TexRefreshQ.clear();
	// Keyed on the same addresses a recycled object reuses, so it has to go
	// too or nothing recycled will ever be attempted again.
	g_nTexTried = 0;
	g_SkinPullQ.clear();
	// THE SKIN REPORT COULD NOT SEE A SINGLE IN-GAME MODEL, and that is why
	// the weapon's grip has never appeared in it. SkinModelNew is a
	// first-seen filter over 64 model pointers and g_nSkinSaid is a global
	// 400-line budget, and NEITHER was ever cleared. A quick load goes
	// through the MENU first, so the interface scene - Cate, her sunglasses,
	// the menu props - filled all 64 slots before the world existed, and
	// every model in the level after it was refused as "already seen".
	// The instrument described the one scene nobody was asking about and
	// then went quiet, which is the third time this project has been lied
	// to by a counter sampled once.
	//
	// A world load is exactly the moment the population under test changes,
	// so the report starts again with it.
	g_nSkinSeen = 0;
	g_nSkinSaid = 0;
	// Per LEVEL, so a sweep's rows are comparable. These accumulate across
	// frames like the normals counters beside them.
	g_nPieceDrawn = 0;
	g_nPieceWhite = 0;
	g_nPieceUnnamed = 0;
	g_nWhiteSaid = 0;
	g_nSkinProbeSaid = 0;
	// The sprite-name budget too. Forty names were spent on MENU sprites during
	// startup - FOLDERBACK, NOLF, MAINMENU, LOADING - before a world existed,
	// so the report described the one population nobody was asking about and
	// then went quiet. Fourth instrument in this project to do it.
	g_nSprNameSaid = 0;
	g_nButeSaid = 0;
	g_nSkinFromButes = 0;
	g_NodeHash.clear();
	g_nInstStill = 0;
	g_nInstMoved = 0;
	g_nInvisInst = 0;
	g_nInvisWhite = 0;
	g_nSkipInvis = 0;
	g_nResolvedWithTex = (size_t)-1;		// batches must re-resolve
	Log("  R3D WORLD LOADED (%ld): dropped %u cached textures - the engine's"
		" allocator reuses these addresses, so every one of them is now a"
		" guess", g_nWorldLoads, (unsigned)nHad);
}

// The GPU timing queries are declared further down this file than this
// function can see, so their release lives beside them.
static void ReleaseGpuQueries();
static void ReleaseCtx1();
static void ReleaseLateBuffers();

void R3D_Destroy()
{
	// BRACKET THE RELOAD, because the leak is on this axis and nothing was
	// measuring it. "world loaded" was the only address-space line, and a
	// clean arm plateaus across eight world loads while eleven RELOADS climb
	// to 96% of the ceiling - so the one number being logged was the one that
	// does not move. A pair of lines per reload turns "about 120 MB, linear"
	// into an arithmetic anyone can check: what was free going in, what is
	// free coming out, and whether the difference returns.
	LogAddressSpace("renderer teardown - BEFORE");
	LM_Destroy();
	for (size_t i = 0; i < g_Tex.size(); ++i)
		if (g_Tex[i].pSRV) { g_Tex[i].pSRV->Release(); ++g_nTexGen; }
	g_Tex.clear(); ++g_nTexListGen;
	g_Batches.clear();
	if (g_pWhite) { g_pWhite->Release(); g_pWhite = nullptr; }
	if (g_pSamp)  { g_pSamp->Release();  g_pSamp  = nullptr; }
	if (g_pTestVB) { g_pTestVB->Release(); g_pTestVB = nullptr; }
	if (g_pRS)  { g_pRS->Release();  g_pRS  = nullptr; }
	if (g_pRSWater) { g_pRSWater->Release(); g_pRSWater = nullptr; }
	if (g_pRSWorldCull) { g_pRSWorldCull->Release(); g_pRSWorldCull = nullptr; }
	if (g_pBlendOpaque) { g_pBlendOpaque->Release(); g_pBlendOpaque = nullptr; }
	if (g_pBlendA2C) { g_pBlendA2C->Release(); g_pBlendA2C = nullptr; }
	if (g_pBoxVB) { g_pBoxVB->Release(); g_pBoxVB = nullptr; }
	if (g_pBoxTex) { g_pBoxTex->Release(); g_pBoxTex = nullptr; }
	if (g_pDSS) { g_pDSS->Release(); g_pDSS = nullptr; }
	PrimReleaseAll();
	if (g_pDSSNoDepth) { g_pDSSNoDepth->Release(); g_pDSSNoDepth = nullptr; }
	if (g_pDSSNoWrite) { g_pDSSNoWrite->Release(); g_pDSSNoWrite = nullptr; }
	if (g_pDSSEnv) { g_pDSSEnv->Release(); g_pDSSEnv = nullptr; }
	if (g_pBlendAddMod) { g_pBlendAddMod->Release(); g_pBlendAddMod = nullptr; }
	if (g_pBlendAlpha) { g_pBlendAlpha->Release(); g_pBlendAlpha = nullptr; }
	if (g_pBlendNoColour) { g_pBlendNoColour->Release(); g_pBlendNoColour = nullptr; }
	if (g_pDSV) { g_pDSV->Release(); g_pDSV = nullptr; }
	if (g_pDS)  { g_pDS->Release();  g_pDS  = nullptr; }
	// THE LAST NINE. Term measured the device at 4 references with only the
	// swap chain, context and RTV made, and at 6 after every subsystem had
	// torn down - 2 made after startup and never released; 9 once a scope had
	// drawn. They were the MSAA colour target and its view (made beside the
	// depth buffer above, released only on a resize), the scope pass's five
	// (colour, its RTV and SRV, depth and its DSV) and two vertex buffers
	// created on first use (ReleaseLateBuffers). A device that survives Term
	// is what the alt-tab crash ("d3d11.dll_unloaded") and the frozen exit
	// both came from (24 September).
	if (g_pMsaaRTV)  { g_pMsaaRTV->Release();  g_pMsaaRTV  = nullptr; }
	if (g_pMsaaTex)  { g_pMsaaTex->Release();  g_pMsaaTex  = nullptr; }
	if (g_pScopeSRV) { g_pScopeSRV->Release(); g_pScopeSRV = nullptr; }
	if (g_pScopeRTV) { g_pScopeRTV->Release(); g_pScopeRTV = nullptr; }
	if (g_pScopeTex) { g_pScopeTex->Release(); g_pScopeTex = nullptr; }
	if (g_pScopeDSV) { g_pScopeDSV->Release(); g_pScopeDSV = nullptr; }
	if (g_pScopeDS)  { g_pScopeDS->Release();  g_pScopeDS  = nullptr; }
	ReleaseLateBuffers();
	g_nTargetW = 0; g_nTargetH = 0; g_nMsaaMade = 1;
	if (g_pVB)  { g_pVB->Release();  g_pVB  = nullptr; }
	if (g_pCB)  { g_pCB->Release();  g_pCB  = nullptr; }
	if (g_pCBFog) { g_pCBFog->Release(); g_pCBFog = nullptr; }
	if (g_pCBMdl) { g_pCBMdl->Release(); g_pCBMdl = nullptr; }
	if (g_pCBDyn) { g_pCBDyn->Release(); g_pCBDyn = nullptr; }
	if (g_pCBGrid) { g_pCBGrid->Release(); g_pCBGrid = nullptr; }
	if (g_pCBEnv) { g_pCBEnv->Release(); g_pCBEnv = nullptr; }
	if (g_pCBWater) { g_pCBWater->Release(); g_pCBWater = nullptr; }
	// THE ENV MAP CACHE HOLDS BORROWED VIEWS. Dtx_Get's views belong to the
	// DTX cache and Dtx_Flush (below) releases them; releasing them here as
	// well was a double release, and the second one crashed the renderer's
	// teardown in R3D_Destroy on every reload with a mapped world up
	//. Clear
	// the names, never the views.
	g_EnvByName.clear();
	if (g_pLGridSRV) { g_pLGridSRV->Release(); g_pLGridSRV = nullptr; }
	if (g_pLGridTex) { g_pLGridTex->Release(); g_pLGridTex = nullptr; }
	if (g_pIL)  { g_pIL->Release();  g_pIL  = nullptr; }
	if (g_pPS)  { g_pPS->Release();  g_pPS  = nullptr; }
	if (g_pVS)  { g_pVS->Release();  g_pVS  = nullptr; }
	// THE BIG ONE, AND IT WAS NEVER RELEASED AT ALL. g_pMeshVB is the model
	// mesh buffer - "6 regions of 196608 vertices (40.5 MB)" in the log - and
	// it was created once and leaked on every teardown. The engine frees and
	// reloads this DLL on every focus loss and gain, so a session that loses
	// focus a dozen times leaks half a gigabyte of a 32-bit address space:
	// measured 11 September, a mission with 11 renderer reloads went from 35%
	// of the ceiling to 96% and then threw std::bad_alloc out of
	// R3D_BuildWorld. Its creation is already guarded by "if (!g_pMeshVB)",
	// so clearing it here is all that is needed for it to come back.
	if (g_pMeshVB) { g_pMeshVB->Release(); g_pMeshVB = nullptr; }
	if (g_pPoolVB) { g_pPoolVB->Release(); g_pPoolVB = nullptr; }
	g_nPoolUsed = 0; ++g_nPoolGen;
	// The sprite buffer is the same shape of mistake - created once behind an
	// "if (!g_pSprVB)" and never released.
	if (g_pSprVB) { g_pSprVB->Release(); g_pSprVB = nullptr; }
	// The occlusion queries. Small - a few hundred bytes - but the same
	// mistake, and a leak that only shows up after a dozen reloads is exactly
	// the kind nobody finds. (The GPU timing queries are declared further down
	// this file than this function can see; they are two dozen objects and are
	// left for whoever moves the declarations.)
	for (int q = 0; q < 2; ++q)
		if (g_pOcc[q]) { g_pOcc[q]->Release(); g_pOcc[q] = nullptr; }
	ReleaseGpuQueries();
	ReleaseCtx1();
	// Small, but they were leaking on exactly the same path.
	if (g_pBlendAdd) { g_pBlendAdd->Release(); g_pBlendAdd = nullptr; }
	if (g_pBlendMod2x) { g_pBlendMod2x->Release(); g_pBlendMod2x = nullptr; }
	if (g_pBlendMul) { g_pBlendMul->Release(); g_pBlendMul = nullptr; }
	if (g_pDSSRev)   { g_pDSSRev->Release();   g_pDSSRev   = nullptr; }

	g_pBuiltFrom = 0; g_nPolys = 0; g_nTris = 0;

	// THE TEXTURE CACHES, WHICH ARE THE LEAK.
	//
	// Measured 11 September: at Term the D3D11 device still had 486, 412 and
	// 397 references outstanding across three reloads. Each surviving resource
	// pins the device, and a device that is not destroyed keeps everything it
	// owns mapped - about 120 MB of a 32-bit address space per reload, until
	// the world build throws std::bad_alloc at 96% of the ceiling.
	//
	// Dtx_Flush existed and was called only when the WORLD changed, never at
	// teardown, so a reload left every texture the level had loaded - 375 of
	// them in one measured run - holding the device open. The sprite
	// animation cache is the same shape.
	Dtx_Flush();
	SprAnim_Flush();

	// AND FORGET THE DEVICE. dllmain's Term releases the device and the
	// context right after calling this, and this file keeps its OWN copies of
	// both - so leaving them set leaves two DANGLING pointers, which is worse
	// than null: a call that arrives afterwards reads freed memory instead of
	// faulting at a recognisable address.
	//
	// The engine does call in afterwards. It frees and reloads this DLL on
	// every focus loss and gain, and a world pass that lands between the
	// teardown and the rebuild reached nine separate ID3D11DeviceContext::Map
	// calls with released buffers. Clearing these two is what lets the single
	// check at the top of R3D_DrawWorld stand for all of them.
	g_pDev = nullptr;
	g_pCtx = nullptr;
	LogAddressSpace("renderer teardown - AFTER");
}

void R3D_SetTarget(ID3D11RenderTargetView* pRTV, int nW, int nH)
{
	g_pRTV = pRTV;
	if (nW == g_nTargetW && nH == g_nTargetH && g_pDSV) return;
	if (g_pDSV) { g_pDSV->Release(); g_pDSV = nullptr; }
	if (g_pDS)  { g_pDS->Release();  g_pDS  = nullptr; }
	g_nTargetW = nW; g_nTargetH = nH;
	if (!g_pDev || nW <= 0 || nH <= 0) return;

	// HOW MANY SAMPLES THE DEVICE WILL ACTUALLY GIVE, asked rather than
	// assumed, and stepped down until it says yes. A count the driver refuses
	// makes CreateTexture2D fail and would leave the renderer with no depth
	// buffer at all, which draws nothing - a very loud failure for a quality
	// setting nobody asked to be load-bearing.
	if (g_pMsaaRTV) { g_pMsaaRTV->Release(); g_pMsaaRTV = nullptr; }
	if (g_pMsaaTex) { g_pMsaaTex->Release(); g_pMsaaTex = nullptr; }
	g_nMsaaMade = 1;
	if (g_nMsaa > 1)
	{
		for (int n = g_nMsaa; n > 1; n /= 2)
		{
			UINT nQual = 0;
			if (SUCCEEDED(g_pDev->CheckMultisampleQualityLevels(
					DXGI_FORMAT_B8G8R8A8_UNORM, (UINT)n, &nQual)) && nQual > 0)
			{ g_nMsaaMade = n; break; }
		}
	}

	D3D11_TEXTURE2D_DESC td{};
	td.Width = nW; td.Height = nH; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_D32_FLOAT;
	td.SampleDesc.Count = (UINT)g_nMsaaMade;
	td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	HRESULT hr = g_pDev->CreateTexture2D(&td, nullptr, &g_pDS);
	if (SUCCEEDED(hr)) hr = g_pDev->CreateDepthStencilView(g_pDS, nullptr, &g_pDSV);
	Log("  R3D: depth buffer %dx%d x%d samples hr=%08X",
		nW, nH, g_nMsaaMade, (unsigned)hr);

	// The colour target the world is drawn into, resolved down into the back
	// buffer when the world pass ends. Only when multisampling: at x1 the
	// world draws straight into the back buffer exactly as it always did, so
	// StubMSAA 1 is a true control arm and not a different code path.
	if (g_nMsaaMade > 1)
	{
		D3D11_TEXTURE2D_DESC ct{};
		ct.Width = nW; ct.Height = nH; ct.MipLevels = 1; ct.ArraySize = 1;
		ct.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		ct.SampleDesc.Count = (UINT)g_nMsaaMade;
		ct.Usage = D3D11_USAGE_DEFAULT;
		ct.BindFlags = D3D11_BIND_RENDER_TARGET;
		HRESULT hc = g_pDev->CreateTexture2D(&ct, nullptr, &g_pMsaaTex);
		if (SUCCEEDED(hc))
			hc = g_pDev->CreateRenderTargetView(g_pMsaaTex, nullptr, &g_pMsaaRTV);
		Log("  R3D: MSAA colour target %dx%d x%d hr=%08X",
			nW, nH, g_nMsaaMade, (unsigned)hc);
		if (FAILED(hc))
		{
			// Fall back rather than fail: no AA is a worse picture, no target
			// is no picture.
			if (g_pMsaaRTV) { g_pMsaaRTV->Release(); g_pMsaaRTV = nullptr; }
			if (g_pMsaaTex) { g_pMsaaTex->Release(); g_pMsaaTex = nullptr; }
			g_nMsaaMade = 1;
			Log("  R3D: MSAA target refused - falling back to no AA");
		}
	}
}

// The world's colour, from the multisampled target into the back buffer.
// Called when the world pass ends, BEFORE anything 2D is drawn, because the
// resolve overwrites the whole target and would erase a HUD drawn first.
void R3D_ResolveMsaa()
{
	if (!g_pCtx || g_nMsaaMade <= 1 || !g_pMsaaTex || !g_pRTV) return;
	ID3D11Resource* pBack = nullptr;
	g_pRTV->GetResource(&pBack);
	if (pBack)
	{
		g_pCtx->ResolveSubresource(pBack, 0, g_pMsaaTex, 0,
								   DXGI_FORMAT_B8G8R8A8_UNORM);
		pBack->Release();
	}
}

void R3D_SetMsaa(int n) { g_nMsaa = (n > 0) ? n : 1; }
void R3D_SetMsaaClear(int b) { g_bMsaaClear = b; }
int  R3D_MsaaMade()     { return g_nMsaaMade; }

// ---------------------------------------------------------------------------
// THE NAME PROBE. Stage 1 of docs/PLAN-FILES-NOT-HEAP.md needs one thing the
// renderer does not have: the FILENAME of the texture the engine just bound.
// The bridge between the live engine and the files on disk is names - a model
// on disk is a model filename and two skin filenames, no pointer to anything - and
// a name is the only identity a texture has that survives a level reload,
// which is precisely what docs/RELOAD-SCRAMBLES-SKINS.md proved we do not have.
//
// The engine loaded these textures from .dtx files itself, so it knew the name
// at some point. This looks for where it kept it: every word of the texture
// object and of its data object, taken as a pointer, then one level of
// indirection through each, checked for a printable NUL-terminated string.
//
// It runs at texture CREATION, never per frame, and it switches itself off
// after kNameProbeMax textures - so its cost is bounded at a few hundred
// milliseconds once per session and it cannot follow anything into a headset
// build. That is deliberate: a per-frame measurement I left on is what cost
// the tester frame rate on 4 September.
// ---------------------------------------------------------------------------
namespace
{
	const int kNameProbeMax = 120;		// textures examined, then it stops

	int  g_bNameProbe    = 0;			// +StubTexName
	long g_nNameProbed   = 0;
	long g_aNameOffKey[64]  = { 0 };	// a .dtx name at [texobj + i*4]
	long g_aNameOffData[64] = { 0 };	// ... at [dataobj + i*4]
	long g_aPathOffKey[64]  = { 0 };	// any printable path, .dtx or not
	struct IndirectHit { int i, j; long n; };
	IndirectHit g_aIndirect[32];
	int  g_nIndirect = 0;
	int  g_nNameSaid = 0;
	long g_nNameFound = 0;				// textures a name was recovered for

	// The heap-versus-file comparison, counted. A RATIO, not a raw number: the
	// denominator is the only thing that separates "the parse is right" from
	// "the comparison never ran".
	long g_nFileCmp = 0, g_nFileAgree = 0, g_nFileDisagree = 0, g_nFileMissing = 0;

	// +StubTexFromFile. When the engine's texture object names a .dtx we can
	// read, the PIXELS come off disk instead of out of the heap. Same key, same
	// lookup, same batches - one variable changed.
	int  g_bTexFromFile = 0;
	long g_nTexFromFile = 0, g_nTexFromHeap = 0, g_nTexFileRefused = 0;
	// Model skin slots, which the engine may never bind at all. Counted before
	// anything depends on it: the question "can a skin be named" has to have a
	// number before the grip is claimed fixed.
	long g_nSkinNameTried = 0, g_nSkinNamed = 0, g_nSkinFromFile = 0;
	// How many skin slots were found holding a cache entry built for a
	// DIFFERENT picture - the engine having recycled the texture object
	// at that address since. This is the count that has to be non-zero
	// on the main menu and zero once it has settled.
	long g_nSkinRecycled = 0;
	// HOW OFTEN THE NAME CHECK RUNS, in frames. Asking the engine for a
	// skin's name means normalising a path and hashing it against the
	// mounted archives, and doing that for every slot of every model on
	// every frame cost 2.3 ms - 11.23 ms/frame became 13.5. Measured, on
	// the quick save, before anyone put a headset on.
	//
	// The engine recycles a texture object when it FREES one, which
	// happens at a load or a folder change - not continuously. Checking
	// each slot every 30th frame catches that within half a second, and
	// the frames it could be wrong for are the frames nobody is looking
	// at. +StubSkinNamePeriod 1 restores the every-frame check, 0 turns
	// the check off entirely - both arms of the A/B in one build.
	int  g_nSkinNamePeriod = 30;
	int  g_nSkinTryThisFrame = 0;

	// A printable, NUL-terminated string at p. Reads through ONE readability
	// test on the largest window that answers, rather than one per byte:
	// IsBadReadPtr is a system call, and 64 offsets x 16 indirections x 128
	// bytes of it would be a probe nobody could afford to run.
	bool StringAt(uint32_t p, char* pOut, size_t nOut)
	{
		size_t nWin = 128;
		while (nWin >= 16 && !Readable(p, nWin)) nWin >>= 1;
		if (nWin < 16) return false;
		if (nWin > nOut - 1) nWin = nOut - 1;
		const char* s = (const char*)(uintptr_t)p;
		size_t n = 0;
		while (n < nWin && s[n])
		{
			const unsigned char c = (unsigned char)s[n];
			if (c < 0x20 || c > 0x7E) return false;
			++n;
		}
		if (n < 5 || n >= nWin) return false;	// empty, or never terminated
		memcpy(pOut, s, n);
		pOut[n] = 0;
		return true;
	}

	bool EndsWithDtx(const char* s)
	{
		const size_t n = strlen(s);
		if (n < 4) return false;
		const char* e = s + n - 4;
		return e[0] == '.'
			&& (e[1] == 'D' || e[1] == 'd')
			&& (e[2] == 'T' || e[2] == 't')
			&& (e[3] == 'X' || e[3] == 'x');
	}

	bool LooksLikePath(const char* s)
	{
		return strchr(s, '\\') != nullptr || strchr(s, '/') != nullptr;
	}

// ---------------------------------------------------------------------------
// THE NAME OF A TEXTURE, from the engine's own texture object. MEASURED, not
// guessed: [[texobj + 0x24] + 0x1C] is a printable string ending in .dtx on
// 120 of 120 textures the engine bound, and the file it names agrees with the
// engine's own copy on dimensions and mean colour on all 120 (the name probe
// and the heap-versus-file check, 4 September). The same string appears again
// at +0x58, +0x8C, +0xC0 and +0xF4 - a 0x34 stride - on slightly fewer each
// time, so the pointer at +0x24 is the first element of an array and the later
// ones belong to neighbouring textures. Read only the first.
//
// RezFS_Exists IS PART OF THE TEST, not a convenience. A printable string that
// happens to end in ".dtx" is a coincidence; one that also names a file in the
// mounted archives is not. Without it this would occasionally hand a wrong
// name to the loader, and a wrong name is a wrong picture - which is exactly
// the failure this whole stage exists to end.
// ---------------------------------------------------------------------------
	const uint32_t kTexNameOuter = 0x24;
	const uint32_t kTexNameInner = 0x1C;

	bool TexNameOf(uint32_t pTexObj, char* pOut, size_t nOut)
	{
		if (pOut && nOut) pOut[0] = 0;
		if (!pTexObj || !pOut || nOut < 2) return false;
		const uint32_t pRec = Word(pTexObj, kTexNameOuter);
		if (!pRec) return false;
		const uint32_t pStr = Word(pRec, kTexNameInner);
		if (!pStr) return false;
		char sz[128];
		if (!StringAt(pStr, sz, sizeof(sz))) return false;
		// A .SPR IS A NAME TOO. The HQ waterfall is Spr\Spr0028.spr on a
		// PhysicsBSP polygon: the engine's texture object carries that name,
		// this refused it for not being a .dtx, and the polygon drew white.
		if (!EndsWithDtx(sz) && !SprAnim_IsSpr(sz)) return false;
		if (!RezFS_Exists(sz)) return false;
		strncpy(pOut, sz, nOut - 1);
		pOut[nOut - 1] = 0;
		return true;
	}

	void NoteIndirect(int i, int j)
	{
		for (int z = 0; z < g_nIndirect; ++z)
			if (g_aIndirect[z].i == i && g_aIndirect[z].j == j)
				{ ++g_aIndirect[z].n; return; }
		if (g_nIndirect >= 32) return;
		g_aIndirect[g_nIndirect].i = i;
		g_aIndirect[g_nIndirect].j = j;
		g_aIndirect[g_nIndirect].n = 1;
		++g_nIndirect;
	}

	// Look for the name and, if it is found, hand it back in pszOut.
	// ---------------------------------------------------------------------
	// THE SAME QUESTION, ASKED OF A MODEL SKIN SLOT. The world's textures give
	// up their name at [[texobj+0x24]+0x1C] on every single one, and model skin
	// slots give up nothing there - 0 of 55838 in the first run that tried. So
	// either these objects are not the same kind of thing, or the name is
	// somewhere else in them.
	//
	// This looks for ANY printable string, not only a .dtx one: "there is no
	// name here" and "there is a name here in a shape I did not expect" are
	// different findings and a .dtx-only search cannot tell them apart. That
	// distinction is the whole point of running it.
	// ---------------------------------------------------------------------
	const int kSkinProbeMax = 200;
	long g_nSkinProbed = 0;
	long g_aSkinStrOff[64] = { 0 };
	struct SkinIndirect { int i, j; long n; };
	SkinIndirect g_aSkinInd[32];
	int  g_nSkinInd = 0;
	int  g_nSkinStrSaid = 0;
	long g_nSkinAnyStr = 0;
	char g_szSkinSample[8][96] = { { 0 } };
	int  g_nSkinSample = 0;

	void NoteSkinIndirect(int i, int j)
	{
		for (int z = 0; z < g_nSkinInd; ++z)
			if (g_aSkinInd[z].i == i && g_aSkinInd[z].j == j)
				{ ++g_aSkinInd[z].n; return; }
		if (g_nSkinInd >= 32) return;
		g_aSkinInd[g_nSkinInd].i = i; g_aSkinInd[g_nSkinInd].j = j;
		g_aSkinInd[g_nSkinInd].n = 1; ++g_nSkinInd;
	}

	void KeepSkinSample(const char* sz)
	{
		if (g_nSkinSample >= 8) return;
		for (int z = 0; z < g_nSkinSample; ++z)
			if (!strcmp(g_szSkinSample[z], sz)) return;
		strncpy(g_szSkinSample[g_nSkinSample], sz, 95);
		g_szSkinSample[g_nSkinSample][95] = 0;
		++g_nSkinSample;
	}

	// READ THE OBJECT, do not only rank it. Four dumps say more about a
	// structure than a histogram over 200 of them: the histogram cannot tell
	// "every skin names its own texture" from "the pointer lands in a shared
	// table and I am reading a neighbour".
	int g_nSkinDumped = 0;
	void SkinDump(uint32_t pObj)
	{
		if (!g_bNameProbe || g_nSkinDumped >= 4 || !pObj) return;
		if (!Readable(pObj, 256)) return;
		++g_nSkinDumped;
		Log("  R3D SKINDUMP %d: object %08X", g_nSkinDumped, pObj);
		char sz[128];
		for (int i = 0; i < 64; ++i)
		{
			const uint32_t w = Word(pObj, i * 4);
			if (!w) continue;
			if (StringAt(w, sz, sizeof(sz)))
				{ Log("      +%03X = %08X -> \"%s\"", i * 4, w, sz); continue; }
			bool bShown = false;
			for (int j = 0; j < 16; ++j)
			{
				const uint32_t w2 = Word(w, j * 4);
				if (!w2 || !StringAt(w2, sz, sizeof(sz))) continue;
				Log("      +%03X = %08X, +%03X -> \"%s\"", i * 4, w, j * 4, sz);
				bShown = true;
			}
			if (!bShown) Log("      +%03X = %08X", i * 4, w);
		}
	}

	void SkinNameProbe(uint32_t pObj)
	{
		if (!g_bNameProbe || g_nSkinProbed >= kSkinProbeMax || !pObj) return;
		++g_nSkinProbed;
		char sz[128];
		bool bAny = false;
		for (int i = 0; i < 64; ++i)
		{
			const uint32_t w = Word(pObj, i * 4);
			if (!w) continue;
			if (StringAt(w, sz, sizeof(sz)))
				{ ++g_aSkinStrOff[i]; KeepSkinSample(sz); bAny = true; }
			for (int j = 0; j < 16; ++j)
			{
				const uint32_t w2 = Word(w, j * 4);
				if (!w2 || !StringAt(w2, sz, sizeof(sz))) continue;
				NoteSkinIndirect(i, j); KeepSkinSample(sz); bAny = true;
			}
		}
		if (bAny) ++g_nSkinAnyStr;

		if (g_nSkinProbed == kSkinProbeMax && !g_nSkinStrSaid)
		{
			g_nSkinStrSaid = 1;
			Log("  R3D SKINNAME: %ld of %ld model skin slots hold ANY printable"
				" string at all", g_nSkinAnyStr, g_nSkinProbed);
			for (int i = 0; i < 64; ++i)
				if (g_aSkinStrOff[i])
					Log("    skinobj +%03X -> a string on %ld of %ld",
						i * 4, g_aSkinStrOff[i], g_nSkinProbed);
			for (int z = 0; z < g_nSkinInd; ++z)
				Log("    skinobj +%03X then +%03X -> a string on %ld of %ld",
					g_aSkinInd[z].i * 4, g_aSkinInd[z].j * 4,
					g_aSkinInd[z].n, g_nSkinProbed);
			for (int z = 0; z < g_nSkinSample; ++z)
				Log("    skinobj string sample: \"%s\"", g_szSkinSample[z]);
			if (!g_nSkinAnyStr)
				Log("    NOTHING. A model skin slot carries no name anywhere in"
					" its first 64 words, direct or one level down - so the route"
					" to a skin filename is the CLIENT, not the heap.");
		}
	}

	void TexNameProbe(uint32_t pKey, uint32_t pData, char* pszOut, size_t nOut)
	{
		if (pszOut && nOut) pszOut[0] = 0;
		if (!g_bNameProbe || g_nNameProbed >= kNameProbeMax) return;
		++g_nNameProbed;

		char sz[128];
		bool bGot = false;

		for (int i = 0; i < 64; ++i)
		{
			const uint32_t w = Word(pKey, i * 4);
			if (!w) continue;
			if (StringAt(w, sz, sizeof(sz)))
			{
				if (LooksLikePath(sz)) ++g_aPathOffKey[i];
				if (EndsWithDtx(sz))
				{
					++g_aNameOffKey[i];
					if (!bGot && pszOut) { strncpy(pszOut, sz, nOut - 1);
										   pszOut[nOut - 1] = 0; bGot = true; }
				}
			}
			// One level down. A LithTech texture object is a handle, so the
			// name is more likely to live in a file-identifier struct it
			// points at than in the object itself.
			for (int j = 0; j < 16; ++j)
			{
				const uint32_t w2 = Word(w, j * 4);
				if (!w2) continue;
				if (!StringAt(w2, sz, sizeof(sz))) continue;
				if (!EndsWithDtx(sz)) continue;
				NoteIndirect(i, j);
				if (!bGot && pszOut) { strncpy(pszOut, sz, nOut - 1);
									   pszOut[nOut - 1] = 0; bGot = true; }
			}
		}
		for (int i = 0; i < 64; ++i)
		{
			const uint32_t w = Word(pData, i * 4);
			if (!w) continue;
			if (StringAt(w, sz, sizeof(sz)) && EndsWithDtx(sz))
			{
				++g_aNameOffData[i];
				if (!bGot && pszOut) { strncpy(pszOut, sz, nOut - 1);
									   pszOut[nOut - 1] = 0; bGot = true; }
			}
		}
		if (bGot) ++g_nNameFound;

		if (g_nNameProbed == kNameProbeMax && !g_nNameSaid)
		{
			g_nNameSaid = 1;
			Log("  R3D TEXNAME: %ld of %ld textures gave up a .dtx name",
				g_nNameFound, g_nNameProbed);
			for (int i = 0; i < 64; ++i)
				if (g_aNameOffKey[i])
					Log("    texobj  +%03X -> \"...dtx\" on %ld of %ld",
						i * 4, g_aNameOffKey[i], g_nNameProbed);
			for (int i = 0; i < 64; ++i)
				if (g_aNameOffData[i])
					Log("    dataobj +%03X -> \"...dtx\" on %ld of %ld",
						i * 4, g_aNameOffData[i], g_nNameProbed);
			for (int z = 0; z < g_nIndirect; ++z)
				Log("    texobj  +%03X then +%03X -> \"...dtx\" on %ld of %ld",
					g_aIndirect[z].i * 4, g_aIndirect[z].j * 4,
					g_aIndirect[z].n, g_nNameProbed);
			// The negative result matters as much as the positive one: if no
			// offset holds a name, the route is not in this object and the
			// client has to supply it. Printable paths that are NOT .dtx say
			// whether there are strings here at all.
			if (!g_nNameFound)
			{
				Log("    NO .DTX NAME ANYWHERE IN THE TEXTURE OBJECT."
					" The route is the client, not the heap.");
				for (int i = 0; i < 64; ++i)
					if (g_aPathOffKey[i])
						Log("    texobj  +%03X -> some path on %ld of %ld",
							i * 4, g_aPathOffKey[i], g_nNameProbed);
			}
		}
	}
}

void R3D_SetTexName(int b) { g_bNameProbe = b; }
void R3D_SetTexFromFile(int b) { g_bTexFromFile = b; }

// Look one up. Creation happens in R3D_NoteTexture, called from BindTexture.
static int TextureFor(uint32_t pTex)
{
	if (!pTex) return -1;
	for (size_t i = 0; i < g_Tex.size(); ++i)
		if (g_Tex[i].pTex == pTex) return (int)i;
	return -1;
}

// A cache entry BY NAME. The address is not an identity - the engine recycles
// texture objects - so anything resolving a texture wants this in front of the
// address lookup, not instead of it.
static int TextureNamed(const char* pszName)
{
	if (!pszName || !*pszName) return -1;
	for (size_t i = 0; i < g_Tex.size(); ++i)
		if (g_Tex[i].szName[0] && _stricmp(g_Tex[i].szName, pszName) == 0)
			return (int)i;
	return -1;
}

// One candidate texture object -> a cache entry, NAME FIRST. If the engine
// will say what this address holds, believe that over whatever we filed under
// the address, because a recycled address answers with its previous owner's
// picture - which is what dressed Cate in a menu bar for a whole session.
//
// BUT THE NAME IS NOT FREE, and this is called for every sprite in the level,
// twice an eye, every frame. Asking for it means reading a string out of the
// engine's heap, normalising a path and hashing it against the mounted
// archives; on Morocco's 116 sprites that measured 11.23 ms/frame -> 13.55.
//
// So: resolve by ADDRESS, which is a pointer compare, and re-verify the name
// only on this object's own frame in the period. The engine recycles a texture
// object when it FREES one - a load or a folder change, not something that
// happens continuously - so a check every 30th frame catches it within half a
// second, and the frames it could be wrong for are the frames of a load.
// The stagger by address keeps every object off the same frame.
static int MakeFileTexture(uint32_t pKey);
static int ResolveTexObj(uint32_t pTex)
{
	if (!pTex) return -1;
	const int nByAddr = TextureFor(pTex);
	// THE STAGGER IS A COST CONTROL, NOT A CORRECTNESS ARGUMENT, AND THE
	// COMMENT ABOVE SAYS SO: "the frames it could be wrong for are the frames
	// of a load". That half second IS the defect - in headset testing, on a load
	// or a menu selection, certain textures dropped out and loaded back in,
	// and it looked wrong, with the menu card a maroon blob and a flower motif
	// wearing Cate's suit.
	//
	// A load or a folder change is exactly when the engine frees texture
	// objects and hands their addresses back for something else, so it is
	// exactly when an address must not be trusted. Verify everything for a
	// short window after one, and keep the cheap stagger for the steady state
	// where nothing is being freed.
	const bool bWindow = (g_nFrames < g_nVerifyUntil);
	const bool bVerify = (g_nSkinNamePeriod > 0)
		&& (nByAddr < 0 || bWindow
			|| ((g_nFrames + (long)(pTex >> 4)) % g_nSkinNamePeriod) == 0);
	if (bWindow) ++g_nVerifyForced;
	if (bVerify)
	{
		char szN[80];
		if (TexNameOf(pTex, szN, sizeof szN))
		{
			const int n = TextureNamed(szN);
			if (n >= 0) return n;

			// THE NAME IS KNOWN TO THE ENGINE AND NOT TO THE CACHE, AND THE
			// ADDRESS IS ALREADY FILED UNDER ANOTHER NAME. That is a recycled
			// object whose new file has not been bound yet, and falling
			// through to the address here is exactly the six frames the tester
			// sees: a sixty-frame series across MAIN -> OPTIONS shows the
			// options card wearing a piece of Cate's sunglasses until the
			// engine binds the card's own art. Load it from the file by name,
			// as the sprites and skins already do; if the file will not
			// load, draw nothing rather than the previous owner's picture.
			if (nByAddr >= 0 && _stricmp(g_Tex[nByAddr].szName, szN) != 0)
			{
				const int m = MakeFileTexture(pTex);
				if (m >= 0) { ++g_nRecycledRefused; return m; }
				++g_nRecycledRefused;
				return -1;
			}
		}
	}
	return nByAddr;
}

// A sprite's texture, by either of the TWO routes there turn out to be.
//
// Route A - obj + 0x1A8 points at a struct with a texture object in its first
// words. Measured on 113 of 113 WORLD sprites, and 0x1A8 is the same generic
// "render data" pointer the world model link uses, so its meaning follows the
// object type. Which word inside varies, so the first that resolves wins.
//
// Route B - [[obj + 0x1B0] + 0x00]. THE MAIN MENU'S BACKDROP IS A SPRITE and
// it answers to nothing on route A: a scan of 160 words of the object and 64
// of its render data found no word that either the cache knew or the engine
// would name. It answers on the first word of 0x1B0 immediately, with
// MENU\SPRTEX\FOLDERBACK1.DTX, the engine's own name agreeing. 0x1B0 sits
// directly below the model skin slots at 0x1B8 and 0x1BC - the same region of
// the object, holding the same kind of thing.
//
// Route B is tried FIRST because it is exact where route A is a scan, and a
// scan that finds the wrong texture cannot be told from one that finds the
// right one.
// Defined below; the sprite route needs it and sits above it.
static int MakeFileTexture(uint32_t pKey);

// THE SAME RESOLVE, BUT THE NAME DECIDES AND A NAMELESS CANDIDATE IS REFUSED.
//
// ResolveTexObj trusts the address when the cache knows it and only re-checks
// the name every StubSkinNamePeriod frames. That is right for a model skin,
// which is re-checked constantly anyway and whose wrong answers are a wrong
// shirt. It is wrong for a sprite found by SCANNING sixteen words for anything
// the cache recognises: there the address IS the guess, so it has to be paid
// for with a name every time. A candidate the engine will not name is not a
// texture object, whatever the cache thinks of its address.
static int ResolveTexObjByName(uint32_t pTex)
{
	if (!pTex) return -1;
	if (!g_bSprStrictTex) return ResolveTexObj(pTex);
	char szN[80];
	if (!TexNameOf(pTex, szN, sizeof szN)) return -1;
	const int n = TextureNamed(szN);
	if (n >= 0) return n;

	// THE NAME RESOLVED AND THE CACHE HAD NEVER HEARD OF IT, which is a
	// different failure from "this is not a texture object" and was being
	// treated as the same one.
	//
	// M05S01 is a rainy level. 198 of the 211 sprites IN FRONT OF THE CAMERA
	// drew nothing, and the probe says why: they name
	// SFX\RAIN\SPRTEX\RNSPL01.DTX perfectly well, and that file has simply
	// never been bound through the path we replaced, so TextureNamed returns
	// -1 and the sprite is refused. Two hundred rain splashes, on a level
	// where it is raining.
	//
	// The model skins already answer exactly this question from the file. Do
	// the same here. The name is still the identity - RezFS_Exists is part of
	// the test, because a string ending in .dtx is a coincidence and one that
	// also names a mounted file is not - so this does NOT reopen the address
	// guessing that +StubSprStrictTex exists to prevent. Turning that switch
	// off instead fills the level with yellow squares; photographed.
	//
	// BOUNDED PER FRAME, like the skin pull. An unbounded retry would run on
	// every sprite of every frame for anything genuinely absent, which is the
	// shape of a cost that does not show up in a still and does show up as
	// the tester reporting the rate dipping.
	if (g_bTexFromFile && g_bSprFromFile && g_nSprFileThisFrame < 8
		&& RezFS_Exists(szN))
	{
		++g_nSprFileThisFrame;
		return MakeFileTexture(pTex);
	}
	return -1;
}

static int SpriteTexture(uint32_t hObj)
{
	const uint32_t pRes = Word(hObj, 0x1B0);
	if (MemKind(pRes, 0x40) == 2)
	{
		const int i = ResolveTexObjByName(Word(pRes, 0));
		if (i >= 0) return i;
	}

	const uint32_t pData = Word(hObj, 0x1A8);
	if (MemKind(pData, 0x40) != 2) return -1;
	for (int k = 0; k < 16; ++k)
	{
		const int i = ResolveTexObjByName(Word(pData, k * 4));
		if (i >= 0) return i;
	}
	return -1;
}

// HOW BIG SPRITES ACTUALLY ARE ON SCREEN, which is a different question from
// how many were drawn and the only one that matters when the size formula
// itself is under test. The quick save publishes 117 sprites and doubling
// every one of them moved 0.01 of a mean pixel: they are all behind the camera
// or a few pixels across, so that level cannot settle anything.
//
// Reported as the TANGENT of the half-angle the quad subtends - half-extent
// over distance, no projection needed, comparable between levels - so 1.0 is a
// sprite as wide as a 90 degree field. Printed with the ACCOUNT as well as in
// the periodic report, because the level sweep kills the process as soon as it
// sees the ACCOUNT line and would otherwise never reach this.
static void SpriteFootprint()
{
	if (!g_bHaveCam || !g_Spr.nCount) return;
	float rq[3], uq[3], fq[3];
	QuatBasis(g_fLastQuat, rq, uq, fq);
	long nFront = 0;
	double fSum = 0.0, fMax = 0.0;
	char szBig[80]; strcpy_s(szBig, "-");
	for (uint32_t z = 0; z < g_Spr.nCount; ++z)
	{
		const int ti = SpriteTexture(g_Spr.inst[z].nObject);
		if (ti < 0 || !g_Tex[ti].pSRV) continue;
		const float* P = g_Spr.inst[z].fPos;
		const float dx = P[0] - g_fLastPos[0];
		const float dy = P[1] - g_fLastPos[1];
		const float dz = P[2] - g_fLastPos[2];
		const float fFwd = dx*fq[0] + dy*fq[1] + dz*fq[2];
		if (fFwd <= 1.0f) continue;		// behind us, or on top of us
		++nFront;
		const float hw = g_Tex[ti].fW * g_Spr.inst[z].fScale[0] * g_fSpriteScale;
		const double t = (double)hw / (double)fFwd;
		fSum += t;
		if (t > fMax) { fMax = t; strcpy_s(szBig, g_Tex[ti].szName); }
	}
	Log("     SPRITE FOOTPRINT: %ld in front of the camera, summed half-angle"
		" tangent %.3f, LARGEST %.3f '%s'", nFront, fSum, fMax, szBig);
}


// Build a cache entry whose PIXELS CAME FROM THE FILE.
//
// This is the whole of stage 1 in one function. The entry is keyed by the
// engine's texture object exactly as before, so every batch, every skin slot
// and every lookup downstream is unchanged - but what the GPU samples is the
// .dtx, with the full mip chain the engine's in-memory copy does not carry.
//
// Three defects stop existing here rather than being fixed one at a time:
// a texture the engine has freed still has correct pixels, because a name
// always means the same file; a reload cannot scramble anything, for the same
// reason; and a skin the engine never binds can be drawn at all, because the
// NAME lives in the texture object and only the PIXELS were ever missing.
//
// The Dtx cache owns its view, so this takes a reference of its own - the
// flush paths release what g_Tex holds and would otherwise free the cache's
// copy out from under it.
// THE TWO CLASSIFICATIONS A SKIN NEEDS, from the file's own info. These are
// the rules MakeFileTexture applies to a texture entry, lifted out so a skin
// that was loaded straight from its .dtx (the PUB SKIN and bute routes, which
// never make an entry) gets the same answers. Without this the helicopter's
// rotor disc - HELICOPTER_ACTION_HEAD.DTX, 64% clear, a hard cut-out - was
// drawn with no alpha test at all, and the clear texels showed their black.
static int GradedOf(const DtxInfo& fi)
{
	return ((fi.nFlags & 0x80u) && fi.fBimodal < 0.05f) ? 1 : 0;
}
static float CutRefOf(const DtxInfo& fi)
{
	if (fi.fAlphaRef > 0.0f) return fi.fAlphaRef / 255.0f;
	if (g_bCutGuess && fi.fCut > 0.005f && fi.fCut < 0.95f
		&& fi.fBimodal > 0.60f && fi.fClearSpread < 8.0f) return 0.5f;
	return 0.0f;
}
static int MakeFileTexture(uint32_t pKey)
{
	if (!g_pDev || !pKey) return -1;
	char szName[80];
	if (!TexNameOf(pKey, szName, sizeof(szName))) return -1;

	DtxInfo fi{};
	uint32_t nAnim1 = 0;
	ID3D11ShaderResourceView* pSRV = nullptr;
	if (SprAnim_IsSpr(szName))
	{
		nAnim1 = (uint32_t)SprAnim_Get(g_pDev, szName);
		pSRV = SprAnim_SRV((int)nAnim1, 0.0);
		const DtxInfo* pi = SprAnim_Info((int)nAnim1);
		if (pi) fi = *pi;
		Log("  R3D: engine texture named a SPRITE, %s - %s", szName,
			pSRV ? "frames loaded, animated on the polygon" : "NOT FOUND");
	}
	else
		pSRV = Dtx_Get(g_pDev, szName, &fi);
	if (!pSRV) { ++g_nTexFileRefused; return -1; }
	pSRV->AddRef();

	TexEntry e{};
	e.nAnim1 = nAnim1;
	e.nLoad = g_nWorldLoads;
	e.pTex = pKey;
	e.pSRV = pSRV;
	e.fW = (float)fi.nWidth; e.fH = (float)fi.nHeight;
	e.nUVShift = fi.nUVShift;
	e.mr = fi.mr; e.mg = fi.mg; e.mb = fi.mb;
	e.bZeroAlpha = fi.bZeroAlpha;
	e.fCut = fi.fCut;
	e.fBi  = fi.fBimodal;
	// GLASS AND WATER BY THEIR OWN TEXTURE. Every Gl*.dtx and Wa*.dtx in the
	// game is a 32-bit or palette texture whose alpha is the SAME partial
	// value on every texel - GL0006 reads 110 of 255 everywhere, WA0010 140 -
	// and whose header carries PREFER4444, the artist keeping that channel.
	// Walls are DXT1 at alpha 255 with no such flag. So a texture that says
	// "keep my alpha" and holds neither holes nor solid texels is a film:
	// HQ's glass doors and the water in its fountain, which drew as solid
	// slabs because the object they sit on has an authored alpha of 1.0 and
	// the world pass never blends. Reported in the headset: a water fountain with
	// solid water, and glass windows that were just flat colour.
	// Bimodality alone, not fCut: the measurer counts every texel under 128
	// as "clear", so half-alpha glass reads as 100% clear and a clear-fraction
	// test rejects exactly the textures this is for. Under 5% of texels at 0
	// or 255 is a film; a character skin with a specular channel measures 12%
	// and a shell casing 4%, and neither reaches the world path anyway.
	e.bGraded = ((fi.nFlags & 0x80u) && fi.fBimodal < 0.05f) ? 1 : 0;
	if (e.bGraded)
	{
		static int s_nSaidGraded = 0;
		if (s_nSaidGraded++ < 12)
			Log("  R3D GLASS BY TEXTURE: %s  alpha graded on %.0f%% of texels"
				" - drawn blended with its own alpha", szName,
				100.0f * (1.0f - fi.fBimodal));
	}
	// The SAME rule as the heap path, in the same order. A cut-out decided
	// differently on the two routes would change which polygons are alpha
	// tested, and that is a picture change hiding inside what is supposed to
	// be a pixel-source change.
	if (fi.fAlphaRef > 0.0f) e.fCutRef = fi.fAlphaRef / 255.0f;
	// AND THE TRANSPARENT REGION HAS TO BE FLAT.
	//
	// The clear fraction and the bimodality say "this alpha channel looks like
	// a mask". They cannot tell a mask from an alpha channel nobody authored,
	// and the difference decides whether alpha testing removes the unused
	// corner of a sheet or 89% of a lamp shade. A real cut-out's unused area
	// is ONE key colour - CATTAIL and FOLIAGE_01 both measure a spread of 0.0
	// - where LAMP_05B measures 43.8 and LIGHTBULB 24.4, because the picture
	// is still there under an alpha channel the artist never touched.
	//
	// 8 is comfortably above the 0.7 a genuine flat region measures and far
	// below the 24 of the mildest real-art case in the game.
	else if (g_bCutGuess
			 && e.fCut > 0.005f && e.fCut < 0.95f && fi.fBimodal > 0.60f
			 && fi.fClearSpread < 8.0f)
	{
		e.fCutRef = 0.5f;
		// NAMED, BECAUSE THIS ONE IS A GUESS. The file's own alpharef is the
		// artist's answer; this branch is the renderer deciding a texture is a
		// cut-out from its pixels alone, and a wrong guess DELETES most of a
		// surface. A lamp shade whose sheet is 89% transparent, on an opaque
		// prop the game never alpha tested, loses 89% of itself and reads as a
		// fragment hanging in the air - which is the Morocco hotel lamp
		// symptom exactly. This is also the route model skins take, so it is
		// the copy that matters for props.
		Log("  R3D CUT GUESS: %s  %.1f%% clear, %.1f%% bimodal, NO alpharef"
			" in the file - alpha tested at 0.5 on a GUESS",
			szName, 100.0f * e.fCut, 100.0f * fi.fBimodal);
	}
	e.bCut = (e.fCutRef > 0.0f) ? 1 : 0;
	e.bOccl = (fi.nWidth == 64 && fi.nHeight == 64 && e.mr < 60.0f
			   && e.mg > 140.0f && e.mb > 80.0f && e.mb < 130.0f) ? 1 : 0;
	TexLiveId(pKey, &e.pData, &e.pPix, &e.nSig);
	strncpy(e.szName, szName, sizeof(e.szName) - 1);
	// REPLACE, DO NOT APPEND. An entry already filed under this key was
	// built for whatever the engine kept at this address BEFORE, and
	// TextureFor returns the FIRST match - so appending a second entry
	// for the same key leaves the wrong picture in front of the right
	// one and changes nothing on screen. That is a fix that silently
	// does not fix anything, which is the worst kind.
	const int nOld = TextureFor(pKey);
	if (nOld >= 0)
	{
		if (g_Tex[nOld].pSRV) { g_Tex[nOld].pSRV->Release(); ++g_nTexGen; }
		g_Tex.erase(g_Tex.begin() + nOld); ++g_nTexListGen;
		++g_nTexRecycled;
	}
	g_Tex.push_back(e); ++g_nTexListGen;
	++g_nTexFromFile;
	g_nResolvedWithTex = (size_t)-1;		// batches must re-resolve
	return (int)g_Tex.size() - 1;
}

// Create a D3D11 texture from an engine texture object whose data is loaded.
//
// This is called from BindTexture and nowhere else, because that is the only
// moment the pixels are known to exist: the engine loads a texture's data on
// demand and the renderer is expected to take its copy during the bind. The
// first version of this built textures during the world walk instead - which
// runs in RebindLightmaps, BEFORE any texture is bound - and created exactly
// zero of them, drawing the whole world white.
//
// pKey is the engine's texture object, which is what a surface record points
// at, so the world walk can look it up later by identity.
bool R3D_HasTexture(uint32_t pKey) { return TextureFor(pKey) >= 0; }

void R3D_NoteTexture(uint32_t pKey, uint32_t pData)
{
	if (!g_pDev || !pKey || !pData) return;

	// Already have it? Only if it is still the SAME texture. A matching key
	// with a different data object is the engine reusing the address, and
	// keeping the old pixels would draw the wrong image on every surface that
	// references it.
	const int nHave = TextureFor(pKey);
	if (nHave >= 0)
	{
		if (g_Tex[nHave].pData == pData) return;
		if (g_Tex[nHave].pSRV) { g_Tex[nHave].pSRV->Release(); ++g_nTexGen; }
		g_Tex.erase(g_Tex.begin() + nHave); ++g_nTexListGen;
		++g_nTexRecycled;
		g_nResolvedWithTex = (size_t)-1;		// batches must re-resolve
	}

	// STAGE 1, MODE 2. The file for EVERY texture the engine names, including
	// the ones the validation below refuses.
	//
	// That refusal turns out to be load bearing, and not on purpose. A texture
	// the heap path rejects gets no cache entry, so its polygons fall into the
	// untextured bucket, where the sky stand-in paints them blue - and TEX/SKY,
	// TEX/INVISIBLE and TEX/HULLMAKER are all in that set. Reading them off disk
	// works perfectly and draws exactly what they are: DEdit marker surfaces,
	// lettered "sky" and "INVISIBLE" across the top of the level.
	//
	// So this mode does not make anything worse - it stops an accident from
	// hiding a defect we already had. The defect is the marker geometry, and it
	// is stage 2's, whose render blocks are BY DEFINITION what the engine draws.
	// DO NOT fit a texture name list here; that is the mistake stage 2 exists to
	// undo. Use mode 2 when stage 2 lands, and mode 1 until then.
	if (g_bTexFromFile >= 2 && MakeFileTexture(pKey) >= 0) return;
	if (IsBadReadPtr((const void*)(uintptr_t)pData, kTexMip0 + 16)) return;

	const uint8_t* d = (const uint8_t*)(uintptr_t)pData;
	const uint32_t w = *(const uint16_t*)(d + kTexWidth);
	const uint32_t h = *(const uint16_t*)(d + kTexHeight);
	const uint8_t* m = d + kTexMip0;
	const uint32_t mw = *(const uint32_t*)(m + 0);
	const uint32_t mh = *(const uint32_t*)(m + 4);
	const uint32_t pp = *(const uint32_t*)(m + 8);
	const uint32_t mp = *(const uint32_t*)(m + 12);

	// Every one of these has to hold, or this is not a texture object and we
	// would be handing the GPU whatever memory happened to be there.
	if (!w || !h || w > 4096 || h > 4096) return;
	if (mw != w || mh != h) return;
	if (!pp) return;

	// Two formats, told apart by measurement rather than by decoding a flag.
	//
	// A mip with pitch == width*4 is 32-bit BGRA. A mip with pitch 0 is
	// compressed, and the gap to the next mip's pixels says which: 512x512
	// occupying 131072 bytes is exactly half a byte per pixel, which is DXT1.
	// The flags word at +0x18 does NOT separate them - 0x88 appears on both -
	// so the size is the honest discriminator.
	const uint32_t p1 = *(const uint32_t*)(m + 24 + 8);
	const uint64_t nGap = (p1 > pp) ? (p1 - pp) : 0;

	DXGI_FORMAT fmt;
	uint32_t nPitch, nBytes;
	if (mp == mw * 4)
	{
		fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
		nPitch = mp;
		nBytes = mp * mh;
	}
	else if (mp == 0 && nGap == (uint64_t)w * h / 2 && !(w % 4) && !(h % 4))
	{
		fmt = DXGI_FORMAT_BC1_UNORM;		// DXT1, 8 bytes per 4x4 block
		nPitch = (w / 4) * 8;
		nBytes = (w / 4) * (h / 4) * 8;
	}
	else return;

	if (IsBadReadPtr((const void*)(uintptr_t)pp, nBytes)) return;

	// STAGE 1, MODE 1. The heap copy has now been fully validated - so this
	// texture is one the renderer WOULD have drawn either way, and swapping in
	// the file's pixels changes the picture in exactly one respect: they are
	// right. Full mip chains instead of the single level the engine keeps,
	// which is what anisotropic filtering needs to do anything; pixels that
	// survive the engine freeing its copy; and an identity - the NAME - that a
	// level reload cannot scramble, which no offset in the texture object has.
	//
	// Deliberately AFTER the validation and not before it. See mode 2.
	if (g_bTexFromFile && MakeFileTexture(pKey) >= 0) return;
	++g_nTexFromHeap;

	D3D11_TEXTURE2D_DESC td{};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = fmt; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA sd{};
	sd.pSysMem = (const void*)(uintptr_t)pp; sd.SysMemPitch = nPitch;

	ID3D11Texture2D* pT = nullptr;
	if (FAILED(g_pDev->CreateTexture2D(&td, &sd, &pT))) return;
	// The pair is in hand here and nowhere else. Rank the offsets.
	if (Readable(pKey, 256))
	{
		++g_nDataPairs;
		for (int z = 0; z < 64; ++z)
			if (Word(pKey, z * 4) == pData) ++g_aDataOff[z];
		if (g_nDataPairs == 60 && !g_nDataOffSaid)
		{
			g_nDataOffSaid = 1;
			for (int z = 0; z < 64; ++z)
				if (g_aDataOff[z] * 4 >= g_nDataPairs * 3)
					Log("  R3D TEXOBJ: +%03X holds the data pointer on"
						" %ld of %ld textures", z * 4,
						g_aDataOff[z], g_nDataPairs);
			Log("  R3D TEXOBJ: %ld pairs ranked", g_nDataPairs);
		}
	}
	TexEntry e{}; e.nLoad = g_nWorldLoads; e.pTex = pKey; e.pData = pData; e.fW = (float)w; e.fH = (float)h;
	// The FILE this picture came from. Two reads and a hash lookup - see
	// TexNameOf - so it is affordable on every texture the engine binds.
	TexNameOf(pKey, e.szName, sizeof(e.szName));
	// The offset RANKING is a separate instrument and stays behind its switch.
	{ char szProbe[80]; TexNameProbe(pKey, pData, szProbe, sizeof(szProbe)); }
	// Record WHICH PICTURE this entry was built from, so a recycled object
	// can be told from a still-current one later.
	{
		uint32_t dTmp = 0, pTmp = 0, sTmp = 0;
		TexLiveId(pKey, &dTmp, &pTmp, &sTmp);
		e.pPix = pTmp; e.nSig = sTmp;
	}
	{
		double sr = 0, sg = 0, sb = 0; long ns = 0;
		long nClear = 0, nA0 = 0, nA255 = 0;
		if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM)
		{
			const uint8_t* px = (const uint8_t*)(uintptr_t)pp;
			for (uint32_t y = 0; y < h; y += 4)
				for (uint32_t x = 0; x < w; x += 4)
				{
					const uint8_t* q = px + (size_t)y * nPitch + (size_t)x * 4;
					sb += q[0]; sg += q[1]; sr += q[2]; ++ns;
					if (q[3] < 128) ++nClear;
					if (q[3] == 0) ++nA0; else if (q[3] == 255) ++nA255;
				}
		}
		else
		{
			// BC1: each 8-byte block opens with two RGB565 endpoints. Averaging
			// those is close enough to the block's mean for this purpose.
			const uint8_t* blk = (const uint8_t*)(uintptr_t)pp;
			const uint32_t nb = (w / 4) * (h / 4);
			for (uint32_t i = 0; i < nb; ++i)
			{
				// BC1 punch-through: c0 <= c1 means index 3 is transparent.
				const uint16_t a0 = *(const uint16_t*)(blk + (size_t)i * 8);
				const uint16_t a1 = *(const uint16_t*)(blk + (size_t)i * 8 + 2);
				// c0 <= c1 puts the block in punch-through mode, but a FLAT block
				// encodes c0 == c1 and is fully opaque - counting those called 229
				// textures cut-outs, including 512x512 walls. The block is only
				// transparent if it also USES index 3 somewhere in its 16 texels.
				if (a0 <= a1)
				{
					const uint32_t ix = *(const uint32_t*)(blk + (size_t)i * 8 + 4);
					for (int t = 0; t < 16; ++t)
						if (((ix >> (t * 2)) & 3u) == 3u) { ++nClear; break; }
				}
				for (int k = 0; k < 2; ++k)
				{
					const uint16_t c = *(const uint16_t*)(blk + (size_t)i * 8 + k * 2);
					sr += ((c >> 11) & 0x1F) * 255 / 31;
					sg += ((c >> 5)  & 0x3F) * 255 / 63;
					sb += ( c        & 0x1F) * 255 / 31;
					++ns;
				}
			}
		}
		if (ns) { e.mr = (float)(sr/ns); e.mg = (float)(sg/ns); e.mb = (float)(sb/ns); }
		// BC1 counts BLOCKS and BGRA counts texels; each divides by its own
		// population, so the fraction is comparable between them.
		const long nPop = (fmt == DXGI_FORMAT_BC1_UNORM)
			? (long)((w / 4) * (h / 4)) : ns;
		e.fCut = nPop ? (float)nClear / (float)nPop : 0.0f;
		// A BAND, not "> 0": all-zero alpha means the channel is unused,
		// not that the whole texture is a hole.
		// A cut-out's alpha is BIMODAL - almost every texel fully clear or
		// fully opaque. A channel nothing ever wrote is spread across the
		// range, and alpha-testing that would punch holes in solid walls.
		const float fBi = (fmt == DXGI_FORMAT_BC1_UNORM || !ns)
			? 1.0f : (float)(nA0 + nA255) / (float)ns;
		// 0.99 was too strict and it is what left the foliage solid. A leaf's
		// EDGE is anti-aliased, so a real cut-out carries a few percent of
		// intermediate alpha: NOLF's tree measures 77.4% bimodal and its palm
		// 97.0%, and both were rejected. The band on fCut is what actually
		// excludes an unused channel - those read 100% clear, every texel.
		e.fBi  = fBi;
		// THE FILE FIRST. NOLF's foliage writes "alpharef 96;" into the .dtx
		// command string, and reading it settles in one lookup what three
		// rounds of tuning a bimodality threshold could not: TREE_06S measures
		// 55.6% clear but only 35.8% bimodal - its snow edges are feathered -
		// so every statistical band wide enough to admit it also admits things
		// that must stay solid. The artist wrote the answer down in 2000.
		{
			DtxInfo dr{};
			if (e.szName[0] && Dtx_LoadInfo(e.szName, &dr) && dr.fAlphaRef > 0.0f)
				e.fCutRef = dr.fAlphaRef / 255.0f;
		}
		bool bGuessedCut = false;
		if (e.fCutRef <= 0.0f
			&& e.fCut > 0.005f && e.fCut < 0.95f && fBi > 0.60f)
		{
			e.fCutRef = 0.5f;		// no declaration: fall back on the pixels
			bGuessedCut = true;
		}
		// WHICH TEXTURES ARE BEING GUESSED AT, BY NAME.
		//
		// The file's own alpharef is the artist's answer and is trusted. The
		// fallback is this renderer GUESSING that a texture is a cut-out from
		// its pixels, and a wrong guess deletes most of a surface: a lamp
		// shade whose sheet is 89% transparent, on an OPAQUE prop the game
		// never alpha tested, loses 89% of itself and reads as a small
		// fragment hanging in the air. That is the Morocco hotel lamp
		// symptom exactly, so the guesses get named rather than made quietly.
		if (bGuessedCut && e.szName[0])
			Log("  R3D CUT GUESS: %s  %.1f%% clear, %.1f%% bimodal, NO alpharef"
				" in the file - alpha tested at 0.5 on a GUESS",
				e.szName, 100.0f * e.fCut, 100.0f * fBi);
		e.bCut = (e.fCutRef > 0.0f) ? 1 : 0;
		// THE OCCLUDER MARKER. blocker/blockerX are skipped by name already,
		// but one occluder brush is still drawn - the 4 Sept headset shot has a green
		// plank lettered OCCLUDER across the sky - so it is not in that class.
		// It is opaque BC1 with no alpha, so nothing alpha-related can hide it.
		// Identified by its own picture instead: of all 435 textures in the
		// level exactly one is 64x64 and this particular green. Logged loudly
		// on both the hit and the skip so a false positive cannot be quiet.
		e.bOccl = (w == 64 && h == 64 && e.mr < 60.0f && e.mg > 140.0f
				  && e.mb > 80.0f && e.mb < 130.0f) ? 1 : 0;
		if (e.bOccl)
			Log("  R3D OCCLUDER TEXTURE: %08X 64x64, mean %.0f %.0f %.0f"
				" - polygons using it are skipped (+StubSkipOcclTex 0 restores)",
				pKey, e.mr, e.mg, e.mb);
		// Every texture, not only the cut-outs: the question "why is this
		// foliage opaque" can only be answered by seeing what its alpha
		// actually looks like, and that is invisible if only hits are logged.
		Log("  R3D TEX: %08X %ux%u %s  mean %3.0f %3.0f %3.0f  "
			"%.1f%% clear, %.1f%% bimodal  %s",
			pKey, w, h, (fmt == DXGI_FORMAT_BC1_UNORM) ? "BC1 " : "BGRA",
			e.mr, e.mg, e.mb, 100.0f * e.fCut, 100.0f * fBi,
			e.bCut ? "<- CUT-OUT, alpha tested" : "");
	}
	// THE HEAP AGAINST THE FILE. Two independent routes to the same picture:
	// this copy was taken out of lithtech.exe's memory, and the .dtx was read
	// off disk by code that had no part in producing it. If the parse in
	// dtx.cpp is right they must agree on the dimensions and on the mean
	// colour; if a channel is swapped or a stride is wrong they cannot. This
	// is the check stage 1 is verified by, and it needs no headset.
	//
	// The mean is sampled the same way on both sides ON PURPOSE - see
	// Measure() in dtx.cpp. Comparing two numbers computed differently would
	// have proved nothing either way.
	if (e.szName[0] && g_bNameProbe)
	{
		DtxInfo fi;
		++g_nFileCmp;
		if (!Dtx_LoadInfo(e.szName, &fi)) ++g_nFileMissing;
		else
		{
			const bool bDim = (fi.nWidth == w && fi.nHeight == h);
			const float dr = fi.mr - e.mr, dg = fi.mg - e.mg, db = fi.mb - e.mb;
			// A generous band: the engine may hold a mip-reduced copy or a
			// recompressed one, so this is looking for a WRONG PICTURE, not
			// for bit equality. A channel swap moves a mean by tens.
			const bool bCol = (dr*dr + dg*dg + db*db) < 30.0f * 30.0f;
			if (bDim && bCol) ++g_nFileAgree;
			else
			{
				++g_nFileDisagree;
				Log("  R3D TEX FILE MISMATCH: %s  heap %ux%u mean %.0f %.0f %.0f"
					"  file %ux%u mean %.0f %.0f %.0f%s",
					e.szName, w, h, e.mr, e.mg, e.mb,
					fi.nWidth, fi.nHeight, fi.mr, fi.mg, fi.mb,
					bDim ? "" : "  <- DIMENSIONS DIFFER");
			}
		}
	}
	const HRESULT hr = g_pDev->CreateShaderResourceView(pT, nullptr, &e.pSRV);
	pT->Release();
	if (FAILED(hr)) return;
	g_Tex.push_back(e); ++g_nTexListGen;
}

// STAGE 2, the first half: read the level's own .DAT and prove the reading.
//
// Nothing is drawn from it here. The renderer still draws the heap, and this
// runs beside it so that the file parse can be checked against the engine's
// own numbers on every level load, before any of it is trusted with a pixel.
//
// THE RENDERER IS NEVER TOLD WHICH LEVEL IT IS. The engine hands over a world
// POINTER and nothing else, so the file is identified by what the two must
// agree about: the world model count and the bounding box. Over the game's 103
// worlds that pair collides zero times, and the box matches to the bit rather
// than to a tolerance.
static void LoadWorldFile(uint32_t pWorld)
{
	if (g_pWorldFile) { World_Release(g_pWorldFile); g_pWorldFile = nullptr; }
	if (!g_bWorldFile) return;

	float vMin[3], vMax[3];
	if (!Readable(pWorld + kWorldBoxMin, 12) || !Readable(pWorld + kWorldBoxMax, 12))
		return;
	memcpy(vMin, (const void*)(uintptr_t)(pWorld + kWorldBoxMin), 12);
	memcpy(vMax, (const void*)(uintptr_t)(pWorld + kWorldBoxMax), 12);
	const uint32_t nHeapModels = Word(pWorld, kWorldModelCount);

	// TRY MORE THAN ONCE. A failure here is not "this level has no file" - it
	// is almost always a bad read, and the cost of accepting it is that the
	// file-texture rescue goes off and most of the level is dropped as
	// untextured. Three attempts, because the failure that produced the black
	// HQ was transient and the retry is free next to a world load.
	for (int nTry = 0; nTry < 3 && !g_pWorldFile; ++nTry)
	{
		if (nTry) Log("  WORLD FILE: retrying the read (attempt %d)", nTry + 1);
		g_pWorldFile = World_Load(nHeapModels, vMin, vMax, Log);
	}
	if (!g_pWorldFile)
	{
		++g_nWorldFileFail;
		Log("  WORLD FILE: NOT IDENTIFIED after 3 attempts - the file-texture"
			" rescue is off for this level, so nothing may be dropped for"
			" want of a texture. See the account below.");
		return;
	}
	++g_nWorldFileOK;
	g_FileByName.clear();
	g_UnmatchedKind.clear();
	g_nUnmDrawn = g_nUnmInvisible = g_nUnmRemoved = 0;
	g_sUnmInvisible.clear();
	// THE ONE SAFE MOMENT for the file-texture cache: the old world's batches
	// are gone, this walk takes fresh views, the mesh runs are per pass, the
	// skin cache is keyed on g_nTexGen, and every engine-texture entry that
	// borrowed a view holds its own reference.
	if (g_bDtxFlushWanted)
	{
		g_bDtxFlushWanted = false;
		Dtx_Flush();
		g_EnvByName.clear();		// its views were the cache's; see R3D_Destroy
		SprAnim_Flush();
		++g_nTexGen;
		Log("  DTX: cache flushed at the world walk (%s)", "new world");
	}
	// The model runs standing now were built for the world being replaced.
	g_MeshRuns.clear();
	g_bMeshInvalidate = true;
	for (size_t i = 0; i < g_pWorldFile->Models.size(); ++i)
		g_FileByName[g_pWorldFile->Models[i].sName] = i;

	// THE CROSS-CHECK, and it is the only reason this call exists yet. Two
	// independently derived pictures of the same level: one walked out of the
	// engine's heap, one parsed off disk. Agreement is evidence; disagreement
	// is a bug found before it could become a picture.
	uint32_t nFilePolys = 0, nLocated = 0, nMarkerSurf = 0, nSurf = 0;
	const uint8_t* pb = nullptr; uint32_t nb = 0;
	World_Bytes(g_pWorldFile, &pb, &nb);
	std::vector<WorldSurface> surf;
	for (size_t i = 0; i < g_pWorldFile->Models.size(); ++i)
	{
		const WorldModelFile& m = g_pWorldFile->Models[i];
		nFilePolys += m.nPolygons;
		if (!m.bLocated) continue;
		++nLocated;
		if (pb && World_Surfaces(*g_pWorldFile, m, pb, nb, &surf))
		{
			nSurf += (uint32_t)surf.size();
			for (size_t k = 0; k < surf.size(); ++k)
				if (surf[k].nFlags == 202) ++nMarkerSurf;
		}
	}
	Log("  WORLD FILE: %s", g_pWorldFile->sPath.c_str());
	// THE LEVEL'S OWN LIGHTING PROPERTIES, read for the first time. Logged
	// before anything uses them: the ambient term is an input this renderer
	// has never had, and whether it belongs ON TOP of the lightmaps or is
	// already baked into them is a question the pictures have to answer.
	Log("  WORLD INFO: '%s'   ->  ambient %.3f %.3f %.3f",
		g_pWorldFile->sInfo.c_str(), g_pWorldFile->fAmbient[0],
		g_pWorldFile->fAmbient[1], g_pWorldFile->fAmbient[2]);
	Log("     models %u file / %u heap%s   polygons %u file",
		g_pWorldFile->nModelCount, nHeapModels,
		g_pWorldFile->nModelCount == nHeapModels ? "  AGREE" : "   <- DISAGREE",
		nFilePolys);
	Log("     planes located on %u of %u models; %u surfaces read, %u carry "
		"flags 202 (the markers)",
		nLocated, (unsigned)g_pWorldFile->Models.size(), nSurf, nMarkerSurf);
}

static void R3D_BuildWorldInner(uint32_t pWorld);

// RUNNING OUT OF ADDRESS SPACE SHOULD NOT BE A CRASH.
//
// On 11 September the world build threw std::bad_alloc out of a vector of
// shader resource views with 79 MB of a 32-bit address space left, and an
// uncaught C++ exception takes the process with it - the player loses the
// session with no explanation. The leak that caused it is fixed, but a build
// that cannot allocate should say so and leave the previous world on screen
// rather than end the game. The log line prints the address space, which is
// the one number that tells the next reader whether this is a leak again.
void R3D_BuildWorld(uint32_t pWorld)
{
	try
	{
		R3D_BuildWorldInner(pWorld);
	}
	catch (const std::bad_alloc&)
	{
		// AND LEAVE NO HALF-BUILT WORLD BEHIND. The build fills the batch
		// list before it replaces the vertex buffer, so an exception part way
		// through leaves batches describing ranges the buffer does not have -
		// and the draw loop would walk them. Reset to the same state
		// R3D_Destroy leaves, which makes the next frame rebuild from
		// scratch rather than draw from a description that no longer matches.
		g_Batches.clear();
		g_nPolys = 0; g_nTris = 0;
		g_pBuiltFrom = 0;

		static int s_nSaid = 0;
		if (++s_nSaid <= 4)
		{
			Log("  R3D: THE WORLD BUILD RAN OUT OF MEMORY and was abandoned"
				" (%d). The previous world stays on screen. If this repeats,"
				" something is leaking per renderer reload - compare the line"
				" below across reloads.", s_nSaid);
			LogAddressSpace("world build out of memory");
		}
	}
}

static void R3D_BuildWorldInner(uint32_t pWorld)
{
	R3D_PHASE("building the world mesh");
	// Rebuild when the world changes OR when more textures have arrived since
	// the last build. The engine binds textures as it needs them, so a single
	// build on the first world frame catches only the ones bound by then -
	// measured: 176 textures and 25174 of 26904 polygons still untextured.
	// Rebuilding costs a few milliseconds and stops on its own once the set
	// stops growing.
	if (!g_pDev || !pWorld) return;
	// NOT rebuilt when the texture count changes any more - see the note on
	// Batch. Only a new world needs new geometry.
	// +StubWorldRebuild <seconds>: rebuild the world mesh on a timer.
	//
	// A DIAGNOSTIC, not a feature. The HQ lobby doors draw closed where retail
	// draws them open, and the standing explanation is that world models are
	// built at their AUTHORED vertex positions and never updated. That assumes
	// the engine keeps a moving door's vertices still and applies a transform
	// somewhere else - which nobody has checked. If instead the engine writes
	// the moved vertices back into the heap, then simply rebuilding shows the
	// door open, and the whole transform pipeline is unnecessary.
	//
	// One switch settles which world we are in.
	if (g_nWorldRebuildMs > 0 && pWorld == g_pBuiltFrom)
	{
		static uint32_t s_nLastMs = 0;
		const uint32_t nNow = GetTickCount();
		if (nNow - s_nLastMs >= (uint32_t)g_nWorldRebuildMs)
		{
			s_nLastMs = nNow;
			g_pBuiltFrom = 0;		// fall through and rebuild
		}
	}
	// REBUILD WHILE THE TEXTURES ARE STILL ARRIVING.
	//
	// The engine binds a level's textures as it needs them, so the set is
	// still filling while we build. On the FIRST world of a session that race
	// is usually won. On the SECOND it is not: the build sees every surface
	// pointing at texture 00000000 and 18453 of M01S02's 26904 polygons come
	// out untextured - which is the black HQ and the empty blue Morocco, with
	// the props and the people still drawing because they are not world
	// geometry.
	//
	// This existed once and was removed because rebuilding left every batch
	// holding a null SRV and drew black. That is fixed: a build now ends with
	// g_nResolvedWithTex = -1 and every batch resolves again. So it comes back.
	//
	// It stops on its own. Both conditions have to hold - the last build left
	// polygons untextured AND more textures have been bound since - and each
	// stops being true once the level is bound. The cap is there so that a
	// level with genuinely untexturable geometry cannot rebuild every frame
	// forever.
	// A NEW LOAD, not just a new pointer. A Morocco save, loaded after
	// three levels, came back in the SAME world block the previous level had
	// used: the pointer matched, these counters kept the old level's numbers,
	// and "still improving" compared 14076 untextured polygons against the
	// old level's 149 - false, so the rebuild that repairs a build made
	// before the textures are readable never fired, and the whole level drew
	// in the untextured stand-in - the whole world gone again in the headset.
	if (pWorld != g_pRetexWorld || g_nWorldLoads != g_nRetexLoad)
	{
		g_pRetexWorld = pWorld;
		g_nRetexLoad = g_nWorldLoads;
		g_nRetexBuilds = 0;
		g_nNoTexPrev = 0;
		g_nRetexStalls = 0;
		g_nRetexSeen = -1;
		g_bRetexGaveUp = false;
	}
	// Rebuild only while it is WORTH it and still WORKING: more than a handful
	// of polygons outstanding, more textures bound since, and the count not
	// stuck. Without the last two tests this chases one stubborn polygon to
	// the cap on every level.
	//
	// "STUCK" IS SEVERAL REBUILDS, NOT ONE. It used to be one: a rebuild
	// that improved nothing ended the loop. But a model's file textures are
	// only trusted once at least one of its textures has been BOUND by the
	// engine (the bridge test below), and the engine binds a level's
	// textures over several frames after the load - with sprites and menu
	// art arriving in between. So the second world of a session built with
	// 0 textures known, rebuilt once when 7 had arrived (all of them sprite
	// sheets), improved nothing, and stopped: 9335 polygons of the HQ never
	// drawn, and the same in Morocco after the summary. "The whole world is
	// black again, with missing walls and buildings." Judged once per build
	// result, and it takes four barren rebuilds in a row to give up.
	if (g_nRetexSeen != g_nRetexBuilds)
	{
		g_nRetexSeen = g_nRetexBuilds;
		if (g_nNoTexPrev == 0 || g_nNoTexAtBuild < g_nNoTexPrev) g_nRetexStalls = 0;
		else ++g_nRetexStalls;
	}
	const bool bStuck = (g_nRetexStalls >= 4 || g_nRetexBuilds >= 16);
	if (bStuck && !g_bRetexGaveUp && g_nNoTexAtBuild > kRetexFloor)
	{
		g_bRetexGaveUp = true;
		Log("  R3D REBUILD: giving up after %d rebuilds (%d barren in a row) with %d"
			" polygons still untextured - this level will draw incomplete",
			g_nRetexBuilds, g_nRetexStalls, g_nNoTexAtBuild);
	}
	if (pWorld == g_pBuiltFrom && g_nNoTexAtBuild > kRetexFloor && !bStuck
		&& g_Tex.size() > g_nBuiltWithTex)
	{
		++g_nRetexBuilds;
		g_nNoTexPrev = g_nNoTexAtBuild;
		Log("  R3D REBUILD %d: the last build left %d polygons with no texture,"
			" and %u textures have been bound since - building again",
			g_nRetexBuilds, g_nNoTexAtBuild,
			(unsigned)(g_Tex.size() - g_nBuiltWithTex));
		g_pBuiltFrom = 0;
	}

	if (pWorld == g_pBuiltFrom
		&& g_bBuiltLM == g_bLMEnable) return;
	g_bBuiltLM = g_bLMEnable;
	g_pBuiltFrom = pWorld;
	LoadWorldFile(pWorld);
	g_nBuiltWithTex = g_Tex.size();
	++g_nBuilds;
	if (g_pVB) { g_pVB->Release(); g_pVB = nullptr; }
	g_nPolys = 0; g_nTris = 0;

	const uint32_t pList = Word(pWorld, kWorldModels);
	const uint32_t nList = Word(pWorld, kWorldModelCount);
	if (!Readable(pList, 4) || !nList || nList > 65536)
	{ Log("  R3D: no WorldModel array at +0x18C"); return; }

	// Triangles collected per texture, then concatenated, so the frame is one
	// draw per texture rather than one per polygon.
	// The lightmaps, decoded and packed once per world. This runs before
	// the walk because the walk needs a rectangle for every polygon it
	// reaches, and it is a no-op on every rebuild after the first.
	LM_Build(pWorld, g_pDev, Log, g_szLMDump);
	g_szLMDump = "";					// once per process, not once per build

	// A bucket is now keyed by TEXTURE **and** WORLD MODEL, for every model
	// except the two BSPs. Merging a door's polygons into the level-wide batch
	// for its texture makes it impossible to draw that door anywhere else, and
	// drawing it somewhere else is the entire point. The BSPs keep sub 0 and
	// stay merged, so the bulk of the level is still a handful of batches.
	std::vector<uint32_t> bucketSub;
	std::vector<uint32_t> bucketKey;		// bucket n+1 draws engine texture n
	// Index-aligned with bucketKey. Empty for a bucket the engine
	// textured; the .DAT's own texture name for one it did not.
	std::vector<std::string> bucketFile;
	int nFromWorldFile = 0;		// polygons rescued by the level file
	int nFromEngineName = 0;	// rescued by the engine texture's own name
	std::vector< std::vector<Vtx> > buckets;
	buckets.resize(1);					// bucket 0 is "no texture"
	// The backdrop's own bucket space, so a texture shared with the world -
	// Invisible.dtx is, on every level - cannot drag world geometry into the
	// depthless sky pass. Same shape as the pair above, drawn separately.
	// The page cache describes the PREVIOUS world's allocations. A build is
	// exactly the moment they stop being true.
	FlushPageCache();
	g_SubPtrs.clear();
	g_WMBase.clear();
	memset(g_aWObjHit, 0, sizeof g_aWObjHit);
	g_nWObjSeen = 0; g_bWObjSaid = 0;

	// +StubBSPCover 1: does drawing PhysicsBSP alone leave any VisBSP geometry
	// out? Asked geometrically rather than by counting textures.
	BSPCoverReport(pWorld);

	// Cleared PER BUILD. A level with no SkyBox must not inherit the last
	// level's centre - it would draw that panorama around the wrong point and
	// nothing in the picture would say so.
	g_bHaveSky = false;
	g_nSkyPolys = 0;
	std::vector<uint32_t>    skyKey;
	std::vector<std::string> skyFile;
	std::vector<uint32_t>    skySub;		// one bucket per (texture, model)
	std::vector<float>       skyOrder;
	std::vector<int>         skyBlend;
	int nSkyObjModels = 0;
	std::vector< std::vector<Vtx> > skyBuckets;
	skyBuckets.resize(1);
	std::vector<Vtx> occBucket;			// sky brushes, depth-only - see g_bSkyOccluder
	g_nSkyOccPolys = 0;
	// Glass keeps its own bucket space too, for the same reason as the sky: it
	// is a separate PASS, and a texture shared with the opaque world must not
	// drag opaque geometry into it.
	std::vector<uint32_t>    transKey;
	std::vector<std::string> transFile;
	std::vector< std::vector<Vtx> > transBuckets;
	transBuckets.resize(1);
	g_nTransPolys = 0;
	int nModels = 0, nNoTex = 0, nLit = 0;
	// WHO FELL TO THE LIGHT GRID, by model. The 12 September batch showed the
	// grid's stand-in wrong where its cells are coarse or coloured - the
	// aeroplane cabin, the Alps snow, the chateau rubble - and the count of
	// lightmap-less polygons alone could not name the model. This can.
	std::map<std::string, uint32_t> nNoLMByModel;
	// AND THE SKY: does the level's sky carry lightmaps, and how bright? Retail
	// lights sky brushes like any brush; ours draws the sky unlit, and the
	// Bremen harbour's came out pink and 1.7x too bright against retail.
	int nSkyLit = 0, nSkyUnlit = 0; double fSkyLitSum = 0.0; long nSkyLitTexels = 0;
	int nVtxWhite = 0, nVtxTinted = 0;
	double fSkyVC[3] = { 0, 0, 0 }; long nSkyVC = 0; int nSkyVCWhite = 0;
	// THE MODEL LIGHT GRID IS FILLED FROM THIS WALK. Every lightmapped
	// polygon is about to be visited anyway, which is the only reason this
	// costs nothing: the light a model stands in is read out of the bake as
	// the bake goes past.
	if (g_pWorldFile)
	{
		g_fWorldAmbient[0] = g_pWorldFile->fAmbient[0];
		g_fWorldAmbient[1] = g_pWorldFile->fAmbient[1];
		g_fWorldAmbient[2] = g_pWorldFile->fAmbient[2];
		LGridBegin(g_pWorldFile->fBoxMin, g_pWorldFile->fBoxMax);
	}
	int nNoTexTrans = 0;		// of those, on a translucent world model
	int nSkippedModels = 0, nSkippedPolys = 0;
	// PER BUILD, not cumulative. THE ACCOUNT below has to add up to the
	// level's own polygon count, and a running total from three level
	// loads ago cannot.
	int nMarkerSkipBuild = 0, nNoTexSkipBuild = 0, nNoTexTransBuild = 0;
	// Polygons we would have dropped, kept because the level file - and so the
	// rescue that would have textured them - was never identified.
	int nNoTexKeptNoFile = 0;
	// EVERY OTHER WAY A POLYGON CAN LEAVE THIS LOOP. The account read 280
	// UNACCOUNTED on m01s02 because these paths existed and not one of them was
	// counted: a polygon dropped silently is indistinguishable from one that was
	// never in the file. Each `continue` below now names itself, so UNACCOUNTED
	// is zero by construction and any residue left over is a real disagreement
	// with the file rather than a bucket nobody wrote.
	// Every polygon the HEAP declares, so there is a denominator on a level
	// whose .DAT could not be identified.
	int nHeapTotalPolys = 0;
	// WHERE THE SKY'S 36 POLYGONS GO. The backdrop was detected, named and
	// measured and then drew nothing, and a count of what reached the buffer
	// cannot say which of nine skips took them.
	int nSkyWhere[10] = { 0 };
	int nModelUnread = 0, nModelUnreadPolys = 0;
	int nModelBadArr = 0, nModelBadArrPolys = 0;
	int nPolyUnread = 0, nPolyBadNv = 0, nPolyVertsUnread = 0, nPolyVertsBad = 0;
	int nPolyDegen = 0, nSkipFlagPolys = 0, nMarkerFlagPolys = 0;
	// {engine texture object, polygons that wanted it and did not get it}
	std::vector<std::pair<uint32_t, int>> missing;

	// Every WorldModel, not just VisBSP. The doors and the breakables are
	// geometry too, and drawing all of them is both simpler and more honest
	// than drawing the level and wondering where the doors went. Their own
	// positions are not applied yet, so anything that has moved since load
	// will be drawn where it was authored.
	for (uint32_t i = 0; i < nList; ++i)
	{
		const uint32_t sub = Word(Word(pList, i * 4), kSubFromElement);
		if (!Readable(sub, 0xB0))
		{
			++nModelUnread;
			if (Readable(sub, kPolyCount + 4))
			{
				const int nP = (int)Word(sub, kPolyCount);
				nModelUnreadPolys += nP; nHeapTotalPolys += nP;
			}
			continue;
		}

		// GEOMETRY THE ENGINE NEVER SHOWS. Two kinds, both found by the ray
		// probe rather than reasoned about:
		//
		//   PhysicsBSP - the collision twin of the level. 13724 polygons,
		//     more than twice VisBSP's 5957, sitting coplanar with the walls
		//     it stands in for. The probe fired along the player's gaze hit
		//     THIS, not the visible world: what the tester was looking at was the
		//     collision hull drawn over the street.
		//
		//   AIVolume* - the AI volumes DEdit writes into the world, 322 of
		//     them carrying 2008 polygons, textured with the editor's marker.
		//     The probe fired straight down hit AIVolume174, which is the
		//     blue ground with yellow shapes reported in headset testing.
		//
		// By NAME, and that is a deliberate retreat. Four searches went
		// looking for the bit the engine uses - over the model header, over
		// the array element, over the polygon and over the surface record -
		// and the only perfect separators any of them found were artefacts:
		// bit 13 of the inline name, and bits of two heap POINTERS whose
		// groups were allocated in different regions. The one real candidate
		// (polygon +0C/+10/+14, zero on every VisBSP polygon and set on every
		// volume's) is set on 96 of the doors' polygons and 3889 of the
		// translucent brushes' too, so it means "not in the render tree" and
		// using it would hide the moving parts of the level.
		//
		// A name is weaker than a flag and it is honest about being weaker.
		// Both of these names are generated - PhysicsBSP by the engine,
		// AIVolume by NOLF's own editor classes - so they are stable across
		// levels, and +StubSkipHidden 0 puts them back for an A/B.
		if (SkippedModel(sub))
		{
			const int nP = (int)Word(sub, kPolyCount);
			nSkippedModels++; nSkippedPolys += nP; nHeapTotalPolys += nP;
			continue;
		}

		const uint32_t pPolys = Word(sub, kPolyArray), nPolys = Word(sub, kPolyCount);
		const uint32_t pVerts = Word(sub, kVertArray), nVerts = Word(sub, kVertCount);
		nHeapTotalPolys += (int)nPolys;
		// Every world model this build knows about, for the object<->model probe.
		g_SubPtrs.push_back(sub);
		if (!Readable(pPolys, 4) || !nPolys || !Readable(pVerts, 12) || !nVerts)
		{ ++nModelBadArr; nModelBadArrPolys += (int)nPolys; continue; }
		++nModels;

		// Once per model, not once per polygon - a string compare against 26904
		// polygons is not free.
		char szModelName[64];
		ModelName(sub, szModelName, sizeof szModelName);
		// WHAT IS SEE-THROUGH, and it is not only TranslucentWorldModel.
		//
		// That was the whole test, and it is why the HQ lobby's glass double doors
		// draw as a solid slab: they are `PartitionGlass`, 148 models and 1938
		// polygons game-wide, and there is also `Glass`, `mdglass`, `slidingglass`,
		// `HitGlass`, `fence`, `grate`, `window`, `WireMesh`, `Water`...
		//
		// Level designers named these by hand over three years, so a fixed list
		// would miss the tail (`mdglass`, `03GlassWall`, `cat grate`). A substring
		// test over the words they actually used covers 99% of the polygons in
		// that census and degrades gracefully on the rest.
		//
		// This is a NAME rule and it is weaker than a flag, exactly like the
		// marker rule. The principled version is NOLF's authored per-object alpha,
		// which lives on the entity rather than on the geometry or the texture -
		// the glass here wears ordinary sign and metal textures, with no alpha
		// channel to read. Bridging that from the client is the real fix.
		// +StubTransNames 0 goes back to TranslucentWorldModel only.
		// THE NAME RULE IS GONE, and it deserved to go. Guessing "is this glass"
		// from the model's name made drinking fountains and vending machines
		// see-through and flickering, because one of them is called
		// Water_Fountain and "water" was on the list. NOLF's own
		// TranslucentWorldModel class is used for plenty of OPAQUE props too, so
		// even the narrow test was wrong.
		//
		// The object knows its own alpha and now sends it. Every world model
		// except the two BSPs already has its own batches - that is what the door
		// transform needed - so the decision moves to DRAW time, per model, from
		// the number the level author actually set.
		const bool bTransModel = false;
		// The backdrop. Its own bounding-box centre is the point the camera has
		// to sit at for the panorama to line up, and it is taken from the
		// VERTICES rather than from a header field so it needs nothing new to
		// be true about the model layout.
		// The BSPs stay merged (sub 0); every other world model is keyed on its
		// own struct so it can be drawn with its own matrix when it moves.
		const uint32_t nSubKey =
			(strcmp(szModelName, "PhysicsBSP") == 0 ||
			 strcmp(szModelName, "VisBSP") == 0) ? 0u : sub;
		const bool bSkyBoxModel = g_bSkyBox && (_stricmp(szModelName, "SkyBox") == 0);
		// A model a SkyPointer names joins the sky, in the pointer's order.
		float fSkyOrderM = 0.0f; bool bSkyObj = false;
		if (g_bSkyBox && !bSkyBoxModel && g_pWorldFile)
			for (size_t so = 0; so < g_pWorldFile->Sky.size(); ++so)
				if (_stricmp(g_pWorldFile->Sky[so].sName.c_str(), szModelName) == 0)
				{ bSkyObj = true; fSkyOrderM = g_pWorldFile->Sky[so].fIndex; break; }
		const bool bSkyModel = bSkyBoxModel || bSkyObj;
		if (bSkyObj)
		{
			++nSkyObjModels;
			Log("  R3D SKY OBJECT: %s (order %.0f, %u polygons) joins the sky",
				szModelName, fSkyOrderM, nPolys);
		}
		if (bSkyBoxModel && Readable(pVerts, (size_t)nVerts * 12))
		{
			float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
			const float* vv = (const float*)(uintptr_t)pVerts;
			for (uint32_t z = 0; z < nVerts; ++z)
				for (int k = 0; k < 3; ++k)
				{
					const float f = vv[z * 3 + k];
					if (f < lo[k]) lo[k] = f;
					if (f > hi[k]) hi[k] = f;
				}
			if (lo[0] < hi[0])
			{
				for (int k = 0; k < 3; ++k) g_fSkyCentre[k] = (lo[k] + hi[k]) * 0.5f;
				g_bHaveSky = true;
				Log("  R3D SKY: %s, %u polygons, centre (%.0f %.0f %.0f),"
					" box %.0f x %.0f x %.0f", szModelName, nPolys,
					g_fSkyCentre[0], g_fSkyCentre[1], g_fSkyCentre[2],
					hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]);
			}
		}

		// THE CORRELATION PROBE. Stage 2 needs the FILE's surface flags
		// against the HEAP's polygons, and the cheapest bridge would be an
		// index: if a model's heap surface records form a regular array,
		// then (pointer - base) / stride is the same ordinal the file uses,
		// and no geometry has to be matched at all.
		//
		// That is an assumption, so it is measured before it is built on.
		// Reported per model: how many distinct surfaces the polygons
		// point at, the stride between them, whether every pointer lands
		// exactly on a stride boundary, and how the count compares with
		// what the FILE declares for the model of the same name.
		// ---- THE BRIDGE: a heap polygon's surface, as a FILE index ----
		//
		// Measured before it was built on, and it held on 565 of 565 models:
		// a model's heap surface records are a DENSE array at stride 72, every
		// pointer on a boundary, the span exactly the surface count the FILE
		// declares. So the ordinal is the bridge and no geometry has to be
		// matched: index = (pointer - base) / 72.
		//
		// Every one of those conditions is RE-CHECKED here per model rather
		// than assumed from that run, and a model that fails any of them
		// simply draws the way it did before. A bridge that cannot prove
		// itself must not be allowed to delete geometry.
		std::vector<WorldSurface> fileSurf;
		const std::vector<std::string>* fileTex = nullptr;
		uint32_t nSurfBase = 0, nSurfStride = 72;
		bool bFileLocated = false;		// the file's planes matched this heap model
		if ((g_nMarkerFlags || g_bBridgeCheck || g_bFileTex) && g_pWorldFile)
		{
			std::map<std::string, size_t>::const_iterator it =
				g_FileByName.find(szModelName);
			if (it != g_FileByName.end())
			{
				const WorldModelFile& fm = g_pWorldFile->Models[it->second];
				bFileLocated = fm.bLocated;
				uint32_t lo = 0xFFFFFFFFu, hi = 0;
				uint32_t nSeen = 0;
				for (uint32_t j = 0; j < nPolys; ++j)
				{
					const uint32_t po = Word(pPolys, j * 4);
					if (!Readable(po, kPolyVertList)) continue;
					const uint32_t sf = Word(po, kPolySurface);
					if (!sf) continue;
					if (sf < lo) lo = sf;
					if (sf > hi) hi = sf;
					++nSeen;
				}
				// The span must be exactly the declared count, which is what
				// says the array is dense and that `lo` is surface 0 rather
				// than merely the lowest one this model happens to use.
				const bool bDense = nSeen && fm.nSurfaces
					&& ((hi - lo) % nSurfStride) == 0
					&& (hi - lo) / nSurfStride + 1 == fm.nSurfaces;
				const uint8_t* pb = nullptr; uint32_t nb = 0;
				if (bDense && World_Bytes(g_pWorldFile, &pb, &nb)
					&& World_Surfaces(*g_pWorldFile, fm, pb, nb, &fileSurf))
				{
					nSurfBase = lo;
					fileTex = &fm.Textures;
					++g_nFileMapped;
				}
				else { fileSurf.clear(); fileTex = nullptr; ++g_nFileUnmapped; }
			}
			else ++g_nFileUnmapped;
		}

		// ---- VERIFY THE BRIDGE BEFORE IT IS ALLOWED TO DELETE ANYTHING --
		//
		// The dense-array test says the mapping is PLAUSIBLE. It does not
		// say it is RIGHT, and the difference cost real scenery: on t01s02
		// the model Door57 passes the density test and still maps polygon
		// 22 to surface 16, where the file says Invisible.dtx and the
		// engine had bound WA0010.DTX. Fifteen polygons of water would
		// have been deleted as markers. In range is what small numbers do.
		//
		// So every model earns the rule the same way, on every load: the
		// FILE's texture name for a surface must equal the name of the
		// texture the ENGINE actually bound for a polygon using it. One
		// disagreement anywhere in the model and the rule is off for that
		// model - it draws exactly as it did before.
		//
		// This also makes a STALE world file harmless. If the wrong .DAT
		// were ever mapped onto a level, it would fail here wholesale
		// rather than quietly deleting whatever its flags happened to say.
		bool bBridgeOK = false;
		if (!fileSurf.empty() && fileTex)
		{
			long nAgree = 0, nBad = 0;
			for (uint32_t j = 0; j < nPolys; ++j)
			{
				const uint32_t po = Word(pPolys, j * 4);
				if (!Readable(po, kPolyVertList)) continue;
				const uint32_t sfp = Word(po, kPolySurface);
				if (sfp < nSurfBase) { ++nBad; break; }
				const uint32_t txh = Readable(sfp, kSurfaceTexture + 4)
					? Word(sfp, kSurfaceTexture) : 0;
				const int tih = TextureFor(txh);
				if (tih < 0 || !g_Tex[tih].szName[0]) { ++g_nBridgeNoName; continue; }
				const uint32_t si = (sfp - nSurfBase) / nSurfStride;
				if (si >= fileSurf.size()
					|| fileSurf[si].nTexture >= fileTex->size()) { ++nBad; break; }
				const char* a = BaseName((*fileTex)[fileSurf[si].nTexture].c_str());
				const char* b = BaseName(g_Tex[tih].szName);
				if (_stricmp(a, b) == 0) { ++nAgree; continue; }
				++nBad;
				if (g_bBridgeCheck && g_nBridgeSaid < 10)
				{
					++g_nBridgeSaid;
					Log("     BRIDGE MISMATCH %-22s poly %5u surface %5u  file %-20s heap %s", szModelName, j, si, a, b);
				}
				break;
			}
			g_nBridgeAgree += nAgree;
			g_nBridgeDisagree += nBad;
			// A model nothing could be compared on has NOT earned the rule.
			// Zero disagreements out of zero comparisons is the shape of
			// answer this file has been fooled by twice before.
			// GEOMETRY CAN VOUCH WHERE NO TEXTURE HAS BEEN BOUND YET. The
			// engine binds a level's textures lazily, as the player looks at
			// things, so on every world after the first most models have NO
			// bound texture when the walk runs - zero comparisons, and the
			// rule above refused them all. The level then drew nine polygons
			// of seventeen thousand, and the rebuilds that were meant to
			// catch up gave up (see the retex loop). But a model whose file
			// planes matched its heap polygons (bLocated, checked at load)
			// has already proved the file is this level's, which is all the
			// name test was for. One disagreement still rejects the model.
			bBridgeOK = (nBad == 0 && (nAgree > 0 || bFileLocated));
			if (nBad == 0 && nAgree == 0 && bFileLocated) ++g_nBridgeByPlane;
			if (bBridgeOK) ++g_nBridgeVerified; else ++g_nBridgeRejected;
		}

		if (g_bSurfProbe)
		{
			std::vector<uint32_t> sp;
			sp.reserve(nPolys);
			for (uint32_t j = 0; j < nPolys; ++j)
			{
				const uint32_t po = Word(pPolys, j * 4);
				if (!Readable(po, kPolyVertList)) continue;
				const uint32_t sf = Word(po, kPolySurface);
				if (sf) sp.push_back(sf);
			}
			std::sort(sp.begin(), sp.end());
			sp.erase(std::unique(sp.begin(), sp.end()), sp.end());
			if (sp.size() >= 2)
			{
				// The stride is the smallest gap between distinct records,
				// which is right whenever any two are adjacent - and a model
				// with dozens of surfaces has adjacent ones.
				uint32_t nStride = 0xFFFFFFFFu;
				for (size_t k = 1; k < sp.size(); ++k)
				{
					const uint32_t d = sp[k] - sp[k - 1];
					if (d && d < nStride) nStride = d;
				}
				uint32_t nAligned = 0;
				if (nStride && nStride != 0xFFFFFFFFu)
					for (size_t k = 0; k < sp.size(); ++k)
						if (((sp[k] - sp[0]) % nStride) == 0) ++nAligned;
				const uint32_t nSpan = (nStride && nStride != 0xFFFFFFFFu)
					? (sp.back() - sp.front()) / nStride + 1 : 0;

				// What the FILE says for the model of this name.
				uint32_t nFileSurf = 0, nFilePoly = 0;
				if (g_pWorldFile)
					for (size_t q = 0; q < g_pWorldFile->Models.size(); ++q)
						if (g_pWorldFile->Models[q].sName == szModelName)
						{
							nFileSurf = g_pWorldFile->Models[q].nSurfaces;
							nFilePoly = g_pWorldFile->Models[q].nPolygons;
							break;
						}
				++g_nProbeModels;
				if (nAligned == sp.size()) ++g_nProbeAligned;
				if (nSpan == sp.size()) ++g_nProbeDense;
				if (nFileSurf && nSpan == nFileSurf) ++g_nProbeSpanEqFile;
				if (nFilePoly && nFilePoly == nPolys) ++g_nProbePolyEq;
				if (nFileSurf) ++g_nProbeNamed;
				if (g_nProbeSaid < 12)
				{
					++g_nProbeSaid;
					Log("     SURF PROBE %-22s heap polys %5u surfaces %5u  stride %3u  aligned %5u  span %5u | file polys %5u surfaces %5u",
						szModelName, nPolys, (unsigned)sp.size(), nStride,
						nAligned, nSpan, nFilePoly, nFileSurf);
				}
			}
		}

		for (uint32_t j = 0; j < nPolys; ++j)
		{
			const uint32_t po = Word(pPolys, j * 4);
			if (!Readable(po, kPolyVertList)) { ++nPolyUnread; if (bSkyModel) ++nSkyWhere[0]; continue; }
			const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + kPolyVertCount);
			if (nv < 3 || nv > 256) { ++nPolyBadNv; if (bSkyModel) ++nSkyWhere[1]; continue; }

			// ---- THE MARKER RULE, BY NAME ----
			// The file says which texture this surface wears. If it is one of
			// the editor's markers the engine never shows it, so neither do we
			// - per SURFACE, so it reaches the markers sitting inside ordinary
			// models, which a per-model name list can never see.
			if (g_bMarkerTex && bBridgeOK && !fileSurf.empty() && fileTex)
			{
				const uint32_t sfp = Word(po, kPolySurface);
				if (sfp >= nSurfBase)
				{
					const uint32_t si = (sfp - nSurfBase) / nSurfStride;
					if (si < fileSurf.size()
						&& fileSurf[si].nTexture < fileTex->size()
						&& IsMarkerTexture(BaseName(
							(*fileTex)[fileSurf[si].nTexture].c_str())))
					{
						// A SKY BRUSH keeps its polygon as a depth-only
						// occluder; the other markers are dropped as before.
						if (g_bSkyOccluder && !bSkyModel && nv >= 3 && nv <= 256
							&& _stricmp(BaseName((*fileTex)[fileSurf[si].nTexture].c_str()), "SKY.DTX") == 0
							&& Readable(po + kPolyVertList, nv * kPolyVertStride))
						{
							float q[256][3]; bool bQ = true;
							for (uint32_t t = 0; t < nv; ++t)
							{
								const uint32_t vp = Word(po + kPolyVertList, t * kPolyVertStride);
								if (vp < pVerts || vp >= pVerts + nVerts * 12 || ((vp - pVerts) % 12)) { bQ = false; break; }
								memcpy(q[t], (const void*)(uintptr_t)vp, 12);
							}
							if (bQ) PushOccluderFan(occBucket, q, nv);
						}
						++g_nMarkerTexSkipped; ++nMarkerSkipBuild; if (bSkyModel) ++nSkyWhere[2]; continue;
					}
				}
			}

			// THE MARKER RULE. The file says this surface is AI.dtx or
			// Invisible.dtx, so the engine never draws it and neither do we -
			// per surface, so it reaches the markers sitting inside ordinary
			// models, which is the case the name list cannot see.
			// `g_nMarkerFlags &&` - AND IT WAS MISSING, WHICH DELETED GEOMETRY
			// ACROSS THE WHOLE GAME.
			//
			// +StubMarkerFlags defaults to 0, and every other line about this rule
			// treats 0 as OFF: the report is guarded `if (g_nMarkerFlags)` and does
			// not print. The SKIP was not guarded, so the test read
			// `fileSurf[si].nFlags == 0` and quietly dropped every polygon whose
			// file surface flags happen to be zero - which is ordinary geometry.
			//
			// It cost 280 polygons on m01s02, and they are exactly the 280 the
			// account could not explain before every skip in this loop was made to
			// name itself. Among them are all SIX panorama faces of the level's
			// SkyBox, whose flags are 0 - so this is also why there has never been
			// a sky. A rule whose default value is "off" has to say so where it
			// acts, not only where it reports.
			if (g_nMarkerFlags && bBridgeOK && !fileSurf.empty())
			{
				const uint32_t sfp = Word(po, kPolySurface);
				if (sfp >= nSurfBase)
				{
					const uint32_t si = (sfp - nSurfBase) / nSurfStride;
					if (si < fileSurf.size()
						&& fileSurf[si].nFlags == g_nMarkerFlags)
					{ ++g_nMarkerSkipped; ++nMarkerFlagPolys; if (bSkyModel) ++nSkyWhere[3]; continue; }
				}
			}

			// +StubSkipFlags <mask>: drop every polygon whose surface flags
			// AND with the mask. The instrument for finding which bit means
			// "the engine does not render this".
			if (g_nSkipFlags)
			{
				const uint32_t sfs = Word(po, kPolySurface);
				if (Readable(sfs, kSurfaceFlags2 + 4)
					&& (Word(sfs, kSurfaceFlags2) & g_nSkipFlags))
				{ ++nSkipFlagPolys; continue; }
			}
			if (!Readable(po + kPolyVertList, nv * kPolyVertStride))
				{ ++nPolyVertsUnread; if (bSkyModel) ++nSkyWhere[5]; continue; }

			// Gather, rejecting anything that is not exactly a vertex. The
			// check that validated this layout over all 5957 polygons is the
			// same one applied here per polygon, so a world whose layout
			// differs draws nothing rather than drawing garbage.
			float p[256][3];
			bool bOK = true;
			for (uint32_t t = 0; t < nv; ++t)
			{
				const uint32_t vp = Word(po + kPolyVertList, t * kPolyVertStride);
				if (vp < pVerts || vp >= pVerts + nVerts * 12 || ((vp - pVerts) % 12))
				{ bOK = false; break; }
				memcpy(p[t], (const void*)(uintptr_t)vp, 12);
			}
			if (!bOK) { ++nPolyVertsBad; if (bSkyModel) ++nSkyWhere[6]; continue; }

			// The texture, and the size to normalise this polygon's UVs by.
			const uint32_t sf  = Word(po, kPolySurface);

			// A WATER VOLUME'S TOP FACE IS DRAWN BY ITS POLYGRID, NOT BY US.
			//
			// VolumeBrush::CreateSurface puts a PolyGrid at the brush's top
			// minus SurfaceHeight/2 and animates it - but the brush's own top
			// face is still there in the world geometry, 1.5 units above it on
			// the HQ pool. Drawing both puts a STATIC flat semi-transparent
			// quad over the moving one, and the static one wins: it is nearer,
			// it never changes, and it is the whole surface rather than a
			// 7x7 ripple. the water read as an
			// almost flat plane of glass, duplicated, with two or so
			// layers lying on top of each other.
			//
			// Only the HORIZONTAL faces go. The vertical ones are the
			// waterfall and the pool walls, which are the picture.
			// +StubWaterTop 0 draws them again.
			if (g_bWaterTop)
			{
				// WHICH NAME IS AVAILABLE? The file bridge is not always up,
				// and this brush's batch is built from the ENGINE's texture -
				// the first version of this rule asked the file alone and
				// never fired once. Ask the file first, fall back to the
				// engine's own texture object.
				char szW[80] = { 0 };
				if (bBridgeOK && !fileSurf.empty() && fileTex && sf >= nSurfBase)
				{
					const uint32_t si = (sf - nSurfBase) / nSurfStride;
					if (si < fileSurf.size() && fileSurf[si].nTexture < fileTex->size())
						strncpy(szW, (*fileTex)[fileSurf[si].nTexture].c_str(), 79);
				}
				if (!szW[0])
				{
					const uint32_t txw = Readable(sf, kSurfaceTexture + 4)
									   ? Word(sf, kSurfaceTexture) : 0;
					if (txw) TexNameOf(txw, szW, sizeof szW);
				}
				if (szW[0])
				{
					++g_nWaterTopGate;
					// THE FULL PATH, NOT THE BASENAME. IsWaterName looks for
					// "\WATER\\" in the directory, so a basename can never
					// match it - the first instrumented run said 8957
					// surfaces named and 0 of them water, on a level whose
					// water batch the same function had already flagged.
					if (IsWaterName(szW))
					{
						++g_nWaterTopNamed;
						bool bFlat = true;
						for (uint32_t t2 = 1; t2 < nv; ++t2)
							if (fabsf(p[t2][1] - p[0][1]) > 0.5f) { bFlat = false; break; }
						// ...unless this brush has NO PolyGrid (ShowSurface 0): then its
						// own top face is the water and must stay.
						bool bNoGrid = false;
						int nShow = -1;
						if (g_pWorldFile)
						{
							std::map<std::string, int>::const_iterator sg = g_pWorldFile->ShowSurface.find(szModelName);
							if (sg != g_pWorldFile->ShowSurface.end()) nShow = sg->second;
							if (nShow == 0) bNoGrid = true;
						}
						// ONLY A VOLUME THAT HAS A SURFACE OBJECT. The skip was for
						// faces a PolyGrid redraws, and only a water volume with
						// ShowSurface 1 gets one. A flat water face on ordinary
						// level geometry (PhysicsBSP, a door) has no PolyGrid
						// over it, and skipping it deleted the sea: the whole
						// open water round the freighter in M05S05's opening
						// cutscene (141 faces), 366 faces in M06S03, a few in
						// the HQ, the club and Berlin - the frame showed the
						// clear, which is the fog colour (desk and a retail
						// capture, 22 September). +StubWaterTop 2 puts the old
						// every-flat-water-face rule back; 0 skips nothing.
						const bool bHasGrid = (nShow == 1);
						if (bFlat && !bNoGrid && (g_bWaterTop == 2 || bHasGrid))
						{
							++g_nWaterTopFlat; ++g_nWaterTopSkipped;
							std::pair<long, int>& wt = g_WaterTopByModel[szModelName];
							++wt.first; wt.second = nShow;
							continue;
						}
					}
				}
			}

			// FLAGS HISTOGRAM. The street draws as a cyan tile with a yellow
			// "AI" on it - an editor marker texture on an AI volume, a brush
			// the retail renderer knows not to draw and we draw straight over
			// the street. So the question is not "which texture is wrong", it
			// is "which bit says do not render this", and the engine's world
			// surface flags are not in the client SDK. Counting the distinct
			// values, with a texture size to recognise them by, is how to find
			// it without guessing.
			if (g_bFlagHist && Readable(sf, kSurfaceFlags2 + 4))
			{
				const uint32_t fl = Word(sf, kSurfaceFlags2) & 0x1FFFFFFFu;
				const uint32_t txf = Readable(sf, kSurfaceTexture + 4)
								   ? Word(sf, kSurfaceTexture) : 0;
				float tw = 0.0f, th = 0.0f;
				const int ti = TextureFor(txf);
				if (ti >= 0) { tw = g_Tex[ti].fW; th = g_Tex[ti].fH; }
				size_t k = 0;
				for (; k < g_FlagHist.size(); ++k)
					if (g_FlagHist[k].nFlags == fl) break;
				if (k == g_FlagHist.size())
				{
					FlagRow row{};
					row.nFlags = fl; row.fW = tw; row.fH = th;
					g_FlagHist.push_back(row);
				}
				++g_FlagHist[k].nPolys;
				if (strcmp(szModelName, "PhysicsBSP") == 0
					|| strncmp(szModelName, "AIVolume", 8) == 0
					|| strncmp(szModelName, "blocker", 7) == 0)
					++g_FlagHist[k].nMarker;
				else if (strncmp(szModelName, "TranslucentWorldModel", 21) == 0)
					++g_FlagHist[k].nTrans;
				else
					++g_FlagHist[k].nOther;

				// The distinct texture NAMES this value is worn by, up to four.
				// nNames keeps counting past the four it can store, so the report
				// can say "and more" rather than quietly implying the set is small.
				if (ti >= 0 && g_Tex[ti].szName[0])
				{
					FlagRow& r = g_FlagHist[k];
					bool bSeen = false;
					const int nHave = r.nNames < 4 ? r.nNames : 4;
					for (int q = 0; q < nHave; ++q)
						if (_stricmp(r.szTex[q], g_Tex[ti].szName) == 0)
							{ bSeen = true; break; }
					if (!bSeen)
					{
						if (r.nNames < 4)
							strcpy_s(r.szTex[r.nNames], g_Tex[ti].szName);
						++r.nNames;
					}
				}
				// Remember the FIRST texture size seen, and note when a value
				// carries more than one - a flag shared by everything says
				// nothing.
				if (g_FlagHist[k].fW != tw || g_FlagHist[k].fH != th)
				{ g_FlagHist[k].fW = -1.0f; g_FlagHist[k].fH = -1.0f; }
			}
			const uint32_t tx  = Readable(sf, kSurfaceTexture + 4)
							   ? Word(sf, kSurfaceTexture) : 0;

			// An occluder brush that is not named blocker*. Skipped by the
			// TEXTURE, which is the only thing that identifies it, and counted
			// so the number is visible rather than assumed.
			if (g_bSkipOcclTex && tx)
			{
				const int nOc = TextureFor(tx);
				if (nOc >= 0 && g_Tex[nOc].bOccl)
				{
					++g_nOcclPolys;
					continue;
				}
			}
			bool bSkip = false;
			float fTW = 1.0f, fTH = 1.0f;
			size_t nBucket = 0;
			// The cache first - those dimensions were taken from the data
			// object when the texture was created and are authoritative -
			// then the engine's object, so a texture that has not been built
			// yet still buckets with its own key and its own UV scale.
			const int nTexIdx = TextureFor(tx);
			bool bDims = false;

			// A CACHED entry is only usable if its dimensions are real.
			//
			// TexDims already refuses a 0x0 texture, but the cache path did
			// not check - and the engine binds a texture before its data is
			// resident, so the first bind can leave an entry with pitch 0 and
			// no dimensions. Every UV on such a polygon was then q / 0, which
			// is an infinity the rasteriser interpolates into nonsense, and
			// the polygon drew as a flat slab of one colour.
			//
			// That is the white washing out the upper half of the street. The
			// probe grid named it: polygon 1060 of VisBSP, surface flags
			// 20000111, texture 11619864 reported as 0x0 and not built - a
			// ceiling-height polygon with a downward normal, spanning the top
			// of the view. Five switch-flipping runs failed to find it
			// because it is not sky, not translucent, not unlit and not a
			// missing texture: the geometry and the texture are both fine and
			// the arithmetic between them divided by zero.
			if (nTexIdx >= 0 && g_Tex[nTexIdx].fW > 0.0f && g_Tex[nTexIdx].fH > 0.0f)
			{
				fTW = g_Tex[nTexIdx].fW; fTH = g_Tex[nTexIdx].fH; bDims = true;
				// A file-loaded texture's header size, less its mipmap offset:
				// see DtxInfo::nUVShift.
				if (g_bMipOffsetUV && (!bSkyModel || g_bMipOffsetSky) && g_Tex[nTexIdx].nUVShift)
				{ fTW /= (float)(1u << g_Tex[nTexIdx].nUVShift); fTH /= (float)(1u << g_Tex[nTexIdx].nUVShift); ++g_nMipOffsetUV; }
			}
			else if (tx) bDims = TexDims(tx, &fTW, &fTH);

			// Belt and braces: nothing downstream may divide by these.
			if (bDims && (fTW <= 0.0f || fTH <= 0.0f)) bDims = false;

			if (tx && bDims)
			{
				// Bucket by the engine's pointer. Whether a D3D11 texture
				// exists for it yet is a question for draw time.
				// The sky gets its OWN key space. Bucketing it with the world by
				// texture would put any world polygon sharing a panorama texture
				// into the sky pass, where it would be drawn without depth and at
				// the wrong place - and Invisible.dtx really is shared.
				// The sky gets its OWN key space. Bucketing it with the world by
				// texture would put any world polygon sharing a panorama texture
				// into the sky pass, where it draws without depth and in the wrong
				// place - and Invisible.dtx really is shared.
				std::vector<uint32_t>&    key = bSkyModel ? skyKey
											  : (bTransModel ? transKey : bucketKey);
				std::vector<std::string>& fil = bSkyModel ? skyFile
											  : (bTransModel ? transFile : bucketFile);
				std::vector< std::vector<Vtx> >& bk = bSkyModel ? skyBuckets
											  : (bTransModel ? transBuckets : buckets);
				size_t k = 0;
				const bool bOwn = (&key == &bucketKey);
				const bool bSkyOwn = (&key == &skyKey);
				for (; k < key.size(); ++k)
					if (key[k] == tx && (!bOwn || bucketSub[k] == nSubKey)
						&& (!bSkyOwn || skySub[k] == sub)) break;
				if (k == key.size())
				{
					key.push_back(tx); fil.push_back(std::string());
					if (bOwn) bucketSub.push_back(nSubKey);
					if (bSkyOwn) { skySub.push_back(sub); skyOrder.push_back(fSkyOrderM); skyBlend.push_back(bSkyObj ? 1 : 0); }
				}
				nBucket = k + 1;
				if (bk.size() <= nBucket) bk.resize(nBucket + 1);
			}

			// The FLOOR, found by a property it must have rather than by
			// guessing which batch it is: a large polygon whose normal points
			// up. Headset testing reports the ground drawing blue with yellow shapes
			// where the level has a stone street, and the two candidate causes
			// - the wrong texture, or the right texture with wrong UVs - are
			// told apart by looking at the texture this polygon resolves to.
			if (nv >= 3)
			{
				const float ax = p[1][0]-p[0][0], ay = p[1][1]-p[0][1], az = p[1][2]-p[0][2];
				const float bx = p[2][0]-p[0][0], by = p[2][1]-p[0][1], bz = p[2][2]-p[0][2];
				float nx = ay*bz - az*by, ny = az*bx - ax*bz, nz = ax*by - ay*bx;
				const float len = sqrtf(nx*nx + ny*ny + nz*nz);
				if (len > 1e-3f)
				{
					const float area = len * 0.5f;
					ny /= len;
					// Facing up, within about 25 degrees, and the biggest so far.
					if (ny > 0.9f && area > g_fFloorArea)
					{
						g_fFloorArea = area;
						g_pFloorTex  = tx;
						g_pFloorSurf = sf;
						g_fFloorTW = fTW;   g_fFloorTH = fTH;
						g_nFloorVerts = (int)nv;
					}
				}
			}

			// ---- THE POLYGON THE ENGINE NEVER TEXTURED ----
			//
			// This is what was actually costing the tester world items. The engine
			// only loads a texture for a surface it intends to draw, and we
			// draw the whole level, so we meet surfaces whose pixels were
			// never loaded. Until now those were counted and then DROPPED -
			// 863 polygons on m01s02, 144 of them translucent scenery cut
			// out entirely - because there was nothing to draw them with.
			//
			// Now there is: the level file names the texture of every
			// surface, and stage 1 can load any .dtx by name. So instead of
			// dropping the polygon we texture it from the file.
			//
			// THIS ADDS, IT NEVER DELETES. The worst a wrong mapping can do
			// here is paint a wrong picture on a polygon that was invisible
			// a moment ago - which is why it is a different class of change
			// from the marker rule. It is still gated on the SAME per-model
			// verification, so a model whose bridge did not check out is
			// left exactly as it was.
			bool bFromFile = false;
			std::string sFileTex;		// the file texture's name, for the sky face log
			if (g_bFileTex && !(tx && bDims) && bBridgeOK
				&& !fileSurf.empty() && fileTex)
			{
				const uint32_t sfp = Word(po, kPolySurface);
				if (sfp >= nSurfBase)
				{
					const uint32_t si = (sfp - nSurfBase) / nSurfStride;
					if (si < fileSurf.size()
						&& fileSurf[si].nTexture < fileTex->size())
					{
						const std::string& sF = (*fileTex)[fileSurf[si].nTexture];
						DtxInfo di;
						if (!sF.empty() && LoadInfoAny(sF.c_str(), &di)
							&& di.nWidth && di.nHeight)
						{
							fTW = (float)di.nWidth;
							fTH = (float)di.nHeight;
							if (g_bMipOffsetUV && (!bSkyModel || g_bMipOffsetSky) && di.nUVShift)
							{ fTW /= (float)(1u << di.nUVShift); fTH /= (float)(1u << di.nUVShift); ++g_nMipOffsetUV; }
							bDims = true;
							std::vector<uint32_t>&    key = bSkyModel ? skyKey
													: (bTransModel ? transKey : bucketKey);
							std::vector<std::string>& fil = bSkyModel ? skyFile
													: (bTransModel ? transFile : bucketFile);
							std::vector< std::vector<Vtx> >& bk = bSkyModel ? skyBuckets
													: (bTransModel ? transBuckets : buckets);
							size_t k = 0;
							const bool bOwn3 = (&key == &bucketKey);
							const bool bSkyOwn3 = (&key == &skyKey);
							for (; k < key.size(); ++k)
								if (!key[k] && fil[k] == sF
									&& (!bOwn3 || bucketSub[k] == nSubKey)
									&& (!bSkyOwn3 || skySub[k] == sub)) break;
							if (k == key.size())
							{
								key.push_back(0); fil.push_back(sF);
								if (bOwn3) bucketSub.push_back(nSubKey);
								if (bSkyOwn3) { skySub.push_back(sub); skyOrder.push_back(fSkyOrderM); skyBlend.push_back(bSkyObj ? 1 : 0); }
							}
							nBucket = k + 1;
							if (bk.size() <= nBucket) bk.resize(nBucket + 1);
							bFromFile = true; sFileTex = sF;
							++nFromWorldFile;
						}
					}
				}
			}

			// ---- THE TEXTURE OBJECT KNOWS ITS OWN NAME ----------------------
			//
			// The engine only LOADS a texture for a surface it means to draw, but it
			// still creates the object and the object still carries the filename -
			// [[texobj + 0x24] + 0x1C], the same field stage 1 reads. So a polygon
			// whose texture has no pixels is not anonymous: it can be named, and a
			// name is all this renderer needs to load the picture off disk.
			//
			// That matters twice over, because it separates two things that were
			// being treated as one. Dropping every untextured polygon takes the sky
			// brushes out (right - they are holes onto the backdrop) AND 535
			// polygons of ST0643 stone on m01s02 (wrong - that is scenery). Drawing
			// them all in stand-in blue paints over the sky (wrong) and hides the
			// stone under flat colour (also wrong).
			//
			// The NAME tells them apart with no bridge, no file surface array and no
			// per-model verification: a marker name is skipped, anything else is
			// loaded from its own .dtx.
			if (!bFromFile && !(tx && bDims) && tx && g_bFileTex)
			{
				char szEng[80] = { 0 };
				if (TexNameOf(tx, szEng, sizeof szEng) && szEng[0])
				{
					if (IsMarkerTexture(BaseName(szEng)))
					{
						if (g_bSkyOccluder && !bSkyModel
							&& _stricmp(BaseName(szEng), "SKY.DTX") == 0)
							PushOccluderFan(occBucket, p, nv);
						++g_nMarkerTexSkipped; ++nMarkerSkipBuild;
						if (bSkyModel) ++nSkyWhere[2];
						continue;
					}
					DtxInfo de;
					if (LoadInfoAny(szEng, &de) && de.nWidth && de.nHeight)
					{
						fTW = (float)de.nWidth; fTH = (float)de.nHeight;
						if (g_bMipOffsetUV && (!bSkyModel || g_bMipOffsetSky) && de.nUVShift)
						{ fTW /= (float)(1u << de.nUVShift); fTH /= (float)(1u << de.nUVShift); ++g_nMipOffsetUV; }
						bDims = true;
						std::vector<uint32_t>&    key = bSkyModel ? skyKey
												  : (bTransModel ? transKey : bucketKey);
						std::vector<std::string>& fil = bSkyModel ? skyFile
												  : (bTransModel ? transFile : bucketFile);
						std::vector< std::vector<Vtx> >& bk = bSkyModel ? skyBuckets
												  : (bTransModel ? transBuckets : buckets);
						const std::string sE(szEng);
						size_t k = 0;
						const bool bOwn2 = (&key == &bucketKey);
						const bool bSkyOwn2 = (&key == &skyKey);
						for (; k < key.size(); ++k)
							if (!key[k] && fil[k] == sE
								&& (!bOwn2 || bucketSub[k] == nSubKey)
								&& (!bSkyOwn2 || skySub[k] == sub)) break;
						if (k == key.size())
						{
							key.push_back(0); fil.push_back(sE);
							if (bOwn2) bucketSub.push_back(nSubKey);
							if (bSkyOwn2) { skySub.push_back(sub); skyOrder.push_back(fSkyOrderM); skyBlend.push_back(bSkyObj ? 1 : 0); }
						}
						nBucket = k + 1;
						if (bk.size() <= nBucket) bk.resize(nBucket + 1);
						bFromFile = true; sFileTex = sE;
						++nFromEngineName;
					}
				}
			}

			if (!bFromFile && !(tx && bDims))
			{
				++nNoTex;
				// NAME THE FIRST FEW, so an untextured polygon is not a number.
				{
					static int s_nNoTexSaid = 0;
					if (s_nNoTexSaid++ < 8)
					{
						const char* pszF = "(no file name)";
						if (bBridgeOK && !fileSurf.empty() && fileTex)
						{
							const uint32_t sfp2 = Word(po, kPolySurface);
							if (sfp2 >= nSurfBase)
							{
								const uint32_t si2 = (sfp2 - nSurfBase) / nSurfStride;
								if (si2 < fileSurf.size() && fileSurf[si2].nTexture < fileTex->size())
									pszF = (*fileTex)[fileSurf[si2].nTexture].c_str();
							}
						}
						Log("  R3D UNTEXTURED polygon: model '%s' (%u polygons), engine tex %08X"
							" dims %s, bridge %s, file surfaces %u, file name '%s'",
							szModelName, nPolys, tx, bDims ? "yes" : "no",
							bBridgeOK ? "ok" : "NOT ok",
							(unsigned)fileSurf.size(), pszF);
					}
				}
				// The engine never binds these, and the dump says why: their
				// data pointer at +0x14 points back into the object itself -
				// an empty intrusive list - so the pixels were never loaded.
				// The engine does not load them because it never draws these
				// surfaces. We draw the whole level, so we meet them.
				//
				// Drawing them white shows geometry the real renderer never
				// shows. Skipping is the default; +StubDrawUntextured 1 puts
				// them back, because "what ARE those 1899 polygons" is a
				// question the picture should still be able to answer.
				// DELETING IS ONLY JUSTIFIED WHEN THE RESCUE WAS AVAILABLE.
				//
				// With the level's .DAT in hand this branch means what it says:
				// a couple of thousand surfaces the engine itself never draws.
				// Without it, it means WE FAILED TO READ THE LEVEL - and on
				// M01S02 it deleted 17521 of 26904 polygons and left the tester a
				// black room with a receptionist in it. A white wall is a
				// cosmetic fault; a missing floor is not a game.
				//
				// So the skip is conditional on g_pWorldFile now. A rule that
				// deletes has to be able to say why it is entitled to.
				if (!g_bDrawUntextured && g_pWorldFile)
					{ bSkip = true; ++nNoTexSkipBuild; }
				else if (!g_bDrawUntextured)
					++nNoTexKeptNoFile;

				// A TRANSLUCENT world model we cannot texture draws as a solid
				// sky-blue quad on a wall, and where its edge is coplanar with that
				// wall the two z-fight. That is the flickering blue sliver in the
				// alleyway: TranslucentWorldModel79 poly 4 against VisBSP poly 669,
				// measured at gap 0.000 over a whole sweep of rays. Retail does not
				// show it at all, so leaving it out is closer to right than blue.
				if (bTransModel)
				{
					++nNoTexTrans;
					if (!g_bDrawUntexturedTrans)
					{
						// Already counted once above if it was also untextured.
						if (!bSkip) ++nNoTexTransBuild;
						bSkip = true;
					}
				}
				// WHICH texture is missing, and how much of the level it
				// costs. "1899 polygons have no texture" is a number nobody
				// can act on; "one missing texture accounts for 1487 of them"
				// names the thing to go and fix. Counted per engine texture
				// object, which is the identity the surfaces reference.
				bool bSeen = false;
				for (auto& e : missing)
					if (e.first == tx) { ++e.second; bSeen = true; break; }
				if (!bSeen && missing.size() < 64) missing.push_back({ tx, 1 });
			}

			if (bSkip) { if (bSkyModel) ++nSkyWhere[7]; continue; }

			// The stored (u, v) are in texture units - proved on 23556 of
			// 23556 vertices to equal dot(v-O,P) and dot(v-O,Q) against the
			// surface's own basis - so dividing by the texture size is the
			// whole of the mapping.
			float uv[256][2];
			for (uint32_t t = 0; t < nv; ++t)
			{
				const float* q = (const float*)(uintptr_t)
					(po + kPolyVertList + t * kPolyVertStride + 4);
				uv[t][0] = q[0] / fTW;
				uv[t][1] = q[1] / fTH;
			}

			// THE BIGGEST POLYGONS, with the texture and the UV span just
			// computed for them. A wall-sized polygon whose UVs cover a fraction
			// of a texel draws as one magnified texel - a flat slab of colour
			// that survives every switch removing geometry, because the geometry
			// is right and the mapping is not. Per POLYGON: a batch is per
			// texture and spans the whole level, so a batch span says nothing.
			if (nv >= 3)
			{
				float wx0=1e30f,wx1=-1e30f,wy0=1e30f,wy1=-1e30f,wz0=1e30f,wz1=-1e30f;
				float pu0=1e30f,pu1=-1e30f,pv0=1e30f,pv1=-1e30f;
				for (uint32_t t2 = 0; t2 < nv; ++t2)
				{
					if(p[t2][0]<wx0)wx0=p[t2][0]; if(p[t2][0]>wx1)wx1=p[t2][0];
					if(p[t2][1]<wy0)wy0=p[t2][1]; if(p[t2][1]>wy1)wy1=p[t2][1];
					if(p[t2][2]<wz0)wz0=p[t2][2]; if(p[t2][2]>wz1)wz1=p[t2][2];
					if(uv[t2][0]<pu0)pu0=uv[t2][0]; if(uv[t2][0]>pu1)pu1=uv[t2][0];
					if(uv[t2][1]<pv0)pv0=uv[t2][1]; if(uv[t2][1]>pv1)pv1=uv[t2][1];
				}
				const float ex=wx1-wx0, ey=wy1-wy0, ez=wz1-wz0;
				const float diag = sqrtf(ex*ex+ey*ey+ez*ez);
				const float du = pu1-pu0, dv = pv1-pv0;
				const float texels = ((du>dv)?du:dv) * ((fTW>1.0f)?fTW:1.0f);
				if (diag > 200.0f)
				{
					BigPoly bp{}; bp.tex=tx; bp.tw=fTW; bp.th=fTH;
					bp.diag=diag; bp.du=du; bp.dv=dv;
					bp.tpu=(diag>1.0f)?texels/diag:1e9f;
					bp.cx=(wx0+wx1)*0.5f; bp.cy=(wy0+wy1)*0.5f; bp.cz=(wz0+wz1)*0.5f;
					g_BigPolys.push_back(bp);
				}
			}

			// The lightmap coordinate, in atlas units. The engine's own
			// expression, checked on 19626 of 19626 polygons: the dot product
			// against the plane-derived basis, over the world's texel size,
			// plus the half texel that puts a vertex on a texel centre.
			float lm[256][2];
			LMPoly L{};
			const bool bLit = g_bLMEnable && LM_ForPoly(po, &L) != 0;
			if (bLit)
			{
				++nLit;
				if (bSkyModel) ++nSkyLit;
				// THIS POLYGON'S AVERAGE LIGHT, INTO THE GRID. Up to 16 texels
				// spread over the rectangle - the mean of a lightmap is what a
				// room's light IS, and reading every texel of every polygon
				// would cost a second on a big level for a number that would
				// not move.
				if (!g_LGrid.cnt.empty() && L.nW > 0 && L.nH > 0)
				{
					float sr = 0.0f, sg = 0.0f, sb = 0.0f; int sn = 0;
					const int stepX = (L.nW + 3) / 4, stepY = (L.nH + 3) / 4;
					for (int ty = 0; ty < L.nH; ty += stepY)
					for (int tx = 0; tx < L.nW; tx += stepX)
					{
						const uint32_t t =
							LM_Texel((int)L.fX + tx, (int)L.fY + ty);
						sb += (float)( t        & 0xFF);
						sg += (float)((t >>  8) & 0xFF);
						sr += (float)((t >> 16) & 0xFF);
						++sn;
					}
					if (sn && bSkyModel)
					{
						fSkyLitSum += (double)(sr + sg + sb) / (3.0 * 255.0);
						nSkyLitTexels += sn;
					}
					if (sn)
					{
						float c[3] = { sr / (sn * 255.0f), sg / (sn * 255.0f),
									   sb / (sn * 255.0f) };
						float ctr[3] = { 0, 0, 0 };
						for (uint32_t t = 0; t < nv; ++t)
						{ ctr[0] += p[t][0]; ctr[1] += p[t][1]; ctr[2] += p[t][2]; }
						ctr[0] /= (float)nv; ctr[1] /= (float)nv; ctr[2] /= (float)nv;
						LGridAdd(ctr, c);
					}
				}
				const float fA = LM_AtlasSize();
				for (uint32_t t = 0; t < nv; ++t)
				{
					const float dx = p[t][0] - L.O[0], dy = p[t][1] - L.O[1],
								dz = p[t][2] - L.O[2];
					const float lu = (dx*L.P[0] + dy*L.P[1] + dz*L.P[2]) / L.fS + 0.5f;
					const float lv = (dx*L.Q[0] + dy*L.Q[1] + dz*L.Q[2]) / L.fS + 0.5f;
					lm[t][0] = (L.fX + lu) / fA;
					lm[t][1] = (L.fY + lv) / fA;
				}
			}
			// -1 means "no lightmap, shade it with the stand-in directional light".
			// -2 means "do not light this at all".
			//
			// THE SKY MUST NOT BE LIT. Its six faces have six different normals, so
			// the stand-in light gives each one a different brightness and the cube's
			// edges become hard lines across the sky - the horizontal seam the tester
			// found in Morocco, where half the sky is shaded and half is not. A
			// backdrop is a photograph of the sky; there is nothing in it to light.
			else if (bSkyModel) ++nSkyUnlit;
			else ++nNoLMByModel[szModelName];
			// THE VERTEX COLOUR, for a polygon with no lightmap: see
			// g_bVertexColour. Carried to the shader in the NORMAL slot (a
			// polygon lit by a stored colour has no use for its normal) with
			// -4 in the first lightmap coordinate. White stays white here;
			// the shader applies the level's light scale as it does to every
			// lit pixel.
			bool bVtxColour = false;
			// AN OBJECT BRUSH WITH NO LIGHTMAP AND A WHITE VERTEX COLOUR IS LIT AT
			// RUN TIME, from the level's light where it stands - the way retail
			// lights a moving world model. The HQ's rotating Unity statue (Big U0,
			// 12 lightmaps of no size) drew as its own white vertex colour: bright
			// marble in a dark lobby, 96-125 against retail's 43 (19 Sep). White
			// here means the compiler left it for the engine, not that it is lit.
			bool bObjectLit = false;
			float vc[256][3];
			if (!bLit && g_bVertexColour)
			{
				bVtxColour = true;
				bool bWhite = true;
				for (uint32_t t = 0; t < nv; ++t)
				{
					const uint32_t c = Word(po + kPolyVertList + t * kPolyVertStride, 20);
					vc[t][0] = (float)((c >> 16) & 0xFF) / 255.0f;
					vc[t][1] = (float)((c >>  8) & 0xFF) / 255.0f;
					vc[t][2] = (float)( c        & 0xFF) / 255.0f;
					if ((c & 0xFFFFFF) != 0xFFFFFF) bWhite = false;
					// +StubVertexColour 2 paints every such polygon magenta:
					// the positive control for the route, since a level's
					// grid-lit polygons are not always in the desk view.
					if (g_bVertexColour == 2) { vc[t][0] = 1.0f; vc[t][1] = 0.0f; vc[t][2] = 1.0f; }
					if (bSkyModel)
					{ fSkyVC[0] += vc[t][0]; fSkyVC[1] += vc[t][1]; fSkyVC[2] += vc[t][2]; ++nSkyVC; }
				}
				// THE SKY TOO. Its brushes carry no lightmap (measured: 0 of
				// 9 on the harbour) so retail lit them by vertex colour like
				// any other; ours drew them at full brightness, and the
				// harbour's sky came out pink and 1.7x too bright. -5 is the
				// sky's own value: lit by the colour, never fogged.
				if (bSkyModel) { if (bWhite) ++nSkyVCWhite; }
				else if (bWhite) ++nVtxWhite; else ++nVtxTinted;
				// OFF, 19 September. This is what darkened the Unity statue.
				//
				// The rule reads well: an object brush with no lightmap and a
				// WHITE vertex colour was left to the engine by the compiler, so
				// light it from the level's grid the way retail lights a moving
				// world model. What it does in practice is hand the statue to a
				// grid whose floor in T01S02 is 0.102 - about a tenth - and
				// against the lobby's bright wall a tenth reads as black. The
				// tester reported the Unity statue broken, with a picture of it as a
				// black silhouette.
				//
				// It shipped in d11c354 beside the dome fix, which is the one
				// that was actually wanted and which stays. I measured the
				// domes and the pedestal that day and never measured the statue
				// body itself, which is the whole reason this got out.
				//
				// The statue being too BRIGHT is still open and is the older
				// complaint - it is not solved by making it too dark.
				bObjectLit = false;
				(void)bWhite; (void)nSubKey; (void)bSkyModel;
			}
			if (!bLit) for (uint32_t t = 0; t < nv; ++t)
			{
				lm[t][0] = bSkyModel ? (bVtxColour ? -5.0f : -2.0f) : ((bVtxColour && !bObjectLit) ? -4.0f : -1.0f);
				// -3 in the second coordinate marks a WORLD polygon with no
				// lightmap - terrain, water - so the shader can light it from
				// the level's light grid (the pre-13 September route) and so
				// dynamic lights reach it. Models keep -1 and their own light.
				lm[t][1] = bSkyModel ? -1.0f : -3.0f;
			}

			// The first few, with the atlas texel that is actually at the
			// coordinate we computed. A coordinate that lands in a gap reads
			// back as black here, and that is the whole question.
			if (bLit && nLit <= 5)
			{
				Log("  LM sample: poly %08X rect %.0f,%.0f %dx%d  S %.1f",
					po, L.fX, L.fY, L.nW, L.nH, L.fS);
				for (uint32_t t = 0; t < nv && t < 4; ++t)
				{
					const float fA = LM_AtlasSize();
					const int tx = (int)(lm[t][0] * fA), ty = (int)(lm[t][1] * fA);
					const uint32_t v = LM_Texel(tx, ty);
					Log("      v%u uv %.5f,%.5f -> atlas texel %d,%d = %02X %02X %02X",
						t, lm[t][0], lm[t][1], tx, ty,
						(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
				}
			}

			// ONE LINE PER SKY FACE. The backdrop draws as flat colour bands
			// rather than the panorama retail shows, and the two candidate causes -
			// the wrong texture, or the right texture with UVs covering a fraction
			// of a texel - are told apart by the numbers, not by looking.
			// Twelve faces per build. M13S01's sky carries a terrain panorama
			// and printed 4326 of these per build; the 12 September session's
			// log was 17 MB, mostly this.
			static long s_nSkyFaceBuild = -1; static int s_nSkyFaceSaid = 0;
			if (s_nSkyFaceBuild != g_nBuilds) { s_nSkyFaceBuild = g_nBuilds; s_nSkyFaceSaid = 0; }
			if (bSkyModel && nv >= 3 && s_nSkyFaceSaid++ < 12)
			{
				float u0=1e30f,u1=-1e30f,v0=1e30f,v1=-1e30f;
				for (uint32_t t2 = 0; t2 < nv; ++t2)
				{
					if(uv[t2][0]<u0)u0=uv[t2][0]; if(uv[t2][0]>u1)u1=uv[t2][0];
					if(uv[t2][1]<v0)v0=uv[t2][1]; if(uv[t2][1]>v1)v1=uv[t2][1];
				}
				char szT[80] = { 0 };
				if (!tx || !TexNameOf(tx, szT, sizeof szT))
				{
					// NAME THE FILE TEXTURE TOO: a sky layer drawn as a hard-edged
					// lighter square (the factory, 22 September) could only be told
					// from a right texture with wrong alpha by its name.
					if (bFromFile && !sFileTex.empty()) snprintf(szT, sizeof szT, "(file) %s", sFileTex.c_str());
					else strncpy(szT, bFromFile ? "(from the file)" : "(none)", 79);
				}
				Log("  R3D SKY FACE: %u verts, tex %08X %s %.0fx%.0f,"
					" u %.4f..%.4f  v %.4f..%.4f  (span %.4f x %.4f)  v0 colour %08X",
					nv, tx, szT, fTW, fTH, u0, u1, v0, v1, u1-u0, v1-v0,
					Word(po + kPolyVertList, 20));
			}

			float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
			float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
			float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
			const float len = sqrtf(nx * nx + ny * ny + nz * nz);
			if (len < 1e-4f) { ++nPolyDegen; if (bSkyModel) ++nSkyWhere[8]; continue; }
			nx /= len; ny /= len; nz /= len;

			// A convex polygon fans from vertex 0 without needing a tessellator.
			for (uint32_t t = 1; t + 1 < nv; ++t)
			{
				const uint32_t idx[3] = { 0, t, t + 1 };
				for (int k = 0; k < 3; ++k)
				{
					Vtx v;
					v.x = p[idx[k]][0]; v.y = p[idx[k]][1]; v.z = p[idx[k]][2];
					v.nx = nx; v.ny = ny; v.nz = nz;
					if (bVtxColour)
					{ v.nx = vc[idx[k]][0]; v.ny = vc[idx[k]][1]; v.nz = vc[idx[k]][2]; }
					v.u = uv[idx[k]][0]; v.v = uv[idx[k]][1];
					v.lu = lm[idx[k]][0]; v.lv = lm[idx[k]][1];
					if (bSkyModel)        skyBuckets[nBucket].push_back(v);
					else if (bTransModel) transBuckets[nBucket].push_back(v);
					else                  buckets[nBucket].push_back(v);
				}
				++g_nTris;
			}
			++g_nPolys;
			if (bSkyModel) ++nSkyWhere[9];
		}
	}

	// Concatenate, recording where each texture's triangles start.
	std::vector<Vtx> verts;
	g_Batches.clear();
	for (size_t b = 0; b < buckets.size(); ++b)
	{
		if (buckets[b].empty()) continue;
		Batch bt{};
		bt.pTexKey = (b == 0) ? 0 : bucketKey[b - 1];
		bt.szFile[0] = 0;
		if (b && bucketKey[b - 1] == 0 && !bucketFile[b - 1].empty())
			strncpy(bt.szFile, bucketFile[b - 1].c_str(), sizeof bt.szFile - 1);
		bt.pSRV    = nullptr;			// resolved at draw time
		bt.nStart = (UINT)verts.size();
		bt.nCount = (UINT)buckets[b].size();
		bt.bSky   = false;
		bt.bTrans = false;
		bt.pSub   = (b == 0) ? 0 : bucketSub[b - 1];
		verts.insert(verts.end(), buckets[b].begin(), buckets[b].end());
		g_Batches.push_back(bt);
	}

	// THE BACKDROP, after the world in the buffer and before it on screen.
	// One vertex buffer, two passes: the sky batches carry a flag and the draw
	// loop runs over them first with the sky matrix and no depth.
	g_nSkyPolys = 0;
	for (size_t b = 0; b < skyBuckets.size(); ++b)
	{
		if (skyBuckets[b].empty()) continue;
		Batch bt{};
		bt.pTexKey = (b == 0) ? 0 : skyKey[b - 1];
		bt.szFile[0] = 0;
		if (b && skyKey[b - 1] == 0 && !skyFile[b - 1].empty())
			strncpy(bt.szFile, skyFile[b - 1].c_str(), sizeof bt.szFile - 1);
		bt.pSRV   = nullptr;
		bt.nStart = (UINT)verts.size();
		bt.nCount = (UINT)skyBuckets[b].size();
		bt.bSky   = true;
		bt.bTrans = false;
		bt.fSkyOrder = (b && b - 1 < skyOrder.size()) ? skyOrder[b - 1] : 0.0f;
		bt.bSkyBlend = (b && b - 1 < skyBlend.size()) ? (skyBlend[b - 1] != 0) : false;
		// THE BRUSH BEHIND THE LAYER, so the draw can ask the engine whether
		// it is hidden and how faded it is. See the sky pass.
		bt.pSub = (b && b - 1 < skySub.size()) ? skySub[b - 1] : 0;
		g_nSkyPolys += (int)(skyBuckets[b].size() / 3);
		verts.insert(verts.end(), skyBuckets[b].begin(), skyBuckets[b].end());
		g_Batches.push_back(bt);
	}
	// THE SKY BRUSHES, depth-only, drawn right after the backdrop.
	if (!occBucket.empty())
	{
		Batch bt{};
		bt.pTexKey = 0; bt.szFile[0] = 0; bt.pSRV = nullptr;
		bt.nStart = (UINT)verts.size();
		bt.nCount = (UINT)occBucket.size();
		bt.bOcc = true;
		verts.insert(verts.end(), occBucket.begin(), occBucket.end());
		g_Batches.push_back(bt);
		Log("  R3D SKY OCCLUDERS: %ld polygons wear Sky.dtx and now write depth only,"
			" so nothing behind a sky wall can show through it (+StubSkyOccluder 0 reverts)",
			g_nSkyOccPolys);
	}
	// GLASS, after everything else in the buffer and last on screen.
	for (size_t b = 0; b < transBuckets.size(); ++b)
	{
		if (transBuckets[b].empty()) continue;
		Batch bt{};
		bt.pTexKey = (b == 0) ? 0 : transKey[b - 1];
		bt.szFile[0] = 0;
		if (b && transKey[b - 1] == 0 && !transFile[b - 1].empty())
			strncpy(bt.szFile, transFile[b - 1].c_str(), sizeof bt.szFile - 1);
		bt.pSRV   = nullptr;
		bt.nStart = (UINT)verts.size();
		bt.nCount = (UINT)transBuckets[b].size();
		bt.bTrans = true;
		g_nTransPolys += (int)(transBuckets[b].size() / 3);
		verts.insert(verts.end(), transBuckets[b].begin(), transBuckets[b].end());
		g_Batches.push_back(bt);
	}
	if (g_nTransPolys)
		Log("  R3D GLASS: %d triangles of TranslucentWorldModel, drawn last with"
			" alpha %.2f and no depth write", g_nTransPolys, g_fTransAlpha);

	if (g_bHaveSky)
		Log("  R3D SKY: of its polygons - %d unreadable, %d bad count, %d a marker"
			" NAME, %d marker FLAGS, %d vertex list unreadable, %d bad vertex,"
			" %d untextured and dropped, %d degenerate, %d REACHED THE BUFFER",
			nSkyWhere[0], nSkyWhere[1], nSkyWhere[2], nSkyWhere[3], nSkyWhere[5],
			nSkyWhere[6], nSkyWhere[7], nSkyWhere[8], nSkyWhere[9]);
	if (g_bHaveSky)
		Log("  R3D SKY: %d triangles in %u batches, drawn first with the camera"
			" at (%.0f %.0f %.0f) and no depth", g_nSkyPolys,
			(unsigned)std::count_if(g_Batches.begin(), g_Batches.end(),
				[](const Batch& b2){ return b2.bSky; }),
			g_fSkyCentre[0], g_fSkyCentre[1], g_fSkyCentre[2]);

	if (verts.empty())
	{
		// AN EMPTY BUILD MUST STILL ARM THE REBUILD. The counts that let the
		// retex logic at the top of this function fire are recorded at the
		// END of a build, and this return skipped them - so a level whose
		// first build ran before the engine had bound a single texture built
		// nothing, recorded nothing, and was never built again: the PREVIOUS
		// level's mesh stayed in the buffer under the new level's lights.
		// Headset testing, Morocco after the training level, through the mission
		// summary: the whole world was black again, with missing walls and
		// buildings. The tutorial before it had 124 polygons textured at its
		// first build, which was enough to arm the retries that filled in
		// the other 9462; this one had none.
		//
		// The reason for the empty build is the ordinary one - the walk runs
		// at the start of the new world's first scene and the engine binds
		// its textures as that scene draws - so the answer is the ordinary
		// retry, not a special case: record the count the retry keys on,
		// floor it so the retry cannot be refused as "not worth it", and let
		// the next texture that arrives rebuild the world as it always does.
		g_nNoTexAtBuild = nNoTexSkipBuild + nNoTexKeptNoFile + nNoTexTransBuild;
		if (g_nNoTexAtBuild <= kRetexFloor) g_nNoTexAtBuild = kRetexFloor + 1;
		Log("  R3D: the world produced no triangles (%d polygons had no texture yet,"
			" %u textures known) - the build is ARMED to run again as textures arrive",
			nNoTexSkipBuild + nNoTexKeptNoFile + nNoTexTransBuild, (unsigned)g_Tex.size());
		return;
	}

	{
		std::sort(g_BigPolys.begin(), g_BigPolys.end(),
			[](const BigPoly& x, const BigPoly& y){ return x.tpu < y.tpu; });
		Log("  R3D: the 12 biggest polygons with the fewest texels per world unit");
		for (size_t i = 0; i < g_BigPolys.size() && i < 12; ++i)
		{
			const BigPoly& r = g_BigPolys[i];
			Log("       tex %08X %4dx%-4d diag %7.0f  u %.4f v %.4f  tex/unit %.5f"
				"  at (%.0f %.0f %.0f)", r.tex, (int)r.tw, (int)r.th, r.diag,
				r.du, r.dv, r.tpu, r.cx, r.cy, r.cz);
		}
		Log("       (%u polygons over 200 units measured)", (unsigned)g_BigPolys.size());
		g_BigPolys.clear();
	}

	g_Sample.clear();
	const size_t nStep = verts.size() / 2000 + 1;
	for (size_t i = 0; i < verts.size(); i += nStep)
	{ g_Sample.push_back(verts[i].x); g_Sample.push_back(verts[i].y);
	  g_Sample.push_back(verts[i].z); }
	g_pSampleVerts = g_Sample.data();
	g_nSampleVerts = (int)(g_Sample.size() / 3);

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = (UINT)(verts.size() * sizeof(Vtx));
	bd.Usage = D3D11_USAGE_IMMUTABLE;
	bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = verts.data();
	const HRESULT hr = g_pDev->CreateBuffer(&bd, &sd, &g_pVB);
	// Kept for R3D_RayCast, with a box per batch.
	g_WorldVertsCpu = verts;
	g_BatchBox.assign(g_Batches.size(), BatchBox());
	for (size_t b = 0; b < g_Batches.size(); ++b)
	{
		BatchBox& bx = g_BatchBox[b];
		bx.mn[0] = bx.mn[1] = bx.mn[2] =  1e30f;
		bx.mx[0] = bx.mx[1] = bx.mx[2] = -1e30f;
		const UINT e = g_Batches[b].nStart + g_Batches[b].nCount;
		for (UINT i = g_Batches[b].nStart; i < e && i < verts.size(); ++i)
		{
			const Vtx& v = verts[i];
			if (v.x < bx.mn[0]) bx.mn[0] = v.x; if (v.x > bx.mx[0]) bx.mx[0] = v.x;
			if (v.y < bx.mn[1]) bx.mn[1] = v.y; if (v.y > bx.mx[1]) bx.mx[1] = v.y;
			if (v.z < bx.mn[2]) bx.mn[2] = v.z; if (v.z > bx.mx[2]) bx.mx[2] = v.z;
		}
	}
	// THE WORLD'S EXTENTS for the sky parallax: the merged BSP (sub 0), not
	// the sky and not the separate world models - the engine's world_bsp.
	g_bWorldExt = 0;
	for (int k = 0; k < 3; ++k) { g_fWorldExtMin[k] = 1e30f; g_fWorldExtMax[k] = -1e30f; }
	for (size_t b = 0; b < g_Batches.size(); ++b)
	{
		if (g_Batches[b].bSky || g_Batches[b].pSub || !g_Batches[b].nCount) continue;
		for (int k = 0; k < 3; ++k)
		{
			if (g_BatchBox[b].mn[k] < g_fWorldExtMin[k]) g_fWorldExtMin[k] = g_BatchBox[b].mn[k];
			if (g_BatchBox[b].mx[k] > g_fWorldExtMax[k]) g_fWorldExtMax[k] = g_BatchBox[b].mx[k];
		}
		g_bWorldExt = 1;
	}
	if (g_bWorldExt && g_pWorldFile)
		Log("  R3D SKY PARALLAX: world %.0f %.0f %.0f .. %.0f %.0f %.0f, view box half-size %.1f %.1f %.1f%s",
			g_fWorldExtMin[0], g_fWorldExtMin[1], g_fWorldExtMin[2], g_fWorldExtMax[0], g_fWorldExtMax[1], g_fWorldExtMax[2],
			g_pWorldFile->fSkyView[0], g_pWorldFile->fSkyView[1], g_pWorldFile->fSkyView[2], g_bSkyParallax ? "" : " (OFF)");
	if (nSkippedModels)
		Log("  R3D: left out %d models carrying %d polygons - the collision"
			" hull and the AI volumes, which the engine never draws",
			nSkippedModels, nSkippedPolys);
		if (g_nOcclPolys)
			Log("      plus %ld polygons skipped for using the OCCLUDER"
				" marker texture - occluder brushes not named blocker*",
				g_nOcclPolys);
	Log("  R3D: built %d models, %d polygons, %d triangles, %u KB  hr=%08X",
		nModels, g_nPolys, g_nTris,
		(unsigned)(bd.ByteWidth / 1024), (unsigned)hr);

	// ---- THE ACCOUNT: every polygon in the level, in exactly one bucket --
	//
	// The tester asked the question that should have been asked first: if the
	// numbers are right, shouldn't they add up to everything? They should,
	// and nothing here was checking that they did. Every counter in this
	// file reports a PART - built, left out, untextured, marker - and no
	// two of them were ever added together and compared with the total the
	// LEVEL FILE declares.
	//
	// That is how four rounds of headset testing went into world geometry that
	// was never the problem. A part can look healthy while the whole is
	// short, and only a denominator says which.
	//
	// UNACCOUNTED must be 0. It is polygons the level has that this
	// renderer neither drew nor can name a reason for dropping, and it is
	// the only number on this line that needs no interpretation.
	{
		// THE FILE'S TOTAL WHERE THERE IS ONE, THE HEAP'S OTHERWISE.
		//
		// This used to be inside `if (g_pWorldFile)`, so a level whose .DAT was
		// not identified printed no account at all - and the level-sweep harness
		// could not tell that from a level that failed to load. M14S02 is exactly
		// that case: it rendered 23.5 million vertices perfectly and reported
		// nothing. An account with a weaker denominator is worth far more than no
		// account, and the line says which one it is using.
		const bool bFromFile = (g_pWorldFile != nullptr);
		int nFileTotal = 0;
		if (bFromFile)
			for (size_t i = 0; i < g_pWorldFile->Models.size(); ++i)
				nFileTotal += (int)g_pWorldFile->Models[i].nPolygons;
		else
			nFileTotal = nHeapTotalPolys;
		const int nMalformed = nModelUnreadPolys + nModelBadArrPolys
			+ nPolyUnread + nPolyBadNv + nPolyVertsUnread + nPolyVertsBad
			+ nPolyDegen;
		// nNoTexKeptNoFile is NOT added here: those polygons were drawn, so
		// they are already inside g_nPolys. Counting them twice would make
		// UNACCOUNTED go negative and hide whatever it is there to catch.
		const int nNamed = g_nPolys + nSkippedPolys + nMarkerSkipBuild
			+ nNoTexSkipBuild + nNoTexTransBuild + g_nOcclPolys
			+ nMarkerFlagPolys + nSkipFlagPolys + nMalformed;
		const int nLeft = nFileTotal - nNamed;
		Log("  R3D ACCOUNT: the level has %d polygons (from the %s)", nFileTotal,
			bFromFile ? "level file" : "HEAP - no .DAT was identified, so the"
				" marker rule and the file-texture rescue are BOTH OFF here");
		Log("     drawn                      %6d", g_nPolys);
		Log("     collision hull + AI volumes %5d  (%d models, never drawn by the engine either)", nSkippedPolys, nSkippedModels);
		Log("     editor marker textures     %6d", nMarkerSkipBuild);
		if (nMarkerFlagPolys)
			Log("     marker surface FLAGS       %6d", nMarkerFlagPolys);
		if (nSkipFlagPolys)
			Log("     +StubSkipFlags mask        %6d", nSkipFlagPolys);
		Log("     no texture, dropped        %6d", nNoTexSkipBuild);
		if (nNoTexKeptNoFile)
			Log("     no texture, KEPT ANYWAY    %6d   <- the level file was"
				" never identified, so these were drawn rather than deleted",
				nNoTexKeptNoFile);
		if (nFromEngineName)
			Log("     (%d rescued by the engine texture's OWN NAME, which it"
				" carries even when its pixels were never loaded)", nFromEngineName);
		Log("     translucent, no texture    %6d", nNoTexTransBuild);
		Log("     occluder texture           %6d", g_nOcclPolys);
		if (nMalformed)
		{
			// NOT a bucket to be satisfied with. Everything here is a polygon
			// the FILE says exists and the HEAP would not give up, so each line
			// is either a layout assumption that does not hold on this level or
			// geometry the engine really has thrown away. Named separately
			// because the fix differs per line.
			Log("     malformed in the heap      %6d   <- each of these is a"
				" polygon the FILE has and the HEAP would not yield", nMalformed);
			if (nModelUnread)  Log("         %d models unreadable (%d polygons)", nModelUnread, nModelUnreadPolys);
			if (nModelBadArr)  Log("         %d models with no polygon/vertex array (%d polygons)", nModelBadArr, nModelBadArrPolys);
			if (nPolyUnread)   Log("         %d polygon records unreadable", nPolyUnread);
			if (nPolyBadNv)    Log("         %d polygons with an impossible vertex count", nPolyBadNv);
			if (nPolyVertsUnread) Log("         %d polygons whose vertex list is unreadable", nPolyVertsUnread);
			if (nPolyVertsBad) Log("         %d polygons naming a vertex outside their own array", nPolyVertsBad);
			if (nPolyDegen)    Log("         %d polygons with no area (degenerate)", nPolyDegen);
		}
		Log("     UNACCOUNTED                %6d%s", nLeft,
			nLeft ? "   <- POLYGONS THIS RENDERER CANNOT EXPLAIN" : "");
		SpriteFootprint();
	}
	// Worst first, because the top one or two are the whole of the problem.
	std::sort(missing.begin(), missing.end(),
			  [](const std::pair<uint32_t,int>& a,
				 const std::pair<uint32_t,int>& b) { return a.second > b.second; });
	if (!missing.empty())
	{
		Log("  R3D: %u distinct textures are referenced but missing, worst first:",
			(unsigned)missing.size());
		// BY NAME. "engine texture 10988DCC: 535 polygons" is a number nobody can
		// act on; the name says at once whether those polygons are the sky brushes
		// (which SHOULD be dropped, so the backdrop shows through) or real scenery
		// that has gone missing. The name is in the texture object even when its
		// pixels were never loaded, which is exactly this case.
		for (size_t i = 0; i < missing.size() && i < 8; ++i)
		{
			char szN[80] = { 0 };
			if (!TexNameOf(missing[i].first, szN, sizeof szN))
				strncpy(szN, "(no name in the object)", 79);
			Log("       %-40s %d polygons (%.1f%% of the level)  [%08X]",
				szN, missing[i].second,
				100.0 * missing[i].second / (g_nPolys ? g_nPolys : 1),
				missing[i].first);
		}

		// WHAT are they? The engine never binds these, so either they are
		// texture objects it has no reason to make resident, or the word at
		// surface+0x30 means something else for these surfaces - in which case
		// those 1899 polygons are not untextured, they are not surfaces we
		// should be drawing at all. Opposite fixes, so the bytes decide it.
		//
		// Printed beside a texture we DID build, because a dump of an unknown
		// structure means nothing without one that is known good next to it.
		for (size_t i = 0; i < missing.size() && i < 2; ++i)
		{
			const uint32_t tx = missing[i].first;
			Log("    --- missing texture %08X ---", tx);
			if (!Readable(tx, 64)) { Log("        NOT READABLE - not an object at all"); continue; }
			for (int w = 0; w < 16; ++w)
				Log("        +%02X  %08X", w * 4, Word(tx, w * 4));
		}
		if (!g_Tex.empty())
		{
			const uint32_t good = g_Tex[0].pTex;
			Log("    --- for comparison, %08X, which we DID build (%gx%g) ---",
				good, g_Tex[0].fW, g_Tex[0].fH);
			if (Readable(good, 64))
				for (int w = 0; w < 16; ++w)
					Log("        +%02X  %08X", w * 4, Word(good, w * 4));
		}
	}

	// THE NEW BATCHES HAVE NO TEXTURES YET, AND NOTHING ELSE WILL NOTICE.
	//
	// Batches resolve their SRVs only when the texture SET has grown since the
	// last resolve - `g_Tex.size() != g_nResolvedWithTex` - which is exactly
	// right in the steady state and wrong the moment the batch list is rebuilt
	// without new textures arriving. Every batch then keeps a null SRV, every
	// batch that names a texture is skipped for having no pixels, and the world
	// draws black.
	//
	// It has been masked all along because a rebuild only ever happened on a new
	// world, where the texture set does grow. Forcing a rebuild on the same
	// world found it at once: 292 batches -> 146, and a 91% black frame.
	g_nResolvedWithTex = (size_t)-1;

	g_MissingAtBuild = missing;
	// Everything this build failed to texture, whatever the reason - dropped,
	// or kept because there was no level file to rescue it with. This is what
	// decides whether another build is worth doing.
	g_nNoTexAtBuild = nNoTexSkipBuild + nNoTexKeptNoFile + nNoTexTransBuild;
	g_nBuiltWithTexReport = g_nBuiltWithTex;

	// What the ground is textured with, and how big its UVs are. Which of the
	// two candidate causes is at fault is decided by the numbers here plus the
	// dumped image: a plausible stone texture with huge UVs is a mapping fault,
	// an obviously-wrong image is a linkage fault.
	if (g_pFloorTex)
	{
		float fw = 0, fh = 0;
		const bool bD = TexDims(g_pFloorTex, &fw, &fh);
		Log("  R3D: FLOOR - the largest upward-facing polygon has area %.0f,"
			" %d vertices, surface %08X, texture %08X (%gx%g, dims %s),"
			" built %s",
			g_fFloorArea, g_nFloorVerts, g_pFloorSurf, g_pFloorTex,
			bD ? fw : 0.0f, bD ? fh : 0.0f, bD ? "ok" : "UNREADABLE",
			TextureFor(g_pFloorTex) >= 0 ? "yes" : "NO");
	}
	else Log("  R3D: FLOOR - no upward-facing polygon found");

	// The object itself, beside a texture we know is good. Whether the floor's
	// texture object is DEAD or merely shaped differently decides the fix, and
	// four words of a dump settle it - the same way the sky question was
	// settled (docs/UNTEXTURED-IS-SKY.md).
	if (g_pFloorTex)
	{
		Log("    floor texture object %08X:", g_pFloorTex);
		if (Readable(g_pFloorTex, 64))
			for (int w = 0; w < 8; ++w)
				Log("        +%02X  %08X", w * 4, Word(g_pFloorTex, w * 4));
		else Log("        NOT READABLE");
		const uint32_t pd = Word(g_pFloorTex, 8);
		Log("      its data object at +8 is %08X, %s", pd,
			Readable(pd, 32) ? "readable" : "NOT READABLE");
		if (Readable(pd, 32))
			Log("        w %u h %u (from +%02X/+%02X)",
				(unsigned)*(const uint16_t*)(uintptr_t)(pd + kTexWidth),
				(unsigned)*(const uint16_t*)(uintptr_t)(pd + kTexHeight),
				kTexWidth, kTexHeight);

		// The pixels we are actually drawing for the floor. If they are the
		// stone street, the fault is in the mapping; if they are the blue and
		// yellow seen in the headset, we are serving the wrong image and the
		// texture-object address was recycled under us.
		const int nFi = TextureFor(g_pFloorTex);
		if (nFi >= 0)
		{
			Log("      cached pData %08X, object's current +0x14 is %08X%s",
				g_Tex[nFi].pData, Word(g_pFloorTex, 0x14),
				(g_Tex[nFi].pData == Word(g_pFloorTex, 0x14))
					? "  - SAME, the cache is current"
					: "  - DIFFERENT, the object has been reset under us");
			if (g_szFloorDump[0] && Readable(g_Tex[nFi].pData, 0x20))
			{
				const uint8_t* d = (const uint8_t*)(uintptr_t)g_Tex[nFi].pData;
				const uint8_t* m = d + kTexMip0;
				const uint32_t mw = *(const uint32_t*)(m + 0);
				const uint32_t mh = *(const uint32_t*)(m + 4);
				const uint32_t pp = *(const uint32_t*)(m + 8);
				const uint32_t mp = *(const uint32_t*)(m + 12);
				Log("      its mip 0 is %ux%u pitch %u at %08X", mw, mh, mp, pp);
			}
		}

		// And one we did build, for comparison.
		for (size_t i = 0; i < g_Tex.size(); ++i)
			if (g_Tex[i].pTex != g_pFloorTex)
			{
				Log("    a texture we DID build, %08X (%gx%g), for comparison:",
					g_Tex[i].pTex, g_Tex[i].fW, g_Tex[i].fH);
				if (Readable(g_Tex[i].pTex, 64))
					for (int w = 0; w < 8; ++w)
						Log("        +%02X  %08X", w * 4, Word(g_Tex[i].pTex, w * 4));
				break;
			}
	}

	if (nFromWorldFile)
		Log("  R3D: %d polygons TEXTURED FROM THE LEVEL FILE that the engine"
			" never bound a texture for - these used to be dropped", nFromWorldFile);
	Log("  R3D WATER TOP: %ld surfaces named, %ld of them WATER, %ld horizontal"
		" and skipped (the polygrid draws that surface)",
		g_nWaterTopGate, g_nWaterTopNamed, g_nWaterTopFlat);
	Log("  R3D SKY IN GLASS: %ld sky batch draws kept out of the see-through pass so far", g_nSkyGlassSkipped);
	Log("  R3D GLASS DEPTH: %s, cut %.2f, %ld depth-only draws so far", g_bGlassDepth ? "on" : "OFF", g_fGlassDepthCut, g_nGlassDepthDraws);
	for (std::map<std::string, std::pair<long, int>>::const_iterator wt = g_WaterTopByModel.begin();
		 wt != g_WaterTopByModel.end(); ++wt)
		Log("  R3D WATER TOP BY MODEL: '%s' %ld flat water faces skipped, ShowSurface %s",
			wt->first.c_str(), wt->second.first,
			wt->second.second < 0 ? "NONE (not a volume with a surface object)"
								  : (wt->second.second ? "1" : "0"));
	g_WaterTopByModel.clear();
	Log("  R3D: %u textures, %u draw batches, %d polygons with no texture"
		"  (build %ld)",
		(unsigned)g_Tex.size(), (unsigned)g_Batches.size(), nNoTex, g_nBuilds);
	Log("  R3D: of those %d untextured polygons, %d are on TranslucentWorldModels"
		" and were %s", nNoTex, nNoTexTrans,
		g_bDrawUntexturedTrans ? "DRAWN (blue quads)" : "skipped");
	Log("  R3D: %d of %d polygons drew with a lightmap, the rest with the"
		" stand-in shade", nLit, g_nPolys);
	if (!nNoLMByModel.empty())
	{
		// The eight models with the most lightmap-less polygons, most first.
		std::vector< std::pair<uint32_t, std::string> > v;
		for (std::map<std::string, uint32_t>::const_iterator it = nNoLMByModel.begin();
			 it != nNoLMByModel.end(); ++it)
			v.push_back(std::make_pair(it->second, it->first));
		std::sort(v.begin(), v.end());
		std::string sTop;
		int nShown = 0;
		for (size_t i = v.size(); i > 0 && nShown < 8; --i, ++nShown)
		{
			char sz[96];
			sprintf_s(sz, "%s %u  ", v[i - 1].second.c_str(), v[i - 1].first);
			sTop += sz;
		}
		Log("  R3D LIGHT GRID TAKERS: %u models have lightmap-less polygons"
			" (lit %s) - most: %s", (unsigned)v.size(),
			g_bVertexColour ? "by their VERTEX COLOUR" : "from the grid", sTop.c_str());
		if (g_bVertexColour)
			Log("  R3D VERTEX COLOUR: %d of those polygons are white (drawn as their"
				" texture), %d carry a tint", nVtxWhite, nVtxTinted);
	}
	if (g_bHaveSky)
		Log("  R3D SKY LIGHTMAPS: %d sky polygons carry a lightmap (mean %.2f),"
			" %d do not", nSkyLit, nSkyLitTexels ? fSkyLitSum / (double)nSkyLitTexels : 0.0,
			nSkyUnlit);
	if (g_bHaveSky && nSkyVC)
		Log("  R3D SKY VERTEX COLOUR: the sky's %d lightmap-less polygons are lit by"
			" their vertex colour, mean (%.2f %.2f %.2f) over %ld vertices, %d polygons"
			" plain white", nSkyUnlit, fSkyVC[0] / nSkyVC, fSkyVC[1] / nSkyVC,
			fSkyVC[2] / nSkyVC, nSkyVC, nSkyVCWhite);
	// ---- THE LEVEL'S OWN LIGHTS ---------------------------------------
	//
	// Until now this grid was filled ONLY from lightmap texels, because the
	// engine reports no dynamic lights and that was read as "the lighting is
	// unreachable". It is not: the .DAT's object block carries every light the
	// level was authored with, and world.cpp now parses them.
	//
	// A texel grid can only see light where a lightmap landed, which is a thin
	// shell through a mostly empty box - the 103-world sweep found 16 levels
	// with a thin grid and 6 with none, and M11S01, one of the empty ones, has
	// 1962 lights in its file.
	//
	// SPLATTED rather than gathered: walking every cell and asking every light
	// is 600k x 2000, where pushing each light into the cells inside its own
	// radius is a few hundred per light. Linear falloff, and the author's own
	// LightObjects flag decides whether a light is one that lights models at
	// all.
	//
	// It goes in BEFORE LGridEnd so the dilation pass extends it the same way
	// it extends the texels, and through LGridAdd so a cell holding both is
	// the average of both rather than one overwriting the other.
	g_nLightsUsed = g_nLightCells = 0;
	if (g_bLightObjects && g_pWorldFile && !g_LGrid.cnt.empty())
	{
		for (size_t li = 0; li < g_pWorldFile->Lights.size(); ++li)
		{
			const WorldLight& L = g_pWorldFile->Lights[li];
			if (!L.bObjects || L.fRadius <= 1.0f) continue;
			++g_nLightsUsed;

			int lo[3], hi[3];
			for (int i = 0; i < 3; ++i)
			{
				lo[i] = (int)floorf((L.fPos[i] - L.fRadius - g_LGrid.fMin[i])
									/ g_LGrid.fCell);
				hi[i] = (int)floorf((L.fPos[i] + L.fRadius - g_LGrid.fMin[i])
									/ g_LGrid.fCell);
			}
			for (int z = lo[2]; z <= hi[2]; ++z)
			for (int y = lo[1]; y <= hi[1]; ++y)
			for (int x = lo[0]; x <= hi[0]; ++x)
			{
				const int ci = LGridIndex(x, y, z);
				if (ci < 0) continue;
				const float cx = g_LGrid.fMin[0] + ((float)x + 0.5f) * g_LGrid.fCell;
				const float cy = g_LGrid.fMin[1] + ((float)y + 0.5f) * g_LGrid.fCell;
				const float cz = g_LGrid.fMin[2] + ((float)z + 0.5f) * g_LGrid.fCell;
				const float dx = cx - L.fPos[0];
				const float dy = cy - L.fPos[1];
				const float dz = cz - L.fPos[2];
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (d2 >= L.fRadius * L.fRadius) continue;
				const float fFall = 1.0f - sqrtf(d2) / L.fRadius;
				const float k = fFall * (L.fBright > 0.0f ? L.fBright : 1.0f);
				const float c[3] = { L.fRGB[0] * k, L.fRGB[1] * k, L.fRGB[2] * k };
				const float ctr[3] = { cx, cy, cz };
				LGridAdd(ctr, c);
				++g_nLightCells;
			}
		}
		Log("  R3D MODEL LIGHTS: %ld of %u lights in the file light models,"
			" splatted into %ld cell samples",
			g_nLightsUsed, (unsigned)g_pWorldFile->Lights.size(),
			g_nLightCells);
	}
	else if (g_bLightObjects && g_pWorldFile)
	{
		Log("  R3D MODEL LIGHTS: %u lights in the file, but the grid is empty"
			" - nothing splatted", (unsigned)g_pWorldFile->Lights.size());
	}

	LGridEnd();
	LGridUpload();
	Log("  R3D MODEL LIGHT GRID: %ld of %ld cells carry light (%.1f%%),"
		" %.0f units per cell, level ambient %.3f %.3f %.3f%s",
		g_nLGridFilled, g_nLGridCells,
		g_nLGridCells ? 100.0 * (double)g_nLGridFilled / (double)g_nLGridCells : 0.0,
		g_LGrid.fCell, g_fWorldAmbient[0], g_fWorldAmbient[1], g_fWorldAmbient[2],
		g_bModelLight ? "" : "   <- +StubModelLight 0: not used");
}

void R3D_AddWorldMs(double fMs);
// Defined beside the stall detector further down; set at the sites that fire.
extern int g_bCamSaidThisFrame;
extern int g_bAccountThisFrame;
int R3D_HaveWorld() { return g_pVB ? 1 : 0; }

// ---- THE GPU'S OWN FRAME TIME ------------------------------------------
// Timestamp queries round each frame, two frames in flight, read a frame
// late without flushing. The watchdog shows the CPU waiting on the driver;
// this says whether it waits because the GPU is over its 11 ms budget.
static ID3D11Query* g_pGpuDisjoint[2] = { nullptr, nullptr };
static ID3D11Query* g_pGpuT0[2] = { nullptr, nullptr };
static ID3D11Query* g_pGpuT1[2] = { nullptr, nullptr };
static ID3D11Query* g_pGpuPh[2][4] = { { nullptr, nullptr, nullptr, nullptr }, { nullptr, nullptr, nullptr, nullptr } };
static int    g_bGpuFirstPass = 0;		// the phase stamps belong to the first pass only
static double g_fGpuPhSum[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
static ID3D11Query* g_pGpuTail[2][3] = { { nullptr, nullptr, nullptr }, { nullptr, nullptr, nullptr } };

// Two dozen small objects, leaked once per renderer reload before this.
// Nothing here is big; the point is that a teardown which forgets ANYTHING
// is a teardown nobody can trust, and the 40 MB mesh buffer was found by
// auditing this same list mechanically.
// The 11.1 context interface, taken once by QueryInterface and never given
// back. One reference, and it was enough to keep the device alive.
ID3D11DeviceContext1* g_pCtx1 = nullptr;
static void ReleaseCtx1()
{
	if (g_pCtx1) { g_pCtx1->Release(); g_pCtx1 = nullptr; }
}

static void ReleaseGpuQueries()
{
	for (int i = 0; i < 2; ++i)
	{
		if (g_pGpuDisjoint[i]) { g_pGpuDisjoint[i]->Release(); g_pGpuDisjoint[i] = nullptr; }
		if (g_pGpuT0[i]) { g_pGpuT0[i]->Release(); g_pGpuT0[i] = nullptr; }
		if (g_pGpuT1[i]) { g_pGpuT1[i]->Release(); g_pGpuT1[i] = nullptr; }
		for (int j = 0; j < 4; ++j)
			if (g_pGpuPh[i][j]) { g_pGpuPh[i][j]->Release(); g_pGpuPh[i][j] = nullptr; }
		for (int j = 0; j < 3; ++j)
			if (g_pGpuTail[i][j]) { g_pGpuTail[i][j]->Release(); g_pGpuTail[i][j] = nullptr; }
	}
}
static int    g_nGpuPassNo = 0;			// world passes seen this frame
static int    g_nGpuSlot = 0, g_bGpuOpen = 0, g_bGpuArmed[2] = { 0, 0 };
static double g_fGpuSum = 0.0, g_fGpuWorst = 0.0;
// THE GPU'S IDLE GAP between our frames, against the CPU's. The game submits
// a frame every 11 ms; if the GPU starts our next frame 90 ms after it
// finished the last one while the CPU gap stayed 11, the GPU was withheld
// from us by someone else in that time - the runtime's encoder, another
// process - and no change inside the game can shorten it.
static UINT64 g_nGpuPrevT1 = 0, g_nGpuPrevFreq = 0;
static double g_fGpuGapWorst = 0.0, g_fGpuGapSum = 0.0; static long g_nGpuGapOver30 = 0;
static LARGE_INTEGER g_qGpuPrevCpu = { 0 };
static double g_fCpuGapAtWorst = 0.0;
static long   g_nGpuFrames = 0, g_nGpuOver11 = 0, g_nGpuOver20 = 0;
extern "C" void R3D_GpuTailStamp(int i)	// 0 second pass done, 1 before publish, 2 after publish
{
	RenderGuard _renderGuard;
	if (!g_pCtx || !g_bGpuOpen || i < 0 || i > 2) return;
	if (g_pGpuTail[g_nGpuSlot][i]) g_pCtx->End(g_pGpuTail[g_nGpuSlot][i]);
}
extern "C" void R3D_GpuPhaseStamp(int i)
{
	RenderGuard _renderGuard;
	if (!g_pCtx || !g_bGpuOpen || !g_bGpuFirstPass || i < 0 || i > 3) return;
	if (g_pGpuPh[g_nGpuSlot][i]) g_pCtx->End(g_pGpuPh[g_nGpuSlot][i]);
	if (i == 3) g_bGpuFirstPass = 0;
}

extern "C" void R3D_GpuFrameBegin()
{
	RenderGuard _renderGuard;
	if (!g_pCtx || !g_pDev || g_bGpuOpen) return;
	if (!g_pGpuDisjoint[0])
	{
		D3D11_QUERY_DESC qd{};
		for (int i = 0; i < 2; ++i)
		{
			qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT; g_pDev->CreateQuery(&qd, &g_pGpuDisjoint[i]);
			qd.Query = D3D11_QUERY_TIMESTAMP; g_pDev->CreateQuery(&qd, &g_pGpuT0[i]);
			g_pDev->CreateQuery(&qd, &g_pGpuT1[i]);
			for (int k = 0; k < 4; ++k) g_pDev->CreateQuery(&qd, &g_pGpuPh[i][k]);
			for (int k = 0; k < 3; ++k) g_pDev->CreateQuery(&qd, &g_pGpuTail[i][k]);
		}
	}
	const int s = g_nGpuSlot;
	if (!g_pGpuDisjoint[s] || !g_pGpuT0[s] || !g_pGpuT1[s]) return;
	g_pCtx->Begin(g_pGpuDisjoint[s]);
	g_pCtx->End(g_pGpuT0[s]);
	g_bGpuOpen = 1;
	g_bGpuFirstPass = 1;
	g_nGpuPassNo = 0;
}

extern "C" void R3D_GpuFrameEnd()
{
	RenderGuard _renderGuard;
	if (!g_pCtx || !g_bGpuOpen) return;
	const int s = g_nGpuSlot;
	g_pCtx->End(g_pGpuT1[s]);
	g_pCtx->End(g_pGpuDisjoint[s]);
	g_bGpuArmed[s] = 1;
	g_bGpuOpen = 0;
	g_nGpuSlot ^= 1;
	// Read the OTHER slot: last frame's, which the GPU has had a frame to finish.
	const int o = g_nGpuSlot;
	if (!g_bGpuArmed[o]) return;
	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
	UINT64 t0 = 0, t1 = 0;
	if (g_pCtx->GetData(g_pGpuDisjoint[o], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	if (g_pCtx->GetData(g_pGpuT0[o], &t0, sizeof t0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	if (g_pCtx->GetData(g_pGpuT1[o], &t1, sizeof t1, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
	g_bGpuArmed[o] = 0;
	if (dj.Disjoint || !dj.Frequency || t1 <= t0) return;
	const double ms = (double)(t1 - t0) * 1000.0 / (double)dj.Frequency;
	{
		LARGE_INTEGER qc, qf; QueryPerformanceCounter(&qc); QueryPerformanceFrequency(&qf);
		if (g_nGpuPrevT1 && g_nGpuPrevFreq == dj.Frequency && t0 > g_nGpuPrevT1)
		{
			const double fGap = (double)(t0 - g_nGpuPrevT1) * 1000.0 / (double)dj.Frequency;
			const double fCpuGap = (g_qGpuPrevCpu.QuadPart && qf.QuadPart)
				? (double)(qc.QuadPart - g_qGpuPrevCpu.QuadPart) * 1000.0 / (double)qf.QuadPart : 0.0;
			g_fGpuGapSum += fGap;
			if (fGap > 30.0) ++g_nGpuGapOver30;
			if (fGap > g_fGpuGapWorst) { g_fGpuGapWorst = fGap; g_fCpuGapAtWorst = fCpuGap; }
		}
		g_nGpuPrevT1 = t1; g_nGpuPrevFreq = dj.Frequency; g_qGpuPrevCpu = qc;
	}
	// The first pass's phases, and everything after it (the second pass,
	// the 2D layer, the copy) as one remainder.
	{
		UINT64 tp[4] = { 0, 0, 0, 0 }; bool bAll = true;
		for (int k = 0; k < 4 && bAll; ++k)
			if (!g_pGpuPh[o][k] || g_pCtx->GetData(g_pGpuPh[o][k], &tp[k], sizeof tp[k], D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) bAll = false;
		UINT64 tt[3] = { 0, 0, 0 }; bool bTail = true;
		for (int k = 0; k < 3 && bTail; ++k)
			if (!g_pGpuTail[o][k] || g_pCtx->GetData(g_pGpuTail[o][k], &tt[k], sizeof tt[k], D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) bTail = false;
		if (bAll && tp[0] >= t0 && tp[3] <= t1 && tp[3] >= tp[0])
		{
			const double k = 1000.0 / (double)dj.Frequency;
			g_fGpuPhSum[0] += (double)(tp[0] - t0) * k;
			g_fGpuPhSum[1] += (double)(tp[1] - tp[0]) * k;
			g_fGpuPhSum[2] += (double)(tp[2] - tp[1]) * k;
			g_fGpuPhSum[3] += (double)(tp[3] - tp[2]) * k;
			if (bTail && tt[0] >= tp[3] && tt[1] >= tt[0] && tt[2] >= tt[1] && t1 >= tt[2])
			{
				g_fGpuPhSum[4] += (double)(tt[0] - tp[3]) * k;	// the second pass
				g_fGpuPhSum[5] += (double)(tt[1] - tt[0]) * k;	// the 2D layer and the copy
				g_fGpuPhSum[6] += (double)(tt[2] - tt[1]) * k;	// the publish
				g_fGpuPhSum[7] += (double)(t1 - tt[2]) * k;	// after it, to the present stamp
			}
			else g_fGpuPhSum[4] += (double)(t1 - tp[3]) * k;
		}
	}
	g_fGpuSum += ms; ++g_nGpuFrames;
	if (ms > g_fGpuWorst) g_fGpuWorst = ms;
	if (ms > 11.1) ++g_nGpuOver11;
	if (ms > 20.0) ++g_nGpuOver20;
	if (g_nGpuFrames >= 900)
	{
		const double n = (double)g_nGpuFrames;
		Log("  GPU FRAME: avg %.2f ms, worst %.1f, %ld of %ld over 11.1 ms, %ld over 20  (the budget is 11.1)"
			" | first pass: world %.2f, models %.2f, glass+sprites %.2f, resolve %.2f | second pass %.2f | 2D+copy %.2f | publish %.2f | to present %.2f",
			g_fGpuSum / n, g_fGpuWorst, g_nGpuOver11, g_nGpuFrames, g_nGpuOver20,
			g_fGpuPhSum[0] / n, g_fGpuPhSum[1] / n, g_fGpuPhSum[2] / n, g_fGpuPhSum[3] / n,
			g_fGpuPhSum[4] / n, g_fGpuPhSum[5] / n, g_fGpuPhSum[6] / n, g_fGpuPhSum[7] / n);
		Log("  GPU IDLE GAP between our frames: avg %.2f ms, worst %.1f (the CPU gap at that moment %.1f), %ld gaps over 30 ms  - a gap far over the CPU's is the GPU withheld from us",
			g_fGpuGapSum / n, g_fGpuGapWorst, g_fCpuGapAtWorst, g_nGpuGapOver30);
		g_fGpuGapSum = 0.0; g_fGpuGapWorst = 0.0; g_fCpuGapAtWorst = 0.0; g_nGpuGapOver30 = 0;
		g_fGpuSum = 0.0; g_fGpuWorst = 0.0; g_nGpuFrames = 0; g_nGpuOver11 = 0; g_nGpuOver20 = 0;
		for (int k = 0; k < 8; ++k) g_fGpuPhSum[k] = 0.0;
	}
}

// ---- WHERE A PASS SPENDS ITS TIME ------------------------------------------
// CPU time between marks, per pass, four buckets. Reported every 900 passes.
static double s_fPhaseMs[4] = { 0, 0, 0, 0 };
static long   s_nPhasePasses = 0;
static LARGE_INTEGER s_qPhaseLast = { 0 };
extern "C" void R3D_GpuPhaseStamp(int i);
static void PhaseMark(int i)
{
	if (i >= 0) R3D_GpuPhaseStamp(i);
	LARGE_INTEGER q, f;
	QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
	if (i >= 0 && i < 4 && s_qPhaseLast.QuadPart && f.QuadPart)
		s_fPhaseMs[i] += (double)(q.QuadPart - s_qPhaseLast.QuadPart) * 1000.0 / (double)f.QuadPart;
	s_qPhaseLast = q;
}
static void PhaseReport()
{
	if (++s_nPhasePasses < 900) return;
	const double n = (double)s_nPhasePasses;
	Log("  R3D COST per pass (CPU ms, avg of %ld): world %.2f | models %.2f | glass+sprites+blend %.2f | resolve %.2f | total %.2f  (x2 passes per frame)",
		s_nPhasePasses, s_fPhaseMs[0] / n, s_fPhaseMs[1] / n, s_fPhaseMs[2] / n, s_fPhaseMs[3] / n,
		(s_fPhaseMs[0] + s_fPhaseMs[1] + s_fPhaseMs[2] + s_fPhaseMs[3]) / n);
	for (int i = 0; i < 4; ++i) s_fPhaseMs[i] = 0.0;
	s_nPhasePasses = 0;
	{
		char sz2D[200]; R2D_MapWaitLine(sz2D, sizeof sz2D);
		Log("  MAP WAITS: cb3d %ld maps, %ld over 2 ms, worst %.1f, total %.1f ms | mesh %ld/%ld worst %.1f | sprites %ld/%ld worst %.1f | %s",
			s_nMapCalls[0], s_nMapSlow[0], s_fMapWorst[0], s_fMapMs[0],
			s_nMapCalls[1], s_nMapSlow[1], s_fMapWorst[1],
			s_nMapCalls[2], s_nMapSlow[2], s_fMapWorst[2], sz2D);
		// A REFUSED MAP IS A CRASH THAT DID NOT HAPPEN, so say how many.
		// The engine calls back into this DLL between a teardown and a
		// rebuild, with every buffer already released and nulled; before the
		// check in TimedMap those calls went into D3D11 with a null resource
		// and took the process down. Three of six missions did exactly that
		// on 11 September once focus was being stolen.
		if (s_nMapRefused)
		{
			Log("  MAP REFUSED %lu times - the renderer had no context or the"
				" buffer was released; each one would have been a crash",
				s_nMapRefused);
			s_nMapRefused = 0;
		}
		for (int i = 0; i < 3; ++i) { s_nMapCalls[i] = s_nMapSlow[i] = 0; s_fMapMs[i] = s_fMapWorst[i] = 0.0; }
	}
}

// The matrix that takes a world model's authored vertices to where the engine
// has moved it - v' = B(v - P0) + P1, B = R1 * R0^T - folded onto the pass's
// view-projection in row-vector form. Same arithmetic as the world pass; kept
// as one function so the glass pass can carry an opened door too.
static void MovedMvp(const WMBase* pWM, const float* mvp, float* out)
{
	float B[9];
	for (int r2 = 0; r2 < 3; ++r2)
		for (int c2 = 0; c2 < 3; ++c2)
		{
			float v2 = 0.0f;
			for (int k2 = 0; k2 < 3; ++k2)
				v2 += pWM->lr[r2 * 3 + k2] * pWM->r[c2 * 3 + k2];
			B[r2 * 3 + c2] = v2;
		}
	float t[3];
	for (int r2 = 0; r2 < 3; ++r2)
		t[r2] = pWM->lp[r2]
			  - (B[r2*3+0]*pWM->p[0] + B[r2*3+1]*pWM->p[1] + B[r2*3+2]*pWM->p[2]);
	const float M[16] = { B[0],B[3],B[6],0, B[1],B[4],B[7],0,
						  B[2],B[5],B[8],0, t[0],t[1],t[2],1 };
	for (int r2 = 0; r2 < 4; ++r2)
		for (int c2 = 0; c2 < 4; ++c2)
		{
			float v2 = 0.0f;
			for (int k2 = 0; k2 < 4; ++k2)
				v2 += M[r2 * 4 + k2] * mvp[k2 * 4 + c2];
			out[r2 * 4 + c2] = v2;
		}
}

// Write a matrix and the lmp row into the world constant buffer, keeping the
// current lightmap scale and output alpha.
static void WriteWorldCB(const float* m, float fCut)
{
	D3D11_MAPPED_SUBRESOURCE wm{};
	if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &wm, 0)))
	{
		memcpy(wm.pData, m, 16 * sizeof(float));
		const float lmpw[4] = { (g_fFlatLight > 0.0f) ? g_fFlatLight : g_fLMScale,
								(g_fFlatLight > 0.0f) ? -1.0f : (float)g_bLMOnly, fCut, g_fAlphaOut };
		memcpy((char*)wm.pData + 16 * sizeof(float), lmpw, sizeof lmpw);
		g_pCtx->Unmap(g_pCB, 0);
		g_fAlphaCutSet = fCut;
	}
}

// A batch's picture now: its animation's frame at this pass's clock, or the
// plain texture, or white so a missing picture is a slab and not a hole.
static ID3D11ShaderResourceView* BatchSRV(size_t b)
{
	if (g_Batches[b].nAnim1)
	{
		ID3D11ShaderResourceView* p = SprAnim_SRV((int)g_Batches[b].nAnim1, g_fNowSec);
		if (p) return p;
	}
	return g_Batches[b].pSRV ? g_Batches[b].pSRV : g_pWhite;
}

// ---------------------------------------------------------------------------
// THE EFFECTS PASS. Everything the client gathered into g_Prim (VRPrims.cpp):
// particle systems as POINTS the renderer faces per eye, rain as LINES, water
// and canvas polygons as TRIS. Its own vertex layout and shader - these carry
// a colour per vertex, which the world's never needed - and its own ring
// buffer. Drawn back to front by run, blended, no depth write, in the sprite
// pass's place in the frame.
// ---------------------------------------------------------------------------
namespace
{
	struct PVtx { float x, y, z; float r, g, b, a; float u, v; };
	ID3D11VertexShader* g_pPrimVS = nullptr;
	ID3D11PixelShader*  g_pPrimPS = nullptr;
	ID3D11InputLayout*  g_pPrimIL = nullptr;
	ID3D11Buffer*       g_pPrimVB = nullptr;
	ID3D11Buffer*       g_pPrimCB = nullptr;
	ID3D11SamplerState* g_pSampClamp = nullptr;
	const UINT kPrimRegions = 6;
	const UINT kPrimVerts = 196608;		// per region
	bool g_bPrimTried = false;

	const char* kPrimShader =
		"cbuffer CB : register(b0) { row_major float4x4 mvp; float4 lmp; };\n"
		"cbuffer PRM : register(b3) { float4 pf; };\n"	// x notex, y diffuse alpha, z multiply
		"Texture2D tex0 : register(t0);\n"
		"SamplerState samp : register(s0);\n"
		"struct VI { float3 pos : POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
		"struct VO { float4 pos : SV_POSITION; float4 c : COLOR0; float2 uv : TEXCOORD0; };\n"
		"VO PVS(VI i) { VO o; o.pos = mul(float4(i.pos, 1.0f), mvp); o.c = i.c; o.uv = i.uv; return o; }\n"
		"float4 PPS(VO i) : SV_TARGET {\n"
		"  float4 t = (pf.x > 0.5f) ? float4(1,1,1,1) : tex0.Sample(samp, i.uv);\n"
		"  float a = (pf.y > 0.5f) ? i.c.a : t.a * i.c.a;\n"
		"  float3 c = t.rgb * i.c.rgb;\n"
		// DEST x ZERO cannot see alpha: fade a multiply run towards white first.
		"  if (pf.z > 0.5f) return float4(lerp(float3(1,1,1), c, a), 1.0f);\n"
		"  return float4(c, a);\n"
		"}\n";

	bool PrimResources()
	{
		if (g_pPrimVS) return true;
		if (g_bPrimTried) return false;
		g_bPrimTried = true;
		ID3DBlob *pVS = nullptr, *pPS = nullptr, *pErr = nullptr;
		HRESULT hr = D3DCompile(kPrimShader, strlen(kPrimShader), nullptr, nullptr, nullptr,
								"PVS", "vs_4_0", 0, 0, &pVS, &pErr);
		if (FAILED(hr))
		{
			Log("  R3D: effects VS failed hr=%08X: %s", (unsigned)hr,
				pErr ? (const char*)pErr->GetBufferPointer() : "");
			if (pErr) pErr->Release();
			return false;
		}
		if (pErr) { pErr->Release(); pErr = nullptr; }
		hr = D3DCompile(kPrimShader, strlen(kPrimShader), nullptr, nullptr, nullptr,
						"PPS", "ps_4_0", 0, 0, &pPS, &pErr);
		if (FAILED(hr))
		{
			Log("  R3D: effects PS failed hr=%08X: %s", (unsigned)hr,
				pErr ? (const char*)pErr->GetBufferPointer() : "");
			if (pErr) pErr->Release();
			pVS->Release();
			return false;
		}
		if (pErr) pErr->Release();
		g_pDev->CreateVertexShader(pVS->GetBufferPointer(), pVS->GetBufferSize(), nullptr, &g_pPrimVS);
		g_pDev->CreatePixelShader(pPS->GetBufferPointer(), pPS->GetBufferSize(), nullptr, &g_pPrimPS);
		const D3D11_INPUT_ELEMENT_DESC il[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		g_pDev->CreateInputLayout(il, 3, pVS->GetBufferPointer(), pVS->GetBufferSize(), &g_pPrimIL);
		pVS->Release(); pPS->Release();
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = kPrimVerts * sizeof(PVtx) * kPrimRegions;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		g_pDev->CreateBuffer(&bd, nullptr, &g_pPrimVB);
		D3D11_BUFFER_DESC cb{};
		cb.ByteWidth = 16; cb.Usage = D3D11_USAGE_DYNAMIC;
		cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		g_pDev->CreateBuffer(&cb, nullptr, &g_pPrimCB);
		if (g_pSamp && !g_pSampClamp)
		{
			D3D11_SAMPLER_DESC sd{}; g_pSamp->GetDesc(&sd);
			sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			g_pDev->CreateSamplerState(&sd, &g_pSampClamp);
		}
		Log("  R3D: effects pass ready - %s shaders, buffer %u regions of %u vertices (%.1f MB)",
			(g_pPrimVS && g_pPrimPS && g_pPrimIL && g_pPrimVB) ? "all" : "NOT ALL",
			kPrimRegions, kPrimVerts, (double)bd.ByteWidth / 1048576.0);
		return g_pPrimVS && g_pPrimPS && g_pPrimIL && g_pPrimVB && g_pPrimCB;
	}

	void PrimReleaseAll()
	{
		if (g_pPrimVS) { g_pPrimVS->Release(); g_pPrimVS = nullptr; }
		if (g_pPrimPS) { g_pPrimPS->Release(); g_pPrimPS = nullptr; }
		if (g_pPrimIL) { g_pPrimIL->Release(); g_pPrimIL = nullptr; }
		if (g_pPrimVB) { g_pPrimVB->Release(); g_pPrimVB = nullptr; }
		if (g_pPrimCB) { g_pPrimCB->Release(); g_pPrimCB = nullptr; }
		if (g_pSampClamp) { g_pSampClamp->Release(); g_pSampClamp = nullptr; }
		g_bPrimTried = false;
	}

	struct PrimDraw { UINT nStart, nCount; D3D11_PRIMITIVE_TOPOLOGY topo;
					  ID3D11ShaderResourceView* pSRV; uint32_t nFlags; };

	// nKindFilter: -1 everything, 2 polygrids only, -2 everything BUT
	// polygrids. See the water note at the call site before the glass pass.
	void DrawPrimRuns(int nKindFilter = -1)
	{
		if (!PrimResources()) return;
		float rr[3], uu[3], ff[3];
		QuatBasis(g_fLastQuat, rr, uu, ff);

		// Back to front by run centre.
		static std::vector<uint32_t> order; static std::vector<float> dist;
		order.clear(); dist.assign(g_Prim.nRuns, 0.0f);
		for (uint32_t i = 0; i < g_Prim.nRuns; ++i)
		{
			const float* c = g_Prim.runs[i].fCentre;
			const float dx = c[0] - g_fLastPos[0], dy = c[1] - g_fLastPos[1], dz = c[2] - g_fLastPos[2];
			dist[i] = dx*dx + dy*dy + dz*dz;
			order.push_back(i);
		}
		std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return dist[a] > dist[b]; });

		static UINT s_nRegion = 0;
		s_nRegion = (s_nRegion + 1) % kPrimRegions;
		const UINT nRegionBytes = s_nRegion * kPrimVerts * sizeof(PVtx);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(TimedMap(g_pPrimVB, D3D11_MAP_WRITE_NO_OVERWRITE, &m, 3))) return;
		PVtx* out = (PVtx*)((char*)m.pData + nRegionBytes);
		UINT n = 0;
		static std::vector<PrimDraw> draws; draws.clear();
		static const float kU[6] = { -1,  1,  1, -1,  1, -1 };
		static const float kV[6] = {  1,  1, -1,  1, -1, -1 };
		for (size_t oi = 0; oi < order.size(); ++oi)
		{
			const VRPrimRun& r = g_Prim.runs[order[oi]];
			if (nKindFilter == 2 && r.nKind != 2) continue;
			if (nKindFilter == -2 && r.nKind == 2) continue;
			if (r.nStart + r.nCount > g_Prim.nVerts) continue;
			if (r.nFlags & VRPRIM_F_REALLYCLOSE) continue;		// view-relative: not yet
			ID3D11ShaderResourceView* pSRV = g_pWhite;
			// THE EYEPIECE wears the scope pass's own picture. Never inside
			// that pass: the target cannot be sampled while it is being drawn.
			if (r.nFlags & VRPRIM_F_SCOPELENS)
			{
				if (g_bScopePass || !g_pScopeSRV) continue;
				pSRV = g_pScopeSRV;
			}
			else if (!(r.nFlags & VRPRIM_F_NOTEX))
			{
				const int nA = SprAnim_Get(g_pDev, r.szTex);
				ID3D11ShaderResourceView* p = SprAnim_SRV(nA, g_fNowSec);
				if (p) pSRV = p; else ++g_nPrimNoTex;
			}
			PrimDraw d{}; d.nStart = n; d.pSRV = pSRV; d.nFlags = r.nFlags;
			d.topo = (r.nType == VRPRIM_T_LINES) ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
												: D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			for (uint32_t i = 0; i < r.nCount; ++i)
			{
				const VRPrimVert& v = g_Prim.verts[r.nStart + i];
				if (r.nType == VRPRIM_T_POINTS)
				{
					if (n + 6 > kPrimVerts) break;
					const float hw = v.fSize;
					for (int c = 0; c < 6; ++c)
					{
						PVtx& o = out[n++];
						o.x = v.fPos[0] + rr[0]*kU[c]*hw + uu[0]*kV[c]*hw;
						o.y = v.fPos[1] + rr[1]*kU[c]*hw + uu[1]*kV[c]*hw;
						o.z = v.fPos[2] + rr[2]*kU[c]*hw + uu[2]*kV[c]*hw;
						o.r = v.fColour[0]; o.g = v.fColour[1]; o.b = v.fColour[2]; o.a = v.fColour[3];
						o.u = (kU[c] + 1.0f) * 0.5f;
						o.v = 1.0f - (kV[c] + 1.0f) * 0.5f;
					}
				}
				else
				{
					if (n + 1 > kPrimVerts) break;
					PVtx& o = out[n++];
					o.x = v.fPos[0]; o.y = v.fPos[1]; o.z = v.fPos[2];
					o.r = v.fColour[0]; o.g = v.fColour[1]; o.b = v.fColour[2]; o.a = v.fColour[3];
					o.u = v.fUV[0]; o.v = v.fUV[1];
				}
			}
			d.nCount = n - d.nStart;
			if (d.nCount) draws.push_back(d);
			if (n >= kPrimVerts) break;
		}
		g_pCtx->Unmap(g_pPrimVB, 0);
		if (draws.empty()) return;

		const UINT nStride = sizeof(PVtx), nOff = nRegionBytes;
		g_pCtx->IASetInputLayout(g_pPrimIL);
		g_pCtx->VSSetShader(g_pPrimVS, nullptr, 0);
		g_pCtx->PSSetShader(g_pPrimPS, nullptr, 0);
		g_pCtx->IASetVertexBuffers(0, 1, &g_pPrimVB, &nStride, &nOff);
		g_pCtx->VSSetConstantBuffers(0, 1, &g_pCB);
		g_pCtx->PSSetConstantBuffers(3, 1, &g_pPrimCB);
		// A PASS MUST SET ITS OWN STATE, unconditionally. The glass pass
		// writes a MOVED world model's own matrix into this buffer and puts
		// the static one back afterwards - but "puts it back" is a chain of
		// three assumptions, and this pass draws in world coordinates and
		// would silently inherit a swinging door's frame if any link ever
		// changed. One buffer write a pass is nothing; the class of bug it
		// removes has cost this project days.
		g_fAlphaOut = 1.0f; g_fAlphaCutSet = -1.0f;
		WriteWorldCB(g_fLastMVP, 0.0f);
		int nBlendNow = -1, nDepthNow = -1, nClampNow = -1;
		D3D11_PRIMITIVE_TOPOLOGY topoNow = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		float pfNow[4] = { -1, -1, -1, -1 };
		const float kF[4] = { 0, 0, 0, 0 };
		for (size_t i = 0; i < draws.size(); ++i)
		{
			const PrimDraw& d = draws[i];
			// THE LENS IS OPAQUE GLASS. Alpha-blended, the disc went see-through
			// wherever the scope's picture carried a low alpha - the additive aim
			// dot wrote one - and the dark tube behind showed as a ring round the
			// dot. 3 = opaque, like a sprite's.
			const int nBlend = (d.nFlags & VRPRIM_F_SCOPELENS) ? 3 : (d.nFlags & VRPRIM_F_ADDITIVE) ? 1 : (d.nFlags & VRPRIM_F_MULTIPLY) ? 2 : 0;
			if (nBlend != nBlendNow)
			{
				nBlendNow = nBlend;
				ID3D11BlendState* pBS = (nBlend == 3 && g_pBlendOpaque) ? g_pBlendOpaque
									  : (nBlend == 2 && g_pBlendMul) ? g_pBlendMul
									  : (nBlend == 1 && g_pBlendAdd) ? g_pBlendAdd : g_pBlendAlpha;
				if (pBS) g_pCtx->OMSetBlendState(pBS, kF, 0xFFFFFFFF);
			}
			const int nDepth = (d.nFlags & VRPRIM_F_NOZREAD) ? 2 : (d.nFlags & VRPRIM_F_ZWRITE) ? 1 : 0;
			if (nDepth != nDepthNow)
			{
				nDepthNow = nDepth;
				g_pCtx->OMSetDepthStencilState(
					nDepth == 2 ? (g_pDSSNoDepth ? g_pDSSNoDepth : g_pDSSNoWrite)
					: nDepth == 1 ? ((g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS)
					: g_pDSSNoWrite, 0);
			}
			const int nClamp = (d.nFlags & VRPRIM_F_CLAMP) ? 1 : 0;
			if (nClamp != nClampNow)
			{
				nClampNow = nClamp;
				ID3D11SamplerState* pS = (nClamp && g_pSampClamp) ? g_pSampClamp : g_pSamp;
				g_pCtx->PSSetSamplers(0, 1, &pS);
			}
			if (d.topo != topoNow) { topoNow = d.topo; g_pCtx->IASetPrimitiveTopology(topoNow); }
			const float pf[4] = { (d.nFlags & VRPRIM_F_NOTEX) ? 1.0f : 0.0f,
								  (d.nFlags & VRPRIM_F_DIFFUSEALPHA) ? 1.0f : 0.0f,
								  (nBlend == 2) ? 1.0f : 0.0f, 0.0f };
			if (memcmp(pf, pfNow, sizeof pf) != 0)
			{
				memcpy(pfNow, pf, sizeof pf);
				D3D11_MAPPED_SUBRESOURCE cm{};
				if (SUCCEEDED(g_pCtx->Map(g_pPrimCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm)))
				{ memcpy(cm.pData, pf, sizeof pf); g_pCtx->Unmap(g_pPrimCB, 0); }
			}
			g_pCtx->PSSetShaderResources(0, 1, &d.pSRV);
			g_pCtx->Draw(d.nCount, d.nStart);
			++g_nPrimRunsDrawn; g_nPrimVertsDrawn += d.nCount;
		}
		// The world's pipeline back for whatever draws next.
		g_pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		g_pCtx->IASetInputLayout(g_pIL);
		g_pCtx->VSSetShader(g_pVS, nullptr, 0);
		g_pCtx->PSSetShader(g_pPS, nullptr, 0);
		g_pCtx->PSSetSamplers(0, 1, &g_pSamp);
		g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStride, &nOff);
		const UINT nStrW = sizeof(Vtx), nOffW = 0;
		g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStrW, &nOffW);
	}
}

// THE ENVIRONMENT PASS FOR ONE BATCH: the same vertices again, the map on t0,
// added, with envp.w raised for the draw. The caller puts its blend back.
static void EnvDraw(size_t b, const float* pQuat, const float* pPos)
{
	if (!g_pCBEnv || !g_pBlendAdd || !g_Batches[b].pEnvSRV) return;
	float er[3], eu[3], ef[3];
	QuatBasis(pQuat, er, eu, ef);
	const float t = (float)g_fNowSec;
	const float kF[4] = { 0, 0, 0, 0 };
	// WATER MOVES, THE REST DOES NOT. A still surface keeps the fine map
	// (0.5: the whole map across a full swing of the reflection) it had
	// before the waterfall work; water takes the coarser one and the pan.
	// SIDEWAYS, along the axis the reflection varies across a wide face, so
	// the pan slides the fine streaks and never lifts a whole face at once:
	// panned up the map, a short wide face brightened and darkened together
	// (the brightness flash of 13 September).
	const bool  bPan   = g_Batches[b].bEnvPan;
	const float fRepU  = (g_fEnvRepeatU > 1.0f) ? 1.0f / g_fEnvRepeatU : 0.01f;
	const float fRepV  = (g_fEnvRepeatV > 1.0f) ? 1.0f / g_fEnvRepeatV : 0.01f;
	// Water runs DOWN its face: the pan grows with time, so a feature at a
	// fixed map row sits ever lower along the face's up axis.
	// Measured 13 September: +pan ran the sheet UP the face; the sign is here.
	// + runs the sheet DOWN in the headset (14 September: at the - sign it
	// moved upwards like wallpaper being pulled up; the desk's
	// shift measure had its sign backwards).
	// In map units per second now (StubEnvPan100). A feature at a fixed v
	// sits higher on the face as the pan grows, so DOWN is the pan shrinking.
	const float fPanV  = bPan ? -g_fEnvPan * t : 0.0f;
	// WATER MODULATES; everything else ADDS, attenuated by its own light.
	// Modulating every map was tried on 19 September and was wrong: the HQ's
	// white dome lights went from flat 255 to 204-242 facets, because a
	// multiply always shows the map's swing, while an add onto a clipped
	// white stays white - which is retail. The statue read grey for another
	// reason entirely (no lightmap; see the object-lit rule in the build).
	// Water's pan and standing-face-only rule travel in envp.x.
	const bool  bMod   = bPan && g_pBlendMod2x != nullptr;
	for (int pass = 0; pass < 2; ++pass)
	{
		const float e[16] = { er[0], er[1], er[2], fRepU, eu[0], eu[1], eu[2], fRepV,
							  pPos ? pPos[0] : 0.0f, pPos ? pPos[1] : 0.0f, pPos ? pPos[2] : 0.0f, bMod ? 1.0f : 0.0f,
							  bPan ? 1.0f : 0.0f, fPanV, bMod ? g_fEnvContrast : g_fEnvScale, pass == 0 ? 1.0f : 0.0f };
		D3D11_MAPPED_SUBRESOURCE em{};
		if (SUCCEEDED(g_pCtx->Map(g_pCBEnv, 0, D3D11_MAP_WRITE_DISCARD, 0, &em)))
		{ memcpy(em.pData, e, sizeof e); g_pCtx->Unmap(g_pCBEnv, 0); }
		if (pass == 0)
		{
			// A PASS MUST SET ITS OWN DEPTH STATE - AND PUT BACK THE ONE IT
			// FOUND, NOT ONE IT GUESSED.
			//
			// This never set a depth state at all, so it ran on whatever the
			// last of 230 batches and several interleaved passes had left. A
			// no-depth state there draws the environment layer over everything
			// in front of it, which on a water surface means the pool and the
			// waterfall painted across the near window mullions - and retail
			// occludes them cleanly (the 13:00 reference shot).
			//
			// The first attempt at this set the state and then RESTORED A
			// GUESS - the world's write-enabled state - while the glass pass
			// that calls it had deliberately set no-write. Every glass batch
			// after the first then wrote depth, and the pool's polygrid, drawn
			// later, was occluded by the water box's own faces. It removed the
			// water entirely. Ask the device what was set and put exactly that
			// back.
			ID3D11DepthStencilState* pWas = nullptr; UINT nWasRef = 0;
			if (g_bEnvDepth && g_pDSSEnv)
			{
				g_pCtx->OMGetDepthStencilState(&pWas, &nWasRef);
				g_pCtx->OMSetDepthStencilState(g_pDSSEnv, nWasRef);
			}
			g_pCtx->OMSetBlendState(bMod ? g_pBlendMod2x : g_pBlendAdd, kF, 0xFFFFFFFF);
			g_pCtx->PSSetShaderResources(0, 1, &g_Batches[b].pEnvSRV);
			// WATER'S REFLECTION CULLS ITS BACK FACES, as the water draw does.
			// This pass ran with the world's cull-none state, so the far side
			// wall and the floor of the Dive's sea box - back faces the water
			// draw never shows - still took the flowing map, and from the boat
			// the far wall is a thin band at the horizon: wedge-shaped
			// patches that moved with the head (a headset clip and the desk,
			// 22 September; +StubEnvMap 0 removed them, a depth bias and a
			// flat-face test did not).
			if (bPan && g_pRSWater) g_pCtx->RSSetState(g_pRSWater);
			g_pCtx->Draw(g_Batches[b].nCount, g_Batches[b].nStart);
			if (bPan && g_pRSWater) g_pCtx->RSSetState(g_pRS);
			++g_nEnvDraws;
			if (pWas) { g_pCtx->OMSetDepthStencilState(pWas, nWasRef); pWas->Release(); }
		}
	}
}

// THE LIGHT ADD, as a full-screen additive triangle in clip space through the
// effects shader. See g_fLightAdd. Drawn into the multisampled target at the
// end of the 3D pass, before the resolve, so both eyes carry it.
static ID3D11Buffer* g_pAddVB = nullptr;
static void DrawLightAdd()
{
	if (!g_pDev || !g_pCtx || !g_pBlendAdd || !g_pDSSNoDepth || !g_pCB) return;
	if (!PrimResources()) return;
	if (!g_pAddVB)
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 3 * sizeof(PVtx);
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(g_pDev->CreateBuffer(&bd, nullptr, &g_pAddVB)) || !g_pAddVB) return;
	}
	D3D11_MAPPED_SUBRESOURCE mm{};
	if (FAILED(g_pCtx->Map(g_pAddVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) return;
	{
		PVtx* v = (PVtx*)mm.pData;
		const float px[3] = { -1.0f, 3.0f, -1.0f }, py[3] = { -1.0f, -1.0f, 3.0f };
		for (int i = 0; i < 3; ++i)
		{
			v[i].x = px[i]; v[i].y = py[i]; v[i].z = 0.5f;
			v[i].r = g_fLightAdd[0]; v[i].g = g_fLightAdd[1]; v[i].b = g_fLightAdd[2]; v[i].a = 1.0f;
			v[i].u = 0.0f; v[i].v = 0.0f;
		}
	}
	g_pCtx->Unmap(g_pAddVB, 0);
	// Identity in the shared constant buffer: the triangle is already in clip
	// space. This is the last draw of the pass; the next pass writes its own.
	D3D11_MAPPED_SUBRESOURCE sm{};
	if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &sm, 0)))
	{
		float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		memcpy(sm.pData, id, sizeof id);
		const float lmp[4] = { g_fLMScale, 0.0f, 0.0f, 1.0f };
		memcpy((char*)sm.pData + sizeof id, lmp, sizeof lmp);
		g_pCtx->Unmap(g_pCB, 0);
	}
	D3D11_MAPPED_SUBRESOURCE cm{};
	if (SUCCEEDED(g_pCtx->Map(g_pPrimCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm)))
	{
		const float pf[4] = { 1.0f, 1.0f, 0.0f, 0.0f };	// no texture, diffuse alpha, not multiply
		memcpy(cm.pData, pf, sizeof pf); g_pCtx->Unmap(g_pPrimCB, 0);
	}
	const float kF[4] = { 0, 0, 0, 0 };
	const UINT nStride = sizeof(PVtx), nOff = 0;
	g_pCtx->IASetInputLayout(g_pPrimIL);
	g_pCtx->IASetVertexBuffers(0, 1, &g_pAddVB, &nStride, &nOff);
	g_pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_pCtx->VSSetShader(g_pPrimVS, nullptr, 0);
	g_pCtx->PSSetShader(g_pPrimPS, nullptr, 0);
	g_pCtx->VSSetConstantBuffers(0, 1, &g_pCB);
	g_pCtx->PSSetConstantBuffers(3, 1, &g_pPrimCB);
	g_pCtx->OMSetBlendState(g_pBlendAdd, kF, 0xFFFFFFFF);
	g_pCtx->OMSetDepthStencilState(g_pDSSNoDepth, 0);
	g_pCtx->Draw(3, 0);
	// The world's pipeline back.
	g_pCtx->OMSetBlendState(g_pBlendOpaque, kF, 0xFFFFFFFF);
	g_bA2CBound = false;
	g_pCtx->OMSetDepthStencilState((g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);
	g_pCtx->IASetInputLayout(g_pIL);
	g_pCtx->VSSetShader(g_pVS, nullptr, 0);
	g_pCtx->PSSetShader(g_pPS, nullptr, 0);
	const UINT nStrW = sizeof(Vtx), nOffW = 0;
	g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStrW, &nOffW);
	static int s_nSaid = 0;
	if (s_nSaid++ < 2)
		Log("  R3D LIGHT ADD: (%.2f %.2f %.2f) added over the whole eye",
			g_fLightAdd[0], g_fLightAdd[1], g_fLightAdd[2]);
}

// The menu sheet backdrop: see the call site in R3D_DrawWorld. Its own
// small vertex buffer, six vertices per sheet, rewritten every pass.
static ID3D11Buffer* g_pSheetVB = nullptr;
// The +StubSkyChecker test pattern (see the sky pass). File scope so the
// teardown can release it: as a function static it outlived the device.
static ID3D11ShaderResourceView* g_pSkyChecker = nullptr;
// Objects created on first use, further down this file than R3D_Destroy can
// see: released from there through this.
static void ReleaseLateBuffers()
{
	if (g_pAddVB)      { g_pAddVB->Release();      g_pAddVB      = nullptr; }
	if (g_pSheetVB)    { g_pSheetVB->Release();    g_pSheetVB    = nullptr; }
	if (g_pSkyChecker) { g_pSkyChecker->Release(); g_pSkyChecker = nullptr; }
}
static int SpriteTexture(uint32_t hObj);
static void DrawMenuSheetBackdrop(float fFovX, float fFovY)
{
	if (!g_pDev || !g_pCtx || !g_pBlendOpaque || !g_pDSSNoDepth) return;
	if (!g_pSheetVB)
	{
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = 8 * 12 * sizeof(Vtx);
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(g_pDev->CreateBuffer(&bd, nullptr, &g_pSheetVB)) || !g_pSheetVB) return;
	}
	float rr[3], uu[3], ff[3];
	QuatBasis(g_fLastQuat, rr, uu, ff);
	const float fWiden = (fFovX > 0.1f && fFovY > 0.1f)
		? (tanf(fFovY * 0.5f) * g_fMenuBand3D) / tanf(fFovX * 0.5f) : 1.0f;
	struct SheetRun { UINT nStart; ID3D11ShaderResourceView* pSRV; };
	SheetRun runs[8]; int nRuns = 0;
	D3D11_MAPPED_SUBRESOURCE mm{};
	if (FAILED(g_pCtx->Map(g_pSheetVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) return;
	Vtx* sv = (Vtx*)mm.pData; UINT nsv = 0;
	static const float kU[6] = { -1,  1,  1, -1,  1, -1 };
	static const float kV[6] = { -1, -1,  1, -1,  1,  1 };
	for (uint32_t z = 0; z < g_Spr.nCount && nRuns < 8; ++z)
	{
		const VRSpriteInst& si = g_Spr.inst[z];
		const int ti = SpriteTexture(si.nObject);
		if (ti < 0 || !g_Tex[ti].pSRV || !IsMenuSheetName(g_Tex[ti].szName)) continue;
		if (g_Tex[ti].fW <= 0.0f || g_Tex[ti].fH <= 0.0f) continue;
		float hw = g_Tex[ti].fW * si.fScale[0] * g_fSpriteScale;
		const float hh = g_Tex[ti].fH * si.fScale[1] * g_fSpriteScale;
		// TWO STRIPS EITHER SIDE OF THE AUTHORED SHEET, not a stretched copy
		// of it (13 September): stretching carried the sheet's transparent
		// window across the band and the pinwheel behind it showed beside
		// the logo. The strips sit at the sheet's own plane and WRITE DEPTH,
		// so the pinwheel's corner beyond the sheet's edge (z 160, behind)
		// is kept out while Cate and the lime boxes (in front) are not.
		if (fWiden <= 1.0f || hw <= 0.0f || hh <= 0.0f) continue;
		const float* P = si.fPos;
		const bool bOwnAxes = (si.nFlags & VRSPRITE_F_ROTATABLE) != 0;
		const float* qr = bOwnAxes ? si.fRight : rr;
		const float* qu = bOwnAxes ? si.fUp    : uu;
		runs[nRuns].nStart = nsv; runs[nRuns].pSRV = g_Tex[ti].pSRV; ++nRuns;
		const float hwOut = hw * fWiden;
		for (int c = 0; c < 12; ++c)
		{
			// c 0..5 the right strip (hw .. hwOut), 6..11 the left one.
			const float fSide = (c < 6) ? 1.0f : -1.0f;
			const float fX = fSide * (hw + (hwOut - hw) * (kU[c % 6] + 1.0f) * 0.5f);
			Vtx& o = sv[nsv++];
			o.x = P[0] + qr[0]*fX + qu[0]*kV[c % 6]*hh;
			o.y = P[1] + qr[1]*fX + qu[1]*kV[c % 6]*hh;
			o.z = P[2] + qr[2]*fX + qu[2]*kV[c % 6]*hh;
			o.nx = -ff[0]; o.ny = -ff[1]; o.nz = -ff[2];
			// ONE PLAIN TEXEL of the sheet, from its top band above the
			// window, where every sheet (menu blue, card orange) is flat.
			o.u = 0.5f;
			o.v = 0.10f;
			o.lu = -2.0f; o.lv = -1.0f;
		}
	}
	g_pCtx->Unmap(g_pSheetVB, 0);
	if (!nRuns) return;
	const float kF[4] = { 0, 0, 0, 0 };
	const UINT nStride = sizeof(Vtx), nOff = 0;
	g_pCtx->IASetVertexBuffers(0, 1, &g_pSheetVB, &nStride, &nOff);
	g_pCtx->OMSetBlendState(g_pBlendOpaque, kF, 0xFFFFFFFF);
	// The pass's own depth state: the strips test and write at the sheet's
	// plane, which is what keeps the pinwheel's corner behind them.
	g_pCtx->OMSetDepthStencilState((g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);
	g_fAlphaCutSet = -1.0f;
	SetAlphaCut(0.0f);
	SetOutAlpha(1.0f);
	for (int r = 0; r < nRuns; ++r)
	{
		g_pCtx->PSSetShaderResources(0, 1, &runs[r].pSRV);
		g_pCtx->Draw(12, runs[r].nStart);
	}
	// Put the pass's state back: its depth, its opaque blend, its vertex buffer.
	g_pCtx->OMSetDepthStencilState((g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);
	g_pCtx->OMSetBlendState(g_pBlendOpaque, kF, 0xFFFFFFFF);
	g_bA2CBound = false;
	{
		const UINT nStrW = sizeof(Vtx), nOffW = 0;
		g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStrW, &nOffW);
	}
	static int s_nSaid = 0;
	if (s_nSaid++ < 2)
		Log("  R3D MENU SHEET BACKDROP: %d sheet(s) given side strips at their own"
			" plane, out to x%.2f for the %.2f:1 panel", nRuns, fWiden, g_fMenuBand3D);
}

void R3D_DrawWorld(const float* pPos, const float* pQuat,
				   float fFovX, float fFovY, float fNear, float fFar,
				   int nLeft, int nTop, int nRight, int nBottom,
				   const float* pTan4, int nEye, int bInterface)
{
	// THE PASS'S OWN CAMERA, every 90 passes: a still scene drifted at the
	// desk with the scene camera, the head and every object static (the
	// club, 21 September), so what each pass is actually handed is logged.
	{
		static unsigned s_nPassSaid = 0;
		if ((++s_nPassSaid % 90) == 1 && pPos && pQuat)
			Log("  R3D PASS eye %d iface %d: pos (%.2f %.2f %.2f) quat (%.4f %.4f %.4f %.4f) fov %.3f %.3f near %.1f far %.0f rect %d %d %d %d tan %s%.4f %.4f %.4f %.4f",
				nEye, bInterface, pPos[0], pPos[1], pPos[2], pQuat[0], pQuat[1], pQuat[2], pQuat[3],
				fFovX, fFovY, fNear, fFar, nLeft, nTop, nRight, nBottom,
				pTan4 ? "" : "(none) ", pTan4 ? pTan4[0] : 0.0f, pTan4 ? pTan4[1] : 0.0f, pTan4 ? pTan4[2] : 0.0f, pTan4 ? pTan4[3] : 0.0f);
	}
	// NOTHING TO DRAW WITH. The engine frees and reloads this DLL on every
	// focus loss and gain; a pass that arrives between the teardown and the
	// rebuild has no device and no buffers, and every call below would read
	// through a null. Refusing the pass leaves the compositor showing the
	// last frame, which is what it does during a reload anyway.
	if (!g_pDev || !g_pCtx)
	{
		static unsigned long s_nNoDev = 0;
		if (++s_nNoDev <= 8)
			Log("R3D: a world pass arrived with no D3D device - the renderer is"
				" between a teardown and a rebuild; the pass was refused (%lu)",
				s_nNoDev);
		return;
	}

	// Timed, so the stall detector can say how much of a long frame was ours.
	// And SPLIT: which part of a pass costs what, averaged over 900 passes.
	// The headset's frame period is 11.1 ms and the HQ level was measured at
	// 10.5-11 ms of work per frame, which is where the repeated frames come
	// from; nothing said which part to make cheaper.
	PhaseMark(-1);
	struct WorldPassTimer
	{
		LARGE_INTEGER q0;
		WorldPassTimer() { QueryPerformanceCounter(&q0); }
		~WorldPassTimer()
		{
			LARGE_INTEGER q1, f;
			QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&f);
			if (f.QuadPart)
				R3D_AddWorldMs((double)(q1.QuadPart - q0.QuadPart) * 1000.0
							   / (double)f.QuadPart);
		}
	} _worldPassTimer;
	R3D_PHASE("starting a world pass");
	// The world, its sky and its glass stay out of a full-card folder; the
	// card's own models and sprites are what the pass is for.
	const bool bNoWorld = (bInterface && g_bInterfaceOnly);
	{
		// ONE CLOCK PER FRAME, NOT PER PASS - and this is a VR bug that no
		// desktop capture could ever show.
		//
		// Stereo here is render-then-copy: the world pass runs TWICE per
		// frame, once per eye. This clock drives every animated texture
		// (BatchSRV -> SprAnim_SRV picks the frame by time), so sampling it
		// inside the pass gave the left eye one animation frame and the
		// right eye the NEXT one. On a monitor that is invisible - you only
		// ever see one of them. In a headset each eye is shown a different
		// picture of the same surface, which is binocular rivalry, and it
		// reads as a flicker or a shimmer that will not hold still.
		//
		// the canopy and the water pool in
		// Morocco both flickered. Both are animated .spr surfaces. Proved by
		// forcing VRIPD to 0, where the two eyes must be pixel-identical and
		// instead differed on 62% of the image in a steam-filled room.
		//
		// Keyed on the client's own frame number, which both passes of a
		// frame share. The wall-clock fallback keeps animation running in
		// states where no models are published (the menu), and 0.1 s is far
		// longer than a frame so it can never fire between two eyes.
		LARGE_INTEGER qn, qfr; QueryPerformanceCounter(&qn); QueryPerformanceFrequency(&qfr);
		const double fWall = qfr.QuadPart ? (double)qn.QuadPart / (double)qfr.QuadPart : 0.0;
		static uint32_t s_nClockFrame = 0xFFFFFFFFu;
		static double   s_fClockWall  = 0.0;
		if (g_Models.nFrame != s_nClockFrame || fWall - s_fClockWall > 0.1)
		{
			s_nClockFrame = g_Models.nFrame;
			s_fClockWall  = fWall;
			g_fNowSec     = fWall;
		}
	}
	// A SCENE WITH NO WORLD IS STILL A SCENE.
	//
	// This returned on `!g_pVB` - no world vertex buffer, nothing to do - and
	// that is why the main menu is black behind its text. The menu IS a 3D
	// scene: CInterfaceMgr::UpdateInterfaceSFX builds an object list and calls
	// RenderObjects(hInterfaceCamera, ...), and the client now publishes those
	// models. They arrived here every frame and the function turned round at the
	// door, because the world they have no part of does not exist.
	//
	// So: proceed when there is a world OR something published to draw.
	if (!g_pCtx || !g_pRTV || !g_pDSV) return;
	if (!g_pVB && !(g_bHaveModels && g_Models.nCount)) return;
	if (nRight <= nLeft || nBottom <= nTop) return;
	// The per-pass budget for pulling a model skin off disk. See the skin
	// block below: bounded work, not work that grows with the scene.
	g_nSkinTryThisFrame = 0;
	// The same per-pass budget for pulling a SPRITE's picture off disk. See
	// ResolveTexObjByName: a sprite that names a mounted file the cache has
	// never seen is a rain splash, not a bad address.
	g_nSprFileThisFrame = 0;
	g_nButeTryThisFrame = 0;

	// ---- A MENU OVER A LOADED LEVEL DRAWS IT FROM THE PLAYER'S EYE --------
	//
	// The engine has two cameras. The world is rendered through the player's;
	// the interface scene - the menu backdrop - is rendered through a camera
	// that sits at the WORLD ORIGIN, orientation identity, and that is correct
	// for the main menu, whose models are placed around the origin to suit it.
	//
	// The in-game PAUSE menu is the same folder state and the same interface
	// camera, but this time a level IS loaded, so the whole city was drawn from
	// a point above its own origin: rooftops at odd angles, a menu prop hanging
	// in the middle of them, and the menu text over the lot. The tester described
	// the world behind the menu as fragmented rather than merely doubled, and
	// that is exactly what it is - not a stereo fault at all.
	//
	// So: an interface scene with a world loaded is drawn from THE LAST POSE
	// THAT EYE SAW THE WORLD FROM. Per eye, because the two differ by the IPD
	// and using one for both would flatten the backdrop to mono. The frustum
	// goes with it - a menu drawn at the interface camera's 90 degrees over a
	// world that was drawn at the headset's would jump on the way in.
	//
	// The main menu is untouched: there is no world, so nothing is substituted
	// and Cate still stands where the interface camera can see her.
	static float s_fWPos[2][3], s_fWQuat[2][4], s_fWTan[2][4];
	static float s_fWFov[2][2], s_fWNear[2], s_fWFar[2];
	static int   s_bWCam[2] = { 0, 0 }, s_bWTan[2] = { 0, 0 };
	const int ie = (nEye == 1) ? 1 : 0;

	if (g_bScopePass)
	{
		// The scope's own camera, used as given: nothing stashed, nothing
		// substituted, the eyes' records untouched.
	}
	else if (!bInterface)
	{
		// A world scene: the pause menu is not up, so its lock is released.
		R2D_UpdateWorldLock(ie, nullptr, 0);
		if (pPos && pQuat)
		{
			memcpy(s_fWPos[ie], pPos, sizeof s_fWPos[ie]);
			memcpy(s_fWQuat[ie], pQuat, sizeof s_fWQuat[ie]);
			s_fWFov[ie][0] = fFovX; s_fWFov[ie][1] = fFovY;
			s_fWNear[ie] = fNear;   s_fWFar[ie] = fFar;
			s_bWCam[ie] = 1;
			s_bWTan[ie] = 0;
			if (pTan4)
			{
				memcpy(s_fWTan[ie], pTan4, sizeof s_fWTan[ie]);
				s_bWTan[ie] = 1;
			}
		}
	}
	else if (g_bInterfaceOnly)
	{
		// A FULL-CARD FOLDER OVER A LOADED LEVEL: the interface camera as the
		// engine gave it, at the origin, and no world drawn under it - see
		// bNoWorld below. The 12 September headset batch: "MISSION STATUS" as
		// text over the Morocco street where retail shows the orange card.
		//
		// AND WITH THE INTERFACE CAMERA'S OWN FIELD, NOT THE EYE'S. Once a
		// level has been played the eye order is latched, so every scene -
		// this one included - arrives with that eye's asymmetric headset
		// frustum. Applied to the card it narrowed the fitted 4:3 view to the
		// eye's and pushed each half's optical centre outward: the card was
		// magnified, cut at the seam, and sat ~290 px apart between the halves
		// while the 2D text stayed put (desk, failure screen and Load folder).
		// The main menu at startup has no eye latched and never met this.
		pTan4 = nullptr;
		static long s_nSaidCard = 0;
		if (s_nSaidCard < 2)
		{
			++s_nSaidCard;
			Log("  R3D: FULL-CARD FOLDER OVER A LOADED LEVEL - eye %d drawn"
				" from the interface camera and its own field, the world left out", ie);
		}
	}
	else if (g_pVB && (s_bWCam[ie] || s_bWCam[0]))
	{
		const int iu = s_bWCam[ie] ? ie : 0;
		pPos   = s_fWPos[iu];
		// THE ROTATION IS THE CLIENT'S, NOT THE REMEMBERED ONE - when the
		// client is turning the interface camera with the head (VRPauseLook),
		// the scene arrives already carrying it, and overwriting it here is
		// exactly what made the backdrop a frozen snapshot. Fall back on the
		// remembered rotation when the scene's is identity, which is what an
		// untouched interface camera reports and what the main menu wants.
		{
			const bool bSceneTurned =
				pQuat && (fabsf(pQuat[0]) > 0.0005f || fabsf(pQuat[1]) > 0.0005f
					   || fabsf(pQuat[2]) > 0.0005f
					   || fabsf(fabsf(pQuat[3]) - 1.0f) > 0.0005f);
			if (!bSceneTurned) pQuat = s_fWQuat[iu];
		}
		fFovX  = s_fWFov[iu][0];
		fFovY  = s_fWFov[iu][1];
		fNear  = s_fWNear[iu];
		fFar   = s_fWFar[iu];
		pTan4  = s_bWTan[iu] ? s_fWTan[iu] : nullptr;

		// The pause menu's world-lock, fed from HERE because this is the only
		// place the interface scene's frustum is known.
		R2D_UpdateWorldLock(ie, pTan4, 1);

		static long s_nSaidPause = 0;
		if (s_nSaidPause < 2)
		{
			++s_nSaidPause;
			Log("  R3D: MENU OVER A LOADED LEVEL - eye %d drawn from the last"
				" world pose (%.1f %.1f %.1f) instead of the interface camera"
				" at the origin", ie, pPos[0], pPos[1], pPos[2]);
		}
	}

	// Remembered before anything can fail below, so a probe fired from the
	// periodic report uses the pose the picture was actually drawn from.
	if (pPos && pQuat)
	{
		memcpy(g_fLastPos, pPos, sizeof g_fLastPos);
		memcpy(g_fLastQuat, pQuat, sizeof g_fLastQuat);
		g_bHaveCam = 1;
		if (!g_bScopePass)
		{
			memcpy(g_fEyeCamPos, pPos, sizeof g_fEyeCamPos);
			memcpy(g_fEyeCamQuat, pQuat, sizeof g_fEyeCamQuat);
			g_bHaveEyeCam = 1;
		}

		// Where the player IS, every two seconds, for the whole run. The
		// existing report prints the camera five times in the opening
		// frames and then never again, which cannot answer the one
		// question movement work asks: did the player actually go anywhere. The
		// distance from the first sample is printed with it so that a
		// walk is one number, not a subtraction across two log lines.
		static DWORD s_dwNextCam = 0;
		static float s_fFirst[3] = { 0.0f, 0.0f, 0.0f };
		static int   s_bHaveFirst = 0;
		const DWORD  dwNow = GetTickCount();
		if (!s_bHaveFirst) { s_bHaveFirst = 1; memcpy(s_fFirst, pPos, sizeof s_fFirst); }
		// Count every world pass, so the line below can report a REAL frame
		// rate during play rather than an average over a run that was mostly
		// spent at a spawn point with nothing in view. A renderer that is fast
		// at the quick save and slow in combat looks identical in a total.
		static long  s_nPassCount = 0;
		static long  s_nFrameMark = 0;
		static DWORD s_dwLastFps = 0;
		static float s_fFps = 0.0f;
		static float s_fPerFrame = 0.0f;
		++s_nPassCount;
		if (dwNow >= s_dwNextCam)
		{
			if (s_dwLastFps)
			{
				const DWORD dwEl = dwNow - s_dwLastFps;
				// Count FRAMES, not world passes. This used to multiply the
				// pass count by 0.5 on the grounds that there are two passes
				// per frame, one per eye - which is true only when the eye
				// pairing is actually working. Under +StubNativeFrustum 0 the
				// world draws once per frame, so the line reported 45.0 while
				// the engine was presenting a measured 90.0, and the frame cap
				// was blamed for a number the instrument had invented.
				const long nFr = g_nFrames - s_nFrameMark;
				if (dwEl && nFr > 0)
					s_fFps = (float)nFr * 1000.0f / (float)dwEl;
				// Passes per frame: 2.0 is stereo, 1.0 is mono. Printed so the
				// configuration is visible in the same line as the rate.
				s_fPerFrame = nFr ? (float)s_nPassCount / (float)nFr : 0.0f;
			}
			s_dwLastFps = dwNow;
			s_nFrameMark = g_nFrames;
			s_nPassCount = 0;
			s_dwNextCam = dwNow + 2000;
			g_bCamSaidThisFrame = 1;
			const float dx = pPos[0] - s_fFirst[0];
			const float dy = pPos[1] - s_fFirst[1];
			const float dz = pPos[2] - s_fFirst[2];
			{
				// The FORWARD vector too. Two poses that differ only in
				// orientation log the same position, so "the camera did not
				// move" and "the camera did not turn" were indistinguishable -
				// which is why --static and --rstick both looked inert.
				float rr2[3], uu2[3], ff2[3];
				QuatBasis(g_fLastQuat, rr2, uu2, ff2);
				Log("  R3D CAM: (%.1f %.1f %.1f) fwd (%.3f %.3f %.3f)"
					"  moved %.1f  | nearest model az %.0f at %.0f units"
					"  | %.1f FPS (%.2f world passes/frame)%s",
					pPos[0], pPos[1], pPos[2],
					ff2[0], ff2[1], ff2[2],
					(float)sqrt(dx*dx + dy*dy + dz*dz),
					g_fNearAz, (g_fNearDist > 1e29f) ? -1.0f : g_fNearDist,
					s_fFps, s_fPerFrame,
					(s_fFps > 1.0f && s_fFps < 45.0f)
						? "   <- SLOW: the GAME will run fast and hit hard"
					: (s_fPerFrame > 0.1f && s_fPerFrame < 1.5f)
						? "   <- MONO: one world pass per frame, so this is NOT"
						  " a stereo picture. Cause unknown - his 4 Sept 12:55"
						  " session did this and no desk arm has reproduced it"
						  " yet. Capture the whole log." : "");
			}
		}
	}

	// Per eye, not per frame. See the note in the header.
	//
	// The clear value is the FAR end of whichever mapping is in use: 1 for
	// conventional, 0 for reversed. Getting this wrong and the comparison
	// right draws nothing at all, which is a very loud failure - deliberately
	// derived from the same flag rather than written down twice.
	g_pCtx->ClearDepthStencilView((g_bScopePass && g_pScopeDSV) ? g_pScopeDSV : g_pDSV, D3D11_CLEAR_DEPTH,
								  g_bRevZ ? 0.0f : 1.0f, 0);

	float mvp[16];
	if (g_bTest)
	{
		memset(mvp, 0, sizeof mvp);
		mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f;	// identity: pass through
	}
	else
	{
		const float fNearUse = (g_fNearOverride > 0.0f) ? g_fNearOverride
							 : (fNear > 0.0f ? fNear : 1.0f);
		const float fFarUse = (g_fFarOverride > 0.0f) ? g_fFarOverride
							: (fFar > fNear ? fFar : 100000.0f);
		// THE MENU ZOOM, applied to the tangents rather than after the fact.
		// One call site, so there is one place for it. The world is untouched
		// because the gate is the flattened-quad case, where no world is drawn
		// at all - see the note at g_fMenuZoom3D.
		float fTanZoom[4];
		const float* pTanUse = pTan4;
		float fZoomUse = g_fMenuZoom3D;
		if (g_bMenuZoomOn && g_fMenuBand3D > 0.0f)
		{
			const float fEw = (float)(nRight - nLeft), fEh = (float)(nBottom - nTop);
			if (fEw > 0.0f && fEh > 0.0f)
			{
				const float fHb = (fEw / g_fMenuBand3D < fEh) ? fEw / g_fMenuBand3D : fEh;
				const float fBand = fHb / fEh;					// rows the panel shows
				float tu, td;
				if (pTan4) { tu = pTan4[2]; td = pTan4[3]; }
				else { tu = tanf(fFovY * 0.5f); td = -tu; }
				const float fHalfV = (tu - td) * 0.5f;				// the pass's own
				const float fAuth = tanf(((fFovY > 0.1f) ? fFovY : 1.309f) * 0.5f);
				if (fHalfV > 0.0f && fAuth > 0.0f)
				{
					float fZ = (fHalfV * fBand) / fAuth;
					// ...BUT NO WIDER THAN 100 DEGREES. The authored view is
					// 90 wide; a 16:9 band at the full 75-degree height shows
					// 107, and the interface scene parks props just off the
					// authored edge - a second green box sat at 104 degrees.
					// The height gives up a few degrees instead; the logo and
					// Cate's head are well inside.
					const float fTanHBand = fAuth * g_fMenuBand3D;
					const float fTanHMax  = tanf(50.0f * 3.14159265f / 180.0f);
					// AND THE 2D LAYER GROWS BY THE SAME STEP, or the text sits
					// inside boxes that have moved away from the centre: the
					// help line above its blue box, the menu items right of
					// centre in the lime one. The 2D fit
					// spans the band with the authored height; this is the
					// extra the cap adds on top of that.
					const float fExtra = (fTanHBand > fTanHMax) ? fTanHBand / fTanHMax : 1.0f;
					if (fTanHBand > fTanHMax) fZ *= fExtra;
					fZoomUse *= fZ;
				}
			}
		}
		// THE 2D LAYER FOLLOWS THIS ZOOM. Its 4:3 core is fitted to the eye's
		// width - the interface camera's 90 degrees - and this zoom is what
		// the 3D scene's 90 degrees actually spans of the eye (0.84 on the
		// Quest 3 at 3840x2076: the band widens the view by 1.195). Fitted
		// without it the menu text overran its lime box; fitted with only the
		// cap step it overshot the other way (13 September, three tries).
		R2D_SetMenuZoomExtra((g_bMenuZoomOn && fZoomUse > 0.0f) ? fZoomUse : 1.0f);
		if (g_bMenuZoomOn && fZoomUse > 0.0f && fZoomUse != 1.0f)
		{
			// [t0,t1] for each axis, from the asymmetric tangents when we have
			// them and from the scene's own full angles when we do not.
			float tl, tr, tu, td;
			if (pTan4) { tl = pTan4[0]; tr = pTan4[1]; tu = pTan4[2]; td = pTan4[3]; }
			else
			{
				const float tx = tanf(fFovX * 0.5f);
				const float ty = tanf(fFovY * 0.5f);
				tl = -tx; tr = tx; tu = ty; td = -ty;
			}
			// ndc n maps to t0 + (n+1)*(t1-t0)/2. The new edges are the old
			// tangents at n = anchor -/+ 1/zoom.
			const float fHalf = 1.0f / fZoomUse;
			const float nx0 = g_fMenuAnchorX3D - fHalf;
			const float nx1 = g_fMenuAnchorX3D + fHalf;
			const float ny0 = g_fMenuAnchorY3D - fHalf;
			const float ny1 = g_fMenuAnchorY3D + fHalf;
			fTanZoom[0] = tl + (nx0 + 1.0f) * (tr - tl) * 0.5f;
			fTanZoom[1] = tl + (nx1 + 1.0f) * (tr - tl) * 0.5f;
			fTanZoom[2] = td + (ny1 + 1.0f) * (tu - td) * 0.5f;
			fTanZoom[3] = td + (ny0 + 1.0f) * (tu - td) * 0.5f;
			pTanUse = fTanZoom;
		}
		ViewProj(pPos, pQuat, fFovX, fFovY, fNearUse, fFarUse, mvp, pTanUse);
	}

	// A draw that executes and shows nothing has two possible causes - the
	// pipeline or the matrix - and they are separable without a picture. Push
	// a sample of the world's own vertices through the same matrix on the CPU
	// and count how many land inside the clip volume. If none do, the matrix is
	// wrong and no amount of staring at the render state will help.
	static long s_nReport = 0, s_nReportEye = 0;
	// Two budgets. The eye cannot be latched until a whole frame has gone
	// by, so a shared counter spends all three reports on the symmetric
	// fallback and never prints the case the run was started to look at.
	const bool bReport = (pTan4 ? (s_nReportEye < 2) : (s_nReport < 3));
	if (bReport && g_pSampleVerts && g_nSampleVerts > 0)
	{
		if (pTan4) ++s_nReportEye; else ++s_nReport;
		int nFront = 0, nInside = 0;
		float fMinZ = 1e30f, fMaxZ = -1e30f;
		for (int i = 0; i < g_nSampleVerts; ++i)
		{
			const float* v = &g_pSampleVerts[i * 3];
			float c[4];
			for (int j = 0; j < 4; ++j)
				c[j] = v[0]*mvp[0*4+j] + v[1]*mvp[1*4+j] + v[2]*mvp[2*4+j] + mvp[3*4+j];
			if (c[3] > 0.0f)
			{
				++nFront;
				if (c[2] / c[3] < fMinZ) fMinZ = c[2] / c[3];
				if (c[2] / c[3] > fMaxZ) fMaxZ = c[2] / c[3];
				if (c[0] >= -c[3] && c[0] <= c[3] && c[1] >= -c[3] && c[1] <= c[3]
					&& c[2] >= 0.0f && c[2] <= c[3]) ++nInside;
			}
		}
		Log("");
		Log("  R3D: camera (%.1f %.1f %.1f) quat (%.4f %.4f %.4f %.4f)",
			pPos[0], pPos[1], pPos[2], pQuat[0], pQuat[1], pQuat[2], pQuat[3]);
		Log("       fov %.4f x %.4f rad (%.1f x %.1f deg), near %.4f far %.1f",
			fFovX, fFovY, fFovX * 57.2958f, fFovY * 57.2958f, fNear, fFar);
		{
			const float zn = fNear > 0.0f ? fNear : 1.0f;
			const float zf = fFar > fNear ? fFar : 100000.0f;
			if (pTan4)
			{
				char szWhat[64];
				sprintf_s(szWhat, "per-eye frustum, eye %d (%s), from the headset",
						  nEye, nEye ? "RIGHT" : "LEFT");
				CheckFrustum(pTan4[0], pTan4[1], pTan4[2], pTan4[3], zn, zf, szWhat);
				// A projection maps its frustum onto the whole viewport, so a
				// viewport of a different shape stretches the result. This is
				// not a matrix error and cannot be fixed in the matrix - it is
				// the render resolution, and it has looked like a rendering bug
				// three times in this project already.
				const float fFr = (pTan4[1] - pTan4[0]) / (pTan4[2] - pTan4[3]);
				const float fVp = (float)(nRight - nLeft)
								/ (float)(nBottom - nTop);
				Log("       frustum aspect %.3f, viewport aspect %.3f  %s",
					fFr, fVp, (fabsf(fFr - fVp) < 0.02f)
						? "- they agree, so pixels are square"
						: "<- SHAPE MISMATCH: the image is stretched to fit."
						  " Use tools/set-res.ps1, not the display menu");
			}
			else CheckFrustum(-tanf(fFovX * 0.5f), tanf(fFovX * 0.5f),
							  tanf(fFovY * 0.5f), -tanf(fFovY * 0.5f), zn, zf,
							  "symmetric frustum, from the scene description");
		}
		Log("       viewport %d,%d..%d,%d   target %dx%d",
			nLeft, nTop, nRight, nBottom, g_nTargetW, g_nTargetH);
		Log("       of %d sampled world vertices: %d in front of the camera,"
			" %d inside the clip volume", g_nSampleVerts, nFront, nInside);
		if (nFront) Log("       their ndc z spans %.4f .. %.4f", fMinZ, fMaxZ);
		Log("       %s", nInside ? "the matrix puts geometry on screen"
								 : "NOTHING is on screen - the matrix is wrong");
	}

	D3D11_MAPPED_SUBRESOURCE ms{};
	if (FAILED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &ms, 0))) return;
	memcpy(ms.pData, mvp, sizeof mvp);
	memcpy(g_fLastMVP, mvp, sizeof mvp);
	{
		const float lmp[4] = { g_fLMScale, (float)g_bLMOnly, 0, g_fAlphaOut };
		g_fAlphaCutSet = 0.0f;	// this write puts lmp.z back to zero
		memcpy((char*)ms.pData + sizeof mvp, lmp, sizeof lmp);
	}
	g_pCtx->Unmap(g_pCB, 0);

	D3D11_VIEWPORT vp{};
	vp.TopLeftX = (float)nLeft; vp.TopLeftY = (float)nTop;
	vp.Width = (float)(nRight - nLeft); vp.Height = (float)(nBottom - nTop);
	vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
	g_pCtx->RSSetViewports(1, &vp);
	g_pCtx->RSSetState(g_pRS);
	if (g_pBlendOpaque)
	{
		const float kNoFactor[4] = { 0, 0, 0, 0 };
		g_pCtx->OMSetBlendState(g_pBlendOpaque, kNoFactor, 0xFFFFFFFF);
		g_bA2CBound = false;
	}

	// THE MULTISAMPLED TARGET WHEN THERE IS ONE. Both eyes draw into it at
	// their own viewports and it is resolved once, when the world pass ends.
	ID3D11RenderTargetView* pColour =
		(g_nMsaaMade > 1 && g_pMsaaRTV) ? g_pMsaaRTV : g_pRTV;
	if (g_bScopePass && g_pScopeRTV) pColour = g_pScopeRTV;

	// ONCE PER FRAME, NOT PER EYE - clearing per eye would erase the eye that
	// had already drawn, since both share this target at different viewports.
	// CLEARING THIS IS NOT OPTIONAL, AND FINDING THAT OUT FIXED MSAA.
	//
	// The first multisampled build drew bright sky-coloured shards through
	// Morocco's alpha-tested lattice. The theory was stale samples; the test
	// was one clear to an impossible colour, and the shards came back MAGENTA
	// exactly where they had been. So the world pass never writes those
	// pixels at all.
	//
	// WITHOUT MSAA THAT IS INVISIBLE, because the world draws straight into
	// the back buffer and those pixels quietly keep what the PREVIOUS FRAME
	// left there - which on consecutive frames of a mostly-static scene looks
	// exactly right. It is a pre-existing fault in this renderer, not one
	// multisampling introduced: the world pass does not cover every pixel and
	// has always relied on that. Multisampling gave it a fresh target with
	// nothing in it and turned a hidden fault into a visible one.
	//
	// Clearing to black each frame is the fix, and it matches what the
	// no-AA path produces on those pixels to the pixel.
	//
	// ONCE PER FRAME, NOT PER EYE - both eyes share this target at different
	// viewports, so clearing per pass would erase the eye already drawn.
	// EVERY PASS CLEARS ITS OWN VIEWPORT - AND ONLY ITS VIEWPORT.
	//
	// "Once per frame, not per eye" above was built on the belief that the
	// two eyes draw into DIFFERENT viewports of this target. For the world
	// they do not: the client renders both eyes into the LEFT half and copies
	// the first across, so the second eye draws over the first eye's pixels.
	// The world pass does not cover the sky - the sky brushes are holes - so
	// in the second eye every sky pixel kept what the FIRST eye had drawn
	// there, and with the eyes 14 degrees apart that was the hillside to the
	// right of the scene, complete with its trees, pasted across the second
	// eye's sky. In the headset, on the intro, some things such as the trees
	// showed only in the left eye and not the right. Proved at the desk: the slab stayed
	// in the second-drawn half through an eye swap, a doubled IPD and an IPD
	// of zero, and the engine's own Clear only ever reached the back buffer.
	//
	// A rect clear (D3D11.1 ClearView) keeps the menu's two halves intact,
	// since THOSE really are drawn at their own viewports in one frame. Where
	// ClearView is not available the old once-per-frame clear remains.
	if (pColour == g_pMsaaRTV)
	{
		const float kMagenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
		float kClear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
		// The fog colour, as the engine's own Clear paints the back buffer:
		// the intro's night sky is dark blue in retail, and clearing this
		// target to black then resolving over the top threw that away.
		R3D_FogClearColour(kClear);
		// +StubMsaaClear 1 makes it magenta instead: the diagnostic that
		// answered this, kept because it will answer the next one too.
		const float* pClr = (g_bMsaaClear == 1) ? kMagenta : kClear;
		// FILE SCOPE, so the teardown can release it. A QueryInterface takes
		// a reference, and this one was never given back: it holds the
		// context, the context holds the device, and a device that is not
		// destroyed keeps every mapping it owns. See ReleaseCtx1.
		extern ID3D11DeviceContext1* g_pCtx1;
		ID3D11DeviceContext1*& s_pCtx1 = g_pCtx1;
		static int s_nCtx1Asked = 0;
		if (!s_nCtx1Asked)
		{
			s_nCtx1Asked = 1;
			if (FAILED(g_pCtx->QueryInterface(__uuidof(ID3D11DeviceContext1),
											  (void**)&s_pCtx1)))
				s_pCtx1 = nullptr;
			Log("  R3D: MSAA target cleared per pass, viewport only: %s",
				s_pCtx1 ? "yes (ClearView)" : "NO - 11.1 context unavailable, once per frame");
		}
		// The WHOLE target on the first pass of a frame - the half no world
		// pass ever draws must not carry last frame's, or a menu's, pixels
		// into the resolve - and this pass's own viewport on every pass.
		if (g_bMsaaFresh) R3D_GpuFrameBegin();
		if (g_bMsaaFresh || !s_pCtx1)
			g_pCtx->ClearRenderTargetView(g_pMsaaRTV, pClr);
		else
		{
			D3D11_RECT rc = { nLeft, nTop, nRight, nBottom };
			s_pCtx1->ClearView(g_pMsaaRTV, pClr, &rc, 1);
		}
		g_bMsaaFresh = 0;
	}
	g_pCtx->OMSetRenderTargets(1, &pColour, (g_bScopePass && g_pScopeDSV) ? g_pScopeDSV : g_pDSV);
	g_pCtx->OMSetDepthStencilState((g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);

	UINT nStride = sizeof(Vtx), nOff = 0;
	ID3D11Buffer* pVB = (g_bTest && g_pTestVB) ? g_pTestVB : g_pVB;
	if (pVB) g_pCtx->IASetVertexBuffers(0, 1, &pVB, &nStride, &nOff);
	g_pCtx->IASetInputLayout(g_pIL);
	g_pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_pCtx->VSSetShader(g_pVS, nullptr, 0);
	g_pCtx->VSSetConstantBuffers(0, 1, &g_pCB);
	g_pCtx->PSSetShader(g_pPS, nullptr, 0);
	// The pixel shader reads the constant buffer too, for the lightmap
	// scale. Binding it to the vertex stage alone left lmp reading as
	// zero and multiplied every lit polygon in the level by nothing.
	g_pCtx->PSSetConstantBuffers(0, 1, &g_pCB);
	// The fog buffer, refreshed only when the level changed it. A level sets
	// it once at load; walking into a foggy shaft changes it live.
	if (g_pCBFog)
	{
		if (g_bFogDirty)
		{
			D3D11_MAPPED_SUBRESOURCE fm{};
			if (SUCCEEDED(g_pCtx->Map(g_pCBFog, 0, D3D11_MAP_WRITE_DISCARD, 0, &fm)))
			{
				const float f[8] = {
					g_fFogRGB[0], g_fFogRGB[1], g_fFogRGB[2],
					(g_bFog && g_bFogAllow) ? 1.0f : 0.0f,
					g_fFogNear, g_fFogFar, (float)g_bAlphaSharp, 0.0f };
				memcpy(fm.pData, f, sizeof f);
				g_pCtx->Unmap(g_pCBFog, 0);
				g_bFogDirty = 0;
			}
		}
		g_pCtx->PSSetConstantBuffers(1, 1, &g_pCBFog);
	}
	// b2 is bound for the whole frame; what it holds changes per run, and a
	// w of 0 means "not a model, ignore me", which is what the world, the
	// glass and the sprites all want.
	if (g_pCBMdl) g_pCtx->PSSetConstantBuffers(2, 1, &g_pCBMdl);
	// b5 and t2: the level's light grid, for world polygons with no lightmap.
	// A w of 0 in the buffer means there is none and the stand-in shade
	// applies; the view slot is left empty in that case, never a 2D white.
	if (g_pCBGrid)
	{
		if (g_bGridDirty)
		{
			D3D11_MAPPED_SUBRESOURCE gm{};
			if (SUCCEEDED(g_pCtx->Map(g_pCBGrid, 0, D3D11_MAP_WRITE_DISCARD, 0, &gm)))
			{
				memcpy(gm.pData, g_fGridCB, sizeof g_fGridCB);
				g_pCtx->Unmap(g_pCBGrid, 0);
				g_bGridDirty = 0;
			}
		}
		g_pCtx->PSSetConstantBuffers(5, 1, &g_pCBGrid);
		// The waterfall's scroll is read by the VERTEX shader.
		if (g_pCBWater)
		{
			// A PASS MUST SET ITS OWN STATE. The scroll is written per batch in the
			// translucent pass and nowhere else, so whatever the LAST batch there
			// left - the waterfall's flow, time-based - was what every batch of
			// the next pass read: in a level whose last translucent batch is a
			// water face, every texture in the world and on every model slid at
			// the waterfall's rate. The club (M04S02) and the underwater levels;
			// measured at the desk with a still camera, 21 September, and gone
			// with the flow stopped. Zero here, at the top of every pass.
			const float w0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			D3D11_MAPPED_SUBRESOURCE wm0{};
			if (SUCCEEDED(g_pCtx->Map(g_pCBWater, 0, D3D11_MAP_WRITE_DISCARD, 0, &wm0)))
			{ memcpy(wm0.pData, w0, sizeof w0); g_pCtx->Unmap(g_pCBWater, 0); }
			g_pCtx->VSSetConstantBuffers(7, 1, &g_pCBWater);
		}
	}
	// b6: the camera's frame for the environment pass, flag off.
	if (g_pCBEnv)
	{
		float er[3], eu[3], ef[3];
		QuatBasis(pQuat, er, eu, ef);
		const float t = (float)g_fNowSec;
		const float e[16] = { er[0], er[1], er[2], g_fEnvCoord, eu[0], eu[1], eu[2], 0,
							  pPos ? pPos[0] : 0.0f, pPos ? pPos[1] : 0.0f, pPos ? pPos[2] : 0.0f, 0,
							  0.0f, -g_fEnvPan * t, g_fEnvScale, 0.0f };
		D3D11_MAPPED_SUBRESOURCE em{};
		if (SUCCEEDED(g_pCtx->Map(g_pCBEnv, 0, D3D11_MAP_WRITE_DISCARD, 0, &em)))
		{ memcpy(em.pData, e, sizeof e); g_pCtx->Unmap(g_pCBEnv, 0); }
		g_pCtx->PSSetConstantBuffers(6, 1, &g_pCBEnv);
	}
	{
		ID3D11ShaderResourceView* pG = g_pLGridSRV;
		g_pCtx->PSSetShaderResources(2, 1, &pG);
	}
	if (g_pCBDyn)
	{
		// The eight nearest dynamic lights that light the world, by distance
		// from this pass's eye. Written every pass; the list changes every
		// frame anyway.
		float fBuf[17 * 4]; memset(fBuf, 0, sizeof fBuf);
		int nUse = 0;
		if (g_bDynLights && g_bHaveDynL && g_DynL.nCount)
		{
			int idx[VRLIGHT_MAX]; float dst[VRLIGHT_MAX]; int n = 0;
			for (uint32_t i = 0; i < g_DynL.nCount; ++i)
			{
				const VRLightInst& w = g_DynL.lights[i];
				if (!(w.nFlags & VRLIGHT_F_WORLD)) continue;
				const float dx = w.fPos[0] - g_fLastPos[0], dy = w.fPos[1] - g_fLastPos[1], dz = w.fPos[2] - g_fLastPos[2];
				const float d = sqrtf(dx*dx + dy*dy + dz*dz) - w.fRadius;	// how far past its reach
				idx[n] = (int)i; dst[n] = d; ++n;
			}
			for (int a = 0; a < n && nUse < 8; ++a)
			{
				int best = -1;
				for (int b = 0; b < n; ++b)
					if (idx[b] >= 0 && (best < 0 || dst[b] < dst[best])) best = b;
				if (best < 0) break;
				const VRLightInst& w = g_DynL.lights[idx[best]];
				fBuf[nUse * 4 + 0] = w.fPos[0]; fBuf[nUse * 4 + 1] = w.fPos[1];
				fBuf[nUse * 4 + 2] = w.fPos[2]; fBuf[nUse * 4 + 3] = w.fRadius;
				fBuf[32 + nUse * 4 + 0] = w.fColour[0]; fBuf[32 + nUse * 4 + 1] = w.fColour[1];
				fBuf[32 + nUse * 4 + 2] = w.fColour[2]; fBuf[32 + nUse * 4 + 3] = 1.0f;
				idx[best] = -1; ++nUse;
			}
		}
		fBuf[64] = (float)nUse;
		D3D11_MAPPED_SUBRESOURCE dm{};
		if (SUCCEEDED(g_pCtx->Map(g_pCBDyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &dm)))
		{ memcpy(dm.pData, fBuf, sizeof fBuf); g_pCtx->Unmap(g_pCBDyn, 0); }
		g_pCtx->PSSetConstantBuffers(4, 1, &g_pCBDyn);
		if (nUse) ++g_nDynWorldLit;
	}

	g_pCtx->PSSetSamplers(0, 1, &g_pSamp);
	{
		// The atlas, or white where there is none - so a level whose
		// lightmaps failed to build draws lit-flat rather than black.
		ID3D11ShaderResourceView* pLM = LM_Atlas() ? LM_Atlas() : g_pWhite;
		g_pCtx->PSSetShaderResources(1, 1, &pLM);
	}
	if (g_bTest)
	{
		g_pCtx->PSSetShaderResources(0, 1, &g_pWhite);
		g_pCtx->Draw(3, 0);
	}
	else
	{
		// Resolve the batches' textures. Only when the set has actually grown,
		// so the steady state costs nothing - and never by rebuilding
		// geometry, which is the whole point of keying batches by the engine's
		// pointer.
		if (g_Tex.size() != g_nResolvedWithTex)
		{
			g_nResolvedWithTex = g_Tex.size();
			int nGot = 0, nCut = 0;
			for (size_t b = 0; b < g_Batches.size(); ++b)
			{
				// A batch the engine never textured names its own file instead,
				// and the stage 1 cache answers by name.
				if (!g_Batches[b].pTexKey && g_Batches[b].szFile[0])
				{
					DtxInfo di{};
					if (g_Batches[b].bSky) { /* see bSkyAdd below */ }
					if (SprAnim_IsSpr(g_Batches[b].szFile))
					{
						g_Batches[b].nAnim1 = SprAnim_Get(g_pDev, g_Batches[b].szFile);
						g_Batches[b].pSRV = SprAnim_SRV(g_Batches[b].nAnim1, 0.0);
						const DtxInfo* pi = SprAnim_Info(g_Batches[b].nAnim1);
						if (pi) di = *pi;
						{
							// AND WHERE THEY ARE, so a frozen sheet can be told
							// from a sheet that is somewhere else (13 September:
							// the HQ waterfall in view did not animate).
							char szBox[128] = "";
							if (b < g_BatchBox.size())
								sprintf_s(szBox, "  bounds %.0f %.0f %.0f .. %.0f %.0f %.0f, %u verts",
									g_BatchBox[b].mn[0], g_BatchBox[b].mn[1], g_BatchBox[b].mn[2],
									g_BatchBox[b].mx[0], g_BatchBox[b].mx[1], g_BatchBox[b].mx[2],
									(unsigned)g_Batches[b].nCount);
							Log("  R3D: world polygons wear the SPRITE %s - %s%s",
								g_Batches[b].szFile, g_Batches[b].pSRV ? "frames loaded" : "NOT FOUND", szBox);
						}
					}
					else
					g_Batches[b].pSRV = Dtx_Get(g_pDev, g_Batches[b].szFile, &di);
					// A SKY BATCH FROM THE FILE says what it got: the factory's
					// sky drew as two flat colours with every face BOUND
					// (22 September), so the picture behind the binding is asked.
					if (g_Batches[b].bSky)
						Log("  R3D SKY TEX: '%s' -> %s %ux%u, %u mips, format %u, zero-alpha %d, %u verts in the batch",
							g_Batches[b].szFile, g_Batches[b].pSRV ? "loaded" : "NOT LOADED",
							di.nWidth, di.nHeight, di.nMips, di.nFormat, (int)di.bZeroAlpha, (unsigned)g_Batches[b].nCount);
					// THE SAME RULE as every other route: the file's own
					// declaration first, the measurement only when it is silent.
					g_Batches[b].fCutRef = (di.fAlphaRef > 0.0f)
						? di.fAlphaRef / 255.0f
						: ((di.fCut > 0.005f && di.fCut < 0.95f
							&& di.fBimodal > 0.60f) ? 0.5f : 0.0f);
					g_Batches[b].bCut = (g_Batches[b].fCutRef > 0.0f);
					// NEVER ON THE BSP (pSub 0). Retail cannot blend a BSP polygon -
					// there is no object to carry an alpha - so the HQ's white dome
					// lights (Gl0001) and ceiling light panels (BLt0007/0018), all
					// flagged 0x80 with graded alpha, draw OPAQUE there: a solid
					// white disc, a solid glowing panel. Blending them at the
					// texture's 30% gave grey facets and a see-through panel
					// (19 Sep). Doors and TranslucentWorldModels keep the rule.
					g_Batches[b].bGraded = ((di.nFlags & 0x80u) && di.fBimodal < 0.05f)
										&& g_Batches[b].pSub != 0;
					g_Batches[b].pEnvSRV = g_bEnvMap ? EnvSRVForName(g_Batches[b].szFile) : nullptr;
					g_Batches[b].bEnvPan = IsWaterName(g_Batches[b].szFile);
					// A DARK PICTURE WITH NO ALPHA, on a NAMED sky object, is
					// a glow. bSkyBlend is the test for "named": the walk sets
					// it for every SkyPointer model and leaves it clear for the
					// SkyBox itself. Without that half the rule would catch a
					// skybox FACE - TEX\SKY\SKY0050.DTX is a night face with no
					// alpha and a mean of 0.170 - and drawing a skybox face
					// additively turns the sky transparent. Measured over all 21
					// sky textures the levels name before this shipped.
					// A sky LAYER wearing a sheet with no alpha channel can only
					// have been meant to add (or chromakey, which adds nothing
					// where it is black); the brightness test that used to sit
					// here excluded nothing real and is gone.
					g_Batches[b].bSkyAdd = g_Batches[b].bSky && g_Batches[b].bSkyBlend
						&& di.bZeroAlpha;
					if (g_Batches[b].pSRV) { ++nGot; if (g_Batches[b].bCut) ++nCut; }
					continue;
				}
				const int i = TextureFor(g_Batches[b].pTexKey);
				g_Batches[b].pSRV = (i >= 0) ? g_Tex[i].pSRV : nullptr;
				g_Batches[b].nAnim1 = (i >= 0) ? g_Tex[i].nAnim1 : 0;
				g_Batches[b].fCutRef = (i >= 0) ? g_Tex[i].fCutRef : 0.0f;
				g_Batches[b].bCut = (g_Batches[b].fCutRef > 0.0f);
				g_Batches[b].bGraded = (i >= 0) && g_Tex[i].bGraded
									&& g_Batches[b].pSub != 0;	// never on the BSP, see above
				g_Batches[b].pEnvSRV = (g_bEnvMap && i >= 0 && g_Tex[i].szName[0])
					? EnvSRVForName(g_Tex[i].szName) : nullptr;
				g_Batches[b].bEnvPan = (i >= 0) && IsWaterName(g_Tex[i].szName);
				g_Batches[b].bSkyAdd = g_Batches[b].bSky && g_Batches[b].bSkyBlend
					&& (i >= 0) && g_Tex[i].bZeroAlpha;
				if (g_Batches[b].pSRV) { ++nGot; if (g_Batches[b].bCut) ++nCut; }
				// WHY a texture is or is not a cut-out. "6 of 134" cannot say
				// whether the one you are looking at was measured and rejected
				// or never measured at all, and those need opposite fixes.
				if (g_bCutProbe && i >= 0)
					Log("    CUT? %-28s %5.1f%% clear  %s", g_Tex[i].szName,
						100.0f * g_Tex[i].fCut,
						g_Tex[i].bCut ? "CUT-OUT" : "solid");
			}
			// EVERY texture, not only the ones a world batch reached. Model
			// skins never appear above - a tree's branches are a MODEL - so a
			// table listing only batch textures cannot say why the foliage is
			// solid. The two numbers that decide it are printed with each.
			if (g_bCutProbe)
			{
				Log("    ---- every texture known, and why it is or is not a cut-out ----");
				for (size_t t = 0; t < g_Tex.size(); ++t)
				{
					// THE KEY IS PART OF THE TABLE. An entry is looked up by the
					// engine's texture-object ADDRESS, so a table printing only
					// names cannot answer "which entry does this skin slot land
					// on, and is that entry still that picture". The name the
					// engine object holds RIGHT NOW is printed beside the name
					// the entry was built with; when those disagree, the address
					// has been recycled under us.
					char szLive[80];
					const bool bLiveName = TexNameOf(g_Tex[t].pTex, szLive,
									     sizeof szLive);
					Log("    TEX %2u key %08X %-30s %4.0fx%-4.0f %5.1f%% clear"
						"  %5.1f%% bimodal  %s%s%s",
						(unsigned)t, g_Tex[t].pTex,
						g_Tex[t].szName[0] ? g_Tex[t].szName : "(no name recovered)",
						g_Tex[t].fW, g_Tex[t].fH, 100.0f * g_Tex[t].fCut,
						100.0f * g_Tex[t].fBi,
						g_Tex[t].bCut ? "CUT-OUT" : "solid",
						bLiveName ? "  ENGINE NOW SAYS " : "",
						bLiveName ? (_stricmp(szLive, g_Tex[t].szName)
												? szLive : "(same)") : "");
				}
			}
			// EVERY BATCH BY NAME AND PLACE, when asked (+StubBatchList 1):
			// what a polygon in view actually wears. 13 September: the HQ
			// waterfall was believed to be a sprite that sits elsewhere.
			if (g_bBatchList && g_BatchBox.size() == g_Batches.size())
				for (size_t b = 0; b < g_Batches.size(); ++b)
					Log("  R3D BATCH %3u: %-40s %5u verts  bounds %.0f %.0f %.0f .. %.0f %.0f %.0f%s%s%s",
						(unsigned)b, g_Batches[b].szFile[0] ? g_Batches[b].szFile : "(engine texture)",
						(unsigned)g_Batches[b].nCount,
						g_BatchBox[b].mn[0], g_BatchBox[b].mn[1], g_BatchBox[b].mn[2],
						g_BatchBox[b].mx[0], g_BatchBox[b].mx[1], g_BatchBox[b].mx[2],
						g_Batches[b].pEnvSRV ? "  ENVMAP" : "", g_Batches[b].bGraded ? "  graded" : "",
						g_Batches[b].nAnim1 ? "  sprite" : "");
			Log("  R3D: resolved %d of %u batches to a texture (%u known,"
				" %ld texture objects recycled by the engine so far)"
				" | %d are CUT-OUTS and are alpha tested%s",
				nGot, (unsigned)g_Batches.size(), (unsigned)g_Tex.size(),
				g_nTexRecycled, nCut,
				nCut ? "" : "  <- no foliage, grilles or wires in this level,"
							" or the cut-out test is not firing");
		}

		// ---- THE BACKDROP, FIRST AND WITHOUT DEPTH ---------------------
		R3D_PHASE("the skybox backdrop");
		//
		// The camera is put at the skybox's own centre - the panorama is
		// painted on the inside of a box and only lines up from there - by
		// PRE-TRANSLATING every vertex by (camera - centre). The shader takes a
		// row-vector position, so pos * (T . mvp) is the same as (pos + d) *
		// mvp, and T . mvp is mvp with d's contribution folded into its last
		// row. No shader change, no second vertex buffer.
		//
		// Depth writes are OFF and the test is disabled, so the backdrop can
		// never occlude anything and everything drawn afterwards covers it.
		// The level's sky BRUSHES are holes - their surfaces wear Sky.dtx and
		// the marker rule already drops them - and this is what shows through.
		// NO SKY FROM INSIDE A FOGGED CONTAINER. A water volume's fog starts
		// behind the camera (FogNearZ -100 on the Dive's sea, -1500 on its
		// swim volume) and ends within 1000 units: everything past that is
		// the fog colour, and the clear already is. The sky pass then painted
		// its white-blue panorama over that clear wherever no polygon reached,
		// so the cutscene's underwater shots were white with blue patches of
		// seabed (headset and desk, 22 September). Retail's water is murk to
		// the horizon; so is this now. World fog, whose near plane is ahead
		// of the camera, keeps its sky.
		// ...BUT ONLY A SHORT FOG. The 103-level sweep of 22 September hid the
		// sky in six levels whose WORLD fog also starts behind the camera - the
		// intro forest (near -500, far 2000), the alpine levels (-150, 2500 and
		// 3500), the storm (5000): those keep their night sky. A water volume's
		// fog ends within 1000 units (800 and 1000 on the Dive's two). The line
		// is the far plane, not the near one.
		const bool bInFoggedContainer = g_bFog && g_bFogAllow && g_fFogNear < 0.0f && g_fFogFar < 1500.0f;
		{
			static int s_nSaidMurk = 0;
			if (bInFoggedContainer && s_nSaidMurk++ < 2)
				Log("  R3D SKY: not drawn - inside a fogged container (fog near %.0f far %.0f), the clear is the murk", g_fFogNear, g_fFogFar);
		}
		if (g_bHaveSky && g_bSkyBox && g_pDSSNoDepth && g_bHaveCam && !bNoWorld && !bInFoggedContainer)
		{
			float skymvp[16];
			memcpy(skymvp, mvp, sizeof skymvp);
			// The sky is viewed from the SkyPointer when the level has one,
			// else from the box's centre.
			const float* pSkyEye0 = (g_pWorldFile && g_pWorldFile->bSkyCam && !g_bSkyCamCentre)
								 ? g_pWorldFile->fSkyCam : g_fSkyCentre;
			// SKY PARALLAX, the engine's d3d_SetupSkyStuff: the camera sits in
			// the SkyDef's view box (pos +- SkyDims*InnerPercent) at the same
			// fraction as the player is across the world's extents - the
			// centre when the player is mid-level. Six levels allow a visible
			// drift (M02S03 +-123 units, M04S02/M06S01/M06S02/M10S03/M10S04
			// +-51); the rest are within a few units. +StubSkyParallax 0: fixed.
			float fSkyEyeP[3] = { pSkyEye0[0], pSkyEye0[1], pSkyEye0[2] };
			if (g_bSkyParallax && g_pWorldFile && g_pWorldFile->bSkyCam && !g_bSkyCamCentre && g_bWorldExt)
				for (int k = 0; k < 3; ++k)
				{
					const float h = g_pWorldFile->fSkyView[k];
					const float span = g_fWorldExtMax[k] - g_fWorldExtMin[k];
					if (h <= 0.0f || span <= 1.0f) continue;
					float f = (g_fLastPos[k] - g_fWorldExtMin[k]) / span;
					if (f < 0.0f) f = 0.0f; if (f > 1.0f) f = 1.0f;
					fSkyEyeP[k] += h * (2.0f * f - 1.0f);
				}
			const float* pSkyEye = fSkyEyeP;
			const float d[3] = { g_fLastPos[0] - pSkyEye[0],
								 g_fLastPos[1] - pSkyEye[1],
								 g_fLastPos[2] - pSkyEye[2] };
			// +StubSkyTrace N: log what the sky pass sees for N passes from
			// present 200 - the camera, the sky eye, the fog it will use.
			if (g_nSkyTrace > 0 && g_nFrames >= 200 && g_nSkyTraceSaid < g_nSkyTrace)
			{
				++g_nSkyTraceSaid;
				Log("  R3D SKY TRACE p=%ld: cam (%.1f %.1f %.1f) skyeye (%.1f %.1f %.1f) fog %d skyfog %d %.0f..%.0f fogrgb %.2f %.2f %.2f mvp[15] %.4f",
					(long)g_nFrames, g_fLastPos[0], g_fLastPos[1], g_fLastPos[2],
					pSkyEye[0], pSkyEye[1], pSkyEye[2], g_bFog, g_bSkyFog, g_fSkyFogNear, g_fSkyFogFar,
					g_fFogRGB[0], g_fFogRGB[1], g_fFogRGB[2], mvp[15]);
			}
			for (int c = 0; c < 4; ++c)
				skymvp[3 * 4 + c] += d[0] * mvp[0 * 4 + c]
								   + d[1] * mvp[1 * 4 + c]
								   + d[2] * mvp[2 * 4 + c];
			D3D11_MAPPED_SUBRESOURCE sm{};
			if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &sm, 0)))
			{
				memcpy(sm.pData, skymvp, sizeof skymvp);
				const float lmp[4] = { g_fLMScale, (float)g_bLMOnly, 0, g_fAlphaOut };
				memcpy((char*)sm.pData + sizeof skymvp, lmp, sizeof lmp);
				g_pCtx->Unmap(g_pCB, 0);
				g_fAlphaCutSet = 0.0f;
				g_pCtx->OMSetDepthStencilState(g_pDSSNoDepth, 0);
				// THE SKY'S OWN FOG, when the level asks for it: see g_bSkyFog.
				const bool bSkyFogNow = g_bFog && g_bFogAllow && g_bSkyFog
									 && g_bSkyFogAllow && g_pCBFog;
				if (bSkyFogNow)
				{
					D3D11_MAPPED_SUBRESOURCE fm{};
					if (SUCCEEDED(g_pCtx->Map(g_pCBFog, 0, D3D11_MAP_WRITE_DISCARD, 0, &fm)))
					{
						const float f[8] = {
							g_fFogRGB[0], g_fFogRGB[1], g_fFogRGB[2], 1.0f,
							g_fSkyFogNear, g_fSkyFogFar, (float)g_bAlphaSharp, 1.0f };
						memcpy(fm.pData, f, sizeof f);
						g_pCtx->Unmap(g_pCBFog, 0);
					}
					static int s_nSkyFogSaid = 0;
					if (s_nSkyFogSaid++ < 3)
						Log("  R3D SKY FOG: the sky pass is fogged %.0f..%.0f from the sky"
							" camera in (%.2f %.2f %.2f)", g_fSkyFogNear, g_fSkyFogFar,
							g_fFogRGB[0], g_fFogRGB[1], g_fFogRGB[2]);
				}
				// The box first, then what the pointers name in their order,
				// the named ones blending their alpha over what is there.
				static std::vector<size_t> skyIdx; skyIdx.clear();
				for (size_t b = 0; b < g_Batches.size(); ++b)
					if (g_Batches[b].bSky) skyIdx.push_back(b);
				std::stable_sort(skyIdx.begin(), skyIdx.end(),
					[](size_t a, size_t b) { return g_Batches[a].fSkyOrder < g_Batches[b].fSkyOrder; });
				int nSkyBlendNow = 0;		// 0 opaque, 1 alpha, 2 additive
				const float kNoF[4] = { 0, 0, 0, 0 };
				float fSkyAlphaNow = g_fAlphaOut;
				// THE SKY FOG IS PER OBJECT. The factory (M07S02) asks for sky
				// fog -1000..100 with its panorama walls 320 units from the sky
				// camera - fully fogged, on purpose, the walls are a terrain
				// panorama the author meant to sink - and sets FogDisable on
				// Clouds1, Moon and MoonFlare so those draw clear over the
				// fog. This pass fogged every layer alike, so the whole sky
				// came out one flat colour with the sun a slightly lighter
				// square (the headset and the desk, 22 September); the
				// harbour's clouds carry no such flag and were fogged rightly.
				int nSkyFogOnNow = bSkyFogNow ? 1 : 0;
				for (size_t si = 0; si < skyIdx.size(); ++si)
				{
					const size_t b = skyIdx[si];
					if (g_Batches[b].pTexKey && !g_Batches[b].pSRV
						&& !g_bDrawNoPixels) continue;
					// A SKY LAYER IS A BRUSH THE ENGINE ANIMATES. 12 September,
					// in the headset: a big black square in the sky over the Moroccan
					// desert. That square was the moon FLARE - a black sheet
					// with a glow, which the engine fades from 1.0 to 0.03 as
					// the moon passes behind cloud and hides outright at
					// times (the run's log: MoonFlare alpha 0.996, 0.208,
					// 0.027, and in the HIDDEN list). This pass drew every
					// layer at full strength whatever the engine said. Now a
					// hidden layer is skipped and a faded one is drawn at its
					// own alpha; an additive layer adds scaled by it.
					const WMBase* pSkyA = g_Batches[b].pSub ? WMBaseFor(g_Batches[b].pSub) : nullptr;
					const bool bLayer = g_Batches[b].bSkyBlend || g_Batches[b].bSkyAdd;
					// +StubSkySkip <name> leaves one named layer out: the desk's
					// way of asking which of three layers is the square.
					if (g_szSkySkip[0] && bLayer && g_Batches[b].pSub)
					{
						char szSk[64] = ""; ModelName(g_Batches[b].pSub, szSk, sizeof szSk);
						if (_stricmp(szSk, g_szSkySkip) == 0) continue;
					}
					if (bLayer && pSkyA && !g_bDrawHiddenWM && (pSkyA->nFlags & VRWORLD_F_INVIS))
					{ ++g_nHiddenWMSkipped; continue; }
					const float fSkyA = (bLayer && pSkyA) ? pSkyA->fAlpha : 1.0f;
					if (g_nSkyTrace > 0 && g_nFrames >= 200 && g_nFrames < 206)
					{
						char szT[64] = "(no brush)";
						if (g_Batches[b].pSub) ModelName(g_Batches[b].pSub, szT, sizeof szT);
						Log("  R3D SKY TRACE p=%ld   layer '%s' batch %u verts %u %s alpha %.3f flags %08X%s",
							(long)g_nFrames, szT, (unsigned)b, (unsigned)g_Batches[b].nCount,
							bLayer ? (g_Batches[b].bSkyAdd ? "ADD" : "BLEND") : "BOX",
							fSkyA, pSkyA ? (unsigned)pSkyA->nFlags : 0xFFFFFFFFu,
							(bLayer && pSkyA && (pSkyA->nFlags & VRWORLD_F_INVIS)) ? " HIDDEN" : "");
					}
					const bool bLayerFogOff = bLayer && pSkyA && (pSkyA->nFlags & VRWORLD_F_FOGOFF);
					if (bSkyFogNow)
					{
						const int nWantFog = bLayerFogOff ? 0 : 1;
						if (nWantFog != nSkyFogOnNow)
						{
							nSkyFogOnNow = nWantFog;
							// Once per world load, the first layer drawn clear: the
							// twelve SKY LAYER lines print before the census carries
							// the flag, so they can never say it.
							static uint32_t s_nFogOffSaidLoad = 0xFFFFFFFFu;
							if (!nWantFog && s_nFogOffSaidLoad != (uint32_t)g_nWorldLoads)
							{
								s_nFogOffSaidLoad = (uint32_t)g_nWorldLoads;
								char szFo[64] = "(no brush)";
								if (g_Batches[b].pSub) ModelName(g_Batches[b].pSub, szFo, sizeof szFo);
								Log("  R3D SKY FOG: layer '%s' carries FogDisable - drawn without the sky fog (first of this load)", szFo);
							}
							D3D11_MAPPED_SUBRESOURCE fm{};
							if (SUCCEEDED(g_pCtx->Map(g_pCBFog, 0, D3D11_MAP_WRITE_DISCARD, 0, &fm)))
							{
								const float f[8] = {
									g_fFogRGB[0], g_fFogRGB[1], g_fFogRGB[2], 1.0f,
									g_fSkyFogNear, g_fSkyFogFar, (float)g_bAlphaSharp, (float)nWantFog };
								memcpy(fm.pData, f, sizeof f);
								g_pCtx->Unmap(g_pCBFog, 0);
							}
						}
					}
					{
						static long s_nSkySaid = 0;
						static uint32_t s_nSkySaidLoad = 0xFFFFFFFFu;
						if (s_nSkySaidLoad != (uint32_t)g_nWorldLoads) { s_nSkySaidLoad = (uint32_t)g_nWorldLoads; s_nSkySaid = 0; }
						if (s_nSkySaid < 12)
						{
							++s_nSkySaid;
							char szL[64] = "(no brush)";
							if (g_Batches[b].pSub) ModelName(g_Batches[b].pSub, szL, sizeof szL);
							Log("  R3D SKY LAYER: '%s' drawn %s, alpha %.3f%s%s | texture %s '%s' key %08X",
								szL, g_Batches[b].bSkyAdd ? "ADDED x alpha" : (g_Batches[b].bSkyBlend ? "alpha-blended" : "opaque (the box)"),
								fSkyA, (pSkyA && (pSkyA->nFlags & VRWORLD_F_INVIS)) ? "  [engine hides it]" : "",
								bLayerFogOff ? " [FogDisable: drawn without the sky fog]" : "",
								g_Batches[b].pSRV ? "BOUND" : "NONE - drawn white", g_Batches[b].szFile, (unsigned)(uintptr_t)g_Batches[b].pTexKey);
						}
					}
					if (fSkyA != fSkyAlphaNow)
					{
						fSkyAlphaNow = fSkyA;
						D3D11_MAPPED_SUBRESOURCE sa{};
						if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &sa, 0)))
						{
							memcpy(sa.pData, skymvp, sizeof skymvp);
							const float lmpA[4] = { g_fLMScale, (float)g_bLMOnly, 0, fSkyA };
							memcpy((char*)sa.pData + sizeof skymvp, lmpA, sizeof lmpA);
							g_pCtx->Unmap(g_pCB, 0);
						}
					}
					// OPAQUE, ALPHA, OR ADDED - and the third is why the moon
					// had a black box round it. Tracked as a value rather than
					// a bool so a third answer has somewhere to live; the same
					// mistake as the mesh pass's old bAdd, noted there too.
					const int nWantSky = g_Batches[b].bSkyAdd ? 2
									   : (g_Batches[b].bSkyBlend ? 1 : 0);
					if (nWantSky != nSkyBlendNow && g_pBlendAlpha && g_pBlendOpaque)
					{
						nSkyBlendNow = nWantSky;
						// ADDED SCALED BY ALPHA, so a faded flare adds faintly
						// rather than at full strength.
						ID3D11BlendState* pBS =
							(nWantSky == 2 && g_pBlendAddMod) ? g_pBlendAddMod
						  : (nWantSky == 2 && g_pBlendAdd)    ? g_pBlendAdd
						  : (nWantSky == 1)                   ? g_pBlendAlpha
						                                      : g_pBlendOpaque;
						g_pCtx->OMSetBlendState(pBS, kNoF, 0xFFFFFFFF);
					}
					ID3D11ShaderResourceView* pS =
						BatchSRV(b);
					// +StubSkyChecker 1: a 64x64 checker in place of every sky
					// texture. A flat sky that stays flat has constant texture
					// coordinates; one that shows squares has a sampler or mip
					// problem instead (the factory, 22 September).
					if (g_bSkyChecker && g_pDev)
					{
						ID3D11ShaderResourceView*& s_pChecker = g_pSkyChecker;
						if (!s_pChecker)
						{
							static uint32_t px[64 * 64];
							for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
								px[y * 64 + x] = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFFFFFFFFu : 0xFF202020u;
							D3D11_TEXTURE2D_DESC td{}; td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1;
							td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
							td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
							D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = px; sd.SysMemPitch = 64 * 4;
							ID3D11Texture2D* pT = nullptr;
							if (SUCCEEDED(g_pDev->CreateTexture2D(&td, &sd, &pT)) && pT)
							{ g_pDev->CreateShaderResourceView(pT, nullptr, &s_pChecker); pT->Release(); }
						}
						if (s_pChecker) pS = s_pChecker;
					}
					g_pCtx->PSSetShaderResources(0, 1, &pS);
					g_pCtx->Draw(g_Batches[b].nCount, g_Batches[b].nStart);
					g_nSkyDrawn += g_Batches[b].nCount;
				}
				// THE WORLD'S FOG BACK, for the passes that follow.
				if (bSkyFogNow)
				{
					D3D11_MAPPED_SUBRESOURCE fm{};
					if (SUCCEEDED(g_pCtx->Map(g_pCBFog, 0, D3D11_MAP_WRITE_DISCARD, 0, &fm)))
					{
						const float f[8] = {
							g_fFogRGB[0], g_fFogRGB[1], g_fFogRGB[2],
							(g_bFog && g_bFogAllow) ? 1.0f : 0.0f,
							g_fFogNear, g_fFogFar, (float)g_bAlphaSharp, 0.0f };
						memcpy(fm.pData, f, sizeof f);
						g_pCtx->Unmap(g_pCBFog, 0);
					}
				}
				// AND PUT IT BACK. The world loop below tracks the blend
				// state in g_bA2CBound and would otherwise believe opaque was
				// still bound while the sky had left alpha or add there.
				if (nSkyBlendNow != 0 && g_pBlendOpaque)
				{
					g_pCtx->OMSetBlendState(g_pBlendOpaque, kNoF, 0xFFFFFFFF);
					g_bA2CBound = false;
				}
				// PUT THE STATE BACK. A pass must set its own state, and the world
				// pass below inherits both of these; leaving the sky matrix in the
				// constant buffer draws the whole level offset by the camera, and
				// leaving depth disabled draws it in submission order.
				if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &sm, 0)))
				{
					memcpy(sm.pData, mvp, sizeof mvp);
					memcpy((char*)sm.pData + sizeof mvp, lmp, sizeof lmp);
					g_pCtx->Unmap(g_pCB, 0);
				}
				g_pCtx->OMSetDepthStencilState(
					(g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);
			}
		}

		// THE MENU'S SHEET AS A BACKDROP, opaque and without depth, BEFORE the
		// models. The main menu's blue sheet has a transparent window with the
		// yellow logo MODEL 10 units behind it and a blue outline sprite
		// between; the artist's layering, and the sprite pass keeps it (sheet
		// alpha-blended, sorted by distance). But on the wide panel the window
		// is wider than the logo, and what showed through the gap was the
		// eye's own background. This paints the sheet's own colour under
		// everything first, so the gap reads as more sheet.
		if (bInterface && g_bMenuZoomOn && g_fMenuBand3D > 0.0f
			&& g_bSprites && g_bHaveSprites && g_Spr.nCount && g_bHaveCam)
			DrawMenuSheetBackdrop(fFovX, fFovY);
		// THE SKY BRUSHES, depth only: after the backdrop, before the world,
		// colour masked so the picture stays the backdrop. See g_bSkyOccluder.
		if (g_bSkyOccluder && g_pBlendNoColour && !g_bModelOnly && !bNoWorld)
		{
			bool bAnyOcc = false;
			const float kZeroF[4] = { 0, 0, 0, 0 };
			for (size_t b = 0; b < g_Batches.size(); ++b)
			{
				if (!g_Batches[b].bOcc) continue;
				if (!bAnyOcc)
				{
					g_pCtx->OMSetBlendState(g_pBlendNoColour, kZeroF, 0xFFFFFFFF);
					g_pCtx->PSSetShaderResources(0, 1, &g_pWhite);
					bAnyOcc = true;
				}
				g_pCtx->Draw(g_Batches[b].nCount, g_Batches[b].nStart);
			}
			if (bAnyOcc && g_pBlendOpaque)
			{
				g_pCtx->OMSetBlendState(g_pBlendOpaque, kZeroF, 0xFFFFFFFF);
				g_bA2CBound = false;
			}
		}

		long nSkippedNoPix = 0;
		// 1: every opaque world batch. 2: only the level's separate world
		// MODELS (doors, the car's body, lid and hood) - mode 1 also took the
		// M01S02 rooftop the player stands on, whose polygons wind the other
		// way (desk, 24 September: 23% of the view black).
		const bool bCullWorld = g_bWorldCull && g_pRSWorldCull;
		bool bCullBound = false;
		for (size_t b = 0; (g_bModelOnly || bNoWorld) ? false : b < g_Batches.size(); ++b)
		{
			if (g_Batches[b].bSky) continue;		// drawn above, without depth
			if (g_Batches[b].bOcc) continue;		// drawn above, depth only
			if (g_Batches[b].bTrans) continue;	// drawn last, blended
			if (g_Batches[b].bGraded) continue;	// glass by texture: last, blended
			{
				const WMBase* pA = g_Batches[b].pSub
								 ? WMBaseFor(g_Batches[b].pSub) : nullptr;
				if (pA && pA->fAlpha < kOpaqueAlpha) continue;	// glass, drawn last
				if (pA && !g_bDrawHiddenWM && (pA->nFlags & VRWORLD_F_INVIS))
				{ ++g_nHiddenWMSkipped; continue; }		// the engine hid it
				if (!pA && g_Batches[b].pSub && g_bHaveWorldObjs && !g_bDrawUnmatchedWM
					&& UnmatchedKindOf(g_Batches[b].pSub) == 2)
				{ ++g_nUnmatchedWMSkipped; continue; }
				// Unmatched, but the engine hid a brush of the same name - see
				// HiddenByName. Without this the canopy is drawn twice.
				if (!pA && g_Batches[b].pSub && g_bHaveWorldObjs && !g_bDrawHiddenWM
					&& HiddenByName(g_Batches[b].pSub))
				{ ++g_nHiddenWMSkipped; continue; }	// no object: removed
			}
			// Bucket 0 is the NO-TEXTURE bucket and its stand-in is the whole
			// point of it - skipping that one turned the sky black. Only a
			// batch that names a texture and failed to get its pixels is
			// dropped here.
			if (g_Batches[b].pTexKey && !g_Batches[b].pSRV && !g_bDrawNoPixels)
			{
				nSkippedNoPix += g_Batches[b].nCount;
				continue;
			}
			if (g_nSkipBatch >= 0 && (int)b == g_nSkipBatch) continue;	// +StubSkipBatch N
			if (bCullWorld)
			{
				const bool bWant = (g_bWorldCull == 1) || (g_bWorldCull == 2 && g_Batches[b].pSub != 0);
				if (bWant != bCullBound) { g_pCtx->RSSetState(bWant ? g_pRSWorldCull : g_pRS); bCullBound = bWant; }
			}
			ID3D11ShaderResourceView* pSRV =
				BatchSRV(b);
			g_pCtx->PSSetShaderResources(0, 1, &pSRV);

			// A CUT-OUT'S CLEAR TEXELS ARE HOLES, not paint. Set BEFORE the
			// moved-model matrix below, because SetAlphaCut rewrites the whole
			// constant buffer from g_fLastMVP and would put the static matrix
			// back under a door that has swung open.
			const float fCut = g_Batches[b].fCutRef;
			SetAlphaCut(fCut);
			if (g_Batches[b].bCut) g_nCutDrawn += g_Batches[b].nCount;
			// Cut-outs draw with alpha-to-coverage, everything else opaque.
			// Tracked, not set per batch: the state only changes at the
			// boundary between the two kinds.
			{
				const bool bWantA2C = (g_bA2C && g_pBlendA2C && g_Batches[b].bCut);
				if (bWantA2C != g_bA2CBound && g_pBlendOpaque)
				{
					g_bA2CBound = bWantA2C;
					const float kNoF[4] = { 0, 0, 0, 0 };
					g_pCtx->OMSetBlendState(bWantA2C ? g_pBlendA2C : g_pBlendOpaque, kNoF, 0xFFFFFFFF);
				}
			}

			// ---- A DOOR THAT HAS OPENED --------------------------------
			//
			// The mesh holds this model's vertices as the level file authored
			// them, which is correct for exactly one transform: the one the
			// object had at load. If the engine has moved it since - a door
			// swinging, a lift rising - take the vertices back into the
			// authored frame and forward into the current one:
			//
			//     v' = B(v - P0) + P1,   B = R1 * R0^T
			//
			// The shader multiplies a ROW vector, so that affine map is folded
			// into the matrix as B TRANSPOSED with the translation in the last
			// row, and the result is premultiplied onto the view-projection.
			// Only models that have actually moved pay for any of this.
			const WMBase* pWM = (g_bWorldXform && g_Batches[b].pSub)
							  ? WMBaseFor(g_Batches[b].pSub) : nullptr;
			const bool bMoveThis = (pWM && pWM->bMoved);
			if (bMoveThis)
			{
				float B[9];
				for (int r2 = 0; r2 < 3; ++r2)
					for (int c2 = 0; c2 < 3; ++c2)
					{
						float v2 = 0.0f;
						for (int k2 = 0; k2 < 3; ++k2)
							v2 += pWM->lr[r2 * 3 + k2] * pWM->r[c2 * 3 + k2];
						B[r2 * 3 + c2] = v2;
					}
				float t[3];
				for (int r2 = 0; r2 < 3; ++r2)
					t[r2] = pWM->lp[r2]
						  - (B[r2*3+0]*pWM->p[0] + B[r2*3+1]*pWM->p[1]
						   + B[r2*3+2]*pWM->p[2]);
				// Row-vector form: rows are B transposed, last row is t.
				float M[16] = { B[0],B[3],B[6],0, B[1],B[4],B[7],0,
								B[2],B[5],B[8],0, t[0],t[1],t[2],1 };
				float mv[16];
				for (int r2 = 0; r2 < 4; ++r2)
					for (int c2 = 0; c2 < 4; ++c2)
					{
						float v2 = 0.0f;
						for (int k2 = 0; k2 < 4; ++k2)
							v2 += M[r2 * 4 + k2] * mvp[k2 * 4 + c2];
						mv[r2 * 4 + c2] = v2;
					}
				D3D11_MAPPED_SUBRESOURCE wm{};
				if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &wm, 0)))
				{
					memcpy(wm.pData, mv, sizeof mv);
					const float lmpw[4] = { (g_fFlatLight > 0.0f) ? g_fFlatLight : g_fLMScale,
								(g_fFlatLight > 0.0f) ? -1.0f : (float)g_bLMOnly, fCut, g_fAlphaOut };
					memcpy((char*)wm.pData + sizeof mv, lmpw, sizeof lmpw);
					g_pCtx->Unmap(g_pCB, 0);
					g_fAlphaCutSet = fCut;
				}
				++g_nWMDrawn;
			}

			// +StubBatchWatch <model>: say how each batch near that model is drawn.
			if (g_szBatchWatch[0] && g_nBatchWatchSaid < 40)
			{
				char szBW[64] = "";
				if (g_Batches[b].pSub) ModelName(g_Batches[b].pSub, szBW, sizeof szBW);
				const BatchBox& bxw = g_BatchBox[b];
				const bool bNear = (bxw.mn[0] < 840 && bxw.mx[0] > 760 && bxw.mn[1] < 645 && bxw.mx[1] > 540 && bxw.mn[2] < -970 && bxw.mx[2] > -995);
				if (_stricmp(szBW, g_szBatchWatch) == 0 || (strstr(g_szBatchWatch, "Hand") && bNear))
				{
					++g_nBatchWatchSaid;
					Log("  R3D BATCH WATCH: batch %u model '%s' sub %08X wm %s moved %d verts %u tex '%s' box %.0f %.0f %.0f .. %.0f %.0f %.0f",
						(unsigned)b, szBW[0] ? szBW : "(merged)", (unsigned)g_Batches[b].pSub, pWM ? "yes" : "NO",
						bMoveThis ? 1 : 0, (unsigned)g_Batches[b].nCount, g_Batches[b].szFile,
						bxw.mn[0], bxw.mn[1], bxw.mn[2], bxw.mx[0], bxw.mx[1], bxw.mx[2]);
				}
			}
			g_pCtx->Draw(g_Batches[b].nCount, g_Batches[b].nStart);
			g_nWorldDrawn += g_Batches[b].nCount;
			// ITS ENVIRONMENT MAP OVER IT (doors, shiny metal, chrome): the
			// retail renderer's EnvMapWorld. Back to opaque afterwards, and
			// the alpha-to-coverage tracker told so it rebinds for a cut-out.
			if (g_bEnvMap && g_Batches[b].pEnvSRV && g_pBlendOpaque)
			{
				EnvDraw(b, pQuat, pPos);
				const float kFe[4] = { 0, 0, 0, 0 };
				g_pCtx->OMSetBlendState(g_pBlendOpaque, kFe, 0xFFFFFFFF);
				g_bA2CBound = false;
			}

			if (bMoveThis)
			{
				// PUT THE MATRIX BACK. Every batch after this one is static.
				D3D11_MAPPED_SUBRESOURCE wm{};
				if (SUCCEEDED(TimedMap(g_pCB, D3D11_MAP_WRITE_DISCARD, &wm, 0)))
				{
					memcpy(wm.pData, mvp, sizeof mvp);
					const float lmpw[4] = { (g_fFlatLight > 0.0f) ? g_fFlatLight : g_fLMScale,
								(g_fFlatLight > 0.0f) ? -1.0f : (float)g_bLMOnly, fCut, g_fAlphaOut };
					memcpy((char*)wm.pData + sizeof mvp, lmpw, sizeof lmpw);
					g_pCtx->Unmap(g_pCB, 0);
					g_fAlphaCutSet = fCut;
				}
			}
		}
		if (bCullBound) g_pCtx->RSSetState(g_pRS);
		PhaseMark(0);		// the world's own batches (and the sky)
		static long s_nSaidNoPix = 0;
		if (nSkippedNoPix && s_nSaidNoPix++ == 0)
			Log("  R3D: skipping %ld vertices whose texture has no pixels", nSkippedNoPix);

		// ---- model meshes ----------------------------------------------
		R3D_PHASE("building the model meshes");
		// THE RUNS ARE THIS PASS'S OR NOBODY'S. g_MeshRuns was cleared inside
		// the block below, so a pass that skipped the block - the first frame
		// of a new world, before the client has published a model - left the
		// PREVIOUS world's runs standing, and the blended pass at the end of
		// the frame drew them: their views belonged to textures the world load
		// had just retired. That was the crash on the first frame of the intro
		// (run 10, d3d11.dll reading "tret" - a freed view whose memory now
		// held a string) and on the first frame of Morocco (run 9).
		//
		// BUT NOT ON EVERY PASS. The second pass of a frame reuses the first
		// pass's runs (bRebuild, below), so an unconditional clear here emptied
		// the LEFT eye of every character and plant (run 11). The runs are
		// cleared when the block is skipped - which is the first frame of a
		// new world, the case above - and at the world walk, which also forces
		// the next pass to rebuild (g_bMeshInvalidate).
		if (!(g_bModelMesh && g_bHaveModels && g_Models.nCount)) g_MeshRuns.clear();
		//
		// Skinned on the CPU from the piece's own records. A vertex is one or
		// more 20-byte records whose weights sum to 1.0, each giving the
		// position in ONE node's space; the world position is the weighted sum
		// of those transformed by the node's matrix, and the client publishes
		// every matrix already.
		//
		// Non-indexed on purpose: the UVs are per FACE CORNER, so an indexed
		// draw would have to split every seam anyway.
		if (g_bModelMesh && g_bHaveModels && g_Models.nCount)
		{
			// 12 MB of Vtx. It was 120000 and the desk run used 112533 of it -
			// 94% full standing STILL, with the nearest model 1687 units away.
			// Walking toward the NPCs overflows it, and an overflow breaks out
			// of the emit loop, so whichever instances come later simply are
			// not there. With the client now publishing nearest-first, an
			// overflow at least drops the farthest - but it should not happen.
			const UINT nMaxV = 300000;
			// A RING, NEVER DISCARDED. This was one 12 MB dynamic buffer mapped
			// WRITE_DISCARD every frame, and a discard makes the driver rename
			// the whole buffer; every so often it has to wait for the GPU to
			// hand one back. The watchdog caught the main thread in exactly
			// that wait - d3d11 -> nvwgf2um -> win32u - in 'building the model
			// meshes', 46 ms at the desk and 100 ms every two seconds on
			// the test PC (80 fps where a rock-solid 90 is needed). Four regions of
			// one buffer, written NO_OVERWRITE in turn: a region is reused only
			// after three frames the GPU has long finished, and the driver is
			// never asked to rename anything. +StubMeshRing 1 is the old way.
			const UINT nRegions = (g_nMeshRing > 0) ? (UINT)g_nMeshRing : 4u;
			// A FAILED ALLOCATION IS NOT WORTH REPEATING EVERY FRAME.
			//
			// This is 46 MB, and when the address space is nearly gone
			// CreateBuffer returns nothing. The old code ignored the result,
			// logged a success message anyway, and tried again on the next
			// frame - 16,761 attempts and 16,761 log lines in one mission on
			// 11 September, while the model mesh path silently did nothing
			// and TimedMap refused a null buffer 900 times a period. Say it
			// once, say it failed, and stop asking for a while.
			if (!g_pMeshVB && s_nMeshVBFailed < 3)
			{
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = nMaxV * sizeof(Vtx) * nRegions;
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				const HRESULT hrVB = g_pDev->CreateBuffer(&bd, nullptr, &g_pMeshVB);
				if (SUCCEEDED(hrVB) && g_pMeshVB)
				{
					s_nMeshVBFailed = 0;
					Log("  R3D: model mesh buffer: %u regions of %u vertices (%.0f MB), %s",
						nRegions, nMaxV, (double)bd.ByteWidth / 1048576.0,
						nRegions > 1 ? "written NO_OVERWRITE in turn, never discarded"
									 : "discarded every frame");
				}
				else
				{
					++s_nMeshVBFailed;
					Log("  R3D: the model mesh buffer (%.0f MB) COULD NOT BE"
						" ALLOCATED (hr %08lX, attempt %d of 3). Models lose"
						" their batched path until the next world.",
						(double)bd.ByteWidth / 1048576.0,
						(unsigned long)hrVB, s_nMeshVBFailed);
					LogAddressSpace("model mesh buffer refused");
				}
			}
			// Built once per PUBLISHED FRAME, not once per eye. The world pass
			// runs twice and the skinned positions are world space, so the
			// second eye can draw the buffer the first eye filled. Halves the
			// cost for nothing.
			static uint32_t s_nBuiltFrame = 0xFFFFFFFFu;
			static UINT     s_nBuiltVerts = 0;
			if (g_bMeshInvalidate) { g_bMeshInvalidate = false; s_nBuiltFrame = 0xFFFFFFFFu; g_MeshRuns.clear(); }
			const bool bRebuild = (s_nBuiltFrame != g_Models.nFrame);

			D3D11_MAPPED_SUBRESOURCE mm{};
			if (bRebuild) g_nMeshRegion = (g_nMeshRegion + 1) % nRegions;
			const UINT nRegionBytes = g_nMeshRegion * nMaxV * sizeof(Vtx);
			if (g_pMeshVB && bRebuild && SUCCEEDED(TimedMap(g_pMeshVB, (nRegions > 1) ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD, &mm, 1)))
			{
				// WHERE THE VERTICES GO.
				//
				// With the cache off this is the mapped buffer, exactly as
				// before - the default path does not change at all. With it on
				// they are built in ordinary memory and copied once at the end,
				// because a D3D11_MAP_WRITE_DISCARD pointer is write-combined:
				// writing it is fine and READING it back to fill a cache is
				// pathologically slow.
				Vtx* const pGpuOut = (Vtx*)((char*)mm.pData + nRegionBytes);
				static std::vector<Vtx> s_Cpu;
				if (g_bSkinCache && s_Cpu.size() < nMaxV) s_Cpu.resize(nMaxV);
				Vtx* out = (g_bSkinCache && s_Cpu.size() >= nMaxV)
						 ? s_Cpu.data() : pGpuOut;
				UINT nv = 0;
				// WHAT THE MODEL MESH COSTS, in milliseconds, measured rather than
				// inferred. This is the number that decides whether the publish cap
				// can be raised, and it has never been taken.
				LARGE_INTEGER qFreq{}, qT0{}, qT1{};
				QueryPerformanceFrequency(&qFreq);
				QueryPerformanceCounter(&qT0);
				const long long nReads0 = g_nReadCalls;
				uint32_t nInst = 0, nPieces = 0, nTris = 0;
				// Why nothing was drawn, if nothing was. A draw of zero triangles
				// and a draw that never ran look identical from outside.
				uint32_t rPlayer=0, rDims=0, rNodes=0, rModel=0, rPieceArr=0,
						 rPiece=0, rCounts=0, rMem=0, rSkin=0, rBehind=0,
						 rInvis=0, rStill=0;
				uint32_t nBadIdx = 0, nSkinTotal = 0;
				g_MeshRuns.clear();
				// THE POOL, created on first use and reset only here, at the
				// start of a build: nothing drawn from it this frame yet.
				if (g_bMeshPool && g_bSkinCache && out != pGpuOut)
				{
					if (!g_pPoolVB)
					{
						D3D11_BUFFER_DESC pd{};
						pd.ByteWidth = kPoolV * sizeof(Vtx);
						pd.Usage = D3D11_USAGE_DEFAULT;
						pd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
						if (FAILED(g_pDev->CreateBuffer(&pd, nullptr, &g_pPoolVB)))
						{
							g_pPoolVB = nullptr; g_bMeshPool = 0;
							Log("  R3D MESH POOL: CreateBuffer failed - pool off, the ring carries everything as before");
						}
						g_nPoolUsed = 0; ++g_nPoolGen;
					}
					if (g_bPoolResetPending || g_nPoolUsed > kPoolV - kPoolV / 8)
					{
						g_nPoolUsed = 0; ++g_nPoolGen; ++g_nPoolResets;
						g_bPoolResetPending = false;
					}
				}
				static std::vector<float> skinned;		// 3 floats per vertex

				g_nScaleNU = 0;
				for (uint32_t i = 0; i < g_Models.nCount; ++i)
				{
					const VRModelInst& mi = g_Models.inst[i];
					// +StubModelWatch at the loop head: every watched instance, before any skip.
					if (g_szModelWatch[0] && g_nModelHeadSaid < 200)
					{
						const uint32_t pMdlH = Word(mi.nObject, 0x1DC);
						const uint32_t pNmH = pMdlH ? Word(pMdlH, 0x04) : 0;
						char szLowH[96] = "";
						if (pNmH && MemKind(pNmH, 8) == 2) { strncpy(szLowH, (const char*)(uintptr_t)pNmH, 95); _strlwr(szLowH); }
						if (strstr(szLowH, g_szModelWatch))
						{
							++g_nModelHeadSaid;
							Log("  R3D MODEL HEAD f=%ld: object %08X at %.1f %.1f %.1f dims %.1f %.1f %.1f flags %08X cam %.1f %.1f %.1f",
								(long)g_nFrames, mi.nObject, mi.fPos[0], mi.fPos[1], mi.fPos[2],
								mi.fDims[0], mi.fDims[1], mi.fDims[2], mi.nFlags, g_fLastPos[0], g_fLastPos[1], g_fLastPos[2]);
						}
					}
					// Counted here, at the top of the loop that actually DRAWS,
					// so the number is instances we put on screen rather than
					// instances the client offered us.
					// THE PLAYER'S OWN BODY IS FLAGGED INVISIBLE IN FIRST PERSON,
					// which is the flat game hiding it from a camera inside its
					// head. +StubBody exists to draw it anyway, and drew nothing,
					// because this rule ran first and honoured the flag. In the
					// headset there was no in-game body for the player.
					if ((mi.nFlags & VRMODEL_F_INVISIBLE)
						&& !(g_bDrawBody && (mi.nFlags & 1u)))
					{
						++g_nInvisInst;
						// THE ENGINE'S OWN BIT, which is a far stronger rule
						// than any name heuristic this project has had to use
						// - but it is NOT unconditionally safe, and that is
						// why it is a switch. The client clears FLAG_VISIBLE
						// on the PLAYER'S OWN BODY in first person and on
						// ATTACHMENTS in some states (GameClientShell.cpp
						// 10269, 13571), so a blind skip could take the
						// weapon with it. Measured both arms before this
						// default was chosen; see the commit.
						// NAME WHAT IS BEING SKIPPED. "10 instances skipped" is a
						// number nobody can act on; "a lamp shade is being
						// skipped" is an answer. Headset testing reports the Morocco
						// hotel lamps as a floating bulb with no shade, and a
						// shade the engine has flagged invisible - which this
						// skips by default - would look exactly like that.
						if (g_bSkipInvisible)
						{
							++g_nSkipInvis; ++rInvis;
							// 40, and DEDUPED by object below, because the
							// interesting one is rare: the tester has a double door
							// whose left handle is absent until the door
							// animates and then hides again, which is exactly
							// what an object the engine flags invisible looks
							// like when this rule skips it. A cap of 12 filled
							// with the same two objects would never show it.
							if (g_nInvisNamed < 40)
							{
								const uint32_t pMdl = Word(mi.nObject, 0x1DC);
								const uint32_t pNm  = pMdl ? Word(pMdl, 0x04) : 0;
								bool bSeen = false;
								for (int q = 0; q < g_nInvisSeen; ++q)
									if (g_aInvisSeen[q] == mi.nObject)
									{ bSeen = true; break; }
								if (!bSeen && g_nInvisSeen < 64)
									g_aInvisSeen[g_nInvisSeen++] = mi.nObject;
								if (!bSeen && MemKind(pNm, 8) == 2)
								{
									++g_nInvisNamed;
									Log("  R3D INVIS SKIP: object %08X  '%.60s'"
										"  at (%.0f %.0f %.0f) dims (%.0f %.0f %.0f)",
										mi.nObject, (const char*)(uintptr_t)pNm,
										mi.fPos[0], mi.fPos[1], mi.fPos[2],
										mi.fDims[0], mi.fDims[1], mi.fDims[2]);
								}
							}
							continue;
						}
					}
					// THE OBJECT'S SCALE, ONCE, FOR BOTH SKINNING PATHS.
					// A zero would collapse the model to a point, and a struct zeroed
					// by an older client would do exactly that - so an unset scale
					// reads as 1, never as 0.
					float fSc[3] = { 1.0f, 1.0f, 1.0f };
					if (g_bModelScale)
					{
						for (int z = 0; z < 3; ++z)
							if (mi.fScale[z] > 0.0001f) fSc[z] = mi.fScale[z];
					}
					// AN OBJECT SCALE IS IN OBJECT SPACE, AND A BONE OFFSET IS NOT.
					//
					// The skin scales each vertex's bone-space offset by fSc and
					// then rotates it by the node. That is only the same thing as
					// scaling the object while the scale is UNIFORM - and every
					// character in the game is (1.3 1.3 1.3), so nothing ever
					// showed it. The intro van's headlight beams are (6 6 320) on
					// SFX/BEAMS/MODELS/SPOTLIGHTBEAM.ABC, whose single node turns
					// the mesh 90 degrees, so the 320 was applied along the disc's
					// thin axis and came out as a 1593-unit shaft straight up out
					// of each lamp instead of a 320-unit beam down the road.
					//
					// So a non-uniform scale takes the rotated offset back into
					// OBJECT space, scales it there, and returns it:
					//
					//     world = t_node + R * ( S . ( R^T * (M_node * bind) ) )
					//
					// which collapses to the old expression exactly when
					// S = (s s s). Uniform-scaled models are therefore bit
					// identical and only this class of object moves at all.
					//
					// R is the object's own rotation. VRModelInst has carried a
					// field for it since the struct was written and the client had
					// never filled it; see the publish. A model whose rotation did
					// not arrive falls back to the old behaviour rather than
					// scaling by an identity that is not the object's frame.
					const bool bScaleNU = g_bObjScaleSpace
						&& (fabsf(fSc[0] - fSc[1]) > 1e-4f
						 || fabsf(fSc[1] - fSc[2]) > 1e-4f)
						&& (mi.fRot[0] != 0.0f || mi.fRot[1] != 0.0f
						 || mi.fRot[2] != 0.0f || mi.fRot[3] != 0.0f);
					float fOR[3], fOU[3], fOF[3];
					if (bScaleNU)
					{
						++g_nScaleNU;
						QuatBasis(mi.fRot, fOR, fOU, fOF);
					}
					// The player's own body, unless it is the VIEW WEAPON -
					// which is at the camera by definition and is the one
					// thing that must survive a filter written to remove
					// things at the camera.
					//
					// ...OR UNLESS WE WANT TO SEE HER. +StubBody 1 draws the
					// player's own model, which is what makes looking down show
					// Cate standing where you are standing instead of nothing.
					// It is one line because the client has been publishing her
					// every frame all along; only this dropped her.
					//
					// The HEAD comes off separately, below - without that you
					// are inside her skull and the view is a wall of face.
					if ((mi.nFlags & 1u)
						&& !(mi.nFlags & VRMODEL_F_VIEWMODEL)
						&& !g_bDrawBody)
					{ ++rPlayer; continue; }
					// A SIZE LIMIT INHERITED FROM THE DEBUG BOXES, and it does not
					// belong here. The stand-in boxes needed it because an enormous
					// box filled the frame and said nothing; the MESH path draws the
					// real model, and a large one is a helicopter or a truck or a
					// sign - exactly the furniture reported missing in the headset. The reason
					// the limit was really there is the NEXT test, which rejects
					// anything the camera is inside, and that one is exact.
					//
					// Raised rather than removed, because GetObjectDims does return
					// nonsense for some objects and a model the size of the level
					// would cover everything. +StubModelDims sets it; what it turns
					// away is named below rather than merely counted.
					// The interface scene's art is large on purpose - a card's
					// bar is 1216 units wide - and there is no level for it to
					// cover, so the limit does not apply there.
					if (!g_bInterfaceOnly
						&& (mi.fDims[0] > g_fModelDimsMax || mi.fDims[1] > g_fModelDimsMax
							|| mi.fDims[2] > g_fModelDimsMax))
					{
						++rDims;
						if (g_nDimsSaid < 8)
						{
							++g_nDimsSaid;
							Log("  R3D MESH: too big to draw - dims (%.0f %.0f %.0f)"
								" at (%.0f %.0f %.0f), %u nodes, limit %.0f",
								mi.fDims[0], mi.fDims[1], mi.fDims[2],
								mi.fPos[0], mi.fPos[1], mi.fPos[2],
								mi.nNodeCount, g_fModelDimsMax);
						}
						continue;
					}
					// Skip anything the CAMERA IS INSIDE - the player's weapon
					// and attachments sit at the camera. The stand-in boxes have
					// filtered this since the first two runs came back filled
					// edge to edge by the weapon's own box; the MESH path never
					// got the same filter, and in the headset screenshot the weapon
					// is a slab of orange sweeping across both eyes, far larger
					// than anything else and hiding the street behind it.
					// ...and the VIEW WEAPON is exempt, for the same reason it
					// is exempt from the player test above: it is at the camera
					// by definition, which is the exact condition this removes.
					// ...and the PLAYER'S OWN BODY, when we are drawing it. Her
					// box contains the camera by definition - the camera is
					// inside her head - so this test removes her every time,
					// which is why +StubBody alone showed nothing but floor.
					// The head is taken off by piece below; the rest of her is
					// meant to be here.
					// ...AND ONLY THE PLAYER'S OWN THINGS. Applied to every
					// model, this removed any CHARACTER the eye came within
					// a forearm of - in the HQ, characters disappeared
					// when the headset moved too close to them. The
					// client now says which models the player owns
					// (VRMODEL_F_OWNED); an older client sets no such bit,
					// and then the rule keeps its old reach.
					const bool bEyeCull = (g_bCullFromEye && g_bHaveEyeCam);
					const float* pCullPos  = bEyeCull ? g_fEyeCamPos  : g_fLastPos;
					const float* pCullQuat = bEyeCull ? g_fEyeCamQuat : g_fLastQuat;
					if (g_bHaveCam
						&& !(mi.nFlags & VRMODEL_F_VIEWMODEL)
						&& !(g_bDrawBody && (mi.nFlags & 1u))
						&& (!g_bOwnedFlagSeen || (mi.nFlags & (VRMODEL_F_OWNED | 1u)))
						&& fabsf(pCullPos[0] - mi.fPos[0]) <= mi.fDims[0]
						&& fabsf(pCullPos[1] - mi.fPos[1]) <= mi.fDims[1]
						&& fabsf(pCullPos[2] - mi.fPos[2]) <= mi.fDims[2])
					{ ++rPlayer; continue; }
					// BEHIND THE CAMERA? SKIP IT BEFORE DOING ANY CPU SKINNING.
					//
					// That is what this test's own comment always said, and it was not
					// what the code did: it sat AFTER the skinning loop, so every
					// instance behind the player was fully skinned - every vertex, every
					// weight record - and then thrown away.
					//
					// The model account is what made that visible. On m01s02, of 192
					// published instances 165 are behind the camera and 25 draw, so about
					// six sevenths of the mesh build was work for nothing. Moved here,
					// before the model pointer is even followed, it needs only the
					// position and dimensions the client already published.
					//
					// Still the weakest possible test: a bounding sphere entirely behind
					// the eye cannot fall in either eye's frustum however asymmetric they
					// are, so it cannot remove anything that would have been drawn.
					if (g_bHaveCam)
					{
						float rr4[3], uu4[3], ff4[3];
						QuatBasis(pCullQuat, rr4, uu4, ff4);
						const float dx4 = mi.fPos[0] - pCullPos[0];
						const float dy4 = mi.fPos[1] - pCullPos[1];
						const float dz4 = mi.fPos[2] - pCullPos[2];
						const float fwd4 = dx4*ff4[0] + dy4*ff4[1] + dz4*ff4[2];
						const float fRad = sqrtf(mi.fDims[0]*mi.fDims[0]
											   + mi.fDims[1]*mi.fDims[1]
											   + mi.fDims[2]*mi.fDims[2]);
						// ...EXCEPT THE VIEW WEAPON. Its published position is the gun's
						// ORIGIN at the hand and its dims are the engine's camera-space
						// half-extents, so a hand swung beside or behind the head puts the
						// origin behind the eye plane while the drawn barrel still
						// reaches into view - and the whole gun vanished. Headset
						// testing, 20 September: the gun model disappeared when the hand
						// moved the gun around a little. The view weapon is skinned
						// whatever its origin does; it is one instance.
						if (fwd4 < -fRad && !(mi.nFlags & VRMODEL_F_VIEWMODEL)) { ++g_nCulledBehind; ++rBehind; continue; }
					}
					if (mi.nNodeFirst + mi.nNodeCount > VRMODELS_MAX_NODES) { ++rNodes; continue; }
					if (!mi.nNodeCount) { ++rNodes; continue; }
					// Did this instance's skeleton change since last frame?
					uint32_t nSkelHash = 0;
					bool bStill = false;
					{
						uint32_t h = 2166136261u;
						const uint32_t nEnd = mi.nNodeFirst + mi.nNodeCount;
						for (uint32_t ni = mi.nNodeFirst; ni < nEnd; ++ni)
						{
							const float* m = g_Models.nodes[ni].m;
							for (int q = 0; q < 12; ++q)
							{
								uint32_t v;
								memcpy(&v, &m[q], 4);
								h = (h ^ v) * 16777619u;
							}
						}
						// EVERYTHING ELSE THAT DECIDES A VERTEX, folded into the
						// same hash - the scale the mesh is posed at and the flag
						// word that picks its lighting value. Free here, and it
						// is what lets the vertex cache trust this verdict.
						for (int q = 0; q < 3; ++q)
						{
							uint32_t v; memcpy(&v, &mi.fScale[q], 4);
							h = (h ^ v) * 16777619u;
						}
						h = (h ^ mi.nFlags) * 16777619u;
						nSkelHash = h;
						std::map<uint32_t, uint32_t>::iterator it
							= g_NodeHash.find(mi.nObject);
						bStill = (it != g_NodeHash.end() && it->second == h);
						if (bStill) ++g_nInstStill; else ++g_nInstMoved;
						g_NodeHash[mi.nObject] = h;
						// The ablation arm. Counted in its own bucket so the
						// model account still balances while it runs.
						if (g_bSkipStill && bStill)
							{ ++g_nSkippedStill; ++rStill; continue; }
					}

					const uint32_t pModel = Word(mi.nObject, 0x1DC);
					if (MemKind(pModel, 0x100) != 2) { ++rModel; continue; }

					// THE CACHE HIT. Four things must agree, and the model
					// pointer is read and validated above before any of them -
					// an object whose address was recycled onto a different
					// model fails here rather than drawing the old one.
					if (g_bSkinCache && bStill && out != pGpuOut)
					{
						std::map<uint32_t, CachedInst>::iterator ci
							= g_SkinCache.find(mi.nObject);
						// RESIDENT: draw it where it already is (see g_pPoolVB).
						if (g_pPoolVB && g_bMeshPool
							&& ci != g_SkinCache.end()
							&& ci->second.pModel  == pModel
							&& ci->second.nHash   == nSkelHash
							&& ci->second.nTexGen == g_nTexGen
							&& !ci->second.v.empty())
						{
							CachedInst& c = ci->second;
							const UINT nCV = (UINT)c.v.size();
							if ((c.nPoolGen != g_nPoolGen || c.nPoolHash != c.nHash)
								&& g_nPoolUsed + nCV <= kPoolV)
							{
								D3D11_BOX bx{};
								bx.left = g_nPoolUsed * (UINT)sizeof(Vtx);
								bx.right = (g_nPoolUsed + nCV) * (UINT)sizeof(Vtx);
								bx.top = 0; bx.bottom = 1; bx.front = 0; bx.back = 1;
								g_pCtx->UpdateSubresource(g_pPoolVB, 0, &bx, &c.v[0], 0, 0);
								c.nPoolStart = g_nPoolUsed;
								c.nPoolGen = g_nPoolGen;
								c.nPoolHash = c.nHash;
								g_nPoolUsed += nCV;
								++g_nPoolUp;
							}
							if (c.nPoolGen == g_nPoolGen && c.nPoolHash == c.nHash)
							{
								for (size_t z = 0; z < c.runs.size(); ++z)
								{
									MeshRun r = c.runs[z];
									r.nStart += c.nPoolStart;
									r.nVB = 1;
									if (!g_MeshRuns.empty()
										&& g_MeshRuns.back().nVB == 1
										&& g_MeshRuns.back().pSRV == r.pSRV
										&& g_MeshRuns.back().fCutRef == r.fCutRef
										&& g_MeshRuns.back().nBlend == r.nBlend
										&& g_MeshRuns.back().fA == r.fA
										&& g_MeshRuns.back().nView == r.nView
										&& g_MeshRuns.back().fLight[0] == r.fLight[0]
										&& g_MeshRuns.back().fLight[1] == r.fLight[1]
										&& g_MeshRuns.back().fLight[2] == r.fLight[2]
										&& g_MeshRuns.back().nStart
											+ g_MeshRuns.back().nCount == r.nStart)
										g_MeshRuns.back().nCount += r.nCount;
									else
										g_MeshRuns.push_back(r);
								}
								nPieces += c.nPieces;
								nTris   += c.nTris;
								++nInst;
								++g_nSkinHit;
								++g_nPoolHit;
								continue;
							}
							++g_nPoolNoRoom;
						}
						if (ci != g_SkinCache.end()
							&& ci->second.pModel  == pModel
							&& ci->second.nHash   == nSkelHash
							&& ci->second.nTexGen == g_nTexGen
							&& nv + ci->second.v.size() <= nMaxV)
						{
							const CachedInst& c = ci->second;
							memcpy(out + nv, c.v.empty() ? (const Vtx*)"" : &c.v[0],
								   c.v.size() * sizeof(Vtx));
							for (size_t z = 0; z < c.runs.size(); ++z)
							{
								MeshRun r = c.runs[z];
								r.nStart += nv;
								// Coalesce with the run before it on the same
								// terms the build loop uses, or a cached
								// instance costs more draw calls than a built
								// one and the arms stop being comparable.
								r.nVB = 0;
								if (!g_MeshRuns.empty()
									&& g_MeshRuns.back().nVB == 0
									&& g_MeshRuns.back().pSRV == r.pSRV
									&& g_MeshRuns.back().fCutRef == r.fCutRef
									&& g_MeshRuns.back().nBlend == r.nBlend
									&& g_MeshRuns.back().fA == r.fA
									&& g_MeshRuns.back().nView == r.nView
									&& g_MeshRuns.back().fLight[0] == r.fLight[0]
									&& g_MeshRuns.back().fLight[1] == r.fLight[1]
									&& g_MeshRuns.back().fLight[2] == r.fLight[2]
									&& g_MeshRuns.back().nStart
										+ g_MeshRuns.back().nCount == r.nStart)
									g_MeshRuns.back().nCount += r.nCount;
								else
									g_MeshRuns.push_back(r);
							}
							nv += (UINT)c.v.size();
							nPieces += c.nPieces;
							nTris   += c.nTris;
							++nInst;
							++g_nSkinHit;
							continue;
						}
					}
					// Every node's matrix checked ONCE for this instance: the
					// record loop used to test NaN and all-zero on every one of
					// a vertex's records. 0 ok, 1 NaN, 2 unposed.
					LARGE_INTEGER qMiss0; QueryPerformanceCounter(&qMiss0);
					static std::vector<uint8_t> s_NodeOk;
					s_NodeOk.assign(mi.nNodeCount, 0);
					for (uint32_t ni = 0; ni < mi.nNodeCount; ++ni)
					{
						const float* m3 = g_Models.nodes[mi.nNodeFirst + ni].m;
						if (!(m3[0] == m3[0]) || !(m3[3] == m3[3])
							|| !(m3[7] == m3[7]) || !(m3[11] == m3[11]))
							s_NodeOk[ni] = 1;
						else if (m3[0] == 0.0f && m3[1] == 0.0f && m3[2] == 0.0f
							&& m3[4] == 0.0f && m3[5] == 0.0f && m3[6] == 0.0f
							&& m3[8] == 0.0f && m3[9] == 0.0f && m3[10] == 0.0f)
							s_NodeOk[ni] = 2;
					}
					const uint32_t pPieceArr = Word(pModel, 0x38);
					const uint32_t nPieceArr = Word(pModel, 0x3C);
					if (MemKind(pPieceArr, 4) != 2 || !nPieceArr || nPieceArr > 64)
						{ ++rPieceArr; continue; }

					// Skin the MODEL's record array, not the piece's. model+58
					// is the array that was verified to reconstruct the object
					// to the decimal; piece+04 is something else and skinning it
					// ran out of records on all 288 pieces. The vertex count is
					// DERIVED from the records rather than taken from a field -
					// the record count is not the vertex count when a vertex
					// carries several weights.
					// The texture hunt walks the PIECES, which is where a
					// per-piece texture would have to live - the model's own
					// arrays are shared by every piece.
					if (g_bTexHunt && !g_bTexHuntSaid)
					{
						++g_nTexHuntObjs;
						for (int off = 0; off < 256; ++off)
						{
							const uint32_t v = Word(mi.nObject, off * 4);
							if (!v) continue;
							if (TextureFor(v) >= 0) ++g_aObjHit[off][0];
							else if (MemKind(v, 0x40) == 2)
							{
								for (int k = 0; k < 16; ++k)
									if (TextureFor(Word(v, k * 4)) >= 0)
									{ ++g_aObjHit[off][1]; break; }
							}
						}
						for (int off = 0; off < 128; ++off)
						{
							const uint32_t v = Word(pModel, off * 4);
							if (!v) continue;
							if (TextureFor(v) >= 0) ++g_aModHit[off][0];
							else if (MemKind(v, 0x40) == 2)
							{
								for (int k = 0; k < 16; ++k)
									if (TextureFor(Word(v, k * 4)) >= 0)
									{ ++g_aModHit[off][1]; break; }
							}
						}
						for (uint32_t q = 0; q < nPieceArr && q < 64; ++q)
						{
							const uint32_t pPiece = Word(pPieceArr + q * 4, 0);
							if (MemKind(pPiece, 0x100) != 2) continue;
							++g_nTexHuntPieces;
							for (int off = 0; off < 64; ++off)
							{
								const uint32_t v = Word(pPiece, off * 4);
								if (!v) continue;
								if (TextureFor(v) >= 0) ++g_aTexHit[off][0];
								else if (MemKind(v, 0x40) == 2)
								{
									for (int k = 0; k < 16; ++k)
										if (TextureFor(Word(v, k * 4)) >= 0)
										{ ++g_aTexHit[off][1]; break; }
								}
							}
						}
					}

					const uint32_t pRec = Word(pModel, 0x58);
					const uint32_t nRec = Word(pModel, 0x5C);
					// Validate the array ONCE, at both ends. This used to call
					// MemKind - a VirtualQuery SYSCALL - on every record and
					// every face: ~50000 per eye per frame, 9 million a second,
					// which took the renderer from 89 fps to 4 and made the
					// GAME run at insane speed, because LithTech takes one
					// simulation step per frame and a quarter-second step means
					// the enemy moves and shoots for a quarter second at once.
					if (!nRec || nRec > 200000) { ++rCounts; continue; }
					if (MemKind(pRec, 20) != 2 ||
						MemKind(pRec + (nRec - 1) * 20, 20) != 2)
						{ ++rCounts; continue; }
					uint32_t nSkinned = 0;
					if (!g_bPieceVA)
					{
						skinned.assign((size_t)nRec * 3, 0.0f);
						float w = 0.0f, acc[3] = { 0, 0, 0 };
						for (uint32_t r = 0; r < nRec; ++r)
						{
							const uint32_t q = pRec + r * 20;
							const float*   vv = (const float*)(uintptr_t)q;
							const uint32_t ni = *(const uint32_t*)(uintptr_t)(q + 16);
							const float    ww = vv[3];
							if (!(ww == ww) || ww < -0.01f || ww > 1.01f) break;
							if (ni < mi.nNodeCount)
							{
								const float* m = g_Models.nodes[mi.nNodeFirst + ni].m;
								// See g_bWeightPre: the offset is pre-scaled by the bias.
								const float wwt = g_bWeightPre ? 1.0f : ww;
								// THE OFFSET IS IN MODEL SPACE, SO THE SCALE GOES HERE.
								// The node translation is already world space and already
								// scaled; scaling it again would move the whole figure.
								const float sx = vv[0] * fSc[0];
								const float sy = vv[1] * fSc[1];
								const float sz = vv[2] * fSc[2];
								acc[0] += wwt * (m[0]*sx + m[1]*sy + m[2] *sz) + ww*m[3];
								acc[1] += wwt * (m[4]*sx + m[5]*sy + m[6] *sz) + ww*m[7];
								acc[2] += wwt * (m[8]*sx + m[9]*sy + m[10]*sz) + ww*m[11];
							}
							w += ww;
							if (w > 0.99f)
							{
								skinned[(size_t)nSkinned * 3 + 0] = acc[0];
								skinned[(size_t)nSkinned * 3 + 1] = acc[1];
								skinned[(size_t)nSkinned * 3 + 2] = acc[2];
								++nSkinned;
								acc[0] = acc[1] = acc[2] = 0.0f;
								w = 0.0f;
							}
						}
					}
					// With the entry array there is no global skin to check; a
					// piece that cannot resolve its own entries is skipped
					// individually, and the log counts it.
					if (!g_bPieceVA && !nSkinned) { ++rSkin; continue; }

					// Skin 0 for this object. Ranked at 184 of 184 instances;
					// see the commit. A model whose skin has not been bound yet
					// falls back to the untextured stand-in rather than
					// vanishing - the engine binds a texture before its pixels
					// are resident and this path must survive that.

					// The object carries a SKIN ARRAY, not one skin: body, head and
					// hands are separate textures (the game's own bute manager has
					// GetBodySkinFilename / GetHeadSkinFilename / GetHandsSkinFilename).
					// Which slot a piece uses is in its NAME - see PieceSkinSlot.
					ID3D11ShaderResourceView* pSlot[4] = { 0, 0, 0, 0 };
					// What each slot's picture IS, for the routes that never
					// make a texture entry. -1 = unknown, ask g_Tex.
					float fSlotCut[4] = { -1, -1, -1, -1 };
					int   nSlotGraded[4] = { -1, -1, -1, -1 };
					uint32_t nSlotRaw[4] = { 0, 0, 0, 0 };
					{ LARGE_INTEGER q; QueryPerformanceCounter(&q); g_qPre += q.QuadPart - qMiss0.QuadPart; }
					LARGE_INTEGER qSl0; QueryPerformanceCounter(&qSl0);
					for (int s = 0; s < 4; ++s)
					{
						nSlotRaw[s] = Word(mi.nObject, 0x1B8 + s * 4);
						// Is the cached entry still for THIS PICTURE? Comparing
						// the data pointer alone was the first attempt and it
						// did not hold - after a reload the skins were still
						// scrambled, differently again. So compare the pixel
						// pointer and a hash of the pixels too, and COUNT
						// which of the three moved, because that says whether
						// the engine recycles the object, the data struct, or
						// merely refills the same buffer.
						if (s < 2 && nSlotRaw[s])
						{
							const int nC = TextureFor(nSlotRaw[s]);
							uint32_t dNow = 0, pNow = 0, sNow = 0;
							const bool bLive = TexLiveId(nSlotRaw[s], &dNow, &pNow, &sNow);
							// The first run of this check reported "0 slots
							// checked" while the same frame's skin report said
							// both slots resolved, so one of these two terms is
							// false and a counter cannot say which. Print the
							// pointers for the first few and let them say it.
							++g_nSkinIdSlots;
							if (nC >= 0)  ++g_nSkinIdCached;
							// The entry was built for whatever lived at this address
							// at the time. If that was before the current world
							// load, the engine has freed and reallocated since,
							// and these are the pixels of a texture that is gone.
							if (nC >= 0 && g_Tex[nC].nLoad < g_nWorldLoads)
								++g_nSkinIdOld;
							if (bLive)    ++g_nSkinIdLive;
							if (g_nSkinIdSaid < 8)
							{
								++g_nSkinIdSaid;
								// WHAT DOES THE ENGINE SAY IS AT THIS ADDRESS, NOW?
								// Cate's pieces resolve to cache entries named
								// MENU\SKINS\HORZBART and BOXB while her real skins sit
								// in the same cache under their own names - and no amount
								// of looking at the cache can say whether the SLOT POINTER
								// is wrong or the ENTRY IT LANDS ON is stale. The engine's
								// own name at that address decides it: if it agrees with
								// the entry, the slot is wrong; if it names her skin, the
								// entry is stale under an address the engine recycled, and
								// a lookup by address can never be right again.
								char szLive[80];
								const bool bName = TexNameOf(nSlotRaw[s], szLive,
												     sizeof szLive);
								Log("  R3D SKINID: slot %d tex %08X -> cache %d (%s),"
									" ENGINE SAYS %s,"
									" live %d, data %08X pix %08X sig %08X"
									"  (obj +1B8 %08X +1BC %08X)",
									s, nSlotRaw[s], nC,
									(nC >= 0) ? g_Tex[nC].szName : "-",
									bName ? szLive : "<NO NAME AT THIS ADDRESS>",
									(int)bLive, dNow, pNow, sNow,
									Word(mi.nObject, 0x1B8), Word(mi.nObject, 0x1BC));
							}
							// WHERE DOES A MODEL SKIN KEEP ITS DATA? +0x08 is
							// null on every one of them, because that offset was
							// ranked on textures the engine had BOUND and it
							// never binds these. So rank the offsets again, on
							// this population, the same way: a word is the data
							// pointer if what it points at validates as a
							// texture-data struct - dimensions in range, and the
							// mip-0 header agreeing with them. Nothing else in a
							// heap object accidentally satisfies both.
							if (!bLive && Readable(nSlotRaw[s], 256))
							{
								++g_nSkinScan;
								for (int z = 0; z < 64; ++z)
								{
									const uint32_t c = Word(nSlotRaw[s], z * 4);
									if (!c || !Readable(c, kTexMip0 + 16)) continue;
									const uint32_t cw = *(const uint16_t*)(uintptr_t)(c + kTexWidth);
									const uint32_t ch = *(const uint16_t*)(uintptr_t)(c + kTexHeight);
									if (!cw || !ch || cw > 4096 || ch > 4096) continue;
									if (Word(c, kTexMip0 + 0) != cw) continue;
									if (Word(c, kTexMip0 + 4) != ch) continue;
									++g_aSkinOff[z];
								}
								if (g_nSkinScan == 200 && !g_nSkinOffSaid)
								{
									g_nSkinOffSaid = 1;
									for (int z = 0; z < 64; ++z)
										if (g_aSkinOff[z] * 4 >= g_nSkinScan)
											Log("  R3D SKINOFF: +%03X validates as a"
												" texture data struct on %ld of %ld"
												" model skins", z * 4, g_aSkinOff[z],
												g_nSkinScan);
									Log("  R3D SKINOFF: %ld model skins scanned",
										g_nSkinScan);
								}
							}
							if (nC >= 0 && bLive)
							{
								++g_nSkinIdChecked;
								const bool bD = (g_Tex[nC].pData != dNow);
								const bool bP = (g_Tex[nC].pPix  != pNow);
								const bool bS = (sNow && g_Tex[nC].nSig != sNow);
								if (bD) ++g_nSkinIdData;
								if (bP) ++g_nSkinIdPix;
								if (bS) ++g_nSkinIdSig;
								if (bD || bP || bS) QueueTexRefresh(nSlotRaw[s]);
							}
						}
						// STAGE 1 ON THE SKINS. A model skin is the case the heap
						// route cannot serve at all: the engine loads a model's skin
						// only when it draws the model through the path we replaced,
						// so texobj+0x08 is null on 66690 of 66690 slots and there
						// are no pixels anywhere to copy. The weapon's grip is that,
						// and so are the translucent world models.
						//
						// THE NAME IS NOT MISSING - only the pixels ever were. The
						// object still says which .dtx it stands for, and the file is
						// on disk whether or not the engine ever read it.
						//
						// Bounded to a few attempts a frame. A slot that cannot be
						// named would otherwise be retried on every object on every
						// frame, which is the shape of a cost that does not show up
						// here and does show up as the tester reporting the rate dipping.
						// SLOTS 2 AND 3 ARE NOT SKINS - they read 000000FF and
						// 19E3CE92, measured. Probing them spent half of the
						// first skin-name run on garbage and made the result
						// unreadable: 47 of 200 held a string, and there was no
						// way to tell a real skin with no name from a slot that
						// was never a texture.
						// "ALREADY CACHED" IS NOT "STILL THIS PICTURE", and the
						// difference is the whole bug. This test used to be
						// TextureFor(slot) < 0 - ask the engine for a name only when
						// the address is unknown - so an address the engine had
						// RECYCLED answered yes, the name was never asked for, and
						// the picture filed under it by its previous owner was drawn.
						// That is how Cate came to wear MENU\SKINS\HORZBART.DTX, a
						// 32x32 flat bar, on the main menu.
						//
						// MEASURED, not reasoned. At the main menu the six
						// MENU\SPRTEX textures still answer to the names we filed
						// them under, and all ten MODEL SKINS answer to each other's
						// in an EXACT REVERSAL of their order:
						//
						//   key 032DB348  filed MENU\SKINS\HORZBART.DTX
						//               engine says CHARS\SKINS\INTHERO_ACTION.DTX
						//   key 032DB244  filed CHARS\SKINS\INTHERO_ACTION.DTX
						//               engine says MENU\SKINS\HORZBART.DTX
						//
						// A reversal is the signature of a LIFO free list handing a
						// freed block back in the opposite order: the menu's skins are
						// released and the character skins allocated into the very
						// same objects. The address is therefore NOT AN IDENTITY, and
						// no offset in the object is - only the name is, which this
						// file has said in a comment since the file route was written.
						//
						// So the question is not "do we have an entry for this
						// address" but "is the entry we have still the picture the
						// engine keeps there". Only a name compare can answer it. It
						// costs one string compare per slot per frame and it stops at
						// the first mismatch, which after the first frame is rare.
						//
						// An entry with NO recovered name is left alone: empty means
						// the route was never found for it, not that it is wrong, and
						// tearing those down every frame would be a rebuild loop.
						// Throttled: see g_nSkinNamePeriod. A slot is checked on its
						// own frames, staggered by object address so they do not all
						// land on the same frame and spike it.
						const int nWas = TextureFor(nSlotRaw[s]);
						char szSlotName[80];
						bool bSlotNamed = false;
						// THE SKIN PATH HONOURS THE VERIFY WINDOW TOO. It re-checked
						// a known address only on the 30-frame stagger, so after a
						// folder change a recycled skin object wore its previous
						// owner's picture for up to half a second: the options
						// card drawn with a piece of Cate's sunglasses for the
						// first fifteen frames of a sixty-frame series across
						// MAIN -> OPTIONS. In the headset, textures dropped out and
						// reloaded on a menu selection. The world and sprite
						// routes already verify every frame inside the window.
						// A YOUNG OBJECT CHECKS EVERY FRAME. A weapon swap makes
						// a new view-weapon object whose skin lands at an
						// address the cache already knows under another name,
						// and for a stagger period it drew that other picture
						// - the gun briefly flashed a different colour, like
						// Cate's outfit.
						std::unordered_map<uint32_t, PubSkin>::const_iterator itY =
							g_PubSkins.find(mi.nObject);
						const bool bYoung = (itY != g_PubSkins.end())
							&& (g_nFrames - itY->second.nFrame) < 120;
						const bool bCheckNow = g_nSkinNamePeriod > 0
							&& (nWas < 0
								|| bYoung
								|| g_nFrames < g_nVerifyUntil
								|| ((g_nFrames + (long)(nSlotRaw[s] >> 4))
									% g_nSkinNamePeriod) == 0);
						if (bCheckNow)
							bSlotNamed = TexNameOf(nSlotRaw[s], szSlotName,
									  sizeof szSlotName);
						const bool bWrongPicture = (nWas >= 0 && bSlotNamed
							&& g_Tex[nWas].szName[0]
							&& _stricmp(g_Tex[nWas].szName, szSlotName) != 0);
						if (s < 2 && bWrongPicture) ++g_nSkinRecycled;
						if (s < 2 && g_bTexFromFile && nSlotRaw[s]
							&& (nWas < 0 || bWrongPicture)
							&& g_nSkinTryThisFrame < 16)
						{
							++g_nSkinTryThisFrame;
							++g_nSkinNameTried;
							SkinNameProbe(nSlotRaw[s]);
							SkinDump(nSlotRaw[s]);
							if (bSlotNamed)
							{
								++g_nSkinNamed;
								if (MakeFileTexture(nSlotRaw[s]) >= 0) ++g_nSkinFromFile;
							}
						}
						const int si = TextureFor(nSlotRaw[s]);
						// Slots 2 and 3 are NOT skins - they read 000000FF, 19E3CE92,
						// plain garbage - and queueing them filled the 256-entry
						// do-not-retry list before a single real skin reached it,
						// which is why the first attempt pulled nothing at all.
						if (s < 2 && si < 0 && nSlotRaw[s])
							QueueSkinPull(nSlotRaw[s]);
						const int si2 = (si >= 0) ? si : TextureFor(nSlotRaw[s]);
						if (si2 >= 0 && g_Tex[si2].pSRV)
						{
							pSlot[s] = g_Tex[si2].pSRV;
							fSlotCut[s] = g_Tex[si2].fCutRef;
							nSlotGraded[s] = g_Tex[si2].bGraded;
						}
					}
					// A PUBLISHED NAME WINS OVER A HEAP GUESS - while the engine's
					// model filename for this object still agrees with the one
					// the client created it with. See g_PubSkins.
					if (g_bPubSkins && !g_PubSkins.empty())
					{
						std::unordered_map<uint32_t, PubSkin>::const_iterator it =
							g_PubSkins.find(mi.nObject);
						static long s_nLook = 0;
						if (++s_nLook <= 6)
							Log("  R3D PUB SKIN: lookup object %08X in %u entries -> %s",
								mi.nObject, (unsigned)g_PubSkins.size(),
								(it != g_PubSkins.end()) ? "found" : "NOT FOUND");
						if (it != g_PubSkins.end())
						{
							const uint32_t pName = Word(pModel, 0x04);
							const bool bStr = (MemKind(pName, 8) == 2);
							const bool bSame = bStr
								&& SameFileName((const char*)(uintptr_t)pName, it->second.szModel);
							if (!bSame)
							{
								++g_nPubSkinStale;
								if (g_nPubSkinSaid < 16)
								{
									++g_nPubSkinSaid;
									Log("  R3D PUB SKIN: object %08X published as %s but the engine's model"
										" name reads %s%s%s - not honoured",
										mi.nObject, it->second.szModel,
										bStr ? "'" : "<not a string at model+04>",
										bStr ? (const char*)(uintptr_t)pName : "",
										bStr ? "'" : "");
								}
							}
							else for (int s = 0; s < 2; ++s)
							{
								if (!it->second.szSkin[s][0]) continue;
								DtxInfo bi;
								ID3D11ShaderResourceView* pB = Dtx_Get(g_pDev, it->second.szSkin[s], &bi);
								if (!pB)
								{
									if (g_nPubSkinSaid < 16)
									{
										++g_nPubSkinSaid;
										Log("  R3D PUB SKIN: object %08X %s: the file %s did not load",
											mi.nObject, it->second.szModel, it->second.szSkin[s]);
									}
									continue;
								}
								if (pSlot[s] != pB && g_nPubSkinSaid < 16)
								{
									++g_nPubSkinSaid;
									Log("  R3D PUB SKIN: object %08X %s slot %d -> %s%s",
										mi.nObject, it->second.szModel, s, it->second.szSkin[s],
										pSlot[s] ? "  (the heap had a different picture)" : "");
								}
								pSlot[s] = pB;
								fSlotCut[s] = CutRefOf(bi);
								nSlotGraded[s] = GradedOf(bi);
								++g_nPubSkinHits;
							}
						}
					}
					ID3D11ShaderResourceView* pSkin =
						(!g_bMeshTint && pSlot[0])
							? pSlot[0]
							: ((g_bMeshTint && g_pBoxTex) ? g_pBoxTex : g_pWhite);
					// Is a piece's face index global to the model or local to
					// the piece's own slice? Report the numbers that decide it.
					if (g_bIdxHunt && g_nIdxSaid < 6 && nPieceArr >= 2)
					{
						++g_nIdxSaid;
						Log("");
						Log("=== model at object %08X: %u pieces, %u skinned"
							" vertices ===", mi.nObject, nPieceArr, nSkinned);
						Log("   model+64 %u  model+68 %u  model+6C %u"
							"   records(+5C) %u",
							Word(pModel, 0x64), Word(pModel, 0x68),
							Word(pModel, 0x6C), Word(pModel, 0x5C));
						// The grouping produced 1774 where the model says 505.
						// Show the raw records rather than reasoning about the rule.
						{
							const uint32_t pR = Word(pModel, 0x58);
							const uint32_t nR = Word(pModel, 0x5C);
							for (uint32_t z = 0; z < 12 && z < nR; ++z)
							{
								const uint32_t r = pR + z * 20;
								if (MemKind(r, 20) != 2) break;
								const float* fp = (const float*)(uintptr_t)r;
								Log("     rec %2u  pos (%9.2f %9.2f %9.2f)"
									"  w %.4f  node %u", z, fp[0], fp[1], fp[2],
									fp[3], Word(r, 16));
							}
						}
						// Sweep the offset at which this model's addressed level
						// of detail might begin, and score each by how many
						// vertices land inside the engine's own box.
						{
							const uint32_t nMV = Word(pModel, 0x64);
							if (nMV && nMV <= nSkinned)
							{
								uint32_t nBestOff = 0; long nBestIn = -1;
								const uint32_t nLast = nSkinned - nMV;
								for (uint32_t o = 0; o <= nLast; ++o)
								{
									long nIn = 0;
									for (uint32_t z = 0; z < nMV; ++z)
									{
										const float* s = &skinned[(size_t)(o + z) * 3];
										if (fabsf(s[0] - mi.fPos[0]) <= mi.fDims[0] + 1.0f
										 && fabsf(s[1] - mi.fPos[1]) <= mi.fDims[1] + 1.0f
										 && fabsf(s[2] - mi.fPos[2]) <= mi.fDims[2] + 1.0f)
											++nIn;
									}
									if (nIn > nBestIn) { nBestIn = nIn; nBestOff = o; }
								}
								Log("   LOD SWEEP: %u candidate offsets for %u"
									" vertices; BEST offset %u with %ld of %u"
									" in the box (%.1f%%)",
									nLast + 1, nMV, nBestOff, nBestIn, nMV,
									100.0 * (double)nBestIn / (double)nMV);
								long nAtZero = 0;
								for (uint32_t z = 0; z < nMV; ++z)
								{
									const float* s = &skinned[(size_t)z * 3];
									if (fabsf(s[0] - mi.fPos[0]) <= mi.fDims[0] + 1.0f
									 && fabsf(s[1] - mi.fPos[1]) <= mi.fDims[1] + 1.0f
									 && fabsf(s[2] - mi.fPos[2]) <= mi.fDims[2] + 1.0f)
										++nAtZero;
								}
								Log("              offset 0 scores %ld of %u"
									" (%.1f%%) for comparison", nAtZero, nMV,
									100.0 * (double)nAtZero / (double)nMV);
							}
						}

						uint32_t nSumV = 0;
						for (uint32_t q = 0; q < nPieceArr; ++q)
						{
							const uint32_t pc2 = Word(pPieceArr, q * 4);
							if (MemKind(pc2, 0x80) != 2) continue;
							const uint32_t nPV = Word(pc2, 0x08);
							const uint32_t pF2 = Word(pc2, 0x14);
							const uint32_t nF2 = Word(pc2, 0x18);
							uint32_t nMax = 0;
							if (nF2 && nF2 < 65536 && MemKind(pF2, 32) == 2
								&& MemKind(pF2 + (nF2 - 1) * 32, 32) == 2)
							{
								for (uint32_t z = 0; z < nF2; ++z)
								{
									const uint16_t* hh =
										(const uint16_t*)(uintptr_t)(pF2 + z * 32);
									for (int c = 0; c < 3; ++c)
										if (hh[c] > nMax) nMax = hh[c];
								}
							}
							// What does the PIECE's own array look like? The model
							// array was cracked by dumping it rather than reasoning
							// about it; piece+04 has only ever been guessed at.
							if (q == 0)
							{
								const uint32_t pPV = Word(pc2, 0x04);
								Log("     piece+04 -> %08X, first records as"
									" {pos, w, node}:", pPV);
								for (uint32_t z = 0; z < 6; ++z)
								{
									const uint32_t rr = pPV + z * 20;
									if (MemKind(rr, 20) != 2) break;
									const float* fq = (const float*)(uintptr_t)rr;
									Log("       %2u  (%9.2f %9.2f %9.2f)  w %.4f"
										"  node %u", z, fq[0], fq[1], fq[2], fq[3],
										Word(rr, 16));
								}
								// The PIECE record as raw dwords, each classified.
								// In the .abc format a piece owns several
								// LODs, each with its OWN vertex list. So the 1774
								// grouped vertices are every LOD of every piece, 505 is
								// the sum of the pieces' LOD0 counts, and each piece must
								// say where its own block starts. Reading these fields as
								// floats hid whatever says it.
								Log("     the PIECE record as dwords:");
								for (int b = 0; b < 32; b += 4)
								{
									char szL[300]; szL[0] = 0;
									for (int c = 0; c < 4; ++c)
									{
										const uint32_t vv = Word(pc2, (b + c) * 4);
										const float ff = *(const float*)&vv;
										const char* pk = (MemKind(vv, 4) == 2) ? "heap"
											: (vv < 65536) ? "int " : (ff > -1e6f && ff < 1e6f
												&& (ff > 1e-6f || ff < -1e-6f)) ? "flt " : "??? ";
										char szOne[80];
										sprintf_s(szOne, "+%02X %08X %s  ",
											(b + c) * 4, vv, pk);
										strcat_s(szL, szOne);
									}
									Log("       %s", szL);
								}
								Log("     piece+04 target as dwords (model array is"
									" %08X, %u grouped vertices):",
									Word(pModel, 0x58), nSkinned);
								{
									uint32_t nSmall = 0, nSeen = 0, nMax = 0;
									const uint32_t nPVc = Word(pc2, 0x08);
									for (int b = 0; b < 16; b += 4)
									{
										char szL[300]; szL[0] = 0;
										for (int c = 0; c < 4; ++c)
										{
											const uint32_t vv = Word(pPV, (b + c) * 4);
											const float ff = *(const float*)&vv;
											char szOne[80];
											sprintf_s(szOne, "+%02X %08X(%.2f) ",
												(b + c) * 4, vv, ff);
											strcat_s(szL, szOne);
										}
										Log("       %s", szL);
									}
									// Are they INDICES? Test every stride that divides
									// cleanly, over the piece's whole count.
									for (int stride = 4; stride <= 32; stride += 4)
									{
										nSmall = nSeen = nMax = 0;
										if (MemKind(pPV, 4) != 2
											|| MemKind(pPV + (nPVc - 1) * stride, 4) != 2)
											continue;
										for (uint32_t z = 0; z < nPVc; ++z)
										{
											const uint32_t vv = Word(pPV + z * stride, 0);
											++nSeen;
											if (vv < nSkinned) ++nSmall;
											if (vv > nMax && vv < 0x10000000u) nMax = vv;
										}
										Log("       stride %2d: %u of %u first dwords are"
											" < %u (max %u)%s", stride, nSmall, nSeen,
											nSkinned, nMax,
											(nSmall == nSeen) ? "  <- ALL are valid indices"
															 : "");
									}
								}
								// And as 12-byte plain positions, in case it is not
								// the weighted format at all.
								for (uint32_t z = 0; z < 4; ++z)
								{
									const uint32_t rr = pPV + z * 12;
									if (MemKind(rr, 12) != 2) break;
									const float* fq = (const float*)(uintptr_t)rr;
									Log("       as12 %2u  (%9.2f %9.2f %9.2f)",
										z, fq[0], fq[1], fq[2]);
								}
							}
							Log("   piece %2u '%.16s'  verts(+08) %5u  faces %5u"
								"  MAX INDEX %5u   running base %5u  %s",
								q, (MemKind(pc2 + 0x48, 16) == 2)
									? (const char*)(uintptr_t)(pc2 + 0x48) : "?",
								nPV, nF2, nMax, nSumV,
								(nMax < nPV) ? "fits its OWN slice"
											 : "does NOT fit its own slice");
							nSumV += nPV;
						}
						Log("   sum of piece vertex counts %u against %u skinned"
							"  -> %s", nSumV, nSkinned,
							(nSumV == nSkinned) ? "EXACT: the pieces are"
								" consecutive slices of the model array"
								: "they do not agree");
						Log("=== end ===");
					}

					const UINT nvInstStart = nv;
					// THE LIGHT THIS INSTANCE STANDS IN, looked up once. The
					// grid is coarse and the search expands, so a model in the
					// air above a lit floor still finds it.
					float fInstLight[3] = { 1.0f, 1.0f, 1.0f };
					// The interface scene's card and flower stand at the world
					// origin, which in a loaded level is anywhere at all - a dark
					// cell, as often as not. Retail lights them flat; so do we.
					if (g_bModelLight && !g_bInterfaceOnly)
					{ LGridAt(mi.fPos, fInstLight); LightDirectAt(mi.fPos, fInstLight); }
					const size_t nRunStart   = g_MeshRuns.size();
					const uint32_t nPieces0  = nPieces;
					const uint32_t nTris0    = nTris;
					// What this instance actually drew, against what the engine
					// says it occupies. A beam across the street is an instance
					// spanning many times its own half-extents.
					float fMn[3] = {  1e30f,  1e30f,  1e30f };
					float fMx[3] = { -1e30f, -1e30f, -1e30f };
					bool bDrewAny = false;
					// Where this piece's slice starts in the model's vertex
					// array. Accumulated over the pieces IN ORDER, because the
					// slices are consecutive - measured, not assumed: the six
					// counts sum to model+64 exactly.
					uint32_t nBase = 0;
					// ONE LATE FRAME, EVERY INSTANCE, EVERY PIECE, and the MEAN COLOUR
					// of the picture each one is actually wearing. A near-black object
					// is either wearing a near-black texture or being shaded to
					// nothing, and no log of names alone can tell those apart.
					// +StubSkinFrame <n>.
					// The FIRST frame at or past the number asked for, and only that
					// one - an exact match would print nothing at all if the frame rate
					// meant the number was stepped over.
					static long s_nSkinShot = -1;
					if (g_nSkinFrame > 0 && s_nSkinShot < 0
						&& g_nFrames >= (long)g_nSkinFrame)
						s_nSkinShot = g_nFrames;
					const bool bSkinFrame = (s_nSkinShot >= 0
						&& g_nFrames == s_nSkinShot);
					if (bSkinFrame)
					{
						static long s_nSkinBanner = -1;
						if (s_nSkinBanner != g_nFrames)
						{
							s_nSkinBanner = g_nFrames;
							Log("=== SKINFRAME %ld: %u instances published,"
								" %u nodes ===", g_nFrames, g_Models.nCount,
								g_Models.nNodeCount);
						}
						Log("  SKINFRAME inst %3u  HOBJ %08X  nodes %u  pieces %u"
							"  flags %08X%s%s  pos (%.1f %.1f %.1f)"
							"  dims (%.0f %.0f %.0f)  scale (%.3f %.3f %.3f)",
							i, mi.nObject, mi.nNodeCount, nPieceArr, mi.nFlags,
							(mi.nFlags & VRMODEL_F_NOLIGHT) ? " [nolight]" : "",
							(mi.nFlags & VRMODEL_F_ADDITIVE) ? " [additive]" : "",
							mi.fPos[0], mi.fPos[1], mi.fPos[2],
							mi.fDims[0], mi.fDims[1], mi.fDims[2],
							fSc[0], fSc[1], fSc[2]);
					}
					{ LARGE_INTEGER q; QueryPerformanceCounter(&q); g_qSlots += q.QuadPart - qSl0.QuadPart; }
					LARGE_INTEGER qPc0; QueryPerformanceCounter(&qPc0); (void)qPc0;
					for (uint32_t k = 0; k < nPieceArr; ++k)
					{
						LARGE_INTEGER qPs0; QueryPerformanceCounter(&qPs0);
						const uint32_t pc = Word(pPieceArr, k * 4);
						if (MemKind(pc, 0x80) != 2) { ++rPiece; continue; }

						// THE PLAYER'S HEAD, WHICH YOU ARE INSIDE.
						//
						// Cate's model declares it: across HERO_ACTION,
						// HERO_WINTER, HERO_CASUAL and HERO_SCUBA, every head
						// and eyelash piece is on MATERIAL 1 and every other
						// piece is on material 0. That is the model's own
						// statement about which pieces wear the head skin -
						// the engine has separate body, head and hands skins
						// and CModelButeMgr has a getter for each - so it is
						// not a list fitted to one character.
						//
						// Matched on the name because that is what this
						// renderer can see of a piece; the names carry the same
						// division, head_zTex1* and Eyelash*_zTex2*, in all
						// four outfits.
						if ((mi.nFlags & 1u) && g_bDrawBody && g_bHideHead
							&& MemKind(pc + 0x48, 32) == 2)
						{
							const char* pszP = (const char*)(uintptr_t)(pc + 0x48);
							if (_strnicmp(pszP, "head", 4) == 0
								|| _strnicmp(pszP, "eyelash", 7) == 0)
							{ ++g_nHeadHidden; continue; }
						}

						// THE VIEW MODEL'S ARMS CANNOT BE RIGHT AT A CONTROLLER.
						//
						// NOLF's player-view weapon is one model with the gun
						// AND Cate's hands and sleeves baked into it, authored
						// to sit at a fixed place on a flat screen. Put its
						// origin at the controller - which is where a held gun
						// belongs - and
						// the gun lands correctly while the forearm extends
						// back past the camera and fills a third of the view.
						// That is the big orange slab in the headset.
						//
						// The arm that belongs at a controller is a posed
						// body's arm bent to it with IK, not the view model's
						// sleeve. Until there is one here, the
						// gun is drawn and the arms are not.
						//
						// The PIECE NAME is the model's own declaration, the
						// same kind of rule as the six editor marker textures -
						// not a list fitted to what looked wrong.
						// ...and a VEHICLE's hands stay: they hold the bars,
						// which is where the player's hands are.
						if ((mi.nFlags & VRMODEL_F_VIEWMODEL) && g_bHideViewArms
							&& !(mi.nFlags & VRMODEL_F_VEHICLE)
							&& MemKind(pc + 0x48, 32) == 2
							&& _strnicmp((const char*)(uintptr_t)(pc + 0x48),
										 "Hand", 4) == 0)
						{ ++g_nViewArmsHidden; continue; }
						// A PIECE THE GAME HAS HIDDEN (VRModelInst::nHideMask, the
						// engine's own per-object status for the first 32 pieces).
						if (k < 32 && (mi.nHideMask & (1u << k))) { ++g_nPiecesHidden; continue; }
						// THE RIDDEN VEHICLE AS ITS HANDS ONLY: the whole vehicle is
						// drawn under the rider (VRVehicleBody) with its own bars, so
						// this view model keeps the pieces named Hand* and no others.
						if ((mi.nFlags & VRMODEL_F_HANDSONLY)
							&& !(MemKind(pc + 0x48, 32) == 2
								 && _strnicmp((const char*)(uintptr_t)(pc + 0x48), "Hand", 4) == 0))
							continue;

						// Which skin THIS piece uses. Falls back to slot 0, which is
						// what every piece used before and is right for the body.
						int nPieceSlot = 0;
						if (g_bPieceSkin && MemKind(pc + 0x48, 32) == 2)
							nPieceSlot = PieceSkinSlot((const char*)(uintptr_t)(pc + 0x48));
						// THE PIECE'S OWN MATERIAL INDEX, at +0x24 of the engine's
						// piece record - the .abc's materialIndex. The name rule
						// above is a convention the HERO_* models follow ('_zTex1'),
						// and the GOTY beach Cate (Props\Models\Cate.abc) does not:
						// her 'torso' is material 1 with no suffix, so it took the
						// face skin and she was drawn as stretched skin instead of
						// a green bikini (headset vs retail, 24 September). Read
						// at the desk: Cate head 0 / torso 1, shades 0; a guard's
						// Head_zTex1 1 and every other piece 0 - the name rule's
						// answer wherever it had one. Two slots only (see
						// PieceSkinSlot). +StubPieceMatIndex 0 reverts to names.
						if (g_bPieceSkin && g_bPieceMatIndex && Readable(pc + 0x24, 4))
						{
							const uint32_t nMat = Word(pc, 0x24);
							if (nMat <= 1) nPieceSlot = (int)nMat;
						}
						ID3D11ShaderResourceView* pPieceSkin = pSkin;
						float fPieceCut = 0.0f;
						if (!g_bMeshTint && nPieceSlot > 0 && pSlot[nPieceSlot])
							pPieceSkin = pSlot[nPieceSlot];
						// Counted where the choice is FINAL, not where the slot
						// was read: a piece can resolve its slot and still end
						// up on the white stand-in.
						// THE LAST ROUTE TO A PICTURE, and the only one left
						// when both skin slots read 00000000: there is no
						// texture object, so there is no name to read off one.
						//
						// The MODEL still knows its own filename (model+0x04),
						// and the game's attribute files declare which skin
						// goes with that model - HHModel beside HHSkin. So ask
						// the game. This is the same principle as reading the
						// .dtx and .dat files rather than the heap, and it is
						// NOT the obvious filename transform, which was
						// measured at 49% across the game's 701 models and
						// thrown away.
						//
						// Bounded per frame like every other file pull here.
						if (pPieceSkin == g_pWhite && g_bSkinFromButes
							&& !nSlotRaw[0] && !nSlotRaw[1]
							&& g_nButeTryThisFrame < 4)
						{
							const uint32_t pName = Word(pModel, 0x04);
							if (MemKind(pName, 8) == 2)
							{
								++g_nButeTryThisFrame;
								const char* pszSkin =
									Butes_SkinFor((const char*)(uintptr_t)pName);
								if (pszSkin)
								{
									DtxInfo bi;
									ID3D11ShaderResourceView* pB =
										Dtx_Get(g_pDev, pszSkin, &bi);
									if (pB)
									{
										pPieceSkin = pB;
										++g_nSkinFromButes;
										if (g_nButeSaid < 6)
										{
											++g_nButeSaid;
											Log("  R3D BUTE SKIN: %s -> %s",
												(const char*)(uintptr_t)pName,
												pszSkin);
										}
									}
								}
							}
						}
						++g_nPieceDrawn;
						if (pPieceSkin == g_pWhite)
						{
							++g_nPieceWhite;
							if (mi.nFlags & VRMODEL_F_INVISIBLE) ++g_nInvisWhite;
							// NAME THE FIRST FEW, and do it OUTSIDE the R3D SKIN
							// report - that one is capped at 400 lines and
							// filtered to models it has not seen, so the piece
							// this count is about can easily not be in it. A
							// sweep that says "312 untextured pieces" and cannot
							// say which model is a number nobody can act on.
							// DOES THIS OBJECT STILL KNOW ITS OWN SKIN'S NAME?
							//
							// Both slots read 00000000, so there is no texture
							// object to name and the file route has nothing to
							// look up. But the object was CREATED with
							// SetObjectFilenames(model, skin) - the engine was
							// told both strings - so the question is whether it
							// kept them anywhere we can reach.
							//
							// Same technique that found the sprite route an hour
							// ago: dump the readable strings around the object
							// and the model struct, and let RezFS_Exists decide
							// which are real. A string ending in .dtx is a
							// coincidence; one that also names a mounted file is
							// not.
							if (g_bSkinNameProbe && g_nSkinProbeSaid < 4)
							{
								++g_nSkinProbeSaid;
								Log("  SKIN NAME PROBE: object %08X piece '%.24s'"
									" model %08X",
									mi.nObject,
									(MemKind(pc + 0x48, 32) == 2)
										? (const char*)(uintptr_t)(pc + 0x48) : "?",
									pModel);
								int nHit = 0;
								for (int lv = 0; lv < 2 && nHit < 8; ++lv)
								{
									const uint32_t pB = (lv == 0) ? mi.nObject : pModel;
									if (MemKind(pB, 0x400) != 2) continue;
									for (int w = 0; w < 256 && nHit < 8; ++w)
									{
										const uint32_t v = Word(pB, w * 4);
										if (!v || MemKind(v, 8) != 2) continue;
										const char* q = (const char*)(uintptr_t)v;
										int c = 0;
										while (c < 79 && q[c] >= 32 && q[c] <= 126) ++c;
										if (c < 6 || q[c] != 0) continue;
										char sz[80];
										memcpy(sz, q, (size_t)c); sz[c] = 0;
										const bool bFile = RezFS_Exists(sz);
										if (!bFile && !strstr(sz, ".dtx")
											&& !strstr(sz, ".DTX")
											&& !strstr(sz, ".abc")
											&& !strstr(sz, ".ABC")) continue;
										++nHit;
										Log("      %s +%03X -> \"%s\"%s",
											lv ? "model " : "object", w * 4, sz,
											bFile ? "   <- IS A MOUNTED FILE" : "");
									}
								}
								if (!nHit)
									Log("      no filename reachable from 256"
										" words of the object or the model");
							}
							if (g_nWhiteSaid < 20)
							{
								++g_nWhiteSaid;
								// WHERE IT IS AND HOW BIG, because the count
								// alone cannot say whether anybody can see it.
								// M14S02 reported 1828 untextured pieces and it
								// is SIX OBJECTS drawn over three hundred
								// frames - a per-frame tally reads like a
								// catastrophe and is six props.
								Log("  R3D WHITE PIECE: object %08X piece %u"
									" '%.24s' -> slot %d, slots %08X %08X"
									"  at (%.0f %.0f %.0f) dims (%.1f %.1f %.1f)"
									"  (the engine bound no skin for this)",
									mi.nObject, k,
									(MemKind(pc + 0x48, 32) == 2)
										? (const char*)(uintptr_t)(pc + 0x48) : "?",
									nPieceSlot, nSlotRaw[0], nSlotRaw[1],
									mi.fPos[0], mi.fPos[1], mi.fPos[2],
									mi.fDims[0], mi.fDims[1], mi.fDims[2]);
							}
						}
							// Which texture entry this run ended up with decides the alpha test.
							int nPieceTex = -1;
							int nPieceGraded = 0;
							{
								const int ti = TexIndexOfSRV(pPieceSkin);
								if (ti >= 0)
								{ fPieceCut = g_Tex[ti].fCutRef;
								  nPieceGraded = g_Tex[ti].bGraded;
								  nPieceTex = ti; }
							}
							// A skin loaded straight from its file has no
							// entry; its slot carries the classification.
							if (nPieceTex < 0 && pPieceSkin == pSlot[nPieceSlot]
								&& fSlotCut[nPieceSlot] >= 0.0f)
							{
								fPieceCut = fSlotCut[nPieceSlot];
								nPieceGraded = nSlotGraded[nPieceSlot] > 0 ? 1 : 0;
							}
							// A CHROME OBJECT'S ALPHA IS ITS REFLECTION MASK, NOT
							// A CUT-OUT. The M08S02 boss (German_Action, whose
							// model butes say EnvironmentMap TRUE) wears a skin
							// with no alpharef whose alpha is 17.8% zero and
							// otherwise 255 - bimodal, so the pixel guess alpha
							// tested it and cut holes in her armor. Only the
							// GUESS is dropped: every guessed cut is exactly
							// 0.5, and a declared "alpharef N" is N/255, which
							// never is. +StubEnvNoCut 0 reverts.
							if (g_bEnvNoCut && (mi.nFlags & VRMODEL_F_ENVMAP)
								&& fPieceCut == 0.5f)
							{
								fPieceCut = 0.0f;
								if (g_nEnvNoCutPieces++ < 8)
									Log("  R3D ENV NO CUT: object %08X piece %d wears %s - chrome,"
										" so its alpha is a reflection mask; guessed cut dropped",
										mi.nObject, k, nPieceTex >= 0 ? g_Tex[nPieceTex].szName : "a file-loaded skin");
							}
							// WHICH PICTURE THIS PIECE IS ACTUALLY WEARING, by
							// name. Cate draws navy and olive on the main menu
							// while her skin file is red and white, and no
							// amount of looking at either end of the chain can
							// say whether the wrong texture is bound or the
							// right one is not.
							if (g_bCutProbe)
							{
								static int s_nSaidSkin = 0;
								if (s_nSaidSkin < 24)
								{
									++s_nSaidSkin;
									Log("    PIECE SKIN: '%.20s' -> SRV %p  %s",
										(MemKind(pc + 0x48, 32) == 2)
											? (const char*)(uintptr_t)(pc + 0x48) : "?",
										(void*)pPieceSkin,
										(nPieceTex >= 0)
											? g_Tex[nPieceTex].szName
											: (pPieceSkin ? "<SRV IS IN NO TEXTURE ENTRY>"
														  : "<NO SKIN AT ALL - white stand-in>"));
								}
							}
							if (bSkinFrame)
							{
								char szTex[200];
								if (nPieceTex >= 0)
									sprintf_s(szTex, "'%.60s'  %.0fx%.0f"
										"  MEAN (%3.0f %3.0f %3.0f)",
										g_Tex[nPieceTex].szName, g_Tex[nPieceTex].fW,
										g_Tex[nPieceTex].fH, g_Tex[nPieceTex].mr,
										g_Tex[nPieceTex].mg, g_Tex[nPieceTex].mb);
								else
									strcpy_s(szTex, pPieceSkin
										? "<SRV IS IN NO TEXTURE ENTRY>"
										: "<NO SKIN AT ALL - white stand-in>");
								Log("  SKINFRAME    piece %2u '%.20s'  slot %d"
									"  SRV %p  %s  cutref %.2f", k,
									(MemKind(pc + 0x48, 32) == 2)
										? (const char*)(uintptr_t)(pc + 0x48) : "?",
									nPieceSlot, (void*)pPieceSkin, szTex, fPieceCut);
							}

						// DOES THIS PIECE STILL GET THE SAME PICTURE IT GOT ON THE
						// FIRST LOAD? That is the reported symptom stated exactly - always
						// random textures, each reload swapping them - and it is
						// the only question that matters, because a skin can
						// resolve perfectly and still be the wrong one.
						//
						// A piece's NAME is a stable key across a reload; the
						// texture object pointer is not, which is the whole bug.
						// The cached entry's mean colour identifies the picture
						// well enough to see one swapped for another - the skins
						// of two different characters are not the same colour.
						//
						// +StubSkinConsist 1. Off by default: it is a linear scan
						// per piece and this file has now been taught twice what
						// that costs in a per-frame path.
						if (g_bSkinConsist && MemKind(pc + 0x48, 32) == 2)
						{
							const char* pn = (const char*)(uintptr_t)(pc + 0x48);
							float cr = -1.0f, cg = -1.0f, cb = -1.0f;
							{
								const int ti = TexIndexOfSRV(pPieceSkin);
								if (ti >= 0)
									{ cr = g_Tex[ti].mr; cg = g_Tex[ti].mg;
									  cb = g_Tex[ti].mb; }
							}
							if (cr >= 0.0f)
							{
								int nSlotIx = -1;
								for (size_t z = 0; z < g_SkinRef.size(); ++z)
									if (!strncmp(g_SkinRef[z].name, pn, 19))
										{ nSlotIx = (int)z; break; }
								if (nSlotIx < 0)
								{
									if (g_SkinRef.size() < 128)
									{
										SkinRef sr{};
										strncpy(sr.name, pn, 19);
										sr.r = cr; sr.g = cg; sr.b = cb;
										sr.nLoad = g_nWorldLoads;
										g_SkinRef.push_back(sr);
									}
								}
								else
								{
									SkinRef& sr = g_SkinRef[nSlotIx];
									++g_nConsistSeen;
									const float dd = fabsf(sr.r - cr) + fabsf(sr.g - cg)
												   + fabsf(sr.b - cb);
									if (dd > 8.0f)
									{
										++g_nConsistBad;
										if (g_nConsistSaid < 10)
										{
											++g_nConsistSaid;
											Log("  R3D SKINSWAP: piece '%.19s' had"
												" (%.0f %.0f %.0f) on load %ld, now"
												" (%.0f %.0f %.0f) on load %ld",
												sr.name, sr.r, sr.g, sr.b, sr.nLoad,
												cr, cg, cb, g_nWorldLoads);
										}
										sr.r = cr; sr.g = cg; sr.b = cb;
										sr.nLoad = g_nWorldLoads;
									}
								}
							}
						}
						const UINT nvPieceStart = nv;

						// The piece's OWN vertex count. Its faces are relative
						// to its slice, so this is the bound an index must
						// satisfy - not the whole model's count.
						const uint32_t nPV = Word(pc, 0x08);
						const uint32_t nV = g_bPieceBase ? nPV : nSkinned;
						const uint32_t nOff = g_bPieceBase ? nBase : 0;
						nBase += nPV;

						// (the per-piece work below is the expensive part, so the
					// cull above it is what pays)

					// ---- the piece's vertex ENTRY array ----------------
						//
						// 32 bytes each: a pointer into the model's record
						// array and a uint16 count. No grouping, no base, no
						// LOD arithmetic - the model stores the mapping and
						// every other reading was trying to rebuild it.
						static std::vector<float> vaskin;
						static std::vector<float> vanrm;
						static std::vector<uint8_t> vaok;
						static std::vector<uint8_t> vabad;
						// The vertex's own BIND position (entry +0x08), and
						// whether it is blended. Both feed the edge instrument.
						static std::vector<float> vabind;
						static std::vector<uint8_t> vablend;
						bool bVAOK = false;
						LARGE_INTEGER qSk0; QueryPerformanceCounter(&qSk0);
						g_qPieceSetup += qSk0.QuadPart - qPs0.QuadPart;
						if (g_bPieceVA && nPV && nPV < 100000)
						{
							const uint32_t pVA = Word(pc, 0x04);
							const uint32_t pMR = Word(pModel, 0x58);
							const uint32_t nMR = Word(pModel, 0x5C);
							// Validate the ENTRY array at both ends once, and
							// bound every record pointer by ARITHMETIC against
							// the model array. Never a syscall per vertex: that
							// mistake cost 89 fps -> 4 and made the game run at
							// fifteen times speed.
							// ONE VirtualQuery for the whole entry array, not one
							// per end. A syscall in a per-piece path is 292 of
							// them a frame here; per-VERTEX it was 9 million a
							// second once and took the renderer to 4 fps.
							if (nMR && nMR < 400000
								&& MemKind(pVA, nPV * 32) == 2)
							{
								const uint32_t nLo = pMR;
								const uint32_t nHi = pMR + nMR * 20;
								vaskin.assign((size_t)nPV * 3, 0.0f);
								vanrm.assign((size_t)nPV * 3, 0.0f);
								// Which vertices actually resolved. Without
								// this, an unresolved one stays at (0,0,0) and
								// every face touching it draws a blade to the
								// world origin.
								vaok.assign((size_t)nPV, 0);
								if (g_bSkinDiag || g_bEdgeChk)
								{
									vabad.assign((size_t)nPV, 0);
									vabind.assign((size_t)nPV * 3, 0.0f);
									vablend.assign((size_t)nPV, 0);
								}
								else vabad.clear();
								uint32_t nGood = 0;
								// Per PIECE. These were globals, so the previous vertex
								// could belong to another NPC entirely - both have a
								// node 3 - and the bind and world distances then had
								// nothing to do with each other. That is where a worst
								// case of 7305 units came from.
								g_nPrevNode = 0xFFFFFFFFu;
								for (uint32_t vi = 0; vi < nPV; ++vi)
								{
									const uint32_t e = pVA + vi * 32;
									// Direct: the whole entry array passed MemKind above.
									const uint32_t pR2 = *(const uint32_t*)(uintptr_t)e;
									// ONE contiguous run, printed as 20-byte records with the vertex each
									// belongs to. Only the EVEN vertices came back implausible, each
									// claiming a single record - so print the neighbours and let the
									// interleaving show itself rather than inferring it from addresses.
									// A PIECE HIDE MASK. ILTModel has GetPieceHideStatus and the header says
									// "only supports the first 32 pieces", so the engine keeps a 32-bit mask
									// per object and we have never looked at it. The piece names say the same
									// thing: this character has THREE torsos and two Right_leg variants and no
									// left leg, which is a wardrobe, not a body. Drawing all of it at once is
									// bulky, panelled, and does not change as the character animates - which
									// is exactly what headset testing reports.
									if (g_nHideDumped < 3 && mi.nNodeCount >= 20 && vi == 0)
									{
										++g_nHideDumped;
										Log("  R3D HIDE: object %08X, %u pieces - object dwords"
											" +0x180..+0x200 (skins are +1B8/+1BC, model +1DC):",
											mi.nObject, nPieceArr);
										for (uint32_t z = 0x180; z < 0x200; z += 0x20)
										{
											Log("    +%03X  %08X %08X %08X %08X %08X %08X %08X %08X", z,
												Word(mi.nObject, z),      Word(mi.nObject, z + 4),
												Word(mi.nObject, z + 8),  Word(mi.nObject, z + 12),
												Word(mi.nObject, z + 16), Word(mi.nObject, z + 20),
												Word(mi.nObject, z + 24), Word(mi.nObject, z + 28));
										}
										// And the model's own head, where a per-model mask would live.
										for (uint32_t z = 0x00; z < 0x40; z += 0x20)
										{
											Log("    model +%03X  %08X %08X %08X %08X %08X %08X %08X %08X",
												z, Word(pModel, z),      Word(pModel, z + 4),
												Word(pModel, z + 8),  Word(pModel, z + 12),
												Word(pModel, z + 16), Word(pModel, z + 20),
												Word(pModel, z + 24), Word(pModel, z + 28));
										}
									}
									if (!g_nRunDumped && mi.nNodeCount >= 20 && vi == 0 && nPV > 8)
									{
										g_nRunDumped = 1;
										Log("  R3D RUN: object %08X, %u nodes, piece has %u vertices;"
											" entry array at %08X, model records at %08X",
											mi.nObject, mi.nNodeCount, nPV, pVA, pMR);
										for (uint32_t z = 0; z < 10 && z < nPV; ++z)
										{
											const uint32_t eZ = pVA + z * 32;
											const uint32_t pZ = Word(eZ, 0);
											const uint32_t cZ = *(const uint16_t*)(uintptr_t)(eZ + 4);
											Log("    entry %2u: ptr %08X count %u  (next ptr %08X,"
												" gap %d bytes)", z, pZ, cZ, Word(eZ + 32, 0),
												(int)(Word(eZ + 32, 0) - pZ));
										}
										const uint32_t p0 = Word(pVA, 0);
										for (uint32_t z = 0; z < 12; ++z)
										{
											const uint32_t q = p0 + z * 20;
											if (MemKind(q, 20) != 2) break;
											const float* f = (const float*)(uintptr_t)q;
											Log("    record %2u at %08X: pos (%8.2f %8.2f %8.2f)"
												"  w %7.4f  node %u", z, q, f[0], f[1], f[2], f[3],
												*(const uint32_t*)(uintptr_t)(q + 16));
										}
									}
									uint32_t nR2 =
										*(const uint16_t*)(uintptr_t)(e + 4);
									// The next entry's records start where this entry's end, so the gap
									// IS a count - but only while the entries are in record order, and
									// each LOD has its own entry array over the SAME record list.
									if (g_nEntryCount != 1 && vi + 1 < nPV)
									{
										const uint32_t pNext = *(const uint32_t*)(uintptr_t)(e + 32);
										if (pNext > pR2)
										{
											const uint32_t nGap = (pNext - pR2) / 20;
											if (nGap >= 1 && nGap <= 8 && (pNext - pR2) % 20 == 0)
												nR2 = nGap;
										}
									}
									if (!nR2 || nR2 > 8) continue;
									if (pR2 < nLo || pR2 + nR2 * 20 > nHi) continue;
									float ac2[3] = { 0, 0, 0 };
									float fWSum = 0.0f;
									bool bAllRecs = true;
									// The 32-byte entry is {ptr, count, float3 pos, float3 normal}.
									// 4 + 4 + 12 + 12 fills it exactly, and the second float3 is
									// unit length - counted below rather than asserted.
									const float* pEN = (const float*)(uintptr_t)(e + 0x14);
									float an2[3] = { 0, 0, 0 };
									uint32_t nBestNode = 0xFFFFFFFFu; float fBestW = -1.0f;
									if (g_bSkinDiag)
									{
										const float lq = pEN[0]*pEN[0] + pEN[1]*pEN[1]
											+ pEN[2]*pEN[2];
										++g_nNrmSeen;
										if (lq > 0.98f && lq < 1.02f) ++g_nNrmUnit;
									}
									for (uint32_t rr = 0; rr < nR2; ++rr)
									{
										const uint32_t q3 = pR2 + rr * 20;
										const float* v3 = (const float*)(uintptr_t)q3;
										const uint32_t n3 =
											*(const uint32_t*)(uintptr_t)(q3 + 16);
										// A node the client never published. The
										// vertex cannot be placed, and a partial
										// weighted sum is not a position - it is
										// this vertex dragged toward the origin.
										if (n3 >= mi.nNodeCount) { bAllRecs = false; break; }
										const float* m3 =
											g_Models.nodes[mi.nNodeFirst + n3].m;
										// Checked once per instance (s_NodeOk), not
										// once per record: 1 NaN, 2 unposed.
										if (n3 < s_NodeOk.size() && s_NodeOk[n3])
										{
											if (s_NodeOk[n3] == 1) ++g_nNodeNaN; else ++g_nUnposed;
											bAllRecs = false; break;
										}
										// An UNPOSED node: the client publishes a
										// zero matrix rather than skipping it, so
										// that the indices after it stay aligned.
										// No real basis is all zero. Placing a
										// vertex with it would put it at the world
										// origin, so the vertex is unresolved.
										// (the unposed test now lives in s_NodeOk)
										// A WEIGHT, not a scale. The old path
										// checked this and the rewrite lost it;
										// a raw dump of these records has shown
										// values like 9.96, and multiplying a
										// bone-space position by ten is exactly
										// a vertex thrown across the level.
										if (g_bSkinDiag)
										{
											++g_nRecSeen;
											const float lp = v3[0]*v3[0] + v3[1]*v3[1] + v3[2]*v3[2];
											const bool bNaN = !(lp == lp) || !(v3[3] == v3[3]);
											if (bNaN) ++g_nRecNaN;
											else if (lp > 3600.0f) ++g_nRecHuge;   // |p| > 60 units
											if (mi.nNodeCount >= 20 && !bNaN)
											{
												const float lq = sqrtf(lp);
												++g_nChrRec; g_fChrSum += lq;
												if (lq > g_fChrMax) g_fChrMax = lq;
												if (lq > 60.0f) ++g_nChrHuge;
											}
											// CHARACTERS only - the first dump caught a large prop,
											// whose 90-unit offsets are entirely legitimate.
											if ((bNaN || lp > 3600.0f) && mi.nNodeCount >= 20
												&& g_nRecDumped < 12)
											{
												++g_nRecDumped;
												Log("  R3D BADREC: object %08X vertex %u record %u/%u at %08X"
													"  raw %08X %08X %08X %08X %08X"
													"  -> pos (%.1f %.1f %.1f) w %.4f node %u  %s",
													mi.nObject, vi, rr, nR2, q3,
													Word(q3, 0), Word(q3, 4), Word(q3, 8),
													Word(q3, 12), Word(q3, 16),
													v3[0], v3[1], v3[2], v3[3],
													*(const uint32_t*)(uintptr_t)(q3 + 16),
													bNaN ? "NaN" : "HUGE");
											}
										}
										const float w4 = v3[3];
										if (!(w4 == w4) || w4 < -0.01f || w4 > 1.01f)
											{ bAllRecs = false; ++g_nBadWeight; break; }
										fWSum += w4;
										if (w4 > fBestW) { fBestW = w4; nBestNode = n3; }
										// The stored offset is already scaled by the bias, so
										// only the TRANSLATION gets multiplied by it here.
										// g_bWeightPre 0 restores the old double scaling.
										const float wt = g_bWeightPre ? 1.0f : w4;
										const float wo = w4;
										// Model-space offset, so the object scale applies here and
										// not to the node translation, which is already world space.
										const float s0 = v3[0] * fSc[0];
										const float s1 = v3[1] * fSc[1];
										const float s2 = v3[2] * fSc[2];
										if (bScaleNU)
										{
											// Rotate the offset UNSCALED, scale it in
											// the object's own frame, rotate it back.
											// See the note where bScaleNU is set.
											const float rx = g_bNodeT
												? (m3[0]*v3[0] + m3[4]*v3[1] + m3[8] *v3[2])
												: (m3[0]*v3[0] + m3[1]*v3[1] + m3[2] *v3[2]);
											const float ry = g_bNodeT
												? (m3[1]*v3[0] + m3[5]*v3[1] + m3[9] *v3[2])
												: (m3[4]*v3[0] + m3[5]*v3[1] + m3[6] *v3[2]);
											const float rz = g_bNodeT
												? (m3[2]*v3[0] + m3[6]*v3[1] + m3[10]*v3[2])
												: (m3[8]*v3[0] + m3[9]*v3[1] + m3[10]*v3[2]);
											const float ox = (rx*fOR[0] + ry*fOR[1] + rz*fOR[2]) * fSc[0];
											const float oy = (rx*fOU[0] + ry*fOU[1] + rz*fOU[2]) * fSc[1];
											const float oz = (rx*fOF[0] + ry*fOF[1] + rz*fOF[2]) * fSc[2];
											ac2[0] += wt * (ox*fOR[0] + oy*fOU[0] + oz*fOF[0]) + wo*m3[3];
											ac2[1] += wt * (ox*fOR[1] + oy*fOU[1] + oz*fOF[1]) + wo*m3[7];
											ac2[2] += wt * (ox*fOR[2] + oy*fOU[2] + oz*fOF[2]) + wo*m3[11];
										}
										else if (g_bNodeT)
										{
											ac2[0] += wt * (m3[0]*s0 + m3[4]*s1 + m3[8] *s2) + wo*m3[3];
											ac2[1] += wt * (m3[1]*s0 + m3[5]*s1 + m3[9] *s2) + wo*m3[7];
											ac2[2] += wt * (m3[2]*s0 + m3[6]*s1 + m3[10]*s2) + wo*m3[11];
										}
										else
										{
											ac2[0] += wt * (m3[0]*s0 + m3[1]*s1 + m3[2] *s2) + wo*m3[3];
											ac2[1] += wt * (m3[4]*s0 + m3[5]*s1 + m3[6] *s2) + wo*m3[7];
											ac2[2] += wt * (m3[8]*s0 + m3[9]*s1 + m3[10]*s2) + wo*m3[11];
										}
										// The normal rides the same bones, rotation only - no
										// translation, or it would stop being a direction.
										//
										// DELIBERATELY NOT SCALED, and that is unchanged
										// from before the object-space fix above. A
										// non-uniform scale needs the inverse transpose
										// on a normal, and neither path has ever applied
										// one - so this is a known gap, not a regression,
										// and it costs lighting accuracy rather than
										// shape. The object it was found on is additive
										// and flagged NOLIGHT, so it has no normal to get
										// wrong; do not fix this without a lit
										// non-uniform model to measure it against.
										an2[0] += w4 * (m3[0]*pEN[0] + m3[1]*pEN[1] + m3[2] *pEN[2]);
										an2[1] += w4 * (m3[4]*pEN[0] + m3[5]*pEN[1] + m3[6] *pEN[2]);
										an2[2] += w4 * (m3[8]*pEN[0] + m3[9]*pEN[1] + m3[10]*pEN[2]);
									}
									// The weights we actually used, whatever the
									// record count turned out to be. Dividing by
									// their total makes this an average of bone
									// positions rather than a sum, so the vertex
									// is inside the hull of the bones that claim
									// it and cannot be thrown across the level.
									if (g_bSkinDiag && bAllRecs) ++g_nWSeen;
									if (g_bSkinDiag && bAllRecs && (fWSum < 0.9f || fWSum > 1.1f))
										++g_nBadWSum;		// counted, not fatal
									if (g_bSkinDiag && bAllRecs)
									{
										const bool bLast = (vi + 1 >= nPV);
										if (bLast) ++g_nLastSeen;
										if (fWSum < 0.9f || fWSum > 1.1f)
										{
											if (bLast) ++g_nBadLast; else ++g_nBadMid;
											if (vi < vabad.size()) vabad[vi] = 1;
										}
									}
									if (!bAllRecs) continue;
									if (fWSum < 0.01f) continue;   // no bone at all
									if (fWSum < 0.999f || fWSum > 1.001f)
									{
										const float fInv = 1.0f / fWSum;
										ac2[0] *= fInv; ac2[1] *= fInv; ac2[2] *= fInv;
									}
									vaskin[(size_t)vi * 3 + 0] = ac2[0];
									vaskin[(size_t)vi * 3 + 1] = ac2[1];
									vaskin[(size_t)vi * 3 + 2] = ac2[2];
									// Normalise rather than divide by the weight sum: a blend of
									// unit normals is not unit, and only the direction matters.
									{
										const float nl = sqrtf(an2[0]*an2[0] + an2[1]*an2[1]
											+ an2[2]*an2[2]);
										if (nl > 1e-6f)
										{
											vanrm[(size_t)vi * 3 + 0] = an2[0] / nl;
											vanrm[(size_t)vi * 3 + 1] = an2[1] / nl;
											vanrm[(size_t)vi * 3 + 2] = an2[2] / nl;
										}
									}
									// The bone with the largest weight, and how far the finished vertex
									// landed from it. Cheap: one distance per vertex, no syscalls.
									if (g_bSkinDiag && nBestNode < mi.nNodeCount)
									{
										const float* mB = g_Models.nodes[mi.nNodeFirst + nBestNode].m;
										const float dxB = ac2[0] - mB[3];
										const float dyB = ac2[1] - mB[7];
										const float dzB = ac2[2] - mB[11];
										const float dB = sqrtf(dxB*dxB + dyB*dyB + dzB*dzB);
										g_fBoneDistSum += dB; ++g_nBoneDist;
										if (dB > g_fBoneDistMax) g_fBoneDistMax = dB;
										// A NOLF character is about 106 units tall, so a vertex more than 40
										// units from its own strongest bone is not on that bone's limb.
										if (dB > 40.0f) ++g_nBoneFar;
									}
									// Only RIGID vertices: one record, weight 1.0, so the bind position is
									// unambiguous and the transform is a single bone.
									if (g_bSkinDiag && nR2 == 1 && fWSum > 0.999f && fWSum < 1.001f
										&& nBestNode < mi.nNodeCount)
									{
										const float* pB2 = (const float*)(uintptr_t)(e + 0x08);
										if (g_nPrevNode == nBestNode)
										{
											const float bx = pB2[0] - g_fPrevBind[0];
											const float by = pB2[1] - g_fPrevBind[1];
											const float bz = pB2[2] - g_fPrevBind[2];
											const float wx = ac2[0] - g_fPrevWorld[0];
											const float wy = ac2[1] - g_fPrevWorld[1];
											const float wz = ac2[2] - g_fPrevWorld[2];
											const float dB2 = sqrtf(bx*bx + by*by + bz*bz);
											const float dW2 = sqrtf(wx*wx + wy*wy + wz*wz);
											if (dB2 > 0.5f && dB2 == dB2 && dW2 == dW2)
											{
												const float er = fabsf(dW2 - dB2);
												++g_nRigidPairs;
												g_fRigidErr += er;
												g_fRigidRel += er / dB2;
												if (er / dB2 > 0.05f) ++g_nRigidBad;
												if (er > g_fRigidWorst) g_fRigidWorst = er;
											}
										}
										g_nPrevNode = nBestNode;
										g_fPrevBind[0] = pB2[0]; g_fPrevBind[1] = pB2[1];
										g_fPrevBind[2] = pB2[2];
										g_fPrevWorld[0] = ac2[0]; g_fPrevWorld[1] = ac2[1];
										g_fPrevWorld[2] = ac2[2];
									}
									if (g_bSkinDiag || g_bEdgeChk)
									{
										const float* pB3 = (const float*)(uintptr_t)(e + 0x08);
										vabind[(size_t)vi * 3 + 0] = pB3[0];
										vabind[(size_t)vi * 3 + 1] = pB3[1];
										vabind[(size_t)vi * 3 + 2] = pB3[2];
										vablend[vi] = (nR2 > 1) ? 1 : 0;
									}
									vaok[vi] = 1;
									++nGood;
								}
								bVAOK = (nGood * 20 >= nPV * 19);	// 95%
								if (bVAOK) ++g_nVAOK; else ++g_nVABad;

								// ---- +StubBeamDump 1 -----------------------
								//
								// THE VAN'S HEADLIGHTS DRAW AS TALL VERTICAL
								// SHAFTS, and R3D DEFORMED could never name them
								// because it scores an instance against the
								// SMALLEST instance of the same model - and both
								// beams are wrong by the same factor, so the ratio
								// is 1.0 and the report stays silent. A test that
								// is relative to the population cannot see a fault
								// the whole population has.
								//
								// The answer key is the FILE.
								// SFX/BEAMS/MODELS/HEADLIGHTBEAM3.ABC is 3 nodes
								// and 2 pieces - 'cyl2_1' weighted entirely to node
								// 1, 'cyl2_2' entirely to node 2 - and each piece's
								// bind box is 320.4 x 130.9 x 1015.5. So this dump
								// prints the BIND box beside the WORLD box: if the
								// bind box matches the file, the mesh arrived
								// intact and the transform is what stretched it.
								//
								// Keyed on ADDITIVE because that is what a light
								// beam is, and this level has exactly two.
								bool bProbeThis = false;
								if (g_szModelProbe[0])
								{
									const uint32_t pMN = Word(pModel, 0x04);
									if (MemKind(pMN, 8) == 2)
									{
										// The engine spells these with
										// backslashes and in mixed case; the
										// probe is a plain case-insensitive
										// substring so "lamp" finds
										// PROPS\MODELS\LAMP_03.ABC.
										char szU[128]; size_t u = 0;
										const char* pS = (const char*)(uintptr_t)pMN;
										for (; u < sizeof szU - 1 && pS[u]; ++u)
											szU[u] = (char)toupper((unsigned char)pS[u]);
										szU[u] = 0;
										bProbeThis = (strstr(szU, g_szModelProbe) != nullptr);
									}
								}
								if ((g_bBeamDump
									 && (mi.nFlags & VRMODEL_F_ADDITIVE)
									 && g_nBeamSaid < 24)
									|| (bProbeThis && g_nBeamSaid < 24))
								{
									++g_nBeamSaid;
									float bLo[3] = { 1e30f, 1e30f, 1e30f };
									float bHi[3] = { -1e30f, -1e30f, -1e30f };
									float wLo[3] = { 1e30f, 1e30f, 1e30f };
									float wHi[3] = { -1e30f, -1e30f, -1e30f };
									for (uint32_t z = 0; z < nPV; ++z)
									{
										if (!vaok[z]) continue;
										for (int c = 0; c < 3; ++c)
										{
											const float b = vabind[(size_t)z * 3 + c];
											const float w = vaskin[(size_t)z * 3 + c];
											if (b < bLo[c]) bLo[c] = b;
											if (b > bHi[c]) bHi[c] = b;
											if (w < wLo[c]) wLo[c] = w;
											if (w > wHi[c]) wHi[c] = w;
										}
									}
									Log("  R3D BEAM: object %08X model %08X piece"
										" '%.20s' (%u of %u)  %u verts, %u resolved",
										mi.nObject, pModel,
										(MemKind(pc + 0x48, 16) == 2)
											? (const char*)(uintptr_t)(pc + 0x48) : "?",
										k, nPieceArr, nPV, nGood);
									if (nGood)
									{
										Log("      BIND  span (%.1f %.1f %.1f)"
											"   min (%.1f %.1f %.1f) max (%.1f %.1f %.1f)",
											bHi[0]-bLo[0], bHi[1]-bLo[1], bHi[2]-bLo[2],
											bLo[0], bLo[1], bLo[2], bHi[0], bHi[1], bHi[2]);
										Log("      WORLD span (%.1f %.1f %.1f)"
											"   min (%.1f %.1f %.1f) max (%.1f %.1f %.1f)",
											wHi[0]-wLo[0], wHi[1]-wLo[1], wHi[2]-wLo[2],
											wLo[0], wLo[1], wLo[2], wHi[0], wHi[1], wHi[2]);
									}
									Log("      object at (%.1f %.1f %.1f) dims"
										" (%.0f %.0f %.0f) scale (%.3f %.3f %.3f)"
										" %u nodes, flags %08X",
										mi.fPos[0], mi.fPos[1], mi.fPos[2],
										mi.fDims[0], mi.fDims[1], mi.fDims[2],
										fSc[0], fSc[1], fSc[2],
										mi.nNodeCount, mi.nFlags);
									Log("      object rot (%.4f %.4f %.4f %.4f)"
										"  scale applied in %s space",
										mi.fRot[0], mi.fRot[1], mi.fRot[2],
										mi.fRot[3],
										bScaleNU ? "OBJECT" : "bone");
									for (uint32_t z = 0; z < mi.nNodeCount
											&& z < 8; ++z)
									{
										const float* m6 =
											g_Models.nodes[mi.nNodeFirst + z].m;
										// The basis row lengths say whether a node
										// carries a SCALE, which is the one thing
										// that turns a 1015-unit beam into a
										// 1600-unit one without moving it.
										const float l0 = sqrtf(m6[0]*m6[0]
											+ m6[1]*m6[1] + m6[2]*m6[2]);
										const float l1 = sqrtf(m6[4]*m6[4]
											+ m6[5]*m6[5] + m6[6]*m6[6]);
										const float l2 = sqrtf(m6[8]*m6[8]
											+ m6[9]*m6[9] + m6[10]*m6[10]);
										Log("      node %u  at (%.1f %.1f %.1f)"
											"  basis rows [%.3f %.3f %.3f | %.3f"
											" %.3f %.3f | %.3f %.3f %.3f]"
											"  row lengths %.3f %.3f %.3f", z,
											m6[3], m6[7], m6[11],
											m6[0], m6[1], m6[2],
											m6[4], m6[5], m6[6],
											m6[8], m6[9], m6[10],
											l0, l1, l2);
									}
								}
							}
							else ++g_nVABad;
						}
						{ LARGE_INTEGER q; QueryPerformanceCounter(&q); g_qSkinLoop += q.QuadPart - qSk0.QuadPart; }

						// The piece's OWN records, grouped to exactly nPV
						// vertices. A vertex costs one to several records, so
						// the array is walked until nPV vertices exist, never
						// for a fixed record count - that is the mistake that
						// made piece+04 look empty before.
						static std::vector<float> pskin;
						bool bPieceOK = false;
						if (g_bPieceVerts && nPV && nPV < 100000)
						{
							const uint32_t pPR = Word(pc, 0x04);
							// Validate generously at both ends, ONCE. Never a
							// syscall per record: that cost 89 fps -> 4 once.
							const uint32_t nMaxRec = nPV * 8;
							if (MemKind(pPR, 20) == 2
								&& MemKind(pPR + (nMaxRec - 1) * 20, 20) == 2)
							{
								pskin.assign((size_t)nPV * 3, 0.0f);
								uint32_t nMade = 0;
								float w2 = 0.0f, ac[3] = { 0, 0, 0 };
								for (uint32_t r2 = 0; r2 < nMaxRec && nMade < nPV; ++r2)
								{
									const uint32_t q2 = pPR + r2 * 20;
									const float*   v2 = (const float*)(uintptr_t)q2;
									const uint32_t n2 = *(const uint32_t*)(uintptr_t)(q2 + 16);
									const float    w3 = v2[3];
									if (!(w3 == w3) || w3 < -0.01f || w3 > 1.01f) break;
									if (n2 < mi.nNodeCount)
									{
										const float* m2 = g_Models.nodes[mi.nNodeFirst + n2].m;
										// See g_bWeightPre: the offset is pre-scaled by the bias.
										const float w3t = g_bWeightPre ? 1.0f : w3;
										ac[0] += w3t * (m2[0]*v2[0] + m2[1]*v2[1] + m2[2] *v2[2]) + w3*m2[3];
										ac[1] += w3t * (m2[4]*v2[0] + m2[5]*v2[1] + m2[6] *v2[2]) + w3*m2[7];
										ac[2] += w3t * (m2[8]*v2[0] + m2[9]*v2[1] + m2[10]*v2[2]) + w3*m2[11];
									}
									w2 += w3;
									if (w2 > 0.99f)
									{
										pskin[(size_t)nMade * 3 + 0] = ac[0];
										pskin[(size_t)nMade * 3 + 1] = ac[1];
										pskin[(size_t)nMade * 3 + 2] = ac[2];
										++nMade;
										ac[0] = ac[1] = ac[2] = 0.0f;
										w2 = 0.0f;
									}
								}
								bPieceOK = (nMade == nPV);
								if (!bPieceOK) ++g_nPieceShort;
							}
						}
						const uint32_t pF = Word(pc, 0x14), nF = Word(pc, 0x18);
						if (!nF || nF > 65535) { ++rCounts; continue; }
						if (MemKind(pF, 32) != 2 ||
							MemKind(pF + (nF - 1) * 32, 32) != 2) { ++rMem; continue; }

						// --- emit the faces ---
						LARGE_INTEGER qEm0; QueryPerformanceCounter(&qEm0);
						for (uint32_t fi = 0; fi < nF; ++fi)
						{
							if (nv + 3 > nMaxV) { ++g_nMeshCap; break; }
							const uint32_t q = pF + fi * 32;
							const uint16_t* h = (const uint16_t*)(uintptr_t)q;
							const float*    f = (const float*)(uintptr_t)(q + 8);
							const uint32_t nVUse = bVAOK ? nPV : nV;
							if (h[0] >= nVUse || h[1] >= nVUse || h[2] >= nVUse)
								{ ++nBadIdx; if (nPieceArr >= 2) ++g_nBadIdxM; continue; }
							// Drop a face naming a vertex that never resolved,
							// rather than drawing it from the world origin.
							if (bVAOK && (!vaok[h[0]] || !vaok[h[1]] || !vaok[h[2]]))
								{ ++g_nOriginFaces; continue; }
							// Into the model's array, at this piece's slice.
							const uint32_t i0 = nOff + h[0];
							const uint32_t i1 = nOff + h[1];
							const uint32_t i2 = nOff + h[2];
							if (!bVAOK && (i0 >= nSkinned || i1 >= nSkinned
										|| i2 >= nSkinned))
								{ ++nBadIdx; continue; }

							const float* a = bVAOK ? &vaskin[(size_t)h[0] * 3]
										 : bPieceOK ? &pskin[(size_t)h[0] * 3]
													: &skinned[(size_t)i0 * 3];
							const float* b = bVAOK ? &vaskin[(size_t)h[1] * 3]
										 : bPieceOK ? &pskin[(size_t)h[1] * 3]
													: &skinned[(size_t)i1 * 3];
							const float* c = bVAOK ? &vaskin[(size_t)h[2] * 3]
										 : bPieceOK ? &pskin[(size_t)h[2] * 3]
													: &skinned[(size_t)i2 * 3];

							// Bind edge against world edge. Squared throughout -
							// one sqrt at report time, none in the frame - and
							// split by whether the triangle touches a blended
							// vertex, so the rigid half is its own control.
							//
							// OFF BY DEFAULT, because it is 24 float operations
							// on EVERY triangle of every model in every eye -
							// 32000 triangles twice a frame at 90 Hz - and this
							// project has learned that a diagnostic in a
							// per-frame path costs frame rate in every arm
							// including the control. It did its job: it proved
							// the weighting fix and it is still the way to
							// prove the next one. +StubEdgeChk 1 turns it on.
							if (g_bEdgeChk && bVAOK)
							{
								const float* pbA = &vabind[(size_t)h[0] * 3];
								const float* pbB = &vabind[(size_t)h[1] * 3];
								const float* pbC = &vabind[(size_t)h[2] * 3];
								const float* pwv[3] = { a, b, c };
								const float* pbv[3] = { pbA, pbB, pbC };
								const bool bBlend = (vablend[h[0]] | vablend[h[1]]
													| vablend[h[2]]) != 0;
								double dB = 0.0, dW = 0.0;
								for (int z = 0; z < 3; ++z)
								{
									const int y = (z + 1) % 3;
									const float b0 = pbv[z][0] - pbv[y][0];
									const float b1 = pbv[z][1] - pbv[y][1];
									const float b2 = pbv[z][2] - pbv[y][2];
									const float w0 = pwv[z][0] - pwv[y][0];
									const float w1 = pwv[z][1] - pwv[y][1];
									const float w2e = pwv[z][2] - pwv[y][2];
									dB += b0*b0 + b1*b1 + b2*b2;
									dW += w0*w0 + w1*w1 + w2e*w2e;
								}
								if (bBlend) { g_fEdgeBindB += dB; g_fEdgeWorldB += dW; ++g_nEdgeB; }
								else        { g_fEdgeBindR += dB; g_fEdgeWorldR += dW; ++g_nEdgeR; }
							}

							if (g_bIdxHunt)
							{
								const float* pv4[3] = { a, b, c };
								for (int z = 0; z < 3; ++z)
									for (int w5 = 0; w5 < 3; ++w5)
									{
										if (pv4[z][w5] < fMn[w5]) fMn[w5] = pv4[z][w5];
										if (pv4[z][w5] > fMx[w5]) fMx[w5] = pv4[z][w5];
									}
							}

							// Self-check: does this triangle sit inside the box
							// the engine reports for the object?
							if (g_bIdxHunt)
							{
								const float* pv3[3] = { a, b, c };
								const bool bMulti = (nPieceArr >= 2);
								if (bMulti) ++g_nFacesM;
								for (int z = 0; z < 3; ++z)
								{
									const bool bIn =
										fabsf(pv3[z][0] - mi.fPos[0]) <= mi.fDims[0] + 1.0f
									 && fabsf(pv3[z][1] - mi.fPos[1]) <= mi.fDims[1] + 1.0f
									 && fabsf(pv3[z][2] - mi.fPos[2]) <= mi.fDims[2] + 1.0f;
									if (bIn) ++g_nInBox; else ++g_nOutBox;
									if (bMulti) { if (bIn) ++g_nInBoxM; else ++g_nOutBoxM; }
								}
							}

							float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
							float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
							float nx = e1[1]*e2[2] - e1[2]*e2[1];
							float ny = e1[2]*e2[0] - e1[0]*e2[2];
							float nz = e1[0]*e2[1] - e1[1]*e2[0];
							const float ln = sqrtf(nx*nx + ny*ny + nz*nz);
							if (ln > 1e-6f) { nx /= ln; ny /= ln; nz /= ln; }

							const float* pv[3] = { a, b, c };
							// Per-vertex normals where the entry array gave us them;
							// the face normal is the fallback, and flat shading is what
							// made a low-poly character look faceted and deformed.
							const float* pn[3] = { 0, 0, 0 };
							if (g_bVtxNrm && bVAOK)
							{
								pn[0] = &vanrm[(size_t)h[0] * 3];
								pn[1] = &vanrm[(size_t)h[1] * 3];
								pn[2] = &vanrm[(size_t)h[2] * 3];
								for (int t = 0; t < 3; ++t)
									if (pn[t][0] == 0.0f && pn[t][1] == 0.0f
										&& pn[t][2] == 0.0f) { pn[0] = 0; break; }
							}
							if (pn[0]) ++g_nTriSmooth; else ++g_nTriFlat;
							if (g_bSkinDiag && mi.nNodeCount >= 20)
							{
								const float* q3v[3] = { a, b, c };
								for (int u = 0; u < 3; ++u)
								{
									const float* s1 = q3v[u];
									const float* s2 = q3v[(u + 1) % 3];
									const float ex = s1[0]-s2[0], ey = s1[1]-s2[1];
									const float ez = s1[2]-s2[2];
									const float el = sqrtf(ex*ex + ey*ey + ez*ez);
									if (!(el == el)) continue;
									++g_nEdges; g_fEdgeSum += el;
									if (el > g_fEdgeMax) g_fEdgeMax = el;
									if (el > 30.0f) ++g_nEdgeLong;
									if (el > 30.0f)
									{
										const bool bB = bVAOK && !vabad.empty()
											&& (vabad[h[u]] || vabad[h[(u + 1) % 3]]);
										if (bB) ++g_nLongBad; else ++g_nLongClean;
									}
								}
							}
							for (int t = 0; t < 3; ++t)
							{
								Vtx& o = out[nv++];
								o.x = pv[t][0]; o.y = pv[t][1]; o.z = pv[t][2];
								if (pn[0])
								{ o.nx = pn[t][0]; o.ny = pn[t][1]; o.nz = pn[t][2]; }
								else { o.nx = nx; o.ny = ny; o.nz = nz; }
								if (pn[0] && g_bIdxHunt)
								{
									// The shader's own directional term, both ways.
									const float dl[3] = { 0.35f, 0.85f, 0.40f };
									const float dn = 1.0f / sqrtf(dl[0]*dl[0] + dl[1]*dl[1]
										+ dl[2]*dl[2]);
									const float dv = fabsf((pn[t][0]*dl[0] + pn[t][1]*dl[1]
										+ pn[t][2]*dl[2]) * dn);
									const float fv = fabsf((nx*dl[0] + ny*dl[1] + nz*dl[2]) * dn);
									const double dd = fabs((0.55 + 0.45 * dv)
										- (0.55 + 0.45 * fv));
									g_fNrmLumSum += dd; ++g_nNrmLumCnt;
									if (dd > g_fNrmLumMax) g_fNrmLumMax = dd;
								}
								o.u = f[t * 2]; o.v = f[t * 2 + 1];
								// -1 is "no lightmap, use the stand-in shade"; -2 is
								// "this lights itself", which is what the sprite pass
								// already uses. An object the engine was told NOT to
								// light wants the second: there is no light in the
								// interface scene, so the stand-in shade drew the menu's
								// flower motifs near-black on top of the logo.
								o.lu = (mi.nFlags & VRMODEL_F_NOLIGHT) ? -2.0f : -1.0f;
								o.lv = -1.0f;
							}
							++nTris;
						}
						{ LARGE_INTEGER q; QueryPerformanceCounter(&q); g_qEmit += q.QuadPart - qEm0.QuadPart; }
						++nPieces;
						bDrewAny = true;
						// One run per piece, because the skin can change between them.
						// An additive instance cannot share a run with a normal one: the
						// blend state is set per run.
						int nAddThis = (g_bAddBlend
							&& (mi.nFlags & VRMODEL_F_ADDITIVE)) ? 1 : 0;
						// A SEE-THROUGH SKIN IS DRAWN SEE-THROUGH. The
						// helicopter's rotor-blur disc and the convertible's
						// windscreen wear a texture whose alpha is a graded
						// film (PREFER4444, not bimodal) - the same test that
						// sends a world batch to the glass pass. Drawn opaque,
						// every clear texel shows the black under it: the
						// 12 September headset batch has a black quadrilateral around
						// every helicopter and a black rear window on the car.
						// 3 = alpha-blended, last, depth tested and not written.
						// Additive and multiply keep precedence; the piece's
						// texture entry was found above for the alpha test.
						// AND AN OBJECT THE GAME MADE TRANSLUCENT. The client
						// publishes GetObjectColor's alpha in fColour[3]; the
						// helicopter's rotor disc is a cut-out skin on an
						// object whose alpha is below 1, which is how retail
						// draws it as a grey blur rather than solid blades.
						float fPieceA = 1.0f;
						if (g_bModelGlass && mi.fColour[3] > 0.0f && mi.fColour[3] < 0.99f)
							fPieceA = mi.fColour[3];
						// NOT AN ENVIRONMENT-MAPPED OBJECT. Its skin's graded
						// alpha is the reflection mask (VRMODEL_F_ENVMAP):
						// the snowmobile's body drew as a film over a black
						// hole until this. The object's own alpha still counts.
						// +StubModelWatch <text>: for instances whose published model
						// file contains <text>, say what each piece is about to become.
						if (g_szModelWatch[0] && g_nModelWatchSaid < 200)
						{
							std::unordered_map<uint32_t, PubSkin>::const_iterator itw = g_PubSkins.find(mi.nObject);
							// The ENGINE's model name too, so an instance with no published
							// skin is still seen (and said to have none).
							const uint32_t pMdlW = Word(mi.nObject, 0x1DC);
							const uint32_t pNmW = pMdlW ? Word(pMdlW, 0x04) : 0;
							const char* pszEng = (pNmW && MemKind(pNmW, 8) == 2) ? (const char*)(uintptr_t)pNmW : "";
							const bool bPubW = (itw != g_PubSkins.end());
							char szLow[96] = ""; strncpy(szLow, pszEng, 95); _strlwr(szLow);
							if ((bPubW && strstr(itw->second.szModel, g_szModelWatch)) || strstr(szLow, g_szModelWatch))
							{
								++g_nModelWatchSaid;
								Log("  R3D MODEL WATCH f=%ld: object %08X '%s' pub %s piece %d at %.0f %.0f %.0f flags %08X alpha %.2f graded %d add %d skin %s",
									(long)g_nFrames, mi.nObject, pszEng, bPubW ? "yes" : "NO", k, mi.fPos[0], mi.fPos[1], mi.fPos[2],
									mi.nFlags, mi.fColour[3], nPieceGraded ? 1 : 0, nAddThis,
									nPieceTex >= 0 ? g_Tex[nPieceTex].szName : "(file-loaded or none)");
								// THE PIECE RECORD ITSELF, to find where the engine keeps the
								// piece's material (texture slot) index.
								if (g_nModelWatchSaid <= 16 && Readable(pc, 0x80))
								{
									char szW[1024]; int o = 0;
									for (uint32_t w = 0; w < 0x80 && o < 1000; w += 4)
										o += sprintf_s(szW + o, sizeof(szW) - o, "%s%02X:%08X", w ? " " : "", w, Word(pc, w));
									Log("    piece %d '%.20s' record: %s", k,
										(MemKind(pc + 0x48, 32) == 2) ? (const char*)(uintptr_t)(pc + 0x48) : "?", szW);
								}
							}
						}
						const int nGradedGlass = (nPieceGraded
							&& !(mi.nFlags & VRMODEL_F_ENVMAP)) ? 1 : 0;
						if (g_bModelGlass && !nAddThis
							&& (nGradedGlass || fPieceA < 1.0f))
						{
							nAddThis = 3;
							++g_nMeshGlassPieces;
							if (g_nMeshGlassSaid < 12)
							{
								++g_nMeshGlassSaid;
								Log("  R3D MODEL GLASS: object %08X piece %d wears %s"
									" (%s%s%.2f) - drawn alpha-blended, last",
									mi.nObject, k,
									nPieceTex >= 0 ? g_Tex[nPieceTex].szName
												   : "a file-loaded skin",
									nPieceGraded ? "graded alpha, " : "",
									"object alpha ", fPieceA);
							}
						}
						// MULTIPLY is checked SECOND, so it wins an instance that
						// somehow carries both - the same precedence the sprite
						// pass uses, and for the same reason: the client clears
						// one when it sets the other, so the order only decides a
						// case that cannot happen. +StubMulBlend 0 puts these
						// back to opaque, which is where they were until now.
						if (g_bMulBlend && g_pBlendMul
							&& (mi.nFlags & VRMODEL_F_MULTIPLY)) nAddThis = 2;
						if (g_bMulForce && g_pBlendMul) nAddThis = 2;
						// Adjacent pieces sharing a skin still coalesce, so a model with
						// one skin costs exactly what it did before.
						if (nv > nvPieceStart)
						{
							if (!g_MeshRuns.empty()
								&& g_MeshRuns.back().pSRV == pPieceSkin
								&& g_MeshRuns.back().fCutRef == fPieceCut
							&& g_MeshRuns.back().nBlend == nAddThis
								&& g_MeshRuns.back().fA == fPieceA
								&& g_MeshRuns.back().nView == ((mi.nFlags & VRMODEL_F_VIEWMODEL) ? 1 : 0)
								&& g_MeshRuns.back().fLight[0] == fInstLight[0]
								&& g_MeshRuns.back().fLight[1] == fInstLight[1]
								&& g_MeshRuns.back().fLight[2] == fInstLight[2]
								&& g_MeshRuns.back().nVB == 0
								&& g_MeshRuns.back().nStart + g_MeshRuns.back().nCount
									== nvPieceStart)
								g_MeshRuns.back().nCount += nv - nvPieceStart;
							else
							{
								MeshRun r; r.nStart = nvPieceStart;
								r.nCount = nv - nvPieceStart; r.pSRV = pPieceSkin;
								r.fCutRef = fPieceCut; r.nBlend = nAddThis;
								r.fA = fPieceA;
								r.nView = (mi.nFlags & VRMODEL_F_VIEWMODEL) ? 1 : 0;
								r.fLight[0] = fInstLight[0];
								r.fLight[1] = fInstLight[1];
								r.fLight[2] = fInstLight[2];
								r.nVB = 0;
								g_MeshRuns.push_back(r);
							}
						}
						// WHICH LOD ARE WE DRAWING? A piece holds one {vtable, pointer, count}
						// block per LOD, 0x6C apart, and we take the FIRST - on the assumption
						// that it is the finest, from a single piece where +08 read 182 and +74
						// read 142. Two samples and an assumption. Headset testing says the characters
						// look like the far-distance LOD, which is exactly what drawing the
						// wrong end of this chain would do, so print the whole chain.
						if (g_nLodSaid < 40 && nPieceArr >= 2)
						{
							++g_nLodSaid;
							char szL[256]; int nAt = 0;
							szL[0] = 0;
							for (int L = 0; L < 8; ++L)
							{
								const uint32_t pB = pc + (uint32_t)L * 0x6C;
								if (MemKind(pB, 0x6C) != 2) break;
								const uint32_t nV2 = Word(pB, 0x08);
								const uint32_t nF2 = Word(pB, 0x18);
								if (!nV2 || nV2 > 200000) break;
								nAt += sprintf_s(szL + nAt, sizeof szL - nAt,
									"  LOD%d %uv/%uf", L, nV2, nF2);
								if (nAt > 200) break;
							}
							Log("  R3D LOD: object %08X piece %u '%.20s' ->%s",
								mi.nObject, k,
								(MemKind(pc + 0x48, 32) == 2)
									? (const char*)(uintptr_t)(pc + 0x48) : "?", szL);
						}
						if (g_nSkinSaid < 400 && nPieceArr >= 1 && SkinModelNew(pModel, k))
						{
							++g_nSkinSaid;
							Log("  R3D SKIN: object %08X piece %u '%.24s'"
								" -> slot %d  %s  DRAWS %s  (slots %08X %08X %08X %08X,"
								" resolved %d%d%d%d)", mi.nObject, k,
								(MemKind(pc + 0x48, 32) == 2)
									? (const char*)(uintptr_t)(pc + 0x48) : "?",
								nPieceSlot,
								(pPieceSkin == g_pWhite) ? "WHITE - nothing bound"
									: (pPieceSkin == pSlot[0] ? "slot 0 texture"
										: "its own texture"),
								SkinSrvName(pPieceSkin),
								nSlotRaw[0], nSlotRaw[1], nSlotRaw[2], nSlotRaw[3],
								pSlot[0] ? 1 : 0, pSlot[1] ? 1 : 0,
								pSlot[2] ? 1 : 0, pSlot[3] ? 1 : 0);
						}
					}
					// The MeshRun push below is the DRAW LIST, so this test may only
					// ask whether anything was drawn. fMx is filled by the per-face
					// bounding box, which is behind +StubMeshIdx - gating the draw on
					// it meant models were built and never submitted with the flag off.
					LARGE_INTEGER qDA0; QueryPerformanceCounter(&qDA0);
					g_qPieceLoop += qDA0.QuadPart - qPc0.QuadPart;
					if (bDrewAny)
					{
						// Does what we drew fit the box the engine reports?
						const float fSp[3] = { fMx[0] - fMn[0], fMx[1] - fMn[1],
											   fMx[2] - fMn[2] };
						const float fBox[3] = { mi.fDims[0] * 2.0f + 1.0f,
												mi.fDims[1] * 2.0f + 1.0f,
												mi.fDims[2] * 2.0f + 1.0f };
						(void)fBox;
						// fSp above is meaningless without the bounding box; every use
						// of it is inside here.
						if (fMx[0] > -1e29f)
						{
							const float fD2 = sqrtf(fSp[0]*fSp[0] + fSp[1]*fSp[1]
												  + fSp[2]*fSp[2]);
							int nAt = -1;
							if (g_nBig < 10) nAt = g_nBig++;
							else
							{
								int nMinI = 0;
								for (int z = 1; z < 10; ++z)
									if (g_aBig[z].fDiag < g_aBig[nMinI].fDiag) nMinI = z;
								if (fD2 > g_aBig[nMinI].fDiag) nAt = nMinI;
							}
							// A character cannot legitimately span thousands of
							// units. Print the transforms that put it there.
							if (fD2 > 2000.0f && g_nNodeDumps < 3)
							{
								++g_nNodeDumps;
								Log("  R3D NODE DUMP: object %08X spans %.0f units"
									" from %u nodes, at (%.0f %.0f %.0f)",
									mi.nObject, fD2, mi.nNodeCount,
									mi.fPos[0], mi.fPos[1], mi.fPos[2]);
								for (uint32_t z = 0; z < mi.nNodeCount && z < 32; ++z)
								{
									const float* m5 =
										g_Models.nodes[mi.nNodeFirst + z].m;
									const float dx5 = m5[3] - mi.fPos[0];
									const float dy5 = m5[7] - mi.fPos[1];
									const float dz5 = m5[11] - mi.fPos[2];
									const float d5 = sqrtf(dx5*dx5 + dy5*dy5 + dz5*dz5);
									Log("      node %2u  at (%9.1f %9.1f %9.1f)"
										"  %8.1f from the object%s", z,
										m5[3], m5[7], m5[11], d5,
										(d5 > 200.0f) ? "   <- NOWHERE NEAR IT" : "");
								}
							}
							if (nAt >= 0)
							{
								g_aBig[nAt].pObj = mi.nObject;
								g_aBig[nAt].pModel = pModel;
								g_aBig[nAt].fDiag = fD2;
								for (int z = 0; z < 3; ++z)
								{
									g_aBig[nAt].fPos[z] = mi.fPos[z];
									g_aBig[nAt].fDims[z] = mi.fDims[z];
								}
								g_aBig[nAt].nNodes = mi.nNodeCount;
								g_aBig[nAt].nPieces = nPieceArr;
							}
						}
						// The instance's own diagonal, against the smallest this
						// MODEL has ever drawn. Collision dims are not visual
						// bounds and judging against them named 26 innocent
						// objects; another instance of the same mesh is a fair
						// comparison.
						const float fDiag = sqrtf(fSp[0]*fSp[0] + fSp[1]*fSp[1]
												+ fSp[2]*fSp[2]);
						int nSlot = -1;
						for (int z = 0; z < g_nSpans; ++z)
							if (g_aSpan[z].pModel == pModel) { nSlot = z; break; }
						if (nSlot < 0 && g_nSpans < 256)
						{
							nSlot = g_nSpans++;
							g_aSpan[nSlot].pModel = pModel;
							g_aSpan[nSlot].fMin = fDiag;
						}
						float fWorst = 0.0f; int nAxis = 0;
						for (int z = 0; z < 3; ++z)
							if (fSp[z] > fSp[nAxis]) nAxis = z;
						if (nSlot >= 0)
						{
							if (fDiag < g_aSpan[nSlot].fMin) g_aSpan[nSlot].fMin = fDiag;
							fWorst = g_aSpan[nSlot].fMin > 0.01f
								? fDiag / g_aSpan[nSlot].fMin : 0.0f;
						}
						if (fWorst > 3.0f)
						{
							bool bSeen = false;
							for (int z = 0; z < g_nNamed; ++z)
								if (g_aNamed[z] == mi.nObject) { bSeen = true; break; }
							if (!bSeen && g_nNamed < 64)
							{
								g_aNamed[g_nNamed++] = mi.nObject;
								Log("  R3D DEFORMED: object %08X (model %08X) is"
									" %.1f x the SMALLEST this model has drawn"
									" (%.0f units against %.0f); longest axis %d"
									" = %.0f; at (%.0f %.0f %.0f), dims"
									" (%.0f %.0f %.0f), %u nodes, %u pieces",
									mi.nObject, pModel, fWorst, fDiag,
									g_aSpan[nSlot].fMin, nAxis, fSp[nAxis],
									mi.fPos[0], mi.fPos[1], mi.fPos[2],
									mi.fDims[0], mi.fDims[1], mi.fDims[2],
									mi.nNodeCount, nPieceArr);
								for (uint32_t z = 0; z < nPieceArr && z < 12; ++z)
								{
									const uint32_t pc3 = Word(pPieceArr, z * 4);
									if (MemKind(pc3 + 0x48, 16) != 2) continue;
									Log("      piece %u '%.20s'  %u verts", z,
										(const char*)(uintptr_t)(pc3 + 0x48),
										Word(pc3, 0x08));
								}
							}
						}
						// The runs are pushed per PIECE now, in the loop above.
						++nInst; nSkinTotal += nSkinned;
						{ LARGE_INTEGER q; QueryPerformanceCounter(&q); g_qMissPath += q.QuadPart - qMiss0.QuadPart; ++g_nMissInst; }

						// KEEP WHAT WAS JUST BUILT. Only from the CPU buffer -
						// see the note where `out` is chosen - and only for an
						// instance that actually produced vertices.
						LARGE_INTEGER qSt0; QueryPerformanceCounter(&qSt0);
						if (g_bSkinCache && out != pGpuOut && nv > nvInstStart)
						{
							++g_nSkinMiss;
							const size_t nBytes =
								(size_t)(nv - nvInstStart) * sizeof(Vtx);
							if (g_nSkinBytes + nBytes > kSkinCacheMax)
								FlushSkinCache();
							CachedInst& c = g_SkinCache[mi.nObject];
							g_nSkinBytes -= c.v.size() * sizeof(Vtx);
							c.pModel  = pModel;
							c.nHash   = nSkelHash;
							c.nTexGen = g_nTexGen;
							c.nPieces = nPieces - nPieces0;
							c.nTris   = nTris   - nTris0;
							c.v.assign(out + nvInstStart, out + nv);
							// NEW VERTICES, SO THE POOL COPY IS NOT THEM. See nPoolHash.
							if (c.nPoolGen == g_nPoolGen) ++g_nPoolStale;
							c.nPoolGen = 0;
							c.runs.clear();
							for (size_t z = nRunStart; z < g_MeshRuns.size(); ++z)
							{
								MeshRun r = g_MeshRuns[z];
								r.nStart -= nvInstStart;
								c.runs.push_back(r);
							}
							g_nSkinBytes += nBytes;
						}
						{ LARGE_INTEGER q; QueryPerformanceCounter(&q); g_qStore += q.QuadPart - qSt0.QuadPart; }
					}
				}

				if (out != pGpuOut && nv)
					memcpy(pGpuOut, out, (size_t)nv * sizeof(Vtx));
				g_pCtx->Unmap(g_pMeshVB, 0);
				QueryPerformanceCounter(&qT1);
				if (qFreq.QuadPart)
				{
					const double ms = 1000.0 * (double)(qT1.QuadPart - qT0.QuadPart)
									/ (double)qFreq.QuadPart;
					g_fMeshMsSum += ms; ++g_nMeshMsCnt;
					if (ms > g_fMeshMsMax) g_fMeshMsMax = ms;
					g_nMeshReads += (g_nReadCalls - nReads0);
				}
				s_nBuiltFrame = g_Models.nFrame;
				s_nBuiltVerts = nv;
				// Runs, not ring vertices: a frame whose instances were ALL
				// resident in the pool has nv == 0 and everything to draw.
				if (!g_MeshRuns.empty())
				{
					if (!g_pOcc[0])
					{
						D3D11_QUERY_DESC qd{};
						qd.Query = D3D11_QUERY_OCCLUSION;
						g_pDev->CreateQuery(&qd, &g_pOcc[0]);
						g_pDev->CreateQuery(&qd, &g_pOcc[1]);
					}

					// Read the OTHER query first - it is a frame old by now, so
					// the answer is already sitting there and nothing waits.
					const int nOther = g_nOccCur ^ 1;
					if (g_pOcc[nOther] && g_bOccArmed[nOther])
					{
						UINT64 nPassed = 0;
						if (g_pCtx->GetData(g_pOcc[nOther], &nPassed, sizeof nPassed,
											D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
						{
							g_bOccArmed[nOther] = 0;
							if (g_nOccReports < 6)
							{
								++g_nOccReports;
								// The FIRST report routinely reads zero: the query is
								// begun on the frame the mesh first builds, before
								// there is a world to test against. Warning on it
								// would hand the next session a false alarm, which is
								// exactly the failure this instrument exists to end.
								Log("  R3D MESH DEPTH: %llu samples of the mesh"
									" PASSED the depth test%s", nPassed,
									(nPassed || g_nOccReports <= 1) ? ""
										: "  <- ZERO: the models are drawn and then"
										  " rejected against the world");
							}
						}
					}

					if (g_pOcc[g_nOccCur]) g_pCtx->Begin(g_pOcc[g_nOccCur]);
					int nVBBound = -1;
					for (size_t r = 0; r < g_MeshRuns.size(); ++r)
					{
						// Left for the blended pass at the end of the frame -
						// additive AND multiply, both of which need what they
						// sit on top of to be in the target already.
						if (g_bAddLast && g_MeshRuns[r].nBlend) continue;
						if (g_bScopePass && g_MeshRuns[r].nView) continue;	// the scope never sees the gun it sits on
						BindRunVB(g_MeshRuns[r].nVB, nVBBound, nRegionBytes);
						g_pCtx->PSSetShaderResources(0, 1, &g_MeshRuns[r].pSRV);
						// A pass must set its own state, and this one changes it
						// per run - so it puts it back on the next run too, not
						// only at the end.
						if (g_pBlendAdd && g_pBlendOpaque)
							g_pCtx->OMSetBlendState(
								(g_MeshRuns[r].nBlend == 2 && g_pBlendMul)
									? g_pBlendMul
								: (g_MeshRuns[r].nBlend == 3 && g_pBlendAlpha)
									? g_pBlendAlpha
								: g_MeshRuns[r].nBlend ? g_pBlendAdd
								: (g_bA2C && g_pBlendA2C && g_MeshRuns[r].fCutRef > 0.0f)
									? g_pBlendA2C		// a leaf: see g_pBlendA2C
								: g_pBlendOpaque,
								kNoFactorM, 0xFFFFFFFF);
						// A multiply run is faded toward WHITE by the shader and
						// skips the fog block; models carry no per-instance fade,
						// so full strength is -1. See SetOutAlpha.
						SetOutAlpha(g_MeshRuns[r].nBlend == 2 ? -1.0f : 1.0f);
						SetModelLight(g_MeshRuns[r].fLight[0],
									  g_MeshRuns[r].fLight[1],
									  g_MeshRuns[r].fLight[2],
									  g_bModelLight ? 1.0f : 0.0f);
						if (g_MeshRuns[r].nBlend == 2) ++g_nMeshMulDrawn;
						if (g_bAlphaTest)
							// +StubAlphaTest 2 tests EVERY run at 0.5, cut-out or
							// not. It separates "the classifier said solid" from
							// "the threshold never reached the draw", and those
							// need opposite repairs.
							SetAlphaCut((g_bAlphaTest > 1 && g_MeshRuns[r].fCutRef <= 0.0f)
										? 0.5f : g_MeshRuns[r].fCutRef);
						g_pCtx->Draw(g_MeshRuns[r].nCount, g_MeshRuns[r].nStart);
					}
					// Only reachable with +StubAddLast 0, but a pass must leave
					// the state it found: a multiply run parks lmp.w negative.
					SetOutAlpha(1.0f);
					SetModelLight(0.0f, 0.0f, 0.0f, 0.0f);
					if (g_pOcc[g_nOccCur])
					{
						g_pCtx->End(g_pOcc[g_nOccCur]);
						g_bOccArmed[g_nOccCur] = 1;
						g_nOccCur ^= 1;
					}
				}
				// Every frame, not once: this is what the dump waits on.
				if (g_bHaveCam && g_Models.nCount)
				{
					float rr[3], uu[3], ff[3];
					QuatBasis(g_fLastQuat, rr, uu, ff);
					g_fNearAz = 999.0f; g_fNearDist = 1e30f;
					for (uint32_t z = 0; z < g_Models.nCount; ++z)
					{
						const VRModelInst& m3 = g_Models.inst[z];
						if (m3.nFlags & 1u) continue;
						if (m3.fDims[0] > 128.0f || m3.fDims[1] > 128.0f
							|| m3.fDims[2] > 128.0f) continue;
						const float dx3 = m3.fPos[0] - g_fLastPos[0];
						const float dy3 = m3.fPos[1] - g_fLastPos[1];
						const float dz3 = m3.fPos[2] - g_fLastPos[2];
						const float d3 = sqrtf(dx3*dx3 + dy3*dy3 + dz3*dz3);
						const float fwd3 = dx3*ff[0] + dy3*ff[1] + dz3*ff[2];
						const float rgt3 = dx3*rr[0] + dy3*rr[1] + dz3*rr[2];
						const float az3 = fabsf(atan2f(rgt3, fwd3)) * 57.2957795f;
						// nearest one that is actually in front
						if (fwd3 > 0.0f && az3 < 35.0f && d3 < g_fNearDist)
						{ g_fNearDist = d3; g_fNearAz = az3; }
					}
				}

				// WHERE are they? "Drew 37511 triangles" and an empty picture
				// agree perfectly if every model is behind a wall or beyond the
				// view. Distance from the camera the frame was drawn from,
				// nearest first, is the one number that separates those.
				static int s_bSaidWhere = 0;
				if (!s_bSaidWhere && g_bHaveCam && g_Models.nCount)
				{
					s_bSaidWhere = 1;
					float fNear[5] = { 1e30f,1e30f,1e30f,1e30f,1e30f };
					uint32_t iNear[5] = { 0,0,0,0,0 };
					uint32_t nWithin1000 = 0;
					for (uint32_t z = 0; z < g_Models.nCount; ++z)
					{
						const VRModelInst& m2 = g_Models.inst[z];
						const float dx = m2.fPos[0] - g_fLastPos[0];
						const float dy = m2.fPos[1] - g_fLastPos[1];
						const float dz = m2.fPos[2] - g_fLastPos[2];
						const float d = sqrtf(dx*dx + dy*dy + dz*dz);
						if (d < 1000.0f) ++nWithin1000;
						for (int q = 0; q < 5; ++q)
							if (d < fNear[q])
							{
								for (int r2 = 4; r2 > q; --r2)
								{ fNear[r2] = fNear[r2-1]; iNear[r2] = iNear[r2-1]; }
								fNear[q] = d; iNear[q] = z; break;
							}
					}
					Log("  R3D MESH: camera (%.0f %.0f %.0f); %u of %u instances"
						" within 1000 units", g_fLastPos[0], g_fLastPos[1],
						g_fLastPos[2], nWithin1000, g_Models.nCount);
					for (int q = 0; q < 5 && fNear[q] < 1e29f; ++q)
					{
						const VRModelInst& m2 = g_Models.inst[iNear[q]];
						// WHICH WAY to look. Guessing the sign of a yaw costs a
						// run each time; the camera's own basis gives it.
						float rr[3], uu[3], ff[3];
						QuatBasis(g_fLastQuat, rr, uu, ff);
						const float dx2 = m2.fPos[0] - g_fLastPos[0];
						const float dy2 = m2.fPos[1] - g_fLastPos[1];
						const float dz2 = m2.fPos[2] - g_fLastPos[2];
						const float fwd = dx2*ff[0] + dy2*ff[1] + dz2*ff[2];
						const float rgt = dx2*rr[0] + dy2*rr[1] + dz2*rr[2];
						const float upd = dx2*uu[0] + dy2*uu[1] + dz2*uu[2];
						Log("        nearest %d: %.0f units, at (%.0f %.0f %.0f),"
							" dims (%.0f %.0f %.0f), %u nodes"
							"  -> az %+.1f deg, el %+.1f deg%s", q, fNear[q],
							m2.fPos[0], m2.fPos[1], m2.fPos[2],
							m2.fDims[0], m2.fDims[1], m2.fDims[2], m2.nNodeCount,
							atan2f(rgt, fwd) * 57.2957795f,
							atan2f(upd, sqrtf(fwd*fwd + rgt*rgt)) * 57.2957795f,
							(fwd < 0.0f) ? "  (BEHIND)" : "");
					}
				}

				if (g_bTexHunt && !g_bTexHuntSaid && g_nTexHuntPieces >= 200)
				{
					g_bTexHuntSaid = 1;
					Log("");
					Log("=== which offset in a PIECE names its texture ===");
					Log("  %ld pieces walked, %u textures known to the renderer",
						g_nTexHuntPieces, (unsigned)g_Tex.size());
					Log("  offset   direct   via a pointer   (of %ld pieces)",
						g_nTexHuntPieces);
					int nBestOff = -1; long nBest = 0; int bBestDirect = 1;
					for (int off = 0; off < 64; ++off)
					{
						if (!g_aTexHit[off][0] && !g_aTexHit[off][1]) continue;
						Log("   +0x%02X   %6ld   %6ld", off * 4,
							g_aTexHit[off][0], g_aTexHit[off][1]);
						if (g_aTexHit[off][0] > nBest)
						{ nBest = g_aTexHit[off][0]; nBestOff = off; bBestDirect = 1; }
						if (g_aTexHit[off][1] > nBest)
						{ nBest = g_aTexHit[off][1]; nBestOff = off; bBestDirect = 0; }
					}
					Log("  --- the OBJECT, over %ld instances ---", g_nTexHuntObjs);
					for (int off = 0; off < 256; ++off)
						if (g_aObjHit[off][0] || g_aObjHit[off][1])
							Log("   obj+0x%03X   %6ld   %6ld", off * 4,
								g_aObjHit[off][0], g_aObjHit[off][1]);
					Log("  --- the MODEL, over %ld instances ---", g_nTexHuntObjs);
					for (int off = 0; off < 128; ++off)
						if (g_aModHit[off][0] || g_aModHit[off][1])
							Log("   model+0x%02X   %6ld   %6ld", off * 4,
								g_aModHit[off][0], g_aModHit[off][1]);
					if (nBestOff < 0)
						Log("  NOTHING in the PIECE resolves to a bound texture.");
					else
						Log("  BEST: piece+0x%02X %s, %ld of %ld pieces (%.0f%%)",
							nBestOff * 4, bBestDirect ? "IS the texture"
							: "points at a struct holding one", nBest,
							g_nTexHuntPieces,
							100.0 * (double)nBest / (double)g_nTexHuntPieces);
					Log("=== end texture hunt ===");
					Log("");
				}

				// PERIODICALLY, not once. This reported once per run, on the
				// first frame that produced any vertices - which is before the
				// player's weapon exists and before there is a camera pose, so
				// it described a moment nobody was asking about and then never
				// spoke again. The client's model counter had the identical bug
				// and read "0 dropped" through a session in which models were
				// visibly coming and going. Same trap, same session, twice.
				static uint32_t s_nSaidMeshAt = 0;
				if (!nv)
				{
					if (g_Models.nFrame - s_nSaidMeshAt >= 450)
					{
						s_nSaidMeshAt = g_Models.nFrame;
						Log("  R3D MESH: drew NOTHING from %u instances. rejected:"
							" player/at-camera %u, dims %u, nodes %u, model ptr %u,"
							" piece array %u, piece %u, counts %u, mem %u,"
							" SKIN RAN OUT %u",
							g_Models.nCount, rPlayer, rDims, rNodes, rModel,
							rPieceArr, rPiece, rCounts, rMem, rSkin);
					}
				}
				else if (g_Models.nFrame - s_nSaidMeshAt >= 450)
				{
					s_nSaidMeshAt = g_Models.nFrame;
					int nCutRuns = 0;
					for (size_t r2 = 0; r2 < g_MeshRuns.size(); ++r2)
						if (g_MeshRuns[r2].fCutRef > 0.0f) ++nCutRuns;
					// ---- THE MODEL ACCOUNT ---------------------------------
					//
					// The same discipline as the polygon account: every instance the
					// client published lands in exactly one bucket, and UNACCOUNTED is
					// forced to zero. The line above already reported FOUR of the nine
					// rejection reasons, so all four could read healthy while a third
					// of the models silently were not there.
					{
						// FLAG_VISIBLE CLEAR is a bucket like any other. It was not
						// one: the skip took an early `continue` and landed in no
						// counter, so the moment +StubSkipInvisible defaulted to 1
						// this account started reporting a non-zero UNACCOUNTED that
						// was entirely explainable - 10 a frame on T01S03, exactly
						// the 4500-over-450-builds the INVISIBLE line reports. A
						// number that cries wolf is worse than no number: the next
						// real one gets read as this one.
						const uint32_t nNamed = nInst + rPlayer + rDims + rNodes
							+ rModel + rPieceArr + rPiece + rCounts + rMem + rSkin
							+ rBehind + rInvis + rStill;
						const int nLeftM = (int)g_Models.nCount - (int)nNamed;
						g_bAccountThisFrame = 1;
						Log("  R3D MODEL ACCOUNT: %u published, %u dropped at the cap"
							" (%u considered)", g_Models.nCount, g_Models.nDropped,
							g_Models.nCount + g_Models.nDropped);
						if (g_nLightDirectInst)
							Log("  R3D MODEL DIRECT LIGHT: a light object reached %ld of %ld instance lookups (%.0f%%)%s",
								g_nLightDirectHits, g_nLightDirectInst,
								100.0 * (double)g_nLightDirectHits / (double)g_nLightDirectInst,
								g_bLightDirect ? "" : "   <- +StubLightDirect 0: not used");
						Log("     drawn                       %5u", nInst);
						Log("     the player, or at the camera%5u", rPlayer);
						Log("     behind the camera           %5u", rBehind);
						if (rDims)     Log("     too big (over %.0f units)      %5u", g_fModelDimsMax, rDims);
						if (rNodes)    Log("     no skeleton published       %5u", rNodes);
						if (rModel)    Log("     no model behind the object  %5u", rModel);
						if (rPieceArr) Log("     no piece array              %5u", rPieceArr);
						if (rPiece)    Log("     no usable piece             %5u", rPiece);
						if (rCounts)   Log("     impossible counts           %5u", rCounts);
						if (rMem)      Log("     arrays not readable         %5u", rMem);
						if (rSkin)     Log("     nothing skinned             %5u", rSkin);
						if (rInvis)    Log("     FLAG_VISIBLE clear, skipped %5u", rInvis);
						if (rStill)    Log("     skeleton still, SKIPPED     %5u"
										   "   <- +StubSkipStill: a MEASUREMENT,"
										   " the picture is wrong on purpose", rStill);
						Log("     UNACCOUNTED                 %5d%s", nLeftM,
							nLeftM ? "   <- INSTANCES THIS RENDERER CANNOT EXPLAIN"
								 : "");
					}

					// THE COST, on its own line so it cannot be lost in the one
					// above. Mean and worst since the last report, plus the number of
					// IsBadReadPtr probes the build made - if that number is in the
					// millions the cost is address validation, not skinning, and the
					// fix is a different one.
					if (g_nMeshMsCnt)
					{
						Log("  R3D MESH COST: %.3f ms mean, %.3f ms worst, over %ld"
							" builds, %u instances  |  %lld Readable() calls per build"
							" (%.0f per instance)%s",
							g_fMeshMsSum / (double)g_nMeshMsCnt,
							g_fMeshMsMax, g_nMeshMsCnt, g_Models.nCount,
							g_nMeshReads / (long long)g_nMeshMsCnt,
							g_Models.nCount ? (double)g_nMeshReads
								/ ((double)g_nMeshMsCnt * (double)g_Models.nCount) : 0.0,
							g_bFastRead ? "  [FastRead ON]" : "");
						{
							LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
							const double k = qf.QuadPart ? 1000.0 / (double)qf.QuadPart : 0.0;
							Log("     rebuilt instances: %.2f per build, %.3f ms of the mean in their"
								" whole miss path, of which %.3f ms the vertex skinning loop",
								(double)g_nMissInst / (double)g_nMeshMsCnt,
								(double)g_qMissPath * k / (double)g_nMeshMsCnt,
								(double)g_qSkinLoop * k / (double)g_nMeshMsCnt);
							Log("     ... face emission %.3f ms, cache store %.3f ms, before the slots %.3f ms, skin slots %.3f ms",
								(double)g_qEmit * k / (double)g_nMeshMsCnt,
								(double)g_qStore * k / (double)g_nMeshMsCnt,
								(double)g_qPre * k / (double)g_nMeshMsCnt,
								(double)g_qSlots * k / (double)g_nMeshMsCnt);
							Log("     ... MemKind: %.0f calls per build, %.3f ms of VirtualQuery per build",
								(double)g_nMemKindCalls / (double)g_nMeshMsCnt,
								(double)g_qMemKind * k / (double)g_nMeshMsCnt);
							g_nMemKindCalls = 0; g_qMemKind = 0;
							Log("     ... whole piece loop %.3f ms, of which per-piece setup before skinning %.3f ms",
								(double)g_qPieceLoop * k / (double)g_nMeshMsCnt,
								(double)g_qPieceSetup * k / (double)g_nMeshMsCnt);
							g_qMissPath = g_qSkinLoop = g_qEmit = g_qStore = g_qPre = g_qSlots = 0; g_nMissInst = 0;
							g_qPieceLoop = g_qPieceSetup = 0;
						}
						if (g_bFastRead)
							Log("     of those, %lld reached a real VirtualQuery -"
								" the rest were answered from the page cache"
								" (%lld the cache called safe and were NOT)",
								g_nPageProbes, g_nPageStale);
						g_nPageProbes = 0;
						g_fMeshMsSum = 0.0; g_fMeshMsMax = 0.0;
						g_nMeshMsCnt = 0; g_nMeshReads = 0;
					}
					Log("  R3D MESH: %u triangles from %u pieces of %u instances"
						" (%u vertices, %u skinned, %u bad index%s)"
						"  | skipped: at-camera/player %u, dims %u, nodes %u,"
						" skin %u  | %u runs (%d cut-out) | normals %ld of %ld unit"
						"  | smooth tris %ld, flat %ld  | VA ok %ld bad %ld"
						"  | shading vs flat: mean %.4f max %.4f over %ld"
						" | IN BOX %.1f%% (%ld of %ld)",
						nTris, nPieces, nInst, nv,
						nSkinTotal, nBadIdx, g_nMeshCap ? ", BUFFER FULL" : "",
						rPlayer, rDims, rNodes, rSkin,
						(unsigned)g_MeshRuns.size(),
						nCutRuns,
						g_nNrmUnit, g_nNrmSeen,
						g_nTriSmooth, g_nTriFlat, g_nVAOK, g_nVABad,
						g_nNrmLumCnt ? g_fNrmLumSum / (double)g_nNrmLumCnt : 0.0,
						g_fNrmLumMax, g_nNrmLumCnt,
						(g_nInBox + g_nOutBox)
							? 100.0 * (double)g_nInBox
							  / (double)(g_nInBox + g_nOutBox) : 0.0,
						g_nInBox, g_nInBox + g_nOutBox);
					// BUFFER FULL above is sticky - one overflow on the first
					// frames of a level, before anything is cached, marked every
					// later report. Say how many builds overflowed since the last
					// report, and start counting again.
					Log("  R3D MESH RING: %d piece emits cut short by the full %u-vertex ring since the last report",
						g_nMeshCap, nMaxV);
					g_nMeshCap = 0;
					// THE SKIN AUDIT, uncapped, and the line a sweep is read on.
					// "0 with NO texture" is the whole claim; anything else names
					// a level where a model draws the white stand-in and the tester
					// would have found it in a headset instead.
					if (g_bSkinCache || g_nSkinHit)
						Log("  R3D SKIN CACHE: %ld instances served from the"
							" cache, %ld built and stored (%.1f%% hit); %.1f MB"
							" held for %u objects",
							g_nSkinHit, g_nSkinMiss,
							(g_nSkinHit + g_nSkinMiss)
								? 100.0 * (double)g_nSkinHit
									/ (double)(g_nSkinHit + g_nSkinMiss) : 0.0,
							(double)g_nSkinBytes / (1024.0 * 1024.0),
							(unsigned)g_SkinCache.size());
					if (g_pPoolVB)
						Log("  R3D MESH POOL: %ld hits drawn resident, %ld uploads, %ld with no room;"
							" %u of %u vertices used, %ld resets; %ld pool copies retired because"
							" the model changed (each one a ghost before the fix)",
							g_nPoolHit, g_nPoolUp, g_nPoolNoRoom, g_nPoolUsed, kPoolV, g_nPoolResets,
							g_nPoolStale);
					if (g_nInstStill + g_nInstMoved)
						Log("  R3D STILL: %ld of %ld skinned instance-frames had"
							" an UNCHANGED skeleton (%.0f%%) - that is the share"
							" a per-object vertex cache would not have to skin",
							g_nInstStill, g_nInstStill + g_nInstMoved,
							100.0 * (double)g_nInstStill
								/ (double)(g_nInstStill + g_nInstMoved));
					Log("  R3D SKINAUDIT: %ld of %ld drawn pieces have NO texture"
						" (white stand-in)%s  | skin slots: %ld tried, %ld named,"
						" %ld loaded from file, %ld rebuilt after the engine"
						" recycled the address",
						g_nPieceWhite, g_nPieceDrawn,
						g_nPieceWhite ? "   <- MODELS ARE DRAWING UNTEXTURED" : "",
						g_nSkinNameTried, g_nSkinNamed, g_nSkinFromFile,
						g_nSkinRecycled);
					// THE VISIBILITY FLAG THE PUBLISH BLOCK NEVER READ. If the
					// untextured pieces are all on objects the engine is not
					// drawing, then they are not a texture defect at all - they
					// are us drawing something retail hides, which is the same
					// class as the editor's marker geometry.
					if (g_nInvisInst || g_nInvisWhite)
						Log("  R3D INVISIBLE: %ld drawn instances have"
							" FLAG_VISIBLE CLEAR, and %ld of the %ld untextured"
							" pieces are on one%s",
							g_nInvisInst, g_nInvisWhite, g_nPieceWhite,
							(g_nPieceWhite && g_nInvisWhite == g_nPieceWhite)
								? "   <- ALL of them: the engine does not draw"
								  " these objects and neither should we" : "");
					if (g_bSkipInvisible)
						Log("  R3D INVISIBLE: %ld instances SKIPPED"
							" (+StubSkipInvisible 1)", g_nSkipInvis);
					// HOW MANY INSTANCES THE OBJECT-SPACE SCALE ACTUALLY ACTS ON.
					// A change nothing takes is a change nobody can measure, and a
					// change everything takes is a risk to every model in the game.
					// This says which it is, per build.
					Log("             OBJECT-SPACE SCALE: %ld of %u instances have"
						" a non-uniform scale and took it%s",
						g_nScaleNU, g_Models.nCount,
						g_bObjScaleSpace ? "" : "   <- +StubObjScale 0, DISABLED");
					Log("             MULTI-PIECE models only (the population this"
						" can act on): IN BOX %.1f%% (%ld of %ld), %ld faces,"
						" %ld bad index",
						(g_nInBoxM + g_nOutBoxM)
							? 100.0 * (double)g_nInBoxM
							  / (double)(g_nInBoxM + g_nOutBoxM) : 0.0,
						g_nInBoxM, g_nInBoxM + g_nOutBoxM, g_nFacesM, g_nBadIdxM);
					if (g_bPieceVerts)
						Log("             piece-own-records: %ld pieces ran short"
							" of their vertex count", g_nPieceShort);
					Log("             piece vertex-entry array: %ld pieces"
						" resolved, %ld could not%s   | %ld instances culled"
						" behind the camera | %ld faces dropped for an"
						" unresolved vertex (these were the spikes)",
						g_nVAOK, g_nVABad,
						(g_nVAOK && !g_nVABad) ? "  <- every piece" : "",
						g_nCulledBehind, g_nOriginFaces);
					if (!g_bBigSaid && g_nBig)
					{
						g_bBigSaid = 1;
						Log("  --- the LARGEST geometry the mesh drew ---");
						for (int z = 0; z < g_nBig; ++z)
						{
							int nBest = z;
							for (int y = z + 1; y < g_nBig; ++y)
								if (g_aBig[y].fDiag > g_aBig[nBest].fDiag) nBest = y;
							const BigInst b2 = g_aBig[nBest];
							g_aBig[nBest] = g_aBig[z]; g_aBig[z] = b2;
							Log("    %2d: %7.0f units  object %08X model %08X"
								"  at (%.0f %.0f %.0f)  dims (%.0f %.0f %.0f)"
								"  %u nodes, %u pieces", z + 1, b2.fDiag,
								b2.pObj, b2.pModel, b2.fPos[0], b2.fPos[1],
								b2.fPos[2], b2.fDims[0], b2.fDims[1], b2.fDims[2],
								b2.nNodes, b2.nPieces);
						}
					}
					if (g_nUnposed)
						Log("             %ld vertices bound to a node the engine"
							" never posed (index alignment held)", g_nUnposed);
					if (g_nBadWeight || g_nBadWSum)
						Log("             weights: %ld records not a fraction,"
							" %ld vertices whose weights did not sum to one"
							" (NORMALISED, not dropped)",
							g_nBadWeight, g_nBadWSum);
						Log("     weight sums: %ld of %ld vertices are not 1.0 (%.1f%%)",
							g_nBadWSum, g_nWSeen,
							g_nWSeen ? 100.0 * (double)g_nBadWSum / (double)g_nWSeen : 0.0);
							Log("     bone distance: mean %.1f, max %.1f, %ld of %ld vertices"
								" further than 40 units from their own strongest bone (%.1f%%)",
								g_nBoneDist ? g_fBoneDistSum / (double)g_nBoneDist : 0.0,
								g_fBoneDistMax, g_nBoneFar, g_nBoneDist,
								g_nBoneDist ? 100.0 * (double)g_nBoneFar / (double)g_nBoneDist : 0.0);
							Log("     weight records: %ld read, %ld with |p| over 60 units,"
								" %ld carrying NaN", g_nRecSeen, g_nRecHuge, g_nRecNaN);
							Log("     CHARACTERS only (>=20 nodes): %ld records, mean |p| %.1f,"
								" max %.1f, %ld over 60 units (%.2f%%); node matrices NaN %ld",
								g_nChrRec, g_nChrRec ? g_fChrSum / (double)g_nChrRec : 0.0,
								g_fChrMax, g_nChrHuge,
								g_nChrRec ? 100.0 * (double)g_nChrHuge / (double)g_nChrRec : 0.0,
								g_nNodeNaN);
							Log("     stretched edges: %ld touch a vertex whose weights"
							" did not sum to one, %ld do not",
							g_nLongBad, g_nLongClean);
						g_nLongBad = 0; g_nLongClean = 0;
						Log("     bad weight sums by position: %ld on the LAST entry"
							" of a piece (of %ld last entries seen), %ld elsewhere",
							g_nBadLast, g_nLastSeen, g_nBadMid);
						g_nBadLast = 0; g_nBadMid = 0; g_nLastSeen = 0;
						Log("     CHARACTER TRIANGLE EDGES: %ld, mean %.2f units,"
							" max %.1f, %ld longer than 30 units (%.2f%%)",
							g_nEdges, g_nEdges ? g_fEdgeSum / (double)g_nEdges : 0.0,
							g_fEdgeMax, g_nEdgeLong,
							g_nEdges ? 100.0 * (double)g_nEdgeLong / (double)g_nEdges : 0.0);
						g_fEdgeSum = 0.0; g_nEdges = 0; g_nEdgeLong = 0;
						g_fEdgeMax = 0.0f;
						if (g_bSkinConsist)
						{
							Log("     SKIN COLOUR DRIFT (NOT A DEFECT COUNT - several NPCs share a body model and legitimately wear different skins, so this counts wardrobe variety too; use SKIN AGE instead): %u piece names, %ld re-sightings, %ld a different colour (%ld world loads)",
								(unsigned)g_SkinRef.size(), g_nConsistSeen, g_nConsistBad, g_nWorldLoads);
							g_nConsistSeen = 0; g_nConsistBad = 0;
						}
						Log("     WORLD DRAWN: %lld vertices from %u batches, %ld builds, world %08X"
							" | %lld vertices ALPHA TESTED (foliage, wires, grilles)%s",
							g_nWorldDrawn, (unsigned)g_Batches.size(), g_nBuilds, g_pBuiltFrom,
							g_nCutDrawn,
							g_nWorldDrawn ? "" : "   <- THE WORLD IS NOT DRAWING");
						g_nWorldDrawn = 0; g_nCutDrawn = 0;
						// The backdrop, with its own denominator. Zero here while
						// g_bHaveSky is set means the pass ran and drew nothing,
						// which is a different fault from having no skybox at all.
						if (g_bHaveSprites)
						{
							// A sprite that publishes but cannot name its texture draws
							// nothing and looks identical to one that was never sent.
							long nRes = 0;
							for (uint32_t z = 0; z < g_Spr.nCount; ++z)
								if (SpriteTexture(g_Spr.inst[z].nObject) >= 0) ++nRes;
							// HOW MANY INSTANCES ACTUALLY CARRY THE FLAG. Wiring it
							// through brightened Cate and the note card and left the
							// flower motifs dark, so the question is whether those
							// objects are flagged at all - a denominator, not a guess.
							{
								long nNL = 0, nAD = 0, nMU = 0, nMULit = 0;
								for (uint32_t z = 0; z < g_Models.nCount; ++z)
								{
									if (g_Models.inst[z].nFlags & VRMODEL_F_NOLIGHT) ++nNL;
									if (g_Models.inst[z].nFlags & VRMODEL_F_ADDITIVE) ++nAD;
									if (g_Models.inst[z].nFlags & VRMODEL_F_MULTIPLY)
									{
										++nMU;
										// LIT OR UNLIT, because a multiply mask that
										// is ALSO shaded darkens the scene twice and
										// there is no way to tell from a still. The
										// engine's own flag decides it here - this
										// renderer does not invent a lighting rule
										// for a blend mode - so the split is printed
										// rather than assumed either way.
										if (!(g_Models.inst[z].nFlags
												& VRMODEL_F_NOLIGHT)) ++nMULit;
									}
								}
								Log("     NOLIGHT: %ld of %u published model instances"
									" are flagged unlit; %ld are flagged ADDITIVE",
									nNL, g_Models.nCount, nAD);
								// THE OBJECT COLOUR, COUNTED BEFORE IT IS USED.
								//
								// Published for the first time in version 4.
								// Nothing draws with it yet: the question this
								// answers is whether the game tints models at
								// all, and how many, because a tint applied to
								// 192 instances that are all white is a change
								// that cannot be seen and cannot be wrong.
								{
									long nTint = 0;
									float fWorst = 1.0f;
									for (uint32_t z = 0; z < g_Models.nCount; ++z)
									{
										const float* c = g_Models.inst[z].fColour;
										const float d =
											(fabsf(c[0] - 1.0f) > 0.02f
											 || fabsf(c[1] - 1.0f) > 0.02f
											 || fabsf(c[2] - 1.0f) > 0.02f)
											? 1.0f : 0.0f;
										if (d > 0.0f)
										{
											++nTint;
											const float lo =
												(c[0] < c[1] ? c[0] : c[1]);
											if ((lo < c[2] ? lo : c[2]) < fWorst)
												fWorst = (lo < c[2] ? lo : c[2]);
										}
									}
									Log("     OBJECT COLOUR: %ld of %u published"
										" instances are TINTED (darkest channel"
										" %.2f) - not applied yet, this is the"
										" measurement", nTint, g_Models.nCount,
										fWorst);
								}
								// HOW DARK IS THE DARKEST MODEL IN THIS LEVEL?
								//
								// The line a sweep of all 103 worlds is read on.
								// Model lighting was verified on TWO levels, and
								// the failure it could have on a third is not
								// subtle: a world with few lightmapped surfaces
								// AND no declared ambient gives every model a
								// light of nearly nothing, and characters go
								// black. That is a gameplay fault, not a
								// picture one, so it gets its own number rather
								// than being left to somebody's eye.
								//
								// The threshold is the level's own ambient floor
								// times the lightmap scale - anything at or
								// below it is a model the grid could not light
								// at all.
								{
									long nDark = 0;
									float fDarkest = 9.0f;
									const float fFloor = (g_fWorldAmbient[0]
										+ g_fWorldAmbient[1] + g_fWorldAmbient[2])
										/ 3.0f;
									for (uint32_t z = 0; z < g_Models.nCount; ++z)
									{
										float lt[3];
										LGridAt(g_Models.inst[z].fPos, lt);
										LightDirectAt(g_Models.inst[z].fPos, lt);
										const float m = (lt[0] + lt[1] + lt[2])
														/ 3.0f * g_fLMScale;
										if (m < fDarkest) fDarkest = m;
										if (m <= 0.12f) ++nDark;
									}
									Log("     MODEL LIGHT: darkest instance %.3f,"
										" %ld of %u at or under 0.12  | grid %ld"
										" of %ld cells, ambient floor %.3f%s",
										(g_Models.nCount ? fDarkest : 0.0f), nDark,
										g_Models.nCount, g_nLGridFilled,
										g_nLGridCells, fFloor,
										(nDark && fFloor <= 0.01f)
											? "   <- DARK MODELS AND NO AMBIENT"
											  " FLOOR: characters may be black here"
											: "");
								}
								// MULTIPLY ON MODELS IS DRAWN AS OF THIS BUILD.
								//
								// It was published and unread for weeks, which is how
								// the last several art bugs on this port started, so
								// the count stays even though the work is done - it
								// is the only way to know whether a frame contained
								// any. FX.TXT sets Multiply on ScaleFX170 and six
								// PolyDebrisFX records, so this reads 0 most of the
								// time and NON-ZERO around debris. A frame with 0 is
								// a frame that says NOTHING about whether the blend
								// is right; find one with a number in it first.
								if (nMU || g_nMeshMulDrawn)
									Log("     %ld models are flagged MULTIPLY"
										" (%ld LIT, %ld unlit); %ld mesh RUNS were"
										" drawn DEST_COLOR x ZERO%s",
										nMU, nMULit, nMU - nMULit, g_nMeshMulDrawn,
										g_bMulBlend ? ""
											: "  <- +StubMulBlend 0: drawn OPAQUE");
								g_nMeshMulDrawn = 0;
							}
							Log("     SPRITES: %u published, %ld resolved a texture,"
								" %ld vertices drawn  | IN FRONT of the camera: %ld,"
								" of which %ld RESOLVED NOTHING%s",
								g_Spr.nCount, nRes, g_nSprDrawn,
								g_nSprFront, g_nSprFrontNoTex,
								g_nSprFrontNoTex
									? "   <- these are effects nobody is drawing" : "");
							Log("     SPRITE BLEND: %ld additive, %ld multiply,"
								" the rest alpha", g_nSprAdditive, g_nSprMultiply);
							// DECALS versus BILLBOARDS. A sprite carrying
							// FLAG_ROTATEABLESPRITE has its own axes and must
							// lie flat on the surface it was put on; everything
							// else faces the camera. Counted because the flag
							// travelling from the client is the whole fix, and
							// a picture cannot say whether it arrived.
							{
								long nRot = 0;
								float fDeg = 0.0f;
								for (uint32_t z = 0; z < g_Spr.nCount; ++z)
								{
									if (!(g_Spr.inst[z].nFlags
											& VRSPRITE_F_ROTATABLE)) continue;
									++nRot;
									if (nRot == 1)
									{
										// Its own plane against the view. A
										// billboard would read 0 every time.
										const float* R = g_Spr.inst[z].fRight;
										const float* U = g_Spr.inst[z].fUp;
										float N[3] = { R[1]*U[2] - R[2]*U[1],
													   R[2]*U[0] - R[0]*U[2],
													   R[0]*U[1] - R[1]*U[0] };
										float rr2[3], uu2[3], ff2[3];
										QuatBasis(g_fLastQuat, rr2, uu2, ff2);
										float d = N[0]*ff2[0] + N[1]*ff2[1]
												+ N[2]*ff2[2];
										if (d < -1.0f) d = -1.0f;
										if (d >  1.0f) d =  1.0f;
										fDeg = acosf(d) * 57.2957795f;
									}
								}
								Log("     SPRITE AXES: %ld of %u carry their own"
									" (decals); the first sits %.0f degrees off"
									" the view axis%s", nRot, g_Spr.nCount, fDeg,
									nRot ? "" : "   <- NO DECALS IN FRAME, this"
									" says nothing yet");
							}
							g_nSprDrawn = 0;
							SpriteFootprint();
							// WHERE A SPRITE KEEPS ITS TEXTURE, ASKED THE WAY THE SKINS
							// WERE ASKED. The main menu's backdrop IS a sprite - the
							// client puts m_BackSprite in as object 0 of the interface
							// list - and it publishes and resolves nothing, so the menu
							// is black. SpriteTexture looks for a texture object the
							// cache knows BY ADDRESS, and an address is not an identity:
							// the engine recycles these, which is what dressed Cate in a
							// menu bar. A NAME identifies a texture even when its address
							// is in no cache entry, so ask every candidate word for one.
							//
							// Prints the object, its +0x1A8 render-data pointer and the
							// first 24 words there - what each word points at, whether
							// the cache knows it, and what the engine calls it. Any word
							// that yields a .dtx name IS the route, whether or not
							// TextureFor has ever heard of it.
							if (g_bCutProbe && g_Spr.nCount)
							{
								static int s_nSprSaid = 0;
								for (uint32_t z = 0; z < g_Spr.nCount && s_nSprSaid < 8; ++z)
								{
									++s_nSprSaid;
									const uint32_t hO = g_Spr.inst[z].nObject;
									const uint32_t pD = Word(hO, 0x1A8);
									// IT RESOLVES AND IT DRAWS AND THE SCREEN DOES NOT CHANGE, so
									// the next question is what it is drawn WITH. An alpha of zero
									// and a half-extent of zero both produce exactly this - a
									// vertex count that climbs and a picture that does not - and
									// neither can be told from the other by looking at the count.
									{
										const int nT = SpriteTexture(hO);
										const float fTW = (nT >= 0) ? g_Tex[nT].fW : 0.0f;
										const float fTH = (nT >= 0) ? g_Tex[nT].fH : 0.0f;
										Log("     SPR OBJ %08X  +1A8 %08X  kind %d  pos %.1f %.1f %.1f  scale %.2f %.2f",
											hO, pD, MemKind(pD, 0x40),
											g_Spr.inst[z].fPos[0], g_Spr.inst[z].fPos[1],
											g_Spr.inst[z].fPos[2],
											g_Spr.inst[z].fScale[0], g_Spr.inst[z].fScale[1]);
										Log("       tex %d (%s) %.0fx%.0f  colour %.2f %.2f %.2f  ALPHA %.2f  half-extent %.1f x %.1f  (sprite scale %.2f)",
											nT, (nT >= 0) ? g_Tex[nT].szName : "-", fTW, fTH,
											g_Spr.inst[z].fColour[0], g_Spr.inst[z].fColour[1],
											g_Spr.inst[z].fColour[2], g_Spr.inst[z].fColour[3],
											fTW * g_Spr.inst[z].fScale[0] * g_fSpriteScale,
											fTH * g_Spr.inst[z].fScale[1] * g_fSpriteScale,
											g_fSpriteScale);
									}
									// A SPRITE IS A .SPR RESOURCE THAT OWNS ITS FRAMES, so the
									// texture is not expected to be a word in the object - it is a
									// word in something the object points at. A flat scan of the
									// object and its render data found nothing across 160 and 64
									// words, which is consistent with that and with nothing else
									// tried so far, so follow one more level.
									//
									// AND COUNT WHAT WAS LOOKED AT. Silence from a probe means
									// "scanned and found nothing" or "never scanned at all", and
									// those need opposite fixes. The counts below separate them.
									for (int pass = 0; pass < 2; ++pass)
									{
										const uint32_t pB = pass ? pD : hO;
										const char* pW = pass ? "+1A8" : "obj ";
										const int nWords = pass ? 64 : 160;
										if (!pB || MemKind(pB, 0x60) != 2)
										{
											Log("       %s UNREADABLE (%08X, kind %d) - NOT SCANNED",
												pW, pB, MemKind(pB, 0x60));
											continue;
										}
										long nPtr = 0, nDeep = 0, nHit = 0;
										for (int k = 0; k < nWords; ++k)
										{
											const uint32_t w = Word(pB, k * 4);
											if (!w) continue;
											char szN[80];
											const bool bN = TexNameOf(w, szN, sizeof szN);
											const int nC2 = TextureFor(w);
											if (bN || nC2 >= 0)
											{
												++nHit;
												Log("       %s +%03X -> %08X  cache %d (%s)  ENGINE SAYS %s",
													pW, k * 4, w, nC2,
													(nC2 >= 0) ? g_Tex[nC2].szName : "-",
													bN ? szN : "<no name>");
												continue;
											}
											// ONE MORE LEVEL. w is some struct; does IT hold a texture?
											if (MemKind(w, 0x40) != 2) continue;
											++nPtr;
											for (int j = 0; j < 32; ++j)
											{
												const uint32_t v = Word(w, j * 4);
												if (!v) continue;
												char szM[80];
												const bool bM = TexNameOf(v, szM, sizeof szM);
												const int nC3 = TextureFor(v);
												if (!bM && nC3 < 0) continue;
												++nDeep;
												Log("       %s +%03X -> %08X +%03X -> %08X  cache %d (%s)  ENGINE SAYS %s",
													pW, k * 4, w, j * 4, v, nC3,
													(nC3 >= 0) ? g_Tex[nC3].szName : "-",
													bM ? szM : "<no name>");
											}
										}
										Log("       %s scanned %d words: %ld direct hits, %ld readable sub-structs, %ld hits one level down",
											pW, nWords, nHit, nPtr, nDeep);
									}
								}
							}
						}
						Log("     WORLD MODELS MOVED: %ld of %u are away from where"
							" they loaded; %ld batches drawn with their own matrix",
							g_nWMMoved, (unsigned)g_WMBase.size(), g_nWMDrawn);
						g_nWMDrawn = 0;
						// BRUSHES THE ENGINE IS NOT DRAWING, which this
						// renderer could not see until the publish carried a
						// flag word. Named, not just counted: "7 hidden" is a
						// number nobody can act on and "charge1 is hidden" is
						// an answer. COUNTED ONLY - nothing skips anything yet.
						{
							long nHid = 0;
							std::string sHid;
							for (size_t z = 0; z < g_WMBase.size(); ++z)
							{
								if (!(g_WMBase[z].nFlags & VRWORLD_F_INVIS))
									continue;
								char szN[32];
								ModelName(g_WMBase[z].pSub, szN, sizeof szN);
								// AND ARE WE ACTUALLY DRAWING IT? Blockers and
								// AI volumes are hidden by the engine AND
								// already skipped here by the marker-name rule,
								// so counting them as "drawn" would report a
								// defect that does not exist - which is exactly
								// what the first version of this line did.
								if (strncmp(szN, "blocker", 7) == 0
									|| strncmp(szN, "AIVolume", 8) == 0) continue;
								++nHid;
								if (sHid.size() < 700)
								{ sHid += szN; sHid += " "; }
							}
							if (nHid)
								Log("     WORLD MODELS THE ENGINE HAS HIDDEN: %ld of %u, %s  | %s",
									nHid, (unsigned)g_WMBase.size(),
									g_bDrawHiddenWM ? "STILL DRAWN (+StubDrawHiddenWM 1)"
													: "left out as the engine meant",
									sHid.c_str());
							g_nHiddenWMSkipped = 0;
						}
						// WHICH WORLD MODELS HAVE NO ENGINE OBJECT BEHIND THEM.
						//
						// A batch whose pSub has no baseline is drawn at the
						// position the level author saved and can never move,
						// open or be destroyed - the door opens and its handle
						// hangs in the air. The level FILE declares far more
						// world models than the client publishes objects for
						// (565 against 232 on Morocco), and most of that gap is
						// legitimate - the two BSPs, blockers, AI volumes and
						// editor marker brushes are not entities and never move.
						// So the count alone decides nothing: this NAMES them,
						// because "333 untracked" is a number nobody can act on
						// and "gate1 is untracked" is a bug report.
						{
							std::vector<uint32_t> seen;
							std::string sNames;
							long nUn = 0;
							for (size_t b2 = 0; b2 < g_Batches.size(); ++b2)
							{
								const uint32_t pS = g_Batches[b2].pSub;
								if (!pS || WMBaseFor(pS)) continue;
								bool bHad = false;
								for (size_t z = 0; z < seen.size(); ++z)
									if (seen[z] == pS) { bHad = true; break; }
								if (bHad) continue;
								seen.push_back(pS);
								++nUn;
								char szN[32];
								ModelName(pS, szN, sizeof szN);
								// The known-static classes are not worth naming;
								// anything else is a candidate for a thing that
								// should have moved and could not.
								if (strcmp(szN, "PhysicsBSP") == 0
									|| strcmp(szN, "VisBSP") == 0
									|| strncmp(szN, "AIVolume", 8) == 0
									|| strncmp(szN, "blocker", 7) == 0) continue;
								if (sNames.size() < 900)
								{ sNames += szN; sNames += " "; }
							}
							if (nUn)
								if (g_nWMSeededAway)
									Log("     WORLD MODELS FIRST SEEN AWAY FROM THEIR AUTHORED PLACE: %ld (drawn from the file's place)",
										g_nWMSeededAway);
								Log("     WORLD MODELS WITH NO ENGINE OBJECT: %ld"
									" (terrain and volumes drawn from the file; %s)%s%s",
									nUn,
									g_bDrawUnmatchedWM ? "the rest drawn too, +StubDrawUnmatchedWM 1"
													   : "the rest left out as removed",
									sNames.empty() ? "" : "  | ",
									sNames.c_str());
							Log("     OF THOSE: %ld drawn, %ld INVISIBLE VOLUMES left out"
								" (ShowSurface 0, +StubVolumeSurface 0 draws them), %ld removed"
								" game objects%s%s",
								g_nUnmDrawn, g_nUnmInvisible, g_nUnmRemoved,
								g_sUnmInvisible.empty() ? "" : "  | invisible: ",
								g_sUnmInvisible.c_str());
						}
						{
							long nSee = 0; float fMin = 1.0f;
							for (size_t z = 0; z < g_WMBase.size(); ++z)
								if (g_WMBase[z].fAlpha < kOpaqueAlpha)
								{
									++nSee;
									if (g_WMBase[z].fAlpha < fMin)
										fMin = g_WMBase[z].fAlpha;
								}
							Log("     GLASS: %ld of %u world models are see-through by"
								" the AUTHOR'S alpha (thinnest %.2f); %ld vertices drawn",
								nSee, (unsigned)g_WMBase.size(), fMin, g_nTransDrawn);
							// WHAT THE ALPHAS ACTUALLY ARE. "15 of 280, thinnest 0.99"
							// is not a glass distribution - real glass is 0.3 to 0.6 -
							// so either GetObjectColor is not the authored value or
							// these are not the objects. A histogram says which.
							{
								int nBin[11] = { 0 };
								for (size_t z = 0; z < g_WMBase.size(); ++z)
								{
									int k = (int)(g_WMBase[z].fAlpha * 10.0f + 0.5f);
									if (k < 0) k = 0; if (k > 10) k = 10;
									++nBin[k];
								}
								char szH[256]; szH[0] = 0;
								for (int k = 0; k <= 10; ++k)
								{
									char szB[24];
									sprintf_s(szB, " %.1f:%d", k / 10.0f, nBin[k]);
									strncat_s(szH, szB, _TRUNCATE);
								}
								Log("     GLASS alpha histogram:%s", szH);
								int nSaid2 = 0;
								for (size_t z = 0; z < g_WMBase.size() && nSaid2 < 8; ++z)
									if (g_WMBase[z].fAlpha < kOpaqueAlpha)
									{
										++nSaid2;
										char szN2[32] = { 0 };
										ModelName(g_WMBase[z].pSub, szN2, sizeof szN2);
										Log("        %-28s alpha %.3f", szN2,
											g_WMBase[z].fAlpha);
									}
							}
						}
						g_nTransDrawn = 0;
						Log("     SKY DRAWN: %ld vertices%s", g_nSkyDrawn,
							g_bHaveSky ? (g_nSkyDrawn ? ""
								: "   <- the level HAS a SkyBox and none of it drew")
							: "   (this level has no SkyBox model)");
						g_nSkyDrawn = 0;
						// STAGE 1. The denominator is the point: "0 disagree" means
						// nothing when 0 were compared, and that is the shape the
						// rigid-pair test failed in for a whole day.
						if (g_bWorldFile)
							Log("     WORLD FILE: %s (%ld loaded, %ld could not be "
								"identified or parsed)",
								g_pWorldFile ? g_pWorldFile->sPath.c_str() : "none",
								g_nWorldFileOK, g_nWorldFileFail);
						if (g_bMarkerTex)
							Log("     MARKER TEXTURES: %ld polygons skipped for wearing one of the six editor marker textures", g_nMarkerTexSkipped);
						if (g_nMarkerFlags)
							Log("     MARKER RULE: flags %u skipped %ld polygons | the "
								"heap<->file bridge held on %ld models and failed on %ld",
								g_nMarkerFlags, g_nMarkerSkipped, g_nFileMapped,
								g_nFileUnmapped);
						Log("     TEXTURE FILES: %ld drawn FROM THE FILE, %ld from the heap, %ld named but refused | model skins: %ld slots tried, %ld named, %ld loaded | dtx cache %u textures, %.1f MB, %u misses%s",
							g_nTexFromFile, g_nTexFromHeap, g_nTexFileRefused,
							g_nSkinNameTried, g_nSkinNamed, g_nSkinFromFile,
							// g_nSkinRecycled is printed by the line below it, not here -
							// this format string is already at its argument list's limit
							// of readability.
							Dtx_CacheCount(), Dtx_LoadedBytes() / 1048576.0,
							Dtx_MissCount(),
							(g_bTexFromFile && !g_nTexFromFile)
								? "   <- NOTHING IS COMING FROM A FILE" : "");
						// THE RECYCLED-ADDRESS COUNT, on its own line because it is the
						// number that says whether the menu's skins are still being
						// drawn on the characters. Non-zero once, at the menu, as the
						// engine swaps the block over; still climbing every frame means
						// the rebuild is not sticking.
						// THE ALL-ZERO-ALPHA RULE'S BLAST RADIUS. It rewrites pixels, so
						// how many textures it touches is not a detail - it is the
						// difference between a menu backdrop and every wall in the game.
						{
							long nOp = 0, nRef = 0;
							Dtx_AlphaStats(&nOp, &nRef);
							if (nOp || nRef)
								Log("     ALL-ZERO ALPHA: %ld textures rewritten opaque,"
									" %ld refused (file shorter than its own mip chain)",
									nOp, nRef);
						}
						if (g_nSkinRecycled)
							Log("     SKIN ADDRESSES RECYCLED BY THE ENGINE: %ld slots found holding another texture's picture and rebuilt from the name", g_nSkinRecycled);
						if (g_bNameProbe)
							Log("     TEXTURE FILES, VERIFICATION: %ld of %ld probed textures named a .dtx | heap vs file: %ld agree, %ld DISAGREE, %ld not on disk",
								g_nNameFound, g_nNameProbed, g_nFileAgree,
								g_nFileDisagree, g_nFileMissing);
						Log("     SKIN AGE: %ld of %ld skin slots are served by a cache entry built BEFORE the current world load (%ld loads) - those are the pixels of a texture the engine has already freed",
							g_nSkinIdOld, g_nSkinIdSlots, g_nWorldLoads);
						g_nSkinIdOld = 0;
						Log("     SKIN IDENTITY: %ld slots seen, %ld in the cache,"
							" %ld readable, %ld checked | stale by data pointer"
							" %ld, by pixel pointer %ld, by content %ld"
							"  | %ld rebuilt, %u queued",
							g_nSkinIdSlots, g_nSkinIdCached, g_nSkinIdLive,
							g_nSkinIdChecked, g_nSkinIdData, g_nSkinIdPix,
							g_nSkinIdSig, g_nTexStale,
							(unsigned)g_TexRefreshQ.size());
						g_nSkinIdChecked = 0; g_nSkinIdData = 0;
						g_nSkinIdPix = 0; g_nSkinIdSig = 0;
						g_nSkinIdSlots = 0; g_nSkinIdCached = 0; g_nSkinIdLive = 0;
						Log("     BIND vs WORLD EDGES: rigid triangles %ld ratio %.4f"
							"  |  BLENDED triangles %ld ratio %.4f   (rigid is the"
							" control and must read 1.00)",
							g_nEdgeR,
							(g_fEdgeBindR > 0.0)
								? sqrt(g_fEdgeWorldR / g_fEdgeBindR) : 0.0,
							g_nEdgeB,
							(g_fEdgeBindB > 0.0)
								? sqrt(g_fEdgeWorldB / g_fEdgeBindB) : 0.0);
						g_fEdgeBindR = g_fEdgeWorldR = 0.0; g_nEdgeR = 0;
						g_fEdgeBindB = g_fEdgeWorldB = 0.0; g_nEdgeB = 0;
						Log("     RIGID PAIRS (same bone, weight 1.0): %ld tested,"
								" mean error %.3f units (%.2f%% of the distance), worst %.2f,"
								" %ld pairs off by more than 5%%",
								g_nRigidPairs, g_nRigidPairs ? g_fRigidErr / (double)g_nRigidPairs : 0.0,
								g_nRigidPairs ? 100.0 * g_fRigidRel / (double)g_nRigidPairs : 0.0,
								g_fRigidWorst, g_nRigidBad);
							g_fRigidErr = 0.0; g_fRigidRel = 0.0; g_nRigidPairs = 0;
							g_nRigidBad = 0; g_fRigidWorst = 0.0f;
							g_nChrRec = 0; g_nChrHuge = 0; g_fChrSum = 0.0; g_fChrMax = 0.0f;
							g_nNodeNaN = 0;
							g_nRecSeen = 0; g_nRecHuge = 0; g_nRecNaN = 0;
							g_fBoneDistSum = 0.0; g_nBoneDist = 0; g_nBoneFar = 0;
							g_fBoneDistMax = 0.0f;
					g_nBadWeight = 0; g_nBadWSum = 0; g_nWSeen = 0;
					g_nCulledBehind = 0; g_nOriginFaces = 0; g_nUnposed = 0;
					g_nPieceShort = 0; g_nVAOK = g_nVABad = 0;
					g_nInBox = g_nOutBox = 0;
					g_nInBoxM = g_nOutBoxM = g_nBadIdxM = g_nFacesM = 0;
				}
			}
			else if (g_pMeshVB && !bRebuild && !g_MeshRuns.empty())
			{
				// Same published frame, second eye: redraw what is already there,
				// run for run. The runs describe the buffer, not the eye.
				int nVBBound = -1;
				for (size_t r = 0; r < g_MeshRuns.size(); ++r)
				{
					// Left for the blended pass at the end of the frame.
					if (g_bAddLast && g_MeshRuns[r].nBlend) continue;
					if (g_bScopePass && g_MeshRuns[r].nView) continue;	// the scope never sees the gun it sits on
					BindRunVB(g_MeshRuns[r].nVB, nVBBound, nRegionBytes);
					g_pCtx->PSSetShaderResources(0, 1, &g_MeshRuns[r].pSRV);
					// The SECOND mesh draw site. It needs the per-run blend as much as
					// the first: two sites, one decision, and only one of them getting
					// it is how a fix comes out looking like a partial fix.
					if (g_pBlendAdd && g_pBlendOpaque)
						g_pCtx->OMSetBlendState(
							(g_MeshRuns[r].nBlend == 2 && g_pBlendMul)
								? g_pBlendMul
							: (g_MeshRuns[r].nBlend == 3 && g_pBlendAlpha)
								? g_pBlendAlpha
							: g_MeshRuns[r].nBlend ? g_pBlendAdd
							: g_pBlendOpaque,
							kNoFactorM, 0xFFFFFFFF);
					SetOutAlpha(g_MeshRuns[r].nBlend == 2 ? -1.0f : 1.0f);
					SetModelLight(g_MeshRuns[r].fLight[0],
								  g_MeshRuns[r].fLight[1],
								  g_MeshRuns[r].fLight[2],
								  g_bModelLight ? 1.0f : 0.0f);
					if (g_MeshRuns[r].nBlend == 2) ++g_nMeshMulDrawn;
						if (g_bAlphaTest)
							// +StubAlphaTest 2 tests EVERY run at 0.5, cut-out or
							// not. It separates "the classifier said solid" from
							// "the threshold never reached the draw", and those
							// need opposite repairs.
							SetAlphaCut((g_bAlphaTest > 1 && g_MeshRuns[r].fCutRef <= 0.0f)
										? 0.5f : g_MeshRuns[r].fCutRef);
					g_pCtx->Draw(g_MeshRuns[r].nCount, g_MeshRuns[r].nStart);
				}
				SetOutAlpha(1.0f);
				SetModelLight(0.0f, 0.0f, 0.0f, 0.0f);
			}
		}

		// ---- GLASS, LAST -----------------------------------------------
		PhaseMark(1);		// the model meshes: skinning, build, draw
		// THE WATER IS DRAWN BEFORE THE WINDOW IT IS SEEN THROUGH.
		//
		// The pool is a polygrid, and polygrids are prims, and prims were
		// drawn in the effects pass - AFTER the glass. The glass pass tests
		// depth and deliberately never writes it, which is right for
		// transparency and means the window pane leaves no depth behind. The
		// pool, drawn afterwards, then tested against the courtyard BEHIND the
		// glass, passed, and painted over the window. Headset testing, 17 September,
		// standing on a bench looking out: the lower water pool in ours showed
		// through the window frame, so the pool texture was visible and the
		// window frame looked semi-translucent just in
		// that area - semi-translucent because a pale sheet was being blended
		// over a dark mullion, and only there because that is where the pool
		// is behind it.
		//
		// Transparent surfaces have to go far to near, so the courtyard's
		// water belongs BEFORE the window. Only polygrids move; particles,
		// lines and canvases stay in the effects pass where the muzzle flash
		// and the rest of them need to be.
		//
		// +StubWaterFirst 0 puts them back after the glass.
		if (g_bWaterFirst && !bNoWorld) DrawPrimRuns(2);

		R3D_PHASE("the glass pass");
		//
		// After the opaque world AND after the models, so a receptionist standing
		// behind a glass partition has already been drawn and shows through it.
		// Depth is TESTED (glass behind a wall is hidden) but not WRITTEN, so two
		// panes both show and nothing behind a pane is rejected by it.
		long nGlassBatches = 0;
		for (size_t b = 0; b < g_Batches.size(); ++b)
		{
			const WMBase* pA = g_Batches[b].pSub
							 ? WMBaseFor(g_Batches[b].pSub) : nullptr;
			if (g_Batches[b].bTrans || g_Batches[b].bGraded
				|| (pA && pA->fAlpha < kOpaqueAlpha))
				++nGlassBatches;
		}
		const bool bAnySprites = (g_bSprites && g_bHaveSprites && g_Spr.nCount);
		// The sprite pass lives inside this block, and a full-card folder
		// needs its sprites - the card IS a sprite - so the world gate sits
		// on the glass batches below, not on the block.
		if ((nGlassBatches || bAnySprites) && g_pBlendAlpha && g_pDSSNoWrite
			&& !g_bModelOnly)
		{
			const float kNoFactor2[4] = { 0, 0, 0, 0 };
			g_pCtx->OMSetBlendState(g_pBlendAlpha, kNoFactor2, 0xFFFFFFFF);
			g_pCtx->OMSetDepthStencilState(g_pDSSNoWrite, 0);
			g_fAlphaOut = g_fTransAlpha;
			g_fAlphaCutSet = -1.0f;		// force the next write
			SetAlphaCut(0.0f);			// which carries the new alpha with it
			{
				const UINT nStride2 = sizeof(Vtx), nOff2 = 0;
				g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStride2, &nOff2);
			}
			for (size_t b = 0; bNoWorld ? false : b < g_Batches.size(); ++b)
			{
				const WMBase* pA = g_Batches[b].pSub
								 ? WMBaseFor(g_Batches[b].pSub) : nullptr;
				if (!g_Batches[b].bTrans && !g_Batches[b].bGraded
					&& !(pA && pA->fAlpha < kOpaqueAlpha)) continue;
				// NOT A SKY BATCH. The sky pass draws those, from the sky
				// camera and without depth; the opaque world loop has always
				// skipped them and this one never did. A sky tile with graded
				// alpha (M05S05's Sky0051, the cloud sheets) was drawn AGAIN
				// here as world geometry at the sky model's live position -
				// 14058 units from where the level file puts it - and from the
				// opening cutscene that is a thin band at the horizon that
				// jumped between two places every frame (the headset and the
				// desk, 22 September). +StubSkyInGlass 1 draws them again.
				if (g_Batches[b].bSky && !g_bSkyInGlass) { ++g_nSkyGlassSkipped; continue; }
				if (pA && !g_bDrawHiddenWM && (pA->nFlags & VRWORLD_F_INVIS))
				{ ++g_nHiddenWMSkipped; continue; }		// shattered glass, a blast hole
				if (!pA && g_Batches[b].pSub && g_bHaveWorldObjs && !g_bDrawUnmatchedWM
					&& UnmatchedKindOf(g_Batches[b].pSub) == 2)
				{ ++g_nUnmatchedWMSkipped; continue; }
				// Unmatched, but the engine hid a brush of the same name - see
				// HiddenByName. Without this the canopy is drawn twice.
				if (!pA && g_Batches[b].pSub && g_bHaveWorldObjs && !g_bDrawHiddenWM
					&& HiddenByName(g_Batches[b].pSub))
				{ ++g_nHiddenWMSkipped; continue; }
				if (g_Batches[b].pTexKey && !g_Batches[b].pSRV && !g_bDrawNoPixels)
					continue;

				// ITS OWN alpha, not one number for everything. A window the author
				// set to 0.2 and a grille they set to 0.8 are different objects and
				// always were. A batch here for its TEXTURE'S alpha is drawn at
				// 1.0 so that alpha rules - the shader multiplies the two.
				const float fA = (pA && pA->fAlpha < kOpaqueAlpha) ? GlassAlphaOf(pA->fAlpha)
							   : g_Batches[b].bGraded ? 1.0f : g_fTransAlpha;
				// A translucent CUT-OUT is both things at once. The Morocco
				// gate is a grille: its bars are solid metal and its gaps are
				// holes, and blending alone leaves the gaps as a faint grey
				// film instead of open air.
				const float fCutT = g_Batches[b].fCutRef;
				if (fA != g_fAlphaOut)
				{
					g_fAlphaOut = fA;
					g_fAlphaCutSet = -1.0f;		// force the write below
				}
				SetAlphaCut(fCutT);
				// A GLASS DOOR THAT HAS OPENED. The world pass carries a moved
				// model's own matrix; this pass never did, so a glass pane on
				// a swinging door stayed in the frame while the frame swung.
				const bool bMovedG = (g_bWorldXform && pA && pA->bMoved);
				float mvG[16];
				if (bMovedG)
				{
					MovedMvp(pA, g_fLastMVP, mvG);
					WriteWorldCB(mvG, fCutT);
				}
				if (g_nSkipBatch >= 0 && (int)b == g_nSkipBatch) continue;	// +StubSkipBatch N
				if (g_Batches[b].bCut) g_nCutDrawn += g_Batches[b].nCount;
				// WATER IS LIT FLAT (see g_fWaterLight), for this draw only.
				const bool bFlat = (g_Batches[b].bEnvPan && g_fWaterLight > 0.0f);
				if (bFlat) { g_fFlatLight = g_fWaterLight; WriteWorldCB(bMovedG ? mvG : g_fLastMVP, fCutT); }
				// THE WATERFALL FALLS, AND NOTHING WE HAD COULD MAKE IT.
				//
				// Retail's sheet scrolls DOWN at 412 px/s in the 13:00 retail clip,
				// its pattern repeating every 66.7 px - a cycle every 0.16 s,
				// measured off a space-time image of 612 frames. The
				// environment map cannot produce that: EnvPanSpeed is 0.0005
				// PER FRAME, about 2 px/s, two orders of magnitude short. That
				// is why five rounds of tuning the map's speed, strength,
				// direction and copy count only ever changed the character of
				// a shimmer - the mechanism was never in the map at all.
				//
				// So the face's OWN texture scrolls, which is what a waterfall
				// does. Only the vertical faces are left in a water batch (the
				// horizontal ones are the polygrid's job and are skipped at
				// build), so this reaches the waterfall and the pool walls and
				// nothing else. The pool surface is a prim and is not in this
				// path at all.
				//
				// StubWaterFlow is in texture repeats per second x 100; 0
				// stops it. The rate is matched against the clip by measuring
				// ours the same way, not by eye.
				if (g_pCBWater)
				{
					// NO SCROLL ON THE SIDES OF A WATER VOLUME. The Dive's sea box
					// wears its water texture on its walls too, and from the boat
					// the far wall was a strip at the horizon that scrolled
					//. A rule by the batch's shape - scroll
					// only a sheet at least half as tall as it is wide - stopped it
					// and also FROZE the HQ waterfall, a 144-unit-tall sheet 1472
					// wide (the headset, the next day; the desk measured its
					// motion at 0.07 grey levels a frame against 1.38 without the
					// rule). A waterfall is a door or a brush; a sea is a water
					// volume whose surface a PolyGrid draws (ShowSurface 1). So
					// the test is what the model IS: a volume with a surface
					// object keeps its walls still, everything else flows.
					// +StubWaterFlowAny 1 scrolls every water batch.
					bool bTall = true;
					if (!g_bWaterFlowAny && g_pWorldFile && g_Batches[b].pSub)
					{
						char szWM[64] = "";
						ModelName(g_Batches[b].pSub, szWM, sizeof szWM);
						std::map<std::string, int>::const_iterator sv = g_pWorldFile->ShowSurface.find(szWM);
						if (sv != g_pWorldFile->ShowSurface.end() && sv->second == 1)
						{
							bTall = false;
							static uint32_t s_nSaidLoad = 0xFFFFFFFFu; static int s_nSaid = 0;
							if (s_nSaidLoad != (uint32_t)g_nWorldLoads) { s_nSaidLoad = (uint32_t)g_nWorldLoads; s_nSaid = 0; }
							if (s_nSaid++ < 4)
								Log("  R3D WATER FLOW: '%s' is a water volume with a surface object - its faces do not scroll", szWM);
						}
					}
					const float fFlow = (g_Batches[b].bEnvPan && bTall)
									  ? g_fWaterFlow * (float)g_fNowSec : 0.0f;
					const float w4[4] = { fFlow, 0.0f, 0.0f, 0.0f };
					D3D11_MAPPED_SUBRESOURCE wmw{};
					if (SUCCEEDED(g_pCtx->Map(g_pCBWater, 0, D3D11_MAP_WRITE_DISCARD, 0, &wmw)))
					{ memcpy(wmw.pData, w4, sizeof w4); g_pCtx->Unmap(g_pCBWater, 0); }
				}
				if (g_Batches[b].bEnvPan && g_pRSWater) g_pCtx->RSSetState(g_pRSWater);
				ID3D11ShaderResourceView* pS2 =
					BatchSRV(b);
				g_pCtx->PSSetShaderResources(0, 1, &pS2);
				g_pCtx->Draw(g_Batches[b].nCount, g_Batches[b].nStart);
				g_nTransDrawn += g_Batches[b].nCount;
				// THE OPAQUE PARTS OF A SEE-THROUGH SURFACE HIDE WHAT IS BEHIND
				// THEM. This pass draws without writing depth, so anything drawn
				// after it - the sprite pass, the models whose skins go to the
				// blended pass at the end - painted over the window's FRAMES: at
				// the HQ waterfall the rocks in the pool and the spray showed
				// through the mullions, where retail's frames hide them (the
				// retail and headset screenshots, 22 September). So after the
				// colour, the same batch again writing DEPTH ONLY for texels at
				// least g_fGlassDepthCut opaque: the frames occlude, the panes
				// stay see-through. Not water, and not an object the level made
				// translucent as a whole. +StubGlassDepth 0 reverts.
				if (g_bGlassDepth && g_pBlendNoColour && !g_Batches[b].bEnvPan
					&& !(pA && pA->fAlpha < kOpaqueAlpha))
				{
					ID3D11DepthStencilState* pDW = g_bRevZ ? g_pDSSRev : g_pDSS;
					if (pDW)
					{
						g_pCtx->OMSetBlendState(g_pBlendNoColour, kNoFactor2, 0xFFFFFFFF);
						g_pCtx->OMSetDepthStencilState(pDW, 0);
						WriteWorldCB(bMovedG ? mvG : g_fLastMVP, g_fGlassDepthCut);
						g_pCtx->Draw(g_Batches[b].nCount, g_Batches[b].nStart);
						++g_nGlassDepthDraws;
						WriteWorldCB(bMovedG ? mvG : g_fLastMVP, fCutT);
						g_pCtx->OMSetBlendState(g_pBlendAlpha, kNoFactor2, 0xFFFFFFFF);
						g_pCtx->OMSetDepthStencilState(g_pDSSNoWrite, 0);
					}
				}
				if (bFlat) { g_fFlatLight = 0.0f; WriteWorldCB(bMovedG ? mvG : g_fLastMVP, fCutT); }
				if (g_Batches[b].bEnvPan && g_pRSWater) g_pCtx->RSSetState(g_pRS);
				// AND PUT THE SCROLL BACK TO ZERO as soon as the water face is
				// drawn, so nothing after it in this pass inherits the flow.
				if (g_Batches[b].bEnvPan && g_pCBWater)
				{
					const float w0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
					D3D11_MAPPED_SUBRESOURCE wm0{};
					if (SUCCEEDED(g_pCtx->Map(g_pCBWater, 0, D3D11_MAP_WRITE_DISCARD, 0, &wm0)))
					{ memcpy(wm0.pData, w0, sizeof w0); g_pCtx->Unmap(g_pCBWater, 0); }
				}
				if (g_bEnvMap && g_Batches[b].pEnvSRV && g_pBlendAlpha)
				{
					EnvDraw(b, pQuat, pPos);
					const float kFe[4] = { 0, 0, 0, 0 };
					g_pCtx->OMSetBlendState(g_pBlendAlpha, kFe, 0xFFFFFFFF);
				}
				if (bMovedG)
				{
					g_fAlphaCutSet = -1.0f;		// force the static matrix back
					SetAlphaCut(fCutT);
				}
			}
			// ---- SPRITES ------------------------------------------------
			R3D_PHASE("the sprite pass");
			// The deliberate fault, well after startup so the count means
			// something. Reads through a deliberately bad pointer.
			if (g_bCrashTest && g_nPhaseFrames > 60)
			{
				Log("  R3D: +StubCrashTest - faulting ON PURPOSE now, to prove"
					" the crash filter and its phase line work");
				volatile const int* pBoom = (const int*)0x00000010;
				g_nDraws += *pBoom;
			}
			//
			// Every effect the game has - lamp glows, muzzle flashes, coronas -
			// and the whole main menu background. A camera-facing quad at the
			// object's position, sized by its scale and tinted by its colour.
			// Drawn here because this pass is already blended with depth read
			// and no depth write, which is exactly what a sprite wants.
			if (g_bSprites && g_bHaveSprites && g_Spr.nCount && g_bHaveCam)
			{
				float rr[3], uu[3], ff[3];
				QuatBasis(g_fLastQuat, rr, uu, ff);
				const UINT nMaxS = VRSPRITE_MAX * 6;
				// A RING, LIKE THE MESH. This buffer was discarded on EVERY
				// PASS, and the map-wait counters named it: "sprites 900/2
				// worst 124.5 ms" - the one map in the frame that waits, and
				// the wait is the whole stall felt in the headset every two seconds.
				// Eight regions, one per pass, written NO_OVERWRITE in turn.
				const UINT nSprRegions = 8;
				if (!g_pSprVB)
				{
					D3D11_BUFFER_DESC sd2{};
					sd2.ByteWidth = nMaxS * sizeof(Vtx) * nSprRegions;
					sd2.Usage = D3D11_USAGE_DYNAMIC;
					sd2.BindFlags = D3D11_BIND_VERTEX_BUFFER;
					sd2.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
					g_pDev->CreateBuffer(&sd2, nullptr, &g_pSprVB);
					Log("  R3D: sprite buffer: %u regions of %u vertices (%.1f MB), written NO_OVERWRITE in turn, never discarded",
						nSprRegions, nMaxS, (double)sd2.ByteWidth / 1048576.0);
				}
				static UINT s_nSprRegion = 0;
				s_nSprRegion = (s_nSprRegion + 1) % nSprRegions;
				const UINT nSprRegionBytes = s_nSprRegion * nMaxS * sizeof(Vtx);
				D3D11_MAPPED_SUBRESOURCE sm2{};
				if (g_pSprVB && SUCCEEDED(TimedMap(g_pSprVB, D3D11_MAP_WRITE_NO_OVERWRITE, &sm2, 2)))
				{
					struct SprRun { UINT nStart, nCount;
									ID3D11ShaderResourceView* pSRV; float fA;
									// 0 alpha, 1 additive, 2 multiply. Was a bool until
									// FLAG2_MULTIPLY turned out to be published too.
									int nBlend;
									// VRSPRITE_F_NODEPTH: the aim dot, drawn on top.
									int nNoDepth; };
					static std::vector<SprRun> runs;
					runs.clear();
					Vtx* sv = (Vtx*)((char*)sm2.pData + nSprRegionBytes);
					UINT nsv = 0;
					// BACK TO FRONT, BECAUSE THIS PASS WRITES NO DEPTH.
					//
					// A sprite is blended with the depth test on and the depth WRITE
					// off - which is right, two sprites should not cut holes in each
					// other - but it means nothing separates one sprite from another
					// except the order they are drawn in. Published order is the
					// engine's interface list, and that is NOT sorted: on the main
					// menu it runs 200, 150, 29, 28, 120, 155 units away, so drawing
					// it as given lays a sprite 155 units off on top of one at 28.
					// That is the logo behind the panel.
					//
					// Farthest first. An index sort, so the published array is left
					// alone - the report below and the client both read it by index.
					static std::vector<uint32_t> sprOrder;
					static std::vector<float>    sprDist;
					// The texture is resolved ONCE per sprite, here, and the
					// draw loop reads it back: SpriteTexture pulls files on a
					// per-frame budget, and resolving twice - once to sort,
					// once to draw - spent it before the sheet was reached.
					static std::vector<int>      sprTex;
					sprOrder.clear(); sprDist.assign(g_Spr.nCount, 0.0f);
					sprTex.assign(g_Spr.nCount, -1);
					for (uint32_t z = 0; z < g_Spr.nCount; ++z)
					{
						sprTex[z] = SpriteTexture(g_Spr.inst[z].nObject);
						const float* q = g_Spr.inst[z].fPos;
						const float dxs = q[0] - g_fLastPos[0];
						const float dys = q[1] - g_fLastPos[1];
						const float dzs = q[2] - g_fLastPos[2];
						sprDist[z] = dxs*dxs + dys*dys + dzs*dzs;
						sprOrder.push_back(z);
					}
					// THE INTERFACE SCENE IS DRAWN IN LIST ORDER, NOT BY DISTANCE.
					// The engine draws its interface list as given - the lime
					// fill first, the blue sheet over it, the logo over that -
					// and that order is the picture the artist made: the logo
					// sits 5 units BEHIND the sheet and shows through the sheet's
					// window. Sorted by distance and with the sheet drawn opaque
					// for the wide panel, the sheet hid the logo; pushed first,
					// the fill (an all-zero alpha the zero-alpha rule draws
					// opaque) covered the sheet. The client publishes the list
					// in the engine's order, so no sort is the right sort here.
					if (g_bSprSort)
						std::sort(sprOrder.begin(), sprOrder.end(),
							[](uint32_t a, uint32_t b)
							{ return sprDist[a] > sprDist[b]; });
					// The camera's forward vector, once, for the in-front test
					// below.
					float sprR[3], sprU[3], sprF[3];
					QuatBasis(g_fLastQuat, sprR, sprU, sprF);
					g_nSprFront = 0;
					g_nSprFrontNoTex = 0;
					for (size_t oi = 0; oi < sprOrder.size() && nsv + 6 <= nMaxS; ++oi)
					{
						const uint32_t i = sprOrder[oi];
						const VRSpriteInst& si = g_Spr.inst[i];
						const int ti = sprTex[i];
						// EVERY DISTINCT SPRITE TEXTURE, ONCE. Hunting a bullet
						// mark in a busy alley by diffing frames finds the gun
						// recoiling and the pedestrians walking; it does not
						// find a 32x32 decal. The renderer knows what it drew -
						// ask it.
						if (ti >= 0 && g_nSprNameSaid < 40)
						{
							// THE NAME IS COPIED, NOT POINTED AT.
							//
							// g_Tex is a std::vector and szName lives INSIDE the
							// entry, so every push_back that grows it moves all 80
							// bytes somewhere else. Holding char* here meant the
							// next _stricmp read freed memory: C0000005 reading
							// 14637D80, inside the byte-by-byte uppercasing loop.
							// A diagnostic that crashes the game is worse than no
							// diagnostic, and a heap address is never an identity.
							static char s_seen[40][80] = { { 0 } };
							const char* pszN = g_Tex[ti].szName;
							bool bNew = pszN[0] != 0;
							for (int q = 0; q < g_nSprNameSaid && bNew; ++q)
								if (_stricmp(s_seen[q], pszN) == 0)
									bNew = false;
							if (bNew)
							{
								strncpy(s_seen[g_nSprNameSaid], pszN, 79);
								s_seen[g_nSprNameSaid][79] = 0;
								++g_nSprNameSaid;
								Log("  SPRITE TEX: %s  (%.0fx%.0f, cut %.2f,"
									" add %d)", pszN, g_Tex[ti].fW, g_Tex[ti].fH,
									g_Tex[ti].fCut,
									(si.nFlags & VRSPRITE_F_ADDITIVE) ? 1 : 0);
							}
						}
						{
							const float fx = si.fPos[0] - g_fLastPos[0];
							const float fy = si.fPos[1] - g_fLastPos[1];
							const float fz = si.fPos[2] - g_fLastPos[2];
							if (fx*sprF[0] + fy*sprF[1] + fz*sprF[2] > 0.0f)
							{
								++g_nSprFront;
								if (ti < 0)
								{
									++g_nSprFrontNoTex;
									// WHERE DOES THIS ONE KEEP ITS NAME? On M05S01
									// 198 of the 211 sprites in front of the camera
									// resolve nothing, and drawing them from an
									// unnameable address is not an option - with the
									// strict check off the level fills with yellow
									// squares, photographed. So the address is not
									// the problem; the NAME is missing, and the
									// question is which word holds it for THIS
									// population. Every other offset in this project
									// was found by dumping the strings around a
									// pointer and reading them.
									if (g_bSprNameProbe && g_nSprProbeSaid < 4)
									{
										++g_nSprProbeSaid;
										const uint32_t hO = si.nObject;
										Log("  SPR NAME PROBE: object %08X"
											" +1A8 %08X +1B0 %08X  pos %.0f %.0f %.0f",
											hO, Word(hO, 0x1A8), Word(hO, 0x1B0),
											si.fPos[0], si.fPos[1], si.fPos[2]);
										// ASK THE ENGINE TO NAME IT, rather than
										// look for strings. Route B was found this
										// way: a word is the texture object if the
										// engine will give a .dtx name for it, and
										// nothing else in a heap object does that
										// by accident. Strings alone found only
										// "Untitled" and a music file, which is how
										// we know these offsets do not mean here
										// what they mean on a world sprite.
										int nFound = 0;
										for (int lv = 0; lv < 3 && nFound < 6; ++lv)
										{
											const uint32_t pB = (lv == 0) ? hO
												: (lv == 1) ? Word(hO, 0x1A8)
															: Word(hO, 0x1B0);
											if (MemKind(pB, 0x200) != 2) continue;
											for (int w = 0; w < 128 && nFound < 6; ++w)
											{
												const uint32_t v = Word(pB, w * 4);
												if (!v || MemKind(v, 0x40) != 2) continue;
												char szN[80];
												if (!TexNameOf(v, szN, sizeof szN)) continue;
												++nFound;
												Log("      lv%d +%03X -> %08X names"
													" \"%s\"%s", lv, w * 4, v, szN,
													RezFS_Exists(szN)
														? "   <- MOUNTED" : "");
											}
										}
										if (!nFound)
											Log("      nothing in 128 words of the"
												" object or of either render pointer"
												" names a texture - this population"
												" keeps its picture somewhere else");
									}
								}
							}
						}

						// TWO THINGS A SCREENSHOT CANNOT CATCH.
						//
						// (1) A SPRITE THAT CHANGES TEXTURE. In the headset, on a
						// load or a menu selection, certain textures dropped out
						// and loaded back in, and it looked wrong - the menu
						// card came out a maroon blob and the flower motif wore
						// CATE'S SUIT. It lasts under a second, so a capture at
						// one second after the keypress already missed it twice.
						// An object that swaps texture is the event itself, so
						// record the swap rather than trying to photograph it.
						//
						// (2) A SPRITE WITH AN ABSURD ASPECT. The van's
						// headlights draw as tall thin vertical shafts, and a
						// quad is texW*scale.x by texH*scale.y - so one of those
						// is wrong and the ratio says which. Reported for any
						// sprite past 4:1 either way, with its name and scale.
						// AND NEVER A LINEAR LIST OF EVERY SPRITE EVER SEEN. This was a
						// vector searched front to back for every sprite on every pass
						// and never pruned - every bullet hole of the session stayed
						// in it. With persistent marks, ten minutes of a firefight made
						// it 3,000 sprites x a list of thousands, twice a frame: the
						// sprite pass went 0.14 -> 4.80 ms per eye and the frame
						// 11.1 -> 15.8 ms, climbing by the minute (desk soak,
						// 24 September). A map, emptied at each world load, and not
						// consulted at all once both of its log budgets are spent.
						static long s_nSaidSwap = 0, s_nSaidAspect = 0;
						if (ti >= 0 && (s_nSaidSwap < 40 || s_nSaidAspect < 40))
						{
							static std::unordered_map<uint32_t, std::string> s_seen;
							static long s_nSeenWorld = -1;
							if (s_nSeenWorld != g_nWorldLoads) { s_seen.clear(); s_nSeenWorld = g_nWorldLoads; }
							const char* pszNow = g_Tex[ti].szName[0]
											   ? g_Tex[ti].szName : "(unnamed)";
							std::unordered_map<uint32_t, std::string>::iterator it = s_seen.find(si.nObject);
							if (it == s_seen.end())
								s_seen.emplace(si.nObject, std::string(pszNow));
							else if (it->second != pszNow)
							{
								if (s_nSaidSwap < 40)
								{
									++s_nSaidSwap;
									Log("  SPRITE TEXTURE SWAP: object %08X was"
										" '%s' and is now '%s'",
										si.nObject, it->second.c_str(), pszNow);
								}
								it->second = pszNow;
							}

							const float aw = g_Tex[ti].fW * si.fScale[0];
							const float ah = g_Tex[ti].fH * si.fScale[1];
							if (aw > 0.0f && ah > 0.0f
								&& (aw / ah > 4.0f || ah / aw > 4.0f)
								&& s_nSaidAspect < 40)
							{
								++s_nSaidAspect;
								Log("  SPRITE ASPECT %.2f:1  '%s'  texture"
									" %.0fx%.0f  scale %.3f %.3f %.3f  ->"
									" half-extent %.1f x %.1f",
									(ah > aw) ? ah / aw : aw / ah, pszNow,
									g_Tex[ti].fW, g_Tex[ti].fH,
									si.fScale[0], si.fScale[1], si.fScale[2],
									aw, ah);
							}
						}
						if (ti < 0 || !g_Tex[ti].pSRV) continue;
						// THE LIME FILL IS INVISIBLE IN RETAIL. menu\spr\folderback
						// is alpha 0 on every texel, which the zero-alpha rule
						// reads as additive art and draws as a lime sheet across
						// the whole view; behind the main menu's window that is
						// the lime box a wide panel showed. Not drawn in a
						// menu; the sheet backdrop below fills the window.
						if (g_bMenuZoomOn && g_fMenuBand3D > 0.0f
							&& IsFolderBackName(g_Tex[ti].szName)) continue;
						// AN ENTRY WITH NO DIMENSIONS DRAWS NOTHING, SILENTLY. The
						// engine binds a texture lazily, so an entry made before
						// the bind can carry 0 x 0 and every sprite wearing it
						// fails the size test below without a word. The file
						// knows the size; ask it once and remember.
						if ((g_Tex[ti].fW <= 0.0f || g_Tex[ti].fH <= 0.0f)
							&& g_Tex[ti].szName[0])
						{
							DtxInfo dz{};
							if (LoadInfoAny(g_Tex[ti].szName, &dz) && dz.nWidth && dz.nHeight)
							{
								g_Tex[ti].fW = (float)dz.nWidth;
								g_Tex[ti].fH = (float)dz.nHeight;
								static int s_nSaidDims = 0;
								if (s_nSaidDims++ < 12)
									Log("  SPRITE DIMS FROM THE FILE: %s had no size on"
										" its entry, now %ux%u", g_Tex[ti].szName,
										dz.nWidth, dz.nHeight);
							}
							else
							{
								static int s_nSaidNoDims = 0;
								if (s_nSaidNoDims++ < 12)
									Log("  SPRITE SKIPPED: %s has no size (entry %.0fx%.0f)"
										" and the file did not answer", g_Tex[ti].szName,
										g_Tex[ti].fW, g_Tex[ti].fH);
							}
						}
						// THE TEXTURE'S SIZE IS THE HALF-EXTENT, NOT THE WHOLE ONE.
						// Every sprite in the game was drawn at half the size the
						// engine draws it, from a 0.5 nobody had ever tested. The
						// main menu's blue panel settles it: with this factor its
						// rectangle is centre (0.4998 0.4996) half (0.3604 0.4996)
						// of the frame, and RETAIL's is the same four decimals in
						// both axes. It was invisible for as long as it was: a
						// corona or a muzzle flash at half size still looks like a
						// corona, and the menu is the first sprite in the game with
						// an edge anyone could measure.
						float hw = g_Tex[ti].fW * si.fScale[0]
									   * g_fSpriteScale;
						const float hh = g_Tex[ti].fH * si.fScale[1]
									   * g_fSpriteScale;
						// THE MENU'S FILL SPRITE REACHES THE BAND'S EDGES. The
						// interface scene is authored for a 4:3 view and its
						// flat backdrop (menu\sprolderback.spr, FOLDERBACK1)
						// covers exactly that; on the 16:9 panel the eye's
						// background showed either side of it. A wide
						// blue is this sprite stretched sideways, and a flat
						// colour stretches for free. Only the fill: the logo,
						// the cards and the help boxes keep their shape.
						bool bSheet = false;
						if (g_bMenuZoomOn && g_fMenuBand3D > 0.0f
							&& fFovX > 0.1f && fFovY > 0.1f)
						{
							// The backdrop SHEETS, by name: the main menu's blue
							// (MAINMENU*), the mission cards' orange
							// (*BACKGROUND*), the loading screen's (LOADING3*)
							// and the lime fill (FOLDERBACK*). Flat designs with
							// a rounded shape, the only sprites a stretch could suit.
							// NOT WIDENED ANY MORE (13 September, the afternoon headset
							// stills): stretching the sheet stretched its transparent
							// window with it, and the pinwheel behind the sheet
							// showed beside the logo as a lime wedge. The sheet keeps
							// its authored size; the backdrop pre-draw fills the
							// band's margins with the sheet's own flat colour.
							if (IsMenuSheetName(g_Tex[ti].szName)) bSheet = true;
						}
						if (hw <= 0.0f || hh <= 0.0f) continue;
						const float* P = si.fPos;
						static const float kU[6] = { -1,  1,  1, -1,  1, -1 };
						static const float kV[6] = { -1, -1,  1, -1,  1,  1 };
						const UINT nStart = nsv;
						// A DECAL USES ITS OWN AXES; EVERYTHING ELSE FACES THE
						// CAMERA. See VRSPRITE_F_ROTATABLE: a bullet hole and a
						// blood splat carry a rotation whose forward is the
						// surface normal so they lie flat on the wall, and
						// billboarding them makes them turn to follow the
						// viewer's head. A corona or a muzzle flash has no
						// orientation and must keep facing the camera.
						const bool bOwnAxes =
							(si.nFlags & VRSPRITE_F_ROTATABLE) != 0;
						const float* qr = bOwnAxes ? si.fRight : rr;
						const float* qu = bOwnAxes ? si.fUp    : uu;
						// The normal is only used by the lighting branch these
						// take (-2, unlit), so it costs nothing to keep it
						// pointing at the camera either way.
						for (int c = 0; c < 6; ++c)
						{
							Vtx& o = sv[nsv++];
							o.x = P[0] + qr[0]*kU[c]*hw + qu[0]*kV[c]*hh;
							o.y = P[1] + qr[1]*kU[c]*hw + qu[1]*kV[c]*hh;
							o.z = P[2] + qr[2]*kU[c]*hw + qu[2]*kV[c]*hh;
							o.nx = -ff[0]; o.ny = -ff[1]; o.nz = -ff[2];
							o.u = (kU[c] + 1.0f) * 0.5f;
							// UPSIDE DOWN, and it is V not U. The quad's vertical
							// axis is the camera's UP vector, and a texture's v
							// grows DOWNWARD from its top row - so mapping v
							// straight onto up draws every sprite inverted.
							//
							// Invisible on everything the world has: a muzzle
							// flash, a corona and a flat green backdrop have no
							// top. The menu's LETTERING is the first sprite in the
							// game that does, and THE OPERATIVE drew upside down.
							//
							// A horizontal flip was tried first and was wrong: it
							// left the letters inverted AND reversed their order.
							// Crop the lettering and look at it - a mean pixel
							// difference cannot tell one flip from the other.
							o.v = 1.0f - (kV[c] + 1.0f) * 0.5f;
							o.lu = -2.0f; o.lv = -1.0f;	// a sprite is its own light
						}
						SprRun r2{}; r2.nStart = nStart; r2.nCount = 6;
						r2.pSRV = g_Tex[ti].pSRV; r2.fA = si.fColour[3];
						// A CORONA IS ADDITIVE ART: a glow on a black field, and
						// black added is nothing. Blended as alpha, that field is
						// opaque and every streetlight and lit window wears a hard
						// black box - which is what the headset showed. The flag
						// was never published until now; see VRSPRITE_F_ADDITIVE.
						r2.nBlend = (g_bSprAdd
							&& (si.nFlags & VRSPRITE_F_ADDITIVE) != 0) ? 1 : 0;
						// AND ITS MIRROR IMAGE. Additive art is a glow on black;
						// multiply art is a mask on WHITE. Every bullet mark and
						// blood splat is the second kind, and their textures carry
						// no alpha at all - BLOODL3.DTX measures alpha 0 on all
						// 4096 of its texels - so alpha blending cannot save them.
						// Drawn any other way they are white squares with a dark
						// blob inside, which is what headset testing reported after shooting
						// an enemy. Checked second: a sprite setting both flags is
						// malformed, and the client clears one when it sets the
						// other, so the order only decides an impossible case.
						if (g_bMulBlend && g_pBlendMul
							&& (si.nFlags & VRSPRITE_F_MULTIPLY) != 0)
							r2.nBlend = 2;
						// A WIDENED SHEET IS DRAWN OPAQUE. MAINMENU1 carries a
						// transparent window over its dark-blue shape (x 41-93%,
						// y 22-49% of the sheet) that the authored 4:3 view never
						// reaches; the wider band does, and the lime fill showed
						// through it. The RGB under the window is the shape's own
						// (0,0,230), so opaque is seamless.
						// THE MENU'S DRAW ORDER, once: name, blend, distance and
						// flags per sprite, in the order they go down.
						if (g_bMenuZoomOn && g_fMenuBand3D > 0.0f)
						{
							static long s_nOrderFrame = -1; static int s_nOrderSaid = 0;
							if (s_nOrderSaid < 3 && s_nOrderFrame != g_nFrames)
							{ s_nOrderFrame = g_nFrames; ++s_nOrderSaid; Log("  MENU SPRITE ORDER (frame %ld):", g_nFrames); }
							if (s_nOrderSaid <= 3 && s_nOrderFrame == g_nFrames)
								Log("    #%u %s  blend %d  at (%.0f %.0f %.0f)  flags %08X  alpha %.2f  half %.0fx%.0f",
									(unsigned)oi, g_Tex[ti].szName, r2.nBlend,
									si.fPos[0], si.fPos[1], si.fPos[2], si.nFlags, si.fColour[3], hw, hh);
						}
						if (r2.nBlend == 1) ++g_nSprAdditive;
						else if (r2.nBlend == 2) ++g_nSprMultiply;
						r2.nNoDepth = ((si.nFlags & VRSPRITE_F_NODEPTH) && g_pDSSNoDepth) ? 1 : 0;
						runs.push_back(r2);
					}
					g_pCtx->Unmap(g_pSprVB, 0);
					if (!runs.empty())
					{
						const UINT nStr2 = sizeof(Vtx), nOff3 = nSprRegionBytes;
						g_pCtx->IASetVertexBuffers(0, 1, &g_pSprVB, &nStr2, &nOff3);
						// A pass must set its own state, and so must a run that
						// differs from the one before it. Tracked rather than set
						// per draw: sprites are sorted back to front, not by
						// blend, so the state can change several times.
						int nBlendNow = -1;
						int nDepthNow = 0;		// the pass set NoWrite (test on)
						for (size_t r3 = 0; r3 < runs.size(); ++r3)
						{
							// A run that asked to be on top switches the depth
							// test off for itself and back on for the next.
							if (runs[r3].nNoDepth != nDepthNow)
							{
								nDepthNow = runs[r3].nNoDepth;
								g_pCtx->OMSetDepthStencilState(
									nDepthNow ? g_pDSSNoDepth : g_pDSSNoWrite, 0);
							}
							const int nWant = runs[r3].nBlend;
							if (nWant != nBlendNow)
							{
								nBlendNow = nWant;
								ID3D11BlendState* pAdd =
									(g_bSprAddMod && g_pBlendAddMod)
										? g_pBlendAddMod : g_pBlendAdd;
								ID3D11BlendState* pBS =
									(nWant == 3) ? g_pBlendOpaque
									: (nWant == 2) ? g_pBlendMul
									: (nWant == 1) ? (pAdd ? pAdd : g_pBlendAlpha)
									: g_pBlendAlpha;
								if (!pBS) pBS = g_pBlendAlpha;
								if (pBS)
								{
									const float kF[4] = { 0, 0, 0, 0 };
									g_pCtx->OMSetBlendState(pBS, kF, 0xFFFFFFFF);
								}
							}
							// A MULTIPLY RUN SENDS ITS FADE AS A NEGATIVE ALPHA.
							// See the shader: DEST_COLOR x ZERO cannot see alpha,
							// so the fade has to happen before the blend. Clamped
							// away from zero because -0.0f == 0.0f and would read
							// as an ordinary opaque run.
							const float fAWant = (runs[r3].nBlend == 2)
								? -((runs[r3].fA < 0.002f) ? 0.002f : runs[r3].fA)
								: runs[r3].fA;
							if (fAWant != g_fAlphaOut)
							{
								g_fAlphaOut = fAWant;
								g_fAlphaCutSet = -1.0f;
								SetAlphaCut(0.0f);
							}
							g_pCtx->PSSetShaderResources(0, 1, &runs[r3].pSRV);
							g_pCtx->Draw(runs[r3].nCount, runs[r3].nStart);
							g_nSprDrawn += runs[r3].nCount;
						}
						// The world's own buffer back for whatever draws next.
						g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStr2, &nOff3);
					}
				}
			}

			// ---- PARTICLES, RAIN, WATER, CANVASES ----------------------
			R3D_PHASE("the effects pass");
			if (g_bPrims && g_bHavePrims && g_Prim.nRuns && g_bHaveCam)
				DrawPrimRuns(g_bWaterFirst ? -2 : -1);

			// PUT THE STATE BACK. A pass must set its own state, and the model
			// boxes and the next eye both inherit these.
			g_fAlphaOut = 1.0f;
			g_fAlphaCutSet = -1.0f;
			SetAlphaCut(0.0f);
			if (g_pBlendOpaque)
				g_pCtx->OMSetBlendState(g_pBlendOpaque, kNoFactor2, 0xFFFFFFFF);
			g_pCtx->OMSetDepthStencilState(
				(g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);
		}

		// ---- BLENDED MODELS, LAST --------------------------------------
		R3D_PHASE("the blended (additive/multiply) model pass");
		//
		// After the world, after the models, after the glass and after the
		// SPRITES - because a sprite is what the menu's motifs are drawn on top
		// of, and an additive draw is only worth anything once the thing it
		// brightens is in the target. A MULTIPLY draw needs the same thing for
		// the opposite reason: it darkens what is already there, and there is
		// nothing to darken until the scene is drawn.
		//
		// Depth TESTED, never WRITTEN: a motif behind Cate must be hidden by
		// her, and a motif must not punch a hole in the sprite behind it.
		if (g_bAddLast && g_pMeshVB && g_pBlendAdd && g_pDSSNoWrite
			&& !g_MeshRuns.empty())
		{
			size_t nAddRuns = 0;
			for (size_t r = 0; r < g_MeshRuns.size(); ++r)
				if (g_MeshRuns[r].nBlend) ++nAddRuns;
			if (nAddRuns)
			{
				const float kNoFactorA[4] = { 0, 0, 0, 0 };
				const UINT nStrideA = sizeof(Vtx), nOffA = 0;
				g_pCtx->OMSetDepthStencilState(g_pDSSNoWrite, 0);
				g_fAlphaOut = 1.0f;
				g_fAlphaCutSet = -1.0f;		// force the next write
				const UINT nOffRing = g_nMeshRegion * 300000u * sizeof(Vtx);
				int nVBBoundA = -1;
				// The pass carries TWO blend states now, so it tracks which one
				// is bound instead of setting it once up front. Runs are in
				// build order, not blend order, so this can change several
				// times - the same shape the sprite pass uses.
				int nBlendNow = -1;
				for (size_t r = 0; r < g_MeshRuns.size(); ++r)
				{
					if (!g_MeshRuns[r].nBlend) continue;
					if (g_MeshRuns[r].nBlend != nBlendNow)
					{
						nBlendNow = g_MeshRuns[r].nBlend;
						ID3D11BlendState* pBS =
							(nBlendNow == 2 && g_pBlendMul) ? g_pBlendMul
							: (nBlendNow == 3 && g_pBlendAlpha) ? g_pBlendAlpha
															: g_pBlendAdd;
						g_pCtx->OMSetBlendState(pBS, kNoFactorA, 0xFFFFFFFF);
						// MULTIPLY IS FLAGGED BY A NEGATIVE OUTPUT ALPHA, which
						// is what makes the shader skip the fog block: fogging a
						// mask tints the scene it multiplies instead of the
						// object, and BaseScaleFX clears bFog on exactly these.
						SetOutAlpha(nBlendNow == 2 ? -1.0f : 1.0f);
					}
					if (nBlendNow == 3) SetOutAlpha(g_MeshRuns[r].fA);
					if (g_MeshRuns[r].nBlend == 2) ++g_nMeshMulDrawn;
					SetModelLight(g_MeshRuns[r].fLight[0],
								  g_MeshRuns[r].fLight[1],
								  g_MeshRuns[r].fLight[2],
								  g_bModelLight ? 1.0f : 0.0f);
					g_pCtx->PSSetShaderResources(0, 1, &g_MeshRuns[r].pSRV);
					if (g_bAlphaTest) SetAlphaCut(g_MeshRuns[r].fCutRef);
					BindRunVB(g_MeshRuns[r].nVB, nVBBoundA, nOffRing);
					g_pCtx->Draw(g_MeshRuns[r].nCount, g_MeshRuns[r].nStart);
				}
				// Put it all back for the stand-ins and the next eye.
				SetModelLight(0.0f, 0.0f, 0.0f, 0.0f);
				g_fAlphaOut = 1.0f;
				g_fAlphaCutSet = -1.0f;
				SetAlphaCut(0.0f);
				if (g_pBlendOpaque)
					g_pCtx->OMSetBlendState(g_pBlendOpaque, kNoFactorA, 0xFFFFFFFF);
				g_pCtx->OMSetDepthStencilState(
					(g_bRevZ && g_pDSSRev) ? g_pDSSRev : g_pDSS, 0);
				g_pCtx->IASetVertexBuffers(0, 1, &g_pVB, &nStrideA, &nOffA);
			}
		}

		// ---- model stand-ins -------------------------------------------
		R3D_PHASE("the model stand-in boxes");
		//
		// A box per instance, at the engine's own position and half-extents.
		// This is not what a character looks like and is not meant to be: it
		// proves the client's list reaches the renderer and lands in the right
		// place in the world, which is everything except the mesh.
		if (g_bModelBoxes && g_bHaveModels && g_Models.nCount)
		{
			const UINT nMaxV = VRMODELS_MAX_INST * 36;
			if (!g_pBoxVB)
			{
				D3D11_BUFFER_DESC bd{};
				bd.ByteWidth = nMaxV * sizeof(Vtx);
				bd.Usage = D3D11_USAGE_DYNAMIC;
				bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
				bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				g_pDev->CreateBuffer(&bd, nullptr, &g_pBoxVB);
			}
			D3D11_MAPPED_SUBRESOURCE bm{};
			if (g_pBoxVB && SUCCEEDED(g_pCtx->Map(g_pBoxVB, 0,
					D3D11_MAP_WRITE_DISCARD, 0, &bm)))
			{
				Vtx* v = (Vtx*)bm.pData; UINT nv = 0;
				static const int kF[6][4] = {
					{0,1,3,2},{4,6,7,5},{0,4,5,1},{2,3,7,6},{0,2,6,4},{1,5,7,3} };
				static const float kN[6][3] = {
					{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0} };
				for (uint32_t i = 0; i < g_Models.nCount && nv + 36 <= nMaxV; ++i)
				{
					const VRModelInst& mi = g_Models.inst[i];
					if (mi.nFlags & 1u) continue;	// the player's own body

					// Size filter. GetObjectDims returns half-extents and some
					// model objects are enormous - the first run put the camera
					// inside a box that filled the entire frame, which tells you
					// nothing about whether the list is right. A character is
					// tens of units; anything past 128 is not one.
					if (mi.fDims[0] > 128.0f || mi.fDims[1] > 128.0f
						|| mi.fDims[2] > 128.0f) continue;
					if (mi.fDims[0] < 1.0f && mi.fDims[1] < 1.0f
						&& mi.fDims[2] < 1.0f) continue;

					// And skip any box the CAMERA IS INSIDE. The player's weapon and
					// attachments sit at the camera, so their boxes enclose the view
					// and fill the entire frame - which is what the first two runs
					// showed, and it says nothing about whether the list is right.
					// An exact point-in-box test rather than a distance guess.
					if (g_bHaveCam
						&& (!g_bOwnedFlagSeen || (mi.nFlags & VRMODEL_F_OWNED))
						&& fabsf(g_fLastPos[0] - mi.fPos[0]) <= mi.fDims[0]
						&& fabsf(g_fLastPos[1] - mi.fPos[1]) <= mi.fDims[1]
						&& fabsf(g_fLastPos[2] - mi.fPos[2]) <= mi.fDims[2]) continue;
					float c[8][3];
					for (int k = 0; k < 8; ++k)
					{
						c[k][0] = mi.fPos[0] + ((k & 4) ? mi.fDims[0] : -mi.fDims[0]);
						c[k][1] = mi.fPos[1] + ((k & 2) ? mi.fDims[1] : -mi.fDims[1]);
						c[k][2] = mi.fPos[2] + ((k & 1) ? mi.fDims[2] : -mi.fDims[2]);
					}
					for (int f = 0; f < 6; ++f)
					{
						static const int kT[6] = { 0,1,2, 0,2,3 };
						for (int s = 0; s < 6; ++s)
						{
							const int ci = kF[f][kT[s]];
							Vtx& o = v[nv++];
							o.x = c[ci][0]; o.y = c[ci][1]; o.z = c[ci][2];
							o.nx = kN[f][0]; o.ny = kN[f][1]; o.nz = kN[f][2];
							o.u = o.v = 0.0f;
							o.lu = o.lv = -1.0f;	// no lightmap: the stand-in shade
						}
					}
				}
				g_pCtx->Unmap(g_pBoxVB, 0);
				if (nv)
				{
					const UINT nStride = sizeof(Vtx), nOff = 0;
					g_pCtx->IASetVertexBuffers(0, 1, &g_pBoxVB, &nStride, &nOff);
					ID3D11ShaderResourceView* pBoxSRV =
						g_pBoxTex ? g_pBoxTex : g_pWhite;
					g_pCtx->PSSetShaderResources(0, 1, &pBoxSRV);
					g_pCtx->Draw(nv, 0);
					static int s_bSaidBox = 0;
					if (!s_bSaidBox)
					{
						s_bSaidBox = 1;
						Log("  R3D MODELS: drew %u box vertices for %u instances",
							nv, g_Models.nCount);
					}
				}
			}
		}
	}
	PhaseMark(2);		// glass, sprites, the blended model pass
	++g_nDraws;

	// THE CAMERA LIGHT ADD, over everything 3D and under the 2D layer, into
	// the multisampled target so the resolve carries it. See g_fLightAdd.
	// HOW MANY REFLECTION PASSES DREW, once every 900 frames: the number that
	// says whether the sheen and the water map reached the picture at all in
	// a headset run (14 September: no visible change).
	{
		static long s_nEnvSaidAt = 0, s_nEnvWas = 0;
		if (g_nFrames - s_nEnvSaidAt >= 900)
		{
			Log("  R3D ENV: %ld reflection passes drawn over the last %ld frames", g_nEnvDraws - s_nEnvWas, g_nFrames - s_nEnvSaidAt);
			s_nEnvSaidAt = g_nFrames; s_nEnvWas = g_nEnvDraws;
		}
	}
	if (g_bLightAddAllow && !bInterface
		&& (g_fLightAdd[0] > 0.002f || g_fLightAdd[1] > 0.002f || g_fLightAdd[2] > 0.002f))
		DrawLightAdd();

	// THE MULTISAMPLED WORLD, DOWN INTO THE BACK BUFFER, HERE.
	//
	// This is the boundary the comment below already names: everything after
	// it is 2D. The resolve overwrites the whole back buffer, so it has to
	// happen before the first HUD pixel and cannot be deferred to Present.
	//
	// Both eyes have drawn into the multisampled target by the time the second
	// pass reaches this line; resolving on each pass costs one extra resolve a
	// frame and keeps this correct if the eye order ever changes.
	if (!g_bScopePass)
	{
		R3D_ResolveMsaa();
		PhaseMark(3);		// the multisample resolve
		if (++g_nGpuPassNo == 2) R3D_GpuTailStamp(0);
		PhaseReport();
	}

	// The 2D layer draws after this and must not be depth tested against the
	// world, so the target goes back to colour only.
	g_pCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
	g_pCtx->OMSetDepthStencilState(nullptr, 0);
	R3D_PHASE("finished a world pass - in the 2D layer or the engine");
	++g_nPhaseFrames;
}

const char* R3D_LastPhase()   { return (const char*)g_pszPhase; }
long        R3D_PhaseFrames() { return g_nPhaseFrames; }

void R3D_SetTest(int b) { g_bTest = b; }
void R3D_SetLMScale(float f) { g_fLMScale = f; }
void R3D_SetLMOnly(int b) { g_bLMOnly = b; }
void R3D_SetCutProbe(int b) { g_bCutProbe = b; }
void R3D_SetCutGuess(int b) { g_bCutGuess = b; }
void R3D_SetEnvNoCut(int b) { g_bEnvNoCut = b; }
void R3D_SetPieceMatIndex(int b) { g_bPieceMatIndex = b; }
void R3D_SetSkinFrame(int n) { g_nSkinFrame = n; }
void R3D_SetSprStrictTex(int b) { g_bSprStrictTex = b; }
void R3D_SetSprNameProbe(int b) { g_bSprNameProbe = b; }
void R3D_SetSprFromFile(int b)  { g_bSprFromFile = b; }
void R3D_SetSkinNameProbe(int b) { g_bSkinNameProbe = b; }
void R3D_SetSkinFromButes(int b) { g_bSkinFromButes = b; }
void R3D_SetHideViewArms(int b)  { g_bHideViewArms = b; }
void R3D_SetDrawBody(int b)      { g_bDrawBody = b; }
void R3D_SetHideHead(int b)      { g_bHideHead = b; }
void R3D_SetLMEnable(int b) { g_bLMEnable = b; }
void R3D_SetAniso(int n)    { g_nAniso = n; }
void R3D_SetSkyStandIn(int b) { g_bSkyStandIn = b; }
void R3D_SetSkyBox(int b) { g_bSkyBox = b; }
void R3D_SetModelDims(float f) { if (f > 0.0f) g_fModelDimsMax = f; }
void R3D_SetFastRead(int b) { g_bFastRead = b; FlushPageCache(); }
void R3D_SetWorldBSP(int n) { g_nWorldBSP = n; }
void R3D_SetBSPCover(int b) { g_bBSPCover = b; }
void R3D_SetTransAlpha(float f) { if (f >= 0.0f && f <= 1.0f) g_fTransAlpha = f; }
void R3D_SetTransNames(int b) { g_bTransNames = b; }
void R3D_SetWorldRebuild(int nMs) { g_nWorldRebuildMs = nMs; }
void R3D_SetSkipTranslucent(int b) { g_bSkipTranslucent = b; }
void R3D_SetDrawNoPixels(int b) { g_bDrawNoPixels = b; }
void R3D_SetModelBoxes(int b) { g_bModelBoxes = b; }
void R3D_SetModelMesh(int b) { g_bModelMesh = b; }
void R3D_SetModelOnly(int b) { g_bModelOnly = b; }
void R3D_SetInterfaceOnly(int b) { g_bInterfaceOnly = b ? 1 : 0; }
void R3D_SetMipOffsetUV(int b) { g_bMipOffsetUV = b; }
void R3D_SetMipOffsetSky(int b) { g_bMipOffsetSky = b; }
void R3D_SetWorldCull(int b) { g_bWorldCull = b; }
void R3D_SetMeshTint(int b)  { g_bMeshTint = b; }
void R3D_SetTexHunt(int b)   { g_bTexHunt = b; }
void R3D_SetIdxHunt(int b)   { g_bIdxHunt = b; }
void R3D_SetBeamDump(int b)  { g_bBeamDump = b; }
void R3D_SetModelProbe(const char* psz)
{
	g_szModelProbe[0] = 0;
	if (!psz || !*psz) return;
	size_t i = 0;
	for (; i < sizeof g_szModelProbe - 1 && psz[i]; ++i)
		g_szModelProbe[i] = (char)toupper((unsigned char)psz[i]);
	g_szModelProbe[i] = 0;
}
void R3D_SetObjScale(int b)  { g_bObjScaleSpace = b; }
void R3D_SetAlphaTest(int b) { g_bAlphaTest = b; }
void R3D_SetA2C(int b) { g_bA2C = b; }
void R3D_SetAlphaSharp(int b) { g_bAlphaSharp = b; g_bFogDirty = 1; }
void R3D_SetDrawSections(int b) { g_bDrawSections = b; }
void R3D_SetCull(int n)         { g_nCullMode = n; }

void R3D_SetPieceSkin(int b) { g_bPieceSkin = b; }
void R3D_SetVtxNrm(int b)    { g_bVtxNrm = b; }
void R3D_SetNodeT(int b)     { g_bNodeT = b; }
void R3D_SetEntryCount(int n)  { g_nEntryCount = n; }
void R3D_SetWeightPre(int b) { g_bWeightPre = b; }
void R3D_SetEdgeChk(int b)   { g_bEdgeChk = b; }
void R3D_SetFlushOnLoad(int b) { g_bFlushOnLoad = b; }
void R3D_SetSkinConsist(int b) { g_bSkinConsist = b; }
void R3D_SetSkipOcclTex(int b) { g_bSkipOcclTex = b; }
void R3D_SetSkinPull(int b)    { g_bSkinPull = b; }
void R3D_SetTexDataFn(void* fn)
{ g_pfnTexData = (void* (__cdecl*)(void*))fn; }

// One per frame, called from the present path. The engine only binds what
// IT draws, and it draws models through a path we do not implement, so a
// skin a piece names can never arrive on its own.
// Rebuild one stale entry per frame. R3D_NoteTexture already replaces an
// entry whose data pointer has changed - it just never got the chance,
// because nothing rebinds a model skin.
static void R3D_RefreshOneStaleTexture();

// Several per frame, not one. A reload invalidates every model skin at once -
// over a thousand of them resolve in a frame here - and one per frame is
// fourteen seconds of a character wearing the wrong clothes while the queue
// drains. Eight is still a few hundred microseconds and it clears in a blink.
void R3D_RefreshStaleTexture()
{
	for (int nDone = 0; nDone < 8; ++nDone) R3D_RefreshOneStaleTexture();
}

static void R3D_RefreshOneStaleTexture()
{
	if (g_TexRefreshQ.empty() || !g_pDev) return;
	const uint32_t pTex = g_TexRefreshQ.back();
	g_TexRefreshQ.pop_back();
	if (!Readable(pTex, 16)) return;
	const uint32_t pData = Word(pTex, 0x08);
	if (!pData || !Readable(pData, 0xE0)) return;
	const int nC = TextureFor(pTex);
	{
		// Already current only if the PICTURE matches, not just the
		// allocation. The old guard compared pData alone and would return
		// here on exactly the case this exists to fix.
		uint32_t dNow = 0, pNow = 0, sNow = 0;
		if (nC >= 0 && TexLiveId(pTex, &dNow, &pNow, &sNow)
			&& g_Tex[nC].pData == dNow && g_Tex[nC].pPix == pNow
			&& (!sNow || g_Tex[nC].nSig == sNow))
			return;
	}
	R3D_NoteTexture(pTex, pData);
	++g_nTexStale;
	// The do-not-retry list is keyed on the same pointers, so a recycled
	// address must be allowed a fresh attempt.
	g_nTexTried = 0;
	if (g_nTexStale <= 12)
		Log("  R3D TEX STALE: %08X was a recycled object - rebuilt it"
			" from data %08X", pTex, pData);
}

void R3D_PullQueuedSkin()
{
	if (!g_bSkinPull || g_SkinPullQ.empty() || !g_pDev) return;
	const uint32_t pTex = g_SkinPullQ.back();
	g_SkinPullQ.pop_back();
	// NOT the engine callback - calling that killed world rendering, and
	// the A/B is in the previous commit. The texture OBJECT stores its own
	// data pointer at +0x08: measured on 60 of 60 bound textures, where
	// both halves of the pair were already in hand. No engine call at all.
	if (!Readable(pTex, 16))
		{ Log("  R3D SKIN PULL: %08X object not readable", pTex); return; }
	const uint32_t pData = Word(pTex, 0x08);
	if (!pData || !Readable(pData, 0xE0))
		{ Log("  R3D SKIN PULL: %08X data %08X not readable",
			pTex, pData); return; }
	const size_t nBefore = g_Tex.size();
	R3D_NoteTexture(pTex, pData);
	if (g_Tex.size() == nBefore)
		Log("  R3D SKIN PULL: %08X data %08X REFUSED by NoteTexture",
			pTex, pData);
	if (g_Tex.size() > nBefore)
	{
		++g_nTexPulled;
		if (g_nTexPulled <= 12)
			Log("  R3D SKIN PULL: %08X was never bound by the engine;"
				" fetched it at the frame boundary", pTex);
	}
}
void R3D_SetPieceBase(int b) { g_bPieceBase = b; }
void R3D_SetSkipInvisible(int b) { g_bSkipInvisible = b; }
void R3D_SetMenuBand(float f) { g_fMenuBand3D = f; }
void R3D_SetMenuZoom(float f, float ax, float ay, int bOn)
{
	g_fMenuZoom3D = (f > 0.0f) ? f : 1.0f;
	g_fMenuAnchorX3D = ax;
	g_fMenuAnchorY3D = ay;
	g_bMenuZoomOn = bOn;
}
void R3D_SetPieceVerts(int b){ g_bPieceVerts = b; }
void R3D_SetPieceVA(int b)   { g_bPieceVA = b; }
void R3D_NearestModel(float* pAz, float* pDist)
{
	if (pAz) *pAz = g_fNearAz;
	if (pDist) *pDist = g_fNearDist;
}

// The client's list for this frame. Copied rather than kept by pointer: this
// is called from the client's update and the draw happens later in the frame,
// so the storage it points at is its own business by then.
// ---------------------------------------------------------------------------
// From the HOBJECT to the mesh.
//
// ILTModel exposes animation and nodes and NO geometry - GetNodeTransform,
// GetSocket, GetPiece, and nothing that hands over a vertex. So the mesh has to
// be walked out of the engine, starting from the object handle the client
// already publishes. Starting from a known object is a far smaller search than
// starting from the world.
//
// Two signatures, both of which the client has already told us:
//
//   THE NODE COUNT - 25 for a character, 3-14 for a prop, and it VARIES between
//   instances, so a field that is right for one instance and also for another
//   of a different size is a structure and not a coincidence.
//
//   THE HALF-EXTENTS - the model's own vertices must fit inside the box the
//   engine reports for it. A long run of float triples that all fit a box we
//   were told the size of is the mesh.
//
// Plus a filename ending .abc, which is what a LithTech model file is.
//
// STAY ON THE HEAP. The first version of this followed the object's vtable at
// +00 into lithtech.exe and dutifully reported every matching integer in its
// code. VirtualQuery tells an image apart from private memory in one call.
// ---------------------------------------------------------------------------
namespace {

// 0 = not readable, 1 = a loaded module's image, 2 = private (heap).
// MEMKIND ANSWERS FROM A REGION CACHE FIRST (+StubMemKindCache, default 1).
// It was a VirtualQuery every call: 1463 of them a frame in M01S02's market,
// 3.1 ms of the frame at ~2 us each - the largest single cost of a rebuilt
// model instance, bigger than skinning it. A committed region is megabytes, so
// the last few regions answer nearly every question. A cached answer is never
// trusted on its own: the span is TOUCHED under SEH (TouchRange, one byte per
// page and both ends), exactly as Readable's page cache does, and a fault drops
// the region and falls through to VirtualQuery for the truth.
int g_bMemKindCache = 1;
namespace
{
	struct MemRegion { uint32_t base, end; int kind; };
	MemRegion g_aMemReg[32] = {};
	int       g_nMemRegNext = 0;
	long long g_nMemKindHit = 0;
}
int MemKind(uint32_t p, size_t n)
{
	if (!p) return 0;
	++g_nMemKindCalls;
	if (g_bMemKindCache && n)
	{
		const uint64_t e = (uint64_t)p + n;
		for (int i = 0; i < 32; ++i)
		{
			const MemRegion& r = g_aMemReg[i];
			if (r.kind && p >= r.base && e <= (uint64_t)r.end)
			{
				if (TouchRange(p, n)) { ++g_nMemKindHit; return r.kind; }
				g_aMemReg[i].kind = 0;		// it moved under us: ask the OS
				break;
			}
		}
	}
	MEMORY_BASIC_INFORMATION mbi{};
	LARGE_INTEGER qa, qb; QueryPerformanceCounter(&qa);
	const SIZE_T nq = VirtualQuery((LPCVOID)(uintptr_t)p, &mbi, sizeof mbi);
	QueryPerformanceCounter(&qb); g_qMemKind += qb.QuadPart - qa.QuadPart;
	if (!nq) return 0;
	if (mbi.State != MEM_COMMIT) return 0;
	const DWORD bad = PAGE_NOACCESS | PAGE_GUARD;
	if (mbi.Protect & bad) return 0;
	// the whole span must sit in this region
	const uintptr_t endReg = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
	if ((uintptr_t)p + n > endReg) return 0;
	const int nKind = (mbi.Type == MEM_IMAGE) ? 1 : 2;
	if (g_bMemKindCache && endReg <= 0xFFFFFFFFu)
	{
		MemRegion& r = g_aMemReg[g_nMemRegNext];
		g_nMemRegNext = (g_nMemRegNext + 1) & 31;
		r.base = (uint32_t)(uintptr_t)mbi.BaseAddress;
		r.end  = (uint32_t)endReg;
		r.kind = nKind;
	}
	return nKind;
}
void MemKindCacheSet(int b) { g_bMemKindCache = b; for (int i = 0; i < 32; ++i) g_aMemReg[i].kind = 0; }

// Is this a run of vertices for a model of these half-extents?
//
// "Every triple fits the box" is far too weak - a page of zeros fits every box.
// A real mesh SPANS its box and is mostly non-zero. Both are required, and the
// measured extent is reported so the claim can be checked rather than believed.
struct VertRun
{
	int   n;
	float lo[3], hi[3];
	int   nNonZero;
};

VertRun VertexRun(uint32_t p, const float* pDims, int nMax)
{
	VertRun r{};
	r.lo[0] = r.lo[1] = r.lo[2] =  1e30f;
	r.hi[0] = r.hi[1] = r.hi[2] = -1e30f;
	const float f[3] = {
		(pDims[0] > 1.0f ? pDims[0] : 1.0f) * 3.0f,
		(pDims[1] > 1.0f ? pDims[1] : 1.0f) * 3.0f,
		(pDims[2] > 1.0f ? pDims[2] : 1.0f) * 3.0f };

	for (; r.n < nMax; ++r.n)
	{
		const uint32_t q = p + (uint32_t)r.n * 12;
		if (MemKind(q, 12) != 2) break;
		const float* v = (const float*)(uintptr_t)q;
		bool bBad = false, bNonZero = false;
		for (int k = 0; k < 3; ++k)
		{
			const float a = v[k];
			if (!(a == a)) { bBad = true; break; }			// NaN
			if (a < -f[k] || a > f[k]) { bBad = true; break; }
			if (a != 0.0f) bNonZero = true;
		}
		if (bBad) break;
		if (bNonZero) ++r.nNonZero;
		for (int k = 0; k < 3; ++k)
		{
			if (v[k] < r.lo[k]) r.lo[k] = v[k];
			if (v[k] > r.hi[k]) r.hi[k] = v[k];
		}
	}
	return r;
}

// Does that run look like the model rather than like padding?
bool RunIsMesh(const VertRun& r, const float* pDims)
{
	if (r.n < 24) return false;
	if (r.nNonZero * 2 < r.n) return false;			// mostly zeros: padding
	int nSpanning = 0;
	for (int k = 0; k < 3; ++k)
	{
		const float ext = r.hi[k] - r.lo[k];
		const float want = (pDims[k] > 1.0f ? pDims[k] : 1.0f) * 0.5f;
		if (ext >= want) ++nSpanning;
	}
	return nSpanning >= 2;						// spans the box on 2 of 3 axes
}

struct WalkCtx
{
	uint32_t     nWantNodes;
	const float* pDims;
	R3D_LogFn    pfnLog;
	uint32_t     seen[2048];
	int          nSeen;
	int          nHits;
};

void WalkForMesh(WalkCtx& c, uint32_t pRoot, int nDepth, const char* pszPath)
{
	if (nDepth < 0 || c.nHits >= 40) return;
	for (int i = 0; i < c.nSeen; ++i) if (c.seen[i] == pRoot) return;
	if (c.nSeen >= (int)(sizeof c.seen / sizeof c.seen[0])) return;
	c.seen[c.nSeen++] = pRoot;
	if (MemKind(pRoot, 0x100) != 2) return;

	// +00 is the vtable and leads into code. +04 and +08 are the OBJECT LIST
	// links - instance 1's +08 points at instance 2 - so following them walks
	// sideways through other objects and every find belongs to the wrong one.
	for (uint32_t off = 4; off < 0x200; off += 4)
	{
		if (off == 0x04 || off == 0x08) continue;
		const uint32_t v = *(const uint32_t*)(uintptr_t)(pRoot + off);

		if (v == c.nWantNodes && c.nWantNodes >= 3)
		{
			c.pfnLog("        NODE COUNT %u at %s+%02X", c.nWantNodes, pszPath, off);
			if (++c.nHits >= 40) return;
		}

		const int nk = MemKind(v, 16);

		if (nk)			// a string may live in either
		{
			const char* q = (const char*)(uintptr_t)v;
			int n = 0;
			while (n < 127 && MemKind(v + (uint32_t)n, 1) && q[n] >= 32 && q[n] <= 126) ++n;
			if (n > 4 && MemKind(v + (uint32_t)n, 1) && q[n] == 0)
			{
				const char* e = q + n - 4;
				if (e[0] == '.' && (e[1]|32) == 'a' && (e[2]|32) == 'b' && (e[3]|32) == 'c')
				{
					char sz[132]; memcpy(sz, q, (size_t)n); sz[n] = 0;
					c.pfnLog("        .abc  at %s+%02X -> \"%s\"", pszPath, off, sz);
					if (++c.nHits >= 40) return;
				}
			}
		}

		if (nk == 2)
		{
			const VertRun vr = VertexRun(v, c.pDims, 1024);
			if (RunIsMesh(vr, c.pDims))
			{
				c.pfnLog("        VERTICES? %s+%02X -> %08X: %d triples,"
						 " %d non-zero, bbox x %.1f..%.1f y %.1f..%.1f"
						 " z %.1f..%.1f", pszPath, off, v, vr.n, vr.nNonZero,
						 vr.lo[0], vr.hi[0], vr.lo[1], vr.hi[1],
						 vr.lo[2], vr.hi[2]);
				if (++c.nHits >= 40) return;
			}
		}

		if (nDepth > 0 && nk == 2 && v != pRoot)
		{
			char szNew[96];
			sprintf_s(szNew, "%s+%02X", pszPath, off);
			if (strlen(szNew) < 80) WalkForMesh(c, v, nDepth - 1, szNew);
		}
	}
}

} // namespace

// Scan committed private memory for filenames ending .abc, and for whatever
// points at them.
//
// Walking OUT from the object did not reach one within three levels, and the
// node count matched nothing - which is fair, because the client COUNTS nodes
// by walking the skeleton, so nothing has to store that number. The filename
// does have to exist somewhere: a LithTech model IS an .abc file. Find the
// strings, then find the pointers to them, and those are the model records.
void R3D_FindAbc(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;

	uint32_t found[64];
	char     names[64][64];
	int      nFound = 0;
	size_t   nBytes = 0;
	int      nRegions = 0;

	MEMORY_BASIC_INFORMATION mbi{};
	for (uintptr_t a = 0x00010000; a < 0x7FF00000 && nFound < 64; )
	{
		if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) break;
		const uintptr_t base = (uintptr_t)mbi.BaseAddress;
		const size_t    size = mbi.RegionSize;
		const DWORD     bad  = PAGE_NOACCESS | PAGE_GUARD;

		if (mbi.State == MEM_COMMIT && !(mbi.Protect & bad) &&
			(mbi.Type == MEM_PRIVATE) && size >= 16 && size < 0x08000000)
		{
			++nRegions; nBytes += size;
			const char* q = (const char*)base;
			for (size_t i = 4; i + 1 < size && nFound < 64; ++i)
			{
				if (q[i] != 'c' && q[i] != 'C') continue;
				if (q[i-1] != 'b' && q[i-1] != 'B') continue;
				if (q[i-2] != 'a' && q[i-2] != 'A') continue;
				if (q[i-3] != '.') continue;
				if (q[i+1] != 0) continue;
				// walk back to the start of the printable run
				size_t j = i - 3;
				while (j > 0 && q[j-1] >= 32 && q[j-1] <= 126 && (i - j) < 60) --j;
				const size_t len = i + 1 - j;
				if (len < 6) continue;
				found[nFound] = (uint32_t)(base + j);
				memcpy(names[nFound], q + j, len);
				names[nFound][len] = 0;
				++nFound;
				i += 4;
			}
		}
		a = base + size;
		if (size == 0) break;
	}

	pfnLog("  R3D ABC: scanned %d private regions, %u KB - found %d names"
		   " ending .abc", nRegions, (unsigned)(nBytes / 1024), nFound);
	for (int i = 0; i < nFound && i < 16; ++i)
		pfnLog("        %08X  \"%s\"", found[i], names[i]);
	if (!nFound) return;

	// Who points at the first few? That pointer sits inside the model's own
	// record, and the mesh hangs off the same record.
	for (int i = 0; i < nFound && i < 3; ++i)
	{
		int nRefs = 0;
		pfnLog("    --- pointers to %08X (\"%s\") ---", found[i], names[i]);
		for (uintptr_t a = 0x00010000; a < 0x7FF00000 && nRefs < 8; )
		{
			if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) break;
			const uintptr_t base = (uintptr_t)mbi.BaseAddress;
			const size_t    size = mbi.RegionSize;
			const DWORD     bad  = PAGE_NOACCESS | PAGE_GUARD;
			if (mbi.State == MEM_COMMIT && !(mbi.Protect & bad) &&
				mbi.Type == MEM_PRIVATE && size >= 4 && size < 0x08000000)
			{
				const uint32_t* w = (const uint32_t*)base;
				for (size_t k = 0; k + 1 < size / 4 && nRefs < 8; ++k)
					if (w[k] == found[i])
					{
						const uint32_t at = (uint32_t)(base + k * 4);
						pfnLog("        referenced from %08X"
							   "   [-8]%08X [-4]%08X [+4]%08X [+8]%08X",
							   at, Word(at - 8, 0), Word(at - 4, 0),
							   Word(at + 4, 0), Word(at + 8, 0));
						++nRefs;
					}
			}
			a = base + size;
			if (size == 0) break;
		}
		if (!nRefs) pfnLog("        nothing points at it (it may be inline)");
	}
}

// How well does reading this array at this stride reproduce the box the engine
// reports for the model? Returns the fraction of the published extent covered
// on the worst of the three axes; 1.0 is a perfect match, 0 is nothing.
static float StrideFit(uint32_t pData, uint32_t nCount, uint32_t nStride,
					   const float* pDims, float* pLo, float* pHi, int* pnGood)
{
	float lo[3] = {  1e30f,  1e30f,  1e30f };
	float hi[3] = { -1e30f, -1e30f, -1e30f };
	int nGood = 0;
	const uint32_t nMax = (nCount > 4096) ? 4096 : nCount;
	for (uint32_t i = 0; i < nMax; ++i)
	{
		const uint32_t q = pData + i * nStride;
		if (MemKind(q, 12) != 2) break;
		const float* v = (const float*)(uintptr_t)q;
		bool bOK = true;
		for (int k = 0; k < 3; ++k)
		{
			const float a = v[k];
			if (!(a == a) || a > 1e6f || a < -1e6f) { bOK = false; break; }
		}
		if (!bOK) continue;
		++nGood;
		for (int k = 0; k < 3; ++k)
		{
			if (v[k] < lo[k]) lo[k] = v[k];
			if (v[k] > hi[k]) hi[k] = v[k];
		}
	}
	if (nGood < 8) return 0.0f;
	float fWorst = 1e30f;
	for (int k = 0; k < 3; ++k)
	{
		pLo[k] = lo[k]; pHi[k] = hi[k];
		const float ext  = hi[k] - lo[k];
		const float want = (pDims[k] > 1.0f ? pDims[k] : 1.0f) * 2.0f;
		// how close is the measured extent to the reported one, either way
		const float r = (ext <= 0.0f) ? 0.0f
					  : (ext < want ? ext / want : want / ext);
		if (r < fWorst) fWorst = r;
	}
	*pnGood = nGood;
	return fWorst;
}

void R3D_MeshStride(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveModels || !g_Models.nCount) { pfnLog("  R3D STRIDE: no models"); return; }

	static const uint32_t kStrides[] =
		{ 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 72, 80, 96, 128 };

	uint32_t nDone = 0, nLast = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < g_Models.nCount && nDone < 2; ++i)
	{
		const VRModelInst& mi = g_Models.inst[i];
		if (!mi.nObject || mi.nNodeCount < 3) continue;
		if (mi.nNodeCount == nLast) continue;
		if (mi.nNodeCount < 20 && nDone == 0) continue;		// a character first
		nLast = mi.nNodeCount; ++nDone;

		const uint32_t pModel = Word(mi.nObject, 0x1DC);
		if (MemKind(pModel, 0x100) != 2) continue;

		pfnLog("    === instance %u, %u nodes, dims (%.0f %.0f %.0f),"
			   " model %08X ===",
			   i, mi.nNodeCount, mi.fDims[0], mi.fDims[1], mi.fDims[2], pModel);

		// Find the arrays by SHAPE: vtable in an image, then a heap pointer,
		// then a plausible count. The entries are not all where a first read
		// of the dump suggested.
		for (uint32_t off = 0; off + 12 <= 0x100; off += 4)
		{
			const uint32_t vt  = Word(pModel, off);
			const uint32_t ptr = Word(pModel, off + 4);
			const uint32_t cnt = Word(pModel, off + 8);
			if (MemKind(vt, 4) != 1) continue;			// vtable, in an image
			if (MemKind(ptr, 16) != 2) continue;		// data, on the heap
			if (cnt < 4 || cnt > 200000) continue;

			pfnLog("      array at model+%02X: data %08X, %u entries", off + 4, ptr, cnt);

			// the first element, raw, so the shape is on record
			const uint32_t* w = (const uint32_t*)(uintptr_t)ptr;
			const float*    f = (const float*)(uintptr_t)ptr;
			if (MemKind(ptr, 32) == 2)
			{
				pfnLog("        hex   %08X %08X %08X %08X %08X %08X %08X %08X",
					   w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
				pfnLog("        float %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f %9.3f",
					   f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
			}

			float fBest = 0.0f; uint32_t nBest = 0;
			for (size_t k = 0; k < sizeof kStrides / sizeof kStrides[0]; ++k)
			{
				float lo[3], hi[3]; int nGood = 0;
				const float fit = StrideFit(ptr, cnt, kStrides[k], mi.fDims,
											lo, hi, &nGood);
				if (fit > 0.45f)
					pfnLog("        stride %3u: fit %.2f over %d, bbox"
						   " x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f",
						   kStrides[k], fit, nGood,
						   lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
				if (fit > fBest) { fBest = fit; nBest = kStrides[k]; }
			}
			if (fBest > 0.45f)
				pfnLog("        -> BEST stride %u, fit %.2f"
					   "   (%u entries x %u = %u bytes)",
					   nBest, fBest, cnt, nBest, cnt * nBest);
			else
				pfnLog("        no stride reproduces the model's box"
					   " - not vertices, or not in model space");
		}
	}
}

// The vertex record, read rather than inferred.
//
// The prop fits stride 12 and reads "pos.x pos.y pos.z, 1.000, int 1" - a
// position, a full weight and a node index. The character reads "pos, 0.450,
// int 3" - a partial weight, so it carries more than one and its record is
// longer. Guessing the stride from a fit score cannot separate those; the
// bytes can.
void R3D_MeshDump(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveModels || !g_Models.nCount) return;

	uint32_t nDone = 0, nLast = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < g_Models.nCount && nDone < 2; ++i)
	{
		const VRModelInst& mi = g_Models.inst[i];
		if (!mi.nObject || mi.nNodeCount < 3) continue;
		if (mi.nNodeCount == nLast) continue;
		if (mi.nNodeCount < 20 && nDone == 0) continue;		// character first
		nLast = mi.nNodeCount; ++nDone;

		const uint32_t pModel = Word(mi.nObject, 0x1DC);
		if (MemKind(pModel, 0x100) != 2) continue;

		pfnLog("    === instance %u, %u nodes, dims (%.0f %.0f %.0f) ===",
			   i, mi.nNodeCount, mi.fDims[0], mi.fDims[1], mi.fDims[2]);
		pfnLog("      model+60..+6C scalars: %u %u %u %u",
			   Word(pModel, 0x60), Word(pModel, 0x64),
			   Word(pModel, 0x68), Word(pModel, 0x6C));

		const uint32_t ptr = Word(pModel, 0x58);
		const uint32_t cnt = Word(pModel, 0x5C);
		if (MemKind(ptr, 256) != 2) continue;
		pfnLog("      array model+58: %08X, %u entries. First 256 bytes:", ptr, cnt);

		for (uint32_t r = 0; r < 16; ++r)
		{
			const uint32_t q = ptr + r * 16;
			if (MemKind(q, 16) != 2) break;
			const uint32_t* w = (const uint32_t*)(uintptr_t)q;
			const float*    f = (const float*)(uintptr_t)q;
			char szF[128];
			// print a dword as a float only when it looks like one
			char szBits[4][20];
			for (int k = 0; k < 4; ++k)
			{
				const float a2 = f[k];
				const uint32_t u = w[k];
				if (u < 0x10000)               sprintf_s(szBits[k], "int %-9u", u);
				else if (!(a2 == a2))          sprintf_s(szBits[k], "%-13s", "NaN");
				else if (a2 > -1e6f && a2 < 1e6f) sprintf_s(szBits[k], "%13.4f", a2);
				else                           sprintf_s(szBits[k], "%-13s", "big");
			}
			sprintf_s(szF, "%s %s %s %s", szBits[0], szBits[1], szBits[2], szBits[3]);
			pfnLog("        +%03X  %08X %08X %08X %08X  | %s",
				   r * 16, w[0], w[1], w[2], w[3], szF);
		}
	}
}

// Skin one bone-space position with one node transform, both conventions.
// m is 12 floats: three basis rows then the translation.
static void SkinBoth(const float* m, const float* p, float* pA, float* pB)
{
	// It is a 3x4 ROW-MAJOR matrix with the translation in the 4th COLUMN, not
	// "three basis rows then the translation" as VRShared.h says. Proved by the
	// data: for an object the engine places at (895.9 -1432.0 1475.9), m[3],
	// m[7] and m[11] are 895.875, -1430.616 and 1475.9, and m[9..11] are not.
	// Reading it the documented way put the translation in the rotation and
	// scattered the mesh over tens of thousands of units.
	// A: world = M . p
	pA[0] = m[0]*p[0] + m[1]*p[1] + m[2] *p[2] + m[3];
	pA[1] = m[4]*p[0] + m[5]*p[1] + m[6] *p[2] + m[7];
	pA[2] = m[8]*p[0] + m[9]*p[1] + m[10]*p[2] + m[11];
	// B: the same with the 3x3 transposed, in case a model disagrees
	pB[0] = m[0]*p[0] + m[4]*p[1] + m[8] *p[2] + m[3];
	pB[1] = m[1]*p[0] + m[5]*p[1] + m[9] *p[2] + m[7];
	pB[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[11];
}

void R3D_MeshVerify(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveModels || !g_Models.nCount) return;

	uint32_t nDone = 0, nLast = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < g_Models.nCount && nDone < 3; ++i)
	{
		const VRModelInst& mi = g_Models.inst[i];
		if (!mi.nObject || mi.nNodeCount < 3) continue;
		if (mi.nNodeCount == nLast) continue;
		nLast = mi.nNodeCount; ++nDone;

		const uint32_t pModel = Word(mi.nObject, 0x1DC);
		if (MemKind(pModel, 0x100) != 2) continue;
		const uint32_t ptr = Word(pModel, 0x58);
		const uint32_t cnt = Word(pModel, 0x5C);
		const uint32_t nDeclVerts = Word(pModel, 0x64);
		if (MemKind(ptr, 20) != 2 || cnt < 1) continue;
		if (mi.nNodeFirst + mi.nNodeCount > VRMODELS_MAX_NODES) continue;

		pfnLog("    === instance %u: %u nodes, %u records, model says %u vertices ===",
			   i, mi.nNodeCount, cnt, nDeclVerts);
		pfnLog("        engine says: pos (%.1f %.1f %.1f), half-extents (%.1f %.1f %.1f)",
			   mi.fPos[0], mi.fPos[1], mi.fPos[2],
			   mi.fDims[0], mi.fDims[1], mi.fDims[2]);

		float loA[3] = { 1e30f,1e30f,1e30f }, hiA[3] = { -1e30f,-1e30f,-1e30f };
		float loB[3] = { 1e30f,1e30f,1e30f }, hiB[3] = { -1e30f,-1e30f,-1e30f };
		uint32_t nVerts = 0, nBadNode = 0, nOpen = 0;
		float wsum = 0.0f;
		float accA[3] = { 0,0,0 }, accB[3] = { 0,0,0 };

		for (uint32_t k = 0; k < cnt; ++k)
		{
			const uint32_t q = ptr + k * 20;
			if (MemKind(q, 20) != 2) break;
			const float*    v  = (const float*)(uintptr_t)q;
			const uint32_t  ni = *(const uint32_t*)(uintptr_t)(q + 16);
			const float     w  = v[3];
			if (!(w == w) || w < -0.01f || w > 1.01f) { ++nOpen; break; }
			if (ni >= mi.nNodeCount) { ++nBadNode; continue; }

			const float* m = g_Models.nodes[mi.nNodeFirst + ni].m;
			float pa[3], pb[3];
			SkinBoth(m, v, pa, pb);
			for (int c = 0; c < 3; ++c) { accA[c] += w * pa[c]; accB[c] += w * pb[c]; }
			wsum += w;

			if (wsum > 0.99f)			// the vertex is complete
			{
				++nVerts;
				for (int c = 0; c < 3; ++c)
				{
					if (accA[c] < loA[c]) loA[c] = accA[c];
					if (accA[c] > hiA[c]) hiA[c] = accA[c];
					if (accB[c] < loB[c]) loB[c] = accB[c];
					if (accB[c] > hiB[c]) hiB[c] = accB[c];
					accA[c] = accB[c] = 0.0f;
				}
				wsum = 0.0f;
			}
		}

		pfnLog("        grouped %u vertices from %u records"
			   " (%u records named a node out of range)", nVerts, cnt, nBadNode);

		// The node transforms themselves. If a node's translation is not near
		// the object, the fault is upstream of any skinning arithmetic - in
		// what the client publishes or in how it is indexed - and no matrix
		// convention will rescue it.
		for (uint32_t k = 0; k < mi.nNodeCount && k < 4; ++k)
		{
			const float* m = g_Models.nodes[mi.nNodeFirst + k].m;
			pfnLog("        node %u: (%.3f %.3f %.3f | %.1f) (%.3f %.3f %.3f | %.1f)"
				   " (%.3f %.3f %.3f | %.1f)", k,
				   m[0], m[1], m[2],  m[3],
				   m[4], m[5], m[6],  m[7],
				   m[8], m[9], m[10], m[11]);
		}

		// How many reconstructed vertices land inside the engine's own box?
		// A bbox is dominated by outliers; a fraction is not.
		{
			uint32_t nIn = 0, nTot = 0;
			float wsum2 = 0.0f, acc[3] = { 0,0,0 };
			for (uint32_t k = 0; k < cnt; ++k)
			{
				const uint32_t q = ptr + k * 20;
				if (MemKind(q, 20) != 2) break;
				const float*   v  = (const float*)(uintptr_t)q;
				const uint32_t ni = *(const uint32_t*)(uintptr_t)(q + 16);
				const float    w  = v[3];
				if (!(w == w) || w < -0.01f || w > 1.01f) break;
				if (ni >= mi.nNodeCount) continue;
				const float* m = g_Models.nodes[mi.nNodeFirst + ni].m;
				float pa[3], pb[3];
				SkinBoth(m, v, pa, pb);
				for (int c = 0; c < 3; ++c) acc[c] += w * pa[c];
				wsum2 += w;
				if (wsum2 > 0.99f)
				{
					++nTot;
					bool bIn = true;
					for (int c = 0; c < 3; ++c)
					{
						const float d = acc[c] - mi.fPos[c];
						const float lim = (mi.fDims[c] > 1.0f ? mi.fDims[c] : 1.0f) * 3.0f;
						if (d < -lim || d > lim) bIn = false;
						acc[c] = 0.0f;
					}
					if (bIn) ++nIn;
					wsum2 = 0.0f;
				}
			}
			pfnLog("        convention A: %u of %u vertices land inside the"
				   " engine's box (%.0f%%)", nIn, nTot,
				   nTot ? (nIn * 100.0f / nTot) : 0.0f);
		}

		for (int arm = 0; arm < 2; ++arm)
		{
			const float* lo = arm ? loB : loA;
			const float* hi = arm ? hiB : hiA;
			if (lo[0] > 1e29f) { pfnLog("        %c: nothing", arm ? 'B' : 'A'); continue; }
			const float cx = (lo[0]+hi[0])*0.5f, cy = (lo[1]+hi[1])*0.5f, cz = (lo[2]+hi[2])*0.5f;
			const float ex = (hi[0]-lo[0])*0.5f, ey = (hi[1]-lo[1])*0.5f, ez = (hi[2]-lo[2])*0.5f;
			pfnLog("        %c: centre (%.1f %.1f %.1f)  half-extents (%.1f %.1f %.1f)"
				   "   off by (%.1f %.1f %.1f)",
				   arm ? 'B' : 'A', cx, cy, cz, ex, ey, ez,
				   cx - mi.fPos[0], cy - mi.fPos[1], cz - mi.fPos[2]);
		}
	}
}

// Does this block read as a list of vertex indices?
// Returns how many consecutive values are in range; 0 if the first few are not.
static uint32_t IndexRun(uint32_t p, uint32_t nVerts, int nWide, uint32_t nMax)
{
	uint32_t n = 0, nHigh = 0;
	for (; n < nMax; ++n)
	{
		const uint32_t q = p + n * (uint32_t)nWide;
		if (MemKind(q, (size_t)nWide) != 2) break;
		const uint32_t v = (nWide == 2)
			? *(const uint16_t*)(uintptr_t)q
			: *(const uint32_t*)(uintptr_t)q;
		if (v >= nVerts) break;
		if (v >= nVerts / 2) ++nHigh;		// guard against a run of zeros
	}
	// a real index list reaches the far end of the vertex list
	return (nHigh >= 4) ? n : 0;
}

void R3D_FindFaces(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveModels || !g_Models.nCount) return;

	uint32_t nDone = 0, nLast = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < g_Models.nCount && nDone < 2; ++i)
	{
		const VRModelInst& mi = g_Models.inst[i];
		if (!mi.nObject || mi.nNodeCount < 3) continue;
		if (mi.nNodeCount == nLast) continue;
		nLast = mi.nNodeCount; ++nDone;

		const uint32_t pModel = Word(mi.nObject, 0x1DC);
		if (MemKind(pModel, 0x100) != 2) continue;

		const uint32_t nVerts  = Word(pModel, 0x64);
		const uint32_t nFaces  = Word(pModel, 0x68);
		const uint32_t nPieces = Word(pModel, 0x6C);
		const uint32_t pPieces = Word(pModel, 0x38);
		const uint32_t nPieceA = Word(pModel, 0x3C);

		pfnLog("    === instance %u: %u nodes, %u verts, %u faces, %u pieces;"
			   " piece array %08X x%u ===",
			   i, mi.nNodeCount, nVerts, nFaces, nPieces, pPieces, nPieceA);
		if (MemKind(pPieces, 4) != 2) { pfnLog("        no piece array"); continue; }

		for (uint32_t k = 0; k < nPieceA && k < 3; ++k)
		{
			const uint32_t pc = Word(pPieces, k * 4);
			if (MemKind(pc, 0x80) != 2)
			{ pfnLog("      piece %u: %08X not readable", k, pc); continue; }

			pfnLog("      --- piece %u at %08X ---", k, pc);
			for (uint32_t off = 0; off < 0x80; off += 16)
			{
				char szKind[8];
				for (int c = 0; c < 4; ++c)
				{
					const uint32_t w = Word(pc, off + (uint32_t)c * 4);
					const int kk = MemKind(w, 4);
					szKind[c] = (w >= pc && w < pc + 0x200) ? 's'
							  : (kk == 2) ? 'h' : (kk == 1) ? 'i' : '.';
				}
				szKind[4] = 0;
				pfnLog("        +%02X  %08X %08X %08X %08X   %s", off,
					   Word(pc, off),     Word(pc, off + 4),
					   Word(pc, off + 8), Word(pc, off + 12), szKind);
			}

			// The face array: piece+14, count piece+18. Both counts are exact
			// (98/134 and 52/77 against the model's own figures), so dump the
			// records rather than test them - a face is a struct with indices,
			// UVs and a normal, which is why an index-list test found nothing.
			{
				const uint32_t pV  = Word(pc, 0x04), nV = Word(pc, 0x08);
				const uint32_t pF  = Word(pc, 0x14), nF = Word(pc, 0x18);
				pfnLog("        piece verts %08X x%u, faces %08X x%u",
					   pV, nV, pF, nF);
				if (MemKind(pF, 128) == 2 && nF)
				{
					pfnLog("        FACE records, first 128 bytes:");
					for (uint32_t r = 0; r < 8; ++r)
					{
						const uint32_t q = pF + r * 16;
						if (MemKind(q, 16) != 2) break;
						const uint32_t* w = (const uint32_t*)(uintptr_t)q;
						const uint16_t* h = (const uint16_t*)(uintptr_t)q;
						const float*    f = (const float*)(uintptr_t)q;
						pfnLog("          +%02X %08X %08X %08X %08X | u16 %5u %5u"
							   " %5u %5u %5u %5u %5u %5u | f %8.3f %8.3f",
							   r * 16, w[0], w[1], w[2], w[3],
							   h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
							   f[0], f[1]);
					}
				}
			}

			// Every heap pointer in the piece, tested as an index list.
			for (uint32_t off = 0; off < 0x80; off += 4)
			{
				const uint32_t v = Word(pc, off);
				if (MemKind(v, 32) != 2) continue;
				const uint32_t n16 = IndexRun(v, nVerts ? nVerts : 65535, 2, 8192);
				const uint32_t n32 = IndexRun(v, nVerts ? nVerts : 65535, 4, 8192);
				if (n16 >= 24 || n32 >= 24)
				{
					const uint16_t* h = (const uint16_t*)(uintptr_t)v;
					pfnLog("        +%02X -> %08X: INDICES? %u as uint16, %u as"
						   " uint32   first16 %u %u %u %u %u %u",
						   off, v, n16, n32, h[0], h[1], h[2], h[3], h[4], h[5]);
					if (nFaces && n16 >= 24)
						pfnLog("             %u uint16 / 3 = %u triangles"
							   " (model says %u faces)", n16, n16 / 3, nFaces);
				}
			}
		}
	}
}

void R3D_ModelWalk(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveModels || !g_Models.nCount)
	{
		pfnLog("  R3D MESH: no models published yet"
			   " - is VRModels on and is the client in step?");
		return;
	}

	pfnLog("  R3D MESH: walking from the HOBJECT. %u instances published.",
		   g_Models.nCount);

	// Instances of DIFFERENT node counts, so a field that matches all of them
	// is a structure rather than an accident that lined up once.
	uint32_t nDone = 0, nLastNodes = 0xFFFFFFFFu;
	for (uint32_t i = 0; i < g_Models.nCount && nDone < 4; ++i)
	{
		const VRModelInst& mi = g_Models.inst[i];
		if (!mi.nObject || mi.nNodeCount < 3) continue;
		if (mi.nNodeCount == nLastNodes) continue;		// spread the sample
		nLastNodes = mi.nNodeCount;
		++nDone;

		pfnLog("    --- instance %u: HOBJECT %08X, %u nodes,"
			   " dims (%.0f %.0f %.0f)%s ---",
			   i, mi.nObject, mi.nNodeCount,
			   mi.fDims[0], mi.fDims[1], mi.fDims[2],
			   (mi.nFlags & 1) ? "  [the player]" : "");

		const int nk = MemKind(mi.nObject, 0x60);
		if (nk != 2)
		{
			pfnLog("        the handle is %s - not heap, so it is not an object"
				   " pointer", nk == 1 ? "inside a module image" : "unreadable");
			continue;
		}

		// 0x200, not 0x60. The first 0x60 is vtable, object-list links and an
		// id; whatever carries the model is past it, and it was never looked at.
		// Each field is marked: h = heap, i = a module image, . = small/zero,
		// s = points at itself (an intrusive list node).
		for (uint32_t off = 0; off < 0x200; off += 16)
		{
			char szKind[8];
			for (int k = 0; k < 4; ++k)
			{
				const uint32_t w = Word(mi.nObject, off + (uint32_t)k * 4);
				const int kk = MemKind(w, 4);
				szKind[k] = (w >= mi.nObject && w < mi.nObject + 0x200) ? 's'
						  : (kk == 2) ? 'h' : (kk == 1) ? 'i' : '.';
			}
			szKind[4] = 0;
			pfnLog("        +%03X  %08X %08X %08X %08X   %s", off,
				   Word(mi.nObject, off),      Word(mi.nObject, off + 4),
				   Word(mi.nObject, off + 8),  Word(mi.nObject, off + 12),
				   szKind);
		}

		// object+0x1DC is the model. Every NODE COUNT hit for the 25-node
		// character resolved through a chain that lands there, and so did the
		// vertex-looking runs - the walker's path reads oddly only because
		// +14 and +10 are intrusive list nodes pointing back into the object.
		// Map it directly rather than through the walker's path notation.
		const uint32_t pModel = Word(mi.nObject, 0x1DC);
		if (MemKind(pModel, 0x100) == 2)
		{
			pfnLog("        MODEL at object+1DC -> %08X", pModel);
			for (uint32_t off = 0; off < 0x100; off += 16)
			{
				char szKind[8];
				for (int k = 0; k < 4; ++k)
				{
					const uint32_t w = Word(pModel, off + (uint32_t)k * 4);
					const int kk = MemKind(w, 4);
					szKind[k] = (w >= pModel && w < pModel + 0x400) ? 's'
							  : (kk == 2) ? 'h' : (kk == 1) ? 'i' : '.';
				}
				szKind[4] = 0;
				pfnLog("          +%02X  %08X %08X %08X %08X   %s", off,
					   Word(pModel, off),     Word(pModel, off + 4),
					   Word(pModel, off + 8), Word(pModel, off + 12), szKind);
			}
			// every heap pointer in it, measured as a vertex run - no cap, so
			// the real length is visible rather than clipped at 1024
			for (uint32_t off = 0; off < 0x100; off += 4)
			{
				const uint32_t v = Word(pModel, off);
				if (MemKind(v, 64) != 2) continue;
				const VertRun r = VertexRun(v, mi.fDims, 65536);
				if (r.n >= 24 && r.nNonZero * 2 >= r.n)
					pfnLog("          +%02X -> %08X: %d triples, %d non-zero,"
						   " bbox x %.1f..%.1f y %.1f..%.1f z %.1f..%.1f",
						   off, v, r.n, r.nNonZero,
						   r.lo[0], r.hi[0], r.lo[1], r.hi[1], r.lo[2], r.hi[2]);
			}
		}
		else pfnLog("        object+1DC is not a heap pointer (%08X)", pModel);

		WalkCtx c{};
		c.nWantNodes = mi.nNodeCount;
		c.pDims = mi.fDims;
		c.pfnLog = pfnLog;
		WalkForMesh(c, mi.nObject, 3, "obj");
		if (!c.nHits)
			pfnLog("        nothing matched %u nodes, fitted the box, or ended"
				   " .abc within 3 levels of heap pointers", mi.nNodeCount);
	}
}

// ---- THE STALL DETECTOR -----------------------------------------------
//
// Headset testing reported big, frequent frame drops. The client log
// puts them every ~180 frames - two seconds at 90 Hz - and 100 ms deep, and
// the desk reproduces the cadence exactly. A mean frame rate hides that
// completely (p50 was 11 ms the whole time); only the distribution shows it,
// which is the lesson of the SWE1R stutter. This attributes each long frame
// to what this renderer did in it, so the next report arrives as a sentence.
static LARGE_INTEGER s_qLastPresent = {};
static double        s_fWorldMsThisFrame = 0.0;
static long long     s_nProbesAtLastPresent = 0;
int  g_bCamSaidThisFrame = 0;
int  g_bAccountThisFrame = 0;
static long          s_nStallSaid = 0;
static double        s_fPresentMsThisFrame = 0.0;	// our SwapBuffers slot
static double        s_fLogMsThisFrame = 0.0;		// time inside file writes

static double s_fLimiterMs = 0.0, s_fPresentCallMs = 0.0;
void R3D_NotePresentParts(double fLimiter, double fPresent)
{ s_fLimiterMs = fLimiter; s_fPresentCallMs = fPresent; }
void R3D_AddWorldMs(double fMs)   { s_fWorldMsThisFrame += fMs; }
void R3D_AddPresentMs(double fMs) { s_fPresentMsThisFrame += fMs; }
void R3D_AddLogMs(double fMs)     { s_fLogMsThisFrame += fMs; }

void R3D_NotePresent()
{
	{
		LARGE_INTEGER q, f;
		QueryPerformanceCounter(&q);
		QueryPerformanceFrequency(&f);
		if (s_qLastPresent.QuadPart && f.QuadPart)
		{
			const double fMs = (double)(q.QuadPart - s_qLastPresent.QuadPart)
							   * 1000.0 / (double)f.QuadPart;
			if (fMs > 25.0 && s_nStallSaid < 60)
			{
				++s_nStallSaid;
				// The four buckets add up to the frame or they do not, and
				// whatever is left over belongs to the ENGINE and the CLIENT -
				// which is the bucket a renderer log cannot see into, and the
				// one that says "not us" when it is large.
				Log("  STALL: %.0f ms between presents | world passes %.1f"
					" | present slot %.1f = limiter %.1f + Present() %.1f +"
					" publish/other %.1f | log writes %.1f | UNACCOUNTED"
					" (engine+client) %.1f | page probes +%lld | cam %d acct %d"
					" | frame %ld",
					fMs, s_fWorldMsThisFrame, s_fPresentMsThisFrame,
					s_fLimiterMs, s_fPresentCallMs,
					s_fPresentMsThisFrame - s_fLimiterMs - s_fPresentCallMs,
					s_fLogMsThisFrame,
					fMs - s_fWorldMsThisFrame - s_fPresentMsThisFrame
						- s_fLogMsThisFrame,
					(long long)(g_nPageProbes - s_nProbesAtLastPresent),
					g_bCamSaidThisFrame, g_bAccountThisFrame, g_nFrames);
			}
		}
		s_qLastPresent = q;
		s_fWorldMsThisFrame = 0.0;
		s_fPresentMsThisFrame = 0.0;
		s_fLogMsThisFrame = 0.0;
		s_nProbesAtLastPresent = g_nPageProbes;
		g_bCamSaidThisFrame = 0;
		g_bAccountThisFrame = 0;
	}
	// A new frame: the multisampled target may be cleared again on the
	// next world pass. Set here rather than in the pass itself because the
	// pass runs once per EYE and both share the target.
	g_bMsaaFresh = 1;
	// The engine's own frame boundary: EndFrameScenes is called from the same
	// slot. Frames and presents are 1:1 here and have been measured so - 3107
	// SwapBuffers against 3108 presents in a full session.
	InterlockedIncrement(&g_nFrames);
}

extern "C" void __cdecl R3D_PublishModels(const VRModelFrame* p)
{
	RenderGuard _renderGuard;
	if (!p || IsBadReadPtr(p, sizeof(uint32_t) * 6)) return;
	if (p->nMagic != VRMODELS_MAGIC || p->nVersion != VRMODELS_VERSION) return;
	if (p->nCount > VRMODELS_MAX_INST || p->nNodeCount > VRMODELS_MAX_NODES) return;
	if (IsBadReadPtr(p, sizeof(VRModelFrame))) return;

	// Does this client say what the player owns? See g_bOwnedFlagSeen. The
	// count is validated above.
	if (!g_bOwnedFlagSeen)
		for (uint32_t i = 0; i < p->nCount; ++i)
			if (p->inst[i].nFlags & VRMODEL_F_OWNED) { g_bOwnedFlagSeen = true; break; }

	// COPY WHAT IS USED, not what the array can hold. The node array grew from
	// 3072 to 8192 entries to let the publish cap rise, which took the struct
	// from about 150 KB to 420 KB - and this runs once per published frame, so
	// a blind full-size memcpy would spend the frame budget carrying zeroes.
	// The header and the instance array are small and fixed; the nodes are the
	// bulk, and nNodeCount says how many of them are real.
	{
		AgePageCache(p->nFrame);
		const size_t nHead = offsetof(VRModelFrame, nodes);
		memcpy(&g_Models, p, nHead);
		if (p->nNodeCount)
			memcpy(g_Models.nodes, p->nodes,
				   (size_t)p->nNodeCount * sizeof(VRModelNode));
	}
	g_bHaveModels = 1;

	static int s_bSaid = 0;
	if (!s_bSaid && p->nCount)
	{
		s_bSaid = 1;
		Log("  R3D MODELS: the client published %u instances, %u nodes"
			" (%u dropped) - the channel is live",
			p->nCount, p->nNodeCount, p->nDropped);
	}
}
// THE LEVEL'S FOG, from the client. The values are engine console variables
// the client already computes for every level - it has been setting them since
// 2000 and only the retail renderer ever read them.
extern "C" void __cdecl R3D_PublishLightAdd(float r, float g, float b)
{
	if (r == g_fLightAdd[0] && g == g_fLightAdd[1] && b == g_fLightAdd[2]) return;
	// Said when it starts and when it ends, not on every step of a fade: a
	// ramp writes a line per frame otherwise (1395 in one session).
	const bool bWas = (g_fLightAdd[0] > 0.002f || g_fLightAdd[1] > 0.002f || g_fLightAdd[2] > 0.002f);
	const bool bNow = (r > 0.002f || g > 0.002f || b > 0.002f);
	g_fLightAdd[0] = r; g_fLightAdd[1] = g; g_fLightAdd[2] = b;
	if (bWas != bNow)
		Log("  R3D LIGHT ADD: %s (%.3f %.3f %.3f)%s", bNow ? "on" : "off", r, g, b,
			g_bLightAddAllow ? "" : "  (StubLightAdd 0: not drawn)");
}
void R3D_SetLightAddAllow(int b) { g_bLightAddAllow = b; }
void R3D_SetBatchList(int b) { g_bBatchList = b; }
void R3D_SetEnvDepth(int b) { g_bEnvDepth = b; }

void R3D_SetWaterFlow(float f) { g_fWaterFlow = f; }

void R3D_SetWaterFirst(int b) { g_bWaterFirst = b; }

void R3D_SetWaterTop(int bOn) { g_bWaterTop = bOn; }

void R3D_SetWaterLook(float fLight, float fContrast) { g_fWaterLight = fLight; if (fContrast > 0.0f) g_fEnvContrast = fContrast; }
void R3D_SetEnvWorld(float fRepU, float fRepV, float fFlow)
{ if (fRepU > 0.0f) g_fEnvRepeatU = fRepU; if (fRepV > 0.0f) g_fEnvRepeatV = fRepV; g_fEnvFlow = fFlow; }
void R3D_SetSkipBatch(int n) { g_nSkipBatch = n; }
void R3D_SetEnvCoord(float f) { if (f > 0.0f) g_fEnvCoord = f; }
void R3D_SetEnvMap(int b, float fScale, float fPan)
{ g_bEnvMap = b; g_fEnvScale = (fScale > 0.0f) ? fScale : 0.5f; g_fEnvPan = fPan; }

extern "C" void __cdecl R3D_PublishSkyFog(int bEnable, float fNear, float fFar)
{
	const int bNew = bEnable ? 1 : 0;
	if (bNew == g_bSkyFog && fNear == g_fSkyFogNear && fFar == g_fSkyFogFar) return;
	g_bSkyFog = bNew; g_fSkyFogNear = fNear; g_fSkyFogFar = fFar;
	Log("  R3D SKY FOG: %s  near %.0f far %.0f (in the level's fog colour)",
		bNew ? "ON" : "off", fNear, fFar);
}
void R3D_SetSkyFogAllow(int b) { g_bSkyFogAllow = b; }

extern "C" void __cdecl R3D_PublishFog(int bEnable, float r, float g, float b,
									   float fNear, float fFar)
{
	RenderGuard _renderGuard;
	// A far plane at or inside the near one is not fog, it is a division by
	// zero waiting to happen. Refuse it and leave the last good setting.
	if (bEnable && !(fFar > fNear)) return;
	const int bNew = bEnable ? 1 : 0;
	if (bNew == g_bFog && r == g_fFogRGB[0] && g == g_fFogRGB[1]
		&& b == g_fFogRGB[2] && fNear == g_fFogNear && fFar == g_fFogFar)
		return;						// nothing changed; do not touch the buffer
	g_bFog = bNew;
	g_fFogRGB[0] = r; g_fFogRGB[1] = g; g_fFogRGB[2] = b;
	// STORED WHETHER OR NOT FOG IS ON. Storing the distances only while on
	// meant that with fog off and the console's FogNearZ differing from the
	// last stored one, the compare above failed on EVERY frame: the buffer
	// was re-marked dirty and this line was written 5817 times in the tester's
	// five-minute session of 13 September - a disk write inside a quarter
	// of the frames, on a machine that was missing 90 by a few per cent.
	g_fFogNear = fNear; g_fFogFar = fFar;
	g_bFogDirty = 1;
	Log("  R3D FOG: %s  colour %.2f %.2f %.2f  near %.0f far %.0f",
		(g_bFog && g_bFogAllow) ? "ON" : "off",
		r, g, b, g_fFogNear, g_fFogFar);
}

// What the frame should be CLEARED to. Where a level has no skybox panorama -
// and the intro forest has none, its 36 skybox polygons all wear a marker
// texture - the background is whatever the clear left behind, and retail
// leaves the fog colour there. That is the difference between a dark blue
// night sky and a pure black one.
extern "C" int __cdecl R3D_FogClearColour(float* pRGB)
{
	RenderGuard _renderGuard;
	if (!g_bFog || !g_bFogAllow || !pRGB) return 0;
	pRGB[0] = g_fFogRGB[0]; pRGB[1] = g_fFogRGB[1]; pRGB[2] = g_fFogRGB[2];
	return 1;
}

void R3D_SetFog(int b) { g_bFogAllow = b; g_bFogDirty = 1; }
void R3D_SetSkinNamePeriod(int n) { g_nSkinNamePeriod = n; }
void R3D_SetSkySkip(const char* psz) { strncpy(g_szSkySkip, psz ? psz : "", sizeof g_szSkySkip - 1); }
void R3D_SetWaterFlowAny(int b) { g_bWaterFlowAny = b; }
void R3D_SetWaterBias(int n) { g_nWaterBias = n; }
void R3D_SetVolumeVisible(int b) { g_bVolumeVisible = b; }
void R3D_SetSkyInGlass(int b) { g_bSkyInGlass = b; }
void R3D_SetSkyCamCentre(int b) { g_bSkyCamCentre = b; }
void R3D_SetSkyTrace(int n) { g_nSkyTrace = n; }
void R3D_SetSkyParallax(int b) { g_bSkyParallax = b; }
void R3D_SetCullFromEye(int b) { g_bCullFromEye = b; }
void R3D_SetBatchWatch(const char* s) { strncpy(g_szBatchWatch, s ? s : "", sizeof g_szBatchWatch - 1); }
void R3D_SetModelWatch(const char* s) { strncpy(g_szModelWatch, s ? s : "", sizeof g_szModelWatch - 1); }
void R3D_SetGlassDepth(int b, int nCut100) { g_bGlassDepth = b; if (nCut100 > 0) g_fGlassDepthCut = nCut100 * 0.01f; }
void R3D_SetSkyChecker(int b) { g_bSkyChecker = b; }

extern "C" void __cdecl R3D_PublishSprites(const VRSpriteFrame* p)
{
	RenderGuard _renderGuard;
	if (!p || IsBadReadPtr(p, sizeof(uint32_t) * 4)) return;
	if (p->nMagic != VRSPRITE_MAGIC || p->nVersion != VRSPRITE_VERSION) return;
	if (p->nCount > VRSPRITE_MAX) return;
	const size_t nBytes = offsetof(VRSpriteFrame, inst)
						+ (size_t)p->nCount * sizeof(VRSpriteInst);
	if (IsBadReadPtr(p, nBytes)) return;
	memcpy(&g_Spr, p, nBytes);
	g_Spr.nCount = p->nCount;
	g_bHaveSprites = 1;

	// WHERE DOES A SPRITE KEEP ITS TEXTURE? +StubSpriteProbe 1. Two answers
	// are counted per offset: the word IS a texture object, or it points at a
	// struct holding one - the same pair the model texture hunt used, because
	// a sprite's picture may sit behind one more indirection than a model's.
	if (!g_bSpriteProbe || g_bSprSaid) return;
	for (uint32_t i = 0; i < g_Spr.nCount; ++i)
	{
		const uint32_t h = g_Spr.inst[i].nObject;
		if (MemKind(h, 0x100) != 2) continue;
		++g_nSprSeen;
		for (int off = 0; off < 256; ++off)
		{
			const uint32_t v = Word(h, off * 4);
			if (!v) continue;
			if (TextureFor(v) >= 0) ++g_aSprHit[off][0];
			else if (MemKind(v, 0x40) == 2)
			{
				for (int k = 0; k < 16; ++k)
					if (TextureFor(Word(v, k * 4)) >= 0) { ++g_aSprHit[off][1]; break; }
			}
		}
	}
	if (g_nSprSeen >= 60)
	{
		g_bSprSaid = 1;
		Log("");
		Log("=== which offset in a SPRITE object names its texture ===");
		Log("  %ld sprite objects walked, %u textures known", g_nSprSeen,
			(unsigned)g_Tex.size());
		int nBest = -1; long nHits = 0; int bDirect = 1;
		for (int off = 0; off < 256; ++off)
		{
			if (!g_aSprHit[off][0] && !g_aSprHit[off][1]) continue;
			Log("   obj+0x%03X   direct %6ld   via a pointer %6ld  (of %ld)",
				off * 4, g_aSprHit[off][0], g_aSprHit[off][1], g_nSprSeen);
			if (g_aSprHit[off][0] > nHits) { nHits = g_aSprHit[off][0]; nBest = off; bDirect = 1; }
			if (g_aSprHit[off][1] > nHits) { nHits = g_aSprHit[off][1]; nBest = off; bDirect = 0; }
		}
		if (nBest < 0)
			Log("  NOTHING in a sprite object resolves to a texture the engine"
				" has bound. Its picture is loaded by another path.");
		else
			Log("  BEST: obj+0x%03X %s, %ld of %ld (%.0f%%)", nBest * 4,
				bDirect ? "IS the texture" : "points at a struct holding one",
				nHits, g_nSprSeen, 100.0 * (double)nHits / (double)g_nSprSeen);
		Log("=== end sprite probe ===");
		Log("");
	}
}

void R3D_SetSpriteProbe(int b) { g_bSpriteProbe = b; }
void R3D_SetSprites(int b) { g_bSprites = b; }
void R3D_SetSpriteScale(float f) { if (f > 0.0f) g_fSpriteScale = f; }

extern "C" void __cdecl R3D_PublishWorldModels(const VRWorldFrame* p)
{
	RenderGuard _renderGuard;
	if (!p || IsBadReadPtr(p, sizeof(uint32_t) * 4)) return;
	if (p->nMagic != VRWORLD_MAGIC || p->nVersion != VRWORLD_VERSION) return;
	if (p->nCount > VRWORLD_MAX) return;
	const size_t nBytes = offsetof(VRWorldFrame, inst)
						+ (size_t)p->nCount * sizeof(VRWorldInst);
	if (IsBadReadPtr(p, nBytes)) return;
	memcpy(&g_World, p, nBytes);
	g_World.nCount = p->nCount;
	g_bHaveWorldObjs = 1;

	// The baseline, taken the first time each model is seen after a build.
	// Nothing has moved at load, so first-seen is the authored transform.
	g_nWMMoved = 0;
	for (uint32_t i = 0; i < g_World.nCount; ++i)
	{
		const VRWorldInst& wi = g_World.inst[i];
		const uint32_t pSub = Word(wi.nObject, kObjToWorldModel);
		if (!pSub) continue;
		const WMBase* pB = WMBaseFor(pSub);
		if (!pB)
		{
			WMBase b{};
			b.pSub = pSub;
			memcpy(b.p,  wi.fPos, sizeof b.p);
			memcpy(b.r,  wi.fRot, sizeof b.r);
			memcpy(b.lp, wi.fPos, sizeof b.lp);
			memcpy(b.lr, wi.fRot, sizeof b.lr);
			b.bMoved = 0;
			b.fAlpha = wi.fAlpha;
			b.nFlags = wi.nFlags;
			// THE BASELINE IS THE FILE'S, NOT THE FIRST POSE SEEN. A save
			// restores doors already open before the first publish, and a
			// baseline taken then draws them closed. See WorldModelFile::fTrans.
			if (g_bWorldSeed && g_pWorldFile)
			{
				char szN[64] = "";
				ModelName(pSub, szN, sizeof szN);
				std::map<std::string, size_t>::const_iterator it = g_FileByName.find(szN);
				if (it != g_FileByName.end() && g_pWorldFile->Models[it->second].bHaveTrans)
				{
					const float* t = g_pWorldFile->Models[it->second].fTrans;
					const float dx = wi.fPos[0] - t[0], dy = wi.fPos[1] - t[1], dz = wi.fPos[2] - t[2];
					const float d2 = dx*dx + dy*dy + dz*dz;
					float dr = 0.0f;
					static const float kI[9] = { 1,0,0, 0,1,0, 0,0,1 };
					for (int k = 0; k < 9; ++k) dr += fabsf(wi.fRot[k] - kI[k]);
					memcpy(b.p, t, sizeof b.p);
					memcpy(b.r, kI, sizeof b.r);
					b.bMoved = (d2 > 0.25f || dr > 0.01f) ? 1 : 0;
					if (b.bMoved)
					{
						++g_nWMSeededAway;
						if (g_nWMSeededAway <= 12)
							Log("  WORLD MODEL %s was ALREADY AWAY from its authored place when first"
								" seen: %.1f units off, rotation off by %.2f (a save game, or it moved"
								" before the first publish) - drawn from the file's place, so it moves",
								szN, sqrtf(d2), dr);
					}
					if (b.bMoved) ++g_nWMMoved;
				}
			}
			g_WMBase.push_back(b);
			continue;
		}
		const float dx = wi.fPos[0] - pB->p[0];
		const float dy = wi.fPos[1] - pB->p[1];
		const float dz = wi.fPos[2] - pB->p[2];
		float dr = 0.0f;
		for (int k = 0; k < 9; ++k) dr += fabsf(wi.fRot[k] - pB->r[k]);
		WMBase* pM = const_cast<WMBase*>(pB);
		float lpPrev[3], lrPrev[9];
		memcpy(lpPrev, pM->lp, sizeof lpPrev); memcpy(lrPrev, pM->lr, sizeof lrPrev);
		memcpy(pM->lp, wi.fPos, sizeof pM->lp);
		memcpy(pM->lr, wi.fRot, sizeof pM->lr);
		pM->fAlpha = wi.fAlpha;
		pM->nFlags = wi.nFlags;
		pM->bMoved = (dx*dx + dy*dy + dz*dz > 0.25f || dr > 0.01f) ? 1 : 0;
		// SAID ONCE PER MODEL THE FIRST TIME IT IS SEEN MOVING between two
		// publishes: the HQ waterfall is a Door-class brush (Door57) and
		// whether the client ever sees it move is the whole question of
		// 13 September's frozen waterfall.
		{
			const float ldx = wi.fPos[0] - lpPrev[0], ldy = wi.fPos[1] - lpPrev[1], ldz = wi.fPos[2] - lpPrev[2];
			float ldr = 0.0f; for (int k = 0; k < 9; ++k) ldr += fabsf(wi.fRot[k] - lrPrev[k]);
			static std::set<uint32_t> s_SaidMove;
			if ((ldx*ldx + ldy*ldy + ldz*ldz > 0.01f || ldr > 0.001f) && s_SaidMove.size() < 64
				&& s_SaidMove.find(pSub) == s_SaidMove.end())
			{
				s_SaidMove.insert(pSub);
				char szMv[64] = ""; ModelName(pSub, szMv, sizeof szMv);
				Log("  WORLD MODEL %s MOVES between publishes: %.2f %.2f %.2f units, rotation %.3f"
					" (%.1f from its baseline)", szMv, ldx, ldy, ldz, ldr, sqrtf(dx*dx + dy*dy + dz*dz));
			}
		}
		// +StubWorldNudge <units>: PRETEND every world model has moved, by that
		// many units straight up. A door cannot be opened from the desk harness,
		// so the transform path would otherwise ship untested; with this, every
		// prop and door in the level should visibly rise by exactly that much and
		// nothing else should move at all. Proved with a forced offset rather
		// than with a headset. 0 is off.
		if (g_fWorldNudge != 0.0f)
		{
			pM->lp[1] = pM->p[1] + g_fWorldNudge;
			pM->bMoved = 1;
		}
		if (pM->bMoved) ++g_nWMMoved;
		pM->nMissing = 0;
		if (pM->bGone) { pM->bGone = 0; }		// back: the publish above restored its flags
	}
	// A MODEL THE PUBLISH NO LONGER CARRIES IS GONE. The engine destroyed it
	// (the aeroplane door takes a "destroy" message after its keyframer flies
	// it out) or removed it, and the sphere search that feeds this publish
	// returns everything the client has, so absence is the signal. Three
	// publishes in a row, so one dropped frame cannot blink a door out; and
	// never from a publish that hit its cap, where absence means nothing.
	if (g_World.nCount < VRWORLD_MAX)
	{
		std::set<uint32_t> seen;
		for (uint32_t i = 0; i < g_World.nCount; ++i)
		{
			const uint32_t pSub = Word(g_World.inst[i].nObject, kObjToWorldModel);
			if (pSub) seen.insert(pSub);
		}
		for (size_t b = 0; b < g_WMBase.size(); ++b)
		{
			WMBase& w = g_WMBase[b];
			if (seen.find(w.pSub) != seen.end()) continue;
			if (++w.nMissing < 3 || w.bGone) continue;
			w.bGone = 1;
			w.nFlags |= VRWORLD_F_INVIS;		// the draw loops already skip these
			static int s_nSaidGone = 0;
			if (s_nSaidGone++ < 24)
			{
				char szG[64] = ""; ModelName(w.pSub, szG, sizeof szG);
				Log("  WORLD MODEL %s GONE: not published for %d frames (destroyed or removed)"
					" - no longer drawn", szG, w.nMissing);
			}
		}
	}

	// ---- WHICH OFFSET IN AN HOBJECT NAMES ITS WORLD MODEL? -------------
	//
	// The client has HOBJECTs, this renderer has WorldModel structs, and
	// nothing yet joins them. The same question was asked of the model
	// pointer (+0x1DC) and the texture name ([[+0x24]+0x1C]) and answered the
	// same way: walk every offset over every object and count how often the
	// word there is a pointer this renderer ALREADY knows to be a world model.
	// A real link hits on nearly every object; a coincidence hits on one.
	//
	// +StubWorldProbe 1. Off by default - it is 256 reads per object.
	if (!g_bWorldProbe || g_bWObjSaid || g_SubPtrs.empty()) return;
	for (uint32_t i = 0; i < g_World.nCount; ++i)
	{
		const uint32_t hObj = g_World.inst[i].nObject;
		if (MemKind(hObj, 0x100) != 2) continue;
		++g_nWObjSeen;
		for (int off = 0; off < 256; ++off)
		{
			const uint32_t v = Word(hObj, off * 4);
			if (!v) continue;
			for (size_t k = 0; k < g_SubPtrs.size(); ++k)
				if (g_SubPtrs[k] == v) { ++g_aWObjHit[off]; break; }
		}
	}
	if (g_nWObjSeen >= 200)
	{
		g_bWObjSaid = 1;
		Log("");
		Log("=== which offset in an HOBJECT names its WORLD MODEL ===");
		Log("  %ld objects walked, %u world models known to this build",
			g_nWObjSeen, (unsigned)g_SubPtrs.size());
		int nBest = -1; long nHits = 0;
		for (int off = 0; off < 256; ++off)
		{
			if (!g_aWObjHit[off]) continue;
			Log("   obj+0x%03X   %6ld of %ld  (%.0f%%)", off * 4,
				g_aWObjHit[off], g_nWObjSeen,
				100.0 * (double)g_aWObjHit[off] / (double)g_nWObjSeen);
			if (g_aWObjHit[off] > nHits) { nHits = g_aWObjHit[off]; nBest = off; }
		}
		if (nBest < 0)
			Log("  NOTHING in the object points at a world model this build knows."
				" The link is indirect, or the client's objects are not these.");
		else
			Log("  BEST: obj+0x%03X, %ld of %ld objects (%.0f%%)",
				nBest * 4, nHits, g_nWObjSeen,
				100.0 * (double)nHits / (double)g_nWObjSeen);
		Log("=== end world model probe ===");
		Log("");
	}
}

void R3D_SetWorldProbe(int b) { g_bWorldProbe = b; }
void R3D_SetWorldXform(int b) { g_bWorldXform = b; }
void R3D_SetWorldSeed(int b) { g_bWorldSeed = b; }
void R3D_SetDrawHiddenWM(int b) { g_bDrawHiddenWM = b; }
void R3D_SetDrawUnmatchedWM(int b) { g_bDrawUnmatchedWM = b; }

namespace {
int UnmatchedKindOf(uint32_t pSub)
{
	std::map<uint32_t, int>::iterator it = g_UnmatchedKind.find(pSub);
	if (it != g_UnmatchedKind.end()) return it->second;
	int nKind = 2;
	char szN[64] = "";
	ModelName(pSub, szN, sizeof szN);
	if (g_pWorldFile)
	{
		std::map<std::string, std::string>::const_iterator c = g_pWorldFile->Classes.find(szN);
		// Not in the object block at all: nothing to say it was removed, so drawn.
		const char* pszWhy = "a game object with no object: removed, NOT drawn";
		if (c == g_pWorldFile->Classes.end())
		{ nKind = 1; pszWhy = "not in the object block, drawn"; }
		else if (IsVolumeClass(c->second))
		{
			// A VOLUME BRUSH IS DRAWN WHEN ITS OWN RECORD SAYS SO. The 9-12
			// September rule drew every volume class, and the 12 September
			// whole-game batch showed what that costs: three SafteyNets
			// covering the training level's walls in a tiled net, eighteen
			// Weather volumes as camouflage boxes across the snow level.
			// Retail never draws them - ShowSurface is FALSE on every one.
			// The level file stores the answer per object; ask it.
			std::map<std::string, int>::const_iterator ss =
				g_pWorldFile->ShowSurface.find(szN);
			bool bShow;
			if (!g_bVolumeSurface) bShow = true;
			else if (ss != g_pWorldFile->ShowSurface.end()) bShow = (ss->second != 0);
			else bShow = IsShownVolumeClass(c->second);
			// ADDITIVE ONLY: a liquid the level marks Visible is drawn even when
			// ShowSurface is 0 - no PolyGrid exists, so the brush IS the water.
			// 24 such bodies rendered as nothing (M04S02 canals, M06S02 beer,
			// M01S03 fountain). Nothing the old rule drew is hidden by this.
			if (!bShow && g_bVolumeSurface)
			{
				std::map<std::string, int>::const_iterator vv = g_pWorldFile->Visible.find(szN);
				if (vv != g_pWorldFile->Visible.end() && vv->second != 0) bShow = true;
			}
			// ...AND A VOLUME WHOSE SURFACE A POLYGRID DRAWS IS NOT DRAWN AS A
			// BRUSH WHEN THE LEVEL MARKS IT INVISIBLE. ShowSurface 1 / Visible 0
			// is 69 liquids: retail draws the PolyGrid and hides the brush.
			// Ours drew the brush's walls and floor as well (only the flat top
			// was skipped), and far out those walls are thin bands at the
			// horizon: the M05S05 opening cutscene showed a tiled strip that
			// alternated frame to frame with a dark bar behind the sea, and
			// M06S03's sea-box wall was the strip that scrolled (22 September).
			// +StubVolumeVisible 0 draws them again.
			// 2 does it only for a volume a KEYFRAMER MOVES (all 103 levels: only
			// M05S05's rising flood, water2). Not the default: a black area seen
			// under the flood's waterline on 24 September was the WINDOW capture
			// tearing - the renderer's own frame dumps showed the floor in every mode.
			// 1 hides every such brush.
			bool bKeyframedVol = false;
			if (g_bVolumeVisible == 2)
			{
				std::string sLow(szN);
				for (size_t q = 0; q < sLow.size(); ++q) sLow[q] = (char)tolower((unsigned char)sLow[q]);
				bKeyframedVol = g_pWorldFile->KeyframedLower.count(sLow) != 0;
			}
			if (bShow && g_bVolumeSurface && (g_bVolumeVisible == 1 || bKeyframedVol) && ss != g_pWorldFile->ShowSurface.end() && ss->second != 0)
			{
				std::map<std::string, int>::const_iterator vv = g_pWorldFile->Visible.find(szN);
				if (vv != g_pWorldFile->Visible.end() && vv->second == 0) bShow = false;
			}
			if (bShow)
			{ nKind = 1; pszWhy = !g_bVolumeSurface
								   ? "a volume, drawn regardless (+StubVolumeSurface 0)"
								   : (ss != g_pWorldFile->ShowSurface.end())
								   ? "a volume with ShowSurface 1, drawn"
								   : "terrain or a volume, drawn"; }
			else
			{
				nKind = 2;
				pszWhy = (ss != g_pWorldFile->ShowSurface.end())
						 ? "a volume with ShowSurface 0: INVISIBLE, not drawn"
						 : "a volume class that hides its surface: INVISIBLE, not drawn";
				++g_nUnmInvisible;
				if (g_sUnmInvisible.size() < 600)
				{ g_sUnmInvisible += szN; g_sUnmInvisible += ' '; }
			}
		}
		if (nKind == 1) ++g_nUnmDrawn; else if (!IsVolumeClass(c == g_pWorldFile->Classes.end() ? std::string() : c->second)) ++g_nUnmRemoved;
		// The removed game objects (doors, switches) are many and dull and
		// used to fill the cap before a single volume was named; the volumes
		// and the terrain are the interesting ones, so they are always said.
		if (g_UnmatchedKind.size() < 64 || nKind == 1 || strstr(pszWhy, "INVISIBLE"))
		{
			// WITH ITS AUTHORED PLACE, so a volume can be stood in at the desk
			// (-At "x,y,z"): the underwater measurement needs a point inside
			// M06S03's Water1, and nothing else in the log said where it was.
			char szAt[64] = "";
			std::map<std::string, size_t>::const_iterator fi = g_FileByName.find(szN);
			if (fi != g_FileByName.end() && g_pWorldFile->Models[fi->second].bHaveTrans)
			{
				const float* t = g_pWorldFile->Models[fi->second].fTrans;
				sprintf_s(szAt, "  at (%.0f %.0f %.0f)", t[0], t[1], t[2]);
			}
			Log("  WORLD MODEL %s has no engine object: class %s -> %s%s", szN,
				(c != g_pWorldFile->Classes.end()) ? c->second.c_str() : "(not in the object block)",
				pszWhy, szAt);
		}
	}
	// A SECOND BRUSH WITH THE SAME BOUNDS AND POLYGON COUNT IS THE SAME
	// BRUSH. Morocco's Canopy_a and Canopy_b are byte-identical alternates
	// (bounds -592 256 -320 .. -432 320 0, same texture) that no engine
	// object claims; drawn both, they z-fight and the awning flashes as the
	// head moves (headset testing, 11 and 13 September). Either alone is the picture,
	// so the second to arrive is left out. Only for brushes this verdict is
	// about to draw; matched objects never come here.
	if (nKind == 1)
	{
		const uint32_t pVerts = Word(pSub, kVertArray), nVerts = Word(pSub, kVertCount);
		const uint32_t nPolys = Word(pSub, kPolyCount);
		if (nVerts && nVerts < 100000 && Readable(pVerts, (size_t)nVerts * 12))
		{
			const float* vv = (const float*)(uintptr_t)pVerts;
			float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
			for (uint32_t z = 0; z < nVerts; ++z)
				for (int k = 0; k < 3; ++k)
				{
					if (vv[z * 3 + k] < lo[k]) lo[k] = vv[z * 3 + k];
					if (vv[z * 3 + k] > hi[k]) hi[k] = vv[z * 3 + k];
				}
			char szKey[128];
			sprintf_s(szKey, "%.0f,%.0f,%.0f/%.0f,%.0f,%.0f/%u", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], nPolys);
			static std::map<std::string, uint32_t> s_First;
			static uint32_t s_nFirstLoad = 0xFFFFFFFFu;
			if (s_nFirstLoad != (uint32_t)g_nWorldLoads) { s_nFirstLoad = (uint32_t)g_nWorldLoads; s_First.clear(); }
			std::map<std::string, uint32_t>::const_iterator f = s_First.find(szKey);
			if (f == s_First.end()) s_First[szKey] = pSub;
			else if (f->second != pSub)
			{
				char szA[64] = "", szB[64] = "";
				ModelName(pSub, szA, sizeof szA); ModelName(f->second, szB, sizeof szB);
				Log("  COINCIDENT BRUSH: %s has the bounds and %u polygons of %s - drawn once,"
					" this one left out (they z-fight drawn together)", szA, nPolys, szB);
				nKind = 2;
			}
		}
	}
	g_UnmatchedKind[pSub] = nKind;
	return nKind;
}
}	// namespace
void R3D_SetWorldNudge(float f) { g_fWorldNudge = f; }
void R3D_SetProbeDump(int b) { g_bProbeDump = b; }
void R3D_SetSkipHidden(int b) { g_bSkipHidden = b; }
void R3D_SetDumpMin(int n)  { g_nDumpMin = n; }
void R3D_SetFlagHist(int b) { g_bFlagHist = b; }
void R3D_SetSkipFlags(uint32_t n) { g_nSkipFlags = n; }
void R3D_SetWorldFile(int b) { g_bWorldFile = b; }
void R3D_SetSurfProbe(int b) { g_bSurfProbe = b; }
void R3D_SetMarkerFlags(uint32_t n) { g_nMarkerFlags = n; }
void R3D_SetBridgeCheck(int b) { g_bBridgeCheck = b; }
void R3D_SetFileTex(int b) { g_bFileTex = b; }
void R3D_SetMarkerTex(int b) { g_bMarkerTex = b; }
void R3D_BridgeReport(R3D_LogFn pfnLog)
{
	if (!pfnLog || (!g_bBridgeCheck && !g_nMarkerFlags)) return;
	const long nTot = g_nBridgeAgree + g_nBridgeDisagree;
	// A disagreement is NOT a failure of the mechanism - it is the mechanism
	// working. The model that disagreed is refused the rule and draws as it
	// always did. The only alarming reading is nothing verifying at all.
	pfnLog("  BRIDGE CHECK: %ld of %ld polygons name the SAME texture from"
		" the file and from the heap%s  (%ld had no heap name to compare)",
		g_nBridgeAgree, nTot,
		g_nBridgeVerified ? "" : "   <- NOTHING VERIFIED",
		g_nBridgeNoName);
	pfnLog("     models: %ld earned the marker rule, %ld were REFUSED it "
		"(they draw exactly as before)", g_nBridgeVerified, g_nBridgeRejected);
}
void R3D_SurfProbeReport(R3D_LogFn pfnLog)
{
	if (!pfnLog || !g_nProbeModels) return;
	// THE DENOMINATOR IS THE POINT. "All aligned" means nothing without
	// knowing how many models were looked at, and this file has been wrong
	// twice before by reporting a numerator on its own.
	pfnLog("  SURF PROBE: %ld models with 2+ surfaces", g_nProbeModels);
	pfnLog("     %ld every pointer on a stride boundary", g_nProbeAligned);
	pfnLog("     %ld the span is exactly the surface count (a DENSE array)",
		g_nProbeDense);
	pfnLog("     %ld of %ld matched a file model by name; of those %ld have"
		" span == the file's surface count and %ld have equal polygon counts",
		g_nProbeNamed, g_nProbeModels, g_nProbeSpanEqFile, g_nProbePolyEq);
}

void R3D_FlagReport(R3D_LogFn pfnLog)
{
	if (!pfnLog || g_FlagHist.empty()) return;
	pfnLog("  R3D: %u distinct surface-flag values over the level"
		   " (mask 0x1FFFFFFF, the top 3 bits are the lightmap axis)",
		   (unsigned)g_FlagHist.size());
	for (size_t i = 0; i < g_FlagHist.size(); ++i)
	{
		const FlagRow& r = g_FlagHist[i];
		char szT[300]{};
		const int nShow = r.nNames < 4 ? r.nNames : 4;
		for (int q = 0; q < nShow; ++q)
		{
			if (q) strcat_s(szT, ", ");
			strcat_s(szT, r.szTex[q]);
		}
		if (r.nNames > 4)
		{
			char szMore[32];
			sprintf_s(szMore, " (+%d more)", r.nNames - 4);
			strcat_s(szT, szMore);
		}
		if (!r.nNames) strcpy_s(szT, "-- no named texture --");
		pfnLog("      flags %08X  %6d polys  marker %5d  translucent %5d  other %5d  %s%2d tex: %s",
			   r.nFlags, r.nPolys, r.nMarker, r.nTrans, r.nOther,
			   (r.nMarker && !r.nOther && !r.nTrans) ? "<= MARKER ONLY  " : "",
			   r.nNames, szT);
	}
}
void R3D_SetLMDump(const char* psz) { g_szLMDump = psz; }

void R3D_Stats(int* pnPolys, int* pnTris, long* pnDraws)
{
	if (pnPolys) *pnPolys = g_nPolys;
	if (pnTris)  *pnTris  = g_nTris;
	if (pnDraws) *pnDraws = g_nDraws;
}


void R3D_MissingReport(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	int nNowHave = 0, nPolysRecoverable = 0;
	for (const auto& e : g_MissingAtBuild)
		if (TextureFor(e.first) >= 0) { ++nNowHave; nPolysRecoverable += e.second; }

	pfnLog("   3D textures: %u held, %u at the last build, %ld builds."
		   " Of %u missing then, %d have arrived since (%d polygons could be"
		   " textured by rebuilding)",
		   (unsigned)g_Tex.size(), (unsigned)g_nBuiltWithTexReport, g_nBuilds,
		   (unsigned)g_MissingAtBuild.size(), nNowHave, nPolysRecoverable);
}


void R3D_SetDrawUntextured(int b) { g_bDrawUntextured = b; }
void R3D_SetSprAdd(int b) { g_bSprAdd = b; }

// Open the verify window. 90 frames is a second at 90 Hz - long enough to
// cover a folder change and the frames either side of it, short enough that
// the steady state is untouched.
extern void Stub_FolderChanged();
void R3D_TexturesMayHaveMoved(const char* pszWhy)
{
	g_nVerifyUntil = g_nFrames + 90;
	Stub_FolderChanged();
	Log("  TEXTURE VERIFY WINDOW opened (%s): every address re-checks its"
		" name for 90 frames", pszWhy ? pszWhy : "?");
}
void R3D_SetSprAddMod(int b) { g_bSprAddMod = b; }
void R3D_SetDrawUntexturedTrans(int b) { g_bDrawUntexturedTrans = b; }

void R3D_SetRevZ(int b) { g_bRevZ = b; }

void R3D_SetNearOverride(float f) { g_fNearOverride = f; }
void R3D_SetFarOverride(float f)  { g_fFarOverride  = f; }

uint32_t R3D_FloorTexture() { return g_pFloorTex; }

// Every cached texture, written out and named by its engine key.
//
// The floor finder answers "what is on the largest upward-facing polygon",
// which is a different question from "what is on the street the player is
// looking at" - it found a mossy ground, and the street is blue. Going the
// other way round is more direct: write them all out, find the blue one by
// eye, and its FILENAME is the engine pointer that owns it.
int R3D_TextureCount() { return (int)g_Tex.size(); }

// ONE PER CALL, and the caller is expected to call it once a frame.
//
// The first version walked all of them inside a single periodic report and
// killed the game every time, at the third texture, whichever texture that
// happened to be. Reading a texture back maps a staging copy, which waits for
// the GPU; three of those back to back inside one callback holds the render
// thread long enough to trip the driver's watchdog. A device reset is not an
// SEH exception, which is why the __try around the dump never saw it and the
// log simply stopped.
//
// Spread over frames it is the same total work and the driver never waits more
// than one read-back at a time. Returns true while there is more to do.
bool R3D_DumpNextTexture(const char* pszDir, R3D_LogFn pfnLog)
{
	while (g_nDumpNext < (int)g_Tex.size())
	{
		const size_t i = (size_t)g_nDumpNext++;
		if (!g_Tex[i].pSRV) continue;
		// Smallest edge worth reading back. Every texture skipped here is a
		// frame not spent stalling, but guessing the threshold costs a run
		// each time - 256 was chosen because "a street is not a small
		// texture", and the street turned out not to be in the 71 it dumped.
		if (g_Tex[i].fW < (float)g_nDumpMin || g_Tex[i].fH < (float)g_nDumpMin)
			continue;

		char szPath[MAX_PATH];
		sprintf_s(szPath, "%stex-%08X-%dx%d.bmp", pszDir, g_Tex[i].pTex,
				  (int)g_Tex[i].fW, (int)g_Tex[i].fH);
		const bool bOk = R3D_DumpTexture(g_Tex[i].pTex, szPath);
		if (pfnLog)
			pfnLog("    tex %u/%u  %08X  %dx%d  %s", (unsigned)i,
				   (unsigned)g_Tex.size(), g_Tex[i].pTex,
				   (int)g_Tex[i].fW, (int)g_Tex[i].fH,
				   bOk ? "written" : "could not be read back");
		return true;
	}
	return false;
}

// Write the pixels we are ACTUALLY drawing for one engine texture out as a
// BMP, by copying the D3D11 texture back through a staging resource.
//
// Every other way of asking "is the ground textured wrongly" has been indirect
// and three of them were misleading. This is the direct one: if the image is
// the stone street then the fault is in the mapping, and if it is blue with
// yellow shapes then the wrong texture is linked to that surface.
// The dump proper. Called through R3D_DumpTexture, which guards it - see
// there for why.
// Read one D3D11 texture back and write it as a 24-bit BMP. Shared by the
// texture dump and the back-buffer dump: the desk's screen capture goes black
// whenever the game window is occluded or the desktop is rearranged, and it
// did exactly that mid-session, so the picture has to come from the renderer.
static bool DumpTex2D(ID3D11Texture2D* pTex, const char* pszPath)
{
	if (!pTex || !g_pDev || !g_pCtx) return false;
	pTex->AddRef();

	D3D11_TEXTURE2D_DESC td{};
	pTex->GetDesc(&td);
	D3D11_TEXTURE2D_DESC sd = td;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	sd.MipLevels = 1;

	ID3D11Texture2D* pStage = nullptr;
	bool bOk = false;
	if (SUCCEEDED(g_pDev->CreateTexture2D(&sd, nullptr, &pStage)) && pStage)
	{
		g_pCtx->CopySubresourceRegion(pStage, 0, 0, 0, 0, pTex, 0, nullptr);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(g_pCtx->Map(pStage, 0, D3D11_MAP_READ, 0, &m)))
		{
			const int W = (int)td.Width, H = (int)td.Height;
			const int nRow = (W * 3 + 3) & ~3;
			BITMAPFILEHEADER fh{};
			fh.bfType = 0x4D42;
			fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
			fh.bfSize = fh.bfOffBits + nRow * H;
			BITMAPINFOHEADER ih{};
			ih.biSize = sizeof(ih); ih.biWidth = W; ih.biHeight = H;
			ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = nRow * H;
			FILE* f = nullptr;
			fopen_s(&f, pszPath, "wb");
			if (f)
			{
				fwrite(&fh, sizeof(fh), 1, f);
				fwrite(&ih, sizeof(ih), 1, f);

				// BY FORMAT. The first version read four bytes per pixel
				// whatever the texture was, so pointing it at a DXT1 surface
				// walked a quarter of the way into the next row's memory and
				// off the end of the last one - which is the access violation
				// at 2456x1328 that got this whole dump switched off by
				// default rather than fixed.
				const bool bBC = (td.Format == DXGI_FORMAT_BC1_UNORM
							   || td.Format == DXGI_FORMAT_BC1_UNORM_SRGB
							   || td.Format == DXGI_FORMAT_BC3_UNORM
							   || td.Format == DXGI_FORMAT_BC3_UNORM_SRGB);
				const bool bBC3 = (td.Format == DXGI_FORMAT_BC3_UNORM
								|| td.Format == DXGI_FORMAT_BC3_UNORM_SRGB);

				// Decode into one RGB image first; a block format has no rows
				// to walk in the file's order.
				unsigned char* pImg =
					(unsigned char*)calloc((size_t)W * H * 3, 1);
				if (pImg && bBC)
				{
					const int nBW = (W + 3) / 4, nBH = (H + 3) / 4;
					const int nBlockBytes = bBC3 ? 16 : 8;
					for (int by = 0; by < nBH; ++by)
					{
						const unsigned char* pRowB =
							(const unsigned char*)m.pData + (size_t)by * m.RowPitch;
						for (int bx = 0; bx < nBW; ++bx)
						{
							const unsigned char* pB =
								pRowB + (size_t)bx * nBlockBytes + (bBC3 ? 8 : 0);
							const unsigned short c0 =
								(unsigned short)(pB[0] | (pB[1] << 8));
							const unsigned short c1 =
								(unsigned short)(pB[2] | (pB[3] << 8));
							unsigned char r[4], g[4], b[4];
							r[0] = (unsigned char)(((c0 >> 11) & 31) * 255 / 31);
							g[0] = (unsigned char)(((c0 >> 5) & 63) * 255 / 63);
							b[0] = (unsigned char)((c0 & 31) * 255 / 31);
							r[1] = (unsigned char)(((c1 >> 11) & 31) * 255 / 31);
							g[1] = (unsigned char)(((c1 >> 5) & 63) * 255 / 63);
							b[1] = (unsigned char)((c1 & 31) * 255 / 31);
							// BC3's colour block is always four-colour; BC1
							// switches to the three-colour form when c0 <= c1.
							if (bBC3 || c0 > c1)
							{
								r[2] = (unsigned char)((2*r[0] + r[1]) / 3);
								g[2] = (unsigned char)((2*g[0] + g[1]) / 3);
								b[2] = (unsigned char)((2*b[0] + b[1]) / 3);
								r[3] = (unsigned char)((r[0] + 2*r[1]) / 3);
								g[3] = (unsigned char)((g[0] + 2*g[1]) / 3);
								b[3] = (unsigned char)((b[0] + 2*b[1]) / 3);
							}
							else
							{
								r[2] = (unsigned char)((r[0] + r[1]) / 2);
								g[2] = (unsigned char)((g[0] + g[1]) / 2);
								b[2] = (unsigned char)((b[0] + b[1]) / 2);
								r[3] = g[3] = b[3] = 0;
							}
							const unsigned int nIdx = (unsigned int)pB[4]
								| ((unsigned int)pB[5] << 8)
								| ((unsigned int)pB[6] << 16)
								| ((unsigned int)pB[7] << 24);
							for (int py = 0; py < 4; ++py)
							for (int px = 0; px < 4; ++px)
							{
								const int X = bx * 4 + px, Y = by * 4 + py;
								if (X >= W || Y >= H) continue;
								const int k = (nIdx >> (2 * (py * 4 + px))) & 3;
								unsigned char* d = pImg + ((size_t)Y * W + X) * 3;
								d[0] = b[k]; d[1] = g[k]; d[2] = r[k];
							}
						}
					}
				}
				else if (pImg)
				{
					for (int y = 0; y < H; ++y)
					{
						const unsigned char* pS =
							(const unsigned char*)m.pData + (size_t)y * m.RowPitch;
						for (int x = 0; x < W; ++x)
						{
							unsigned char* d = pImg + ((size_t)y * W + x) * 3;
							if (g_bDumpAlphaAsGrey) d[0] = d[1] = d[2] = pS[x*4+3];
							else { d[0] = pS[x*4+0]; d[1] = pS[x*4+1]; d[2] = pS[x*4+2]; }
						}
					}
				}

				unsigned char* pRow = (unsigned char*)calloc(nRow, 1);
				if (pImg && pRow)
				{
					for (int y = H - 1; y >= 0; --y)
					{
						memcpy(pRow, pImg + (size_t)y * W * 3, (size_t)W * 3);
						fwrite(pRow, nRow, 1, f);
					}
					bOk = true;
				}
				free(pRow);
				free(pImg);
				fclose(f);
			}
			g_pCtx->Unmap(pStage, 0);
		}
		pStage->Release();
	}
	pTex->Release();
	return bOk;
}

static bool DumpTextureInner(uint32_t pKey, const char* pszPath)
{
	const int n = TextureFor(pKey);
	if (n < 0 || !g_Tex[n].pSRV) return false;
	ID3D11Resource* pRes = nullptr;
	g_Tex[n].pSRV->GetResource(&pRes);
	if (!pRes) return false;
	ID3D11Texture2D* pTex = nullptr;
	pRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTex);
	pRes->Release();
	if (!pTex) return false;
	const bool b = DumpTex2D(pTex, pszPath);
	pTex->Release();
	return b;
}

// The frame we actually drew, straight off the back buffer. Same guard as the
// texture dump - one read-back per call, and never more than one in a frame.
void R3D_SetDumpAlphaAsGrey(int b) { g_bDumpAlphaAsGrey = b; }

bool R3D_DumpBackBuffer(ID3D11Texture2D* pBack, const char* pszPath)
{
	__try { return DumpTex2D(pBack, pszPath); }
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// GUARDED, because one texture in the level takes the process down.
//
// Walking all 461 cached textures died on the same entry every time - a
// 256x256 that another 256x256 two places earlier dumped perfectly - and took
// the game with it, so the walk never reached the texture it was run to find.
// The cache holds an SRV per engine pointer and the engine reuses those
// addresses, so an entry can outlive the texture it names; reading one back is
// the only operation here that touches the object rather than the pointer,
// which is why nothing else has ever noticed.
//
// This is the project rules's third rule applied to a diagnostic: degrade, do not
// crash. A texture that cannot be read back is reported and skipped, and the
// other 460 still answer the question.
bool R3D_DumpTexture(uint32_t pKey, const char* pszPath)
{
	__try
	{
		return DumpTextureInner(pKey, pszPath);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// ---------------------------------------------------------------------------
// The ray probe: what is drawn WHERE THE PLAYER IS LOOKING.
//
// A report of blue ground with yellow shapes is a statement about a place in the
// picture, and every instrument aimed at it so far answered a different
// question. The floor heuristic takes the largest upward-facing polygon in the
// LEVEL, which need not be the street the player is standing on; the texture
// dump writes out 71 images with no way to say which one the player is treading on.
//
// A ray from the camera hits the thing complained about. The report then names
// it from three independent directions - the WorldModel that owns it, the
// engine's surface flags, and the texture object with its own pixels written
// out beside them - so the answer does not rest on any one of them being
// interpreted correctly.
// ---------------------------------------------------------------------------

struct ProbeHit
{
	float    fT;				// distance along the ray
	float    fHit[3];
	float    fN[3];				// the polygon's normal, normalised
	uint32_t nModel, nPoly;
	uint32_t pSurf, pTex;
	float    fTW, fTH;
	int      nVerts;
	char     szModel[64];
};

// Moller-Trumbore, single sided off: a floor is hit from above and a ceiling
// from below and the probe must not care which way the winding runs.
static bool RayTri(const float* o, const float* d,
				   const float* a, const float* b, const float* c, float* pT)
{
	const float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
	const float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
	const float pv[3] = { d[1]*e2[2] - d[2]*e2[1],
						  d[2]*e2[0] - d[0]*e2[2],
						  d[0]*e2[1] - d[1]*e2[0] };
	const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
	if (det > -1e-6f && det < 1e-6f) return false;
	const float inv = 1.0f / det;
	const float tv[3] = { o[0]-a[0], o[1]-a[1], o[2]-a[2] };
	const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
	if (u < 0.0f || u > 1.0f) return false;
	const float qv[3] = { tv[1]*e1[2] - tv[2]*e1[1],
						  tv[2]*e1[0] - tv[0]*e1[2],
						  tv[0]*e1[1] - tv[1]*e1[0] };
	const float v = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
	if (v < 0.0f || u + v > 1.0f) return false;
	const float t = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
	if (t <= 0.001f) return false;
	*pT = t;
	return true;
}

// Any printable string a structure points at, within the first nWords dwords.
// A texture that names itself ends the argument about which texture it is; a
// texture that does not costs four lines of log.
static void LogStringsNear(uint32_t p, int nWords, const char* pszWhat,
						   R3D_LogFn pfnLog)
{
	if (!pfnLog || !Readable(p, 4)) return;
	for (int w = 0; w < nWords; ++w)
	{
		const uint32_t v = Word(p, w * 4);
		if (!Readable(v, 8)) continue;
		const char* q = (const char*)(uintptr_t)v;
		int c = 0;
		while (c < 63 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126) ++c;
		if (c < 5) continue;
		if (IsBadReadPtr(q + c, 1) || q[c] != 0) continue;	// must be terminated
		char sz[64];
		memcpy(sz, q, (size_t)c); sz[c] = 0;
		pfnLog("        %s +%02X -> \"%s\"", pszWhat, w * 4, sz);
	}
}

// Walk the world once, keeping the nearest polygon the ray meets. Same
// validation as the build loop, deliberately: a probe that accepts polygons
// the renderer rejects would name something that is not on the screen.
static bool ProbeWorld(const float* o, const float* d, ProbeHit* pOut)
{
	const uint32_t pWorld = g_pBuiltFrom;
	if (!pWorld) return false;
	const uint32_t pList = Word(pWorld, kWorldModels);
	const uint32_t nList = Word(pWorld, kWorldModelCount);
	if (!Readable(pList, 4) || !nList || nList > 65536) return false;

	bool bAny = false;
	pOut->fT = 1e30f;

	for (uint32_t i = 0; i < nList; ++i)
	{
		const uint32_t sub = Word(Word(pList, i * 4), kSubFromElement);
		if (!Readable(sub, 0xB0)) continue;
		if (SkippedModel(sub)) continue;		// the renderer's own rule
		const uint32_t pPolys = Word(sub, kPolyArray), nPolys = Word(sub, kPolyCount);
		const uint32_t pVerts = Word(sub, kVertArray), nVerts = Word(sub, kVertCount);
		if (!Readable(pPolys, 4) || !nPolys || !Readable(pVerts, 12) || !nVerts)
			continue;

		for (uint32_t j = 0; j < nPolys; ++j)
		{
			const uint32_t po = Word(pPolys, j * 4);
			if (!Readable(po, kPolyVertList)) continue;
			const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + kPolyVertCount);
			if (nv < 3 || nv > 256) continue;
			if (!Readable(po + kPolyVertList, nv * kPolyVertStride)) continue;

			float p[256][3];
			bool bOK = true;
			for (uint32_t t = 0; t < nv; ++t)
			{
				const uint32_t vp = Word(po + kPolyVertList, t * kPolyVertStride);
				if (vp < pVerts || vp >= pVerts + nVerts * 12 || ((vp - pVerts) % 12))
				{ bOK = false; break; }
				memcpy(p[t], (const void*)(uintptr_t)vp, 12);
			}
			if (!bOK) continue;

			// The same fan the builder emits.
			for (uint32_t t = 1; t + 1 < nv; ++t)
			{
				float ft;
				if (!RayTri(o, d, p[0], p[t], p[t+1], &ft)) continue;
				if (ft >= pOut->fT) continue;

				bAny = true;
				pOut->fT = ft;
				pOut->nModel = i; pOut->nPoly = j; pOut->nVerts = (int)nv;
				for (int k = 0; k < 3; ++k) pOut->fHit[k] = o[k] + d[k] * ft;

				const float ax = p[1][0]-p[0][0], ay = p[1][1]-p[0][1], az = p[1][2]-p[0][2];
				const float bx = p[2][0]-p[0][0], by = p[2][1]-p[0][1], bz = p[2][2]-p[0][2];
				float nx = ay*bz - az*by, ny = az*bx - ax*bz, nz = ax*by - ay*bx;
				const float len = sqrtf(nx*nx + ny*ny + nz*nz);
				if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
				pOut->fN[0] = nx; pOut->fN[1] = ny; pOut->fN[2] = nz;

				const uint32_t sf = Word(po, kPolySurface);
				pOut->pSurf = sf;
				pOut->pTex  = Readable(sf, kSurfaceTexture + 4)
							? Word(sf, kSurfaceTexture) : 0;
				pOut->fTW = pOut->fTH = 0.0f;
				const int ti = TextureFor(pOut->pTex);
				if (ti >= 0) { pOut->fTW = g_Tex[ti].fW; pOut->fTH = g_Tex[ti].fH; }
				else TexDims(pOut->pTex, &pOut->fTW, &pOut->fTH);

				// The owning WorldModel's inline name (docs/WORLD-NAMED.md:
				// sub+0 is a vtable, sub+4 an inline string).
				ModelName(sub, pOut->szModel, sizeof pOut->szModel);
			}
		}
	}
	return bAny;
}

// Every polygon the ray meets, nearest first - not just the closest one.
//
// A localised flicker asks one question the nearest-hit probe cannot answer:
// is there a SECOND surface at the same depth? That is what z-fighting IS, and
// against a nearest-hit probe the spot looks perfectly healthy, because one of
// the two fighting polygons is duly reported and the other is never mentioned.
//
// Collects, sorts, prints the nearest few, and says outright when two hits are
// close enough in depth to fight.
struct ProbeDepthHit
{
	float    fT;
	uint32_t nModel, nPoly;
	uint32_t pSurf, pTex;
	float    fN[3];
	char     szModel[64];
};

void R3D_ProbeRayDepth(const float* pO, const float* pD, const char* pszWhat,
					   R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	float d[3] = { pD[0], pD[1], pD[2] };
	const float l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
	if (l < 1e-6f) return;
	d[0] /= l; d[1] /= l; d[2] /= l;

	const uint32_t pWorld = g_pBuiltFrom;
	if (!pWorld) { pfnLog("  R3D DEPTH %s: no world", pszWhat); return; }
	const uint32_t pList = Word(pWorld, kWorldModels);
	const uint32_t nList = Word(pWorld, kWorldModelCount);
	if (!Readable(pList, 4) || !nList || nList > 65536) return;

	std::vector<ProbeDepthHit> hits;

	for (uint32_t i = 0; i < nList; ++i)
	{
		const uint32_t sub = Word(Word(pList, i * 4), kSubFromElement);
		if (!Readable(sub, 0xB0)) continue;
		if (SkippedModel(sub)) continue;		// the renderer's own rule
		const uint32_t pPolys = Word(sub, kPolyArray), nPolys = Word(sub, kPolyCount);
		const uint32_t pVerts = Word(sub, kVertArray), nVerts = Word(sub, kVertCount);
		if (!Readable(pPolys, 4) || !nPolys || !Readable(pVerts, 12) || !nVerts)
			continue;

		for (uint32_t j = 0; j < nPolys; ++j)
		{
			const uint32_t po = Word(pPolys, j * 4);
			if (!Readable(po, kPolyVertList)) continue;
			const uint32_t nv = *(const uint16_t*)(uintptr_t)(po + kPolyVertCount);
			if (nv < 3 || nv > 256) continue;
			if (!Readable(po + kPolyVertList, nv * kPolyVertStride)) continue;

			float p[256][3];
			bool bOK = true;
			for (uint32_t t = 0; t < nv; ++t)
			{
				const uint32_t vp = Word(po + kPolyVertList, t * kPolyVertStride);
				if (vp < pVerts || vp >= pVerts + nVerts * 12 || ((vp - pVerts) % 12))
				{ bOK = false; break; }
				memcpy(p[t], (const void*)(uintptr_t)vp, 12);
			}
			if (!bOK) continue;

			for (uint32_t t = 1; t + 1 < nv; ++t)
			{
				float ft;
				if (!RayTri(pO, d, p[0], p[t], p[t+1], &ft)) continue;

				ProbeDepthHit h{};
				h.fT = ft; h.nModel = i; h.nPoly = j;
				h.pSurf = Word(po, kPolySurface);
				h.pTex  = Readable(h.pSurf, kSurfaceTexture + 4)
						? Word(h.pSurf, kSurfaceTexture) : 0;

				const float ax = p[1][0]-p[0][0], ay = p[1][1]-p[0][1], az = p[1][2]-p[0][2];
				const float bx = p[2][0]-p[0][0], by = p[2][1]-p[0][1], bz = p[2][2]-p[0][2];
				float nx = ay*bz - az*by, ny = az*bx - ax*bz, nz = ax*by - ay*bx;
				const float ln = sqrtf(nx*nx + ny*ny + nz*nz);
				if (ln > 1e-6f) { nx /= ln; ny /= ln; nz /= ln; }
				h.fN[0] = nx; h.fN[1] = ny; h.fN[2] = nz;

				ModelName(sub, h.szModel, sizeof h.szModel);
				hits.push_back(h);
				break;			// one hit per polygon, not per fan triangle
			}
		}
	}

	if (hits.empty())
	{
		pfnLog("  R3D DEPTH %s: along (%.3f %.3f %.3f) - NOTHING HIT",
			   pszWhat, d[0], d[1], d[2]);
		return;
	}

	std::sort(hits.begin(), hits.end(),
			  [](const ProbeDepthHit& a, const ProbeDepthHit& b) { return a.fT < b.fT; });

	pfnLog("  R3D DEPTH %s: along (%.3f %.3f %.3f) - %d surfaces on this ray",
		   pszWhat, d[0], d[1], d[2], (int)hits.size());

	const size_t nShow = hits.size() < 6 ? hits.size() : 6;
	for (size_t k = 0; k < nShow; ++k)
	{
		const ProbeDepthHit& h = hits[k];
		const uint32_t fl = Readable(h.pSurf, kSurfaceFlags2 + 4)
						  ? Word(h.pSurf, kSurfaceFlags2) : 0;
		char szGap[80] = "";
		if (k > 0)
		{
			const float gap = h.fT - hits[k-1].fT;
			// Depth precision is relative, so judge the gap against the distance.
			// Anything under a thousandth of the range is a coin toss per pixel.
			const bool bFight = (gap < h.fT * 0.001f) || (gap < 0.25f);
			sprintf_s(szGap, "  gap %+.3f%s", gap,
					  bFight ? "   <- COPLANAR, THESE TWO FIGHT" : "");
		}
		pfnLog("      %u) t %8.2f  \"%s\" poly %u  normal (%+.2f %+.2f %+.2f)"
			   "  surf %08X flags %08X tex %08X%s",
			   (unsigned)k, h.fT, h.szModel, h.nPoly,
			   h.fN[0], h.fN[1], h.fN[2], h.pSurf, fl, h.pTex, szGap);
	}
}

// Aim a probe by angle within the left eye's real frustum, so a feature seen
// in a screenshot can be named by measuring where it is rather than by adding
// a grid point and hoping one lands on it.
void R3D_ProbeAt(float fAzDeg, float fElDeg, R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveCam) { pfnLog("  R3D DEPTH: no camera yet"); return; }

	float r[3], u[3], f[3];
	QuatBasis(g_fLastQuat, r, u, f);

	const float ax = fAzDeg * 0.01745329f;
	const float ay = fElDeg * 0.01745329f;
	const float cx = cosf(ax), sx = sinf(ax);
	const float cy = cosf(ay), sy = sinf(ay);
	const float d[3] = {
		(f[0]*cx + r[0]*sx) * cy + u[0]*sy,
		(f[1]*cx + r[1]*sx) * cy + u[1]*sy,
		(f[2]*cx + r[2]*sx) * cy + u[2]*sy };

	char szName[48];
	sprintf_s(szName, "az%+.1f el%+.1f", fAzDeg, fElDeg);
	R3D_ProbeRayDepth(g_fLastPos, d, szName, pfnLog);
}

void R3D_ProbeRay(const float* pO, const float* pD, const char* pszWhat,
				  const char* pszDumpDir, R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	float d[3] = { pD[0], pD[1], pD[2] };
	const float l = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
	if (l < 1e-6f) return;
	d[0] /= l; d[1] /= l; d[2] /= l;

	ProbeHit h{};
	if (!ProbeWorld(pO, d, &h))
	{
		pfnLog("  R3D PROBE %s: from (%.1f %.1f %.1f) along (%.2f %.2f %.2f)"
			   " - NOTHING HIT", pszWhat, pO[0], pO[1], pO[2], d[0], d[1], d[2]);
		return;
	}

	const uint32_t fl = Readable(h.pSurf, kSurfaceFlags2 + 4)
					  ? Word(h.pSurf, kSurfaceFlags2) : 0;
	pfnLog("  R3D PROBE %s: hit at %.1f units, point (%.1f %.1f %.1f),"
		   " normal (%.2f %.2f %.2f)",
		   pszWhat, h.fT, h.fHit[0], h.fHit[1], h.fHit[2],
		   h.fN[0], h.fN[1], h.fN[2]);
	pfnLog("        WorldModel [%u] \"%s\", polygon %u of it, %d vertices",
		   h.nModel, h.szModel, h.nPoly, h.nVerts);
	{
		const int tiP = TextureFor(h.pTex);
		char szMean[64];
		if (tiP >= 0)
			sprintf_s(szMean, ", mean RGB %3.0f %3.0f %3.0f",
				g_Tex[tiP].mr, g_Tex[tiP].mg, g_Tex[tiP].mb);
		else strcpy_s(szMean, "");
		char szLine[192];
		sprintf_s(szLine, "        surface %08X  flags %08X  texture %08X"
			" (%gx%g, built %s%s)", h.pSurf, fl, h.pTex, h.fTW, h.fTH,
			tiP >= 0 ? "yes" : "NO", szMean);
		pfnLog(szLine);
	}

	// Anything either object names itself. The engine keeps texture data in a
	// separate object at +8 and either could carry the filename.
	LogStringsNear(h.pTex, 32, "texture", pfnLog);
	LogStringsNear(Word(h.pTex, 8), 48, "texdata", pfnLog);
	LogStringsNear(h.pSurf, 18, "surface", pfnLog);

	// OPT-IN. The first run of this probe died here: reading a texture back
	// waits for the GPU inside the render thread, and the log ends mid-report
	// with the process alive and frozen - the same failure the whole-cache dump
	// hit. The probe's finding is the NAME, and that arrives without it.
	if (g_bProbeDump && pszDumpDir && *pszDumpDir && h.pTex && TextureFor(h.pTex) >= 0)
	{
		char szPath[MAX_PATH];
		sprintf_s(szPath, "%sprobe-%s-%08X.bmp", pszDumpDir, pszWhat, h.pTex);
		pfnLog("        pixels: %s",
			   R3D_DumpTexture(h.pTex, szPath) ? szPath : "COULD NOT BE READ BACK");
	}
}

// Three rays from the camera the world was last drawn from: at the player's feet, a
// few paces ahead, and straight along the gaze. The street complained
// about is under one of them whichever way the player happens to be facing, and the
// three together also say whether the fault is one surface or the whole floor.
void R3D_ProbeFromCamera(const char* pszDumpDir, R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	if (!g_bHaveCam) { pfnLog("  R3D PROBE: no camera yet"); return; }

	float r[3], u[3], f[3];
	QuatBasis(g_fLastQuat, r, u, f);
	pfnLog("  R3D PROBE: camera (%.1f %.1f %.1f) forward (%.2f %.2f %.2f)",
		   g_fLastPos[0], g_fLastPos[1], g_fLastPos[2], f[0], f[1], f[2]);

	const float dn[3] = { 0.0f, -1.0f, 0.0f };
	R3D_ProbeRay(g_fLastPos, dn, "down", pszDumpDir, pfnLog);

	// Forward and 30 degrees down: the piece of street a standing player is
	// looking at, not the one under their feet.
	const float da[3] = { f[0] * 0.866f - 0.0f, f[1] * 0.866f - 0.5f,
						  f[2] * 0.866f - 0.0f };
	R3D_ProbeRay(g_fLastPos, da, "ahead", pszDumpDir, pfnLog);

	R3D_ProbeRay(g_fLastPos, f, "gaze", pszDumpDir, pfnLog);

	// A GRID across the eye's own frustum, so a feature seen in a frame dump
	// can be named instead of guessed at.
	//
	// Three rays down the middle cannot answer "what is that slab in the upper
	// left", and five separate switch-flipping runs failed to: sky, no-pixel
	// textures, translucent models, lightmaps and UV magnification were each
	// eliminated without ever naming the geometry. This names it.
	//
	// The angles are the left eye's real frustum, L54 R40 U44 D55, so grid
	// column 0 is the left edge of the picture and column 4 the right.
	{
		static const float kDeg[5] = { -54.0f, -27.0f, 0.0f, 20.0f, 40.0f };
		static const float kUp[3]  = {  44.0f,  0.0f, -55.0f };
		static const char* const kRow[3] = { "top", "mid", "low" };
		for (int ry = 0; ry < 3; ++ry)
			for (int rx = 0; rx < 5; ++rx)
			{
				const float ax = kDeg[rx] * 0.01745329f;
				const float ay = kUp[ry]  * 0.01745329f;
				const float cx = cosf(ax), sx = sinf(ax);
				const float cy = cosf(ay), sy = sinf(ay);
				const float d[3] = {
					(f[0]*cx + r[0]*sx) * cy + u[0]*sy,
					(f[1]*cx + r[1]*sx) * cy + u[1]*sy,
					(f[2]*cx + r[2]*sx) * cy + u[2]*sy };
				char szName[24];
				sprintf_s(szName, "grid %s %+.0f", kRow[ry], kDeg[rx]);
				R3D_ProbeRay(g_fLastPos, d, szName, nullptr, pfnLog);
			}
	}
}

// ---------------------------------------------------------------------------
// Which WorldModels is the engine NOT drawing, and how does it know?
//
// The ray probe found the polygon under the player's feet in a WorldModel
// named "AIVolume174" - an editor volume the retail renderer never shows and
// we draw straight over the street. Its surface flags are 00001081, the value
// 24453 of the level's polygons carry, so the flags do not say it; something
// on the MODEL does.
//
// So this looks for that something rather than guessing it: take the names as
// ground truth, and search every dword of the WorldModel structure for a bit
// that is set for the models we should draw and clear for the volumes (or the
// other way round). A perfect separator over 565 models is not a coincidence,
// and if there is none this says so instead of offering the best near miss as
// though it were an answer.
// ---------------------------------------------------------------------------
void R3D_ModelReport(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	const uint32_t pWorld = g_pBuiltFrom;
	if (!pWorld) { pfnLog("  R3D MODELS: no world built yet"); return; }
	const uint32_t pList = Word(pWorld, kWorldModels);
	const uint32_t nList = Word(pWorld, kWorldModelCount);
	if (!Readable(pList, 4) || !nList || nList > 65536) return;

	const uint32_t kScan = 0x200;			// bytes of the WorldModel searched

	struct Row { uint32_t sub, elem; int nPolys; bool bVolume; char szName[64]; };
	std::vector<Row> rows;
	// Name prefixes, so "how much of the level is volumes" is one line rather
	// than 565.
	struct Pre { char sz[64]; int nModels, nPolys; };
	std::vector<Pre> pre;

	for (uint32_t i = 0; i < nList; ++i)
	{
		const uint32_t elem = Word(pList, i * 4);
		const uint32_t sub  = Word(elem, kSubFromElement);
		if (!Readable(sub, 0xB0)) continue;
		Row r{}; r.sub = sub; r.elem = elem;
		r.nPolys = (int)Word(sub, kPolyCount);
		const char* q = (const char*)(uintptr_t)(sub + kNameInSub);
		int c = 0;
		while (c < 63 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126)
		{ r.szName[c] = q[c]; ++c; }
		r.szName[c] = 0;
		// The ground truth: an AI volume is one by name. Nothing else here
		// uses the name, and the point of the search is to stop needing it.
		r.bVolume = (strncmp(r.szName, "AIVolume", 8) == 0);
		rows.push_back(r);

		char szPre[64]; int k = 0;
		while (k < 63 && r.szName[k] && !(r.szName[k] >= '0' && r.szName[k] <= '9'))
		{ szPre[k] = r.szName[k]; ++k; }
		szPre[k] = 0;
		size_t m = 0;
		for (; m < pre.size(); ++m) if (strcmp(pre[m].sz, szPre) == 0) break;
		if (m == pre.size()) { Pre p{}; strcpy_s(p.sz, szPre); pre.push_back(p); }
		++pre[m].nModels; pre[m].nPolys += r.nPolys;
	}

	int nVol = 0, nVolPolys = 0, nAllPolys = 0;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		if (rows[i].bVolume) { ++nVol; nVolPolys += rows[i].nPolys; }
		nAllPolys += rows[i].nPolys;
	}

	pfnLog("  R3D MODELS: %u WorldModels, %d polygons; %d of them are named"
		   " AIVolume*, carrying %d polygons (%.1f%% of the level)",
		   (unsigned)rows.size(), nAllPolys, nVol, nVolPolys,
		   nAllPolys ? 100.0 * nVolPolys / nAllPolys : 0.0);

	std::sort(pre.begin(), pre.end(),
			  [](const Pre& a, const Pre& b) { return a.nPolys > b.nPolys; });
	pfnLog("      name prefixes, by polygons:");
	for (size_t i = 0; i < pre.size() && i < 14; ++i)
		pfnLog("        %-20s %4d models  %6d polygons",
			   pre[i].sz, pre[i].nModels, pre[i].nPolys);

	if (!nVol || (int)rows.size() == nVol)
	{ pfnLog("      no separation to look for"); return; }

	// The search, over the WorldModel and over the array element that points
	// at it - the flag could live on either.
	//
	// THE NAME IS EXCLUDED, and the first run of this is why. It reported two
	// PERFECT separators, +004 bit 13 and +004 bit 21, over all 565 models -
	// and +0x04 is the inline name string. Bit 13 of "AIVo" against bit 13 of
	// "Tran" is a fact about spelling, not about rendering. A search whose
	// ground truth is the name will always find the name; the region it lives
	// in has to be taken off the table before the result means anything.
	const uint32_t kNameLo = 0x04, kNameHi = 0x30;

	pfnLog("      searching the WorldModel and its array element for a bit"
		   " that separates them (the name field, +%02X..+%02X, excluded):",
		   kNameLo, kNameHi);
	int nFound = 0;
	struct Near { int nMiss; uint32_t off; int bit, nPol, bElem; };
	Near best[5];
	for (int k = 0; k < 5; ++k) best[k] = Near{ 1 << 30, 0, 0, 0, 0 };

	for (int bElem = 0; bElem < 2; ++bElem)
	{
		const uint32_t nRange = bElem ? 0x80 : kScan;
		for (uint32_t off = 0; off < nRange; off += 4)
		{
			if (!bElem && off >= kNameLo && off < kNameHi) continue;
			for (int bit = 0; bit < 32; ++bit)
			{
				int nVolSet = 0, nOtherSet = 0;
				for (size_t i = 0; i < rows.size(); ++i)
				{
					const uint32_t base = bElem ? rows[i].elem : rows[i].sub;
					if (!Readable(base + off, 4)) continue;
					if (!((Word(base, off) >> bit) & 1)) continue;
					if (rows[i].bVolume) ++nVolSet; else ++nOtherSet;
				}
				const int nMissDraw = nVolSet + (((int)rows.size() - nVol) - nOtherSet);
				const int nMissHide = (nVol - nVolSet) + nOtherSet;
				const int nMiss = nMissDraw < nMissHide ? nMissDraw : nMissHide;
				const int nPol  = nMissDraw < nMissHide ? 1 : 0;
				if (nMiss == 0 && nFound < 12)
				{
					++nFound;
					pfnLog("        PERFECT: %s +%03X bit %2d - set means %s"
						   " (%d volumes, %d others)",
						   bElem ? "element" : "model", off, bit,
						   nPol ? "DRAW" : "do not draw", nVol,
						   (int)rows.size() - nVol);
				}
				for (int k = 0; k < 5; ++k)
					if (nMiss < best[k].nMiss)
					{
						for (int m = 4; m > k; --m) best[m] = best[m-1];
						best[k] = Near{ nMiss, off, bit, nPol, bElem };
						break;
					}
			}
		}
	}
	if (!nFound)
	{
		pfnLog("        none separates them. The five closest, so the near"
			   " misses can be judged rather than guessed at:");
		for (int k = 0; k < 5; ++k)
			pfnLog("          %s +%03X bit %2d, set means %-12s wrong about"
				   " %d of %u models", best[k].bElem ? "element" : "model  ",
				   best[k].off, best[k].bit,
				   best[k].nPol ? "DRAW" : "do not draw",
				   best[k].nMiss, (unsigned)rows.size());
	}

	// A SECOND search, for a different shape of answer. The bit search can
	// only find a flag; if what marks a volume is that some pointer on it is
	// null - no client object attached, nothing bound to it, no lightmap - then
	// no single bit says so and the search above returns a shrug.
	//
	// So: which offsets are zero for one group and non-zero for the other.
	pfnLog("      and for a field that is ZERO for one group and set for the"
		   " other:");
	int nZeroFound = 0;
	for (int bElem = 0; bElem < 2 && nZeroFound < 12; ++bElem)
	{
		const uint32_t nRange = bElem ? 0x80 : kScan;
		for (uint32_t off = 0; off < nRange; off += 4)
		{
			if (!bElem && off >= kNameLo && off < kNameHi) continue;
			int nVolZero = 0, nOtherZero = 0;
			for (size_t i = 0; i < rows.size(); ++i)
			{
				const uint32_t base = bElem ? rows[i].elem : rows[i].sub;
				if (!Readable(base + off, 4)) continue;
				if (Word(base, off)) continue;
				if (rows[i].bVolume) ++nVolZero; else ++nOtherZero;
			}
			const int nOthers = (int)rows.size() - nVol;
			if ((nVolZero == nVol && nOtherZero == 0)
			 || (nVolZero == 0 && nOtherZero == nOthers))
			{
				if (++nZeroFound > 12) break;
				pfnLog("        PERFECT: %s +%03X is zero for %s, set for the"
					   " other %d", bElem ? "element" : "model", off,
					   nVolZero ? "all 322 volumes" : "everything else",
					   nVolZero ? nOthers : nVol);
			}
		}
	}
	if (!nZeroFound) pfnLog("        no offset is zero for exactly one group.");

	// The array element itself, which the searches treat as bytes and a human
	// can read as structure. If a volume differs from a door by having nothing
	// bound to it, this is where that shows.
	pfnLog("      the array element, dwords +00..+3C:");
	{
		int nShownV2 = 0, nShownO2 = 0;
		for (size_t i = 0; i < rows.size(); ++i)
		{
			const bool bWant = rows[i].bVolume ? (nShownV2 < 2) : (nShownO2 < 4);
			if (!bWant) continue;
			if (rows[i].bVolume) ++nShownV2; else ++nShownO2;
			char szLine[512]; int n = 0;
			for (uint32_t w = 0; w < 0x40; w += 4)
				n += sprintf_s(szLine + n, sizeof(szLine) - n, "%08X ",
							   Word(rows[i].elem, w));
			pfnLog("        %-24s %s", rows[i].szName, szLine);
		}
	}

	// One model of each named kind, side by side. The search can only find a
	// single bit; a human reading eight rows finds a field that counts, or one
	// that holds a small enumeration, which is what a "type" usually is.
	pfnLog("      one of each kind, dwords +30..+8C (past the name):");
	for (size_t i = 0; i < pre.size() && i < 10; ++i)
	{
		for (size_t r = 0; r < rows.size(); ++r)
		{
			if (strncmp(rows[r].szName, pre[i].sz, strlen(pre[i].sz)) != 0) continue;
			char szLine[512]; int n = 0;
			for (uint32_t w = 0x30; w < 0x90; w += 4)
				n += sprintf_s(szLine + n, sizeof(szLine) - n, "%08X ",
							   Word(rows[r].sub, w));
			pfnLog("        %-24s %s", rows[r].szName, szLine);
			break;
		}
	}

	// The volumes and a few others side by side, so a human can see what the
	// search saw. A dump of one structure means nothing without another.
	pfnLog("      three volumes and three others, first 24 dwords of each:");
	int nShownV = 0, nShownO = 0;
	for (size_t i = 0; i < rows.size(); ++i)
	{
		const Row& r = rows[i];
		const bool bWant = r.bVolume ? (nShownV < 3) : (nShownO < 3);
		if (!bWant) continue;
		if (r.bVolume) ++nShownV; else ++nShownO;
		char szLine[512]; int n = 0;
		for (int w = 0; w < 24; ++w)
			n += sprintf_s(szLine + n, sizeof(szLine) - n, "%08X ",
						   Word(r.sub, w * 4));
		pfnLog("        %-18s %s", r.szName, szLine);
	}
}

// ---------------------------------------------------------------------------
// The same search, one level down: the POLYGON and its SURFACE.
//
// The model header carries no bit that tells an AI volume from a door - two
// searches over 565 models found none, and that is a result rather than a
// failure. But the retail renderer skips these brushes somehow, and it walks
// the same data we do, so the mark is somewhere it reads. The polygon record
// and the 72-byte surface record are the two places left.
//
// Ground truth is cleaner here than it was for the models: every polygon in a
// model named AIVolume* is one the engine does not draw, and every polygon in
// VisBSP is one it does. 2008 against 5957, with no third case to blur it.
// ---------------------------------------------------------------------------
void R3D_SurfaceSearch(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;
	const uint32_t pWorld = g_pBuiltFrom;
	if (!pWorld) return;
	const uint32_t pList = Word(pWorld, kWorldModels);
	const uint32_t nList = Word(pWorld, kWorldModelCount);
	if (!Readable(pList, 4) || !nList || nList > 65536) return;

	// {polygon, surface, is it one the engine draws}
	struct PRow { uint32_t po, sf; bool bDrawn; };
	std::vector<PRow> polys;

	for (uint32_t i = 0; i < nList; ++i)
	{
		const uint32_t sub = Word(Word(pList, i * 4), kSubFromElement);
		if (!Readable(sub, 0xB0)) continue;
		char szName[64]; int c = 0;
		const char* q = (const char*)(uintptr_t)(sub + kNameInSub);
		while (c < 63 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126)
		{ szName[c] = q[c]; ++c; }
		szName[c] = 0;
		const bool bVol = (strncmp(szName, "AIVolume", 8) == 0);
		const bool bVis = (strcmp(szName, "VisBSP") == 0);
		if (!bVol && !bVis) continue;			// only the two clean cases

		const uint32_t pPolys = Word(sub, kPolyArray), nPolys = Word(sub, kPolyCount);
		if (!Readable(pPolys, 4) || !nPolys) continue;
		for (uint32_t j = 0; j < nPolys; ++j)
		{
			const uint32_t po = Word(pPolys, j * 4);
			if (!Readable(po, kPolyVertList)) continue;
			const uint32_t sf = Word(po, kPolySurface);
			polys.push_back(PRow{ po, sf, bVis });
		}
	}

	int nDrawn = 0;
	for (size_t i = 0; i < polys.size(); ++i) if (polys[i].bDrawn) ++nDrawn;
	const int nHidden = (int)polys.size() - nDrawn;
	pfnLog("  R3D SURFACES: %d polygons in VisBSP (drawn) against %d in the"
		   " AI volumes (not drawn); searching the polygon record and the"
		   " surface record for a bit that tells them apart:",
		   nDrawn, nHidden);
	if (!nDrawn || !nHidden) return;

	int nFound = 0;
	struct Near { int nMiss; uint32_t off; int bit, nPol, bSurf; };
	Near best[6];
	for (int k = 0; k < 6; ++k) best[k] = Near{ 1 << 30, 0, 0, 0, 0 };

	for (int bSurf = 0; bSurf < 2; ++bSurf)
	{
		const uint32_t nRange = bSurf ? 0x48 : 0x54;
		for (uint32_t off = 0; off < nRange; off += 4)
		{
			for (int bit = 0; bit < 32; ++bit)
			{
				int nDrawnSet = 0, nHiddenSet = 0;
				for (size_t i = 0; i < polys.size(); ++i)
				{
					const uint32_t base = bSurf ? polys[i].sf : polys[i].po;
					if (!Readable(base + off, 4)) continue;
					if (!((Word(base, off) >> bit) & 1)) continue;
					if (polys[i].bDrawn) ++nDrawnSet; else ++nHiddenSet;
				}
				const int nMissDraw = nHiddenSet + (nDrawn - nDrawnSet);
				const int nMissHide = (nHidden - nHiddenSet) + nDrawnSet;
				const int nMiss = nMissDraw < nMissHide ? nMissDraw : nMissHide;
				const int nPol  = nMissDraw < nMissHide ? 1 : 0;
				if (nMiss == 0 && nFound < 12)
				{
					++nFound;
					pfnLog("        PERFECT: %s +%02X bit %2d - set means %s",
						   bSurf ? "surface" : "polygon", off, bit,
						   nPol ? "DRAW" : "do not draw");
				}
				for (int k = 0; k < 6; ++k)
					if (nMiss < best[k].nMiss)
					{
						for (int m = 5; m > k; --m) best[m] = best[m-1];
						best[k] = Near{ nMiss, off, bit, nPol, bSurf };
						break;
					}
			}
		}
	}
	if (!nFound)
	{
		pfnLog("        none. The six closest:");
		for (int k = 0; k < 6; ++k)
			pfnLog("          %s +%02X bit %2d, set means %-12s wrong about"
				   " %d of %u polygons", best[k].bSurf ? "surface" : "polygon",
				   best[k].off, best[k].bit,
				   best[k].nPol ? "DRAW" : "do not draw",
				   best[k].nMiss, (unsigned)polys.size());
	}

	// THE CONTROL, and it decides whether any of the above is usable.
	//
	// The separators the search just reported are mostly artefacts: polygon
	// +0C and +28 are POINTERS, and the two groups' records were allocated in
	// different heap regions, so "bit 24 of +28" separates addresses, not
	// meanings - the same trap as bit 13 of the name, one level down. What is
	// left after discarding those is that +0C, +10 and +14 are ZERO on every
	// VisBSP polygon and set on the volumes'.
	//
	// Which could mean "do not draw" - or could simply mean "not part of the
	// render tree", which is equally true of a DOOR. Those need opposite
	// treatments, and one sample of each cannot tell them apart. So: the same
	// three fields, tallied over every kind of model in the level. If the
	// doors and the translucent brushes look like the volumes, this field is
	// not the answer and using it would hide half the level's moving parts.
	pfnLog("      the control - those three fields over EVERY kind of model,"
		   " not just the two the search used:");
	pfnLog("        %-24s %7s %10s %10s %10s", "kind", "polys",
		   "+0C set", "+10 bit0", "+14 bit15");
	{
		struct KRow { char sz[64]; int nPolys, nC, n10, n14; };
		std::vector<KRow> kinds;
		for (uint32_t i = 0; i < nList; ++i)
		{
			const uint32_t sub = Word(Word(pList, i * 4), kSubFromElement);
			if (!Readable(sub, 0xB0)) continue;
			char szName[64]; int c = 0;
			const char* q = (const char*)(uintptr_t)(sub + kNameInSub);
			while (c < 63 && !IsBadReadPtr(q + c, 1) && q[c] >= 32 && q[c] <= 126)
			{ szName[c] = q[c]; ++c; }
			szName[c] = 0;
			char szPre[64]; int k = 0;
			while (k < 63 && szName[k] && !(szName[k] >= '0' && szName[k] <= '9'))
			{ szPre[k] = szName[k]; ++k; }
			szPre[k] = 0;
			size_t m = 0;
			for (; m < kinds.size(); ++m) if (strcmp(kinds[m].sz, szPre) == 0) break;
			if (m == kinds.size()) { KRow r{}; strcpy_s(r.sz, szPre); kinds.push_back(r); }

			const uint32_t pPolys = Word(sub, kPolyArray), nPolys = Word(sub, kPolyCount);
			if (!Readable(pPolys, 4) || !nPolys) continue;
			for (uint32_t j = 0; j < nPolys; ++j)
			{
				const uint32_t po = Word(pPolys, j * 4);
				if (!Readable(po, kPolyVertList)) continue;
				++kinds[m].nPolys;
				if (Word(po, 0x0C))              ++kinds[m].nC;
				if (Word(po, 0x10) & 1)          ++kinds[m].n10;
				if (Word(po, 0x14) & 0x8000)     ++kinds[m].n14;
			}
		}
		std::sort(kinds.begin(), kinds.end(),
				  [](const KRow& a, const KRow& b) { return a.nPolys > b.nPolys; });
		for (size_t i = 0; i < kinds.size() && i < 14; ++i)
			pfnLog("        %-24s %7d %10d %10d %10d", kinds[i].sz,
				   kinds[i].nPolys, kinds[i].nC, kinds[i].n10, kinds[i].n14);
	}

	// And the records themselves, one of each, because the searches see bits
	// and a reader sees fields.
	for (int bDrawn = 0; bDrawn < 2; ++bDrawn)
		for (size_t i = 0; i < polys.size(); ++i)
		{
			if (polys[i].bDrawn != (bDrawn != 0)) continue;
			char szP[512]; int n = 0;
			for (uint32_t w = 0; w < 0x54; w += 4)
				n += sprintf_s(szP + n, sizeof(szP) - n, "%08X ", Word(polys[i].po, w));
			pfnLog("        %s polygon %08X: %s",
				   bDrawn ? "drawn " : "hidden", polys[i].po, szP);
			n = 0;
			for (uint32_t w = 0; w < 0x48; w += 4)
				n += sprintf_s(szP + n, sizeof(szP) - n, "%08X ", Word(polys[i].sf, w));
			pfnLog("        %s surface %08X: %s",
				   bDrawn ? "drawn " : "hidden", polys[i].sf, szP);
			break;
		}
}

void R3D_StaleReport(R3D_LogFn pfnLog)
{
	if (!pfnLog) return;

	// How many of the textures we are drawing have had their engine object
	// reset under us since we cached them?
	//
	// The cache is keyed on the engine's texture-object ADDRESS, and the engine
	// reuses addresses. The existing recycling check compares the data pointer
	// on every BindTexture - but it only runs when the engine binds that key
	// again, and for a texture it has stopped binding it never runs at all.
	// The ground is one of those: its object's data pointer at +0x14 now points
	// back into itself, an empty list, while our cached copy came from memory
	// that has since been freed and reused.
	// The test is the STRUCTURAL one, not an equality.
	//
	// A first attempt compared the cached pData against the object's +0x14 and
	// reported 461 of 461 stale - which cannot be true, because most of the
	// level draws correctly. Those two were never the same quantity: pData is
	// BindTexture's argument, +0x14 is the object's own data-list head.
	//
	// What IS meaningful is the signature found for the sky
	// (docs/UNTEXTURED-IS-SKY.md): an object whose +0x14 points back into
	// itself at +0x44 is an EMPTY list, so that object has no pixel data now.
	int nEmpty = 0, nHasData = 0;
	for (size_t i = 0; i < g_Tex.size(); ++i)
	{
		const uint32_t cur = Word(g_Tex[i].pTex, 0x14);
		if (cur == g_Tex[i].pTex + 0x44) ++nEmpty; else ++nHasData;
	}
	pfnLog("   3D textures: %d whose engine object still holds data, %d whose"
		   " data list is now EMPTY (we draw those from a cached copy)",
		   nHasData, nEmpty);
	pfnLog("   3D textures: %ld recycled addresses answered from the file or refused"
		   " instead of from the previous owner's picture", g_nRecycledRefused);
}

void R3D_SetModelScale(int b) { g_bModelScale = b; }

void R3D_SetSprSort(int b) { g_bSprSort = b; }

void R3D_SetAddBlend(int b) { g_bAddBlend = b; }
void R3D_SetMulBlend(int b) { g_bMulBlend = b; }
void R3D_SetModelGlass(int b) { g_bModelGlass = b; }
void R3D_SetVolumeSurface(int b) { g_bVolumeSurface = b; }
void R3D_SetVertexColour(int b) { g_bVertexColour = b; }
void R3D_SetMulForce(int b) { g_bMulForce = b; }
void R3D_SetCrashTest(int b) { g_bCrashTest = b; }
void R3D_SetSkipStill(int b) { g_bSkipStill = b; }
void R3D_SetSkinCache(int b) { g_bSkinCache = b; FlushSkinCache(); }
void R3D_SetMeshPool(int b) { g_bMeshPool = b; FlushSkinCache(); }
void R3D_SetSkinDiag(int b) { g_bSkinDiag = b; }
void R3D_SetMemKindCache(int b) { MemKindCacheSet(b); }
void R3D_SetModelLight(int b) { g_bModelLight = b; }
void R3D_SetTerrainGrid(int b) { g_bTerrainGrid = b; }
void R3D_SetSkyOccluder(int b) { g_bSkyOccluder = b; }
void R3D_SetLightScaleAllow(int b) { g_bLightScaleAllow = b; }
void R3D_SetLightScaleTest(int n) { g_fLightScaleTest = (n > 0) ? (float)n / 100.0f : 0.0f; }

// The skins an object was CREATED with, from the client's CreateObject. See
// g_PubSkins. Bounded: a map that only grows would carry every object the
// game ever made, so past 4096 entries it starts over - the worst case is
// one menu drawn from the heap for a frame.
extern "C" void __cdecl R3D_PublishObjectSkins(uint32_t hObj, const char* szModel,
												const char* szSkin0, const char* szSkin1)
{
	RenderGuard _renderGuard;
	if (!hObj || !szModel || !szModel[0]) return;
	if (g_PubSkins.size() >= 4096) g_PubSkins.clear();
	static long s_nRecv = 0;
	if (++s_nRecv <= 8)
		Log("  R3D PUB SKIN: received object %08X %s -> %s%s%s", hObj, szModel,
			szSkin0 ? szSkin0 : "", (szSkin1 && szSkin1[0]) ? " + " : "", (szSkin1 && szSkin1[0]) ? szSkin1 : "");
	PubSkin& p = g_PubSkins[hObj];
	p.nFrame = g_nFrames;
	strncpy(p.szModel, szModel, sizeof p.szModel - 1); p.szModel[sizeof p.szModel - 1] = 0;
	strncpy(p.szSkin[0], szSkin0 ? szSkin0 : "", sizeof p.szSkin[0] - 1); p.szSkin[0][sizeof p.szSkin[0] - 1] = 0;
	strncpy(p.szSkin[1], szSkin1 ? szSkin1 : "", sizeof p.szSkin[1] - 1); p.szSkin[1][sizeof p.szSkin[1] - 1] = 0;
}
extern "C" void __cdecl R3D_ForgetObject(uint32_t hObj)
{
	RenderGuard _renderGuard;
	g_PubSkins.erase(hObj);
}
void R3D_SetPubSkins(int b) { g_bPubSkins = b; }

// The engine's global light scale, from the client, every frame. A repeat
// costs a compare; a change marks the constant buffer for the next pass.
extern "C" void __cdecl R3D_PublishLightScale(float r, float g, float b)
{
	RenderGuard _renderGuard;
	// +StubLightScale100 N forces N/100 on every channel: the positive
	// control, because a level with a scale of 1,1,1 cannot show the
	// multiply working.
	if (g_fLightScaleTest > 0.0f) r = g = b = g_fLightScaleTest;
	if (r == g_fLightScale[0] && g == g_fLightScale[1] && b == g_fLightScale[2]) return;
	g_fLightScale[0] = r; g_fLightScale[1] = g; g_fLightScale[2] = b;
	const float fUse = g_bLightScaleAllow ? 1.0f : 0.0f;
	g_fGridCB[8]  = fUse * r + (1.0f - fUse);
	g_fGridCB[9]  = fUse * g + (1.0f - fUse);
	g_fGridCB[10] = fUse * b + (1.0f - fUse);
	g_fGridCB[11] = 0.0f;
	g_bGridDirty = 1;
	// The interface fade animates this every frame; the first few and then
	// one in fifty is enough to see it move.
	if (g_nLightScaleSaid < 8 || (g_nLightScaleSaid % 50) == 0)
		Log("  R3D LIGHT SCALE: %.3f %.3f %.3f%s", r, g, b,
			g_bLightScaleAllow ? "" : "  (ignored: +StubLightScale 0)");
	++g_nLightScaleSaid;
}
void R3D_SetLightObjects(int b) { g_bLightObjects = b; }
void R3D_SetLightDirect(int b) { g_bLightDirect = b; }
void R3D_SetLightGain(float f) { if (f > 0.0f) g_fLightGain = f; }
void R3D_SetAddLast(int b) { g_bAddLast = b; }
void R3D_SetMeshRing(int n) { g_nMeshRing = (n > 0) ? n : 4; }

// ---------------------------------------------------------------------------
// R3D_RayCast - the aim ray against the level's own polygons.
//
// pFrom/pTo: the segment, world units, live space. On a hit: pOut[0..2] the
// point in live space, pOut[3] the fraction along the segment. Sky batches
// are never hit; a world model the engine has hidden, or that the classifier
// left out as removed, is not there; a MOVED world model is tested in its
// authored frame and the point carried back. Returns 1 on a hit.
// ---------------------------------------------------------------------------
extern "C" int __cdecl R3D_RayCast(const float* pFrom, const float* pTo, float* pOut)
{
	RenderGuard _renderGuard;
	if (!pFrom || !pTo || !pOut || g_WorldVertsCpu.empty() || g_BatchBox.size() != g_Batches.size())
		return 0;
	float fBest = 1.0f; bool bHit = false; float fBestPt[3] = { 0, 0, 0 };
	static unsigned s_nCalls = 0; unsigned nBoxPass = 0, nTris = 0;
	const bool bTell = ((s_nCalls++ % 150) == 0);
	for (size_t b = 0; b < g_Batches.size(); ++b)
	{
		const Batch& bt = g_Batches[b];
		if (bt.bSky || !bt.nCount) continue;
		const WMBase* pA = bt.pSub ? WMBaseFor(bt.pSub) : nullptr;
		if (pA && !g_bDrawHiddenWM && (pA->nFlags & VRWORLD_F_INVIS)) continue;
		if (!pA && bt.pSub && g_bHaveWorldObjs && !g_bDrawUnmatchedWM
			&& UnmatchedKindOf(bt.pSub) == 2) continue;

		// The segment in this batch's AUTHORED frame.
		float o[3] = { pFrom[0], pFrom[1], pFrom[2] };
		float e[3] = { pTo[0], pTo[1], pTo[2] };
		float B[9] = { 1,0,0, 0,1,0, 0,0,1 }, t[3] = { 0, 0, 0 };
		const bool bMoved = (g_bWorldXform && pA && pA->bMoved);
		if (bMoved)
		{
			for (int r2 = 0; r2 < 3; ++r2)
				for (int c2 = 0; c2 < 3; ++c2)
				{
					float v2 = 0.0f;
					for (int k2 = 0; k2 < 3; ++k2) v2 += pA->lr[r2 * 3 + k2] * pA->r[c2 * 3 + k2];
					B[r2 * 3 + c2] = v2;
				}
			for (int r2 = 0; r2 < 3; ++r2)
				t[r2] = pA->lp[r2] - (B[r2*3+0]*pA->p[0] + B[r2*3+1]*pA->p[1] + B[r2*3+2]*pA->p[2]);
			// x_authored = B^T (x_live - t)
			auto Back = [&](float* x) {
				const float d[3] = { x[0] - t[0], x[1] - t[1], x[2] - t[2] };
				for (int r2 = 0; r2 < 3; ++r2)
					x[r2] = B[0*3+r2]*d[0] + B[1*3+r2]*d[1] + B[2*3+r2]*d[2];
			};
			Back(o); Back(e);
		}
		// The box, slab test on the segment.
		const BatchBox& bx = g_BatchBox[b];
		float t0 = 0.0f, t1 = 1.0f; bool bMiss = false;
		for (int k = 0; k < 3 && !bMiss; ++k)
		{
			const float d = e[k] - o[k];
			if (fabsf(d) < 1e-6f) { if (o[k] < bx.mn[k] || o[k] > bx.mx[k]) bMiss = true; continue; }
			float ta = (bx.mn[k] - o[k]) / d, tb = (bx.mx[k] - o[k]) / d;
			if (ta > tb) { const float w = ta; ta = tb; tb = w; }
			if (ta > t0) t0 = ta;
			if (tb < t1) t1 = tb;
			if (t0 > t1) bMiss = true;
		}
		if (bMiss || t0 > fBest) continue;
		++nBoxPass; nTris += bt.nCount / 3;

		const float dx = e[0] - o[0], dy = e[1] - o[1], dz = e[2] - o[2];
		const UINT nEnd = bt.nStart + bt.nCount;
		for (UINT i = bt.nStart; i + 2 < nEnd && i + 2 < g_WorldVertsCpu.size(); i += 3)
		{
			const Vtx& a = g_WorldVertsCpu[i];
			const Vtx& bv = g_WorldVertsCpu[i + 1];
			const Vtx& c = g_WorldVertsCpu[i + 2];
			// Moller-Trumbore, both faces (a polygon's winding is the level's).
			const float e1x = bv.x - a.x, e1y = bv.y - a.y, e1z = bv.z - a.z;
			const float e2x = c.x - a.x,  e2y = c.y - a.y,  e2z = c.z - a.z;
			const float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
			const float det = e1x * px + e1y * py + e1z * pz;
			if (fabsf(det) < 1e-8f) continue;
			const float inv = 1.0f / det;
			const float tx = o[0] - a.x, ty = o[1] - a.y, tz = o[2] - a.z;
			const float u = (tx * px + ty * py + tz * pz) * inv;
			if (u < 0.0f || u > 1.0f) continue;
			const float qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
			const float v = (dx * qx + dy * qy + dz * qz) * inv;
			if (v < 0.0f || u + v > 1.0f) continue;
			const float tt = (e2x * qx + e2y * qy + e2z * qz) * inv;
			if (tt <= 0.0f || tt >= fBest) continue;
			fBest = tt; bHit = true;
			float pt[3] = { o[0] + dx * tt, o[1] + dy * tt, o[2] + dz * tt };
			if (bMoved)
			{
				// back to live space: x_live = B x_authored + t
				float lv[3];
				for (int r2 = 0; r2 < 3; ++r2)
					lv[r2] = B[r2*3+0]*pt[0] + B[r2*3+1]*pt[1] + B[r2*3+2]*pt[2] + t[r2];
				pt[0] = lv[0]; pt[1] = lv[1]; pt[2] = lv[2];
			}
			fBestPt[0] = pt[0]; fBestPt[1] = pt[1]; fBestPt[2] = pt[2];
		}
	}
	if (bTell)
		Log("  R3D_RayCast: from %.0f %.0f %.0f to %.0f %.0f %.0f | %u batches, %u verts, %u boxes passed, %u tris | %s at %.3f -> %.0f %.0f %.0f",
			pFrom[0], pFrom[1], pFrom[2], pTo[0], pTo[1], pTo[2],
			(unsigned)g_Batches.size(), (unsigned)g_WorldVertsCpu.size(), nBoxPass, nTris,
			bHit ? "HIT" : "miss", fBest, fBestPt[0], fBestPt[1], fBestPt[2]);
	if (!bHit) return 0;
	pOut[0] = fBestPt[0]; pOut[1] = fBestPt[1]; pOut[2] = fBestPt[2]; pOut[3] = fBest;
	return 1;
}

// ---------------------------------------------------------------------------
// R3D_PublishScope - where the scope looks from this frame, and how wide.
// ---------------------------------------------------------------------------
extern "C" void __cdecl R3D_PublishScope(const VRScopeFrame* p)
{
	RenderGuard _renderGuard;
	if (!p || IsBadReadPtr(p, sizeof(VRScopeFrame))) { g_bHaveScope = 0; return; }
	if (p->nMagic != VRSCOPE_MAGIC || p->nVersion != VRSCOPE_VERSION) { g_bHaveScope = 0; return; }
	memcpy(&g_Scope, p, sizeof g_Scope);
	g_bHaveScope = p->nActive ? 1 : 0;
	static int s_nSaidZoom = -99;
	if (g_bHaveScope && p->nZoom != s_nSaidZoom)
	{
		s_nSaidZoom = p->nZoom;
		Log("  R3D SCOPE: zoom level %d, field %.1f deg, from %.0f %.0f %.0f",
			p->nZoom, p->fFovDeg, p->fPos[0], p->fPos[1], p->fPos[2]);
	}
}

// R3D_DrawScopePass - the third world pass, into the scope's texture. Called
// by the scene hook once a frame, before the first eye, so the eyes' lens
// discs sample a finished picture.
// ---------------------------------------------------------------------------
void R3D_DrawScopePass()
{
	if (!g_bHaveScope || !g_pDev || !g_pCtx || g_bScopePass) return;
	if (!g_pScopeTex)
	{
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = td.Height = (UINT)g_nScopeSize; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = g_pDev->CreateTexture2D(&td, nullptr, &g_pScopeTex);
		if (SUCCEEDED(hr)) hr = g_pDev->CreateRenderTargetView(g_pScopeTex, nullptr, &g_pScopeRTV);
		if (SUCCEEDED(hr)) hr = g_pDev->CreateShaderResourceView(g_pScopeTex, nullptr, &g_pScopeSRV);
		D3D11_TEXTURE2D_DESC dd = td;
		dd.Format = DXGI_FORMAT_D32_FLOAT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (SUCCEEDED(hr)) hr = g_pDev->CreateTexture2D(&dd, nullptr, &g_pScopeDS);
		if (SUCCEEDED(hr)) hr = g_pDev->CreateDepthStencilView(g_pScopeDS, nullptr, &g_pScopeDSV);
		Log("  R3D SCOPE: %dx%d target %s (hr %08lX)", g_nScopeSize, g_nScopeSize,
			SUCCEEDED(hr) ? "created" : "FAILED", (unsigned long)hr);
		if (FAILED(hr)) { g_bHaveScope = 0; return; }
	}
	const float kBlack[4] = { 0, 0, 0, 1 };
	g_pCtx->ClearRenderTargetView(g_pScopeRTV, kBlack);
	g_bScopePass = 1;
	const float fFov = g_Scope.fFovDeg * 0.01745329f;
	R3D_DrawWorld(g_Scope.fPos, g_Scope.fQuat, fFov, fFov,
				  g_Scope.fNear > 0.0f ? g_Scope.fNear : 1.0f,
				  g_Scope.fFar  > 0.0f ? g_Scope.fFar  : 100000.0f,
				  0, 0, g_nScopeSize, g_nScopeSize, nullptr, 0, 0);
	g_bScopePass = 0;
	++g_nScopePasses;
	if (g_nScopePasses <= 3 || (g_nScopePasses % 900) == 0)
		Log("  R3D SCOPE: pass %ld drawn (%.1f deg)", g_nScopePasses, g_Scope.fFovDeg);
}

// R3D_PublishPrims - the client's effects for this frame. See VRShared.h.
// ---------------------------------------------------------------------------
extern "C" void __cdecl R3D_PublishPrims(const VRPrimFrame* p)
{
	RenderGuard _renderGuard;
	if (!p || IsBadReadPtr(p, sizeof(uint32_t) * 6)) return;
	if (p->nMagic != VRPRIM_MAGIC || p->nVersion != VRPRIM_VERSION) return;
	if (p->nRuns > VRPRIM_MAX_RUNS || p->nVerts > VRPRIM_MAX_VERTS) return;
	const size_t nHead = offsetof(VRPrimFrame, runs) + (size_t)p->nRuns * sizeof(VRPrimRun);
	if (IsBadReadPtr(p, nHead)) return;
	memcpy(&g_Prim, p, nHead);
	if (p->nVerts)
		memcpy(g_Prim.verts, p->verts, (size_t)p->nVerts * sizeof(VRPrimVert));
	g_Prim.nRuns = p->nRuns; g_Prim.nVerts = p->nVerts;
	g_bHavePrims = 1;
	static uint32_t s_nSaidAt = 0;
	if (p->nFrame - s_nSaidAt >= 900 && p->nRuns)
	{
		s_nSaidAt = p->nFrame;
		unsigned nA = 0, nF = 0, nM = 0; SprAnim_Stats(&nA, &nF, &nM);
		Log("  R3D: effects frame %u: %u runs, %u verts | drawn since last: %ld runs, %ld verts,"
			" %ld with no picture | %u sprite animations held (%u frames, %u missing)",
			p->nFrame, p->nRuns, p->nVerts, g_nPrimRunsDrawn, g_nPrimVertsDrawn, g_nPrimNoTex,
			nA, nF, nM);
		g_nPrimRunsDrawn = g_nPrimVertsDrawn = g_nPrimNoTex = 0;
	}
}

void R3D_SetPrims(int b) { g_bPrims = b; }

// ---------------------------------------------------------------------------
// R3D_PublishLights - the client's dynamic lights for this frame.
// ---------------------------------------------------------------------------
extern "C" void __cdecl R3D_PublishLights(const VRLightFrame* p)
{
	RenderGuard _renderGuard;
	if (!p || IsBadReadPtr(p, sizeof(uint32_t) * 4)) return;
	if (p->nMagic != VRLIGHT_MAGIC || p->nVersion != VRLIGHT_VERSION) return;
	if (p->nCount > VRLIGHT_MAX) return;
	const size_t nBytes = offsetof(VRLightFrame, lights) + (size_t)p->nCount * sizeof(VRLightInst);
	if (IsBadReadPtr(p, nBytes)) return;
	memcpy(&g_DynL, p, nBytes);
	g_DynL.nCount = p->nCount;
	g_bHaveDynL = 1;
	static uint32_t s_nSaidAt = 0;
	if (p->nFrame - s_nSaidAt >= 900)
	{
		s_nSaidAt = p->nFrame;
		Log("  R3D: dynamic lights: %u this frame; %ld passes since last lit the world with at least one",
			p->nCount, g_nDynWorldLit);
		g_nDynWorldLit = 0;
	}
}

void R3D_SetDynLights(int b) { g_bDynLights = b; }
