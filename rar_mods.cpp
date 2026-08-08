#include "serialization.h"   // OutputArchive (cereal binary) + stdafx (string/vector) + cereal type headers
#include "directory_path.h"
#include "file_path.h"
#include "rar_hash.h"        // rarSha256Hex
#include "rar_mods.h"
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
