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

// RAR data protection: one SHA-256 over the RULE-BEARING part of data_free -- game_config/ and ui/, every
// file, recursively, in a deterministic order, read as raw bytes. images/ and the loose root files are
// deliberately excluded: they are art and platform glue, cannot change what the game DOES, and would make
// the hash differ between builds for no reason.
//
// Identical code runs on the client and on the Linux server, so the two agree byte for byte -- the same
// property the mod hashes rely on. dataFreePath = the data_free folder itself. Returns "" if it is missing.
std::string rarHashDataFree(const std::string& dataFreePath);

// The same content as an unpackable blob, and the inverse. rarHashDataFree is the SHA-256 OF this bundle, so
// what a client downloads is exactly what it verifies. rarUnbundleDataFree overwrites game_config/ and ui/
// wholesale (deleting files the bundle lacks) and leaves the rest of data_free alone; it validates the blob
// BEFORE touching disk and returns false on a bad one, so a failed download cannot leave a broken install.
std::string rarBundleDataFree(const std::string& dataFreePath);
bool rarUnbundleDataFree(const std::string& dataFreePath, const std::string& bundle);
