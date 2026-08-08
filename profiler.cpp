#include "profiler.h"

#ifdef SIMPLE_PROFILER

#include <map>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstring>

thread_local SimpleProfScope* SimpleProfScope::current = nullptr;

namespace {
std::mutex& slotsMutex() { static std::mutex m; return m; }
std::vector<SimpleProfSlot*>& allSlots() { static std::vector<SimpleProfSlot*> v; return v; }
}

SimpleProfSlot* simpleProfGetSlot(const char* name) {
  std::lock_guard<std::mutex> lock(slotsMutex());
  // de-dupe by name so the same function across call sites merges
  static std::map<std::string, SimpleProfSlot*> byName;
  auto it = byName.find(name);
  if (it != byName.end())
    return it->second;
  auto* slot = new SimpleProfSlot();
  slot->name = name;
  byName[name] = slot;
  allSlots().push_back(slot);
  return slot;
}

static void simpleProfDump(const char* path) {
  std::vector<SimpleProfSlot*> slots;
  {
    std::lock_guard<std::mutex> lock(slotsMutex());
    slots = allSlots();
  }
  std::sort(slots.begin(), slots.end(), [](SimpleProfSlot* a, SimpleProfSlot* b) {
    return a->selfNanos.load() > b->selfNanos.load();
  });
  FILE* f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "%-70s %12s %12s %14s %12s\n", "function", "self_ms", "incl_ms", "calls", "self_us/call");
  fprintf(f, "%s\n", std::string(122, '-').c_str());
  for (auto* s : slots) {
    long long self = s->selfNanos.load();
    long long incl = s->inclusiveNanos.load();
    long long calls = s->calls.load();
    if (calls == 0)
      continue;
    // trim the verbose __PRETTY_FUNCTION__ to something readable
    std::string name = s->name;
    auto paren = name.find('(');
    if (paren != std::string::npos)
      name = name.substr(0, paren);
    if (name.size() > 69)
      name = name.substr(name.size() - 69);
    fprintf(f, "%-70s %12.1f %12.1f %14lld %12.3f\n",
        name.c_str(), self / 1e6, incl / 1e6, calls,
        calls ? (self / 1e3) / (double)calls : 0.0);
  }
  fclose(f);
}

void simpleProfStartDumper() {
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      simpleProfDump("profile_report.txt");
    }
  }).detach();
}

#endif

#ifdef SAMPLE_PROFILER

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <cstdio>

namespace {

HANDLE g_target = nullptr;        // the game-loop thread being sampled
DWORD64 g_exeBase = 0;            // load base of keeper.exe (to identify game frames)
struct Counts { long long self = 0; long long incl = 0; };
std::map<DWORD64, Counts> g_byAddr;   // only touched by the sampler thread
std::vector<std::vector<DWORD64>> g_stacks;  // per-sample stacks (for attribution)
long long g_sampleCount = 0;
bool g_cleared = false;

void sampleOnce() {
  if (SuspendThread(g_target) == (DWORD) -1)
    return;
  CONTEXT ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.ContextFlags = CONTEXT_FULL;
  DWORD64 stack[64];
  int n = 0;
  HANDLE proc = GetCurrentProcess();
  if (GetThreadContext(g_target, &ctx)) {
    STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = ctx.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (; n < 64; ++n) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, g_target, &frame, &ctx,
              nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
        break;
      if (frame.AddrPC.Offset == 0)
        break;
      stack[n] = frame.AddrPC.Offset;
    }
  }
  ResumeThread(g_target);
  // Drop the load phase once: clear accumulated data after ~12s of warmup so the
  // save-loading splash / tile loading doesn't pollute the steady-state profile.
  if (!g_cleared && ++g_sampleCount > 12000) {
    g_byAddr.clear();
    g_stacks.clear();
    g_cleared = true;
  }
  if (n > 0)
    g_byAddr[stack[0]].self++;
  for (int i = 0; i < n; ++i) {
    bool seen = false;
    for (int j = 0; j < i; ++j)
      if (stack[j] == stack[i]) { seen = true; break; }
    if (!seen)
      g_byAddr[stack[i]].incl++;
  }
  if (g_stacks.size() < 400000)
    g_stacks.emplace_back(stack, stack + n);
}

std::string resolve(DWORD64 addr) {
  char buf[sizeof(SYMBOL_INFO) + 512];
  auto* sym = (SYMBOL_INFO*) buf;
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;
  DWORD64 disp = 0;
  if (SymFromAddr(GetCurrentProcess(), addr, &disp, sym))
    return sym->Name;
  return "???";
}

void dumpReport(const char* path) {
  HANDLE proc = GetCurrentProcess();
  std::map<DWORD64, std::string> nameCache;
  std::map<DWORD64, char> exeCache; // 1 = in keeper.exe
  auto rname = [&](DWORD64 a) -> const std::string& {
    auto it = nameCache.find(a);
    if (it != nameCache.end())
      return it->second;
    return nameCache[a] = resolve(a);
  };
  auto inExe = [&](DWORD64 a) -> bool {
    auto it = exeCache.find(a);
    if (it != exeCache.end())
      return it->second;
    return (exeCache[a] = (SymGetModuleBase64(proc, a) == g_exeBase)) != 0;
  };
  auto isStd = [](const std::string& n) {
    return n.rfind("std::", 0) == 0 || n.rfind("stx::", 0) == 0 ||
           n.rfind("_", 0) == 0 || n.rfind("void ", 0) == 0;
  };
  // Attribute each sample to the innermost keeper.exe frame that is real game
  // logic (not a std/stx container template compiled into the exe).
  std::map<std::string, long long> gameLogic;
  long long total = g_stacks.size();
  for (auto& st : g_stacks)
    for (auto a : st) {
      if (!inExe(a))
        continue;
      const std::string& nm = rname(a);
      if (isStd(nm))
        continue;
      gameLogic[nm]++;
      break;
    }
  if (total == 0)
    return;
  FILE* f = fopen(path, "w");
  if (!f)
    return;
  std::vector<std::pair<std::string, long long>> g(gameLogic.begin(), gameLogic.end());
  std::sort(g.begin(), g.end(), [](auto& a, auto& b) { return a.second > b.second; });
  fprintf(f, "=== Cost by GAME-LOGIC function (allocation/std/syscall charged to it)  samples=%lld ===\n", total);
  fprintf(f, "%-66s %10s %8s\n", "function", "pct", "n");
  fprintf(f, "%s\n", std::string(88, '-').c_str());
  for (auto& e : g) {
    double pct = 100.0 * e.second / total;
    if (pct < 0.3)
      break;
    std::string name = e.first;
    if (name.size() > 65) name = name.substr(0, 65);
    fprintf(f, "%-66s %10.2f %8lld\n", name.c_str(), pct, e.second);
  }
  // Raw leaf self, for reference.
  std::map<std::string, Counts> byName;
  for (auto& e : g_byAddr) {
    auto& c = byName[rname(e.first)];
    c.self += e.second.self;
    c.incl += e.second.incl;
  }
  std::vector<std::pair<std::string, Counts>> v(byName.begin(), byName.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.self > b.second.self; });
  fprintf(f, "\n=== Raw self (leaf actually executing) ===\n");
  fprintf(f, "%-66s %8s %8s\n", "function", "self%", "incl%");
  fprintf(f, "%s\n", std::string(84, '-').c_str());
  for (auto& e : v) {
    double selfPct = 100.0 * e.second.self / total;
    double inclPct = 100.0 * e.second.incl / total;
    if (selfPct < 0.4 && inclPct < 1.5)
      continue;
    std::string name = e.first;
    if (name.size() > 65) name = name.substr(0, 65);
    fprintf(f, "%-66s %8.2f %8.2f\n", name.c_str(), selfPct, inclPct);
  }
  fclose(f);
}

} // namespace

void sampleProfStart() {
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
      &g_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
  g_exeBase = (DWORD64) GetModuleHandle(nullptr);
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  std::thread([] {
    int n = 0;
    for (;;) {
      Sleep(1); // ~1000 samples/sec
      sampleOnce();
      if (++n % 4000 == 0)
        dumpReport("profile_report.txt");
    }
  }).detach();
}

#endif
