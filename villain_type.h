#pragma once

#include "util.h"

RICH_ENUM(VillainType,
  MAIN,
  LESSER,
  MINOR,
  ALLY,
  PLAYER,
  NONE,
  RETIRED
);

class TStringId;

extern TStringId getName(VillainType);
extern bool blocksInfluence(VillainType);
// A world-map faction site that the player can attack and wipe out.
// ALLY is included ON PURPOSE. The RAR world is ONE shared map generated from every villain group, so a
// faction that is an ally to one keeper (TEUTONS -> lawful) is an enemy to another (evil) and can absolutely
// be destroyed by him. Leaving ALLY out here meant a conquered ally was never reported defeated (its world-map
// icon stayed alive forever) and its aftermath was never written back. Keep the defeat-report and the
// aftermath-writeback checks on THIS predicate so they can't drift apart again.
extern bool isConquerableSite(VillainType);