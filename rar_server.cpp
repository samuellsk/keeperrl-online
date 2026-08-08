// KeeperRL integrated online ("RAR") server. Runs inside keeper.exe via
// `keeper.exe --rar_server <port>`. For now it provides:
//   GET  /ping                          -> "ok"
//   POST /register   body: login\npassword           -> create account
//   POST /login      body: login\npassword           -> verify credentials
//   POST /savehash   body: login\npassword\nkeeper\nhash  -> store the player's save hash
//   GET  /savehash/<login>/<keeper>     -> the stored hash (404 if none)
//   GET  /keepers/<login>               -> newline-separated list of the player's keepers
// Accounts and save-hashes are persisted to small text files next to the exe.
// Real-time PvP transport (TCP/UDP) will be added here later as a separate channel.
// TLS: the whole RAR channel (credentials + save blobs) must not travel in cleartext, and a plain-HTTP
// listener answers any stray byte with "HTTP/1.1 400 Bad Request", advertising itself to port scanners.
// Under TLS a non-TLS probe just fails the handshake, so the port stops announcing what it is.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "extern/httplib.h"
#include "rar_server.h"
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>
#include "rar_hash.h"
#include "rar_mods.h"

#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <vector>
#include <string>
#include <random>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <thread>
#include <chrono>
#include <algorithm>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
// POSIX shims so the Windows _mkdir/_chdir below compile on Linux (the server's target). The _WIN32 branch is
// left exactly as it was, so the Windows build is unchanged.
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#define _mkdir(d) ::mkdir((d), 0755)
#define _chdir(d) ::chdir(d)
#define closesocket(s) ::close(s)
#endif

namespace {

inline void ensureDir(const char* d) { _mkdir(d); } // -1/EEXIST if present is fine

// IDENTITY: a keeper is identified by the composite gameId "<account>~<keeper>" (deterministic per account +
// keeper name -> a re-save always overwrites the same slot, so an account can never accumulate two keepers
// with the same name). Its server blob lives at saves/<account>/<keeper>.dat -- split the composite back into
// that folder path here. Returns "" for a malformed id (no '~', or path-traversal chars); callers treat "" as
// "no blob". The '~' separator has no '/' or '..', so it passes every existing path-traversal guard as-is.
std::string dungeonPath(const std::string& gameId) {
  auto sep = gameId.find('~');
  if (sep == std::string::npos || gameId.find('/') != std::string::npos || gameId.find("..") != std::string::npos)
    return "";
  return "saves/" + gameId.substr(0, sep) + "/" + gameId.substr(sep + 1) + "/dungeon.dat";
}
// Create saves/<account>/ before writing a blob into it.
void ensureDungeonDir(const std::string& gameId) {
  auto sep = gameId.find('~');
  if (sep != std::string::npos) {
    ensureDir("saves");
    ensureDir(("saves/" + gameId.substr(0, sep)).c_str());
    ensureDir(("saves/" + gameId.substr(0, sep) + "/" + gameId.substr(sep + 1)).c_str());
  }
}

// Immediate subdirectory names of `path` (no "." / ".."). Empty vector if the directory doesn't exist.
std::vector<std::string> listSubdirs(const std::string& path) {
  std::vector<std::string> out;
#ifdef _WIN32
  struct _finddata_t fd;
  auto h = _findfirst((path + "/*").c_str(), &fd);
  if (h == -1) return out;
  do {
    std::string n = fd.name;
    if (n != "." && n != ".." && (fd.attrib & _A_SUBDIR))
      out.push_back(n);
  } while (_findnext(h, &fd) == 0);
  _findclose(h);
#else
  DIR* d = ::opendir(path.c_str());
  if (!d) return out;
  while (auto* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    struct stat st;
    if (::stat((path + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      out.push_back(n);
  }
  ::closedir(d);
#endif
  return out;
}

// PER-KEEPER STORAGE. Everything the server knows about one keeper lives in ONE directory:
//   saves/<account>/<keeper>/dungeon.dat   the blob
//   saves/<account>/<keeper>/keeper.txt    hashes, claimed tile, turns, invaded/conquered flags, biome
// so a keeper can be backed up, restored or moved to another server by copying that folder -- instead of
// surgically editing seven global files that each hold one column of every keeper's state.
std::string keeperDir(const std::string& gameId) {
  auto sep = gameId.find('~');
  if (sep == std::string::npos || gameId.find('/') != std::string::npos || gameId.find("..") != std::string::npos)
    return "";
  return "saves/" + gameId.substr(0, sep) + "/" + gameId.substr(sep + 1);
}
std::string keeperFile(const std::string& gameId) {
  auto d = keeperDir(gameId);
  return d.empty() ? "" : d + "/keeper.txt";
}

// ACCOUNT ROLE. "developer" unlocks content that ordinary players must not be able to pick (a boss keeper,
// the dev build-menu tools). Everyone still RUNS the same mods -- the content has to exist in the world for
// everybody, since the developer plays that keeper against them -- only what you can SELECT differs.
// Stored per account at saves/<account>/account.txt, same reasoning as the per-keeper folders: an account is
// then one directory to back up or move, with no global table to keep in sync.
std::map<std::string, std::string> g_accountRole;  // login -> "developer" (absent/anything else = ordinary user)

std::mutex g_mutex;
std::map<std::string, std::string> g_accounts;    // login -> password hash
// Single-session enforcement: one account may be logged in on ONE computer at a time. Each client has a
// per-process token; the holder heartbeats every minute. A session with no heartbeat for SESSION_TTL is
// considered dead (client crashed / flag not cleared) and is freed so the account can log in again.
struct SessionInfo { std::string token; std::time_t lastActivity; };
std::map<std::string, SessionInfo> g_sessions;    // login -> live session (IN-MEMORY only; restart clears)
// Tunables below are overridable at runtime from rar_server_config.txt (see loadServerConfig) -- non-const so
// the admin can retune timings/policy WITHOUT a rebuild. Values here are the production defaults.
const char* SERVER_CONFIG_FILE = "rar_server_config.txt"; // optional; absent => the defaults below stand
int SERVER_PORT = RAR_DEFAULT_PORT;               // TCP port to bind; an explicit `--rar_server <port>` still wins
int POOL_REPLENISH_SECS = 30;                     // how often the full server re-checks villain-pool depth
std::time_t SESSION_TTL = 300;                    // 5 min without a heartbeat => session is stale
std::map<std::string, std::string> g_saveHashes;  // "login\tkeeper" -> hash of the last CLEAN save (.kep)
// "login\tkeeper" -> hash of the last AUTOSAVE (.aut). Deliberately a SEPARATE field from g_saveHashes: the
// two files mean different things. A .kep is a clean save & exit and its state was also uploaded as the
// dungeon blob; a .aut only exists when the game never exited cleanly (a crash), and its state is NOT on the
// server yet. Keeping one hash for both would make it impossible to tell, on load, whether the local file is
// a legitimate post-crash autosave or a tampered save.
std::map<std::string, std::string> g_autosaveHashes;
// A claimed world-map site. perm=false => temporary (game started, not yet saved);
// perm=true => permanent (saved keeper, held even while the owner is offline).
struct Claim { std::string login; std::string gameId; std::string name; bool perm = false; };
std::map<std::string, Claim> g_claims;             // "x,y" -> Claim
// Invasion reservation lock: at most one invader may hold a given (offline) dungeon at a time.
// Kept in memory only -- a server restart clears all locks (safe default: better than stuck locks).
// Each holds an expiry so a crashed/killed invader who never released frees up automatically.
struct Reservation { std::string login; std::time_t expires = 0; };
std::map<std::string, Reservation> g_reservations; // gameId -> Reservation
// Dungeons whose invader was FORCE-EVICTED because the owner's grace ran out (he closed the game or
// crashed while inside). A writeback arriving after that point is stale -- the owner has been let in and
// may already be playing -- so it is refused rather than allowed to clobber him.
std::map<std::string, std::time_t> g_evicted;
std::time_t EVICTED_TTL = 3600;
// TEMPORARY: treat EVERY account as a developer, regardless of its account.txt. Set ALL_DEVELOPERS<TAB>0 in
// rar_server_config.txt (or delete the line) to go back to per-account roles -- the roles themselves are kept,
// this only overrides the answer.
int ALL_DEVELOPERS = 1;
std::time_t RESERVE_TTL = 1200;                    // 20 min; refreshed on hold, cleared on release
// SIEGE: the owner of an offline dungeon came back while an invader is inside. The invader gets a short grace
// to retreat, then his client force-exits control (which walks his team home = invasion over). Both sides poll
// this state. Cleared when the reservation is released.
std::map<std::string, std::time_t> g_ownerReturn;  // gameId -> deadline the invader is forced out at
std::time_t OWNER_RETURN_GRACE = 60;               // 1 min to get out voluntarily
// After a siege the returning owner is protected from ANY new invasion for a while, so he can't be
// perma-locked out of his own dungeon by attackers queueing up one after another.
std::map<std::string, std::time_t> g_siegeProtected; // gameId -> protected until
std::time_t SIEGE_PROTECT_TTL = 1800;              // 30 min
// 4c reconcile: gameIds whose server dungeon was damaged by an INVADER since the owner last saved.
// Set on an invader's writeback, cleared on the owner's own re-save. Persisted so the flag survives a
// server restart (the owner may still be offline). The owner checks this on continue -> adopt the copy.
std::set<std::string> g_invaded;
const char* INVADED_FILE = "rar_invaded.txt";
// Anticheat/invasion: per-dungeon hash of the RAW game bytes of the current blob. Updated by whoever
// uploads (owner OR invader). The owner compares it to their local copy on load; if it differs the
// server copy is authoritative (invaded or tampered -> load server).
std::map<std::string, std::string> g_dungeonHash;
const char* DUNGEONHASH_FILE = "rar_dungeonhash.txt";
// Conquered keepers (leader slain during an invasion): gameId -> unix time of conquest. The base stays on
// the world map for CONQUERED_TTL, then is removed entirely (claim + blob + hash). Not re-invadeable.
std::map<std::string, std::string> g_conquered;   // gameId -> conquest time (decimal string)
std::map<std::string, std::string> g_conqueredBy; // gameId -> name of the slayer (shown to the owner on load)

// RAR live PvP: an in-flight live-invasion, brokered here so the invader and the ONLINE defender find each
// other, agree on a shared RNG seed, and exchange the packed start-state blob. The real-time tick traffic does
// NOT go through here -- it uses the separate lockstep relay (RAR_LOCKSTEP_DEFAULT_PORT). In-memory only: a
// server restart cancels any battle mid-flight, which is fine (both clients fall back to their normal games).
struct PvpSession {
  std::string targetGameId;   // the defender's gameId being invaded
  std::string invaderLogin;   // who started it
  std::string invaderName;    // shown to the defender ("you are being invaded by ...")
  int seed = 0;               // shared lockstep RNG seed -- both peers Random.init(seed)
  std::time_t created = 0;
  bool defenderJoined = false;
  bool invaderJoined = false;
  std::string defenderBlob;   // the defender's packed base (uploaded by defender, fetched by invader)
  std::string invaderBlob;    // the invader's packed keeper (uploaded by invader, fetched by defender)
};
std::map<std::string, PvpSession> g_pvpSessions;  // sessionId -> live PvP session
// Keepers currently AWAY raiding someone else. Their base is undefended while they're out, so they must not be
// invadable until they get home. gameId -> when they left (a stale entry expires, so a crash can't protect a
// keeper forever).
std::map<std::string, std::time_t> g_awayKeepers;
std::time_t AWAY_TTL = 3600;
std::time_t PVP_SESSION_TTL = 120;                // an unclaimed/half-open invite expires after 2 min
const char* CONQUERED_FILE = "rar_conquered.txt";
const char* CONQUEREDBY_FILE = "rar_conquered_by.txt";
std::time_t CONQUERED_TTL = 1800; // 30 min (was a leftover 60s TESTING value); rar_server_config.txt overrides
// A defeated villain lingers on the world map as a lootable "aftermath" (defeated sprite, revisitable) for
// this long, then is removed entirely (blob deleted, dropped from the roster). Its tier already respawned
// elsewhere at defeat time, so this is purely the corpse grace window.
std::time_t VILLAIN_DEFEAT_TTL = 300; // 5 minutes; rar_server_config.txt overrides
// Newbie invasion protection: a keeper can't be invaded by others until it has been PLAYED for this many
// turns (ACTUAL in-game turns, reported on each save; offline time doesn't count). Stops players spamming
// the world with un-played keepers just to annoy others; drops off once they're genuinely established.
long long PROTECTION_TURNS = 48000; // ~3 days of play (was a leftover 1000 TESTING value); config overrides
std::map<std::string, std::string> g_playedTurns;   // gameId -> actual-play turn count (decimal string)
const char* PLAYEDTURNS_FILE = "rar_played_turns.txt";
// Phase B: villain roster for alive/dead tracking + pool-based respawn. Loaded from the --rar_gen_world
// manifests (plain text, content-free). Position key = "x_y". defeatTime=0 while alive; set to the unix
// defeat time once killed (drives the 5-min lootable grace before removal).
// A VILLAIN IS ITS FILE. Identity is a random id (the filename in rar_villains/), NEVER the map position:
// position is just an attribute, so Knights respawning on a tile some long-dead Knights once held is a
// genuinely different villain rather than the same one resurrected. ONE folder holds them all -- a villain
// with a position is placed on the map, one without is a spare (that folder IS the old "pool"). The biome its
// interior was generated for travels with it, so a villain can always be re-placed onto a matching tile.
struct Villain {
  std::string tier;      // MAIN / LESSER / MINOR / ALLY
  std::string enemyId;
  std::string biome;     // biome the interior was BUILT for -- must match the tile it sits on
  std::string pos;       // "x_y" while placed on the map; empty => unplaced spare
  bool alive = true;
  std::time_t defeatTime = 0; // set on defeat; drives the lootable grace before removal
};
std::map<std::string, Villain> g_villains;        // villainId -> villain. The FOLDER is the source of truth.
std::map<std::string, std::string> g_villainAt;   // index: "x_y" -> villainId (placed villains only)
std::map<std::string, std::string> g_villainSlots;// rar_villain_slots.txt: candidate EMPTY land tiles "x_y" -> biome
std::map<std::string, int> g_villainMinAlive;     // rar_villain_config.txt: tier -> min alive before respawn
std::map<std::string, int> g_villainPoolTarget;   // rar_villain_config.txt POOL_<tier>: UNUSED spares to keep PER villain type
RarVillainGen g_villainGen;                        // set by the "full" server -> live pool replenish (empty = off)
// Every (tier, enemyId, biome) the WORLD uses, handed to us by the full server (which CAN read rar_campaign.dat).
// This is what the pool is replenished AGAINST. It deliberately does NOT come from the pool itself: deriving the
// combo list from the existing spares meant the pool could only ever top up combos it ALREADY had, so a newly
// configured tier (ALLY) or villain could never appear without a world regen.
std::vector<RarVillainCombo> g_villainCombos;
// Combos whose generation failed (e.g. a faction that can't actually build on that biome). Remembered for the
// process lifetime so a permanently-failing combo doesn't burn a full dungeon-build attempt every cycle.
std::set<std::string> g_villainComboFailed;
const char* VILLAIN_STATE_FILE = "rar_villain_state.txt"; // dynamic: dead + consumed spares + respawned villains
const char* ACCOUNTS_FILE = "rar_accounts.txt";
const char* HASHES_FILE = "rar_savehashes.txt";
const char* AUTOHASHES_FILE = "rar_autosavehashes.txt";
const char* CLAIMS_FILE = "rar_claims.txt";
const std::string SALT = "keeperrl-rar-v1-";

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == '\n' || c == '\r') {
      if (!cur.empty() || c == '\n') out.push_back(cur);
      cur.clear();
    } else
      cur += c;
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

// std::getline strips only '\n', so a file written on Windows (CRLF) leaves a trailing '\r' on every value.
// That '\r' silently breaks equality checks -- e.g. an account's stored password hash becomes "<hash>\r" and
// never matches pwHash(pw), so EVERY account fails to log in. Strip it here so state/config files load
// identically no matter which OS wrote them. Used everywhere we'd otherwise call std::getline.
static bool getlineCR(std::istream& in, std::string& out) {
  if (!std::getline(in, out)) return false;
  if (!out.empty() && out.back() == '\r') out.pop_back();
  return true;
}

void loadKV(const char* path, std::map<std::string, std::string>& m) {
  std::ifstream in(path);
  std::string line;
  while (getlineCR(in, line)) {
    if (line.empty()) continue;
    auto tab = line.rfind('\t');
    if (tab == std::string::npos) continue;
    m[line.substr(0, tab)] = line.substr(tab + 1);
  }
}

void saveKV(const char* path, const std::map<std::string, std::string>& m) {
  std::ofstream out(path, std::ios::trunc);
  for (auto& e : m) out << e.first << '\t' << e.second << '\n';
}

std::string pwHash(const std::string& pw) { return rarHashHex(SALT + pw); }

std::vector<std::string> splitTabs(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) { if (c == '\t') { out.push_back(cur); cur.clear(); } else cur += c; }
  out.push_back(cur);
  return out;
}

void loadClaims() {
  std::ifstream in(CLAIMS_FILE);
  std::string line;
  while (getlineCR(in, line)) {
    if (line.empty()) continue;
    auto f = splitTabs(line); // x, y, login, gameId, perm, name
    if (f.size() >= 6)
      g_claims[f[0] + "," + f[1]] = Claim{ f[2], f[3], f[5], f[4] == "1" };
  }
}

void saveClaims() {
  std::ofstream out(CLAIMS_FILE, std::ios::trunc);
  for (auto& e : g_claims) {
    auto comma = e.first.find(',');
    out << e.first.substr(0, comma) << '\t' << e.first.substr(comma + 1) << '\t'
        << e.second.login << '\t' << e.second.gameId << '\t' << (e.second.perm ? "1" : "0")
        << '\t' << e.second.name << '\n';
  }
}


// The biome each keeper's dungeon was built for -- recorded from the tile it first claimed. A world regen can
// change what biome a tile is; a keeper found sitting on the wrong one is treated as having been MOVED and is
// re-placed onto a matching tile (same rule the villains already use).
std::map<std::string, std::string> g_keeperBiome;  // gameId -> biome

// Write EVERYTHING the server knows about one keeper. Call under g_mutex, after changing any of its state.
// Replaces the old pattern of rewriting a global file (every keeper's column) on every single change.
void saveKeeper(const std::string& gameId) {
  auto path = keeperFile(gameId);
  if (path.empty())
    return;
  ensureDungeonDir(gameId);
  auto sep = gameId.find('~');
  std::string login = gameId.substr(0, sep);
  std::string hashKey = login + "\t" + gameId;
  std::ofstream out(path, std::ios::trunc);
  auto put = [&](const char* k, const std::string& v) { if (!v.empty()) out << k << '\t' << v << '\n'; };
  auto get = [](const std::map<std::string, std::string>& m, const std::string& k) {
    auto it = m.find(k); return it == m.end() ? std::string() : it->second;
  };
  put("savehash", get(g_saveHashes, hashKey));
  put("autosavehash", get(g_autosaveHashes, hashKey));
  put("dungeonhash", get(g_dungeonHash, gameId));
  put("turns", get(g_playedTurns, gameId));
  put("conquered", get(g_conquered, gameId));
  put("conquered_by", get(g_conqueredBy, gameId));
  put("biome", get(g_keeperBiome, gameId));
  if (g_invaded.count(gameId))
    out << "invaded\t1\n";
  for (auto& e : g_claims)             // the claim lives with its owner, not in a global tile table
    if (e.second.gameId == gameId) {
      auto comma = e.first.find(',');
      out << "claim_x\t" << e.first.substr(0, comma) << '\n';
      out << "claim_y\t" << e.first.substr(comma + 1) << '\n';
      out << "claim_perm\t" << (e.second.perm ? "1" : "0") << '\n';
      out << "claim_login\t" << e.second.login << '\n';
      put("claim_name", e.second.name);
      break;
    }
}

// Rebuild every in-memory map by walking saves/<account>/<keeper>/keeper.txt. Call under g_mutex.
int loadKeepers() {
  int n = 0;
  for (auto& acct : listSubdirs("saves"))
    for (auto& keeper : listSubdirs("saves/" + acct)) {
      std::string gameId = acct + "~" + keeper;
      std::ifstream in("saves/" + acct + "/" + keeper + "/keeper.txt");
      if (!in)
        continue;
      std::string line, cx, cy, cname, clogin, cperm = "0";
      bool haveClaim = false;
      std::string hashKey = acct + "\t" + gameId;
      while (getlineCR(in, line)) {
        auto tab = line.find('\t');
        if (tab == std::string::npos) continue;
        auto k = line.substr(0, tab), v = line.substr(tab + 1);
        if (k == "savehash") g_saveHashes[hashKey] = v;
        else if (k == "autosavehash") g_autosaveHashes[hashKey] = v;
        else if (k == "dungeonhash") g_dungeonHash[gameId] = v;
        else if (k == "turns") g_playedTurns[gameId] = v;
        else if (k == "conquered") g_conquered[gameId] = v;
        else if (k == "conquered_by") g_conqueredBy[gameId] = v;
        else if (k == "biome") g_keeperBiome[gameId] = v;
        else if (k == "invaded") { if (v == "1") g_invaded.insert(gameId); }
        else if (k == "claim_x") { cx = v; haveClaim = true; }
        else if (k == "claim_y") cy = v;
        else if (k == "claim_perm") cperm = v;
        else if (k == "claim_name") cname = v;
        else if (k == "claim_login") clogin = v;
      }
      if (haveClaim && !cy.empty())
        g_claims[cx + "," + cy] = Claim{ clogin.empty() ? acct : clogin, gameId, cname, cperm == "1" };
      ++n;
    }
  return n;
}

// Remove a keeper's whole folder: blob + keeper.txt + the directory itself. Without this the leftover
// keeper.txt is read back by loadKeepers() on the next start and the deleted keeper walks again.
void removeKeeperDir(const std::string& gameId) {
  auto dir = keeperDir(gameId);
  if (dir.empty())
    return;
  std::remove(dungeonPath(gameId).c_str());
  std::remove(keeperFile(gameId).c_str());
#ifdef _WIN32
  _rmdir(dir.c_str());
#else
  ::rmdir(dir.c_str());
#endif
}

void loadInvaded();   // defined below (needs g_invaded); used by the one-time migration above
void loadClaims();

std::string accountFile(const std::string& login) {
  if (login.empty() || login.find('/') != std::string::npos || login.find("..") != std::string::npos)
    return "";
  return "saves/" + login + "/account.txt";
}

void saveAccount(const std::string& login) {   // call under g_mutex
  auto path = accountFile(login);
  if (path.empty())
    return;
  ensureDir("saves");
  ensureDir(("saves/" + login).c_str());
  std::ofstream out(path, std::ios::trunc);
  auto it = g_accountRole.find(login);
  out << "role\t" << (it == g_accountRole.end() ? std::string("user") : it->second) << "\n";
}

void loadAccounts() {   // call under g_mutex
  for (auto& acct : listSubdirs("saves")) {
    std::ifstream in("saves/" + acct + "/account.txt");
    if (!in)
      continue;
    std::string line;
    while (getlineCR(in, line)) {
      auto tab = line.find('\t');
      if (tab == std::string::npos) continue;
      if (line.substr(0, tab) == "role")
        g_accountRole[acct] = line.substr(tab + 1);
    }
  }
}

// "developer" or "user". Unknown accounts are ordinary users -- the gate fails CLOSED.
std::string roleOf(const std::string& login) {
  if (ALL_DEVELOPERS)
    return "developer";   // temporary global override, see ALL_DEVELOPERS
  auto it = g_accountRole.find(login);
  return (it != g_accountRole.end() && it->second == "developer") ? "developer" : "user";
}

// Every gameId the server currently knows anything about.
std::set<std::string> allKeeperIds() {
  std::set<std::string> ids;
  for (auto& e : g_dungeonHash) ids.insert(e.first);
  for (auto& e : g_playedTurns) ids.insert(e.first);
  for (auto& e : g_conquered) ids.insert(e.first);
  for (auto& e : g_conqueredBy) ids.insert(e.first);
  for (auto& e : g_keeperBiome) ids.insert(e.first);
  for (auto& g : g_invaded) ids.insert(g);
  for (auto& e : g_claims) ids.insert(e.second.gameId);
  for (auto& m : { g_saveHashes, g_autosaveHashes })
    for (auto& e : m) {                       // key is "login	gameId"
      auto tab = e.first.find('	');
      if (tab != std::string::npos) ids.insert(e.first.substr(tab + 1));
    }
  return ids;
}

// Persist every keeper. The call sites below used to rewrite a whole global file per change, so this is the
// same work and the same semantics -- just fanned out into the per-keeper folders. Call under g_mutex.
void saveAllKeepers() {
  for (auto& id : allKeeperIds())
    saveKeeper(id);
}

// ONE-TIME migration from the old global files. Runs only while they are still present: everything is read into
// memory the old way, written back out per keeper, and the old files renamed to *.migrated (kept, not deleted --
// this touches live player data). Idempotent: once renamed, the next start goes straight to loadKeepers().
bool migrateFlatFiles() {
  std::ifstream probe(HASHES_FILE);
  if (!probe)
    return false;
  probe.close();
  loadKV(HASHES_FILE, g_saveHashes);
  loadKV(AUTOHASHES_FILE, g_autosaveHashes);
  loadKV(DUNGEONHASH_FILE, g_dungeonHash);
  loadKV(PLAYEDTURNS_FILE, g_playedTurns);
  loadKV(CONQUERED_FILE, g_conquered);
  loadKV(CONQUEREDBY_FILE, g_conqueredBy);
  loadClaims();
  loadInvaded();
  // Every gameId mentioned anywhere becomes a folder.
  std::set<std::string> ids;
  for (auto& e : g_dungeonHash) ids.insert(e.first);
  for (auto& e : g_playedTurns) ids.insert(e.first);
  for (auto& e : g_conquered) ids.insert(e.first);
  for (auto& g : g_invaded) ids.insert(g);
  for (auto& e : g_claims) ids.insert(e.second.gameId);
  for (auto& m : { g_saveHashes, g_autosaveHashes })
    for (auto& e : m) {                       // key is "login\tgameId"
      auto tab = e.first.find('\t');
      if (tab != std::string::npos) ids.insert(e.first.substr(tab + 1));
    }
  int moved = 0;
  for (auto& gameId : ids) {
    auto sep = gameId.find('~');
    if (sep == std::string::npos) continue;
    ensureDungeonDir(gameId);
    // the blob moves from saves/<acct>/<keeper>.dat into the keeper's own folder
    std::string oldBlob = "saves/" + gameId.substr(0, sep) + "/" + gameId.substr(sep + 1) + ".dat";
    std::ifstream chk(oldBlob, std::ios::binary);
    if (chk) { chk.close(); if (std::rename(oldBlob.c_str(), dungeonPath(gameId).c_str()) == 0) ++moved; }
    saveKeeper(gameId);
  }
  for (const char* f : { HASHES_FILE, AUTOHASHES_FILE, DUNGEONHASH_FILE, PLAYEDTURNS_FILE,
                         CONQUERED_FILE, CONQUEREDBY_FILE, CLAIMS_FILE, INVADED_FILE })
    std::rename(f, (std::string(f) + ".migrated").c_str());
  std::printf("RAR server: migrated %d keeper(s) to saves/<account>/<keeper>/ (%d blob(s) moved); "
      "old files kept as *.migrated\n", (int) ids.size(), moved);
  std::fflush(stdout);
  return true;
}

// The one shared world: a seed + fixed worldgen params so every client that joins
// generates the identical world map. Generated once, then persisted.
struct World { int seed; int mainV; int lesserV; int minorV; int allies; int retiredV; std::string name; };
World g_world;
const char* WORLD_FILE = "rar_world.txt";

void loadOrGenWorld() {
  std::ifstream in(WORLD_FILE);
  std::vector<std::string> lines;
  std::string line;
  while (getlineCR(in, line)) lines.push_back(line);
  if (lines.size() >= 7) {
    g_world = { std::stoi(lines[0]), std::stoi(lines[1]), std::stoi(lines[2]),
        std::stoi(lines[3]), std::stoi(lines[4]), std::stoi(lines[5]), lines[6] };
  } else {
    std::srand((unsigned) std::time(nullptr));
    g_world = { std::rand(), 12, 12, 16, 5, 0, "RAR World" };
    std::ofstream out(WORLD_FILE, std::ios::trunc);
    out << g_world.seed << "\n" << g_world.mainV << "\n" << g_world.lesserV << "\n"
        << g_world.minorV << "\n" << g_world.allies << "\n" << g_world.retiredV << "\n" << g_world.name << "\n";
  }
}

bool authOk(const std::string& login, const std::string& pw) {
  auto it = g_accounts.find(login);
  return it != g_accounts.end() && it->second == pwHash(pw);
}

// True if 'login' has a LIVE session owned by a DIFFERENT token (=> another computer). A session whose
// last heartbeat is older than SESSION_TTL is treated as dead (flag not cleared properly) and freed here,
// so a fresh login is allowed. call under g_mutex.
bool sessionHeldByOther(const std::string& login, const std::string& token) {
  auto it = g_sessions.find(login);
  if (it == g_sessions.end())
    return false;
  if (std::time(nullptr) - it->second.lastActivity > SESSION_TTL) {
    g_sessions.erase(it); // stale: no activity within 5 min -> clear + allow
    return false;
  }
  return it->second.token != token;
}

void purgeExpiredReservations() { // call under g_mutex
  std::time_t now = std::time(nullptr);
  for (auto it = g_reservations.begin(); it != g_reservations.end();)
    if (it->second.expires <= now) it = g_reservations.erase(it); else ++it;
  // An eviction record only needs to outlive the evicted client's attempt to write back.
  for (auto it = g_evicted.begin(); it != g_evicted.end();)
    if (now - it->second > EVICTED_TTL) it = g_evicted.erase(it); else ++it;
}

// True if 'login' has a heartbeat within SESSION_TTL (i.e. a client is currently playing). call under g_mutex.
bool isOnline(const std::string& login) {
  auto it = g_sessions.find(login);
  return it != g_sessions.end() && std::time(nullptr) - it->second.lastActivity <= SESSION_TTL;
}

// Drop live-PvP sessions older than PVP_SESSION_TTL. A battle handshake completes in seconds (the defender polls
// every ~4s), so any session this old is a stale orphan -- INCLUDING ones the invader uploaded to but that were
// never consumed (those used to linger forever and re-fire "phantom" invasions on the target). call under g_mutex.
void purgeExpiredAway() { // call under g_mutex
  std::time_t now = std::time(nullptr);
  for (auto it = g_awayKeepers.begin(); it != g_awayKeepers.end();)
    if (now - it->second > AWAY_TTL) it = g_awayKeepers.erase(it); else ++it;
}

void purgeExpiredPvp() {
  std::time_t now = std::time(nullptr);
  for (auto it = g_pvpSessions.begin(); it != g_pvpSessions.end();)
    if (now - it->second.created > PVP_SESSION_TTL)
      it = g_pvpSessions.erase(it);
    else ++it;
}

// A short random hex session id for a live PvP battle. call under g_mutex.
std::string newPvpSessionId() {
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < 16; ++i)
    s += hex[std::rand() & 0xF];
  return s;
}

void loadInvaded() { // call under g_mutex
  std::ifstream in(INVADED_FILE);
  std::string line;
  while (getlineCR(in, line)) if (!line.empty()) g_invaded.insert(line);
}

void saveInvaded() { // call under g_mutex
  std::ofstream out(INVADED_FILE, std::ios::trunc);
  for (auto& g : g_invaded) out << g << '\n';
}

// The login that owns the claim for this gameId, or "" if none. call under g_mutex.
std::string claimOwnerOf(const std::string& gameId) {
  for (auto& e : g_claims)
    if (e.second.gameId == gameId) return e.second.login;
  return "";
}
// The keeper/base NAME a player chose for this dungeon (the world-map label, e.g. "world three"), from its
// claim. Used to name the slayer on the victim's slain screen -- NOT the killer creature's random first name.
std::string claimNameOf(const std::string& gameId) {
  for (auto& e : g_claims)
    if (e.second.gameId == gameId) return e.second.name;
  return "";
}
// Newbie invasion protection: true while a keeper has been played fewer than PROTECTION_TURNS actual turns.
// Unknown (a pre-feature keeper that never reported) counts as NOT protected. call under g_mutex.
bool isNewbieProtected(const std::string& gameId) {
  auto it = g_playedTurns.find(gameId);
  if (it == g_playedTurns.end())
    return false;
  try { return std::stoll(it->second) < PROTECTION_TURNS; } catch (...) { return false; }
}

// When a conquered keeper's map-husk grace elapses, remove it from the WORLD MAP (claim + aftermath blob +
// hash + invaded flag) and stop the grace timer -- BUT keep g_conqueredBy. The slain record must OUTLIVE the
// map husk: the victim is usually offline for far longer than the grace, and they must ALWAYS be told "your
// keeper was slain" the next time they load (however many days later), instead of resuming their pre-invasion
// save as if nothing happened. g_conqueredBy is cleared only when the victim actually acknowledges it (via
// /slain_ack on their slain-load). call under g_mutex.
void purgeConqueredKeepers() {
  std::time_t now = std::time(nullptr);
  bool changed = false;
  for (auto it = g_conquered.begin(); it != g_conquered.end();) {
    std::time_t t = 0;
    try { t = (std::time_t) std::stoll(it->second); } catch (...) {}
    if (now - t >= CONQUERED_TTL) {
      const std::string gameId = it->first;
      for (auto ci = g_claims.begin(); ci != g_claims.end();)
        if (ci->second.gameId == gameId) ci = g_claims.erase(ci); else ++ci;
      g_dungeonHash.erase(gameId);
      g_invaded.erase(gameId);
      // NOTE: g_conqueredBy[gameId] is deliberately KEPT here so the slain owner is still notified on their
      // (later) load; it's cleared by /slain_ack once they've seen the message.
      g_keeperBiome.erase(gameId);
      if (gameId.find("..") == std::string::npos && gameId.find('/') == std::string::npos)
        removeKeeperDir(gameId);
      std::printf("[conquered] %s husk removed from the world map (grace elapsed); slain record kept\n",
          gameId.c_str()); std::fflush(stdout);
      it = g_conquered.erase(it);
      changed = true;
    } else ++it;
  }
  if (changed) {
    saveAllKeepers();
  }
}

// ---- Phase B: villains (all call under g_mutex except the loaders at startup) ----
std::string villainFile(const std::string& id) { return "rar_villains/" + id + ".dat"; }
bool villainFileExists(const std::string& id) {
  if (id.empty() || id.find("..") != std::string::npos || id.find('/') != std::string::npos)
    return false;
  std::ifstream in(villainFile(id), std::ios::binary);
  return (bool) in;
}
// Fresh random villain id (the filename). Only has to be unique within the folder.
std::string newVillainId() {
  static const char* CH = "abcdefghijklmnopqrstuvwxyz0123456789";
  static std::mt19937 rng((unsigned) std::time(nullptr) ^ (unsigned) (size_t) &CH);
  std::string id;
  do {
    id.clear();
    for (int i = 0; i < 12; ++i)
      id += CH[rng() % 36];
  } while (g_villains.count(id) || villainFileExists(id));
  return id;
}
// Manifest: id  tier  enemyId  biome  pos  alive  defeatTime   (pos empty => unplaced spare)
// THE FOLDER IS THE TRUTH: a manifest row whose .dat is gone is dropped on load.
void loadVillains() {
  g_villains.clear();
  g_villainAt.clear();
  std::ifstream in("rar_villains.txt"); std::string line;
  while (getlineCR(in, line)) {
    auto p = splitTabs(line);
    if (p.size() < 4)
      continue;
    Villain v;
    v.tier = p[1]; v.enemyId = p[2]; v.biome = p[3];
    v.pos = (p.size() > 4) ? p[4] : std::string();
    v.alive = (p.size() > 5) ? (p[5] == "1") : true;
    try { v.defeatTime = (p.size() > 6) ? (std::time_t) std::stoll(p[6]) : 0; } catch (...) { v.defeatTime = 0; }
    if (!villainFileExists(p[0])) {
      std::printf("[villain] dropping '%s' (%s %s) -- no file in rar_villains/\n",
          p[0].c_str(), v.tier.c_str(), v.enemyId.c_str());
      continue;
    }
    g_villains[p[0]] = v;
    if (!v.pos.empty())
      g_villainAt[v.pos] = p[0];
  }
  std::fflush(stdout);
}
void saveVillains() { // call under g_mutex
  std::ofstream out("rar_villains.txt", std::ios::trunc);
  for (auto& e : g_villains)
    out << e.first << "\t" << e.second.tier << "\t" << e.second.enemyId << "\t" << e.second.biome << "\t"
        << e.second.pos << "\t" << (e.second.alive ? 1 : 0) << "\t" << (long long) e.second.defeatTime << "\n";
}
void loadVillainSlots() { // candidate empty land tiles + their biome
  g_villainSlots.clear();
  std::ifstream in("rar_villain_slots.txt"); std::string line;
  while (getlineCR(in, line)) {
    auto p = splitTabs(line);
    if (p.size() >= 2) g_villainSlots[p[0]] = p[1];
  }
}
// Optional server tunables: "KEY<TAB>VALUE" per line, '#' comments, blank lines ignored. Absent file / absent
// key => the production default compiled above stands. Lets timings + policy be retuned WITHOUT a rebuild.
void loadServerConfig() {
  std::ifstream in(SERVER_CONFIG_FILE);
  if (!in)
    return; // optional by design
  std::string line;
  while (getlineCR(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    auto p = splitTabs(line);
    if (p.size() < 2)
      continue;
    try {
      if (p[0] == "SERVER_PORT")             SERVER_PORT = std::stoi(p[1]);
      else if (p[0] == "PROTECTION_TURNS")   PROTECTION_TURNS = std::stoll(p[1]);
      else if (p[0] == "CONQUERED_TTL")      CONQUERED_TTL = (std::time_t) std::stoll(p[1]);
      else if (p[0] == "VILLAIN_DEFEAT_TTL") VILLAIN_DEFEAT_TTL = (std::time_t) std::stoll(p[1]);
      else if (p[0] == "SESSION_TTL")        SESSION_TTL = (std::time_t) std::stoll(p[1]);
      else if (p[0] == "RESERVE_TTL")        RESERVE_TTL = (std::time_t) std::stoll(p[1]);
      else if (p[0] == "OWNER_RETURN_GRACE") OWNER_RETURN_GRACE = (std::time_t) std::stoll(p[1]);
      else if (p[0] == "SIEGE_PROTECT_TTL")  SIEGE_PROTECT_TTL = (std::time_t) std::stoll(p[1]);
      else if (p[0] == "POOL_REPLENISH_SECS") POOL_REPLENISH_SECS = std::stoi(p[1]);
      else if (p[0] == "ALL_DEVELOPERS")      ALL_DEVELOPERS = std::stoi(p[1]);
    } catch (...) {} // a malformed value keeps the default rather than taking down the server
  }
}

void loadVillainConfig() {
  g_villainMinAlive.clear();
  g_villainPoolTarget.clear();
  std::ifstream in("rar_villain_config.txt"); std::string line;
  while (getlineCR(in, line)) {
    auto p = splitTabs(line);
    if (p.size() < 2)
      continue;
    // "MAIN 6" = min-alive-before-respawn; "POOL_MAIN 10" = UNUSED spares the full server keeps per villain
    // type (edit this line + restart -- or just wait, the live replenish picks it up -- to deepen the pool
    // WITHOUT a world regen). Both are plain-text so the content-free server reads them too.
    if (p[0].rfind("POOL_", 0) == 0) {
      try { g_villainPoolTarget[p[0].substr(5)] = std::stoi(p[1]); } catch (...) {}
    } else {
      try { g_villainMinAlive[p[0]] = std::stoi(p[1]); } catch (...) {}
    }
  }
}
// The old split roster/state/pool manifests are gone: rar_villains.txt now carries everything (placement,
// alive, defeat time), so "save the state" is simply "write the manifest".
void saveVillainState() { saveVillains(); } // call under g_mutex (kept as an alias for existing call sites)

// A tier's presence on the MAP = alive AND placed. Unplaced spares don't count towards the minimum.
int villainAliveCount(const std::string& tier) { // call under g_mutex
  int n = 0;
  for (auto& e : g_villains)
    if (e.second.tier == tier && e.second.alive && !e.second.pos.empty()) ++n;
  return n;
}
// Free = a known land tile with no LIVING villain on it (a defeated corpse still occupies its tile until the
// grace expires, so we don't spawn a replacement on top of it). call under g_mutex.
bool tileFree(const std::string& pos) {
  auto it = g_villainAt.find(pos);
  if (it == g_villainAt.end())
    return true;
  auto v = g_villains.find(it->second);
  return v == g_villains.end() || !v->second.alive;
}
// Place a villain onto a tile (updates both the villain and the position index). call under g_mutex.
void placeVillain(const std::string& id, const std::string& pos) {
  auto& v = g_villains[id];
  if (!v.pos.empty())
    g_villainAt.erase(v.pos);
  v.pos = pos;
  if (!pos.empty())
    g_villainAt[pos] = id;
}
// A random free tile whose biome matches. Empty string if none. call under g_mutex.
std::string freeTileForBiome(const std::string& biome) {
  static std::mt19937 rng((unsigned) std::time(nullptr));
  std::vector<std::string> cand;
  for (auto& s : g_villainSlots)
    if (s.second == biome && tileFree(s.first))
      cand.push_back(s.first);
  if (cand.empty())
    return "";
  return cand[rng() % cand.size()];
}
// A tile no villain AND no keeper holds. g_villainSlots is keyed "x_y", claims are keyed "x,y".
bool tileFreeForKeeper(const std::string& slotPos) {
  if (!tileFree(slotPos))
    return false;
  auto us = slotPos.find('_');
  if (us == std::string::npos)
    return false;
  return !g_claims.count(slotPos.substr(0, us) + "," + slotPos.substr(us + 1));
}
// A random free tile of this biome, for a keeper. "" if the world has none left.
std::string freeTileForKeeperBiome(const std::string& biome) {
  static std::mt19937 rng((unsigned) std::time(nullptr) ^ 0x5eed);
  std::vector<std::string> cand;
  for (auto& s : g_villainSlots)
    if (s.second == biome && tileFreeForKeeper(s.first))
      cand.push_back(s.first);
  if (cand.empty())
    return "";
  return cand[rng() % cand.size()];
}

// A keeper whose claimed tile is no longer the biome its dungeon was built for has been MOVED under it (a world
// regen re-rolled the map). Re-place it onto a random free tile of its own biome, keeping the keeper, its blob
// and its history -- only the position changes. Same rule, and the same caution, as the villain pass: a tile
// whose biome we do NOT know is never evidence of a mismatch, so we never move on a guess.
// call under g_mutex, AFTER loadVillainSlots()/loadVillains().
void replaceMovedKeepers() {
  std::vector<std::pair<std::string, std::string>> moves;  // oldKey -> newKey
  for (auto& e : g_claims) {
    auto biome = g_keeperBiome.find(e.second.gameId);
    if (biome == g_keeperBiome.end() || biome->second.empty())
      continue;                       // biome unknown (pre-upgrade keeper) -> nothing to compare against
    auto comma = e.first.find(',');
    std::string slotPos = e.first.substr(0, comma) + "_" + e.first.substr(comma + 1);
    auto slot = g_villainSlots.find(slotPos);
    if (slot == g_villainSlots.end())
      continue;                       // tile's biome unknown to us -> no EVIDENCE of a mismatch
    if (slot->second == biome->second)
      continue;                       // still on a tile of its own biome
    auto tile = freeTileForKeeperBiome(biome->second);
    if (tile.empty()) {
      std::printf("[keeper] '%s' is on a %s tile but was built for %s -- no free %s tile to move it to\n",
          e.second.gameId.c_str(), slot->second.c_str(), biome->second.c_str(), biome->second.c_str());
      continue;
    }
    auto us = tile.find('_');
    moves.push_back({ e.first, tile.substr(0, us) + "," + tile.substr(us + 1) });
  }
  for (auto& m : moves) {
    auto claim = g_claims[m.first];
    g_claims.erase(m.first);
    g_claims[m.second] = claim;
    std::printf("[keeper] moved '%s' %s -> %s (tile biome no longer matched %s)\n",
        claim.gameId.c_str(), m.first.c_str(), m.second.c_str(), g_keeperBiome[claim.gameId].c_str());
  }
  if (!moves.empty()) {
    saveAllKeepers();
    std::fflush(stdout);
  }
}

// Remove defeated villains whose 5-min lootable grace has elapsed: delete the (aftermath) blob and drop the
// slot from the roster so clients stop drawing the defeated sprite. call under g_mutex.
void purgeDefeatedVillains() {
  std::time_t now = std::time(nullptr);
  bool changed = false;
  for (auto it = g_villains.begin(); it != g_villains.end();) {
    if (!it->second.alive && it->second.defeatTime > 0 && now - it->second.defeatTime >= VILLAIN_DEFEAT_TTL) {
      const std::string& key = it->first;
      if (villainFileExists(key))
        std::remove(villainFile(key).c_str());
      std::printf("[villain] '%s' (%s at %s) removed from the world (loot grace elapsed)\n", key.c_str(),
          it->second.enemyId.c_str(), it->second.pos.c_str()); std::fflush(stdout);
      if (!it->second.pos.empty())
        g_villainAt.erase(it->second.pos);
      it = g_villains.erase(it);
      changed = true;
    } else
      ++it;
  }
  if (changed)
    saveVillainState();
}
// Drop any roster villain whose interior blob is MISSING from rar_villains/. Such an entry still draws on
// every client's world map (reconcileVillains places exactly the roster) but 404s the moment the player tries
// to invade it -> "That site's map couldn't be loaded from the server." Orphans arise from botched world
// regens, backup/restore mismatches, or a manual .dat deletion that left the roster line behind. Pruning here
// keeps the roster and the on-disk blobs consistent; the caller's per-tier top-up then respawns a real,
// loadable replacement so the alive-count is restored (at a fresh, valid tile). Returns #removed. call under g_mutex.
// The folder is the truth, so a manifest row whose file vanished mid-run is dropped (loadVillains already
// does this at startup; this catches losses that happen later).
int pruneOrphanVillains() {
  int removed = 0;
  for (auto it = g_villains.begin(); it != g_villains.end();) {
    if (!villainFileExists(it->first)) {
      std::printf("[villain] pruned '%s' (%s %s) -- no file in rar_villains/\n",
          it->first.c_str(), it->second.tier.c_str(), it->second.enemyId.c_str()); std::fflush(stdout);
      if (!it->second.pos.empty())
        g_villainAt.erase(it->second.pos);
      it = g_villains.erase(it);
      ++removed;
    } else
      ++it;
  }
  if (removed)
    saveVillains();
  return removed;
}
// The keeper equivalent of pruneOrphanVillains(): drop claims whose dungeon blob never arrived. Such a claim
// is a ghost -- it holds a tile and draws on every player's world map, but invading it fails with "that site's
// map couldn't be loaded". A claim is created NON-permanent at base placement (/claim) and only promoted on
// save & exit (/claim_save), so a non-permanent claim with no blob is a keeper that was created and then
// abandoned without ever saving. /release normally cleans that up, but a client that crashed or was killed
// never sends it -- so it lingers forever. Prune those here, at startup.
// A PERMANENT claim with no blob is deliberately NOT pruned: that keeper really did save, so a missing blob
// means the blob was lost, and erasing the claim would quietly delete a real keeper. Warn and leave it.
// call under g_mutex.
int pruneStrayClaims() {
  int removed = 0;
  for (auto it = g_claims.begin(); it != g_claims.end();) {
    const std::string& gameId = it->second.gameId;
    bool badId = gameId.empty() || gameId.find("..") != std::string::npos || gameId.find('/') != std::string::npos;
    if (!badId) {
      std::ifstream in(dungeonPath(gameId), std::ios::binary);
      if (in) { ++it; continue; } // blob present -> a real keeper
    }
    if (it->second.perm && !badId) {
      std::printf("[claim] WARNING: keeper %s ('%s') at %s SAVED but has no blob in rar_dungeons/ -- keeping "
          "the claim, this needs a look\n", gameId.c_str(), it->second.name.c_str(), it->first.c_str());
      std::fflush(stdout);
      ++it;
      continue;
    }
    std::printf("[claim] pruned stray %s ('%s', login '%s') at %s -- never saved, no blob in rar_dungeons/\n",
        gameId.c_str(), it->second.name.c_str(), it->second.login.c_str(), it->first.c_str());
    std::fflush(stdout);
    it = g_claims.erase(it);
    ++removed;
  }
  if (removed)
    saveAllKeepers();
  return removed;
}

// A keeper is only "real" once it has an uploaded dungeon blob (written on save & exit). A savehash without a
// blob is a stray from a game that was started + hashed but never properly saved & exited -- it can't be
// downloaded, yet /keepers used to list it, so the account showed a ghost duplicate (e.g. two "Lilliana").
bool hasDungeonBlob(const std::string& gameId) {
  if (gameId.empty() || gameId.find("..") != std::string::npos || gameId.find('/') != std::string::npos)
    return false;
  std::ifstream in(dungeonPath(gameId), std::ios::binary);
  return (bool) in;
}

// Drop savehash / autosavehash / played-turn entries whose keeper has no dungeon blob (orphans). Mirrors
// pruneStrayClaims so the server's records stay consistent: a keeper exists <=> it has a blob. call under g_mutex.
int pruneStraySaveHashes() {
  int removed = 0;
  auto pruneMap = [&](std::map<std::string, std::string>& m, bool keyHasLogin) {
    for (auto it = m.begin(); it != m.end();) {
      auto tab = it->first.find('\t');
      std::string gameId = (keyHasLogin && tab != std::string::npos) ? it->first.substr(tab + 1) : it->first;
      if (hasDungeonBlob(gameId)) { ++it; continue; }
      std::printf("[keeper] pruned stray save record '%s' -- no blob in rar_dungeons/\n", it->first.c_str());
      it = m.erase(it);
      ++removed;
    }
  };
  pruneMap(g_saveHashes, true);
  pruneMap(g_autosaveHashes, true);
  pruneMap(g_playedTurns, false);
  if (removed) {
    saveAllKeepers();
  }
  std::fflush(stdout);
  return removed;
}

// Spawn tier villains on RANDOM empty land tiles (from rar_villain_slots) until the tier is back at its min.
// Each spawn uses a spare whose BIOME matches the chosen tile (grass tile -> grassland villain, etc.).
// call under g_mutex.
void respawnTier(const std::string& tier) {
  auto minIt = g_villainMinAlive.find(tier);
  if (minIt == g_villainMinAlive.end() || minIt->second <= 0)
    return;
  while (villainAliveCount(tier) < minIt->second) {
    // Villains are biome-locked (EnemyInfo::getBiome), so a spare can only be placed on a tile of ITS biome --
    // never a wrong-biome fallback. Spares are simply the unplaced villains already sitting in the folder;
    // placing one is a pure metadata change (no file copy) -- the villain keeps its own identity and interior.
    std::string chosenId, chosenTile;
    for (auto& e : g_villains) {
      if (e.second.tier != tier || !e.second.pos.empty() || !e.second.alive)
        continue; // only unplaced, living spares of this tier
      auto tile = freeTileForBiome(e.second.biome);
      if (!tile.empty()) { chosenId = e.first; chosenTile = tile; break; }
    }
    if (chosenId.empty())
      break; // no spare of this tier that we can biome-match to a free tile
    placeVillain(chosenId, chosenTile);
    std::printf("[villain] placed %s '%s' (%s) at %s [id %s]\n", tier.c_str(),
        g_villains[chosenId].enemyId.c_str(), g_villains[chosenId].biome.c_str(), chosenTile.c_str(),
        chosenId.c_str()); std::fflush(stdout);
  }
  saveVillains();
}

// A placed villain whose tile's biome no longer matches its own (e.g. after a world regen) is MOVED to a
// random free tile of the right biome -- the villain (its file, interior and history) is kept, only its
// position changes. If nothing matches it becomes an unplaced spare and the top-up will place it later.
int rebindVillainBiomes() { // call under g_mutex
  int moved = 0, unplaced = 0;
  for (auto& e : g_villains) {
    auto& v = e.second;
    if (v.pos.empty())
      continue;
    auto slot = g_villainSlots.find(v.pos);
    if (slot == g_villainSlots.end())
      continue; // tile's biome is unknown to us -> no EVIDENCE of a mismatch, so never move it on a guess
    if (slot->second == v.biome)
      continue; // still on a tile of its own biome
    std::string from = v.pos;
    placeVillain(e.first, ""); // free the tile first so it can be a candidate for others
    auto tile = freeTileForBiome(v.biome);
    if (!tile.empty()) {
      placeVillain(e.first, tile);
      std::printf("[villain] moved '%s' (%s, biome %s) %s -> %s (tile biome no longer matched)\n",
          e.first.c_str(), v.enemyId.c_str(), v.biome.c_str(), from.c_str(), tile.c_str());
      ++moved;
    } else {
      std::printf("[villain] unplaced '%s' (%s, biome %s) from %s -- no free tile of that biome\n",
          e.first.c_str(), v.enemyId.c_str(), v.biome.c_str(), from.c_str());
      ++unplaced;
    }
  }
  if (moved || unplaced) {
    std::fflush(stdout);
    saveVillains();
  }
  return moved + unplaced;
}

// Rewrite rar_villain_pool.txt from g_villainPool (the spare INVENTORY; used/unused lives in the state file).
// call under g_mutex.
void savePoolManifest() { saveVillains(); } // spares live in the villain folder now; one manifest for all

// LIVE POOL REPLENISH (full server only). For every (tier, enemyId, biome) combo the world uses, keep at
// least g_villainPoolTarget[tier] UNUSED spares on hand, generating any shortfall via g_villainGen (the
// server's own content). The expensive blob generation runs WITHOUT g_mutex held, so it never blocks request
// handling -- only the quick before/after bookkeeping is locked. No-op once every combo is at target, and a
// no-op entirely on a content-free (legacy) launch where g_villainGen is empty.
void replenishPool() {
  if (!g_villainGen || g_villainCombos.empty())
    return;
  struct Need { std::string tier, enemyId, biome; int count = 0; };
  std::vector<Need> needs;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    // Walk the WORLD's combos (not the pool's) and top each one up to its POOL_<tier> target.
    for (auto& c : g_villainCombos) {
      if (g_villainComboFailed.count(c.tier + "/" + c.enemyId + "/" + c.biome))
        continue;
      auto tIt = g_villainPoolTarget.find(c.tier);
      int target = (tIt == g_villainPoolTarget.end()) ? 0 : tIt->second;
      if (target <= 0)
        continue;
      int unused = 0; // spares = UNPLACED villains of this combo already in the folder
      for (auto& e : g_villains)
        if (e.second.pos.empty() && e.second.alive &&
            e.second.tier == c.tier && e.second.enemyId == c.enemyId && e.second.biome == c.biome)
          ++unused;
      if (unused < target)
        needs.push_back(Need{ c.tier, c.enemyId, c.biome, target - unused });
    }
  }
  if (needs.empty())
    return;
  // Generate each combo's shortfall off-lock (CPU-heavy: every spare is a full dungeon build), then COMMIT that
  // combo before moving on -- so a big first-pass shortfall persists incrementally (visible progress, and a
  // restart mid-way resumes instead of losing everything).
  int totalMade = 0;
  for (auto& n : needs) {
    std::vector<std::string> blobs;
    for (int i = 0; i < n.count; ++i) {
      std::string blob = g_villainGen(n.tier, n.enemyId, n.biome);
      if (blob.empty()) {
        // Nothing at all came out on the first try -> treat the combo as impossible (that faction can't build on
        // that biome) and stop retrying it. A LATER failure is treated as transient and retried next cycle.
        if (i == 0) {
          std::lock_guard<std::mutex> lk(g_mutex);
          g_villainComboFailed.insert(n.tier + "/" + n.enemyId + "/" + n.biome);
          std::printf("[villain] pool replenish: cannot generate %s '%s' on %s -- skipping this combo\n",
              n.tier.c_str(), n.enemyId.c_str(), n.biome.c_str()); std::fflush(stdout);
        }
        break; // commit what we have, retry the rest next cycle
      }
      blobs.push_back(std::move(blob));
    }
    if (blobs.empty())
      continue;
    std::lock_guard<std::mutex> lk(g_mutex);
    ensureDir("rar_villains");
    // New spares are just UNPLACED villains in the same folder, each with its own random id -- no separate
    // pool, and no index scheme to collide on.
    for (auto& blob : blobs) {
      std::string id = newVillainId();
      std::ofstream out(villainFile(id), std::ios::binary);
      out.write(blob.data(), blob.size()); out.close();
      g_villains[id] = Villain{ n.tier, n.enemyId, n.biome, "", true, 0 };
    }
    saveVillains();
    totalMade += blobs.size();
    std::printf("[villain] pool replenish: +%zu %s spare(s) for '%s' (%s)\n", blobs.size(), n.tier.c_str(),
        n.enemyId.c_str(), n.biome.c_str()); std::fflush(stdout);
  }
  if (totalMade)
    std::printf("[villain] pool replenish: %d new spare(s) this pass\n", totalMade), std::fflush(stdout);
}

// ---- TLS: self-signed identity + public-key pin -------------------------------------------------------
const char* CERT_FILE = "rar_cert.pem";
const char* KEY_FILE  = "rar_key.pem";

// Create a self-signed cert + key ONCE (kept across restarts, so the client's pin stays valid). This server is
// private with a known client, so a CA-signed cert buys nothing -- the client pins our PUBLIC KEY instead,
// which is a stronger identity check than any hostname/CA chain. Returns false if we couldn't produce one.
bool ensureCert() {
  { std::ifstream c(CERT_FILE, std::ios::binary), k(KEY_FILE, std::ios::binary);
    if (c && k) return true; } // already have an identity -> keep it (pin must stay stable)
  EVP_PKEY* pkey = EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", (size_t) 2048);
  if (!pkey) { std::fprintf(stderr, "RAR server: RSA keygen failed\n"); return false; }
  X509* x = X509_new();
  ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
  X509_gmtime_adj(X509_getm_notBefore(x), 0);
  X509_gmtime_adj(X509_getm_notAfter(x), 60L * 60 * 24 * 3650); // 10 years: no silent expiry mid-campaign
  X509_set_version(x, 2);
  X509_set_pubkey(x, pkey);
  X509_NAME* name = X509_get_subject_name(x);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char*) "rar", -1, -1, 0);
  X509_set_issuer_name(x, name); // self-signed
  bool ok = X509_sign(x, pkey, EVP_sha256()) > 0;
  if (ok) {
    FILE* cf = std::fopen(CERT_FILE, "wb");
    if (cf) { ok = PEM_write_X509(cf, x) > 0; std::fclose(cf); } else ok = false;
    FILE* kf = std::fopen(KEY_FILE, "wb");
    if (kf) { ok = ok && PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr) > 0; std::fclose(kf); }
    else ok = false;
  }
  X509_free(x);
  EVP_PKEY_free(pkey);
  if (!ok) std::fprintf(stderr, "RAR server: failed to write %s / %s\n", CERT_FILE, KEY_FILE);
  return ok;
}

// The pin the client must carry: base64(SHA256(DER SubjectPublicKeyInfo)) -- exactly curl's
// CURLOPT_PINNEDPUBLICKEY "sha256//..." format. Printed at startup so the admin can paste it into appconfig.
std::string computePubKeyPin() {
  FILE* cf = std::fopen(CERT_FILE, "rb");
  if (!cf) return "";
  X509* x = PEM_read_X509(cf, nullptr, nullptr, nullptr);
  std::fclose(cf);
  if (!x) return "";
  unsigned char* der = nullptr;
  int len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(x), &der); // SubjectPublicKeyInfo, what curl hashes
  std::string pin;
  if (len > 0 && der) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(der, len, md);
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_write(b64, md, SHA256_DIGEST_LENGTH);
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    if (bptr && bptr->length) pin.assign(bptr->data, bptr->length);
    BIO_free_all(b64);
    OPENSSL_free(der);
  }
  X509_free(x);
  return pin.empty() ? "" : ("sha256//" + pin);
}

// ---- Pre-shared-key knock gate (runs BEFORE the TLS handshake) ----------------------------------------
// TLS alone still answers a scanner's ClientHello with an alert, which fingerprints the port as TLS. With this
// gate the server reads a fixed-size PSK knock off the RAW socket first; anything that can't produce it (a
// scanner, telnet, a stray HTTP client) is closed with ZERO bytes sent back, so the port reveals nothing.
// The knock is HMAC-SHA256(psk, time-window) -- it is NOT the auth boundary (TLS + pinning + login are);
// it is an obscurity gate, so a 30s replay window is an acceptable trade for needing no round-trip.
const char* PSK_FILE = "rar_psk.txt";
const size_t KNOCK_LEN = 32;          // raw HMAC-SHA256
// NOT tunable: the client hardcodes the same window, so changing it here alone would silently break every
// client (they'd compute a knock the server never accepts). Keep server+client in lockstep if ever changed.
const long KNOCK_WINDOW = 30;         // seconds per window; we accept now-1, now, now+1 (clock skew)
std::string g_psk;

// Load the PSK, creating a random one on first run. Printed at startup for the admin to paste into appconfig.
bool ensurePsk() {
  { std::ifstream in(PSK_FILE);
    getlineCR(in, g_psk);
    while (!g_psk.empty() && (g_psk.back() == '\n' || g_psk.back() == '\r' || g_psk.back() == ' ')) g_psk.pop_back();
    if (!g_psk.empty()) return true; }
  unsigned char buf[32];
  if (RAND_bytes(buf, sizeof(buf)) != 1) return false;
  static const char* hex = "0123456789abcdef";
  g_psk.clear();
  for (unsigned char b : buf) { g_psk += hex[b >> 4]; g_psk += hex[b & 0xF]; }
  std::ofstream out(PSK_FILE);
  if (!out) return false;
  out << g_psk << "\n";
  return true;
}

std::string knockFor(long window) {
  std::string msg = "rar-knock:" + std::to_string(window);
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), g_psk.data(), (int) g_psk.size(),
      (const unsigned char*) msg.data(), msg.size(), mac, &len);
  return std::string((char*) mac, len);
}

// Read exactly KNOCK_LEN bytes (short timeout so a silent/half-open probe can't tie up a thread) and check it
// against the adjacent time windows in constant time.
bool knockOk(socket_t sock) { // socket_t is httplib's global alias for SOCKET
  // SO_RCVTIMEO is one of the few socket options that differs by platform: Windows takes a DWORD of ms,
  // POSIX takes a struct timeval. Same 3s intent either way.
#ifdef _WIN32
  DWORD tv = 3000; // ms
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv));
#else
  struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
  char buf[KNOCK_LEN];
  size_t got = 0;
  while (got < KNOCK_LEN) {
    int n = recv(sock, buf + got, (int) (KNOCK_LEN - got), 0);
    if (n <= 0) return false; // timeout / closed / probe sent nothing -> no knock
    got += (size_t) n;
  }
  long now = (long) (std::time(nullptr) / KNOCK_WINDOW);
  for (long w = now - 1; w <= now + 1; ++w) {
    std::string expect = knockFor(w);
    if (expect.size() == KNOCK_LEN &&
        CRYPTO_memcmp(expect.data(), buf, KNOCK_LEN) == 0)
      return true;
  }
  return false;
}

// SSLServer that only lets knocked connections reach the TLS handshake.
class KnockSSLServer : public httplib::SSLServer {
  public:
  KnockSSLServer(const char* cert, const char* key) : httplib::SSLServer(cert, key) {}
  protected:
  bool process_and_close_socket(socket_t sock) override {
    if (!knockOk(sock)) {
      // Silent drop: no response, no alert, nothing to fingerprint.
      closesocket(sock);
      return false;
    }
    return httplib::SSLServer::process_and_close_socket(sock);
  }
};

} // namespace

void runRarServer(int port, RarVillainGen gen, std::vector<RarVillainCombo> combos) {
  g_villainGen = std::move(gen); // non-empty on a "full" (content-loaded) launch -> enables live pool replenish
  g_villainCombos = std::move(combos); // what the world actually uses -> what the pool gets replenished against
  // All server state lives in a "server/" subdirectory next to the exe. chdir into it at startup so the
  // admin can just run `keeper.exe --rar_server 8080` from the normal location and it picks up the world +
  // accounts + keepers automatically (all rar_* paths below are cwd-relative).
  ensureDir("server");
  if (_chdir("server") != 0)
    std::fprintf(stderr, "RAR server: couldn't enter ./server directory\n");
  // RAR: auto-publish mods on every startup -- scan the standard top-level mods/ folder (now at ../mods
  // since we chdir'd into server/) and (re)write rar_mods.txt + rar_mods/*.dat here. So the admin just drops
  // a mod in mods/ and restarts the server; no separate --rar_gen_world step is needed to push it. Bundling
  // is byte-identical to the client's, so hash-checks line up.
  {
    int n = rarPublishMods("../mods", ".");
    std::printf("RAR server: published %d mod(s) from ../mods -> rar_mods/ + rar_mods.txt\n", n);
    std::fflush(stdout);
  }
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    loadKV(ACCOUNTS_FILE, g_accounts);   // the credential table stays global -- it is not per-keeper state
    loadAccounts();                      // ...but each account's ROLE lives in its own folder
    // Per-keeper state lives in saves/<account>/<keeper>/keeper.txt. On the first start after the upgrade the
    // old global files are still there: import them, fan them out into per-keeper folders, and retire them.
    if (!migrateFlatFiles()) {
      int n = loadKeepers();
      std::printf("RAR server: loaded %d keeper(s) from saves/<account>/<keeper>/\n", n);
      std::fflush(stdout);
    }
    loadServerConfig(); // optional rar_server_config.txt -> overrides the compiled-in tunables
    // Port precedence: an explicit `--rar_server <port>` (port > 0) always wins; a bare `--rar_server` arrives
    // here as 0 and falls back to the config's SERVER_PORT (itself defaulting to RAR_DEFAULT_PORT). Resolved
    // now, before the printf/listen below use `port`.
    if (port <= 0)
      port = SERVER_PORT;
    loadVillainSlots();   // map tiles + biomes FIRST -- loadVillains/rebind need them
    loadVillains();       // the folder is the truth: rows without a .dat are dropped here
    replaceMovedKeepers(); // keepers whose tile biome no longer matches were moved by a regen -> re-place them
    loadVillainConfig();
    loadOrGenWorld();
    // STARTUP VILLAIN TOP-UP: bring each tier up to its configured min-alive (rar_villain_config.txt) using the
    // spare pool, exactly like a death-triggered respawn -- so RAISING a tier's min and restarting the server
    // ADDS villains onto the existing world (new empty tiles, biome-matched) WITHOUT regenerating it. No-op when
    // already at/above min, or when the pool of spares for that tier is exhausted (then you must regen with a
    // bigger poolXxxVillainsGenerated). Villain COUNT is thus controlled live via the min values.
    {
      // First drop any orphaned roster villains (roster line but no interior blob) so we don't keep drawing
      // uninvadeable "couldn't be loaded" sites; the top-up below then respawns loadable replacements.
      if (int orphans = pruneOrphanVillains())
        std::printf("RAR server: pruned %d orphaned villain(s) with no interior blob\n", orphans);
      // Same treatment for KEEPER claims that never got a dungeon blob uploaded (created, never saved).
      if (int strays = pruneStrayClaims())
        std::printf("RAR server: pruned %d stray keeper claim(s) with no dungeon blob\n", strays);
      // And for orphan save/autosave-hashes: a keeper with a hash but no blob is a ghost that /keepers would
      // otherwise list (the "two Lilliana" bug) and that the client could never download.
      if (int strayHashes = pruneStraySaveHashes())
        std::printf("RAR server: pruned %d stray keeper save-record(s) with no dungeon blob\n", strayHashes);
      // A villain's interior is built for ONE biome. If the world changed under it (regen) its tile may no
      // longer match -- move it to a tile of its own biome rather than leaving a desert keep in a forest.
      if (int rebound = rebindVillainBiomes())
        std::printf("RAR server: re-placed %d villain(s) whose tile biome no longer matched\n", rebound);
      int before = 0;
      for (auto& e : g_villainMinAlive) before += villainAliveCount(e.first);
      for (auto& e : g_villainMinAlive) respawnTier(e.first);
      int after = 0;
      for (auto& e : g_villainMinAlive) after += villainAliveCount(e.first);
      if (after != before) {
        saveVillainState();
        std::printf("RAR server: startup top-up spawned %d villain(s) from the pool (tiers now at min; %d alive)\n",
            after - before, after);
        std::fflush(stdout);
      }
    }
  }
  // FULL SERVER: live pool auto-replenish. When launched with a content-backed generator (runRarServerFull),
  // keep the respawn pool topped up to its configured per-villain-type depth in the background -- so respawns
  // never exhaust the pool and deepening it is just a config edit, never a world regen. Run one pass now so the
  // pool is stocked before the first request, then re-check on a slow timer (draining only happens on the
  // occasional villain defeat). No-op on a content-free launch (g_villainGen empty).
  if (g_villainGen) {
    std::printf("RAR server: content loaded -> live villain-pool replenish enabled\n"); std::fflush(stdout);
    // Replenish on a background thread (NOT synchronously) so a big first-pass shortfall -- each spare is a full
    // dungeon build -- never blocks the server from accepting connections. Runs one pass right away, then
    // re-checks on a slow timer (the pool only drains on the occasional villain defeat).
    std::thread([]{
      for (;;) {
        replenishPool();
        std::this_thread::sleep_for(std::chrono::seconds(POOL_REPLENISH_SECS));
      }
    }).detach();
  }
  // TLS listener. Everything (credentials + save blobs) is encrypted, and -- unlike the old plain-HTTP
  // listener -- a stray telnet/scanner byte now fails the TLS handshake instead of getting an
  // "HTTP/1.1 400 Bad Request" that advertised this as a web server.
  if (!ensureCert()) {
    std::fprintf(stderr, "RAR server: no TLS identity -- refusing to start (would have to run in cleartext)\n");
    return;
  }
  if (!ensurePsk()) {
    std::fprintf(stderr, "RAR server: couldn't load/create %s -- refusing to start\n", PSK_FILE);
    return;
  }
  // SIEGE_PROTECT_TTL was readable from the config but missing here, so the one timer you could not verify
  // from the banner was the one governing re-invasion. All tunables that can be overridden are printed.
  std::printf("RAR server: tunables (%s) -- SERVER_PORT=%d PROTECTION_TURNS=%lld CONQUERED_TTL=%lds VILLAIN_DEFEAT_TTL=%lds "
              "SESSION_TTL=%lds RESERVE_TTL=%lds SIEGE_PROTECT_TTL=%lds POOL_REPLENISH_SECS=%ds ALL_DEVELOPERS=%d\n",
              SERVER_CONFIG_FILE,
              port, (long long) PROTECTION_TURNS, (long) CONQUERED_TTL, (long) VILLAIN_DEFEAT_TTL,
              (long) SESSION_TTL, (long) RESERVE_TTL, (long) SIEGE_PROTECT_TTL, POOL_REPLENISH_SECS,
              ALL_DEVELOPERS);
  std::fflush(stdout);
  if (!g_villainCombos.empty()) {
    std::map<std::string, int> perTier;
    for (auto& c : g_villainCombos)
      ++perTier[c.tier];
    std::string s;
    for (auto& e : perTier)
      s += (s.empty() ? "" : " ") + e.first + "=" + std::to_string(e.second);
    std::printf("RAR server: %zu villain combo(s) from the world -- %s (pool replenishes against these)\n",
        g_villainCombos.size(), s.c_str());
    std::fflush(stdout);
  }
  std::string pin = computePubKeyPin();
  std::printf("RAR server: TLS + PSK knock gate enabled (%s). Put BOTH of these in appconfig.txt:\n"
              "  \"server_cert_pin\"  \"%s\"\n"
              "  \"server_psk\"       \"%s\"\n", CERT_FILE, pin.c_str(), g_psk.c_str());
  std::fflush(stdout);
  KnockSSLServer svr(CERT_FILE, KEY_FILE);
  if (!svr.is_valid()) {
    std::fprintf(stderr, "RAR server: TLS cert/key rejected (%s / %s) -- not starting\n", CERT_FILE, KEY_FILE);
    return;
  }

  svr.Get("/ping", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("ok", "text/plain");
  });

  // The shared world config: seed + villain counts + name, one value per line.
  svr.Get("/world", [](const httplib::Request&, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(g_mutex);
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%d\n%d\n%d\n%d\n%d\n%d\n%s\n",
        g_world.seed, g_world.mainV, g_world.lesserV, g_world.minorV,
        g_world.allies, g_world.retiredV, g_world.name.c_str());
    res.set_content(buf, "text/plain");
  });

  // The authoritative world blob (terrain + villains), generated by `keeper.exe --rar_gen_world
  // rar_campaign.dat`. Served verbatim; the client deserializes it and does NO local generation.
  svr.Get("/world_data", [](const httplib::Request&, httplib::Response& res) {
    std::ifstream in("rar_campaign.dat", std::ios::binary);
    if (!in) {
      res.status = 404;
      res.set_content("no world -- run: keeper.exe --rar_gen_world rar_campaign.dat", "text/plain");
      return;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_content(bytes, "application/octet-stream");
  });

  // SHA-256 of the world blob, so a client can skip re-downloading ~0.5MB of terrain it already has. The
  // world only changes on a regen, whereas villains change constantly -- but those come from /villain_roster
  // (tiny) and OVERWRITE the blob's dwellers on reconcile, so the blob is effectively static scenery.
  // Hashed per-request (like /world_data) so a regen without a restart is picked up.
  svr.Get("/world_hash", [](const httplib::Request&, httplib::Response& res) {
    std::ifstream in("rar_campaign.dat", std::ios::binary);
    if (!in) { res.status = 404; res.set_content("no world", "text/plain"); return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_content(rarSha256Hex(bytes), "text/plain");
  });

  // Crash reports. body: login\npassword\nfilename\n<lzma blob>. Stored verbatim under rar_crashes/ for the
  // admin to decompress + inspect (a .dmp opens in a debugger; the paired .txt is a symbolized stack).
  // AUTHENTICATED: anonymous uploads would let anyone who can reach the port fill the disk. Nothing is lost by
  // requiring a login -- the client keeps crash files until the server confirms them, so a crash that happens
  // before/without a login simply ships at the next successful login.
  svr.Post("/crash", [](const httplib::Request& req, httplib::Response& res) {
    const std::string& b = req.body;
    size_t n1 = b.find('\n');
    size_t n2 = (n1 == std::string::npos) ? n1 : b.find('\n', n1 + 1);
    size_t n3 = (n2 == std::string::npos) ? n2 : b.find('\n', n2 + 1);
    if (n3 == std::string::npos) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::string login = b.substr(0, n1);
    std::string pw = b.substr(n1 + 1, n2 - n1 - 1);
    std::string name = b.substr(n2 + 1, n3 - n2 - 1);
    std::string blob = b.substr(n3 + 1);
    // Never let a client-supplied name escape the folder or overwrite anything outside it.
    if (name.empty() || name.find("..") != std::string::npos || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
      res.status = 400; res.set_content("bad name", "text/plain"); return;
    }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(login, pw)) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (login.find("..") != std::string::npos || login.find('/') != std::string::npos ||
        login.find('\\') != std::string::npos) { // authOk passed, but never trust it as a path component
      res.status = 400; res.set_content("bad login", "text/plain"); return;
    }
    ensureDir("rar_crashes");
    std::string path = "rar_crashes/" + login + "_" + name + ".lzma";
    std::ofstream out(path, std::ios::binary);
    out.write(blob.data(), blob.size());
    out.close();
    std::printf("[crash] '%s' uploaded %s (%zu bytes compressed)\n", login.c_str(), name.c_str(), blob.size());
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // Mod manifest (step 7): "modname\thash" lines, written by --rar_gen_world. Empty if vanilla.
  svr.Get("/mods", [](const httplib::Request&, httplib::Response& res) {
    std::ifstream in("rar_mods.txt");
    if (!in) { res.set_content("", "text/plain"); return; }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_content(body, "text/plain");
  });

  // A single mod bundle (all its files serialized), for client auto-download/install.
  svr.Get(R"(/mod/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string name = req.matches[1];
    if (name.find("..") != std::string::npos || name.find('/') != std::string::npos) {
      res.status = 400; res.set_content("bad name", "text/plain"); return; // path-traversal guard
    }
    std::ifstream in("rar_mods/" + name + ".dat", std::ios::binary);
    if (!in) { res.status = 404; res.set_content("no such mod", "text/plain"); return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_content(bytes, "application/octet-stream");
  });

  svr.Post("/register", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 2 || p[0].empty()) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_accounts.count(p[0])) { res.status = 409; res.set_content("exists", "text/plain"); return; }
    g_accounts[p[0]] = pwHash(p[1]);
    saveKV(ACCOUNTS_FILE, g_accounts);
    std::printf("[register] '%s' from %s\n", p[0].c_str(), req.remote_addr.c_str());
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // Set an account's role. body: login\npassword\ntargetLogin\nrole
  // Only a DEVELOPER may grant or revoke a role, so this can't be used to self-promote. The very first
  // developer is made by hand: write "role<TAB>developer" into saves/<account>/account.txt and restart.
  svr.Post("/set_role", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (roleOf(p[0]) != "developer") { res.status = 403; res.set_content("forbidden", "text/plain"); return; }
    if (!g_accounts.count(p[2])) { res.status = 404; res.set_content("no such account", "text/plain"); return; }
    if (p[3] != "developer" && p[3] != "user") { res.status = 400; res.set_content("bad role", "text/plain"); return; }
    g_accountRole[p[2]] = p[3];
    saveAccount(p[2]);
    std::printf("[set_role] '%s' set '%s' -> %s\n", p[0].c_str(), p[2].c_str(), p[3].c_str());
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  svr.Post("/login", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\nsessionToken
    if (p.size() < 2) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("fail", "text/plain"); return; }
    std::string token = p.size() > 2 ? p[2] : "";
    if (sessionHeldByOther(p[0], token)) {
      std::printf("[login] '%s' REJECTED (already logged in elsewhere)\n", p[0].c_str()); std::fflush(stdout);
      res.status = 409; res.set_content("already logged in", "text/plain"); return;
    }
    g_sessions[p[0]] = SessionInfo{ token, std::time(nullptr) }; // acquire the single-session lock
    std::printf("[login] '%s' from %s (%s)\n", p[0].c_str(), req.remote_addr.c_str(), roleOf(p[0]).c_str());
    std::fflush(stdout);
    // "ok<newline><role>". An older client reads only the first token, so this stays backward compatible.
    res.set_content("ok\n" + roleOf(p[0]), "text/plain");
  });

  // Client "I'm still here" ping (every ~60s). Refreshes the session so it doesn't go stale. Only the
  // session owner (matching token) or a free/stale slot may refresh -- a ping can't steal an active session.
  svr.Post("/heartbeat", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\nsessionToken
    if (p.size() < 2) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    std::string token = p.size() > 2 ? p[2] : "";
    auto it = g_sessions.find(p[0]);
    if (it == g_sessions.end() || std::time(nullptr) - it->second.lastActivity > SESSION_TTL
        || it->second.token == token)
      g_sessions[p[0]] = SessionInfo{ token, std::time(nullptr) };
    res.set_content("ok", "text/plain");
  });

  // Explicit logout (graceful app exit): release the session immediately so the account can log in again
  // right away on this or another computer. Only the owner (matching token) may release.
  svr.Post("/logout", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\nsessionToken
    if (p.size() < 2) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    std::string token = p.size() > 2 ? p[2] : "";
    auto it = g_sessions.find(p[0]);
    if (it != g_sessions.end() && it->second.token == token) {
      g_sessions.erase(it);
      std::printf("[logout] '%s'\n", p[0].c_str()); std::fflush(stdout);
    }
    res.set_content("ok", "text/plain");
  });

  svr.Post("/savehash", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    g_saveHashes[p[0] + "\t" + p[2]] = p[3];
    saveAllKeepers();
    std::printf("[savehash] '%s'/'%s' = %s\n", p[0].c_str(), p[2].c_str(), p[3].c_str());
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  svr.Get(R"(/savehash/([^/]+)/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_saveHashes.find(req.matches[1].str() + "\t" + req.matches[2].str());
    if (it == g_saveHashes.end()) { res.status = 404; res.set_content("none", "text/plain"); return; }
    res.set_content(it->second, "text/plain");
  });

  // Hash of the player's last AUTOSAVE (.aut). Uploaded on every autosave; the .aut state itself is NOT
  // uploaded (that would push megabytes every few minutes). Its only job is to let the owner prove, on the
  // next load after a crash, that the .aut on his disk is genuinely the file he last wrote -- at which point
  // the client pushes it up as the dungeon blob. body: login\npassword\nkeeper\nhash
  svr.Post("/autosavehash", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    g_autosaveHashes[p[0] + "\t" + p[2]] = p[3];
    saveAllKeepers();
    std::printf("[autosavehash] '%s'/'%s' = %s\n", p[0].c_str(), p[2].c_str(), p[3].c_str());
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  svr.Get(R"(/autosavehash/([^/]+)/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_autosaveHashes.find(req.matches[1].str() + "\t" + req.matches[2].str());
    if (it == g_autosaveHashes.end()) { res.status = 404; res.set_content("none", "text/plain"); return; }
    res.set_content(it->second, "text/plain");
  });

  // Temporarily claim a start site (game started, not yet saved).
  // body: login\npassword\ngameId\nkeeperName\nx\ny
  svr.Post("/claim", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 6) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    std::string key = p[4] + "," + p[5];
    auto it = g_claims.find(key);
    if (it != g_claims.end() && it->second.login != p[0]) {
      res.status = 409; res.set_content("taken", "text/plain"); return; // held by another player
    }
    // A keeper (identity "<account>~<name>") owns exactly ONE site. If this id already claims a DIFFERENT
    // position, the account already has a keeper with this name -- reject rather than let it take a second
    // site and later overwrite the first one's save slot. The client warns at name entry; this is the backstop.
    // (A keeper whose claim was lost has no entry here, so claim-recovery re-placement still works.)
    for (auto& e : g_claims)
      if (e.first != key && e.second.gameId == p[2]) {
        std::printf("[claim] REJECTED '%s': keeper '%s' already holds site %s\n",
            p[0].c_str(), p[2].c_str(), e.first.c_str());
        std::fflush(stdout);
        res.status = 409; res.set_content("keeper exists", "text/plain"); return;
      }
    bool wasPerm = (it != g_claims.end()) && it->second.perm;
    g_claims[key] = Claim{ p[0], p[2], p[3], wasPerm }; // keep perm if re-claiming own permanent site
    // Remember the BIOME this keeper settled on. That is what "the biome its dungeon was built for" means, and
    // it is what a later world regen is checked against -- a keeper found on a different biome is re-placed.
    {
      auto slot = g_villainSlots.find(p[4] + "_" + p[5]);
      if (slot != g_villainSlots.end())
        g_keeperBiome[p[2]] = slot->second;
    }
    saveAllKeepers();
    std::printf("[claim] '%s' (%s) site %s%s\n", p[0].c_str(), p[3].c_str(), key.c_str(), wasPerm ? " [perm]" : "");
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // Promote this account's claim for a keeper to permanent (on save & exit).
  // body: login\npassword\ngameId
  svr.Post("/claim_save", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    bool found = false;
    for (auto& e : g_claims)
      if (e.second.login == p[0] && e.second.gameId == p[2]) { e.second.perm = true; found = true; }
    if (found) { saveAllKeepers(); std::printf("[claim_save] '%s' keeper %s -> permanent\n", p[0].c_str(), p[2].c_str()); std::fflush(stdout); }
    res.set_content("ok", "text/plain");
  });

  // Release a temporary claim (game abandoned without saving). Permanent claims are kept.
  // body: login\npassword\ngameId
  svr.Post("/release", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    bool changed = false;
    for (auto it = g_claims.begin(); it != g_claims.end();) {
      if (it->second.login == p[0] && it->second.gameId == p[2] && !it->second.perm) {
        it = g_claims.erase(it); changed = true;
      } else ++it;
    }
    if (changed) { saveAllKeepers(); std::printf("[release] '%s' temp claim for %s\n", p[0].c_str(), p[2].c_str()); std::fflush(stdout); }
    res.set_content("ok", "text/plain");
  });

  // Force-delete an account's claim for a keeper, PERMANENT included (keeper erased/abandoned).
  // Only the owning login can delete its own claim. body: login\npassword\ngameId
  svr.Post("/delete_claim", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    bool changed = false;
    for (auto it = g_claims.begin(); it != g_claims.end();) {
      if (it->second.login == p[0] && it->second.gameId == p[2]) {
        it = g_claims.erase(it); changed = true;
      } else ++it;
    }
    if (changed) { saveAllKeepers(); std::printf("[delete_claim] '%s' erased claim for %s\n", p[0].c_str(), p[2].c_str()); std::fflush(stdout); }
    // FORGET the keeper completely. Deleting only the folder was not enough: the hashes/turns/flags stayed in
    // memory, so allKeeperIds() still listed this gameId and the next saveAllKeepers() RECREATED the folder
    // with a fresh keeper.txt -- a deleted keeper came back (blob-less, but present).
    {
      auto sep = p[2].find('~');
      std::string hashKey = (sep == std::string::npos ? p[2] : p[2].substr(0, sep)) + "\t" + p[2];
      g_saveHashes.erase(hashKey);
      g_autosaveHashes.erase(hashKey);
      g_dungeonHash.erase(p[2]);
      g_playedTurns.erase(p[2]);
      g_conquered.erase(p[2]);
      g_conqueredBy.erase(p[2]);
      g_keeperBiome.erase(p[2]);
      g_invaded.erase(p[2]);
      g_reservations.erase(p[2]);
      g_awayKeepers.erase(p[2]);
    }
    if (p[2].find("..") == std::string::npos && p[2].find('/') == std::string::npos)
      removeKeeperDir(p[2]);   // the whole folder: blob, keeper.txt, directory
    res.set_content("ok", "text/plain");
  });

  // Reserve a dungeon for invasion (at most one invader at a time). body: login\npassword\ngameId
  // 200 "ok" if granted (free, already yours, or expired); 409 "reserved" if another invader holds it.
  svr.Post("/reserve_dungeon", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    purgeExpiredReservations();
    purgeConqueredKeepers();
    if (g_conquered.count(p[2])) {
      res.status = 409; res.set_content("conquered", "text/plain"); return; // leader already slain, not invadeable
    }
    if (isNewbieProtected(p[2])) {
      res.status = 403; res.set_content("protected", "text/plain"); return; // still under newbie protection
    }
    { auto sp = g_siegeProtected.find(p[2]); // owner recently came back mid-siege -> nobody re-invades for a while
      if (sp != g_siegeProtected.end()) {
        if (std::time(nullptr) < sp->second) {
          res.status = 403; res.set_content("protected", "text/plain"); return;
        }
        g_siegeProtected.erase(sp);
      } }
    auto it = g_reservations.find(p[2]);
    if (it != g_reservations.end() && it->second.login != p[0]) {
      res.status = 409; res.set_content("reserved", "text/plain"); return; // held by another invader
    }
    g_evicted.erase(p[2]);   // a new, properly granted invasion may write back again
    g_reservations[p[2]] = Reservation{ p[0], std::time(nullptr) + RESERVE_TTL };
    std::printf("[reserve] '%s' locked %s\n", p[0].c_str(), p[2].c_str()); std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // Release an invasion reservation (invasion ended). body: login\npassword\ngameId. Idempotent.
  svr.Post("/release_dungeon", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    auto it = g_reservations.find(p[2]);
    if (it != g_reservations.end() && it->second.login == p[0]) {
      g_reservations.erase(it);
      std::printf("[release] '%s' freed %s\n", p[0].c_str(), p[2].c_str()); std::fflush(stdout);
    }
    // Siege over: the owner polls for the reservation to disappear, and this only happens AFTER the invader's
    // rarUploadDungeon (same detached thread, upload first) -- so "released" already means the aftermath is on
    // the server and the owner is safe to download it.
    g_ownerReturn.erase(p[2]);
    res.set_content("ok", "text/plain");
  });

  // OWNER is trying to get back into his own dungeon. body: login\npassword\ngameId.
  // -> "clear"            : nobody inside, go ahead and load
  // -> "siege\n<seconds>" : an invader is inside; he's on a countdown, poll until this returns clear.
  // First knock starts the invader's eviction countdown AND stamps the anti-lockout protection immediately,
  // so a second attacker can't reserve the moment the first one is evicted.
  svr.Post("/owner_returning", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    purgeExpiredReservations();
    const std::string& gameId = p[2];
    std::time_t now = std::time(nullptr);
    if (!g_reservations.count(gameId)) { // no invader (or he just left) -> nothing blocking the owner
      g_ownerReturn.erase(gameId);
      res.set_content("clear", "text/plain");
      return;
    }
    auto it = g_ownerReturn.find(gameId);
    if (it == g_ownerReturn.end()) {
      g_ownerReturn[gameId] = now + OWNER_RETURN_GRACE;
      g_siegeProtected[gameId] = now + SIEGE_PROTECT_TTL; // block NEW invaders from here on
      std::printf("[siege] owner '%s' returning to %s -- invader has %llds, protected for %llds\n",
          p[0].c_str(), gameId.c_str(), (long long) OWNER_RETURN_GRACE, (long long) SIEGE_PROTECT_TTL);
      std::fflush(stdout);
      it = g_ownerReturn.find(gameId);
    }
    long long left = (long long) (it->second - now);
    if (left <= 0) {
      // GRACE ELAPSED -> actually evict. The countdown promises "the invader is forced out in Ns", but
      // nothing enforced it: the reservation was only ever dropped when the invader RELEASED it. An
      // invader who closed the game (or crashed) never releases, so the owner sat on "forced out in 0s"
      // until RESERVE_TTL (20 min) expired -- locked out of his own dungeon by a player who was gone.
      // Drop the reservation ourselves and remember the eviction, so a late writeback from that
      // invader cannot overwrite the dungeon the owner is by then already playing.
      g_reservations.erase(gameId);
      g_ownerReturn.erase(gameId);
      g_evicted[gameId] = now;
      std::printf("[siege] grace elapsed -- evicting invader from %s; owner may enter\n", gameId.c_str());
      std::fflush(stdout);
      res.set_content("clear", "text/plain");
      return;
    }
    res.set_content("siege\n" + std::to_string(left), "text/plain");
  });

  // INVADER polls this while inside someone's dungeon. body: login\npassword\ngameId.
  // -> "ok"                  : carry on
  // -> "ownerback\n<seconds>": the owner is waiting; at 0 the client force-exits control (team walks home,
  //                            which triggers the normal writeback+upload+release path).
  svr.Post("/invasion_status", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    auto it = g_ownerReturn.find(p[2]);
    if (it == g_ownerReturn.end()) { res.set_content("ok", "text/plain"); return; }
    long long left = (long long) (it->second - std::time(nullptr));
    if (left < 0) left = 0;
    res.set_content("ownerback\n" + std::to_string(left), "text/plain");
  });

  // ---- RAR live PvP brokering (real-time tick traffic is separate, on the lockstep relay port) -----------
  // The invader announces a live invasion of a target. If the target's owner is ONLINE we open a session with a
  // shared seed and return it; otherwise "OFFLINE" and the invader falls back to the async (retired-blob) path.
  // body: login\npassword\ntargetGameId\ninvaderName  ->  "LIVE\n<sessionId>\n<seed>"  or  "OFFLINE"
  svr.Post("/pvp_invite", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    purgeExpiredPvp();
    const std::string& targetGameId = p[2];
    std::string owner = claimOwnerOf(targetGameId);
    if (owner.empty() || owner == p[0] || !isOnline(owner)) { res.set_content("OFFLINE", "text/plain"); return; }
    purgeExpiredAway();
    // He is out raiding somebody else -- his base is unattended, so it can't be invaded until he returns.
    if (g_awayKeepers.count(targetGameId)) { res.set_content("AWAY", "text/plain"); return; }
    for (auto& e : g_pvpSessions) // reuse an existing pending invite from this invader for this target
      if (e.second.targetGameId == targetGameId && e.second.invaderLogin == p[0]) {
        res.set_content("LIVE\n" + e.first + "\n" + std::to_string(e.second.seed), "text/plain");
        return;
      }
    std::string sid = newPvpSessionId();
    int seed = (int) (std::rand() ^ ((int) std::time(nullptr) << 1) ^ ((int) g_pvpSessions.size() * 2654435761u));
    if (seed == 0) seed = 1;
    g_pvpSessions[sid] = PvpSession{ targetGameId, p[0], p[3], seed, std::time(nullptr), false, false, "", "" };
    std::printf("[pvp] invite '%s' -> '%s' (owner '%s' online) session=%s seed=%d\n",
        p[0].c_str(), targetGameId.c_str(), owner.c_str(), sid.c_str(), seed); std::fflush(stdout);
    res.set_content("LIVE\n" + sid + "\n" + std::to_string(seed), "text/plain");
  });

  // Mark/unmark a keeper as AWAY (out invading). Only its owner may set it.
  // body: login \n password \n gameId \n 0|1
  svr.Post("/pvp_away", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (claimOwnerOf(p[2]) != p[0]) { res.status = 403; res.set_content("not owner", "text/plain"); return; }
    if (p[3] == "1") g_awayKeepers[p[2]] = std::time(nullptr);
    else g_awayKeepers.erase(p[2]);
    res.set_content("ok", "text/plain");
  });

  // The defender polls for a pending live invite against their OWN gameId (only the claim owner is answered).
  // body: login\npassword\nmyGameId  ->  "<sessionId>\n<invaderName>\n<seed>"  or  "NONE"
  svr.Post("/pvp_poll", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    purgeExpiredPvp();
    if (claimOwnerOf(p[2]) != p[0]) { res.set_content("NONE", "text/plain"); return; }
    for (auto& e : g_pvpSessions)
      if (e.second.targetGameId == p[2]) {
        res.set_content(e.first + "\n" + e.second.invaderName + "\n" + std::to_string(e.second.seed), "text/plain");
        return;
      }
    res.set_content("NONE", "text/plain");
  });

  // Each side uploads its packed start-state for a session; the OTHER side downloads it (GET below). Both then
  // load BOTH blobs and construct the same combined battlefield, so their lockstep sims start bit-identical.
  // role 0 = defender's base, role 1 = invader's keeper. Header lines then the raw blob.
  // body: login\npassword\nsessionId\nrole\n<blob bytes>
  svr.Post("/pvp_state", [](const httplib::Request& req, httplib::Response& res) {
    const std::string& b = req.body;
    size_t n1 = b.find('\n');
    size_t n2 = (n1 == std::string::npos) ? n1 : b.find('\n', n1 + 1);
    size_t n3 = (n2 == std::string::npos) ? n2 : b.find('\n', n2 + 1);
    size_t n4 = (n3 == std::string::npos) ? n3 : b.find('\n', n3 + 1);
    if (n4 == std::string::npos) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::string login = b.substr(0, n1);
    std::string pw = b.substr(n1 + 1, n2 - n1 - 1);
    std::string sid = b.substr(n2 + 1, n3 - n2 - 1);
    std::string role = b.substr(n3 + 1, n4 - n3 - 1);
    std::string blob = b.substr(n4 + 1);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(login, pw)) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    auto it = g_pvpSessions.find(sid);
    if (it == g_pvpSessions.end()) { res.status = 404; res.set_content("no session", "text/plain"); return; }
    if (role == "0") {
      if (claimOwnerOf(it->second.targetGameId) != login) { res.status = 403; res.set_content("not owner", "text/plain"); return; }
      it->second.defenderBlob = std::move(blob);
      it->second.defenderJoined = true;
    } else if (role == "1") {
      if (it->second.invaderLogin != login) { res.status = 403; res.set_content("not invader", "text/plain"); return; }
      it->second.invaderBlob = std::move(blob);
      it->second.invaderJoined = true;
    } else { res.status = 400; res.set_content("bad role", "text/plain"); return; }
    std::printf("[pvp] state role %s uploaded for session %s (%zu bytes)\n", role.c_str(), sid.c_str(), blob.size());
    std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // Download a side's packed start-state (404 until uploaded). role 0 = defender's base, role 1 = invader's.
  svr.Get(R"(/pvp_state/([0-9a-f]+)/([01]))", [](const httplib::Request& req, httplib::Response& res) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_pvpSessions.find(req.matches[1].str());
    if (it == g_pvpSessions.end()) { res.status = 404; res.set_content("none", "text/plain"); return; }
    const std::string& blob = (req.matches[2].str() == "0") ? it->second.defenderBlob : it->second.invaderBlob;
    if (blob.empty()) { res.status = 404; res.set_content("none", "text/plain"); return; }
    res.set_content(blob, "application/octet-stream");
  });

  // Either side signals it has joined / left; returns "BOTH" once both are in, else "WAIT". role: 0=def 1=inv.
  // A leave (role prefixed with '-') drops the session. body: login\npassword\nsessionId\nrole
  svr.Post("/pvp_ready", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    auto it = g_pvpSessions.find(p[2]);
    if (it == g_pvpSessions.end()) { res.set_content("GONE", "text/plain"); return; }
    if (!p[3].empty() && p[3][0] == '-') { g_pvpSessions.erase(it); res.set_content("GONE", "text/plain"); return; }
    if (p[3] == "0") it->second.defenderJoined = true;
    else if (p[3] == "1") it->second.invaderJoined = true;
    res.set_content((it->second.defenderJoined && it->second.invaderJoined) ? "BOTH" : "WAIT", "text/plain");
  });

  // The invader slew this keeper's leader. Mark the base conquered: it stays on the world map for
  // CONQUERED_TTL (30 min) then is purged entirely. body: login\npassword\ngameId. Idempotent.
  svr.Post("/conquered", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\ntargetGameId\nslayerNameFallback\nslayerGameId(optional)
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (!g_conquered.count(p[2])) { // keep the first conquest time (don't extend the grace on re-report)
      // Prefer the slayer's KEEPER/base name from their claim (the world-map label, e.g. "world three");
      // fall back to the name the client sent (its account login) if the slayer has no claim.
      std::string slayerName = (p.size() >= 5) ? claimNameOf(p[4]) : std::string();
      if (slayerName.empty()) slayerName = p[3];
      g_conquered[p[2]] = std::to_string((long long) std::time(nullptr));
      g_conqueredBy[p[2]] = slayerName;
      saveAllKeepers();
      std::printf("[conquered] '%s' (keeper '%s') slew the keeper of %s (grace until removal)\n",
          p[0].c_str(), slayerName.c_str(), p[2].c_str()); std::fflush(stdout);
    }
    res.set_content("ok", "text/plain");
  });

  // Report a keeper's ACTUAL play-turn count (sent on every save). Drives newbie invasion protection:
  // until it reaches PROTECTION_TURNS the keeper can't be invaded. body: login\npassword\ngameId\nturns.
  svr.Post("/played_turns", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (p[2].empty() || p[2].find("..") != std::string::npos || p[2].find('/') != std::string::npos) {
      res.status = 400; res.set_content("bad gameId", "text/plain"); return;
    }
    g_playedTurns[p[2]] = p[3];
    saveAllKeepers();
    res.set_content("ok", "text/plain");
  });

  // The name of whoever slew this keeper's leader, or "" if the keeper is not conquered. The owner
  // checks this on Load: non-empty => refuse to load, show "Your keeper was slain by <name>".
  svr.Get(R"(/conquered_by/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string gameId = req.matches[1];
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_conqueredBy.find(gameId);
    res.set_content(it == g_conqueredBy.end() ? "" : it->second, "text/plain");
  });

  // The slain owner has been shown "your keeper was slain" and erased their local save -> clear the (kept)
  // slain record so it doesn't linger. body: login\npassword\ngameId. Only the owner may ack their own.
  svr.Post("/slain_ack", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body);
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (g_conqueredBy.erase(p[2]) > 0) {
      g_conquered.erase(p[2]); // in case still within grace
      saveAllKeepers();
      std::printf("[conquered] '%s' acknowledged the slaying of %s -> record cleared\n",
          p[0].c_str(), p[2].c_str()); std::fflush(stdout);
    }
    res.set_content("ok", "text/plain");
  });

  // Upload a keeper's BASE-ONLY dungeon snapshot (for async invasions while offline).
  // body: login\npassword\ngameId\n<binary blob>. The blob may contain anything after the 3rd '\n'.
  svr.Post("/dungeon", [](const httplib::Request& req, httplib::Response& res) {
    const std::string& b = req.body;
    size_t n1 = b.find('\n');
    size_t n2 = (n1 == std::string::npos) ? n1 : b.find('\n', n1 + 1);
    size_t n3 = (n2 == std::string::npos) ? n2 : b.find('\n', n2 + 1);
    size_t n4 = (n3 == std::string::npos) ? n3 : b.find('\n', n3 + 1);
    if (n4 == std::string::npos) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::string login = b.substr(0, n1);
    std::string pw = b.substr(n1 + 1, n2 - n1 - 1);
    std::string gameId = b.substr(n2 + 1, n3 - n2 - 1);
    std::string rawHash = b.substr(n3 + 1, n4 - n3 - 1); // hash of the RAW game bytes (owner/invader)
    std::string blob = b.substr(n4 + 1);
    { std::lock_guard<std::mutex> lk(g_mutex);
      if (!authOk(login, pw)) { res.status = 401; res.set_content("auth fail", "text/plain"); return; } }
    if (gameId.empty() || gameId.find("..") != std::string::npos || gameId.find('/') != std::string::npos) {
      res.status = 400; res.set_content("bad gameId", "text/plain"); return;
    }
    { std::lock_guard<std::mutex> lk(g_mutex);
      // Refuse a writeback from an invader we already evicted: the owner was let into this dungeon when
      // the siege grace elapsed, so this blob describes a state that has since been superseded.
      auto ev = g_evicted.find(gameId);
      if (ev != g_evicted.end() && claimOwnerOf(gameId) != login) {
        std::printf("[siege] REFUSED stale writeback for %s from '%s' (evicted)\n",
            gameId.c_str(), login.c_str());
        std::fflush(stdout);
        res.status = 409; res.set_content("evicted", "text/plain"); return;
      } }
    ensureDungeonDir(gameId);
    std::ofstream out(dungeonPath(gameId), std::ios::binary);
    out.write(blob.data(), blob.size());
    out.close();
    { std::lock_guard<std::mutex> lk(g_mutex);
      // Record the per-dungeon raw hash so the owner can detect on load that the server copy differs
      // from their local one (invaded or tampered -> load the server copy).
      g_dungeonHash[gameId] = rawHash;
      g_evicted.erase(gameId);   // the owner's own save supersedes the eviction; the slate is clean again
      saveAllKeepers();
      std::string owner = claimOwnerOf(gameId);
      bool byOwner = (owner.empty() || owner == login); // unclaimed => treat uploader as owner
      if (byOwner) g_invaded.erase(gameId); else g_invaded.insert(gameId);
      saveAllKeepers();
      std::printf("[dungeon] '%s' uploaded %s (%zu bytes, hash %.8s)%s\n", login.c_str(), gameId.c_str(),
          blob.size(), rawHash.c_str(), byOwner ? "" : " [INVADED writeback]"); std::fflush(stdout);
    }
    res.set_content("ok", "text/plain");
  });

  // Anticheat/invasion: the server's stored raw hash for this dungeon ("" if none).
  svr.Get(R"(/dungeon_hash/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string gameId = req.matches[1];
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_dungeonHash.find(gameId);
    res.set_content(it == g_dungeonHash.end() ? "" : it->second, "text/plain");
  });

  // (legacy) has this dungeon been invaded since its owner last saved? "1"/"0". Kept for diagnostics.
  svr.Get(R"(/dungeon_invaded/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string gameId = req.matches[1];
    std::lock_guard<std::mutex> lk(g_mutex);
    res.set_content(g_invaded.count(gameId) ? "1" : "0", "text/plain");
  });

  // Download a keeper's base-only dungeon blob (invader side).
  svr.Get(R"(/dungeon/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string name = req.matches[1];
    if (name.find("..") != std::string::npos || name.find('/') != std::string::npos) {
      res.status = 400; res.set_content("bad name", "text/plain"); return;
    }
    std::ifstream in(dungeonPath(name), std::ios::binary);
    if (!in) { res.status = 404; res.set_content("no dungeon", "text/plain"); return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_content(bytes, "application/octet-stream");
  });

  // Phase A: download a pre-generated villain map (keyed by "x_y"), so clients never generate villains.
  // The blobs are produced by `--rar_gen_world` into rar_villains/. Read per-request (regen w/o restart).
  svr.Get(R"(/villain/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string key = req.matches[1]; // the client addresses villains by MAP POSITION "x_y"
    if (key.find("..") != std::string::npos || key.find('/') != std::string::npos) {
      res.status = 400; res.set_content("bad key", "text/plain"); return;
    }
    std::string id; // resolve position -> villain id (identity is the file, not the tile)
    { std::lock_guard<std::mutex> lk(g_mutex);
      auto at = g_villainAt.find(key);
      if (at != g_villainAt.end()) id = at->second; }
    if (id.empty()) { res.status = 404; res.set_content("no villain", "text/plain"); return; }
    std::ifstream in(villainFile(id), std::ios::binary);
    if (!in) { res.status = 404; res.set_content("no villain", "text/plain"); return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.set_content(bytes, "application/octet-stream");
  });

  // Phase B: a client reports it defeated the villain at "x_y" (killed its leader). Mark it dead, drop its
  // active blob, and respawn the tier from the spare pool if it fell below the configured minimum.
  svr.Post("/villain_defeated", [](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\nx_y
    if (p.size() < 3) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    auto at = g_villainAt.find(p[2]); // client reports the TILE; resolve it to the villain sitting there
    auto it = (at == g_villainAt.end()) ? g_villains.end() : g_villains.find(at->second);
    if (it != g_villains.end() && it->second.alive) {
      it->second.alive = false;
      it->second.defeatTime = std::time(nullptr); // start the 5-min lootable grace; KEEP the blob so it's
                                                  // revisitable (the invader may re-upload the aftermath).
      std::string tier = it->second.tier;
      std::printf("[villain] '%s' defeated %s (%s), alive now %d (5-min loot grace)\n", p[0].c_str(),
          p[2].c_str(), tier.c_str(), villainAliveCount(tier)); std::fflush(stdout);
      respawnTier(tier); // replacement spawns elsewhere immediately; the corpse lingers for looting
      saveVillainState();
    }
    res.set_content("ok", "text/plain");
  });

  // Upload the post-battle "aftermath" of a defeated villain (dead defenders, dropped loot, damage) so the
  // grace-period revisit shows the real outcome. body: login\npassword\nx_y\n<blob>. Only overwrites a
  // villain that is currently defeated-in-grace; also (re)starts the grace clock if it wasn't set.
  svr.Post("/villain_writeback", [](const httplib::Request& req, httplib::Response& res) {
    const std::string& b = req.body;
    size_t n1 = b.find('\n');
    size_t n2 = (n1 == std::string::npos) ? n1 : b.find('\n', n1 + 1);
    size_t n3 = (n2 == std::string::npos) ? n2 : b.find('\n', n2 + 1);
    if (n3 == std::string::npos) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::string login = b.substr(0, n1);
    std::string pw = b.substr(n1 + 1, n2 - n1 - 1);
    std::string key = b.substr(n2 + 1, n3 - n2 - 1);
    std::string blob = b.substr(n3 + 1);
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(login, pw)) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (key.empty() || key.find("..") != std::string::npos || key.find('/') != std::string::npos) {
      res.status = 400; res.set_content("bad key", "text/plain"); return;
    }
    auto at = g_villainAt.find(key); // aftermath is posted for a TILE -> resolve to the villain there
    auto it = (at == g_villainAt.end()) ? g_villains.end() : g_villains.find(at->second);
    if (it == g_villains.end() || it->second.alive) { // only defeated-in-grace villains accept an aftermath
      res.status = 409; res.set_content("not defeated", "text/plain"); return;
    }
    ensureDir("rar_villains");
    std::ofstream out(villainFile(it->first), std::ios::binary);
    out.write(blob.data(), blob.size()); out.close();
    if (it->second.defeatTime == 0) it->second.defeatTime = std::time(nullptr);
    saveVillainState();
    std::printf("[villain] '%s' uploaded aftermath for %s (%zu bytes)\n", login.c_str(), key.c_str(),
        blob.size()); std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // Phase B: the currently-DEAD (defeated, still in loot grace) villain positions ("x_y" per line).
  svr.Get("/villain_state", [](const httplib::Request&, httplib::Response& res) {
    std::string out;
    std::lock_guard<std::mutex> lk(g_mutex);
    purgeDefeatedVillains(); // drop corpses whose grace elapsed before reporting
    for (auto& e : g_villains) // report TILES (the client keys its world map by position), not villain ids
      if (!e.second.alive && !e.second.pos.empty())
        out += e.second.pos + "\n";
    res.set_content(out, "text/plain");
  });

  // The CURRENT villain roster: "x_y  TIER  enemyId  alive(1|0)" per line. Includes both live villains
  // (originals + respawns) AND defeated-in-grace corpses (alive=0). The client reconciles its world-map
  // villains to exactly this: live ones are invadeable, alive=0 ones render the defeated sprite and stay
  // revisitable (to loot) until the server purges them after the grace period.
  svr.Get("/villain_roster", [](const httplib::Request&, httplib::Response& res) {
    std::string out;
    std::lock_guard<std::mutex> lk(g_mutex);
    purgeDefeatedVillains(); // remove corpses whose loot grace elapsed before reporting the roster
    // Only PLACED villains are on the map; unplaced spares are inventory the client never sees. Reported by
    // TILE so the client's world map keys stay positional.
    for (auto& e : g_villains)
      if (!e.second.pos.empty())
        out += e.second.pos + "\t" + e.second.tier + "\t" + e.second.enemyId + "\t" +
               (e.second.alive ? "1" : "0") + "\n";
    res.set_content(out, "text/plain");
  });

  // ---- Testing utility: server-side export/import of a keeper's base between accounts/keepers ----
  auto badName = [](const std::string& s) {
    return s.empty() || s.find("..") != std::string::npos || s.find('/') != std::string::npos;
  };
  // EXPORT: copy a keeper's current server blob + hash into a named library entry (rar_exports/).
  svr.Post("/export_keeper", [badName](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\ngameId\nexportName
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (badName(p[2]) || badName(p[3])) { res.status = 400; res.set_content("bad name", "text/plain"); return; }
    std::ifstream in(dungeonPath(p[2]), std::ios::binary);
    if (!in) { res.status = 404; res.set_content("no such keeper blob", "text/plain"); return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    ensureDir("rar_exports");
    std::ofstream out("rar_exports/" + p[3] + ".dat", std::ios::binary);
    out.write(bytes.data(), bytes.size()); out.close();
    auto hit = g_dungeonHash.find(p[2]);
    std::ofstream hout("rar_exports/" + p[3] + ".hash");
    hout << (hit == g_dungeonHash.end() ? std::string() : hit->second); hout.close();
    std::printf("[export] '%s' exported keeper %s -> '%s' (%zu bytes)\n", p[0].c_str(), p[2].c_str(),
        p[3].c_str(), bytes.size()); std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });
  // IMPORT: overwrite a target keeper's server blob with a library entry + set a hash that differs from the
  // target's LOCAL save, so on Load its owner downloads the imported base. Only the target's owner may import.
  svr.Post("/import_keeper", [badName](const httplib::Request& req, httplib::Response& res) {
    auto p = splitLines(req.body); // login\npassword\nexportName\ntargetGameId
    if (p.size() < 4) { res.status = 400; res.set_content("bad request", "text/plain"); return; }
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!authOk(p[0], p[1])) { res.status = 401; res.set_content("auth fail", "text/plain"); return; }
    if (badName(p[2]) || badName(p[3])) { res.status = 400; res.set_content("bad name", "text/plain"); return; }
    std::string owner = claimOwnerOf(p[3]);
    if (!owner.empty() && owner != p[0]) { res.status = 403; res.set_content("not your keeper", "text/plain"); return; }
    std::ifstream in("rar_exports/" + p[2] + ".dat", std::ios::binary);
    if (!in) { res.status = 404; res.set_content("no such export", "text/plain"); return; }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); in.close();
    ensureDungeonDir(p[3]);
    std::ofstream out(dungeonPath(p[3]), std::ios::binary);
    out.write(bytes.data(), bytes.size()); out.close();
    std::ifstream hin("rar_exports/" + p[2] + ".hash");
    std::string hash((std::istreambuf_iterator<char>(hin)), std::istreambuf_iterator<char>()); hin.close();
    g_dungeonHash[p[3]] = hash;                       // != the target's local hash -> owner downloads on Load
    g_invaded.erase(p[3]); g_conquered.erase(p[3]); g_conqueredBy.erase(p[3]); // fresh: not invaded/conquered
    saveAllKeepers();
    std::printf("[import] '%s' imported '%s' -> keeper %s (%zu bytes)\n", p[0].c_str(), p[2].c_str(),
        p[3].c_str(), bytes.size()); std::fflush(stdout);
    res.set_content("ok", "text/plain");
  });

  // All claimed sites, tab-separated: "x\ty\tlogin\tgameId\tperm\tname\tconquered(1|0)" per line. A conquered
  // base (leader slain, still in its removal grace) is flagged so the client draws the defeated sprite on it.
  svr.Get("/claims", [](const httplib::Request&, httplib::Response& res) {
    std::string out;
    std::lock_guard<std::mutex> lk(g_mutex);
    purgeConqueredKeepers();  // remove bases whose grace elapsed before reporting the world map
    purgeDefeatedVillains();  // and villain corpses whose loot grace elapsed
    for (auto& e : g_claims) {
      auto comma = e.first.find(',');
      bool conquered = g_conquered.count(e.second.gameId) > 0;
      bool protectedNewbie = isNewbieProtected(e.second.gameId);
      out += e.first.substr(0, comma) + "\t" + e.first.substr(comma + 1) + "\t" +
             e.second.login + "\t" + e.second.gameId + "\t" + (e.second.perm ? "1" : "0") + "\t" +
             e.second.name + "\t" + (conquered ? "1" : "0") + "\t" + (protectedNewbie ? "1" : "0") + "\n";
    }
    res.set_content(out, "text/plain");
  });

  svr.Get(R"(/keepers/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
    const std::string login = req.matches[1].str();
    std::string out;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& e : g_saveHashes) {
      auto tab = e.first.find('\t');
      if (tab != std::string::npos && e.first.substr(0, tab) == login) {
        auto gameId = e.first.substr(tab + 1);
        if (hasDungeonBlob(gameId)) // only real, downloadable keepers -- never a hash-only orphan
          out += gameId + "\n";
      }
    }
    res.set_content(out, "text/plain");
  });

  std::printf("KeeperRL RAR server listening on 0.0.0.0:%d  (accounts=%zu, keepers=%zu)\n",
      port, g_accounts.size(), g_saveHashes.size());
  std::fflush(stdout);
  if (!svr.listen("0.0.0.0", port))
    std::fprintf(stderr, "RAR server: failed to bind port %d\n", port);
}
