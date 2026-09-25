// ----------------------------------------------------------------------- //
//
// MODULE  : butes.h
//
// PURPOSE : Which SKIN belongs to which MODEL, read from the game's own
//           attribute files.
//
//           The engine binds a model's skin only when it draws the model
//           through the path we replaced, so an object nobody has walked past
//           yet has both skin slots reading 00000000 - no texture object, no
//           name, nothing for the file route to look up. A De Lisle carbine
//           lying on the floor of T10S01 draws WHITE for exactly that reason,
//           and its skin is sitting on disk the whole time.
//
//           THE GAME DECLARES THE PAIR. ATTRIBUTES/WEAPONS.TXT:
//
//             HHModel = "Guns\Models_HH\delisle_hh.abc"
//             HHSkin  = "Guns\Skins_HH\delisle_hh.dtx"
//
//           So this is not a rule fitted to a filename - it is the game's own
//           statement, read the same way the .dtx, .dat and .abc files are.
//           That distinction matters here: the obvious transform,
//           MODELS_x -> SKINS_x and .abc -> .dtx, HOLDS ON 49% OF THE GAME'S
//           701 MODELS. Half. Measured before it was written, and rejected.
//
//           The model's own filename is at model + 0x04, found by dumping the
//           readable strings around the model struct and letting RezFS_Exists
//           say which were real - the same probe that found the sprite route.
//
// ----------------------------------------------------------------------- //

#ifndef NOLFVR_BUTES_H
#define NOLFVR_BUTES_H

#include <stdint.h>
#include "rezfs.h"

// Parse every ATTRIBUTES/*.TXT the archives mount and build the map. Safe to
// call more than once; the second call does nothing. Logs its own counts, so a
// parse that silently found nothing cannot pass for one that worked.
void Butes_Load(R3D_LogFn pfnLog);

// The skin declared for this model, or null. The argument is a model filename
// as the engine spells it - backslashes and mixed case are fine.
//
// Returns null for a model the attributes do not mention, and ALSO for the
// three that declare more than one skin (lipstick, perfume and the helicopter
// window, which are genuine variants). Refusing an ambiguous answer is the
// point: drawing the wrong lipstick is a worse outcome than drawing the white
// stand-in we draw today, and it would be much harder to notice.
const char* Butes_SkinFor(const char* pszModel);

uint32_t Butes_Count();			// models mapped
uint32_t Butes_Ambiguous();		// models refused for declaring several skins

#endif
