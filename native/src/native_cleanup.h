#pragma once

#include <Windows.h>

// Unregister shell verbs, disable autostart; optional wipe of Native data dir.
void RunNativeCleanup(HWND owner);
