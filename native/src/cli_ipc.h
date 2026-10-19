#pragma once

#include <Windows.h>

#include <string>

constexpr wchar_t kNativeMsgWindowClass[] = L"DeskTidyNativeMsg";
constexpr UINT kCliActionMsg = WM_APP + 90;

// Forward a CLI flag like L"--notes" to the running instance.
bool ForwardCliToRunningInstance(const std::wstring& action);
