// Launch keeper_updater and wait for its verdict.
//
// This lives in its own translation unit ON PURPOSE: <windows.h> defines a Rectangle (from wingdi.h) that
// collides with the engine's own Rectangle class, so including it anywhere near the game headers breaks the
// build. Here it is included alone, and main.cpp only sees the one-line declaration in rar_integrity.h.
//
// Nothing here is ever fatal. A missing updater, a failed spawn, any error at all - the caller carries on
// into the game. An integrity check that can stop somebody playing is worse than no integrity check.

#include "rar_integrity.h"

#ifdef WINDOWS
#include <windows.h>
#else
#include <cstdlib>
#include <sys/wait.h>
#endif

int rarRunIntegrityCheck() {
#ifdef WINDOWS
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  ZeroMemory(&si, sizeof(si));
  si.cb = sizeof(si);
  ZeroMemory(&pi, sizeof(pi));
  // CREATE_NO_WINDOW: the updater is a console program and we do not want a black box flashing up over the
  // game's window while it works. Its output still reaches a console when the game is run with --console.
  char cmd[] = "keeper_updater.exe --check-only";
  if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    return -1;   // no updater next to us, or it could not start
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int) code;
#else
  int rc = std::system("./keeper_updater --check-only");
  return rc == -1 ? -1 : WEXITSTATUS(rc);
#endif
}
