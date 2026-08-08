#pragma once
// Client-side hooks for the RAR online backend (talks to `keeper.exe --rar_server`
// over HTTP via libcurl). Kept free of game headers so it's cheap to include.
#include <string>
#include <vector>
#include <utility>
#include <set>

// Configure the session. serverUrl empty => online disabled entirely. certPin is the server's PINNED public
// key ("sha256//<base64>", printed by the server at startup); REQUIRED for https:// -- the client refuses to
// talk over unpinned TLS, since the server is self-signed and unpinned TLS is trivially MITM'd.
// psk = the pre-shared key for the pre-TLS knock (printed by the server at startup); also REQUIRED for
// https:// -- the server silently drops any connection that doesn't knock first.
void rarInit(const std::string& serverUrl, const std::string& login, const std::string& password,
    const std::string& certPin = "", const std::string& psk = "", const std::string& serverListUrl = "");

// Fetch the public list of servers ("host:port" per line, '#' comments). Plain TLS WITHOUT cert verification
// (no CA bundle to ship/maintain) and without our server's pin/knock. Safe only because the list supplies an
// ADDRESS and nothing else: the cert pin from appconfig still gates the server we actually talk to, so a
// spoofed list can deny service but not hijack. See httpExternal() -- putting per-server pins in the list
// would break that reasoning and require real verification. false if unreachable/empty -> fall back to
// the appconfig server_url.
bool rarFetchServerList(std::vector<std::string>& out);
// Point the client at a server chosen from that list (cert pin + PSK still come from appconfig).
void rarSetServerUrl(const std::string& url);
// Upload one already-compressed crash artifact. AUTHENTICATED -- fails while logged out, so callers must run
// it after login (the file is kept and retried, so nothing is lost by waiting).
bool rarUploadCrash(const std::string& name, const std::string& blob);
// True if a server_url is set (online available, but the player may not be logged in yet).
bool rarConfigured();
// Set/replace the logged-in credentials (called after the in-game login prompt).
void rarSetCredentials(const std::string& login, const std::string& password);
// True if online AND a login is set (i.e. logged in -> save-hash hooks are active).
bool rarEnabled();
const std::string& rarSessionLogin();
// Compose the online keeper identity "<account>~<keeper>" (deterministic, path-safe). See rar_client.cpp.
std::string rarComposeGameId(const std::string& account, const std::string& keeper);

// Account + save-hash calls (return true on HTTP 200). On failure, rarLastError() explains.
bool rarServerReachable();          // GET /ping -> true if the server is up
bool rarRegister();                 // register the session login/password
// Result of a login attempt (single-session enforced by the server).
enum class RarLoginResult { Ok, BadCredentials, AlreadyLoggedIn, Unreachable };
// Verify credentials AND acquire the single-session lock; on Ok, starts the heartbeat that keeps it alive.
RarLoginResult rarLoginCheck();
// True if the logged-in account is a DEVELOPER. Content marked developerOnly (a boss keeper, the dev build
// tools) is hidden from everyone else -- the mods themselves stay active for all players, since that content
// still has to exist in the world. Fails CLOSED: no login, old server or no role => false.
bool rarIsDeveloper();
// Release the session (graceful exit) + stop heartbeating, so the account can log in again immediately.
void rarLogout();
bool rarUploadSaveHash(const std::string& keeperId, const std::string& hashHex);
// Returns the server's stored hash for this keeper, or empty string if none/unreachable.
std::string rarGetSaveHash(const std::string& keeperId);
// Same, for the last AUTOSAVE (.aut). Separate field on purpose -- see rar_client.cpp.
bool rarUploadAutosaveHash(const std::string& keeperId, const std::string& hashHex);
std::string rarGetAutosaveHash(const std::string& keeperId);
const std::string& rarLastError();

// Hash a file's bytes with the shared FNV-1a (matches the server). "" if unreadable.
std::string rarHashFile(const std::string& path);

// Per-PC registry mapping a save's game-identifier to the account that owns it, so the
// load menu shows only the logged-in account's keepers -- not other accounts' saves on
// the same PC, and not pre-existing original .kep files (online = no import of old saves).
void rarSetSaveRegistry(const std::string& filePath);
void rarRecordSaveOwnership(const std::string& gameId); // tag gameId as owned by current login
bool rarOwnsSave(const std::string& gameId);            // true if current login owns gameId (LOCAL hint only)
// The SERVER's authoritative list of the logged-in account's keepers. Callers must FAIL CLOSED on false --
// never fall back to the local registry, or deleted keepers reappear and re-upload themselves.
bool rarListKeepers(std::set<std::string>& out);

// The shared world config from the server (same for every player => same world map).
struct RarWorld {
  bool valid = false;
  int seed = 0;
  int mainVillains = 0, lesserVillains = 0, minorVillains = 0, allies = 0, retiredVillains = 0;
  std::string worldName;
};
RarWorld rarGetWorld();

// Fetch the authoritative world blob (terrain + villains) as raw bytes. false if unavailable.
// The game side deserializes it into a Campaign; the client stays free of game types.
bool rarFetchWorldData(std::string& out);
// SHA-256 of that blob, so callers can cache it locally and skip re-downloading ~0.5MB of scenery that only
// changes on a world regen. (Live villain state does NOT come from the blob -- see rarGetVillainRoster.)
bool rarWorldHash(std::string& out);

// Step 7 mod sync: fetch the server's mod manifest (modName -> contentHash) and individual
// mod bundles. The game side compares hashes, installs mismatches, and sets the active-mod list.
bool rarFetchModManifest(std::vector<std::pair<std::string, std::string>>& out);
bool rarFetchModBundle(const std::string& name, std::string& out);

// Temporarily claim the chosen start site (game started, not yet saved). Shows the
// keeper name to other players and blocks the site. true = claimed; false = taken/error.
bool rarClaimSite(const std::string& gameId, const std::string& keeperName, int x, int y);
// Promote this keeper's claim to permanent (call on save & exit) -- held even offline.
void rarClaimSave(const std::string& gameId);
// Release the active temporary claim if the game was abandoned without saving.
void rarReleaseActiveTempClaim();
// Force-delete this account's claim for a keeper (permanent included) when the keeper is
// erased from the menu or abandoned in-game, so the tile frees up for other players.
void rarDeleteClaim(const std::string& gameId);
// Upload this keeper's full stripped game state as the server blob, with a per-dungeon hash of the raw
// game bytes (owner or invader may update it; the owner compares it to their local copy on load).
bool rarUploadDungeon(const std::string& gameId, const std::string& blob, const std::string& hashHex);
// The server's stored hash of the current dungeon blob's raw bytes ("" if none). Anticheat + invasion:
// if this differs from the owner's local copy hash, the server copy is authoritative.
std::string rarDungeonHash(const std::string& gameId);
// Reserve a dungeon for invasion (one invader at a time).
enum class RarReserveResult { GRANTED, PROTECTED, DENIED }; // PROTECTED=newbie protection; DENIED=busy/error
RarReserveResult rarReserveDungeon(const std::string& gameId);
// Release an invasion reservation when the invasion ends (best-effort, idempotent).
void rarReleaseDungeon(const std::string& gameId);

// SIEGE: an offline player's dungeon is being invaded and the OWNER comes back.
// Owner side: knock before loading. UnderSiege => an invader is inside and is now on a countdown; keep polling
// until Clear, which the server only reports once the invader RELEASED -- and he releases only after his
// aftermath upload finishes, so Clear also means "the aftermath is on the server, safe to download".
// The first knock also starts the invader's eviction timer and stamps 30-min anti-lockout protection.
// Unreachable => let the owner in rather than lock him out of his own dungeon over a network blip.
enum class RarSiegeResult { Clear, UnderSiege, Unreachable };
RarSiegeResult rarOwnerReturning(const std::string& gameId, long long& secondsLeft);

// Invader side: poll while inside. OwnerReturning => warn, and at secondsLeft==0 force-exit control (which
// walks the team home = invasion over). Unknown => transient failure; do NOT evict on it.
enum class RarInvasionStatus { Ok, OwnerReturning, Unknown };
RarInvasionStatus rarInvasionStatus(const std::string& gameId, long long& secondsLeft);

// RAR live PvP brokering (real-time tick traffic uses the separate lockstep relay). The server pairs an invader
// with an ONLINE defender, hands both a shared seed, and relays the defender's packed start-state blob so both
// lockstep sims start bit-identical. See rar_server.cpp /pvp_* routes.
std::string rarServerHost(); // bare host of the RAR server; the lockstep relay lives here on its own port
enum class RarPvpReady { Wait, Both, Gone };
// Live-invite result: Live = battle brokered; Away = the target is out raiding (undefended, not invadable);
// Offline = not reachable live -> fall back to the async invasion.
enum class RarPvpInvite { Live, Away, Offline };
RarPvpInvite rarPvpInvite(const std::string& targetGameId, const std::string& invaderName, std::string& sessionId, int& seed);
// Mark THIS keeper as away raiding (true when leaving on an invasion, false once home) so nobody can invade an
// unattended base. Best-effort; the server also expires a stale flag.
bool rarPvpSetAway(const std::string& gameId, bool away);
bool rarPvpPoll(const std::string& myGameId, std::string& sessionId, std::string& invaderName, int& seed);
bool rarPvpUploadState(const std::string& sessionId, int role, const std::string& blob);
bool rarPvpFetchState(const std::string& sessionId, int role, std::string& out);
RarPvpReady rarPvpReady(const std::string& sessionId, int role, bool leave);
// Defender-side background watch (no network on the game-loop thread).
void rarStartPvpWatch(const std::string& myGameId);
void rarStopPvpWatch();
bool rarPvpPendingInvite(std::string& sessionId, std::string& invaderName, int& seed);
bool rarPvpInvitePending(); // peek only -- lets a turn-based (control mode) loop notice an incoming invasion
// Report that this keeper's leader was slain during an invasion (slayerName = the invader). The server
// keeps the base on the world map for a grace period then removes it. Best-effort, idempotent.
void rarMarkConquered(const std::string& gameId, const std::string& fallbackName, const std::string& slayerGameId);
// Name of whoever slew this keeper's leader, or "" if not conquered. The owner checks this on Load ->
// non-empty means refuse to load and show "Your keeper was slain by <name>".
std::string rarConqueredBy(const std::string& gameId);
// Owner acknowledges being slain (message shown, local save erased) -> server clears the slain record.
void rarAckSlain(const std::string& gameId);
// 4c: true if this keeper's server dungeon was damaged by an invader since the owner last saved.
bool rarDungeonInvaded(const std::string& gameId);
// Download another player's cached dungeon blob so it can be loaded + invaded. false if none.
bool rarFetchDungeon(const std::string& gameId, std::string& out);
// Phase A: download a pre-generated villain map (key = "x_y") to load on demand when travelling there.
bool rarFetchVillain(const std::string& key, std::string& out);
// Phase B: report that the villain at "x_y" was defeated (leader killed) -> server marks it dead + respawns.
void rarMarkVillainDefeated(const std::string& key);
// Upload the post-battle aftermath of a defeated villain (revisitable to loot during the grace period).
bool rarVillainWriteback(const std::string& key, const std::string& blob);
// Phase B: fetch the currently-dead villain positions ("x_y" per line) so the client hides them from the map.
bool rarFetchVillainState(std::string& out);
// World-map sync: refresh the dead-villain overlay from the server (call when opening the world map), then
// query it while rendering/selecting sites so the shared map reflects deaths + respawns live.
void rarRefreshVillainState();
bool rarIsVillainDead(int x, int y);

struct RarClaim { int x = 0; int y = 0; std::string login; std::string gameId; std::string name;
    bool permanent = false; bool conquered = false; // conquered = leader slain, in removal grace
    bool protectedNewbie = false; };                 // still under newbie invasion protection (not invadeable)
// Report a keeper's actual play-turn count (call on save) -> drives server-side newbie invasion protection.
void rarReportPlayedTurns(const std::string& gameId, long long turns);
std::vector<RarClaim> rarGetClaims(); // all claimed sites (for showing others' territory)

// World-map sync: the server's CURRENT villain roster. Live villains (originals + respawns) plus
// defeated-in-grace corpses flagged defeated=true (render the defeated sprite, still revisitable to loot).
struct RarVillain { int x = 0; int y = 0; std::string tier; std::string enemyId; bool defeated = false; };
std::vector<RarVillain> rarGetVillainRoster();

// Standalone round-trip self-test (register/login/upload/get) for the current session,
// printing results. Returns 0 on success. Used by the --rar_client_test flag.
int rarClientSelfTest();
// Drives the SIEGE state machine (reserve -> owner knocks -> countdown -> evict -> release -> clear)
// against a live server, since reproducing it for real needs two players and a 60s window.
int rarSiegeSelfTest(const std::string& gameId);
