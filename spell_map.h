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

#include "game_time.h"
#include "spell.h"
#include "entity_map.h"
#include "attr_type.h"

class Spell;
class CreatureFactory;
struct ItemAbility;

class SpellMap {
  public:
  void add(Spell, AttrType, int level);
  void remove(SpellId);
  GlobalTime getReadyTime(const Creature*, const Spell*) const;
  void setReadyTime(const Creature*, const Spell*, GlobalTime);
  vector<const Spell*> getAvailable(const Creature*) const;
  bool contains(const SpellId) const;
  void onExpLevelReached(Creature*, AttrType, int level);
  void setAllReady();
  // Re-read every learned spell's DEFINITION from the content files. A learned spell is stored here as a full
  // copy (see SpellInfo below), so editing spells.txt only changes what new casters learn -- every creature
  // that already knows the spell keeps the version it learned, forever. Returns how many were refreshed.
  //
  // Only the definition is replaced: cooldown (`timeout`), `level` and `expType` are runtime state and are
  // left exactly as they are, so nothing is re-armed or re-levelled by reloading. A spell that no longer
  // exists in the files is left untouched rather than dropped -- removing it would strip an ability a live
  // creature is mid-cooldown on.
  int refreshFromFactory(const CreatureFactory&);

  template <class Archive>
  void serialize(Archive& ar, const unsigned int version);

  private:
  struct SpellInfo {
    Spell SERIAL(spell);
    optional<GlobalTime> SERIAL(timeout);
    int SERIAL(level);
    AttrType SERIAL(expType);
    SERIALIZE_ALL(spell, timeout, level, expType)
  };
  vector<SpellInfo> SERIAL(elems);
  const SpellInfo* getInfo(SpellId) const;
  SpellInfo* getInfo(SpellId);
};

