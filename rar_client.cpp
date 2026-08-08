#include "rar_client.h"
#include "rar_hash.h"

#include <curl/curl.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#ifdef _WIN32
#include <winsock2.h>
#else
// POSIX equivalents for the port-knock socket callback (uses closesocket). Windows branch unchanged.
#include <sys/socket.h>
#include <unistd.h>
#define closesocket(s) ::close(s)
#endif
#include <fstream>
#include <mutex>
#include <map>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <thread>
#include <chrono>
#include <random>
#include <set>

namespace {

std::string g_serverUrl;   // e.g. "https://192.168.0.10:38552" (no trailing slash)
std::string g_certPin;     // "sha256//<base64>" -- the server's PINNED public key (appconfig server_cert_pin)
std::string g_psk;         // pre-shared key for the pre-TLS knock (appconfig server_psk)
std::string g_serverListUrl; // appconfig server_list_url: a public list of "host:port" servers to pick from
std::string g_login;
std::string g_password;
std::string g_lastError;
std::mutex g_mutex;        // libcurl easy handles are not shared; serialize our calls
std::string g_registryPath;                    // per-PC save-ownership file
std::map<std::string, std::string> g_saveOwners; // gameId -> login
std::string g_activeTempClaim;                 // gameId of a temp claim not yet saved
// Single-session state. g_sessionToken identifies THIS process (so same-computer re-login is allowed,
// a different computer is rejected). The session login/password are captured on a successful login and
// used by the heartbeat thread -- kept separate from g_login/g_password, which rarLoginFlow clears on retry.
std::string g_sessionToken;
// RAR data protection: this client's data_free hash, set once at startup by main.cpp (which knows the data
// paths) and sent with every login so the server can tell whether our rule files match its own.
std::string g_dataFreeHash;

std::string g_sessionLogin;
std::string g_sessionPassword;
std::atomic<bool> g_sessionActive{false};
std::atomic<bool> g_heartbeatStarted{false};
// Live-PvP defender watch: a background thread polls /pvp_poll for the active keeper's gameId so the game loop
// never blocks on the network. g_pvpWatchGameId is the keeper to poll for ("" = don't poll); a pending invite
// is stashed here and consumed by rarPvpPendingInvite.
std::atomic<bool> g_pvpWatchStarted{false};
std::mutex g_pvpMutex;
std::string g_pvpWatchGameId;
std::string g_pvpInviteSession, g_pvpInviteName;
int g_pvpInviteSeed = 0;
bool g_pvpInvitePending = false;
// World-map sync overlay: positions ("x_y") of villains the server currently marks DEAD (defeated, not yet
// respawned). Refreshed from /villain_state whenever the world map is opened; read by the map renderer +
// travel selectability so the shared map reflects deaths/respawns live. Main-thread access only.
std::set<std::string> g_deadVillains;

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  ((std::string*) userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// ---- pre-TLS PSK knock (mirrors knockFor/knockOk in rar_server.cpp) ------------------------------------
// The server drops any connection that doesn't open with this knock, WITHOUT replying -- so scanners can't
// fingerprint the port. It must go out after connect() but before the TLS handshake, and libcurl gives us no
// hook there (SOCKOPTFUNCTION runs before connect, PREREQFUNCTION after TLS). So we open+connect the socket
// ourselves here, send the knock, and hand curl a ready socket (sockoptKnockCb reports ALREADY_CONNECTED).
std::string knockFor(long window) {
  std::string msg = "rar-knock:" + std::to_string(window);
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  HMAC(EVP_sha256(), g_psk.data(), (int) g_psk.size(),
      (const unsigned char*) msg.data(), msg.size(), mac, &len);
  return std::string((char*) mac, len);
}

curl_socket_t openSocketKnockCb(void*, curlsocktype, struct curl_sockaddr* addr) {
  curl_socket_t s = ::socket(addr->family, addr->socktype, addr->protocol);
  if (s == CURL_SOCKET_BAD)
    return CURL_SOCKET_BAD;
  if (::connect(s, &addr->addr, addr->addrlen) != 0) {
    closesocket(s);
    return CURL_SOCKET_BAD;
  }
  std::string k = knockFor((long) (std::time(nullptr) / 30));
  size_t sent = 0;
  while (sent < k.size()) {
    int n = ::send(s, k.data() + sent, (int) (k.size() - sent), 0);
    if (n <= 0) { closesocket(s); return CURL_SOCKET_BAD; }
    sent += (size_t) n;
  }
  return s;
}

int sockoptKnockCb(void*, curl_socket_t, curlsocktype) {
  return CURL_SOCKOPT_ALREADY_CONNECTED; // we already connected it in openSocketKnockCb
}

// Performs an HTTP request. postBody==nullptr => GET. Returns true if the transfer
// completed; httpCode holds the status. Caller holds g_mutex.
bool http(const std::string& url, const std::string* postBody, std::string& out, long& httpCode) {
  CURL* curl = curl_easy_init();
  if (!curl) { g_lastError = "curl init failed"; return false; }
  out.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L); // dungeon uploads are ~1MB; allow LAN latency
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  if (url.rfind("https://", 0) == 0) {
    // The server presents a SELF-SIGNED identity, so a CA chain / hostname check is meaningless here -- the
    // PINNED PUBLIC KEY is the trust anchor instead, and it's a stronger check than either (it must be exactly
    // our server's key, so a MITM with any other cert -- even a valid CA-signed one -- is rejected).
    // Refuse to talk without a pin: unpinned self-signed TLS is trivially MITM'd, which defeats the point.
    if (g_certPin.empty()) {
      g_lastError = "server_cert_pin missing in appconfig.txt -- refusing unpinned TLS (MITM risk)";
      curl_easy_cleanup(curl);
      return false;
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, g_certPin.c_str());
    // The server won't answer a byte without the knock, so a missing PSK would surface as a mystery timeout.
    // Fail loudly with the actual reason instead.
    if (g_psk.empty()) {
      g_lastError = "server_psk missing in appconfig.txt -- the server drops unknocked connections";
      curl_easy_cleanup(curl);
      return false;
    }
    curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, openSocketKnockCb);
    curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, sockoptKnockCb);
  }
  struct curl_slist* headers = nullptr;
  if (postBody) {
    // Force octet-stream: curl's default POSTFIELDS Content-Type is x-www-form-urlencoded, which
    // httplib caps at a tiny size (-> 413 on our multi-hundred-KB dungeon uploads).
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long) postBody->size());
  }
  CURLcode res = curl_easy_perform(curl);
  httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_easy_cleanup(curl);
  if (headers) curl_slist_free_all(headers);
  if (res != CURLE_OK) {
    g_lastError = std::string("network: ") + curl_easy_strerror(res);
    return false;
  }
  return true;
}

// path like "/login"; returns true on HTTP 200.
bool post(const std::string& path, const std::string& body, std::string& out) {
  std::lock_guard<std::mutex> lk(g_mutex);
  long code = 0;
  if (!http(g_serverUrl + path, &body, out, code)) return false;
  if (code != 200) { g_lastError = "server returned " + std::to_string(code) + ": " + out; return false; }
  return true;
}

bool get(const std::string& path, std::string& out, long& code) {
  std::lock_guard<std::mutex> lk(g_mutex);
  return http(g_serverUrl + path, nullptr, out, code);
}

// Percent-encode ONE URL path component. curl does not encode the URL you hand it, so a name with a space --
// e.g. the mod "Fallen Angel" or an account name with a space -- produces an invalid request line and the
// download fails. Encode per component (the caller adds the '/' separators); httplib on the server decodes the
// path before routing, so "Fallen%20Angel" round-trips back to "Fallen Angel".
std::string urlEncode(const std::string& s) {
  char* e = curl_easy_escape(nullptr, s.c_str(), (int) s.size());
  if (!e) return s;
  std::string out(e);
  curl_free(e);
  return out;
}

// Like post() but exposes the HTTP status. Returns true if the transfer completed (any status).
bool postCode(const std::string& path, const std::string& body, std::string& out, long& code) {
  std::lock_guard<std::mutex> lk(g_mutex);
  return http(g_serverUrl + path, &body, out, code);
}

// Launch (once per process) the background thread that pings /heartbeat every ~60s while a session is
// active, so the server knows this computer is still logged in and doesn't free the single-session lock.
void startHeartbeat() {
  if (g_heartbeatStarted.exchange(true))
    return; // already running
  std::thread([] {
    while (true) {
      for (int i = 0; i < 60; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!g_sessionActive)
        continue;
      std::string login = g_sessionLogin, pw = g_sessionPassword, token = g_sessionToken;
      if (login.empty())
        continue;
      std::string out;
      post("/heartbeat", login + "\n" + pw + "\n" + token, out); // best-effort
    }
  }).detach();
}

} // namespace

void rarInit(const std::string& serverUrl, const std::string& login, const std::string& password,
    const std::string& certPin, const std::string& psk, const std::string& serverListUrl) {
  g_serverUrl = serverUrl;
  while (!g_serverUrl.empty() && g_serverUrl.back() == '/') g_serverUrl.pop_back();
  g_certPin = certPin;
  g_psk = psk;
  g_serverListUrl = serverListUrl;
  g_login = login;
  g_password = password;
  // A random per-process token identifies THIS running client for single-session enforcement.
  std::random_device rd;
  static const char* hex = "0123456789abcdef";
  std::string tok;
  for (int i = 0; i < 32; ++i)
    tok += hex[rd() & 0xF];
  g_sessionToken = tok;
}

bool rarConfigured() { return !g_serverUrl.empty(); }
void rarSetDataFreeHash(const std::string& h) { g_dataFreeHash = h; }

void rarSetCredentials(const std::string& login, const std::string& password) {
  g_login = login;
  g_password = password;
}
bool rarEnabled() { return !g_serverUrl.empty() && !g_login.empty(); }
const std::string& rarSessionLogin() { return g_login; }

// The bare host of the RAR server (no scheme, no port, no path) -- the lockstep relay runs on this same host
// on its own port (RAR_LOCKSTEP_DEFAULT_PORT). Used to point the live-PvP relay client at the box.
std::string rarServerHost() {
  std::string u = g_serverUrl;
  auto p = u.find("://");
  if (p != std::string::npos) u = u.substr(p + 3);
  auto slash = u.find('/'); if (slash != std::string::npos) u = u.substr(0, slash);
  auto colon = u.find(':'); if (colon != std::string::npos) u = u.substr(0, colon);
  return u;
}

// Build the online keeper identity "<account>~<keeper>". Deterministic per (account, keeper name), so a
// re-save overwrites the same slot instead of minting a new keeper. Strips '~' '/' '\' and ".." from each part
// so the id is a safe saves/<account>/<keeper> path on both client and server.
std::string rarComposeGameId(const std::string& account, const std::string& keeper) {
  auto clean = [](const std::string& s) {
    std::string out;
    for (char c : s)
      if (c != '~' && c != '/' && c != '\\')
        out += c;
    size_t p;
    while ((p = out.find("..")) != std::string::npos)
      out.erase(p, 1);
    return out;
  };
  return clean(account) + "~" + clean(keeper);
}
const std::string& rarLastError() { return g_lastError; }

bool rarServerReachable() {
  std::string out; long code = 0;
  return get("/ping", out, code) && code == 200;
}

bool rarRegister() {
  std::string out;
  return post("/register", g_login + "\n" + g_password, out);
}

static bool g_isDeveloper = false;   // set from the /login reply; cleared on logout

bool rarIsDeveloper() {
  return g_isDeveloper;
}

RarLoginResult rarLoginCheck() {
  std::string out; long code = 0;
  g_isDeveloper = false;              // fail closed until the server says otherwise
  if (!postCode("/login", g_login + "\n" + g_password + "\n" + g_sessionToken + "\n" + g_dataFreeHash, out, code))
    return RarLoginResult::Unreachable; // network error / server down
  if (code == 200) {
    // reply is "ok" (older server) or "ok<newline><role>"
    auto nl = out.find('\n');
    if (nl != std::string::npos) {
      auto role = out.substr(nl + 1);
      while (!role.empty() && (role.back() == '\r' || role.back() == '\n'))
        role.pop_back();
      g_isDeveloper = (role == "developer");
    }
    g_sessionLogin = g_login;         // remember what to heartbeat (survives rarLoginFlow clearing g_login)
    g_sessionPassword = g_password;
    g_sessionActive = true;
    startHeartbeat();
    return RarLoginResult::Ok;
  }
  if (code == 409)
    return RarLoginResult::AlreadyLoggedIn; // active session on another computer
  return RarLoginResult::BadCredentials;    // 401 wrong user/pass
}

void rarLogout() {
  g_sessionActive = false;
  g_isDeveloper = false;
  if (g_sessionLogin.empty())
    return;
  std::string out;
  post("/logout", g_sessionLogin + "\n" + g_sessionPassword + "\n" + g_sessionToken, out); // best-effort
  g_sessionLogin.clear();
  g_sessionPassword.clear();
}

bool rarUploadSaveHash(const std::string& keeperId, const std::string& hashHex) {
  std::string out;
  return post("/savehash", g_login + "\n" + g_password + "\n" + keeperId + "\n" + hashHex, out);
}

std::string rarGetSaveHash(const std::string& keeperId) {
  std::string out;
  long code = 0;
  if (get("/savehash/" + urlEncode(g_login) + "/" + urlEncode(keeperId), out, code) && code == 200)
    return out;
  return "";
}

// The AUTOSAVE hash lives in its own server field, separate from the clean-save hash: a .kep means "saved and
// already uploaded as the dungeon blob", a .aut means "crashed, state NOT on the server yet". One field for
// both would make those two indistinguishable on load.
bool rarUploadAutosaveHash(const std::string& keeperId, const std::string& hashHex) {
  std::string out;
  return post("/autosavehash", g_login + "\n" + g_password + "\n" + keeperId + "\n" + hashHex, out);
}

std::string rarGetAutosaveHash(const std::string& keeperId) {
  std::string out;
  long code = 0;
  if (get("/autosavehash/" + urlEncode(g_login) + "/" + urlEncode(keeperId), out, code) && code == 200)
    return out;
  return "";
}

RarWorld rarGetWorld() {
  RarWorld w;
  std::string out;
  long code = 0;
  if (!get("/world", out, code) || code != 200)
    return w;
  std::vector<std::string> lines;
  std::string cur;
  for (char c : out) {
    if (c == '\n') { lines.push_back(cur); cur.clear(); }
    else if (c != '\r') cur += c;
  }
  if (!cur.empty()) lines.push_back(cur);
  if (lines.size() >= 7) {
    w.seed = std::atoi(lines[0].c_str());
    w.mainVillains = std::atoi(lines[1].c_str());
    w.lesserVillains = std::atoi(lines[2].c_str());
    w.minorVillains = std::atoi(lines[3].c_str());
    w.allies = std::atoi(lines[4].c_str());
    w.retiredVillains = std::atoi(lines[5].c_str());
    w.worldName = lines[6];
    w.valid = true;
  }
  return w;
}

// Fetch the authoritative world blob (raw bytes). Returns false if the server has no world yet.
// The caller (game side) deserializes the bytes into a Campaign -- rar_client stays game-type-free.
bool rarFetchWorldData(std::string& out) {
  long code = 0;
  std::string body;
  if (!get("/world_data", body, code) || code != 200)
    return false;
  out = std::move(body);
  return !out.empty();
}

// Fetch from a THIRD-PARTY https host (the public server list). Deliberately NOT the normal http() path:
// that forces OUR server's cert pin + PSK knock on every https URL, which would reject GitHub outright.
//
// Cert verification is OFF here, and that is a DELIBERATE, BOUNDED choice:
//   * The list carries ONLY an address -- no pin, no PSK, nothing secret. The cert PIN (from appconfig, local
//     and trusted) is what actually authenticates the server, so a spoofed list cannot make us talk to an
//     attacker: a wrong server fails the pin check. Worst case is denial of service, not compromise.
//   * Verifying would mean shipping+maintaining a CA bundle: this libcurl uses OpenSSL, whose built-in CA dir
//     is an MSYS2 path a native keeper.exe cannot resolve, so there is no OS trust store to fall back on.
//     A bundled CA file goes stale as roots rotate -- unwanted upkeep for something the pin already covers.
//
// !! IF THE LIST EVER CARRIES PER-SERVER PINS (needed for a 2nd server with its own cert), THIS MUST GO BACK
// !! TO REAL CA VERIFICATION -- a spoofed list could then supply BOTH a fake address AND a matching fake pin,
// !! which is a full MITM. Today's safety rests entirely on the pin being local.
bool httpExternal(const std::string& url, std::string& out, long& code) {
  CURL* curl = curl_easy_init();
  if (!curl) { g_lastError = "curl init failed"; return false; }
  out.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // raw.githubusercontent.com redirects
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L); // see the note above -- the cert PIN is the real gate
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  CURLcode res = curl_easy_perform(curl);
  code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  if (res != CURLE_OK)
    g_lastError = std::string("server list: ") + curl_easy_strerror(res);
  curl_easy_cleanup(curl);
  return res == CURLE_OK;
}

bool rarFetchServerList(std::vector<std::string>& out) {
  out.clear();
  if (g_serverListUrl.empty())
    return false;
  std::string body;
  long code = 0;
  std::lock_guard<std::mutex> lk(g_mutex);
  if (!httpExternal(g_serverListUrl, body, code) || code != 200) {
    if (code != 200 && g_lastError.empty())
      g_lastError = "server list: HTTP " + std::to_string(code);
    return false;
  }
  size_t p = 0;
  while (p < body.size()) {
    size_t e = body.find('\n', p);
    if (e == std::string::npos) e = body.size();
    std::string line = body.substr(p, e - p);
    p = e + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos) continue;
    line = line.substr(s);
    if (line.empty() || line[0] == '#')
      continue; // comments + blanks
    out.push_back(line); // "host:port"
  }
  return !out.empty();
}

bool rarUploadCrash(const std::string& name, const std::string& blob) {
  if (g_login.empty() || g_password.empty()) {
    g_lastError = "not logged in";
    return false; // keep the file; it ships after the next successful login
  }
  std::string out;
  return post("/crash", g_login + "\n" + g_password + "\n" + name + "\n" + blob, out);
}

void rarSetServerUrl(const std::string& url) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_serverUrl = url;
  while (!g_serverUrl.empty() && g_serverUrl.back() == '/') g_serverUrl.pop_back();
}

bool rarWorldHash(std::string& out) {
  long code = 0;
  std::string body;
  if (!get("/world_hash", body, code) || code != 200)
    return false;
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
    body.pop_back();
  out = std::move(body);
  return !out.empty();
}

// Fetch the server's mod manifest: list of (modName, contentHash). Returns false only on a
// transport error; an empty manifest (vanilla server) returns true with an empty vector.
bool rarFetchModManifest(std::vector<std::pair<std::string, std::string>>& out) {
  long code = 0;
  std::string body;
  if (!get("/mods", body, code) || code != 200)
    return false;
  out.clear();
  std::string line;
  for (size_t i = 0; i <= body.size(); ++i) {
    if (i == body.size() || body[i] == '\n') {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto tab = line.find('\t');
      if (tab != std::string::npos)
        out.push_back({line.substr(0, tab), line.substr(tab + 1)});
      line.clear();
    } else
      line += body[i];
  }
  return true;
}

// Download a single mod's serialized bundle (raw bytes). The game side installs it.
bool rarFetchModBundle(const std::string& name, std::string& out) {
  long code = 0;
  std::string body;
  if (!get("/mod/" + urlEncode(name), body, code) || code != 200)
    return false;
  out = std::move(body);
  return true;
}

bool rarClaimSite(const std::string& gameId, const std::string& keeperName, int x, int y) {
  std::string out;
  bool ok = post("/claim", g_login + "\n" + g_password + "\n" + gameId + "\n" + keeperName + "\n" +
      std::to_string(x) + "\n" + std::to_string(y), out);
  if (ok)
    g_activeTempClaim = gameId; // remember it so we can release it if the game is abandoned
  return ok;
}

void rarClaimSave(const std::string& gameId) {
  std::string out;
  post("/claim_save", g_login + "\n" + g_password + "\n" + gameId, out);
  if (g_activeTempClaim == gameId)
    g_activeTempClaim.clear(); // now permanent, no longer a temp claim to release
}

void rarReleaseActiveTempClaim() {
  if (g_activeTempClaim.empty() || g_login.empty())
    return;
  std::string out;
  post("/release", g_login + "\n" + g_password + "\n" + g_activeTempClaim, out);
  g_activeTempClaim.clear();
}

// Force-delete this account's claim for a keeper (permanent included) -- keeper erased/abandoned.
void rarDeleteClaim(const std::string& gameId) {
  if (g_login.empty() || gameId.empty())
    return;
  std::string out;
  post("/delete_claim", g_login + "\n" + g_password + "\n" + gameId, out);
  if (g_activeTempClaim == gameId)
    g_activeTempClaim.clear();
}

// Upload this keeper's base-only dungeon snapshot for async invasions (server keeps it while
// the owner is offline). Binary-safe: gameId + blob go after the login/pw header lines.
bool rarUploadDungeon(const std::string& gameId, const std::string& blob, const std::string& hashHex) {
  if (g_login.empty() || gameId.empty())
    return false;
  // body: login\npassword\ngameId\nrawHash\n<blob>. The per-dungeon rawHash lets ANYONE (owner or an
  // invader) update it, and the owner compares it against their local copy on load (server wins if diff).
  std::string body = g_login + "\n" + g_password + "\n" + gameId + "\n" + hashHex + "\n" + blob;
  std::string out;
  return post("/dungeon", body, out);
}

// The server's stored hash of the current dungeon blob (of the RAW game bytes). "" if none/unreachable.
std::string rarDungeonHash(const std::string& gameId) {
  if (gameId.empty())
    return "";
  std::string out; long code = 0;
  if (get("/dungeon_hash/" + urlEncode(gameId), out, code) && code == 200)
    return out;
  return "";
}

// Reserve a dungeon for invasion (one invader at a time). GRANTED = free/ours/expired; PROTECTED = still
// under newbie protection (403); DENIED = held by another invader/conquered (409) or a network error.
RarReserveResult rarReserveDungeon(const std::string& gameId) {
  if (g_login.empty() || gameId.empty())
    return RarReserveResult::DENIED;
  std::string out; long code = 0;
  if (postCode("/reserve_dungeon", g_login + "\n" + g_password + "\n" + gameId, out, code) && code == 200)
    return RarReserveResult::GRANTED;
  if (code == 403)
    return RarReserveResult::PROTECTED;
  return RarReserveResult::DENIED;
}

// Release an invasion reservation (invasion ended / couldn't load). Best-effort, idempotent.
// Parses "word\n<number>" bodies used by the siege endpoints.
static long long siegeSeconds(const std::string& body) {
  auto nl = body.find('\n');
  if (nl == std::string::npos)
    return 0;
  try { return std::stoll(body.substr(nl + 1)); } catch (...) { return 0; }
}

RarSiegeResult rarOwnerReturning(const std::string& gameId, long long& secondsLeft) {
  secondsLeft = 0;
  std::string out;
  long code = 0;
  if (!postCode("/owner_returning", g_login + "\n" + g_password + "\n" + gameId, out, code) || code != 200)
    return RarSiegeResult::Unreachable; // can't ask -> caller decides (we let the owner in rather than block)
  if (out.rfind("siege", 0) == 0) {
    secondsLeft = siegeSeconds(out);
    return RarSiegeResult::UnderSiege;
  }
  return RarSiegeResult::Clear;
}

RarInvasionStatus rarInvasionStatus(const std::string& gameId, long long& secondsLeft) {
  secondsLeft = 0;
  std::string out;
  long code = 0;
  if (!postCode("/invasion_status", g_login + "\n" + g_password + "\n" + gameId, out, code) || code != 200)
    return RarInvasionStatus::Unknown; // transient network blip -> don't evict the invader over it
  if (out.rfind("ownerback", 0) == 0) {
    secondsLeft = siegeSeconds(out);
    return RarInvasionStatus::OwnerReturning;
  }
  return RarInvasionStatus::Ok;
}

void rarReleaseDungeon(const std::string& gameId) {
  if (g_login.empty() || gameId.empty())
    return;
  std::string out;
  post("/release_dungeon", g_login + "\n" + g_password + "\n" + gameId, out);
}

// The invader slew this keeper's leader. Tell the server so the base is removed after the grace period and
// the owner learns who slew them on their next Load. slayerGameId = the INVADER's own gameId -> the server
// resolves the slayer's KEEPER NAME from its claim (the world-map label, e.g. "world three"). fallbackName
// (the account login) is only used if the slayer has no claim.
void rarMarkConquered(const std::string& gameId, const std::string& fallbackName, const std::string& slayerGameId) {
  if (g_login.empty() || gameId.empty())
    return;
  // Never send an empty name -- the owner uses a non-empty reply as "this keeper is conquered".
  std::string name = fallbackName.empty() ? g_login : fallbackName;
  std::string out;
  post("/conquered", g_login + "\n" + g_password + "\n" + gameId + "\n" + name + "\n" + slayerGameId, out);
}

// Name of whoever slew this keeper's leader, or "" if not conquered / unreachable.
std::string rarConqueredBy(const std::string& gameId) {
  if (gameId.empty())
    return "";
  std::string out; long code = 0;
  if (get("/conquered_by/" + gameId, out, code) && code == 200)
    return out;
  return "";
}

// Report this keeper's ACTUAL play-turn count (call on every save). Drives newbie invasion protection:
// the server won't let others invade until it reaches the protection threshold. best-effort.
void rarReportPlayedTurns(const std::string& gameId, long long turns) {
  if (g_login.empty() || gameId.empty())
    return;
  std::string out;
  post("/played_turns", g_login + "\n" + g_password + "\n" + gameId + "\n" + std::to_string(turns), out);
}

// The slain owner has seen the "your keeper was slain" message and erased their local save -> tell the
// server to clear the slain record. best-effort.
void rarAckSlain(const std::string& gameId) {
  if (g_login.empty() || gameId.empty())
    return;
  std::string out;
  post("/slain_ack", g_login + "\n" + g_password + "\n" + gameId, out);
}

// 4c: has this keeper's server dungeon been damaged by an invader since its owner last saved?
// The owner checks this on continue -> if true, adopt the server's damaged copy. false on error.
bool rarDungeonInvaded(const std::string& gameId) {
  if (gameId.empty())
    return false;
  std::string out; long code = 0;
  if (get("/dungeon_invaded/" + gameId, out, code) && code == 200)
    return out == "1";
  return false;
}

// Download another player's cached dungeon blob (to load + invade it). false if none uploaded (404).
bool rarFetchDungeon(const std::string& gameId, std::string& out) {
  if (gameId.empty())
    return false;
  long code = 0;
  std::string body;
  if (!get("/dungeon/" + gameId, body, code) || code != 200)
    return false;
  out = std::move(body);
  return !out.empty();
}

// ---- RAR live PvP brokering (see rar_server.cpp /pvp_* routes) ----------------------------------------------
// Invader announces a live invasion of targetGameId. true (LIVE) with sessionId+seed set => the target's owner
// is online, do a live lockstep battle; false => target offline / not claimed / error, fall back to async.
bool rarPvpSetAway(const std::string& gameId, bool away) {
  if (g_login.empty() || gameId.empty())
    return false;
  std::string out;
  return post("/pvp_away", g_login + "\n" + g_password + "\n" + gameId + "\n" + (away ? "1" : "0"), out);
}

RarPvpInvite rarPvpInvite(const std::string& targetGameId, const std::string& invaderName, std::string& sessionId, int& seed) {
  if (g_login.empty() || targetGameId.empty())
    return RarPvpInvite::Offline;
  std::string out;
  if (!post("/pvp_invite", g_login + "\n" + g_password + "\n" + targetGameId + "\n" + invaderName, out))
    return RarPvpInvite::Offline;
  if (out.rfind("AWAY", 0) == 0)   // he is out raiding -- his base is unattended, so it can't be invaded
    return RarPvpInvite::Away;
  if (out.rfind("LIVE", 0) != 0) // "OFFLINE" or anything else
    return RarPvpInvite::Offline;
  auto n1 = out.find('\n');
  auto n2 = (n1 == std::string::npos) ? n1 : out.find('\n', n1 + 1);
  if (n2 == std::string::npos) return RarPvpInvite::Offline;
  sessionId = out.substr(n1 + 1, n2 - n1 - 1);
  seed = std::atoi(out.c_str() + n2 + 1);
  return sessionId.empty() ? RarPvpInvite::Offline : RarPvpInvite::Live;
}

// Defender poll: true with sessionId/invaderName/seed set if a live invite is pending against myGameId.
bool rarPvpPoll(const std::string& myGameId, std::string& sessionId, std::string& invaderName, int& seed) {
  if (g_login.empty() || myGameId.empty())
    return false;
  std::string out;
  if (!post("/pvp_poll", g_login + "\n" + g_password + "\n" + myGameId, out))
    return false;
  if (out == "NONE" || out.empty())
    return false;
  auto n1 = out.find('\n');
  auto n2 = (n1 == std::string::npos) ? n1 : out.find('\n', n1 + 1);
  if (n2 == std::string::npos) return false;
  sessionId = out.substr(0, n1);
  invaderName = out.substr(n1 + 1, n2 - n1 - 1);
  seed = std::atoi(out.c_str() + n2 + 1);
  return !sessionId.empty();
}

// Upload this side's packed start-state (role 0 = defender's base, role 1 = invader's keeper). Both peers then
// load BOTH blobs and construct the same combined battlefield, so their lockstep sims start bit-identical.
bool rarPvpUploadState(const std::string& sessionId, int role, const std::string& blob) {
  if (g_login.empty() || sessionId.empty())
    return false;
  std::string out;
  return post("/pvp_state", g_login + "\n" + g_password + "\n" + sessionId + "\n" + std::to_string(role) + "\n" + blob, out);
}

// Download a side's packed start-state (role 0 = defender's base, role 1 = invader's keeper). Fails until that
// side has uploaded it (caller polls).
bool rarPvpFetchState(const std::string& sessionId, int role, std::string& out) {
  if (sessionId.empty())
    return false;
  long code = 0;
  std::string body;
  if (!get("/pvp_state/" + sessionId + "/" + std::to_string(role), body, code) || code != 200)
    return false;
  out = std::move(body);
  return !out.empty();
}

// role: 0=defender 1=invader. leave=true drops the session (peer sees Gone). Returns Both once both are in.
RarPvpReady rarPvpReady(const std::string& sessionId, int role, bool leave) {
  if (g_login.empty() || sessionId.empty())
    return RarPvpReady::Gone;
  std::string r = leave ? ("-" + std::to_string(role)) : std::to_string(role);
  std::string out;
  if (!post("/pvp_ready", g_login + "\n" + g_password + "\n" + sessionId + "\n" + r, out))
    return RarPvpReady::Gone;
  if (out == "BOTH") return RarPvpReady::Both;
  if (out == "WAIT") return RarPvpReady::Wait;
  return RarPvpReady::Gone;
}

// Start (once) the background defender-watch thread and point it at myGameId. Cheap to call every load; it just
// updates which keeper to poll for. The game loop reads results via rarPvpPendingInvite (no network on its thread).
void rarStartPvpWatch(const std::string& myGameId) {
  { std::lock_guard<std::mutex> lk(g_pvpMutex); g_pvpWatchGameId = myGameId; g_pvpInvitePending = false; }
  if (g_pvpWatchStarted.exchange(true))
    return;
  std::thread([] {
    while (true) {
      for (int i = 0; i < 4; ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));
      std::string gid;
      { std::lock_guard<std::mutex> lk(g_pvpMutex); gid = g_pvpWatchGameId; }
      if (gid.empty())
        continue;
      std::string sid, name; int seed = 0;
      if (rarPvpPoll(gid, sid, name, seed)) {
        std::lock_guard<std::mutex> lk(g_pvpMutex);
        if (g_pvpWatchGameId == gid) { // still the active keeper
          g_pvpInviteSession = sid; g_pvpInviteName = name; g_pvpInviteSeed = seed; g_pvpInvitePending = true;
        }
      }
    }
  }).detach();
}

// Stop polling for invites (keeper unloaded / left the game).
void rarStopPvpWatch() {
  std::lock_guard<std::mutex> lk(g_pvpMutex);
  g_pvpWatchGameId.clear();
  g_pvpInvitePending = false;
}

// Peek WITHOUT consuming: is an invite waiting? Used by the turn-based game loop, which otherwise never runs
// its per-frame code while the player idles in control mode -- so the invite would never be noticed.
bool rarPvpInvitePending() {
  std::lock_guard<std::mutex> lk(g_pvpMutex);
  return g_pvpInvitePending;
}

// Non-blocking: true (consuming it) if the watch thread has seen a live invite for this keeper.
bool rarPvpPendingInvite(std::string& sessionId, std::string& invaderName, int& seed) {
  std::lock_guard<std::mutex> lk(g_pvpMutex);
  if (!g_pvpInvitePending)
    return false;
  sessionId = g_pvpInviteSession; invaderName = g_pvpInviteName; seed = g_pvpInviteSeed;
  g_pvpInvitePending = false;
  return true;
}

bool rarFetchVillain(const std::string& key, std::string& out) {
  if (key.empty())
    return false;
  long code = 0;
  std::string body;
  if (!get("/villain/" + key, body, code) || code != 200)
    return false;
  out = std::move(body);
  return !out.empty();
}

void rarMarkVillainDefeated(const std::string& key) {
  if (g_login.empty() || key.empty())
    return;
  std::string out;
  post("/villain_defeated", g_login + "\n" + g_password + "\n" + key, out); // best-effort
}

// Upload the post-battle aftermath of a defeated villain so grace-period revisits show the real outcome.
// body: login\npassword\nx_y\n<blob>. best-effort; the server only accepts it for a defeated-in-grace villain.
bool rarVillainWriteback(const std::string& key, const std::string& blob) {
  if (g_login.empty() || key.empty() || blob.empty())
    return false;
  std::string body = g_login + "\n" + g_password + "\n" + key + "\n" + blob;
  std::string out;
  return post("/villain_writeback", body, out);
}

bool rarFetchVillainState(std::string& out) {
  long code = 0;
  return get("/villain_state", out, code) && code == 200;
}

// Re-pull the server's dead-villain set into the overlay. On failure keep the previous set (don't flicker).
void rarRefreshVillainState() {
  std::string out; long code = 0;
  if (!get("/villain_state", out, code) || code != 200)
    return;
  std::set<std::string> next;
  std::string line;
  for (char c : out) {
    if (c == '\n') { if (!line.empty()) next.insert(line); line.clear(); }
    else if (c != '\r') line += c;
  }
  if (!line.empty()) next.insert(line);
  g_deadVillains = std::move(next);
}

bool rarIsVillainDead(int x, int y) {
  return g_deadVillains.count(std::to_string(x) + "_" + std::to_string(y)) > 0;
}

std::vector<RarVillain> rarGetVillainRoster() {
  std::vector<RarVillain> res;
  std::string out; long code = 0;
  if (!get("/villain_roster", out, code) || code != 200)
    return res;
  std::string line;
  auto flush = [&] {
    // line = "x_y\tTIER\tenemyId\talive(1|0)"  (alive field optional for back-compat)
    std::vector<std::string> f; std::string cur;
    for (char c : line) { if (c == '\t') { f.push_back(cur); cur.clear(); } else cur += c; }
    f.push_back(cur);
    if (f.size() < 2) return;
    auto us = f[0].find('_');
    if (us == std::string::npos) return;
    RarVillain v;
    try { v.x = std::stoi(f[0].substr(0, us)); v.y = std::stoi(f[0].substr(us + 1)); }
    catch (...) { return; }
    v.tier = f[1];
    if (f.size() >= 3) v.enemyId = f[2];
    if (f.size() >= 4) v.defeated = (f[3] == "0"); // alive=0 -> defeated-in-grace (lootable corpse)
    res.push_back(v);
  };
  for (char c : out) { if (c == '\n') { if (!line.empty()) flush(); line.clear(); } else if (c != '\r') line += c; }
  if (!line.empty()) flush();
  return res;
}

std::vector<RarClaim> rarGetClaims() {
  std::vector<RarClaim> res;
  std::string out;
  long code = 0;
  if (!get("/claims", out, code) || code != 200)
    return res;
  std::string line;
  auto flush = [&] {
    if (line.empty()) return;
    // "x\ty\tlogin\tgameId\tperm\tname"
    std::vector<std::string> f;
    std::string cur;
    for (char c : line) { if (c == '\t') { f.push_back(cur); cur.clear(); } else cur += c; }
    f.push_back(cur);
    if (f.size() >= 6) {
      RarClaim c;
      c.x = std::atoi(f[0].c_str());
      c.y = std::atoi(f[1].c_str());
      c.login = f[2];
      c.gameId = f[3];
      c.permanent = (f[4] == "1");
      c.name = f[5];
      if (f.size() >= 7) c.conquered = (f[6] == "1"); // leader slain, in removal grace -> defeated sprite
      if (f.size() >= 8) c.protectedNewbie = (f[7] == "1"); // still under newbie invasion protection
      res.push_back(c);
    }
    line.clear();
  };
  for (char c : out) { if (c == '\n') flush(); else if (c != '\r') line += c; }
  flush();
  return res;
}

std::string rarHashFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return "";
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return rarHashHex(data);
}

void rarSetSaveRegistry(const std::string& filePath) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_registryPath = filePath;
  g_saveOwners.clear();
  std::ifstream in(filePath);
  std::string line;
  while (std::getline(in, line)) {
    auto tab = line.rfind('\t');
    if (tab != std::string::npos)
      g_saveOwners[line.substr(0, tab)] = line.substr(tab + 1);
  }
}

void rarRecordSaveOwnership(const std::string& gameId) {
  std::lock_guard<std::mutex> lk(g_mutex);
  if (g_login.empty() || g_registryPath.empty()) return;
  auto it = g_saveOwners.find(gameId);
  if (it != g_saveOwners.end() && it->second == g_login) return; // already recorded
  g_saveOwners[gameId] = g_login;
  std::ofstream out(g_registryPath, std::ios::trunc);
  for (auto& e : g_saveOwners) out << e.first << '\t' << e.second << '\n';
}

bool rarOwnsSave(const std::string& gameId) {
  std::lock_guard<std::mutex> lk(g_mutex);
  auto it = g_saveOwners.find(gameId);
  return it != g_saveOwners.end() && it->second == g_login;
}

// Ask the SERVER which keepers this account actually owns. The server is the single point of truth: the local
// rar_saves.txt registry is only a per-PC hint and must NEVER decide what's loadable. A local .kep the server
// doesn't know about is stale (deleted keeper/account, or another PC's) -- loading it would resurrect the
// keeper on the next save AND drag its own out-of-date world map back in. Returns false if the server can't
// be asked, in which case callers must FAIL CLOSED (show nothing) rather than fall back to local state.
// RAR data protection: what the server we are connecting to says its rule files hash to, and the files
// themselves. Empty hash = an older server that does not publish one; treat that as "no opinion" and leave
// the client's content alone rather than wiping it.
std::string rarFetchServerDataFreeHash() {
  std::string body;
  long code = 0;
  if (!get("/data_free_hash", body, code) || code != 200)
    return "";
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r' || body.back() == ' '))
    body.pop_back();
  return body;
}

bool rarFetchServerDataFree(std::string& out) {
  long code = 0;
  if (!get("/data_free", out, code) || code != 200) {
    g_lastError = "couldn't download data_free from the server";
    return false;
  }
  return !out.empty();
}

bool rarListKeepers(std::set<std::string>& out) {
  out.clear();
  std::string body;
  long code = 0;
  if (!get("/keepers/" + g_login, body, code) || code != 200) {
    g_lastError = "couldn't fetch the account's keepers from the server";
    return false;
  }
  size_t p = 0;
  while (p < body.size()) {
    size_t e = body.find('\n', p);
    if (e == std::string::npos) e = body.size();
    std::string id = body.substr(p, e - p);
    while (!id.empty() && (id.back() == '\r' || id.back() == ' ')) id.pop_back();
    if (!id.empty()) out.insert(id);
    p = e + 1;
  }
  return true;
}

// Exercises the SIEGE state machine against a live server through the real transport (knock+TLS+pin), because
// reproducing it for real needs two players and a 60s window. Uses one throwaway gameId; safe to re-run.
int rarSiegeSelfTest(const std::string& gameId) {
  int bad = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("[siege-test] %-46s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) ++bad;
    std::fflush(stdout);
  };
  if (!rarEnabled()) { std::printf("[siege-test] not configured (need server_url + --rar_login)\n"); return 1; }
  rarReleaseDungeon(gameId); // start clean in case a previous run left state
  long long secs = 0;
  check(rarReserveDungeon(gameId) == RarReserveResult::GRANTED, "invader reserves the dungeon");
  check(rarInvasionStatus(gameId, secs) == RarInvasionStatus::Ok, "invader status: quiet (owner offline)");
  // owner knocks -> siege starts
  check(rarOwnerReturning(gameId, secs) == RarSiegeResult::UnderSiege, "owner knocks -> UNDER SIEGE");
  check(secs > 0, "owner sees a countdown > 0");
  check(rarInvasionStatus(gameId, secs) == RarInvasionStatus::OwnerReturning, "invader now told: owner returning");
  std::printf("[siege-test]   (invader has %llds to get out)\n", secs); std::fflush(stdout);
  // anti-lockout: nobody may reserve it now, not even the invader already inside
  check(rarReserveDungeon(gameId) == RarReserveResult::PROTECTED, "30-min protection blocks NEW invasions");
  // wait out the grace -> countdown must reach 0 (that's when the client force-exits control)
  std::printf("[siege-test] waiting out the grace...\n"); std::fflush(stdout);
  for (int i = 0; i < 40 && secs > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    rarInvasionStatus(gameId, secs);
  }
  check(secs == 0, "countdown reaches 0 -> invader is evicted");
  // invader's writeback+upload finishes, THEN he releases -> only now may the owner enter
  rarReleaseDungeon(gameId);
  check(rarOwnerReturning(gameId, secs) == RarSiegeResult::Clear, "after release -> owner is CLEAR to load");
  check(rarReserveDungeon(gameId) == RarReserveResult::PROTECTED, "protection still holds after the siege");
  rarReleaseDungeon(gameId);
  std::printf("[siege-test] %s\n", bad == 0 ? "ALL PASSED" : "FAILURES ABOVE");
  return bad == 0 ? 0 : 1;
}

int rarClientSelfTest() {
  std::printf("[rar-client] server=%s login=%s\n", g_serverUrl.c_str(), g_login.c_str());
  { // server list (third-party TLS w/ real CA verification -- proves the CA store works in a native build)
    std::vector<std::string> servers;
    if (rarFetchServerList(servers)) {
      std::printf("[rar-client] server list: %zu entry(s) from %s\n", servers.size(), g_serverListUrl.c_str());
      for (auto& s : servers)
        std::printf("[rar-client]   - %s\n", s.c_str());
    } else
      std::printf("[rar-client] server list: FAILED (%s)\n", rarLastError().c_str());
  }
  if (!rarEnabled()) { std::printf("[rar-client] not configured (need server_url + --rar_login)\n"); return 1; }
  bool reg = rarRegister();
  std::printf("[rar-client] register: %s\n", reg ? "ok (new account)" :
      ("(" + rarLastError() + ") -- fine if already registered").c_str());
  bool login = (rarLoginCheck() == RarLoginResult::Ok);
  std::printf("[rar-client] login:    %s\n", login ? "ok" : ("FAIL (" + rarLastError() + ")").c_str());
  if (!login) return 1;
  const std::string keeper = "selftest_keeper";
  const std::string h = rarHashHex("some-fake-save-bytes");
  bool up = rarUploadSaveHash(keeper, h);
  std::printf("[rar-client] upload hash %s: %s\n", h.c_str(), up ? "ok" : ("FAIL (" + rarLastError() + ")").c_str());
  std::string got = rarGetSaveHash(keeper);
  std::printf("[rar-client] read back:  %s  (%s)\n", got.c_str(), got == h ? "MATCH" : "MISMATCH");
  // /keepers is what the load menu gates on: if it reports failure the menu returns to the main screen,
  // which forces a fresh login. An account with ZERO keepers must come back OK-with-nothing, not "failed".
  std::set<std::string> keepers;
  bool listed = rarListKeepers(keepers);
  std::printf("[rar-client] keepers:   %s (%zu)%s\n", listed ? "ok" : "FAIL", keepers.size(),
      listed ? "" : (" -- " + rarLastError()).c_str());
  for (auto& k : keepers)
    std::printf("[rar-client]   - %s\n", k.c_str());
  // The exact sequence the load menu runs when you erase your LAST keeper: delete, then immediately re-list.
  // If this second list reports failure the menu bails to the main screen and the player has to log in again.
  rarDeleteClaim(g_login + "~" + keeper);
  std::set<std::string> after;
  bool listed2 = rarListKeepers(after);
  std::printf("[rar-client] list after delete: %s (%zu)%s\n", listed2 ? "ok" : "FAIL", after.size(),
      listed2 ? "" : (" -- " + rarLastError()).c_str());
  return (up && got == h && listed && listed2) ? 0 : 1;
}
