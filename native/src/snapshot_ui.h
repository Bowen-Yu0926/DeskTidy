#pragma once

#include "layout.h"

#include <Windows.h>

// List / apply / delete snapshots. Returns true if a snapshot was applied
// (caller should reload fences from *applied).
bool ManageSnapshotsDialog(HWND owner, AppState* applied);
