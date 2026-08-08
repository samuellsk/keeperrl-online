#pragma once
// Entry point for KeeperRL's integrated online ("RAR") server mode.
// Launched via `keeper.exe --rar_server <port>` (see main.cpp). Runs the HTTP
// backend (accounts + save-hash validation) and does not start the game itself.
#include <functional>
#include <string>
#include <vector>

// Port used by `--rar_server` when no port is given on the command line. Keep in sync with the port in
// appconfig.txt's "server_url" -- that is what clients dial.
constexpr int RAR_DEFAULT_PORT = 38552;

// A content-backed villain-map generator supplied by the "full" server (MainLoop::runRarServerFull builds it
// from the game's own ContentFactory). Signature: (tier, enemyId, biome) -> lzma-compressed villain blob, or
// "" on failure. When provided, the server LIVE-replenishes its own respawn pool in the background instead of
// relying on a fixed pool baked at world-gen time. When empty (legacy content-free launch), the server runs
// with just whatever spares are already on disk.
using RarVillainGen =
    std::function<std::string(const std::string& tier, const std::string& enemyId, const std::string& biome)>;

// One (tier, enemyId, biome) the WORLD actually uses -- i.e. a kind of villain the respawn pool must be able
// to hand out. Derived from the world blob by the full server, because the server itself never deserializes
// rar_campaign.dat (it only serves it verbatim) and so cannot know which biome a faction lives on.
// This is the authoritative combo list: the pool no longer has to be baked at world-gen, so ADDING A TIER
// (e.g. ALLY) or deepening POOL_* takes effect on the next server start, with no world regen.
struct RarVillainCombo { std::string tier, enemyId, biome; };

void runRarServer(int port, RarVillainGen gen = {}, std::vector<RarVillainCombo> combos = {});
