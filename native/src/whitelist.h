#pragma once

#include <Windows.h>

#include <string>
#include <vector>

std::vector<std::wstring> LoadWhitelist();
bool SaveWhitelist(const std::vector<std::wstring>& names);
bool IsWhitelisted(const std::wstring& path_or_name,
                   const std::vector<std::wstring>& names);
void EditWhitelistDialog(HWND owner);
