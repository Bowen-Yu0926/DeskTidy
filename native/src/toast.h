#pragma once

#include <Windows.h>

#include <string>

// Brief tip; auto-dismisses after ~2.2s.
// Default: top-center of the primary work area (system/status tips).
void ShowToast(const std::wstring& text);
// Speech / pet tip: centered above ``anchor``.
// If ``clear_above`` is a visible HWND (e.g. page chips), place above that
// stack instead so the text does not cover it.
void ShowToastNear(HWND anchor, const std::wstring& text,
                   HWND clear_above = nullptr);
