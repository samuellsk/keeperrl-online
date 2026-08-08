#pragma once

#include "campaign.h"
#include "biome_id.h"
#include "retired_games.h"
#include "t_string.h"
#include "random_layout_id.h" // generateServerWorldSites takes the world-map layout by id

struct CampaignSetup;
struct VillainCounts;
struct CampaignInfo;
class GameConfig;
struct AvatarInfo;
class ContentFactory;

using VillainsTuple = map<VillainGroup, vector<Campaign::VillainInfo>>;
using GameIntros = vector<TString>;

class CampaignBuilder {
  public:
  CampaignBuilder(View*, RandomGen&, Options*, VillainsTuple, GameIntros, const AvatarInfo&);
  optional<CampaignSetup> prepareCampaign(ContentFactory*, function<optional<RetiredGames>(CampaignType)>,
      CampaignType defaultType, string worldName,
      optional<Table<Campaign::SiteInfo>> serverSites = none); // RAR online: server-authoritative world
  static CampaignSetup getEmptyCampaign();

  // RAR online: generate the canonical shared-world sites (terrain + villains, NO keeper dweller).
  // Deterministic given the RNG. Used by the --rar_gen_world tool; the resulting Table is the
  // authoritative world served to all clients (they do NOT regenerate).
  // terrainSeed pins the TERRAIN to its own RNG: villain-placement retries then re-roll only the villains, so
  // the same seed always yields the same map. Without it a retry re-ran getTerrain on the advanced shared RNG.
  // worldMapId picks WHICH world-map layout to build. It used to be hardwired to worldMaps[0] -- vanilla's
  // "world_map" -- so a mod could add a world map to world_maps.txt and it could never be selected.
  static Table<Campaign::SiteInfo> generateServerWorldSites(RandomGen&, ContentFactory*, Options*,
      const VillainsTuple&, const vector<VillainGroup>&, int terrainSeed, RandomLayoutId worldMapId);
  // Dev tool: run any named layout (random_layouts.txt) at a given size -> Campaign of stacked tile layers
  // for the graphical preview. none if generation fails. worldMap picks the render zoom (dungeons zoom in).
  static optional<Campaign> previewLayoutCampaign(RandomGen&, ContentFactory*, const string& layoutName, Vec2 size, bool worldMap, int zoom, const string& mappingName);
  // RAR: rebuild a keeper on the EXISTING shared world (server rar_campaign.dat sites) so an imported base lands
  // in the populated world (villains + all) instead of a 1x1 empty campaign. Places the keeper on a RANDOM empty
  // tile whose biome matches the base's biome. none if the world has no free tile of that biome.
  // preferredPos: keep the keeper where it already is IF that tile is still free and the right biome in this
  // world (used when re-syncing an existing keeper to a regenerated world -- only relocate if it must).
  // claimedByOthers: tiles held by OTHER keepers' claims. The world blob carries only terrain + villains --
  // rival keepers live in the server's separate claim list -- so without this the keeper could be re-homed
  // straight on top of someone else's base.
  static optional<Campaign> reconstructKeeperCampaign(RandomGen&, ContentFactory*, Table<Campaign::SiteInfo> worldSites,
      BiomeId biome, ViewIdList playerViewId, TribeId tribe, const string& worldName,
      optional<Vec2> preferredPos = none, const vector<Vec2>& claimedByOthers = {});

  private:
  View* view;
  RandomGen& random;
  Options* options;
  VillainsTuple villains;
  GameIntros gameIntros;
  const AvatarInfo& avatarInfo;
  vector<OptionId> getCampaignOptions(CampaignType) const;
  vector<OptionId> getPrimaryOptions() const;
  void setPlayerPos(Campaign&, Vec2, ViewIdList, ContentFactory*);
  bool biomeAllowed(const Campaign&, Vec2) const;              // RAR forcedBiome: silent check
  bool canSettleOn(const Campaign&, Vec2, ContentFactory*);    // ...and the one that explains the refusal
  vector<CampaignType> getAvailableTypes() const;
  bool placeVillains(const ContentFactory*, Campaign&, Table<bool>&, vector<Campaign::SiteInfo::Dweller>, int count,
      Range playerDist);
  vector<Campaign::VillainInfo> getVillains(const vector<VillainGroup>&, VillainType);
  bool placeVillains(const ContentFactory*, Campaign&, const VillainCounts&, const optional<RetiredGames>&,
      const vector<VillainGroup>&);
  // Static versions (friends of Campaign via CampaignBuilder) so the server-world generator can
  // reuse the exact placement logic without an AvatarInfo/View. random + villains passed explicitly.
  static bool placeVillainsInner(RandomGen&, const ContentFactory*, Campaign&, Table<bool>&,
      vector<Campaign::SiteInfo::Dweller>, int count, Range playerDist);
  static vector<Campaign::VillainInfo> getVillainsFrom(const VillainsTuple&, const vector<VillainGroup>&, VillainType);
  static bool placeVillainsAll(RandomGen&, const VillainsTuple&, const ContentFactory*, Campaign&,
      const VillainCounts&, const optional<RetiredGames>&, const vector<VillainGroup>&);
  const vector<TString>& getIntroMessages(CampaignType) const;
  void setCountLimits(const CampaignInfo&);
};

struct CampaignSetup {
  Campaign campaign;
  string gameIdentifier;
  TString gameDisplayName;
  vector<TString> introMessages;
  optional<ExternalEnemiesType> externalEnemies;
  EnemyAggressionLevel enemyAggressionLevel;
};
