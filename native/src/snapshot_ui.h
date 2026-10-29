#pragma once

#include "layout.h"

#include <Windows.h>

// List / apply / delete snapshots. Returns true if a snapshot was applied
// (caller should reload fences from *applied, then RestoreSnapshotShellIcons
// on *applied_path when provided).
bool ManageSnapshotsDialog(HWND owner, AppState* applied,
                           std::wstring* applied_path = nullptr);
