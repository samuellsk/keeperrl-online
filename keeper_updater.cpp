// keeper_updater -- integrity check and repair for a KeeperRL Online install, run BEFORE the game.
//
// WHY IT IS A SEPARATE PROGRAM, not a mode of keeper.exe:
//   * Windows locks a running .exe, so the game can never replace its own binary. Only another process can.
//   * data_free is parsed into memory at startup, so repairing it from inside a running game would not take
//     effect until a restart anyway. Checking first and then launching sidesteps that entirely.
//
// WHY IT SHARES NO GAME CODE: it must keep working against game versions whose content format has changed --
// that is exactly when you need it most. Its only dependencies are libcurl and rar_hash.h, which is a
// header-only SHA-256 with no includes of its own. It never loads game content and never links the engine.
//
// AUTHORITY ORDER (deliberate):
//   1. GitHub  -- the release the player installed. Works with no game server, and is the public baseline.
//   2. The game server, checked by the game itself at login. If a server publishes different content it WINS,
//      so somebody running a private server with their own rules has clients follow that server rather than
//      being pinned to the public release.
//
// The player is always TOLD what was repaired. Silent repair would mean quietly changing someone's game.

#include "rar_hash.h"     // rarSha256Hex -- header-only, no dependencies

#include <curl/curl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

namespace {

// ---- tiny helpers ---------------------------------------------------------------------------------------

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  ((std::string*) userdata)->append(ptr, size * nmemb);
  return size * nmemb;
}

// GET a URL into `out`. Plain HTTPS with normal CA verification -- this talks to GitHub, not to our
// self-signed game server, so the pinning the game client uses does not apply here.
bool httpGet(const std::string& url, std::string& out) {
  CURL* c = curl_easy_init();
  if (!c)
    return false;
  out.clear();
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   // raw.githubusercontent redirects
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "keeper_updater");
  // Windows libcurl ships no CA bundle, so ordinary HTTPS verification fails against GitHub with a
  // "could not reach" that looks exactly like being offline. Use the OS certificate store instead.
  // Verification stays ON: this is a plain public host, not our pinned self-signed game server.
#ifdef CURLSSLOPT_NATIVE_CA
  curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long) CURLSSLOPT_NATIVE_CA);
#endif
  CURLcode res = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(c);
  return res == CURLE_OK && code == 200;
}

bool readFileBinary(const std::string& path, std::string& out) {
  std::ifstream in(path.c_str(), std::ios::binary);
  if (!in.good())
    return false;
  std::stringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

// Create every directory along a relative path. No std::filesystem: the game builds as C++14 and the updater
// deliberately matches, so it can be compiled by the same toolchain with no extra flags.
void makeParentDirs(const std::string& relPath) {
  std::string acc;
  for (size_t i = 0; i < relPath.size(); ++i) {
    if (relPath[i] == '/' || relPath[i] == '\\') {
#ifdef _WIN32
      CreateDirectoryA(acc.c_str(), nullptr);
#else
      mkdir(acc.c_str(), 0755);
#endif
    }
    acc.push_back(relPath[i]);
  }
}

bool writeFileBinary(const std::string& path, const std::string& data) {
  makeParentDirs(path);
  std::ofstream out(path.c_str(), std::ios::binary | std::ios::trunc);
  if (!out.good())
    return false;
  out.write(data.data(), (std::streamsize) data.size());
  return out.good();
}

// appconfig.txt is a flat list of "key" "value" pairs. Only a couple of keys are needed here, so this is a
// deliberately dumb scan rather than a real parser -- the updater must not depend on the game's config code.
std::string configValue(const std::string& text, const std::string& key) {
  std::string needle = "\"" + key + "\"";
  size_t p = text.find(needle);
  if (p == std::string::npos)
    return "";
  p = text.find('"', p + needle.size());
  if (p == std::string::npos)
    return "";
  size_t e = text.find('"', p + 1);
  if (e == std::string::npos)
    return "";
  return text.substr(p + 1, e - p - 1);
}

struct Entry {
  std::string sha;
  long long size;
};

// Manifest lines are "<relative/path>\t<sha256>\t<bytes>". Anything malformed is skipped rather than fatal:
// a manifest from a newer release must never stop an older updater from doing the part it does understand.
std::map<std::string, Entry> parseManifest(const std::string& text) {
  std::map<std::string, Entry> ret;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (line.empty() || line[0] == '#')
      continue;
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos)
      continue;
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos)
      continue;
    Entry e;
    e.sha = line.substr(t1 + 1, t2 - t1 - 1);
    e.size = atoll(line.c_str() + t2 + 1);
    ret[line.substr(0, t1)] = e;
  }
  return ret;
}

void launchGame(const std::string& exe, int argc, char** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i)      // pass our own arguments through to the game
    args.push_back(argv[i]);
#ifdef _WIN32
  std::string cmd = "\"" + exe + "\"";
  for (size_t i = 0; i < args.size(); ++i)
    cmd += " " + args[i];
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));
  std::vector<char> buf(cmd.begin(), cmd.end());
  buf.push_back('\0');
  if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    std::printf("[updater] could not start %s\n", exe.c_str());
    return;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
#else
  std::vector<char*> cargs;
  cargs.push_back(const_cast<char*>(exe.c_str()));
  for (size_t i = 0; i < args.size(); ++i)
    cargs.push_back(const_cast<char*>(args[i].c_str()));
  cargs.push_back(nullptr);
  execv(exe.c_str(), cargs.data());
  std::printf("[updater] could not exec %s\n", exe.c_str());
#endif
}

} // namespace

// Exit codes, so keeper.exe can tell the player what happened:
//   0  verified, nothing to do        10  files were repaired
//   20 check skipped (not configured, offline, or no manifest)
//   30 something could not be repaired
enum { EXIT_OK = 0, EXIT_REPAIRED = 10, EXIT_SKIPPED = 20, EXIT_FAILED = 30 };

int main(int argc, char** argv) {
  // When the GAME launches us it wants a verdict, not a relaunch -- it is already running and will load the
  // repaired files itself. Standalone (a player double-clicking us) we check and then start the game.
  bool checkOnly = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--check-only")
      checkOnly = true;
#ifdef _WIN32
  const std::string gameExe = "keeper.exe";
#else
  const std::string gameExe = "./keeper";
#endif
  std::string cfg;
  if (!readFileBinary("appconfig.txt", cfg)) {
    std::printf("[updater] no appconfig.txt here -- run me from the game folder. Starting the game anyway.\n");
    launchGame(gameExe, argc, argv);
    return 0;
  }
  std::string manifestUrl = configValue(cfg, "manifest_url");
  std::string contentBase = configValue(cfg, "content_base_url");
  if (manifestUrl.empty() || contentBase.empty()) {
    // Not configured => nothing to check against. Never block the player over a missing setting.
    std::printf("[updater] manifest_url/content_base_url not set in appconfig.txt -- skipping the check.\n");
    launchGame(gameExe, argc, argv);
    return 0;
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::string manifestText;
  if (!httpGet(manifestUrl, manifestText)) {
    // Offline, or GitHub is down. That must never stop someone playing.
    std::printf("[updater] could not reach %s -- skipping the check.\n", manifestUrl.c_str());
    curl_global_cleanup();
    if (!checkOnly) launchGame(gameExe, argc, argv);
    return EXIT_SKIPPED;
  }
  auto manifest = parseManifest(manifestText);
  std::printf("[updater] manifest: %d file(s)\n", (int) manifest.size());

  // Pass 1: find what differs. Nothing is written yet -- the player is told first, and a failed download
  // must not be able to leave a half-repaired install.
  std::vector<std::string> missing, changed;
  for (auto& e : manifest) {
    std::string local;
    if (!readFileBinary(e.first, local))
      missing.push_back(e.first);
    else if (rarSha256Hex(local) != e.second.sha)
      changed.push_back(e.first);
  }
  if (missing.empty() && changed.empty()) {
    std::printf("[updater] all %d file(s) verified -- nothing to repair.\n", (int) manifest.size());
    curl_global_cleanup();
    launchGame(gameExe, argc, argv);
    return 0;
  }

  std::printf("\n  Your game files do not match the official release.\n");
  if (!changed.empty()) {
    std::printf("  %d modified:\n", (int) changed.size());
    for (size_t i = 0; i < changed.size() && i < 10; ++i)
      std::printf("    %s\n", changed[i].c_str());
    if (changed.size() > 10)
      std::printf("    ... and %d more\n", (int) changed.size() - 10);
  }
  if (!missing.empty()) {
    std::printf("  %d missing:\n", (int) missing.size());
    for (size_t i = 0; i < missing.size() && i < 10; ++i)
      std::printf("    %s\n", missing[i].c_str());
    if (missing.size() > 10)
      std::printf("    ... and %d more\n", (int) missing.size() - 10);
  }
  std::printf("  Repairing them now.\n\n");

  // Pass 2: download everything first, verify each against the manifest, and only then write. A file that
  // fails to download or fails its hash is left ALONE rather than replaced with something wrong.
  std::map<std::string, std::string> fetched;
  int failed = 0;
  std::vector<std::string> todo = changed;
  todo.insert(todo.end(), missing.begin(), missing.end());
  for (auto& path : todo) {
    std::string body;
    std::string url = contentBase;
    if (!url.empty() && url[url.size() - 1] != '/')
      url += "/";
    url += path;
    if (!httpGet(url, body) || rarSha256Hex(body) != manifest[path].sha) {
      std::printf("[updater] FAILED to fetch a good copy of %s -- left untouched\n", path.c_str());
      ++failed;
      continue;
    }
    fetched[path] = body;
  }
  int repaired = 0;
  for (auto& f : fetched)
    if (writeFileBinary(f.first, f.second))
      ++repaired;
    else
      std::printf("[updater] could not write %s (is it read-only, or is the game running?)\n", f.first.c_str());

  std::printf("\n[updater] repaired %d file(s)%s.\n", repaired,
      failed ? " -- some could not be fetched, see above" : "");
  curl_global_cleanup();
  if (!checkOnly) launchGame(gameExe, argc, argv);
  return failed ? EXIT_FAILED : EXIT_REPAIRED;
}
