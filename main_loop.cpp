#include "avatar_info.h"
#include "file_path.h"
#include "stdafx.h"
#include "main_loop.h"
#include "sokoban_input.h" // RAR: genServerWorld builds a local SokobanInput (the gen path has none)
#include "view.h"
#include "highscores.h"
#include "music.h"
#include "options.h"
#include "progress_meter.h"
#include "file_sharing.h"
#include "square.h"
#include "model_builder.h"
#include "parse_game.h"
#include "name_generator.h"
#include "view.h"
#include "village_control.h"
#include "campaign.h"
#include "game.h"
#include "player_control.h"
#include "build_info.h"   // TEMP: developerOnly verification // RAR: leaveControl() on save&exit while controlling a team
#include "model.h"
#include "clock.h"
#include "view_id.h"
#include "saved_game_info.h"
#include "enemy_id.h" // RAR villain writeback: rebuild the retired-site header (EnemyId + VillainType)
#include "retired_games.h"
#include "save_file_info.h"
#include "creature.h"
#include "creature_attributes.h" // rar_import_base: clear timed lasting-effects on the imported creatures
#include "campaign_builder.h"
#include "campaign_type.h"
#include "serialization.h"
#include "villain_group.h"
#include "rar_hash.h"
#include "rar_client.h"
#include "rar_server.h"
#include "rar_mods.h"
#include "layout_generator.h" // previewLayout iterates factory.randomLayouts (map<..., LayoutGenerator>)
#include "layout_canvas.h"    // previewLayout catches LayoutGenerationError instead of exiting
#include "rar_compress.h"
#include <sstream>
#include <thread>
#include <chrono>
#include "game_save_type.h"
#include "exit_info.h"
#include "tutorial.h"
#include "model.h"
#include "item_type.h"
#include "pretty_printing.h"
#include "content_factory.h"
#include "enemy_factory.h"
#include "external_enemies.h"
#include "game_config.h"
#include "avatar_menu_option.h"
#include "creature_name.h"
#include "tileset.h"
#include "content_factory.h"
#include "scroll_position.h"
#include "miniunz.h"
#include "external_enemies_type.h"
#include "mod_info.h"
#include "container_range.h"
#include "extern/iomanip.h"
#include "enemy_info.h"
#include "level.h"
#include "position.h"
#include "body.h"
#include "unique_entity.h"
#include "dummy_view.h"
#include "workshops.h"
#include "workshop_type.h"
#include "workshop_item.h"
#include "rar_lockstep_net.h"
#include "zones.h"
#include "task.h"
#include "minion_activity.h"
#include "user_input.h"
#include "view_id.h"
#include "collective_builder.h"
#include "collective_config.h"
#include "creature_factory.h"
#include "monster.h"
#include "monster_ai.h"
#include "minion_trait.h"
#include "tribe.h"
#include "movement_type.h"
#include "tribe_alignment.h"
#include "simple_game.h"
#include "monster_ai.h"
#include "mem_usage_counter.h"
#include "gui_elem.h"
#include "encyclopedia.h"
#include "spectator.h"
#include "event_listener.h"
#include "game_event.h"
#include "fx_info.h"
#include "fx_name.h"
#include "spell_map.h"
#include "spell.h"
#include "player_message.h"
#include "collective_teams.h"
#include "creature_view.h"
#include "map_memory.h"
#include "game_info.h"
#include "movement_info.h"
#include "view_object.h"
#include "view_index.h"
#include "view_layer.h"
#include <unordered_map>
#include <map>
#include "game_info.h"
#include "tribe_alignment.h"
#include "unlocks.h"
#include "scripted_ui_data.h"
#include "version.h"
#include "rar_client.h"
#include "collective.h"
#include "translations.h"

#ifdef USE_STEAMWORKS
#include "steam_ugc.h"
#include "steam_client.h"
#endif

MainLoop::MainLoop(View* v, Highscores* h, FileSharing* fSharing, const DirectoryPath& paidDataPath,
    const DirectoryPath& freePath, const DirectoryPath& uPath, const DirectoryPath& modsDir, Options* o, Jukebox* j,
    SokobanInput* soko, TileSet* tileSet, Unlocks* unlocks, SteamAchievements* achievements, Translations* translations,
    int sv, string modVersion)
      : view(v), paidDataPath(paidDataPath), dataFreePath(freePath), userPath(uPath), modsDir(modsDir), options(o),
        jukebox(j), highscores(h), fileSharing(fSharing), sokobanInput(soko), tileSet(tileSet), saveVersion(sv),
        modVersion(modVersion), unlocks(unlocks), steamAchievements(achievements), translations(translations) {
  CHECK(!!unlocks);
}

vector<SaveFileInfo> MainLoop::getSaveFiles(const DirectoryPath& path, const string& suffix) {
  vector<SaveFileInfo> ret;
  for (auto file : path.getFiles()) {
    if (file.hasSuffix(suffix))
      ret.push_back({file.getFileName(), file.getModificationTime(), false});
  }
  sort(ret.begin(), ret.end(), [](const SaveFileInfo& a, const SaveFileInfo& b) {
        return a.date > b.date;
      });
  return ret;
}

static TString getDateString(time_t t) {
  char buf[100];
  strftime(buf, sizeof(buf), "%c", std::localtime(&t));
  return TString(string(buf));
}

bool MainLoop::isCompatible(int loadedVersion) {
  return loadedVersion > 2 && loadedVersion <= saveVersion && loadedVersion / 100 == saveVersion / 100;
}

static string getSaveSuffix(GameSaveType t) {
  switch (t) {
    case GameSaveType::KEEPER: return ".kep";
    case GameSaveType::RETIRED_SITE: return ".sit";
    case GameSaveType::RETIRED_CAMPAIGN: return ".cam";
    case GameSaveType::WARLORD: return ".war";
    case GameSaveType::AUTOSAVE: return ".aut";
  }
}

bool MainLoop::useSingleThread() {
  return options->getBoolValue(OptionId::SINGLE_THREAD);
}

template <typename T>
optional<T> MainLoop::loadFromFile(const FilePath& filename) {
  auto f = [&] {
    T obj;
    CompressedInput input(filename.getPath());
    string discard;
    SavedGameInfo discard2;
    int version;
    input.getArchive() >> version >> discard >> discard2;
    input.getArchive() >> obj;
    return std::move(obj);
  };
  if (useSingleThread())
    return f();
  else
    try { return f(); }
  catch (...) {
    return none;
  }
}

void MainLoop::saveGame(PGame& game, const FilePath& path) {
  FilePath tmpPath = path.withSuffix(".tmp");
  {
    CompressedOutput out(tmpPath.getPath());
    string name = toString(game->getGameDisplayName());
    SavedGameInfo savedInfo = game->getSavedGameInfo(tileSet->getSpriteMods());
    out.getArchive() << saveVersion << name << savedInfo;
    out.getArchive() << game;
  }
  tmpPath.copyTo(path);
  tmpPath.erase();
}

struct RetiredModelInfo {
  shared_ptr<Model> SERIAL(model);
  ContentFactory SERIAL(factory);
  SERIALIZE_ALL_NO_VERSION(model, factory)
};

struct RetiredModelInfoWithReference {
  shared_ptr<Model> SERIAL(model);
  ContentFactory* SERIAL(factory);
  SERIALIZE_ALL_NO_VERSION(model, serializeAsValue(factory))
};

optional<RetiredModelInfo> MainLoop::loadRetiredModelFromFile(const FilePath& path) {
  for (auto alignment : ENUM_ALL(TribeAlignment))
    TribeId::switchForSerialization(getPlayerTribeId(alignment), TribeId::getRetiredKeeper());
  auto _ = OnExit([]{TribeId::clearSwitch();});
  return loadFromFile<RetiredModelInfo>(path);
}

void MainLoop::saveMainModel(PGame& game, const FilePath& modelPath) {
  FilePath tmpPath = modelPath.withSuffix(".tmp");
  {
    CompressedOutput modelOut(tmpPath.getPath());
    string name = toString(game->getGameDisplayName());
    SavedGameInfo savedInfo = game->getSavedGameInfo(tileSet->getSpriteMods());
    modelOut.getArchive() << saveVersion << name << savedInfo;
    RetiredModelInfoWithReference info {
      game->getMainModel().giveMeSharedPointer(),
      game->getContentFactory()
    };
    modelOut.getArchive() << info;
  }
  tmpPath.copyTo(modelPath);
  tmpPath.erase();
}

// RAR invasion writeback: serialize an arbitrary model (the invaded dungeon) to RAW bytes (the
// same content saveMainModel writes, minus the gzip wrapper). The download side re-gzips before
// loadRetiredModelFromFile, so raw here avoids a redundant gzip+gunzip round-trip.
string MainLoop::serializeModelRaw(shared_ptr<Model> model, SavedGameInfo info, ContentFactory* factory) {
  std::stringstream ss;
  {
    OutputArchive ar(ss);
    string name = "invaded";
    ar << saveVersion << name << info;
    RetiredModelInfoWithReference rinfo { std::move(model), factory };
    ar << rinfo;
  }
  return ss.str();
}

// RAR online: upload the keeper's full stripped game state (the SAME bytes just written to the local
// stripped .kep by saveUI) as the server blob. Local + server hold the identical stripped save; saveUI
// already uploaded its hash, so blob and hash agree. The owner loads the local copy when not invaded,
// or this server copy (re-uploaded damaged by an invader) when invaded. The invader downloads this to
// invade and re-uploads it damaged when done.
void MainLoop::rarUploadKeeperDungeon(PGame& game, GameSaveType type) {
  auto gameId = game->getGameIdentifier();
  // The stripped local save: the .kep just written by saveUI, or -- recovering after a crash -- the .aut,
  // which is written in the same stripped form and so uploads verbatim.
  FilePath path = getSavePath(game, type);
  // The lzma compress + upload takes a few seconds -- show a splash so the game doesn't look frozen.
  doWithSplash(TString("Uploading your keep to the server..."_s), [&] {
    igzstream gzin(path.getPath());
    string raw((std::istreambuf_iterator<char>(gzin)), std::istreambuf_iterator<char>());
    gzin.close();
    string blob = raw.empty() ? string() : rarLzmaCompress(raw);
    if (!blob.empty()) {
      if (rarUploadDungeon(gameId, blob, rarSha256Hex(raw)))
        INFO << "RAR: uploaded keep state (raw " << raw.size() << " -> lzma " << blob.size() << ") for " << gameId;
      else
        INFO << "RAR: state upload failed: " << rarLastError();
    }
  });
}

int MainLoop::getSaveVersion(const SaveFileInfo& save) {
  return getSaveVersion(save, userPath);
}

// Online saves live in saves/<account>/, NOT the flat userPath -- resolving them against the root returned -1
// ("incompatible") and silently dropped the keeper from the load menu, so pass the directory they came from.
int MainLoop::getSaveVersion(const SaveFileInfo& save, const DirectoryPath& dir) {
  if (auto info = getNameAndVersion(dir.file(save.filename)))
    return info->second;
  else
    return -1;
}

void MainLoop::uploadFile(const FilePath& path, const string& title, const SavedGameInfo& info) {
  FileSharing::CancelFlag cancel;
  optional<string> error;
  optional<string> url;
  doWithSplash(TSentence("UPLOADING", TString(string(path.getPath()))), 1,
      [&] (ProgressMeter& meter) {
        error = fileSharing->uploadSite(cancel, path, title, getOldInfo(info), meter, url);
      },
      [&] {
        cancel.cancel();
      });
  if (url)
    if (view->yesOrNoPrompt(TStringId("RETIRED_DUNGEON_UPLOADED")))
      openUrl("https://steamcommunity.com/sharedfiles/filedetails/?id=" + *url);
  if (error && !cancel.flag)
    view->presentText(TString(TStringId("ERROR_UPLOADING_FILE")), TString(*error));
}

FilePath MainLoop::getSavePath(const PGame& game, GameSaveType gameType) {
  auto id = game->getGameIdentifier();
  auto sep = id.find('~');
  if (sep != string::npos) {
    // Online identity "<account>~<keeper>" -> saves/<account>/<keeper>.<suffix>. Create the folders on demand.
    auto dir = userPath.subdirectory("saves");
    dir.createIfDoesntExist();
    dir = dir.subdirectory(id.substr(0, sep));
    dir.createIfDoesntExist();
    return dir.file(id.substr(sep + 1) + getSaveSuffix(gameType));
  }
  CHECK(stripFilename(id) == id);
  return userPath.file(id + getSaveSuffix(gameType)); // offline fallback (game is online-only in practice)
}

void MainLoop::saveUI(PGame& game, GameSaveType type) {
  auto path = getSavePath(game, type);
  function<void()> uploadFun = nullptr;
  if (type == GameSaveType::RETIRED_SITE) {
    int saveTime = game->getMainModel()->getSaveProgressCount();
    doWithSplash(TStringId("RETIRING_SITE"), saveTime,
        [&] (ProgressMeter& meter) {
          Level::progressMeter = &meter;
          if (!game->getSavedGameInfo(tileSet->getSpriteMods()).retiredEnemyInfo)
            // only upload if it's not a retired enemy
            uploadFun = [this, path, name = view->translate(game->getGameDisplayName()),
                savedInfo = game->getSavedGameInfo(tileSet->getSpriteMods())] {
              uploadFile(path, name, savedInfo);
            };
          saveMainModel(game, path);
        });
  } else {
    int saveTime = game->getSaveProgressCount();
    doWithSplash(type == GameSaveType::AUTOSAVE ? TStringId("AUTOSAVING") : TStringId("SAVING_GAME"), saveTime,
        [&] (ProgressMeter& meter) {
        Level::progressMeter = &meter;
        MEASURE(saveGame(game, path), "saving time")});
  }
  Level::progressMeter = nullptr;
  if (uploadFun)
    uploadFun();
  // RAR online: tie this save to the logged-in account (so the load menu shows only
  // this account's keepers), and on a real "save & exit" (KEEPER, not autosave) push the
  // save's hash to the server for next-login validation.
  if (rarEnabled()) {
    rarRecordSaveOwnership(game->getGameIdentifier());
    if (type == GameSaveType::KEEPER)
      rarClaimSave(game->getGameIdentifier()); // site claim becomes permanent (held offline)
    // Upload the save's hash on BOTH save & exit (.kep) and autosave (.aut), but into SEPARATE server fields.
    // They mean different things: a .kep's state is ALSO uploaded as the dungeon blob right below, a .aut's is
    // not (that would push megabytes every few minutes). Sharing one field made a legitimate post-crash
    // autosave indistinguishable from a tampered save on load.
    if (type == GameSaveType::KEEPER || type == GameSaveType::AUTOSAVE) {
      const bool isAuto = (type == GameSaveType::AUTOSAVE);
      auto hash = rarHashFile(path.getPath());
      if (isAuto ? rarUploadAutosaveHash(game->getGameIdentifier(), hash)
                 : rarUploadSaveHash(game->getGameIdentifier(), hash))
        INFO << "RAR: uploaded " << (isAuto ? ".aut" : ".kep")
             << " hash for " << game->getGameIdentifier();
      else
        INFO << "RAR: save-hash upload failed: " << rarLastError();
      // Report actual play-turns so the server can lift newbie invasion protection once the keeper is
      // genuinely established (offline time never advances getGlobalTime, so this counts real play only).
      rarReportPlayedTurns(game->getGameIdentifier(), game->getGlobalTime().getVisibleInt());
    }
  }
}

void MainLoop::eraseSaveFile(const PGame& game, GameSaveType type) {
  getSavePath(game, type).erase();
}

enum class MainLoop::ExitCondition {
  ALLIES_WON,
  ENEMIES_WON,
  TIMEOUT,
  UNKNOWN
};

void MainLoop::bugReportSave(PGame& game, FilePath path) {
  int saveTime = game->getSaveProgressCount();
  doWithSplash(TStringId("SAVING_GAME"), saveTime,
      [&] (ProgressMeter& meter) {
      Level::progressMeter = &meter;
      MEASURE(saveGame(game, path), "saving time")});
  Level::progressMeter = nullptr;
}

template <class T>
static void dumpMemUsage(const T& elem) {
#ifdef MEM_USAGE_TEST
  dumpGuiLineNumbers(std::cout);
  MemUsageArchive ar;
  ar << elem;
  ar.dumpUsage(std::cout);
#endif
}

// RAR authoritative live PvP: host->invader battlefield stream + invader->host command helpers (defined later).
static string rarSerializeBattlefield(Model* m);
static void rarApplyInvaderCommand(Game* host, const vector<long long>& teamIds, const string& cmd);
static void rarApplyBattlefield(Game* puppet, const string& data, int animTurn);
// RAR live PvP sync: each side streams only the creatures it OWNS (invader = her team, defender = everything
// else) as deltas + death markers, and applies what the other side sends. Definitions further down.
struct RarLiveStream {
  std::unordered_map<long long, std::pair<long long, Vec2>> sent; // id -> last position we transmitted
  std::unordered_map<long long, int> sentHp;                      // id -> last health we transmitted (permille)
  // Creatures the peer created for us (summons, wave enemies, ...). They get a fresh local id, so remember which
  // local creature stands for which remote id, or later position/death updates wouldn't find them.
  std::unordered_map<long long, long long> remoteToLocal;
  // Where the peer says each of its creatures should be. A move can't always be applied the moment it arrives
  // (the tile may be briefly occupied) and deltas are never re-sent, so an un-applied move would leave that unit
  // permanently a few tiles off. We keep the target and retry every tick until it matches.
  std::unordered_map<long long, std::pair<long long, Vec2>> desired;
  // Every id the AUTHORITATIVE side has declared dead. A death is a FACT, not a one-off event: if we could not
  // apply it the moment it arrived (the creature was under the player's control, its position was momentarily
  // invalid, or it had not been resolved yet) the old code simply dropped it -- and the creature then walked home
  // alive while its corpse lay in the enemy's dungeon. Keeping the ids lets us re-apply until it sticks.
  std::set<long long> deadIds;
  optional<bool> sentPause; // last pause state we transmitted
};
static string rarStreamOwned(const std::unordered_map<long long, Creature*>& owned, Model*,
    const std::set<long long>& teamIds, bool sendTeam, RarLiveStream&, optional<bool> paused, bool all);

// RAR live PvP: spell / projectile VISUALS. These are events, not state -- they happen once and are gone, so the
// position stream can't carry them. The authoritative side records them as they fire and the peer replays them,
// so a fireball or an arrow is seen on both screens.
class RarFxCollector : public OwnedObject<RarFxCollector>, public EventListener<RarFxCollector> {
  public:
  vector<string> lines;
  void onEvent(const GameEvent& event) {
    using namespace EventInfo;
    event.visit<void>(
        [&](const FX& info) {
          if (auto* lev = info.position.getLevel()) {
            auto dir = info.direction.value_or(Vec2(0, 0));
            lines.push_back("F " + toString((long long) lev->getUniqueId()) + " "
                + toString(info.position.getCoord().x) + " " + toString(info.position.getCoord().y) + " "
                + toString(int(info.fx.name)) + " " + toString(dir.x) + " " + toString(dir.y));
          }
        },
        [&](const SpellCast& info) {
          if (info.caster)
            lines.push_back("SC " + toString(info.caster->getUniqueId().getGenericId()) + " "
                + string(info.spell.data()));
        },
        [&](const Projectile& info) {
          if (auto* lev = info.begin.getLevel())
            lines.push_back("J " + toString((long long) lev->getUniqueId()) + " "
                + toString(info.begin.getCoord().x) + " " + toString(info.begin.getCoord().y) + " "
                + toString(info.end.getCoord().x) + " " + toString(info.end.getCoord().y) + " "
                + (info.fx ? toString(int(info.fx->name)) : string("-1")) + " "
                + (info.viewId ? string(info.viewId->data()) : string("-")));
        },
        [&](const auto&) {}
    );
  }
};
static void rarApplyStream(Model*, const string& data, const std::set<long long>& teamIds, bool applyTeam,
    RarLiveStream&, bool all);

// RAR authoritative live PvP: the invader's observer view. Same rendering as Spectator, but animation is driven
// by a REAL-TIME virtual-turn clock (the puppet game never steps, so its local time -- Spectator's animation
// clock -- is frozen). Each host update = one virtual turn; getAnimationTime advances smoothly across it, so the
// MovementInfo slides that rarApplyBattlefield adds render as smooth walks instead of teleports.
class RarObserverView : public CreatureView {
  public:
  Level* obsLevel;
  View* obsView;
  RarObserverView(Level* l, View* v) : obsLevel(l), obsView(v) {}
  int virtualTurn = 0;
  double interval = 0.1; // seconds between host snapshots
  std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
  void onHostUpdate() { ++virtualTurn; lastUpdate = std::chrono::steady_clock::now(); }
  void setLevel(Level* l) { if (l) obsLevel = l; }  // z-level scroll: view another floor of the battlefield
  virtual const MapMemory& getMemory() const override { return MapMemory::empty(); }
  virtual void getViewIndex(Vec2 pos, ViewIndex& index) const override {
    Position position(pos, obsLevel);
    position.getViewIndex(index, nullptr);
    if (const Creature* c = position.getCreature())
      index.insert(c->getViewObject());
  }
  virtual void refreshGameInfo(GameInfo& gameInfo) const override { gameInfo.infoType = GameInfo::InfoType::SPECTATOR; }
  virtual Vec2 getScrollCoord() const override { return obsLevel->getBounds().middle(); }
  virtual Level* getCreatureViewLevel() const override { return obsLevel; }
  virtual double getAnimationTime() const override {
    double frac = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastUpdate).count() / interval;
    return (double) virtualTurn + std::min(1.0, std::max(0.0, frac));
  }
  virtual vector<Vec2> getVisibleEnemies() const override { return {}; }
  virtual CenterType getCenterType() const override { return CenterType::NONE; }
  virtual const vector<Vec2>& getUnknownLocations(const Level*) const override {
    static vector<Vec2> empty; return empty;
  }
};

MainLoop::ExitCondition MainLoop::playGame(PGame game, bool withMusic, bool noAutoSave,
    function<optional<ExitCondition>(Game*)> exitCondition, milliseconds stepTimeMilli, optional<int> maxTurns) {
  // SIEGE (invader side) polling state -- see the check inside the loop below.
  auto rarNextSiegeCheck = std::chrono::steady_clock::now();
  bool rarSiegeWarned = false;
  // RAR live PvP (defender side): a background thread polls for a live invite against my base so the game loop
  // never blocks on the network -- see the (cheap) check inside the loop below. Stopped on exit.
  if (rarEnabled())
    rarStartPvpWatch(game->getGameIdentifier());
  registerModPlaytime(true);
  OnExit on_exit([&]() {
    registerModPlaytime(false);
    rarStopPvpWatch();
  });
  // --reload_data (dev): re-read the data files and REPLACE the loaded game's ContentFactory, so edits to the
  // config (build menu, furniture costs, tiles, ...) show up on load without a full restart. Full replace (not
  // merge -- merge only ADDS new keys, never updates existing values), so removing content a live object still
  // references can break; it's opt-in/experimental. Also rebuilds the keeper's build menu, which is baked into
  // PlayerControl at creation, and reloads the tileset below from the fresh tilePaths.
  if (reloadDataOnLoad || options->getBoolValue(OptionId::RELOAD_DATA)) {
    *game->getContentFactory() = createContentFactory(false);
    // Replacing the ContentFactory only refreshes DEFINITIONS. Anything Game::spawnKeeper COPIED out of the
    // keeper definition at creation is game state living in the save, and a content reload does not reach it --
    // so re-apply those here too, or an edited keeper_creatures.txt silently has no effect on an existing game.
    {
      auto factory = game->getContentFactory();
      for (auto& p : factory->keeperCreatures)
        if (p.first == game->getAvatarId()) {
          if (auto pc = game->getPlayerControl()) {
            pc->reloadBuildMenu(factory, p.second);
            // Workshop recipe lists live in the save too, so an edited workshops_menu.txt needs this to
            // reach a running game. Queues are preserved.
            pc->reloadWorkshops(factory, p.second);
          }
          // zLevelGroups decides which z-level set is drawn from when the player digs down. It is baked into
          // Game at spawnKeeper, so without this a modded/edited group list only applies to a NEW game -- you
          // would keep digging into the old groups' levels.
          game->zLevelGroups = p.second.zLevelGroups;
          for (auto& f : p.second.flags)
            game->effectFlags.insert(f);
          break;
        }
    }
    std::cout << "[reload_data] reloaded data files into the game on load\n"; std::cout.flush();
  }
  if (tileSet) {
    if (rarEnabled() && modsChangedThisSync) {
      // Online AND the mod set actually changed this sync -> refresh graphics from the CURRENT (server) mods,
      // so a keeper continued from an older save picks up a newly added graphics mod. If the hashes already
      // matched, modsChangedThisSync is false and we SKIP this reload -- the startup tileset already reflects
      // the server's mods, so there's nothing to redo.
      tileSet->setTilePathsAndReload(getTilePathsForAllMods());
      modsChangedThisSync = false; // consumed
    } else if (!rarEnabled())
      tileSet->setTilePathsAndReload(game->getContentFactory()->tilePaths); // offline: the save's own paths
  }
  view->reset();
  if (!noAutoSave)
    view->setBugReportSaveCallback([&] (FilePath path) { bugReportSave(game, path); });
  DestructorFunction removeCallback([&] { view->setBugReportSaveCallback(nullptr); });
  Encyclopedia encyclopedia(game->getContentFactory());
  // RAR Phase A: online, villain models aren't generated at start -- install the on-demand loader that
  // Game::chooseSite calls to download + materialise a villain the first time the player travels there.
  if (rarEnabled())
    game->setVillainLoader([this] (Vec2 pos) { return rarLoadVillainModel(pos); });
  game->initialize(options, highscores, view, fileSharing, &encyclopedia, unlocks, steamAchievements);
  doWithSplash(TStringId("INITIALIZING_GAME"), game->getAllModels().size(),
      [&] (ProgressMeter& meter) {
        game->initializeModels(meter);
      });
  Intervalometer meter(stepTimeMilli);
  Intervalometer pausingMeter(stepTimeMilli);
  auto lastMusicUpdate = GlobalTime(-1000);
  auto lastAutoSave = game->getGlobalTime();
  optional<GlobalTime> exitTime;
  if (maxTurns)
    exitTime = game->getGlobalTime() + TimeInterval(*maxTurns);
  // RAR invasion: the target's WHOLE game (loaded from its blob) is kept here for the fight. Its model is
  // SHARED into `game` for combat (no factory merge -- ContentId is globally interned), so all changes
  // (terrain digging, deaths, loot) land in this game too; at writeback we re-serialize THIS game.
  PGame rarInvasionTarget;
  Vec2 rarInvasionOrigPos;
  // AUTHORITATIVE live PvP (host side): the loaded invader game whose team is injected into THIS running game.
  // Held for the battle's duration so the shared model stays alive. rarLiveHosting guards against re-triggering.
  PGame rarLiveInvaderGame;
  bool rarLiveHosting = false;
  Vec2 rarLiveSitePos;                // campaign tile the invader's model was hung on (for teardown)
  string rarLiveSessionId;            // brokered PvP session -- must be dropped when the battle ends
  bool rarLiveWasLive = false;        // THIS invasion is a live PvP battle. Sticky: rarIsLivePvp() is cleared as
                                      // soon as the battle ends (defeat/disconnect/teardown), so it cannot be
                                      // used later to decide that the offline writeback must be skipped.
  std::unordered_map<long long, Creature*> rarLiveOwned; // my own battle creatures, by id: lets the stream
                                                         // tell RETREATED (alive) from DEAD without searching models
  bool rarLiveLinkLogged = false;     // report the relay link state once per battle
  std::chrono::steady_clock::time_point rarLiveStart; // when this battle began (grace before teardown)
  optional<TeamId> rarLiveCreatedTeam; // the team made for this invasion -- cancelled on teardown, else the
                                       // Teams panel keeps one leftover entry per invasion
  // Host->invader live stream: the relay connection (role 0), connected on a background thread so the host's loop
  // never blocks; once connected the loop streams the battlefield every ~100ms. Joined/closed on exit.
  LockstepNet rarHostNet;
  std::thread rarHostConnectThread;
  std::atomic<bool> rarHostConnected{false};
  int rarHostSeq = 0;
  int rarHostCmdSeq = 0;              // remote stream sequence
  vector<long long> rarLiveTeamIds;  // the invading team (leader first)
  // Live sync state (same code drives BOTH sides; only ownership differs).
  Model* rarLiveBattlefield = nullptr;      // the shared model being fought over
  std::set<long long> rarLiveTeamIdSet;     // the invader's creatures = what the INVADER owns
  bool rarLiveIsInvader = false;            // am I the invader (own the team) or the defender (own the rest)?
  bool rarLiveEndSignalled = false;         // the defender sent END OF BATTLE ("E") -- see the handler below
  RarLiveStream rarLiveStream;
  // MUST be owner-allocated: EventListener registers itself through OwnedObject::getThis(), which is only
  // valid for an object held by an OwnerPointer. As a plain stack object the weak pointer is empty and the first
  // event fired aborts with "weakPointer is false".
  auto rarFx = makeOwner<RarFxCollector>();  // records spell/projectile visuals to send to the peer
  auto rarNextStream = std::chrono::steady_clock::now();
  OnExit rarHostNetCleanup([&]{
    rarHostNet.close();
    if (rarHostConnectThread.joinable()) rarHostConnectThread.join();
  });
  size_t rarLastDeadSeen = 0;        // how many authoritative deaths we have already reconciled
  // Re-apply any authoritative death that could not be applied when it arrived. Only the INVADER does this: the
  // defender's simulation IS the authority, he is never told what died on his own map.
  auto rarReconcileDeaths = [&]() {
    if (!rarLiveIsInvader || rarLiveStream.deadIds.empty())
      return;
    for (Model* mm : game->getAllModels())
      for (Creature* c : mm->getAllCreatures()) {
        if (c->isDead() || !rarLiveStream.deadIds.count(c->getUniqueId().getGenericId()))
          continue;
        if (game->getPlayerCreatures().contains(c))
          continue;   // still in the player's hands -- retried once control is released
        if (c->getPosition().isValid() && c->getPosition().getCreature() == c)
          c->dieNoReason();
      }
  };
  // OFFLINE-INVASION DIAGNOSTIC: the defenders are alive when we land but dead soon after, so snapshot the
  // corpse count on arrival and, a few dozen turns later, report every NEW death together with who killed it.
  double rarInvDiagAt = -1;          // model local time at which to report
  bool rarInvDiagShown = false;
  size_t rarInvDeadAtArrival = 0;
  set<string> rarReportedVillains; // Phase B: villain positions ("x_y") already reported as defeated
  while (1) {
    if (exitTime && game->getGlobalTime() >= *exitTime)
      throw GameExitException();
    double step = 1;
    if (!game->isTurnBased()) {
      double gameTimeStep = view->getGameSpeed() / stepTimeMilli.count();
      auto timeMilli = view->getTimeMilli();
      double count = meter.getCount(timeMilli);
      //INFO << "Intervalometer " << timeMilli << " " << count;
      step = min(1.0, double(count) * gameTimeStep);
      // RAR live PvP: at NORMAL speed the sim advances only ~2.8 turns/sec (one turn ~360ms), and a controlled
      // creature can only act ON its turn -- so an order sat waiting for the next one (measured ~1.5s from click
      // to execution). When an order is queued, advance a full turn NOW so the creature acts immediately; the
      // world still runs at its normal pace when the player isn't commanding anything.
      if (game->rarIsLivePvp() && view->rarHasPendingInput())
        step = 1;
      if (maxTurns)
        step = 1;
      if (view->isClockStopped()) {
        // Advance the clock a little more until the local time reaches 0.99,
        // so creature animations are paused at their actual positions.
        double localTime = game->getMainModel()->getLocalTimeDouble();
        if (localTime - trunc(localTime) < pauseAnimationRemainder) {
          step = min(1.0, double(pausingMeter.getCount(view->getTimeMilliAbsolute())) * gameTimeStep);
          step = min(step, pauseAnimationRemainder - (localTime - trunc(localTime)));
        }
      } else
        pausingMeter.clear();
    }
    // RAR live PvP: the clock never stops, so a press can land while the sim is running other creatures' turns.
    // Park it in the game's order buffer; the next controlled creature to act executes it (press -> queue -> next
    // turn). Without this the press sits unread and looks ignored.
    // NOTE: no input draining here. The view's InputQueue already buffers presses across turns, so the controlled
    // creature reads them on its next turn by itself. Draining into a second buffer only re-ordered orders --
    // incidental inputs (e.g. CREATURE_MAP_CLICK from a map click) ended up consuming turns AHEAD of the move,
    // which made a click take several turns to take effect. Presses were never actually lost here; that was
    // flushEvents/lockKeyboard in WindowView, disabled for live PvP via rarSetLivePvpInput.
    INFO << "Time step " << step;
    // The simulation gets a fixed CPU budget per frame. 20ms is plenty for a normal keeper game, but during a live
    // battle the invader's current model is the DEFENDER'S WHOLE BASE -- if a turn needs more CPU than the budget,
    // turns stretch out in real time (measured ~1.5s/turn), and a controlled creature can only act on its turn, so
    // every order felt delayed by seconds. Give the sim a bigger slice while a live battle is running; rendering is
    // already capped at ~30fps so the extra time comes out of idle frame time.
    const auto simBudget = milliseconds{game->rarIsLivePvp() ? 100 : 20};
    if (auto exitInfo = game->update(step, Clock::getRealMillis() + simBudget)) {
      exitInfo->visit(
          [&](ExitAndQuit) {
            if (rarEnabled()) // abandon: free this keeper's site for other players (permanent claim included)
              rarDeleteClaim(game->getGameIdentifier());
            eraseAllSavesExcept(game, none);
            dumpMemUsage(game);
          },
          [&](GameSaveType type) {
            // RAR: save & exit is blocked (in Game::exitAction) while an invasion is live, so by here
            // there's no invaded dungeon. If still controlling a (non-invasion) team, release it so
            // the save is a clean keeper-mode save at our own base.
            if (rarEnabled() && !game->getPlayerCreatures().empty() && game->getPlayerControl())
              game->getPlayerControl()->leaveControl();
            if (type == GameSaveType::RETIRED_SITE) {
              game->prepareSiteRetirement();
              saveUI(game, type);
              game->doneRetirement();
            } else {
              // online: strip the recomputable pathfinding sectors so BOTH the local save and the server
              // blob are the same small "stripped" form (game discarded on save&exit; sectors rebuild on
              // load). This is the local stripped copy the owner loads when NOT invaded.
              if (rarEnabled() && (type == GameSaveType::KEEPER || type == GameSaveType::AUTOSAVE))
                for (Model* m : game->getAllModels())
                  for (Level* l : m->getLevels())
                    l->clearSectors();
              saveUI(game, type);
            }
            eraseAllSavesExcept(game, type);
            // RAR online: upload the same stripped local save as the server blob (+ its hash, in saveUI).
            // Control was released above so getPlayerCreatures() is empty.
            if (rarEnabled() && type == GameSaveType::KEEPER && game->getPlayerCreatures().empty())
              rarUploadKeeperDungeon(game, GameSaveType::KEEPER);
          }
      );
      return ExitCondition::UNKNOWN;
    }
    // RAR online: service a queued invasion -- download+load the rival's dungeon on demand, inject
    // it into the running game, and move the controlled team onto it (control follows the team).
    if (rarEnabled())
      if (auto req = game->getInvasionRequest()) {
        Vec2 targetPos = req->first;
        string targetId = req->second;
        vector<Creature*> team = game->getInvasionTeam(); // capture BEFORE clearing (clear wipes it)
        game->clearInvasionRequest();
        bool rarHandledLive = false; // LIVE invasion handled below -> skip the offline (retired-blob) path
        // RAR live PvP: if the target's owner is ONLINE, do a real-time lockstep battle instead of the async
        // (retired-blob) invasion. Broker via the server: announce -> exchange packed blobs -> run the battle.
        {
          string sessionId; int seed = 0;
          // Announce the KEEPER's name, not the account login. The online id is "<account>~<keeper>", so take
          // the part after the '~' (falls back to the account if the id has no separator).
          string myKeeperName = game->getGameIdentifier();
          if (auto sep = myKeeperName.find('~'); sep != string::npos)
            myKeeperName = myKeeperName.substr(sep + 1);
          if (myKeeperName.empty())
            myKeeperName = rarSessionLogin();
          auto inviteResult = rarPvpInvite(targetId, myKeeperName, sessionId, seed);
          if (inviteResult == RarPvpInvite::Away) {
            view->presentText(none, TString("That keeper is away raiding another base.\n"
                "You cannot invade an unattended dungeon."_s));
          } else if (inviteResult == RarPvpInvite::Live) {
            // AUTHORITATIVE invader (A3a): send my forces (team ids on line 1 so the host transfers the WHOLE
            // team), then WAIT for the host to send back the battlefield (role 0) and SPECTATE it. Live updates
            // (A2) + control (A4) come next; for now the invader sees the host's base with the team on it.
            string teamHdr;
            set<long long> teamIdSet;
            for (Creature* c : team) {
              teamHdr += (teamHdr.empty() ? "" : ",") + toString(c->getUniqueId().getGenericId());
              teamIdSet.insert(c->getUniqueId().getGenericId());
            }
            string combinedBlob; bool got = false;
            doWithSplash(TString("Invading the enemy keeper's base..."_s), [&] {
              string payload = teamHdr + "\n" + rarPackGameBlob(game);
              if (!rarPvpUploadState(sessionId, 1, payload)) return;
              auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
              while (std::chrono::steady_clock::now() < deadline) {
                if (rarPvpFetchState(sessionId, 0, combinedBlob) && !combinedBlob.empty()) { got = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
              }
            });
            if (got) {
              // Load the host's live base and inject it into MY game exactly like the offline invasion does, then
              // transfer my team onto it. playGame CONTINUES -> I control my team with the REAL vanilla team-control
              // UI (minimap, z-arrows, panel, spells). Position sync between the two games is layered on next.
              string raw = rarLzmaDecompress(combinedBlob);
              FilePath st = userPath.file("rar_invade" + getSaveSuffix(GameSaveType::KEEPER));
              { ogzstream o(st.getPath()); o.write(raw.data(), raw.size()); }
              // Guarded: a truncated/stale snapshot must fail the invasion, not terminate the game.
              optional<SavedGameInfo> savedInfo;
              optional<PGame> loaded;
              try {
                savedInfo = loadSavedGameInfo(st);
                loaded = loadFromFile<PGame>(st);
              } catch (...) {
                savedInfo = none;
                loaded = none;
              }
              st.erase();
              if (loaded && *loaded && savedInfo) {
                rarInvasionTarget = std::move(*loaded);
                // The defender snapshots his game mid-play, so it can contain creatures he was CONTROLLING.
                // Loaded here they'd be "players" in MY game -- my camera and control mode jumped to his team.
                // Strip the player controllers directly: this copy was never initialize()d, so it has NO view,
                // and PlayerControl::leaveControl() would scroll the view / transfer creatures -> instant crash.
                for (Creature* c : rarInvasionTarget->getMainModel()->getAllCreatures())
                  if (auto* ctrl = c->getController())
                    if (ctrl->isPlayer())
                      c->setController(makeOwner<Monster>(c, MonsterAIFactory::idle()));
                rarInvasionOrigPos = rarInvasionTarget->getMainModel()->position;
                Model* invaded = game->addInvasionSite(targetPos,
                    PModel(rarInvasionTarget->getMainModel().giveMeSharedPointer()), *savedInfo);
                vector<Creature*> moved;
                optional<Position> anchor; // land the squad TOGETHER: vanilla spreads arrivals over every transfer
                for (Creature* c : team)   // landing square, which split the team across two spots on arrival
                  if (game->canTransferCreature(c, invaded)) {
                    if (anchor) {
                      vector<Position> near;
                      for (Position p : anchor->neighbors8())
                        near.push_back(p);
                      for (Position p : anchor->neighbors8())
                        for (Position q : p.neighbors8())
                          near.push_back(q);
                      game->transferCreature(c, invaded, near);
                    } else
                      game->transferCreature(c, invaded);
                    // Arriving for a fight: nobody storms a base asleep. Wake anyone who was resting when the
                    // team was picked, otherwise sleeping minions walk around the battlefield.
                    c->removeEffect(LastingEffect::SLEEP, false);
                    for (Creature* comp : c->getCompanions())
                      comp->removeEffect(LastingEffect::SLEEP, false);
                    moved.push_back(c);
                    if (!anchor)
                      anchor = c->getPosition();
                  }
                game->setWasTransfered();
                game->recordActiveInvasion(targetPos, targetId, invaded, *savedInfo, moved);
                rarPvpSetAway(game->getGameIdentifier(), true); // I'm out: my own base can't be invaded now
                rarLiveWasLive = true;   // finish this invasion the LIVE way, whatever state the flags are in
                rarHandledLive = true; // continue playGame -> real invasion, real team control
                // Live sync: I own my team; the defender owns his base. Connect as role 1 in the background so
                // the game never blocks, then the loop below streams my team and applies his units.
                game->rarSetLivePvp(true);
                rarLiveBattlefield = invaded;
                rarLiveStart = std::chrono::steady_clock::now();
                rarLiveLinkLogged = false;
                rarLiveEndSignalled = false;   // stale from a previous battle would end this one instantly
                // The stream state is per-BATTLE. It was declared once and reused, so every battle started holding the
                // previous one's ids: the last team's entries were still in `sent`, so the first tick reported them
                // all as vanished (a burst of bogus deaths), and stale ids stayed on the dead list.
                rarLiveStream = RarLiveStream();
                rarLastDeadSeen = 0;
                // Frame numbering is PER CONNECTION: tryGetRemote() matches an exact tick and connect() wipes
                // the receive buffer, so both counters must restart with the link. Carried over from a previous
                // battle each side sits waiting for a frame number the other will never send again -- no positions
                // arrive and no orders are read, which looks exactly like "clicking does nothing".
                rarHostSeq = 0;
                rarHostCmdSeq = 0;
                rarLiveIsInvader = true;
                game->rarSetLiveInvader(true);   // I am the non-authoritative side: my clicks become orders
                rarLiveSessionId = sessionId;
                rarFx->subscribeTo(invaded);   // record my spell/projectile visuals for the peer
                for (Creature* c : moved) {
                  rarLiveTeamIdSet.insert(c->getUniqueId().getGenericId());
                  rarLiveOwned[c->getUniqueId().getGenericId()] = c;
                  for (Creature* comp : c->getCompanions()) {  // spirits/summons come along with their owner
                    rarLiveTeamIdSet.insert(comp->getUniqueId().getGenericId()); // are mine too
                    rarLiveOwned[comp->getUniqueId().getGenericId()] = comp;
                    // ...and are PUPPETS here: the defender simulates them (there they follow their owner). Left
                    // with their own AI they wandered every turn on this screen only, jittering against the
                    // authoritative positions and blocking the squad's own path.
                    comp->setController(makeOwner<Monster>(comp, MonsterAIFactory::idle()));
                  }
                }
                // EXCLUSIVE OWNERSHIP: the defender simulates his own base, so here his creatures are PUPPETS
                // driven by his stream. Without this they also ran their local AI -- wandering differently on each
                // screen ("shuffling" the defender never saw) and killing things independently, which is what made
                // the two sides drift apart. Each creature is now simulated by exactly one machine.
                for (Creature* c : invaded->getAllCreatures())
                  if (!rarLiveTeamIdSet.count(c->getUniqueId().getGenericId()))
                    c->setController(makeOwner<Monster>(c, MonsterAIFactory::idle()));
                // ...and the base's own LOGIC must not run here either. Creature AI isn't the only thing that
                // acts: the collectives and the attack-wave spawner would keep working in this copy, inventing
                // creatures and events the defender never had (a wave started on the invader's side only). The
                // defender simulates all of that and streams the results.
                invaded->rarDisableSpawning();
                // The loaded copy of the defender's game brings its OWN PlayerControl, subscribed to this model's
                // events. Every creature my stream moves fires CreatureMoved, which that control handles by
                // updating minion visibility -- on a game that was never initialised here. Unsubscribe it FIRST:
                // replacing the control below destroys the object, and a still-subscribed listener is then called
                // on freed memory (access violation inside VisibilityMap/PositionMap).
                if (auto* tpc = rarInvasionTarget->getPlayerControl())
                  tpc->unsubscribe();
                for (Collective* col : invaded->getCollectives())
                  col->setControl(CollectiveControl::idle(col));
                // That loop DESTROYED the loaded copy's PlayerControl (the collective owned it as its control),
                // but rarInvasionTarget still holds a raw pointer to it -- and addEvent()'s CreatureMoved
                // shortcut calls that pointer directly, without consulting the listener list. Every creature the
                // stream moves fires CreatureMoved, so the freed control was being driven all battle long: the
                // access violations inside its VisibilityMap/PositionMap are exactly that.
                rarInvasionTarget->rarClearPlayerControl();
                // Assigning over a still-joinable std::thread calls std::terminate ("Terminated due to unknown
                // reason") -- close the socket so any previous connect returns, then join before reusing it.
                rarHostNet.close();
                if (rarHostConnectThread.joinable())
                  rarHostConnectThread.join();
                rarHostConnected.store(false);
                rarHostConnectThread = std::thread([&rarHostNet, &rarHostConnected, sid = sessionId]() {
                  rarHostConnected.store(rarHostNet.connect(rarServerHost(), RAR_LOCKSTEP_DEFAULT_PORT, sid, 1, 120000));
                });
                game->rarSetLivePvp(true); // live PvP: never turn-based, even while controlling the team
                // Take control of the team so the VIEW follows them onto the enemy base (the offline invasion
                // works because you travel already-controlling them; here we may not be, so control now).
                // Summoned COMPANIONS (e.g. the shamans' SPIRITs) travel along but keep their own tribe, which
                // made them hostile to the squad. Put them on their summoner's tribe so they fight WITH the team.
                for (Creature* c : moved)
                  for (Creature* comp : c->getCompanions())
                    if (comp && comp->getTribeId() != c->getTribeId())
                      comp->setTribe(c->getTribeId());
                if (!moved.empty() && game->getPlayerCreatures().empty())
                  rarLiveCreatedTeam = game->getPlayerControl()->rarControlTeam(moved); // the WHOLE squad
              } else
                view->presentText(none, TString("Could not load the battlefield."_s));
            } else
              view->presentText(none, TString("Could not reach the enemy keeper."_s));
          }
        }
        if (!rarHandledLive) {
        // 4b reservation: lock the target so only one invader hits an offline dungeon at a time.
        auto reserve = rarReserveDungeon(targetId);
        if (reserve == RarReserveResult::PROTECTED) {
          view->presentText(none, TString("This keeper is protected.\nYou cannot invade him at this time."_s));
        } else if (reserve != RarReserveResult::GRANTED) {
          view->presentText(none, TString("This keeper is already under attack by another invader. Try again later."_s));
        } else {
        optional<SavedGameInfo> savedInfo;
        doWithSplash(TString("Preparing the invasion..."_s), 1, [&] (ProgressMeter&) {
          string blob;
          if (rarFetchDungeon(targetId, blob)) {
            string rawd = rarLzmaDecompress(blob);
            if (!rawd.empty()) {
              FilePath t = userPath.file("rar_invade" + getSaveSuffix(GameSaveType::KEEPER));
              { ogzstream out(t.getPath()); out.write(rawd.data(), rawd.size()); }
              savedInfo = loadSavedGameInfo(t);
              // Keep the target's WHOLE game (model + PlayerControl + factory) -- at writeback we
              // re-serialize IT so all the invasion's changes (terrain, deaths, loot) persist.
              if (auto tg = loadFromFile<PGame>(t))
                rarInvasionTarget = std::move(*tg);
              t.erase();
            }
          }
        });
        if (rarInvasionTarget && savedInfo) {
          rarInvasionOrigPos = rarInvasionTarget->getMainModel()->position;
          // SHARE the target's model into our game for the fight (no factory merge needed -- ContentId is
          // globally interned, so our same-mod factory resolves the target's content).
          Model* invaded = game->addInvasionSite(targetPos,
              PModel(rarInvasionTarget->getMainModel().giveMeSharedPointer()), *savedInfo);
          vector<Creature*> moved;
          for (Creature* c : team) // the FULL team, not just the controlled one
            if (game->canTransferCreature(c, invaded)) {
              game->transferCreature(c, invaded);
              moved.push_back(c);
            }
          game->setWasTransfered();
          // remember the live invasion so we can write the dungeon back when control is released
          game->recordActiveInvasion(targetPos, targetId, invaded, *savedInfo, moved);
          rarPvpSetAway(game->getGameIdentifier(), true); // out raiding -> my base is protected until I'm home
        } else {
          rarInvasionTarget = nullptr;
          rarReleaseDungeon(targetId); // reserved but nothing to load -> free it again
          view->presentText(none, TString("Couldn't load the target dungeon from the server."_s));
        }
        } // end 4b reservation-granted branch
        } // end if (!rarHandledLive) -- offline (retired-blob) invasion path
      }
    // RAR live PvP (defender side): poll for a live invite against my base. When one arrives, announce it, pack
    // my base, exchange blobs with the invader, and drop into the real-time lockstep battle. Until this fires I
    // keep playing as usual. Only when NOT already running an invasion of my own.
    if (rarEnabled() && !rarLiveHosting && !game->hasActiveInvasion()) {
      string sessionId, invaderName; int seed = 0;
      if (rarPvpPendingInvite(sessionId, invaderName, seed)) {
        // AUTHORITATIVE host: the defender does NOT pack/send his own base and does NOT switch modes. He just
        // fetches the invader's team and injects it into his LIVE game, then keeps playing. (A brief notice only.)
        // Non-blocking: a modal dialog would freeze THIS player's clock while the invader keeps playing (an
        // instant desync), and the arrival work below must proceed regardless. Goes to the message log instead.
        if (auto* pc = game->getPlayerControl())
          pc->addMessage(PlayerMessage(TString("You are being invaded by " + invaderName + "!"),
              MessagePriority::HIGH)); // NOT critical: PlayerControl::addMessage stops the clock for CRITICAL
        string invBlob; bool got = false;
        doWithSplash(TString("An enemy keeper's forces are arriving..."_s), [&] {
          auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
          while (std::chrono::steady_clock::now() < deadline) {
            if (rarPvpFetchState(sessionId, 1, invBlob) && !invBlob.empty()) { got = true; break; }
            if (rarPvpReady(sessionId, 0, false) == RarPvpReady::Gone) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
          }
        });
        string diag = string("got=") + (got ? "y" : "n") + " sz=" + toString((int) invBlob.size());
        if (got) {
          // Send my CLEAN base to the invader FIRST (before injecting her team), so she loads a base WITHOUT a
          // duplicate of her own team -- she injects her team onto it on her side. Then inject her team into MY
          // game so I see the invasion, and open the relay for position sync.
          doWithSplash(TString("An enemy keeper is invading your base..."_s), [&] {
            rarPvpUploadState(sessionId, 0, rarSerializeGameBlob(game));
          });
          // Guarded: a stale or truncated blob (e.g. a session that ended) must not take the game down -- an
          // uncaught exception here terminates the process outright ("Terminated due to unknown reason").
          try {
            rarLiveInvaderGame = rarInjectLiveInvader(game.get(), invBlob, diag, rarLiveTeamIds, rarLiveSitePos);
          } catch (...) {
            rarLiveInvaderGame = nullptr;
          }
          if (rarLiveInvaderGame) {
            rarLiveHosting = true;
            game->rarSetLivePvp(true);         // live PvP: no turn-based pausing while the battle is on
            rarStopPvpWatch();                 // stop polling -- we're now hosting this battle
            // Live sync: the invader owns her team (here they're PUPPETS driven by her stream), I own my base.
            rarLiveBattlefield = game->getMainModel().get();
            rarLiveStart = std::chrono::steady_clock::now();
            rarLiveStream = RarLiveStream();   // per-battle: see the invader side for what carrying it over broke
            rarLastDeadSeen = 0;
            rarHostSeq = 0;                    // per-CONNECTION, same reason -- see the invader side
            rarHostCmdSeq = 0;
            if (auto* dpc = game->getPlayerControl())
              dpc->addMessage(PlayerMessage(TString("[live] HOST START team=" + toString((int) rarLiveTeamIds.size())),
                  MessagePriority::HIGH));
            rarLiveIsInvader = false;
            game->rarSetLiveInvader(false);  // I am authoritative: my own team control stays completely normal
            rarLiveSessionId = sessionId;
            rarFx->subscribeTo(rarLiveBattlefield); // record my spell/projectile visuals for the peer
            // The wait for the invader's forces can leave this game's clock stopped. The battle is starting now,
            // so make sure time is running again -- otherwise the defender stays frozen while the invader plays.
            if (view->isClockStopped())
              view->continueClock();
            for (long long id : rarLiveTeamIds)
              rarLiveTeamIdSet.insert(id);
            // Same guard as the invader side: never assign over a joinable thread.
            rarHostNet.close();
            if (rarHostConnectThread.joinable())
              rarHostConnectThread.join();
            rarHostConnected.store(false);
            rarHostConnectThread = std::thread([&rarHostNet, &rarHostConnected, sid = sessionId]() {
              rarHostConnected.store(rarHostNet.connect(rarServerHost(), RAR_LOCKSTEP_DEFAULT_PORT, sid, 0, 120000));
            });
          }
        }
        (void) diag; // (injection diagnostics kept in the string, no longer shown -- a modal dialog stops this
                     // player's clock while the other keeps playing, which desyncs the battle)
        // NOTE: no return -- the defender keeps playing his normal game with the invaders present.
      }
    }
    // LIVE PvP SYNC -- the DEFENDER IS AUTHORITATIVE. The invader sends where she wants her team (a prediction
    // from her local control) and the defender applies it; his simulation then decides where everything actually
    // is and he streams back the position of EVERY creature -- including her team -- which her client applies over
    // its local state. So the two screens agree by construction instead of drifting apart.
    // Runs on THIS thread so it never races the simulation.
    if (rarLiveBattlefield && rarHostConnected.load()) {
      // A live battle can NEVER be paused: a stopped clock on one side while the other keeps playing is exactly
      // how the two games drift apart. Force time to keep running for both players.
      if (view->isClockStopped())
        view->continueClock();
      if (!rarLiveLinkLogged) {   // report once per battle whether the relay actually paired
        rarLiveLinkLogged = true;
        if (auto* lpc = game->getPlayerControl())
          lpc->addMessage(PlayerMessage(TString(string("[live] LINK ")
              + (rarHostNet.connected() ? "ok" : "FAIL")), MessagePriority::HIGH));
      }
      std::string snap;
      while (rarHostNet.tryGetRemote(rarHostCmdSeq, snap)) {
        // "C <order>" lines: the invader's banner orders. Only the defender acts on them -- his sim walks her team
        // (leader goes, the rest follow) and the resulting positions stream back to her.
        if (!rarLiveIsInvader)
          for (auto& line : split(snap, {'\n'}))
            if (line.size() > 2 && line[0] == 'C' && line[1] == ' ') {
              if (auto* dpc = game->getPlayerControl())
                dpc->addMessage(PlayerMessage(TString("[live] ORDER team=" + toString((int) rarLiveTeamIds.size())),
                    MessagePriority::HIGH));
              rarApplyInvaderCommand(game.get(), rarLiveTeamIds, line.substr(2));
            }
        rarApplyStream(rarLiveBattlefield, snap, rarLiveTeamIdSet, /*applyTeam*/ !rarLiveIsInvader, rarLiveStream,
            /*all*/ rarLiveIsInvader);
        // "E" = the defender declares the battle over. Read it only AFTER the frame has been applied above: the
        // final deaths ride in this same frame, so by now whoever is still standing here really did survive.
        if (rarLiveIsInvader)
          for (auto& line : split(snap, {'\n'}))
            if (line == "E")
              rarLiveEndSignalled = true;
        ++rarHostCmdSeq;
      }
      auto snow = std::chrono::steady_clock::now();
      if (snow >= rarNextStream) {
        rarNextStream = snow + std::chrono::milliseconds(50); // tighter interval = less visible position lag
        auto delta = rarStreamOwned(rarLiveOwned, rarLiveBattlefield, rarLiveTeamIdSet, /*sendTeam*/ rarLiveIsInvader,
            rarLiveStream, none, /*all*/ !rarLiveIsInvader);
        for (auto& l : rarFx->lines)             // spell/projectile visuals fired since the last update
          delta += l + "\n";
        rarFx->lines.clear();
        if (auto order = game->rarTakeOrder())   // the invader's banner order rides along with her update
          delta = "C " + *order + "\n" + delta;
        if (!rarHostNet.sendTick(rarHostSeq++, delta))
          rarLiveBattlefield = nullptr; // peer gone -> stop syncing, both keep playing
      }
      if (rarLiveStream.deadIds.size() != rarLastDeadSeen) {
        rarLastDeadSeen = rarLiveStream.deadIds.size();
        rarReconcileDeaths();
      }
      if (!rarHostNet.connected())
        rarLiveBattlefield = nullptr;
    }
    // NOTE: the teardown below is deliberately OUTSIDE the "battlefield && connected" block above. When the peer
    // drops we set rarLiveBattlefield = nullptr, which made that block stop running -- so the teardown never
    // executed: rarLiveHosting stayed true forever, the invite watch was never restarted, and the defender then
    // silently IGNORED every future invasion (the invader waited on "invading..." indefinitely).
    // INVADER: the defender VANISHED mid-battle (closed the game with the X, lost connection, crashed). Nothing
    // drives the battlefield any more -- his units and my own squad are puppets waiting on a stream that will
    // never arrive, and my orders go into a dead socket. Treat it as a forced retreat: release control, which
    // walks the squad home through the normal path (AI restored, team cleaned up, invasion ended properly).
    // The defender declared the battle over (his last invader fell, or he tore the battle down). Deaths from that
    // final frame are already applied, so this ends the excursion the normal way: release control, survivors
    // travel home through endActiveInvasion. Handled BEFORE the lost-contact fallback: the defender closes the
    // socket right after sending "E", and without this the graceful end was reported as a lost connection.
    if (rarLiveIsInvader && rarLiveEndSignalled) {
      rarLiveEndSignalled = false;
      rarHostConnected.store(false);   // handled -- the fallback below must not fire too
      rarReconcileDeaths();            // apply anything still outstanding before anyone travels home
      if (auto* pc = game->getPlayerControl()) {
        pc->addMessage(PlayerMessage(TString("The battle is over. Your surviving forces return home."_s),
            MessagePriority::HIGH));
        if (!game->getPlayerCreatures().empty())
          pc->leaveControl();
      }
    }
    if (rarLiveIsInvader && game->rarIsLivePvp() && rarHostConnected.load() && !rarHostNet.connected()) {
      rarHostConnected.store(false);           // handle once
      rarReconcileDeaths();            // the link died, but what it already told us stays true
      if (auto* pc = game->getPlayerControl()) {
        pc->addMessage(PlayerMessage(TString("You have lost contact with the enemy keeper. Your forces retreat."_s),
            MessagePriority::HIGH));
        if (!game->getPlayerCreatures().empty())
          pc->leaveControl();                  // -> team travels home -> endActiveInvasion -> normal teardown
      }
    }
    {
      // DEFENDER: tear the battle down when the invader is gone (disconnected, or every invader dead). Without
      // this the invader's whole base model stays hung on my campaign AND her creatures stay on my map -- a later
      // save & exit would serialize HER base into MY save and upload it as my dungeon.
      // Grace period: the invaders take a moment to appear/settle after injection. Without it the check below
      // sees "no invaders alive" on the very first tick and tears the battle down immediately -- the defender
      // logged "invaded" then "repelled" back to back while the invader was still standing there, and her orders
      // had nothing left to command.
      if (rarLiveHosting && !rarLiveIsInvader &&
          std::chrono::steady_clock::now() - rarLiveStart > std::chrono::seconds(10)) {
        bool anyInvaderAlive = false;
        for (Creature* c : game->getMainModel()->getAllCreatures())
          if (rarLiveTeamIdSet.count(c->getUniqueId().getGenericId()) && !c->isDead()) {
            anyInvaderAlive = true;
            break;
          }
        if (!rarHostNet.connected() || !anyInvaderAlive) {
          // FLUSH FIRST -- before anything is removed and before the socket is closed. Deaths are only ever put on
          // the wire by a stream tick, and that runs every 50ms; the last invaders to fall die BETWEEN two ticks,
          // so closing here simply threw their death notices away. The invader never learned they had fallen: her
          // copies were still alive locally, the dead socket read as "lost contact", and the forced retreat then
          // walked ALREADY-DEAD minions home -- they existed twice over, as corpses here and alive in her dungeon.
          // This final diff emits those outstanding "D" lines, and "E" tells her the battle is over.
          // Order matters: the stragglers below are still on the map at this point, so they are NOT in the
          // vanished set and are correctly left alone instead of being reported dead.
          if (rarHostNet.connected()) {
            auto last = rarStreamOwned(rarLiveOwned, game->getMainModel().get(), rarLiveTeamIdSet,
                /*sendTeam*/ false, rarLiveStream, none, /*all*/ true);
            rarHostNet.sendTick(rarHostSeq++, last + "E\n");
          }
          // Remove any invaders still standing. They RETREATED (the invader released control / left), and in her
          // own game they walked home alive -- killing them here would wrongly report them dead to her. Take them
          // off my map instead: erase from the level and out of the time queue, no death, no corpse.
          for (Creature* c : game->getMainModel()->getAllCreatures())
            if (rarLiveTeamIdSet.count(c->getUniqueId().getGenericId()) && !c->isDead()) {
              auto p = c->getPosition();
              if (p.isValid() && p.getLevel() && p.getCreature() == c)
                game->getMainModel()->extractCreature(c); // pulls it out of the time queue AND the level safely
                                                          // (returned owner pointer drops here -> creature freed)
            }
          // Stop syncing BEFORE anything is freed, same reason as the invader-side teardown above.
          rarHostNet.close();
          if (rarHostConnectThread.joinable())
            rarHostConnectThread.join();   // must not stay joinable -> next invasion would std::terminate
          rarHostConnected.store(false);
          rarLiveBattlefield = nullptr;
          rarFx->unsubscribe();
          rarFx->lines.clear();
          // Drop every reference into HER model before it is freed. My PlayerControl keeps transient battle
          // state (message positions, attack/stun notifications) and my creatures keep lastCombatIntent pointing
          // at whoever they just fought -- the invaders. Destroying her model with those still set left dangling
          // pointers and the game vanished silently the moment she was defeated. The offline invasion does
          // exactly this scrub for the same reason.
          if (auto* dpc = game->getPlayerControl())
            dpc->rarClearStaleRefs();   // clears blindly -- inspecting the positions is what crashed
          for (Creature* c : game->getMainModel()->getAllCreatures())
            c->clearLastCombatIntent();
          game->destroyInvasionSite(rarLiveSitePos);   // unhook her model -> it never reaches my save
          rarLiveInvaderGame = nullptr;
          rarLiveHosting = false;
          rarLiveTeamIdSet.clear();
          rarLiveOwned.clear();
          rarLiveTeamIds.clear();
          game->rarSetLivePvp(false);                  // back to normal rules
          if (!rarLiveSessionId.empty())               // close the session out, or I get re-invited into it
            rarPvpReady(rarLiveSessionId, 0, true);
          rarLiveSessionId.clear();
          if (auto* pc = game->getPlayerControl())
            pc->addMessage(PlayerMessage(TString(string("The invaders have been repelled. [live] HOST END conn=")
                + (rarHostNet.connected() ? "y" : "n")), MessagePriority::HIGH));
          rarStartPvpWatch(game->getGameIdentifier()); // can be invaded again
        }
      }
      // The defender's simulation killed a creature I was controlling -> the battle is over for me. End it here,
      // in the game loop, instead of killing the creature out-of-band mid-simulation.
      if (game->rarIsLivePvp() && game->rarIsLiveDefeat()) {
        // Defeat ends the BATTLE, not the game. Returning from playGame here dumped the player out to the main
        // menu; instead release control so the view goes back to my own keeper and base, and let the normal
        // invasion-end path bring any survivors home.
        rarHostNet.close();
        rarLiveBattlefield = nullptr;
        game->rarSetLivePvp(false);
        game->rarClearLiveDefeat();
        if (auto* pc = game->getPlayerControl()) {
          pc->addMessage(PlayerMessage(TString("Your forces have been defeated in the enemy's base."_s),
              MessagePriority::HIGH));
          if (!game->getPlayerCreatures().empty())
            pc->leaveControl();
        }
        // The creature whose death ended the battle could not be killed at the time -- it was the last one I
        // controlled. Control is released now, so apply the authoritative result; skipping it sent a creature
        // home alive whose corpse is lying in the enemy's dungeon. It may be on the battlefield or already back
        // on my own model, so look across both.
        rarReconcileDeaths();   // control is released now -- anything held back above can finally be applied
        if (auto victim = game->rarTakeLiveDefeatVictim())
          for (Model* mm : game->getAllModels()) {
            bool done = false;
            for (Creature* c : mm->getAllCreatures())
              if (c->getUniqueId().getGenericId() == *victim && !c->isDead()
                  && c->getPosition().isValid() && c->getPosition().getCreature() == c) {
                c->dieNoReason();
                done = true;
                break;
              }
            if (done)
              break;
          }
      }
    }
    // SIEGE (invader side): the dungeon's owner logged in while we're inside. He can't enter until we're out,
    // so we get a short grace to retreat and are then evicted. Poll on a timer -- not every frame.
    // Evicting = leaveControl(): it walks every team member off the invaded model back to our own base, which
    // is exactly what "exit control mode" does by hand, and the block below then sees invasionTeamLeftDungeon()
    // and runs the normal writeback -> upload -> release path. Nothing invasion-specific to unwind.
    if (rarEnabled() && game->hasActiveInvasion() && !game->invasionTeamLeftDungeon()) {
      auto now = std::chrono::steady_clock::now();
      if (now >= rarNextSiegeCheck) {
        rarNextSiegeCheck = now + std::chrono::seconds(5);
        long long left = 0;
        if (auto gid = game->getActiveInvasionGameId())
          if (rarInvasionStatus(*gid, left) == RarInvasionStatus::OwnerReturning) {
            if (!rarSiegeWarned) { // announce once, not every poll
              rarSiegeWarned = true;
              view->presentTextCenter(TString("Master of dungeon is coming."_s));
              view->presentTextCenter(TString("You have 1 minute left before forceably removed."_s));
            }
            if (left <= 0 && game->getPlayerControl()) {
              view->presentTextCenter(TString("You have been forced out of the dungeon."_s));
              game->getPlayerControl()->leaveControl(); // team walks home -> invasion ends below
            }
          }
      }
    }
    // RAR online: once the team has LEFT the invaded tile (travelled out or wiped), write the
    // damaged dungeon back and FULLY remove it. Order is critical: serialize the still-intact model
    // FIRST, then drop our shared_ptr + destroyInvasionSite() -> refcount 0 -> the model is destroyed
    // and every weak_ptr into it (creature AI, known tiles) EXPIRES, so the invader's save is clean.
    // The lzma+upload runs on a detached thread (only bytes captured) so the game doesn't freeze.
    // The team this invasion created can only be cancelled while nothing is controlled. When a battle ended with
    // the squad still in hand (chaining straight into the next invasion) the cancel below was skipped and the id
    // merely "kept for later" -- but nothing ever came back for it. The team leaked, the NEXT invasion created
    // another one with the same minions, and the Teams panel filled up with duplicate rows. This is that "later".
    if (rarLiveCreatedTeam && !rarLiveBattlefield && !game->hasActiveInvasion()
        && game->getPlayerCreatures().empty()) {
      if (auto* mypc = game->getPlayerControl())
        if (mypc->getTeams().exists(*rarLiveCreatedTeam))
          mypc->getTeams().cancel(*rarLiveCreatedTeam);
      rarLiveCreatedTeam = none;
    }
    if (rarEnabled() && game->hasActiveInvasion() && game->invasionTeamLeftDungeon())
      if (auto ended = game->endActiveInvasion()) {
        Vec2 pos = ended->pos;
        string gid = ended->gameId;
        ended = none;
        rarPvpSetAway(game->getGameIdentifier(), false); // home again -> invadable as normal
        // LIVE PvP: shut the sync down BEFORE the model is freed below. rarLiveBattlefield points AT that model,
        // and the FX listener is subscribed to it -- releasing control (leaveControl) reaches here, so the next
        // sync tick would read freed memory and the game would vanish with no crash message.
        // rarIsLivePvp() alone is WRONG here: the defeat handler clears it before this point, so after a defeat
        // this whole teardown was skipped -- the brokered session stayed open, the invasion's team was never
        // cancelled (which is why the Teams panel collected a duplicate row per battle), squad AI was never
        // restored and the tally never printed. The sticky flag is still set; it is consumed further below.
        if (rarLiveWasLive || game->rarIsLivePvp()) {
          rarHostNet.close();
          if (rarHostConnectThread.joinable())
            rarHostConnectThread.join();   // must not stay joinable -> next invasion would std::terminate
          rarHostConnected.store(false);
          // [live] diagnostic: dead = deaths the defender reported to me, home = team members that came back
          // alive. Anything other than dead+home == the squad size means a death was lost on the wire.
          if (auto* mypc = game->getPlayerControl()) {
            int home = 0;
            for (Creature* c : game->getMainModel()->getAllCreatures())
              if (rarLiveTeamIdSet.count(c->getUniqueId().getGenericId()) && !c->isDead())
                ++home;
            mypc->addMessage(PlayerMessage(TString(string("[live] TALLY dead=")
                + toString((int) rarLiveStream.deadIds.size()) + " home=" + toString(home)
                + " team=" + toString((int) rarLiveTeamIdSet.size())), MessagePriority::HIGH));
          }
          rarLiveBattlefield = nullptr;
          rarLiveTeamIdSet.clear();
          rarLiveOwned.clear();
          rarLiveTeamIds.clear();
          rarFx->unsubscribe();
          rarFx->lines.clear();
          // Same scrub the DEFENDER does when he tears a battle down -- the invader never did it, and she is the
          // one whose squad comes HOME and keeps moving afterwards. Her PlayerControl holds transient refs into
          // the battlefield model that is freed just below (message positions, battle summary, and the
          // visibility map's per-creature tile lists); the next move of a returned minion walked them.
          if (auto* mypc2 = game->getPlayerControl())
            mypc2->rarClearStaleRefs();
          rarLiveIsInvader = false;
          game->rarSetLiveInvader(false);
          // Drop the brokered session. Left alive, the server keeps offering it and the DEFENDER is invited into
          // the same finished battle again ("you are being invaded..." right after it ended) -- then tries to load
          // a battle that no longer exists.
          if (!rarLiveSessionId.empty())
            rarPvpReady(rarLiveSessionId, 1, true);
          rarLiveSessionId.clear();
          // Remove the team this invasion created; vanilla only auto-cancels single-member teams, so otherwise
          // every invasion leaves another entry behind in the Teams panel.
          // Restore normal AI to my own forces: during the battle the companions were PUPPETS (idle) driven by
          // the defender's stream. Left that way they stand frozen at home, not following their master.
          // Skip while the squad is STILL CONTROLLED (chained invasion: travelling straight to the next target) --
          // replacing a controlled creature's controller would rip it out of the player's hands mid-campaign.
          if (auto* mycol = game->getPlayerCollective())
            if (game->getPlayerCreatures().empty())
            for (Creature* c : mycol->getCreatures()) {
              if (rarLiveTeamIdSet.count(c->getUniqueId().getGenericId()) && !c->isDead())
                c->setController(makeOwner<Monster>(c, MonsterAIFactory::collective(mycol)));
              for (Creature* comp : c->getCompanions())
                if (!comp->isDead())
                  comp->setController(makeOwner<Monster>(comp, MonsterAIFactory::summoned(c)));
            }
          // ONLY when nothing is controlled any more. Chaining invasions (travel straight from this base to the
          // next target) ends this battle while the squad is STILL CONTROLLED -- cancelling their team there
          // leaves a controlled creature with no active team, and PlayerControl::getTeam asserts on that
          // (player_control.cpp CHECK(!ret.empty())). Still controlling => the team is in use, leave it alone.
          if (rarLiveCreatedTeam && game->getPlayerCreatures().empty())
            if (auto* mypc = game->getPlayerControl())
              if (mypc->getTeams().exists(*rarLiveCreatedTeam))
                mypc->getTeams().cancel(*rarLiveCreatedTeam);
          if (game->getPlayerCreatures().empty())
            rarLiveCreatedTeam = none;   // keep the record while it is still in use, so it can be cleaned up later
        }
        // Unregister the (shared) model from OUR game + scrub OUR refs to it. The target game still owns
        // the model (shared), so it survives -- with all the invasion's damage (terrain, deaths, loot).
        game->destroyInvasionSite(pos);
        string outRaw;
        bool conquered = false;
        string slayerName;
        // LIVE PvP: do NOT touch the server for this base at all. The defender is ONLINE and his own game is
        // authoritative -- uploading our copy under our login would overwrite his base AND flag it "invaded", so
        // he'd get a bogus "somebody invaded while you were away, loading the aftermath" on his next load. Also
        // skip conquest-marking and the reservation release (we never reserved it for a live battle).
        const bool wasLive = rarLiveWasLive || game->rarIsLivePvp();
        rarLiveWasLive = false;
        if (wasLive) {
          rarInvasionTarget = nullptr;
          game->rarSetLivePvp(false); // battle over -> back to normal turn-based rules
        }
        if (rarInvasionTarget) {
          // Did the invader slay the target keeper's leader? If so the base is doomed -> tell the server,
          // which keeps it on the world map for the grace period then removes it entirely.
          if (auto tc = rarInvasionTarget->getPlayerCollective())
            conquered = tc->isConquered();
          if (conquered) // the slayer = THIS (invading) player's KEEPER; shown to the owner on their next Load
            if (auto pc = game->getPlayerCollective())
              if (auto leader = pc->getLeaders().getFirstElement())
                if (auto& fn = (*leader)->getName().first()) // the keeper's chosen NAME, not its species
                  slayerName = *fn;
          // (if the keeper has no chosen name, rarMarkConquered falls back to the invader's account login)
          Model* m = rarInvasionTarget->getMainModel().get();
          m->setGame(rarInvasionTarget.get());     // re-home the model in its own game
          m->position = rarInvasionOrigPos;
          // The fight advanced m's internal time inside the INVADER's game, but rarInvasionTarget's own
          // localTime table is still pre-invasion -> on load the update loop stalls (minions frozen until
          // control is taken). Sync the table to m's real time so minions act immediately on load.
          rarInvasionTarget->resyncModelLocalTime(m);
          // Sever cross-refs to the INVADER's creatures before serializing (defenders' combat intent +
          // the owner PlayerControl's transient battle notifications) so we don't drag invader state in.
          if (auto pc = rarInvasionTarget->getPlayerControl())
            pc->scrubInvadedModelRefs(game->getMainModel().get());
          for (Model* mm : rarInvasionTarget->getAllModels()) {
            for (Creature* c : mm->getAllCreatures())
              c->clearLastCombatIntent();
            for (Level* l : mm->getLevels())
              l->clearSectors();               // drop recomputable pathfinding -> ~.sit size
          }
          FilePath t = userPath.file(gid + ".rarwb");
          doWithSplash(TString("Recording the aftermath..."_s), [&] {
            saveGame(rarInvasionTarget, t);    // the ACTUAL fought state, terrain + deaths + all
            igzstream gzin(t.getPath());
            outRaw.assign((std::istreambuf_iterator<char>(gzin)), std::istreambuf_iterator<char>());
            gzin.close();
          });
          t.erase();
          rarInvasionTarget = nullptr;
        }
        string slayerGameId = game->getGameIdentifier(); // the INVADER's gameId -> server resolves our keeper
                                                          // NAME from its claim (world-map label, not species)
        if (!wasLive)
          std::thread([outRaw = std::move(outRaw), gid, conquered, slayerName, slayerGameId]() {
            if (!outRaw.empty()) {
              string blob = rarLzmaCompress(outRaw);
              if (!blob.empty())
                rarUploadDungeon(gid, blob, rarSha256Hex(outRaw)); // update the per-dungeon hash too
            }
            if (conquered)
              rarMarkConquered(gid, slayerName, slayerGameId); // leader slain -> server keeps the base then removes it
            rarReleaseDungeon(gid); // invasion over -> free the reservation for other invaders
          }).detach();
      }
    // Phase B: report any villain the player has conquered (leader killed) so the server marks it defeated
    // (+ starts its loot grace + respawns the tier). Fires immediately at the kill so the world-map sprite
    // flips to defeated even while the player is still looting.
    if (rarEnabled())
      for (Collective* col : game->getCollectives()) {
        if (isConquerableSite(col->getVillainType()) && col->isConquered()) {
          Vec2 pos = col->getModel()->position;
          string key = toString(pos.x) + "_" + toString(pos.y);
          if (rarReportedVillains.insert(key).second)
            std::thread([key] { rarMarkVillainDefeated(key); }).detach();
        }
      }
    // RAR villain aftermath writeback: once the invading team has LEFT a villain we conquered, serialize its
    // post-battle state (dead defenders, dropped loot, damage) and upload it so grace-period revisits show
    // the real aftermath, then fully drop the transient model so it never leaks into the player's save.
    if (rarEnabled())
      while (auto wb = game->takeVillainWriteback()) {
        string key = toString(wb->pos.x) + "_" + toString(wb->pos.y);
        Model* m = wb->model.get();
        // RAR: the invading team has LEFT this conquered villain. Upload its post-battle aftermath so OTHER
        // players' grace-period revisits see the real state -- but KEEP the model alive locally. It used to be
        // destroyed right here, the same tick the player returns to base, which is exactly why the PILLAGE
        // action never appeared in the village panel on the FIRST return (the player had to make a pointless
        // second trip that re-injected the villain -- and that re-injected copy was NOT written back again, so
        // it survived and finally showed pillage). Keeping it alive on the first writeback reproduces that
        // working state immediately: the panel shows PILLAGE for each conquered village, the player pillages at
        // leisure from base, and takeVillainWriteback won't touch it again (it's now in villainWrittenBack).
        for (Creature* c : m->getAllCreatures())
          c->clearLastCombatIntent();     // drop cross-model Creature* refs before serializing this model's blob
        game->resyncModelLocalTime(m);    // so the villain's survivors aren't frozen on a later load
        SavedGameInfo info;
        info.name = wb->enemyId;
        info.progressCount = 1;
        if (!wb->enemyId.empty())
          info.retiredEnemyInfo = SavedGameInfo::RetiredEnemyInfo{ EnemyId(wb->enemyId.c_str()), wb->type };
        string raw = serializeModelRaw(wb->model, info, game->getContentFactory());
        wb = none;                        // release our extra shared_ptr ref; models[pos] keeps the model alive
        std::thread([key, raw = std::move(raw)]() {
          string blob = rarLzmaCompress(raw);
          if (!blob.empty())
            rarVillainWriteback(key, blob);
        }).detach();
      }
    if (exitCondition)
      if (auto c = exitCondition(game.get()))
        return *c;
    auto gameTime = game->getGlobalTime();
    if (lastMusicUpdate < gameTime && withMusic) {
      jukebox->setType(game->getCurrentMusic(), true);
      lastMusicUpdate = gameTime;
    }
    auto autoSaveFreq = options->getIntValue(OptionId::AUTOSAVE2);
    if (autoSaveFreq > 0 && lastAutoSave < gameTime - TimeInterval(autoSaveFreq) && !noAutoSave
        && !game->isInvading()) { // RAR: don't autosave mid-invasion (keeper OR villain) -- the transient dungeon must not leak into the save
      // NOTE: deliberately NO clearSectors() here, unlike the save & exit path. Level::sectors is NOT
      // serialized (it is absent from Level::serialize, and Level even rebuilds WALK sectors eagerly on load),
      // so stripping does nothing to the file -- it would only throw away a live cache and force a rebuild
      // mid-play for no gain. The .aut is therefore ALREADY in the same shape as a .kep and uploads verbatim.
      saveUI(game, GameSaveType::AUTOSAVE);
      eraseAllSavesExcept(game, GameSaveType::AUTOSAVE);
      lastAutoSave = gameTime;
    }
    view->refreshView();
  }
}

void MainLoop::eraseAllSavesExcept(const PGame& game, optional<GameSaveType> except) {
  for (auto erasedType : ENUM_ALL(GameSaveType))
    if (erasedType != GameSaveType::WARLORD && erasedType != except)
      eraseSaveFile(game, erasedType);
}

optional<RetiredGames> MainLoop::getRetiredGames(CampaignType type) {
  if (rarEnabled())
    return none; // RAR online: no retired dungeons -- skip the online fetch and its splash message
  switch (type) {
    case CampaignType::FREE_PLAY: {
      RetiredGames ret;
      for (auto& info : getSaveFiles(userPath, getSaveSuffix(GameSaveType::RETIRED_CAMPAIGN)))
        if (isCompatible(getSaveVersion(info)))
          if (auto saved = loadSavedGameInfo(userPath.file(info.filename)))
            ret.addLocal(*saved, info, true);
      for (auto& info : getSaveFiles(userPath, getSaveSuffix(GameSaveType::RETIRED_SITE)))
        if (isCompatible(getSaveVersion(info)))
          if (auto saved = loadSavedGameInfo(userPath.file(info.filename)))
            if (!saved->retiredEnemyInfo)
              ret.addLocal(*saved, info, false);
      vector<FileSharing::SiteInfo> onlineSites;
      optional<string> error;
      FileSharing::CancelFlag cancel;
      doWithSplash(TStringId("FETCHING_LIST_OF_RETIRED_DUNGEONS"), 1,
          [&] (ProgressMeter& progress) {
            if (auto sites = fileSharing->listSites(cancel, progress))
              onlineSites = *sites;
            else
              error = sites.error();
          },
          [&] {
            cancel.cancel();
          });
      if (error)
        view->presentText(none, TString(*error));
      for (auto& elem : onlineSites)
        if (isCompatible(elem.version))
          ret.addOnline(fromOldInfo(elem.gameInfo), elem.fileInfo, elem.totalGames, elem.wonGames, elem.subscribed,
              elem.author, elem.isFriend);
      ret.sort();
      return ret;
    }
    default:
      return none;
  }
}

PGame MainLoop::prepareTutorial(const ContentFactory* contentFactory) {
  PGame game = loadGame(dataFreePath.file("tutorial.kep"), TStringId("TUTORIAL"));
  if (game) {
    USER_CHECK(contentFactory->immigrantsData.count("tutorial"));
    Tutorial::createTutorial(*game, contentFactory);
  } else
    view->presentText(none, TStringId("FAILED_TO_LOAD_TUTORIAL"));
  return game;
}

struct ModelTable {
  Table<PModel> models;
  vector<ContentFactory> factories;
  int numRetiredVillains;
};

vector<string> MainLoop::getCurrentMods() const {
  return options->getVectorStringValue(OptionId::CURRENT_MOD2)
      .filter([&](const string& name) { return !!getLocalModVersionInfo(name); });
}

TilePaths MainLoop::getTilePathsForAllMods() const {
  auto readTiles = [&] (const GameConfig* config, vector<string> modNames) {
    vector<TileInfo> tileDefs;
    if (auto res = config->readObject(tileDefs, GameConfigId::TILES, nullptr))
      return optional<TilePaths>();
    return optional<TilePaths>(TilePaths(std::move(tileDefs), std::move(modNames)));
  };
  auto currentMod = getCurrentMods();
  GameConfig currentConfig = getGameConfig(currentMod);
  auto ret = readTiles(&currentConfig, currentMod);
  for (auto modName : modsDir.getSubDirs())
    if (!!getLocalModVersionInfo(modName)) {
      GameConfig config({modsDir.subdirectory(modName)});
      if (auto paths = readTiles(&config, {modName})) {
        if (ret)
          ret->merge(*paths);
        else
          ret = paths;
      }
    }
  USER_CHECK(ret) << "No available tile paths found";
  return *ret;
}

bool MainLoop::rarLoginFlow() {
  if (!rarConfigured())
    return true; // offline build: no login needed
  // Pick the server from the public list before asking for credentials. The list only supplies an ADDRESS --
  // the cert pin + PSK still come from appconfig, so nothing secret is published and a spoofed list can't
  // hijack anyone (a wrong server fails the pin check). If the list is unreachable we silently keep the
  // appconfig server_url, so a local/dev setup still works with no internet.
  {
    std::vector<std::string> servers; // std:: -- the client API is game-header-free (KeeperRL's vector differs)
    bool got = false;
    // Fetch the list in the BACKGROUND. A normal lookup is a blink, so showing a splash makes it flash
    // annoyingly -- instead show NOTHING if it finishes quickly, and only pop the "Looking up servers..." splash
    // if it's actually taking a while (a slow network, or the 5s connect timeout when GitHub is unreachable).
    std::atomic<bool> done{false};
    std::thread fetchThread([&] { got = rarFetchServerList(servers); done.store(true); });
    auto graceStart = std::chrono::steady_clock::now();
    while (!done.load() && std::chrono::steady_clock::now() - graceStart < std::chrono::milliseconds(350))
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!done.load())
      doWithSplash(TString("Looking up servers..."_s), [&] { fetchThread.join(); }); // slow -> show feedback
    else
      fetchThread.join(); // instant -> no splash at all
    if (!got)
      // Couldn't reach the server list. Say so (this is the "unable to contact" case the player should see),
      // then fall back to the appconfig server_url so a local/dev setup still works with no internet.
      view->presentTextCenter(TString("Couldn't reach the server list. Using the configured server."_s));
    if (got && !servers.empty()) {
      string chosen;
      if (servers.size() == 1)
        chosen = servers[0]; // only one server -> don't bother the player with a choice
      else {
        vector<TString> elems;
        for (auto& s : servers)
          elems.push_back(TString(s));
        auto idx = view->chooseAtMouse(elems);
        if (!idx)
          return false; // cancelled -> back to menu
        chosen = servers[*idx];
      }
      rarSetServerUrl(chosen.rfind("https://", 0) == 0 ? chosen : "https://" + chosen);
    }
  }
  rarSetCredentials("", ""); // online: always require a fresh login each time Play is pressed
  while (1) {
    auto login = view->getText(
        TString("Account name: an existing name logs you in.\nA new name creates an account."_s), "", 32);
    if (!login || login->empty())
      return false; // cancelled -> back to menu
    auto pw = view->getText(TString("Password:"_s), "", 32);
    if (!pw)
      return false;
    rarSetCredentials(*login, *pw);
    auto res = rarLoginCheck();
    if (res == RarLoginResult::Ok) {
      rarUploadPendingCrashes(); // only once authenticated: /crash needs a login, and it makes spam harder
      return true; // existing account, correct password, session acquired
    }
    if (res == RarLoginResult::AlreadyLoggedIn) {
      view->presentTextCenter(TString("This account is already logged in on another computer."_s));
      rarSetCredentials("", "");
      continue;
    }
    // Bad credentials could mean a brand-new account name -> try to register + log in.
    if (res == RarLoginResult::BadCredentials && rarRegister() && rarLoginCheck() == RarLoginResult::Ok) {
      rarUploadPendingCrashes();
      return true;
    }
    // Distinguish a down server from bad credentials: ping first.
    if (res == RarLoginResult::Unreachable || !rarServerReachable())
      view->presentTextCenter(TString("Server is currently not available. Please try again later."_s));
    else
      view->presentTextCenter(TString("Wrong username or password. Please try again."_s));
    rarSetCredentials("", ""); // clear so we stay logged-out while retrying
  }
}

PGame MainLoop::prepareCampaign(RandomGen& random) {
  // RAR online: match our mods to the server's before loading any content, so the downloaded
  // world blob's content IDs resolve. Aborts to the menu if a required mod can't be fetched.
  if (rarEnabled() && !syncServerMods())
    return nullptr;
  while (1) {
    ContentFactory contentFactory;
    tileSet->clear();
    // Using a splash screen causes a segfault due to reloading the tileset while scriptedUI is running
    //doWithSplash("Loading gameplay data", [&] {
      contentFactory = createContentFactory(false);
      if (tileSet)
        tileSet->setTilePaths(contentFactory.tilePaths);
    //});
    tileSet->loadTextures();
    // RAR online: drive the difficulty-related options from the shared world's campaign_info.txt before
    // the campaign is built. EXP_INCREASE (enemy difficulty curve) is server-fixed + HIDDEN. ENDLESS_ENEMIES
    // is config-DEFAULTED but remains a visible player option (the player may override it in the settings).
    if (rarEnabled()) {
      options->setValue(OptionId::EXP_INCREASE, contentFactory.campaignInfo.expIncrease);
      options->setValue(OptionId::ENDLESS_ENEMIES, contentFactory.campaignInfo.endlessEnemies);
    }
    if (!rarEnabled() && options->getIntValue(OptionId::SUGGEST_TUTORIAL) == 1) {
      auto tutorialIndex = view->multiChoice(TString(TStringId("START_WITH_TUTORIAL_PROMPT")), {
        TStringId("YES"),
        TStringId("NO"),
        TStringId("NO_AND_NEVER_ASK")
      });
      if (tutorialIndex == 0) {
        auto contentFactory2 = createContentFactory(true);
        if (auto ret = prepareTutorial(&contentFactory2))
          return ret;
      } else
      if (tutorialIndex == 2) {
        options->setValue(OptionId::SUGGEST_TUTORIAL, 0);
      }
    }
    auto avatarChoice = getAvatarInfo(view, contentFactory.keeperCreatures, &contentFactory, *unlocks, options);
    if (auto avatar = avatarChoice.getReferenceMaybe<AvatarInfo>()) {
      // RAR online: the world is SERVER-AUTHORITATIVE. Download the concrete map (terrain +
      // villains) generated once by `--rar_gen_world`; the client does NO generation, so every
      // player sees the identical world. Each still picks their own start tile (atomic claim).
      string worldName = contentFactory.getCreatures().getNameGenerator()->getNext(NameGeneratorId("WORLD"));
      optional<Table<Campaign::SiteInfo>> serverSites;
      if (rarEnabled()) {
        auto world = rarGetWorld();
        if (world.valid && !world.worldName.empty())
          worldName = world.worldName;
        string blob;
        if (rarFetchWorldData(blob)) {
          try {
            std::stringstream ss(blob);
            Table<Campaign::SiteInfo> sites;
            {
              InputArchive ar(ss);
              ar >> sites;
            }
            // World-map villains are reconciled to the server's live roster AFTER the campaign is built
            // (below), so respawns on NEW tiles appear + defeated ones vanish. Nothing to do to `sites` here.
            serverSites = std::move(sites);
          } catch (...) {
            view->presentText(none, TString("Failed to read the shared world from the server."_s));
          }
        } else
          view->presentText(none, TString(
              "The server has no world yet. Ask the admin to run: keeper.exe --rar_gen_world rar_campaign.dat"_s));
      }
      CampaignBuilder builder(view, random, options, contentFactory.villains, contentFactory.gameIntros, *avatar);
      tileSet->setTilePathsAndReload(getTilePathsForAllMods());
     if (auto setup = builder.prepareCampaign(&contentFactory, bindMethod(&MainLoop::getRetiredGames, this),
          CampaignType::FREE_PLAY, worldName, std::move(serverSites))) {
        // NOTE: villains are reconciled to the server's live roster INSIDE prepareCampaign, before the
        // base-placement UI draws the map -- not here. Reconciling after the fact showed the player a stale
        // map to choose his site on, and could overwrite the tile he had just picked. Don't move it back.
        // RAR online: other players' bases show as lightweight MARKERS only (from rarGetClaims,
        // step 8.1) -- we do NOT download their dungeons here. The full .sit is fetched on-demand
        // only for the single dungeon the player chooses to invade (8b, the "preparing invasion"
        // flow), to avoid pulling every dungeon (and the bandwidth) at startup.
        auto models = prepareCampaignModels(*setup, *avatar, random, &contentFactory);
        for (auto& f : models.factories)
          contentFactory.merge(std::move(f));
        map<string, string> analytics {
          {"retired_villains", toString(models.numRetiredVillains)},
          {"biome", setup->campaign.getBaseBiome().data()}
        };
        startedNewGame = true; // a brand-new keeper -> show the turn-0 rules wall once, back in start()
        return Game::campaignGame(std::move(models.models), *setup, std::move(*avatar), std::move(contentFactory),
            std::move(analytics));
      } else
        continue;
    } else {
      auto option = *avatarChoice.getValueMaybe<AvatarMenuOption>();
      switch (option) {
        case AvatarMenuOption::GO_BACK:
          return nullptr;
        case AvatarMenuOption::CHANGE_MOD:
          if (rarEnabled())
            showServerMods(); // online: read-only -- the server owns the mod set
          else
            showMods();
          continue;
        case AvatarMenuOption::TUTORIAL: {
          auto contentFactory = createContentFactory(true);
          if (auto ret = prepareTutorial(&contentFactory))
            return ret;
          else
            continue;
        }
      }
    }
  }
}

void MainLoop::showCredits() {
  auto data = ScriptedUIDataElems::Record{};
  view->scriptedUI("credits", data);
}

void MainLoop::showAchievements() {
  auto factory = createContentFactory(false);
  auto data = ScriptedUIDataElems::List{};
  for (auto& id : factory.achievementsOrder) {
    auto& info = factory.achievements.at(id);
    auto r = ScriptedUIDataElems::Record{};
    r.elems["name"] = info.name;
    r.elems["description"] = info.description;
    r.elems["view_id"] = info.viewId;
    if (unlocks->isAchieved(id))
      r.elems["unlocked"] = TString("blabla"_s);
    data.push_back(std::move(r));
  }
  view->scriptedUI("achievements", data);
}

const auto modVersionFilename = "version_info";

optional<ModVersionInfo> MainLoop::getLocalModVersionInfo(const string& mod) const {
  ifstream in(modsDir.subdirectory(mod).file(modVersionFilename).getPath());
  ModVersionInfo info {};
  in >> info.steamId >> info.version >> info.compatibilityTag;
  if (info.compatibilityTag == modVersion) // this also handles the check if the file existed and had sane contents
    return info;
  else
    return none;
}

void MainLoop::updateLocalModVersion(const string& mod, const ModVersionInfo& info) {
  ofstream out(modsDir.subdirectory(mod).file(modVersionFilename).getPath());
  if (!!out) {
    out << info.steamId << "\n" << info.version << "\n" << info.compatibilityTag << "\n";
  }
}

const auto modDetailsFilename = "details.txt";

optional<ModDetails> MainLoop::getLocalModDetails(const string& mod) {
  ifstream in(modsDir.subdirectory(mod).file(modDetailsFilename).getPath());
  ModDetails ret;
  in >> std::quoted(ret.author) >> std::quoted(ret.description);
  return ret;
}

void MainLoop::updateLocalModDetails(const string& mod, const ModDetails& info) {
  ofstream out(modsDir.subdirectory(mod).file(modDetailsFilename).getPath());
  if (!!out) {
    out << std::quoted(info.author) << "\n" << std::quoted(info.description) << "\n";
  }
}

void MainLoop::removeMod(const string& name) {
  // TODO: how to make it safer?
  auto modDir = modsDir.subdirectory(name);
  modDir.removeRecursively();
}

// When mod changes name, we have to remove old directory
void MainLoop::removeOldSteamMod(SteamId steamId, const string& newName) {
  auto modDir = modsDir;
  auto modList = modDir.getSubDirs();
  for (auto& modName : modList)
    if (modName != newName)
      if (auto ver = getLocalModVersionInfo(modName))
        if (ver->steamId == steamId)
          removeMod(modName);
}

vector<ModInfo> MainLoop::getAllMods(const vector<ModInfo>& onlineMods) {
  vector<string> modList = modsDir.getSubDirs();
  // check if the currentMod exists and has current version
  vector<ModInfo> allMods;
  set<SteamId> alreadyDownloaded;
  for (auto& mod : modList)
    if (auto version = getLocalModVersionInfo(mod)) {
      ModInfo modInfo;
      modInfo.versionInfo = *version;
      modInfo.name = mod;
      modInfo.canUpload = true;
      if (auto details = getLocalModDetails(mod))
        modInfo.details = *details;
      for (auto& onlineMod : onlineMods)
        if (onlineMod.versionInfo.steamId == version->steamId) {
          modInfo = onlineMod;
          if (!modInfo.canUpload && modInfo.versionInfo.version > version->version)
            modInfo.actions.push_back("Update");
          alreadyDownloaded.insert(onlineMod.versionInfo.steamId);
          break;
        }
      if (options->hasVectorStringValue(OptionId::CURRENT_MOD2, mod)) {
        modInfo.isActive = true;
        modInfo.actions.push_back("Deactivate");
      } else
        modInfo.actions.push_back("Activate");
      if (modInfo.canUpload)
        modInfo.actions.push_back("Upload");
      modInfo.isLocal = true;
      allMods.push_back(std::move(modInfo));
    }
  for (auto& mod : onlineMods)
    if (!alreadyDownloaded.count(mod.versionInfo.steamId)) {
      allMods.push_back(mod);
      allMods.back().actions.push_back("Download");
    }
  return allMods;
}

void MainLoop::downloadMod(ModInfo& mod) {
  optional<string> error;
  FileSharing::CancelFlag cancel;
  doWithSplash(TSentence("DOWNLOADING", TString(mod.name)), 1,
      [&] (ProgressMeter& meter) {
        modsDir.createIfDoesntExist();
        error = fileSharing->downloadMod(cancel, mod.name, mod.versionInfo.steamId, modsDir, meter);
        if (!error) {
          updateLocalModVersion(mod.name, mod.versionInfo);
          updateLocalModDetails(mod.name, mod.details);
          removeOldSteamMod(mod.versionInfo.steamId, mod.name);
        }
      },
      [&] {
        cancel.cancel();
      });
  if (error && !cancel.flag)
    view->presentText(TString(TStringId("ERROR_DOWNLOADING_FILE")), TString(*error));
}

void MainLoop::uploadMod(ModInfo& mod) {
  auto config = getGameConfig({mod.name});
  ContentFactory f;
  if (auto err = f.readData(&config, {mod.name})) {
    view->presentText(TString("Mod \"" + mod.name + "\" has errors: "), TString(*err));
    return;
  }
  FileSharing::CancelFlag cancel;
  optional<string> error;
  doWithSplash(TSentence("UPLOADING", TString(mod.name)), 1,
      [&] (ProgressMeter& meter) {
        error = fileSharing->uploadMod(cancel, mod, modsDir, meter);
        updateLocalModVersion(mod.name, mod.versionInfo);
      },
      [&] {
        cancel.cancel();
      });
  if (error && !cancel.flag)
    view->presentText(TString(TStringId("ERROR_UPLOADING_MOD")), TString(*error));
}

void MainLoop::createNewMod() {
  if (auto name = view->getText(TString("Enter a name for your new mod:"_s), "", 15)) {
    if (name->empty()) {
      view->presentText(none, TString("Mod name can't be empty"_s));
      return;
    }
    if (modsDir.getSubDirs().contains(*name)) {
      view->presentText(none, TString("Mod \"" + *name + "\" is alread installed"));
      return;
    }
    auto targetPath = modsDir.subdirectory(*name);
    targetPath.createIfDoesntExist();
    view->presentText(none, TString("Your mod is located in folder \""_s + targetPath.absolute().getPath() + "\""));
    updateLocalModVersion(*name, ModVersionInfo{0, 0, modVersion});
    updateLocalModDetails(*name, ModDetails{"", ""});
  }
}

vector<ModInfo> MainLoop::getOnlineMods() {
  vector<ModInfo> ret;
  optional<string> error;
  FileSharing::CancelFlag cancel;
  doWithSplash(TStringId("FETCHING_LIST_OF_ONLINE_MODS"), 1,
      [&] (ProgressMeter& meter) {
        if (auto mods = fileSharing->getOnlineMods(cancel, meter))
          ret = *mods;
        else
          error = mods.error();
        sort(ret.begin(), ret.end(), [](const ModInfo& m1, const ModInfo& m2) { return m1.upvotes > m2.upvotes; });
      },
      [&]{
        cancel.cancel();
      });
  if (error && !cancel.flag)
    view->presentText(none, TString(*error));
  return ret;
}

void MainLoop::showMods() {
  int highlighted = 0;
  int modIndex = 0;
  ScriptedUIState uiState{};
  uiState.highlightedElem = modIndex;
  vector<ModInfo> onlineMods = getOnlineMods();
  while (1) {
    bool clicked = false;
    auto getModInfo = [&] (ModInfo& mod) {
      auto modInfo = ScriptedUIDataElems::Record{{
        {"name"_s, TString(mod.name)},
        {"author"_s, TString(mod.details.author)},
        {"description"_s, TString(mod.details.description)},
      }};
      if (mod.upvotes + mod.downvotes > 0) {
        const int maxStars = 5;
        int rating = maxStars * mod.upvotes / (mod.downvotes + mod.upvotes);
        auto stars = ScriptedUIDataElems::List{};
        for (int j = 0; j < maxStars; ++j)
          stars.push_back(TString(j < rating ? "★"_s : "☆"_s));
        modInfo.elems["stars"] = std::move(stars);
      }
      auto getCallback = [&mod, &onlineMods, &clicked, this](const string& action) -> ScriptedUIDataElems::Callback {
        if (action == "Activate")
          return {[this, &clicked, name = mod.name] {
            options->addVectorStringValue(OptionId::CURRENT_MOD2, name);
            translations->setCurrentMods(options->getVectorStringValue(OptionId::CURRENT_MOD2));
            options->setChoices(OptionId::LANGUAGE, translations->getLanguages());
            clicked = true;
            return true;
          }};
        else if (action == "Deactivate")
          return {[this, &clicked, name = mod.name] {
            options->removeVectorStringValue(OptionId::CURRENT_MOD2, name);
            translations->setCurrentMods(options->getVectorStringValue(OptionId::CURRENT_MOD2));
            auto currentLang = options->getStringValue(OptionId::LANGUAGE);
            if (!translations->getLanguages().contains(currentLang))
              options->setValue(OptionId::LANGUAGE, "English"_s);
            options->setChoices(OptionId::LANGUAGE, translations->getLanguages());
            clicked = true;
            return true;
          }};
        else if (action == "Download" || action == "Update")
          return {[this, &clicked, &mod] {
            downloadMod(mod);
            clicked = true;
            return true;
          }};
        else if (action == "Upload")
          return {[this, &clicked, &mod, &onlineMods] {
            uploadMod(mod);
            onlineMods = getOnlineMods();
            clicked = true;
            return true;
          }};
        fail();
      };
      for (auto& action : mod.actions)
        modInfo.elems[action] = getCallback(action);
      if (steamAchievements && mod.versionInfo.steamId != 0)
        modInfo.elems["show_workshop"] = ScriptedUIDataElems::Callback {
            [id = mod.versionInfo.steamId] {
              openUrl("https://steamcommunity.com/sharedfiles/filedetails/?id=" + toString(id));
              return false;
            }
        };
      return modInfo;
    };
    vector<ModInfo> allMods = getAllMods(onlineMods);
    auto getState = [](const ModInfo& mod) {
      if (mod.isLocal)
        return 0;
      if (mod.isSubscribed)
        return 1;
      else
        return 2;
    };
    sort(allMods.begin(), allMods.end(), [getState](const ModInfo& m1, const ModInfo& m2) {
      int m1up = -m1.upvotes;
      int m2up = -m2.upvotes;
      return std::forward_as_tuple(getState(m1), m1up, m1.name)
           < std::forward_as_tuple(getState(m2), m2up, m2.name);
    });
    auto modLists = vector<ScriptedUIDataElems::List>(4);
    for (int i : All(allMods)) {
      auto& mod = allMods[i];
      auto modButton = ScriptedUIDataElems::Record{{
        {"name"_s, TString(mod.name)},
        {"choose"_s, ScriptedUIDataElems::Callback{[&modIndex, &clicked, i, &uiState] {
          modIndex = i; uiState.highlightedElem = i; clicked = true; return true;
        }}}
      }};
      if (i == modIndex)
        modButton.elems["selected"] = TString("xyz"_s);
      if (mod.isActive)
        modButton.elems["active"] = TString("xyz"_s);
      modLists[getState(mod)].push_back(std::move(modButton));
    }
    auto data = ScriptedUIDataElems::Record{{
      {"create_new", ScriptedUIDataElems::Callback{ [this, &clicked] { createNewMod(); clicked = true; return true;} }},
      {"local", modLists[0]},
      {"subscribed", modLists[1]},
      {"online", modLists[2]},
    }};
    if (!allMods.empty())
      data.elems["selected_mod"] = getModInfo(allMods[modIndex]);
    auto oldFunNext = *uiState.highlightNext.getValueMaybe<ScriptedUIDataElems::Callback>();
    auto oldFunPrev = *uiState.highlightPrevious.getValueMaybe<ScriptedUIDataElems::Callback>();
    uiState.highlightNext = ScriptedUIDataElems::Callback{
      [&oldFunNext, &modIndex, &uiState, &allMods, &clicked] {
        oldFunNext.fun();
        uiState.highlightedElem = max(0, min(allMods.size() - 1, *uiState.highlightedElem));
        modIndex = *uiState.highlightedElem;
        clicked = true;
        return true;
      }
    };
    uiState.highlightPrevious = ScriptedUIDataElems::Callback{
      [&oldFunPrev, &modIndex, &uiState, &allMods, &clicked] {
        oldFunPrev.fun();
        uiState.highlightedElem = max(0, min(allMods.size() - 1, *uiState.highlightedElem));
        modIndex = *uiState.highlightedElem;
        clicked = true;
        return true;
      }
    };
    view->scriptedUI("mods", data, uiState);
    uiState.highlightNext = oldFunNext;
    uiState.highlightPrevious = oldFunPrev;
    if (!clicked)
      break;
  }
}

// RAR online: a READ-ONLY mods view. The server dictates the mod set (syncServerMods mirrors its folder), so
// the client can only LOOK -- there are no activate/deactivate/download/upload/create callbacks, so the mods.txt
// template (all buttons are `Using "<action>"`) renders none of them. Reuses the "local" list + "selected_mod"
// detail panel; every listed mod is marked active (green check).
void MainLoop::showServerMods() {
  auto activeMods = getCurrentMods(); // = exactly the server's active mods, after syncServerMods
  int modIndex = 0;
  ScriptedUIState uiState{};
  while (1) {
    bool clicked = false;
    auto modList = ScriptedUIDataElems::List{};
    for (int i : All(activeMods)) {
      auto button = ScriptedUIDataElems::Record{{
        {"name"_s, TString(activeMods[i])},
        {"active"_s, TString("xyz"_s)}, // all shown mods ARE active on the server -> green check, no toggle
        {"choose"_s, ScriptedUIDataElems::Callback{[&modIndex, &clicked, i] {
          modIndex = i; clicked = true; return true; // selection only -- picks which one to describe
        }}}
      }};
      if (i == modIndex)
        button.elems["selected"] = TString("xyz"_s);
      modList.push_back(std::move(button));
    }
    auto data = ScriptedUIDataElems::Record{{
      {"local", std::move(modList)} // no "subscribed"/"online"/"create_new" -> nothing to add or change
    }};
    if (!activeMods.empty()) {
      const auto& name = activeMods[modIndex];
      auto details = getLocalModDetails(name);
      // selected_mod carries ONLY name/author/description -- no action callbacks -> the button row is empty.
      data.elems["selected_mod"] = ScriptedUIDataElems::Record{{
        {"name"_s, TString(name)},
        {"author"_s, TString(details ? details->author : ""_s)},
        {"description"_s, TString(details ? details->description : ""_s)}
      }};
    }
    view->scriptedUI("mods", data, uiState);
    if (!clicked)
      break; // exit button pressed (no selection change) -> close
  }
}

void MainLoop::playMenuMusic() {
  jukebox->setCurrentVolume(options->getIntValue(OptionId::MUSIC));
  jukebox->setType(MusicType::MAIN, true);
}

void MainLoop::considerGameEventsPrompt() {
  if (options->getIntValue(OptionId::GAME_EVENTS) == 1) {
    if (view->yesOrNoPrompt(TStringId("GAME_STATS_QUESTION")))
      options->setValue(OptionId::GAME_EVENTS, 2);
    else
      options->setValue(OptionId::GAME_EVENTS, 0);
  }
}

void MainLoop::considerFreeVersionText(bool tilesPresent) {
  if (!tilesPresent)
    view->presentText(none, TString("You are playing a version of KeeperRL without graphical tiles. "
        "Besides lack of graphics and music, this "
        "is the same exact game as the full version. If you'd like to buy the full version, "
        "please visit keeperrl.com.\n \nYou can also get it by donating to any wildlife charity. "
        "More information on the website."_s));
}

DirectoryPath MainLoop::getVanillaDir() const {
  return dataFreePath.subdirectory("game_config");
}

GameConfig MainLoop::getVanillaConfig() const {
  return GameConfig({getVanillaDir()});
}

void MainLoop::genZLevels(const string& keeperType) {
  auto factory = createContentFactory(false);
  vector<ZLevelInfo> allZLevels;
  for (auto& keeper : factory.keeperCreatures)
    if (keeper.first == keeperType)
      for (auto& g : keeper.second.zLevelGroups)
        allZLevels.append(factory.zLevels.at(g));
  for (int i : Range(1, 30)) {
    auto level = *chooseZLevel(Random, allZLevels, i);
    std::cout << i << " ";
    level.visit(
      [&](const FullZLevel& l) {
        std::cout << "Full z-level ";
        if (l.enemy)
          std::cout << " " << l.enemy->data() << " (" << l.attackChance << " attack chance)";
      },
      [](const WaterZLevel& l) {
        std::cout << "Water z-level (" << l.waterType.data() << ")"  ;
      },
      [&](const EnemyZLevel& l) {
        std::cout << "Enemy z-level " << l.enemy.data() << " (" << l.attackChance << " attack chance)";
      }
    );
    std::cout << "\n";
  }
  double totalAttacks = 0;
  double totalAttacks10 = 0;
  const int numTries = 5000;
  for (int _ : Range(numTries)) {
    double numAttacks = 0;
    for (int i : Range(1, 30)) {
      auto level = *chooseZLevel(Random, allZLevels, i);
      level.visit(
        [&](const FullZLevel& l) {
          if (l.enemy)
            numAttacks += l.attackChance;
        },
        [](const WaterZLevel& l) {
        },
        [&](const EnemyZLevel& l) {
          numAttacks += l.attackChance;
        }
      );
      if (i == 10)
        totalAttacks10 += numAttacks / numTries;
    }
    totalAttacks += numAttacks / numTries;
  }
  std::cout << "\n" << totalAttacks10 << " attacks in first 10 levels\n";
  std::cout << totalAttacks << " attacks in first 30 levels\n";
}

static void printWorldSummary(const char* label, const Table<Campaign::SiteInfo>& sites) {
  auto b = sites.getBounds();
  int villains = 0, keepers = 0, biomes = 0, blocked = 0;
  for (auto v : b) {
    auto& s = sites[v];
    if (s.biome)
      ++biomes;
    if (s.blocked)
      ++blocked;
    if (s.dweller)
      s.dweller->match(
          [&](const Campaign::VillainInfo&) { ++villains; },
          [&](const Campaign::RetiredInfo&) { ++villains; },
          [&](const Campaign::KeeperInfo&) { ++keepers; });
  }
  std::cout << label << " size=" << b.width() << "x" << b.height()
            << " biomeTiles=" << biomes << " blocked=" << blocked
            << " villains=" << villains << " keepers=" << keepers << "\n";
  std::cout.flush();
}

void MainLoop::genServerWorld(const string& outFile, optional<int> fixedSeed,
    optional<string> worldMapName) {
  // RAR: auto-activate EVERY mod present in the server's mods folder. The admin just drops a mod directory
  // in and re-runs --rar_gen_world -- the world is generated WITH it (createContentFactory reads
  // CURRENT_MOD2) and it's published for clients to auto-sync (publishServerMods reads CURRENT_MOD2 too).
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath()); // mods/load_order.txt decides the order
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  std::cout << "[rar-gen] auto-activated " << folderMods.size() << " mod(s) from the mods folder";
  for (auto& m : folderMods) std::cout << " '" << m << "'";
  std::cout << "\n"; std::cout.flush();
  auto factory = createContentFactory(false);
  RandomGen random;
  // TERRAIN seed: given one (--rar_gen_seed, e.g. copied out of --gen_preview) the map is reproducible --
  // same seed + same size + same content = the same world, every time. Otherwise roll a fresh one. Villain
  // PLACEMENT still uses its own RNG below, so two runs at the same seed share terrain, not villain spots.
  int terrainSeed = fixedSeed.value_or((int) time(nullptr));
  random.init((int) time(nullptr));
  std::cout << "[rar-gen] terrain seed = " << terrainSeed
      << (fixedSeed ? "  (fixed via --rar_gen_seed)" : "  (random -- pass it to --rar_gen_seed to rebuild this map)")
      << "\n";
  std::cout.flush();
  // Also drop it next to the world so the seed is recoverable long after the gen log is gone.
  { std::ofstream seedOut("rar_gen_seed.txt", std::ios::trunc); seedOut << terrainSeed << "\n"; }
  // WHICH world map to build. This used to be hardwired to worldMaps[0] (vanilla's "world_map"), so a mod
  // could add an entry to world_maps.txt and it was simply unreachable -- the only way to iterate on a world
  // layout was to edit vanilla's random_layouts.txt in place. Mod content merges fine; nothing selected it.
  // --rar_gen_worldmap takes either a world_maps.txt layout id or any layout name in random_layouts.txt.
  RandomLayoutId worldMapId = factory.worldMaps[0].layout;
  if (worldMapName) {
    RandomLayoutId wanted(worldMapName->data());
    bool inWorldMaps = false;
    for (auto& wm : factory.worldMaps)
      if (wm.layout == wanted)
        inWorldMaps = true;
    // Accept a raw random_layouts.txt name too: while iterating on a layout it is a nuisance to have to add
    // it to world_maps.txt first, and generation only ever needs the layout itself.
    if (!inWorldMaps && !factory.randomLayouts.count(wanted)) {
      std::cout << "[rar-gen] unknown world map '" << *worldMapName << "'.\n  world_maps.txt offers:";
      for (auto& wm : factory.worldMaps)
        std::cout << " '" << wm.layout.data() << "'";
      std::cout << "\n  (or any layout name from random_layouts.txt, mods included)\n";
      std::cout.flush();
      return;
    }
    worldMapId = wanted;
  }
  std::cout << "[rar-gen] world map = '" << worldMapId.data() << "'"
      << (worldMapName ? "  (--rar_gen_worldmap)" : "  (default: first entry in world_maps.txt)") << "\n";
  std::cout.flush();
  vector<VillainGroup> groups;
  for (auto& elem : factory.villains)
    groups.push_back(elem.first);
  auto sites = CampaignBuilder::generateServerWorldSites(random, &factory, options, factory.villains, groups,
      terrainSeed, worldMapId);
  printWorldSummary("[rar-gen] generated", sites);
  std::stringstream ss;
  {
    OutputArchive ar(ss);
    ar << sites;
  }
  string bytes = ss.str();
  ofstream out(outFile, std::ios::binary);
  out.write(bytes.data(), bytes.size());
  out.close();
  std::cout << "[rar-gen] wrote " << bytes.size() << " bytes to " << outFile << "\n";
  std::cout.flush();
  // Phase A: pre-generate every villain's map ONCE here (server-side) and store it keyed by world
  // position, so clients never generate villains -- they download the canonical map on demand when they
  // travel there. Alignment is fixed to EVIL (dark-keeper neighbours) and gen difficulty is the base (0);
  // per-player combat difficulty is applied client-side after download via getBaseLevelIncrease.
  DirectoryPath villainDir("rar_villains");
  villainDir.createIfDoesntExist();
  EnemyFactory villainEnemies(Random, factory.getCreatures().getNameGenerator(), factory.enemies,
      factory.buildingInfo, {});
  // The --rar_gen_world path builds MainLoop with a null sokobanInput; some villains have a sokoban puzzle
  // level, so construct a real one here (same files main.cpp uses) or their generation null-derefs.
  SokobanInput villainSokoban(dataFreePath.file("sokoban_input.txt"), userPath.file("sokoban_state.txt"));
  ModelBuilder villainBuilder(nullptr, random, options, &villainSokoban, &factory, std::move(villainEnemies));
  auto& campaignInfo = factory.campaignInfo;
  // Build one villain map as an lzma blob (same transport clients load).
  auto genVillainBlob = [&] (EnemyId enemyId, VillainType type, BiomeId biome) -> string {
    PModel model = villainBuilder.campaignSiteModel(enemyId, type, TribeAlignment::EVIL, biome, 0);
    for (Level* l : model->getLevels())
      l->clearSectors(); // recomputable on load -> keep the blob small (same trick as keeper dungeons)
    SavedGameInfo info;
    info.name = enemyId.data();
    info.progressCount = 1;
    info.retiredEnemyInfo = SavedGameInfo::RetiredEnemyInfo{enemyId, type};
    return rarLzmaCompress(serializeModelRaw(model.giveMeSharedPointer(), info, &factory));
  };
  // (a) The ACTIVE villains placed on the map, keyed by "x_y". Also record a server manifest (rar_villains.txt:
  // "x_y  TIER  enemyId") so the content-free --rar_server knows the roster for alive/dead + respawn tracking.
  // A VILLAIN IS ITS FILE: each gets a RANDOM id (never the map position), so a villain that later respawns
  // on a tile some long-dead villain once held is a genuinely different entity. Manifest row:
  // id  tier  enemyId  biome  pos  alive  defeatTime   (empty pos = unplaced spare; see the spares below).
  auto newId = [ids = std::set<string>()] () mutable {
    static const char* CH = "abcdefghijklmnopqrstuvwxyz0123456789";
    string s;
    do { s.clear(); for (int i = 0; i < 12; ++i) s += CH[Random.get(36)]; } while (!ids.insert(s).second);
    return s;
  };
  ofstream manifest("rar_villains.txt");
  int nVillains = 0;
  for (Vec2 v : sites.getBounds())
    if (auto villain = sites[v].getVillain()) {
      if (!sites[v].biome)
        continue;
      string blob = genVillainBlob(villain->enemyId, villain->type, *sites[v].biome);
      string pos = toString(v.x) + "_" + toString(v.y);
      string id = newId();
      ofstream out2(villainDir.file(id + ".dat").getPath(), std::ios::binary);
      out2.write(blob.data(), blob.size());
      manifest << id << "\t" << EnumInfo<VillainType>::getString(villain->type) << "\t"
               << villain->enemyId.data() << "\t" << sites[v].biome->data() << "\t" << pos << "\t1\t0\n";
      ++nVillains;
      std::cout << "[rar-gen] villain " << id << " '" << villain->enemyId.data() << "' at " << pos << " "
                << blob.size() << "b\n"; std::cout.flush();
    }
  std::cout << "[rar-gen] pre-generated " << nVillains << " active villain maps\n"; std::cout.flush();
  // (a2) Candidate RESPAWN SLOTS: every empty land tile + its biome, so respawns can land on RANDOM new
  // tiles (not just the original villain positions). rar_villain_slots.txt: "x,y  BIOME".
  // EVERY usable land tile + its biome -- including tiles a villain currently sits on. The server decides what
  // is FREE from its own occupancy index, and needs the biome of an OCCUPIED tile too so it can tell whether a
  // placed villain still matches the ground under it (and move it if the world changed).
  ofstream slotsManifest("rar_villain_slots.txt");
  vector<BiomeId> biomes; // distinct biomes with usable land -> which biomes the spares must cover
  int nSlots = 0;
  for (Vec2 v : sites.getBounds())
    if (sites[v].biome && !sites[v].blocked && (sites[v].isEmpty() || sites[v].getVillain())) {
      slotsManifest << v.x << "_" << v.y << "\t" << sites[v].biome->data() << "\n";
      if (std::find(biomes.begin(), biomes.end(), *sites[v].biome) == biomes.end())
        biomes.push_back(*sites[v].biome);
      ++nSlots;
    }
  slotsManifest.close();
  std::cout << "[rar-gen] " << nSlots << " empty respawn slots across " << biomes.size() << " biomes\n";
  std::cout.flush();
  // (b) The RESPAWN POOL. poolXxxVillainsGenerated is now the spare count PER DISTINCT VILLAIN TYPE (enemyId),
  // NOT per tier/biome: if the MAIN tier has 4 different villains (red dragon, green dragon, lizardmen,
  // knights) then poolMainVillainsGenerated=10 bakes 10 spares of EACH -> a deep, even pool per villain so
  // respawns don't collapse onto whichever type won the old random.choose. Still biome-matched (a respawn on
  // a grass tile gets a grassland interior). Blob files: rar_villain_pool/<TIER>_<BIOME>_<i>.dat; inventory
  // rar_villain_pool.txt: "TIER  BIOME  index  enemyId" (unchanged -- the server needs no changes).
  // For a (tier, biome) spare we must reuse an enemyId that ALREADY generates on that biome (a baked villain
  // of that tier+biome) -- villains can't be built in arbitrary biomes (e.g. WHITE_DRAGON fails in some).
  auto tierBiomePositions = [&] (VillainType t, BiomeId b) {
    vector<Vec2> res;
    for (Vec2 v : sites.getBounds())
      if (auto vi = sites[v].getVillain())
        if (vi->type == t && sites[v].biome && *sites[v].biome == b)
          res.push_back(v);
    return res;
  };
  int nPool = 0;
  // ALLY is a respawning tier like any other: on the SHARED world an ally of one keeper is an enemy of
  // another and can be wiped out, so it needs spares to come back from -- otherwise a destroyed ally was gone
  // from the world permanently.
  // These two are CONSTANTS, not campaign_info.txt fields, on purpose: CampaignInfo goes into every save, so a
  // new field there breaks every existing save (see the note in campaign_info.h). They only seed the generated
  // rar_villain_config.txt -- ALLY / POOL_ALLY are read live from that file, so tune them THERE, no regen.
  const int RAR_MIN_ALLIES_ALIVE_DEFAULT = 4;
  const int RAR_POOL_ALLY_DEFAULT = 10;
  for (auto tier : {make_pair(VillainType::MAIN, campaignInfo.poolMainVillainsGenerated),
                    make_pair(VillainType::LESSER, campaignInfo.poolLesserVillainsGenerated),
                    make_pair(VillainType::MINOR, campaignInfo.poolMinorVillainsGenerated),
                    make_pair(VillainType::ALLY, RAR_POOL_ALLY_DEFAULT)}) {
    string tierStr = EnumInfo<VillainType>::getString(tier.first);
    int poolPerVillain = tier.second; // poolXxxVillainsGenerated = spares baked PER distinct villain type
    int tierSpares = 0;
    int tierTypes = 0;
    for (BiomeId biome : biomes) {
      auto positions = tierBiomePositions(tier.first, biome);
      if (positions.empty())
        continue; // no villain of this tier lives on this biome -> no matched spares (respawn falls back)
      string biomeStr = biome.data();
      // Distinct villain TYPES (enemyId) of this tier living on this biome.
      vector<string> enemyIds;
      for (Vec2 v : positions) {
        string e = sites[v].getVillain()->enemyId.data();
        if (std::find(enemyIds.begin(), enemyIds.end(), e) == enemyIds.end())
          enemyIds.push_back(e);
      }
      tierTypes += enemyIds.size();
      // poolPerVillain copies of EACH type, then shuffled: the server drains a (tier,biome)'s spares in
      // manifest order on respawn, so a shuffled mix hands out varied types instead of all of one first.
      vector<string> spares;
      for (auto& e : enemyIds)
        for (int i : Range(poolPerVillain))
          spares.push_back(e);
      random.shuffle(spares.begin(), spares.end());
      for (int idx : Range((int) spares.size())) {
        const string& enemyId = spares[idx];
        string blob = genVillainBlob(EnemyId(enemyId.c_str()), tier.first, biome);
        // Spares are just UNPLACED villains in the SAME folder (empty pos) -- no separate pool, no index scheme.
        string id = newId();
        ofstream pout(villainDir.file(id + ".dat").getPath(), std::ios::binary);
        pout.write(blob.data(), blob.size());
        manifest << id << "\t" << tierStr << "\t" << enemyId << "\t" << biomeStr << "\t\t1\t0\n";
        ++nPool; ++tierSpares;
      }
    }
    std::cout << "[rar-gen] pool " << tierStr << ": " << tierSpares << " biome-matched spares ("
        << poolPerVillain << " per villain type x " << tierTypes << " type/biome combos)\n"; std::cout.flush();
  }
  manifest.close(); // ONE manifest holds both the placed villains and the unplaced spares
  // (c) The respawn thresholds + pool depths, so the server knows when to respawn each tier AND (when run as a
  // full/content-loaded server) how deep to keep each villain type's spare pool via live replenish. The admin
  // can edit POOL_* here + restart to deepen the pool WITHOUT a world regen.
  ofstream cfg("rar_villain_config.txt");
  cfg << "MAIN\t" << campaignInfo.minMainVillainsAlive << "\n"
      << "LESSER\t" << campaignInfo.minLesserVillainsAlive << "\n"
      << "MINOR\t" << campaignInfo.minMinorVillainsAlive << "\n"
      << "ALLY\t" << RAR_MIN_ALLIES_ALIVE_DEFAULT << "\n"
      << "POOL_MAIN\t" << campaignInfo.poolMainVillainsGenerated << "\n"
      << "POOL_LESSER\t" << campaignInfo.poolLesserVillainsGenerated << "\n"
      << "POOL_MINOR\t" << campaignInfo.poolMinorVillainsGenerated << "\n"
      << "POOL_ALLY\t" << RAR_POOL_ALLY_DEFAULT << "\n";
  cfg.close();
  std::cout << "[rar-gen] pre-generated " << nPool << " pool spares + config\n"; std::cout.flush();
  // The world was generated with the currently-active mods; publish them so clients auto-sync.
  publishServerMods();
}

void MainLoop::runRarServerFull(int port) {
  // "Full" server: load the game's own content ONCE and hand the server a villain-map generator, so it can
  // LIVE-replenish its respawn pool (rar_villain_pool/) in the background instead of relying on a fixed pool
  // baked at world-gen. Everything the generator needs (factory + ModelBuilder) is built here and kept alive
  // for the whole run -- runRarServer() blocks below, and the replenish thread calls the captured generator
  // while this frame is live. If content fails to load we fall back to the legacy content-free server.
  try {
    vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath()); // mods/load_order.txt decides the order
    options->setValue(OptionId::CURRENT_MOD2, folderMods); // same mods the server (re)publishes -> content parity
    // Build the factory INTO the shared_ptr first, then take pointers into its stable address -- so nothing is
    // moved after EnemyFactory/ModelBuilder capture references into it. shared_ptrs so the generator lambda
    // (called from the replenish thread) keeps them all alive for the whole run.
    auto factoryPtr = std::make_shared<ContentFactory>(createContentFactory(false));
    auto& factory = *factoryPtr;
    auto random = std::make_shared<RandomGen>();
    random->init((int) time(nullptr));
    EnemyFactory villainEnemies(Random, factory.getCreatures().getNameGenerator(), factory.enemies,
        factory.buildingInfo, {});
    // Some villains have a sokoban puzzle level; construct a real input or their generation null-derefs.
    auto villainSokoban = std::make_shared<SokobanInput>(dataFreePath.file("sokoban_input.txt"),
        userPath.file("sokoban_state.txt"));
    auto villainBuilder = std::make_shared<ModelBuilder>(nullptr, *random, options, villainSokoban.get(),
        factoryPtr.get(), std::move(villainEnemies));
    auto genMutex = std::make_shared<std::mutex>();
    // `this` is captured for the member serializeModelRaw(); the MainLoop outlives the server (runRarServer
    // blocks below in this same call), so it stays valid for every replenish-thread invocation.
    RarVillainGen gen = [factoryPtr, villainBuilder, random, villainSokoban, genMutex, this]
        (const std::string& tierStr, const std::string& enemyIdStr, const std::string& biomeStr) -> std::string {
      // One replenish thread calls this, but guard anyway: ModelBuilder + RandomGen are not re-entrant.
      std::lock_guard<std::mutex> lk(*genMutex);
      optional<VillainType> type;
      for (auto t : ENUM_ALL(VillainType))
        if (EnumInfo<VillainType>::getString(t) == tierStr) { type = t; break; }
      if (!type)
        return "";
      try {
        PModel model = villainBuilder->campaignSiteModel(EnemyId(enemyIdStr.c_str()), *type,
            TribeAlignment::EVIL, BiomeId(biomeStr.c_str()), 0);
        for (Level* l : model->getLevels())
          l->clearSectors();
        SavedGameInfo info;
        info.name = enemyIdStr;
        info.progressCount = 1;
        info.retiredEnemyInfo = SavedGameInfo::RetiredEnemyInfo{ EnemyId(enemyIdStr.c_str()), *type };
        return rarLzmaCompress(serializeModelRaw(model.giveMeSharedPointer(), info, factoryPtr.get()));
      } catch (...) {
        return ""; // a villain that can't build on this biome just gets skipped this cycle
      }
    };
    // Tell the server which (tier, enemyId, biome) the WORLD actually uses, so it can replenish the pool
    // against THAT rather than against the spares it already has. The server can't work this out itself: it
    // only serves rar_campaign.dat verbatim and never deserializes it, so it has no idea which biome a faction
    // lives on -- and the generator needs the biome. Deriving it here means the pool no longer has to be baked
    // at world-gen: a newly configured tier (ALLY) or POOL_* depth just fills in on the next start, no regen.
    // NOTE: no chdir has happened yet (runRarServer does its own), so the world is still under server/.
    std::vector<RarVillainCombo> combos; // std:: on purpose -- KeeperRL's `vector` is its own wrapper type
    try {
      Table<Campaign::SiteInfo> worldSites;
      std::ifstream w("server/rar_campaign.dat", std::ios::binary);
      if (w) {
        std::string bytes((std::istreambuf_iterator<char>(w)), std::istreambuf_iterator<char>());
        std::stringstream ss(bytes); InputArchive ar(ss); ar >> worldSites;
        for (Vec2 v : worldSites.getBounds()) {
          auto& site = worldSites[v];
          auto villain = site.getVillain();
          // isConquerableSite = the tiers that can be wiped out, i.e. exactly the ones that need spares to come
          // back from. Same predicate the defeat-report and writeback use, so the pool can't drift from them.
          if (!villain || !site.biome || !isConquerableSite(villain->type))
            continue;
          RarVillainCombo c{ EnumInfo<VillainType>::getString(villain->type), villain->enemyId.data(),
              site.biome->data() };
          bool have = false;
          for (auto& e : combos)
            if (e.tier == c.tier && e.enemyId == c.enemyId && e.biome == c.biome) { have = true; break; }
          if (!have)
            combos.push_back(std::move(c));
        }
      } else
        std::cout << "RAR server: no server/rar_campaign.dat -- pool replenish disabled until a world exists\n";
    } catch (...) {
      combos.clear();
      std::cerr << "RAR server: couldn't read the world's villain combos -> no pool replenish\n";
    }
    std::cout << "RAR server: loaded content (" << folderMods.size() << " mod(s), " << combos.size()
        << " villain combo(s) from the world) -> villain pool will live-replenish\n"; std::cout.flush();
    // blocks; keeps factory/builder alive via the captured shared_ptrs
    runRarServer(port, std::move(gen), std::move(combos));
  } catch (...) {
    std::cerr << "RAR server: content load failed -> running content-free (no live pool replenish)\n";
    runRarServer(port);
  }
}

string MainLoop::bundleMod(const string& modName) {
  // Delegate to the shared content-free bundler (rar_mods.cpp) so the client's hash and the server's
  // published hash are computed over IDENTICAL bytes.
  return rarBundleModDir(modsDir.subdirectory(modName).getPath());
}

void MainLoop::unbundleMod(const string& modName, const string& bytes) {
  modsDir.createIfDoesntExist(); // the client may have no mods dir yet
  auto dir = modsDir.subdirectory(modName);
  dir.removeRecursively(); // clean install so stale files can't linger
  dir.createIfDoesntExist();
  vector<pair<string, string>> files;
  {
    std::stringstream ss(bytes);
    InputArchive ar(ss);
    ar >> files;
  }
  for (auto& elem : files) {
    auto parts = split(elem.first, {'/'});
    DirectoryPath cur = dir;
    for (int i = 0; i + 1 < (int) parts.size(); ++i) {
      cur = cur.subdirectory(parts[i]);
      cur.createIfDoesntExist();
    }
    ofstream fout(cur.file(parts.back()).getPath(), std::ios::binary);
    fout.write(elem.second.data(), elem.second.size());
  }
}

void MainLoop::publishServerMods() {
  // Publish every mod in the mods folder into the current dir (during gen that's server/). Same shared code
  // the --rar_server runs at startup, so gen and the live server produce identical manifests either way.
  int n = rarPublishMods(modsDir.getPath(), ".");
  std::cout << "[rar-gen] published " << n << " mod(s) to rar_mods/ + rar_mods.txt\n";
  std::cout.flush();
}

// Diagnose a keeper that the load menu refuses: this runs EXACTLY what loadServerGame() does -- fetch the
// blob, lzma-decode it, and deserialize it as a full PGame -- but reports which step failed and why, instead
// of the single "Couldn't download your keep from the server" the menu shows for all three failure modes.
void MainLoop::rarKeeperLoadTest(const string& gameId) {
  string blob;
  if (!rarFetchDungeon(gameId, blob)) { std::cout << "[keeper] FETCH FAILED (no blob on server?)" << "\n"; return; }
  std::cout << "[keeper] fetched lzma=" << blob.size() << "\n";
  string raw = rarLzmaDecompress(blob);
  if (raw.empty()) { std::cout << "[keeper] LZMA DECODE FAILED\n"; return; }
  std::cout << "[keeper] decompressed raw=" << raw.size() << "\n";
  FilePath t = userPath.file("rar_keepertest" + getSaveSuffix(GameSaveType::KEEPER));
  { ogzstream out(t.getPath()); out.write(raw.data(), raw.size()); }
  // Deliberately NOT going through loadFromFile(): it swallows the exception, and the exception text is the
  // whole point here (a content id missing because a mod was removed reads very differently from a truncated
  // stream or a save-version mismatch).
  try {
    PGame game;
    CompressedInput input(t.getPath());
    string discard;
    SavedGameInfo discard2;
    int version;
    input.getArchive() >> version >> discard >> discard2;
    std::cout << "[keeper] save_version=" << version << "\n";
    input.getArchive() >> game;
    std::cout << "[keeper] LOADED OK" << "\n";
    // friendlyTribes is a BITSET indexed by the interned TribeId, serialized as raw bits. If the set of
    // tribes (or their load order) changed since this save was written, every bit now refers to a
    // DIFFERENT tribe -- including the tribe's own bit, which is what makes it hostile to itself.
    // Compare what the SAVE thinks against a tribe map generated from the CURRENT content.
    if (game) {
      for (auto& name : {"UNDEAD_KEEPER", "DARK_KEEPER", "BANDIT", "MONSTER"}) {
        TribeId id(name);
        auto* t = game->getTribe(id);
        std::cout << "[tribe] " << name << " internalId=" << (int) id.getInternalId()
                  << " selfFriendly=" << (t ? (t->isEnemy(t) ? "NO(self-hostile)" : "yes") : "??")
                  << "\n";
      }
    }
    // Who is in there, and on which tribe. A colony whose own minions are split across tribes will fight
    // itself the moment it is loaded, so this is the first thing to check when "they killed each other".
    if (game && game->getMainModel()) {
      std::map<std::string, int> byTribe;
      for (Creature* c : game->getMainModel()->getAllCreatures())
        ++byTribe[std::string(c->getTribeId().data())];
      int dead = (int) game->getMainModel()->getDeadCreatures().size();
      std::cout << "[keeper] alive=" << game->getMainModel()->getAllCreatures().size()
                << " dead=" << dead << "\n";
      for (auto& e : byTribe)
        std::cout << "[keeper]   tribe " << e.first << " = " << e.second << "\n";
    }
  } catch (std::exception& e) {
    std::cout << "[keeper] LOAD FAILED: " << e.what() << "\n";
  } catch (...) {
    std::cout << "[keeper] LOAD FAILED (unknown exception)\n";
  }
  t.erase();
}

void MainLoop::rarInvasionLoadTest(const string& gameId) {
  // The heart of the on-demand invasion: fetch a cached dungeon, decode the transport (lzma ->
  // raw -> re-gzip), and load it into a live Model via the proven retired-dungeon loader.
  string blob;
  if (!rarFetchDungeon(gameId, blob)) { std::cout << "[inv] fetch FAILED (no dungeon?)\n"; return; }
  std::cout << "[inv] fetched lzma blob=" << blob.size() << "\n";
  string raw = rarLzmaDecompress(blob);
  std::cout << "[inv] decompressed raw=" << raw.size() << "\n";
  if (raw.empty()) { std::cout << "[inv] lzma decode FAILED\n"; return; }
  FilePath tmp = userPath.file("rar_invtest" + getSaveSuffix(GameSaveType::RETIRED_SITE));
  { ogzstream out(tmp.getPath()); out.write(raw.data(), raw.size()); }
  auto contentFactory = createContentFactory(false); // loadRetiredModelFromFile needs content registered
  auto savedInfo = loadSavedGameInfo(tmp);
  auto info = loadRetiredModelFromFile(tmp);
  tmp.erase();
  if (info && info->model && savedInfo) {
    std::cout << "[inv] MODEL LOADED OK -- alive=" << info->model->getAllCreatures().size()
              << " dead=" << info->model->getDeadCreatures().size() << "\n";
    // Size experiment: serialize as-is (with the invader's fully-computed pathfinding sectors), then
    // clear the sectors (recomputable on load) and serialize again -- to see how much of the writeback
    // bloat is just sectors, and whether the stripped model is back down near base-.sit size.
    string ser1 = serializeModelRaw(info->model, *savedInfo, &contentFactory);
    string lz1 = rarLzmaCompress(ser1);
    std::cout << "[size] WITH sectors:     raw=" << ser1.size() << " lzma=" << lz1.size() << "\n";
    for (Level* l : info->model->getLevels())
      l->clearSectors();
    string ser2 = serializeModelRaw(info->model, *savedInfo, &contentFactory);
    string lz2 = rarLzmaCompress(ser2);
    std::cout << "[size] STRIPPED sectors: raw=" << ser2.size() << " lzma=" << lz2.size() << "\n";
  } else
    std::cout << "[inv] model load FAILED\n";
  std::cout.flush();
}

// RAR lockstep feasibility spike (--rar_lockstep_selftest <save.kep> [turns]).
// Deterministic lockstep is the ONLY model that gives true real-time PvP (both players run the identical sim
// and exchange only inputs). It works IFF the simulation is bit-deterministic: same start state + same seed +
// same inputs => bit-identical result on every machine. This harness measures exactly that, with NO network
// and NO second player: it loads ONE save twice, seeds the global RNG identically, steps each copy forward the
// same number of turns with no player input, and fingerprints the world state after every turn. If the two
// fingerprint streams stay identical, the AI-level sim is deterministic and lockstep is viable; if they
// diverge, it reports the first turn + the first creature that differs, which is where the audit starts.
// Scope note: this drives Model::update directly (the per-creature AI/tick path) rather than Game::update,
// because Game::update needs a live View. That covers the highest-risk nondeterminism (AI, RNG draw order,
// pathfinding, combat); game-level tick determinism is a later, separate check.
namespace {
struct CreatureSnap { long long id; int x, y; long long levelId; long long healthMilli; };
// Deterministic world fingerprint: sort creatures by their stable generic id, then FNV-1a over
// (id, x, y, level, health) of each -- position/level/health are what a desync first shows up in.
static void snapshotModel(Model* m, vector<CreatureSnap>& out) {
  out.clear();
  for (Creature* c : m->getAllCreatures()) {
    Position p = c->getPosition();
    CreatureSnap s;
    s.id = c->getUniqueId().getGenericId();
    if (Level* l = p.getLevel()) {
      Vec2 co = p.getCoord();
      s.x = co.x; s.y = co.y;
      s.levelId = (long long) l->getUniqueId();
    } else { s.x = s.y = -1; s.levelId = -1; } // creature in transit / off-level
    s.healthMilli = (long long) llround(c->getBody().getHealth() * 1000.0);
    out.push_back(s);
  }
  std::sort(out.begin(), out.end(), [](const CreatureSnap& a, const CreatureSnap& b){ return a.id < b.id; });
}
// RAR lockstep: optionally drive PRODUCTION so the determinism test exercises crafting/workshop paths (recipe
// choice, product spawn, RNG) that idle base-play doesn't hit. Enabled by env RAR_LOCKSTEP_PROD. Deterministic:
// workshops.types is a std::map (ordered), and we queue fixed recipe indices, so both processes queue identically.
static void rarLockstepSetupProduction(Game* game) {
  auto col = game->getPlayerCollective();
  if (!col) return;
  auto& workshops = col->getWorkshops();
  int queued = 0;
  for (auto& elem : workshops.types) {
    int n = (int) elem.second.getOptions().size();
    for (int i = 0; i < n && i < 4; ++i) { // queue the first few recipes of each workshop
      elem.second.queue(col, i, 0);
      ++queued;
    }
  }
  std::cout << "[lockstep] production mode: queued " << queued << " workshop item(s)\n"; std::cout.flush();
}

static uint64_t hashSnap(const vector<CreatureSnap>& snap) {
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&](uint64_t v){ h ^= v; h *= 1099511628211ULL; };
  for (auto& s : snap) {
    mix((uint64_t) s.id); mix((uint64_t)(uint32_t) s.x); mix((uint64_t)(uint32_t) s.y);
    mix((uint64_t) s.levelId); mix((uint64_t) s.healthMilli);
  }
  mix((uint64_t) snap.size());
  return h;
}
} // namespace

void MainLoop::rarLockstepSelfTest(const string& saveFile, int numTurns) {
  const int SEED = 1234567;
  if (numTurns <= 0) numTurns = 2000;
  // Register content (interned ContentIds the save's objects resolve against), matching the server's mods so
  // the loaded factory lines up with the save -- same setup repairVillains/genServerWorld use.
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath());
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  (void) factory;
  // Run one full simulation from a fresh load of the save. Returns the per-turn fingerprint stream. If
  // detailTurn >= 0, also captures the full sorted creature snapshot at that turn into *detailOut (used to
  // pinpoint the first divergence on a second pass).
  auto simulate = [&](const char* label, vector<CreatureSnap>* detailOut, int detailTurn,
      vector<long long>* drawsOut, int overrideTurns, vector<void*>* traceOut) -> vector<uint64_t> {
    int turns = overrideTurns > 0 ? overrideTurns : numTurns;
    vector<uint64_t> stream;
    // Per-run heap churn: in-process, run B tends to reuse run A's freed memory -> similar addresses -> any
    // address-ordered nondeterminism COINCIDENTALLY matches between A and B (false negative). A fresh process
    // (real lockstep) gets different addresses and diverges. Leaking a different-sized block before each run
    // shifts where this run's game allocates, making the in-process twin test as sensitive as cross-process --
    // so a single trace run reproduces the bug. Intentional leak; this is a dev-only self-test.
    // Modest per-run heap churn so in-process runs A/B don't sit at identical addresses (catches heap-layout
    // nondeterminism). NOTE: code/vtable-address nondeterminism only shows ACROSS processes -- use the cross-
    // process dump/symtrace for those. Intentional dev-only leak.
    static int churnCounter = 0;
    volatile char* churn = new char[(++churnCounter) * 256 * 1024];
    churn[0] = 1;
    // Seed BEFORE load+init, not after: initializeModels (collective warm-up) consumes the global RNG, so if we
    // seeded later, run A's init would start from a fresh RNG while run B's would start from whatever run A's
    // whole simulation left -> different pre-step state -> false "divergence". Seeding first makes load, init AND
    // stepping all run from the identical RNG state on every run, which is the only fair determinism test.
    Random.init(SEED);
    auto tLoad0 = std::chrono::steady_clock::now();
    auto loaded = loadFromFile<PGame>(FilePath::fromFullPath(saveFile));
    if (!loaded) { std::cout << "[lockstep] FAILED to load save: " << saveFile << "\n"; std::cout.flush(); return stream; }
    PGame game = std::move(*loaded);
    // Wire the game up like the real load path so the sim's tick/AI can reach Game+View (sounds, events, etc.)
    // without null-derefing. A DummyView no-ops all rendering/audio; highscores/fileSharing/achievements are
    // unused by pure stepping so stay null. initializeModels sets up sectors/collectives (same as playGame).
    Clock clock(true);
    DummyView dummyView(&clock);
    Encyclopedia encyclopedia(game->getContentFactory());
    game->initialize(options, nullptr, &dummyView, nullptr, &encyclopedia, unlocks, nullptr);
    ProgressMeter meter(1);
    game->initializeModels(meter);
    if (std::getenv("RAR_LOCKSTEP_PROD")) rarLockstepSetupProduction(game.get());
    Model* model = game->getMainModel().get();
    auto msSince = [](std::chrono::steady_clock::time_point t0) {
      return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    std::cout << "[lockstep] sim " << label << ": loaded+init in " << msSince(tLoad0) << "ms, "
              << model->getAllCreatures().size() << " creatures, stepping " << turns << " turns"
              << (traceOut ? " (tracing RNG draws)" : "") << "...\n";
    std::cout.flush();
    stream.reserve(turns);
    double t = model->getLocalTimeDouble();
    vector<CreatureSnap> snap;
    long long guardTrips = 0;
    if (traceOut) Random.startDrawTrace(); // record each draw's call stack for this run
    auto tStep0 = std::chrono::steady_clock::now();
    for (int i = 0; i < turns; ++i) {
      t += 1.0;
      // REAL drain: process every creature due before t (Game::updateModel semantics). The real loop breaks on a
      // WALLCLOCK deadline, but that's nondeterministic (machine-speed-dependent) so it can't be used for
      // lockstep -- we bound the drain with a fixed DETERMINISTIC move cap instead. A legit turn drains at most a
      // few hundred moves; hitting the cap means a creature is taking zero-time actions (would infinite-loop with
      // no bound). The cap is identical on both runs, so it stays deterministic; guardTrips flags it happened.
      const int DRAIN_CAP = 100000;
      int drained = 0;
      while (model->update(t) && ++drained < DRAIN_CAP) {}
      if (drained >= DRAIN_CAP) ++guardTrips;
      snapshotModel(model, snap);
      stream.push_back(hashSnap(snap));
      if (drawsOut)
        drawsOut->push_back(Random.getDrawCount()); // cumulative RNG draws up to and including this turn
      if (detailOut && i == detailTurn)
        *detailOut = snap;
      if ((i + 1) % 100 == 0) { // flushed heartbeat so a long run is visibly alive + shows its rate
        long long ms = msSince(tStep0);
        std::cout << "\r[lockstep] sim " << label << ": turn " << (i + 1) << "/" << turns
                  << "  (" << (ms > 0 ? (long long)(i + 1) * 1000 / ms : 0) << " turns/s)   " << std::flush;
      }
    }
    if (traceOut) { Random.stopDrawTrace(); *traceOut = Random.getDrawTrace(); }
    std::cout << "\n[lockstep] sim " << label << ": " << turns << " turns in " << msSince(tStep0) << "ms";
    if (guardTrips) std::cout << "  [WARNING: drain cap hit on " << guardTrips << " turn(s) -- zero-time-action creature]";
    std::cout << "\n"; std::cout.flush();
    return stream;
  };
  std::cout << "[lockstep] save=" << saveFile << " turns=" << numTurns << " seed=" << SEED << "\n";
  vector<long long> drawsA, drawsB;
  auto runA = simulate("A", nullptr, -1, &drawsA, 0, nullptr);
  auto runB = simulate("B", nullptr, -1, &drawsB, 0, nullptr);
  if (runA.empty() || runB.empty()) { std::cout << "[lockstep] aborted (load failure)\n"; std::cout.flush(); return; }
  int diverge = -1;
  int n = (int) min(runA.size(), runB.size());
  for (int i = 0; i < n; ++i)
    if (runA[i] != runB[i]) { diverge = i; break; }
  if (diverge < 0 && runA.size() == runB.size()) {
    std::cout << "\n[lockstep] RESULT: DETERMINISTIC -- identical for all " << numTurns
              << " turns. Lockstep is viable at the model-sim level.\n";
    std::cout.flush();
    return;
  }
  std::cout << "\n[lockstep] RESULT: DIVERGED at turn " << diverge
            << " (A=" << std::hex << (diverge >= 0 ? runA[diverge] : 0)
            << " B=" << (diverge >= 0 ? runB[diverge] : 0) << std::dec << ").\n";
  // RNG-draw diagnosis: did the two runs draw the SAME number of random values up to the divergence?
  //   draws DIFFER  -> the bug changed RNG *ordering* (a processing-order nondeterminism -- e.g. two creatures
  //                    handled in different order -> they consume the RNG in a different sequence).
  //   draws MATCH   -> the RNG stream was identical; the divergence came from a NON-RNG address-dependent value
  //                    used directly (e.g. iterating a pointer-ordered set, or an uninitialized read).
  {
    int drawDiverge = -1;
    int dn = (int) min(drawsA.size(), drawsB.size());
    for (int i = 0; i < dn; ++i)
      if (drawsA[i] != drawsB[i]) { drawDiverge = i; break; }
    if (drawDiverge < 0)
      std::cout << "[lockstep] RNG draws: IDENTICAL through turn " << (dn - 1)
                << " (A drew " << (dn ? drawsA[dn-1] : 0) << ", B drew " << (dn ? drawsB[dn-1] : 0)
                << ") -> divergence is a DIRECT address-dependent value, NOT RNG ordering.\n";
    else
      std::cout << "[lockstep] RNG draws: first differ at turn " << drawDiverge
                << " (A=" << drawsA[drawDiverge] << " B=" << drawsB[drawDiverge]
                << ") -> divergence is a PROCESSING-ORDER bug that reorders RNG consumption.\n";
  }
  // Second pass: capture the full creature snapshot at the divergence turn from both runs and print the first
  // creature that differs -- the concrete starting point for the determinism audit.
  if (diverge >= 0) {
    vector<CreatureSnap> a, b;
    simulate("A2", &a, diverge, nullptr, 0, nullptr);
    simulate("B2", &b, diverge, nullptr, 0, nullptr);
    std::cout << "[lockstep] creatures at turn " << diverge << ": A=" << a.size() << " B=" << b.size() << "\n";
    size_t lim = min(a.size(), b.size());
    for (size_t i = 0; i < lim; ++i)
      if (a[i].id != b[i].id || a[i].x != b[i].x || a[i].y != b[i].y ||
          a[i].levelId != b[i].levelId || a[i].healthMilli != b[i].healthMilli) {
        std::cout << "[lockstep] first differing creature: id=" << a[i].id
                  << "  A(x=" << a[i].x << " y=" << a[i].y << " lvl=" << a[i].levelId << " hp=" << a[i].healthMilli << ")"
                  << "  B(x=" << b[i].x << " y=" << b[i].y << " lvl=" << b[i].levelId << " hp=" << b[i].healthMilli << ")\n";
        break;
      }
  }
  // Third pass (the trace): re-run both with RNG call-stack tracing for just enough turns to reach the
  // divergence, then find the FIRST draw whose call stack differs between the runs and symbolize it. That
  // stack is the exact code path where the two runs first take a different RNG-consuming branch -- i.e. the
  // root nondeterministic site (or one frame above it).
  if (diverge >= 0) {
    vector<void*> traces[2];
    int traceTurns = diverge + 2;
    // Both runs MUST be launched from the SAME source line, else the outer harness frames (this call site)
    // differ between the runs and produce false stack diffs. Loop over one call site to keep them identical.
    for (int r = 0; r < 2; ++r)
      simulate(r == 0 ? "A-trace" : "B-trace", nullptr, -1, nullptr, traceTurns, &traces[r]);
    vector<void*>& traceA = traces[0];
    vector<void*>& traceB = traces[1];
    const int F = RandomGen::DRAW_TRACE_FRAMES;
    // Only compare the INNERMOST frames (the draw site + immediate callers). The outer frames are the harness
    // and CRT/system startup, whose raw addresses jitter run-to-run and symbolize to garbage -- comparing them
    // produces false diffs. The innermost frames uniquely identify the divergent game call path.
    const int CMP = 5;
    long long drawsAn = traceA.size() / F, drawsBn = traceB.size() / F;
    long long lim = min(drawsAn, drawsBn);
    long long firstDiff = -1;
    for (long long d = 0; d < lim && firstDiff < 0; ++d)
      for (int f = 0; f < CMP; ++f)
        if (traceA[d * F + f] != traceB[d * F + f]) { firstDiff = d; break; }
    if (firstDiff < 0 && drawsAn != drawsBn)
      firstDiff = lim; // stacks identical up to the shorter run; the extra draw is the first divergence
    std::cout << "\n[lockstep] TRACE: A made " << drawsAn << " draws, B made " << drawsBn
              << " (up to turn " << traceTurns << ").\n";
    if (firstDiff < 0) {
      std::cout << "[lockstep] TRACE: draw call-stacks identical -- divergence not RNG-call-site based.\n";
    } else {
      std::cout << "[lockstep] TRACE: first differing draw = #" << firstDiff << ". Call stack of that draw:\n";
      char sym[600];
      for (int f = 0; f < F; ++f) {
        std::cout << "  A #" << f << ": ";
        if (firstDiff < drawsAn) { rarSymbolize(traceA[firstDiff * F + f], sym, sizeof(sym)); std::cout << sym; }
        else std::cout << "(A ended)";
        std::cout << "\n";
      }
      for (int f = 0; f < F; ++f) {
        std::cout << "  B #" << f << ": ";
        if (firstDiff < drawsBn) { rarSymbolize(traceB[firstDiff * F + f], sym, sizeof(sym)); std::cout << sym; }
        else std::cout << "(B ended)";
        std::cout << "\n";
      }
    }
  }
  std::cout.flush();
}

// RAR lockstep: ONE simulation run in a FRESH process, writing the per-turn fingerprint stream to a file.
// This is the cross-process determinism test that matches real lockstep (each player launches their own clean
// process). Run it twice as separate processes and diff the two files: identical => the sim is deterministic
// across processes (the in-process self-test's divergence was just allocator-carryover between its A/B runs);
// different => genuine cross-process nondeterminism that lockstep must fix. Same seed+turns as the peer.
void MainLoop::rarLockstepDump(const string& saveFile, int numTurns, int seed, const string& outPath) {
  if (numTurns <= 0) numTurns = 500;
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath());
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  (void) factory;
  Random.init(seed); // seed BEFORE load+init (same reason as the self-test)
  auto loaded = loadFromFile<PGame>(FilePath::fromFullPath(saveFile));
  if (!loaded) { std::cout << "[lockstep-dump] FAILED to load: " << saveFile << "\n"; std::cout.flush(); return; }
  PGame game = std::move(*loaded);
  Clock clock(true);
  DummyView dummyView(&clock);
  Encyclopedia encyclopedia(game->getContentFactory());
  game->initialize(options, nullptr, &dummyView, nullptr, &encyclopedia, unlocks, nullptr);
  ProgressMeter meter(1);
  game->initializeModels(meter);
  if (std::getenv("RAR_LOCKSTEP_PROD")) rarLockstepSetupProduction(game.get());
  Model* model = game->getMainModel().get();
  std::ofstream out(outPath);
  double t = model->getLocalTimeDouble();
  vector<CreatureSnap> snap;
  for (int i = 0; i < numTurns; ++i) {
    t += 1.0;
    int drained = 0;
    while (model->update(t) && ++drained < 100000) {}
    snapshotModel(model, snap);
    out << std::hex << hashSnap(snap) << std::dec << " draws=" << Random.getDrawCount() << "\n";
  }
  out.close();
  std::cout << "[lockstep-dump] wrote " << numTurns << " fingerprints (seed=" << seed << ") to " << outPath << "\n";
  std::cout.flush();
}

// RAR lockstep: SYMBOLIZED per-draw trace of the LAST simulated turn, written to a file as file:line stacks.
// For code-address-dependent nondeterminism (differs only ACROSS processes -- e.g. a vtable/typeid in a hash),
// the in-process trace can't reproduce it. Run this twice as fresh processes with numTurns = the divergence
// turn, then diff the two files: the first differing line is the draw whose GAME call path diverged. Symbolized
// (file:line) so it's comparable across processes despite ASLR.
void MainLoop::rarLockstepSymTrace(const string& saveFile, int numTurns, int seed, const string& outPath) {
  if (numTurns <= 0) numTurns = 500;
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath());
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  (void) factory;
  Random.init(seed);
  auto loaded = loadFromFile<PGame>(FilePath::fromFullPath(saveFile));
  if (!loaded) { std::cout << "[lockstep-symtrace] FAILED to load: " << saveFile << "\n"; std::cout.flush(); return; }
  PGame game = std::move(*loaded);
  Clock clock(true);
  DummyView dummyView(&clock);
  Encyclopedia encyclopedia(game->getContentFactory());
  game->initialize(options, nullptr, &dummyView, nullptr, &encyclopedia, unlocks, nullptr);
  ProgressMeter meter(1);
  game->initializeModels(meter);
  Model* model = game->getMainModel().get();
  double t = model->getLocalTimeDouble();
  for (int i = 0; i < numTurns; ++i) {
    t += 1.0;
    if (i == numTurns - 1) Random.startDrawTrace(); // trace ONLY the last (divergence) turn
    int drained = 0;
    while (model->update(t) && ++drained < 100000) {}
  }
  Random.stopDrawTrace();
  auto& tr = Random.getDrawTrace();
  const int F = RandomGen::DRAW_TRACE_FRAMES;
  std::ofstream out(outPath);
  char sym[600];
  long long nDraws = (long long) tr.size() / F;
  for (long long d = 0; d < nDraws; ++d) {
    // innermost GAME frames (skip 0-1 = RandomGen leaf+wrapper); symbolized so it's ASLR-independent
    for (int f = 2; f < 8; ++f) {
      rarSymbolize(tr[d * F + f], sym, sizeof(sym));
      out << sym << " | ";
    }
    out << "\n";
  }
  out.close();
  std::cout << "[lockstep-symtrace] wrote " << nDraws << " symbolized draw-stacks for turn " << numTurns
            << " to " << outPath << "\n";
  std::cout.flush();
}

// RAR lockstep: apply one received/local command to the game. This is the RECEIVING half of input capture --
// the same function runs on both machines for the same command, so it MUST be deterministic. In real PvP the
// command targets the ISSUER's collective; the headless gametest has a single collective so it just applies to
// the player collective. Extend the switch as more actions are wired.
// Resolve a Creature* from its stable generic id by scanning the collective (no global getById exists; the id
// is deterministic so both machines find the same creature).
static Creature* rarFindCreature(Collective* col, long long genericId) {
  for (Creature* c : col->getCreatures())
    if ((long long) c->getUniqueId().getGenericId() == genericId)
      return c;
  return nullptr;
}

static void rarApplyLockstepCommand(Game* game, Collective* col, const LockstepCommand& c) {
  if (!col) return;
  Level* ground = col->getModel()->getGroundLevel();
  switch (c.type) {
    case LSC_SET_ZONE:   col->setZone(Position(Vec2(c.x, c.y), ground), (ZoneId) c.arg); break;
    case LSC_ERASE_ZONE: col->eraseZone(Position(Vec2(c.x, c.y), ground), (ZoneId) c.arg); break;
    case LSC_MINION_ACTIVITY:
      if (Creature* cr = rarFindCreature(col, c.id))
        col->setMinionActivity(cr, (MinionActivity) c.arg);
      break;
    case LSC_CREATURE_GOTO:
      // Banner-style move order: send the minion to a spot on ITS OWN floor (not the ground level -- the drop
      // happens on whatever level the minion is on) and let its AI take over there, with the guard_post banner
      // shown where dropped. Same effect as the game's own minion drag-drop.
      if (Creature* cr = rarFindCreature(col, c.id))
        if (Level* lev = cr->getPosition().getLevel()) {
          PTask task = Task::goToAndWait(Position(Vec2(c.x, c.y), lev), 15_visible);
          task->setViewId(ViewId("guard_post"));
          col->setTask(cr, std::move(task));
        }
      break;
    case LSC_RETREAT: // handled by the battle loop (ends the invader's participation), not a collective mutation
    case LSC_ZLEVEL:  // handled by the loop (changes the viewed/active level), not a collective mutation
    case LSC_NONE:
    default: break;
  }
}

// RAR live PvP: the set of sim-affecting UserInputs we exchange over lockstep and re-apply via processInput on
// BOTH machines (routed to the issuing player's PlayerControl). This is what makes the defender play "business
// as usual" -- build, place, assign, equip, workshop, research, team, immigration all work, just with the
// lockstep input delay. View-only inputs (camera/selection/info) stay local; direct-control and invade-others
// are blocked. Each entry maps a UserInputId to its ASSIGNed payload type (see user_input.h).
using RarCreatureId = UniqueEntity<Creature>::Id;

#define RAR_SYNC_INPUTS(X) \
  X(RECT_SELECTION, BuildingClickInfo) \
  X(RECT_CONFIRM, BuildingClickInfo) \
  X(CREATURE_DRAG_DROP, CreatureDropInfo) \
  X(CREATURE_GROUP_DRAG_ON_MAP, CreatureGroupDropInfo) \
  X(TEAM_DRAG_DROP, TeamDropInfo) \
  X(CREATURE_TASK_ACTION, TaskActionInfo) \
  X(CREATURE_EQUIPMENT_ACTION, EquipmentActionInfo) \
  X(MINION_ACTION, MinionActionInfo) \
  X(ADD_TO_TEAM, TeamCreatureInfo) \
  X(REMOVE_FROM_TEAM, TeamCreatureInfo) \
  X(ADD_GROUP_TO_TEAM, TeamGroupInfo) \
  X(WORKSHOP_CHANGE_COUNT, WorkshopCountInfo) \
  X(WORKSHOP_UPGRADE, WorkshopUpgradeInfo) \
  X(WORKSHOP_ADD, int) \
  X(CREATURE_PROMOTE, PromotionActionInfo) \
  X(AI_TYPE, AIActionInfo) \
  X(INVENTORY_ITEM, InventoryItemInfo) \
  X(INTRINSIC_ATTACK, InventoryItemInfo) \
  X(TEAM_MEMBER_ACTION, TeamMemberActionInfo) \
  X(EQUIPMENT_GROUP_ACTION, EquipmentGroupAction) \
  X(CREATE_TEAM, RarCreatureId) \
  X(CREATE_TEAM_FROM_GROUP, TString) \
  X(CANCEL_TEAM, int) \
  X(SELECT_TEAM, int) \
  X(LIBRARY_ADD, TechId) \
  X(TOGGLE_TEAM_ORDER, TeamOrder) \
  X(IMMIGRANT_ACCEPT, int) \
  X(IMMIGRANT_REJECT, int) \
  X(IMMIGRANT_AUTO_ACCEPT, int) \
  X(IMMIGRANT_AUTO_REJECT, int)

// Serialize a sim-affecting UserInput as (idInt, issuer's viewed levelId, payload). Returns none for inputs we
// don't sync (view-only or control-only) -- those are handled locally / ignored by the battle loop.
static optional<string> rarSerializeUserInput(const UserInput& input, long long levelId) {
  auto id = input.getId();
  std::stringstream ss;
  {
    OutputArchive ar(ss);
    int idInt = int(id);
    ar(idInt);
    ar(levelId);
    switch (id) {
#define RAR_SER(ID, TYPE) case UserInputId::ID: ar(input.get<TYPE>()); break;
      RAR_SYNC_INPUTS(RAR_SER)
#undef RAR_SER
      case UserInputId::RECT_CANCEL: break; // no payload, but clears rect-build selection state on both peers
      default: return none;
    }
  }
  return ss.str();
}

// Reconstruct and apply an input blob to the issuing player's PlayerControl. Pin the PC to the level the issuer
// was viewing so build/click inputs resolve to the same floor on both machines (each player's camera is local).
static void rarApplyUserInput(PlayerControl* pc, Model* model, View* view, const string& blob) {
  if (!pc || blob.empty()) return;
  std::stringstream ss(blob);
  InputArchive ar(ss);
  int idInt = 0; long long levelId = 0;
  ar(idInt); ar(levelId);
  auto id = UserInputId(idInt);
  Level* target = nullptr;
  for (Level* l : model->getLevels())
    if (l->getUniqueId() == levelId) { target = l; break; }
  Level* saved = target ? pc->rarSwapCurrentLevel(target) : nullptr;
  switch (id) {
#define RAR_APP(ID, TYPE) case UserInputId::ID: { TYPE v; ar(v); pc->processInput(view, UserInput(UserInputId::ID, v)); break; }
    RAR_SYNC_INPUTS(RAR_APP)
#undef RAR_APP
    case UserInputId::RECT_CANCEL: pc->processInput(view, UserInput(UserInputId::RECT_CANCEL)); break;
    default: break;
  }
  if (target) pc->rarSwapCurrentLevel(saved);
}

// Pack/unpack a per-tick batch of input blobs for the wire (cereal length-prefixed strings).
static string rarPackBlobs(const std::vector<string>& blobs) {
  std::stringstream ss;
  OutputArchive ar(ss);
  int n = int(blobs.size());
  ar(n);
  for (auto& b : blobs) ar(b);
  return ss.str();
}
static std::vector<string> rarUnpackBlobs(const string& data) {
  std::vector<string> out;
  if (data.empty()) return out;
  std::stringstream ss(data);
  InputArchive ar(ss);
  int n = 0; ar(n);
  for (int i = 0; i < n; ++i) { string b; ar(b); out.push_back(std::move(b)); }
  return out;
}

// RAR lockstep: the REAL game sim driven through the full netcode stack (relay + session handshake + seed
// exchange + per-tick command exchange). Two processes run this (role 0 and 1) against the SAME save; role 0
// is authoritative for the seed. Each tick both send a command (empty for now -- input capture is the next
// step) and wait for the peer's, then step ONE deterministic tick and fingerprint. Their fingerprint files
// must be identical => the transport-driven lockstep loop keeps two real Models in sync. This is the bridge
// from the synthetic net-test to actual gameplay; it does NOT touch the normal game loop (gated behind this
// dev flag). arg: save turns relayHost relayPort role outfile
void MainLoop::rarLockstepGameTest(const string& saveFile, int numTurns, const string& host, int port,
    int role, const string& outPath) {
  LockstepSession session;
  LockstepSessionParams params;
  if (role == 0) { params.gameId = "gametest"; params.commandDelay = 3; } // host sets the config + (auto) seed
  std::cout << "[gametest] role " << role << ": connecting to relay " << host << ":" << port << "...\n"; std::cout.flush();
  if (!session.begin(host, port, "gametest", role, params, 30000)) {
    std::cout << "[gametest] role " << role << ": session handshake FAILED\n"; std::cout.flush(); return;
  }
  const int seed = session.params().seed;
  const int delay = session.params().commandDelay;
  std::cout << "[gametest] role " << role << ": session up, seed=" << seed << " delay=" << delay << "\n"; std::cout.flush();
  // Load the shared starting state, seeded by the AGREED seed.
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath());
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  (void) factory;
  Random.init(seed);
  auto loaded = loadFromFile<PGame>(FilePath::fromFullPath(saveFile));
  if (!loaded) { std::cout << "[gametest] FAILED to load: " << saveFile << "\n"; std::cout.flush(); return; }
  PGame game = std::move(*loaded);
  Clock clock(true);
  DummyView dummyView(&clock);
  Encyclopedia encyclopedia(game->getContentFactory());
  game->initialize(options, nullptr, &dummyView, nullptr, &encyclopedia, unlocks, nullptr);
  ProgressMeter meter(1);
  game->initializeModels(meter);
  Model* model = game->getMainModel().get();
  std::ofstream out(outPath);
  double t = model->getLocalTimeDouble();
  vector<CreatureSnap> snap;
  // The command THIS peer issues for a given tick (in real PvP: captured from local UI input). For the test,
  // role 0 places a GUARD1 banner near the ground-level centre at tick 100 -- a real, sim-affecting command --
  // and everything else is empty. We keep our own sent commands so we can apply them when their tick executes.
  std::map<int, std::vector<LockstepCommand>> myCommands;
  auto localCommandsFor = [&](int tick) -> std::vector<LockstepCommand> {
    std::vector<LockstepCommand> cmds;
    if (role == 0 && tick == 100) {
      Vec2 c = model->getGroundLevel()->getBounds().middle();
      cmds.push_back(LockstepCommand{ LSC_SET_ZONE, c.x, c.y, (int) ZoneId::GUARD1, 0 });
    }
    return cmds;
  };
  // Apply both peers' commands for a tick in a FIXED order (role 0's, then role 1's) so both machines apply
  // them identically.
  auto applyBoth = [&](const std::vector<LockstepCommand>& mine, const std::vector<LockstepCommand>& theirs) {
    const std::vector<LockstepCommand>& first = (role == 0) ? mine : theirs;
    const std::vector<LockstepCommand>& second = (role == 0) ? theirs : mine;
    for (auto& c : first) rarApplyLockstepCommand(game.get(), game->getPlayerCollective(), c);
    for (auto& c : second) rarApplyLockstepCommand(game.get(), game->getPlayerCollective(), c);
  };
  // Prime: send our commands for ticks 0..delay-1 before stepping.
  for (int i = 0; i < delay; ++i) {
    auto cmds = localCommandsFor(i);
    myCommands[i] = cmds;
    if (!session.net().sendTick(i, serializeCommands(cmds))) { std::cout << "[gametest] send failed priming\n"; return; }
  }
  int stepped = 0;
  for (int i = 0; i < numTurns; ++i) {
    auto future = localCommandsFor(i + delay);
    myCommands[i + delay] = future;
    if (!session.net().sendTick(i + delay, serializeCommands(future))) { std::cout << "[gametest] send failed tick " << i << "\n"; break; }
    std::string remoteRaw;
    if (!session.net().waitRemote(i, remoteRaw, 15000)) { std::cout << "[gametest] peer dropped/timeout at tick " << i << "\n"; break; }
    applyBoth(myCommands[i], deserializeCommands(remoteRaw)); // apply this tick's commands, fixed order, BEFORE stepping
    myCommands.erase(i);
    t += 1.0;
    int drained = 0;
    while (model->update(t) && ++drained < 100000) {}
    snapshotModel(model, snap);
    out << std::hex << hashSnap(snap) << "\n";
    ++stepped;
    if ((i + 1) % 100 == 0) { std::cout << "\r[gametest] role " << role << ": lockstep tick " << (i + 1) << "/" << numTurns << std::flush; }
  }
  // Clean teardown barrier (control tick -3): announce we're done and wait for the peer's, so neither side
  // closes its socket while the other is still sending -- that end-of-run race truncated the slower peer.
  session.net().sendTick(-3, "");
  std::string done;
  session.net().waitRemote(-3, done, 5000);
  out.close();
  session.net().close();
  std::cout << "\n[gametest] role " << role << ": stepped " << stepped << " lockstep ticks, wrote " << outPath << "\n"; std::cout.flush();
}

// RAR live PvP (two-collective): inject a small INVADER team onto the loaded (defender's) base so the fight is
// invader-vs-defender, not two copies of one keeper. Deterministic -- both peers run this identically after the
// same seed+load, so both machines get the same battlefield without transferring anything. Returns the invader
// collective (its creatures are what the invader player commands); null on failure. NOTE: for the real invasion
// this will instead inject the invader's ACTUAL team (serialized from their game); this spawn is the test setup.
static Collective* rarInjectInvaderTeam(Game* game, ContentFactory* factory, PlayerControl** outPC) {
  Model* model = game->getMainModel().get();
  Level* ground = model->getGroundLevel();
  auto tribe = TribeId::getBandit(); // hostile to the keeper
  // Deterministic landing tiles: first walkable+free tiles scanning the ground bounds in order.
  vector<Position> landing;
  for (Vec2 v : ground->getBounds()) {
    Position pos(v, ground);
    if (pos.canEnterEmpty(MovementTrait::WALK))
      landing.push_back(pos);
    if (landing.size() >= 40)
      break;
  }
  if (landing.empty()) return nullptr;
  CollectiveBuilder builder(CollectiveConfig::noImmigrants(), tribe, "invaders");
  builder.setModel(model); // build() requires a model/level to anchor the collective
  vector<Creature*> team;
  for (int i = 0; i < 4; ++i) {
    PCreature pc;
    try { pc = factory->getCreatures().fromId(CreatureId("BANDIT"), tribe); } catch (...) {}
    if (!pc) continue;
    Creature* c = pc.get();
    if (model->landCreature(landing, std::move(pc))) {
      builder.addCreature(c, {MinionTrait::FIGHTER});
      team.push_back(c);
    }
  }
  if (team.empty()) return nullptr;
  PCollective col = builder.build(factory);
  Collective* raw = col.get();
  model->addCollective(std::move(col));
  // Make the team obey the collective (task-driven) so it responds to the invader's banner/goto commands, rather
  // than roaming on default MonsterAI. Same pattern the game uses for a collective's own minions.
  for (Creature* c : team)
    c->setController(makeOwner<Monster>(c, MonsterAIFactory::collective(raw)));
  // Give the invader team a PlayerControl so the invader's WINDOW can view + command it (its own keeper-like
  // interface over the bandits). Created identically on BOTH machines so the collective's tick behaviour stays
  // deterministic; only the invader's window renders it, and the invader's commands arrive via lockstep.
  auto ipc = PlayerControl::create(raw, {}, TribeAlignment::EVIL);
  PlayerControl* ipcRaw = ipc.get();
  raw->setControl(std::move(ipc));
  if (outPC) *outPC = ipcRaw;
  return raw;
}

// RAR live PvP: serialize the CURRENT game to a transport blob (raw, decompressed) -- the shared start-state
// this side contributes. Releases control + strips recomputable sectors first, so it's a clean keeper-mode
// snapshot that round-trips through loadFromFile identically on both peers (determinism needs identical bytes).
string MainLoop::rarPackGameBlob(PGame& game) {
  if (!game->getPlayerCreatures().empty() && game->getPlayerControl())
    game->getPlayerControl()->leaveControl();
  for (Model* m : game->getAllModels())
    for (Level* l : m->getLevels())
      l->clearSectors();
  FilePath t = userPath.file("rar_pvp_pack" + getSaveSuffix(GameSaveType::KEEPER));
  string raw;
  saveGame(game, t);                       // gzips to disk
  { igzstream gzin(t.getPath());           // read back decompressed -> the raw save bytes
    raw.assign((std::istreambuf_iterator<char>(gzin)), std::istreambuf_iterator<char>()); }
  t.erase();
  return rarLzmaCompress(raw, 1);          // lzma for transport (a deep base is ~150MB raw -> a few MB). Preset 1
                                           // (fast) not 6: the blob is transient, so favour a short pause over a
                                           // smaller file. rarRunLiveInvasion decompresses on load.
}

// RAR live PvP entry from the real game. Both sides have exchanged packed blobs via the server broker; write
// them to temp saves (re-gzip the raw bytes so loadFromFile can read them) and run the lockstep battle, paired
// on the relay by sessionId. Defender = role 0, invader = role 1. Temp saves are cleaned up after.
void MainLoop::rarRunLiveInvasion(const string& defenderBlob, const string& invaderBlob, const string& sessionId, int role) {
  FilePath defT = userPath.file("rar_pvp_def" + getSaveSuffix(GameSaveType::KEEPER));
  FilePath invT = userPath.file("rar_pvp_inv" + getSaveSuffix(GameSaveType::KEEPER));
  // Blobs arrive lzma-compressed (rarPackGameBlob); decompress to the raw save bytes then re-gzip to a temp
  // .kep so loadFromFile (inside runLockstepBattle) can read them.
  string defRaw = rarLzmaDecompress(defenderBlob);
  string invRaw = rarLzmaDecompress(invaderBlob);
  { ogzstream o(defT.getPath()); o.write(defRaw.data(), defRaw.size()); }
  { ogzstream o(invT.getPath()); o.write(invRaw.data(), invRaw.size()); }
  runLockstepBattle(defT.getPath(), invT.getPath(), rarServerHost(), RAR_LOCKSTEP_DEFAULT_PORT, role, sessionId);
  defT.erase();
  invT.erase();
}

// RAR AUTHORITATIVE live PvP (A2): serialize the battlefield's live creature positions (the HOST is authoritative;
// this is what streams to the invader each tick). One line per creature: "id levelId x y". Cheap + text.
static string rarSerializeBattlefield(Model* m) {
  string out;
  for (Creature* c : m->getAllCreatures()) {
    auto p = c->getPosition();
    if (!p.isValid() || !p.getLevel())   // a dying/off-map creature has no valid position -> deref would segfault
      continue;
    out += toString(c->getUniqueId().getGenericId()) + " " + toString((long long) p.getLevel()->getUniqueId())
         + " " + toString(p.getCoord().x) + " " + toString(p.getCoord().y) + "\n";
  }
  return out;
}

// RAR AUTHORITATIVE live PvP (A2): apply a streamed battlefield snapshot to the invader's NON-simulated puppet
// copy -- move each creature to the host's position. The puppet has the SAME creature ids (it loaded the host's
// serialized game), so we match by id and teleport. No local simulation; the host's stream is the truth.
static void rarApplyBattlefield(Game* puppet, const string& data, int animTurn) {
  Model* m = puppet->getMainModel().get();
  std::unordered_map<long long, Level*> levById;
  for (Level* l : m->getLevels())
    levById[(long long) l->getUniqueId()] = l;
  // Parse the stream: the set of creatures the host still has alive, and where.
  std::unordered_map<long long, Position> targets;
  for (auto& line : split(data, {'\n'})) {
    if (line.empty()) continue;
    auto toks = split(line, {' '});
    if (toks.size() < 4) continue;
    auto lit = levById.find(fromString<long long>(toks[1]));
    if (lit != levById.end())
      targets.emplace(fromString<long long>(toks[0]),
          Position(Vec2(fromString<int>(toks[2]), fromString<int>(toks[3])), lit->second));
  }
  for (Creature* c : m->getAllCreatures()) { // getAllCreatures returns by value -> safe to erase while iterating
    Position cur = c->getPosition();
    auto it = targets.find(c->getUniqueId().getGenericId());
    if (it == targets.end()) {
      // Not in the host's stream -> it died there. Use the VANILLA death so the game drops the real corpse
      // (skeleton for undead, etc.) and removes the creature itself. Guard against dying it twice.
      if (!c->isDead() && cur.isValid() && cur.getCreature() == c)
        c->dieNoReason();
      continue;
    }
    // Alive -> move to the host's position (game's own move keeps the bucket_map right). Only if the target is
    // free; if occupied it catches up once the tile clears (self-correcting, no collision crash). Add a
    // MovementInfo spanning this virtual turn so the renderer SLIDES the sprite there (smooth walk, not teleport).
    if (cur.isValid() && cur != it->second && it->second.isValid() && !it->second.getCreature()) {
      Vec2 dir = it->second.getCoord() - cur.getCoord();
      cur.moveCreature(it->second, false);
      c->rarSetMovementInfo(MovementInfo(dir, LocalTime(animTurn), LocalTime(animTurn + 1),
          m->getMoveCounter(), MovementInfo::MOVE));
    }
  }
}

// RAR live PvP SYNC. Both players run the same battlefield model (each loaded the other's blob, so creature ids
// match). OWNERSHIP: the invader owns her team, the defender owns everything else. Each side streams ONLY the
// creatures it owns and applies what the other sends -- so nobody fights over the same unit and there is no
// determinism requirement. Deltas only (a 29-level base has far too many creatures to resend every tick), plus
// explicit death markers. Wire format, one line each: "M <id> <levelId> <x> <y>" / "D <id>".
static string rarStreamOwned(const std::unordered_map<long long, Creature*>& owned, Model* m,
    const std::set<long long>& teamIds, bool sendTeam, RarLiveStream& st, optional<bool> paused, bool all) {
  string out;
  // Shared pause. Sent EVERY tick, not just on change: a state sent once can be missed or arrive out of order and
  // then one side stays frozen forever while the other plays on (which is exactly the desync pausing was meant to
  // help investigate). Re-sending makes it self-healing -- a wrong pause is corrected within one tick.
  if (paused) {
    st.sentPause = paused;
    out += string("P ") + (*paused ? "1" : "0") + "\n";
  }
  // CAP the payload per tick. The defender streams EVERY creature and his base can hold a great many across ~30
  // levels, so the first message would be one enormous frame -- which is what killed his game the moment the
  // invader landed. Anything that doesn't fit is simply not marked as sent and goes out on a later tick.
  const int maxEntries = 150;
  int emitted = 0;
  std::set<long long> alive;
  for (Creature* c : m->getAllCreatures()) {
    long long id = c->getUniqueId().getGenericId();
    bool mine = all || (teamIds.count(id) > 0) == sendTeam;
    auto p = c->getPosition();
    if (!p.isValid() || !p.getLevel())
      continue;
    auto entry = std::make_pair((long long) p.getLevel()->getUniqueId(), p.getCoord());
    // The DEFENDER tracks every creature so he can report deaths for both sides -- combat runs on his machine
    // (his real minions vs the invader's puppets), so he is the authority on who died. The invader tracks only
    // her own team; reporting deaths she "saw" locally would fight his authoritative result.
    if (mine || !sendTeam)
      alive.insert(id);
    if (!mine)
      continue;                                // positions: only for what I own
    const int hp = (int) (c->getBody().getHealth() * 1000);   // permille, so it survives the text wire
    auto hpIt = st.sentHp.find(id);
    const bool hpChanged = hpIt == st.sentHp.end() || abs(hpIt->second - hp) >= 10; // ignore <1% jitter
    auto it = st.sent.find(id);
    if (it != st.sent.end() && it->second == entry && !hpChanged)
      continue;                                // unchanged -> nothing to send
    if (emitted >= maxEntries)
      continue;                                // over budget -> leave unsent, it goes out next tick
    ++emitted;
    const bool isNew = (it == st.sent.end());  // capture BEFORE the insert below invalidates `it`
    st.sent[id] = entry;
    st.sentHp[id] = hp;
    if (isNew) {
      // First time we mention this creature. Send its TYPE so the peer can create it if it doesn't have it --
      // this is how summons, scrolls and wave enemies (spawned mid-battle) appear on the other screen. For the
      // creatures that existed when the battle started the peer already has this id and just moves it.
      if (auto cid = c->getAttributes().getCreatureId())
        out += "S " + toString(id) + " " + string(cid->data()) + " " + string(c->getTribeId().data()) + " "
             + toString(entry.first) + " " + toString(entry.second.x) + " " + toString(entry.second.y) + "\n";
      else // no content id (unique/named creature) -- can't be recreated remotely, just position it
        out += "M " + toString(id) + " " + toString(entry.first) + " " + toString(entry.second.x) + " "
             + toString(entry.second.y) + "\n";
    } else
      out += "M " + toString(id) + " " + toString(entry.first) + " " + toString(entry.second.x) + " "
           + toString(entry.second.y) + "\n";
  }
  // Vanished from the battlefield since last tick. That is NOT automatically death: a RETREATING squad leaves this
  // model as it travels home. Ask the creature itself (we keep direct pointers to our own battle creatures) --
  // alive means it merely LEFT ("L", peer removes it), otherwise it really died ("D", peer kills it properly).
  for (auto it = st.sent.begin(); it != st.sent.end();)
    if (!alive.count(it->first)) {
      auto ow = owned.find(it->first);
      const bool leftAlive = ow != owned.end() && ow->second && !ow->second->isDead();
      out += (leftAlive ? "L " : "D ") + toString(it->first) + "\n";
      st.sentHp.erase(it->first);
      it = st.sent.erase(it);
    } else
      ++it;
  return out;
}

static void rarApplyStream(Model* m, const string& data, const std::set<long long>& teamIds, bool applyTeam,
    RarLiveStream& st, bool all) {
  if (data.empty())
    return;
  std::unordered_map<long long, Creature*> byId;
  for (Creature* c : m->getAllCreatures())
    byId[c->getUniqueId().getGenericId()] = c;
  // Resolve a remote id to a local creature: either we already have that id (everything present when the battle
  // started), or it's one the peer spawned mid-battle and we created a stand-in for.
  auto resolve = [&](long long id) -> Creature* {
    auto it = byId.find(id);
    if (it != byId.end())
      return it->second;
    auto mapped = st.remoteToLocal.find(id);
    if (mapped != st.remoteToLocal.end()) {
      auto local = byId.find(mapped->second);
      if (local != byId.end())
        return local->second;
    }
    return nullptr;
  };
  std::unordered_map<long long, Level*> levById;
  for (Level* l : m->getLevels())
    levById[(long long) l->getUniqueId()] = l;
  int animTurn = (int) m->getLocalTimeDouble();
  for (auto& line : split(data, {'\n'})) {
    if (line.empty()) continue;
    auto toks = split(line, {' '});
    if (toks.size() < 2) continue;
    try { // a malformed line must never kill the game: fromString throws, and an uncaught throw here exits silently
    if (toks[0] == "P") { // shared pause (debug aid): mirror the peer's pause state
      if (View* v = m->getGame() ? m->getGame()->getView() : nullptr) {
        bool wantPause = toks[1] == "1";
        if (wantPause != v->isClockStopped()) {
          if (wantPause) v->stopClock(); else v->continueClock();
        }
      }
      continue;
    }
    // "C <order>" is the invader's banner order -- handled by the game loop, NOT here. It must be skipped before
    // the id parse below: fromString THROWS on a non-numeric token ("move"), and an uncaught exception in the game
    // loop terminates the process silently (no crash dialog) -- which is exactly what killed the defender the
    // moment the invader clicked to move.
    if (toks[0] == "C")
      continue;
    // "SC <casterId> <spellId>" -- the peer's creature cast a spell. Replay the ACT only: show the message and
    // start the same cooldown on our copy of the caster. We deliberately do NOT call Spell::apply -- damage,
    // deaths and summons come from the authoritative side, and applying here would duplicate them.
    if (toks[0] == "SC" && toks.size() >= 3) {
      if (auto* g = m->getGame()) {
        std::unordered_map<long long, Creature*> casters;
        for (Creature* cr : m->getAllCreatures())
          casters[cr->getUniqueId().getGenericId()] = cr;
        auto cit2 = casters.find(fromString<long long>(toks[1]));
        if (cit2 != casters.end() && !cit2->second->isDead())
          if (auto* spell = g->getContentFactory()->getCreatures().getSpell(SpellId(toks[2].c_str()))) {
            spell->addMessage(cit2->second);
            if (auto now = cit2->second->getGlobalTime())
              cit2->second->getSpellMap().setReadyTime(cit2->second, spell,
                  *now + cit2->second->calculateSpellCooldown(spell->getCooldown()));
          }
      }
      continue;
    }
    // Replay the peer's spell/projectile visuals, but only for the level we're actually looking at.
    if (toks[0] == "F" && toks.size() >= 7) {
      if (auto* g = m->getGame())
        if (View* v = g->getView()) {
          auto lit = levById.find(fromString<long long>(toks[1]));
          if (lit != levById.end() && lit->second == g->getCurrentModel()->getGroundLevel())
            v->animation(FXSpawnInfo(FXInfo(FXName(fromString<int>(toks[4]))),
                Vec2(fromString<int>(toks[2]), fromString<int>(toks[3])),
                Vec2(fromString<int>(toks[5]), fromString<int>(toks[6]))));
        }
      continue;
    }
    if (toks[0] == "J" && toks.size() >= 8) {
      if (auto* g = m->getGame())
        if (View* v = g->getView()) {
          int fxName = fromString<int>(toks[6]);
          optional<ViewId> vid;
          if (toks[7] != "-")
            vid = ViewId(toks[7].c_str());
          optional<FXInfo> fx;
          if (fxName >= 0)
            fx = FXInfo(FXName(fxName));
          v->animateObject(Vec2(fromString<int>(toks[2]), fromString<int>(toks[3])),
              Vec2(fromString<int>(toks[4]), fromString<int>(toks[5])), vid, fx);
        }
      continue;
    }
    long long id = fromString<long long>(toks[1]);
    // "S id creatureType tribe lev x y" -- a creature the peer owns. Create it only if we don't know it yet
    // (mid-battle summon / wave enemy); otherwise it's just a position update for something we already have.
    if (toks[0] == "S" && toks.size() >= 7) {
      if (!resolve(id)) {
        auto lit = levById.find(fromString<long long>(toks[4]));
        if (lit == levById.end()) continue;
        Position at(Vec2(fromString<int>(toks[5]), fromString<int>(toks[6])), lit->second);
        if (!at.isValid() || at.getCreature()) continue;   // occupied -> retry on a later update
        if (auto* game = m->getGame()) {
          auto spawned = game->getContentFactory()->getCreatures().fromId(CreatureId(toks[2].c_str()),
              TribeId(toks[3].c_str()));
          if (spawned) {
            Creature* raw = spawned.get();
            raw->setController(makeOwner<Monster>(raw, MonsterAIFactory::idle())); // puppet: the peer drives it
            at.addCreature(std::move(spawned));
            st.remoteToLocal[id] = raw->getUniqueId().getGenericId();
          }
        }
        continue;
      }
      // known already -> fall through and treat the trailing coords (and health) as a move
      toks = toks.size() >= 8 ? vector<string>{string("M"), toks[1], toks[4], toks[5], toks[6], toks[7]}
                              : vector<string>{string("M"), toks[1], toks[4], toks[5], toks[6]};
    }
    Creature* c = resolve(id);
    if (!c) continue;
    if (toks[0] == "L") {
      if (auto* g = m->getGame())
        if (auto* pc = g->getPlayerControl())
          pc->addMessage(PlayerMessage(TString("[live] LEFT " + string(c->getName().bare().data())),
              MessagePriority::HIGH));
      // The peer's creature RETREATED off this battlefield (it is alive in their game). Take it off our map
      // without killing it -- no death, no corpse, no "your minion was killed" message.
      auto p = c->getPosition();
      if (!c->isDead() && p.isValid() && p.getLevel() && p.getCreature() == c)
        m->extractCreature(c);
      st.desired.erase(id);
      continue;
    }
    if (toks[0] == "D") {
      st.deadIds.insert(id);   // record FIRST: every path below may legitimately skip the kill for now
      // Deaths are accepted for ANY creature: the defender simulates the fight, so his result is authoritative
      // for both sides (otherwise a unit killed on his screen kept walking around on hers). EXCEPT a creature the
      // local player is controlling: killing that out-of-band leaves a dead creature in the time queue and the
      // engine FATALs (model.cpp "Dead: ..."), plus it triggers the normal keeper-death screen mid-battle. Flag
      // it instead and let the game loop end the battle cleanly.
      if (auto* g = m->getGame())
        if (g->getPlayerCreatures().contains(c)) {
          // We used to flag defeat and SKIP the kill for anything the player controlled. The invader controls her
          // WHOLE squad, so that made every one of her minions immortal here: units the defender's simulation had
          // killed stayed alive on her screen and walked home, leaving them dead in his dungeon and alive in hers.
          // Vanilla already handles a controlled team member dying -- PlayerControl::onMemberKilledOrStunned pops
          // its controller and picks a new leader -- so kill it normally. Two cases must NOT be killed here:
          //   - the collective LEADER (my keeper): its death is game over, and losing a raid must not end the game;
          //   - the LAST creature I control: releasing control is the game loop's job, so flag defeat and let it
          //     apply the death once control is gone (otherwise it, too, walks home alive).
          bool isLeader = false;
          if (auto* col = g->getPlayerCollective())
            isLeader = col->getCreatures(MinionTrait::LEADER).contains(c);
          if (isLeader) {
            g->rarSetLiveDefeat();   // my keeper: losing a raid must not end the game, so don't kill it
            continue;
          }
          // Only the TEAM LEADER is under the player's control, so "nothing controlled left" is NOT defeat --
          // it just means the leader fell. Pass leadership to a surviving squad member and fight on. Defeat is
          // when there is nobody left to lead; then the kill is deferred until control has been released.
          if (auto* pc = g->getPlayerControl())
            if (!pc->rarPassLeadership(c)) {
              g->rarSetLiveDefeat(id);
              continue;
            }
        }
      if (auto* g = m->getGame())
        if (auto* pc = g->getPlayerControl())
          pc->addMessage(PlayerMessage(TString("[live] DIED " + string(c->getName().bare().data())),
              MessagePriority::HIGH));
      if (!c->isDead() && c->getPosition().isValid() && c->getPosition().getCreature() == c)
        c->dieNoReason();          // vanilla death -> proper corpse
      continue;
    }
    if (!all && (teamIds.count(id) > 0) != applyTeam)  // positions: only what the OTHER player owns
      continue;
    if (c->isDead()) continue;
    if (toks[0] != "M" || toks.size() < 5) continue;
    // Record where it should be; the pass below applies it (and keeps retrying if it can't yet).
    st.desired[id] = std::make_pair(fromString<long long>(toks[2]),
        Vec2(fromString<int>(toks[3]), fromString<int>(toks[4])));
    // Health comes from the authoritative side only (`all` = the invader applying the defender's world), so a
    // wounded creature looks wounded on both screens instead of only where the fighting was simulated.
    if (all && toks.size() >= 6)
      c->getBody().rarSetHealth(fromString<int>(toks[5]) / 1000.0);
    } catch (...) { continue; } // malformed/garbled line -> ignore it, never take the game down
  }
  // Apply/retry desired positions. Anything that couldn't move (tile momentarily taken) stays queued, so a unit
  // can never end up stuck a few tiles away from where its owner actually has it.
  for (auto it = st.desired.begin(); it != st.desired.end();) {
    Creature* c = resolve(it->first);
    auto lit = levById.find(it->second.first);
    if (!c || c->isDead() || lit == levById.end()) { it = st.desired.erase(it); continue; }
    Position cur = c->getPosition();
    Position target(it->second.second, lit->second);
    if (!cur.isValid() || !target.isValid()) { ++it; continue; }
    if (cur == target) { it = st.desired.erase(it); continue; }   // matched -> nothing pending
    Vec2 dir = target.getCoord() - cur.getCoord();
    if (!target.getCreature()) {
      cur.moveCreature(target, false);         // engine move: keeps the level's bucket map consistent
      c->rarSetMovementInfo(MovementInfo(dir, LocalTime(animTurn), LocalTime(animTurn + 1),
          m->getMoveCounter(), MovementInfo::MOVE));   // slide + face the right way instead of teleporting
      it = st.desired.erase(it);
    } else if (Creature* occupant = target.getCreature()) {
      // The tile is taken. Creatures walk THROUGH their own by SWAPPING places (Creature::move ->
      // swapPosition, the "excuse me" shuffle) -- so a swap is a completely normal move, not a blockage.
      // Skipping it was why a squad shuffling in a corridor ended up in a different formation on each screen.
      // Only swap when we're allowed to move the occupant too, otherwise we'd displace a creature its own
      // owner is authoritative for.
      bool mayMoveOccupant = all ||
          (teamIds.count(occupant->getUniqueId().getGenericId()) > 0) == applyTeam;
      if (mayMoveOccupant && !occupant->isDead() && cur.isSameLevel(target)) {
        cur.swapCreatures(occupant);
        c->rarSetMovementInfo(MovementInfo(dir, LocalTime(animTurn), LocalTime(animTurn + 1),
            m->getMoveCounter(), MovementInfo::MOVE));
        occupant->rarSetMovementInfo(MovementInfo(-dir, LocalTime(animTurn), LocalTime(animTurn + 1),
            m->getMoveCounter(), MovementInfo::MOVE));
        it = st.desired.erase(it);
      } else
        ++it;                                  // not ours to move -> try again next tick
    } else
      ++it;
  }
}

// RAR AUTHORITATIVE live PvP (A4): apply an invader command (received over the relay) to her team in the HOST's
// authoritative game. Model: leader (teamIds[0], the invader's keeper) goes to the target and fights on the way;
// the rest FOLLOW the leader. "stop" = hold position. Real-time -- the host's normal sim runs it.
static void rarApplyInvaderCommand(Game* host, const vector<long long>& teamIds, const string& cmd) {
  if (teamIds.empty()) return;
  Model* m = host->getMainModel().get();
  std::unordered_map<long long, Creature*> byId;
  for (Creature* c : m->getAllCreatures())
    byId[c->getUniqueId().getGenericId()] = c;
  vector<Creature*> team;                       // living team members, leader first (teamIds[0])
  for (long long id : teamIds) {
    auto it = byId.find(id);
    if (it != byId.end()) team.push_back(it->second);
  }
  if (team.empty()) return;
  auto toks = split(cmd, {' '});
  if (toks.empty()) return;
  if (toks[0] == "move" && toks.size() >= 4) {
    Level* lev = nullptr;
    long long levId = fromString<long long>(toks[1]);
    for (Level* l : m->getLevels())
      if ((long long) l->getUniqueId() == levId) { lev = l; break; }
    if (!lev && team[0]->getPosition().isValid())
      lev = team[0]->getPosition().getLevel();
    if (!lev)
      return;                                  // unknown level and no fallback -- ignore rather than deref null
    Position dest(Vec2(fromString<int>(toks[2]), fromString<int>(toks[3])), lev);
    if (!dest.isValid())
      return;
    bool chase = toks.size() < 5 || toks[4] != "0"; // default chase; "0" = don't chase (just go to the point)
    // CONVERGE on the clicked spot: every unit heads there itself, the way dragging a team to a tile works in
    // vanilla (goToAndWait + a banner). Follow-the-leader was wrong for this -- the followers trailed the leader
    // instead of gathering, and one blocked unit stalled the whole squad.
    for (Creature* c : team) {
      PTask task = Task::goToAndWait(dest, 1000_visible);
      task->setViewId(ViewId("guard_post"));   // the banner marking where they were sent
      c->setController(makeOwner<Monster>(c, MonsterAIFactory::singleTask(std::move(task), chase)));
    }
  } else if (toks[0] == "stop") {
    for (Creature* c : team)
      c->setController(makeOwner<Monster>(c, MonsterAIFactory::stayInLocation({c->getPosition().getCoord()}, false)));
  }
}

// RAR AUTHORITATIVE live PvP (A1): inject a live invader's team into the DEFENDER's running game. The invader
// uploaded their whole game (lzma); here the host decompresses it, injects the invader's model as a campaign site
// (same machinery the offline invasion uses), and transfers their leader onto the defender's base, retagged
// hostile so the defender's minions fight it. The defender keeps playing -- no freeze, no reload. Returns the
// loaded invader PGame (KEEP IT ALIVE for the battle; the injected model is shared from it), or null on failure.
PGame MainLoop::rarInjectLiveInvader(Game* defenderGame, const string& invaderBlob, string& diag, vector<long long>& outTeamIds,
    Vec2& outSitePos) {
  // First line = the invading team's creature ids (comma-separated); the rest is the lzma game blob.
  auto nl = invaderBlob.find('\n');
  string teamHdr = (nl == string::npos) ? string() : invaderBlob.substr(0, nl);
  string compressed = (nl == string::npos) ? invaderBlob : invaderBlob.substr(nl + 1);
  set<long long> teamIds;
  for (auto& s : split(teamHdr, {','}))
    if (!s.empty()) teamIds.insert(fromString<long long>(s));
  diag += " team=" + toString((int) teamIds.size());
  diag += " blob=" + toString(compressed.size());
  string raw = rarLzmaDecompress(compressed);
  diag += " raw=" + toString(raw.size());
  if (raw.empty()) { diag += " DECOMPRESS_FAIL"; return nullptr; }
  FilePath t = userPath.file("rar_live_inv" + getSaveSuffix(GameSaveType::KEEPER));
  { ogzstream o(t.getPath()); o.write(raw.data(), raw.size()); }
  optional<SavedGameInfo> info = loadSavedGameInfo(t);
  optional<PGame> loaded = loadFromFile<PGame>(t);
  t.erase();
  if (!loaded || !*loaded || !info) { diag += " LOAD_FAIL"; return nullptr; }
  PGame invGame = std::move(*loaded);
  if (!invGame->getPlayerControl() || !invGame->getPlayerCollective()) { diag += " NO_PC"; return nullptr; }
  // The invader's loaded game brings BOTH of the hazards the invader side already guards against on its copy
  // (see the battlefield load in playGame) -- this side never did, and both bite here.
  // 1. Her PlayerControl is an EventListener subscribed to this model, and addInvasionSite() below re-homes that
  //    model into MY game. Every creature moved by the stream then fires CreatureMoved into HER control, which
  //    updates minion visibility on a game that was never initialised here. Worse, the battle teardown destroys
  //    her game (rarLiveInvaderGame = nullptr) while it is STILL SUBSCRIBED -- so the NEXT invasion fires events
  //    straight into a freed object and the game vanishes with no message (access violation inside
  //    VisibilityMap/PositionMap, which is exactly the crash on a second live invasion).
  // 2. She may have been CONTROLLING creatures when the blob was made. Loaded here they are "players" driven by
  //    my view: her keeper would take over my camera and control mode. Strip them to idle AI.
  invGame->getPlayerControl()->unsubscribe();
  for (Creature* c : invGame->getMainModel()->getAllCreatures())
    if (auto* ctrl = c->getController())
      if (ctrl->isPlayer())
        c->setController(makeOwner<Monster>(c, MonsterAIFactory::idle()));
  // Hang the invader's model on a free campaign tile (same as the async invasion / staging).
  Vec2 basePos = defenderGame->getCurrentModel()->position;
  Vec2 sitePos = basePos;
  bool foundSite = false;
  for (Vec2 v : defenderGame->getCampaign().getSites().getBounds())
    if (v != basePos && defenderGame->getCampaign().getSites()[v].isEmpty()) { sitePos = v; foundSite = true; break; }
  diag += foundSite ? " site=y" : " site=NO";
  outSitePos = sitePos;   // remembered so the battle can be torn down cleanly when it ends
  Model* injected = defenderGame->addInvasionSite(sitePos, PModel(invGame->getMainModel().giveMeSharedPointer()), *info);
  diag += injected ? " inj=y" : " inj=NO";
  Model* battlefield = defenderGame->getMainModel().get(); // the defender's base = floor 0 lives here
  // The invading team = the collective creatures whose ids the invader sent. Fallback to the leader if none given.
  vector<Creature*> team;
  for (Creature* c : invGame->getPlayerCollective()->getCreatures())
    if (teamIds.count(c->getUniqueId().getGenericId()))
      team.push_back(c);
  if (team.empty())
    for (Creature* c : invGame->getPlayerCollective()->getLeaders())
      team.push_back(c);
  // Put the invader's KEEPER (leader) first -- it's the team leader the followers follow at A4.
  if (!invGame->getPlayerCollective()->getLeaders().empty()) {
    Creature* keeper = invGame->getPlayerCollective()->getLeaders()[0];
    for (int i = 0; i < (int) team.size(); ++i)
      if (team[i] == keeper) { std::swap(team[0], team[i]); break; }
  }
  int moved = 0;
  optional<Position> firstPos;
  Creature* t0 = nullptr; Creature* t1 = nullptr; // first two teammates, for the friendly-fire diagnostic
  for (Creature* inv : team)
    if (defenderGame->canTransferCreature(inv, battlefield)) {
      defenderGame->transferCreature(inv, battlefield);
      inv->setTribe(TribeId::getInvaders()); // INVADER tribe (supposed to be friendly within itself)
      inv->removeEffect(LastingEffect::SLEEP, false);   // invaders arrive awake, not sleepwalking
      // Companions (a shaman's spirits, summons, steeds) are dragged along by transferCreature WITHOUT passing
      // through this loop -- so they kept their original tribe and ended up hostile to the invaders AND the
      // defender. Retag and puppet them exactly like their owner.
      for (Creature* comp : inv->getCompanions()) {
        comp->setTribe(TribeId::getInvaders());
        comp->setController(makeOwner<Monster>(comp, MonsterAIFactory::idle()));
        outTeamIds.push_back(comp->getUniqueId().getGenericId());
      }
      // PUPPET: this creature is driven by the invader's position stream, so it must not move itself -- its own AI
      // would fight the stream (and used to walk it home off the map). Rest-only AI; the invader steers it.
      inv->setController(makeOwner<Monster>(inv, MonsterAIFactory::idle()));
      outTeamIds.push_back(inv->getUniqueId().getGenericId()); // leader first (team[0])
      if (!t0) t0 = inv; else if (!t1) t1 = inv;
      if (!firstPos) firstPos = inv->getPosition();
      ++moved;
    }
  // DIAGNOSTIC: is a teammate seen as an enemy, and via which path? tribe=<their tribe>, enemyCC=creature-level
  // isEnemy, enemyTT=tribe-level isEnemy. If enemyTT=Y the TRIBE is the cause; enemyCC=Y & enemyTT=N is private/hated.
  if (t0 && t1)
    diag += string(" tribe=") + t0->getTribeId().data() + " enemyCC=" + (t0->isEnemy(t1) ? "Y" : "N")
          + " enemyTT=" + (t0->getTribe()->isEnemy(t1->getTribe()) ? "Y" : "N");
  diag += " moved=" + toString(moved) + "/" + toString((int) team.size());
  if (firstPos) {
    diag += " landed=(" + toString(firstPos->getCoord().x) + "," + toString(firstPos->getCoord().y) + ")";
    if (auto pc = defenderGame->getPlayerControl()) // A1 DEBUG: point the defender's view at the invaders
      pc->rarSetViewPos(*firstPos);
  }
  // The invader's loaded game keeps its own PlayerControl, and addInvasionSite() has just registered HER
  // collectives into MY game -- so they tick here, running her control on a game that was never initialised.
  // Idle those controls (same thing the invader's side does to my copy) and then clear her game's raw pointer
  // to the control that just got freed: addEvent()'s CreatureMoved shortcut would otherwise call it directly,
  // unsubscribe or not, for every creature the battle moves.
  for (Collective* col : injected ? injected->getCollectives() : std::vector<Collective*>{})
    col->setControl(CollectiveControl::idle(col));
  invGame->rarClearPlayerControl();
  defenderGame->setWasTransfered();
  return invGame;
}

// RAR authoritative live PvP (A3a): serialize a game to a compressed transport blob WITHOUT mutating it (unlike
// rarPackGameBlob which leaveControl()s + clearSectors()). The host is mid-play, so it must not disturb its own
// game to send the battlefield to the invader.
string MainLoop::rarSerializeGameBlob(PGame& game) {
  FilePath t = userPath.file("rar_ser" + getSaveSuffix(GameSaveType::KEEPER));
  string raw;
  saveGame(game, t);
  { igzstream gzin(t.getPath());
    raw.assign((std::istreambuf_iterator<char>(gzin)), std::istreambuf_iterator<char>()); }
  t.erase();
  return rarLzmaCompress(raw, 1);
}

// RAR authoritative live PvP (A3a): the invader loads the host's battlefield blob and SPECTATES it read-only.
// Renders the base (where the invader's team landed) via a Spectator until the invader exits. This is the first
// visible cut of the remote client -- it proves the invader can SEE the host's battlefield. A2 will layer a live
// per-tick creature stream on top (so it updates), and A4 will add remote control of the invader's team.
void MainLoop::rarSpectateInvasion(PGame combinedGame, const string& sessionId, const set<long long>& teamIds) {
  (void) sessionId;
  if (!view) return;
  Encyclopedia enc(combinedGame->getContentFactory());
  combinedGame->initialize(options, highscores, view, fileSharing, &enc, unlocks, steamAchievements);
  doWithSplash(TStringId("INITIALIZING_GAME"), combinedGame->getAllModels().size(),
      [&] (ProgressMeter& meter) { combinedGame->initializeModels(meter); });
  // Render the BATTLEFIELD directly (the defender's model, where the team was transferred) via a Spectator --
  // NOT the invader's PlayerControl, which is anchored to the invader's OWN base model and refuses to show the
  // defender's level. Center on a team creature so the invader sees her forces. Spectator normally exits on any
  // key; rarSetInteractiveSpectator lets it scroll (only ESC exits) so this is a real, look-around view.
  Level* level = combinedGame->getMainModel()->getGroundLevel();
  optional<Position> teamPos;
  for (Creature* c : combinedGame->getMainModel()->getAllCreatures())
    if (teamIds.count(c->getUniqueId().getGenericId())) { teamPos = c->getPosition(); level = c->getPosition().getLevel(); break; }
  RarObserverView spectator(level, view);
  view->rarSetInteractiveSpectator(true);
  // Connect to the relay (role 1) and receive the host's live battlefield stream. My copy is NEVER simulated --
  // I just apply the host's positions each frame (thin client; the host is the sole authority).
  LockstepNet net;
  bool linked = false;
  doWithSplash(TString("Linking to the battlefield..."_s), [&] {
    linked = net.connect(rarServerHost(), RAR_LOCKSTEP_DEFAULT_PORT, sessionId, 1, 120000);
  });
  int seq = 0, cmdSeq = 0;
  Model* bfModel = combinedGame->getMainModel().get();
  bool chase = true;    // 'c' toggles: chase = fight on the way; !chase = "don't chase" (just go)
  bool running = true;
  bool centered = false;
  while (running) {
    // Drain any new host snapshots (in order) and apply them to the puppet before rendering.
    std::string snap;
    while (net.tryGetRemote(seq, snap)) {
      spectator.onHostUpdate();                                       // advance the animation clock one virtual turn
      rarApplyBattlefield(combinedGame.get(), snap, spectator.virtualTurn); // moves slide; deaths -> real corpses
      ++seq;
    }
    if (linked && !net.connected()) { running = false; break; } // host ended the battle
    view->updateView(&spectator, false);
    if (!centered && teamPos) { view->setScrollPos(*teamPos); centered = true; } // center on my forces once
    view->refreshView();
    // Command keys: 'c' toggles chase, 's' = stop/hold, '.'/',' scroll DOWN/UP a floor.
    if (int key = view->rarConsumeSpectatorKey()) {
      if (key == 'c') chase = !chase;
      else if (key == 's') net.sendTick(cmdSeq++, "stop");
      else if (key == '.' || key == ',') { // '.'='>' down (deeper), ','='<' up
        if (auto depth = bfModel->getMainLevelDepth(spectator.obsLevel)) {
          int nd = *depth + (key == '.' ? 1 : -1);
          if (bfModel->getMainLevelsDepth().contains(nd)) {
            Level* nl = bfModel->getMainLevel(nd);
            spectator.setLevel(nl);
            view->setScrollPos(Position(nl->getBounds().middle(), nl)); // recenter on the new floor
          }
        }
      }
    }
    for (UserInput input = view->getAction(); input.getId() != UserInputId::IDLE; input = view->getAction()) {
      if (input.getId() == UserInputId::EXIT) { running = false; break; }
      // Left-click a tile on the CURRENT floor -> order the team there (leader goes, rest follow, chasing unless
      // toggled off). The target level is whatever floor is being viewed, so it works across z-levels (the host
      // pathfinds via stairs). Right-click -> stop/hold.
      else if (input.getId() == UserInputId::TILE_CLICK) {
        Vec2 p = input.get<Vec2>();
        net.sendTick(cmdSeq++, "move " + toString((long long) spectator.obsLevel->getUniqueId())
            + " " + toString(p.x) + " " + toString(p.y) + " " + toString(chase ? 1 : 0));
      }
      else if (input.getId() == UserInputId::CREATURE_MAP_CLICK_EXTENDED)
        net.sendTick(cmdSeq++, "stop");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  net.close();
  view->rarSetInteractiveSpectator(false);
}

// RAR live PvP: a playable lockstep battle in a REAL window. First live-testable cut -- launched via
// --rar_lockstep_battle so two windows can drive the same base in real-time lockstep. Architecture: render
// CONTINUOUSLY (smooth) but advance the SIM only one fixed tick when both peers' commands for that tick are in
// (stall = keep rendering). Input capture v1: minion drag-drop -> a banner-style GOTO command (shows on both
// windows); other inputs apply locally as UI navigation. This is the integration piece that needs live 2-window
// testing to tune (tick cadence, which inputs to capture, rendering). It does NOT touch normal playGame.
void MainLoop::runLockstepBattle(const string& defenderSave, const string& invaderSave, const string& host, int port, int role, const string& sessionId) {
  if (!view) { std::cout << "[battle] no view (must run with a real window)\n"; return; }
  // Content + tileset first (the view's splash/presentText need the tileset's scriptedUI templates).
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath());
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  (void) factory;
  if (tileSet) tileSet->setTilePathsAndReload(getTilePathsForAllMods());
  view->reset();
  // Handshake (splash while waiting for the other player).
  LockstepSession session;
  LockstepSessionParams params;
  if (role == 0) { params.gameId = sessionId; params.commandDelay = 3; }
  bool connected = false;
  doWithSplash(TString("Waiting for the other player to join the battle..."_s), [&] {
    connected = session.begin(host, port, sessionId, role, params, 60000);
  });
  if (!connected) { view->presentText(none, TString("Could not connect / pair with the other player."_s)); return; }
  const int seed = session.params().seed;
  const int delay = session.params().commandDelay;
  Random.init(seed);
  // Both machines build the SAME battlefield the way the real invasion does: load the INVADER's game as primary,
  // inject the DEFENDER's base as an invasion site, and transfer the invader's real team onto it. Real creatures
  // => real vision/tribe/control; the existing invasion machinery handles hostility. Deterministic on both peers.
  optional<PGame> invLoaded, defLoaded;
  optional<SavedGameInfo> invInfo;
  doWithSplash(TString("Loading the battlefield..."_s), [&] {
    invLoaded = loadFromFile<PGame>(FilePath::fromFullPath(invaderSave));
    defLoaded = loadFromFile<PGame>(FilePath::fromFullPath(defenderSave));
    invInfo = loadSavedGameInfo(FilePath::fromFullPath(invaderSave));
  });
  if (!invLoaded || !*invLoaded || !defLoaded || !*defLoaded || !invInfo) {
    view->presentText(none, TString("Failed to load one of the saves."_s)); return;
  }
  // DEFENDER's game is PRIMARY (fully initialized -> a normal keeper, correct tribes/control -> no friendly fire
  // and role 0 works). The INVADER's model is injected only so we can transfer their keeper onto the battlefield.
  PGame game = std::move(*defLoaded);     // Moriaty = primary, the battlefield + defender
  PGame target = std::move(*invLoaded);   // Violet -- her model is injected; keep it alive for the battle
  Encyclopedia encyclopedia(game->getContentFactory());
  game->initialize(options, highscores, view, fileSharing, &encyclopedia, unlocks, steamAchievements);
  doWithSplash(TStringId("INITIALIZING_GAME"), game->getAllModels().size(),
      [&] (ProgressMeter& meter) { game->initializeModels(meter); });
  if (!game->getPlayerControl() || !target->getPlayerControl()) {
    view->presentText(none, TString("A save has no keeper control."_s)); return;
  }
  // Stage the invasion: hang the invader's model on a free campaign tile, then transfer ONLY the invader's KEEPER
  // (leader) onto the defender's base -- like an invader arriving with just their keeper.
  Model* battlefield = game->getMainModel().get(); // the defender's base = the shared battlefield both step
  bool siteInjected = false, foundSite = false, canXfer = false, transferred = false;
  int leaderCount = 0;
  doWithSplash(TString("Staging the invasion..."_s), [&] {
    Vec2 basePos = game->getCurrentModel()->position;
    Vec2 sitePos = basePos;
    for (Vec2 v : game->getCampaign().getSites().getBounds())
      if (v != basePos && game->getCampaign().getSites()[v].isEmpty()) { sitePos = v; foundSite = true; break; }
    Model* violetSite = game->addInvasionSite(sitePos, PModel(target->getMainModel().giveMeSharedPointer()), *invInfo);
    siteInjected = (violetSite != nullptr && violetSite != battlefield);
    auto& leaders = target->getPlayerCollective()->getLeaders();
    leaderCount = (int) leaders.size();
    if (!leaders.empty()) {
      canXfer = game->canTransferCreature(leaders[0], battlefield);
      if (canXfer) { game->transferCreature(leaders[0], battlefield); transferred = true; }
    }
    game->setWasTransfered();
  });
  (void) foundSite; (void) siteInjected; (void) canXfer; (void) transferred; (void) leaderCount;
  // Sides: role 0 = defender (the primary game's keeper -- Moriaty), role 1 = invader (Violet's keeper on the base).
  Collective* defenderCol = game->getPlayerCollective();   // Moriaty (primary, normal keeper)
  Collective* invaderCol = target->getPlayerCollective();  // Violet's collective, shared into `game` by addInvasionSite
  // The invader needs a PlayerControl IN THE PRIMARY GAME (target's own PC is in the wrong game context and would
  // render Violet's home). Create one for Violet's collective here and point its view at the battlefield where
  // their keeper landed. Created identically on both peers -> deterministic; only the invader's window renders it.
  PlayerControl* invaderPC = nullptr;
  {
    auto ipc = PlayerControl::create(invaderCol, {}, TribeAlignment::EVIL);
    invaderPC = ipc.get();
    invaderCol->setControl(std::move(ipc));
    if (!invaderCol->getLeaders().empty())
      invaderPC->rarSetViewPos(invaderCol->getLeaders()[0]->getPosition());
  }
  PlayerControl* pc = (role == 0) ? game->getPlayerControl() : invaderPC;
  Model* model = battlefield; // the shared battlefield both peers step in lockstep
  PlayerControl* defenderPC = game->getPlayerControl();
  (void) defenderCol; // documented above; inputs route via defenderPC/invaderPC now
  // Lockstep state -- each tick carries a batch of serialized UserInput blobs per player. On apply, each blob is
  // reconstructed and fed to processInput on the ISSUING player's PlayerControl, so both machines run the exact
  // same sequence of actions against the shared battlefield.
  std::map<int, std::vector<string>> myBlobs;
  std::vector<string> pendingLocal; // sim-affecting input blobs captured since the last tick, sent ahead
  auto applyBoth = [&](const std::vector<string>& mine, const std::vector<string>& theirs) {
    // Fixed order on both machines: role 0's inputs first (-> defenderPC), then role 1's (-> invaderPC).
    const auto& role0 = (role == 0) ? mine : theirs;
    const auto& role1 = (role == 0) ? theirs : mine;
    for (auto& b : role0) rarApplyUserInput(defenderPC, model, view, b);
    for (auto& b : role1) rarApplyUserInput(invaderPC, model, view, b);
  };
  for (int i = 0; i < delay; ++i) { myBlobs[i] = {}; session.net().sendTick(i, ""); } // prime
  int tick = 0;
  double t = model->getLocalTimeDouble();
  // Snapshot the SIM's RNG state here (identical on both peers -- same seed, same save, same init). Rendering
  // between ticks draws from the global Random; we restore this before each step and re-save after, so the
  // simulation's RNG stream stays deterministic and identical on both machines regardless of frame rate.
  std::mt19937 simGen = Random.saveGenerator();
  auto lastStep = std::chrono::steady_clock::now();
  // Fast ticks with a FRACTIONAL sim advance: 100ms/tick x 0.25 turn = the same game speed as 400ms/full-turn,
  // but 4x more render frames per turn (smoother) and 4x lower input latency (delay*100ms instead of delay*400).
  const auto STEP = std::chrono::milliseconds(100);
  const double TURN_PER_TICK = 0.25;
  bool running = true;
  bool stalled = false;
  while (running) {
    // 1. Render + present. pc->render builds the GUI and sets gameReady; refreshView actually DRAWS it and
    //    processes SDL events (queuing input for getAction below). Both must run on the render thread -- this
    //    battle loop runs on the main/render thread, so they do. Without refreshView the window stays black.
    pc->render(view);
    view->refreshView();
    // 2. Drain input. Three buckets: (a) VIEW-ONLY -> applied locally to MY pc only (per-player camera/selection,
    //    no sim effect, never synced); (b) BLOCKED -> ignored (taking direct control = turn-based, and invading);
    //    (c) everything else = a committed SIM-affecting action -> captured, synced, and applied at the tick on
    //    BOTH machines via processInput. That gives "business as usual" (build/place/assign/dev-tools all work),
    //    just with lockstep's input delay.
    for (UserInput input = view->getAction(); input.getId() != UserInputId::IDLE; input = view->getAction()) {
      switch (input.getId()) {
        case UserInputId::EXIT:
          running = false; break;
        // (b) BLOCKED: taking direct control (would flip to turn-based, desync) and invading others mid-battle.
        case UserInputId::TOGGLE_CONTROL_MODE:
        case UserInputId::EXIT_CONTROL_MODE:
        case UserInputId::ACTIVATE_TEAM:
        case UserInputId::GO_TO_ENEMY:
        case UserInputId::VILLAGE_ACTION:
          break;
        // (a) VIEW-ONLY: local per-player state -- camera/level view, selection, info panels, opening menus.
        case UserInputId::SCROLL_STAIRS:
        case UserInputId::SCROLL_TO_HOME:
        case UserInputId::DRAW_WORLD_MAP:
        case UserInputId::MESSAGE_INFO:
        case UserInputId::CREATURE_BUTTON:
        case UserInputId::CREATURE_GROUP_BUTTON:
        case UserInputId::CREATURE_MAP_CLICK:
        case UserInputId::CREATURE_MAP_CLICK_EXTENDED:
        case UserInputId::CREATURE_DRAG:
        case UserInputId::SHOW_HISTORY:
        case UserInputId::WORKSHOP:
        case UserInputId::WORKSHOP_TAB:
        case UserInputId::REFRESH:
          pc->processInput(view, input);
          break;
        // (c) SIM-affecting: capture the input (with the level I'm viewing) and sync it. If rarSerializeUserInput
        //     returns a blob it's a synced action -> queued for both machines; otherwise it's not synced -> ignore.
        default:
          if (auto blob = rarSerializeUserInput(input, pc->rarGetViewLevelId()))
            pendingLocal.push_back(std::move(*blob));
          break;
      }
      if (!running) break;
    }
    // 3. Advance the sim ONE tick at the fixed cadence, but only when the peer's command for this tick is here.
    if (std::chrono::steady_clock::now() - lastStep >= STEP) {
      // send our input blobs for the tick `delay` ahead (batch captured since last tick)
      myBlobs[tick + delay] = pendingLocal;
      session.net().sendTick(tick + delay, rarPackBlobs(pendingLocal));
      pendingLocal.clear();
      std::string remoteRaw;
      if (session.net().tryGetRemote(tick, remoteRaw)) {
        Random.restoreGenerator(simGen); // resume the deterministic sim stream (discard rendering's Random draws)
        applyBoth(myBlobs[tick], rarUnpackBlobs(remoteRaw));
        myBlobs.erase(tick);
        t += TURN_PER_TICK;
        int drained = 0;
        while (model->update(t) && ++drained < 100000) {}
        simGen = Random.saveGenerator(); // preserve the sim RNG for the next tick, before rendering perturbs it
        ++tick;
        lastStep = std::chrono::steady_clock::now();
        stalled = false;
      } else {
        if (!session.net().connected()) { view->presentText(TString("Live PvP"_s), TString("The other player disconnected."_s)); running = false; }
        else if (!stalled) { stalled = true; /* waiting for peer; keep rendering */ }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(3)); // don't busy-spin
  }
  session.net().close();
}

// RAR Phase A: fetch the server's pre-generated villain map for a world position and load it into a live
// Model (transport: lzma -> raw -> re-gzip -> loadRetiredModelFromFile). Called on demand by Game::chooseSite
// when the player travels to a villain that wasn't generated at start. Returns null on any failure.
PModel MainLoop::rarLoadVillainModel(Vec2 pos) {
  string key = toString(pos.x) + "_" + toString(pos.y);
  PModel result;
  doWithSplash(TString("Approaching enemy territory..."_s), [&] {
    string blob;
    if (!rarFetchVillain(key, blob) || blob.empty())
      return;
    string raw = rarLzmaDecompress(blob);
    if (raw.empty())
      return;
    FilePath tmp = userPath.file("rar_villain" + getSaveSuffix(GameSaveType::RETIRED_SITE));
    { ogzstream out(tmp.getPath()); out.write(raw.data(), raw.size()); }
    auto info = loadRetiredModelFromFile(tmp);
    tmp.erase();
    if (info && info->model)
      result = PModel(std::move(info->model));
  });
  return result;
}

void MainLoop::compressSelfTest(const string& inFile) {
  // inFile = a gzip .sit (e.g. an old rar_dungeons/*.dat). Exercise the full transport transcode.
  igzstream gzin(inFile.c_str());
  string raw((std::istreambuf_iterator<char>(gzin)), std::istreambuf_iterator<char>());
  gzin.close();
  std::cout << "[cmp] gunzipped raw=" << raw.size() << "\n";
  string blob = rarLzmaCompress(raw);
  std::cout << "[cmp] lzma blob=" << blob.size() << "\n";
  string raw2 = rarLzmaDecompress(blob);
  std::cout << "[cmp] lzma round-trip raw2=" << raw2.size() << " match=" << (raw == raw2 ? "YES" : "NO") << "\n";
  FilePath tmp = userPath.file("rar_cmp_test.sit");
  { ogzstream out(tmp.getPath()); out.write(raw2.data(), raw2.size()); }
  auto info = loadSavedGameInfo(tmp);
  std::cout << "[cmp] re-gzip loads as .sit: " << (info ? "YES" : "NO") << "\n";
  tmp.erase();
  std::cout.flush();
}

void MainLoop::modSyncSelfTest(const string& modName) {
  string bundle = bundleMod(modName);
  string h1 = rarSha256Hex(bundle);
  std::cout << "[mod-test] bundled '" << modName << "' " << bundle.size() << " bytes hash=" << h1 << "\n";
  unbundleMod(modName + "_rtcopy", bundle);
  string h2 = rarSha256Hex(bundleMod(modName + "_rtcopy"));
  std::cout << "[mod-test] reinstalled '" << modName << "_rtcopy' hash=" << h2 << " -> "
            << (h1 == h2 ? "MATCH (round-trip OK)" : "MISMATCH") << "\n";
  std::cout.flush();
}

bool MainLoop::syncServerMods() {
  modsChangedThisSync = false;
  if (!rarEnabled())
    return true;
  std::vector<std::pair<std::string, std::string>> manifest;
  if (!rarFetchModManifest(manifest))
    return true; // server has no /mods (older server) -> nothing required, proceed
  vector<string> required;
  for (auto& elem : manifest) {
    const string& name = elem.first;
    const string& wantHash = elem.second;
    required.push_back(name);
    // HASH FIRST: only touch the disk if the installed mod's bytes don't already match the server's. A client
    // that's already in sync does zero downloads and (below) skips the tileset reload entirely.
    string localHash;
    if (modsDir.subdirectory(name).exists())
      localHash = rarSha256Hex(bundleMod(name)); // recompute the installed mod's hash to detect tampering
    if (localHash != wantHash) {
      string bundle;
      bool ok = false;
      doWithSplash(TString("Downloading required mod: "_s + name), 1,
          [&] (ProgressMeter&) { ok = rarFetchModBundle(name, bundle); });
      if (!ok) {
        view->presentText(none, TString("Failed to download required mod from the server: "_s + name));
        return false;
      }
      unbundleMod(name, bundle);
      modsChangedThisSync = true; // a mod's bytes changed on disk
    }
  }
  // Match the client's active-mod set exactly to the server's (order preserved) -- but only WRITE it if it
  // actually differs, so an already-synced client leaves options + tileset untouched.
  if (options->getVectorStringValue(OptionId::CURRENT_MOD2) != required) {
    options->setValue(OptionId::CURRENT_MOD2, required);
    modsChangedThisSync = true;
  }
  return true;
}

// RAR: extract a keeper's base MODEL ONLY (tiles/furniture/creatures/z-levels + its avatar id) with NO
// ContentFactory/settings, so it survives across builds with incompatible settings (ContentIds serialize
// as strings -> resolve by name in any build). The full-blob load still needs a compatible CampaignInfo
// layout to READ the source; the OUTPUT is settings-free and version-robust.
void MainLoop::exportBase(const string& gameId, const string& outFile) {
  options->setValue(OptionId::CURRENT_MOD2, vector<string>(rarModsInLoadOrder(modsDir.getPath()))); // folder mods, in load order
  auto contentFactory = createContentFactory(false);
  std::ifstream in("rar_dungeons/" + gameId + ".dat", std::ios::binary);
  if (!in) { std::cout << "[export] no server blob rar_dungeons/" << gameId << ".dat\n"; std::cout.flush(); return; }
  std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
  std::string raw = rarLzmaDecompress(blob);
  if (raw.empty()) { std::cout << "[export] lzma decode failed\n"; std::cout.flush(); return; }
  FilePath t = userPath.file("rar_exportbase" + getSaveSuffix(GameSaveType::KEEPER));
  { ogzstream gz(t.getPath()); gz.write(raw.data(), raw.size()); }
  optional<PGame> game;
  try { game = loadFromFile<PGame>(t); } catch (...) { game = none; }
  t.erase();
  if (!game || !*game) {
    std::cout << "[export] FAILED to load the keeper blob for " << gameId
              << " -- saved by an INCOMPATIBLE build (CampaignInfo layout). Revert CampaignInfo to that "
                 "layout to read it.\n"; std::cout.flush(); return;
  }
  auto model = (*game)->getMainModel().giveMeSharedPointer();
  string avatarId = (*game)->getAvatarId();
  int nc = (*game)->getMainModel()->getAllCreatures().size();
  std::ofstream out(outFile, std::ios::binary);
  try {
    OutputArchive ar(out); string tag = "RARBASE1"; ar(tag); ar(saveVersion); ar(avatarId); ar(model);
  } catch (std::exception& e) {
    // Most commonly a bad outFile path (e.g. a bash-style /c/... path the Windows ofstream can't open) or a
    // full disk -> the stream write fails. Report instead of crashing with an uncaught exception.
    std::cout << "[export] write FAILED: " << e.what() << " -- use a Windows-style outFile path (C:/dir/file)\n";
    std::cout.flush(); return;
  }
  std::cout << "[export] wrote model-only base -> " << outFile << " (avatar '" << avatarId << "', "
            << nc << " creatures). Settings-free / version-robust.\n"; std::cout.flush();
}

// RAR: import a model-only base (from exportBase) into a FULLY PLAYABLE keeper: build a throwaway fresh
// keeper game of the same avatar type, then adoptInvadedModel() swaps in the imported model (reusing its
// collective as the PLAYER collective + a fresh PlayerControl). Serialize the reconstructed keeper as the
// target's server blob (current layout) + update its hash so the owner downloads it on Load.
void MainLoop::importBase(const string& inFile, const string& targetGameId) {
  options->setValue(OptionId::CURRENT_MOD2, vector<string>(rarModsInLoadOrder(modsDir.getPath()))); // load order -> matches the gen'd world
  auto contentFactory = createContentFactory(false);
  string tag, avatarId; int ver; shared_ptr<Model> imported;
  { std::ifstream in(inFile, std::ios::binary);
    if (!in) { std::cout << "[import] can't open " << inFile << "\n"; std::cout.flush(); return; }
    InputArchive ar(in); ar(tag); ar(ver); ar(avatarId); ar(imported); }
  if (tag != "RARBASE1" || !imported) { std::cout << "[import] not a valid base file\n"; std::cout.flush(); return; }
  // Strip TIMED lasting-effects from every creature: a creature that was e.g. asleep at export time kept its
  // absolute GlobalTime timeout, which after the world/time reset reads as tens of thousands of turns. Clearing
  // only the timed table (lastingEffects) leaves PERMANENT effects (permanentEffects) untouched.
  { int nCreatures = 0;
    for (Creature* c : imported->getAllCreatures()) {
      for (auto e : ENUM_ALL(LastingEffect))
        c->getAttributes().clearLastingEffect(e);
      ++nCreatures;
    }
    std::cout << "[import] cleared timed lasting-effects on " << nCreatures << " creatures (permanent kept)\n";
    std::cout.flush();
  }
  const KeeperCreatureInfo* ki = nullptr;
  for (auto& p : contentFactory.keeperCreatures) if (p.first == avatarId) { ki = &p.second; break; }
  if (!ki) { std::cout << "[import] unknown avatar '" << avatarId << "' in current content\n"; std::cout.flush(); return; }
  // The keeper's biome comes from its own base model (Model::biomeId is serialized), so a desert keeper
  // re-homes onto desert, etc. -- independent of the export file version.
  BiomeId keeperBiome = imported->getBiomeId();
  // Load the CURRENT shared-world sites (server rar_campaign.dat -- present in cwd after the server chdir) so
  // the reconstructed keeper lands IN the populated world (villains + terrain), not a 1x1 empty campaign.
  Table<Campaign::SiteInfo> worldSites;
  { std::ifstream in("rar_campaign.dat", std::ios::binary);
    if (!in) { std::cout << "[import] no shared world rar_campaign.dat in cwd -- run --rar_gen_world first\n"; std::cout.flush(); return; }
    string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::stringstream ss(bytes); InputArchive ar(ss); ar >> worldSites; }
  string worldName = "RAR World";
  { std::ifstream w("rar_world.txt"); string line, last; while (std::getline(w, line)) if (!line.empty()) last = line;
    if (!last.empty()) worldName = last; }
  // 1. throwaway fresh keeper game of the same avatar type (adopt discards its base but needs a valid game)
  AvatarInfo avatar = getQuickGameAvatar(nullptr, *ki, &contentFactory.getCreatures());
  avatar.avatarId = avatarId;
  RandomGen rnd; rnd.init((int) time(nullptr));
  auto campaignOpt = CampaignBuilder::reconstructKeeperCampaign(rnd, &contentFactory, std::move(worldSites),
      keeperBiome, avatar.playerCreature->getMaxViewIdUpgrade(), avatar.playerCreature->getTribeId(), worldName);
  if (!campaignOpt) {
    std::cout << "[import] no free '" << keeperBiome.data() << "' tile on the shared world for the keeper\n";
    std::cout.flush(); return;
  }
  EnemyFactory ef(Random, contentFactory.getCreatures().getNameGenerator(), contentFactory.enemies,
      contentFactory.buildingInfo, {});
  SokobanInput sok(dataFreePath.file("sokoban_input.txt"), userPath.file("sokoban_state.txt"));
  ModelBuilder mb(nullptr, Random, options, &sok, &contentFactory, std::move(ef));
  PModel freshBase = mb.campaignBaseModel(avatar, keeperBiome, none);
  CampaignSetup setup = CampaignBuilder::getEmptyCampaign();
  setup.campaign = std::move(*campaignOpt); // the real shared-world campaign (keeper placed on a biome tile)
  setup.gameIdentifier = targetGameId; // becomes the game's identifier via the Game ctor
  // Save/load-menu display name: a reconstructed game otherwise has an EMPTY gameDisplayName, so once the
  // keeper is saved locally it shows up as "" in the load menu. Build "<keeper> of <world>" like a normal game.
  for (Collective* col : imported->getCollectives())
    if (col->getVillainType() == VillainType::PLAYER && !col->getLeaders().empty()) {
      setup.gameDisplayName = TSentence("OF", col->getLeaders()[0]->getName().firstOrBare(), TString(worldName));
      break;
    }
  Vec2 basePos = setup.campaign.getPlayerPos();
  std::cout << "[import] placing keeper on shared world at " << basePos.x << "_" << basePos.y
            << " biome '" << keeperBiome.data() << "'\n"; std::cout.flush();
  Table<PModel> models(setup.campaign.getSites().getBounds());
  models[basePos] = std::move(freshBase);
  auto game = Game::campaignGame(std::move(models), setup, std::move(avatar), std::move(contentFactory), {});
  // Diagnostic: list the collectives in the base so we can confirm adopt picks the KEEPER (PLAYER), not an
  // enemy faction living in the dungeon (bandits/zombies/etc.).
  std::cout << "[import] base collectives:";
  for (Collective* col : imported->getCollectives())
    std::cout << " {" << EnumInfo<VillainType>::getString(col->getVillainType())
              << " leaders=" << col->getLeaders().size() << " creatures=" << col->getCreatures().size() << "}";
  std::cout << "\n"; std::cout.flush();
  // 2. swap in the imported model as the (now player-owned) base
  SavedGameInfo si;
  if (!game->adoptInvadedModel(PModel(std::move(imported)), si)) {
    std::cout << "[import] adopt failed (no keeper collective in the base?)\n"; std::cout.flush(); return;
  }
  // 3. serialize the reconstructed keeper (current layout) + write the target server blob + hash
  std::stringstream ss;
  { OutputArchive ar(ss); string name = targetGameId; SavedGameInfo si2;
    ar << saveVersion << name << si2; ar << game; }
  string raw = ss.str();
  // Load-back check BEFORE writing the server blob: reconstruct the game from the exact bytes the client will
  // load, to catch the empty-Table / bad-campaign class of crash HERE (server-side) instead of on the client.
  // (A CHECK failure aborts loudly with a FATAL; a clean failure returns none -- either way we don't deploy.)
  { FilePath t = userPath.file("rar_importcheck" + getSaveSuffix(GameSaveType::KEEPER));
    { ogzstream gz(t.getPath()); gz.write(raw.data(), raw.size()); }
    optional<PGame> check;
    try { check = loadFromFile<PGame>(t); } catch (...) { check = none; }
    t.erase();
    if (!check || !*check) {
      std::cout << "[import] ABORT: reconstructed blob failed to load back -- NOT writing it (would crash the client)\n";
      std::cout.flush(); return;
    }
    // Exercise the exact table that crashed the client (campaign.belowMaxAgressorCutOff via
    // passesMaxAggressorCutOff at the base position): a 0x0-Table campaign aborts HERE (server-side) with the
    // FATAL instead of on the client.
    (void) (*check)->passesMaxAggressorCutOff((*check)->getMainModel().get());
    std::cout << "[import] load-back OK: " << (*check)->getMainModel()->getAllCreatures().size()
              << " creatures in the base, aggressor-cutoff table valid\n"; std::cout.flush();
  }
  string lz = rarLzmaCompress(raw);
  DirectoryPath("rar_dungeons").createIfDoesntExist();
  { std::ofstream out(("rar_dungeons/" + targetGameId + ".dat"), std::ios::binary); out.write(lz.data(), lz.size()); }
  // update rar_dungeonhash.txt so the target's LOCAL save (different hash) triggers the server download
  string hash = rarSha256Hex(raw);
  { std::map<string, string> hm;
    { std::ifstream hin("rar_dungeonhash.txt"); string line;
      while (std::getline(hin, line)) { auto t = line.rfind('\t'); if (t != string::npos) hm[line.substr(0, t)] = line.substr(t + 1); } }
    hm[targetGameId] = hash;
    std::ofstream hout("rar_dungeonhash.txt"); for (auto& e : hm) hout << e.first << '\t' << e.second << '\n'; }
  std::cout << "[import] reconstructed keeper -> rar_dungeons/" << targetGameId << ".dat (" << lz.size()
            << " bytes, hash " << hash.substr(0, 12) << "). Restart the server; owner -> Load to download it.\n";
  std::cout.flush();
}

// RAR: re-home a keeper onto the CURRENT shared world (rar_campaign.dat) WITHOUT the lossy model-only
// export/import. Loads the keeper's FULL server blob (which loads + re-saves cleanly -- everything the model
// references is present, so nothing dangles), swaps ONLY the campaign to the new world (fresh sites + a valid
// keeper tile), and writes it straight back. This is the robust migration for deeply-played bases that the
// model-only pipeline can't extract. Usage: --rar_rehome_keeper 'gameId targetGameId' (targetGameId optional,
// defaults to gameId in place).
void MainLoop::rehomeKeeper(const string& gameId, const string& targetGameId) {
  options->setValue(OptionId::CURRENT_MOD2, vector<string>(rarModsInLoadOrder(modsDir.getPath())));
  auto contentFactory = createContentFactory(false);
  // 1. Load the FULL keeper game from its server blob.
  std::ifstream in("rar_dungeons/" + gameId + ".dat", std::ios::binary);
  if (!in) { std::cout << "[rehome] no server blob rar_dungeons/" << gameId << ".dat\n"; std::cout.flush(); return; }
  std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
  std::string raw = rarLzmaDecompress(blob);
  if (raw.empty()) { std::cout << "[rehome] lzma decode failed\n"; std::cout.flush(); return; }
  FilePath t = userPath.file("rar_rehome" + getSaveSuffix(GameSaveType::KEEPER));
  { ogzstream gz(t.getPath()); gz.write(raw.data(), raw.size()); }
  optional<PGame> game;
  try { game = loadFromFile<PGame>(t); } catch (...) { game = none; }
  t.erase();
  if (!game || !*game) { std::cout << "[rehome] FAILED to load keeper blob for " << gameId << "\n"; std::cout.flush(); return; }
  // 2. Load the NEW shared-world sites (must match the models Table size the keeper was built with).
  Table<Campaign::SiteInfo> worldSites;
  { std::ifstream w("rar_campaign.dat", std::ios::binary);
    if (!w) { std::cout << "[rehome] no rar_campaign.dat -- run --rar_gen_world first\n"; std::cout.flush(); return; }
    std::string bytes((std::istreambuf_iterator<char>(w)), std::istreambuf_iterator<char>());
    std::stringstream ss(bytes); InputArchive ar(ss); ar >> worldSites; }
  string worldName = "RAR World";
  { std::ifstream w("rar_world.txt"); string line, last; while (std::getline(w, line)) if (!line.empty()) last = line;
    if (!last.empty()) worldName = last; }
  // 3. Keeper biome + viewId + tribe from the loaded game (for the new campaign's keeper dweller).
  BiomeId biome = (*game)->getMainModel()->getBiomeId();
  Collective* pcol = (*game)->getPlayerCollective();
  if (!pcol || pcol->getLeaders().empty()) { std::cout << "[rehome] no player keeper leader in the base\n"; std::cout.flush(); return; }
  Creature* keeper = pcol->getLeaders()[0];
  ViewIdList viewId = keeper->getMaxViewIdUpgrade();
  TribeId tribe = keeper->getTribeId();
  // 4. Build the fresh campaign on the new world + swap it in (base model moves to the chosen tile).
  RandomGen rnd; rnd.init((int) time(nullptr));
  auto camp = CampaignBuilder::reconstructKeeperCampaign(rnd, &contentFactory, std::move(worldSites), biome,
      viewId, tribe, worldName);
  if (!camp) { std::cout << "[rehome] no free '" << biome.data() << "' tile on the new world for the keeper\n"; std::cout.flush(); return; }
  Vec2 newPos = camp->getPlayerPos();
  std::cout << "[rehome] placing keeper at " << newPos.x << "_" << newPos.y << " biome '" << biome.data() << "'\n"; std::cout.flush();
  (*game)->rehomeToNewWorld(std::move(*camp), newPos);
  // 5. Serialize the FULL game -> target server blob (+ hash), with a load-back guard before writing.
  const string& outId = targetGameId.empty() ? gameId : targetGameId;
  std::stringstream ss;
  { OutputArchive ar(ss); string name = outId; SavedGameInfo si; ar << saveVersion << name << si; ar << *game; }
  string outRaw = ss.str();
  { FilePath tc = userPath.file("rar_rehomecheck" + getSaveSuffix(GameSaveType::KEEPER));
    { ogzstream gz(tc.getPath()); gz.write(outRaw.data(), outRaw.size()); }
    optional<PGame> check;
    try { check = loadFromFile<PGame>(tc); } catch (...) { check = none; }
    tc.erase();
    if (!check || !*check) { std::cout << "[rehome] ABORT: re-homed blob failed load-back -- NOT writing it\n"; std::cout.flush(); return; }
    (void) (*check)->passesMaxAggressorCutOff((*check)->getMainModel().get());
    std::cout << "[rehome] load-back OK: " << (*check)->getMainModel()->getAllCreatures().size()
              << " creatures, world swapped, aggressor table valid\n"; std::cout.flush();
  }
  string lz = rarLzmaCompress(outRaw);
  DirectoryPath("rar_dungeons").createIfDoesntExist();
  { std::ofstream out("rar_dungeons/" + outId + ".dat", std::ios::binary); out.write(lz.data(), lz.size()); }
  string hash = rarSha256Hex(outRaw);
  { std::map<string, string> hm;
    { std::ifstream hin("rar_dungeonhash.txt"); string line;
      while (std::getline(hin, line)) { auto tt = line.rfind('\t'); if (tt != string::npos) hm[line.substr(0, tt)] = line.substr(tt + 1); } }
    hm[outId] = hash;
    std::ofstream hout("rar_dungeonhash.txt"); for (auto& e : hm) hout << e.first << '\t' << e.second << '\n'; }
  // Move the keeper's world-map claim to the new tile (rar_claims.txt: "x\ty\tlogin\tgameId\t...\tname") so
  // others invade at the right coords and the map sprite matches the base's actual new position.
  { vector<string> lines; bool found = false;
    { std::ifstream cin("rar_claims.txt"); string line;
      while (std::getline(cin, line)) {
        vector<string> f; size_t p = 0, q;
        while ((q = line.find('\t', p)) != string::npos) { f.push_back(line.substr(p, q - p)); p = q + 1; }
        f.push_back(line.substr(p));
        if (f.size() >= 4 && f[3] == outId) {
          f[0] = toString(newPos.x); f[1] = toString(newPos.y); found = true;
          string rebuilt; for (size_t i = 0; i < f.size(); ++i) rebuilt += (i ? "\t" : "") + f[i];
          line = rebuilt;
        }
        lines.push_back(line);
      } }
    if (found) { std::ofstream cout2("rar_claims.txt"); for (auto& l : lines) cout2 << l << "\n"; }
    std::cout << "[rehome] claim " << (found ? "updated" : "NOT found") << " for " << outId
              << " -> " << newPos.x << "_" << newPos.y << "\n"; std::cout.flush();
  }
  std::cout << "[rehome] re-homed keeper -> rar_dungeons/" << outId << ".dat (" << lz.size()
            << " bytes, hash " << hash.substr(0, 12) << "). Restart the server; owner -> Load.\n";
  std::cout.flush();
}

// RAR: end-to-end self-test of the base export/import pipeline (no existing keeper needed): build a fresh
// keeper -> export model-only -> import (reconstruct) -> load the reconstructed blob.
void MainLoop::rarBaseSelfTest() {
  auto cf = createContentFactory(false);
  if (cf.keeperCreatures.empty()) { std::cout << "[selftest] no keeper creatures\n"; return; }
  string avatarId = cf.keeperCreatures[0].first;
  auto ki = cf.keeperCreatures[0].second;
  AvatarInfo avatar = getQuickGameAvatar(nullptr, ki, &cf.getCreatures());
  avatar.avatarId = avatarId;
  EnemyFactory ef(Random, cf.getCreatures().getNameGenerator(), cf.enemies, cf.buildingInfo, {});
  SokobanInput sok(dataFreePath.file("sokoban_input.txt"), userPath.file("sokoban_state.txt"));
  ModelBuilder mb(nullptr, Random, options, &sok, &cf, std::move(ef));
  BiomeId biome = cf.biomeInfo.begin()->first;
  PModel base = mb.campaignBaseModel(avatar, biome, none);
  CampaignSetup setup = CampaignBuilder::getEmptyCampaign();
  setup.gameIdentifier = "selftestsrc";
  Table<PModel> models(setup.campaign.getSites().getBounds());
  models[setup.campaign.getPlayerPos()] = std::move(base);
  auto game = Game::campaignGame(std::move(models), setup, std::move(avatar), std::move(cf), {});
  std::cout << "[selftest] built fresh keeper OK (base creatures="
            << game->getMainModel()->getAllCreatures().size() << ", avatar '" << avatarId << "')\n"; std::cout.flush();
  auto model = game->getMainModel().giveMeSharedPointer();
  { std::ofstream out("rar_selftest.base", std::ios::binary);
    OutputArchive ar(out); string tag = "RARBASE1"; ar(tag); ar(saveVersion); ar(avatarId); ar(model); }
  std::cout << "[selftest] exported model-only base\n"; std::cout.flush();
  game = nullptr;
  importBase("rar_selftest.base", "selftestkeeper");
  std::ifstream in("rar_dungeons/selftestkeeper.dat", std::ios::binary);
  if (!in) { std::cout << "[selftest] import produced no blob -> FAILED\n"; return; }
  std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
  std::string raw = rarLzmaDecompress(blob);
  FilePath t = userPath.file("rar_selfload" + getSaveSuffix(GameSaveType::KEEPER));
  { ogzstream gz(t.getPath()); gz.write(raw.data(), raw.size()); }
  optional<PGame> loaded; try { loaded = loadFromFile<PGame>(t); } catch (...) {}
  t.erase();
  std::cout << "[selftest] reconstructed-blob load: "
            << (loaded && *loaded ? "OK -- FULL CYCLE PASSED" : "FAILED") << "\n"; std::cout.flush();
  remove("rar_selftest.base"); remove("rar_dungeons/selftestkeeper.dat");
}

void MainLoop::previewLayout(const string& layoutName, const string& sizeStr) {
  // Dev tool: interactive offline layout generator. A graphical map preview (real tile sprites + drag-pan)
  // on the left; a control panel on the right (type, X/Y size, scrollable layout list). Picking a layout /
  // changing size / rerolling regenerates. World-map type is auto-detected: the layout's block in
  // random_layouts.txt contains ALL of FOREST, SNOW, MOUNTAIN, DESERT.
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath()); // mods/load_order.txt decides the order
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  if (tileSet) { // load this content's sprite paths so terrain renders with graphics, not glyphs
    tileSet->setTilePaths(factory.tilePaths);
    tileSet->loadTextures();
  }
  vector<string> layouts;
  for (auto& elem : factory.randomLayouts)
    layouts.push_back(elem.first.data());
  std::sort(layouts.begin(), layouts.end());
  if (layouts.empty()) {
    view->presentText(none, TString("No layouts found in random_layouts.txt."_s));
    return;
  }
  // All layout_mapping.txt mappings (dungeon token -> furniture). Usually just "default"; mods can add more.
  vector<string> mappings;
  for (auto& elem : factory.layoutMapping)
    mappings.push_back(elem.first.data());
  std::sort(mappings.begin(), mappings.end());
  // Vanilla AND every active mod's random_layouts.txt. This text is only used to CLASSIFY each layout as a
  // world map or a dungeon (see isWorldMap below), but reading vanilla alone meant a modded world map found
  // no text for itself, failed the four-biome-token test, and was filed as a DUNGEON -- so it never appeared
  // in the world-map list and the tool looked like it wasn't loading mods at all. It was; it just hid them.
  // Mod Defs (macros a modded layout calls) have to be here for the same reason.
  string configText;
  if (auto c = dataFreePath.subdirectory("game_config").file("random_layouts.txt").readContents())
    configText = *c;
  int modLayoutFiles = 0;
  for (auto& mod : folderMods)
    if (auto c = modsDir.subdirectory(mod).file("random_layouts.txt").readContents()) {
      configText += "\n" + *c;   // newline: a file not ending in one would glue its last token to the next
      ++modLayoutFiles;
    }
  // The biome tokens (FOREST/SNOW/MOUNTAIN/DESERT) don't live in a layout's own block -- they're Set() in
  // `Def WorldMapFeatures()/WorldMapTrees()` macros the layout CALLS. So parse all "Def Name(...) ... End"
  // bodies, then for a layout follow its calls (recursively) and check the combined text for all 4 tokens.
  map<string, string> defs;
  for (size_t p = 0; (p = configText.find("Def ", p)) != string::npos; ) {
    size_t nameStart = p + 4;
    size_t paren = configText.find('(', nameStart);
    size_t bodyStart = (paren == string::npos) ? string::npos : configText.find(')', paren);
    if (bodyStart == string::npos) break;
    string name = configText.substr(nameStart, paren - nameStart);
    name.erase(std::remove_if(name.begin(), name.end(), [](char c){ return std::isspace((unsigned char) c); }), name.end());
    size_t end = configText.find("\nEnd", bodyStart);
    if (end == string::npos) end = configText.size();
    defs[name] = configText.substr(bodyStart + 1, end - bodyStart - 1);
    p = end + 4;
  }
  auto blockOf = [&](const string& name) -> string { // the "name" { ... } block (balanced braces)
    auto pos = configText.find("\"" + name + "\"");
    if (pos == string::npos) return "";
    auto brace = configText.find('{', pos);
    if (brace == string::npos) return "";
    int depth = 0; size_t end = brace;
    for (size_t i = brace; i < configText.size(); ++i) {
      if (configText[i] == '{') ++depth;
      else if (configText[i] == '}') { if (--depth == 0) { end = i; break; } }
    }
    return configText.substr(brace, end - brace + 1);
  };
  auto isWorldMap = [&](const string& name) { // 99% signal: layout (with its Def calls) sets all 4 biomes
    string combined;
    set<string> visited;
    vector<string> stack{ blockOf(name) };
    while (!stack.empty()) {
      string text = std::move(stack.back()); stack.pop_back();
      combined += text;
      for (size_t i = 0; i < text.size(); ) { // find every identifier immediately followed by '('
        if (std::isalpha((unsigned char) text[i]) || text[i] == '_') {
          size_t j = i;
          while (j < text.size() && (std::isalnum((unsigned char) text[j]) || text[j] == '_')) ++j;
          size_t k = j;
          while (k < text.size() && std::isspace((unsigned char) text[k])) ++k;
          if (k < text.size() && text[k] == '(') {
            string id = text.substr(i, j - i);
            if (defs.count(id) && visited.insert(id).second)
              stack.push_back(defs.at(id));
          }
          i = j;
        } else ++i;
      }
    }
    return combined.find("FOREST") != string::npos && combined.find("SNOW") != string::npos &&
           combined.find("MOUNTAIN") != string::npos && combined.find("DESERT") != string::npos;
  };
  // Classify every layout by TYPE up front. The type toggle then FILTERS the list: World map shows only
  // layouts with all 4 biome tokens; Dungeon shows the rest (a future dungeon renderer will use these).
  vector<string> worldMapLayouts, dungeonLayouts;
  for (auto& name : layouts)
    (isWorldMap(name) ? worldMapLayouts : dungeonLayouts).push_back(name);
  View::LayoutPreviewInfo info;
  if (layouts.contains(layoutName)) { // honor the passed layout -> its own type
    info.current = layoutName;
    info.worldMap = isWorldMap(layoutName);
  } else {                            // no valid layout passed -> default to world map (or dungeon if none)
    info.worldMap = !worldMapLayouts.empty();
    info.current = info.worldMap ? worldMapLayouts[0] : dungeonLayouts[0];
  }
  info.layouts = info.worldMap ? worldMapLayouts : dungeonLayouts;
  // Say out loud what got loaded and how it was classified -- "my mod isn't showing up" is otherwise
  // indistinguishable from "my mod was filed under the other type". Visible with --console.
  std::cout << "[map-editor] " << layouts.size() << " layout(s) from vanilla + " << modLayoutFiles
      << " mod file(s): " << worldMapLayouts.size() << " world map(s), " << dungeonLayouts.size()
      << " dungeon(s)\n  world maps:";
  for (auto& n : worldMapLayouts)
    std::cout << " " << n;
  std::cout << "\n";
  std::cout.flush();
  auto parts = split(sizeStr, {':'});
  if (parts.size() == 2) { info.sizeX = max(1, fromString<int>(parts[0])); info.sizeY = max(1, fromString<int>(parts[1])); }
  // Starting render zoom per type: dungeon renders in 24px cells so zoom 1 is already native; the world map
  // uses 8px cells and starts at the config zoom. Mouse wheel adjusts it; toggling type resets to the default.
  auto defaultZoom = [&](bool wm) { return wm ? factory.campaignInfo.mapZoom : 1; };
  info.zoom = defaultZoom(info.worldMap);
  info.mappings = mappings;
  info.mapping = mappings.contains("default") ? "default" : (mappings.empty() ? "" : mappings[0]);
  RandomGen random;
  int seed = (int) time(nullptr);
  // Layout errors become exceptions for the lifetime of this menu instead of exiting the program.
  LayoutErrorsThrowScope layoutErrorScope;
  string lastLayoutError;
  while (true) {
    info.seed = seed;  // show the seed this map came from -- feed it to --rar_gen_world to rebuild the terrain
    random.init(seed); // re-seed with the CURRENT seed each iteration: same seed+params => same map, so
                       // zooming keeps the map stable. Only Reroll bumps the seed for a fresh layout.
    // Generate whichever layout is selected (world map OR dungeon) into stacked tile layers. none if the
    // layout can't be generated at this size -> the menu shows a "couldn't generate" placeholder.
    // A layout that overruns the map (or is misconfigured) would normally be a FATAL user error and kill the
    // program -- see layout_canvas.h. In here it is a catchable LayoutGenerationError instead, so a bad size
    // just reports itself and leaves the menu up to try another one. Only report a CHANGED message: the loop
    // regenerates on every pass, so re-showing the identical error would pop a dialog on each redraw.
    optional<Campaign> campaign;
    try {
      campaign = CampaignBuilder::previewLayoutCampaign(random, &factory, info.current,
          Vec2(info.sizeX, info.sizeY), info.worldMap, info.zoom, info.mapping);
      lastLayoutError.clear();
    } catch (const LayoutGenerationError& e) {
      campaign = none;
      if (e.message != lastLayoutError) {
        lastLayoutError = e.message;
        view->presentText(none, TString(e.message +
            "\n\nThis layout can't be generated at " + toString(info.sizeX) + "x" + toString(info.sizeY) +
            ". Change the map size (or pick another layout) and try again."_s));
      }
    }
    auto action = view->previewLayoutMenu(campaign ? &*campaign : nullptr, info);
    using Id = View::LayoutPreviewActionId;
    switch (action.id) {
      case Id::CLOSE: return;
      case Id::REROLL: ++seed; break;
      case Id::EDIT_X:
        if (auto n = view->getNumber(TString("Map width (X)"_s), Range(1, 500), info.sizeX)) info.sizeX = *n;
        break;
      case Id::EDIT_Y:
        if (auto n = view->getNumber(TString("Map height (Y)"_s), Range(1, 500), info.sizeY)) info.sizeY = *n;
        break;
      case Id::EDIT_SEED:
        // Type a seed to jump straight back to a map you liked (or one --rar_gen_world reported).
        if (auto n = view->getNumber(TString("Seed"_s), Range(0, 2000000000), seed)) seed = *n;
        break;
      case Id::TOGGLE_TYPE:
        info.worldMap = !info.worldMap;
        info.layouts = info.worldMap ? worldMapLayouts : dungeonLayouts;
        if (!info.layouts.empty())
          info.current = info.layouts[0]; // jump to the first layout of the newly-selected type
        info.zoom = defaultZoom(info.worldMap); // reset zoom to the new type's native scale
        break;
      case Id::SELECT_LAYOUT:
        info.current = action.layoutName; // type is the filter now, so selecting doesn't change it
        break;
      case Id::ZOOM_IN: info.zoom = min(info.zoom + 1, 8); break;
      case Id::ZOOM_OUT: info.zoom = max(info.zoom - 1, 1); break;
      case Id::SELECT_MAPPING: info.mapping = action.layoutName; break; // layoutName field carries the picked name
    }
  }
}

void MainLoop::repairVillains(const string& campaignFile) {
  // Regenerate any MISSING villain .dat blobs for villains still on the world map, WITHOUT changing the world
  // layout: positions/enemyIds/biomes come straight from the existing campaign, so keeper claims stay valid.
  // Produces the exact same map genServerWorld would (EVIL alignment, difficulty 0). Fixes orphaned roster
  // entries whose blob was deleted (e.g. by the old defeat path) so travelling to them no longer 404s/crashes.
  vector<string> folderMods = rarModsInLoadOrder(modsDir.getPath()); // mods/load_order.txt decides the order
  options->setValue(OptionId::CURRENT_MOD2, folderMods);
  auto factory = createContentFactory(false);
  Table<Campaign::SiteInfo> sites;
  { ifstream in(campaignFile, std::ios::binary);
    if (!in) { std::cout << "[rar-repair] can't open " << campaignFile << "\n"; std::cout.flush(); return; }
    string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::stringstream ss(bytes);
    InputArchive ar(ss);
    ar >> sites;
  }
  DirectoryPath villainDir("rar_villains");
  villainDir.createIfDoesntExist();
  RandomGen random;
  random.init((int) time(nullptr));
  EnemyFactory villainEnemies(Random, factory.getCreatures().getNameGenerator(), factory.enemies,
      factory.buildingInfo, {});
  SokobanInput villainSokoban(dataFreePath.file("sokoban_input.txt"), userPath.file("sokoban_state.txt"));
  ModelBuilder villainBuilder(nullptr, random, options, &villainSokoban, &factory, std::move(villainEnemies));
  auto genVillainBlob = [&] (EnemyId enemyId, VillainType type, BiomeId biome) -> string {
    PModel model = villainBuilder.campaignSiteModel(enemyId, type, TribeAlignment::EVIL, biome, 0);
    for (Level* l : model->getLevels())
      l->clearSectors();
    SavedGameInfo info;
    info.name = enemyId.data();
    info.progressCount = 1;
    info.retiredEnemyInfo = SavedGameInfo::RetiredEnemyInfo{enemyId, type};
    return rarLzmaCompress(serializeModelRaw(model.giveMeSharedPointer(), info, &factory));
  };
  int repaired = 0, present = 0;
  for (Vec2 v : sites.getBounds())
    if (auto villain = sites[v].getVillain()) {
      if (!sites[v].biome)
        continue;
      string key = toString(v.x) + "_" + toString(v.y);
      { ifstream f(villainDir.file(key + ".dat").getPath(), std::ios::binary);
        if (f.good() && f.peek() != std::ifstream::traits_type::eof()) { ++present; continue; } }
      string blob = genVillainBlob(villain->enemyId, villain->type, *sites[v].biome);
      ofstream out(villainDir.file(key + ".dat").getPath(), std::ios::binary);
      out.write(blob.data(), blob.size());
      ++repaired;
      std::cout << "[rar-repair] regenerated " << key << " '" << villain->enemyId.data() << "' " << blob.size()
                << "b\n"; std::cout.flush();
    }
  std::cout << "[rar-repair] done: " << repaired << " regenerated, " << present << " already present\n";
  std::cout.flush();
}

void MainLoop::testServerWorld(const string& inFile) {
  ifstream in(inFile, std::ios::binary);
  string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  std::cout << "[rar-test] read " << bytes.size() << " bytes from " << inFile << "\n";
  std::stringstream ss(bytes);
  Table<Campaign::SiteInfo> sites;
  {
    InputArchive ar(ss);
    ar >> sites;
  }
  printWorldSummary("[rar-test] deserialized", sites);
}

GameConfig MainLoop::getGameConfig(const vector<string>& modNames) const {
  return GameConfig(concat({getVanillaDir()}, modNames
      .transform([&](const string& name) { return modsDir.subdirectory(name); })));
}

// --dump_tribes: print the COMPLETE friend/foe matrix that tribes.txt produces, one line per tribe listing
// every tribe it is hostile to. Built with Tribe::generateTribes -- the same function the game runs on -- so
// this is the real graph, not a re-reading of the file. tribes.txt is easy to rewrite and impossible to eyeball
// (enemies is mutual, enemyOfAll is implicit, allies are applied last): dump before a change, dump after, diff.
void MainLoop::dumpTribes() {
  std::cout.flush();
  ContentFactory factory;
  auto mods = getCurrentMods();
  auto config = getGameConfig(mods);
  if (auto err = factory.readData(&config, mods)) {
    std::cout << "CONTENT ERROR: " << *err << "\n";
    std::cout.flush();
    return;
  }
  auto graph = Tribe::generateTribes(factory.tribes);
  vector<string> names;
  for (auto& elem : factory.tribes)
    names.push_back(elem.first.data());
  sort(names.begin(), names.end());
  std::cout << "tribes: " << names.size() << "  (mods: " << mods << ")\n";
  for (auto& a : names) {
    auto ita = graph.find(TribeId(a.c_str()));
    vector<string> enemies;
    for (auto& b : names)
      if (a != b) {
        auto itb = graph.find(TribeId(b.c_str()));
        if (ita != graph.end() && itb != graph.end() && ita->second->isEnemy(itb->second.get()))
          enemies.push_back(b);
      }
    // Self-hostility is the classic corrupted-graph symptom, so state it explicitly rather than hide it.
    bool selfEnemy = ita != graph.end() && ita->second->isEnemy(ita->second.get());
    std::cout << "  " << a << (selfEnemy ? "  [SELF-HOSTILE!]" : "") << "\n      enemies(" << enemies.size()
        << "): " << enemies << "\n";
  }
  std::cout.flush();
}

// --dump_workshops [group]: load content WITH the active mods and print each workshop group's recipe list.
// Answers "did my mod's modify/append actually land?" without starting a game.
void MainLoop::dumpWorkshops(const string& group) {
  std::cout << "modsDir: " << modsDir.getPath() << "\n";
  std::cout << "CURRENT_MOD2 raw:";
  for (auto& m : options->getVectorStringValue(OptionId::CURRENT_MOD2))
    std::cout << " [" << m << "]";
  std::cout << "\nafter version filter:";
  for (auto& m : getCurrentMods())
    std::cout << " [" << m << "]";
  std::cout << "\n";
  std::cout.flush();
  // NOT createContentFactory(): that reports a bad mod through USER_INFO, which needs a View. We are
  // headless (view == nullptr), so it would segfault and the error -- the thing we actually want -- is
  // lost along with the buffered stdout. Do the two steps ourselves and print the message.
  ContentFactory factory;
  {
    auto mods = getCurrentMods();
    auto config = getGameConfig(mods);
    if (auto err = factory.readData(&config, mods)) {
      std::cout << "CONTENT ERROR with mods " << mods << ":\n  " << *err << "\n";
      std::cout << "(retrying with vanilla only)\n";
      std::cout.flush();
      factory = ContentFactory();
      auto vanilla = getGameConfig({});
      if (auto err2 = factory.readData(&vanilla, {}))
        std::cout << "VANILLA ALSO BROKEN: " << *err2 << "\n";
      else
        std::cout << "vanilla content OK -- the failure is in the mod(s) above\n";
      std::cout.flush();
      return;
    }
  }
  for (auto& g : factory.workshopGroups) {
    if (!group.empty() && g.first != group)
      continue;
    std::cout << "=== group \"" << g.first << "\"\n";
    for (auto& ws : g.second) {
      std::cout << "  " << ws.first.data() << " (" << ws.second.size() << " recipes)\n";
      for (auto& item : ws.second) {
        auto resolved = item.get(ws.first, &factory);
        std::cout << "      " << resolved.name << "  cost=" << resolved.cost.id.data() << " "
            << resolved.cost.value << (resolved.techId ? "  tech="_s + resolved.techId->data() : ""_s) << "\n";
      }
    }
  }
  std::cout.flush();
}

ContentFactory MainLoop::createContentFactory(bool vanillaOnly) const {
  ContentFactory ret;
  auto tryConfig = [&](const vector<string>& modNames) {
    ret = ContentFactory();
    auto config = getGameConfig(modNames);
    return ret.readData(&config, modNames);
  };
  if (vanillaOnly) {
#ifdef RELEASE
    if (auto err = tryConfig({}))
      USER_FATAL << "Error loading vanilla game data: " << *err;
#else
    while (true) {
      if (auto err = tryConfig({}))
        USER_INFO << "Error loading vanilla game data: " << *err;
      else
        break;
    }
#endif
  } else {
    auto chosenMod = getCurrentMods();
    if (auto err = tryConfig(chosenMod)) {
      USER_INFO << "Error loading mod \"" << chosenMod << "\": " << *err << "\n\nUsing vanilla game data";
      if (auto err = tryConfig({}))
        USER_FATAL << "Error loading vanilla game data: " << *err;
    }
  }
  return ret;
}

vector<SaveFileInfo> MainLoop::getSaveOptions(const vector<GameSaveType>& games) {
  vector<SaveFileInfo> ret;
  for (auto elem : games) {
    vector<SaveFileInfo> files = getSaveFiles(userPath, getSaveSuffix(elem));
    files = files.filter([this] (const SaveFileInfo& info) { return isCompatible(getSaveVersion(info));});
    append(ret, files);
  }
  return ret;
}

void MainLoop::launchQuickGame(optional<int> maxTurns, optional<string> keeperName) {
  PGame game;
  tileSet->clear();
  auto contentFactory = createContentFactory(true);
  if (tileSet)
    tileSet->setTilePaths(contentFactory.tilePaths);
  tileSet->loadTextures();
  if (!keeperName) {
    auto files = getSaveOptions({GameSaveType::AUTOSAVE, GameSaveType::KEEPER});
    auto toLoad = std::min_element(files.begin(), files.end(),
        [](const auto& f1, const auto& f2) { return f1.date > f2.date; });
    if (toLoad != files.end()) {
      auto path = userPath.file((*toLoad).filename);
      game = loadGame(path, getNameAndVersion(path)->first);
    } else
      return;
  } else {
    auto& keeperCreature = [&] ()-> const KeeperCreatureInfo& {
      for (auto& elem : contentFactory.keeperCreatures)
        if (elem.first == keeperName)
          return elem.second;
      USER_FATAL << "keeper not found " << *keeperName;
      fail();
    }();
    AvatarInfo avatar = getQuickGameAvatar(view, keeperCreature, &contentFactory.getCreatures());
    CampaignBuilder builder(view, Random, options, contentFactory.villains, contentFactory.gameIntros, avatar);
    auto result = builder.prepareCampaign(&contentFactory, bindMethod(&MainLoop::getRetiredGames, this),
        CampaignType::QUICK_MAP, "Jarnsaxaland");
    auto models = prepareCampaignModels(*result, std::move(avatar), Random, &contentFactory);
    game = Game::campaignGame(std::move(models.models), *result, std::move(avatar), std::move(contentFactory), {});
    dumpMemUsage(game);
  }
  playGame(std::move(game), true, false, nullptr, milliseconds{3}, maxTurns);
}

// A message wall: whatever plain text sits in game_config/<name>.txt is shown once in a scrolling window,
// using the scripted UI of the same name. Deliberately NOT run through the content parser -- it is raw text,
// so an admin can drop announcements in without worrying about quoting, braces or ids. Empty or missing file
// => nothing is shown at all. Each line becomes its own row so blank lines and line breaks survive; one big
// Paragraph would reflow them into a wall of prose.
//
// MODS CAN REPLACE IT: the last active mod that ships mods/<mod>/<name>.txt wins, otherwise vanilla's copy is
// used. Override rather than append, because the point is to UPDATE the text -- appending would leave the old
// announcement stuck above the new one with no way to remove it.
void MainLoop::showMessageWall(const string& name) {
  optional<string> contents;
  if (auto c = dataFreePath.subdirectory("game_config").file(name + ".txt").readContents())
    contents = *c;
  for (auto& mod : getCurrentMods())
    if (auto c = modsDir.subdirectory(mod).file(name + ".txt").readContents())
      contents = *c;   // later mod wins
  if (!contents)
    return;
  auto lines = split(*contents, {'\n'});
  bool anyText = false;
  for (auto& l : lines)
    if (l.find_first_not_of(" \t\r") != string::npos)
      anyText = true;
  if (!anyText)
    return;   // empty or whitespace only -- treat as "no message"
  ScriptedUIDataElems::List rows;
  for (auto& l : lines) {
    auto line = l;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    // A blank line must still occupy a row, or the spacing the author wrote collapses.
    rows.push_back(ScriptedUIDataElems::Record{{{"text", ScriptedUIData{TString(line.empty() ? " "_s : line)}}}});
  }
  auto data = ScriptedUIDataElems::Record{{{"lines", ScriptedUIData{std::move(rows)}}}};
  ScriptedUIState state;
  view->scriptedUI(ScriptedUIId(name.data()), data, state);
}

void MainLoop::start(bool tilesPresent) {
  tileSet->setTilePathsAndReload(getTilePathsForAllMods());
  view->playVideo(paidDataPath.file("intro.ogv").getPath());
  view->reset();
  showMessageWall("welcome_message");
  considerFreeVersionText(tilesPresent);
  considerGameEventsPrompt();
  bool controllerHint = false;
  if (options->getBoolValue(OptionId::CONTROLLER_HINT_MAIN_MENU)) {
    controllerHint = true;
    options->setValue(OptionId::CONTROLLER_HINT_MAIN_MENU, 0);
  }
  const auto vanillaContent = createContentFactory(true);
  while (1) {
    playMenuMusic();
    auto data = ScriptedUIDataElems::Record{};
    optional<int> choice;
    PGame game;
    data.elems["play"] = ScriptedUIDataElems::Callback{[&game, this] {
      return !!(game = loadOrNewGame());
    }};
    data.elems["settings"] = ScriptedUIDataElems::Callback{[&vanillaContent, this] {
      options->handle(view, &vanillaContent, OptionSet::GENERAL);
      return false;
    }};
    data.elems["highscores"] = ScriptedUIDataElems::Callback{[this] {
      highscores->present(view);
      return false;
    }};
    data.elems["credits"] = ScriptedUIDataElems::Callback{[this] {
      showCredits();
      return false;
    }};
    if (!steamAchievements)
      data.elems["achievements"] = ScriptedUIDataElems::Callback{[this] {
        showAchievements();
        return false;
      }};
    data.elems["quit"] = ScriptedUIDataElems::Callback{[&choice] { choice = 4; return true;}};
    data.elems["version"] = TString(string(BUILD_DATE) + " " + BUILD_VERSION);
    data.elems["install_id"] = TString(fileSharing->getInstallId());
    if (controllerHint)
      data.elems["controller_hint"] = ScriptedUIDataElems::Callback{[&controllerHint] {
        controllerHint = false;
        return true;
      }};
    ScriptedUIState uiState{};
    view->scriptedUI("main_menu", data, uiState);
    if (game) {
      // Turn-0 rules wall for a NEW keeper only (loading an existing one must not re-show it). Not gated by
      // the HINTS option the way the vanilla intro text is -- these are server rules, not gameplay tips.
      if (startedNewGame) {
        startedNewGame = false;
        showMessageWall("game_message");
      }
      playGame(std::move(game), true, false);
      view->reset();
      // Online: if the game was left without a "save & exit", free its temporary
      // site claim (a KEEPER save has already upgraded it to permanent by now).
      rarReleaseActiveTempClaim();
    }
    if (!choice)
      continue;
    switch (*choice) {
      case 2: ; break;
      case 4: rarLogout(); return; // graceful quit: release the single-session lock immediately
    }
  }
}

void MainLoop::doWithSplash(const TString& text, int totalProgress,
    function<void(ProgressMeter&)> fun, function<void()> cancelFun) {
  if (useSingleThread()) {
    ProgressMeter meter(1.0 / totalProgress);
    fun(meter);
  } else
    view->doWithSplash(text, totalProgress, std::move(fun), std::move(cancelFun));
}

void MainLoop::doWithSplash(const TString& text, function<void()> fun, function<void()> cancelFun) {
  if (useSingleThread())
    fun();
  else {
    view->displaySplash(nullptr, text, cancelFun);
    auto t = makeScopedThread([fun, this] { fun(); view->clearSplash(); });
    view->refreshView();
  }
}

void MainLoop::modelGenTest(int numTries, const vector<string>& types, RandomGen& random, Options* options) {
  ProgressMeter meter(1);
  auto contentFactory = createContentFactory(false);
  vector<BiomeId> biomes;
  for (auto& elem : contentFactory.biomeInfo)
    biomes.push_back(elem.first);
  EnemyFactory enemyFactory(Random, contentFactory.getCreatures().getNameGenerator(), contentFactory.enemies,
      contentFactory.buildingInfo, {});
  ModelBuilder(&meter, random, options, sokobanInput, &contentFactory, std::move(enemyFactory))
      .measureSiteGen(numTries, types, std::move(biomes));
}

static CreatureList readAlly(ifstream& input) {
  string ally;
  input >> ally;
  CreatureList ret(100, CreatureId(ally.data()));
  int levelIncrease = 0;
  input >> levelIncrease;
  vector<ItemType> equipment;
  string equipmentText;
  input >> equipmentText;
  for (auto id : split(equipmentText, {','})) {
    ItemType type;
    if (auto error = PrettyPrinting::parseObject(type, id))
      FATAL << "Can't parse item type: " << id << ": " << *error;
    else
      equipment.push_back(type);
  }
  ret.addInventory(equipment);
  ret.setCombatExperience(levelIncrease);
  return ret;
}

void MainLoop::battleTest(int numTries, const FilePath& levelPath, const FilePath& battleInfoPath, string enemy) {
  ifstream input(battleInfoPath.getPath());
  CreatureList enemies;
  for (auto& elem : split(enemy, {','})) {
    auto enemySplit = split(elem, {':'});
    auto enemyId = enemySplit[0];
    int count = enemySplit.size() > 1 ? fromString<int>(enemySplit[1]) : 1;
    for (int i : Range(count))
      enemies.addUnique(CreatureId(enemyId.data()));
  }
  int cnt = 0;
  input >> cnt;
  auto contentFactory = createContentFactory(false);
  for (int i : Range(cnt)) {
    auto allies = readAlly(input);
    std::cout << allies.getSummary(&contentFactory.getCreatures()) << ": ";
    battleTest(numTries, levelPath, {allies}, {enemies});
  }
}

static vector<CreatureList> readAllies(const FilePath& battleInfoPath) {
  ifstream input(battleInfoPath.getPath());
  int cnt = 0;
  input >> cnt;
  vector<CreatureList> allies;
  for (int i : Range(cnt))
    allies.push_back(readAlly(input));
  return allies;
}

void MainLoop::campaignBattleText(int numTries, const FilePath& levelPath, EnemyId keeperId, VillainGroup group) {
  auto contentFactory = createContentFactory(false);
  for (auto villainType : {VillainType::NONE, VillainType::LESSER, VillainType::MAIN})
    for (auto& villain : contentFactory.villains.at(group))
      if (villain.type == villainType) {
        std::cerr << "Running " << villain.enemyId.data() << std::endl;
        auto res = campaignBattleText(numTries, levelPath, keeperId, villain.enemyId);
        std::cerr << villain.enemyId.data() << " RES " << res << std::endl;
      }
}

int MainLoop::campaignBattleText(int numTries, const FilePath& levelPath, EnemyId keeperId, EnemyId enemyId) {
  auto contentFactory = createContentFactory(false);
  auto minionsTmp = contentFactory.enemies.at(keeperId).settlement.inhabitants;
  auto enemy = contentFactory.enemies.at(enemyId);
  //for (int increase : Range(0, 100)) {
    vector<CreatureList> minions = {minionsTmp.leader, minionsTmp.fighters};
    /*for (auto& elem : minions) {
      //elem.clearExpLevel();
      elem.clearBaseLevel();
      elem.increaseBaseLevel(EnumMap<ExperienceType, int>([increase](ExperienceType t) { return increase; }));
    }
    std::cerr << "Increase " << increase << std::endl;*/
    int res = battleTest(numTries, levelPath, minions,
        {enemy.settlement.inhabitants.fighters, enemy.settlement.inhabitants.leader});
    /*if (res >= numTries * 9 / 10)
      return increase;*/
  //}
  return -1;
}

void MainLoop::endlessTest(int numTries, const FilePath& levelPath, const FilePath& battleInfoPath, optional<int> numEnemy) {
  auto allies = readAllies(battleInfoPath);
  auto contentFactory = createContentFactory(false);
  //RandomGen random;
  ExternalEnemies enemies(Random, &contentFactory.getCreatures(), EnemyFactory(Random, contentFactory.getCreatures().getNameGenerator(),
      contentFactory.enemies, contentFactory.buildingInfo, contentFactory.externalEnemies.at("basic"))
      .getExternalEnemies(), ExternalEnemiesType::FROM_START);
  for (int turn : Range(100000))
    if (auto wave = enemies.popNextWave(LocalTime(turn))) {
      std::cerr << "Turn " << turn << ": " << wave->enemy.name.data() << "\n";
      int totalWins = 0;
      for (auto& allyInfo : allies) {
        //std::cerr << allyInfo.getSummary(&contentFactory.getCreatures()) << ": ";
        int numWins = battleTest(numTries, levelPath, {allyInfo}, {wave->enemy.creatures});
        totalWins += numWins;
      }
      std::cerr << totalWins << " wins\n";
      std::cout << "Turn " << turn << ": " << wave->enemy.name.data() << ": " << totalWins << "\n";
    }
}

int MainLoop::battleTest(int numTries, const FilePath& levelPath, vector<CreatureList> ally, vector<CreatureList> enemies) {
  ProgressMeter meter(1);
  int numAllies = 0;
  int numEnemies = 0;
  int numUnknown = 0;
  auto allyTribe = TribeId::getDarkKeeper();
  for (int i : Range(numTries)) {
    auto contentFactory = createContentFactory(false);
    EnemyFactory enemyFactory(Random, contentFactory.getCreatures().getNameGenerator(),
        contentFactory.enemies, contentFactory.buildingInfo, {});
    vector<PCreature> allyCopy;
    for (auto& elem : ally)
      allyCopy.append(elem.generate(Random, &contentFactory.getCreatures(), TribeId::getDarkKeeper(), MonsterAIFactory::monster()));
    auto model = ModelBuilder(&meter, Random, options, sokobanInput,
        &contentFactory, std::move(enemyFactory)).battleModel(levelPath, std::move(allyCopy), enemies);
    auto game = Game::splashScreen(std::move(model), CampaignBuilder::getEmptyCampaign(), std::move(contentFactory), view);
    auto exitCondition = [&](Game* game) -> optional<ExitCondition> {
      HashSet<TribeId> tribes;
      for (auto& m : game->getAllModels())
        for (auto c : m->getAllCreatures())
          tribes.insert(c->getTribeId());
      if (tribes.size() == 1) {
        if (*tribes.begin() == allyTribe)
          return ExitCondition::ALLIES_WON;
        else
          return ExitCondition::ENEMIES_WON;
      }
      if (game->getGlobalTime().getVisibleInt() > 200)
        return ExitCondition::TIMEOUT;
      if (tribes.empty())
        return ExitCondition::UNKNOWN;
      else
        return none;
    };
    auto result = playGame(std::move(game), false, true, exitCondition, milliseconds{3});
    switch (result) {
      case ExitCondition::ALLIES_WON:
        ++numAllies;
        std::cerr << "a";
        break;
      case ExitCondition::ENEMIES_WON:
        ++numEnemies;
        std::cerr << "e";
        break;
      case ExitCondition::TIMEOUT:
        ++numUnknown;
        std::cerr << "t";
        break;
      case ExitCondition::UNKNOWN:
        ++numUnknown;
        std::cerr << "u";
        break;
    }
    std::cerr.flush();
  }
  std::cerr << " " << numAllies << ":" << numEnemies;
  if (numUnknown > 0)
    std::cerr << " (" << numUnknown << ") unknown";
  std::cerr << "\n";
  return numAllies;
}

PModel MainLoop::getBaseModel(ModelBuilder& modelBuilder, CampaignSetup& setup, const AvatarInfo& avatarInfo) {
  auto ret = [&] {
    switch (setup.campaign.getType()) {
      case CampaignType::QUICK_MAP:
        return modelBuilder.tutorialModel(avatarInfo.creatureInfo.startingBase);
      default:
        return modelBuilder.campaignBaseModel(avatarInfo, setup.campaign.getBaseBiome(), setup.externalEnemies);
    }
  }();
  return ret;
}

vector<ExternalEnemy> getExternalEnemiesFor(const AvatarInfo& info, const ContentFactory* contentFactory) {
  vector<ExternalEnemy> ret;
  for (auto& g : info.creatureInfo.endlessEnemyGroups)
    ret.append(contentFactory->externalEnemies.at(g));
  return ret;
}

ModelTable MainLoop::prepareCampaignModels(CampaignSetup& setup, const AvatarInfo& avatarInfo, RandomGen& random,
    ContentFactory* contentFactory) {
  EnemyFactory enemyFactory(Random, contentFactory->getCreatures().getNameGenerator(), contentFactory->enemies,
      contentFactory->buildingInfo, getExternalEnemiesFor(avatarInfo, contentFactory));
  ModelBuilder modelBuilder(nullptr, random, options, sokobanInput, contentFactory, std::move(enemyFactory));
  return prepareCampaignModels(setup, avatarInfo, std::move(modelBuilder), contentFactory);
}

ModelTable MainLoop::prepareCampaignModels(CampaignSetup& setup, const AvatarInfo& avatarInfo,
    ModelBuilder modelBuilder, ContentFactory* contentFactory) {
  Table<PModel> models(setup.campaign.getSites().getBounds());
  auto& sites = setup.campaign.getSites();
  for (Vec2 v : sites.getBounds())
    if (auto retired = sites[v].getRetired()) {
      if (retired->fileInfo.download)
        downloadGame(retired->fileInfo);
    }
  optional<string> failedToLoad;
  int numSites = setup.campaign.getNumNonEmpty();
  vector<ContentFactory> factories;
  int numRetiredVillains = 0;
  doWithSplash(TStringId("GENERATING_MAP"), numSites,
      [&] (ProgressMeter& meter) {
        for (Vec2 v : sites.getBounds()) {
          if (!sites[v].isEmpty())
            meter.addProgress();
          int difficulty = setup.campaign.getBaseLevelIncrease(v);
          if (auto info = sites[v].getKeeper()) {
            models[v] = getBaseModel(modelBuilder, setup, avatarInfo);
          } else if (auto villain = sites[v].getVillain()) {
            // RAR online (Phase A): DON'T generate villain maps on the client. The map still renders the
            // villain from its SiteInfo; the actual model is downloaded from the server on demand the
            // moment the player travels there (Game::chooseSite -> the villain loader). Leave the slot null.
            if (rarEnabled())
              continue;
            for (auto& info : getSaveFiles(userPath, getSaveSuffix(GameSaveType::RETIRED_SITE))) {
              auto version = getSaveVersion(info);
              if (isCompatible(version) && version >= 8101)
                if (auto saved = loadSavedGameInfo(userPath.file(info.filename)))
                  if (auto& retiredInfo = saved->retiredEnemyInfo)
                    if (retiredInfo->enemyId == villain->enemyId)
                      if (auto model = loadRetiredModelFromFile(userPath.file(info.filename))) {
                        models[v] = PModel(std::move(model->model));
                        ++numRetiredVillains;
                        remove(userPath.file(info.filename).getPath());
                        break;
                      }
            }
            if (!models[v])
              models[v] = modelBuilder.campaignSiteModel(villain->enemyId, villain->type, avatarInfo.tribeAlignment,
                  *setup.campaign.getSites()[v].biome, difficulty);
            for (auto c : models[v]->getAllCreatures())
              c->setCombatExperience(difficulty);
          } else if (auto retired = sites[v].getRetired()) {
            if (auto info = loadRetiredModelFromFile(userPath.file(retired->fileInfo.filename))) {
              models[v] = PModel(std::move(info->model));
              for (auto col : models[v]->getCollectives())
                if (col->getVillainType() == VillainType::MAIN)
                  col->setVillainType(VillainType::RETIRED);
              if (endsWith(retired->fileInfo.filename, getSaveSuffix(GameSaveType::RETIRED_CAMPAIGN)))
                for (auto c : models[v]->getAllCreatures())
                  c->setCombatExperience(50);
              factories.push_back(std::move(info->factory));
            } else {
              failedToLoad = retired->fileInfo.filename;
              setup.campaign.removeDweller(v);
            }
          }
        }
      });
  if (failedToLoad)
    view->presentText(none, TString("Error reading " + *failedToLoad + ". Leaving blank site."));
  return ModelTable{std::move(models), std::move(factories), numRetiredVillains};
}

// RAR: ship any crash reports left in crashes/ to the server, compressed. Runs on STARTUP, not from the crash
// handler: at crash time the process is in an unhandled-exception state with a possibly-corrupt heap, so doing
// TLS + curl + our knock there could hang or fault again and lose the report entirely. Writing the files is
// all the handler does; the next launch does the risky part safely.
// Each file is deleted only after the server confirms it, so an interrupted upload just retries next launch.
// Runs on a detached thread -- a multi-MB dump must never delay the main menu.
void MainLoop::rarUploadPendingCrashes() {
  if (!rarConfigured())
    return; // no server to send to; leave the files for later
  DirectoryPath dir("crashes");
  if (!dir.exists())
    return;
  // Snapshot the list on THIS thread: don't hand game path objects to a detached thread.
  std::vector<std::pair<std::string, std::string>> pending; // (full path, bare filename)
  for (auto& f : dir.getFiles())
    pending.push_back({f.getPath(), f.getFileName()});
  if (pending.empty())
    return;
  std::thread([pending] {
    for (auto& e : pending) {
      std::ifstream in(e.first, std::ios::binary);
      if (!in)
        continue;
      std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      if (raw.empty()) { std::remove(e.first.c_str()); continue; } // empty dump -> nothing to report
      std::string blob = rarLzmaCompress(raw);
      if (blob.empty())
        continue;
      if (rarUploadCrash(e.second, blob)) {
        std::remove(e.first.c_str()); // server has it -> stop carrying it around
        std::cout << "[crash] uploaded + removed " << e.second << " (" << raw.size() / 1024 << " KB -> "
                  << blob.size() / 1024 << " KB)\n";
        std::cout.flush();
      }
    }
  }).detach();
}

// Headless driver for the crash upload (--rar_crash_test), so the deferred path can actually be verified
// without reproducing a real crash + GUI launch. Waits, because the upload runs on a detached thread that
// process exit would otherwise kill mid-flight.
void MainLoop::rarUploadPendingCrashesNow() {
  DirectoryPath dir("crashes");
  int before = dir.exists() ? (int) dir.getFiles().size() : 0;
  std::cout << "[crash-test] pending in crashes/: " << before << "\n"; std::cout.flush();
  rarUploadPendingCrashes();
  std::this_thread::sleep_for(std::chrono::seconds(10)); // let the detached upload finish
  int after = dir.exists() ? (int) dir.getFiles().size() : 0;
  std::cout << "[crash-test] remaining after upload: " << after << " (uploaded " << (before - after) << ")\n";
  std::cout.flush();
}

// RAR: the world map belongs to the SERVER, not to the save. On every load we throw away the campaign frozen
// into the .kep and rebuild it from the server's CURRENT world, keeping the keeper where it is if that tile
// still works. So regenerating the world (e.g. after adding a biome) needs no per-keeper migration: old
// keepers just pick up the new map on their next load, and any whose tile no longer matches their base's
// biome (or got taken by a villain) are moved to a random tile of the RIGHT biome.
// The base MODEL is never touched -- only the campaign is swapped -- so nothing is lost or re-serialized.
// Campaign state is server-derived anyway (villains come from /world_data, defeated flags from the live
// roster via reconcileVillains), so rebuilding it loses nothing but the stale map.
void MainLoop::rarSyncWorldOnLoad(PGame& game) {
  if (!rarEnabled() || !game)
    return;
  Model* base = game->getMainModel().get();
  auto pcol = game->getPlayerCollective();
  if (!base || !pcol || pcol->getLeaders().empty())
    return; // not a keeper game (tutorial/warlord) -> leave it alone
  string blob;
  Table<Campaign::SiteInfo> sites;
  bool got = false;
  bool fromCache = false; // for the RAR_SYNC_INFO readout: did we actually hit the network for the map?
  doWithSplash(TString("Syncing the world map with the server..."_s), [&] {
    // The world blob is ~0.5MB of SCENERY that only changes on a regen, so don't re-download it every load:
    // compare the server's hash to our cached copy and reuse the cache when it matches. (The blob's villain
    // dwellers are irrelevant either way -- reconcileVillains overwrites all of them from the live roster,
    // which is a tiny separate fetch. So villains stay live while scenery stays cached.)
    FilePath cache = userPath.file("rar_world_cache.dat");
    string serverHash;
    if (rarWorldHash(serverHash)) {
      std::ifstream in(cache.getPath(), std::ios::binary);
      if (in) {
        string cached((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        if (!cached.empty() && rarSha256Hex(cached) == serverHash) {
          blob = std::move(cached); // cache hit -> no download
          fromCache = true;
        }
      }
    }
    if (blob.empty()) {
      if (!rarFetchWorldData(blob))
        return;
      if (!serverHash.empty()) { // only cache what we can validate next time
        std::ofstream out(cache.getPath(), std::ios::binary);
        out.write(blob.data(), blob.size());
      }
    }
    try {
      std::stringstream ss(blob);
      InputArchive ar(ss);
      ar >> sites;
      got = true;
    } catch (...) {}
  });
  if (!got) {
    // Keep playing on the save's own (possibly stale) map rather than blocking the load.
    view->presentText(none, TString("Couldn't sync the world map from the server; using the last known map."_s));
    return;
  }
  string worldName = "RAR World";
  { auto w = rarGetWorld(); if (w.valid && !w.worldName.empty()) worldName = w.worldName; }
  // CONTENT is server-owned too: Game::serialize freezes a whole ContentFactory into every save, so a keeper
  // created before a new biome/villain existed carries a snapshot that can't describe the server's CURRENT
  // world -- and Campaign::updateInhabitants does factory->enemies.at(enemyId), which THROWS on an unknown id.
  // Online the client's data files are already synced to the server's content, so they are authoritative:
  // replace the snapshot BEFORE rebuilding the campaign. (Same replace as the RELOAD_DATA option, which runs
  // later in playGame -- too late for us.) Rebuild the build menu too: it's baked into PlayerControl.
  *game->getContentFactory() = createContentFactory(false);
  if (auto pc = game->getPlayerControl()) {
    auto f = game->getContentFactory();
    for (auto& p : f->keeperCreatures)
      if (p.first == game->getAvatarId()) { pc->reloadBuildMenu(f, p.second); break; }
  }
  Vec2 oldPos = base->position;
  Creature* keeper = pcol->getLeaders()[0];
  // RAR: a keeper can end up with NO claim on the server -- the startup stray-prune drops any claim that never
  // got a dungeon blob (created, then only ever autosaved: the blob is uploaded on save & exit ONLY), and a
  // server crash can lose one too. The KEEPER is never lost by this -- the account's keeper list comes from the
  // save hashes, not the claims -- but without a claim its base draws on nobody's world map and its tile is
  // free for the next player to take. So re-claim it here, on load. Other keepers' tiles are passed as
  // excluded so a relocation can't land on top of somebody (the world blob has terrain + villains only; rival
  // keepers live in the claim list, which reconstructKeeperCampaign would otherwise never see).
  const string myGameId = game->getGameIdentifier();
  bool haveClaim = false;
  vector<Vec2> claimedByOthers;
  for (auto& c : rarGetClaims()) {
    if (c.gameId == myGameId)
      haveClaim = true;
    else
      claimedByOthers.push_back(Vec2(c.x, c.y));
  }
  RandomGen rnd;
  rnd.init((int) time(nullptr));
  auto camp = CampaignBuilder::reconstructKeeperCampaign(rnd, game->getContentFactory(), std::move(sites),
      base->getBiomeId(), keeper->getMaxViewIdUpgrade(), keeper->getTribeId(), worldName, oldPos,
      claimedByOthers);
  if (!camp) {
    // No tile of this keeper's biome is free on the new world. Do NOT quietly place it somewhere wrong --
    // stay on the last known map and tell the player to get the world fixed, since only the administrator
    // can add tiles of that biome or free up the ones that exist.
    view->presentText(none, TSentence("KEEPER_BIOME_UNAVAILABLE",
        TString(string(base->getBiomeId().data()))));
    return;
  }
  Vec2 newPos = camp->getPlayerPos();
  game->rehomeToNewWorld(std::move(*camp), newPos);
  // The campaign we just rebuilt came from the server's RAW world blob, so every villain still carries the
  // type the world was generated with -- notably ALLY for factions that are an ally to SOMEONE. Reconcile it
  // for THIS keeper now. Until this ran, the only thing that reconciled after a load was opening the travel
  // map (Game::chooseSite), so a freshly loaded game showed other keepers' allies as our green "ally" in the
  // villages panel, and the villain-wave code saw ALLY where the world map (once opened) showed MINOR.
  game->reconcileVillainsForLoad();
  // Claim was missing -> take the tile back now that the base is settled on it. reconstructKeeperCampaign
  // prefers our old tile and only moves us when it is gone, so this usually re-claims exactly where we were.
  if (!haveClaim) {
    string name = view->translate(game->getGameDisplayName());
    if (rarClaimSite(myGameId, name, newPos.x, newPos.y))
      INFO << "RAR: restored missing site claim for " << myGameId << " at " << newPos.x << "," << newPos.y;
    else
      // Someone claimed the tile between our fetch and now; the base still plays, it just isn't on the shared
      // map until the next load finds it a free one. Don't block the load over it.
      view->presentText(none, TString("Your keep's claim on the shared world was missing and couldn't be "
          "restored ("_s + rarLastError() + "). It will be retried next time you load."));
  }
  // Dev readout: there's no other way to tell a cache hit from a download from inside the game. Held ~2s.
  if (options->getBoolValue(OptionId::RAR_SYNC_INFO)) {
    string info = "World sync\n\nmap:  "_s
        + (fromCache ? "CACHED, no download (" + toString(blob.size() / 1024) + " KB on disk)"
                     : "DOWNLOADED " + toString(blob.size() / 1024) + " KB")
        + "\nkeep: " + (newPos == oldPos
              ? "stayed at " + toString(oldPos.x) + "," + toString(oldPos.y)
              : "MOVED " + toString(oldPos.x) + "," + toString(oldPos.y) + " -> " + toString(newPos.x) + ","
                    + toString(newPos.y))
        + "\n\n(villains always come live from the roster)";
    doWithSplash(TString(info), [] { std::this_thread::sleep_for(std::chrono::seconds(2)); });
  }
  if (newPos != oldPos)
    view->presentTextCenter(TString("The world has changed. Your keep now lies elsewhere."_s));
}

PGame MainLoop::loadGame(const FilePath& file, const TString& name) {
  // Declared out here (not inside the rarEnabled() block below) because the recovery upload happens at the
  // very END of this function, once the game is actually loaded and world-synced.
  bool restoreBlobFromAutosave = false;
  bool loadedAutosave = false;
  string gameId; // online identity "<account>~<keeper>"; stays empty offline
  // ANTICHEAT + INVASION -- check FIRST, before loading anything: compare our LOCAL file's hash to the
  // server's per-dungeon hash. If they differ (invaded OR tampered) the server is authoritative, so
  // download+load the SERVER copy instead -- we must never briefly load the wrong (local) state.
  if (rarEnabled()) {
    string fn = file.getFileName();
    string keeperName = fn.size() > 4 ? fn.substr(0, fn.size() - 4) : fn; // filename minus .kep/.aut = keeper name
    gameId = rarComposeGameId(rarSessionLogin(), keeperName); // online identity "<account>~<keeper>"
    // SIEGE (owner side): somebody is inside our dungeon right now. Knocking starts his eviction countdown
    // and stamps our anti-lockout protection. We must NOT load until he's out, or we'd play from a state he's
    // still editing and his writeback would clobber us. Wait for the server to report clear -- which happens
    // only once he RELEASED, and he releases only after his aftermath upload lands, so clear also means the
    // aftermath is on the server and the hash check below will pull it.
    {
      long long left = 0;
      auto siege = rarOwnerReturning(gameId, left);
      if (siege == RarSiegeResult::UnderSiege) {
        view->presentTextCenter(TString("Your dungeon is under siege."_s));
        view->presentTextCenter(TString("If invader will not retreat it will be forcefully removed in 1 minute."_s));
        while (siege == RarSiegeResult::UnderSiege) {
          // Short splashes in a loop = a countdown that actually updates while we poll.
          doWithSplash(TString("Your dungeon is under siege.\nThe invader is forced out in "_s
              + toString(left) + "s...\n\nWaiting for him to leave and upload the aftermath."), [&] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            siege = rarOwnerReturning(gameId, left);
          });
        }
        if (siege == RarSiegeResult::Clear)
          view->presentTextCenter(TString("The invader is gone. Entering your dungeon."_s));
      }
      // Unreachable -> fall through and let the owner in: better than locking him out of his own keep.
    }
    bool useServer = false, wasInvaded = false;
    // Only ever one save on disk (eraseAllSavesExcept), so a .aut existing AT ALL means the last session
    // never exited cleanly -- i.e. we crashed. That is the whole crash signal; nothing else is needed.
    const string autoSuffix = getSaveSuffix(GameSaveType::AUTOSAVE);
    const bool isAutosave = fn.size() > autoSuffix.size() &&
        fn.compare(fn.size() - autoSuffix.size(), autoSuffix.size(), autoSuffix) == 0;
    loadedAutosave = isAutosave;
    string slayer; // non-empty => an invader slew this keeper's leader; the keeper is gone.
    // Query the server UNDER a splash (conquest check + hash compare: decompress + SHA-256 + queries).
    doWithSplash(TString("Checking your keep with the server..."_s), [&] {
      slayer = rarConqueredBy(gameId);
      if (!slayer.empty())
        return; // conquered -> nothing else to check, we won't load it
      if (isAutosave) {
        // The .aut's state was never uploaded (autosave sends only its hash), so the server's dungeon hash is
        // still the one from our last save & exit and can NEVER match it. Running the normal comparison here
        // would therefore always "fail" and silently roll the player back to that older clean save, throwing
        // away everything he played before the crash. Decide on the autosave's own terms instead:
        wasInvaded = rarDungeonInvaded(gameId);
        if (wasInvaded)
          // Somebody invaded while we were gone: they loaded the last state the SERVER had, fought in it and
          // wrote the aftermath back. That aftermath is the newer, true history -- our pre-crash autosave
          // never happened as far as the world is concerned. Discard it and take the server's copy.
          useServer = true;
        else if (rarGetAutosaveHash(gameId) == rarHashFile(file.getPath()))
          // Untouched by anyone else and byte-identical to the file we wrote when we autosaved -> genuinely
          // ours, and newer than the server's. Load it, and hand it to the server afterwards.
          restoreBlobFromAutosave = true;
        else
          useServer = true; // doesn't match what we uploaded -> tampered with -> server is authoritative
        return;
      }
      auto serverHash = rarDungeonHash(gameId);
      if (!serverHash.empty()) {
        igzstream gzin(file.getPath()); // hash the RAW game bytes (same form we upload) -> comparable
        string localRaw((std::istreambuf_iterator<char>(gzin)), std::istreambuf_iterator<char>());
        gzin.close();
        if (rarSha256Hex(localRaw) != serverHash) {
          useServer = true;
          wasInvaded = rarDungeonInvaded(gameId);
        }
      } else
        // The server has no blob for us at all -- it lost it (crash/data loss), or we never managed a save &
        // exit. Our clean local save is all that's left, so put it back after loading.
        restoreBlobFromAutosave = true;
    });
    if (!slayer.empty()) {
      // The keeper's leader was slain -> refuse to load; tell the owner who did it, then remove the
      // dead keeper (local save + world-map claim) so it's gone for good. Flag it so the caller doesn't
      // then show the generic "Failed to load the save file" error on this (intentional) null return.
      view->presentTextCenter(TString("Your keeper was slain by\n" + slayer));
      rarDeleteClaim(gameId);
      file.erase();
      rarAckSlain(gameId); // owner notified + local save gone -> clear the (persisted) slain record
      rarKeeperSlain = true;
      return nullptr;
    }
    if (useServer) {
      // Announce the invasion BEFORE loading, then load the server copy with the normal progress bar.
      if (wasInvaded)
        view->presentTextCenter(TString(
            "While you were away, somebody invaded your dungeon.\nLoading the aftermath...."_s));
      PGame served = loadServerGame(gameId, name);
      rarSyncWorldOnLoad(served); // server copy gets the current world too
      return served;
    }
  }
  optional<PGame> game;
  if (auto info = loadSavedGameInfo(file))
    doWithSplash(TSentence("LOADING_GAME", name), info->progressCount,
        [&] (ProgressMeter& meter) {
          Level::progressMeter = &meter;
          INFO << "Loading from " << file;
          MEASURE(game = loadFromFile<PGame>(file), "Loading game");
    });
  Level::progressMeter = nullptr;
  if (!game || !*game)
    return nullptr;
  if (rarEnabled())
    (*game)->setGameIdentifier(gameId); // adopt the runtime "<account>~<keeper>" identity (migrated saves self-heal)
  rarSyncWorldOnLoad(*game); // the save's frozen world map is discarded for the server's current one
  // Crash recovery: the server's copy of our dungeon is missing or older than what we just loaded. Push the
  // local save up verbatim -- it is already in the stripped form (both .kep and .aut are written that way), so
  // the blob is identical in shape to the one a save & exit produces, and the dungeon hash now matches our
  // local file again. Without this, the keeper stays uninvadeable and every later load would see a stale hash.
  if (restoreBlobFromAutosave)
    rarUploadKeeperDungeon(*game, loadedAutosave ? GameSaveType::AUTOSAVE : GameSaveType::KEEPER);
  return std::move(*game);
}

// RAR online: download + load the keeper's server blob (its full sectors-stripped state -- the authoritative
// copy, which is the damaged aftermath if it was invaded). Download, unpack and load all run under the
// normal loading progress bar so the player sees continuous feedback.
PGame MainLoop::loadServerGame(const string& gameId, const TString& name) {
  optional<PGame> game;
  bool downloaded = true;
  doWithSplash(TSentence("LOADING_GAME", name), 1, [&] (ProgressMeter& meter) {
    Level::progressMeter = &meter;
    string blob;
    if (!rarFetchDungeon(gameId, blob)) { downloaded = false; return; }
    string raw = rarLzmaDecompress(blob);
    if (raw.empty()) { downloaded = false; return; }
    FilePath t = userPath.file("rar_load" + getSaveSuffix(GameSaveType::KEEPER));
    { ogzstream out(t.getPath()); out.write(raw.data(), raw.size()); }
    game = loadFromFile<PGame>(t);
    t.erase();
  });
  Level::progressMeter = nullptr;
  if (!downloaded || !game || !*game) {
    view->presentText(none, TString("Couldn't download your keep from the server."_s));
    return nullptr;
  }
  (*game)->setGameIdentifier(gameId); // adopt the "<account>~<keeper>" identity so save-back writes the right slot
  return std::move(*game);
}

bool MainLoop::downloadGame(const SaveFileInfo& file) {
  FileSharing::CancelFlag cancel;
  optional<string> error;
  doWithSplash(TSentence("DOWNLOADING", TString(file.filename)), 1,
      [&] (ProgressMeter& meter) {
        error = fileSharing->downloadSite(cancel, file, userPath, meter);
      },
      [&] {
        cancel.cancel();
      });
  if (error && !cancel.flag)
    view->presentText(none, TString(*error));
  return !error;
}

static void changeSaveType(const FilePath& file, GameSaveType newType) {
  optional<FilePath> newFile;
  for (GameSaveType oldType : ENUM_ALL(GameSaveType)) {
    string suf = getSaveSuffix(oldType);
    if (file.hasSuffix(suf)) {
      if (oldType == newType)
        return;
      newFile = file.changeSuffix(suf, getSaveSuffix(newType));
      break;
    }
  }
  CHECK(!!newFile);
  remove(newFile->getPath());
  rename(file.getPath(), newFile->getPath());
}

PGame MainLoop::loadOrNewGame() {
  if (!rarLoginFlow())
    return nullptr; // online: must log in before loading or starting a game
  // Mirror the server's mods folder to this client BEFORE the load/new menu, so CONTINUING a keeper picks up
  // whatever's currently on the server -- not just starting a new game. syncServerMods sets CURRENT_MOD2 to the
  // server's manifest (download-if-missing), so the server's mods folder is the single source of what's active.
  if (rarEnabled() && !syncServerMods())
    return nullptr;
  return loadOrNewGameSelect();
}

// RAR: the load/new-game menu and its loading. Runs AFTER login and recurses on ITSELF (not loadOrNewGame),
// so a slain/erased/failed-to-load keeper re-shows the menu while STAYING LOGGED IN -- it never re-prompts
// for login. When the account has no keepers left, this drops straight into create-a-new-keeper.
PGame MainLoop::loadOrNewGameSelect() {
  auto games = ScriptedUIDataElems::List{};
  optional<pair<SaveFileInfo, TString>> savedGame;
  optional<SaveFileInfo> warlordGame;
  optional<SaveFileInfo> eraseGame;
  optional<pair<string, TString>> eraseServerKeeper;   // gameId + label: delete a keeper that has no local save
  optional<pair<string, TString>> serverGameToLoad; // (keeperId, display name) for a keeper with no local save
  // ONLINE: the SERVER decides which keepers this account has -- fetch that ONCE up front. The local
  // rar_saves.txt registry is only a per-PC hint and must never decide what's loadable: trusting it let a
  // local .kep whose keeper had been DELETED server-side still load (carrying its own stale world map) and
  // then re-upload itself on save, resurrecting the deleted keeper + account. Fail CLOSED if the server can't
  // be asked -- showing local saves in that case is exactly the bug.
  std::set<string> serverKeepers;
  bool haveServerKeepers = false;
  if (rarEnabled()) {
    haveServerKeepers = rarListKeepers(serverKeepers);
    if (!haveServerKeepers)
      view->presentTextCenter(TString("Couldn't reach the server to list your keepers. Try again later."_s));
  }
  // Online, a keeper's local cache lives at saves/<account>/<keeper>.{kep,aut}; the filename base is the keeper
  // name and its identity is rarComposeGameId(account, name). Offline (unused) stays flat in userPath.
  DirectoryPath saveDir = userPath;
  if (rarEnabled()) {
    saveDir = userPath.subdirectory("saves");
    saveDir.createIfDoesntExist();
    saveDir = saveDir.subdirectory(rarSessionLogin());
    saveDir.createIfDoesntExist();
  }
  auto addGames = [&](GameSaveType type) {
    vector<SaveFileInfo> files = getSaveFiles(saveDir, getSaveSuffix(type));
    // resolve the version against saveDir -- these files are in saves/<account>/, not the flat userPath
    files = files.filter([&] (const SaveFileInfo& info) { return isCompatible(getSaveVersion(info, saveDir));});
    if (rarEnabled()) {
      // Only show keepers the SERVER says this account owns (also hides other accounts' saves and any
      // pre-existing original .kep files -- no importing). Match on the composite "<account>~<keeper>".
      string suffix = getSaveSuffix(type);
      files = files.filter([&](const SaveFileInfo& info) {
        auto name = info.filename.substr(0, info.filename.size() - suffix.size());
        return haveServerKeepers && serverKeepers.count(rarComposeGameId(rarSessionLogin(), name)) > 0;
      });
    }
    if (!files.empty()) {
      append(games, files.transform(
          [&] (const SaveFileInfo& info) {
              auto nameAndVersion = *getNameAndVersion(saveDir.file(info.filename));
              auto gameInfo = loadSavedGameInfo(saveDir.file(info.filename));
              auto record = ScriptedUIDataElems::Record{{
                {"label", ScriptedUIData{nameAndVersion.first}},
                {"date", ScriptedUIData{getDateString(info.date)}},
                {"viewIds", ScriptedUIData{gameInfo->minions.transform([](auto minion){ return ScriptedUIData{minion.viewId}; })}},
                {"erase", ScriptedUIData{ScriptedUIDataElems::Callback{[&eraseGame, info]{
                  eraseGame = info;
                  return true;
                }}}}
              }};
              if (type == GameSaveType::WARLORD)
                record.elems["warlord"] = ScriptedUIData{ScriptedUIDataElems::Callback{[&warlordGame, info]{
                  warlordGame = info;
                  return true;
                }}};
              else
                record.elems["load"] = ScriptedUIData{ScriptedUIDataElems::Callback{[&savedGame, name = nameAndVersion.first, info]{
                  savedGame = make_pair(info, name);
                  return true;
                }}};
              return record;
          }));
    }
  };
  if (!rarEnabled()) {
    addGames(GameSaveType::AUTOSAVE);
    addGames(GameSaveType::KEEPER);
    addGames(GameSaveType::WARLORD);
  } else {
    // ================= ONLINE: THE SERVER'S KEEPER LIST *IS* THE MENU =================
    // Build the list by iterating the SERVER's keepers for this account -- never by scanning local files. A
    // local save is ONLY a cache: it is used when it's present and loadable, otherwise the keeper is downloaded
    // from the server. Consequences, by construction:
    //   * every keeper the server says we own ALWAYS gets a row (it can never go missing),
    //   * a local save the server does NOT know about cannot have been made on this server -> it is DELETED.
    if (!haveServerKeepers)
      return nullptr; // server unreachable: message already shown. NEVER fall through to a local-only list.
    // 1) Delete local keeper saves the server doesn't know about.
    for (auto type : {GameSaveType::KEEPER, GameSaveType::AUTOSAVE}) {
      string suffix = getSaveSuffix(type);
      for (auto& info : getSaveFiles(saveDir, suffix)) {
        auto name = info.filename.substr(0, info.filename.size() - suffix.size());
        if (!serverKeepers.count(rarComposeGameId(rarSessionLogin(), name))) {
          INFO << "RAR: deleting local save the server doesn't know about: " << info.filename;
          saveDir.file(info.filename).erase();
        }
      }
    }
    // 2) Exactly one row per server keeper: local cache if usable, otherwise download.
    for (auto& keeperId : serverKeepers) {
      auto sep = keeperId.find('~'); // "<account>~<keeper>"
      string keeperName = (sep == string::npos) ? keeperId : keeperId.substr(sep + 1);
      optional<SaveFileInfo> cached;
      for (auto type : {GameSaveType::KEEPER, GameSaveType::AUTOSAVE}) {
        if (cached)
          break;
        string suffix = getSaveSuffix(type);
        for (auto& info : getSaveFiles(saveDir, suffix))
          if (info.filename == keeperName + suffix && isCompatible(getSaveVersion(info, saveDir))) {
            cached = info;
            break;
          }
      }
      if (cached) {
        auto nameAndVersion = getNameAndVersion(saveDir.file(cached->filename));
        auto gameInfo = loadSavedGameInfo(saveDir.file(cached->filename));
        TString label = nameAndVersion ? nameAndVersion->first : TString(keeperName);
        games.push_back(ScriptedUIData{ScriptedUIDataElems::Record{{
            {"label", ScriptedUIData{label}},
            {"date", ScriptedUIData{getDateString(cached->date)}},
            {"viewIds", ScriptedUIData{gameInfo
                ? gameInfo->minions.transform([](auto minion){ return ScriptedUIData{minion.viewId}; })
                : ScriptedUIDataElems::List{}}},
            {"load", ScriptedUIData{ScriptedUIDataElems::Callback{[&savedGame, label, info = *cached]{
              savedGame = make_pair(info, label);
              return true;
            }}}},
            {"erase", ScriptedUIData{ScriptedUIDataElems::Callback{[&eraseGame, info = *cached]{
              eraseGame = info;
              return true;
            }}}}
        }}});
      } else {
        TString label = TString(keeperName);
        games.push_back(ScriptedUIData{ScriptedUIDataElems::Record{{
            {"label", ScriptedUIData{label}},
            {"date", ScriptedUIData{TString("on server"_s)}},
            {"viewIds", ScriptedUIData{ScriptedUIDataElems::List{}}},
            {"load", ScriptedUIData{ScriptedUIDataElems::Callback{[&serverGameToLoad, keeperId, label]{
              serverGameToLoad = make_pair(keeperId, label);
              return true;
            }}}},
            // A keeper with no usable local cache had NO erase callback at all, so one that the client can't
            // load (wrong save version, corrupt, or simply never downloaded here) could never be got rid of:
            // the menu rebuilds itself from the server list every time, so the dead row came back forever.
            {"erase", ScriptedUIData{ScriptedUIDataElems::Callback{[&eraseServerKeeper, keeperId, label]{
              eraseServerKeeper = make_pair(keeperId, label);
              return true;
            }}}}
        }}});
      }
    }
  }
  auto data = ScriptedUIDataElems::Record{};
  if (games.empty())
    return prepareCampaign(Random);
  data.elems["games"] = std::move(games);
  bool newGame = false;
  data.elems["new"] = ScriptedUIData{ScriptedUIDataElems::Callback{[&newGame]{
    newGame = true;
    return true;
  }}};
  // What content is actually loaded, who wrote it and why -- reachable from the keeper list, not just from the
  // new-game flow. Online it is the READ-ONLY server view (the server owns the mod set); offline it's the
  // normal mods screen.
  bool showModsMenu = false;
  data.elems["mods"] = ScriptedUIData{ScriptedUIDataElems::Callback{[&showModsMenu]{
    showModsMenu = true;
    return true;
  }}};
  ScriptedUIState uiState{};
  view->scriptedUI("load_menu", data, uiState);
  if (showModsMenu) {
    if (rarEnabled())
      showServerMods();
    else
      showMods();
    return loadOrNewGameSelect();   // back to the keeper list
  }
  if (newGame) {
    if (auto res = prepareCampaign(Random))
      return res;
  } else if (savedGame) {
    rarKeeperSlain = false;
    if (PGame ret = loadGame(saveDir.file(savedGame->first.filename), savedGame->second)) {
      if (eraseSave())
        changeSaveType(saveDir.file(savedGame->first.filename), GameSaveType::AUTOSAVE);
      return ret;
    } else if (!rarKeeperSlain) // slain-keeper already showed its own message -> don't also show a load error
      view->presentText(none, TStringId("FAILED_TO_LOAD_SAVE"));
    return loadOrNewGameSelect(); // stay logged in; re-show menu (or create-keeper if this was the last save)
  } else if (serverGameToLoad) {
    // No local save for this keeper -> pull the authoritative copy from the server and play it. On save & exit
    // it writes a local .kep + re-uploads, so next time it loads from the local cache like any other.
    if (PGame served = loadServerGame(serverGameToLoad->first, serverGameToLoad->second)) {
      rarSyncWorldOnLoad(served); // the blob's frozen world map is replaced with the server's current one
      return served;
    }
    return loadOrNewGameSelect();
  } else if (warlordGame) {
    if (auto game = prepareWarlord(*warlordGame))
      return game;
    return loadOrNewGameSelect();
  } else if (eraseGame) {
    if (view->yesOrNoPrompt(TSentence("CONFIRM_FILE_ERASE", TString(eraseGame->filename)))) {
      if (rarEnabled()) {
        // Erasing a keeper deletes it EVERYWHERE: the server copy (blob, keeper.txt, claim, hashes) and every
        // local file for it. Two bugs used to survive here:
        //   * only a .kep filename matched, so erasing a keeper whose cache was an AUTOSAVE (.aut -- what you
        //     have after a crash) left the server copy alone, and the menu, which is built FROM the server
        //     list, simply put the row back;
        //   * only the one matched file was removed, leaving the other of the .kep/.aut pair behind.
        const string& fn = eraseGame->filename;
        string keeperName = fn;
        for (auto type : {GameSaveType::KEEPER, GameSaveType::AUTOSAVE}) {
          string suffix = getSaveSuffix(type);
          if (fn.size() > suffix.size() && fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0) {
            keeperName = fn.substr(0, fn.size() - suffix.size());
            break;
          }
        }
        rarDeleteClaim(rarComposeGameId(rarSessionLogin(), keeperName));
        for (auto type : {GameSaveType::KEEPER, GameSaveType::AUTOSAVE})
          saveDir.file(keeperName + getSaveSuffix(type)).erase();
      }
      saveDir.file(eraseGame->filename).erase();
    }
    return loadOrNewGameSelect();
  } else if (eraseServerKeeper) {
    // Server-only keeper: nothing local to remove, so this is purely the server-side delete.
    if (view->yesOrNoPrompt(TSentence("CONFIRM_FILE_ERASE", eraseServerKeeper->second))) {
      rarDeleteClaim(eraseServerKeeper->first);
      auto sep = eraseServerKeeper->first.find('~');
      if (sep != string::npos) {   // belt and braces: drop any stale local file under that name
        string keeperName = eraseServerKeeper->first.substr(sep + 1);
        for (auto type : {GameSaveType::KEEPER, GameSaveType::AUTOSAVE})
          saveDir.file(keeperName + getSaveSuffix(type)).erase();
      }
    }
    return loadOrNewGameSelect();
  }
  return nullptr;
}

struct WarlordInfo {
  vector<PCreature> SERIAL(creatures);
  ContentFactory SERIAL(contentFactory);
  string SERIAL(gameIdentifier);
  SERIALIZE_ALL_NO_VERSION(creatures, contentFactory, gameIdentifier)
};

PGame MainLoop::prepareWarlord(const SaveFileInfo& fileInfo) {
/*  if (auto warlordInfo = loadFromFile<WarlordInfo>(userPath.file(fileInfo.filename))) {
    ContentFactory contentFactory;
    tileSet->clear();
    // Using a splash screen causes a segfault due to reloading the tileset while scriptedUI is running
    //doWithSplash("Loading gameplay data", [&] {
      contentFactory = createContentFactory(false);
      if (tileSet)
        tileSet->setTilePaths(contentFactory.tilePaths);
    //});
    tileSet->loadTextures();
    auto retiredGames = *getRetiredGames(CampaignType::FREE_PLAY);
    for (int i : All(retiredGames.getAllGames()))
      if (retiredGames.getAllGames()[i].fileInfo.getGameId() == warlordInfo->gameIdentifier) {
        retiredGames.erase(i);
        break;
      }
    auto playerInfos = warlordInfo->creatures.transform(
        [&](auto& c) { return PlayerInfo(c.get(), &warlordInfo->contentFactory); });
    sort(++playerInfos.begin(), playerInfos.end(),
          [](auto c1, auto c2) { return c1.bestAttack.value > c2.bestAttack.value; });
    auto chosen = view->prepareWarlordGame(retiredGames, playerInfos, 12, 10);
    if (!chosen.empty()) {
      auto setup = CampaignBuilder::getWarlordCampaign(retiredGames.getActiveGames(),
          warlordInfo->creatures[0]->getName().firstOrBare());
      EnemyFactory enemyFactory(Random, contentFactory.getCreatures().getNameGenerator(), contentFactory.enemies,
          contentFactory.buildingInfo, {});
      ModelBuilder modelBuilder(nullptr, Random, options, sokobanInput, &contentFactory, std::move(enemyFactory));
      auto models = prepareCampaignModels(setup, TribeAlignment::LAWFUL, std::move(modelBuilder));
      for (auto& f : models.factories)
        warlordInfo->contentFactory.merge(std::move(f));
      vector<PCreature> creatures;
      for (int index : chosen)
        creatures.push_back(
            [&]{
              for (auto& c : warlordInfo->creatures)
                if (c && c->getUniqueId() == playerInfos[index].creatureId)
                  return std::move(c);
              fail();
            }()
        );
      return Game::warlordGame(std::move(models.models), setup, std::move(creatures),
          std::move(warlordInfo->contentFactory), warlordInfo->gameIdentifier);
    }
  } else
    view->presentText("Sorry", "Failed to load the warlord file :(");*/
  return nullptr;
}

bool MainLoop::eraseSave() {
#ifdef RELEASE
  return !options->getBoolValue(OptionId::KEEP_SAVEFILES);
#endif
  return false;
}

void MainLoop::registerModPlaytime(bool started) {
#ifdef USE_STEAMWORKS
  if (!steam::Client::isAvailable())
    return;

  string currentMod = options->getStringValue(OptionId::CURRENT_MOD2);
  if (auto localVer = getLocalModVersionInfo(currentMod)) {
    steam::ItemId itemId(localVer->steamId);
    auto& ugc = steam::UGC::instance();
    if (started)
      ugc.startPlaytimeTracking({itemId});
    else
      ugc.stopPlaytimeTracking({itemId});
  }
#endif
}
