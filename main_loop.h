#pragma once

#include "util.h"
#include "file_sharing.h"
#include "exit_info.h"
#include "game_time.h"

class View;
class Highscores;
class FileSharing;
class Options;
class Jukebox;
class Campaign;
class Model;
class RetiredGames;
struct SaveFileInfo;
class GameEvents;
class SokobanInput;
struct CampaignSetup;
class ModelBuilder;
class ItemType;
class CreatureList;
class GameConfig;
class AvatarInfo;
class NameGenerator;
struct ModelTable;
class TileSet;
class ContentFactory;
class TilePaths;
struct ModVersionInfo;
struct ModDetails;
class TribeId;
struct RetiredModelInfo;
class Unlocks;
class SteamAchievements;
class TString;
class Translations;

class MainLoop {
  public:
  MainLoop(View*, Highscores*, FileSharing*, const DirectoryPath& paidDataPath, const DirectoryPath& dataFreePath,
      const DirectoryPath& userPath, const DirectoryPath& modsDir, Options*, Jukebox*, SokobanInput*, TileSet*, Unlocks*,
      SteamAchievements*, Translations*, int saveVersion, string modVersion);

  void start(bool tilesPresent);
  void modelGenTest(int numTries, const vector<std::string>& types, RandomGen&, Options*);
  void battleTest(int numTries, const FilePath& levelPath, const FilePath& battleInfoPath, string enemyId);
  int battleTest(int numTries, const FilePath& levelPath, vector<CreatureList> ally, vector<CreatureList> enemies);
  void endlessTest(int numTries, const FilePath& levelPath, const FilePath& battleInfoPath, optional<int> numEnemy);
  void campaignBattleText(int numTries, const FilePath& levelPath, EnemyId keeperId, VillainGroup);
  int campaignBattleText(int numTries, const FilePath& levelPath, EnemyId keeperId, EnemyId);
  void launchQuickGame(optional<int> maxTurns, optional<string> keeperName);
  void genZLevels(const string& keeperType);
  void rarUploadPendingCrashesNow(); // RAR: --rar_crash_test -> drive the crash upload headlessly + wait
  void genServerWorld(const string& outFile, optional<int> fixedSeed, optional<string> worldMapName); // RAR: --rar_gen_world -> serialize canonical world
  void runRarServerFull(int port); // RAR: --rar_server -> content-loaded server w/ live villain-pool replenish
  void testServerWorld(const string& inFile); // RAR: --rar_world_selftest -> round-trip summary
  void repairVillains(const string& campaignFile); // RAR: --rar_repair_villains -> regen missing villain blobs in place
  void previewLayout(const string& layoutName, const string& size); // dev tool: graphical preview of a random_layouts.txt layout
  void modSyncSelfTest(const string& modName); // RAR: --rar_mod_selftest -> bundle/install round-trip
  void dumpWorkshops(const string& group); // --dump_workshops: print merged workshop recipes (mods included)
  void showMessageWall(const string& name); // game_config/<name>.txt (mod-overridable) -> scrolling window
  bool startedNewGame = false;             // set when prepareCampaign made a game -> show the turn-0 wall
  void dumpTribes();                       // --dump_tribes: print the full friend/foe matrix from tribes.txt
  void compressSelfTest(const string& inFile);  // RAR: --rar_compress_test -> gzip<->lzma transcode round-trip
  void rarInvasionLoadTest(const string& gameId);
  void rarKeeperLoadTest(const string& gameId); // RAR: --rar_load_dungeon_test -> download+load a dungeon model
  void rarLockstepSelfTest(const string& saveFile, int numTurns); // RAR: --rar_lockstep_selftest -> twin-sim determinism check
  void rarLockstepDump(const string& saveFile, int numTurns, int seed, const string& outPath); // one sim -> hash file (fresh-process determinism test)
  void rarLockstepSymTrace(const string& saveFile, int numTurns, int seed, const string& outPath); // symbolized draw-stacks of the last turn (cross-process trace)
  void rarLockstepGameTest(const string& saveFile, int numTurns, const string& host, int port, int role, const string& outPath); // real sim through the netcode
  void runLockstepBattle(const string& defenderSave, const string& invaderSave, const string& host, int port, int role, const string& sessionId); // live PvP battle in a real window
  // Live PvP entry from the real game: both sides have exchanged packed blobs (defender base + invader keeper)
  // via the server broker; write them to temp saves and run the lockstep battle, paired on the relay by sessionId.
  void rarRunLiveInvasion(const string& defenderBlob, const string& invaderBlob, const string& sessionId, int role);
  // AUTHORITATIVE live PvP (A1): host-side. Decompress the invader's uploaded game, inject it as a site into the
  // DEFENDER's RUNNING game and transfer the invader's team onto floor 0 (retagged hostile). The defender keeps
  // playing -- no freeze, no reload. Returns the loaded invader PGame (keep alive for the battle), or null.
  PGame rarInjectLiveInvader(Game* defenderGame, const string& invaderBlob, string& diag, vector<long long>& outTeamIds,
      Vec2& outSitePos);
  // AUTHORITATIVE live PvP (A3a): serialize a game to a transport blob WITHOUT mutating it (no leaveControl/
  // clearSectors) -- the host uses this to send the current battlefield to the invader mid-play.
  string rarSerializeGameBlob(PGame& game);
  // AUTHORITATIVE live PvP (A3a): the invader loads the host's battlefield blob and views it through a
  // PlayerControl on HER collective (found via her team's creature ids), centered on her team. Interactive
  // (scroll/select) -- NOT a read-only Spectator (that exits on any key). A2 adds the live stream, A4 forwards control.
  void rarSpectateInvasion(PGame combinedGame, const string& sessionId, const set<long long>& teamIds);
  // Serialize the current game to a transport blob (raw decompressed bytes), stripping sectors + releasing control
  // first (a clean keeper-mode snapshot) -- the shared lockstep start-state this side contributes.
  string rarPackGameBlob(PGame& game);
  PModel rarLoadVillainModel(Vec2 pos);          // RAR Phase A: download+load a villain map on demand
  void exportBase(const string& gameId, const string& outFile); // RAR: extract a keeper's base MODEL-ONLY (no settings)
  void importBase(const string& inFile, const string& targetGameId); // RAR: rebuild a playable keeper from a base file
  void rehomeKeeper(const string& gameId, const string& targetGameId); // RAR: swap a full keeper blob onto the current world
  void rarBaseSelfTest();                        // RAR: end-to-end export/import cycle self-test
  ContentFactory createContentFactory(bool vanillaOnly) const;
  // Dev/experimental (--reload_data): re-read the data files into the game's ContentFactory on every game load
  // so config edits (build menu, furniture, tiles...) take effect without a full restart.
  void setReloadDataOnLoad(bool b) { reloadDataOnLoad = b; }

  private:
  bool reloadDataOnLoad = false;

  optional<RetiredGames> getRetiredGames(CampaignType);
  int getSaveVersion(const SaveFileInfo& save);
  int getSaveVersion(const SaveFileInfo& save, const DirectoryPath& dir); // online: saves/<account>/
  void uploadFile(const FilePath& path, const string& title, const SavedGameInfo&);
  void saveUI(PGame&, GameSaveType type);
  vector<SaveFileInfo> getSaveOptions(const vector<GameSaveType>&);

  void doWithSplash(const TString& text, int totalProgress, function<void(ProgressMeter&)> fun,
    function<void()> cancelFun = nullptr);

  void doWithSplash(const TString& text, function<void()> fun, function<void()> cancelFun = nullptr);

  PGame prepareCampaign(RandomGen&);
  bool rarLoginFlow(); // online: prompt for account login/password. false if cancelled.
  // RAR online mod sync (step 7): server publishes its active mods; client downloads/installs any
  // it's missing or that mismatch, then sets its active-mod list to the server's set.
  string bundleMod(const string& modName);       // serialize a mod dir's files -> bytes
  void unbundleMod(const string& modName, const string& bytes); // install bytes -> mod dir
  void publishServerMods();                       // admin/gen side: write rar_mods/ + rar_mods.txt
  bool syncServerMods();                          // client side: match local mods to the server. false=abort
  bool modsChangedThisSync = false;               // syncServerMods sets this: a mod was (re)installed or the
                                                  // enabled set changed -> the tileset must be reloaded. If the
                                                  // hashes already matched, it stays false and we skip the reload.
  // Uploads the given local save file verbatim as the server dungeon blob. Both .kep and .aut are written in
  // the same sectors-stripped form, so either is byte-compatible as a blob. KEEPER = save&exit; AUTOSAVE =
  // post-crash recovery from the load path.
  void rarUploadKeeperDungeon(PGame& game, GameSaveType type);
  PGame prepareWarlord(const SaveFileInfo&);
  enum class ExitCondition;
  ExitCondition playGame(PGame, bool withMusic, bool noAutoSave, function<optional<ExitCondition> (Game*)> = nullptr,
      milliseconds stepTimeMilli = milliseconds{3}, optional<int> maxTurns = none);
  void showCredits();
  void showAchievements();
  void showMods();
  void showServerMods(); // RAR online: READ-ONLY view of the mods the server has active. No activate/add/remove.
  void playMenuMusic();
  ModelTable prepareCampaignModels(CampaignSetup& campaign, const AvatarInfo&, RandomGen&, ContentFactory*);
  ModelTable prepareCampaignModels(CampaignSetup& campaign, const AvatarInfo&, ModelBuilder, ContentFactory*);
  PGame loadGame(const FilePath&, const TString& name);
  void rarSyncWorldOnLoad(PGame&); // RAR: replace the save's frozen world map with the server's current one
  void rarUploadPendingCrashes();  // RAR: compress+upload anything left in crashes/ (deferred from the crash)
  PGame loadServerGame(const string& gameId, const TString& name); // RAR: download+load the server blob
  PGame loadOrNewGame();
  PGame loadOrNewGameSelect(); // RAR: menu/loading loop, assumes already logged in -- recurses WITHOUT re-login
  FilePath getSavePath(const PGame&, GameSaveType);
  void eraseSaveFile(const PGame&, GameSaveType);

  bool downloadGame(const SaveFileInfo&);
  bool eraseSave();
  static vector<SaveFileInfo> getSaveFiles(const DirectoryPath& path, const string& suffix);
  bool isCompatible(int loadedVersion);

  View* view = nullptr;
  DirectoryPath paidDataPath;
  DirectoryPath dataFreePath;
  DirectoryPath userPath;
  DirectoryPath modsDir;
  Options* options = nullptr;
  Jukebox* jukebox = nullptr;
  Highscores* highscores = nullptr;
  FileSharing* fileSharing = nullptr;
  SokobanInput* sokobanInput;
  TileSet* tileSet;
  int saveVersion;
  string modVersion;
  bool rarKeeperSlain = false; // RAR: loadGame set this + returned null because the keeper was slain (not a load error)
  PModel getBaseModel(ModelBuilder&, CampaignSetup&, const AvatarInfo&);
  void considerGameEventsPrompt();
  void considerFreeVersionText(bool tilesPresent);
  void eraseAllSavesExcept(const PGame&, optional<GameSaveType>);
  PGame prepareTutorial(const ContentFactory*);
  void bugReportSave(PGame&, FilePath);
  void saveGame(PGame&, const FilePath&);
  void saveMainModel(PGame&, const FilePath& modelPath);
  string serializeModelRaw(shared_ptr<Model>, SavedGameInfo, ContentFactory*); // RAR invasion writeback
  TilePaths getTilePathsForAllMods() const;
  vector<string> getCurrentMods() const;

  optional<ModVersionInfo> getLocalModVersionInfo(const string& mod) const;
  void updateLocalModVersion(const string& mod, const ModVersionInfo&);
  optional<ModDetails> getLocalModDetails(const string& mod);
  void updateLocalModDetails(const string& mod, const ModDetails&);
  void removeMod(const string &modName);
  void removeOldSteamMod(SteamId, const string &newName);

  void registerModPlaytime(bool started);
  vector<ModInfo> getAllMods(const vector<ModInfo>& onlineMods);
  void downloadMod(ModInfo&);
  void uploadMod(ModInfo&);
  void createNewMod();
  vector<ModInfo> getOnlineMods();
  GameConfig getVanillaConfig() const;
  GameConfig getGameConfig(const vector<string>& modNames) const;
  DirectoryPath getVanillaDir() const;
  template<typename T>
  optional<T> loadFromFile(const FilePath&);
  optional<RetiredModelInfo> loadRetiredModelFromFile(const FilePath&);
  bool useSingleThread();
  Unlocks* unlocks;
  SteamAchievements* steamAchievements = nullptr;
  Translations* translations;
};
