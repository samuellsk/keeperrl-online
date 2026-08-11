#include "owner_pointer.h"
#include "stdafx.h"
#include "game.h"
#include "view.h"
#include "clock.h"
#include "tribe.h"
#include "music.h"
#include "player_control.h"
#include "village_control.h"
#include "model.h"
#include "creature.h"
#include "spectator.h"
#include "statistics.h"
#include "collective.h"
#include "options.h"
#include "territory.h"
#include "level.h"
#include "highscores.h"
#include "player.h"
#include "item_factory.h"
#include "item.h"
#include "map_memory.h"
#include "creature_attributes.h"
#include "name_generator.h"
#include "campaign.h"
#include "save_file_info.h"
#include "rar_client.h"
#include "file_sharing.h"
#include "villain_type.h"
#include "attack_trigger.h"
#include "view_object.h"
#include "campaign.h"
#include "construction_map.h"
#include "campaign_builder.h"
#include "campaign_type.h"
#include "game_save_type.h"
#include "collective_config.h"
#include "attack_behaviour.h"
#include "village_behaviour.h"
#include "collective_builder.h"
#include "game_event.h"
#include "version.h"
#include "content_factory.h"
#include "collective_name.h"
#include "avatar_info.h"
#include "scripted_ui.h"
#include "scripted_ui_data.h"
#include "body.h"
#include "enemy_aggression_level.h"
#include "unlocks.h"
#include "steam_achievements.h"
#include "progress_meter.h"
#include "task.h"                 // RAR villain waves: Task::attackCreatures
#include "monster_ai.h"
#include "collective_attack.h"
#include "enemy_info.h"
#include "creature_list.h"
#include "village_behaviour.h"
#include "attack_trigger.h"
#include "collective_config.h"
#include "construction_map.h"
#include "inhabitants_info.h"
#include "settlement_info.h"

template <class Archive>
void Game::serialize(Archive& ar, const unsigned int version) {
  ar & SUBCLASS(OwnedObject<Game>);
  ar(villainsByType, collectives, lastTick, playerControl, playerCollective, currentTime, avatarId, numLesserVillainsDefeated);
  ar(musicType, statistics, tribes, gameIdentifier, players, contentFactory, sunlightTimeOffset, allianceAttackPossible);
  ar(gameDisplayName, models, visited, baseModel, campaign, localTime, turnEvents, effectFlags, zLevelGroups);
  if (version == 1)
    ar(enemyAggressionLevel);
  else if (Archive::is_loading::value)
    enemyAggressionLevel = EnemyAggressionLevel::NONE;
  if (Archive::is_loading::value)
    sunlightInfo.update(getGlobalTime() + sunlightTimeOffset);
}

SERIALIZABLE(Game);
SERIALIZATION_CONSTRUCTOR_IMPL(Game);

Game::Game(Table<PModel>&& m, Vec2 basePos, const CampaignSetup& c, ContentFactory f)
    : models(std::move(m)), visited(models.getBounds(), false), baseModel(basePos),
      musicType(MusicType::PEACEFUL), campaign(c.campaign),
      contentFactory(std::move(f)) {
  // Tribes come from content now (tribes.txt), so build them AFTER contentFactory is set -- not in the init
  // list, where contentFactory hasn't been moved in yet.
  tribes = Tribe::generateTribes(contentFactory->tribes);
  for (auto pos : models.getBounds())
    if (models[pos])
      models[pos]->position = pos;
  gameIdentifier = c.gameIdentifier;
  gameDisplayName = c.gameDisplayName;
  enemyAggressionLevel = c.enemyAggressionLevel;
  for (Vec2 v : models.getBounds())
    if (Model* m = models[v].get()) {
      for (Collective* col : m->getCollectives()) {
        auto control = dynamic_cast<VillageControl*>(col->getControl());
        control->updateAggression(c.enemyAggressionLevel);
        addCollective(col);
      }
      m->updateSunlightMovement();
      for (auto c : m->getAllCreatures())
        c->setGlobalTime(getGlobalTime());
    }
  turnEvents = {0, 10, 50, 100, 300, 500};
  for (int i : Range(200))
    turnEvents.insert(1000 * (i + 1));
}

EnemyAggressionLevel Game::getEnemyAggressionLevel() const {
  return enemyAggressionLevel;
}

void Game::addCollective(Collective* col) {
  if (!collectives.contains(col)) {
    collectives.push_back(col);
    auto type = col->getVillainType();
    villainsByType[type].push_back(col);
  }
}

void Game::spawnKeeper(AvatarInfo avatarInfo, vector<TString> introText) {
  auto model = getMainModel().get();
  Level* level = model->getGroundLevel();
  Creature* keeperRef = avatarInfo.playerCreature.get();
  CHECK(level->landCreature(StairKey::keeperSpawn(), keeperRef)) << "Couldn't place keeper on level.";
  model->addCreature(std::move(avatarInfo.playerCreature));
  auto& keeperInfo = avatarInfo.creatureInfo;
  auto builder = CollectiveBuilder(CollectiveConfig::keeper(
          TimeInterval(keeperInfo.immigrantInterval), keeperInfo.maxPopulation, keeperInfo.populationString,
          keeperInfo.prisoners, keeperInfo.prisonerPredicate, ConquerCondition::KILL_LEADER,
          keeperInfo.requireQuartersForExp),
      keeperRef->getTribeId(), "keeper collective")
      .setModel(model)
      .addCreature(keeperRef, keeperInfo.minionTraits);
  if (avatarInfo.chosenBaseName)
    builder.setLocationName(*avatarInfo.chosenBaseName);
  if (avatarInfo.creatureInfo.startingBase && avatarInfo.creatureInfo.startingBase->isActive()) {
    builder.setLevel(level);
    if (avatarInfo.creatureInfo.startingBase->addTerritory)
      builder.addArea(level->getLandingSquares(StairKey::keeperSpawn())
          .transform([](auto& pos){ return pos.getCoord(); }));
  }
  model->addCollective(builder.build(contentFactory.get()));
  playerCollective = model->getCollectives().back();
  CHECK(!!playerCollective->getName()->shortened);
  auto playerControlOwned = PlayerControl::create(playerCollective, introText, keeperInfo.tribeAlignment);
  playerControl = playerControlOwned.get();
  playerCollective->setControl(std::move(playerControlOwned));
  playerCollective->setVillainType(VillainType::PLAYER);
  addCollective(playerCollective);
  playerControl->loadImmigrationAndWorkshops(contentFactory.get(), keeperInfo);
  for (auto tech : keeperInfo.initialTech)
    playerCollective->acquireTech(tech, false);
  for (auto resource : keeperInfo.credit)
    playerCollective->returnResource(resource);
  for (auto& f : keeperInfo.flags)
    effectFlags.insert(f);
  zLevelGroups = keeperInfo.zLevelGroups;
  allianceAttackPossible =
#ifdef RELEASE
    Random.roll(3);
#else
    true;
#endif
}

Game::~Game() {}

PGame Game::campaignGame(Table<PModel>&& models, CampaignSetup setup, AvatarInfo avatar,
    ContentFactory contentFactory, map<string, string> analytics) {
  auto ret = makeOwner<Game>(std::move(models), setup.campaign.getPlayerPos(), setup, std::move(contentFactory));
  ret->avatarId = avatar.avatarId;
  ret->analytics = analytics;
  for (auto model : ret->getAllModels())
    model->setGame(ret.get());
  auto avatarCreature = avatar.playerCreature.get();
  if (avatarCreature->getAttributes().isAffectedPermanently(LastingEffect::SUNLIGHT_VULNERABLE) ||
      avatarCreature->getBody().isIntrinsicallyAffected(LastingEffect::SUNLIGHT_VULNERABLE, ret->getContentFactory()))
    ret->sunlightTimeOffset = 1501_visible;
  // Remove sunlight vulnerability temporarily otherwise placing the creature anywhere without cover will fail.
  avatarCreature->getAttributes().removePermanentEffect(LastingEffect::SUNLIGHT_VULNERABLE, 1);
  ret->sunlightInfo.update(ret->getGlobalTime() + ret->sunlightTimeOffset);
  ret->spawnKeeper(std::move(avatar), setup.introMessages);
  // Restore vulnerability. If the effect wasn't present in the first place then it will zero-out.
  avatarCreature->getAttributes().addPermanentEffect(LastingEffect::SUNLIGHT_VULNERABLE, 1);
  return ret;
}

PGame Game::warlordGame(Table<PModel> models, CampaignSetup setup, vector<PCreature> creatures,
    ContentFactory contentFactory, string avatarId) {
  auto ret = makeOwner<Game>(std::move(models), setup.campaign.getPlayerPos(), setup, std::move(contentFactory));
  ret->avatarId = std::move(avatarId);
  for (auto model : ret->getAllModels())
    model->setGame(ret.get());
  for (auto& c : creatures)
    if (c->getAttributes().isAffectedPermanently(LastingEffect::SUNLIGHT_VULNERABLE))
      ret->sunlightTimeOffset = 1501_visible;
  // Remove sunlight vulnerability temporarily otherwise placing the creature anywhere without cover will fail.
  for (auto& c : creatures)
    c->getAttributes().removePermanentEffect(LastingEffect::SUNLIGHT_VULNERABLE, 1);
  ret->sunlightInfo.update(ret->getGlobalTime() + ret->sunlightTimeOffset);
  auto ref = getWeakPointers(creatures);
  ret->getMainModel()->landWarlord(std::move(creatures));
  // Restore vulnerability. If the effect wasn't present in the first place then it will zero-out.
  for (auto& c : ref)
    c->getAttributes().addPermanentEffect(LastingEffect::SUNLIGHT_VULNERABLE, 1);
  return ret;
}

PGame Game::splashScreen(PModel&& model, const CampaignSetup& s, ContentFactory f, View* view) {
  auto modelRef = model.get();
  Table<PModel> t(1, 1);
  t[0][0] = std::move(model);
  auto game = makeOwner<Game>(std::move(t), Vec2(0, 0), s, std::move(f));
  for (auto model : game->getAllModels())
    model->setGame(game.get());
  auto spectator = makeOwner<Spectator>(game->models[0][0]->getGroundLevel(), view);
  spectator->subscribeTo(modelRef);
  game->spectator = std::move(spectator);
  game->turnEvents.clear();
  return game;
}

void Game::rarSetLivePvp(bool b) {
  rarLivePvp = b;
  // Real-time control also needs real-time INPUT: the view must queue keypresses instead of flushing/locking one
  // per turn, so an order pressed while the creature is acting runs on its next turn (no timing window).
  if (view) {
    view->rarSetLivePvpInput(b);
    view->rarSetSpeedLocked(b);   // no speed changes mid-battle: both games must advance at the same rate
  }
}

bool Game::isTurnBased() {
  // RAR live PvP: during a live battle time must keep flowing even while controlling a team -- the other player
  // is playing in real time, so turn-based pausing would stall/desync the fight.
  if (rarLivePvp)
    return false;
  return !getPlayerCreatures().empty();
}

GlobalTime Game::getGlobalTime() const {
  PROFILE;
  return GlobalTime((int) currentTime);
}

const vector<Collective*>& Game::getVillains(VillainType type) const {
  static vector<Collective*> empty;
  if (villainsByType.count(type))
    return villainsByType.at(type);
  else
    return empty;
}

Model* Game::getCurrentModel() const {
  if (!players.empty())
    return players[0]->getPosition().getModel();
  else
    return models[baseModel].get();
}

int Game::getModelDifficulty(const Model* model) const {
  return campaign->getBaseLevelIncrease(model->position);
}

bool Game::passesMaxAggressorCutOff(const Model* model) {
  return campaign->passesMaxAggressorCutOff(model->position);
}

// ---- RAR villain waves -------------------------------------------------------------------------------
// A world-map villain's model is only downloaded when the player TRAVELS there, so its VillageControl never
// runs at the base and the vanilla attack path is dead. This rebuilds the behaviour from the villain's
// DEFINITION (enemies.txt) and delivers it like an endless-enemy wave: attackers spawn at the base's transfer
// landing with a single "kill the keeper" task and never retreat.

// The villain's own strength, derived from its definition (its live collective doesn't exist here).
// Computed from the creature ATTRIBUTES, never from a live Creature: Creature::getDifficultyPoints() calls
// getGame()->getContentFactory(), and a creature that was never placed on a level has no game -> null deref.
// This mirrors that same formula (DEFENSE + every attack attr) using the factory we already hold. Cached per
// enemy, because this runs for every villain on every tick.
static double villainDefPower(const EnemyInfo& info, CreatureFactory* creatureFactory,
    const ContentFactory* factory, map<string, double>& cache) {
  auto& fighters = info.settlement.inhabitants.fighters;
  if (fighters.all.empty())
    return 0;
  const string key = info.id ? info.id->data() : string();
  if (!key.empty()) {
    auto it = cache.find(key);
    if (it != cache.end())
      return it->second;
  }
  double sum = 0;
  for (auto& elem : fighters.all) {
    auto attributes = creatureFactory->getAttributesFromId(elem.second);
    double value = attributes.getRawAttr(AttrType("DEFENSE"));
    for (auto& attr : factory->attrInfo)
      if (attr.second.isAttackAttr)
        value += attributes.getRawAttr(attr.first);
    sum += value;
  }
  const double ret = max(0, fighters.count.getStart()) * (sum / fighters.all.size());
  if (!key.empty())
    cache[key] = ret;
  return ret;
}

// How many defeated villains share this one's tribe -- our stand-in for the half of SelfVictims we can still
// see ("you slaughtered my kinsmen elsewhere"). The villain-side victim counters live in its model, which
// isn't loaded at the base.
static int defeatedSameTribe(const Campaign* campaign, const ContentFactory* factory, TribeId tribe) {
  int ret = 0;
  for (Vec2 v : campaign->getSites().getBounds())
    if (campaign->isDefeated(v))
      if (auto villain = campaign->getSites()[v].getVillain()) {
        auto it = factory->enemies.find(villain->enemyId);
        if (it != factory->enemies.end() && it->second.settlement.tribe == tribe)
          ++ret;
      }
  return ret;
}

// Same probability scale as VillageBehaviour::getTriggerValue. The two triggers that need villain-side state
// we cannot see from the base return 0 rather than being faked.
static double villainTriggerProb(const AttackTrigger& trigger, const Game* game, const Collective* player,
    const EnemyInfo& info, double villainPower, double peakPower, int sameTribeDefeated,
    bool nearBase) {
  const double powerMaxProb = 1.0 / 10000;
  const double victimsMaxProb = 1.0 / 500;
  const double populationMaxProb = 1.0 / 500;
  const double goldMaxProb = 1.0 / 1000;
  const double finishOffMaxProb = 1.0 / 1000;
  const double proximityMaxProb = 1.0 / 5000;
  const double timerProb = 1.0 / 3000;
  const double numConqueredMaxProb = 1.0 / 3000;
  const double aggravatingMinionProb = 1.0 / 5000;
  const double playerPower = player->getDangerLevel();
  return trigger.visit<double>(
      [&](const Timer& t) {
        return game->getGlobalTime().getVisibleInt() >= t.value ? timerProb : 0.0;
      },
      [&](const RoomTrigger& t) {
        return t.probPerSquare * player->getConstructions().getBuiltCount(t.type);
      },
      [&](const Power&) {
        if (villainPower == 0 || playerPower == 0)
          return 0.0;
        const double a = playerPower / villainPower;
        return (a < 0.5 || a > 2) ? 0.0 : powerMaxProb;
      },
      [&](const FinishOff&) {
        // "The keeper has been beaten down below half his peak -- move in and finish him."
        if (peakPower < villainPower || playerPower * 2 >= peakPower)
          return 0.0;
        return finishOffMaxProb * (1 - 2 * (playerPower / peakPower) * 0.75);
      },
      [&](const SelfVictims&) {
        const double v = sameTribeDefeated == 0 ? 0.0 : sameTribeDefeated == 1 ? 0.3
            : sameTribeDefeated == 2 ? 0.7 : 1.0;
        return victimsMaxProb * v;
      },
      [&](const EnemyPopulation& t) {
        return player->getPopulationSize() >= t.value ? populationMaxProb : 0.0;
      },
      [&](const Resource& r) {
        return player->numResource(r.resource) >= r.value ? goldMaxProb : 0.0;
      },
      [&](const StolenItems&) { return 0.0; },       // needs the villain's own theft memory
      [&](const MiningInProximity&) { return 0.0; }, // needs the villain's territory
      [&](const Proximity&) { return nearBase ? proximityMaxProb : 0.0; },
      [&](const NumConquered& t) {
        return game->getNumLesserVillainsDefeated() >= t.value ? numConqueredMaxProb : 0.0;
      },
      [&](Immediate) { return 1.0; },
      [&](AggravatingMinions) {
        double res = 0;
        for (auto c : player->getCreatures())
          if (c->isAffected(LastingEffect::AGGRAVATES))
            res += aggravatingMinionProb;
        return res;
      }
  );
}

const map<Vec2, Game::VillainWave>& Game::getVillainWaves() const {
  return villainWaves;
}

const Campaign& Game::getCampaign() const {
  return *campaign;
}

void Game::reconcileVillainsForLoad() {
  if (!rarEnabled())
    return;
  // reconcileVillains CLEARS every villain dweller and re-places the roster, so an empty roster would wipe
  // the whole map. A failed/unreachable fetch also returns empty -- indistinguishable from "no villains" --
  // so treat empty as "no data" and leave the map alone rather than blanking it.
  auto roster = rarGetVillainRoster();
  // Which sites still hold loot, in one request. Pulled here (and after each pillage) so the villains panel
  // can answer per-site from a cache instead of hitting the network on every refresh.
  rarRefreshVillainLoot();
  if (roster.empty())
    return;
  campaign->reconcileVillains(getContentFactory(), roster, getPlayerVillainGroups(), getPlayerTribe());
}

void Game::considerVillainWaves() {
  if (!rarEnabled())
    return;
  auto player = getPlayerCollective();
  if (!player)
    return;
  auto baseModel = getMainModel().get();
  if (!baseModel || getCurrentModel() != baseModel)
    return;                                   // only while the player is home
  const int now = getGlobalTime().getVisibleInt();
  // Opening grace: the world ignores a brand-new keeper for a while. Without this, villains can show up as
  // "Triggered" on turn one -- before the player has any chance to prepare, and before the numbers that drive
  // most triggers (population, gold, danger level) mean anything. The prep delay then runs on TOP of this, so
  // the earliest a wave can actually land is roughly turn 1800-2500.
  if (now < getContentFactory()->campaignInfo.villainWaveGracePeriod)
    return;
  peakPlayerPower = max(peakPlayerPower, player->getDangerLevel());
  auto factory = getContentFactory();
  auto& creatureFactory = factory->getCreatures();
  const auto& tuning = factory->campaignInfo;   // villain-wave knobs, from campaign_info.txt
  // Read the aggression level from the OPTION, not from Game::enemyAggressionLevel. That member is never
  // actually serialized -- Game has no CEREAL_CLASS_VERSION, so its version is always 0 and the loader takes
  // the  branch that forces it to NONE on EVERY load. Relying on it silenced every villain the moment a
  // game was saved and reloaded. Reading the option also makes the setting changeable mid-game.
  auto aggression = EnemyAggressionLevel::MODERATE;
  switch (getOptions()->getIntValue(OptionId::ENEMY_AGGRESSION)) {
    case 0: aggression = EnemyAggressionLevel::NONE; break;
    case 2: aggression = EnemyAggressionLevel::EXTREME; break;
    default: break;
  }
  static map<string, double> villainPowerCache; // per-enemy, stable for the process (content is fixed)
  for (Vec2 v : campaign->getSites().getBounds()) {
    if (campaign->isDefeated(v))
      continue;
    auto villain = campaign->getSites()[v].getVillain();
    // isConquerableSite() is the wrong test here: it INCLUDES ALLY, because on the shared map someone else's
    // ally can still be wiped out. But reconcileVillains has already re-typed this map for THIS keeper --
    // anything still tagged ALLY is genuinely OUR ally (it is in our own villainGroups), and an ally must
    // never send a wave at us. Others' allies arrive here as MINOR and do attack, which is the point.
    if (!villain || villain->type == VillainType::ALLY)
      continue;
    if (!isConquerableSite(villain->type))
      continue;
    auto enemyIt = factory->enemies.find(villain->enemyId);
    if (enemyIt == factory->enemies.end())
      continue;
    const EnemyInfo& info = enemyIt->second;
    if (!info.behaviour || info.behaviour->triggers.empty())
      continue;                               // no attack behaviour defined at all
    // Respect the campaign's enemy-aggression setting, exactly as VillageControl::updateAggression does for
    // ordinary villains: NONE means villains never attack at all, EXTREME adds a bare timer so they come for
    // you eventually even unprovoked.
    if (aggression == EnemyAggressionLevel::NONE)
      continue;
    auto& fighters = info.settlement.inhabitants.fighters;
    if (fighters.all.empty())
      continue;                               // no minions -> never attack with the leader alone
    auto& state = villainWaves[v];
    // 1. A scheduled wave that has come due -> deliver it.
    if (state.scheduled && now >= *state.scheduled) {
      state.scheduled = none;
      // Clamp by the villain's ACTUAL garrison, not just config.maxPopulation. maxPopulation DEFAULTS TO
      // 10000 in CollectiveConfig and most villains never set it -- taking 75%% of that spawned 7500 dwarves
      // and carpeted the whole base map. A villain can never send more fighters than it has.
      const int garrison = max(1, min(info.config.getMaxPopulation(), fighters.count.getEnd()));
      const int waveSize = max(1, garrison * tuning.villainWaveForcePercent / 100);
      auto wave = fighters;                   // copy the definition's fighter list
      wave.count = Range(waveSize, waveSize + 1);
      auto attackTask = !player->getLeaders().empty()
          ? Task::attackCreatures(player->getLeaders())
          : Task::attackCreatures(player->getCreatures(MinionTrait::FIGHTER));
      if (!attackTask)
        continue;
      auto taskRef = attackTask.get();
      Level* level = baseModel->getGroundLevel();
      // Always put this villain on cooldown, even if the landing below fails. Otherwise a level with no
      // transfer-landing squares would re-roll and re-fail every turn -- the panel would keep flashing
      // "Triggered" while nothing ever arrives, with no clue why.
      const int waveCooldown = waveSize * (int) info.config.getImmigrantInterval().getVisibleInt()
          + Random.get(tuning.villainWaveCooldownRollMin, tuning.villainWaveCooldownRollMax + 1);
      state.nextEligible = now + waveCooldown;
      if (level->getLandingSquares(StairKey::transferLanding()).empty()) {
        INFO << "RAR villain wave: " << villain->enemyId.data() << " could not attack -- the base level has "
             << "no transfer-landing squares to arrive on";
        continue;
      }
      auto creatures = wave.generate(Random, &creatureFactory, info.settlement.tribe,
          MonsterAIFactory::singleTask(std::move(attackTask)), true /*nonUnique: no named/leader spawns*/);
      vector<Creature*> attackers;
      const Vec2 landingDir(Random.choose<Dir>());
      for (auto& c : creatures) {
        auto ref = c.get();
        c->setCombatExperience(campaign->getBaseLevelIncrease(v));  // its world-map experience
        // Retag the wave onto HOSTILE. They spawn on the villain's own tribe, which may be FRIENDLY with the
        // player's keeper tribe (e.g. GREENSKIN goblins are not in DARK_KEEPER's enemy list) -- the attack
        // would arrive and then just stand there. HOSTILE is hostile to every keeper tribe: the two built-in
        // ones by name, and enemyOfAll content tribes (UNDEAD_KEEPER, FALLEN_ANGEL) mutually via addEnemy.
        // Unlike the RAR keeper-vs-keeper invasion this is NOT restored: these are throwaway creatures
        // generated for the wave, not borrowed from a real dungeon.
        c->setTribe(TribeId::getHostile());
        if (level->landCreature(StairKey::transferLanding(), std::move(c), landingDir))
          attackers.push_back(ref);
      }
      INFO << "RAR villain wave: " << villain->enemyId.data() << " sent " << (int) attackers.size()
           << "/" << waveSize << " attackers";
      if (!attackers.empty()) {
        state.attackers = attackers;   // panel shows red "attacking" while any of these are still alive
        // Name the attackers, not their address: villain->name is the world-map category ("Cave"), so the
        // message read "You are under attack by Cave". Vanilla passes the collective's full name here, which
        // for a settled villain resolves to its race -- getDisplayName gets to the same answer without one.
        auto attackerName = capitalFirst(info.getDisplayName(&creatureFactory).value_or(villain->name));
        player->getControl()->addAttack(CollectiveAttack({taskRef}, attackerName,
            wave.getViewId(&creatureFactory), attackers));
      }
      continue;
    }
    if (state.scheduled)
      continue;                               // already inbound -- don't re-roll
    if (now < state.nextEligible)
      continue;                               // still recovering from its last wave
    // 2. Roll this villain's triggers.
    // Vanilla fires Proximity only for an IMMEDIATELY neighbouring site (getModelDistance == dist8 == 1).
    // On a 100x65 shared world that almost never happens, so widen it to a radius: any villain within this
    // many tiles of the base counts as being on your doorstep. Deliberate deviation from vanilla.
    const bool nearBase = v.dist8(campaign->getPlayerPos()) <= tuning.villainProximityRadius;
    const double villainPower = villainDefPower(info, &creatureFactory, factory, villainPowerCache);
    const int sameTribe = defeatedSameTribe(campaign.get(), factory, info.settlement.tribe);
    // The experience cutoff is ABSOLUTE -- no trigger bypasses it, and a villain that fails it shows nothing
    // in the panel either. Vanilla lets SelfVictims/StolenItems through because there they mean "this villain
    // personally lost members to YOU", which only a villain you actually attacked can feel. Our SelfVictims is
    // a tribe-wide proxy (any defeated settlement of the same tribe), so honouring that bypass let a 50-exp
    // fortress the player had never touched provoke itself against a keeper whose best kill was worth 6.
    state.triggers.clear();
    if (!campaign->passesMaxAggressorCutOff(v))
      continue;
    if (villain->type == VillainType::MAIN && getNumLesserVillainsDefeated() < 3)
      continue;
    double prob = 0;
    auto triggers = info.behaviour->triggers;
    if (aggression == EnemyAggressionLevel::EXTREME)
      triggers.push_back(Timer{1000});
    for (auto& trigger : triggers) {
      const double val = villainTriggerProb(trigger, this, player, info, villainPower, peakPlayerPower,
          sameTribe, nearBase);
      prob = max(prob, val);
      if (val > 0)
        state.triggers.push_back(TriggerInfo{trigger, val}); // drives the villages panel + its tooltip
    }
    if (prob > 0 && Random.chance(prob))
      // triggered -- give the player time to prepare before it lands
      state.scheduled = now + Random.get(tuning.villainWavePrepMin, tuning.villainWavePrepMax + 1);
  }
}

int Game::getNumLesserVillainsDefeated() const {
  return numLesserVillainsDefeated;
}

PModel& Game::getMainModel() {
  return models[baseModel];
}

vector<Model*> Game::getAllModels() const {
  vector<Model*> ret;
  for (Vec2 v : models.getBounds())
    if (models[v])
      ret.push_back(models[v].get());
  return ret;
}

bool Game::isSingleModel() const {
  // Base the "single map" decision on the CAMPAIGN (occupied sites), not on how many models are currently
  // materialised. RAR online loads villain models lazily (only the base exists at start), so counting live
  // models wrongly reported single-map -> the world-map button + edge-travel were disabled.
  return campaign->getNumNonEmpty() <= 1;
}

int Game::getSaveProgressCount() const {
  int saveTime = 0;
  for (auto model : getAllModels())
    saveTime += model->getSaveProgressCount();
  return saveTime;
}

void Game::prepareSiteRetirement() {
  for (Vec2 v : models.getBounds())
    if (models[v] && v != baseModel)
      models[v]->discardForRetirement();
  for (Collective* col : models[baseModel]->getCollectives())
    if (col != playerCollective)
      col->setVillainType(VillainType::NONE);
  if (playerCollective->getVillainType() == VillainType::PLAYER) {
    // if it's not PLAYER then it's a conquered collective and villainType and VillageControl is already set up
    playerCollective->setVillainType(VillainType::RETIRED);
    playerCollective->setControl(VillageControl::create(
        playerCollective, CONSTRUCT(VillageBehaviour,
            c.minPopulation = 24;
            c.minTeamSize = 5;
            c.triggers = makeVec<AttackTrigger>(
                RoomTrigger{FurnitureType("THRONE"), 0.0003},
                SelfVictims{},
                StolenItems{}
            );
            c.attackBehaviour = KillLeader{};
            c.ransom = make_pair(0.8, Random.get(500, 700));)));
  }
  playerCollective->retire();
  vector<Position> locationPos;
  for (auto f : contentFactory->furniture.getTrainingFurniture(AttrType("SPELL_DAMAGE")))
    for (auto pos : playerCollective->getConstructions().getBuiltPositions(f))
      locationPos.push_back(pos);
  if (locationPos.empty())
    locationPos = playerCollective->getTerritory().getAll();
  if (!locationPos.empty())
    playerCollective->getTerritory().setCentralPoint(
        Position(Rectangle::boundingBox(locationPos.transform([](Position p){ return p.getCoord();})).middle(),
            playerCollective->getModel()->getGroundLevel()));
  for (auto c : copyOf(playerCollective->getCreatures()))
    c->retire();
  playerControl = nullptr;
  Model* mainModel = models[baseModel].get();
  mainModel->setGame(nullptr);
  for (Collective* col : models[baseModel]->getCollectives())
    for (Creature* c : copyOf(col->getCreatures()))
      if (c->getPosition().getModel() != mainModel)
        transferCreature(c, mainModel);
  for (Vec2 v : models.getBounds())
    if (models[v] && v != baseModel)
      for (Collective* col : models[v]->getCollectives())
        for (Creature* c : copyOf(col->getCreatures()))
          if (c->getPosition().getModel() == mainModel)
            transferCreature(c, models[v].get());
  mainModel->prepareForRetirement();
  UniqueEntity<Item>::offsetForSerialization(Random.getLL());
  UniqueEntity<Creature>::offsetForSerialization(Random.getLL());
}

void Game::doneRetirement() {
  UniqueEntity<Item>::clearOffset();
  UniqueEntity<Creature>::clearOffset();
}

optional<ExitInfo> Game::updateInput() {
  if (spectator)
    while (1) {
      UserInput input = view->getAction();
      if (input.getId() == UserInputId::EXIT)
        return ExitInfo(ExitAndQuit());
      if (input.getId() == UserInputId::IDLE)
        break;
    }
  // RAR live PvP: this drains the WHOLE input queue into the keeper overseer every frame. Vanilla is safe because
  // controlling creatures makes isTurnBased() true, so it's skipped and input reaches the controlled creature.
  // Live PvP forces isTurnBased() false (time must keep flowing), which would let the overseer swallow every
  // click/keypress meant for the team. So while creatures are controlled, leave the input for them.
  if (playerControl && !isTurnBased() && !(rarLivePvp && !getPlayerCreatures().empty())) {
    while (1) {
      UserInput input = view->getAction();
      if (input.getId() == UserInputId::IDLE)
        break;
      else
        lastUpdate = none;
      playerControl->processInput(view, input);
      if (exitInfo)
        return exitInfo;
    }
  }
  return none;
}

static const TimeInterval initialModelUpdate = 2_visible;

void Game::initializeModels(ProgressMeter& meter) {
  // REPAIR, before anything reads a position. Saves written before sites were released on exit can hold
  // player claims inside a downloaded site -- territory and even storage squares, granted by that site's own
  // claim-effect floors. Those squares are not the player's to keep, and once the model is gone every one of
  // them is a live crash: the build menu counts resources by reading items off storage squares, and a
  // position whose level has died passes isValid() (it only checks a bool) and faults on dereference.
  // Hand them all back here, once, for any site that is no longer loaded.
  // NOT gated on rarEnabled(): a keeper holding squares inside a model that isn't its own is dangerous in any
  // mode, and outside RAR it simply never happens, so the strip is a no-op there. Gating it also made the
  // repair impossible to exercise from a save file, which is how it shipped once without being verified.
  {
    if (auto pc = getPlayerCollective()) {
      int repaired = 0;
      // (a) Claims inside a downloaded SITE. The player should never carry these across a load: the site is
      // transient and may be released at any point, and every claim in it is then a live crash waiting for
      // whichever system reads it first. Strip them whatever the model's fate, rather than depending on
      // knowing exactly when it gets released.
      for (Vec2 v : models.getBounds())
        if (v != baseModel)
          if (auto model = models[v].get())
            repaired += pc->forgetPositionsOn(model);
      // (b) Claims on a level that is already gone -- a save written before (a) existed.
      set<const Level*> live;
      for (Vec2 v : models.getBounds())
        if (auto model = models[v].get())
          for (auto level : model->getLevels())
            live.insert(level);
      repaired += pc->forgetPositionsNotOn(live);
      if (repaired > 0)
        INFO << "RAR: released " << repaired << " squares the keeper had claimed inside downloaded sites";
    }
  }
  for (auto col : getCollectives())
    col->update(col->getModel() == getCurrentModel());
  // Give every model a couple of turns so that things like shopkeepers can initialize.
  for (Vec2 v : models.getBounds())
    if (auto model = models[v].get()) {
      for (auto c : model->getAllCreatures()) {
        //c->tick(); Ticking crashes if it's a player and it dies. It was most likely only an optimization
        auto level = c->getPosition().getLevel();
        level->getSectors(c->getMovementType());
        level->getSectors(c->getMovementType().setForced());
        level->getSectors(MovementType(MovementTrait::WALK).setForced());
        level->getSectors(MovementType(MovementTrait::FLY));
      }
      // Use top level's id as unique id of the model.
      auto id = model->getGroundLevel()->getUniqueId();
      if (!localTime.count(id))
        localTime[id] = (model->getLocalTime() + initialModelUpdate).getDouble();
      if (getCurrentModel() != model)
        updateModel(model, localTime[id], none);
      meter.addProgress();
    }
}

void Game::increaseTime(double diff) {
  auto before = getGlobalTime();
  currentTime += diff;
  auto after = getGlobalTime();
  if (after > before)
    for (auto m : getAllModels())
      for (auto c : m->getAllCreatures())
        c->setGlobalTime(after);
}

optional<ExitInfo> Game::update(double timeDiff, milliseconds endTime) {
  //CHECK(timeDiff >= 0); this will probably fail - check
  PROFILE_BLOCK("Game::update");
  if (auto exitInfo = updateInput())
    return exitInfo;
  considerRealTimeRender();
  Model* currentModel = getCurrentModel();
  auto currentId = currentModel->getGroundLevel()->getUniqueId();
  while (!lastTick || currentTime >= *lastTick + 1) {
    if (!lastTick)
      lastTick = (int)currentTime;
    else
      *lastTick += 1;
    tick(GlobalTime(*lastTick));
  }
  considerRetiredLoadedEvent(currentModel->position);
  if (!updateModel(currentModel, localTime[currentId] + timeDiff, endTime)) {
    localTime[currentId] += timeDiff;
    increaseTime(timeDiff);
  } // Consider setting back the model's local time as now it's desynced from the localTime table.
  return exitInfo;
}

void Game::considerRealTimeRender() {
  auto absoluteTime = view->getTimeMilliAbsolute();
  if (!lastUpdate || absoluteTime - *lastUpdate > milliseconds{10}) {
    if (playerControl)
      playerControl->render(view);
    if (spectator)
      view->updateView(spectator.get(), false);
    lastUpdate = absoluteTime;
  }
}

void Game::setWasTransfered() {
  wasTransfered = true;
}

// Return true when the player has just left turn-based mode so we don't increase time in that case.
bool Game::updateModel(Model* model, double totalTime, optional<milliseconds> endTime) {
  do {
    bool wasPlayer = !getPlayerCreatures().empty();
    if (!model->update(totalTime))
      return false;
    if (wasPlayer && getPlayerCreatures().empty())
      return true;
    if (wasTransfered) {
      wasTransfered = false;
      return false;
    }
    if (exitInfo)
      return true;
    if (endTime && Clock::getRealMillis() > *endTime)
      return true;
  } while (1);
}

bool Game::isVillainActive(const Collective* col) {
  const Model* m = col->getModel();
  return m == getMainModel().get() || campaign->isInInfluence(m->position);
}

void Game::updateSunlightMovement() {
  auto previous = sunlightInfo.getState();
  sunlightInfo.update(getGlobalTime() + sunlightTimeOffset);
  if (previous != sunlightInfo.getState())
    for (Vec2 v : models.getBounds())
      if (Model* m = models[v].get()) {
        m->updateSunlightMovement();
        if (playerControl)
          playerControl->onSunlightVisibilityChanged();
      }
}

void Game::tick(GlobalTime time) {
  PROFILE_BLOCK("Game::tick");
  if (!turnEvents.empty() && time.getVisibleInt() > *turnEvents.begin()) {
    auto turn = *turnEvents.begin();
    if (turn == 0) {
      auto values = campaign->getParameters();
      values["current_mod"] = getOptions()->getStringValue(OptionId::CURRENT_MOD2);
      values["version"] = string(BUILD_DATE) + " " + string(BUILD_VERSION);
      values["avatar_id"] = avatarId;
      for (auto& elem : analytics)
        values.insert(elem);
      uploadEvent("campaignStarted", values);
    } else
      uploadEvent("turn", {{"turn", toString(turn)}});
    turnEvents.erase(turn);
  }
  updateSunlightMovement();
  // RAR: world-map villains attack from their DEFINITION (their models aren't loaded, so the VillageControl
  // path below can never fire for them). Per turn, same cadence as the collective updates.
  considerVillainWaves();
  INFO << "Global time " << time;
  for (Collective* col : collectives) {
    if (isVillainActive(col))
      col->update(col->getModel() == getCurrentModel());
  }
  considerAllianceAttack();
}

void Game::setExitInfo(ExitInfo info) {
  exitInfo = std::move(info);
}

void Game::exitAction() {
  ScriptedUIState state;
  auto data = ScriptedUIDataElems::Record{};
  data.elems["save"] = ScriptedUIDataElems::Callback{[this]{
    // RAR online: never save while an invasion is live -- the target dungeon (a rival keeper OR a downloaded
    // villain) is transient/server-owned, and saving mid-invasion corrupts the save. Block save & exit while
    // an invasion is active OR the player is controlling a team AWAY from their own base model (= invading a
    // villain). Make them bring the team home first.
    if (isInvading()) {
      getView()->presentText(none, TString(
          "You can't save & exit during an invasion. Bring your team back to your own dungeon first."_s));
      return false;
    }
    if (getView()->yesOrNoPrompt(TStringId("SAVE_AND_EXIT_CONFIRM"))) {
      setExitInfo(GameSaveType::KEEPER);
      return true;
    }
    return false;
  }};
  data.elems["abandon"] = ScriptedUIDataElems::Callback{[this] {
    if (getView()->yesOrNoPrompt(TStringId("ABANDON_GAME_CONFIRM"))) {
      addAnalytics("gameAbandoned", "");
      setExitInfo(ExitAndQuit());
      return true;
    }
    return false;
  }};
  data.elems["options"] = ScriptedUIDataElems::Callback{[this]{
    getOptions()->handle(getView(), &*contentFactory, OptionSet::GENERAL);
    return false;
  }};
#ifdef RELEASE
  bool canRetire = playerControl && !playerControl->getTutorial() && getPlayerCreatures().empty() && gameWon();
#else
  bool canRetire = playerControl && !playerControl->getTutorial() && getPlayerCreatures().empty();
#endif
  if (canRetire)
    data.elems["retire"] = ScriptedUIDataElems::Callback{[this]{
      getView()->stopClock();
      playerControl->takeScreenshot();
      return true;
    }};
  else
    data.elems["retire_inactive"] = ScriptedUIDataElems::Record{};
  getView()->scriptedUI("exit_menu", data, state);
}

Position Game::getTransferPos(Model* from, Model* to) const {
  return to->getGroundLevel()->getLandingSquare(StairKey::transferLanding(),
      from->position - to->position);
}

void Game::transferCreature(Creature* c, Model* to, const vector<Position>& destinations) {
  Model* from = c->getLevel()->getModel();
  if (from != to && !c->getRider()) {
    if (destinations.empty())
      to->transferCreature(from->extractCreature(c), from->position - to->position);
    else
      to->transferCreature(from->extractCreature(c), destinations);
    for (auto& summon : c->getCompanions())
      if (c->getSteed() != summon)
        transferCreature(summon, to, destinations);
    if (auto steed = c->getSteed())
      for (auto& summon : steed->getCompanions())
        transferCreature(summon, to, destinations);
  }
}

bool Game::canTransferCreature(Creature* c, Model* to) {
  return to->canTransferCreature(c, c->getLevel()->getModel()->position - to->position);
}

int Game::getModelDistance(const Collective* c1, const Collective* c2) const {
  return c1->getModel()->position.dist8(c2->getModel()->position);
}

// The logged-in keeper's villain groups (keeper_creatures.txt). Decides who counts as an ALLY on the shared
// world map -- everyone else's allies read as enemies to us. Empty if the avatar type isn't in the factory.
vector<VillainGroup> Game::getPlayerVillainGroups() const {
  for (auto& p : contentFactory->keeperCreatures)
    if (p.first == avatarId)
      return p.second.villainGroups;
  return {};
}

// The logged-in keeper's own tribe -- what reconcileVillains checks a would-be ALLY against. Taken from the
// live collective when there is one (that is the tribe the minions actually fight on), falling back to the
// keeper's content definition for the cases that run before/without a collective.
TribeId Game::getPlayerTribe() const {
  if (auto col = getPlayerCollective())
    return col->getTribeId();
  for (auto& p : contentFactory->keeperCreatures)
    if (p.first == avatarId)
      return p.second.tribe.value_or(getPlayerTribeId(p.second.tribeAlignment));
  return TribeId::getDarkKeeper();
}

void Game::presentWorldmap() {
  if (rarEnabled())
    campaign->reconcileVillains(getContentFactory(), rarGetVillainRoster(), getPlayerVillainGroups(), getPlayerTribe());
  view->presentWorldmap(*campaign, baseModel);
}

Model* Game::chooseSite(Model* current) {
  // RAR online: rival keeper bases (from the shared-world claims) are offered as invasion targets,
  // even outside travel influence. Picking one queues an on-demand invasion instead of a transfer.
  vector<pair<Vec2, TString>> invadeable;
  vector<RarClaim> claims;
  if (rarEnabled()) {
    campaign->reconcileVillains(getContentFactory(), rarGetVillainRoster(), getPlayerVillainGroups(), getPlayerTribe());
    claims = rarGetClaims();
    for (auto& claim : claims)
      // Protected keepers stay selectable (the reserve step rejects them with "This keeper is protected.").
      // Exclude only THIS keeper, by gameId -- NOT the whole account. One login can own several keepers, and
      // they are independent rivals on the shared map: they must see and be able to invade each other. The
      // server agrees -- /reserve_dungeon has no same-account guard.
      if (claim.gameId != getGameIdentifier() && !claim.gameId.empty())
        invadeable.push_back(make_pair(Vec2(claim.x, claim.y), TString(claim.name)));
  }
  if (auto dest = view->chooseSite(TStringId("CHOOSE_DESTINATION_SITE"), *campaign, current->position, invadeable)) {
    for (auto& claim : claims)
      if (claim.gameId != getGameIdentifier() && Vec2(claim.x, claim.y) == *dest) {
        // record the target; transferAction (which has the full team) will requestInvasion.
        setPendingInvasion(*dest, claim.gameId);
        return nullptr;
      }
    // RAR Phase A: online, villain models aren't generated at start -- download + inject on first travel.
    if (!models[*dest] && villainLoader)
      if (auto m = villainLoader(*dest)) {
        Model* injected = injectSiteModel(*dest, std::move(m));
        auto vi = campaign->getSites()[*dest].getVillain();
        if (vi) // remember the enemyId for aftermath writeback
          recordInjectedVillain(*dest, vi->enemyId.data());
        // I'm INVADING this site: retag its defenders to HOSTILE so combat works regardless of tribe alliance.
        // A former ally (e.g. GREENSKIN goblins for a DARK_KEEPER) is demoted to an enemy on the map but stays
        // tribe-FRIENDLY to my keeper -- without this I couldn't attack them and they wouldn't fight back.
        // HOSTILE is enemy to every keeper tribe. My transferred team keeps its own tribe (it isn't part of this
        // loaded model). An ACTUAL ally (type ALLY, green on the map) is left alone -- visiting isn't war.
        bool retagHostile = !vi || vi->type != VillainType::ALLY;
        int difficulty = campaign->getBaseLevelIncrease(*dest); // per-player combat scaling by distance
        for (auto c : injected->getAllCreatures()) {
          c->setCombatExperience(difficulty);
          if (retagHostile)
            c->setTribe(TribeId::getHostile());
        }
      }
    // RAR: the villain map may be unavailable (server has no blob for this tile -- e.g. an orphaned roster
    // entry). Never crash on that -- abort the travel gracefully so the player can pick another target.
    if (!models[*dest]) {
      // The server refuses an invasion while another keeper holds that villain, and says so. Show ITS reason
      // when there is one -- "another keeper is in there" is a completely different thing to the player than
      // "couldn't be loaded", and the generic line made a working rule look like a fault.
      auto why = rarLastError();
      view->presentText(none, !why.empty()
          ? TString(why)
          : TString("That site's map couldn't be loaded from the server. Try another target."_s));
      return nullptr;
    }
    return models[*dest].get();
  }
  return nullptr;
}

void Game::setPendingInvasion(Vec2 pos, const string& gameId) {
  pendingInvasionTarget = make_pair(pos, gameId);
}

optional<pair<Vec2, string>> Game::takePendingInvasion() {
  auto ret = pendingInvasionTarget;
  pendingInvasionTarget = none;
  return ret;
}

void Game::requestInvasion(Vec2 pos, const string& gameId, vector<Creature*> team) {
  invasionRequest = make_pair(pos, gameId);
  invasionTeam = std::move(team);
}

optional<pair<Vec2, string>> Game::getInvasionRequest() const {
  return invasionRequest;
}

const vector<Creature*>& Game::getInvasionTeam() const {
  return invasionTeam;
}

void Game::clearInvasionRequest() {
  invasionRequest = none;
  invasionTeam.clear();
}

void Game::recordActiveInvasion(Vec2 pos, const string& gameId, Model* model, SavedGameInfo info,
    vector<Creature*> team) {
  activeInvasion = ActiveInvasion{pos, gameId, model, info, std::move(team)};
  // RAR: retag the invading team to the transient Invaders tribe so a SAME-alignment defender keeper treats
  // them as enemies (identical base keeper tribe would otherwise read as allies -> the defenders "join" you).
  // Only the invader's own team is touched, and only for this keeper invasion; restored in endActiveInvasion.
  for (Creature* c : activeInvasion->team) {
    activeInvasion->savedTribes[c] = c->getTribeId();
    c->setTribe(TribeId::getInvaders());
    // COMPANIONS (a shaman's SPIRITs, summons, steeds) are dragged along by transferCreature but are NOT part
    // of the recorded team, so they stayed on their original tribe while their owner moved to Invaders -- and
    // then landed hostile to the very squad they belong to. Retag them too, remembering each one's own tribe
    // so endActiveInvasion puts it back. (The live-PvP path already did this; the OFFLINE invasion never did.)
    for (Creature* comp : c->getCompanions())
      if (comp && comp->getTribeId() != TribeId::getInvaders()) {
        activeInvasion->savedTribes[comp] = comp->getTribeId();
        comp->setTribe(TribeId::getInvaders());
      }
  }
}

bool Game::hasActiveInvasion() const {
  return !!activeInvasion;
}

void Game::recordInjectedVillain(Vec2 pos, const string& enemyId) {
  injectedVillainEnemyId[pos] = enemyId;
}

optional<Game::VillainWriteback> Game::takeVillainWriteback() {
  Model* base = models[baseModel].get();
  for (Vec2 v : models.getBounds()) {
    Model* m = models[v].get();
    if (!m || m == base || villainWrittenBack.count(v))
      continue;
    // A site the player VISITED and left has to be written back and released too, not just one they conquered.
    // Requiring isConquered() here meant walking into a villain (or an ally, which is never conquered at all)
    // and walking out left the downloaded model attached to the game forever -- along with any territory the
    // site's own claim-effect floors handed to the visiting keeper. Take the conquered collective when there
    // is one, so the aftermath is reported against the right faction; otherwise take the site's owner.
    Collective* villainCol = nullptr;
    for (Collective* col : m->getCollectives())
      if (isConquerableSite(col->getVillainType()) && col->isConquered()) {
        villainCol = col;
        break;
      }
    const bool keepLoaded = !!villainCol;
    if (!villainCol)
      for (Collective* col : m->getCollectives())
        if (isConquerableSite(col->getVillainType())) {
          villainCol = col;
          break;
        }
    if (!villainCol)
      continue;
    // Only capture once the invading team has LEFT (no player-collective creature remains on this model),
    // so the serialized aftermath is the stable post-battle state -- not mid-fight with the invaders inside.
    bool teamInside = false;
    if (auto pc = getPlayerCollective())
      for (Creature* c : m->getAllCreatures())
        if (pc->getCreatures().contains(c)) { teamInside = true; break; }
    if (teamInside)
      continue;
    villainWrittenBack.insert(v);
    string enemyId = injectedVillainEnemyId.count(v) ? injectedVillainEnemyId.at(v) : string();
    // Hand back every claim the player picked up inside this site BEFORE anything can release the model. A
    // site's prebuilt floors can carry a claim effect, so simply standing in one grants the visiting keeper
    // real territory and even storage squares; leaving those attached to a transient model is what produced
    // the dangling-position crash. Doing it here covers both flows, since both come through this function.
    if (auto pc = getPlayerCollective())
      if (int given = pc->forgetPositionsOn(m))
        INFO << "RAR: released " << given << " claimed squares inside the site at " << v;
    return VillainWriteback{ v, models[v].giveMeSharedPointer(), enemyId, villainCol->getVillainType(), keepLoaded };
  }
  return none;
}

void Game::rearmVillainWriteback(Vec2 villainPos) {
  // Re-enable one more writeback for this villain. takeVillainWriteback only fires while the site is a
  // conquered villain the team has left, so re-arming an ordinary/base tile is harmless -- the scan just
  // finds nothing to capture.
  villainWrittenBack.erase(villainPos);
}

bool Game::isInvading() const {
  return hasActiveInvasion() ||
      (!getPlayerCreatures().empty() && getCurrentModel() != models[baseModel].get());
}

optional<string> Game::getActiveInvasionGameId() const {
  if (activeInvasion)
    return activeInvasion->gameId;
  return none;
}

bool Game::invasionTeamLeftDungeon() const {
  if (!activeInvasion)
    return false;
  for (Creature* c : activeInvasion->model->getAllCreatures())
    if (activeInvasion->team.contains(c))
      return false; // a team member is still inside the invaded dungeon
  return true;
}

optional<Game::EndedInvasion> Game::endActiveInvasion() {
  if (!activeInvasion)
    return none;
  auto inv = *activeInvasion;
  activeInvasion = none;
  // RAR: restore the invaders' real tribe (they were retagged to Invaders for the fight) before they head
  // home / the model is written back, so no creature is left on the transient tribe.
  for (auto& elem : inv.savedTribes)
    elem.first->setTribe(elem.second);
  // COMPANIONS too. They are not part of the recorded team (transferCreature drags them along implicitly), and
  // anything a retagged creature SUMMONED during the fight -- a shaman's spirits -- was born on the transient
  // Invaders tribe. Left that way they come home hostile to their own keeper and kill the team.
  for (auto& elem : inv.savedTribes)
    for (Creature* comp : elem.first->getCompanions())
      if (comp->getTribeId() == TribeId::getInvaders())
        comp->setTribe(elem.second);
  Model* base = models[baseModel].get();
  // Send any survivors still inside back home so the uploaded dungeon is defender-only.
  for (Creature* c : copyOf(inv.model->getAllCreatures()))
    if (inv.team.contains(c) && canTransferCreature(c, base))
      transferCreature(c, base);
  // Safety net: nothing may travel home still wearing the transient Invaders tribe. A creature summoned during
  // the fight can outlive its summoner (or belong to a team member who died), so it would never be reached by
  // the restores above -- and it would then be an enemy inside its own base.
  if (!inv.savedTribes.empty()) {
    auto homeTribe = inv.savedTribes.begin()->second;
    for (Creature* c : base->getAllCreatures())
      if (c->getTribeId() == TribeId::getInvaders())
        c->setTribe(homeTribe);
  }
  // Hand back the STILL-INTACT model (in models[inv.pos]) for the caller to serialize; the caller
  // then calls destroyInvasionSite(pos) to fully remove it. Serialize-before-destroy avoids the
  // getThis() null-weakPointer crash, and full destruction (below) expires all weak_ptrs to it.
  return EndedInvasion{ models[inv.pos].giveMeSharedPointer(), inv.pos, inv.gameId, inv.info };
}

void Game::resyncModelLocalTime(Model* m) {
  localTime[m->getGroundLevel()->getUniqueId()] = m->getLocalTimeDouble();
}

void Game::destroyInvasionSite(Vec2 pos) {
  // A rival keeper's dungeon: the dweller was a RetiredInfo marker added by addInvasionSite, so it goes too.
  releaseSiteModel(pos, true);
}

// The SAME teardown for a world villain or ally the player merely travelled to. Everything a downloaded model
// is referenced by has to be unwound before it can be freed -- freeing one without this is what turned the
// dangling-position crash into a dangling-Collective* crash in the villains panel. The only difference from a
// rival-keeper invasion is the campaign dweller: a villain must STAY on the world map, so its site keeps its
// dweller and only the loaded copy of its interior goes away.
void Game::releaseSiteModel(Vec2 pos, bool removeDweller) {
  if (pos == baseModel)
    return;
  Model* m = models[pos].get();
  if (!m)
    return;
  // Hand back anything the keeper claimed inside the site (a prebuilt floor with a claim effect grants
  // territory just for standing on it) BEFORE the model goes, or those squares dangle exactly as before.
  if (playerCollective)
    playerCollective->forgetPositionsOn(m);
  for (Collective* col : m->getCollectives()) {
    collectives.removeElementMaybe(col);
    for (auto& e : villainsByType)
      e.second.removeElementMaybe(col);
  }
  localTime.erase(m->getGroundLevel()->getUniqueId());
  if (removeDweller)
    campaign->removeDweller(pos);
  // The invading team's minions recorded known tiles on the enemy level for their (surviving) keeper
  // collective. Those Positions hold a raw Level* that serializes via getThis(); once the model is
  // freed below they'd dangle and crash the save. Scrub every surviving collective's known tiles back
  // to its own model WHILE the invaded levels are still alive (limitToModel derefs each Position).
  for (Collective* col : collectives)
    col->limitKnownTilesToOwnModel();
  // A surviving invader that fought a defender holds a raw Creature* to it via lastCombatIntent (the
  // one raw-pointer combat ref -- kills/enemies/holding are all id-based and safe). Freeing the invaded
  // model dangles it at save time. Trying to match "attacker is on the invaded model" proved leaky
  // (the target can be a dead creature, a steed or a held creature not in the model's creature lists),
  // so just clear the transient combat intent on every surviving creature (and its steed) outright.
  auto scrubIntent = [](Creature* c) {
    if (c->getLastCombatIntent())
      c->clearLastCombatIntent();
    if (Creature* s = c->getSteed())
      if (s->getLastCombatIntent())
        s->clearLastCombatIntent();
  };
  for (Model* sm : getAllModels())
    if (sm != m) {
      for (Creature* c : sm->getAllCreatures())
        scrubIntent(c);
      for (auto& c : sm->getDeadCreatures())
        scrubIntent(c.get());
    }
  // PlayerControl accumulates raw refs into the invaded model during the excursion: message Positions
  // (combat/kill notifications on the enemy level) and BattleSummary/attack/stun Creature*s (esp.
  // enemiesKilled when you kill defenders). Scrub them before the model is freed.
  if (playerControl)
    playerControl->scrubInvadedModelRefs(m);
  models[pos] = PModel(); // drop the game's owner ptr; once the caller drops its shared_ptr too the
                          // model is destroyed and every weak_ptr into it expires -> serialize-safe.
  // PlayerControl::unknownLocations caches villain-location Positions (incl. one on the invaded level,
  // re-cached when prepareForRetirement fired events). It's keyed by raw Position -> would dangle on
  // the freed level. The invaded collectives are gone from getCollectives() now, so rebuilding it
  // from scratch drops the stale entry cleanly.
  if (playerControl)
    playerControl->updateUnknownLocations();
  // Re-arm: a later visit re-downloads this site (getting whatever aftermath was written back, including any
  // damage another player did in the meantime) and leaving must release it again.
  villainWrittenBack.erase(pos);
  injectedVillainEnemyId.erase(pos);
}

Model* Game::addInvasionSite(Vec2 pos, PModel model, SavedGameInfo info) {
  if (models[pos])
    return models[pos].get(); // already invaded this tile this session -- reuse, don't double-inject
  models[pos] = std::move(model);
  Model* m = models[pos].get();
  m->position = pos;
  m->setGame(this);
  campaign->setDweller(pos, Campaign::SiteInfo::Dweller(
      Campaign::RetiredInfo{info, SaveFileInfo{"", 0, false, none}}));
  // Wire the freshly-loaded model into the running game the same way the Game ctor +
  // initializeModels do for startup models -- without this the game crashes (collectives
  // unregistered, no localTime entry, movement sectors/globals not set up).
  for (Collective* col : m->getCollectives()) {
    if (auto control = dynamic_cast<VillageControl*>(col->getControl()))
      control->updateAggression(enemyAggressionLevel);
    addCollective(col);
  }
  m->updateSunlightMovement();
  for (auto c : m->getAllCreatures())
    c->setGlobalTime(getGlobalTime());
  for (auto c : m->getAllCreatures()) {
    auto level = c->getPosition().getLevel();
    level->getSectors(c->getMovementType());
    level->getSectors(c->getMovementType().setForced());
    level->getSectors(MovementType(MovementTrait::WALK).setForced());
    level->getSectors(MovementType(MovementTrait::FLY));
  }
  auto id = m->getGroundLevel()->getUniqueId();
  if (!localTime.count(id))
    localTime[id] = (m->getLocalTime() + initialModelUpdate).getDouble();
  updateModel(m, localTime[id], none);
  return m;
}

void Game::setVillainLoader(function<PModel(Vec2)> loader) {
  villainLoader = std::move(loader);
}

void Game::setVillainPillager(function<bool(Vec2, int, long long, string&, bool&)> f) {
  villainPillager = std::move(f);
}

bool Game::rarPillageSite(Vec2 pos, int colIndex, long long baseVersion, string& outMessage,
    bool& outFactionEmptied) {
  outFactionEmptied = false;
  if (!villainPillager) {
    outMessage = "Not connected.";
    return false;
  }
  return villainPillager(pos, colIndex, baseVersion, outMessage, outFactionEmptied);
}

Model* Game::injectSiteModel(Vec2 pos, PModel model) {
  if (models[pos])
    return models[pos].get(); // already materialised this session
  models[pos] = std::move(model);
  Model* m = models[pos].get();
  m->position = pos;
  m->setGame(this);
  // Same wiring the Game ctor does for startup models (and addInvasionSite for injected ones): register
  // collectives, sunlight, global time, movement sectors + a localTime entry -- but KEEP the campaign
  // dweller (this is the villain that's already shown on the map), unlike addInvasionSite.
  for (Collective* col : m->getCollectives()) {
    if (auto control = dynamic_cast<VillageControl*>(col->getControl()))
      control->updateAggression(enemyAggressionLevel);
    addCollective(col);
  }
  m->updateSunlightMovement();
  for (auto c : m->getAllCreatures())
    c->setGlobalTime(getGlobalTime());
  for (auto c : m->getAllCreatures()) {
    auto level = c->getPosition().getLevel();
    level->getSectors(c->getMovementType());
    level->getSectors(c->getMovementType().setForced());
    level->getSectors(MovementType(MovementTrait::WALK).setForced());
    level->getSectors(MovementType(MovementTrait::FLY));
  }
  auto id = m->getGroundLevel()->getUniqueId();
  if (!localTime.count(id))
    localTime[id] = (m->getLocalTime() + initialModelUpdate).getDouble();
  updateModel(m, localTime[id], none);
  return m;
}

bool Game::adoptInvadedModel(PModel damaged, SavedGameInfo info) {
  Model* dm = damaged.get();
  if (!dm)
    return false;
  // Pick the collective to hand back to the player. The RAR server blob is a LIVE keeper save
  // (rarUploadKeeperDungeon uploads the stripped .kep AS-IS, not retired), so the owner's collective is
  // PLAYER -- prefer that. Fall back to RETIRED (older/retired blobs) then any collective with a leader.
  // BUG FIX: previously this ONLY looked for RETIRED; for a live save it fell through to "first collective
  // with a leader" and grabbed an ENEMY base in the dungeon (e.g. bandits/zombies), handing the player
  // control of the wrong faction instead of the keeper + his minions.
  Collective* newPlayerCol = nullptr;
  for (Collective* col : dm->getCollectives())
    if (col->getVillainType() == VillainType::PLAYER) { newPlayerCol = col; break; }
  if (!newPlayerCol)
    for (Collective* col : dm->getCollectives())
      if (col->getVillainType() == VillainType::RETIRED) { newPlayerCol = col; break; }
  if (!newPlayerCol)
    for (Collective* col : dm->getCollectives())
      if (!col->getLeaders().empty()) { newPlayerCol = col; break; }
  if (!newPlayerCol)
    return false;
  // Rebuilding PlayerControl needs the keeper config, keyed by the saved avatarId in the content factory.
  const KeeperCreatureInfo* ki = nullptr;
  for (auto& p : contentFactory->keeperCreatures)
    if (p.first == avatarId) { ki = &p.second; break; }
  if (!ki)
    return false;
  // Tear down the OLD (pristine) base model's registration before it's replaced, so nothing dangles.
  if (Model* old = models[baseModel].get()) {
    for (Collective* col : old->getCollectives()) {
      collectives.removeElementMaybe(col);
      for (auto& e : villainsByType)
        e.second.removeElementMaybe(col);
    }
    localTime.erase(old->getGroundLevel()->getUniqueId());
  }
  playerControl = nullptr;   // old control is owned by the old collective; dies with the old model below
  playerCollective = nullptr;
  models[baseModel] = std::move(damaged); // destroys the old base model + all its now-unreferenced objects
  // Un-retire: give the damaged collective back to the player (reverse of prepareSiteRetirement).
  dm->position = baseModel;
  dm->setGame(this);
  newPlayerCol->setVillainType(VillainType::PLAYER);
  auto pc = PlayerControl::create(newPlayerCol, {}, ki->tribeAlignment);
  playerControl = pc.get();
  newPlayerCol->setControl(std::move(pc)); // replaces the retirement VillageControl
  playerCollective = newPlayerCol;
  playerControl->loadImmigrationAndWorkshops(contentFactory.get(), *ki);
  // NOTE: the imported blob is a LIVE keeper save (uploaded AS-IS by rarUploadKeeperDungeon, never
  // prepareSiteRetirement'd), so every other collective already holds its natural saved state -- the
  // keeper's local enemies are tribe-hostile as they were in-game. We deliberately do NOT escalate them
  // (an older version blanket-set every non-player NONE collective to MINOR + village aggression, which
  // was only correct for the retirement path that first neuters them; on a live save it artificially turned
  // passive dungeon inhabitants/wildlife/prisoners into aggressive villains). Faithful restore = leave them.
  // Wire the model into the running game (same init as addInvasionSite / initializeModels).
  for (Collective* col : dm->getCollectives())
    addCollective(col);
  dm->updateSunlightMovement();
  for (auto c : dm->getAllCreatures())
    c->setGlobalTime(getGlobalTime());
  for (auto c : dm->getAllCreatures()) {
    auto level = c->getPosition().getLevel();
    level->getSectors(c->getMovementType());
    level->getSectors(c->getMovementType().setForced());
    level->getSectors(MovementType(MovementTrait::WALK).setForced());
    level->getSectors(MovementType(MovementTrait::FLY));
  }
  auto id = dm->getGroundLevel()->getUniqueId();
  if (!localTime.count(id))
    localTime[id] = (dm->getLocalTime() + initialModelUpdate).getDouble();
  // NOTE: unlike addInvasionSite we do NOT updateModel() here. Adoption runs inside loadGame, before
  // Game::initialize() sets the View -- ticking now would fire furniture effects -> Position::addSound
  // -> game->getView() (null) -> crash. The running main loop ticks this (now the base) model normally.
  return true;
}

void Game::rehomeToNewWorld(Campaign newCampaign, Vec2 newBasePos) {
  // RAR world regen without lossy model extraction: this is moriaty's FULL game (loaded from the server blob,
  // so nothing dangles), and we swap ONLY the world. Move the intact base model to a valid tile in the new
  // world's Table position, then replace the campaign with one built on the new world. Both worlds share the
  // configured size, so the models Table bounds already match the new campaign's sites -- no re-bounding.
  // Every on-demand site model we injected (villains travelled to / conquered) is a CACHE of the OLD world:
  // in the new one that tile may belong to a different villain, or to the keeper itself. Drop them all --
  // destroyInvasionSite unregisters their collectives properly, so nothing dangles -- and let the villain
  // loader re-download on demand. This also guarantees the keeper's target tile is free below.
  for (Vec2 v : models.getBounds())
    if (v != baseModel && models[v])
      destroyInvasionSite(v);
  if (newBasePos != baseModel) {
    models[newBasePos] = std::move(models[baseModel]);
    models[newBasePos]->position = newBasePos;
    baseModel = newBasePos;
  }
  *campaign = std::move(newCampaign); // reconstructKeeperCampaign already refreshed influence + aggressor tables
}

void Game::considerRetiredLoadedEvent(Vec2 coord) {
  if (!visited[coord]) {
    visited[coord] = true;
    if (auto retired = campaign->getSites()[coord].getRetired())
        uploadEvent("retiredLoaded", {{"retiredId", retired->fileInfo.getGameId()}});
  }
}

Statistics& Game::getStatistics() {
  return *statistics;
}

const Statistics& Game::getStatistics() const {
  return *statistics;
}

Tribe* Game::getTribe(TribeId id) const {
  return tribes.at(id).get();
}

Collective* Game::getPlayerCollective() const {
  return playerCollective;
}

PlayerControl* Game::getPlayerControl() const {
  return playerControl;
}

MusicType Game::getCurrentMusic() const {
  return musicType;
}

void Game::setDefaultMusic() {
  if (sunlightInfo.getState() == SunlightState::NIGHT)
    musicType = MusicType::NIGHT;
  else
    musicType = getCurrentModel()->getDefaultMusic().value_or(MusicType::PEACEFUL);
}

void Game::setCurrentMusic(MusicType type) {
  musicType = type;
}

const SunlightInfo& Game::getSunlightInfo() const {
  return sunlightInfo;
}

TString Game::getGameDisplayName() const {
  return gameDisplayName;
}

string Game::getGameIdentifier() const {
  return gameIdentifier;
}

void Game::setGameIdentifier(const string& id) {
  gameIdentifier = id;
}

string Game::getGameOrRetiredIdentifier(Position pos) const {
  Vec2 coords = pos.getModel()->position;
  if (auto retired = campaign->getSites()[coords].getRetired())
    return retired->fileInfo.getGameId();
  return gameIdentifier;
}

View* Game::getView() const {
  return view;
}

ContentFactory* Game::getContentFactory() {
  return &*contentFactory;
}

WarlordInfoWithReference Game::getWarlordInfo() {
  auto creatures = playerCollective->getLeaders();
  for (auto c : playerCollective->getCreatures(MinionTrait::FIGHTER))
    if (!creatures.contains(c))
      creatures.push_back(c);
  return WarlordInfoWithReference {
    creatures.transform([&](auto c) {
      CHECK(c->getPosition().getModel() == this->getMainModel().get());
      c->removeGameReferences();
      return c->getThis().giveMeSharedPointer();
    }),
    getContentFactory(),
    getGameIdentifier()
  };
}

void Game::conquered(const TString& title, int numKills, int points) {
  TString text = combineWithNewLine(concat(
    {TSentence("YOU_HAVE_CONQUERED_THIS_LAND", TString(numKills), TString(points))},
    statistics->getText()));
  view->presentText(TString(TStringId("VICTORY")), text);
  Highscores::Score score = CONSTRUCT(Highscores::Score,
        c.worldName = getWorldName();
        c.points = points;
        c.gameId = getGameIdentifier();
        c.playerName = view->translate(title);
        c.gameResult = view->translate(TStringId("ACHIEVED_WORLD_DOMINATION"));
        c.gameWon = true;
        c.turns = getGlobalTime().getVisibleInt();
        c.campaignType = campaign->getType();
  );
  highscores->add(score);
  highscores->present(view, score);
}

void Game::retired(const TString& title, int numKills, int points) {
  int turns = getGlobalTime().getVisibleInt();
  int dungeonTurns =
      (getPlayerCollective()->getLocalTime() - initialModelUpdate).getVisibleInt();
  vector<TString> text = {
    TSentence("YOU_HAVE_SURVIVED", TString(turns), TString(numKills))
  };
  if (dungeonTurns > 0) {
    text.push_back(TSentence("TURNS_DEFENDING_THE_BASE", TString(dungeonTurns)));
    if (turns > dungeonTurns)
      text.push_back(TSentence("TURNS_SPENT_ATTACKING", TString(turns - dungeonTurns)));
  }
  text.push_back(TStringId("THANK_YOU_FOR_PLAYING"));
  text.append(statistics->getText());
  view->presentText(TString(TStringId("SURVIVED")), combineWithNewLine(text));
  Highscores::Score score = CONSTRUCT(Highscores::Score,
        c.worldName = getWorldName();
        c.points = points;
        c.gameId = getGameIdentifier();
        c.playerName = view->translate(title);
        c.gameResult = view->translate(TStringId("RETIRED_GAME_RESULT"));
        c.gameWon = false;
        c.turns = turns;
        c.campaignType = campaign->getType();
  );
  highscores->add(score);
  highscores->present(view, score);
}

bool Game::isGameOver() const {
  return !!exitInfo;
}

void Game::gameOver(const Creature* creature, int numKills, int points) {
  int turns = getGlobalTime().getVisibleInt();
  int dungeonTurns = (getPlayerCollective()->getLocalTime() - initialModelUpdate).getVisibleInt();
  vector<TString> text = {
    TSentence("AND_SO_DIES", creature->getName().title())
  };
  if (auto reason = creature->getDeathReason()) {
    text[0] = TSentence("AND_SO_DIES_OF_REASON", creature->getName().title(), *reason);
  }
  text.push_back(TSentence("YOU_KILLED_AND_SCORED", TString(numKills), TString(points)));
  if (dungeonTurns > 0) {
    text.push_back(TSentence("TURNS_DEFENDING_THE_BASE", TString(dungeonTurns)));
    if (turns > dungeonTurns)
      text.push_back(TSentence("TURNS_SPENT_ATTACKING", TString(turns - dungeonTurns)));
  }
  text.append(statistics->getText());
  view->presentTextBelow(TString(TStringId("GAME_OVER")), combineWithNewLine(text));
  Highscores::Score score = CONSTRUCT(Highscores::Score,
        c.worldName = getWorldName();
        c.points = points;
        c.gameId = getGameIdentifier();
        c.playerName = view->translate(creature->getName().firstOrBare());
        if (auto reason = creature->getDeathReason())
          c.gameResult = view->translate(*reason);
        c.gameWon = false;
        c.turns = turns;
        c.campaignType = campaign->getType();
  );
  highscores->add(score);
  highscores->present(view, score);
  exitInfo = ExitInfo(ExitAndQuit());
}

Options* Game::getOptions() {
  return options;
}

Encyclopedia* Game::getEncyclopedia() {
  return encyclopedia;
}

Unlocks* Game::getUnlocks() const {
  return unlocks;
}

void Game::initialize(Options* o, Highscores* h, View* v, FileSharing* f, Encyclopedia* e, Unlocks* u,
    SteamAchievements* achievements) {
  options = o;
  highscores = h;
  view = v;
  fileSharing = f;
  encyclopedia = e;
  unlocks = u;
  steamAchievements = achievements;
  // Rebuild the friend/foe graph from content rather than backfilling only the missing tribes: the serialized
  // friendlyTribes bitsets are keyed by runtime-interned ids and are meaningless once the tribe set or its load
  // order has changed -- which is how an invaded dungeon's defenders ended up hostile to their OWN tribe and
  // wiped each other out. This also covers what addMissingTribes did (new tribes are inserted).
  Tribe::rewireFromDefs(tribes, contentFactory->tribes);
}

const string& Game::getWorldName() const {
  return campaign->getWorldName();
}

const vector<Collective*>& Game::getCollectives() const {
  return collectives;
}

void Game::addPlayer(Creature* c) {
  if (!players.contains(c))
    players.push_back(c);
}

void Game::removePlayer(Creature* c) {
  players.removeElement(c);
}

const vector<Creature*>& Game::getPlayerCreatures() const {
  return players;
}

SavedGameInfo Game::getSavedGameInfo(vector<string> spriteMods) const {
  auto factory = contentFactory.get();
  auto sortMinions = [&](vector<Creature*>& minions, Creature* leader) {
    sort(minions.begin(), minions.end(), [&] (const Creature* c1, const Creature* c2) {
        return c1 == leader ||
            (c2 != leader && c1->getBestAttack(factory).value > c2->getBestAttack(factory).value);});
    CHECK(minions[0] == leader);
  };
  if (Collective* col = getPlayerCollective()) {
    vector<Creature*> creatures = col->getCreatures();
    vector<SavedGameInfo::MinionInfo> minions;
    // RAR invasion writeback: a CONQUERED target dungeon can have NO leader (killed) or even NO creatures
    // at all (whole dungeon wiped). Build the minion display-metadata only when creatures remain; a fully
    // wiped collective just serializes with an empty minion list. Normal (leader-alive) saves are unaffected.
    if (!creatures.empty()) {
      auto& leaders = col->getLeaders();
      Creature* leader = leaders.empty() ? creatures[0] : leaders[0]; // no leader -> use any survivor to sort
      CHECK(leader);
      sortMinions(creatures, leader);
      creatures.resize(min<int>(creatures.size(), 4));
      for (Creature* c : creatures)
        minions.push_back(SavedGameInfo::MinionInfo::get(factory, c));
    }
    optional<SavedGameInfo::RetiredEnemyInfo> retiredInfo;
    if (auto id = col->getEnemyId()) {
      retiredInfo = SavedGameInfo::RetiredEnemyInfo{*id, col->getVillainType()};
      CHECK(retiredInfo->villainType == VillainType::LESSER || retiredInfo->villainType == VillainType::MAIN)
          << EnumInfo<VillainType>::getString(retiredInfo->villainType);
    }
    auto name = col->getName()->shortened.value_or(TString("???"_s));
    return SavedGameInfo{minions, retiredInfo, ""_s, getSaveProgressCount(), std::move(spriteMods)};
  } else {
    vector<Creature*> allCreatures;
    for (auto player : players)
      for (auto c : dynamic_cast<Player*>(player->getController())->getTeam())
        if (!allCreatures.contains(c))
          allCreatures.push_back(c);
    sortMinions(allCreatures, players[0]);
    return SavedGameInfo{
        allCreatures.transform([&](auto c) { return SavedGameInfo::MinionInfo::get(factory, c); }),
        none,
        string(players[0]->getName().bare().data()),
        getSaveProgressCount(),
        std::move(spriteMods)};
  }
}

void Game::uploadEvent(const string& name, const map<string, string>& m) {
  auto values = m;
  values["eventType"] = name;
  values["gameId"] = getGameIdentifier();
  fileSharing->uploadGameEvent(values);
}

void Game::addAnalytics(const string& name, const string& value) {
  uploadEvent("customEvent", {
    {"name", name},
    {"value", value}
  });
}

void Game::achieve(AchievementId id) const {
  if (steamAchievements)
    steamAchievements->achieve(id);
  if (!unlocks->isAchieved(id)) {
    unlocks->achieve(id);
    if (!steamAchievements) {
      auto& info = contentFactory->achievements.at(id);
      view->windowedMessage(info.viewId, TSentence("ACHIEVEMENT_UNLOCKED", info.name));
    }
  }
}

void Game::handleMessageBoard(Position pos, Creature* c) {
  auto gameId = getGameOrRetiredIdentifier(pos);
  auto boardId = int(combineHash(pos.getCoord(), pos.getLevel()->getUniqueId(), gameId));
  FileSharing::CancelFlag cancel;
  view->displaySplash(nullptr, TStringId("FETCHING_BOARD_CONTENTS"), [&] {
    cancel.cancel();
  });
  vector<FileSharing::BoardMessage> messages;
  optional<string> error;
  thread t([&] {
    if (auto m = fileSharing->getBoardMessages(cancel, boardId))
      messages = *m;
    else
      error = m.error();
    view->clearSplash();
  });
  view->refreshView();
  t.join();
  if (error) {
    view->presentText(none, TString(*error));
    return;
  }
  auto data = ScriptedUIDataElems::Record{};
  auto list = ScriptedUIDataElems::List{};
  ScriptedUIState uiState{};
  for (auto message : messages) {
    list.push_back(ScriptedUIDataElems::Record{
      {
        {"author", TString(message.author)},
        {"text", TString("\"" + message.text + "\"")}
      }
    });
  }
  data.elems["messages"] = std::move(list);
  bool wrote = false;
  data.elems["write_something"] = ScriptedUIDataElems::Callback{
      [this, c, boardId, gameId] {
        if (auto text = view->getText(TStringId("ENTER_MESSAGE"), "", 80)) {
          if (text->size() >= 2) {
            if (!fileSharing->uploadBoardMessage(gameId, boardId, view->translate(c->getName().title()), *text))
              view->presentText(none, TStringId("ENABLE_ONLINE_SETTING"));
          } else
            view->presentText(none, TStringId("BOARD_MESSAG_TOO_SHORT"));
        }
        return true;
      }
  };
  view->scriptedUI("message_board", data, uiState);
}

void Game::considerAllianceAttack() {
  if (!allianceAttackPossible || !getPlayerControl() || !Random.roll(1000))
    return;
  int numTriggered = 0;
  vector<Collective*> candidates;
  for (auto col : getVillains(VillainType::MAIN)) {
    if (!isVillainActive(col) || !col->getControl()->canPerformAttack())
      return;
    if (col->getName()->race.empty())
      continue;
    auto triggers = col->getTriggers(getPlayerCollective());
    if (triggers.empty())
      continue;
    candidates.push_back(col);
    if (!triggers.filter([](auto& t) { return t.value > 0; }).empty())
      ++numTriggered;
  }
  if (numTriggered >= candidates.size() - 1 && candidates.size() >= 2) {
    for (auto col : candidates)
      col->getControl()->launchAllianceAttack(candidates.filter([&](auto other) { return other != col; }));
    getPlayerControl()->addAllianceAttack(candidates);
    allianceAttackPossible = false;
    addAnalytics("alliance", "");
  }
}

bool Game::gameWon() const {
  // RAR online is an endless, respawning world: the MAIN villains live on the shared server and are only
  // injected on-demand, so getCollectives() holds just the one you're fighting. The stock check would then
  // report "you conquered this land" the instant you kill that single injected MAIN villain -- a false victory
  // (40+ villains remain on the server and more respawn). The game simply never ends this way online.
  if (rarEnabled())
    return false;
  for (Collective* col : getCollectives())
    if (!col->isConquered() && col->getVillainType() == VillainType::MAIN)
      return false;
  return true;
}

void Game::considerAchievement(const GameEvent& event) {
  using namespace EventInfo;
  event.visit<void>(
      [&](const ConqueredEnemy& info) {
        if (info.byPlayer && info.collective != playerCollective)
          switch (info.collective->getVillainType()) {
            case VillainType::LESSER:
              achieve(AchievementId("lesser_villain"));
              break;
            case VillainType::MAIN:
              achieve(AchievementId("main_villain"));
              break;
            default:
              break;
          }
      },
      [&](const CreatureKilled& info) {
        if (auto& a = info.victim->getAttributes().killedAchievement)
          achieve(*a);
      },
      [&](const CreatureStunned& info) {
        if (auto& a = info.victim->getAttributes().killedAchievement)
          achieve(*a);
      },
      [&](const RetiredGame& info) {
        achieve(AchievementId("retired"));
      },
      [](auto&) {}
  );
}

void Game::clearPlayerControl() {
  playerControl = nullptr;
}

void Game::addEvent(const GameEvent& event) {
  if (event.contains<EventInfo::CreatureMoved>() && !!playerControl)
    playerControl->onEvent(event); // shortcut to optimize because only PlayerControl cares about this event
  else
    for (Vec2 v : models.getBounds())
      if (models[v])
        models[v]->addEvent(event);
  using namespace EventInfo;
  event.visit<void>(
      [&](const ConqueredEnemy& info) {
        Collective* col = info.collective;
        if (col->getVillainType() != VillainType::NONE) {
          if (auto id = col->getEnemyId())
            uploadEvent("customEvent", {
              {"name", "villainConquered"},
              {"value", id->data()}
            });
          Vec2 coords = col->getModel()->position;
          if (!campaign->isDefeated(coords)) {
            if (auto retired = campaign->getSites()[coords].getRetired())
              uploadEvent("retiredConquered", {{"retiredId", retired->fileInfo.getGameId()}});
            if (coords != campaign->getPlayerPos())
              campaign->setDefeated(contentFactory.get(), coords);
          }
        }
        if (col->getVillainType() == VillainType::LESSER || col->getVillainType() == VillainType::MAIN)
          ++numLesserVillainsDefeated;
        if (col->getVillainType() == VillainType::MAIN && gameWon()) {
          addEvent(WonGame{});
        }
      },
      [&](const auto&) {}
  );
  considerAchievement(event);
}

