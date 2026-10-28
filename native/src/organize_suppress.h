#pragma once

#include <string>

// Temporarily skip auto-organize / watcher for paths DeskTidy just touched
// (e.g. user unpinned / dragged out of a fence).
// Default 12s covers paste/import churn. UnpinPath should pass a long window
// (hours) so auto-organize does not immediately re-fence a user float.
void SuppressOrganizePath(const std::wstring& path, double seconds = 12.0);
void ClearOrganizeSuppress(const std::wstring& path = {});
bool IsOrganizeSuppressed(const std::wstring& path);
