#pragma once

#include "util.h"
#include "tribe.h"

class PrettyInputArchive;

// One tribe's definition as loaded from tribes.txt. The tribe's identity is the map KEY (a TribeId); this
// holds only its relationships. `enemies` is mutual -- listing HUMAN under DARK_KEEPER also makes DARK_KEEPER
// an enemy of HUMAN, exactly like the old hardcoded addEnemies(). `diplomatic` tribes track per-creature
// standing (attacking a member turns that individual hostile); non-diplomatic tribes are statically friend/foe.
struct TribeDef {
  vector<TribeId> SERIAL(enemies);
  bool SERIAL(enemyOfAll) = false;
  bool SERIAL(diplomatic) = true;
  // A player-keeper tribe: its minions dig only where their collective orders them and must never use the
  // "hostile dig" pathing (tunnelling through walls for a shortcut) that ENEMIES use to break into a dungeon.
  // Set it on every keeper tribe, including custom ones (UNDEAD_KEEPER, ...) -- it is what lets an INVADED
  // rival keeper's minions be recognised, since they are on their own tribe, not the local player's.
  //
  // DELIBERATELY NOT SERIAL. TribeDef is serialized inside ContentFactory, i.e. into every save, and OPTION()
  // is only optional to the CONFIG reader -- to the binary archive every field is mandatory, so adding one to
  // the binary layout silently breaks every existing save (that already happened once). This field is read
  // from tribes.txt ONLY, via the PrettyInputArchive specialization below, and is repopulated on every content
  // load. The binary layout stays at the three SERIAL fields above -- keep it that way.
  bool keeperTribe = false;
  // Exceptions to `enemyOfAll` / `enemies`: these tribes stay FRIENDLY. Applied after all hostility is wired,
  // so it is the way to say "hostile to everything except my patron" -- e.g. an ANCIENT_TOMB guardian that
  // fights the world but serves the TOMB keeper. Mutual, like `enemies`: listing TOMB here is enough, the
  // other tribe needs no edit.
  //
  // DELIBERATELY NOT SERIAL, same as keeperTribe -- TribeDef is serialized into every save and the binary
  // layout must stay at the three SERIAL fields. Read from tribes.txt only.
  vector<TribeId> allies;
  // The mirror of `enemyOfAll`: friendly with EVERY tribe, including ones added later, and `enemies` is the
  // exception list. Applied in the same last pass as `allies`, so it also overrides other tribes' enemyOfAll --
  // which is the whole point (a tribe nobody fights, e.g. PEACEFUL, or the sokoban boulder's tribe).
  //   enemyOfAll = true  + allies  = {...}   -> hostile to all EXCEPT these
  //   allyToAll  = true  + enemies = {...}   -> friendly to all EXCEPT these
  // The exception is MUTUAL, exactly like `enemies` already is: a tribe that names me in ITS enemies is
  // excluded too, so authored hostility from either side always survives.
  //
  // DELIBERATELY NOT SERIAL, same as keeperTribe/allies -- TribeDef is serialized into every save and the
  // binary layout must stay at the three SERIAL fields. Read from tribes.txt only.
  bool allyToAll = false;

  template <class Archive>
  void serialize(Archive& ar, const unsigned int version);
};

// Content (tribes.txt) parse: same three fields PLUS keeperTribe. Declared here so every translation unit that
// parses a TribeDef picks the specialization instead of instantiating the binary-layout generic above.
template <>
void TribeDef::serialize(PrettyInputArchive& ar, const unsigned int version);
