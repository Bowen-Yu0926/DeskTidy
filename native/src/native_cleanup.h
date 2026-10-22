#pragma once

#include <Windows.h>

// Unregister shell verbs, disable autostart; optional wipe of Native data dir.
void RunNativeCleanup(HWND owner);
// Headless uninstall cleanup invoked by the installer via --uninstall-cleanup.
// Stops other instances, restores desktop/namespace icons, removes autostart +
// shell verbs + Run values, and purges config/cache/logs (preserves user
// 笔记 / 录屏 / 纪要 document dirs). Never shows UI. Returns 0 on completion.
int RunUninstallCleanup();
