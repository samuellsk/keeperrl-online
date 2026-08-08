#pragma once

#include "util.h"
#include "saved_game_info.h"
#include "save_file_info.h"
#include "enemy_id.h"
#include "tribe.h"
#include "biome_id.h"
#include "villain_group.h"
#include "t_string.h"
#include "rar_client.h" // RAR: RarVillain for reconcileVillains

class View;
class ProgressMeter;
class Options;
class RetiredGames;
class ContentFactory;

struct CampaignSetup;

struct VillainViewId {
  ViewIdList SERIAL(ids);
  SERIALIZE_ALL(ids)
  void serialize(PrettyInputArchive& ar, unsigned int);
};

class Campaign {
  public:
  struct VillainInfo {
    ViewId SERIAL(dwellingId);
    EnemyId SERIAL(enemyId);
    TString SERIAL(name);
    bool SERIAL(alwaysPresent) = false;
    bool isEnemy() const;
    VillainType SERIAL(type);
    // RAR: this faction is SOMEONE ELSE'S ally that reconcileVillains demoted to MINOR for THIS keeper (it's not
    // in my villainGroups, so it's an enemy to me). Transient -- NOT serialized; reconcileVillains re-derives it
    // every load. Used to paint an ORANGE world-map highlight so a converted ally reads differently from a
    // native minor villain.
    bool convertedFromAlly = false;
    // What to SHOW the player. A converted ally is mechanically MINOR on purpose -- that is what keeps
    // blocksInfluence() false, so travel influence, distance difficulty scaling and the aggressor cutoff are
    // left exactly as they were. But reading "minor" for what is a full faction settlement is misleading, so it
    // is PRESENTED as LESSER (name + yellow marker). Display only: never use this for game logic, use `type`.
    VillainType getDisplayType() const;   // defined in campaign.cpp -- VillainType is incomplete here
    SERIALIZE_ALL(NAMED(dwellingId), NAMED(enemyId), NAMED(name), NAMED(type), OPTION(alwaysPresent))
  };
  struct KeeperInfo {
    ViewIdList SERIAL(viewId);
    TribeId SERIAL(tribe);
    SERIALIZE_ALL(viewId, tribe)
  };
  struct RetiredInfo {
    SavedGameInfo SERIAL(gameInfo);
    SaveFileInfo SERIAL(fileInfo);
    SERIALIZE_ALL(gameInfo, fileInfo)
  };
  struct SiteInfo {
    vector<ViewId> SERIAL(viewId);
    optional<BiomeId> SERIAL(biome);
    typedef variant<VillainInfo, RetiredInfo, KeeperInfo> Dweller;
    optional<Dweller> SERIAL(dweller);
    vector<SavedGameInfo::MinionInfo> SERIAL(inhabitants);
    optional<VillainInfo> getVillain() const;
    bool isConvertedAlly() const; // RAR: a demoted-to-MINOR former ally -> orange world-map highlight
    optional<RetiredInfo> getRetired() const;
    optional<KeeperInfo> getKeeper() const;
    bool isEnemy() const;
    bool isEmpty() const;
    bool SERIAL(blocked) = false;
    optional<ViewIdList> getDwellingViewId() const;
    optional<TString> getDwellerDescription() const;
    optional<TString> getDwellerName() const;
    optional<VillainType> getVillainType() const;
    SERIALIZE_ALL(viewId, biome, dweller, blocked, inhabitants)
  };

  const Table<SiteInfo>& getSites() const;
  Vec2 getPlayerPos() const;
  Vec2 getOriginalPlayerPos() const;
  bool isGoodStartPos(Vec2) const;
  BiomeId getBaseBiome() const;
  const string& getWorldName() const;
  bool isDefeated(Vec2) const;
  void setDefeated(const ContentFactory*, Vec2);
  void removeDweller(Vec2);
  void setDweller(Vec2, SiteInfo::Dweller); // RAR online: inject a downloaded retired dungeon to invade
  // RAR world-map sync: make the villain dwellers on the map EXACTLY match the server's alive roster --
  // clear all villains, then place each roster entry (removes the defeated, adds respawns on new tiles).
  // myGroups = the LOGGED-IN keeper's villainGroups. The RAR world is shared and generated from every group,
  // so ally-ness is decided per keeper here: an ALLY in my groups stays an ally, anyone else's ally becomes
  // an enemy (MINOR) on my map. Pass the keeper's own groups or every faction reads as an ally to everyone.
  // playerTribe = the keeper's own tribe. TRIBES OVERRULE THE VILLAIN GROUP: an ALLY whose faction tribe is
  // hostile to playerTribe (enemyOfAll without an `allies` entry, or an explicit `enemies` edge) is demoted to
  // an enemy too, so the world map can't offer an alliance the creatures themselves would never honour.
  void reconcileVillains(const ContentFactory*, const vector<RarVillain>&, const vector<VillainGroup>& myGroups,
      TribeId playerTribe);
  bool canTravelTo(Vec2) const;
  bool isInInfluence(Vec2) const;
  int getNumNonEmpty() const;
  int getMapZoom() const;
  int getMinimapZoom() const;
  void setRenderZoom(int mapZoom, int minimapZoom); // offline map-generator: a hand-built Campaign has no zoom set
  int getBaseLevelIncrease(Vec2) const;
  bool passesMaxAggressorCutOff(Vec2);
  CampaignType getType() const;
  void updateInhabitants(ContentFactory*);

  map<string, string> getParameters() const;

  SERIALIZATION_DECL(Campaign)

  private:
  friend class CampaignBuilder;
  void refreshInfluencePos(const ContentFactory*);
  void refreshMaxAggressorCutOff();
  Campaign(Table<SiteInfo>, CampaignType, const string& worldName, int expIncrease);
  Table<SiteInfo> SERIAL(sites);
  Vec2 SERIAL(playerPos);
  string SERIAL(worldName);
  Table<bool> SERIAL(defeated);
  set<Vec2> SERIAL(influencePos);
  CampaignType SERIAL(type);
  int SERIAL(mapZoom);
  int SERIAL(minimapZoom) = 2;
  Vec2 SERIAL(originalPlayerPos);
  Table<bool> SERIAL(belowMaxAgressorCutOff);
  int SERIAL(expIncrease) = 3;
};

CEREAL_CLASS_VERSION(Campaign, 1)
