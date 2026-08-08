#include "serialization.h"   // OutputArchive (cereal binary) + stdafx (string/vector) + cereal type headers
#include "directory_path.h"
#include "file_path.h"
#include "rar_hash.h"        // rarSha256Hex
#include "rar_mods.h"
#include "util.h"        // split(): unbundle walks the relative paths in the blob
#include <sstream>
#include <fstream>
#include <algorithm>

// Read a file's EXACT bytes. Deliberately not FilePath::readContents(): that opens in the default (text)
// mode, and on Windows text mode translates CRLF and stops at a 0x1A byte -- so a mod's .png files came back
// mangled and short. The client then hashed 149KB where the Linux server hashed 363KB for the very same
// folder, the hashes could never match, and syncServerMods re-downloaded every image-carrying mod on every
// single sync. Must stay binary on both platforms or the hashes diverge again.
static optional<string> readFileBinary(const FilePath& f) {
  std::ifstream in(f.getPath(), std::ios::binary);
  if (!in.good())
    return none;
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// Recursively gather a mod folder's files as (relative-path, contents) pairs. Mirrors the original
// collectModFiles that lived in main_loop.cpp -- moved here so client + server share ONE implementation.
static void collectModFiles(const DirectoryPath& dir, const string& prefix, vector<pair<string, string>>& out) {
  for (auto& f : dir.getFiles())
    if (auto c = readFileBinary(f))
      out.push_back(make_pair(prefix + f.getFileName(), *c));
  for (auto& sub : dir.getSubDirs())
    collectModFiles(dir.subdirectory(sub), prefix + sub + "/", out);
}

string rarBundleModDir(const string& modDirPath) {
  vector<pair<string, string>> files;
  collectModFiles(DirectoryPath(modDirPath), "", files);
  std::sort(files.begin(), files.end()); // deterministic order -> stable hash across machines
  std::stringstream ss;
  {
    OutputArchive ar(ss);
    ar << files;
  }
  return ss.str();
}

std::vector<std::string> rarModsInLoadOrder(const string& modsDirPath) {
  DirectoryPath modsDir(modsDirPath);
  std::vector<std::string> present;
  if (modsDir.exists())
    for (auto& m : modsDir.getSubDirs())
      present.push_back(m);
  auto installed = [&](const std::string& m) {
    return std::find(present.begin(), present.end(), m) != present.end();
  };
  std::vector<std::string> ordered;
  auto already = [&](const std::string& m) {
    return std::find(ordered.begin(), ordered.end(), m) != ordered.end();
  };
  { std::ifstream in(modsDir.file("load_order.txt").getPath());
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
      auto s = line.find_first_not_of(" \t");
      if (s == std::string::npos)
        continue;
      line = line.substr(s);
      if (line.empty() || line[0] == '#')
        continue;
      if (installed(line) && !already(line)) // silently skip entries for mods that aren't installed
        ordered.push_back(line);
    } }
  for (auto& m : present) // anything not listed keeps working -- appended after the ordered ones
    if (!already(m))
      ordered.push_back(m);
  return ordered;
}

int rarPublishMods(const string& modsDirPath, const string& outDirPath) {
  DirectoryPath modsDir(modsDirPath);
  DirectoryPath outDir(outDirPath);
  DirectoryPath bundleDir = outDir.subdirectory("rar_mods");
  bundleDir.createIfDoesntExist();
  std::ofstream manifest(outDir.file("rar_mods.txt").getPath());
  int n = 0;
  // Publish in LOAD ORDER: the client mirrors this manifest's order exactly (syncServerMods), so this is what
  // actually decides the client's mod order too.
  if (modsDir.exists())
    for (auto& mod : rarModsInLoadOrder(modsDirPath)) {
      string bundle = rarBundleModDir(modsDir.subdirectory(mod).getPath());
      string hash = rarSha256Hex(bundle); // SHA-256: tamper-resistant, matches the dungeon anticheat
      std::ofstream out(bundleDir.file(mod + ".dat").getPath(), std::ios::binary);
      out.write(bundle.data(), bundle.size());
      manifest << mod << "\t" << hash << "\n";
      ++n;
    }
  manifest.close();
  return n;
}

// The rule-bearing part of data_free as an unpackable bundle: same cereal (relpath, bytes) format a mod
// bundle uses, so the client can unpack it with the same code. game_config/ and ui/ only -- images/ and the
// loose root files are art and platform glue, cannot change what the game DOES, and would make the hash
// differ between builds for no reason. The subfolder list is FIXED so dropping a folder into data_free can
// never silently change what is protected.
string rarBundleDataFree(const string& dataFreePath) {
  DirectoryPath root(dataFreePath);
  if (!root.exists())
    return "";
  vector<pair<string, string>> files;
  for (auto& sub : {"game_config", "ui"}) {
    auto dir = root.subdirectory(sub);
    if (dir.exists())
      collectModFiles(dir, string(sub) + "/", files);
  }
  std::sort(files.begin(), files.end()); // deterministic across filesystems
  std::stringstream ss;
  {
    OutputArchive ar(ss);
    ar << files;
  }
  return ss.str();
}

// Hash IS the hash of the bundle, deliberately: whatever a client downloads is exactly what it verifies, so
// the two can never drift apart the way a separately-computed digest could.
std::string rarHashDataFree(const string& dataFreePath) {
  auto bundle = rarBundleDataFree(dataFreePath);
  return bundle.empty() ? "" : rarSha256Hex(bundle);
}

// Unpack a bundle produced by rarBundleDataFree over an existing data_free. Files are OVERWRITTEN in place
// and anything in game_config/ or ui/ that the bundle does not contain is DELETED -- that is the point, a
// tampered install may have added files as well as edited them. Nothing outside those two folders is
// touched, so images/ and the loose root files survive. Returns false if the blob will not parse, in which
// case NOTHING has been written yet: the caller must not be left with a half-repaired install.
bool rarUnbundleDataFree(const string& dataFreePath, const string& bundle) {
  vector<pair<string, string>> files;
  try {
    std::stringstream ss(bundle);
    InputArchive ar(ss);
    ar >> files;
  } catch (...) {
    return false;
  }
  if (files.empty())
    return false;
  DirectoryPath root(dataFreePath);
  root.createIfDoesntExist();
  // Wipe the two managed folders first so orphans cannot survive, then write the bundle back. Safe only
  // because the blob is fully parsed above -- a bad download can no longer destroy the install.
  for (auto& sub : {"game_config", "ui"}) {
    auto dir = root.subdirectory(sub);
    if (dir.exists())
      dir.removeRecursively();
  }
  for (auto& elem : files) {
    auto parts = split(elem.first, {'/'});
    DirectoryPath cur = root;
    for (int i = 0; i + 1 < (int) parts.size(); ++i) {
      cur = cur.subdirectory(parts[i]);
      cur.createIfDoesntExist();
    }
    std::ofstream out(cur.file(parts.back()).getPath(), std::ios::binary);
    out.write(elem.second.data(), elem.second.size());
  }
  return true;
}

// ---- release manifest -----------------------------------------------------------------------------------
// A PLAIN TEXT list of every file in an install, one per line:
//
//     <relative/path><TAB><sha256><TAB><bytes>
//
// Deliberately not the cereal bundle the game uses. keeper_updater has to keep working across game versions,
// including ones whose serialization changed, so it must not share a binary format with the thing it updates.
// Text also means the manifest can be read by anything -- a script, a browser, a human diffing two releases.
//
// `protectedOnly` writes just the rule-bearing subset (data_free/game_config + data_free/ui), i.e. exactly
// what rarHashDataFree covers. That is the tamper-check list. The full form additionally covers the binary,
// the DLLs and the rest, which is what a repair-the-install update needs.
static void collectAll(const DirectoryPath& dir, const string& prefix, vector<pair<string, string>>& out) {
  for (auto& f : dir.getFiles())
    if (auto c = readFileBinary(f))
      out.push_back(make_pair(prefix + f.getFileName(), *c));
  for (auto& sub : dir.getSubDirs())
    collectAll(dir.subdirectory(sub), prefix + sub + "/", out);
}

int rarWriteManifest(const string& rootPath, const string& outFilePath, bool protectedOnly) {
  DirectoryPath root(rootPath);
  if (!root.exists())
    return 0;
  vector<pair<string, string>> files;
  if (protectedOnly) {
    auto df = root.subdirectory("data_free");
    for (auto& sub : {"game_config", "ui"}) {
      auto dir = df.subdirectory(sub);
      if (dir.exists())
        collectAll(dir, string("data_free/") + sub + "/", files);
    }
  } else
    collectAll(root, "", files);
  std::sort(files.begin(), files.end());
  std::ofstream out(outFilePath);
  for (auto& f : files)
    out << f.first << "\t" << rarSha256Hex(f.second) << "\t" << f.second.size() << "\n";
  return (int) files.size();
}
