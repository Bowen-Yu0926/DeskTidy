#pragma once

#include "desktop_items.h"
#include "pin_store.h"

#include <string>
#include <vector>

// Python: ensure_hosted_system_namespace_public_icons — create .lnk for
// This PC / Recycle Bin, pin into system_common, hide Explorer copies.
int EnsureHostedSystemNamespaceIcons(PinMap* pins);

// Directory holding DeskTidy-created namespace .lnk files.
std::wstring PublicSystemDir();

// Append existing namespace shortcuts as DesktopItems (for scans / public host).
void AppendPublicSystemItems(std::vector<DesktopItem>* out);

// Restore hosted system namespace icons (This PC / Recycle Bin) to their
// original visibility recorded before hiding. Idempotent; no-op when the state
// file is absent; deletes the state file on success. Call from normal exit,
// the "hide desktop icons off" toggle, and --uninstall-cleanup.
void RestoreDesktopIconsIfHidden();

// P1-11: read DeskTidyNS:{CLSID} from a hosted .lnk description (empty = none).
std::wstring GetLnkNamespaceClsid(const std::wstring& lnk_path);
// Show the Explorer namespace icon again after the hosted .lnk is deleted.
void ReturnNamespaceIconToDesktop(const std::wstring& clsid);

// Re-apply hidden state after an Explorer rebuild (only when currently hosted).
void ReapplyNamespaceIconHiddenState();

// Startup reconcile for crash recovery: if a state file survived a crash but
// this run should NOT hide, restore immediately; otherwise keep hidden.
void ReconcileNamespaceIconsOnStartup(bool should_hide);

// Explorer-rebuild guard: lazy background poll (<=2s) that re-hides hosted
// icons after the desktop shell is restarted. Started automatically on first
// hide; runs for the process lifetime (cheap FindWindow poll).
void StartNamespaceIconRebuildGuard();
void StopNamespaceIconRebuildGuard();
