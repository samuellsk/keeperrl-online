#pragma once
#include <string>
#include <vector>

// RAR: content-free mod bundling shared by the game client (MainLoop::bundleMod) and the headless RAR
// server, so the bundle bytes -- and therefore the SHA-256 the client hash-checks -- are IDENTICAL on both
// sides. Kept out of main_loop.cpp so rar_server.cpp can publish mods without pulling in game content.

// Serialize a mod folder (all files, recursively, deterministic order) into the cereal blob that
// MainLoop::unbundleMod unpacks on the client. modDirPath = filesystem path to the mod's own folder.
std::string rarBundleModDir(const std::string& modDirPath);

// The mods to activate, IN LOAD ORDER. Order is read from "<modsDirPath>/load_order.txt": one mod name per
// line, '#' comments and blanks ignored. Listed mods load first in that order; any remaining folder mods are
// appended, so dropping in a new mod still works without editing the file. A listed mod that isn't installed
// is skipped. No file => plain folder order (previous behaviour).
// WHY it matters: mods build on each other -- e.g. one adds a biome and a later one adds villains for that
// biome -- and the dependency has to be merged first. Note ContentFactory::merge only ADDS missing keys and
// never overwrites, so for any duplicate key the EARLIER mod in this order wins.
// The server publishes its manifest in this order and the client mirrors the server's order exactly
// (syncServerMods), so client and server always agree -- the file only needs to exist server-side.
std::vector<std::string> rarModsInLoadOrder(const std::string& modsDirPath);

// Scan every immediate subfolder of modsDirPath, bundle each, and (re)write:
//   outDirPath/rar_mods.txt        -- lines "<mod>\t<sha256>"
//   outDirPath/rar_mods/<mod>.dat  -- the bundle
// Content-free: the --rar_server calls this at startup to auto-publish whatever mods sit in the standard
// mods/ folder, so no separate --rar_gen_world step is needed just to push a mod. Returns the mod count.
int rarPublishMods(const std::string& modsDirPath, const std::string& outDirPath);
