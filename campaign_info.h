#pragma once

#include "stdafx.h"
#include "util.h"

class PrettyInputArchive;

struct CampaignInfo {
  Vec2 SERIAL(size);
  int SERIAL(maxMainVillains);
  int SERIAL(maxLesserVillains);
  int SERIAL(maxMinorVillains);
  int SERIAL(maxAllies);
  int SERIAL(initialRadius);
  int SERIAL(mapZoom) = 2;
  int SERIAL(minimapZoom) = 2;
  // RAR Phase B: minimum alive villains per tier before the server respawns more (0 = never respawn).
  int SERIAL(minMainVillainsAlive) = 0;
  int SERIAL(minLesserVillainsAlive) = 0;
  int SERIAL(minMinorVillainsAlive) = 0;
  // RAR Phase B: how many spare villain maps per tier to pre-generate for respawns (the respawn pool).
  int SERIAL(poolMainVillainsGenerated) = 10;
  int SERIAL(poolLesserVillainsGenerated) = 10;
  int SERIAL(poolMinorVillainsGenerated) = 10;
  // NOTE: there is deliberately NO minAlliesAlive / poolAlliesGenerated here. CampaignInfo is serialized into
  // every save (ContentFactory::campaignInfo), and OPTION() is only optional to the PRETTY (config .txt)
  // archive -- to the BINARY archive it is a plain mandatory field. So ADDING A *SERIAL* FIELD HERE SILENTLY
  // BREAKS EVERY EXISTING SAVE. The ALLY respawn knobs live in server/rar_villain_config.txt (ALLY / POOL_ALLY),
  // which is read live and hand-editable -- the right home for a server-side knob anyway. Don't move them here.
  // RAR: endless enemy waves attacking the keeper's dungeon. 0 = off, 1 = from the start, 2 = after winning.
  int SERIAL(endlessEnemies) = 0;
  // RAR: enemy difficulty curve (server/config-fixed online, not a player option). 0 = mild, 1 = normal, 2 = extreme.
  int SERIAL(expIncrease) = 1;

  // ---- RAR villain-wave tuning: CONTENT-ONLY, deliberately NOT SERIAL ----------------------------------
  // These are read from campaign_info.txt but are NOT part of the binary save layout (see the pretty-archive
  // specialization below and campaign_info.cpp). That is what makes them safe to add: the fields above are
  // frozen into every existing save, these are not. Add any future knob here the same way -- never as SERIAL.
  // They drive Game::considerVillainWaves.
  int villainWaveGracePeriod = 1000;   // no villain stirs before this turn (a new keeper gets peace to build)
  int villainWavePrepMin = 800;        // once triggered, the wave lands this many turns later, at the earliest
  int villainWavePrepMax = 1500;       // ... and this many at the latest (the player's warning window)
  int villainWaveForcePercent = 75;    // % of the villain's maxPopulation that marches (its fighters, never the leader)
  int villainProximityRadius = 7;      // Proximity trigger: villain within this many world-map tiles of the base
  int villainWaveCooldownRollMin = 400;// random slack added on top of waveSize * immigrantInterval ...
  int villainWaveCooldownRollMax = 1400;// ... before the same villain may attack again

  template <class Archive>
  void serialize(Archive& ar, const unsigned int version);
};

// Content (campaign_info.txt) parse: the SERIAL fields PLUS the villain-wave knobs. Declared here so every
// translation unit that parses a CampaignInfo picks this specialization over the binary-layout generic.
template <>
void CampaignInfo::serialize(PrettyInputArchive& ar, const unsigned int version);
