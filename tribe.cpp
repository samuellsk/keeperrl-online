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

#include "stdafx.h"

#include "tribe.h"
#include "tribe_def.h"
#include "creature.h"

template <class Archive> 
void Tribe::serialize(Archive& ar, const unsigned int version) {
  ar(diplomatic, standing, friendlyTribes, id);
}

SERIALIZABLE(Tribe);

SERIALIZATION_CONSTRUCTOR_IMPL(Tribe);

Tribe::Tribe(TribeId d, bool p) : diplomatic(p), friendlyTribes(TribeSet::getFull()), id(d) {
}

double Tribe::getStanding(const Creature* c) const {
  auto tribeId = c->getTribeId();
  if (!friendlyTribes.contains(tribeId))
    return -1;
  if (tribeId == id)
    return 1;
  if (auto res = standing.getMaybe(c)) 
    return *res;
  return 0;
}

void Tribe::initStanding(const Creature* c) {
  standing.set(c, getStanding(c));
}

// Undo hostility in BOTH directions. enemyOfAll erases the friendly bit on both tribes, so restoring it has to
// be mutual too, or the pair stays hostile from one side and Creature::isEnemy (which ORs both directions)
// still reports them as enemies.
void Tribe::addAlly(Tribe* t) {
  if (t != this) {
    friendlyTribes.insert(t->id);
    t->friendlyTribes.insert(id);
  }
}

void Tribe::addEnemy(Tribe* t) {
  if (t != this) {
    friendlyTribes.erase(t->id);
    t->friendlyTribes.erase(id);
  }
}

static const double killPenalty = 0.5;
static const double attackPenalty = 0.2;
static const double thiefPenalty = 0.5;

double Tribe::getMultiplier(const Creature* member) {
  return 1;
}

void Tribe::onMemberKilled(Creature* member, Creature* attacker) {
  CHECK(member->getTribe() == this);
  if (attacker == nullptr)
    return;
  if (diplomatic) {
    initStanding(attacker);
    standing.getOrFail(attacker) -= killPenalty * getMultiplier(member);
  }
}

bool Tribe::isEnemy(const Creature* c) const {
  return getStanding(c) < 0;
}

bool Tribe::isEnemy(const Tribe* t) const {
  return !friendlyTribes.contains(t->id);
}

const TribeSet& Tribe::getFriendlyTribes() const {
  return friendlyTribes;
}

void Tribe::onItemsStolen(Creature* attacker) {
  if (diplomatic) {
    initStanding(attacker);
    standing.getOrFail(attacker) -= thiefPenalty;
    addEnemy(attacker->getTribe());
  }
}

static void addEnemies(Tribe::Map& map, TribeId tribe, vector<TribeId> ids) {
  for (TribeId id : ids)
    map[tribe]->addEnemy(map[id].get());
}

void Tribe::init(Tribe::Map& map, TribeId id, bool diplomatic) {
  map[id].reset(new Tribe(id, diplomatic));
}

Tribe::Map Tribe::generateTribes(const std::map<TribeId, TribeDef>& defs) {
  Map ret;
  // First construct every tribe (all start friendly with everyone via the Tribe ctor's getFull()), then wire
  // the enemy graph -- so an enemy edge can reference a tribe declared later in the file.
  for (auto& elem : defs)
    init(ret, elem.first, elem.second.diplomatic);
  for (auto& elem : defs) {
    // enemyOfAll = hostile to every other tribe, so a tribe added later is automatically an enemy with no edit
    // to this one's list. addEnemy is mutual + a no-op on self, so this also makes everyone hostile back.
    if (elem.second.enemyOfAll)
      for (auto& other : ret)
        ret[elem.first]->addEnemy(other.second.get());
    for (auto& enemy : elem.second.enemies) {
      CHECK(ret.count(enemy)) << "tribes.txt: tribe '" << elem.first.data() << "' lists unknown enemy '"
          << enemy.data() << "'";
      ret[elem.first]->addEnemy(ret[enemy].get()); // addEnemy is mutual, matching the old addEnemies()
    }
  }
  // ALLIES LAST, in their own pass over every tribe. They are exceptions to the hostility wired above, so they
  // must be applied after ALL of it -- including other tribes' enemyOfAll, which would otherwise undo an ally
  // link declared earlier in the file just because that tribe happened to be processed later.
  for (auto& elem : defs) {
    // allyToAll: the mirror of enemyOfAll, and it belongs in THIS pass for the same reason -- it has to beat
    // every other tribe's enemyOfAll. `enemies` is its exception list, honoured in BOTH directions because
    // `enemies` is documented as mutual: a tribe that names me stays my enemy even though I tolerate the world.
    if (elem.second.allyToAll)
      for (auto& other : ret) {
        if (elem.second.enemies.contains(other.first))
          continue;
        auto od = defs.find(other.first);
        if (od != defs.end() && od->second.enemies.contains(elem.first))
          continue;
        ret[elem.first]->addAlly(other.second.get());
      }
    for (auto& ally : elem.second.allies) {
      CHECK(ret.count(ally)) << "tribes.txt: tribe '" << elem.first.data() << "' lists unknown ally '"
          << ally.data() << "'";
      ret[elem.first]->addAlly(ret[ally].get());
    }
  }
  return ret;
}

void Tribe::rewireFromDefs(Map& map, const std::map<TribeId, TribeDef>& defs) {
  Map fresh = generateTribes(defs);
  for (auto& elem : fresh) {
    auto it = map.find(elem.first);
    if (it == map.end())
      map[elem.first] = std::move(elem.second);            // tribe added to content since this save
    else
      it->second->friendlyTribes = elem.second->friendlyTribes; // authoritative graph; keep runtime standings
  }
}

void Tribe::addMissingTribes(Map& map, const std::map<TribeId, TribeDef>& defs) {
  // Insert any tribe present in the content defs but missing from a loaded save's map. A brand-new tribe is
  // treated as an enemy by the old ones (its id isn't in their serialized friendlyTribes) until its own edges
  // are wired -- so re-wire the whole graph from defs for the tribes we add, and leave existing ones' standings.
  Map fresh = generateTribes(defs);
  for (auto& elem : fresh)
    if (!map.count(elem.first))
      map[elem.first] = std::move(elem.second);
}

// Named shorthands for the built-in tribe ids the engine references directly. Each is just the content string
// a tribes.txt would use, so these MUST match the keys in tribes.txt exactly.
TribeId TribeId::getMonster()       { return TribeId("MONSTER"); }
TribeId TribeId::getInvaders()      { return TribeId("INVADER"); }
TribeId TribeId::getPest()          { return TribeId("PEST"); }
TribeId TribeId::getWildlife()      { return TribeId("WILDLIFE"); }
TribeId TribeId::getHuman()         { return TribeId("HUMAN"); }
TribeId TribeId::getElf()           { return TribeId("ELF"); }
TribeId TribeId::getDarkElf()       { return TribeId("DARK_ELF"); }
TribeId TribeId::getDwarf()         { return TribeId("DWARF"); }
TribeId TribeId::getGnome()         { return TribeId("GNOME"); }
TribeId TribeId::getWhiteKeeper()   { return TribeId("WHITE_KEEPER"); }
TribeId TribeId::getBandit()        { return TribeId("BANDIT"); }
TribeId TribeId::getHostile()       { return TribeId("HOSTILE"); }
TribeId TribeId::getPeaceful()      { return TribeId("PEACEFUL"); }
TribeId TribeId::getDarkKeeper()    { return TribeId("DARK_KEEPER"); }
TribeId TribeId::getRetiredKeeper() { return TribeId("RETIRED_KEEPER"); }
TribeId TribeId::getLizard()        { return TribeId("LIZARD"); }
TribeId TribeId::getGreenskin()     { return TribeId("GREENSKIN"); }
TribeId TribeId::getAnt()           { return TribeId("ANT"); }
TribeId TribeId::getShelob()        { return TribeId("SHELOB"); }

static HashMap<TribeId, TribeId> serialSwitch;

void TribeId::switchForSerialization(TribeId from, TribeId to) {
  serialSwitch[from] = to;
}

void TribeId::clearSwitch() {
  serialSwitch.clear();
}

template <class Archive>
void TribeId::serialize(Archive& ar, const unsigned int version) {
  ContentId<TribeId>::serialize(ar, version); // by name
  if (serialSwitch.count(*this))
    *this = serialSwitch.at(*this);
}

SERIALIZABLE(TribeId);

// cereal's serialization default ctor. ContentId has no default ctor, so seed the base with a dummy id -- it's
// immediately overwritten by ContentId::serialize reading the real name on load.
TribeId::TribeId() : ContentId(InternalId(0)) {}

// TribeId is a bitset index via ContentId's internal id. getFull() = friendly with every possible tribe (all
// bits set, so contains() is true for any id whose internal id fits), matching the old "start friendly, then
// remove enemies" model.
TribeSet TribeSet::getFull() {
  TribeSet ret;
  ret.elems.set();
  return ret;
}

void TribeSet::clear() {
  elems.reset();
}

TribeSet& TribeSet::insert(TribeId id) {
  int key = id.getInternalId();
  CHECK(key >= 0 && key < (int) elems.size()) << "tribe internal id " << key << " exceeds TribeSet capacity";
  elems.set(key);
  return *this;
}

TribeSet& TribeSet::erase(TribeId id) {
  int key = id.getInternalId();
  CHECK(key >= 0 && key < (int) elems.size());
  elems.reset(key);
  return *this;
}

bool TribeSet::contains(TribeId id) const {
  int key = id.getInternalId();
  CHECK(key >= 0 && key < (int) elems.size());
  return elems.test(key);
}

int TribeSet::getHash() const {
  return (int) std::hash<bitset<maxTribes>>()(elems);
}

bool TribeSet::operator==(const TribeSet& o) const {
  return elems == o.elems;
}

// A TribeSet is "friendly with everything, minus these enemies". The bitset is keyed by the RUNTIME-interned
// TribeId, so writing raw bits made a save readable ONLY by a process that interned tribes in the exact same
// order -- anyone else's copy silently mapped every bit onto a different tribe (a tribe could even come back
// hostile to itself). Store the ENEMIES BY ID instead: ContentId serializes as a name and re-interns on load,
// so the meaning survives a different mod set, load order or machine. Enemies are the small side of the set,
// and they are always real interned tribes.
template <class Archive>
void TribeSet::serialize(Archive& ar, const unsigned int version) {
  if (version == 0) {
    ar(elems);   // legacy saves: raw bits. Meaningful only in their writing process; Tribe::rewireFromDefs
    return;      // rebuilds the graph from content on load, which is what repairs them.
  }
  vector<TribeId> enemies;
  if (Archive::is_saving::value) {
    const int n = min<int>(TribeId::getNumIds(), maxTribes);
    for (int i = 0; i < n; ++i)
      if (!elems.test(i))
        enemies.push_back(TribeId(TribeId::InternalId(i)));
  }
  ar(enemies);
  if (!Archive::is_saving::value) {
    elems.set();               // friendly by default, exactly like getFull()
    for (auto& id : enemies)
      erase(id);
  }
}

SERIALIZABLE(TribeSet);

#include "pretty_archive.h"
template
void TribeId::serialize(PrettyInputArchive& ar1, unsigned);
