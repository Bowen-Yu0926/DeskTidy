#include "instance_lock.h"

namespace {
HANDLE g_mutex = nullptr;
// P2-8: same mutex as Python (`Local\DeskTidyMainInstance`) so the two
// builds cannot both stay running. Name lives in instance_lock.h so probes
// cannot drift from the owner.
}  // namespace

bool AcquireNativeInstanceLock() {
  g_mutex = CreateMutexW(nullptr, TRUE, kNativeInstanceMutexName);
  if (!g_mutex) {
    return false;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(g_mutex);
    g_mutex = nullptr;
    return false;
  }
  return true;
}

void ReleaseNativeInstanceLock() {
  if (g_mutex) {
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    g_mutex = nullptr;
  }
}
