// dtx - see dtx.h for the format and why the files are the answer.

#include "dtx.h"
#include "rezfs.h"

#include <string>
#include <vector>
#include <string.h>
#include <math.h>			// fabs, for the transparent-region spread

namespace
{
	const uint32_t kHeader = 164;

	// The file's own format identifiers. 1 (BPP_8) and 2 (BPP_16) do not occur
	// in anything the game mounts, so they are refused rather than guessed at:
	// the size identity cannot tell a 32-bit file from a 16-bit one that
	// happens to have twice the mips, and a wrong guess here is a garbage
	// upload, not a missing texture.
	enum { kFmt8P = 0, kFmt8 = 1, kFmt16 = 2, kFmt32 = 3,
		   kFmtDXT1 = 4, kFmtDXT3 = 5, kFmtDXT5 = 6, kFmt32P = 7 };

	// FORMAT 7 IS THE ONE FILE THE SIZE IDENTITY EVER FAILED ON, and it was
	// never a malformed file - it was a format nothing here knew about.
	//
	// TEX/GLASS/GL01/GL0007.DTX ships TWICE. NOLF.rez has an ordinary 32-bit
	// four-mip copy of 87204 bytes; NolfGoty.rez replaces it with a 22724-byte
	// one, and GOTY mounts later so GOTY's copy is the one the game uses. That
	// copy declares sections = 1, stores ONE INDEX PER TEXEL, and keeps its
	// colours in a trailing section named "PALLETE32" - the game's own
	// spelling - whose last 1024 bytes are 256 BGRA entries, every one at
	// alpha 0x80. Half-alpha glass, which is exactly what M04S01 asks for.
	//
	// So the identity was never "164 + mips == size". It is "164 + mips +
	// sections == size", and it held on 4173 files only because 4173 files
	// have no sections. With this it holds on 4174 of 4174.
	//
	// Expanded to BGRA at load, so nothing downstream - Measure, the pitch,
	// the all-zero-alpha rule - has to learn a second pixel layout.
	const uint32_t kPaletteBytes = 256 * 4;
	// The palette is taken from the END of the file. The 22 bytes between the
	// section's name and it are not decoded: one sample cannot settle a
	// section header, and guessing at one is how this project has lost time
	// before. Taking it from the end needs no such guess, and the arithmetic
	// still has to land on the file's last byte to be believed.
	bool FindPalette(const uint8_t* pFile, uint32_t nSize,
					 uint32_t nMipBytes, const uint8_t** ppPal)
	{
		if (nSize < kHeader + nMipBytes + kPaletteBytes) return false;
		const uint8_t* pTail = pFile + kHeader + nMipBytes;
		const uint32_t nTail = nSize - kHeader - nMipBytes;
		// The section must actually be there and be the one we think it is.
		bool bNamed = false;
		for (uint32_t i = 0; i + 9 <= nTail; ++i)
			if (memcmp(pTail + i, "PALLETE32", 9) == 0) { bNamed = true; break; }
		if (!bNamed) return false;
		*ppPal = pFile + nSize - kPaletteBytes;
		return true;
	}

	uint32_t BlockBytes(uint32_t nFormat)
	{
		if (nFormat == kFmtDXT1) return 8;
		if (nFormat == kFmtDXT3 || nFormat == kFmtDXT5) return 16;
		return 0;					// not block compressed
	}

	// How often the all-zero-alpha rule fired, and how often it was
	// refused for want of a file long enough to hold the mip chain.
	// +StubZeroAlpha 0 turns the rule off, because a rule that REWRITES
	// PIXELS on 75 of 471 world textures has to be answerable to an A/B
	// against retail, not to an argument about what an author meant.
	int  g_bZeroAlphaRule = 1;
	long g_nAlphaOpaqued = 0;
	long g_nAlphaRefused = 0;
	// +StubTexMaxDim: the largest texture edge we will hand the GPU. 0 = no
	// cap, which is right for the stock game - its textures top out at 512.
	// See the mip-drop below for why an upscale pack needs one.
	int  g_nTexMaxDim = 0;
	long g_nTexCapped = 0;

	uint32_t MipBytes(uint32_t w, uint32_t h, uint32_t nFormat)
	{
		const uint32_t nb = BlockBytes(nFormat);
		if (nb)
		{
			const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
			return (bw ? bw : 1) * (bh ? bh : 1) * nb;
		}
		if (nFormat == kFmt32P) return w * h;	// one INDEX per texel
		return w * h * 4;			// 8P and 32 are both four bytes per texel
	}

	uint32_t MipPitch(uint32_t w, uint32_t nFormat)
	{
		const uint32_t nb = BlockBytes(nFormat);
		if (nb) { const uint32_t bw = (w + 3) / 4; return (bw ? bw : 1) * nb; }
		return w * 4;
	}

	DXGI_FORMAT DxgiFor(uint32_t nFormat)
	{
		switch (nFormat)
		{
		case kFmt8P:				// palettised on the engine's side only
		case kFmt32P:				// expanded through its palette at load
		case kFmt32:   return DXGI_FORMAT_B8G8R8A8_UNORM;
		case kFmtDXT1: return DXGI_FORMAT_BC1_UNORM;
		case kFmtDXT3: return DXGI_FORMAT_BC2_UNORM;
		case kFmtDXT5: return DXGI_FORMAT_BC3_UNORM;
		default:       return DXGI_FORMAT_UNKNOWN;
		}
	}

	// Mean colour and transparency of mip 0, sampled exactly as
	// R3D_NoteTexture samples the engine's copy - every fourth texel in both
	// axes for BGRA, both endpoints of every block for the compressed
	// formats. The point of these numbers is that they are COMPARABLE: the
	// file and the heap are two independent routes to the same picture, so
	// when both are in hand they must agree, and a channel swap or a wrong
	// stride shows up as a disagreement rather than as a wrong-looking wall
	// somebody has to notice in a headset.
	void Measure(const uint8_t* p, uint32_t w, uint32_t h, uint32_t nFormat,
				 DtxInfo* pOut)
	{
		double sr = 0, sg = 0, sb = 0;
		long ns = 0, nClear = 0, nPop = 0, nA0 = 0, nA255 = 0;
		// The transparent region's own colour, for the spread. Two passes
		// over the samples: the mean first, then the deviation from it.
		long nT = 0; double tr = 0.0, tg = 0.0, tb = 0.0;
		const uint32_t nb = BlockBytes(nFormat);
		if (!nb)
		{
			const uint32_t nPitch = w * 4;
			for (uint32_t y = 0; y < h; y += 4)
				for (uint32_t x = 0; x < w; x += 4)
				{
					const uint8_t* q = p + (size_t)y * nPitch + (size_t)x * 4;
					sb += q[0]; sg += q[1]; sr += q[2]; ++ns;
					if (q[3] < 128) ++nClear;
					if (q[3] == 0) ++nA0; else if (q[3] == 255) ++nA255;
					if (q[3] < 8)
					{ tb += q[0]; tg += q[1]; tr += q[2]; ++nT; }
				}
			nPop = ns;
		}
		else
		{
			// BC1 opens each block with its two RGB565 endpoints; BC2 and BC3
			// spend the first eight bytes on alpha and put the same colour
			// block at +8.
			const uint32_t nCol = (nFormat == kFmtDXT1) ? 0 : 8;
			const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
			const uint32_t nBlocks = (bw ? bw : 1) * (bh ? bh : 1);
			for (uint32_t i = 0; i < nBlocks; ++i)
			{
				const uint8_t* blk = p + (size_t)i * nb;
				const uint16_t c0 = *(const uint16_t*)(blk + nCol);
				const uint16_t c1 = *(const uint16_t*)(blk + nCol + 2);
				if (nFormat == kFmtDXT1)
				{
					// Punch-through only when the block both declares it
					// (c0 <= c1) and actually uses index 3. A flat block
					// encodes c0 == c1 and is fully opaque; counting those
					// called 229 opaque walls cut-outs the first time.
					if (c0 <= c1)
					{
						const uint32_t ix = *(const uint32_t*)(blk + 4);
						for (int t = 0; t < 16; ++t)
							if (((ix >> (t * 2)) & 3u) == 3u) { ++nClear; break; }
					}
				}
				else if (nFormat == kFmtDXT3)
				{
					for (int t = 0; t < 16; ++t)
					{
						const uint8_t a = (t & 1) ? (blk[t >> 1] >> 4)
												  : (blk[t >> 1] & 0x0F);
						if (a < 8) { ++nClear; break; }
					}
				}
				else	// DXT5: two endpoints and three-bit selectors
				{
					if (blk[0] < 128 || blk[1] < 128) ++nClear;
				}
				for (int k = 0; k < 2; ++k)
				{
					const uint16_t c = *(const uint16_t*)(blk + nCol + k * 2);
					sr += ((c >> 11) & 0x1F) * 255 / 31;
					sg += ((c >> 5)  & 0x3F) * 255 / 63;
					sb += ( c        & 0x1F) * 255 / 31;
					++ns;
				}
			}
			nPop = (long)nBlocks;
		}
		if (ns) { pOut->mr = (float)(sr / ns); pOut->mg = (float)(sg / ns);
				  pOut->mb = (float)(sb / ns); }
		pOut->fCut = nPop ? (float)nClear / (float)nPop : 0.0f;
		pOut->fBimodal = (nb || !ns) ? 1.0f
						 : (float)(nA0 + nA255) / (float)ns;

		// THE SPREAD OF THE TRANSPARENT REGION. See dtx.h: a flat one means the
		// artist cut something out, a varied one means the alpha channel was
		// never authored and the picture underneath is the surface.
		pOut->fClearSpread = 0.0f;
		if (!nb && nT > 0)
		{
			const double mr2 = tr / nT, mg2 = tg / nT, mb2 = tb / nT;
			double dev = 0.0;
			const uint32_t nPitch2 = w * 4;
			for (uint32_t y = 0; y < h; y += 4)
				for (uint32_t x = 0; x < w; x += 4)
				{
					const uint8_t* q = p + (size_t)y * nPitch2 + (size_t)x * 4;
					if (q[3] >= 8) continue;
					dev += fabs((double)q[2] - mr2) + fabs((double)q[1] - mg2)
						 + fabs((double)q[0] - mb2);
				}
			pOut->fClearSpread = (float)(dev / (3.0 * (double)nT));
		}
	}

	std::string Norm(const char* p)
	{
		std::string s;
		if (!p) return s;
		while (*p == '/' || *p == '\\') ++p;
		for (; *p; ++p)
		{
			char c = (*p == '\\') ? '/' : *p;
			if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
			s.push_back(c);
		}
		return s;
	}

	struct CacheEntry
	{
		std::string					sName;
		ID3D11ShaderResourceView*	pSRV;		// null means a remembered miss
		DtxInfo						info;
	};
	std::vector<CacheEntry> g_Cache;
	uint32_t g_nBytes = 0;
	uint32_t g_nMiss  = 0;

	// A CEILING, because nothing else ever removes an entry. The cache is keyed
	// by name and a name always means the same file, so an entry stays valid
	// forever - which is the whole point, and also means a long session that
	// walks through several levels would accumulate every texture in the game.
	// The mounted archives hold 287 MB of pixels in total, so this can only be
	// reached by playing far enough to have seen most of them, and dropping the
	// lot costs one reload of what the current level actually uses.
	const uint32_t kCacheCeiling = 192u * 1024u * 1024u;
	R3D_LogFn g_pfnLog = nullptr;
}

bool Dtx_Describe(const uint8_t* pFile, uint32_t nSize, DtxInfo* pOut)
{
	if (!pFile || !pOut || nSize <= kHeader) return false;
	memset(pOut, 0, sizeof(*pOut));

	const uint32_t nResType = *(const uint32_t*)(pFile + 0x00);
	const int32_t  nVersion = *(const int32_t*)(pFile + 0x04);
	const uint32_t w        = *(const uint16_t*)(pFile + 0x08);
	const uint32_t h        = *(const uint16_t*)(pFile + 0x0A);
	const uint32_t nMips    = *(const uint16_t*)(pFile + 0x0C);
	const uint32_t nSect    = *(const uint16_t*)(pFile + 0x0E);
	const uint32_t nFormat  = pFile[0x18 + 2];

	if (nResType != 0 || nVersion != -5) return false;
	if (!w || !h || w > 8192 || h > 8192) return false;
	{
		const uint32_t nOff = pFile[0x18 + 3];
		const bool bS3tc = (nFormat == 4 || nFormat == 5 || nFormat == 6);
		pOut->nUVShift = (!bS3tc && nOff > 0 && nOff < 4 && (w >> nOff) && (h >> nOff)) ? nOff : 0;
	}
	if (!nMips || nMips > 16) return false;
	// ONE FILE MOUNTED HAS A SECTION, and this line used to say none did.
	// TEX/GLASS/GL01/GL0007.DTX, format 7, and its section is the palette its
	// pixels are indices into - see the note at kFmt32P. Everything else is
	// still refused: a section this reader cannot name is a part of the file
	// it cannot account for, and the size identity below would be asserting
	// over a length it does not understand.
	if (nSect && nFormat != kFmt32P) return false;
	if (DxgiFor(nFormat) == DXGI_FORMAT_UNKNOWN) return false;

	// THE SIZE IDENTITY. The header is a fixed 164 bytes, the mip chain
	// follows it with no padding, and nothing follows the mip chain - so this
	// sum either lands on the last byte of the file or the parse is wrong.
	// It is the only check here that could not be satisfied by a file which
	// merely begins convincingly.
	uint32_t nBytes = 0;
	for (uint32_t i = 0; i < nMips; ++i)
	{
		const uint32_t mw = (w >> i) ? (w >> i) : 1;
		const uint32_t mh = (h >> i) ? (h >> i) : 1;
		nBytes += MipBytes(mw, mh, nFormat);
	}
	// 164 + mips + SECTIONS. The sections term is zero on 4173 of the 4174
	// files the game mounts, which is why it went unnoticed for so long - and
	// non-zero on the one file that always failed.
	uint32_t nSections = 0;
	if (nFormat == kFmt32P)
	{
		const uint8_t* pPal = nullptr;
		if (!FindPalette(pFile, nSize, nBytes, &pPal)) return false;
		nSections = nSize - kHeader - nBytes;
	}
	if (kHeader + nBytes + nSections != nSize) return false;

	pOut->nWidth = w; pOut->nHeight = h; pOut->nMips = nMips;
	pOut->nFormat = nFormat; pOut->fmt = DxgiFor(nFormat); pOut->nBytes = nBytes;
	pOut->nFlags = *(const uint32_t*)(pFile + 0x10);

	// THE COMMAND STRING, at +0x24 and 128 bytes long. It holds a
	// semicolon-separated list the tools wrote - "Detailtex Tex\...\Dtl0006.dtx"
	// on a wall, "alpharef 96;" on a tree. The second is the whole answer to
	// which textures are cut-outs, and it needed no statistics at all.
	pOut->fAlphaRef = 0.0f;
	{
		char szCmd[129];
		memcpy(szCmd, pFile + 0x24, 128);
		szCmd[128] = 0;
		for (char* q = szCmd; *q; ++q)
			if (*q >= 'A' && *q <= 'Z') *q = (char)(*q - 'A' + 'a');
		pOut->szEnvMap[0] = 0;
		{
			// The case was folded above; the archive lookup does not mind.
			const char* pE = strstr(szCmd, "envmap");
			if (pE)
			{
				pE += 6;
				while (*pE == ' ' || *pE == '\t' || *pE == '=') ++pE;
				int k = 0;
				while (*pE && *pE != ';' && *pE != ' ' && k < 63) pOut->szEnvMap[k++] = *pE++;
				pOut->szEnvMap[k] = 0;
			}
		}
		const char* pA = strstr(szCmd, "alpharef");
		if (pA)
		{
			pA += 8;
			while (*pA == ' ' || *pA == '\t' || *pA == '=') ++pA;
			int nRef = 0, nDig = 0;
			while (*pA >= '0' && *pA <= '9' && nDig < 4)
			{ nRef = nRef * 10 + (*pA++ - '0'); ++nDig; }
			// A declared 0 means "test against nothing", which is no test at
			// all - and it is also what an unparsable string leaves behind,
			// so the two are treated the same on purpose.
			if (nDig && nRef > 0 && nRef <= 255) pOut->fAlphaRef = (float)nRef;
		}
	}
	return true;
}

ID3D11ShaderResourceView* Dtx_CreateSRV(ID3D11Device* pDev,
										const uint8_t* pFile, uint32_t nSize,
										DtxInfo* pOut)
{
	DtxInfo info;
	if (!pDev || !Dtx_Describe(pFile, nSize, &info)) return nullptr;

	// FORMAT 7 IS EXPANDED HERE AND NOWHERE ELSE. One index per texel through
	// a 256-entry BGRA palette, into an ordinary 32-bit chain - so Measure,
	// MipPitch and the all-zero-alpha rule below all keep working on the one
	// pixel layout they already understand, and no other function in this file
	// has to know format 7 exists. See the note at kFmt32P.
	std::vector<uint8_t> expanded;
	const uint8_t* pPixels = pFile + kHeader;
	if (info.nFormat == kFmt32P)
	{
		const uint8_t* pPal = nullptr;
		if (!FindPalette(pFile, nSize, info.nBytes, &pPal)) return nullptr;
		uint32_t nOut = 0;
		for (uint32_t i = 0; i < info.nMips; ++i)
		{
			const uint32_t mw = (info.nWidth >> i) ? (info.nWidth >> i) : 1;
			const uint32_t mh = (info.nHeight >> i) ? (info.nHeight >> i) : 1;
			nOut += mw * mh * 4;
		}
		expanded.resize(nOut);
		const uint8_t* pSrc = pFile + kHeader;
		uint8_t* pDst = &expanded[0];
		for (uint32_t i = 0; i < info.nMips; ++i)
		{
			const uint32_t mw = (info.nWidth >> i) ? (info.nWidth >> i) : 1;
			const uint32_t mh = (info.nHeight >> i) ? (info.nHeight >> i) : 1;
			const uint32_t n = mw * mh;
			for (uint32_t k = 0; k < n; ++k)
			{
				const uint8_t* e = pPal + (size_t)pSrc[k] * 4;
				pDst[k * 4 + 0] = e[0];
				pDst[k * 4 + 1] = e[1];
				pDst[k * 4 + 2] = e[2];
				pDst[k * 4 + 3] = e[3];
			}
			pSrc += n;
			pDst += (size_t)n * 4;
		}
		pPixels = &expanded[0];
		// From here on it IS a 32-bit texture, and saying so is what keeps
		// every path below from needing a special case.
		info.nFormat = kFmt32;
		info.nBytes = nOut;
	}

	D3D11_SUBRESOURCE_DATA sd[16] = {};
	const uint8_t* p = pPixels;
	for (uint32_t i = 0; i < info.nMips; ++i)
	{
		const uint32_t mw = (info.nWidth >> i) ? (info.nWidth >> i) : 1;
		const uint32_t mh = (info.nHeight >> i) ? (info.nHeight >> i) : 1;
		sd[i].pSysMem = p;
		sd[i].SysMemPitch = MipPitch(mw, info.nFormat);
		p += MipBytes(mw, mh, info.nFormat);
	}
	Measure(pPixels, info.nWidth, info.nHeight, info.nFormat, &info);

	// A TEXTURE WHOSE ALPHA IS ENTIRELY ZERO HAS NO ALPHA - IT IS NOT
	// INVISIBLE. MENU\SPRTEX\FOLDERBACK1.DTX is the green the whole main
	// menu sits on: 32x32, mean RGB 171,227,129, and every one of its
	// 1024 texels has alpha 0. Drawn in the blended pass that is nothing
	// at all, which is why the menu was black behind Cate.
	//
	// An author does not ship art that cannot be seen, so an all-zero
	// alpha channel is ABSENT DATA, not a request for transparency. The
	// test is the whole channel, not a sample: 'mostly zero' is a real
	// cut-out (NOLF.DTX is 8.4% clear and means it) and must not be
	// touched. Only the degenerate all-zero case is rewritten.
	//
	// Compressed formats are left alone. DXT1 carries its transparency
	// in the block encoding rather than a channel, so 'every byte of the
	// alpha channel' is not a question that can be asked of them.
	std::vector<uint8_t> opaque;
	if (g_bZeroAlphaRule && BlockBytes(info.nFormat) == 0 && info.nMips)
	{
		// pPixels, NOT the file: a format-7 texture has been expanded through
		// its palette by now, and asking "is every fourth byte zero" of the
		// raw INDEX data would be asking it of a different picture.
		const uint8_t* q0 = pPixels;
		const size_t n0 = (size_t)MipBytes(info.nWidth, info.nHeight,
					  info.nFormat);
		bool bAllZero = (n0 >= 4);
		for (size_t k = 3; k < n0; k += 4)
			if (q0[k]) { bAllZero = false; break; }
		info.bZeroAlpha = bAllZero ? 1u : 0u;
		if (bAllZero)
		{
			// Every mip, not only mip 0: a chain whose top level is
			// opaque and whose lower levels are clear would fade out
			// with distance, which is a bug that only shows up far away.
			size_t nTotal = 0;
			for (uint32_t i = 0; i < info.nMips; ++i)
			{
				const uint32_t mw = (info.nWidth >> i) ? (info.nWidth >> i) : 1;
				const uint32_t mh = (info.nHeight >> i) ? (info.nHeight >> i) : 1;
				nTotal += MipBytes(mw, mh, info.nFormat);
			}
			// THE FILE MUST ACTUALLY HOLD THE CHAIN. nMips comes out of the
			// header and nTotal is computed from it, so a header that
			// disagrees with the file - or a mip count this loader rounds
			// differently from the tool that wrote it - reads past the end
			// and hands the GPU whatever followed the file in memory. That
			// is a wrong picture, and a wrong picture on a wall is exactly
			// the failure this whole path exists to end.
			// The expanded buffer is exactly nTotal bytes by construction, so
			// the length test only has to be made against the FILE when the
			// pixels are still the file's own.
			const bool bShort = expanded.empty()
				? ((size_t)kHeader + nTotal > (size_t)nSize)
				: (nTotal > expanded.size());
			if (bShort) { ++g_nAlphaRefused; }
			else {
			++g_nAlphaOpaqued;
			opaque.assign(pPixels, pPixels + nTotal);
			for (size_t k = 3; k < opaque.size(); k += 4) opaque[k] = 255;
			const uint8_t* r = opaque.data();
			for (uint32_t i = 0; i < info.nMips; ++i)
			{
				const uint32_t mw = (info.nWidth >> i) ? (info.nWidth >> i) : 1;
				const uint32_t mh = (info.nHeight >> i) ? (info.nHeight >> i) : 1;
				sd[i].pSysMem = r;
				r += MipBytes(mw, mh, info.nFormat);
			}
			// And it is no longer a cut-out, because it no longer has a
			// texel that could be cut. Leaving fCut at 1.0 would hand the
			// alpha test a threshold to discard every pixel with.
			info.fCut = 0.0f;
			info.fAlphaRef = 0.0f;
			}
		}
	}

	// ---- A CEILING ON TEXTURE SIZE, BY DROPPING WHOLE MIP LEVELS ---------
	//
	// The ESRGAN upscale pack ships character skins at 2048x2048, uncompressed
	// 32-bit - about 22 MB each with mips - in a 32-bit process. Bisected with
	// tools/rez-write.py: the pack's CHARS folder crashes, and NEITHER HALF OF
	// IT DOES.
	//
	//   CHARS whole        196 files  1.6 GB   crash
	//   CHARS minus 2048s  174 files  1.1 GB   clean
	//   CHARS 2048s only    22 files  0.5 GB   clean
	//
	// Neither half alone, both together - that is a cumulative limit, not a
	// bad file, and it is why the crash is in the display driver rather than
	// anywhere we can see.
	//
	// A DTX already carries its mip chain, so the cheapest possible cap is to
	// start at a smaller mip and tell D3D that IS the texture: 2048 -> 1024
	// costs a quarter of the memory and is still four times the original 512.
	// No resampling, no quality decision, nothing to get wrong.
	//
	// +StubTexMaxDim 0 (the default) leaves everything alone; the stock game's
	// textures top out at 512 and never reach any cap worth setting.
	uint32_t nSkip = 0;
	if (g_nTexMaxDim > 0)
	{
		while ((info.nWidth >> nSkip) > (uint32_t)g_nTexMaxDim
			   || (info.nHeight >> nSkip) > (uint32_t)g_nTexMaxDim)
		{
			if (nSkip + 1 >= info.nMips) break;	// no smaller level exists
			++nSkip;
		}
		if (nSkip)
		{
			++g_nTexCapped;
			static long s_nSaidCap = 0;
			if (s_nSaidCap < 8 && g_pfnLog)
			{
				++s_nSaidCap;
				g_pfnLog("  DTX: %ux%u capped to %ux%u (dropped %u mip%s)",
						 info.nWidth, info.nHeight,
						 info.nWidth >> nSkip, info.nHeight >> nSkip,
						 nSkip, (nSkip == 1) ? "" : "s");
			}
			info.nWidth  = (info.nWidth  >> nSkip) ? (info.nWidth  >> nSkip) : 1;
			info.nHeight = (info.nHeight >> nSkip) ? (info.nHeight >> nSkip) : 1;
			info.nMips  -= nSkip;
		}
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = info.nWidth; td.Height = info.nHeight;
	td.MipLevels = info.nMips; td.ArraySize = 1;
	td.Format = info.fmt; td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	ID3D11Texture2D* pT = nullptr;
	// The subresource array starts at the mip we are now calling level 0.
	if (FAILED(pDev->CreateTexture2D(&td, sd + nSkip, &pT)) || !pT)
		return nullptr;
	ID3D11ShaderResourceView* pSRV = nullptr;
	const HRESULT hr = pDev->CreateShaderResourceView(pT, nullptr, &pSRV);
	pT->Release();
	if (FAILED(hr)) return nullptr;
	if (pOut) *pOut = info;
	return pSRV;
}

bool Dtx_LoadInfo(const char* pszPath, DtxInfo* pOut)
{
	uint32_t nSize = 0;
	uint8_t* pFile = RezFS_Read(pszPath, &nSize);
	if (!pFile) return false;
	const bool bOK = Dtx_Describe(pFile, nSize, pOut);
	// Format 7's pixels are indices until Dtx_CreateSRV expands them, and
	// Measure only understands BGRA and the block formats. Describing it is
	// still right - the caller learns the size, the format and the alpha ref -
	// but measuring the mean colour of a palette INDEX would be a number that
	// looks like a colour and is not one, which is worse than no number.
	if (bOK && pOut->nFormat != kFmt32P)
		Measure(pFile + kHeader, pOut->nWidth, pOut->nHeight,
				pOut->nFormat, pOut);
	RezFS_Free(pFile);
	return bOK;
}

ID3D11ShaderResourceView* Dtx_LoadSRV(ID3D11Device* pDev, const char* pszPath,
									  DtxInfo* pOut)
{
	uint32_t nSize = 0;
	uint8_t* pFile = RezFS_Read(pszPath, &nSize);
	if (!pFile) return nullptr;
	ID3D11ShaderResourceView* pSRV = Dtx_CreateSRV(pDev, pFile, nSize, pOut);
	RezFS_Free(pFile);
	return pSRV;
}

ID3D11ShaderResourceView* Dtx_Get(ID3D11Device* pDev, const char* pszPath,
								  DtxInfo* pOut)
{
	if (!pDev || !pszPath || !*pszPath) return nullptr;
	const std::string sKey = Norm(pszPath);
	for (size_t i = 0; i < g_Cache.size(); ++i)
		if (g_Cache[i].sName == sKey)
		{
			if (pOut) *pOut = g_Cache[i].info;
			return g_Cache[i].pSRV;
		}

	// NEVER FLUSHED FROM HERE. This used to drop the whole cache the moment
	// the ceiling was crossed - from inside a lookup, mid-frame, in the middle
	// of the world batch resolve loop - and the world batches hold these views
	// WITHOUT a reference. Every batch resolved before the drop kept a freed
	// view; the driver's worker thread faulted on it two frames later
	// (nvwgf2um.dll, reading 0x0000000C) on the first frame of Morocco, the
	// SECOND level of the headset session on 9 September. No sweep could see it:
	// a fresh process never crosses 192 MB. The cache is flushed at world
	// load instead (R3D_WorldLoaded), when nothing holds a view.
	if (g_nBytes >= kCacheCeiling && g_pfnLog)
	{
		static uint32_t s_nSaid = 0;
		if (s_nSaid++ < 3)
			g_pfnLog("  DTX: cache is %u MB over %u textures - past the %u MB ceiling; it is"
					 " flushed at the next world load, not now",
					 g_nBytes / 1048576u, (uint32_t)g_Cache.size(), kCacheCeiling / 1048576u);
	}

	CacheEntry e;
	e.sName = sKey;
	memset(&e.info, 0, sizeof(e.info));
	e.pSRV = Dtx_LoadSRV(pDev, sKey.c_str(), &e.info);
	// A miss is CACHED. Without this a name the archives do not carry costs a
	// whole file-system lookup on every frame that asks for it, which is the
	// shape of a per-frame cost that does not show up until a player reports the
	// frame rate dipping.
	if (!e.pSRV) ++g_nMiss; else g_nBytes += e.info.nBytes;
	g_Cache.push_back(e);
	if (pOut) *pOut = e.info;
	return e.pSRV;
}

void Dtx_Flush()
{
	for (size_t i = 0; i < g_Cache.size(); ++i)
		if (g_Cache[i].pSRV) g_Cache[i].pSRV->Release();
	g_Cache.clear();
	g_nBytes = 0;
	g_nMiss = 0;
}

uint32_t Dtx_CacheCount()  { return (uint32_t)g_Cache.size(); }
uint32_t Dtx_LoadedBytes() { return g_nBytes; }
uint32_t Dtx_MissCount()   { return g_nMiss; }

namespace
{
	struct SweepState
	{
		uint32_t nSeen, nOK, nBytes;
		uint32_t aFormat[8];
		char     szFirstBad[128];
		uint32_t nBadSize, nBadHeader;
	};

	void SweepOne(const char* pszPath, uint32_t /*nSize*/, void* pUser)
	{
		SweepState* s = (SweepState*)pUser;
		++s->nSeen;
		uint32_t n = 0;
		uint8_t* p = RezFS_Read(pszPath, &n);
		if (!p) return;
		DtxInfo info;
		if (Dtx_Describe(p, n, &info))
		{
			++s->nOK;
			s->nBytes += info.nBytes;
			if (info.nFormat < 8) ++s->aFormat[info.nFormat];
		}
		else
		{
			// Which KIND of failure, because "it did not parse" is not a
			// finding. A bad magic means the file is not a texture; a size
			// that misses means the arithmetic is wrong and every texture is
			// suspect.
			const bool bHdr = (n <= kHeader
							   || *(const uint32_t*)p != 0
							   || *(const int32_t*)(p + 4) != -5);
			if (bHdr) ++s->nBadHeader; else ++s->nBadSize;
			if (!s->szFirstBad[0])
			{
				strncpy(s->szFirstBad, pszPath, sizeof(s->szFirstBad) - 1);
				s->szFirstBad[sizeof(s->szFirstBad) - 1] = 0;
			}
		}
		RezFS_Free(p);
	}
}

void Dtx_SetLog(R3D_LogFn pfnLog) { g_pfnLog = pfnLog; }

void Dtx_SetMaxDim(int nMax) { g_nTexMaxDim = nMax; }
long Dtx_CappedCount()       { return g_nTexCapped; }

uint32_t Dtx_SelfTest(R3D_LogFn pfnLog, uint32_t* pnTotal)
{
	g_pfnLog = pfnLog;
	SweepState s;
	memset(&s, 0, sizeof(s));
	RezFS_ForEach(".DTX", SweepOne, &s);
	if (pnTotal) *pnTotal = s.nSeen;
	if (pfnLog)
	{
		pfnLog("  DTX SELFTEST: %u of %u parse - %u DXT1, %u DXT3, %u DXT5,"
			   " %u 32-bit, %u palettised, %.1f MB of pixels",
			   s.nOK, s.nSeen, s.aFormat[kFmtDXT1], s.aFormat[kFmtDXT3],
			   s.aFormat[kFmtDXT5], s.aFormat[kFmt32], s.aFormat[kFmt8P],
			   s.nBytes / 1048576.0);
		if (s.nOK != s.nSeen)
			pfnLog("  DTX SELFTEST: %u refused on the header, %u on the SIZE"
				   " IDENTITY (first: %s)%s",
				   s.nBadHeader, s.nBadSize, s.szFirstBad,
				   (s.nBadSize > 1) ? "  <- THE PARSE IS WRONG" : "");
	}
	return s.nOK;
}

// The all-zero-alpha rule's blast radius, as two numbers.
void Dtx_AlphaStats(long* pnOpaqued, long* pnRefused)
{
	if (pnOpaqued) *pnOpaqued = g_nAlphaOpaqued;
	if (pnRefused) *pnRefused = g_nAlphaRefused;
}

void Dtx_SetZeroAlphaRule(int b) { g_bZeroAlphaRule = b; }
