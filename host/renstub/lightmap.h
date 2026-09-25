// ---------------------------------------------------------------------------
// lightmap - NOLF's static lighting, out of engine memory and into one atlas.
//
// Everything here was read out of the retail renderer and the engine before it
// was written, and every step was checked on the whole level rather than on a
// sample. docs/LIGHTMAPS.md records the run; the short version is:
//
//   surface + 0x38 bit 0x80    the surface is lightmapped
//   poly + 0x48 / + 0x49       the lightmap's width and height, as BYTES
//   poly + 0x0C / + 0x10       an array of 4-byte {uint16 anim, uint16 data}
//   world + 0x08 / + 0x0C      the light animations, 0x60 bytes each; NOLF's
//                              levels have exactly one, "LightAnim_BASE"
//   anim + 0x24 / + 0x34       the frame array and the current frame; the
//                              frame's data is an array of {pointer, length}
//                              indexed by the ref's second uint16
//   the record                 16-bit run-length, bit 15 of a value means a
//                              count byte follows, so the texel is 15 bits
//   poly + 0x34                the lightmap origin
//   surface + 0x34             the plane; its normal drives the basis
//   world + 0xF8               the texel size in world units (24 in m01s02)
//
// and the basis itself is lithtech.exe + 0x3CD10:  P = V x n,  Q = n x P,
// where V comes from a six-entry table indexed by the top three bits of the
// surface flags.
//
// Nothing here writes to engine memory and nothing here calls into the engine.
// ---------------------------------------------------------------------------

#pragma once

#include <d3d11.h>
#include <stdint.h>

typedef void (*LM_LogFn)(const char* fmt, ...);

// Where one polygon's lightmap ended up, and how to address it.
struct LMPoly
{
	float fX, fY;			// the rectangle's origin in the atlas, in texels
	int   nW, nH;			// its size in texels
	float P[3], Q[3];		// the lightmap axes
	float O[3];				// the lightmap origin, in world space
	float fS;				// world units per lightmap texel
};

// The decompressor, on its own, so the probe in dllmain.cpp and the atlas
// builder cannot drift apart. Returns 1 only when the record is consumed
// exactly - a decoder with the wrong stride still produces plausible pixels.
int  LM_Decode(uint32_t pSrc, uint32_t nLen, uint16_t* pOut, uint32_t nMax,
			   uint32_t* pnOut);

// The engine's basis table, read out of the running lithtech.exe because it is
// past the end of the file's raw .data and only exists once the process is up.
// Safe to call more than once. Returns 1 when six unit vectors were found.
int  LM_AxisTable(LM_LogFn pfnLog);

// P and Q for one surface, exactly as lithtech.exe + 0x3CD10 computes them.
int  LM_Basis(uint32_t pSurface, float* pP, float* pQ);

// Walk the world, decode every lightmap and pack them into one atlas texture.
// Rebuilding for the same world is a no-op. Returns 1 when there is an atlas.
// pszDump, when not empty, is a path to write the populated part of the
// atlas to as a BMP.
int  LM_Build(uint32_t pWorld, ID3D11Device* pDev, LM_LogFn pfnLog,
			  const char* pszDump);
void LM_Destroy();

ID3D11ShaderResourceView* LM_Atlas();
float LM_AtlasSize();

// Look one polygon up. Returns 0 for a polygon with no lightmap, which is a
// normal answer: 19626 of the level's 26904 polygons have one.
int  LM_ForPoly(uint32_t pPoly, LMPoly* pOut);

// Read one atlas texel back, so a computed coordinate can be checked
// against what is actually there rather than against what should be.
uint32_t LM_Texel(int nX, int nY);
