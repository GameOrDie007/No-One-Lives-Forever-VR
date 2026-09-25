// spranim - a .spr is a list of .dtx frames and a rate. The world puts them
// on polygons (the HQ waterfall is one), particle systems name them as their
// picture, and polygrids wear them as a surface. This resolves a name to its
// frames and answers "which frame now?".
//
// Indices are 1-based so a zero-initialised holder means "none".
#pragma once
#include <d3d11.h>
#include "dtx.h"

bool SprAnim_IsSpr(const char* pszName);

// 1-based handle, 0 if the file could not be read or had no frames.
// A .dtx name is accepted too and becomes a one-frame animation, so a caller
// with a name of either kind needs only this.
int SprAnim_Get(ID3D11Device* pDev, const char* pszName);

// The frame at time fNow (seconds, any origin). Null if the frame's texture
// refused to load.
ID3D11ShaderResourceView* SprAnim_SRV(int nAnim1, double fNow);

// The first frame's texture info, for the cut-out decision. Null if none.
const DtxInfo* SprAnim_Info(int nAnim1);

// The first frame's .dtx path of a .spr, for anything that needs a picture's
// size before the animation exists. False if the file is missing or empty.
bool SprAnim_FirstFrame(const char* pszSpr, char* pOut, size_t nOut);

// Forget everything: the views belong to the DTX cache and go when it does.
void SprAnim_Flush();

// How many animations are held, and how many frames failed to load.
void SprAnim_Stats(unsigned* pnAnims, unsigned* pnFrames, unsigned* pnMissing);
