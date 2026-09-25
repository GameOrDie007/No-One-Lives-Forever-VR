// ---------------------------------------------------------------------------
// version.h - the version resource every NOLF1 VR binary carries.
//
// A binary with no version resource is anonymous to Windows and to virus
// scanners, and an anonymous unsigned DLL beside a game reads like something
// that crept in. These say what the file is, whose project it is, and whose
// game it is for. The client DLL carries the same numbers in
// src/nolf1-modernizer/NOLF/ClientShellDLL/VRVersion.rc - change both.
// ---------------------------------------------------------------------------
#pragma once

#define NOLFVR_VER_MAJOR   1
#define NOLFVR_VER_MINOR   0
#define NOLFVR_VER_PATCH   0
#define NOLFVR_VER_BUILD   0
#define NOLFVR_VER_STRING  "1.0.0"

#define NOLFVR_COMPANY     "Game Or Die"
#define NOLFVR_PRODUCT     "NOLF1 VR"
#define NOLFVR_COPYRIGHT   "NOLF1 VR - a free mod. No One Lives Forever (C) 2000 Monolith Productions."
#define NOLFVR_COMMENTS    "A VR mod for No One Lives Forever GOTY 1.004. No game data is included."
