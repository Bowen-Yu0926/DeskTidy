#include "pin_store.h"

#include "paths_util.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdio>
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
  for (unsigned char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (c >= 0x20) {
          out.push_back(static_cast<char>(c));
        } else {
          char hex[8];
          snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned int>(c));
          out += hex;
        }
        break;
    }
  }
  return out;
}

bool ReadJsonQuoted(const std::string& s, size_t* pos, std::string* out) {
  if (*pos >= s.size() || s[*pos] != '"') {
    return false;
  }
  ++(*pos);
  out->clear();
  while (*pos < s.size() && s[*pos] != '"') {
    if (s[*pos] == '\\' && *pos + 1 < s.size()) {
      ++(*pos);
      const char esc = s[*pos];
      switch (esc) {
        case 'n': out->push_back('\n'); ++(*pos); break;
        case 'r': out->push_back('\r'); ++(*pos); break;
        case 't': out->push_back('\t'); ++(*pos); break;
        case 'b': out->push_back('\b'); ++(*pos); break;
        case 'f': out->push_back('\f'); ++(*pos); break;
        case 'u': {
          if (*pos + 4 < s.size()) {
            unsigned cp = 0;
            bool ok = true;
            for (int k = 1; k <= 4; ++k) {
              const char h = s[*pos + k];
              cp <<= 4;
              if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
              else { ok = false; break; }
            }
            if (ok) {
              // A non-BMP character arrives as a UTF-16 \uD800-\uDBFF / \uDC00-\uDFFF
              // pair. Encoding each half as UTF-8 separately yields invalid bytes;
              // WidenUtf8 then produces a different pin key and emoji paths lose
              // their pins. Combine the pair before UTF-8 encoding.
              *pos += 5;  // past 'u' + 4 hex digits
              if (cp >= 0xD800 && cp <= 0xDBFF && *pos + 6 <= s.size() &&
                  s[*pos] == '\\' && s[*pos + 1] == 'u') {
                unsigned low = 0;
                bool low_ok = true;
                for (int k = 0; k < 4; ++k) {
                  const char h = s[*pos + 2 + k];
                  low <<= 4;
                  if (h >= '0' && h <= '9') low |= static_cast<unsigned>(h - '0');
                  else if (h >= 'a' && h <= 'f') low |= static_cast<unsigned>(h - 'a' + 10);
                  else if (h >= 'A' && h <= 'F') low |= static_cast<unsigned>(h - 'A' + 10);
                  else { low_ok = false; break; }
                }
                if (low_ok && low >= 0xDC00 && low <= 0xDFFF) {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                  *pos += 6;
                }
              }
              if (cp < 0x80) {
                out->push_back(static_cast<char>(cp));
              } else if (cp < 0x800) {
                out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
              } else if (cp < 0x10000) {
                out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
              } else {
                out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
                out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
              }
              break;
            }
          }
          out->push_back('u');
          ++(*pos);
          break;
        }
        default:
          out->push_back(esc);
          ++(*pos);
          break;
      }
    } else {
      out->push_back(s[(*pos)++]);
    }
  }
  if (*pos >= s.size() || s[*pos] != '"') {
    return false;
  }
  ++(*pos);
  return true;
}

}  // namespace

std::wstring NormalizePinKey(const std::wstring& path) {
  std::wstring key = path;
  // OLE / Shell sometimes hands \\?\D:\… while desktop scan uses D:\… —
  // without stripping, SetPin and FindPin miss each other → fence + float twin.
  if (key.size() >= 4 && key[0] == L'\\' && key[1] == L'\\' &&
      key[2] == L'?' && key[3] == L'\\') {
    if (key.size() >= 8 &&
        (key[4] == L'U' || key[4] == L'u') &&
        (key[5] == L'N' || key[5] == L'n') &&
        (key[6] == L'C' || key[6] == L'c') && key[7] == L'\\') {
      // \\?\UNC\server\share\… → \\server\share\…
      key = L"\\\\" + key.substr(8);
    } else {
      key = key.substr(4);
    }
  }
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
    std::string path;
    if (!ReadJsonQuoted(json, &pos, &path)) {
      break;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
      break;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
      break;
    }
    std::string fence;
    if (!ReadJsonQuoted(json, &pos, &fence)) {
      break;
    }
    if (!path.empty() && !fence.empty()) {
      pins[NormalizePinKey(Widen(path))] = fence;
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
