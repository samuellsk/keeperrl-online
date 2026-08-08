#ifdef WINDOWS

#include <windows.h>
#include <dbghelp.h>
#include <direct.h>
#include <ctime>
#include <cstdio>
#include "version.h"
#include "shellscalingapi.h"

// RAR crash reports live in their own folder, one TIMESTAMPED pair (.txt stack + .dmp minidump) per crash --
// the old fixed "KeeperRL.dmp"/"rar_crash.txt" in the game root overwrote itself, so only the last crash ever
// survived. A later run finds anything left here, compresses it and uploads it (see rarUploadPendingCrashes);
// files are only deleted once the server has them, so a crash during the upload just retries next launch.
// Deliberately kept dumb: this runs inside an unhandled-exception filter, where the heap may already be
// corrupt, so we do nothing here but name + write files. No allocation-heavy work, no network.
static const char* CRASH_DIR = "crashes";

// Fills buf with "crashes/crash_YYYYMMDD_HHMMSS" -- both artifacts of one crash share the stem so they pair up.
static void crashStem(char* buf, size_t n) {
  _mkdir(CRASH_DIR);
  std::time_t t = std::time(nullptr);
  std::tm* lt = std::localtime(&t);
  if (lt)
    snprintf(buf, n, "%s/crash_%04d%02d%02d_%02d%02d%02d", CRASH_DIR, lt->tm_year + 1900, lt->tm_mon + 1,
        lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec);
  else
    snprintf(buf, n, "%s/crash_unknown", CRASH_DIR);
}

// RAR lockstep diagnostic: capture the current call stack (skipping this helper + its immediate caller) and
// symbolize a single runtime address to "func (file:line)". Reuses dbghelp like the crash writer above.
int rarCaptureStack(void** frames, int maxFrames) {
  return (int) CaptureStackBackTrace(2, (ULONG) maxFrames, frames, nullptr);
}

static bool g_rarSymInit = false;
void rarSymbolize(void* addr, char* out, unsigned long n) {
  if (!out || n == 0) return;
  out[0] = 0;
  if (!addr) { snprintf(out, n, "(null)"); return; }
  HANDLE proc = GetCurrentProcess();
  if (!g_rarSymInit) {
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    g_rarSymInit = true;
  }
  char buf[sizeof(SYMBOL_INFO) + 512] = {};
  SYMBOL_INFO* sym = (SYMBOL_INFO*) buf;
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, (DWORD64)(uintptr_t) addr, &disp, sym)) {
    IMAGEHLP_LINE64 line = {}; line.SizeOfStruct = sizeof(line); DWORD ld = 0;
    if (SymGetLineFromAddr64(proc, (DWORD64)(uintptr_t) addr, &ld, &line))
      snprintf(out, n, "%s (%s:%lu)", sym->Name, line.FileName, line.LineNumber);
    else
      snprintf(out, n, "%s +0x%llx", sym->Name, (unsigned long long) disp);
  } else
    snprintf(out, n, "0x%llx", (unsigned long long)(uintptr_t) addr);
}

int printStacktraceWithGdb() {
  char gdbcmd[512] = {0};
  sprintf(gdbcmd, "rungdb.bat \"" BUILD_VERSION " " BUILD_DATE "\"");
  fputs(gdbcmd, stderr);
  fflush(stderr);
  return system(gdbcmd);
}

// RAR: write a symbolized stack trace next to the dump (needs a PDB: build with -gcodeview + --pdb).
static void writeSymbolizedStack(EXCEPTION_POINTERS* ep, const char* stem) {
  if (!ep || !ep->ContextRecord)
    return;
  char path[512];
  snprintf(path, sizeof(path), "%s.txt", stem);
  FILE* f = fopen(path, "w");
  if (!f)
    return;
  HANDLE proc = GetCurrentProcess();
  HANDLE thread = GetCurrentThread();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
  SymInitialize(proc, nullptr, TRUE);
  fprintf(f, "Exception 0x%lx at 0x%llx\n", ep->ExceptionRecord->ExceptionCode,
      (unsigned long long) ep->ExceptionRecord->ExceptionAddress);
  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 frame = {};
  frame.AddrPC.Offset = ctx.Rip; frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp; frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 80; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr,
        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
      break;
    char buf[sizeof(SYMBOL_INFO) + 512] = {};
    SYMBOL_INFO* sym = (SYMBOL_INFO*) buf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 511;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
      IMAGEHLP_LINE64 line = {}; line.SizeOfStruct = sizeof(line); DWORD ld = 0;
      if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &ld, &line))
        fprintf(f, "  #%d %s  (%s:%lu)\n", i, sym->Name, line.FileName, line.LineNumber);
      else
        fprintf(f, "  #%d %s +0x%llx\n", i, sym->Name, (unsigned long long) disp);
    } else
      fprintf(f, "  #%d 0x%llx\n", i, (unsigned long long) frame.AddrPC.Offset);
    fflush(f);
  }
  fclose(f);
}

void miniDumpFunction(unsigned int nExceptionCode, EXCEPTION_POINTERS *pException) {
  char stem[512];
  crashStem(stem, sizeof(stem));
  writeSymbolizedStack(pException, stem);
  char dumpPath[512];
  snprintf(dumpPath, sizeof(dumpPath), "%s.dmp", stem);
  HANDLE hFile = CreateFileA(dumpPath, GENERIC_READ | GENERIC_WRITE,
    0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if ((hFile != NULL) && (hFile != INVALID_HANDLE_VALUE)) {
    MINIDUMP_EXCEPTION_INFORMATION mdei;
    mdei.ThreadId = GetCurrentThreadId();
    mdei.ExceptionPointers = pException;
    mdei.ClientPointers = FALSE;
    MINIDUMP_TYPE mdt = (MINIDUMP_TYPE)(
      MiniDumpWithDataSegs |
      MiniDumpWithHandleData |
      MiniDumpWithIndirectlyReferencedMemory |
      MiniDumpWithThreadInfo |
      MiniDumpWithUnloadedModules);
    BOOL rv = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
      hFile, mdt, (pException != nullptr) ? &mdei : nullptr, nullptr, nullptr);
    CloseHandle(hFile);
    printStacktraceWithGdb();
  }
}

LONG WINAPI miniDumpFunction2(EXCEPTION_POINTERS *ExceptionInfo) {
  miniDumpFunction(123, ExceptionInfo);
  return EXCEPTION_EXECUTE_HANDLER;
}


void initializeMiniDump() {
  SetUnhandledExceptionFilter(miniDumpFunction2);
}
//#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004

void attachConsole() {
  if(AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()){
      freopen("CONOUT$", "w", stdout);
      freopen("CONOUT$", "w", stderr);
      HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
      if (handle != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
          if (GetConsoleMode(handle, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(handle, mode);
          }
      }
  }
}
void setConsoleColor(int col) {
  auto handle = GetStdHandle(STD_OUTPUT_HANDLE);
  FlushConsoleInputBuffer(handle);
  SetConsoleTextAttribute(handle, col);
}

void dpiAwareness() {
  SetProcessDPIAware();
}

extern "C"
{
    __declspec( dllexport ) unsigned int NvOptimusEnablement                = 0x00000001;
    __declspec( dllexport ) int AmdPowerXpressRequestHighPerformance = 1;
}

#else
int rarCaptureStack(void**, int) { return 0; }
void rarSymbolize(void*, char* out, unsigned long n) { if (out && n) out[0] = 0; }
void attachConsole() {
}
void initializeMiniDump() {
}
void setConsoleColor(int) {
}
void dpiAwareness() {
}

#endif
