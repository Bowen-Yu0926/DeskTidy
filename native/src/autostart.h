#pragma once

#include <Windows.h>

#include <string>

bool IsNativeAutostartEnabled();
bool SetNativeAutostart(bool enable);
std::wstring NativeExePath();
