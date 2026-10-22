#pragma once

#include <string>

std::wstring NativeDataDir();
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);
std::string NarrowUtf8(const std::wstring& ws);
std::wstring WidenUtf8(const std::string& s);

// U4 portable mode. True when the exe directory contains `portable.txt` or the
// command line carries `--portable`. When portable, NativeDataDir() resolves to
// `<exe dir>\data` instead of %LOCALAPPDATA%\DeskTidyNative (Python semantics).
bool IsPortableMode();
// Directory of the running exe (no trailing slash), resolved once.
std::wstring NativeExeDir();
