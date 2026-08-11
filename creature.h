/* Copyright (C) 2013-2014 Michal Brzozowski (rusolis@poczta.fm)

   This file is part of KeeperRL.

   KeeperRL is free software; you can redistribute it and/or modify it under the terms of the
   GNU General Public License as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   KeeperRL is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
   even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along with this program.
   If not, see http://www.gnu.org/licenses/ . */

#pragma once

#include "enums.h"
#include "event_listener.h"
#include "unique_entity.h"
#include "creature_action.h"
#include "renderable.h"
#include "movement_type.h"
#include "position.h"
#include "event_generator.h"
#include "entity_set.h"
#include "destroy_action.h"
#include "best_attack.h"
#include "msg_type.h"
#include "game_time.h"
#include "creature_status.h"
#include "view_id.h"
#include "furniture_type.h"
#include "attr_type.h"
#include "buff_id.h"
#include "player_message.h"

class SpecialTrait;
class Level;
class Tribe;
class ViewObject;
class Attack;
class Controller;
class ControllerFactory;
class PlayerMessage;
class ShortestPath;
class LevelShortestPath;
class Equipment;
class Spell;
class CreatureAttributes;
class Body;
class MinionActivityMap;
class CreatureName;
class SpellMap;
class Sound;
class Game;
class CreatureListener;
class CreatureDebt;
class Vision;
struct AdjectiveInfo;
struct MovementInfo;
struct NavigationFlags;
class SpellMap;
class SpellSchool;
class ContentFactory;
struct AutomatonPart;
struct PromotionInfo;

// How loaded a crafter gets before it stops working and makes one trip to storage. Shared by the producing
// side (stop crafting) and the AI side (go store it) so the two can never disagree and leave a minion either
// looping to the storeroom per item, or forced past its carry limit.
constexpr double craftCarryFraction = 0.8;

class Creature : public Renderable, public UniqueEntity<Creature>, public OwnedObject<Creature>, public EventListener<Creature> {
  public:
  Creature(const ViewObject&, TribeId, CreatureAttributes, SpellMap);
  virtual ~Creature();

  static vector<vector<Creature*>> stack(const vector<Creature*>&,
      function<string(Creature*)> addSuffix = [](Creature*) { return ""; });

  const ViewObject& getViewObjectFor(const Tribe* observer) const;
  void rarSetMovementInfo(MovementInfo); // RAR live PvP: drive the slide animation on a stream-moved puppet
  void setAlternativeViewId(optional<ViewId>);
  bool hasAlternativeViewId() const;
  void setViewId(ViewId);
  void makeMove();
  optional<LocalTime> getLocalTime() const;
  optional<GlobalTime> getGlobalTime() const;
  Level* getLevel() const;
  Game* getGame() const;
  const vector<Creature*>& getVisibleEnemies() const;
  Creature* getClosestEnemy(bool meleeOnly = false) const;
  const vector<Creature*>& getVisibleCreatures() const;
  bool shouldAIAttack(const Creature* enemy) const;
  bool shouldAIChase(const Creature* enemy) const;
  bool dontChase() const;
  vector<Position> getVisibleTiles() const;
  void setGlobalTime(GlobalTime);
  void setPosition(Position);
  Position getPosition() const;
  bool dodgeAttack(const Attack&);
  bool takeDamage(const Attack&);
  void onAttackedBy(Creature*);
  bool heal(double amount = 1000);
  void setTeamExperience(double, bool leadershipExp);
  void take(PItem item, const ContentFactory* = nullptr);
  void take(vector<PItem> item);
  const Equipment& getEquipment() const;
  Equipment& getEquipment();
  vector<PItem> steal(const vector<Item*> items);
  bool canSeeInPosition(const Creature*, GlobalTime) const;
  bool canSeeInPositionIfNotBlind(const Creature*, GlobalTime) const;
  bool canSeeOutsidePosition(const Creature*, GlobalTime) const;
  bool canSee(const Creature*) const;
  bool canSeeIfNotBlind(const Creature*, GlobalTime) const;
  bool canSee(Position) const;
  bool canSee(Vec2) const;
  bool isEnemy(const Creature*) const;
  void tick();
  void upgradeViewId(int level);
  ViewIdList getMaxViewIdUpgrade() const;
  ViewIdList getViewIdWithWeapon() const;

  const CreatureName& getName() const;
  CreatureName& getName();
  const char* identify() const;
  int getAttr(AttrType, bool includeWeapon = true, bool includeTeamExp = true) const;
  int getAttrWithExp(AttrType, int combatExperience, bool includeWeapon = true) const;
  int getSpecialAttr(AttrType, const Creature* against) const;
  int getAttrBonus(AttrType, int rawAttr, bool includeWeapon) const;

  double getFlankedMod() const;
  int getPoints() const;
  const Vision& getVision() const;
  const CreatureDebt& getDebt() const;
  CreatureDebt& getDebt();

  const Tribe* getTribe() const;
  Tribe* getTribe();
  TribeId getTribeId() const;
  void setTribe(TribeId);
  bool isFriend(const Creature*) const;
  vector<Item*> getGold(int num) const;

  void takeItems(vector<PItem> items, Creature* from);
  bool canTakeItems(const vector<Item*>& items) const;

  const CreatureAttributes& getAttributes() const;
  CreatureAttributes& getAttributes();
  pair<CreatureAttributes, SpellMap> setAttributes(CreatureAttributes, SpellMap);
  void pushAttributes(CreatureAttributes, SpellMap);
  void popAttributes();
  const Body& getBody() const;
  Body& getBody();
  bool isDead() const;
  void clearInfoForRetiring();
  void clearLastCombatIntent(); // RAR: drop the raw Creature* combat-intent (e.g. before a foreign model is freed)
  bool rarIsOrphanedSummon(const Model* m) const; // RAR: a summon whose summoner is not on model m
  optional<TString> getDeathReason() const;
  GlobalTime getDeathTime() const;
  struct KillInfo {
    Creature::Id SERIAL(creature);
    ViewIdList SERIAL(viewId);
    SERIALIZE_ALL(creature, viewId)
  };
  const vector<KillInfo>& getKills() const;
  const vector<TString>& getKillTitles() const;

  MovementType getMovementType() const;
  MovementType getMovementType(Game*) const;
  MovementType getSelfMovementType(Game*, bool amSteed) const;

  int getDifficultyPoints() const;

  TString getPluralTheName(Item* item, int num) const;
  TString getPluralAName(Item* item, int num) const;
  CreatureAction move(Position, optional<Position> nextPosIntent = none) const;
  CreatureAction move(Vec2) const;
  CreatureAction forceMove(Position) const;
  CreatureAction forceMove(Vec2) const;
  static CreatureAction wait();
  vector<Item*> getPickUpOptions() const;
  CreatureAction pickUp(const vector<Item*>& item) const;
  bool canCarryMoreWeight(double) const;
  // How many of these this creature could still pick up (0 = already at its limit). The honest weight check --
  // canCarryMoreWeight ignores its argument and only looks at the CURRENT load.
  int canCarry(const vector<Item*>&) const;
  // Carrying at least this fraction of the carry limit. Used to batch crafting: a crafter keeps working while
  // it has room and makes ONE trip to storage when it is nearly loaded, instead of a trip per item.
  bool isCarryingFractionOfLimit(double fraction) const;
  CreatureAction drop(const vector<Item*>& item) const;
  void drop(vector<PItem> item);
  CreatureAction attack(Creature*) const;
  int getDefaultWeaponDamage() const;
  CreatureAction execute(Creature*, TStringId verbSecond, TStringId verbThird) const;
  CreatureAction applyItem(Item* item) const;
  CreatureAction equip(Item* item, const ContentFactory* = nullptr) const;
  CreatureAction unequip(Item* item, const ContentFactory* = nullptr) const;
  bool canEquipIfEmptySlot(const Item* item, TString* reason = nullptr) const;
  bool canEquip(const Item* item) const;
  CreatureAction throwItem(Item*, Position target, bool isFriendlyAI) const;
  optional<int> getThrowDistance(const Item*) const;
  CreatureAction applySquare(Position, FurnitureLayer) const;
  CreatureAction hide() const;
  bool isHidden() const;
  bool knowsHiding(const Creature*) const;
  CreatureAction flyAway() const;
  CreatureAction disappear() const;
  CreatureAction torture(Creature*) const;
  CreatureAction chatTo(Creature*) const;
  CreatureAction pet(Creature*) const;
  CreatureAction stealFrom(Vec2 direction, const vector<Item*>&) const;
  CreatureAction give(Creature* whom, vector<Item*> items) const;
  CreatureAction payFor(const vector<Item*>&) const;
  CreatureAction construct(Vec2 direction, FurnitureType) const;
  CreatureAction whip(const Position&, double animChance) const;
  CreatureAction eat(Item*) const;
  CreatureAction destroy(Vec2 direction, const DestroyAction&) const;
  void forceMount(Creature* whom);
  void forceDismount(Vec2 dir);
  void tryToDismount();
  bool isSteedGoodSize(Creature* whom) const;
  vector<TString> getSteedSizes() const;
  CreatureAction mount(Creature* whom) const;
  Creature* getSteed() const;
  Creature* getRider() const;
  CreatureAction dismount() const;
  void destroyImpl(Vec2 direction, const DestroyAction& action);
  CreatureAction copulate(Vec2 direction) const;
  bool canCopulateWith(const Creature*) const;
  CreatureAction consume(Creature*) const;
  bool canConsume(const Creature*) const;
  CreatureAction push(Creature*);

  void displace(Vec2);
  void retire();
  void removeGameReferences();

  void increaseExpLevel(AttrType, double increase);

  BestAttack getBestAttack(const ContentFactory*) const;
  BestAttack getBestAttackWithExp(const ContentFactory*, int combatExp) const;

  vector<pair<Item*, double>> getRandomWeapons() const;
  int getMaxSimultaneousWeapons() const;
  Item* getFirstWeapon() const;
  void dropUnsupportedEquipment();
  vector<vector<Item*>> stackItems(vector<Item*>) const;
  CreatureAction moveTowards(Position, NavigationFlags);
  CreatureAction moveTowards(Position);
  CreatureAction moveAway(Position, bool pathfinding = true);
  vector<Position> getCurrentPath() const;
  bool canNavigateToOrNeighbor(Position) const;
  bool canNavigateTo(Position pos) const;

  bool atTarget() const;

  enum class DropType { NOTHING, ONLY_INVENTORY, EVERYTHING };
  void dieWithAttacker(Creature* attacker, DropType = DropType::EVERYTHING);
  void dieWithLastAttacker(DropType = DropType::EVERYTHING);
  void dieNoReason(DropType = DropType::EVERYTHING);
  void dieWithReason(TString reason, DropType = DropType::EVERYTHING);

  void setHeld(Creature* holding);
  Creature* getHoldingCreature() const;

  bool isPlayer() const;

  void you(MsgType type, TString param) const;
  void you(MsgType type) const;
  void verb(TStringId second, TStringId third, MessagePriority = MessagePriority::NORMAL) const;
  void verb(TStringId second, TStringId third, TString param, MessagePriority = MessagePriority::NORMAL) const;
  void verb(TStringId second, TStringId third, TString param1, TString param2,
      MessagePriority = MessagePriority::NORMAL) const;
  void secondPerson(const PlayerMessage&) const;
  void thirdPerson(const PlayerMessage& playerCanSee, const PlayerMessage& cant) const;
  void thirdPerson(const PlayerMessage& playerCanSee) const;
  void message(const PlayerMessage&) const;
  void privateMessage(const PlayerMessage&) const;
  void addFX(const FXInfo&) const;

  Controller* getController() const;
  void pushController(PController);
  void setController(PController);
  void popController();

  TimeInterval calculateSpellCooldown(Range cooldown) const;
  CreatureAction castSpell(const Spell*) const;
  CreatureAction castSpell(const Spell*, Position) const;
  TimeInterval getSpellDelay(const Spell*) const;
  bool isReady(const Spell*) const;
  const SpellMap& getSpellMap() const;
  SpellMap& getSpellMap();

  SERIALIZATION_DECL(Creature)

  bool addEffect(LastingEffect, TimeInterval time, bool msg = true);
  bool addEffect(LastingEffect, TimeInterval time, GlobalTime, bool msg = true, const ContentFactory* = nullptr);
  bool removeEffect(LastingEffect, bool msg = true);
  bool addPermanentEffect(LastingEffect, int count = 1, bool msg = true, const ContentFactory* = nullptr);
  bool removePermanentEffect(LastingEffect, int count = 1, bool msg = true);
  bool isAffected(LastingEffect, optional<GlobalTime>) const;
  bool isAffected(LastingEffect, GlobalTime) const;
  bool isAffected(LastingEffect) const;
  optional<TimeInterval> getTimeRemaining(LastingEffect) const;

  bool addEffect(BuffId, TimeInterval time, bool msg = true);
  bool addEffect(BuffId, TimeInterval time, GlobalTime, const ContentFactory*, bool msg = true);
  bool removeEffect(BuffId, bool msg = true);
  bool addPermanentEffect(BuffId, int count = 1, bool msg = true, const ContentFactory* = nullptr);
  bool removePermanentEffect(BuffId, int count = 1, bool msg = true, const ContentFactory* = nullptr);
  bool isAffected(BuffId) const;
  bool isAffectedPermanently(BuffId) const;

  bool isUnknownAttacker(const Creature*) const;
  vector<AdjectiveInfo> getGoodAdjectives(const ContentFactory*) const;
  vector<AdjectiveInfo> getBadAdjectives(const ContentFactory*) const;

  void addPersonalEvent(TString);
  struct CombatIntentInfo {
    enum class Type { ATTACK, CHASE, RETREAT } SERIAL(type);
    bool isHostile() const;
    // WeakPointer (not raw Creature*) so it serializes safely even if the attacker's model was freed
    // (e.g. a destroyed invaded dungeon) -- an expired weak_ptr writes as null instead of a getThis crash.
    WeakPointer<Creature> SERIAL(attacker);
    GlobalTime SERIAL(time);
    SERIALIZE_ALL(attacker, time, type)
  };
  void addCombatIntent(Creature* attacker, CombatIntentInfo::Type);
  optional<CombatIntentInfo> getLastCombatIntent() const;
  void onKilledOrCaptured(Creature* victim);
  Creature* getsCreditForKills();
  void updateCombatExperience(Creature* victim);
  double getCombatExperience(bool respectMaxPromotion, bool includeTeamExp) const;
  double getTeamExperience() const;
  void setCombatExperience(double);
  void setMaxPromotion(int level);
  int getCombatExperienceCap() const;
  int getRawAttr(AttrType, int combatExp) const;

  void updateViewObject(const ContentFactory*);
  void updateViewObjectFlanking();
  void swapPosition(Vec2 direction, bool withExcuseMe = true);
  bool canSwapPositionWithEnemy(Creature* other) const;
  bool canSwapPositionInMovement(Creature* other) const;
  vector<PItem> generateCorpse(const ContentFactory*, Game*, bool instantlyRotten = false);
  int getLastMoveCounter() const;

  EnumSet<CreatureStatus>& getStatus();
  const EnumSet<CreatureStatus>& getStatus() const;
  vector<Creature*> getCompanions() const;
  Creature* getSteedCompanion() const;
  void removeCompanions(int index);
  void toggleCaptureOrder();
  void setCaptureOrder(bool);
  bool isCaptureOrdered() const;
  bool canBeCaptured() const;
  void removePrivateEnemy(const Creature*);
  void setDuel(TribeId enemyTribe, Creature* opponent, GlobalTime timeout);
  bool wasDuel() const;
  const vector<AutomatonPart>& getAutomatonParts() const;
  bool isAutomaton() const;
  void addAutomatonPart(AutomatonPart);
  vector<PItem> SERIAL(drops);
  struct PhylacteryInfo {
    Position SERIAL(pos);
    FurnitureType SERIAL(type);
    Creature* killedBy = nullptr;
    SERIALIZE_ALL(pos, type);
  };
  void setPhylactery(Position, FurnitureType);
  const optional<PhylacteryInfo>& getPhylactery() const;
  void unbindPhylactery();
  void addPromotion(PromotionInfo);
  const vector<PromotionInfo>& getPromotions() const;
  bool addButcheringEvent(const string& villageName);

  void onEvent(const GameEvent&);

  enum class SpeedModifier {
    SLOW,
    NORMAL,
    FAST
  };

  unordered_set<string> SERIAL(effectFlags);
  vector<SpecialTrait> SERIAL(specialTraits);
  Creature::Id SERIAL(getsKillCredits);

  TribeSet getFriendlyTribes() const;

  private:

  CreatureAction moveTowards(Position, bool away, NavigationFlags);
  optional<MovementInfo> spendTime(TimeInterval = 1_visible, SpeedModifier = SpeedModifier::NORMAL);
  void addMovementInfo(MovementInfo);

  HeapAllocated<CreatureAttributes> SERIAL(attributes);
  Position SERIAL(position);
  HeapAllocated<Equipment> SERIAL(equipment);
  heap_optional<LevelShortestPath> SERIAL(shortestPath);
  struct LastStairsNavigation {
    Level* from;
    Position target;
    Position stairs;
  };
  optional<LastStairsNavigation> lastStairsNavigation;
  EntitySet<Creature> SERIAL(knownHiding);
  TribeId SERIAL(tribe);
  optional<GlobalTime> SERIAL(deathTime);
  bool SERIAL(hidden) = false;
  Creature* lastAttacker = nullptr;
  public:
  Creature* getLastAttacker() const { return lastAttacker; } // RAR diagnostics: who killed this creature
  private:
  optional<TString> SERIAL(deathReason);
  optional<Position> SERIAL(nextPosIntent);
  EntitySet<Creature> SERIAL(unknownAttackers);
  EntityMap<Creature, GlobalTime> SERIAL(privateEnemies);
  optional<Creature::Id> SERIAL(holding);
  vector<PController> SERIAL(controllerStack);
  vector<TString> SERIAL(killTitles);
  vector<KillInfo> SERIAL(kills);
  mutable int SERIAL(difficultyPoints) = 0;
  int SERIAL(points) = 0;
  using MoveId = pair<int, LevelId>;
  MoveId getCurrentMoveId() const;
  mutable optional<pair<MoveId, vector<Creature*>>> visibleEnemies;
  mutable optional<pair<MoveId, vector<Creature*>>> visibleCreatures;
  HeapAllocated<Vision> SERIAL(vision);
  bool forceMovement = false;
  void setForceMovement(bool value);
  optional<CombatIntentInfo> SERIAL(lastCombatIntent);
  HeapAllocated<CreatureDebt> SERIAL(debt);
  int SERIAL(lastMoveCounter) = 0;
  EnumSet<CreatureStatus> SERIAL(statuses);
  bool SERIAL(capture) = 0;
  bool captureDamage(double damage, Creature* attacker);
  mutable Game* gameCache = nullptr;
  optional<GlobalTime> SERIAL(globalTime);
  void considerMovingFromInaccessibleSquare();
  void updateLastingFX(ViewObject&, const ContentFactory*);
  HeapAllocated<SpellMap> SERIAL(spellMap);
  optional<ViewId> SERIAL(primaryViewId);
  struct CompanionGroup {
    vector<WeakPointer<Creature>> SERIAL(creatures);
    optional<AttrType> SERIAL(statsBase);
    bool SERIAL(notUsed);
    SERIALIZE_ALL(creatures, statsBase, notUsed)
  };
  vector<CompanionGroup> SERIAL(companions);
  void tickCompanions();
  bool considerSavingLife(DropType, const Creature* attacker);
  void tryToDestroyLastingEffect(LastingEffect);
  vector<AdjectiveInfo> getSpecialAttrAdjectives(const ContentFactory*, bool good) const;
  vector<AutomatonPart> SERIAL(automatonParts);
  vector<pair<CreatureAttributes, SpellMap>> SERIAL(attributesStack);
  optional<PhylacteryInfo> SERIAL(phylactery);
  bool considerPhylactery(DropType, const Creature* attacker);
  bool considerPhylacteryOrSavingLife(DropType, const Creature* attacker);
  vector<PromotionInfo> SERIAL(promotions);
  PCreature SERIAL(steed);
  vector<pair<BuffId, GlobalTime>> SERIAL(buffs);
  HashMap<BuffId, int> SERIAL(buffCount);
  HashMap<BuffId, int> SERIAL(buffPermanentCount);
  vector<AdjectiveInfo> getLastingEffectAdjectives(const ContentFactory*, bool bad) const;
  bool removeBuff(int index, bool msg);
  bool processBuffs();
  double SERIAL(combatExperience) = 0;
  int SERIAL(maxPromotion) = 10000;
  double SERIAL(teamExperience) = 0;
  bool SERIAL(leadershipExp) = false;
  int SERIAL(highestAttackValueEver) = 0;
  struct ButcherInfo {
    string SERIAL(villageName);
    int SERIAL(kills);
    SERIALIZE_ALL(villageName, kills)
  };
  optional<ButcherInfo> SERIAL(butcherInfo);
  AttrType modifyDamageAttr(AttrType, const ContentFactory*) const;
  struct DuelInfo {
    TribeId SERIAL(enemy);
    Creature::Id SERIAL(opponent);
    GlobalTime SERIAL(timeout);
    SERIALIZE_ALL(enemy, opponent, timeout)
  };
  optional<DuelInfo> SERIAL(duelInfo);
};

struct AdjectiveInfo {
  TString name;
  TString help;
  optional<int> timeout;
  int count;
  TString getText() const;
};

#ifndef PARSE_GAME
CEREAL_CLASS_VERSION(Creature, 2)
#endif
