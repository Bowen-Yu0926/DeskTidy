#pragma once

#include "layout.h"

#include <Windows.h>

// Modal canvas: drag fences, rename, set portal. Returns true when applied.
bool ShowLayoutEditor(HWND owner, AppState* state);
