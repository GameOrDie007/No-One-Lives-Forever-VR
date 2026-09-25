// ---------------------------------------------------------------------------
// render3d - the world, drawn.
//
// Kept out of dllmain.cpp for the same reason render2d is: that file is the
// ABI and the measurement harness, and this one is drawing.
//
// Everything this needs was measured rather than assumed, and each piece has a
// check recorded against it:
//
//   docs/WORLD-REACHED.md   the world arrives in the first dword of
//                           RebindLightmaps' argument
//   docs/WORLD-NAMED.md     [world + 0x18C] is 565 named WorldModels
//   docs/GEOMETRY-FOUND.md  one of them is "VisBSP"; its +0xA8 is 10829
//                           vertices and +0xA0 is 5957 polygon pointers, each
//                           polygon a 0x54-byte header, a uint16 count at
//                           +0x50 and an inline list of 24-byte entries whose
//                           first dword is a vertex. 5957 of 5957 coplanar.
//
// This module owns no engine knowledge beyond those offsets, and it never
// writes to engine memory.
// ---------------------------------------------------------------------------

#pragma once

#include <d3d11.h>
#include <stdint.h>

// The model contract lives with the rest of the client/renderer agreement.
#include "VRShared.h"

typedef void (*R3D_LogFn)(const char* fmt, ...);

bool R3D_Create(ID3D11Device* pDev, ID3D11DeviceContext* pCtx, R3D_LogFn pfnLog);
void R3D_Destroy();

// The back buffer changed size, so the depth buffer has to follow it.
void R3D_SetTarget(ID3D11RenderTargetView* pRTV, int nW, int nH);

// Walk the world and build one vertex buffer. Safe to call repeatedly; it
// rebuilds only when the world pointer changes, which is what a level load
// looks like from here.
void R3D_BuildWorld(uint32_t pWorld);

// Take a copy of one engine texture. Called from BindTexture, which is the only
// moment its pixels are known to be loaded - the engine loads texture data on
// demand and the renderer takes its copy during the bind. pKey is the engine's
// texture object, the same pointer a surface record holds.
void R3D_NoteTexture(uint32_t pKey, uint32_t pData);

// STAGE 1 of docs/PLAN-FILES-NOT-HEAP.md: hunt for the FILENAME of a texture
// the engine just bound, by walking the texture object and its data object
// for a printable string that ends in .dtx. The bridge between the live
// engine and the art on disk is names, and this is the search for where the
// engine kept the one it loaded the texture with.
//
// It stops itself after 120 textures, so it is bounded whether or not the
// switch is left on - but it is off by default anyway, because a measurement
// left running is what cost frame rate in an earlier build.
void R3D_SetTexName(int b);

// STAGE 1, the switch that matters: draw the engine's textures from the game's
// own .dtx files instead of from copies taken out of its heap. Falls back to
// the heap for anything it cannot name or cannot parse, so the worst case is
// the behaviour that shipped.
//
//   0  the heap, as before
//   1  the file, for every texture the heap path also accepts. Same set of
//      polygons drawn, same everything - but with the full mip chain the
//      engine does not keep, pixels that outlive the engine's copy, and a
//      name for an identity, which is what a reload cannot scramble.
//   2  the file for every texture the engine NAMES, including those the heap
//      path refuses. That set is almost entirely DEdit marker surfaces
//      (TEX/SKY, TEX/INVISIBLE, TEX/HULLMAKER), which have been drawing as
//      blue sky stand-in only because their textures were refused. Mode 2
//      shows them for what they are; hiding them again is stage 2's job and
//      must not be done with a name list here.
void R3D_SetTexFromFile(int b);

// Has a texture actually been created for this engine object? The caller must
// retry NoteTexture until this is true, NOT merely until it has seen the
// texture once: the engine binds a texture before its data is resident, and
// the first bind can carry a mip with pitch 0. Deduplicating on "seen" left
// 25174 of 26904 polygons untextured.
bool R3D_HasTexture(uint32_t pKey);

// Draw it, for one eye. The rectangle is the scene description's own viewport,
// so a side-by-side pair is two calls with two rectangles and nothing else
// different. Depth is cleared per call rather than per frame: the second eye
// draws into the same target, and a depth buffer left over from the first is
// exactly the failure that reads as "one eye is fine and the other is empty".
// pTan4, when given, is {tanLeft, tanRight, tanUp, tanDown} for this eye and
// replaces the two symmetric field angles entirely. tanLeft and tanDown are
// negative; the four are NOT redundant with each other and none of them may
// be derived from another.
// bInterface marks the INTERFACE scene - the menu. The engine renders that
// one through a camera parked at the WORLD ORIGIN, which is fine when there is
// no world and wrong the moment there is: the in-game pause menu is a folder
// state too, so the level was being drawn from above its own origin behind the
// menu text. See R3D_DrawWorld for what is substituted.
void R3D_DrawScopePass();	// the third pass, into the scope's texture; see render3d.cpp
void R3D_DrawWorld(const float* pPos, const float* pQuat,
                   float fFovX, float fFovY, float fNear, float fFar,
                   int nLeft, int nTop, int nRight, int nBottom,
                   const float* pTan4, int nEye, int bInterface);

// Is a level loaded? The caller needs this to tell a MAIN menu, which has no
// world and wants its 4:3 pillarbox, from a PAUSE menu, which has one.
int  R3D_HaveWorld();

// Draw one clip-space triangle instead of the world, through the same shaders,
// target and present path. The one-variable split between "the pipeline does
// not reach the screen" and "the world data or the camera is wrong".
void R3D_SetTest(int b);

// How much to multiply a lightmap texel by. The level's largest channel
// value is 31 of 31 counted over all 748434 texels, so the data is already
// full scale and this is 1. +StubLMScale100 sets it in hundredths, so 150
// is 1.5 - there so the level can be compared against the retail renderer
// without a rebuild.
void R3D_SetLMScale(float f);
// Multisampling for the WORLD pass. The 2D layer is drawn after the
// resolve, at one sample, so text stays crisp.
void R3D_SetMsaa(int n);
// Stall attribution: time our present slot and our log writes spend.
void R3D_AddPresentMs(double fMs);
void R3D_NotePresentParts(double fLimiter, double fPresent);
void R3D_AddLogMs(double fMs);
void R3D_SetMsaaClear(int b);
int  R3D_MsaaMade();
// Resolve the world into the back buffer. Must run before any 2D.
void R3D_ResolveMsaa();

// 0 draws normally. 1 draws the lightmap with no texture. 2 paints the
// lightmap COORDINATE as colour, so "does the second texture coordinate
// reach the pixel shader" has a picture. 3 samples the atlas with the
// TEXTURE coordinate, so "is t1 bound at all" has one too.
void R3D_SetLMOnly(int b);

// Name every batch's cut-out measurement at resolve time. +StubCutProbe 1.
void R3D_SetCutProbe(int b);
// +StubCutGuess 0: trust only the file's own alpharef and never guess a
// cut-out from the pixels. The guess deletes 89% of a lamp shade.
void R3D_SetCutGuess(int b);
void R3D_SetEnvNoCut(int b);
void R3D_SetPieceMatIndex(int b);

// Name every model piece's skin, for every instance, on ONE LATE FRAME.
// +StubSkinFrame <n>; 0 is off. The menu's throttled skin-name check needs a
// few frames to correct a slot, so anything capped at "the first N" prints the
// stale names - which has now misled twice in one day.
void R3D_SetSkinFrame(int n);

// A sprite's texture must agree BY NAME every frame, and a candidate the
// engine will not name is refused. +StubSprStrictTex 0 trusts the address, as
// before - which on M05S01 dresses a night alley in the main menu's olive card.
void R3D_SetSprStrictTex(int b);
void R3D_SetSprNameProbe(int b);
void R3D_SetSprFromFile(int b);
void R3D_SetSkinNameProbe(int b);
void R3D_SetSkinFromButes(int b);
void R3D_SetHideViewArms(int b);
void R3D_SetDrawBody(int b);
void R3D_SetHideHead(int b);

// Turn lightmaps off entirely, so every polygon draws with the stand-in
// normal shade. This is the A of the A/B: it is the picture the renderer
// drew before any of this, reachable without a rebuild.
void R3D_SetLMEnable(int b);

// Sampler anisotropy. Every texture here has one mip level, so a grazing
// surface aliases into moving vertical stripes without it. 0 or 1 selects
// the old bilinear sampler.
void R3D_SetAniso(int n);

// 1 (default) draws untextured polygons - overwhelmingly the sky - in a blue
// measured off a retail frame. 0 restores the white stand-in.
void R3D_SetSkyStandIn(int b);
// +StubSkyBox 0: draw the SkyBox world model as ordinary geometry again.
void R3D_SetSkyBox(int b);
// +StubModelDims <units>: the mesh path's half-extent limit per model.
void R3D_SetModelDims(float f);
// +StubFastRead 1: cache address validity per page instead of probing per read.
void R3D_SetFastRead(int b);
// +StubWorldBSP 0|1|2: draw VisBSP, PhysicsBSP, or both.
void R3D_SetWorldBSP(int n);
// +StubBSPCover 1: report VisBSP faces with no coplanar PhysicsBSP face.
void R3D_SetBSPCover(int b);
// +StubTransAlpha <0-100>: how see-through a TranslucentWorldModel is.
void R3D_SetTransAlpha(float f);
// +StubTransNames 0: only TranslucentWorldModel counts as see-through.
void R3D_SetTransNames(int b);
// +StubWorldRebuild <ms>: rebuild the world mesh on a timer (diagnostic).
void R3D_SetWorldRebuild(int nMs);
// The client hands over every OT_WORLDMODEL's live position and rotation.
struct VRWorldFrame;
extern "C" void __cdecl R3D_PublishWorldModels(const VRWorldFrame* p);
// +StubWorldProbe 1: find the offset in an HOBJECT that names its world model.
struct VRSpriteFrame;
extern "C" void __cdecl R3D_PublishSprites(const VRSpriteFrame* p);

// The level's fog, from the client, which reads the engine console variables
// the level set. Colour is 0..1 per channel; the distances are world units.
// The level's SKY fog (WorldProperties SkyFogEnable / SkyFogNearZ / SkyFogFarZ,
// mirrored into the console by the client): the sky pass is fogged at these
// distances from the sky camera in the level's fog colour.
extern "C" void __cdecl R3D_PublishSkyFog(int bEnable, float fNear, float fFar);
// The camera's light add (CScreenTintMgr: a container's tint, the damage
// flash, the poison's green), 0..1 per channel, added over the whole eye.
extern "C" void __cdecl R3D_PublishLightAdd(float r, float g, float b);
void R3D_SetLightAddAllow(int b);
// Environment maps over world polygons (the texture's command string): on/off,
// the map's strength, and its pan per second.
void R3D_SetBatchList(int b);
void R3D_SetWaterLook(float fLight, float fContrast);
void R3D_SetEnvDepth(int b);
void R3D_SetWaterFlow(float f);
void R3D_SetWaterFirst(int b);
void R3D_SetWaterTop(int bOn);
void R3D_SetEnvWorld(float fRepU, float fRepV, float fFlow);
void R3D_SetSkipBatch(int n);
void R3D_SetEnvCoord(float f);
void R3D_SetEnvMap(int b, float fScale, float fPan);
void R3D_SetSkyFogAllow(int b);
extern "C" void __cdecl R3D_PublishFog(int bEnable, float r, float g, float b,
									   float fNear, float fFar);

// Non-zero and fills pRGB when the frame should be cleared to the fog colour
// rather than to black. See the note by its definition.
extern "C" int __cdecl R3D_FogClearColour(float* pRGB);

// +StubFog 0 disables fog entirely, whatever the level asks for.
void R3D_SetFog(int b);

// How often, in frames, a model skin slot re-checks that the cache entry
// filed under its address is still the picture the engine keeps there.
// 1 = every frame (costly), 0 = never. +StubSkinNamePeriod, default 30.
void R3D_SetSkinNamePeriod(int n);
void R3D_SetSkySkip(const char* psz);
void R3D_SetWaterFlowAny(int b);
void R3D_SetWaterBias(int n);
void R3D_SetVolumeVisible(int b);
void R3D_SetSkyInGlass(int b);
void R3D_SetSkyCamCentre(int b);
void R3D_SetSkyTrace(int n);
void R3D_SetSkyParallax(int b);
void R3D_SetMipOffsetUV(int b);
void R3D_SetMipOffsetSky(int b);
void R3D_SetWorldCull(int b);
void R3D_SetCullFromEye(int b);
void R3D_SetBatchWatch(const char* s);
void R3D_SetModelWatch(const char* s);
void R3D_SetGlassDepth(int b, int nCut100);
void R3D_SetSkyChecker(int b);
// +StubSpriteProbe 1: find where a sprite object keeps its texture.
void R3D_SetSpriteProbe(int b);
// +StubSprites 0 / +StubSpriteScale100 <n>: the effect quads.
void R3D_SetSprites(int b);
void R3D_SetPrims(int b);
void R3D_SetDynLights(int b);
void R3D_SetSpriteScale(float f);
void R3D_SetWorldProbe(int b);
// +StubWorldXform 0: freeze world models at their authored positions again.
void R3D_SetWorldXform(int b);
void R3D_SetWorldSeed(int b);
void R3D_SetDrawHiddenWM(int b);
void R3D_SetDrawUnmatchedWM(int b);
// +StubWorldNudge <units>: force every world model to move, to prove the path.
void R3D_SetWorldNudge(float f);

// Diagnostic: drop the TranslucentWorldModels entirely.
void R3D_SetSkipTranslucent(int b);

// 0 (default) skips batches whose texture object has no pixels; 1 draws them
// as a white 1x1, which is how to see where they are.
void R3D_SetDrawNoPixels(int b);

// The client's per-frame model list. Resolved out of this DLL by name; see
// VRShared.h for why the client is the side that knows.
extern "C" void __cdecl R3D_PublishModels(const VRModelFrame* p);

// Draw a box per model instance - the stand-in until meshes are read.
void R3D_SetModelBoxes(int b);
void R3D_SetModelMesh(int b);

// Draw ONLY the model meshes - no world. The split between "the model geometry
// never reaches the screen" and "it reaches it and something hides it".
void R3D_SetModelOnly(int b);

// Draw the untextured model mesh in the box's orange rather than the untextured
// fallback - which is the SKY stand-in colour, so characters drew sky-coloured.
void R3D_SetMeshTint(int b);

// Rank every offset in a model PIECE by how often it resolves to a texture the
// engine has already bound. The last unknown in the model format.
void R3D_SetTexHunt(int b);

// Are a piece's face indices global to the model or local to its own slice?
void R3D_SetIdxHunt(int b);
void R3D_SetBeamDump(int b);
// +StubModelProbe <substring>: dump the bind box beside the world box for any
// instance whose model FILENAME contains this, case-insensitively. The beam
// dump asks "did the mesh arrive intact, and is it the transform that moved
// it"; this asks it of a model you can name.
void R3D_SetModelProbe(const char* psz);
void R3D_SetObjScale(int b);
void R3D_SetPieceSkin(int b);
void R3D_SetVtxNrm(int b);
void R3D_SetNodeT(int b);
void R3D_SetEntryCount(int n);
void R3D_SetWeightPre(int b);
void R3D_SetEdgeChk(int b);
void R3D_SetFlushOnLoad(int b);
void R3D_SetSkinConsist(int b);
void R3D_WorldLoaded();
void R3D_SetAlphaTest(int b);
void R3D_SetA2C(int b);
void R3D_SetAlphaSharp(int b);
void R3D_SetDrawSections(int b);
// +StubCull: 0 none (default), 1 back faces, 2 front faces. See R3D_Create.
void R3D_SetCull(int n);
void R3D_SetSkipOcclTex(int b);
void R3D_SetTexDataFn(void* fn);
void R3D_PullQueuedSkin();
void R3D_RefreshStaleTexture();
void R3D_SetSkinPull(int b);

// Index a piece's faces into its own slice of the model's vertex array rather
// than into the whole array. 0 restores the old behaviour for one A/B.
void R3D_SetPieceBase(int b);
void R3D_SetMenuZoom(float f, float ax, float ay, int bOn);
void R3D_SetSkipInvisible(int b);

// Skin each piece from its OWN record array (piece+04) to exactly its own
// vertex count (piece+08), rather than from a slice of the model's array.
void R3D_SetPieceVerts(int b);

// Skin from the piece's 32-byte vertex ENTRY array (piece+04), each entry
// pointing at its own weight records in the model array with a uint16 count.
void R3D_SetPieceVA(int b);

// Bearing and distance to the nearest model that is IN FRONT of the
// camera, so a frame dump can wait for one instead of being timed by hand.
void R3D_NearestModel(float* pAz, float* pDist);

// One engine FRAME has ended. Called from SwapBuffers, which is the frame
// boundary the engine itself uses, so the FPS line counts frames rather than
// assuming a fixed number of world passes per frame. It assumed two, a run
// drew one, and it reported half the real rate for a whole session.
void R3D_NotePresent();

// Write every cached texture out, named by its engine key. The instrument for
// "which texture is this surface actually drawing", asked from the picture
// backwards instead of from the polygon forwards.
// Dumps ONE texture per call - call it once a frame. Reading a texture back
// waits for the GPU, and several of those inside one callback trips the
// driver watchdog and resets the device. Returns true while there is more.
bool R3D_DumpNextTexture(const char* pszDir, R3D_LogFn pfnLog);

// Smallest edge the dump will read back, so narrowing it does not need a
// rebuild. Each texture costs a frame.
void R3D_SetDumpMin(int n);

// The surface-flags histogram, and the switch that acts on it. The engine's
// world surface flags are not in the client SDK, so which bit means "do not
// render this" has to be measured rather than looked up.
// The frame as the renderer drew it, written straight from the back buffer.
// Independent of the desktop: a window capture returns black whenever the game
// window is occluded or the monitors are rearranged, and both have happened.
bool R3D_DumpBackBuffer(struct ID3D11Texture2D* pBack, const char* pszPath);
void R3D_SetDumpAlphaAsGrey(int b);

void R3D_SetFlagHist(int b);
void R3D_SetSkipFlags(uint32_t n);
void R3D_SetWorldFile(int b);
void R3D_SetSurfProbe(int b);
void R3D_SetMarkerFlags(uint32_t n);
void R3D_SetBridgeCheck(int b);
void R3D_SetFileTex(int b);
void R3D_SetMarkerTex(int b);
void R3D_BridgeReport(R3D_LogFn pfnLog);
void R3D_SurfProbeReport(R3D_LogFn pfnLog);
void R3D_FlagReport(R3D_LogFn pfnLog);

// How many textures the cache holds, so a caller can wait for a level's
// textures to exist before asking for them.
int R3D_TextureCount();

// Where to write the atlas as a BMP, once, on the first world build.
void R3D_SetLMDump(const char* psz);

void R3D_Stats(int* pnPolys, int* pnTris, long* pnDraws);

// Whether the textures the last world build could not find have arrived since.
// The difference between "the engine never gives us these" and "the world was
// built too early and never rebuilt" - which are opposite fixes.
void R3D_MissingReport(R3D_LogFn pfnLog);

// Draw polygons whose texture the engine never loaded. ON by default, because
// they are the SKY: the engine draws the sky by another path and never loads
// these textures, and our white stand-in reads as overcast. Turning this off
// leaves a black hole where the sky is - which is how they were identified.
void R3D_SetDrawUntextured(int b);
void R3D_SetSprAdd(int b);
void R3D_TexturesMayHaveMoved(const char* pszWhy);
void R3D_SetSprAddMod(int b);
void R3D_SetDrawUntexturedTrans(int b);

// Reversed-Z: map near->1 and far->0, test GREATER, clear to 0. With near 0.44
// and far 100000 the conventional mapping spends almost all of a float's
// precision within a few units of the eye, which is the textbook cause of
// Z-fighting streaks. On by default; +StubRevZ 0 is the A of the A/B.
void R3D_SetRevZ(int b);

// Force the near plane, in world units; 0 = whatever the scene description
// says. A measuring instrument, not a setting: a high near plane gives
// conventional Z enough precision to act as a reference the two depth
// mappings can be judged against.
void R3D_SetNearOverride(float f);

// Force the far plane. Purely a measuring instrument: nudging it changes every
// depth value while clipping nothing, so pixels that change are pixels whose
// ordering was decided by precision rather than by geometry.
void R3D_SetFarOverride(float f);

// The engine texture object on the largest upward-facing polygon in the level -
// the ground. 0 until a world has been built.
uint32_t R3D_FloorTexture();

// How many cached textures have had their engine object reset under them. The
// cache is keyed on an address the engine reuses.
void R3D_StaleReport(R3D_LogFn pfnLog);

// What is drawn along a ray, named: the WorldModel that owns the polygon, the
// engine's surface flags, the texture object, and that texture's pixels
// written out beside them. The instrument for a complaint about a PLACE in the
// picture rather than about a number - a blue ground names a polygon
// under the player's feet, and nothing else here can find that polygon.
void R3D_ProbeRay(const float* pOrigin, const float* pDir, const char* pszWhat,
                  const char* pszDumpDir, R3D_LogFn pfnLog);

// Every WorldModel named, with the polygons it carries, and a search for the
// bit that tells the drawn ones from the editor volumes the retail renderer
// never shows. Names are the ground truth here, not the answer - the answer is
// a field the engine itself uses.
void R3D_ModelReport(R3D_LogFn pfnLog);
// The breadcrumb the stall watchdog reads: which part of a frame the main thread was in.
extern "C" const char* R3D_Phase();
extern "C" void R3D_GpuFrameBegin();	// timestamp at the first world pass of a frame
extern "C" void R3D_GpuFrameEnd();		// timestamp before Present; reads last frame's pair
extern "C" void R3D_GpuTailStamp(int i);	// 0 second pass done, 1 before publish, 2 after

// Leave PhysicsBSP and the AI volumes out of the world - the collision hull
// and the editor's volume brushes, neither of which the engine ever shows.
// On by default; +StubSkipHidden 0 draws them again, which is the A of the
// A/B and the picture every screenshot before 3 September was taken from.
void R3D_SetSkipHidden(int b);

// The same question asked of the polygon and its surface record, with the
// cleanest ground truth the level offers: VisBSP's polygons are drawn and the
// AI volumes' are not.
void R3D_SurfaceSearch(R3D_LogFn pfnLog);

// Let the ray probe read its texture back. Off by default: the read-back
// stalls the render thread and the first run of the probe froze the game.
void R3D_SetProbeDump(int b);

// Three of those from the pose the world was last drawn from: down, ahead and
// along the gaze.
void R3D_ProbeFromCamera(const char* pszDumpDir, R3D_LogFn pfnLog);

// From the published HOBJECT towards the mesh: searches for the node count
// the client already told us, and for a filename ending .abc.
// +StubModelWalk 1.
void R3D_ModelWalk(R3D_LogFn pfnLog);

// Scan the heap for filenames ending .abc and for what points at them.
// +StubModelWalk 1 runs this too.
void R3D_FindAbc(R3D_LogFn pfnLog);

// Measure each model array's element stride against the published
// half-extents. +StubModelWalk 1 runs this too.
void R3D_MeshStride(R3D_LogFn pfnLog);
void R3D_MeshDump(R3D_LogFn pfnLog);
void R3D_MeshVerify(R3D_LogFn pfnLog);
void R3D_FindFaces(R3D_LogFn pfnLog);

// Every surface along one ray, nearest first, flagging pairs close enough in
// depth to z-fight. The nearest-hit probe cannot see a fight at all.
void R3D_ProbeRayDepth(const float* pO, const float* pD, const char* pszWhat,
					   R3D_LogFn pfnLog);

// The same, aimed by angle within the left eye's frustum: +StubProbeAz /
// +StubProbeEl, so a feature measured off a screenshot can be named.
void R3D_ProbeAt(float fAzDeg, float fElDeg, R3D_LogFn pfnLog);

// Write the pixels we are actually drawing for one engine texture out as a BMP.
bool R3D_DumpTexture(uint32_t pKey, const char* pszPath);

// Apply each model instance's object scale to the mesh offsets it skins.
// +StubModelScale, default 1.
void R3D_SetModelScale(int b);

// Draw sprites back to front. The sprite pass writes no depth, so order
// alone separates them. +StubSprSort, default 1.
void R3D_SetSprSort(int b);

// Draw FLAG2_ADDITIVE models with additive blending.
// +StubAddBlend, default 1.
void R3D_SetAddBlend(int b);
// FLAG2_MULTIPLY on sprites and models. +StubMulBlend 0 draws them
// alpha-blended, which is what every build before this one did.
void R3D_SetMulBlend(int b);
// The client says a full-card folder (mission status, summary, briefing...)
// is up: draw the interface scene from the interface camera and leave the
// world out, as retail does. 0 for the pause family, which keeps the world.
void R3D_SetInterfaceOnly(int b);
// Graded-alpha model skins (rotor blur, windscreens) drawn alpha-blended in
// the last pass. +StubModelGlass 0 draws them opaque, as before 13 September.
void R3D_SetModelGlass(int b);
// The menu panel's aspect (width / height): the interface scene is zoomed so
// the centred band of that shape spans the scene's own vertical angle.
void R3D_SetMenuBand(float f);
// An unmatched volume brush is drawn only when its own ShowSurface says so.
// +StubVolumeSurface 0 draws every volume class, as before 13 September.
void R3D_SetVolumeSurface(int b);
// A world polygon with no lightmap is lit by its stored vertex colour (retail's
// way); 0 goes back to the light-grid stand-in.
void R3D_SetVertexColour(int b);
// +StubMulForce 1: every model run drawn multiply, whatever it is flagged.
// A diagnostic - it is how the mesh multiply path is tested without a
// firefight, since only seven FX records in the game ever ask for it.
void R3D_SetMulForce(int b);

// WHAT THE RENDERER WAS DOING WHEN IT DIED, for the crash filter. A phase name
// stored as a const char* - one instruction, no formatting, safe to read from
// an exception filter - plus how many world passes had completed. See the note
// at g_pszPhase: this renderer is one enormous per-frame function, so "it
// crashed in d3dstub" is all the module walk can ever say on its own.
// +StubCrashTest 1: fault deliberately inside the sprite pass, once, after 60
// world passes. The only way to prove the crash filter and its phase line
// actually produce output. Default 0.
void R3D_SetCrashTest(int b);
// +StubSkipStill 1: skip every instance whose skeleton did not move. A
// MEASUREMENT with a deliberately wrong picture - it bounds what a per-object
// vertex cache could save before one is built. Default 0.
void R3D_SetSkipStill(int b);
// +StubSkinCache 1: keep the skinned vertices of an instance whose skeleton,
// scale and flags have not changed, instead of walking the engine's heap and
// re-skinning it every build. Default 0 while it is unproven in a headset.
void R3D_SetSkinCache(int b);
void R3D_SetMeshPool(int b);
void R3D_SetSkinDiag(int b);
void R3D_SetMemKindCache(int b);
// +StubModelLight 0: put the fixed directional stand-in back. Default 1 - a
// model is lit by the level's own baked light, read out of the lightmaps into
// a coarse grid at world build.
void R3D_SetModelLight(int b);
void R3D_SetTerrainGrid(int b);		// +StubTerrainGrid: light lightmap-less world polygons from the grid
void R3D_SetLightScaleAllow(int b);	// +StubLightScale: apply the engine's global light scale
void R3D_SetLightScaleTest(int n);	// +StubLightScale100 N: force N/100 as the positive control
void R3D_SetPubSkins(int b);		// +StubPubSkins: skins by the name the client created the object with
void R3D_SetSkyOccluder(int b);		// +StubSkyOccluder: sky brushes write depth only, behind the backdrop
// +StubLightObjects 0: light models from lightmap texels only, the way
// it worked before the level's own light objects were found.
void R3D_SetLightObjects(int b);
void R3D_SetLightDirect(int b);
void R3D_SetLightGain(float f);
const char* R3D_LastPhase();
long        R3D_PhaseFrames();

// Draw FLAG2_ADDITIVE models after the sprites instead of in the mesh pass.
// +StubAddLast 0 restores the old order. An additive draw adds to what is
// already in the target, and the menu's motifs sit on a SPRITE.
void R3D_SetAddLast(int b);
void R3D_SetMeshRing(int n);	// model mesh ring regions, 1 = discard every frame
