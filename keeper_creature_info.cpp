#include "keeper_creature_info.h"
#include "special_trait.h"
#include "creature_id.h"
#include "tribe_alignment.h"
#include "lasting_effect.h"
#include "attr_type.h"

template <class Archive>
void KeeperCreatureInfo::serialize(Archive& ar, const unsigned int version) {
  ar(NAMED(creatureId), NAMED(tribeAlignment), NAMED(immigrantGroups), NAMED(technology), NAMED(initialTech), NAMED(buildingGroups), NAMED(workshopGroups), NAMED(description), OPTION(specialTraits), OPTION(noLeader), NAMED(baseNameGen), OPTION(minionTraits), OPTION(maxPopulation), OPTION(immigrantInterval), OPTION(populationString), OPTION(prisoners), OPTION(endlessEnemyGroups), NAMED(unlock), OPTION(credit), OPTION(flags), NAMED(zLevelGroups), NAMED(villainGroups), OPTION(requireQuartersForExp), OPTION(startingBase), NAMED(baseName), OPTION(enemyAggression), OPTION(tribe));
  if (version >= 1)
    ar(OPTION(prisonerPredicate));
  if (version >= 2)
    ar(OPTION(developerOnly));
  if (version >= 3)
    ar(OPTION(forcedBiome), OPTION(forcedBiomeMessage));
}

SERIALIZABLE(KeeperCreatureInfo);

KeeperCreatureInfo::KeeperCreatureInfo() {
}

STRUCT_IMPL(KeeperCreatureInfo)

#include "pretty_archive.h"
template
void KeeperCreatureInfo::serialize(PrettyInputArchive& ar1, unsigned);
