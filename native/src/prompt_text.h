#pragma once

#include <Windows.h>

#include <string>

// Simple modal text prompt. Returns empty on cancel.
std::wstring PromptText(HWND owner, const wchar_t* title, const wchar_t* label,
                        const std::wstring& initial);
