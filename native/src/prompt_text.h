#pragma once

#include <Windows.h>

#include <string>

// Modal text prompt. Prefers Mist card (WebView2); Win32 fallback.
// Returns empty on cancel.
std::wstring PromptText(HWND owner, const wchar_t* title, const wchar_t* label,
                        const std::wstring& initial);

// Classic Win32-only prompt (used as Mist fallback / nested-safe path).
std::wstring PromptTextWin32(HWND owner, const wchar_t* title,
                             const wchar_t* label,
                             const std::wstring& initial);
