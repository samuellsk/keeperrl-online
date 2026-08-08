#include "stdafx.h"
#include "campaign_info.h"

// BINARY layout: EXACTLY the SERIAL fields, in their original order, and nothing else. CampaignInfo lives
// inside ContentFactory, so this list is frozen into every existing save -- adding to it breaks them all.
// The villain-wave knobs are content-only and appear solely in the pretty specialization below.
template <class Archive>
void CampaignInfo::serialize(Archive& ar, const unsigned int version) {
  ar(NAMED(size), NAMED(maxMainVillains), NAMED(maxLesserVillains), NAMED(maxMinorVillains), NAMED(maxAllies),
      NAMED(initialRadius), OPTION(mapZoom), OPTION(minimapZoom), OPTION(minMainVillainsAlive),
      OPTION(minLesserVillainsAlive), OPTION(minMinorVillainsAlive), OPTION(poolMainVillainsGenerated),
      OPTION(poolLesserVillainsGenerated), OPTION(poolMinorVillainsGenerated), OPTION(endlessEnemies),
      OPTION(expIncrease));
}

SERIALIZABLE(CampaignInfo);

#include "pretty_archive.h"

// CONTENT layout (campaign_info.txt): everything above, plus the villain-wave tuning knobs.
template <>
void CampaignInfo::serialize(PrettyInputArchive& ar, const unsigned int version) {
  ar(NAMED(size), NAMED(maxMainVillains), NAMED(maxLesserVillains), NAMED(maxMinorVillains), NAMED(maxAllies),
      NAMED(initialRadius), OPTION(mapZoom), OPTION(minimapZoom), OPTION(minMainVillainsAlive),
      OPTION(minLesserVillainsAlive), OPTION(minMinorVillainsAlive), OPTION(poolMainVillainsGenerated),
      OPTION(poolLesserVillainsGenerated), OPTION(poolMinorVillainsGenerated), OPTION(endlessEnemies),
      OPTION(expIncrease),
      OPTION(villainWaveGracePeriod), OPTION(villainWavePrepMin), OPTION(villainWavePrepMax),
      OPTION(villainWaveForcePercent), OPTION(villainProximityRadius),
      OPTION(villainWaveCooldownRollMin), OPTION(villainWaveCooldownRollMax));
}
