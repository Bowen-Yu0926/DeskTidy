#pragma once

#include <Windows.h>

#include <functional>
#include <string>

// Brief tip; auto-dismisses after ~2.2s.
// Default: top-center of the primary work area (system/status tips).
void ShowToast(const std::wstring& text);
// Speech / pet tip: centered above ``anchor``.
// If ``clear_above`` is a visible HWND (e.g. page chips), place above that
// stack instead so the text does not cover it.
void ShowToastNear(HWND anchor, const std::wstring& text,
                   HWND clear_above = nullptr);
// Same as ShowToast but custom lifetime (ms). Use for download progress.
void ShowToastTimed(const std::wstring& text, UINT duration_ms);
// Soft actionable tip (e.g. startup update). Click runs ``on_click`` then
// dismisses. Longer default lifetime (~8s).
void ShowActionToast(const std::wstring& text, std::function<void()> on_click);
// Update text of the visible toast in place (no flash). If none is up, shows
// a timed toast. Resets the dismiss timer when ``duration_ms`` > 0.
void UpdateToastText(const std::wstring& text, UINT duration_ms = 4000);
