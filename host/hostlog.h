// ----------------------------------------------------------------------- //
//
// MODULE  : hostlog.h
//
// PURPOSE : Header-only structured logging for the x64 host tools. Same
//           discipline as the client's VRLog: one line per event, flushed
//           every write, its own directory per run.
//
// ----------------------------------------------------------------------- //

#pragma once

#include <windows.h>
#include <cstdio>
#include <cstdarg>

namespace HostLog
{
	inline FILE*&			File()  { static FILE* f = nullptr; return f; }
	inline char*			Dir()   { static char d[MAX_PATH] = ""; return d; }	// this run's folder
	inline LARGE_INTEGER&	Freq()  { static LARGE_INTEGER v{}; return v; }
	inline LARGE_INTEGER&	Start() { static LARGE_INTEGER v{}; return v; }

	inline double NowMs()
	{
		LARGE_INTEGER n;
		QueryPerformanceCounter(&n);
		return (double)(n.QuadPart - Start().QuadPart) * 1000.0 / (double)Freq().QuadPart;
	}

	// NOTE: timestamps are MILLISECONDS. Reading them as seconds cost two
	// wrong theories during M4 - see docs/M4-TRANSPORT.md.
	inline void Msg(const char* pFmt, ...)
	{
		char buf[1024];
		va_list a;
		va_start(a, pFmt);
		vsnprintf(buf, sizeof(buf), pFmt, a);
		va_end(a);

		printf("[%9.3f ms] %s\n", NowMs(), buf);
		if (File())
		{
			fprintf(File(), "[%9.3f ms] %s\n", NowMs(), buf);
			fflush(File());
		}
	}

	inline void Open(const char* pPrefix)
	{
		QueryPerformanceFrequency(&Freq());
		QueryPerformanceCounter(&Start());

		SYSTEMTIME st;
		GetLocalTime(&st);
		char dir[MAX_PATH], path[MAX_PATH];
		CreateDirectoryA("logs", nullptr);
		sprintf_s(dir, "logs\\%s-%04d%02d%02d-%02d%02d%02d", pPrefix,
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		CreateDirectoryA(dir, nullptr);
		strcpy_s(Dir(), MAX_PATH, dir);
		sprintf_s(path, "%s\\%s.log", dir, pPrefix);
		fopen_s(&File(), path, "w");
		Msg("log: %s", path);
	}

	inline void Close()
	{
		if (File()) { fclose(File()); File() = nullptr; }
	}
}
