// RAR real-time PvP -- lockstep transport + command-exchange protocol implementation.
// Self-contained (own socket includes); excluded from nothing special -- it links against ws2_32 (win) /
// libc sockets (posix), both already in the build.

#include "rar_lockstep_net.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <chrono>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef int socklen_t;
  #define RAR_BADSOCK INVALID_SOCKET
  typedef SOCKET rar_socket_t;
  static int rarCloseSock(rar_socket_t s) { return ::closesocket(s); }
  static void rarSockInit() { static bool done = false; if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = true; } }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int rar_socket_t;
  #define RAR_BADSOCK (-1)
  static int rarCloseSock(rar_socket_t s) { return ::close(s); }
  static void rarSockInit() {}
#endif

// ---- framing: 4-byte big-endian length prefix + payload -----------------------------------------------------
namespace {

bool sendAll(rar_socket_t s, const char* p, size_t n) {
  size_t sent = 0;
  while (sent < n) {
    int k = (int) ::send(s, p + sent, (int) (n - sent), 0);
    if (k <= 0) return false;
    sent += k;
  }
  return true;
}

bool recvAll(rar_socket_t s, char* p, size_t n) {
  size_t got = 0;
  while (got < n) {
    int k = (int) ::recv(s, p + got, (int) (n - got), 0);
    if (k <= 0) return false;
    got += k;
  }
  return true;
}

bool sendFrame(rar_socket_t s, const std::string& payload) {
  uint32_t len = (uint32_t) payload.size();
  unsigned char hdr[4] = { (unsigned char)(len >> 24), (unsigned char)(len >> 16),
                           (unsigned char)(len >> 8), (unsigned char)(len) };
  if (!sendAll(s, (const char*) hdr, 4)) return false;
  return payload.empty() ? true : sendAll(s, payload.data(), payload.size());
}

bool recvFrame(rar_socket_t s, std::string& out) {
  unsigned char hdr[4];
  if (!recvAll(s, (char*) hdr, 4)) return false;
  uint32_t len = ((uint32_t) hdr[0] << 24) | ((uint32_t) hdr[1] << 16) | ((uint32_t) hdr[2] << 8) | hdr[3];
  if (len > 64u * 1024u * 1024u) return false; // sanity cap
  out.resize(len);
  if (len == 0) return true;
  return recvAll(s, &out[0], len);
}

// A peer message payload = [4-byte big-endian tick][command bytes]. tick == -1 (0xFFFFFFFF) is the relay's
// READY control message (empty command) sent once both peers are paired.
std::string encodePeerMsg(int32_t tick, const std::string& cmd) {
  std::string p;
  p.resize(4);
  uint32_t t = (uint32_t) tick;
  p[0] = (char)(t >> 24); p[1] = (char)(t >> 16); p[2] = (char)(t >> 8); p[3] = (char)(t);
  p += cmd;
  return p;
}

bool decodePeerMsg(const std::string& p, int32_t& tick, std::string& cmd) {
  if (p.size() < 4) return false;
  uint32_t t = ((uint32_t)(unsigned char) p[0] << 24) | ((uint32_t)(unsigned char) p[1] << 16) |
               ((uint32_t)(unsigned char) p[2] << 8) | (uint32_t)(unsigned char) p[3];
  tick = (int32_t) t;
  cmd = p.substr(4);
  return true;
}

rar_socket_t tcpConnect(const std::string& host, int port, int timeoutMs) {
  rarSockInit();
  struct addrinfo hints; std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  char portStr[16]; std::snprintf(portStr, sizeof(portStr), "%d", port);
  struct addrinfo* res = nullptr;
  if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) return RAR_BADSOCK;
  rar_socket_t s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == RAR_BADSOCK) { freeaddrinfo(res); return RAR_BADSOCK; }
  bool ok = ::connect(s, res->ai_addr, (socklen_t) res->ai_addrlen) == 0;
  freeaddrinfo(res);
  if (!ok) { rarCloseSock(s); return RAR_BADSOCK; }
  int one = 1;
  ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*) &one, sizeof(one)); // low latency: no Nagle
  (void) timeoutMs;
  return s;
}

} // namespace

// ---- relay (box) --------------------------------------------------------------------------------------------
namespace {

// Pipe framed messages from `src` to `dst` until either closes.
void relayPipe(rar_socket_t src, rar_socket_t dst, std::atomic<bool>* alive) {
  std::string frame;
  while (alive->load() && recvFrame(src, frame))
    if (!sendFrame(dst, frame)) break;
  alive->store(false);
}

struct PendingSession {
  rar_socket_t sock[2] = { RAR_BADSOCK, RAR_BADSOCK };
};

} // namespace

void rarLockstepRelay(int port) {
  if (port <= 0) port = RAR_LOCKSTEP_DEFAULT_PORT;
  rarSockInit();
  rar_socket_t listenSock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenSock == RAR_BADSOCK) { std::printf("[relay] socket() failed\n"); return; }
  int one = 1;
  ::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*) &one, sizeof(one));
  struct sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons((unsigned short) port);
  if (::bind(listenSock, (struct sockaddr*) &addr, sizeof(addr)) != 0) {
    std::printf("[relay] bind(%d) failed\n", port); rarCloseSock(listenSock); return;
  }
  ::listen(listenSock, 16);
  std::printf("[relay] lockstep relay listening on port %d\n", port); std::fflush(stdout);

  std::mutex mtx;
  std::map<std::string, PendingSession> pending; // session id -> waiting peer(s)

  while (true) {
    rar_socket_t c = ::accept(listenSock, nullptr, nullptr);
    if (c == RAR_BADSOCK) continue;
    int nod = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*) &nod, sizeof(nod));
    // Handle the handshake + pairing on its own thread so a slow/never-completing peer can't block accept().
    std::thread([c, &mtx, &pending]() {
      std::string hs;
      if (!recvFrame(c, hs)) { rarCloseSock(c); return; }
      // handshake payload = "session\trole"
      auto tab = hs.find('\t');
      if (tab == std::string::npos) { rarCloseSock(c); return; }
      std::string session = hs.substr(0, tab);
      int role = std::atoi(hs.c_str() + tab + 1);
      if (role != 0 && role != 1) { rarCloseSock(c); return; }
      rar_socket_t peerA = RAR_BADSOCK, peerB = RAR_BADSOCK;
      {
        std::lock_guard<std::mutex> lk(mtx);
        auto& ps = pending[session];
        if (ps.sock[role] != RAR_BADSOCK) { rarCloseSock(ps.sock[role]); } // replace a stale dup of this role
        ps.sock[role] = c;
        if (ps.sock[0] != RAR_BADSOCK && ps.sock[1] != RAR_BADSOCK) {
          peerA = ps.sock[0]; peerB = ps.sock[1];
          pending.erase(session);
        }
      }
      if (peerA == RAR_BADSOCK) return; // still waiting for the other peer; the pairing thread will drive it
      std::printf("[relay] session paired, relaying\n"); std::fflush(stdout);
      // Tell both peers the session is READY (tick -1, empty command).
      std::string ready = encodePeerMsg(-1, "");
      sendFrame(peerA, ready); sendFrame(peerB, ready);
      auto alive = std::make_shared<std::atomic<bool>>(true);
      std::thread t1([peerA, peerB, alive]() { relayPipe(peerA, peerB, alive.get()); });
      relayPipe(peerB, peerA, alive.get());
      t1.join();
      rarCloseSock(peerA); rarCloseSock(peerB);
      std::printf("[relay] session ended\n"); std::fflush(stdout);
    }).detach();
  }
}

// ---- client connection --------------------------------------------------------------------------------------
struct LockstepNet::Impl {
  rar_socket_t sock = RAR_BADSOCK;
  std::thread rxThread;
  std::mutex mtx;
  std::condition_variable cv;
  std::map<int32_t, std::string> remote; // tick -> remote peer's command
  std::atomic<bool> ready{false};
  std::atomic<bool> dead{false};

  void rxLoop() {
    std::string frame;
    while (!dead.load() && recvFrame(sock, frame)) {
      int32_t tick; std::string cmd;
      if (!decodePeerMsg(frame, tick, cmd)) break;
      if (tick == -1) { ready.store(true); cv.notify_all(); continue; } // READY control
      std::lock_guard<std::mutex> lk(mtx);
      remote[tick] = std::move(cmd);
      cv.notify_all();
    }
    dead.store(true);
    cv.notify_all();
  }
};

LockstepNet::LockstepNet() : impl(new Impl()) {}
LockstepNet::~LockstepNet() { close(); delete impl; }

bool LockstepNet::connect(const std::string& host, int port, const std::string& session, int role, int timeoutMs) {
  // Reset per-connection state so this object can be REUSED for a later battle. close() leaves dead=true, and
  // ready/remote hold the previous session's values -- without clearing them a second connect() returns with a
  // receive loop that exits instantly, so no orders or positions ever flow (the link looked dead on re-invasion).
  {
    std::lock_guard<std::mutex> lk(impl->mtx);
    impl->remote.clear();
  }
  impl->dead.store(false);
  impl->ready.store(false);
  impl->sock = tcpConnect(host, port, timeoutMs);
  if (impl->sock == RAR_BADSOCK) return false;
  if (!sendFrame(impl->sock, session + "\t" + std::to_string(role))) { close(); return false; }
  impl->rxThread = std::thread([this]() { impl->rxLoop(); });
  // Wait for the relay's READY (both peers joined).
  std::unique_lock<std::mutex> lk(impl->mtx);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (!impl->ready.load() && !impl->dead.load())
    if (impl->cv.wait_until(lk, deadline) == std::cv_status::timeout) break;
  return impl->ready.load();
}

bool LockstepNet::sendTick(int tick, const std::string& cmd) {
  if (impl->sock == RAR_BADSOCK || impl->dead.load()) return false;
  return sendFrame(impl->sock, encodePeerMsg((int32_t) tick, cmd));
}

bool LockstepNet::tryGetRemote(int tick, std::string& out) {
  std::lock_guard<std::mutex> lk(impl->mtx);
  auto it = impl->remote.find((int32_t) tick);
  if (it == impl->remote.end()) return false;
  out = std::move(it->second);
  impl->remote.erase(it);
  return true;
}

bool LockstepNet::waitRemote(int tick, std::string& out, int timeoutMs) {
  std::unique_lock<std::mutex> lk(impl->mtx);
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (true) {
    auto it = impl->remote.find((int32_t) tick);
    if (it != impl->remote.end()) { out = std::move(it->second); impl->remote.erase(it); return true; }
    if (impl->dead.load()) return false;
    if (impl->cv.wait_until(lk, deadline) == std::cv_status::timeout) return false;
  }
}

bool LockstepNet::connected() const { return impl->sock != RAR_BADSOCK && !impl->dead.load(); }

void LockstepNet::close() {
  if (impl->sock != RAR_BADSOCK) {
    impl->dead.store(true);
    rarCloseSock(impl->sock);
    impl->sock = RAR_BADSOCK;
  }
  if (impl->rxThread.joinable()) impl->rxThread.join();
}

// ---- command (de)serialization ------------------------------------------------------------------------------
// Wire form: "<count>;t,x,y,arg,id;t,x,y,arg,id;..."  (compact, delimiter-based; no game types involved)
std::string serializeCommands(const std::vector<LockstepCommand>& cmds) {
  std::string s = std::to_string(cmds.size());
  for (auto& c : cmds) {
    s += ';';
    s += std::to_string(c.type) + "," + std::to_string(c.x) + "," + std::to_string(c.y) + "," +
         std::to_string(c.arg) + "," + std::to_string(c.id);
  }
  return s;
}

std::vector<LockstepCommand> deserializeCommands(const std::string& s) {
  std::vector<LockstepCommand> out;
  if (s.empty()) return out;
  // split on ';'
  std::vector<std::string> parts;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i)
    if (i == s.size() || s[i] == ';') { parts.push_back(s.substr(start, i - start)); start = i + 1; }
  if (parts.empty()) return out;
  for (size_t p = 1; p < parts.size(); ++p) { // parts[0] is the count (advisory)
    const std::string& f = parts[p];
    LockstepCommand c;
    int field = 0; size_t fs = 0;
    for (size_t i = 0; i <= f.size(); ++i)
      if (i == f.size() || f[i] == ',') {
        long long v = std::atoll(f.substr(fs, i - fs).c_str());
        switch (field) { case 0: c.type = (int) v; break; case 1: c.x = (int) v; break;
          case 2: c.y = (int) v; break; case 3: c.arg = (int) v; break; case 4: c.id = v; break; }
        ++field; fs = i + 1;
      }
    out.push_back(c);
  }
  return out;
}

// ---- headless end-to-end self-test --------------------------------------------------------------------------
namespace {

// A tiny DETERMINISTIC stand-in for the game sim: fold each tick's two commands into a running 64-bit hash in a
// FIXED order (role 0's command, then role 1's). If the transport delivers both peers' commands correctly and
// in lockstep, both peers compute the identical final hash.
uint64_t foldCmd(uint64_t h, const std::string& c) {
  for (char ch : c) { h ^= (unsigned char) ch; h *= 1099511628211ULL; }
  h ^= 1469598103934665603ULL; h *= 1099511628211ULL;
  return h;
}

// The command a peer emits for a tick -- deterministic so both peers can predict nothing but still exercise
// real bytes flowing over the wire.
std::string genCmd(int tick, int role) {
  return "r" + std::to_string(role) + "t" + std::to_string(tick) + "#" + std::to_string((tick * 2654435761u) ^ (role * 40503u));
}

int runPeer(int role, int port, int numTicks, int delay, uint64_t& outHash, std::atomic<bool>& outOk) {
  LockstepNet net;
  if (!net.connect("127.0.0.1", port, "nettest", role, 15000)) { outOk.store(false); return 1; }
  // Prime the pipeline: send our commands for ticks 0..delay-1 before executing any tick.
  for (int t = 0; t < delay; ++t)
    if (!net.sendTick(t, genCmd(t, role))) { outOk.store(false); return 1; }
  uint64_t h = 1469598103934665603ULL;
  for (int t = 0; t < numTicks; ++t) {
    // send our command for the tick `delay` ahead
    if (!net.sendTick(t + delay, genCmd(t + delay, role))) { outOk.store(false); return 1; }
    // gather BOTH peers' commands for this tick (mine locally, peer's from the wire) and fold in fixed role order
    std::string mine = genCmd(t, role);
    std::string theirs;
    if (!net.waitRemote(t, theirs, 15000)) { outOk.store(false); return 1; }
    const std::string& c0 = (role == 0) ? mine : theirs;
    const std::string& c1 = (role == 0) ? theirs : mine;
    h = foldCmd(h, c0);
    h = foldCmd(h, c1);
  }
  outHash = h;
  outOk.store(true);
  net.close();
  return 0;
}

} // namespace

// ---- battle session: handshake + seed exchange --------------------------------------------------------------
namespace {

// Control "tick" numbers reserved for the handshake (real sim ticks are >= 0; -1 is the relay READY).
const int CTRL_PARAMS = -2; // host -> guest: session params; guest -> host: ACK (echoes params for verification)

std::string serializeParams(const LockstepSessionParams& p) {
  // "LSP\t<version>\t<seed>\t<delay>\t<gameId>"  (gameId last; it can't contain a tab -- it's account~keeper)
  return "LSP\t" + std::to_string(p.protocolVersion) + "\t" + std::to_string(p.seed) + "\t" +
         std::to_string(p.commandDelay) + "\t" + p.gameId;
}

bool deserializeParams(const std::string& s, LockstepSessionParams& p) {
  if (s.rfind("LSP\t", 0) != 0) return false;
  std::vector<std::string> f;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i)
    if (i == s.size() || s[i] == '\t') { f.push_back(s.substr(start, i - start)); start = i + 1; }
  if (f.size() < 5) return false; // [0]=LSP [1]=ver [2]=seed [3]=delay [4..]=gameId (may itself be empty)
  p.protocolVersion = std::atoi(f[1].c_str());
  p.seed = std::atoi(f[2].c_str());
  p.commandDelay = std::atoi(f[3].c_str());
  p.gameId = f[4];
  return true;
}

int generateSeed() {
  // Unique-enough per battle; the value only needs to be identical on both peers (it is -- the host sends it),
  // not cryptographically strong. Mix wall-clock with a process-run counter.
  static std::atomic<int> counter{0};
  auto now = (long long) std::chrono::high_resolution_clock::now().time_since_epoch().count();
  int s = (int) ((now ^ (now >> 21)) & 0x7fffffff) ^ (++counter * 2654435761u);
  return s == 0 ? 1 : (s & 0x7fffffff);
}

} // namespace

bool LockstepSession::begin(const std::string& host, int port, const std::string& sessionId, int role,
    LockstepSessionParams& params, int timeoutMs) {
  if (role != 0 && role != 1) return false;
  if (!net_.connect(host, port, sessionId, role, timeoutMs)) return false;
  role_ = role;
  if (role == 0) {
    // Host is authoritative: fix the seed, publish params, wait for the guest's ACK.
    if (params.seed == 0) params.seed = generateSeed();
    params_ = params;
    if (!net_.sendTick(CTRL_PARAMS, serializeParams(params_))) return false;
    std::string ack;
    if (!net_.waitRemote(CTRL_PARAMS, ack, timeoutMs)) return false;
    LockstepSessionParams echoed;
    if (!deserializeParams(ack, echoed) || echoed.seed != params_.seed) return false; // guest must agree
  } else {
    // Guest adopts the host's params, then ACKs by echoing them back.
    std::string p;
    if (!net_.waitRemote(CTRL_PARAMS, p, timeoutMs)) return false;
    if (!deserializeParams(p, params_)) return false;
    if (!net_.sendTick(CTRL_PARAMS, serializeParams(params_))) return false;
    params = params_;
  }
  return true;
}

int rarLockstepNetTest() {
  const int port = 39917;
  const int numTicks = 500;
  const int delay = 3;
  std::printf("[nettest] starting relay + 2 peers, %d ticks, delay %d...\n", numTicks, delay); std::fflush(stdout);
  std::thread relay([&]() { rarLockstepRelay(port); });
  relay.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let the relay bind/listen
  uint64_t h0 = 0, h1 = 0;
  std::atomic<bool> ok0{false}, ok1{false};
  std::thread p0([&]() { runPeer(0, port, numTicks, delay, h0, ok0); });
  std::thread p1([&]() { runPeer(1, port, numTicks, delay, h1, ok1); });
  p0.join(); p1.join();
  if (!ok0.load() || !ok1.load()) {
    std::printf("[nettest] FAILED: a peer errored/disconnected (ok0=%d ok1=%d)\n", (int) ok0.load(), (int) ok1.load());
    return 1;
  }
  if (h0 != h1) {
    std::printf("[nettest] FAILED: peers desynced. h0=%llx h1=%llx\n",
        (unsigned long long) h0, (unsigned long long) h1);
    return 1;
  }
  std::printf("[nettest] OK: both peers in lockstep after %d ticks, hash=%llx\n",
      numTicks, (unsigned long long) h0);
  return 0;
}

// ---- session (handshake + seed) self-test -------------------------------------------------------------------
namespace {

struct SessionPeerResult {
  int seed = 0;
  std::string gameId;
  uint64_t hash = 0;
  std::atomic<bool> ok{false};
};

void runSessionPeer(int role, int port, SessionPeerResult& res) {
  LockstepSession session;
  LockstepSessionParams params;
  if (role == 0) { // host fills the authoritative params
    params.gameId = "necro~Moriaty";
    params.commandDelay = 3;
    params.seed = 0; // auto-generate
  }
  if (!session.begin("127.0.0.1", port, "battletest", role, params, 15000)) { res.ok.store(false); return; }
  res.seed = session.params().seed;
  res.gameId = session.params().gameId;
  // Run a short lockstep exchange whose per-tick command is derived from the AGREED SEED. If the seed exchange
  // worked, both peers generate matching command streams and fold to the same hash; if it didn't, they diverge.
  const int numTicks = 200;
  const int delay = session.params().commandDelay;
  auto& net = session.net();
  auto cmdFor = [&](int tick, int r) {
    uint32_t x = (uint32_t) session.params().seed ^ (tick * 2654435761u) ^ (r * 40503u);
    return std::to_string(x);
  };
  for (int t = 0; t < delay; ++t)
    if (!net.sendTick(t, cmdFor(t, role))) { res.ok.store(false); return; }
  uint64_t h = 1469598103934665603ULL;
  for (int t = 0; t < numTicks; ++t) {
    if (!net.sendTick(t + delay, cmdFor(t + delay, role))) { res.ok.store(false); return; }
    std::string mine = cmdFor(t, role), theirs;
    if (!net.waitRemote(t, theirs, 15000)) { res.ok.store(false); return; }
    const std::string& c0 = (role == 0) ? mine : theirs;
    const std::string& c1 = (role == 0) ? theirs : mine;
    h = foldCmd(h, c0);
    h = foldCmd(h, c1);
  }
  h ^= (uint64_t)(uint32_t) session.params().seed; // fold the seed so the hash reflects seed agreement
  res.hash = h;
  res.ok.store(true);
  session.net().close();
}

} // namespace

int rarLockstepSessionTest() {
  const int port = 39918;
  std::printf("[sessiontest] starting relay + 2 peers doing handshake + seed exchange...\n"); std::fflush(stdout);
  std::thread relay([&]() { rarLockstepRelay(port); });
  relay.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  SessionPeerResult r0, r1;
  std::thread p0([&]() { runSessionPeer(0, port, r0); });
  std::thread p1([&]() { runSessionPeer(1, port, r1); });
  p0.join(); p1.join();
  if (!r0.ok.load() || !r1.ok.load()) {
    std::printf("[sessiontest] FAILED: a peer errored (ok0=%d ok1=%d)\n", (int) r0.ok.load(), (int) r1.ok.load());
    return 1;
  }
  std::printf("[sessiontest] host seed=%d gameId='%s' | guest seed=%d gameId='%s'\n",
      r0.seed, r0.gameId.c_str(), r1.seed, r1.gameId.c_str());
  if (r0.seed != r1.seed || r0.seed == 0) {
    std::printf("[sessiontest] FAILED: seeds not agreed (host=%d guest=%d)\n", r0.seed, r1.seed);
    return 1;
  }
  if (r0.gameId != r1.gameId) {
    std::printf("[sessiontest] FAILED: gameId not agreed\n");
    return 1;
  }
  if (r0.hash != r1.hash) {
    std::printf("[sessiontest] FAILED: seed-seeded lockstep desynced (h0=%llx h1=%llx)\n",
        (unsigned long long) r0.hash, (unsigned long long) r1.hash);
    return 1;
  }
  std::printf("[sessiontest] OK: handshake agreed on seed=%d, and a seed-driven lockstep exchange stayed in sync.\n",
      r0.seed);
  return 0;
}
