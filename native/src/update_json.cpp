#include "update_json.h"

#include <Windows.h>

#include <cstdlib>

namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                    static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                      out.data(), n);
  return out;
}

}  // namespace

std::string JsonStringField(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find('"', json.find(':', p) + 1);
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  std::string v;
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
    }
    v.push_back(json[p++]);
  }
  return v;
}

std::string JsonNumberField(const std::string& json, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  size_t s = p + 1;
  while (s < json.size() && (json[s] == ' ' || json[s] == '\t')) {
    ++s;
  }
  size_t e = s;
  while (e < json.size() && json[e] >= '0' && json[e] <= '9') {
    ++e;
  }
  return json.substr(s, e - s);
}

// Asset objects in a GitHub release response carry a nested "uploader" object,
// so "find the next '}'" stops inside it and the slice never reaches the
// asset's own "browser_download_url" — every asset then looked like it had no
// download URL and the installer refused with「该版本未提供 Windows 安装包」.
size_t JsonObjectEnd(const std::string& json, size_t start) {
  int depth = 0;
  bool in_str = false;
  bool esc = false;
  for (size_t i = start; i < json.size(); ++i) {
    const char c = json[i];
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
    } else if (c == '"') {
      in_str = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) {
        return i;
      }
    }
  }
  return std::string::npos;
}

// Walk the top-level objects of the array that follows the `"key"` field.
// `fn` receives each element as a slice. This is what makes the scan correct:
// searching for '{' from the start of the document finds the ROOT object
// first, whose matching '}' spans the whole body, so every asset collapsed
// into one "element" and only the first (or none) was ever considered.
template <typename Fn>
void ForEachArrayObjectImpl(const std::string& json, const char* key, Fn fn) {
  const std::string needle = std::string("\"") + key + "\"";
  const size_t k = json.find(needle);
  if (k == std::string::npos) {
    return;
  }
  size_t i = json.find('[', k + needle.size());
  if (i == std::string::npos) {
    return;
  }
  int depth = 0;
  bool in_str = false;
  bool esc = false;
  for (; i < json.size(); ++i) {
    const char c = json[i];
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '[') {
      ++depth;
    } else if (c == ']') {
      if (--depth == 0) {
        return;  // end of this array
      }
    } else if (c == '{' && depth == 1) {
      const size_t end = JsonObjectEnd(json, i);
      if (end == std::string::npos) {
        return;
      }
      fn(json.substr(i, end - i + 1));
      i = end;
    }
  }
}

void ForEachArrayObject(const std::string& json, const char* key,
                        const std::function<void(const std::string&)>& fn) {
  ForEachArrayObjectImpl(json, key, fn);
}

bool FindExeAsset(const std::string& json, std::string* name,
                  std::wstring* url, long long* size) {
  long long best = -1;
  std::string best_name;
  std::string best_url;
  ForEachArrayObject(json, "assets", [&](const std::string& obj) {
    const std::string nm = JsonStringField(obj, "name");
    const std::string ur = JsonStringField(obj, "browser_download_url");
    if (nm.empty() || ur.empty()) {
      return;
    }
    if (nm.size() < 4 || nm.rfind(".exe") != nm.size() - 4) {
      return;
    }
    if (nm.rfind("DeskTidy_Setup_", 0) != 0) {
      return;
    }
    const std::string szs = JsonNumberField(obj, "size");
    const long long sz = szs.empty() ? 0 : std::atoll(szs.c_str());
    if (sz > best) {
      best = sz;
      best_name = nm;
      best_url = ur;
    }
  });
  if (best < 0) {
    return false;
  }
  if (name) {
    *name = best_name;
  }
  if (url) {
    *url = Widen(best_url);
  }
  if (size) {
    *size = best;
  }
  return true;
}
