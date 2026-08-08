#pragma once
// RAR real-time PvP -- lockstep transport + command-exchange protocol. Kept free of game headers (like
// rar_client.h) so it's cheap to include and can't drag winsock/windows.h into the unity build.
//
// Model: a battle SESSION has exactly two peers (defender = role 0, invader = role 1). Both run the full
// DETERMINISTIC simulation (see the lockstep determinism work) and exchange ONLY per-tick commands. A relay on
// the box pairs the two peers by session id and forwards framed messages between them (a dumb pipe -- it never
// interprets commands, so it needs none of the game). Determinism + identical command streams => identical
// simulations on both machines, i.e. true real-time PvP with input-delay latency instead of a netcode rewrite.

#include <string>
#include <vector>

// Box side: run the relay. Pairs two clients per session id and forwards framed messages. Blocks forever.
// port <= 0 => RAR_LOCKSTEP_DEFAULT_PORT.
void rarLockstepRelay(int port);
static const int RAR_LOCKSTEP_DEFAULT_PORT = 38553;

// Client side: one connection to the relay for one battle session.
class LockstepNet {
 public:
  LockstepNet();
  ~LockstepNet();
  LockstepNet(const LockstepNet&) = delete;
  LockstepNet& operator=(const LockstepNet&) = delete;

  // Connect to the relay at host:port, join `session` as `role` (0 or 1), and block until the OTHER peer has
  // also joined (the relay sends a READY once both are present) or timeoutMs elapses. false on any failure.
  bool connect(const std::string& host, int port, const std::string& session, int role, int timeoutMs = 30000);

  // Send this peer's command bytes for simulation tick `tick`. Empty cmd is fine (means "no input") and MUST
  // still be sent every tick so the peer can advance. false on transport error.
  bool sendTick(int tick, const std::string& cmd);

  // Non-blocking: if the remote peer's command for `tick` has arrived, move it into `out` and return true.
  bool tryGetRemote(int tick, std::string& out);

  // Block up to timeoutMs for the remote peer's command for `tick`. false = timeout or disconnect (=> the peer
  // dropped; the caller should end the battle / fall back to AI).
  bool waitRemote(int tick, std::string& out, int timeoutMs = 15000);

  bool connected() const;
  void close();

 private:
  struct Impl;
  Impl* impl;
};

// Headless end-to-end self-test (--rar_lockstep_nettest): starts a local relay + two peers that exchange a
// deterministic command stream through it and verifies both stay in lockstep. Returns 0 on success, nonzero on
// failure. Proves the transport + protocol independently of the game.
int rarLockstepNetTest();

// ---- battle session: handshake + seed exchange (layer above LockstepNet) -----------------------------------
// The parameters both peers of a battle must AGREE ON before stepping. The seed is the linchpin: lockstep only
// works if both machines Random.init(seed) identically. The DEFENDER (host, role 0) is authoritative -- it
// picks the seed and the config; the INVADER (guest, role 1) adopts them. gameId names the defender's dungeon
// blob both load as the shared starting state.
struct LockstepSessionParams {
  int protocolVersion = 1;
  int seed = 0;         // shared RNG seed; 0 on the host's input => auto-generated in begin()
  int commandDelay = 3; // ticks of input latency (the lockstep buffer depth)
  std::string gameId;   // the defender's dungeon identity (the base both sides load)
};

// Establishes a battle session over the relay: connects, then does the param+seed handshake so BOTH peers end
// holding identical LockstepSessionParams. After begin() returns true, drive the fight with net().sendTick /
// net().waitRemote using params().commandDelay and seeding the sim with params().seed.
class LockstepSession {
 public:
  // role 0 = defender/host (fills `params` on input: seed optional, gameId/delay set), role 1 = invader/guest
  // (`params` is output). host & session id are how both find each other on the relay (assigned by the server).
  bool begin(const std::string& host, int port, const std::string& sessionId, int role,
      LockstepSessionParams& params, int timeoutMs = 30000);
  LockstepNet& net() { return net_; }
  int role() const { return role_; }
  const LockstepSessionParams& params() const { return params_; }
 private:
  LockstepNet net_;
  int role_ = -1;
  LockstepSessionParams params_;
};

// Headless self-test (--rar_lockstep_sessiontest): local relay + two peers do the handshake and verify they end
// with identical params/seed, then run a short lockstep exchange seeded by it. Returns 0 on success.
int rarLockstepSessionTest();

// ---- command protocol -------------------------------------------------------------------------------------
// A single sim-affecting player action, serialized as the tick command. Deliberately a flat, game-header-free
// struct (ints only) so the transport module stays independent of the game; the game side (main_loop.cpp)
// translates real UserInputs into these on capture and applies them via PlayerControl/Collective on receipt.
// Extensible: add a type + fields as more actions are wired. Live PvP is OVERSEER control -- these are
// keeper-level actions (place a guard banner, designate, assign, retreat, z-level), NOT creature puppeting.
enum LockstepCmdType {
  LSC_NONE = 0,
  LSC_SET_ZONE = 1,      // arg = ZoneId, (x,y) = position on the ground level  (guard banner = GUARD1..3)
  LSC_ERASE_ZONE = 2,    // arg = ZoneId, (x,y) = position
  LSC_MINION_ACTIVITY = 3, // id = creature, arg = MinionActivity
  LSC_RETREAT = 4,       // invader leaves the battle (no fields)
  LSC_ZLEVEL = 5,        // arg = direction (+1 down / -1 up)
  LSC_CREATURE_GOTO = 6, // id = creature, (x,y) = destination -- banner-style move order (goToAndWait)
  // ... build/designate/workshop/etc. added as they're wired ...
};
struct LockstepCommand {
  int type = LSC_NONE;
  int x = 0, y = 0;
  int arg = 0;
  long long id = 0;
};

// A tick carries zero or more commands (a player may act several times in one tick). Serialize the whole batch
// to the wire string and back.
std::string serializeCommands(const std::vector<LockstepCommand>&);
std::vector<LockstepCommand> deserializeCommands(const std::string&);
