#pragma once

#include <string>

std::wstring NativeDataDir();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::string NarrowUtf8(const std::wstring& ws);
std::wstring WidenUtf8(const std::string& s);
