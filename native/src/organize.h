#pragma once

#include "desktop_items.h"
#include "pin_store.h"

#include <string>

// Virtual organize: assign default pins for items without an explicit pin.
// Returns how many pins were written.
int OrganizeApplyDefaultPins(PinMap* pins, const std::vector<DesktopItem>& items,
                             const std::string& software_id,
                             const std::string& common_id);

// Physical organize: move unpinned desktop files into DeskTidy subfolders.
int OrganizeMoveIntoFolders(const std::vector<DesktopItem>& items);
// Count how many items would be moved (respects whitelist).
int OrganizeCountPhysicalMoves(const std::vector<DesktopItem>& items);
// Names of items that would be moved (for preview).
std::vector<std::wstring> OrganizePreviewPhysicalMoves(
    const std::vector<DesktopItem>& items);
// Show preview MessageBox; returns true if user confirms.
bool OrganizeConfirmPhysical(HWND owner, const std::vector<DesktopItem>& items);
