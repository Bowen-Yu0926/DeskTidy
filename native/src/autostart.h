#pragma once

#include <Windows.h>

#include <string>

bool IsNativeAutostartEnabled();
bool SetNativeAutostart(bool enable);
std::wstring NativeExePath();
// P2-6: one-shot — drop Python Startup .lnk / StartupApproved leftovers and
// ensure the HKCU Run value points at this native exe when autostart is on.
void MigratePythonAutostartOnce();
