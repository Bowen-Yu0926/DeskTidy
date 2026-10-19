#pragma once

#include <string>

// Temporarily skip auto-organize / watcher for paths DeskTidy just touched
// (e.g. user unpinned / dragged out of a fence).
void SuppressOrganizePath(const std::wstring& path, double seconds = 12.0);
void ClearOrganizeSuppress(const std::wstring& path = {});
bool IsOrganizeSuppressed(const std::wstring& path);
