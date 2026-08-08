#pragma once

#include "util.h"
#include "tech_id.h"
#include "name_generator_id.h"
#include "minion_trait.h"
#include "cost_info.h"
#include "villain_group.h"
#include "keeper_base_info.h"
#include "tribe.h"
#include "t_string.h"
#include "creature_predicate.h"
#include "biome_id.h"
#include "scripted_ui_data.h"

class SpecialTrait;

struct KeeperCreatureInfo {
  KeeperCreatureInfo();
  STRUCT_DECLARATIONS(KeeperCreatureInfo)
  vector<CreatureId> SERIAL(creatureId);
  TribeAlignment SERIAL(tribeAlignment);
  // Optional: put this keeper on a specific content tribe (from tribes.txt) instead of the default keeper
  // tribe for its alignment (EVIL->DARK_KEEPER, LAWFUL->WHITE_KEEPER). This is how you make a keeper with a
  // genuinely different faction + friend/foe set, e.g. tribe = UNDEAD_KEEPER with no allies. tribeAlignment
  // still applies for the two-camp RAR logic; only the tribe identity changes.
  optional<TribeId> SERIAL(tribe);
  vector<string> SERIAL(immigrantGroups);
  vector<TechId> SERIAL(technology);
  vector<TechId> SERIAL(initialTech);
  vector<string> SERIAL(buildingGroups);
  vector<string> SERIAL(workshopGroups);
  vector<string> SERIAL(zLevelGroups);
  TString SERIAL(description);
  vector<SpecialTrait> SERIAL(specialTraits);
  optional<NameGeneratorId> SERIAL(baseNameGen);
  EnumSet<MinionTrait> SERIAL(minionTraits) = {MinionTrait::LEADER};
  int SERIAL(maxPopulation) = 10;
  int SERIAL(immigrantInterval) = 140;
  TString SERIAL(populationString) = TStringId("POPULATION");
  bool SERIAL(noLeader) = false;
  bool SERIAL(prisoners) = true;
  optional<CreaturePredicate> SERIAL(prisonerPredicate) = none;
  vector<string> SERIAL(endlessEnemyGroups) = {"basic"};
  optional<string> SERIAL(unlock);
  vector<CostInfo> SERIAL(credit);
  vector<string> SERIAL(flags);
  vector<VillainGroup> SERIAL(villainGroups);
  bool SERIAL(requireQuartersForExp) = true;
  optional<KeeperBaseInfo> SERIAL(startingBase) = none;
  TString SERIAL(baseName);
  bool SERIAL(enemyAggression) = true;
  // RAR: only a DEVELOPER account may pick this keeper. The mod defining it stays active for EVERY player --
  // its creatures, items and z-levels must exist in the world, because the developer plays this keeper against
  // them (a boss). Only the avatar menu is gated.
  bool SERIAL(developerOnly) = false;
  // RAR: restrict where this keeper may found its base. Matters ONLY during world-map placement -- once the
  // base exists nothing re-checks it. A tomb lord who fights with mummies and scarabs belongs in the desert;
  // without this he can be started in snow, where his whole design makes no sense.
  // Biomes this keeper may found its base on. EMPTY = no restriction (the default), so every keeper that
  // doesn't mention it is unaffected. Several are allowed:  forcedBiome = { "DESERT" "ANCIENT_DESERT" }
  vector<BiomeId> SERIAL(forcedBiome);
  // Shown when the player tries to settle on the wrong biome. Same shape as the UI effect used elsewhere in
  // the content files (a scripted-UI id plus its data), so it can carry a picture and flavour text rather
  // than a bare refusal. Falls back to a plain message when not set.
  struct BiomeMessage {
    ScriptedUIId SERIAL(id);
    ScriptedUIData SERIAL(data);
    SERIALIZE_ALL(id, data)
  };
  optional<BiomeMessage> SERIAL(forcedBiomeMessage);
  template <class Archive>
  void serialize(Archive& ar, const unsigned int);
};

static_assert(std::is_nothrow_move_constructible<KeeperCreatureInfo>::value, "T should be noexcept MoveConstructible");

CEREAL_CLASS_VERSION(KeeperCreatureInfo, 3)