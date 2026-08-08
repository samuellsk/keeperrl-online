#include "stdafx.h"
#include "campaign.h"
#include "view_id.h"
#include "model.h"
#include "progress_meter.h"
#include "campaign_type.h"
#include "villain_type.h"
#include "pretty_archive.h"
#include "perlin_noise.h"
#include "content_factory.h"
#include "enemy_info.h"
#include "tribe.h"
#include "monster_ai.h"
#include "creature.h"

SERIALIZATION_CONSTRUCTOR_IMPL(Campaign);

template <class Archive>
void Campaign::serialize(Archive& ar, const unsigned int version) {
  ar(sites, playerPos, worldName, defeated, influencePos, type, mapZoom, minimapZoom, originalPlayerPos, belowMaxAgressorCutOff);
  if (version == 1)
    ar(expIncrease);
}

SERIALIZABLE(Campaign);

void VillainViewId::serialize(PrettyInputArchive& ar1, unsigned int) {
  if (ar1.peek() == "{" && ar1.peek(2) == "{")
    ar1(ids);
  else {
    ViewId id;
    ar1(id);
    ids.push_back(id);
  }
}

const Table<Campaign::SiteInfo>& Campaign::getSites() const {
  return sites;
}

CampaignType Campaign::getType() const {
  return type;
}

bool Campaign::canTravelTo(Vec2 pos) const {
  return isInInfluence(pos) && !sites[pos].isEmpty();
}

Vec2 Campaign::getPlayerPos() const {
  return playerPos;
}

Vec2 Campaign::getOriginalPlayerPos() const {
  return originalPlayerPos;
}

BiomeId Campaign::getBaseBiome() const {
  return *sites[playerPos].biome;
}

Campaign::Campaign(Table<SiteInfo> s, CampaignType t, const string& w, int expIncrease)
    : sites(s), worldName(w), defeated(sites.getBounds(), false), type(t), expIncrease(expIncrease) {
}

bool Campaign::isGoodStartPos(Vec2 pos) const {
  for (auto v : Rectangle::centered(pos, 1))
    if (v.inRectangle(sites.getBounds()) && !!sites[v].dweller &&
        !sites[v].dweller->contains<Campaign::KeeperInfo>())
      return false;
  return !!sites[pos].biome;
}

const string& Campaign::getWorldName() const {
  return worldName;
}

bool Campaign::isDefeated(Vec2 pos) const {
  return defeated[pos];
}

void Campaign::removeDweller(Vec2 pos) {
  sites[pos].dweller = none;
}

void Campaign::setDweller(Vec2 pos, SiteInfo::Dweller dweller) {
  sites[pos].dweller = std::move(dweller);
}

void Campaign::reconcileVillains(const ContentFactory* f, const vector<RarVillain>& roster,
    const vector<VillainGroup>& myGroups, TribeId playerTribe) {
  // TRIBES OVERRULE THE VILLAIN GROUP. A villain group says what ROLE a faction plays on the world map; the
  // tribe graph says who actually fights whom. They are independent, so a group could offer an alliance with a
  // faction my minions attack on sight -- e.g. TOMB is enemyOfAll with allies={ANCIENT_TOMB}, yet EVIL_KEEPER
  // lists GNOMES as ALLY. Build the same graph the game runs on (generateTribes, straight from tribes.txt) and
  // let it veto: not friendly -> not an ally, whatever the group says.
  auto tribeGraph = Tribe::generateTribes(f->tribes);
  auto myTribe = tribeGraph.find(playerTribe);
  auto tribeSaysEnemy = [&](const EnemyId& enemyId) {
    if (myTribe == tribeGraph.end())
      return false; // keeper tribe isn't in tribes.txt -- can't judge, so leave the group's answer alone
    auto e = f->enemies.find(enemyId);
    if (e == f->enemies.end())
      return false; // unknown faction (mod removed it): not our call to make
    auto other = tribeGraph.find(e->second.settlement.tribe);
    return other != tribeGraph.end() && myTribe->second->isEnemy(other->second.get());
  };
  // PER-KEEPER ALLIES. The RAR world is ONE shared map generated from EVERY villain group, so it contains both
  // the evil keeper's allies (GNOMES) and the lawful keeper's (TEUTONS). Single-player never has this problem
  // -- it builds the map from the avatar's own villainGroups. Here the map is shared, so ally-ness has to be
  // decided per keeper at reconcile time: a faction is an ALLY only if it's an ally in MY villainGroups;
  // anyone else's ally is just an enemy to me. Same world, different reading per keeper.
  map<string, SiteInfo::Dweller> mine; // MY groups win -- keeps my allies allied, my enemies as configured
  for (auto& g : myGroups) {
    auto git = f->villains.find(g);
    if (git != f->villains.end())
      for (auto& vi : git->second) {
        auto copy = vi;
        if (copy.type == VillainType::ALLY && tribeSaysEnemy(copy.enemyId)) {
          copy.type = VillainType::MINOR;      // same demotion (and same reasons) as someone else's ally below
          copy.convertedFromAlly = true;
        }
        mine.emplace(copy.enemyId.data(), SiteInfo::Dweller(copy));
      }
  }
  // Fallback: factions from OTHER keepers' groups still sit on the shared map and must be drawn, so we need
  // their viewId/name -- but NOT their ally-ness.
  map<string, Campaign::VillainInfo> others;
  for (auto& group : f->villains)
    for (auto& vi : group.second)
      others.emplace(vi.enemyId.data(), vi);
  auto dwellerFor = [&](const string& enemyId) -> optional<SiteInfo::Dweller> {
    auto it = mine.find(enemyId);
    if (it != mine.end())
      return it->second; // in my groups -> exactly as my config says (ally stays ally)
    auto it2 = others.find(enemyId);
    if (it2 == others.end())
      return none;
    auto vi = it2->second;
    if (vi.type == VillainType::ALLY) {
      // Someone else's ally, not mine -> an enemy to me. MINOR on purpose: blocksInfluence() is true only for
      // MAIN/LESSER, so MINOR flips the map to "enemy" WITHOUT silently re-shaping travel influence, distance
      // difficulty scaling or the aggressor cutoff -- which promoting it to LESSER/MAIN would.
      vi.type = VillainType::MINOR;
      vi.convertedFromAlly = true; // mark it so the world map paints it ORANGE (a demoted ally, not a native minor)
    }
    return SiteInfo::Dweller(vi); // already an enemy tier in their group -> it's an enemy to us anyway
  };
  // Clear every current villain dweller AND its defeated flag (keeper/retired dwellers untouched); the
  // roster below re-derives both, so removed villains vanish and respawns/corpses are placed fresh.
  for (Vec2 v : sites.getBounds())
    if (sites[v].getVillain()) {
      sites[v].dweller = none;
      defeated[v] = false;
    }
  // Place exactly the server's roster (its biome-matched interior is downloaded on invade). A roster entry
  // flagged defeated is a lootable corpse in its grace window -> mark it defeated so the world map draws the
  // vanilla campaign_defeated sprite (and hides the live one), while it stays revisitable to loot.
  for (auto& rv : roster) {
    Vec2 p(rv.x, rv.y);
    if (p.inRectangle(sites.getBounds()))
      if (auto d = dwellerFor(rv.enemyId)) {
        sites[p].dweller = std::move(*d);
        defeated[p] = rv.defeated;
      }
  }
  refreshInfluencePos(f);
  refreshMaxAggressorCutOff();
}

void Campaign::setDefeated(const ContentFactory* f, Vec2 pos) {
  defeated[pos] = true;
  refreshInfluencePos(f);
  refreshMaxAggressorCutOff();
}

bool Campaign::VillainInfo::isEnemy() const {
  return type != VillainType::ALLY;
}

void Campaign::updateInhabitants(ContentFactory* factory) {
  for (auto pos : sites.getBounds()) {
    auto& site = sites[pos];
    site.inhabitants.clear();
    if (site.dweller)
      site.dweller->match(
          [&](const VillainInfo& info) {
            auto& inhabitants = factory->enemies.at(info.enemyId).settlement.inhabitants;
            auto creatures = inhabitants.leader.generate(Random,
                &factory->getCreatures(), TribeId::getMonster(), MonsterAIFactory::monster());
            creatures.append(inhabitants.fighters.generate(Random,
                &factory->getCreatures(), TribeId::getMonster(), MonsterAIFactory::monster()));
            auto exp = getBaseLevelIncrease(pos);
            for (auto& c : creatures) {
              c->setCombatExperience(exp);
              site.inhabitants.push_back(SavedGameInfo::MinionInfo::get(factory, c.get()));
              if (site.inhabitants.size() >= 4)
                break;
            }
          },
          [&](const RetiredInfo& info) { site.inhabitants = info.gameInfo.minions ;},
          [&](const KeeperInfo&) { site.inhabitants.clear(); });
  }
}

optional<Campaign::VillainInfo> Campaign::SiteInfo::getVillain() const {
  if (dweller)
    return dweller->getValueMaybe<VillainInfo>();
  return none;
}

VillainType Campaign::VillainInfo::getDisplayType() const {
  return convertedFromAlly ? VillainType::LESSER : type;
}

bool Campaign::SiteInfo::isConvertedAlly() const {
  auto v = getVillain();
  return v && v->convertedFromAlly;
}

optional<Campaign::KeeperInfo> Campaign::SiteInfo::getKeeper() const {
  if (dweller)
    return dweller->getValueMaybe<KeeperInfo>();
  return none;
}

optional<Campaign::RetiredInfo> Campaign::SiteInfo::getRetired() const {
  if (dweller)
    return dweller->getValueMaybe<RetiredInfo>();
  return none;
}

bool Campaign::SiteInfo::isEmpty() const {
  return !dweller;
}

optional<TString> Campaign::SiteInfo::getDwellerDescription() const {
  if (dweller)
    return dweller->match(
        [](const VillainInfo& info) ->TString {
          return TSentence("VILLAIN_NAME_AND_DESCRIPTION", info.name, getName(info.type));
        },
        [](const RetiredInfo& info) ->TString  { return TSentence("RETIRED_PLAYER_DESCRIPTION", TString(info.gameInfo.name)) ;},
        [](const KeeperInfo&) ->TString { return TStringId("PLAYER_SITE_DESCRIPTION"); });
  else
    return none;
}

optional<TString> Campaign::SiteInfo::getDwellerName() const {
  if (dweller)
    return dweller->match(
        [](const VillainInfo& info) -> TString { return info.name; },
        [](const RetiredInfo& info) -> TString { return TString(info.gameInfo.name);},
        [](const KeeperInfo&) -> TString { return TStringId("PLAYER_SITE_NAME"); });
  else
    return none;
}

optional<VillainType> Campaign::SiteInfo::getVillainType() const {
  if (dweller)
    return dweller->match(
        [](const VillainInfo& info) { return info.type; },
        [](const RetiredInfo&) { return VillainType::RETIRED; },
        [](const KeeperInfo&) { return VillainType::PLAYER; });
  else
    return none;
}

optional<ViewIdList> Campaign::SiteInfo::getDwellingViewId() const {
  if (dweller)
    return dweller->match(
        [](const VillainInfo& info) { return ViewIdList{{info.dwellingId}}; },
        [](const RetiredInfo& info) { return ViewIdList{{ViewId("map_dungeon1")}}; },
        [](const KeeperInfo& info) { return ViewIdList{{ViewId("map_base1")}}; });
  else
    return none;
}

bool Campaign::SiteInfo::isEnemy() const {
  return getRetired() || (getVillain() && getVillain()->isEnemy());
}

bool Campaign::isInInfluence(Vec2 pos) const {
  return influencePos.count(pos);
}

int Campaign::getBaseLevelIncrease(Vec2 pos) const {
  double dist = pos.distD(playerPos);
  int res = 0;
  for (Vec2 v : sites.getBounds())
    if (blocksInfluence(sites[v].getVillainType().value_or(VillainType::NONE)) && v.distD(playerPos) < dist)
      ++res;
  return res * expIncrease;
}

bool Campaign::passesMaxAggressorCutOff(Vec2 pos) {
  // belowMaxAgressorCutOff is a COMPUTED table that is also serialized. A reconstructed/round-tripped campaign
  // (e.g. the export/import base pipeline) can deserialize it with stale or garbage bounds -> a raw index would
  // FATAL (util.h:981). Self-heal: if it doesn't match the live sites, recompute it; and treat an out-of-world
  // position as failing the cutoff instead of crashing.
  if (belowMaxAgressorCutOff.getBounds() != sites.getBounds())
    refreshMaxAggressorCutOff();
  if (!pos.inRectangle(belowMaxAgressorCutOff.getBounds()))
    return false;
  return belowMaxAgressorCutOff[pos];
}

constexpr int maxAggressorDiff = 10;

void Campaign::refreshMaxAggressorCutOff() {
  belowMaxAgressorCutOff = Table<bool>(sites.getBounds(), false);
  auto maxConquered = 0;
  for (Vec2 v : sites.getBounds())
    if (blocksInfluence(sites[v].getVillainType().value_or(VillainType::NONE)) && defeated[v])
      maxConquered = max(maxConquered, getBaseLevelIncrease(v));
  for (Vec2 v : sites.getBounds())
    belowMaxAgressorCutOff[v] = getBaseLevelIncrease(v) <= maxConquered + maxAggressorDiff;
}

void Campaign::refreshInfluencePos(const ContentFactory* f) {
  map<Vec2, Vec2> siteOwners;
  map<Vec2, HashSet<Vec2>> territories;
  influencePos.clear();
  const int initialRadius = f->campaignInfo.initialRadius;
  for (auto v : Rectangle::centered(originalPlayerPos, initialRadius))
    if (v.distD(originalPlayerPos) <= initialRadius + 0.5)
      influencePos.insert(v);
  bool hasDefeated = [&] {
    for (auto v : sites.getBounds())
      if (defeated[v])
        return true;
    return false;
  }();
  struct QueueElem {
    Vec2 pos;
    double dist;
    Vec2 owner;
    bool operator < (const QueueElem& e) const {
      return dist > e.dist;
    }
  };
  priority_queue<QueueElem> q;
  for (int x : sites.getBounds().getXRange()) {
    q.push(QueueElem{Vec2(x, 0), 0, Vec2(0, 0)});
    q.push(QueueElem{Vec2(x, sites.getBounds().bottom() - 1), 0, Vec2(0, 0)});
  }
  for (int y : sites.getBounds().getYRange()) {
    q.push(QueueElem{Vec2(0, y), 0, Vec2(0, 0)});
    q.push(QueueElem{Vec2(sites.getBounds().right() - 1, y), 0, Vec2(0, 0)});
  }
  for (Vec2 v : sites.getBounds())
    if (!!sites[v].dweller && blocksInfluence(*sites[v].getVillainType()))
      q.push(QueueElem{v, 0, v});
  RandomGen gen;
  gen.init(2134);
  auto noiseTable = genNoiseMap(gen, sites.getBounds(), NoiseInit { 1, 1, 1, 1, 1 }, 0.65);
  while (!q.empty()) {
    auto e = q.top();
    q.pop();
    if (siteOwners.count(e.pos))
      continue;
    siteOwners[e.pos] = e.owner;
    if (e.owner != Vec2(0, 0) || e.dist < 15)
      for (auto v : e.pos.neighbors4())
        if (v.inRectangle(sites.getBounds()) && !sites[v].blocked && !siteOwners.count(v))
          q.push(QueueElem({v, e.dist + 3 * noiseTable[v], e.owner}));
  }
  for (auto& elem : siteOwners)
    if (elem.second != Vec2(0, 0))
      territories[elem.second].insert(elem.first);
  for (auto& elem : territories) {
    for (auto pos : elem.second)
      for (auto v : pos.neighbors4())
        if (v.inRectangle(sites.getBounds()))
          if (auto owner = getValueMaybe(siteOwners, v))
            if (defeated[*owner]) {
              influencePos.insert(elem.second.begin(), elem.second.end());
              goto nextTerritory;
            }
    nextTerritory:
    continue;
  }
  CHECK(!influencePos.empty());
}

int Campaign::getNumNonEmpty() const {
  int ret = 0;
  for (Vec2 v : sites.getBounds())
    if (!sites[v].isEmpty())
      ++ret;
  return ret;
}

int Campaign::getMapZoom() const {
  return mapZoom;
}

void Campaign::setRenderZoom(int mz, int mmz) {
  mapZoom = mz;
  minimapZoom = mmz;
}

int Campaign::getMinimapZoom() const {
  return minimapZoom;
}

map<string, string> Campaign::getParameters() const {
  int numMain = 0;
  int numLesser = 0;
  int numAlly = 0;
  int numRetired = 0;
  for (Vec2 v : sites.getBounds())
    if (sites[v].getRetired())
      ++numRetired;
    else if (auto villain = sites[v].getVillain())
      switch (villain->type) {
        case VillainType::ALLY: ++numAlly; break;
        case VillainType::MAIN: ++numMain; break;
        case VillainType::LESSER: ++numLesser; break;
        default: break;
      }
  auto gameType = EnumInfo<CampaignType>::getString(type);
  if (type == CampaignType::QUICK_MAP)
    gameType = "WARLORD";
  return {
    {"main", toString(numMain)},
    {"lesser", toString(numLesser)},
    {"allies", toString(numAlly)},
    {"retired", toString(numRetired)},
    {"game_type", gameType},
  };
}

