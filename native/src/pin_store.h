#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// path (lowercase) -> fence id
using PinMap = std::unordered_map<std::wstring, std::string>;

std::wstring PinFilePath();
PinMap LoadPins();
bool SavePins(const PinMap& pins);
std::wstring NormalizePinKey(const std::wstring& path);

void SetPin(PinMap* pins, const std::wstring& path, const std::string& fence_id);
void ClearPin(PinMap* pins, const std::wstring& path);
const std::string* FindPin(const PinMap& pins, const std::wstring& path);
