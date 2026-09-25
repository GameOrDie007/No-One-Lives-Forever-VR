// spranim - see spranim.h.
//
// THE FILE. GAMESTER/ENGINE.SPR, 156 bytes, read by hand:
//   u32 frames (4)   u32 rate (12)   u32 transparent   u32 translucent   u32 key
//   then per frame: u16 length, that many characters, no terminator.
// The names inside are the game's own paths (TEX\GAMESTER\sprites\ENGINE0.dtx)
// and go to Dtx_Get as they are; it normalises case and slashes.
#include "spranim.h"
#include "rezfs.h"
#include <string>
#include <vector>
#include <string.h>

namespace
{
	struct Anim
	{
		std::string sName;		// as asked, upper-cased, slashes normalised
		std::vector<ID3D11ShaderResourceView*> frames;
		std::vector<std::string> names;
		float fRate;
		DtxInfo info;
		bool bInfo;
		unsigned nMissing;
	};
	std::vector<Anim> g_Anims;
	unsigned g_nMissingTotal = 0;

	std::string Key(const char* p)
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
}

bool SprAnim_IsSpr(const char* pszName)
{
	if (!pszName) return false;
	const size_t n = strlen(pszName);
	return n > 4 && _stricmp(pszName + n - 4, ".spr") == 0;
}

int SprAnim_Get(ID3D11Device* pDev, const char* pszName)
{
	if (!pDev || !pszName || !*pszName) return 0;
	const std::string k = Key(pszName);
	for (size_t i = 0; i < g_Anims.size(); ++i)
		if (g_Anims[i].sName == k) return (int)i + 1;

	Anim a; a.sName = k; a.fRate = 0.0f; a.bInfo = false; a.nMissing = 0;
	if (SprAnim_IsSpr(pszName))
	{
		uint32_t nSize = 0;
		uint8_t* p = RezFS_Read(pszName, &nSize);
		if (p && nSize >= 20)
		{
			uint32_t nFrames = 0, nRate = 0;
			memcpy(&nFrames, p, 4); memcpy(&nRate, p + 4, 4);
			size_t o = 20;
			for (uint32_t f = 0; f < nFrames && f < 256 && o + 2 <= nSize; ++f)
			{
				uint16_t len = 0; memcpy(&len, p + o, 2); o += 2;
				if (o + len > nSize) break;
				a.names.push_back(std::string((const char*)p + o, len));
				o += len;
			}
			a.fRate = (float)nRate;
		}
		if (p) RezFS_Free(p);
	}
	else
	{
		a.names.push_back(pszName);
	}
	for (size_t f = 0; f < a.names.size(); ++f)
	{
		DtxInfo di{};
		ID3D11ShaderResourceView* pSRV = Dtx_Get(pDev, a.names[f].c_str(), &di);
		if (pSRV && !a.bInfo) { a.info = di; a.bInfo = true; }
		if (!pSRV) { ++a.nMissing; ++g_nMissingTotal; }
		a.frames.push_back(pSRV);
	}
	g_Anims.push_back(a);
	return (int)g_Anims.size();
}

ID3D11ShaderResourceView* SprAnim_SRV(int nAnim1, double fNow)
{
	if (nAnim1 <= 0 || (size_t)nAnim1 > g_Anims.size()) return nullptr;
	const Anim& a = g_Anims[nAnim1 - 1];
	if (a.frames.empty()) return nullptr;
	size_t f = 0;
	if (a.frames.size() > 1 && a.fRate > 0.0f)
	{
		const double t = fNow * (double)a.fRate;
		f = (size_t)((long long)t % (long long)a.frames.size());
	}
	return a.frames[f];
}

const DtxInfo* SprAnim_Info(int nAnim1)
{
	if (nAnim1 <= 0 || (size_t)nAnim1 > g_Anims.size()) return nullptr;
	const Anim& a = g_Anims[nAnim1 - 1];
	return a.bInfo ? &a.info : nullptr;
}

bool SprAnim_FirstFrame(const char* pszSpr, char* pOut, size_t nOut)
{
	if (!pszSpr || !pOut || nOut < 2) return false;
	pOut[0] = 0;
	uint32_t nSize = 0;
	uint8_t* p = RezFS_Read(pszSpr, &nSize);
	if (!p) return false;
	bool bOK = false;
	if (nSize >= 22)
	{
		uint32_t nFrames = 0; memcpy(&nFrames, p, 4);
		uint16_t len = 0; memcpy(&len, p + 20, 2);
		if (nFrames && len && 22 + (size_t)len <= nSize && (size_t)len < nOut)
		{
			memcpy(pOut, p + 22, len); pOut[len] = 0; bOK = true;
		}
	}
	RezFS_Free(p);
	return bOK;
}

void SprAnim_Flush()
{
	g_Anims.clear();
	g_nMissingTotal = 0;
}

void SprAnim_Stats(unsigned* pnAnims, unsigned* pnFrames, unsigned* pnMissing)
{
	unsigned nF = 0;
	for (size_t i = 0; i < g_Anims.size(); ++i) nF += (unsigned)g_Anims[i].frames.size();
	if (pnAnims) *pnAnims = (unsigned)g_Anims.size();
	if (pnFrames) *pnFrames = nF;
	if (pnMissing) *pnMissing = g_nMissingTotal;
}
