// ---------------------------------------------------------------------------
// world - read the level's own .DAT instead of the engine's heap.
//
// Stage 2 of docs/PLAN-FILES-NOT-HEAP.md. The reference implementation, and
// every identity enforced here, is tools/dat.py: 103 of 103 worlds parse, 20198
// world models, 1265645 polygons. This is that parse in C++, against the same
// bytes, so the two can disagree and be caught.
//
// WHY THE FILE AND NOT THE HEAP. Everything the renderer currently draws is
// reverse-engineered out of lithtech.exe's memory, and every rule not
// re-derived from the file is a defect waiting to be found in a headset. The
// clearest case is the marker geometry - the AI volumes and hull brushes the
// engine knows not to draw. On 5 September the heap's surface flags were ruled
// out as a way of telling those apart, with a denominator: one value covers
// 14751 marker polygons AND 5898 ordinary ones. The FILE's flags separate
// perfectly - 202 is worn by AI.dtx and Invisible.dtx and by nothing else.
//
// THE BRIDGE IS NAMES. Each world model carries its own texture name list, and
// those are exactly the names the stage 1 Dtx cache is keyed by.
// ---------------------------------------------------------------------------

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "render3d.h"		// R3D_LogFn

struct WorldSurface
{
	uint16_t nTexture;		// index into the model's own texture list
	uint32_t nPlane;
	uint16_t nFlags;		// 202 == a marker: AI.dtx or Invisible.dtx
};

struct WorldModelFile
{
	std::string sName;
	uint32_t nPoints, nPlanes, nSurfaces, nPolygons, nNodes, nVertexRefs;
	uint32_t nStart, nNext;

	std::vector<std::string> Textures;

	// THE AUTHORED PLACE. The file stores each world model's translation
	// after its bounding box, and the engine spawns the object there. It is
	// the baseline a moved door is measured from - "where it loaded" - and
	// it used to be taken from the first published pose instead, which is
	// wrong the moment a SAVE restores a door already open: the baseline was
	// the open pose, the geometry drew closed, and the handles (engine
	// objects, at the real pose) floated where the open door should be.
	// Seen in Morocco: large double doors that had been closed could be walked
	// straight through, with the door handles floating in the air.
	float    fTrans[3];
	bool     bHaveTrans;

	// Located, not computed. See World_LocatePlanes.
	uint32_t nPlaneAt;			// 0 if the planes could not be found
	uint32_t nSurfaceAt;		// always nPlaneAt + nPlanes * 16
	bool     bLocated;			// the planes are where we think they are
};

// A LIGHT THE LEVEL WAS AUTHORED WITH.
//
// docs/THE-LEVELS-OWN-LIGHTS.md. The engine reports no dynamic lights, which is
// true and was over-read as "the lighting is unreachable" - it is in the FILE,
// in the object block this parser used to stop at. M01S02 holds 463 Light, 54
// DirLight, 6 ObjectLight and a StaticSunLight among its 2200 objects.
struct WorldLight
{
	float fPos[3];
	float fRGB[3];			// 0..1, converted from the file's 0..255
	float fRadius;			// LightRadius, world units
	float fBright;			// BrightScale, 1.0 when not stated
	bool  bObjects;			// LightObjects - the author's own "this one
							// lights MODELS" flag. True when not stated.
	std::string sClass;		// Light, DirLight, ObjectLight, FlickerLight...
};

struct WorldFile
{
	std::string sPath;
	uint32_t nVersion;
	float    fBoxMin[3], fBoxMax[3];		// the world tree's OUTER box
	uint32_t nModelCount;
	// THE LEVEL'S OWN PROPERTIES STRING, which the parser used to step over.
	// Every NOLF world carries one - Morocco's reads
	// "LMGridSize 24;AmbientLight 48 48 48" - and the ambient term in it is a
	// lighting input this renderer has never had.
	std::string sInfo;
	// Parsed out of sInfo, 0..1. All zero when the level does not declare one.
	float    fAmbient[3];
	std::vector<WorldModelFile> Models;
	// EVERY OBJECT'S CLASS, BY NAME. The object block names each object and
	// its class ("charge1" is a SwitchT, "Garden_Water" a Water). A world
	// model the client has no engine object for is either a volume brush -
	// the server never gives those FLAG_VISIBLE, and the sphere search never
	// returns a container - or an object the level REMOVED at start:
	// ObjectRemover deletes whole groups so replays differ, and Morocco's
	// eight charges, M02S02's barrels and switches, M05S04's chemicals are
	// all in such groups. The class is what tells the two apart.
	std::map<std::string, std::string> Classes;
	// EACH VOLUME BRUSH'S OWN ANSWER TO "AM I DRAWN?". The game's VolumeBrush
	// declares a ShowSurface property; Water, Ice, CorrosiveFluid and
	// FreezingWater default it TRUE and the rest - Weather, SafteyNet,
	// Ladder, Wind, ColdAir, Filter, EndlessFall, Electricity, PoisonGas -
	// declare it hidden and FALSE (ObjectDLL/VolumeBrushTypes.cpp). DEdit
	// writes the value into every object's record either way: T05S01's
	// SafteyNet0 stores 0 and its Water0 stores 1. Name -> 0/1, present only
	// for objects whose record carried the property.
	std::map<std::string, int> ShowSurface;
	// THE ENGINE'S OWN ANSWER, WHICH IS NOT THE SAME QUESTION.
	//
	// ShowSurface asks "does this volume get a PolyGrid surface object?".
	// Visible asks "is the brush itself drawn?". For every non-liquid volume
	// in the game - 838 of them, Ladder, Filter, Weather, SafteyNet, Wind,
	// EndlessFall, Electricity, PoisonGas - both are false and the two rules
	// agree, which is why ShowSurface served as a proxy for a week.
	//
	// The 93 they disagree on are all liquids, and they disagree BOTH ways
	// (tools/dat-objects.py, swept over all 103 worlds, 18 September):
	//
	//   69  ShowSurface 1 / Visible 0 - a PolyGrid draws the surface and the
	//       brush is hidden. Drawing it too is the surface drawn twice, as two layers.
	//   24  ShowSurface 0 / Visible 1 - NO PolyGrid, and the brush's own faces
	//       ARE the liquid. Hiding it renders the body as NOTHING: M04S02's
	//       three canal waters, M06S02's beer, M01S03's drinking fountain.
	//
	// Name -> 0/1, present only where the record carried the property.
	std::map<std::string, int> Visible;
	// Every object a KeyFramer moves, by its ObjectName, lower case. A rising
	// flood (M05S05's water2) is a volume brush a keyframer lifts; see the
	// StubVolumeVisible rule.
	std::set<std::string> KeyframedLower;
	// Every light object in the file that carries a position, a colour and a
	// radius. Empty when the object block could not be walked - the world
	// still loads, because geometry does not depend on this.
	std::vector<WorldLight> Lights;
	// SKY POINTERS. A level's sky is more than its SkyBox: a SkyPointer
	// object names another world model (clouds, stars, a moon), gives it a
	// draw order (Index) and stands where the sky is viewed from. Berlin's
	// night sky is a box, two cloud layers, stars and a moon; without these
	// it was the box alone - a flat blue.
	struct SkyObj { std::string sName; float fIndex; float fInner; };
	std::vector<SkyObj> Sky;
	float fSkyView[3];	// half-size of the SkyDef view box (SkyDims x InnerPercent); 0 = no parallax
	float fSkyCam[3];		// the first pointer's position
	bool  bSkyCam;
	bool  bSkyCamFromDemo;	// the sky camera came from a DemoSkyWorldModel with SkyDims - the engine's own SkyDef
	bool  bSkyCamFromDims;	// the sky camera came from the pointer that defines the sky box (SkyDims non-zero)
};

// Parse a .DAT already in memory. Returns false and logs why on any identity
// failure - a parse that cannot prove itself is not used.
bool World_Parse(const uint8_t* pData, uint32_t nSize, const char* pszPath,
				 WorldFile* pOut, R3D_LogFn pfnLog);

// Which of the game's 103 worlds is the engine running?
//
// THE RENDERER IS NEVER TOLD. The engine hands us a world POINTER and nothing
// else - no name, no path - so the file has to be identified by what the heap
// and the file must agree about: the world model count and the bounding box.
// Both come from the engine's own header, neither is anything this code chose,
// and over 103 candidates the pair is decisive.
//
// Returns the path, or an empty string when nothing matches or more than one
// does. Ambiguity is reported, never resolved by picking.
std::string World_Identify(uint32_t nModelCount,
						   const float* pMin, const float* pMax,
						   R3D_LogFn pfnLog);

// Read the surfaces of one model. Empty if its planes were not located.
bool World_Surfaces(const WorldFile& w, const WorldModelFile& m,
					const uint8_t* pData, uint32_t nSize,
					std::vector<WorldSurface>* pOut);

// The whole job in one call: identify, read, parse. Null on any failure, and
// the reason is logged. The caller owns the result.
WorldFile* World_Load(uint32_t nModelCount, const float* pMin, const float* pMax,
					  R3D_LogFn pfnLog);
void World_Release(WorldFile* p);

// The file bytes a World_Load result is still pointing into. World_Surfaces
// needs them, and holding one copy beats holding two - a level's surface
// records are several megabytes.
bool World_Bytes(WorldFile* w, const uint8_t** pb, uint32_t* pn);
