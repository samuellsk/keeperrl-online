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

#include "stdafx.h"

#include <ctime>
#include <locale>
#ifdef WINDOWS
#include <direct.h> // RAR: _chdir into server/ for --rar_gen_world (match --rar_server's own chdir)
#else
// POSIX shims for the RAR _mkdir/_chdir/_getcwd calls (server/gen/export/import/rehome paths). Windows unchanged.
#include <unistd.h>
#include <sys/stat.h>
#define _mkdir(d) ::mkdir((d), 0755)
#define _chdir(d) ::chdir(d)
#define _getcwd(b, n) ::getcwd((b), (n))
#endif

#define ProgramOptions_no_colors
#include "extern/ProgramOptions.h"

#include <exception>

#include "view.h"
#include "options.h"
#include "technology.h"
#include "music.h"
#include "test.h"
#include "tile.h"
#include "spell.h"
#include "window_view.h"
#include "file_sharing.h"
#include "highscores.h"
#include "main_loop.h"
#include "clock.h"
#include "parse_game.h"
#include "vision.h"
#include "model_builder.h"
#include "sound_library.h"
#include "audio_device.h"
#include "sokoban_input.h"
#include "keybinding_map.h"
#include "campaign_type.h"
#include "dummy_view.h"
#include "sound.h"
#include "game_config.h"
#include "name_generator.h"
#include "enemy_factory.h"
#include "tileset.h"
#include "campaign_builder.h"
#include "attack_trigger.h"
#include "fx_manager.h"
#include "fx_renderer.h"
#include "fx_view_manager.h"
#include "layout_renderer.h"
#include "unlocks.h"
#include "steam_input.h"
#include "steam_achievements.h"

#include "stack_printer.h"
#include "translations.h"
#include "rar_server.h"
#include "rar_lockstep_net.h"
#include "rar_client.h"
#include "rar_mods.h"   // rarHashDataFree: our data_free fingerprint, reported at login

#ifdef USE_STEAMWORKS
#include "steam_base.h"
#include "steam_client.h"
#include "steam_user.h"
#endif

#ifndef DATA_DIR
#define DATA_DIR "."
#endif

static void initializeRendererTiles(Renderer& r, const DirectoryPath& path) {
  r.setAnimationsDirectory(path.subdirectory("animations"));
  r.loadAnimations();
}

static double getMaxVolume() {
  return 0.7;
}

vector<pair<MusicType, FilePath>> getMusicTracks(const DirectoryPath& path, bool present) {
  if (!present)
    return {};
  else
    return {
      {MusicType::MAIN, path.file("main.ogg")},
      {MusicType::PEACEFUL, path.file("peaceful1.ogg")},
      {MusicType::PEACEFUL, path.file("peaceful2.ogg")},
      {MusicType::PEACEFUL, path.file("peaceful3.ogg")},
      {MusicType::PEACEFUL, path.file("peaceful4.ogg")},
      {MusicType::PEACEFUL, path.file("peaceful5.ogg")},
      {MusicType::DESERT, path.file("desert1.ogg")},
      {MusicType::DESERT, path.file("desert2.ogg")},
      {MusicType::SNOW, path.file("snow1.ogg")},
      {MusicType::SNOW, path.file("snow2.ogg")},
      {MusicType::BATTLE, path.file("battle1.ogg")},
      {MusicType::BATTLE, path.file("battle2.ogg")},
      {MusicType::BATTLE, path.file("battle3.ogg")},
      {MusicType::BATTLE, path.file("battle4.ogg")},
      {MusicType::BATTLE, path.file("battle5.ogg")},
      {MusicType::NIGHT, path.file("night1.ogg")},
      {MusicType::NIGHT, path.file("night2.ogg")},
      {MusicType::NIGHT, path.file("night3.ogg")},
      {MusicType::ADV_BATTLE, path.file("adv_battle1.ogg")},
      {MusicType::ADV_BATTLE, path.file("adv_battle2.ogg")},
      {MusicType::ADV_BATTLE, path.file("adv_battle3.ogg")},
      {MusicType::ADV_BATTLE, path.file("adv_battle4.ogg")},
      {MusicType::ADV_PEACEFUL, path.file("adv_peaceful1.ogg")},
      {MusicType::ADV_PEACEFUL, path.file("adv_peaceful2.ogg")},
      {MusicType::ADV_PEACEFUL, path.file("adv_peaceful3.ogg")},
      {MusicType::ADV_PEACEFUL, path.file("adv_peaceful4.ogg")},
      {MusicType::ADV_PEACEFUL, path.file("adv_peaceful5.ogg")},
  };
}

static int keeperMain(po::parser&);
static po::parser getCommandLineFlags();

static po::parser getCommandLineFlags() {
  po::parser flags;
  flags["help"].description("Print help");
  flags["steam"].description("Run with Steam");
  flags["user_dir"].type(po::string).description("Directory for options and save files");
  flags["data_dir"].type(po::string).description("Directory containing the game data");
  flags["restore_settings"].description("Restore settings to default values.");
  flags["run_tests"].description("Run all unit tests and exit");
  flags["worldgen_test"].type(po::i32).description("Test how often world generation fails");
  flags["worldgen_maps"].type(po::string).description("List of maps or enemy types in world generation test. Skip to test all.");
  flags["battle_level"].type(po::string).description("Path to battle test level");
  flags["battle_info"].type(po::string).description("Path to battle info file");
  flags["battle_enemy"].type(po::string).description("Battle enemy id");
  flags["battle_enemy_two"].type(po::string).description("Battle enemy 2 id");
  flags["endless_enemy"].type(po::string).description("Endless mode enemy index");
  flags["battle_view"].description("Open game window and display battle");
  flags["battle_rounds"].type(po::i32).description("Number of battle rounds");
  flags["layout_size"].type(po::string).description("[layout] Map size for the layout tools, e.g. 200:130");
  flags["layout_name"].type(po::string).description("[layout] TEXT renderer: dump a layout as glyphs (needs --layout_size). For the graphical one use --map_editor");
  flags["stderr"].description("Log to stderr");
  flags["console"].description("Attach windows console");
  flags["nolog"].description("No logging");
  flags["no_crash_reports"].description("Don't intercept game crashes and send crash reports to the developer");
  flags["rar_server"].type(po::i32).description("[server] Run as the RAR online server instead of starting the game. Port is optional; if omitted, uses SERVER_PORT from rar_server_config.txt (default " + toString(RAR_DEFAULT_PORT) + ")");
  flags["rar_login"].type(po::string).description("[online] RAR online: account login (temporary until the in-game login UI)");
  flags["rar_password"].type(po::string).description("[online] RAR online: account password");
  flags["rar_client_test"].description("[diag] RAR online: run a client<->server round-trip self-test and exit");
  flags["rar_lockstep_relay"].type(po::i32).description("[pvp] RAR PvP: run the lockstep transport relay on the given port (box side), then block");
  flags["rar_lockstep_nettest"].description("[pvp] RAR PvP: headless transport+protocol self-test (local relay + 2 peers), then exit");
  flags["rar_lockstep_sessiontest"].description("[pvp] RAR PvP: headless battle-session handshake + seed-exchange self-test, then exit");
  flags["rar_lockstep_gametest"].type(po::string).description("[pvp] RAR PvP: run the REAL sim through the netcode, arg 'save turns relayHost relayPort role outfile'; run twice (role 0/1) against a relay + diff");
  flags["rar_lockstep_battle"].type(po::string).description("[pvp] RAR PvP: play a live lockstep battle in a window, arg 'defenderSave invaderSave relayHost relayPort role' (role 0=defender, 1=invader)");
  flags["rar_siege_test"].description("[diag] RAR online: self-test the owner-returns-during-invasion (siege) flow, then exit");
  flags["rar_gen_worldmap"].type(po::string).description("[server] RAR online: which world-map layout --rar_gen_world builds (world_maps.txt id, or any random_layouts.txt name -- mods included)");
  flags["rar_gen_seed"].type(po::i32).description("[server] RAR online: terrain seed for --rar_gen_world (from --gen_preview) -> reproducible map");
  flags["rar_gen_world"].type(po::string).description("[server] RAR online: generate the canonical world to the given file and exit");
  flags["rar_world_selftest"].type(po::string).description("[diag] RAR online: deserialize a world file and print a summary, then exit");
  flags["rar_repair_villains"].type(po::string).description("[server] RAR online: regenerate any missing villain blobs for the given campaign file (in server/), then exit");
  flags["map_editor"].type(po::string).description("[layout] GRAPHICAL layout/world-map previewer: pick a layout, set size, reroll, read off the seed. Arg optional");
  flags["gen_preview"].type(po::string).description("[layout] Alias of --map_editor (older name)");
  flags["rar_mod_selftest"].type(po::string).description("[diag] RAR online: bundle+reinstall a mod dir and verify the hash round-trip, then exit");
  flags["dump_tribes"].description("[diag] Print the complete tribe friend/foe matrix (active mods included) and exit");
  flags["dump_workshops"].type(po::string).description("[diag] Print merged workshop recipes (active mods included) and exit; arg = group name or empty for all");
  flags["rar_compress_test"].type(po::string).description("[diag] RAR online: gzip<->lzma transcode round-trip on a .sit/.dat, then exit");
  flags["rar_load_dungeon_test"].type(po::string).description("[diag] RAR online: download+load a cached dungeon model by gameId, then exit");
  flags["rar_keeper_load_test"].type(po::string).description("[diag] RAR online: download a KEEPER blob by gameId and try to load it as a full game (what the load menu does), then exit");
  flags["rar_lockstep_selftest"].type(po::string).description("[pvp] RAR PvP: twin-sim determinism check on a save file, arg 'save.kep [turns]', then exit");
  flags["rar_lockstep_dump"].type(po::string).description("[pvp] RAR PvP: one sim run -> fingerprint file, arg 'save.kep turns seed outfile'; run twice + diff for cross-process determinism");
  flags["rar_lockstep_symtrace"].type(po::string).description("[pvp] RAR PvP: symbolized draw-stacks of the LAST turn, arg 'save.kep turns seed outfile'; run twice fresh + diff to locate cross-process divergence");
  flags["rar_export_base"].type(po::string).description("[server] RAR: export a keeper's base MODEL-ONLY, arg 'gameId outFile', then exit");
  flags["rar_import_base"].type(po::string).description("[server] RAR: rebuild a playable keeper from a base file, arg 'inFile targetGameId', then exit");
  flags["rar_rehome_keeper"].type(po::string).description("[server] RAR: swap a keeper's full blob onto the current world, arg 'gameId [targetGameId]', then exit");
  flags["rar_crash_test"].description("[diag] RAR: compress+upload any pending crashes/ reports to the server, then exit");
  flags["rar_base_selftest"].description("[diag] RAR: end-to-end export/import cycle self-test, then exit");
  flags["reload_data"].description("Dev: re-read the data files into the game on every load (config edits take effect without a restart)");
  flags["free_mode"].description("Run in free ascii mode");
  flags["gen_z_levels"].type(po::string).description("Generate and print z-level types for a given keeper");
  flags["translate_sentences"].type(po::string).description("Read translatable sentences from given file, translate them using the current language and output to stdout.");
  flags["quick_game"].description("Skip main menu and load the last save file or start a single map game");
  flags["new_game"].type(po::string).description("Skip main menu and start a single map game");
  flags["max_turns"].type(po::i32).description("Quit the game after a given max number of turns");
  flags["export_translatable_strings"].type(po::string).description("This experimental option will try to to replace translatable strings in game files with translation ids.");
  flags["export_translatable_sentences"].type(po::string).description("This experimental option will output every sentence in the game in the pre-translated form.");
  return flags;
}

#undef main

void onException() {
  if (auto ex = std::current_exception()) {
    try {
      std::rethrow_exception(ex);
    } catch (std::exception &ex) {
      FATAL << "Uncaught exception: " << ex.what();
    } catch (...) {
      FATAL << "Uncaught exception (unknown)";
    }
  } else
    FATAL << "Terminated due to unknown reason";
  // just in case FATAL doesn't crash
  abort();
}

int main(int argc, char* argv[]) {
  po::parser flags = getCommandLineFlags();
  // Make the port optional on --rar_server: the flag is typed i32, and ProgramOptions has no notion of an
  // optional value -- a bare `--rar_server` fails to parse outright ("expects an argument of type i32"), so it
  // can't be defaulted later where was_set() is checked. Inject a "0" sentinel here instead, before parsing:
  // runRarServer() reads 0 as "no explicit port" and falls back to rar_server_config.txt's SERVER_PORT (which
  // itself defaults to RAR_DEFAULT_PORT). Only a bare flag is touched: `--rar_server 1234` and
  // `--rar_server=1234` are left exactly as given and still override the config.
  std::vector<char*> args(argv, argv + argc); // std:: on purpose -- KeeperRL's `vector` wrapper has no insert()
  static string barePortSentinel = "0";
  for (int i = 0; i < (int) args.size(); ++i)
    if (string(args[i]) == "--rar_server" && (i + 1 >= (int) args.size() || args[i + 1][0] == '-')) {
      args.insert(args.begin() + i + 1, &barePortSentinel[0]); // &s[0] -> char* in every standard; .data() is const pre-C++17
      break;
    }
  if (!flags.parseArgs(args.size(), args.data()))
    return -1;
  // NOTE: --rar_server is handled further down (near --rar_gen_world), AFTER statics/paths/options are set up,
  // so the server can load the game's own content and live-replenish its villain pool (MainLoop::runRarServerFull).
  if (!flags["no_crash_reports"].was_set())
    initializeMiniDump();
  std::set_terminate(onException);
  setInitializedStatics();
  if (flags["console"].was_set())
    attachConsole();
  ofstream stringsOut;
  if (flags["export_translatable_strings"].was_set()) {
    stringsOut.open(flags["export_translatable_strings"].get().string);
    TString::enableExportingStrings(&stringsOut);
  }
  return keeperMain(flags);
}

static string getRandomInstallId(RandomGen& random) {
  string ret;
  for (int i : Range(4)) {
    ret += random.choose('e', 'u', 'i', 'o', 'a');
    ret += random.choose('q', 'w', 'r', 't', 'y', 'p', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'z', 'x', 'c', 'v', 'b',
        'n', 'm');
  }
  return ret;
}

static string getInstallId(const FilePath& path, RandomGen& random) {
  string ret;
  ifstream in(path.getPath());
  if (in)
    in >> ret;
  else {
    ret = getRandomInstallId(random);
    ofstream(path.getPath()) << ret;
  }
  return ret;
}

struct AppConfig {
  AppConfig(FilePath path) {
    if (auto error = PrettyPrinting::parseObject(values, {path}, nullptr))
      USER_FATAL << *error;
  }

  template <typename T>
  T get(const char* key) {
    if (auto value = getReferenceMaybe(values, key)) {
      if (auto ret = fromStringSafe<T>(*value))
        return *ret;
      else
        USER_FATAL << "Error reading config value: " << key << " from: " << *value;
    } else
      USER_FATAL << "Config value not found: " << key;
    fail();
  }

  bool is_true(const char* key) {
    if (auto value = getReferenceMaybe(values, key))
      if (auto ret = fromStringSafe<int>(*value))
        return *ret > 0;
    return false;
  }

  template <typename T>
  optional<T> getMaybe(const char* key) {
    if (auto value = getReferenceMaybe(values, key))
      return fromStringSafe<T>(*value);
    return none;
  }

  private:
  map<string, string> values;
};

static void showLogoSplash(Renderer& renderer, FilePath logoPath, atomic<bool>& splashDone) {
  auto logoTexture = Texture::loadMaybe(logoPath);
  while (!splashDone) {
    renderer.drawAndClearBuffer();
    sleep_for(milliseconds(30));
    if (logoTexture) {
      auto pos = (renderer.getSize() - logoTexture->getSize()) / 2;
      renderer.drawImage(pos.x, pos.y, *logoTexture);
    }
  }
}

static int keeperMain(po::parser& commandLineFlags) {
  ENABLE_PROFILER;
  if (commandLineFlags["help"].was_set()) {
    std::cout << commandLineFlags << endl;
    return 0;
  }
  FatalLog.addOutput(DebugOutput::crash());
  FatalLog.addOutput(DebugOutput::toStream(std::cerr));
  UserErrorLog.addOutput(DebugOutput::exitProgram());
  UserErrorLog.addOutput(DebugOutput::toStream(std::cerr));
  UserInfoLog.addOutput(DebugOutput::toStream(std::cerr));
  auto trigger = AttackTrigger(StolenItems{});
  CHECK(!!trigger.getReferenceMaybe<StolenItems>());
#ifndef RELEASE
  ogzstream compressedLog("log.gz");
  /*if (!commandLineFlags["nolog"].was_set())
    InfoLog.addOutput(DebugOutput::toStream(compressedLog));*/
#endif
  FatalLog.addOutput(DebugOutput::toString(
      [](const string& s) { ofstream("stacktrace.out") << s << "\n" << std::flush; } ));
  if (commandLineFlags["stderr"].was_set() || commandLineFlags["run_tests"].was_set())
    InfoLog.addOutput(DebugOutput::toStream(std::cerr));
  if (commandLineFlags["run_tests"].was_set()) {
    testAll();
    return 0;
  }
  if (commandLineFlags["new_game"].was_set())
    USER_CHECK(!commandLineFlags["new_game"].get().string.empty()) << "Please enter keeper name";
  DirectoryPath dataPath([&]() -> string {
    if (commandLineFlags["data_dir"].was_set())
      return commandLineFlags["data_dir"].get().string;
    else
      return DATA_DIR;
  }());
  auto freeDataPath = dataPath.subdirectory("data_free");
  auto paidDataPath = dataPath.subdirectory("data");
  auto contribDataPath = dataPath.subdirectory("data_contrib");
  bool tilesPresent = !commandLineFlags["free_mode"].was_set() && paidDataPath.exists();
  DirectoryPath userPath([&] () -> string {
    if (commandLineFlags["user_dir"].was_set())
      return commandLineFlags["user_dir"].get().string;
#ifdef USER_DIR
    else if (const char* userDir = USER_DIR)
      return userDir;
#endif // USER_DIR
#ifndef WINDOWS
    else if (const char* localPath = std::getenv("XDG_DATA_HOME"))
      return localPath + string("/KeeperRL");
#endif
#ifdef ENABLE_LOCAL_USER_DIR // Some environments don't define XDG_DATA_HOME
    else if (const char* homePath = std::getenv("HOME"))
      return homePath + string("/.local/share/KeeperRL");
#endif // ENABLE_LOCAL_USER_DIR
    else
      return ".";
  }());
  INFO << "Data path: " << dataPath;
  INFO << "User path: " << userPath;
  optional<int> maxTurns;
  if (commandLineFlags["max_turns"].was_set())
    maxTurns = commandLineFlags["max_turns"].get().i32;
  Clock clock(!!maxTurns);
  userPath.createIfDoesntExist();
  auto settingsPath = userPath.file("options_v1_0.txt");
  auto userKeysPath = userPath.file("keybindings.txt");
  auto highscoresPath = userPath.file("highscores_v1_0.dat");
  if (commandLineFlags["restore_settings"].was_set()) {
    settingsPath.erase();
    userKeysPath.erase();
    highscoresPath.erase();
  }
  unique_ptr<MySteamInput> steamInput;
  unique_ptr<SteamAchievements> steamAchievements;
  #ifdef RELEASE
    AppConfig appConfig(dataPath.file("appconfig.txt"));
  #else
    AppConfig appConfig(dataPath.file("appconfig-dev.txt"));
  #endif
  // Read these BEFORE the CLI tool dispatches below: those construct their own MainLoop, and MainLoop filters
  // its mod list through getLocalModVersionInfo(), which drops every mod whose version_info compatibilityTag
  // != modVersion. Passing "" there silently disabled EVERY mod for --rar_server / --rar_gen_world / etc --
  // the content factory quietly fell back to vanilla while still reporting mods as active.
  const auto modVersion = appConfig.get<string>("mod_version");
  const auto saveVersion = appConfig.get<int>("save_version");
  // NEVER fall back to offline/vanilla KeeperRL: that path is not maintained here and behaves wrongly (no
  // login, straight into keeper creation). rarConfigured() is "server_url is set", so an empty one silently
  // turned the whole game vanilla. Default it to a local server instead -- the public server list normally
  // replaces this at login anyway, and this only decides where we go when that list can't be reached.
  auto rarServerUrl = appConfig.getMaybe<string>("server_url").value_or("");
  if (rarServerUrl.empty())
    rarServerUrl = "https://localhost:" + toString(RAR_DEFAULT_PORT);
  rarInit(rarServerUrl,
      commandLineFlags["rar_login"].was_set() ? commandLineFlags["rar_login"].get().string : "",
      commandLineFlags["rar_password"].was_set() ? commandLineFlags["rar_password"].get().string : "",
      appConfig.getMaybe<string>("server_cert_pin").value_or(""),
      appConfig.getMaybe<string>("server_psk").value_or(""),
      appConfig.getMaybe<string>("server_list_url").value_or(""));
  rarSetSaveRegistry(userPath.file("rar_saves.txt").getPath());
  // RAR data protection: hash our own rule files once here (paths are known, nothing has been loaded yet) and
  // hand it to the client layer, which sends it with every login. The server compares against its own copy and
  // records who is running altered content. Purely informational to us -- the server decides what to do.
  rarSetDataFreeHash(rarHashDataFree(freeDataPath.getPath()));
  if (commandLineFlags["rar_client_test"].was_set())
    return rarClientSelfTest();
  if (commandLineFlags["rar_lockstep_nettest"].was_set())
    return rarLockstepNetTest();
  if (commandLineFlags["rar_lockstep_sessiontest"].was_set())
    return rarLockstepSessionTest();
  if (commandLineFlags["rar_lockstep_relay"].was_set()) {
    rarLockstepRelay(commandLineFlags["rar_lockstep_relay"].get().i32);
    return 0;
  }
  if (commandLineFlags["rar_siege_test"].was_set())
    return rarSiegeSelfTest("siegetest_dummy");
  #ifdef USE_STEAMWORKS
    steamInput = make_unique<MySteamInput>();
    optional<steam::Client> steamClient;
    if (appConfig.get<int>("steamworks") > 0) {
      if (steam::initAPI()) {
        steamClient.emplace();
        steamInput->init();
        steamAchievements = make_unique<SteamAchievements>();
        INFO << "\n" << steamClient->info();
      }
  #ifdef RELEASE
      else
        USER_INFO << "Unable to connect with the Steam client.";
  #endif
    }
  #else
    // Non-Steam build: the engine still uses these unconditionally, so provide
    // real (stubbed) instances instead of leaving the pointers null.
    steamInput = make_unique<MySteamInput>();
    steamAchievements = make_unique<SteamAchievements>();
  #endif
  KeybindingMap keybindingMap(freeDataPath.file("default_keybindings.txt"), userKeysPath);
  Options options(settingsPath, &keybindingMap, steamInput.get());
  if (options.getBoolValue(OptionId::DPI_AWARE))
    dpiAwareness();
  Random.init(int(time(nullptr)));
  auto installId = getInstallId(userPath.file("installId.txt"), Random);
  if (steamInput->isRunningOnDeck())
    installId += "_deck";
  AudioDevice audioDevice;
  optional<string> audioError = audioDevice.initialize();
  auto modsDir = userPath.subdirectory(gameConfigSubdir);
  auto allUnlocked = Unlocks::allUnlocked();
  if (commandLineFlags["gen_z_levels"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.genZLevels(commandLineFlags["gen_z_levels"].get().string);
    exit(0);
  }
  if (commandLineFlags["rar_server"].was_set()) {
    // Full/content-loaded server: read content via ABSOLUTE paths (so createContentFactory works regardless of
    // cwd), and do NOT chdir here -- runRarServer() does its own chdir into server/ for all its file I/O.
    auto absPaid = paidDataPath.absolute();
    auto absFree = freeDataPath.absolute();
    auto absUser = userPath.absolute();
    auto absMods = modsDir.absolute();
    MainLoop loop(nullptr, nullptr, nullptr, absPaid, absFree, absUser, absMods, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.runRarServerFull(commandLineFlags["rar_server"].get().i32);
    exit(0);
  }
  if (commandLineFlags["rar_gen_world"].was_set()) {
    // RAR: gen MUST write into the same server/ directory that --rar_server reads from (the server chdir's
    // into it at startup). Content (mods/, data_free/) lives at the TOP level, so read it via ABSOLUTE paths,
    // then chdir into server/ so every relative gen output -- rar_campaign.dat, rar_mods.txt, rar_mods/,
    // rar_villains/, rar_villain_*.txt -- lands in server/ and the server picks it up automatically.
    auto absPaid = paidDataPath.absolute();
    auto absFree = freeDataPath.absolute();
    auto absUser = userPath.absolute();
    auto absMods = modsDir.absolute();
#ifdef WINDOWS
    _mkdir("server");
    if (_chdir("server") != 0)
      std::cerr << "rar_gen_world: couldn't enter ./server directory\n";
#endif
    MainLoop loop(nullptr, nullptr, nullptr, absPaid, absFree, absUser, absMods, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    optional<int> genSeed;
    if (commandLineFlags["rar_gen_seed"].was_set())
      genSeed = commandLineFlags["rar_gen_seed"].get().i32;
    optional<string> genWorldMap;
    if (commandLineFlags["rar_gen_worldmap"].was_set())
      genWorldMap = commandLineFlags["rar_gen_worldmap"].get().string;
    loop.genServerWorld(commandLineFlags["rar_gen_world"].get().string, genSeed, genWorldMap);
    exit(0);
  }
  if (commandLineFlags["rar_world_selftest"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.testServerWorld(commandLineFlags["rar_world_selftest"].get().string);
    exit(0);
  }
  if (commandLineFlags["rar_repair_villains"].was_set()) {
    // Like gen: read content via ABSOLUTE paths, then chdir into server/ so it repairs rar_villains/ there.
    auto absPaid = paidDataPath.absolute();
    auto absFree = freeDataPath.absolute();
    auto absUser = userPath.absolute();
    auto absMods = modsDir.absolute();
#ifdef WINDOWS
    _mkdir("server");
    if (_chdir("server") != 0)
      std::cerr << "rar_repair_villains: couldn't enter ./server directory\n";
#endif
    MainLoop loop(nullptr, nullptr, nullptr, absPaid, absFree, absUser, absMods, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.repairVillains(commandLineFlags["rar_repair_villains"].get().string);
    exit(0);
  }
  // export/import base chdir into ./server (like --rar_gen_world) so rar_dungeons/, rar_campaign.dat and
  // rar_dungeonhash.txt are found there; content is read via ABSOLUTE paths. File args are made absolute BEFORE
  // the chdir so a relative in/out path still resolves against the original working dir.
  auto rarMakeAbsolute = [](string p) -> string {
#ifdef WINDOWS
    if (!p.empty() && (p.size() < 2 || p[1] != ':') && p[0] != '/' && p[0] != '\\') {
      char buf[4096];
      if (_getcwd(buf, sizeof(buf))) return string(buf) + "/" + p;
    }
#endif
    return p;
  };
  if (commandLineFlags["rar_export_base"].was_set()) {
    auto arg = commandLineFlags["rar_export_base"].get().string;
    auto sp = arg.find(' ');
    if (sp == string::npos) { std::cout << "usage: --rar_export_base 'gameId outFile'\n"; exit(1); }
    string gameId = arg.substr(0, sp), outFile = rarMakeAbsolute(arg.substr(sp + 1));
    auto absPaid = paidDataPath.absolute(); auto absFree = freeDataPath.absolute();
    auto absUser = userPath.absolute(); auto absMods = modsDir.absolute();
#ifdef WINDOWS
    _mkdir("server");
    if (_chdir("server") != 0) std::cerr << "rar_export_base: couldn't enter ./server directory\n";
#endif
    MainLoop loop(nullptr, nullptr, nullptr, absPaid, absFree, absUser, absMods, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.exportBase(gameId, outFile);
    exit(0);
  }
  if (commandLineFlags["rar_import_base"].was_set()) {
    auto arg = commandLineFlags["rar_import_base"].get().string;
    auto sp = arg.find(' ');
    if (sp == string::npos) { std::cout << "usage: --rar_import_base 'inFile targetGameId'\n"; exit(1); }
    string inFile = rarMakeAbsolute(arg.substr(0, sp)), targetGameId = arg.substr(sp + 1);
    auto absPaid = paidDataPath.absolute(); auto absFree = freeDataPath.absolute();
    auto absUser = userPath.absolute(); auto absMods = modsDir.absolute();
#ifdef WINDOWS
    _mkdir("server");
    if (_chdir("server") != 0) std::cerr << "rar_import_base: couldn't enter ./server directory\n";
#endif
    MainLoop loop(nullptr, nullptr, nullptr, absPaid, absFree, absUser, absMods, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.importBase(inFile, targetGameId);
    exit(0);
  }
  if (commandLineFlags["rar_rehome_keeper"].was_set()) {
    // Swap a keeper's FULL blob onto the current rar_campaign.dat world (no lossy model-only round-trip).
    auto arg = commandLineFlags["rar_rehome_keeper"].get().string;
    auto sp = arg.find(' ');
    string gameId = (sp == string::npos) ? arg : arg.substr(0, sp);
    string targetGameId = (sp == string::npos) ? string() : arg.substr(sp + 1);
    auto absPaid = paidDataPath.absolute(); auto absFree = freeDataPath.absolute();
    auto absUser = userPath.absolute(); auto absMods = modsDir.absolute();
#ifdef WINDOWS
    _mkdir("server");
    if (_chdir("server") != 0) std::cerr << "rar_rehome_keeper: couldn't enter ./server directory\n";
#endif
    MainLoop loop(nullptr, nullptr, nullptr, absPaid, absFree, absUser, absMods, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rehomeKeeper(gameId, targetGameId);
    exit(0);
  }
  if (commandLineFlags["rar_crash_test"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarUploadPendingCrashesNow();
    exit(0);
  }
  if (commandLineFlags["rar_base_selftest"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarBaseSelfTest();
    exit(0);
  }
  if (commandLineFlags["rar_mod_selftest"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.modSyncSelfTest(commandLineFlags["rar_mod_selftest"].get().string);
    exit(0);
  }
  if (commandLineFlags["dump_tribes"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.dumpTribes();
    exit(0);
  }
  if (commandLineFlags["dump_workshops"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.dumpWorkshops(commandLineFlags["dump_workshops"].get().string);
    exit(0);
  }
  if (commandLineFlags["rar_compress_test"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.compressSelfTest(commandLineFlags["rar_compress_test"].get().string);
    exit(0);
  }
  if (commandLineFlags["rar_keeper_load_test"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarKeeperLoadTest(commandLineFlags["rar_keeper_load_test"].get().string);
    return 0;
  }
  if (commandLineFlags["rar_load_dungeon_test"].was_set()) {
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarInvasionLoadTest(commandLineFlags["rar_load_dungeon_test"].get().string);
    exit(0);
  }
  if (commandLineFlags["rar_lockstep_selftest"].was_set()) {
    auto arg = commandLineFlags["rar_lockstep_selftest"].get().string;
    auto sp = arg.find(' ');
    string savePath = rarMakeAbsolute(sp == string::npos ? arg : arg.substr(0, sp));
    int turns = sp == string::npos ? 0 : fromString<int>(arg.substr(sp + 1));
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarLockstepSelfTest(savePath, turns);
    exit(0);
  }
  if (commandLineFlags["rar_lockstep_dump"].was_set()) {
    // arg = "<save path...> <turns> <seed> <outfile>" -- take the last 3 space-separated tokens as
    // turns/seed/outfile so the save path may itself contain spaces.
    auto arg = commandLineFlags["rar_lockstep_dump"].get().string;
    auto toks = split(arg, {' '});
    if (toks.size() < 4) { std::cout << "usage: --rar_lockstep_dump 'save.kep turns seed outfile'\n"; exit(1); }
    string outfile = rarMakeAbsolute(toks.back());
    int seed = fromString<int>(toks[toks.size() - 2]);
    int turns = fromString<int>(toks[toks.size() - 3]);
    string savePath;
    for (size_t i = 0; i + 3 < toks.size(); ++i) { if (i) savePath += " "; savePath += toks[i]; }
    savePath = rarMakeAbsolute(savePath);
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarLockstepDump(savePath, turns, seed, outfile);
    exit(0);
  }
  if (commandLineFlags["rar_lockstep_symtrace"].was_set()) {
    auto arg = commandLineFlags["rar_lockstep_symtrace"].get().string;
    auto toks = split(arg, {' '});
    if (toks.size() < 4) { std::cout << "usage: --rar_lockstep_symtrace 'save.kep turns seed outfile'\n"; exit(1); }
    string outfile = rarMakeAbsolute(toks.back());
    int seed = fromString<int>(toks[toks.size() - 2]);
    int turns = fromString<int>(toks[toks.size() - 3]);
    string savePath;
    for (size_t i = 0; i + 3 < toks.size(); ++i) { if (i) savePath += " "; savePath += toks[i]; }
    savePath = rarMakeAbsolute(savePath);
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarLockstepSymTrace(savePath, turns, seed, outfile);
    exit(0);
  }
  if (commandLineFlags["rar_lockstep_gametest"].was_set()) {
    // arg: "<save...> <turns> <relayHost> <relayPort> <role> <outfile>" -- last 5 tokens fixed, rest = save path
    auto arg = commandLineFlags["rar_lockstep_gametest"].get().string;
    auto toks = split(arg, {' '});
    if (toks.size() < 6) { std::cout << "usage: --rar_lockstep_gametest 'save turns relayHost relayPort role outfile'\n"; exit(1); }
    size_t n = toks.size();
    string outfile = rarMakeAbsolute(toks[n - 1]);
    int role = fromString<int>(toks[n - 2]);
    int port = fromString<int>(toks[n - 3]);
    string relayHost = toks[n - 4];
    int turns = fromString<int>(toks[n - 5]);
    string savePath;
    for (size_t i = 0; i + 5 < n; ++i) { if (i) savePath += " "; savePath += toks[i]; }
    savePath = rarMakeAbsolute(savePath);
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    loop.rarLockstepGameTest(savePath, turns, relayHost, port, role, outfile);
    exit(0);
  }
  // --layout_name is the TEXT/glyph renderer. It is tested here, long before the window exists, so it must
  // stand aside when the GRAPHICAL previewer was asked for: that one needs a real View + TileSet and can only
  // run further down, once they are built. Without this guard "--map_editor --layout_name=x" fell into the
  // text renderer and reported "Layout not found", which is what made the two tools so confusing.
  if (commandLineFlags["layout_name"].was_set() &&
      !commandLineFlags["map_editor"].was_set() && !commandLineFlags["gen_preview"].was_set()) {
    USER_CHECK(commandLineFlags["layout_size"].was_set()) << "Need to specify layout_size option";
    MainLoop loop(nullptr, nullptr, nullptr, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr, nullptr, nullptr,
        &allUnlocked, nullptr, nullptr, 0, modVersion);
    generateMapLayout(loop,
        commandLineFlags["layout_name"].get().string,
        freeDataPath.file("glyphs.txt"),
        commandLineFlags["layout_size"].get().string
    );
    exit(0);
  }
  SokobanInput sokobanInput(freeDataPath.file("sokoban_input.txt"), userPath.file("sokoban_state.txt"));
  string uploadUrl = appConfig.get<string>("upload_url");
  FileSharing fileSharing(uploadUrl, modVersion, saveVersion, options, installId);
  Highscores highscores(highscoresPath);
  if (commandLineFlags["worldgen_test"].was_set()) {
    ofstream output("worldgen_out.txt");
    UserInfoLog.addOutput(DebugOutput::toStream(output));
    MainLoop loop(nullptr, &highscores, &fileSharing, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr,
        &sokobanInput, nullptr, &allUnlocked, nullptr, nullptr, 0, "");
    vector<string> types;
    if (commandLineFlags["worldgen_maps"].was_set())
      types = split(commandLineFlags["worldgen_maps"].get().string, {','});
    loop.modelGenTest(commandLineFlags["worldgen_test"].get().i32, types, Random, &options);
    return 0;
  }
  auto battleTest = [&] (View* view, TileSet* tileSet) {
    MainLoop loop(view, &highscores, &fileSharing, paidDataPath, freeDataPath, userPath, modsDir, &options, nullptr,
        &sokobanInput, tileSet,  &allUnlocked, nullptr, nullptr, 0, "");
    auto level = commandLineFlags["battle_level"].get().string;
    auto numRounds = commandLineFlags["battle_rounds"].was_set() ? commandLineFlags["battle_rounds"].get().i32 : 1;
    try {
      if (commandLineFlags["endless_enemy"].was_set()) {
        auto info = commandLineFlags["battle_info"].get().string;
        auto enemy = commandLineFlags["endless_enemy"].get().string;
        optional<int> chosenEnemy;
        if (enemy != "all")
          chosenEnemy = fromString<int>(enemy);
        loop.endlessTest(numRounds, FilePath::fromFullPath(level), FilePath::fromFullPath(info), chosenEnemy);
      } else {
        auto enemyId = commandLineFlags["battle_enemy"].get().string;
        auto enemy2Id = commandLineFlags["battle_enemy_two"].get().string;
        if (enemyId == "campaign")
          loop.campaignBattleText(numRounds, FilePath::fromFullPath(level), EnemyId(enemy2Id.data()), VillainGroup("EVIL_KEEPER"));
        else
          loop.campaignBattleText(numRounds, FilePath::fromFullPath(level), EnemyId(enemy2Id.data()), EnemyId(enemyId.data()));
      }
    } catch (GameExitException) {}
  };
  if (commandLineFlags["battle_level"].was_set() && !commandLineFlags["battle_view"].was_set()) {
    battleTest(new DummyView(&clock), nullptr);
    return 0;
  }
  if (commandLineFlags["translate_sentences"].was_set()) {
    auto path = commandLineFlags["translate_sentences"].get().string;
    auto sentences = new map<TStringId, TString>();
    Translations translations(freeDataPath.subdirectory("game_config").subdirectory("translations"), modsDir, sentences);
    translations.setCurrentMods(options.getVectorStringValue(OptionId::CURRENT_MOD2));
    options.setChoices(OptionId::LANGUAGE, translations.getLanguages());
    auto res = PrettyPrinting::parseObject(*sentences, {*FilePath::fromFullPath(path).readContents()}, {path});
    if (res)
      USER_FATAL << *res;
    for (auto& elem : *sentences) {
      std::cerr << "Translating " << elem.first.data() << std::endl;
      auto res = translations.get(options.getStringValue(OptionId::LANGUAGE), elem.second);
      std::cout << elem.first.data() << " " << res << std::endl;
    }
    return 0;
  }
  Renderer renderer(
      &clock,
      steamInput.get(),
      "KeeperRL",
      contribDataPath,
      freeDataPath.file("images/mouse_cursor.png"),
      freeDataPath.file("images/mouse_cursor2.png"),
      freeDataPath.file("images/icon.png"),
      freeDataPath.file("images/map_font2.png"));
  initializeGLExtensions();

#ifndef RELEASE
  installOpenglDebugHandler();
#endif
  FatalLog.addOutput(DebugOutput::toString([&renderer](const string& s) { renderer.showError(s);}));
  UserErrorLog.addOutput(DebugOutput::toString([&renderer](const string& s) { renderer.showError(s);}));
  UserInfoLog.addOutput(DebugOutput::toString([&renderer](const string& s) { renderer.showError(s);}));
  atomic<bool> splashDone { false };
  SoundLibrary* soundLibrary = nullptr;
  auto loadThread = makeThread([&] {
    if (tilesPresent && !audioError) {
      soundLibrary = new SoundLibrary(audioDevice, paidDataPath.subdirectory("sound"));
      options.addTrigger(OptionId::SOUND, [&soundLibrary](int volume) {
        soundLibrary->setVolume(volume);
        soundLibrary->playSound(SoundId("SPELL_DECEPTION"));
      });
      soundLibrary->setVolume(options.getIntValue(OptionId::SOUND));
    } else
      soundLibrary = new SoundLibrary();
    splashDone = true;
  });
  showLogoSplash(renderer, freeDataPath.file("images/succubi.png"), splashDone);
  loadThread.join();
  map<TStringId, TString>* sentences = nullptr;
  optional<string> exportSentencesPath;
  if (commandLineFlags["export_translatable_sentences"].was_set()) {
    exportSentencesPath = commandLineFlags["export_translatable_sentences"].get().string;
    sentences = new map<TStringId, TString>();
    auto res = PrettyPrinting::parseObject(*sentences,
        {*FilePath::fromFullPath(*exportSentencesPath).readContents()},
        {*exportSentencesPath});
    if (res)
      USER_FATAL << *res;
  }
  Translations translations(freeDataPath.subdirectory("game_config").subdirectory("translations"), modsDir, sentences);
  translations.setCurrentMods(options.getVectorStringValue(OptionId::CURRENT_MOD2));
  options.setChoices(OptionId::LANGUAGE, translations.getLanguages());
  GuiFactory guiFactory(renderer, &clock, &options, &translations, soundLibrary, freeDataPath);
  TileSet tileSet(paidDataPath.subdirectory("images"), modsDir, freeDataPath.subdirectory("ui"));
  renderer.setTileSet(&tileSet);
  unique_ptr<fx::FXManager> fxManager;
  unique_ptr<fx::FXRenderer> fxRenderer;
  unique_ptr<FXViewManager> fxViewManager;
  guiFactory.loadImages();
  if (tilesPresent)
    initializeRendererTiles(renderer, paidDataPath.subdirectory("images"));
  if (paidDataPath.exists()) {
    auto particlesPath = paidDataPath.subdirectory("images").subdirectory("particles");
    if (particlesPath.exists()) {
      INFO << "FX: initialization";
      fxManager = make_unique<fx::FXManager>();
      fxRenderer = make_unique<fx::FXRenderer>(particlesPath, *fxManager);
      fxRenderer->loadTextures();
      fxViewManager = make_unique<FXViewManager>(fxManager.get(), fxRenderer.get());
    }
  }
  FileSharing bugreportSharing("http://retired.keeperrl.com/~bugreports", modVersion, saveVersion, options, installId);
  bugreportSharing.downloadPersonalMessage();
  unique_ptr<View> view;
  view.reset(WindowView::createDefaultView(
      {renderer, guiFactory, tilesPresent, &options, &clock, soundLibrary, &bugreportSharing, userPath, installId,
          appConfig.is_true("debug_options")}));
#ifndef RELEASE
  InfoLog.addOutput(DebugOutput::toString([&view](const string& s) { view->logMessage(s);}));
#endif
  view->initialize(std::move(fxRenderer), std::move(fxViewManager));
  if (commandLineFlags["battle_level"].was_set() && commandLineFlags["battle_view"].was_set()) {
    battleTest(view.get(), &tileSet);
    return 0;
  }
  Jukebox jukebox(
      audioDevice,
      getMusicTracks(paidDataPath.subdirectory("music"), tilesPresent && !audioError),
      getMaxVolume());
  options.addTrigger(OptionId::MUSIC, [&jukebox](int volume) { jukebox.setCurrentVolume(volume); });
  Unlocks unlocks(&options, userPath.file("unlocks.txt"));
  MainLoop loop(view.get(), &highscores, &fileSharing, paidDataPath, freeDataPath, userPath, modsDir, &options, &jukebox,
      &sokobanInput, &tileSet, &unlocks, steamAchievements.get(), &translations, saveVersion, modVersion);
  if (commandLineFlags["reload_data"].was_set())
    loop.setReloadDataOnLoad(true); // dev: re-read config into the game on every load
  // GRAPHICAL layout previewer. It MUST live here, after `loop` is built with a real View + TileSet -- it
  // draws with actual tile sprites. (Running it earlier off a nullptr-View MainLoop segfaults; the text
  // renderer above is the one that can run headless.) --layout_name doubles as the layout name, since
  // passing it alongside --map_editor is the obvious thing to try.
  if (commandLineFlags["map_editor"].was_set() || commandLineFlags["gen_preview"].was_set()) {
    string previewSize = commandLineFlags["layout_size"].was_set()
        ? commandLineFlags["layout_size"].get().string : "80:40";
    string previewName = commandLineFlags["map_editor"].was_set()
        ? commandLineFlags["map_editor"].get().string : commandLineFlags["gen_preview"].get().string;
    if (previewName.empty() && commandLineFlags["layout_name"].was_set())
      previewName = commandLineFlags["layout_name"].get().string;
    loop.previewLayout(previewName, previewSize);
    return 0;
  }
  if (commandLineFlags["rar_lockstep_battle"].was_set()) {
    // arg: "<defenderSave> <invaderSave> <relayHost> <relayPort> <role>" -- 5 space-separated tokens (save paths
    // must not contain spaces here).
    auto toks = split(commandLineFlags["rar_lockstep_battle"].get().string, {' '});
    if (toks.size() != 5) { std::cout << "usage: --rar_lockstep_battle 'defenderSave invaderSave relayHost relayPort role'\n"; return 1; }
    string defenderSave = rarMakeAbsolute(toks[0]);
    string invaderSave = rarMakeAbsolute(toks[1]);
    string lhost = toks[2];
    int lport = fromString<int>(toks[3]);
    int role = fromString<int>(toks[4]);
    loop.runLockstepBattle(defenderSave, invaderSave, lhost, lport, role, "battle");
    return 0;
  }
  try {
    if (audioError)
      USER_INFO << "Failed to initialize audio. The game will be started without sound. " << *audioError;
    if (commandLineFlags["quick_game"].was_set())
      loop.launchQuickGame(maxTurns, none);
    if (commandLineFlags["new_game"].was_set()) {
      USER_CHECK(!commandLineFlags["new_game"].get().string.empty());
      loop.launchQuickGame(maxTurns, commandLineFlags["new_game"].get().string);
    }
    loop.start(tilesPresent);
  } catch (GameExitException ex) {
  }
  jukebox.toggle(false);
  if (commandLineFlags["export_translatable_sentences"].was_set()) {
    ofstream out(commandLineFlags["export_translatable_sentences"].get().string);
    for (auto& elem : *sentences)
      out << elem.first.data() << " " << elem.second << "\n";
  }
  return 0;
}

