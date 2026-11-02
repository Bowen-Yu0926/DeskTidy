#pragma once

#include <Windows.h>

#include <string>

// Mist-card message / prompt helpers (WebView2). Falls back to Win32 when
// WebView2 / HTML assets are unavailable.

enum class MistMsgKind {
  Alert,        // OK
  Confirm,      // Yes / No
  YesNoCancel,  // Yes / No / Cancel
};

enum class MistMsgTone {
  Info,
  Warn,
  Danger,
};

enum class MistMsgResult {
  Ok,
  Yes,
  No,
  Cancel,
};

MistMsgResult MistMessage(HWND owner, const wchar_t* title,
                          const wchar_t* message, MistMsgKind kind,
                          MistMsgTone tone = MistMsgTone::Info);

// Convenience wrappers.
inline void MistAlert(HWND owner, const wchar_t* title, const wchar_t* message,
                      MistMsgTone tone = MistMsgTone::Info) {
  MistMessage(owner, title, message, MistMsgKind::Alert, tone);
}

inline bool MistConfirm(HWND owner, const wchar_t* title,
                        const wchar_t* message,
                        MistMsgTone tone = MistMsgTone::Warn) {
  return MistMessage(owner, title, message, MistMsgKind::Confirm, tone) ==
         MistMsgResult::Yes;
}

// Prefetch WebView2 user-data for mist confirm cards (DeskNote boot).
void MistWarmMessageEnv();

// Mist prompt (prompt.html). Empty string on cancel / failure.
std::wstring MistPromptText(HWND owner, const wchar_t* title,
                            const wchar_t* label, const std::wstring& initial);
