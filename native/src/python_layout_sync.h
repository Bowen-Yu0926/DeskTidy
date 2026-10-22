#pragma once

// One-shot migration from the legacy Python app (%USERPROFILE%\.desktidy).
// After success (or if Python data is absent), native layout/pins are the only
// source of truth — nothing re-reads Python on later launches.
// Returns true when layout.json / pins.json were written this call.
bool MigratePythonLayoutOnce();
