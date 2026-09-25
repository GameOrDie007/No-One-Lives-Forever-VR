#include "butes.h"

#include <string.h>
#include <stdlib.h>
#include <string>
#include <map>
#include <set>

namespace
{
	// model (normalised) -> the skins declared for it. A set, because the
	// COUNT is the thing that decides whether the answer can be used.
	std::map<std::string, std::set<std::string> > g_Map;
	// The answer, once a model has exactly one skin. Kept separately so
	// Butes_SkinFor can hand back a stable pointer.
	std::map<std::string, std::string> g_One;
	uint32_t g_nAmbiguous = 0;
	int      g_bLoaded = 0;

	// Lower case, forward slashes, no leading slash. The game spells the same
	// path three different ways in one file - "Guns\Skins_HH\coin_hh.dtx",
	// "guns\skins_hh\coin_hh.dtx" and "Guns\skins_hh\coin_hh.dtx" all appear -
	// and without this those read as three different skins and the model gets
	// refused as ambiguous. Normalising drops the ambiguous count from 9 to 3.
	std::string Norm(const char* p)
	{
		std::string s;
		for (; p && *p; ++p)
		{
			char c = *p;
			if (c == '\\') c = '/';
			if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
			if (s.empty() && c == '/') continue;
			s.push_back(c);
		}
		return s;
	}

	bool EndsWith(const std::string& s, const char* pszSuffix)
	{
		const size_t n = strlen(pszSuffix);
		return s.size() >= n && s.compare(s.size() - n, n, pszSuffix) == 0;
	}

	// One line of a bute file. Returns the prefix before "Model"/"Skin" and the
	// quoted value, e.g. "HHModel = \"Guns\\...\"" -> ("hh", "guns/...").
	//
	// The PREFIX is what pairs them: PVModel with PVSkin, HHModel with HHSkin,
	// InterfaceModel with InterfaceSkin. Pairing by position instead would
	// cross a weapon's player-view skin onto its hand-held model, which is a
	// wrong picture that looks plausible.
	bool Field(const char* p, const char* pszWhat,
			   std::string* pPrefix, std::string* pValue)
	{
		while (*p == ' ' || *p == '\t') ++p;
		const char* pKey = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '=') ++p;
		const size_t nKey = (size_t)(p - pKey);
		const size_t nWhat = strlen(pszWhat);
		if (nKey < nWhat) return false;
		if (_strnicmp(pKey + nKey - nWhat, pszWhat, nWhat) != 0) return false;
		while (*p == ' ' || *p == '\t') ++p;
		if (*p != '=') return false;
		++p;
		while (*p == ' ' || *p == '\t') ++p;
		if (*p != '"') return false;
		++p;
		const char* pV = p;
		while (*p && *p != '"') ++p;
		if (*p != '"') return false;
		*pPrefix = Norm(std::string(pKey, nKey - nWhat).c_str());
		*pValue  = Norm(std::string(pV, (size_t)(p - pV)).c_str());
		return true;
	}

	void ParseOne(const char* pszPath)
	{
		uint32_t nSize = 0;
		uint8_t* pFile = RezFS_Read(pszPath, &nSize);
		if (!pFile) return;

		// prefix -> the model most recently declared under it, within this
		// record. Cleared at every '[' section header, because a skin must not
		// pair with a model from the record above it.
		std::map<std::string, std::string> cur;
		const char* p = (const char*)pFile;
		const char* pEnd = p + nSize;
		std::string line;
		while (p <= pEnd)
		{
			if (p == pEnd || *p == '\n' || *p == '\r')
			{
				if (!line.empty())
				{
					const char* q = line.c_str();
					while (*q == ' ' || *q == '\t') ++q;
					if (*q == '[') cur.clear();
					std::string pre, val;
					if (Field(line.c_str(), "Model", &pre, &val)
						&& EndsWith(val, ".abc"))
						cur[pre] = val;
					else if (Field(line.c_str(), "Skin", &pre, &val)
							 && EndsWith(val, ".dtx"))
					{
						std::map<std::string, std::string>::iterator it
							= cur.find(pre);
						if (it != cur.end())
							g_Map[it->second].insert(val);
					}
					line.clear();
				}
				if (p == pEnd) break;
				++p;
				continue;
			}
			line.push_back(*p);
			++p;
		}
		RezFS_Free(pFile);
	}

	void OnFile(const char* pszPath, uint32_t, void*)
	{
		// ATTRIBUTES/ only. Every .txt in the archives would drag in readmes
		// and level scripts, and a Model/Skin pair found in one of those is not
		// a declaration, it is a coincidence in prose.
		const std::string s = Norm(pszPath);
		if (s.compare(0, 11, "attributes/") != 0) return;
		ParseOne(pszPath);
	}
}

void Butes_Load(R3D_LogFn pfnLog)
{
	if (g_bLoaded) return;
	g_bLoaded = 1;

	RezFS_ForEach(".TXT", OnFile, nullptr);

	for (std::map<std::string, std::set<std::string> >::const_iterator it
			= g_Map.begin(); it != g_Map.end(); ++it)
	{
		if (it->second.size() == 1)
			g_One[it->first] = *it->second.begin();
		else
			++g_nAmbiguous;
	}

	if (pfnLog)
	{
		// THE COUNTS, because a parse that quietly found nothing looks exactly
		// like one that worked. The reference reading over the same archives is
		// 141 models with 3 ambiguous, and every mapped skin present on disk;
		// if these numbers move, the parse changed, not the game.
		uint32_t nMissing = 0;
		for (std::map<std::string, std::string>::const_iterator it = g_One.begin();
			 it != g_One.end(); ++it)
			if (!RezFS_Exists(it->second.c_str())) ++nMissing;
		pfnLog("  BUTES: %u models declare a skin in ATTRIBUTES/*.TXT,"
			   " %u usable, %u refused for declaring several, %u whose skin is"
			   " NOT on disk%s",
			   (unsigned)g_Map.size(), (unsigned)g_One.size(),
			   (unsigned)g_nAmbiguous, nMissing,
			   g_Map.empty() ? "   <- NOTHING PARSED" : "");
	}
}

const char* Butes_SkinFor(const char* pszModel)
{
	if (!pszModel || !*pszModel || g_One.empty()) return nullptr;
	std::map<std::string, std::string>::const_iterator it
		= g_One.find(Norm(pszModel));
	return (it == g_One.end()) ? nullptr : it->second.c_str();
}

uint32_t Butes_Count()     { return (uint32_t)g_One.size(); }
uint32_t Butes_Ambiguous() { return g_nAmbiguous; }
