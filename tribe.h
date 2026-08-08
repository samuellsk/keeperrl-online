/* Copyright (C) 2013-2014 Michal Brzozowski (rusolis@poczta.fm)

   This file is part of KeeperRL.

   KeeperRL is free software; you can redistribute it and/or modify it under the terms of the
   GNU General Public License as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   KeeperRL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along with this program.
   If not, see http://www.gnu.org/licenses/ . */

#pragma once

#include <map>
#include <set>

#include "enums.h"
#include "entity_map.h"
#include "hashing.h"
#include "content_id.h"

class Creature;
struct TribeDef;

// A tribe is now CONTENT: TribeId is a string id defined in tribes.txt, not a fixed C++ enum. The get*()
// helpers below are just named shorthands for the built-in tribe ids the engine references directly (keeper
// tribes, invaders, wildlife, ...) -- they resolve the same string a content file would. Custom tribes /
// alignments (UNDEAD, DEMONS, ...) are added purely in tribes.txt and need no code here.
class TribeId : public ContentId<TribeId> {
  public:
  using ContentId::ContentId;

  static TribeId getMonster();
  static TribeId getPest();
  static TribeId getWildlife();
  static TribeId getHuman();
  static TribeId getElf();
  static TribeId getDarkElf();
  static TribeId getDwarf();
  static TribeId getGnome();
  static TribeId getWhiteKeeper();
  static TribeId getBandit();
  static TribeId getHostile();
  static TribeId getShelob();
  static TribeId getPeaceful();
  static TribeId getDarkKeeper();
  static TribeId getRetiredKeeper();
  static TribeId getLizard();
  static TribeId getGreenskin();
  static TribeId getAnt();
  // RAR: transient tribe a keeper's invading team is retagged to during a keeper-vs-keeper invasion, so a
  // SAME-alignment defender treats them as enemies (same base keeper tribe would read as allies). Enemy of
  // every other tribe; restored to the team's real tribe when the invasion ends.
  static TribeId getInvaders();

  // This is a ridiculous, but effective hack to switch one tribe for another in entire model before retiring a game.
  static void switchForSerialization(TribeId from, TribeId to);
  static void clearSwitch();

  // Custom serialize (shadows ContentId's): (de)serialize the id by name via the base, then apply any active
  // switchForSerialization mapping -- that's how a loaded retired dungeon's keeper tribe becomes RetiredKeeper.
  SERIALIZATION_DECL(TribeId)
};

// A set of TribeId, backed by a bitset indexed by each tribe's ContentId internal id. 128 bits is far more
// than the ~19 built-in tribes plus any modded ones; getInternalId() is assigned globally in first-seen order.
class TribeSet {
  public:
  static TribeSet getFull();
  void clear();
  TribeSet& insert(TribeId);
  TribeSet& erase(TribeId);
  bool contains(TribeId) const;
  int getHash() const;

  bool operator==(const TribeSet&) const;

  template <class Archive>
  void serialize(Archive& ar, const unsigned int version);

  private:
  static constexpr int maxTribes = 128;
  bitset<maxTribes> SERIAL(elems);
};

CEREAL_CLASS_VERSION(TribeSet, 1)   // v1 stores enemies BY ID (name) instead of raw, index-keyed bits

class Tribe {
  public:
  Tribe(const Tribe&) = delete;
  Tribe& operator = (Tribe&&) = default;
  Tribe(Tribe&&) = default;
  bool isEnemy(const Creature*) const;
  bool isEnemy(const Tribe*) const;
  void addEnemy(Tribe*);
  void addAlly(Tribe*);   // the inverse: restores mutual friendliness (see TribeDef::allies)
  const TribeSet& getFriendlyTribes() const;

  void onMemberKilled(Creature* member, Creature* killer);
  void onItemsStolen(Creature* thief);

  SERIALIZATION_DECL(Tribe)

  typedef HashMap<TribeId, PTribe> Map;

  // Build every tribe + its friend/foe graph from the content defs loaded out of tribes.txt.
  static Map generateTribes(const std::map<TribeId, TribeDef>& defs);
  // Backfill any tribe present in the content defs but missing from a loaded save's map (e.g. a tribe added
  // since the save was written), so getTribe() never throws. Existing entries + their standings are kept.
  static void addMissingTribes(Map&, const std::map<TribeId, TribeDef>& defs);
  // Re-derive the whole friend/foe graph from content. friendlyTribes is a BITSET keyed by the RUNTIME-interned
  // TribeId and serialized as raw bits, so a save read by a process that interned tribes in a different order
  // (different mods, different load order, another player's dungeon) sees every bit as a DIFFERENT tribe --
  // including its own, which makes a tribe hostile to itself. The graph is fully derivable from tribes.txt, so
  // rebuild it on load instead of trusting the bits. Per-creature standings are left untouched.
  static void rewireFromDefs(Map&, const std::map<TribeId, TribeDef>& defs);

  private:
  Tribe(TribeId, bool diplomatic);
  static void init(Tribe::Map&, TribeId, bool diplomatic);
  double getStanding(const Creature*) const;

  bool SERIAL(diplomatic);

  void initStanding(const Creature*);
  double getMultiplier(const Creature* member);

  EntityMap<Creature, double> SERIAL(standing);
  TribeSet SERIAL(friendlyTribes);
  TribeId SERIAL(id);
};

