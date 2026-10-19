#pragma once

#include <Windows.h>

#include <string>

// Brief top-center tip; auto-dismisses after ~2.2s.
void ShowToast(const std::wstring& text);
