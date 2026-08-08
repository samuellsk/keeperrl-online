#include "stdafx.h"
#include "campaign_builder.h"
#include "rar_client.h"
#include "options.h"
#include "campaign_type.h"
#include "util.h"
#include "view.h"
#include "enemy_factory.h"
#include "villain_type.h"
#include "creature.h"
#include "creature_name.h"
#include "retired_games.h"
#include "view_object.h"
#include "name_generator.h"
#include "creature_factory.h"
#include "tribe_alignment.h"
#include "external_enemies_type.h"
#include "enemy_aggression_level.h"
#include "campaign_info.h"
#include "content_factory.h"
#include "avatar_info.h"
#include "layout_canvas.h"
#include "layout_generator.h"
#include "layout_mapping.h"
#include "furniture.h"
#include "enemy_info.h"
#include "villain_group.h"

void CampaignBuilder::setCountLimits(const CampaignInfo& info) {
  int minMainVillains =
#ifdef RELEASE
  1;
#else
  0;
#endif
  options->setLimits(OptionId::MAIN_VILLAINS, Range(minMainVillains, info.maxMainVillains + 1));
  options->setLimits(OptionId::LESSER_VILLAINS, Range(1, info.maxLesserVillains + 1));
  options->setLimits(OptionId::MINOR_VILLAINS, Range(0, info.maxMinorVillains + 1));
  options->setLimits(OptionId::ALLIES, Range(0, info.maxAllies + 1));
}

vector<OptionId> CampaignBuilder::getCampaignOptions(CampaignType type) const {
  if (rarEnabled())
    // RAR online: villain counts + the enemy difficulty curve (EXP_INCREASE) are server/config-fixed and
    // hidden, but each player picks their OWN endless-enemy waves and aggression -- these are per-client
    // (applied to that client's models) and neither triggers updateMap, so the shared map never re-rolls.
    return { OptionId::ENDLESS_ENEMIES, OptionId::ENEMY_AGGRESSION };
  switch (type) {
    case CampaignType::QUICK_MAP:
      return {OptionId::LESSER_VILLAINS, OptionId::ALLIES};
    case CampaignType::FREE_PLAY:
      return {
        OptionId::MAIN_VILLAINS,
        OptionId::LESSER_VILLAINS,
        OptionId::MINOR_VILLAINS,
        OptionId::ALLIES,
        OptionId::EXP_INCREASE,
        OptionId::ENDLESS_ENEMIES,
        OptionId::ENEMY_AGGRESSION,
      };
  }
}

vector<CampaignType> CampaignBuilder::getAvailableTypes() const {
  return {
    CampaignType::FREE_PLAY,
#ifndef RELEASE
    CampaignType::QUICK_MAP,
#endif
  };
}

// The biome names this keeper accepts, as one "a or b or c" phrase for the refusal messages.
static TString forcedBiomeNames(const ContentFactory* f, const vector<BiomeId>& forced) {
  vector<TString> names;
  for (auto& b : forced)
    names.push_back(f->biomeInfo.at(b).name);
  return combineWithOr(std::move(names));
}

// RAR: a keeper with forcedBiome may only found its base on one of those biomes. Checked ONLY here, at
// placement -- nothing re-validates it later, so an existing base is never disturbed. Returns false (and tells
// the player why) when the tile is the wrong biome.
bool CampaignBuilder::biomeAllowed(const Campaign& campaign, Vec2 pos) const {
  auto& forced = avatarInfo.creatureInfo.forcedBiome;
  if (forced.empty())
    return true;   // no restriction
  auto& tileBiome = campaign.getSites()[pos].biome;
  return tileBiome && forced.contains(*tileBiome);
}

bool CampaignBuilder::canSettleOn(const Campaign& campaign, Vec2 pos, ContentFactory* f) {
  auto& forced = avatarInfo.creatureInfo.forcedBiome;
  if (forced.empty())
    return true;
  auto& tileBiome = campaign.getSites()[pos].biome;
  if (tileBiome && forced.contains(*tileBiome))
    return true;
  // Content can supply a themed refusal (picture + flavour text); otherwise say it plainly.
  if (auto& msg = avatarInfo.creatureInfo.forcedBiomeMessage) {
    ScriptedUIState state{};
    view->scriptedUI(msg->id, msg->data, state);
  } else
    view->presentText(none, TSentence("KEEPER_REQUIRES_BIOME", forcedBiomeNames(f, forced)));
  return false;
}

void CampaignBuilder::setPlayerPos(Campaign& campaign, Vec2 pos, ViewIdList playerViewId, ContentFactory* f) {
  campaign.sites[campaign.playerPos].dweller.reset();
  campaign.playerPos = pos;
  campaign.sites[campaign.playerPos].dweller =
      Campaign::SiteInfo::Dweller(Campaign::KeeperInfo{playerViewId,
          avatarInfo.playerCreature->getTribeId()});
  campaign.updateInhabitants(f);
}

static Table<Campaign::SiteInfo> getTerrain(RandomGen& random, const ContentFactory* factory,
    RandomLayoutId worldMapId, Vec2 size) {
  unordered_set<Token> allViewIds;
  for (auto& def : factory->tilePaths.definitions)
    allViewIds.insert(def.viewId.data());
  LayoutCanvas::Map map{Table<vector<Token>>(Rectangle(Vec2(0, 0), size))};
  LayoutCanvas canvas{map.elems.getBounds(), &map};
  bool generated = false;
  for (int i : Range(20))
    if (factory->randomLayouts.at(worldMapId).make(canvas, random)) {
      generated = true;
      break;
    }
  CHECK(generated) << "Failed to generate world map";
  Table<Campaign::SiteInfo> ret(size, {});
  for (Vec2 v : ret.getBounds())
    for (auto& token : map.elems[v]) {
      if (token == "blocked")
        ret[v].blocked = true;
      else if (allViewIds.count(token))
        ret[v].viewId.push_back(ViewId(token.data()));
      else
        for (auto& biome : factory->biomeInfo)
          if (token == biome.first.data())
            ret[v].biome = biome.first;
    }
  return ret;
}

struct VillainCounts {
  int numMain;
  int numLesser;
  int numMinor;
  int numAllies;
  int maxRetired;
};

static VillainCounts getVillainCounts(CampaignType type, Options* options) {
  switch (type) {
    case CampaignType::FREE_PLAY: {
      return {
        options->getIntValue(OptionId::MAIN_VILLAINS),
        options->getIntValue(OptionId::LESSER_VILLAINS),
        options->getIntValue(OptionId::MINOR_VILLAINS),
        options->getIntValue(OptionId::ALLIES),
        10
      };
    }
    case CampaignType::QUICK_MAP:
      return {0, 0, 0, 0};
  }
}

CampaignBuilder::CampaignBuilder(View* v, RandomGen& rand, Options* o, VillainsTuple villains, GameIntros intros, const AvatarInfo& a)
    : view(v), random(rand), options(o), villains(std::move(villains)), gameIntros(intros), avatarInfo(a) {
}

static string getNewIdSuffix() {
  vector<char> chars;
  for (char c : Range(128))
    if (isalnum(c))
      chars.push_back(c);
  string ret;
  for (int i : Range(4))
    ret += Random.choose(chars);
  return ret;
}

constexpr auto mapMargin = 10;

bool CampaignBuilder::placeVillains(const ContentFactory* contentFactory, Campaign& campaign, Table<bool>& blocked,
    vector<Campaign::SiteInfo::Dweller> villains, int count, Range playerDist) {
  return placeVillainsInner(random, contentFactory, campaign, blocked, std::move(villains), count, playerDist);
}

bool CampaignBuilder::placeVillainsInner(RandomGen& random, const ContentFactory* contentFactory, Campaign& campaign,
    Table<bool>& blocked, vector<Campaign::SiteInfo::Dweller> villains, int count, Range playerDist) {
  if (villains.empty())
    return true;
  for (int i = 0; villains.size() < count; ++i)
    villains.push_back(villains[i]);
  CHECK(count >= 0);
  if (villains.size() > count)
    villains.resize(count);
  for (int i : All(villains)) {
    auto biome = [&] {
      return villains[i].match(
          [&](const Campaign::VillainInfo& info) { return contentFactory->enemies.at(info.enemyId).getBiome(); },
          [](const Campaign::RetiredInfo& info) -> optional<BiomeId> { return none; },
          [](const Campaign::KeeperInfo& info) -> optional<BiomeId> { return none; }
      );
    }();
    auto placed = [&] {
      for (Vec2 v : random.permutation(campaign.sites.getBounds().minusMargin(mapMargin).getAllSquares())) {
        if (!blocked[v] && playerDist.contains((int) v.distD(campaign.getPlayerPos())) &&
            !!campaign.sites[v].biome && (!biome || campaign.sites[v].biome == biome)) {
          campaign.sites[v].dweller = villains[i];
          for (auto v2 : Rectangle::centered(v, 4).intersection(blocked.getBounds())) // min villain spacing
            blocked[v2] = true;
          return true;
        }
      }
      return false;
    }();
    if (!placed)
      return false;
  }
  return true;
}

using Dweller = Campaign::SiteInfo::Dweller;

vector<Dweller> shuffle(RandomGen& random, vector<Campaign::VillainInfo> v) {
  int numAlways = 0;
  for (auto& elem : v)
    if (elem.alwaysPresent) {
      swap(v[numAlways], elem);
      ++numAlways;
    }
  random.shuffle(v.begin() + numAlways, v.end());
  return v.transform([](auto& t) { return Dweller(t); });
}

vector<Campaign::VillainInfo> CampaignBuilder::getVillains(const vector<VillainGroup>& groups, VillainType type) {
  return getVillainsFrom(villains, groups, type);
}

vector<Campaign::VillainInfo> CampaignBuilder::getVillainsFrom(const VillainsTuple& villains,
    const vector<VillainGroup>& groups, VillainType type) {
  vector<Campaign::VillainInfo> ret;
  for (auto& group : groups)
    if (auto it = villains.find(group); it != villains.end())
      for (auto& elem : it->second) {
        if (elem.type != type)
          continue;
        // ONE entry per villain, however many groups list it. Groups overlap heavily -- EVIL_COTTAGES,
        // LAWFUL_COTTAGES and NECRO_COTTAGES all 'inherit COTTAGES', so without this every cottage villain
        // entered the draw once PER group. That is pure weighting bias: the RAR world gen draws from EVERY
        // group at once, so an oft-listed villain crowded out a rarely-listed one for no authored reason.
        // Safe to drop: no group lists the same villain twice, so nothing here is intentional weighting, and
        // the placed COUNT is unaffected -- placeVillainsInner() pads back up to `count` by repeating entries
        // from the (already shuffled) list. Linear scan: these lists are a few dozen entries at most.
        bool haveIt = false;
        for (auto& added : ret)
          if (added.enemyId == elem.enemyId) {
            haveIt = true;
            break;
          }
        if (!haveIt)
          ret.push_back(elem);
      }
  return ret;
}

bool CampaignBuilder::placeVillains(const ContentFactory* contentFactory, Campaign& campaign,
    const VillainCounts& counts, const optional<RetiredGames>& retired, const vector<VillainGroup>& villainGroups) {
  return placeVillainsAll(random, villains, contentFactory, campaign, counts, retired, villainGroups);
}

bool CampaignBuilder::placeVillainsAll(RandomGen& random, const VillainsTuple& villainsMap,
    const ContentFactory* contentFactory, Campaign& campaign, const VillainCounts& counts,
    const optional<RetiredGames>& retired, const vector<VillainGroup>& villainGroups) {
  int retiredLimit = counts.numMain;
  auto allMainVillains = getVillainsFrom(villainsMap, villainGroups, VillainType::MAIN);
  auto regularMainVillains = allMainVillains.filter([](auto& v) { return !v.alwaysPresent; });
  auto endGameVillains = allMainVillains.filter([](auto& v) { return v.alwaysPresent; })
      .transform([](auto& t) { return Dweller(t); });
  const int numAlwaysPresent = endGameVillains.size();
  if (retired) {
    int numRetired = min(retired->getNumActive(), min(retiredLimit, counts.maxRetired));
    endGameVillains.append(retired->getActiveGames().transform(
          [](const RetiredGames::RetiredGame& game) -> Dweller {
            return Campaign::RetiredInfo{game.gameInfo, game.fileInfo};
          }).getPrefix(numRetired));
  }
  Table<bool> blocked(campaign.sites.getBounds(), false);
  for (auto v : campaign.sites.getBounds())
    if (campaign.sites[v].blocked)
      blocked[v] = true;
  for (auto v : Rectangle::centered(campaign.playerPos, 5).intersection(blocked.getBounds()))
    blocked[v] = true;
  auto initialRadius = contentFactory->campaignInfo.initialRadius;
  if (!placeVillainsInner(random, contentFactory, campaign, blocked, shuffle(random, regularMainVillains),
      max(0, counts.numMain - numAlwaysPresent), Range(0, 1000)))
    return false;
  auto allLesser = shuffle(random, getVillainsFrom(villainsMap, villainGroups, VillainType::LESSER));
  if (!placeVillainsInner(random, contentFactory, campaign, blocked, allLesser.getPrefix(3), min(3, counts.numLesser), Range(1, initialRadius)))
    return false;
  if (allLesser.size() > 3 && counts.numLesser > 3)
    if (!placeVillainsInner(random, contentFactory, campaign, blocked, allLesser.getSubsequence(3), counts.numLesser - 3, Range(initialRadius + 2, 1000)))
      return false;
  if (!placeVillainsInner(random, contentFactory, campaign, blocked, shuffle(random, getVillainsFrom(villainsMap, villainGroups, VillainType::MINOR)),
          counts.numMinor, Range(0, 1000)) ||
      !placeVillainsInner(random, contentFactory, campaign, blocked, shuffle(random, getVillainsFrom(villainsMap, villainGroups, VillainType::ALLY)),
          counts.numAllies, Range(0, 1000)))
    return false;
  if (!placeVillainsInner(random, contentFactory, campaign, blocked, endGameVillains, endGameVillains.size(),
      Range((campaign.getSites().getBounds().width() - mapMargin) / 2 - 5, 1000)))
    return false;
  return true;
}

static bool autoConfirm(CampaignType type) {
  switch (type) {
    case CampaignType::QUICK_MAP:
      return true;
    default:
      return false;
  }
}

const vector<TString>& CampaignBuilder::getIntroMessages(CampaignType type) const {
  return gameIntros;
}

static optional<ExternalEnemiesType> getExternalEnemies(Options* options) {
  auto v = options->getIntValue(OptionId::ENDLESS_ENEMIES);
  if (v == 0)
    return none;
  if (v == 1)
    return ExternalEnemiesType::FROM_START;
  if (v == 2)
    return ExternalEnemiesType::AFTER_25K;
  if (v == 3)
    return ExternalEnemiesType::AFTER_50K;
  if (v == 4)
    return ExternalEnemiesType::AFTER_100K;
  FATAL << "Bad endless enemies value " << v;
  fail();
}

static EnemyAggressionLevel getAggressionLevel(Options* options) {
  auto v = options->getIntValue(OptionId::ENEMY_AGGRESSION);
  if (v == 0)
    return EnemyAggressionLevel::NONE;
  if (v == 1)
    return EnemyAggressionLevel::MODERATE;
  if (v == 2)
    return EnemyAggressionLevel::EXTREME;
  FATAL << "Bad enemy aggression value " << v;
  fail();
}

static int getExpIncrease(Options* options) {
  auto v = options->getIntValue(OptionId::EXP_INCREASE);
  if (v == 0)
    return 2;
  if (v == 1)
    return 3;
  if (v == 2)
    return 5;
  FATAL << "Bad exp increase value " << v;
  fail();
}

optional<CampaignSetup> CampaignBuilder::prepareCampaign(ContentFactory* contentFactory,
    function<optional<RetiredGames>(CampaignType)> genRetired,
    CampaignType type, string worldName, optional<Table<Campaign::SiteInfo>> serverSites) {
  auto& campaignInfo = contentFactory->campaignInfo;
  Vec2 size = campaignInfo.size;
  int numBlocked = 0.6 * size.x * size.y;
  auto retired = genRetired(type);
  View::CampaignMenuState menuState { true, CampaignMenuIndex{CampaignMenuElems::None{}} };
  options->setChoices(OptionId::ENDLESS_ENEMIES, {
      view->translate(TStringId("ENDLESS_ENEMIES_NONE")),
      view->translate(TStringId("ENDLESS_ENEMIES_FROM_START")),
      view->translate(TStringId("ENDLESS_ENEMIES_AFTER_25K")),
      view->translate(TStringId("ENDLESS_ENEMIES_AFTER_50K")),
      view->translate(TStringId("ENDLESS_ENEMIES_AFTER_100K"))});
  options->setChoices(OptionId::ENEMY_AGGRESSION, {
      view->translate(TStringId("ENEMY_AGGRESSION_NONE")),
      view->translate(TStringId("ENEMY_AGGRESSION_MODERATE")),
      view->translate(TStringId("ENEMY_AGGRESSION_EXTREME"))});
  options->setChoices(OptionId::EXP_INCREASE, {
      view->translate(TStringId("EXP_INCREASE_MILD")),
      view->translate(TStringId("EXP_INCREASE_NORMAL")),
      view->translate(TStringId("EXP_INCREASE_EXTREME"))});
  int worldMapIndex = 0;
  auto worldMapId = [&] {
    return contentFactory->worldMaps[worldMapIndex].layout;
  };
  // RAR online: pin the shared world's TERRAIN to a dedicated fixed-seed RNG so every client
  // generates the IDENTICAL map -- independent of keeper choice or villain-placement retries.
  // Previously a failed placeVillains -> `continue` re-ran getTerrain on the shared `random`,
  // which had already advanced, re-rolling the terrain and desyncing worlds between accounts.
  optional<int> rarTerrainSeed;
  if (rarEnabled()) {
    auto w = rarGetWorld();
    if (w.valid)
      rarTerrainSeed = w.seed;
  }
  int failedPlaceVillains = 0;
  while (1) {
    setCountLimits(campaignInfo);
    // RAR online (serverSites set): the world is SERVER-AUTHORITATIVE -- use the downloaded
    // terrain + villains verbatim, NO client generation, so every player sees the identical map.
    Table<Campaign::SiteInfo> terrain = [&] {
      if (serverSites)
        return *serverSites;
      if (rarTerrainSeed) {
        RandomGen terrainRng;
        terrainRng.init(*rarTerrainSeed);
        return getTerrain(terrainRng, contentFactory, worldMapId(), size);
      }
      return getTerrain(random, contentFactory, worldMapId(), size);
    }();
    Campaign campaign(terrain, type, worldName, getExpIncrease(options));
    campaign.mapZoom = campaignInfo.mapZoom;
    campaign.minimapZoom = campaignInfo.minimapZoom;
    if (serverSites) {
      // RAR: reconcile to the server's live roster HERE, before anything reads the map. The player picks his
      // base on this very map, so it has to already show the truth: another keeper's allies drawn as enemies
      // (ally-ness is per keeper -- see Campaign::reconcileVillains), defeated villains gone, respawns on
      // their new tiles. This used to run only AFTER prepareCampaign returned, so the site-selection map
      // showed raw server data -- allies that aren't mine appeared as allies until the game actually started.
      // Doing it first also fixes a latent bug: the default start tile below was picked against the stale
      // map, and the reconcile that followed placed roster villains BY POSITION -- so a respawn sitting on
      // that tile would overwrite the keeper dweller we had just put there.
      if (rarEnabled())
        campaign.reconcileVillains(contentFactory, rarGetVillainRoster(), avatarInfo.villainGroups,
            avatarInfo.creatureInfo.tribe.value_or(getPlayerTribeId(avatarInfo.tribeAlignment)));
      // villains already baked into `terrain`; just drop THIS player's keeper at a default good
      // start tile (they re-pick in the UI, then the atomic claim decides the collision).
      bool placed = false;
      for (auto pos : campaign.getSites().getBounds()
          .minusMargin(campaignInfo.initialRadius + 1).getAllSquares())
        if (campaign.isGoodStartPos(pos) && biomeAllowed(campaign, pos)) {
          setPlayerPos(campaign, pos, avatarInfo.playerCreature->getMaxViewIdUpgrade(), contentFactory);
          campaign.originalPlayerPos = pos;
          placed = true;
          break;
        }
      // forcedBiome and this world has no free tile of it -> REFUSE to create the keeper. Letting it through
      // would drop the keeper on a biome its whole design contradicts, and nothing re-checks after creation.
      if (!placed && !avatarInfo.creatureInfo.forcedBiome.empty()) {
        view->presentText(none, TSentence("KEEPER_BIOME_UNAVAILABLE",
            forcedBiomeNames(contentFactory, avatarInfo.creatureInfo.forcedBiome)));
        return none;
      }
      campaign.updateInhabitants(contentFactory);
    } else {
      RandomGen& startPosRng = rarEnabled() ? random : Random;
      bool placed = false;
      for (auto pos : startPosRng.permutation(campaign.getSites().getBounds()
          .minusMargin(campaignInfo.initialRadius + 1).getAllSquares())) {
        if ((campaign.isGoodStartPos(pos) || type == CampaignType::QUICK_MAP) && biomeAllowed(campaign, pos)) {
          setPlayerPos(campaign, pos, avatarInfo.playerCreature->getMaxViewIdUpgrade(), contentFactory);
          campaign.originalPlayerPos = pos;
          placed = true;
          break;
        }
      }
      if (!placed && !avatarInfo.creatureInfo.forcedBiome.empty()) {
        view->presentText(none, TSentence("KEEPER_BIOME_UNAVAILABLE",
            forcedBiomeNames(contentFactory, avatarInfo.creatureInfo.forcedBiome)));
        return none;
      }
      const auto villainCounts = getVillainCounts(type, options);
      if (!placeVillains(contentFactory, campaign, villainCounts, retired, avatarInfo.villainGroups)) {
        if (++failedPlaceVillains > 300)
          USER_FATAL << "Failed to place all villains on the world map";
        continue;
      }
      failedPlaceVillains = 0;
      campaign.updateInhabitants(contentFactory);
    }
    // RAR online: fetch every claimed site so they show on the map (with keeper name) and can't be picked.
    // ALL claims block, including this account's own other keepers: the keeper being created has no claim yet,
    // and an existing base of ours is just as occupied as anyone else's -- excluding them by login let you
    // drop a new keeper straight on top of your own base, and hid it from the placement map.
    vector<pair<Vec2, TString>> claimedSites;
    if (rarEnabled())
      for (auto& claim : rarGetClaims())
        claimedSites.push_back(make_pair(Vec2(claim.x, claim.y), TString(claim.name)));
    while (1) {
      bool updateMap = false;
      campaign.refreshInfluencePos(contentFactory);
      CampaignAction action = autoConfirm(type) ? CampaignActionId::CONFIRM
          : view->prepareCampaign(View::CampaignOptions {
              campaign,
              (retired && type == CampaignType::FREE_PLAY && !rarEnabled()) ? optional<RetiredGames&>(*retired) : none, // RAR: no retired-dungeons panel online
              getCampaignOptions(type),
              TStringId("CAMPAIGN_HELP_TEXT"),
              contentFactory->biomeInfo.at(*campaign.getSites()[campaign.getPlayerPos()].biome).name,
              contentFactory->worldMaps.transform([](auto& elem) { return elem.name; }),
              worldMapIndex,
              claimedSites
            },
              menuState);
      switch (action.getId()) {
        case CampaignActionId::REROLL_MAP:
          if (!rarEnabled()) { // online: world is server-fixed, ignore reroll
            terrain = getTerrain(random, contentFactory, worldMapId(), size);
            updateMap = true;
          }
          break;
        case CampaignActionId::UPDATE_MAP:
          updateMap = true;
          break;
        case CampaignActionId::SET_POSITION:
          if (canSettleOn(campaign, action.get<Vec2>(), contentFactory))
            setPlayerPos(campaign, action.get<Vec2>(), avatarInfo.playerCreature->getMaxViewIdUpgrade(),
                contentFactory);
          break;
        case CampaignActionId::CHANGE_WORLD_MAP:
          if (!rarEnabled()) { // online: world map is server-fixed, ignore switching
            worldMapIndex = action.get<int>();
            retired = genRetired(type);
            updateMap = true;
          }
          break;
        case CampaignActionId::UPDATE_OPTION:
          switch (action.get<OptionId>()) {
            case OptionId::PLAYER_NAME:
            case OptionId::ENDLESS_ENEMIES:
            case OptionId::ENEMY_AGGRESSION:
              break;
            default:
              updateMap = true;
              break;
          }
          break;
        case CampaignActionId::CANCEL:
          return none;
        case CampaignActionId::CONFIRM:
          { // RAR: removed the "add retired dungeons?" confirm prompt (no retired dungeons online)
            string gameIdentifier;
            TString gameDisplayName;
            string keeperName; // the clean keeper name -- becomes the identity + the save filename
            if (avatarInfo.chosenBaseName) {
              keeperName = *avatarInfo.chosenBaseName;
              gameDisplayName = capitalFirst(TSentence("OF",
                  avatarInfo.playerCreature->getName().plural(), TString(*avatarInfo.chosenBaseName)));
            } else {
              keeperName = avatarInfo.playerCreature->getName().first().value_or(
                  view->translate(avatarInfo.playerCreature->getName().bare()));
              gameDisplayName = TSentence("OF", TString(keeperName), TString(campaign.worldName));
            }
            keeperName = stripFilename(std::move(keeperName));
            // ONLINE IDENTITY: "<account>~<keeper>" -- deterministic, so a re-save always overwrites the same
            // slot (an account can never end up with two keepers of the same name). No random suffix, no world.
            // Offline keeps the old unique-id form, but the game is online-only in practice.
            if (rarEnabled())
              gameIdentifier = rarComposeGameId(rarSessionLogin(), keeperName);
            else
              gameIdentifier = stripFilename(keeperName + "_" + campaign.worldName + getNewIdSuffix());
            if (rarEnabled()) { // online: temporarily claim this start site on the shared world
              auto pos = campaign.getPlayerPos();
              if (!rarClaimSite(gameIdentifier, keeperName, pos.x, pos.y)) {
                view->presentText(none, TString("That start site is already taken. Please choose a different one."_s));
                break; // back to the campaign map to pick another site
              }
            }
            auto aggressionLevel = avatarInfo.creatureInfo.enemyAggression
                ? getAggressionLevel(options)
                : EnemyAggressionLevel::NONE;
            campaign.refreshMaxAggressorCutOff();
            return CampaignSetup{campaign, gameIdentifier, gameDisplayName,
                {}, getExternalEnemies(options), aggressionLevel}; // RAR: removed the "Welcome to KeeperRL" intro message
          }
      }
      if (updateMap)
        break;
    }
  }
}

CampaignSetup CampaignBuilder::getEmptyCampaign() {
  Campaign ret(Table<Campaign::SiteInfo>(1, 1), CampaignType::QUICK_MAP, "", 1);
  return CampaignSetup{ret, "", TString(), {}, none, EnemyAggressionLevel::MODERATE};
}

optional<Campaign> CampaignBuilder::reconstructKeeperCampaign(RandomGen& random, ContentFactory* factory,
    Table<Campaign::SiteInfo> worldSites, BiomeId biome, ViewIdList playerViewId, TribeId tribe,
    const string& worldName, optional<Vec2> preferredPos, const vector<Vec2>& claimedByOthers) {
  // Candidate tiles: empty (no dweller/villain), not blocked, not held by another keeper, and the SAME biome
  // as the imported base. So a desert keeper re-homes onto desert, a forest keeper onto forest, etc.
  auto tileOk = [&](Vec2 v) {
    return v.inRectangle(worldSites.getBounds()) && worldSites[v].isEmpty() && !worldSites[v].blocked
        && worldSites[v].biome == biome && !claimedByOthers.contains(v);
  };
  vector<Vec2> candidates;
  for (Vec2 v : worldSites.getBounds())
    if (tileOk(v))
      candidates.push_back(v);
  // Stay put when the keeper's existing tile still works in THIS world (a regen may not have changed it) --
  // only relocate when the tile became the wrong biome / got occupied / fell outside the map.
  if (preferredPos && tileOk(*preferredPos))
    candidates = { *preferredPos };
  if (candidates.empty())
    return none;
  Vec2 pos = candidates[random.get(candidates.size())];
  Campaign campaign(std::move(worldSites), CampaignType::FREE_PLAY, worldName, 1);
  // Drop the keeper on the chosen empty tile (pos is empty, so no villain is overwritten). Mirrors
  // CampaignBuilder::setPlayerPos without the reset-old-pos step (there is no prior keeper here).
  campaign.playerPos = pos;
  campaign.originalPlayerPos = pos; // influence radius + aggressor cutoff are anchored to this
  campaign.sites[pos].dweller = Campaign::SiteInfo::Dweller(Campaign::KeeperInfo{playerViewId, tribe});
  campaign.updateInhabitants(factory);
  // The Campaign ctor leaves mapZoom UNINITIALIZED (a hand-built campaign), so the keeper's world map renders
  // at zoom 0 -> only ~1 tile visible. Set the config zoom like a normally-generated campaign.
  campaign.setRenderZoom(factory->campaignInfo.mapZoom, factory->campaignInfo.minimapZoom);
  // Build the derived tables that are SERIALIZED but computed, not constructed: without these
  // belowMaxAgressorCutOff stays a default 0x0 Table and passesMaxAggressorCutOff(pos) crashes on the
  // client after load (FATAL util.h:981, Table index out of bounds at the base pos). Same pair the normal
  // campaign flow (reconcileVillains / setDefeated) runs after any change.
  campaign.refreshInfluencePos(factory);
  campaign.refreshMaxAggressorCutOff();
  return campaign;
}

// RAR online: generate the canonical shared-world sites (terrain + villains, NO keeper dweller).
// Villain distances are anchored to a fixed deterministic reference tile; each real player adds
// their own keeper at the tile they claim. Deterministic given `random`.
optional<Campaign> CampaignBuilder::previewLayoutCampaign(RandomGen& random, ContentFactory* factory,
    const string& layoutName, Vec2 size, bool worldMap, int zoom, const string& mappingName) {
  // Dev tool: run ANY named layout from random_layouts.txt at a chosen size and wrap the result in a Campaign
  // so it can be shown graphically. Returns none if generation fails (no CHECK crash), so the caller can show a
  // message. World maps render at the config zoom; dungeons zoom in for tile detail.
  RandomLayoutId id(layoutName.data());
  if (!factory->randomLayouts.count(id))
    return none;
  unordered_set<Token> allViewIds;
  for (auto& def : factory->tilePaths.definitions)
    allViewIds.insert(def.viewId.data());
  LayoutCanvas::Map map{Table<vector<Token>>(Rectangle(Vec2(0, 0), size))};
  LayoutCanvas canvas{map.elems.getBounds(), &map};
  bool ok = false;
  for (int i : Range(20))
    if (factory->randomLayouts.at(id).make(canvas, random)) { ok = true; break; }
  if (!ok)
    return none;
  // Dungeon tokens are NOT tile viewIds -- they're keys in a layout_mapping (e.g. "rock" -> Place "MOUNTAIN").
  // Resolve the chosen mapping and turn each token into the furniture it places, then that furniture's viewId.
  // World maps skip this: their tokens ARE map_ tile viewIds / biome names.
  const LayoutMapping* mapping = nullptr;
  if (!worldMap) {
    LayoutMappingId mid(mappingName.data());
    if (factory->layoutMapping.count(mid))
      mapping = &factory->layoutMapping.at(mid);
  }
  Table<Campaign::SiteInfo> ret(size, {});
  for (Vec2 v : ret.getBounds()) {
    if (worldMap) {
      for (auto& token : map.elems[v]) {
        if (token == "blocked")
          ret[v].blocked = true;
        else if (allViewIds.count(token))
          ret[v].viewId.push_back(ViewId(token.data())); // stacked -> layers
        else
          for (auto& biome : factory->biomeInfo)
            if (token == biome.first.data())
              ret[v].biome = biome.first;
      }
    } else {
      // One furniture per layer (a later Place on a layer overwrites; ClearFurniture/ClearLayer remove),
      // applied in token order -- mirrors LevelBuilder::putFurniture. map<> iterates in FurnitureLayer order
      // (GROUND<FLOOR<MIDDLE<CEILING) = the in-game draw order.
      std::map<FurnitureLayer, ViewId> layers; // std:: -- the local 'map' (LayoutCanvas::Map) shadows the alias
      auto place = [&](FurnitureType type) {
        auto& data = factory->furniture.getData(type);
        if (data.getViewObject())
          layers[data.getLayer()] = data.getViewObject()->id();
      };
      function<void(const LayoutAction&)> apply = [&](const LayoutAction& a) {
        if (auto chain = a.getReferenceMaybe<LayoutActions::Chain>()) {
          for (auto& sub : *chain)
            apply(sub);
        } else if (auto type = a.getReferenceMaybe<FurnitureType>()) // LayoutActions::Place
          place(*type);
        else if (auto ph = a.getReferenceMaybe<LayoutActions::PlaceHostile>())
          place(ph->type);
        else if (auto st = a.getReferenceMaybe<LayoutActions::Stairs>())
          place(st->type);
        else if (a.contains<LayoutActions::ClearFurniture>())
          layers.clear();
        else if (auto cl = a.getReferenceMaybe<LayoutActions::ClearLayer>())
          layers.erase(*cl);
      };
      for (auto& token : map.elems[v]) {
        if (token == "blocked")
          ret[v].blocked = true;
        else if (mapping)
          if (auto action = getReferenceMaybe(mapping->actions, token))
            apply(*action);
      }
      for (auto& layer : layers)
        ret[v].viewId.push_back(layer.second);
    }
  }
  Campaign campaign(std::move(ret), CampaignType::FREE_PLAY, "Layout preview: " + layoutName, 1);
  // Render zoom = tile-size multiplier (mouse-wheel controlled). Dungeons render at 24px cells, so zoom 1 is
  // already native; world maps use 8px cells and start at the config zoom for a comparable on-screen size.
  campaign.setRenderZoom(zoom, factory->campaignInfo.minimapZoom);
  return campaign;
}

Table<Campaign::SiteInfo> CampaignBuilder::generateServerWorldSites(RandomGen& random,
    ContentFactory* contentFactory, Options* options, const VillainsTuple& villains,
    const vector<VillainGroup>& groups, int terrainSeed, RandomLayoutId worldMapId) {
  auto& campaignInfo = contentFactory->campaignInfo;
  Vec2 size = campaignInfo.size;
  // RAR: the shared world's villain counts come straight from campaign_info.txt (maxXxxVillains), NOT the
  // per-player option sliders -- so the admin controls the world size purely via config (unhardcoded).
  VillainCounts counts { campaignInfo.maxMainVillains, campaignInfo.maxLesserVillains,
      campaignInfo.maxMinorVillains, campaignInfo.maxAllies, 0 };
  for (int attempt : Range(500)) {
    // Terrain from its OWN freshly-seeded RNG every attempt -> identical map each time; only the villain
    // placement below draws from `random`, which advances, so a retry re-rolls villains and not the world.
    // This is what makes a seed reproducible: the same seed + size + content always gives the same terrain.
    RandomGen terrainRng;
    terrainRng.init(terrainSeed);
    Table<Campaign::SiteInfo> terrain = getTerrain(terrainRng, contentFactory, worldMapId, size);
    Campaign campaign(terrain, CampaignType::FREE_PLAY, "server", getExpIncrease(options));
    campaign.mapZoom = campaignInfo.mapZoom;
    campaign.minimapZoom = campaignInfo.minimapZoom;
    optional<Vec2> anchor;
    for (auto pos : random.permutation(campaign.getSites().getBounds()
        .minusMargin(campaignInfo.initialRadius + 1).getAllSquares()))
      if (campaign.isGoodStartPos(pos)) {
        anchor = pos;
        break;
      }
    if (!anchor)
      continue;
    campaign.playerPos = *anchor;
    campaign.originalPlayerPos = *anchor;
    if (placeVillainsAll(random, villains, contentFactory, campaign, counts, none, groups)) {
      campaign.updateInhabitants(contentFactory);
      return campaign.sites;
    }
  }
  USER_FATAL << "Failed to generate the server world map";
  return Table<Campaign::SiteInfo>(size, {});
}
