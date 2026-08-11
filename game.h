#pragma once

#include "util.h"
#include "sunlight_info.h"
#include "tribe.h"
#include "enum_variant.h"
#include "position.h"
#include "exit_info.h"
#include "game_time.h"
#include "t_string.h"
#include "attack_trigger.h"   // RAR villain waves: cached TriggerInfo for the villages panel
#include "villain_group.h"
#include "saved_game_info.h" // RAR: EndedInvasion/ActiveInvasion store a SavedGameInfo by value
#include "user_input.h"      // RAR live PvP: buffered player orders (queue a press, run it next turn)

class Options;
class Highscores;
class View;
class Statistics;
class PlayerControl;
class CreatureView;
class FileSharing;
class Technology;
class GameEvent;
class Campaign;
class SavedGameInfo;
struct CampaignSetup;
class AvatarInfo;
class ContentFactory;
class NameGenerator;
class Encyclopedia;
class Unlocks;
class SteamAchievements;
class ProgressMeter;

struct WarlordInfoWithReference {
  vector<shared_ptr<Creature>> SERIAL(creatures);
  ContentFactory* SERIAL(contentFactory);
  string SERIAL(gameIdentifier);
  SERIALIZE_ALL_NO_VERSION(creatures, serializeAsValue(contentFactory), gameIdentifier)
};

class Game : public OwnedObject<Game> {
  public:
  static PGame campaignGame(Table<PModel>&&, CampaignSetup, AvatarInfo, ContentFactory, map<string, string> analytics);
  static PGame splashScreen(PModel&&, const CampaignSetup&, ContentFactory, View*);
  static PGame warlordGame(Table<PModel>, CampaignSetup, vector<PCreature>, ContentFactory, string avatarId);

  optional<ExitInfo> update(double timeDiff, milliseconds endTime);
  void setExitInfo(ExitInfo);
  Options* getOptions();
  Encyclopedia* getEncyclopedia();
  Unlocks* getUnlocks() const;
  EnemyAggressionLevel getEnemyAggressionLevel() const;
  void initialize(Options*, Highscores*, View*, FileSharing*, Encyclopedia*, Unlocks*, SteamAchievements*);
  void initializeModels(ProgressMeter&);
  View* getView() const;
  ContentFactory* getContentFactory();
  vector<VillainGroup> getPlayerVillainGroups() const; // RAR: whose allies are MY allies on the shared map
  TribeId getPlayerTribe() const; // RAR: the tribe that vetoes an ALLY my minions would attack anyway
  const string& getAvatarId() const { return avatarId; } // RAR: keeper-type id, needed to rebuild PlayerControl on import
  WarlordInfoWithReference getWarlordInfo();
  void exitAction();
  Model* chooseSite(Model* current); // non-const: may queue an on-demand RAR invasion
  void presentWorldmap();
  // if destinations are empty then creature is placed on the edge of the map
  void transferCreature(Creature*, Model* to, const vector<Position>& destinations = {});
  bool canTransferCreature(Creature*, Model* to);
  Position getTransferPos(Model* from, Model* to) const;
  string getGameIdentifier() const;
  // RAR: online identity is the composite "<account>~<keeper>", derived at runtime (NOT the value baked into an
  // old save). Set it right after loading a keeper so save-path + server keying use the current identity; a
  // migrated save then re-serializes the composite on its next save, self-healing.
  void setGameIdentifier(const string&);
  string getGameOrRetiredIdentifier(Position) const;
  TString getGameDisplayName() const;
  MusicType getCurrentMusic() const;
  void setCurrentMusic(MusicType);
  void setDefaultMusic();
  Statistics& getStatistics();
  const Statistics& getStatistics() const;
  Tribe* getTribe(TribeId) const;
  GlobalTime getGlobalTime() const;
  Collective* getPlayerCollective() const;
  PlayerControl* getPlayerControl() const;
  void addPlayer(Creature*);
  void removePlayer(Creature*);
  const vector<Creature*>& getPlayerCreatures() const;

  int getModelDistance(const Collective* c1, const Collective* c2) const;

  const vector<Collective*>& getVillains(VillainType) const;
  const vector<Collective*>& getCollectives() const;

  const SunlightInfo& getSunlightInfo() const;
  const string& getWorldName() const;
  bool gameWon() const;

  void gameOver(const Creature* player, int numKills, int points);
  void conquered(const TString& title, int numKills, int points);
  void retired(const TString& title, int numKills, int points);

  bool isGameOver() const;
  bool isTurnBased();
  // RAR live PvP: force real-time even while controlling a team (live player-vs-player must never pause).
  // NOT serialized -- it only applies to the live battle session.
  void rarSetLivePvp(bool);
  // Am I the INVADER in this live battle? The invader is the non-authoritative side: her orders are forwarded to
  // the defender and her creatures are positioned by his simulation. The DEFENDER must keep normal local control.
  void rarSetLiveInvader(bool b) { rarLiveInvader = b; }
  bool rarIsLiveInvader() const { return rarLiveInvader; }
  bool rarIsLivePvp() const { return rarLivePvp; }
  // RAR live PvP: AUTO FIGHT. A controlled creature normally does nothing until the player inputs something,
  // so a defender holding a minion had it stand still while everything around it fought. Off by default.
  bool rarIsAutoFight() const { return rarAutoFight; }
  void rarToggleAutoFight() { rarAutoFight = !rarAutoFight; }
  // RAR live PvP order buffer: a keypress can arrive at ANY moment (the clock never stops), including while the
  // sim is running other creatures' turns. Every input is parked here and consumed by the next controlled
  // creature that takes its turn -- so a press is never dropped and never needs to hit a timing window.
  // Movement is held in its OWN slot and always takes priority: a new move supersedes the previous one (holding a
  // key or clicking repeatedly must not pool stale orders that replay later), and it runs on the very NEXT turn
  // rather than queueing behind incidental inputs -- one map click can also emit e.g. CREATURE_MAP_CLICK, and with
  // one order consumed per turn those made a click take several turns to take effect. Other orders queue FIFO,
  // capped so a backlog can never build up.
  // RAR live PvP: an ORDER the invader issued (e.g. "move <levelId> <x> <y> <chase>"). Her client never moves her
  // creatures itself -- that would fight the defender's authoritative positions -- it sends the order to him, his
  // simulation walks the team, and the result streams back. Set by Player, drained by the game loop.
  // RAR live PvP: the defender's simulation reported one of MY controlled creatures dead. We must NOT kill it
  // behind the game's back (a dead creature left in the time queue FATALs in Model::update) -- instead flag it and
  // let the game loop end the battle properly.
  // victim: the creature whose authoritative death ended the battle, when it could not be killed on the spot
  // (it was the last one under the player's control). The game loop kills it once control has been released.
  void rarSetLiveDefeat(long long victim = -1) {
    rarLiveDefeat = true;
    if (victim >= 0)
      rarLiveDefeatVictim = victim;
  }
  optional<long long> rarTakeLiveDefeatVictim() {
    auto v = rarLiveDefeatVictim;
    rarLiveDefeatVictim = none;
    return v;
  }
  bool rarIsLiveDefeat() const { return rarLiveDefeat; }
  void rarClearLiveDefeat() { rarLiveDefeat = false; }
  void rarSetOrder(const string& s) { rarOutgoingOrder = s; }
  optional<string> rarTakeOrder() { auto r = rarOutgoingOrder; rarOutgoingOrder = none; return r; }
  void rarQueueInput(UserInput i) {
    if (i.getId() == UserInputId::MOVE || i.getId() == UserInputId::TILE_CLICK)
      rarPendingMove = i;
    else if (rarPendingInput.size() < 4)
      rarPendingInput.push_back(i);
  }
  optional<UserInput> rarTakeInput() {
    if (rarPendingMove) {
      UserInput r = *rarPendingMove;
      rarPendingMove = none;
      return r;
    }
    if (rarPendingInput.empty())
      return none;
    UserInput r = rarPendingInput.front();
    rarPendingInput.erase(rarPendingInput.begin());
    return r;
  }
  bool isVillainActive(const Collective*);
  SavedGameInfo getSavedGameInfo(vector<string> spriteMods) const;

  /** Removes creature from the queue. Assumes it has already been removed from its level. */
  void killCreature(Creature*, Creature* attacker);

  void handleMessageBoard(Position, Creature*);

  PModel& getMainModel();
  void rehomeToNewWorld(Campaign newCampaign, Vec2 newBasePos); // RAR: move the intact base to a new-world tile + swap campaign
  vector<Model*> getAllModels() const;
  bool isSingleModel() const;
  int getSaveProgressCount() const;
  Model* getCurrentModel() const;
  int getModelDifficulty(const Model*) const;
  bool passesMaxAggressorCutOff(const Model*);
  int getNumLesserVillainsDefeated() const;
  // RAR: evaluate every alive world-map villain's attack triggers and deliver any due wave. Call once per
  // update while the player is at his base. See the villainWaves members below.
  void considerVillainWaves();
  // RAR: re-type the world map for THIS keeper (others' allies -> MINOR) from the server roster. Called after
  // a load rebuilds the campaign from the raw server blob, which carries the world's original villain types.
  void reconcileVillainsForLoad();
  // RAR: read-only views for the villages panel, which must show world-map villains even though they have no
  // local Collective (see considerVillainWaves).
  struct VillainWave {
    int nextEligible = 0;        // this villain can't start another wave before this turn (cooldown)
    optional<int> scheduled;     // a triggered wave LANDS on this turn (the player's prep window)
    vector<TriggerInfo> triggers;// what is currently provoking it -> the villages panel + its tooltip
    vector<Creature*> attackers; // the wave it last landed; the panel shows red "attacking" while any live
  };
  const map<Vec2, VillainWave>& getVillainWaves() const;
  const Campaign& getCampaign() const;

  void prepareSiteRetirement();
  void doneRetirement();
  void addCollective(Collective*);

  void addEvent(const GameEvent&);
  void addAnalytics(const string& name, const string& value);
  void achieve(AchievementId) const;
  void setWasTransfered();

  // RAR online async invasion. chooseSite() records a picked target (pendingInvasionTarget);
  // transferAction reads it and calls requestInvasion with the FULL team; MainLoop::playGame
  // services the request (downloads+loads the dungeon), addInvasionSite injects it, then the
  // whole team is transferred onto it.
  void setPendingInvasion(Vec2 pos, const string& gameId);
  optional<pair<Vec2, string>> takePendingInvasion(); // returns + clears
  void requestInvasion(Vec2 pos, const string& gameId, vector<Creature*> team);
  optional<pair<Vec2, string>> getInvasionRequest() const; // (targetPos, targetGameId)
  const vector<Creature*>& getInvasionTeam() const;
  void clearInvasionRequest();
  Model* addInvasionSite(Vec2 pos, PModel model, SavedGameInfo info);
  // 4c owner reconcile: the owner's dungeon was invaded (damaged) while they were offline. Replace the
  // freshly-loaded (pristine) base model with the server's damaged copy and un-retire its collective
  // back into the player's control (reverse of prepareSiteRetirement). true if adopted.
  bool adoptInvadedModel(PModel damaged, SavedGameInfo info);
  // After the team is moved in, record the live invasion so it can be ended (writeback) when the
  // player releases control (getPlayerCreatures() becomes empty).
  void recordActiveInvasion(Vec2 pos, const string& gameId, Model* model, SavedGameInfo info,
      vector<Creature*> team);
  bool hasActiveInvasion() const;
  // RAR villain aftermath writeback: find a conquered villain the invading team has LEFT (so its post-battle
  // state is stable), mark it captured so it isn't re-processed, and hand back its model for serialize+upload.
  // Returns none if the team is still inside or nothing is pending. The caller uploads the aftermath, then
  // calls destroyInvasionSite(pos) to fully drop the model. type/enemyId rebuild the retired-site header.
  // `keepLoaded`: this site still has something the player can come back for -- it was conquered, or a
  // sub-faction on it was wiped and its loot is pillageable -- so the model stays resident and PILLAGE can
  // reach into it from base. A site that was merely visited is released once its state is written back.
  struct VillainWriteback { Vec2 pos; shared_ptr<Model> model; string enemyId; VillainType type; bool keepLoaded; };
  optional<VillainWriteback> takeVillainWriteback();
  // RAR: the villain at this world tile changed AFTER its aftermath was already captured -- the player pillaged
  // loot off it from base. Clear its "already written back" mark so takeVillainWriteback re-captures and
  // re-uploads the now-emptier state. Without this the server keeps the ORIGINAL loot forever, so every visit
  // (and every session) re-offers the same items -- and re-pillaging an item whose unique id is still in base
  // storage from a previous pillage crashes IndexedVector::insert. No-op offline (the set is empty).
  void rearmVillainWriteback(Vec2 villainPos);
  // RAR: unwind every reference into a downloaded site and free it. removeDweller=false keeps the villain on
  // the world map (only the loaded interior goes); destroyInvasionSite passes true for a rival keeper.
  void releaseSiteModel(Vec2 pos, bool removeDweller);
  struct RarSiteLoot {
    TString name;        // the faction we beat there
    ViewIdList viewId;   // ...and its own icon, so the row's picture matches its title
  };
  void rarMarkSiteHasMyLoot(Vec2 pos, int colIndex, TString defeatedName, ViewIdList defeatedViewId) {
    rarSitesWithMyLoot[make_pair(pos, colIndex)] =
        RarSiteLoot{ std::move(defeatedName), std::move(defeatedViewId) };
  }
  void rarClearSiteLoot(Vec2 pos, int colIndex) { rarSitesWithMyLoot.erase(make_pair(pos, colIndex)); }
  void rarClearSiteLoot(Vec2 pos) {           // whole tile: used when the site is taken over or emptied
    for (auto it = rarSitesWithMyLoot.begin(); it != rarSitesWithMyLoot.end();)
      it = (it->first.first == pos) ? rarSitesWithMyLoot.erase(it) : std::next(it);
  }
  bool rarSiteHasMyLoot(Vec2 pos) const {
    for (auto& e : rarSitesWithMyLoot)
      if (e.first.first == pos)
        return true;
    return false;
  }
  const map<pair<Vec2, int>, RarSiteLoot>& rarGetSitesWithMyLoot() const { return rarSitesWithMyLoot; }
  // RAR: a site-lost notice waiting to be shown full-screen. Raised where the loss is DETECTED (while the
  // villain panel is being rebuilt) and shown from a place that is allowed to open a window -- putting a modal
  // up from inside panel construction re-enters the view while it is being filled in. Transient like the loot
  // map above: never serialized, so it cannot reach a save.
  void rarQueueTakeoverPopup(TString text) { rarPendingTakeover = std::move(text); }
  optional<TString> rarTakePendingTakeoverPopup() {
    auto ret = std::move(rarPendingTakeover);
    rarPendingTakeover = none;
    return ret;
  }
  // RAR: label an injected villain with its enemyId, so a later writeback can rebuild the retired-site header.
  void recordInjectedVillain(Vec2 pos, const string& enemyId);
  // RAR: true while the player is mid-invasion -- an active keeper invasion OR controlling a team AWAY from
  // their own base model (invading a downloaded villain). Save/autosave are blocked while this holds.
  bool isInvading() const;
  bool invasionTeamLeftDungeon() const; // true once no team member remains in the invaded dungeon (left/dead)
  optional<string> getActiveInvasionGameId() const; // target's gameId while an invasion is live (siege polling)
  struct EndedInvasion { shared_ptr<Model> model; Vec2 pos; string gameId; SavedGameInfo info; };
  // Return surviving invaders home + hand back the (still-intact) invaded model for upload. The
  // caller MUST serialize it, then call destroyInvasionSite(pos) to fully remove it.
  optional<EndedInvasion> endActiveInvasion();
  // Fully destroy the invaded dungeon: unregister its collectives, drop the model (with the caller's
  // shared_ptr also released this makes it refcount-0 -> weak_ptrs to its entities EXPIRE -> the save
  // is clean). Must be called AFTER the model has been serialized for the writeback.
  void destroyInvasionSite(Vec2 pos);
  // RAR: an offline invasion advanced this model's internal time inside the INVADER's game, so THIS
  // game's localTime table is now stale (behind the model). On load the update loop would stall -> minions
  // stand frozen until control is taken. Resync the table entry to the model's real time before saving.
  void resyncModelLocalTime(Model* m);
  // RAR Phase A: wire a freshly-loaded model (e.g. a villain downloaded on demand) into the running game
  // WITHOUT touching its campaign dweller (unlike addInvasionSite). Returns the live Model*.
  Model* injectSiteModel(Vec2 pos, PModel model);
  // RAR Phase A: callback that fetches+loads a villain's model for a world position (set by MainLoop when
  // online). chooseSite uses it to lazily materialise a villain the moment the player travels there.
  void setVillainLoader(function<PModel(Vec2)>);
  // RAR remote pillage. Same seam as villainLoader: PlayerControl can't reach MainLoop, and the work needs
  // MainLoop's download + serialize. Not serialized -- reinstalled each run when online.
  void setVillainPillager(function<bool(Vec2, int, long long, string&, bool&)>);
  bool rarPillageSite(Vec2 pos, int colIndex, long long baseVersion, string& outMessage,
      bool& outFactionEmptied);

  ~Game();

  SERIALIZATION_DECL(Game)

  Game(Table<PModel>&&, Vec2 basePos, const CampaignSetup&, ContentFactory);

  unordered_set<string> SERIAL(effectFlags);
  vector<string> SERIAL(zLevelGroups);

  void clearPlayerControl();

  private:
  function<PModel(Vec2)> villainLoader; // not serialized -- MainLoop re-installs it each run when online
  function<bool(Vec2, int, long long, string&, bool&)> villainPillager; // ditto, for remote pillage
  void tick(GlobalTime);
  bool updateModel(Model*, double timeDiff, optional<milliseconds> endTime);
  void uploadEvent(const string& name, const map<string, string>&);
  void considerAchievement(const GameEvent&);

  SunlightInfo sunlightInfo;
  Table<PModel> SERIAL(models);
  Table<bool> SERIAL(visited);
  map<LevelId, double> SERIAL(localTime);
  Vec2 SERIAL(baseModel);
  View* view = nullptr;
  double SERIAL(currentTime) = 0;
  optional<ExitInfo> exitInfo;
  Tribe::Map SERIAL(tribes);
  optional<int> SERIAL(lastTick);
  string SERIAL(gameIdentifier);
  TString SERIAL(gameDisplayName);
  map<VillainType, vector<Collective*>> SERIAL(villainsByType);
  vector<Collective*> SERIAL(collectives);
  MusicType SERIAL(musicType);
  OwnerPointer<CreatureView> spectator;
  HeapAllocated<Statistics> SERIAL(statistics);
  Options* options = nullptr;
  Highscores* highscores = nullptr;
  Encyclopedia* encyclopedia = nullptr;
  optional<milliseconds> lastUpdate;
  PlayerControl* SERIAL(playerControl) = nullptr;
  public:
  // RAR: forget this game's PlayerControl. It is a RAW pointer, but the object is OWNED by the collective as
  // its CollectiveControl -- so Collective::setControl() frees it and leaves this dangling. That matters more
  // than it looks: addEvent() has a CreatureMoved SHORTCUT that calls playerControl->onEvent() DIRECTLY,
  // bypassing the event-listener list, so unsubscribing the control does NOT stop it being called. A loaded
  // copy of someone else's game (an invasion battlefield) must have this cleared once its control is replaced.
  void rarClearPlayerControl() { playerControl = nullptr; }
  private:
  Collective* SERIAL(playerCollective) = nullptr;
  HeapAllocated<Campaign> SERIAL(campaign);
  bool wasTransfered = false;
  optional<pair<Vec2, string>> pendingInvasionTarget; // RAR transient: chooseSite -> transferAction handoff
  optional<pair<Vec2, string>> invasionRequest;       // RAR transient: serviced by playGame
  vector<Creature*> invasionTeam;                      // RAR transient: the full team to move
  struct ActiveInvasion { Vec2 pos; string gameId; Model* model; SavedGameInfo info; vector<Creature*> team;
      map<Creature*, TribeId> savedTribes; }; // RAR: team's real tribes while retagged to Invaders
  optional<ActiveInvasion> activeInvasion;             // RAR transient: live invasion
  map<Vec2, string> injectedVillainEnemyId;            // RAR transient: enemyId per downloaded villain (writeback header)
  // RAR pillage: tiles where THIS keeper defeated a faction and walked out leaving its loot behind, mapped to
  // THAT FACTION's name. The panel must say who was actually beaten ("Elves"), not who owns the tile
  // ("Unicorns") -- a site can hold several factions and killing one says nothing about the others.
  // TRANSIENT by design: it must not bloat the save, and it is only a hint for the panel -- the loot itself
  // lives on the server, so losing this on reload costs a button, never any loot.
  // Keyed by (tile, collective index) -- a site can hold several factions and you may have beaten more than
  // one. Keying by tile alone collapsed them into a single row named after whichever came first, and
  // pillaging it emptied all of them at once.
  map<pair<Vec2, int>, RarSiteLoot> rarSitesWithMyLoot;
  optional<TString> rarPendingTakeover;   // transient, NOT serialized -- see rarQueueTakeoverPopup
  set<Vec2> villainWrittenBack;                        // RAR transient: villain positions already aftermath-captured
  // RAR villain waves. A world-map villain's model is only downloaded when the player TRAVELS there, so its
  // VillageControl never runs at the base and the vanilla attack path is dead. considerVillainWaves() rebuilds
  // the behaviour from each villain's DEFINITION instead. All state here is TRANSIENT (no SERIAL) -- it must
  // never change the save layout. Cooldowns therefore reset on load, which is deliberate: a freshly loaded
  // game gets a full prep window rather than an instant wave.
  map<Vec2, VillainWave> villainWaves;
  double peakPlayerPower = 0;                          // highest danger level the player reached -> FinishOff
  vector<Creature*> SERIAL(players);
  FileSharing* fileSharing = nullptr;
  set<int> SERIAL(turnEvents);
  TimeInterval SERIAL(sunlightTimeOffset);
  friend class GameListener;
  void considerRealTimeRender();
  void considerRetiredLoadedEvent(Vec2 coord);
  optional<ExitInfo> updateInput();
  void increaseTime(double diff);
  void spawnKeeper(AvatarInfo, vector<TString> introText);
  HeapAllocated<ContentFactory> SERIAL(contentFactory);
  void updateSunlightMovement();
  string SERIAL(avatarId);
  map<string, string> analytics;
  Unlocks* unlocks = nullptr;
  SteamAchievements* steamAchievements = nullptr;
  bool rarLivePvp = false; // RAR live PvP: no turn-based pausing during a live battle (not serialized)
  bool rarLiveInvader = false;            // RAR live PvP: this game is the invading (non-authoritative) side
  bool rarAutoFight = false;              // RAR live PvP: controlled creatures fight on their own when idle
  bool rarLiveDefeat = false;             // RAR live PvP: my controlled creature was killed on the defender's side
  optional<long long> rarLiveDefeatVictim; // RAR live PvP: which one -- killed once control is released
  optional<string> rarOutgoingOrder;      // RAR live PvP: order to send to the authoritative defender
  optional<UserInput> rarPendingMove;     // RAR live PvP: latest movement order (priority, supersedes)
  std::vector<UserInput> rarPendingInput; // RAR live PvP: other queued orders (FIFO, capped)
  void considerAllianceAttack();
  bool SERIAL(allianceAttackPossible) = true;
  EnemyAggressionLevel SERIAL(enemyAggressionLevel);
  int SERIAL(numLesserVillainsDefeated) = 0;
};
