#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// path (lowercase) -> fence id
using PinMap = std::unordered_map<std::wstring, std::string>;

std::wstring PinFilePath();
PinMap LoadPins();
bool SavePins(const PinMap& pins);
// One-shot: re-add pins present in .bak / .bak-python-import whose paths still
// exist (or namespace :: pins). Returns how many keys were restored.
int HealPinsFromBakOnce(PinMap* pins);
std::wstring NormalizePinKey(const std::wstring& path);

void SetPin(PinMap* pins, const std::wstring& path, const std::string& fence_id);
void ClearPin(PinMap* pins, const std::wstring& path);
const std::string* FindPin(const PinMap& pins, const std::wstring& path);
