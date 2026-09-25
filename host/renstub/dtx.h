// ---------------------------------------------------------------------------
// dtx - a texture read from the game's own file, not from the engine's heap.
//
// Stage 1 of docs/PLAN-FILES-NOT-HEAP.md, standing on the REZ file system that
// stage 0 built. Everything the renderer draws today is a copy taken out of
// lithtech.exe during BindTexture, which is the only moment its pixels are
// known to be resident. That single constraint is the root of a whole class of
// defect: a skin the engine never binds has no pixels at all (the weapon grip),
// a reload frees every texture and hands the addresses back for other pictures
// (the monkey seller in Cate's clothes), and there is no identity on a texture
// object to tell those apart - measured, docs/RELOAD-SCRAMBLES-SKINS.md.
//
// The file has none of those problems. It is the same bytes every time, it is
// there whether or not the engine has drawn anything, and it carries a full mip
// chain the engine's in-memory copy does not.
//
// THE FORMAT, verified against every texture in the mounted archives.
//
//   +0x00  uint32  resource type, always 0
//   +0x04  int32   version, always -5
//   +0x08  uint16  width          +0x0A  uint16 height
//   +0x0C  uint16  mip count      +0x0E  uint16 section count
//   +0x10  int32   flags          +0x14  int32  user flags
//   +0x18  uint8   extra[12]  - extra[2] is the format identifier
//   +0x24  char    command string[128], NUL terminated, usually empty
//   0xA4   the mip chain, largest first, no padding
//
// The header is 164 bytes and the mip chain follows it with nothing between,
// so `164 + sum(mip bytes) == file size` is an arithmetic identity the file
// either satisfies or does not. It holds on 4173 of the 4174 textures the
// engine actually mounts. The one exception is TEX/GLASS/GL01/GL0007.DTX from
// NolfGoty.rez, which declares format 7 and one section; it is refused and the
// engine's own copy is used for it, which is the behaviour for anything this
// parser does not fully understand.
//
// PALETTISED TEXTURES STILL STORE 32-BIT PIXELS. 605 of the mounted textures
// declare format 0 (BPP_8P) and every one of them is four bytes per texel on
// disk - the identifier says what the engine converts to, not what is written
// down. Believing it named the storage put 604 files 4x under their real size,
// which is exactly the kind of quiet mis-parse that hands the GPU whatever
// memory happened to follow. The size identity is what caught it.
// ---------------------------------------------------------------------------

#pragma once

#include <d3d11.h>
#include <stdint.h>

#include "render3d.h"		// R3D_LogFn

struct DtxInfo
{
	uint32_t	nWidth, nHeight;
	uint32_t	nMips;			// mip levels actually created
	// THE HEADER'S NON-S3TC MIPMAP OFFSET, extra[3]: the engine loads such a
	// texture from that mip down and the game treats it as that size, so a
	// world polygon's texel coordinates are in the SMALLER texture's units.
	// TEX/SIGNS/GERMAN/GSIGN009.DTX (the Berlin gate booth's GEFAHR sign) is
	// 128x128 with offset 1: divided by 128 its polygon showed one quarter of
	// the sign at twice the size. 0 for the compressed formats, which the
	// byte does not govern.
	uint32_t	nUVShift;
	uint32_t	nFormat;		// the file's own format identifier
	uint32_t	nFlags;			// the header's flags word (+0x10). 0x80 is
								// PREFER4444: the artist saying the alpha
								// channel is meant to be kept.
	DXGI_FORMAT	fmt;			// what it became
	uint32_t	nBytes;			// the mip chain's size, header excluded
	float		mr, mg, mb;		// mean colour of mip 0, sampled as
								// R3D_NoteTexture samples it so the two
								// numbers can be compared directly
	float		fCut;			// fraction of transparent texels/blocks
	// THE FILE'S OWN ANSWER, 0 when it does not give one. A DTX carries a
	// 128-byte command string, and NOLF's foliage says "alpharef 96;" in it -
	// the artist declaring the alpha-test threshold, in 0..255, that this
	// texture is meant to be drawn with. Every tree, every flower, every
	// grille says so. Guessing it from the pixels is answering a question
	// somebody already wrote down.
	float		fAlphaRef;		// 0..255 as declared, or 0 for "not declared"
	// WHAT LIVES IN THE TRANSPARENT REGION, as a mean absolute deviation of
	// its RGB. This is what tells a real cut-out from a texture whose alpha
	// channel is junk, and the difference is stark:
	//
	//   CATTAIL.DTX     65.5% clear, spread  0.0   a flat key colour
	//   FOLIAGE_01.DTX  78.1% clear, spread  0.0   a flat key colour
	//   LAMP_05B.DTX    88.6% clear, spread 43.8   real, varied art
	//   LIGHTBULB.DTX   50.0% clear, spread 24.4   real, varied art
	//
	// An artist cutting a plant out fills the unused area with ONE colour
	// nobody will ever see. An artist painting an opaque lamp shade never
	// authors alpha at all, so the "transparent" region still holds the
	// picture - and alpha testing it deletes most of the shade.
	//
	// 0 for the block formats, which are not measured this way.
	float		fClearSpread;
	// THE ALPHA CHANNEL WAS ENTIRELY ZERO, before the rule below rewrote it
	// opaque. On its own that only means "nobody authored alpha"; together
	// with a DARK picture it means the art is meant to be added, not blended -
	// a flare, a glow, a moon. Drawn opaque instead, it is a black square.
	// a black square where the moon should have
	// been. FLR0030.DTX, the MoonFlare: 256x256, alpha 0..0,
	// border brightness 0.0.
	uint32_t	bZeroAlpha;
	// THE ENVIRONMENT MAP the command string names ("EnvMap Tex\EnvMap\
	// EnvMap011.dtx" on the HQ waterfall's WA0010), or "". The retail
	// renderer draws it over the surface, panned by EnvPanSpeed: that is the
	// waterfall's shimmer with a still camera (13 September).
	char		szEnvMap[64];
	float		fBimodal;		// fraction of alpha texels that are fully clear or
							// fully opaque. A channel nothing ever wrote reads 100%
							// clear and is NOT a cut-out; the two are told apart by
							// this, not by fCut alone. 1.0 for the block formats.
};

// Read the header and check the size identity. Touches no pixels and creates
// nothing; false means this buffer is not a texture we are willing to decode.
bool Dtx_Describe(const uint8_t* pFile, uint32_t nSize, DtxInfo* pOut);

// Build a D3D11 texture with the FULL mip chain the file carries. The caller
// owns the returned view. Null on any failure, including a header that does
// not satisfy the size identity.
ID3D11ShaderResourceView* Dtx_CreateSRV(ID3D11Device* pDev,
										const uint8_t* pFile, uint32_t nSize,
										DtxInfo* pOut);

// The same, by name, through the REZ file system. Path separators may be '/'
// or '\\' and case does not matter.
ID3D11ShaderResourceView* Dtx_LoadSRV(ID3D11Device* pDev, const char* pszPath,
									  DtxInfo* pOut);

// Header only, by name. Cheap enough to ask about a texture without deciding
// to upload it - which is what the engine-versus-file comparison needs.
bool Dtx_LoadInfo(const char* pszPath, DtxInfo* pOut);

// ---------------------------------------------------------------------------
// The name-keyed cache. THE BRIDGE BETWEEN THE ENGINE AND THE FILES IS NAMES -
// a model on disk is a model filename and two skin filenames and no pointer,
// and that is not a coincidence: a name
// is the only identity a texture has that survives a level reload.
// ---------------------------------------------------------------------------

// Get it, loading on the first ask. The cache owns the view; do not release it.
// Null if the file is missing or refused, and the miss is remembered so a bad
// name costs one lookup rather than one per frame.
ID3D11ShaderResourceView* Dtx_Get(ID3D11Device* pDev, const char* pszPath,
								  DtxInfo* pOut);

// Where this module says things. The cache ceiling is the only event it
// reports on its own, and a flush nobody was told about would look like a
// stall with no cause.
void     Dtx_SetLog(R3D_LogFn pfnLog);
// +StubTexMaxDim: cap the largest texture edge by dropping whole mip levels.
// 0 disables it. The stock game never exceeds 512; an upscale pack does, and
// 2048x2048 uncompressed skins are what exhausts a 32-bit process.
void     Dtx_SetMaxDim(int nMax);
long     Dtx_CappedCount();

void     Dtx_Flush();			// release everything; safe at any time
uint32_t Dtx_CacheCount();		// how many are resident
uint32_t Dtx_LoadedBytes();		// how much GPU memory that is
uint32_t Dtx_MissCount();		// names asked for that no file answered

// Parse every .DTX the archives mount, without uploading any of them, and
// report how many satisfy the size identity. This is the structural check the
// offline sweep does, run against the same files the game will actually see -
// so a mount-order change or a different install cannot silently invalidate it.
// Returns the number that parsed; *pnTotal gets the number examined.
uint32_t Dtx_SelfTest(R3D_LogFn pfnLog, uint32_t* pnTotal);

// How many textures had an all-zero alpha channel rewritten to opaque,
// and how many were refused because the file was too short for the chain.
void Dtx_AlphaStats(long* pnOpaqued, long* pnRefused);

// Treat an all-zero alpha channel as ABSENT rather than as fully
// transparent, rewriting it opaque. +StubZeroAlpha, default 1.
void Dtx_SetZeroAlphaRule(int b);
