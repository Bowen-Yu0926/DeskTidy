#pragma once

#include <string>

// UI language: "zh" (default) or "en".
// Persisted at %LocalAppData%\DeskTidyNative\ui_lang.txt

void LoadLanguage();
const char* CurrentLang();
void SetLanguage(const char* lang);
void ToggleLanguage();

// Lookup builtin zh/en string table. Missing key → widened key bytes.
std::wstring Tr(const char* key);
