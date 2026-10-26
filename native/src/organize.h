#pragma once

#include "desktop_items.h"
#include "layout.h"
#include "pin_store.h"
#include "rule_store.h"

#include <string>
#include <unordered_map>
#include <vector>

// Virtual organize: prefer fence organize_kinds (P1-4), then legacy
// ResolveFenceId. Returns how many pins were written.
int OrganizeApplyDefaultPins(PinMap* pins, const std::vector<DesktopItem>& items,
                             const RuleMap& rules,
                             const std::string& software_id,
                             const std::string& common_id,
                             const std::vector<FenceLayout>* fences = nullptr,
                             int current_page = -1, bool reassign = false);

// Lines "name → fence title" for items that would get a default pin.
std::vector<std::wstring> OrganizePreviewVirtualPins(
    const PinMap& pins, const std::vector<DesktopItem>& items,
    const RuleMap& rules,
    const std::unordered_map<std::string, std::wstring>& fence_titles,
    const std::string& software_id = "system_software",
    const std::string& common_id = "system_common",
    const std::vector<FenceLayout>* fences = nullptr,
    int current_page = -1);

// ---------------------------------------------------------------------------
// G4 — per-fence organize kinds (Python fence_rules.py).
// ---------------------------------------------------------------------------
// Classify a desktop item: "icon" (.lnk/.url/.exe) or "file" (everything else).
std::string OrganizeKindOfItem(const DesktopItem& item);
// True when the item's kind is in the fence's organize_kinds. Empty = no rule.
bool FenceAcceptsItem(const std::vector<std::string>& organize_kinds,
                      const DesktopItem& item);
// First fence whose organize_kinds accepts the item, preferring fences on
// ``current_page`` and only then falling back to other pages (Python
// resolve_organize_target walks the current page first). Empty id = none
// (caller may fall back to legacy ResolveFenceId extension rules).
std::string ResolveFenceByKind(const DesktopItem& item,
                               const std::vector<FenceLayout>& fences,
                               int current_page = -1);

// ---------------------------------------------------------------------------
// G5 — portal / desktop scan exclude patterns (Python desktop_scanner.py).
// ---------------------------------------------------------------------------
// Default patterns (Python default_settings): desktop.ini, DeskTidy.
std::vector<std::wstring> DefaultExcludePatterns();
// Read exclude_patterns from prefs; falls back to DefaultExcludePatterns when
// the prefs field is absent/empty.
std::vector<std::wstring> LoadExcludePatterns();
// Python is_ignored_desktop_entry: temp files (~$*, ~*, .tmp, office suffixes),
// dot-files, and the configured exclude patterns.
bool IsPortalEntryExcluded(const std::wstring& name,
                           const std::vector<std::wstring>& exclude_patterns);
// Enumerate a portal folder into DesktopItems, applying hidden/temp/exclude
// filters. Mirrors the inline portal scan in app.cpp but honors exclude_patterns.
std::vector<DesktopItem> ScanPortalItems(
    const std::wstring& portal_dir,
    const std::vector<std::wstring>& exclude_patterns);
