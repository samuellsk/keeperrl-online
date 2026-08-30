#pragma once

#include "cereal/cereal.hpp"
#include "util.h"
#include "creature_id.h"
#include "tribe.h"

// RAR: a named pool of creatures whose CONTENTS depend on the player's keeper tribe.
//
// WHY: a layout token resolved to one fixed creature, so `AlliedPrisoner "ELF_ARCHER"` handed every keeper the
// same ally -- a necromancer freeing an elf archer from his own dungeon's prison. The token has to stay put
// (it is baked into random_layouts and into every mod that uses it), so the indirection goes behind it: the
// token names a GROUP, and the group answers differently per keeper tribe.
//
// Entries are matched in order against the player's tribe. An entry with NO tribe is the catch-all and is
// used when nothing else matches, so adding a keeper tribe never breaks an existing group -- it just falls
// back until somebody writes a pool for it.
struct LayoutGroupEntry {
  optional<TribeId> SERIAL(tribe);       // absent = default, used when no entry matches the player's tribe
  vector<CreatureId> SERIAL(creatures);  // one is picked at random
  SERIALIZE_ALL(NAMED(tribe), NAMED(creatures))
};
