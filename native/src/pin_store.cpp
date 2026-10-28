#include "pin_store.h"

#include "paths_util.h"

#include <Windows.h>
#include <ShlObj.h>

#include <fstream>
#include <sstream>

namespace {

std::string Narrow(const std::wstring& ws) {
  if (ws.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
  std::string out(n > 0 ? n - 1 : 0, '\0');
  if (n > 1) {
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr,
                        nullptr);
  }
  return out;
}

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring out(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) {
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  }
  return out;
}

std::string EscapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      default:
        out.push_back(c);
        break;
    }
  }
  return out;
}

}  // namespace

std::wstring NormalizePinKey(const std::wstring& path) {
  std::wstring key = path;
  for (auto& ch : key) {
    if (ch == L'/') {
      ch = L'\\';
    } else if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return key;
}

std::wstring PinFilePath() {
  return JoinPath(NativeDataDir(), L"pins.json");
}

namespace {

PinMap ParsePinsJson(const std::string& json) {
  PinMap pins;
  // Very small parser: "path":"fence_id" pairs inside "pins"
  size_t pos = json.find("\"pins\"");
  if (pos == std::string::npos) {
    return pins;
  }
  pos = json.find('{', pos);
  if (pos == std::string::npos) {
    return pins;
  }
  ++pos;
  while (pos < json.size()) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' ||
            json[pos] == '\t' || json[pos] == ',')) {
      ++pos;
    }
    if (pos >= json.size() || json[pos] == '}') {
      break;
    }
    if (json[pos] != '"') {
      break;
    }
    ++pos;
    std::string path;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
        path.push_back(json[pos++]);
      } else {
        path.push_back(json[pos++]);
      }
    }
    if (pos >= json.size()) {
      break;
    }
    ++pos;  // closing "
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
      break;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
      break;
    }
    ++pos;
    std::string fence;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        ++pos;
        fence.push_back(json[pos++]);
      } else {
        fence.push_back(json[pos++]);
      }
    }
    if (!path.empty() && !fence.empty()) {
      pins[NormalizePinKey(Widen(path))] = fence;
    }
    if (pos < json.size() && json[pos] == '"') {
      ++pos;
    }
  }
  return pins;
}

PinMap LoadPinsFromPath(const std::wstring& path) {
  std::string json;
  if (!ReadUtf8FileWithBak(path, &json)) {
    return {};
  }
  return ParsePinsJson(json);
}

}  // namespace

PinMap LoadPins() { return LoadPinsFromPath(PinFilePath()); }

int HealPinsFromBakOnce(PinMap* pins) {
  if (!pins) {
    return 0;
  }
  const std::wstring marker =
      JoinPath(NativeDataDir(), L"pins_bak_heal.done");
  if (!FileMissingOrTiny(marker, 1)) {
    return 0;
  }
  const std::wstring root = NativeDataDir();
  int added = 0;
  for (const wchar_t* name :
       {L"pins.json.bak", L"pins.json.bak-python-import"}) {
    const auto donor = LoadPinsFromPath(JoinPath(root, name));
    for (const auto& kv : donor) {
      if (pins->count(kv.first) != 0) {
        continue;
      }
      const bool keep =
          kv.first.find(L"::") != std::wstring::npos ||
          GetFileAttributesW(kv.first.c_str()) != INVALID_FILE_ATTRIBUTES;
      if (!keep) {
        continue;
      }
      (*pins)[kv.first] = kv.second;
      ++added;
    }
  }
  HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    const char ok[] = "1\n";
    DWORD w = 0;
    WriteFile(h, ok, 2, &w, nullptr);
    CloseHandle(h);
  }
  return added;
}

bool SavePins(const PinMap& pins) {
  std::ostringstream out;
  out << "{\n  \"pins\": {\n";
  size_t i = 0;
  for (const auto& kv : pins) {
    out << "    \"" << EscapeJson(Narrow(kv.first)) << "\":\""
        << EscapeJson(kv.second) << "\"";
    if (++i < pins.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  }\n}\n";
  return AtomicWriteUtf8File(PinFilePath(), out.str());
}

void SetPin(PinMap* pins, const std::wstring& path, const std::string& fence_id) {
  if (!pins || path.empty() || fence_id.empty()) {
    return;
  }
  (*pins)[NormalizePinKey(path)] = fence_id;
}

void ClearPin(PinMap* pins, const std::wstring& path) {
  if (!pins) {
    return;
  }
  pins->erase(NormalizePinKey(path));
}

const std::string* FindPin(const PinMap& pins, const std::wstring& path) {
  const auto it = pins.find(NormalizePinKey(path));
  if (it == pins.end()) {
    return nullptr;
  }
  return &it->second;
}
