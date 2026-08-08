#include "enemy_info.h"
#include "immigrant_info.h"
#include "creature_factory.h"
#include "creature_list.h"

SERIALIZE_DEF(EnemyInfo, NAMED(settlement), OPTION(config), NAMED(behaviour), NAMED(levelConnection), OPTION(immigrants), OPTION(discoverable), NAMED(createOnBones), OPTION(biome), NAMED(otherEnemy))

SERIALIZATION_CONSTRUCTOR_IMPL(EnemyInfo)

EnemyInfo::EnemyInfo(SettlementInfo s, CollectiveConfig c, optional<VillageBehaviour> v,
    optional<LevelConnection> l)
  : settlement(s), config(c), behaviour(v), levelConnection(l) {
}

void EnemyInfo::updateBuildingInfo(const map<BuildingId, BuildingInfo>& info) {
  using namespace MapLayoutTypes;
  settlement.type.visit(
        [&](const Builtin& elem) { const_cast<Builtin&>(elem).buildingInfo = info.at(elem.buildingId); },
        [&](const Predefined& elem) { const_cast<Predefined&>(elem).buildingInfo = info.at(elem.buildingId); },
        [&](const RandomLayout&) {}
  );
}

static optional<CreatureId> firstCreature(const CreatureList& list) {
  if (!list.uniques.empty())
    return list.uniques[0];
  if (!list.all.empty())
    return list.all[0].second;
  return none;
}

// Who this enemy IS, worked out from the definition alone -- no Collective and no generated creatures needed.
// A settled villain gets its name from CollectiveBuilder::generateName once its creatures exist, but a
// world-map villain the player has never travelled to has neither, so anything that has to name it before then
// (the RAR villain panel, an incoming wave) needs this. Same order of preference as generateName: the
// settlement's race first, then whoever leads it.
// Returns none when the definition names neither -- the caller decides what to fall back to.
optional<TString> EnemyInfo::getDisplayName(const CreatureFactory* factory) const {
  if (settlement.race)
    return *settlement.race;
  if (auto id = firstCreature(settlement.inhabitants.leader))
    return factory->getName(*id);
  if (auto id = firstCreature(settlement.inhabitants.fighters))
    return factory->getName(*id);
  return none;
}

optional<BiomeId> EnemyInfo::getBiome() const {
  if (!!biome)
    return biome;
  return settlement.type.visit(
      [&](const MapLayoutTypes::Builtin& type) -> optional<BiomeId> {
        switch (type.id) {
          case BuiltinLayoutId::CASTLE2:
          case BuiltinLayoutId::TOWER:
          case BuiltinLayoutId::VILLAGE:
            return BiomeId("GRASSLAND");
          case BuiltinLayoutId::CAVE:
          case BuiltinLayoutId::MINETOWN:
          case BuiltinLayoutId::SMALL_MINETOWN:
          case BuiltinLayoutId::ANT_NEST:
          case BuiltinLayoutId::VAULT:
            return BiomeId("MOUNTAIN");
          case BuiltinLayoutId::FORREST_COTTAGE:
          case BuiltinLayoutId::FORREST_VILLAGE:
          case BuiltinLayoutId::ISLAND_VAULT_DOOR:
          case BuiltinLayoutId::FOREST:
            return BiomeId("FOREST");
          case BuiltinLayoutId::CEMETERY:
            return BiomeId("GRASSLAND");
          default: return none;
        }
      },
      [&](const auto&) -> optional<BiomeId> {
        return none;
      }
    );
}

STRUCT_IMPL(EnemyInfo)

EnemyInfo& EnemyInfo::setVillainType(VillainType type) {
  villainType = type;
  return *this;
}

EnemyInfo& EnemyInfo::setId(EnemyId i) {
  id = i;
  return *this;
}

template <class Archive>
void LevelConnection::LevelInfo::serialize(Archive& ar, unsigned int v) {
  ar(NAMED(enemy), NAMED(levelSize), NAMED(levelType), NAMED(name), OPTION(isLit), OPTION(canTransfer), OPTION(aiFollows));
}

#include "pretty_archive.h"
template
void EnemyInfo::serialize(PrettyInputArchive& ar1, unsigned);

template
void LevelConnection::LevelInfo::serialize(PrettyInputArchive& ar, unsigned int v);
