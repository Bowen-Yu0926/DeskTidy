#include "float_placements.h"

#include "display_geom.h"
#include "paths_util.h"
#include "pin_store.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace {

double Clamp01(double v) {
  if (v < 0.0) {
    return 0.0;
  }
  if (v > 1.0) {
    return 1.0;
  }
  return v;
}

bool FindDoubleNear(const std::string& body, size_t from, const char* key,
                    double* out) {
  if (!out || !key) {
    return false;
  }
  const std::string needle = std::string("\"") + key + "\":";
  const size_t pos = body.find(needle, from);
  if (pos == std::string::npos || pos > from + 400) {
    return false;
  }
  size_t i = pos + needle.size();
  while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) {
    ++i;
  }
  try {
    size_t consumed = 0;
    *out = std::stod(body.substr(i), &consumed);
    return consumed > 0;
  } catch (...) {
    return false;
  }
}

bool FindIntNear(const std::string& body, size_t from, const char* key,
                 int* out) {
  double d = 0;
  if (!FindDoubleNear(body, from, key, &d)) {
    return false;
  }
  *out = static_cast<int>(std::lround(d));
  return true;
}

}  // namespace

std::wstring FloatPlacementsPath() {
  return JoinPath(NativeDataDir(), L"float_placements.json");
}

FloatPlacementMap LoadFloatPlacements() {
  FloatPlacementMap map;
  std::string json;
  if (!ReadUtf8FileWithBak(FloatPlacementsPath(), &json) || json.empty()) {
    return map;
  }
  size_t pos = 0;
  while (true) {
    const size_t pkey = json.find("\"path\"", pos);
    if (pkey == std::string::npos) {
      break;
    }
    const size_t colon = json.find(':', pkey);
    const size_t q0 = json.find('"', colon == std::string::npos ? pkey : colon + 1);
    if (q0 == std::string::npos) {
      break;
    }
    const size_t q1 = json.find('"', q0 + 1);
    if (q1 == std::string::npos) {
      break;
    }
    std::string path_u8;
    path_u8.reserve(q1 - q0);
    for (size_t i = q0 + 1; i < q1; ++i) {
      if (json[i] == '\\' && i + 1 < q1) {
        ++i;
        path_u8.push_back(json[i]);
      } else {
        path_u8.push_back(json[i]);
      }
    }
    FloatPlacement fp;
    fp.path = WidenUtf8(path_u8);
    FindIntNear(json, pkey, "page", &fp.page);
    double rx = 0.05;
    double ry = 0.05;
    const bool has_rx = FindDoubleNear(json, pkey, "rx", &rx);
    const bool has_ry = FindDoubleNear(json, pkey, "ry", &ry);
    fp.rx = Clamp01(rx);
    fp.ry = Clamp01(ry);
    fp.has_pos = has_rx && has_ry;
    if (!fp.path.empty()) {
      map[NormalizePinKey(fp.path)] = fp;
    }
    pos = q1 + 1;
  }
  return map;
}

bool SaveFloatPlacements(const FloatPlacementMap& map) {
  std::ostringstream out;
  out << "{\n  \"items\": [\n";
  size_t i = 0;
  for (const auto& kv : map) {
    const FloatPlacement& fp = kv.second;
    out << "    {\"path\":\"" << EscapeJsonUtf8(NarrowUtf8(fp.path))
        << "\",\"page\":" << fp.page;
    if (fp.has_pos) {
      out << ",\"rx\":" << fp.rx << ",\"ry\":" << fp.ry;
    }
    out << "}";
    if (++i < map.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return AtomicWriteUtf8File(FloatPlacementsPath(), out.str());
}

void FloatRatiosFromScreen(POINT screen, double* rx, double* ry) {
  if (!rx || !ry) {
    return;
  }
  const RECT work = WorkAreaAtPoint(screen);
  const int ww = (std::max)(1, static_cast<int>(work.right - work.left));
  const int wh = (std::max)(1, static_cast<int>(work.bottom - work.top));
  *rx = Clamp01(static_cast<double>(screen.x - work.left) / ww);
  *ry = Clamp01(static_cast<double>(screen.y - work.top) / wh);
}

POINT FloatScreenPoint(const FloatPlacement& fp) {
  POINT probe{0, 0};
  const RECT primary = PrimaryWorkArea();
  probe.x = primary.left + static_cast<LONG>(fp.rx * (primary.right - primary.left));
  probe.y = primary.top + static_cast<LONG>(fp.ry * (primary.bottom - primary.top));
  const RECT work = WorkAreaAtPoint(probe);
  POINT out{};
  out.x = work.left + static_cast<LONG>(
              Clamp01(fp.rx) * (std::max)(1, static_cast<int>(work.right - work.left)));
  out.y = work.top + static_cast<LONG>(
              Clamp01(fp.ry) * (std::max)(1, static_cast<int>(work.bottom - work.top)));
  return out;
}

void UpsertFloatPlacement(FloatPlacementMap* map, const std::wstring& path,
                          int page, POINT screen) {
  if (!map || path.empty()) {
    return;
  }
  FloatPlacement fp;
  fp.path = path;
  fp.page = (std::max)(0, page);
  FloatRatiosFromScreen(screen, &fp.rx, &fp.ry);
  fp.has_pos = true;
  (*map)[NormalizePinKey(path)] = std::move(fp);
}

void SetFloatPage(FloatPlacementMap* map, const std::wstring& path, int page) {
  if (!map || path.empty()) {
    return;
  }
  const std::wstring key = NormalizePinKey(path);
  auto it = map->find(key);
  if (it == map->end()) {
    FloatPlacement fp;
    fp.path = path;
    fp.page = (std::max)(0, page);
    fp.has_pos = false;
    (*map)[key] = std::move(fp);
    return;
  }
  it->second.page = (std::max)(0, page);
  it->second.path = path;
}

void EraseFloatPlacement(FloatPlacementMap* map, const std::wstring& path) {
  if (!map) {
    return;
  }
  map->erase(NormalizePinKey(path));
}

void RetargetFloatPlacement(FloatPlacementMap* map, const std::wstring& from,
                            const std::wstring& to) {
  if (!map || from.empty() || to.empty()) {
    return;
  }
  const std::wstring fk = NormalizePinKey(from);
  auto it = map->find(fk);
  if (it == map->end()) {
    return;
  }
  FloatPlacement fp = it->second;
  map->erase(it);
  fp.path = to;
  (*map)[NormalizePinKey(to)] = std::move(fp);
}
