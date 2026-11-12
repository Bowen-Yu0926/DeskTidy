#include "notes_cloud_sync.h"

#include "paths_util.h"
#include "vault_http.h"

#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::wstring StorePath() {
  return JoinPath(NativeDataDir(), L"notes_cloud_sync.json");
}

std::string Narrow(const std::wstring& w) { return NarrowUtf8(w); }
std::wstring Widen(const std::string& u) { return WidenUtf8(u); }

std::string Esc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      o.push_back('\\');
      o.push_back(static_cast<char>(c));
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\r') {
      o += "\\r";
    } else if (c == '\t') {
      o += "\\t";
    } else if (c < 0x20) {
      char buf[8];
      sprintf_s(buf, "\\u%04x", c);
      o += buf;
    } else {
      o.push_back(static_cast<char>(c));
    }
  }
  return o;
}

std::wstring NormPath(std::wstring p) {
  for (auto& c : p) {
    if (c == L'/') {
      c = L'\\';
    }
    if (c >= L'A' && c <= L'Z') {
      c = static_cast<wchar_t>(c - L'A' + L'a');
    }
  }
  while (!p.empty() && (p.back() == L'\\' || p.back() == L' ')) {
    p.pop_back();
  }
  return p;
}

bool ReadFileUtf8(const std::wstring& path, std::string* out) {
  if (!out) {
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

int JsonIntAfter(const std::string& json, size_t from, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos) {
    return 0;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return 0;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  return atoi(json.c_str() + p);
}

long long JsonLongAfter(const std::string& json, size_t from, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos) {
    return 0;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return 0;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  return _atoi64(json.c_str() + p);
}

bool JsonBoolAfter(const std::string& json, size_t from, const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos) {
    return false;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  return p < json.size() && json[p] == 't';
}

std::wstring JsonStringAfter(const std::string& json, size_t from,
                             const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos) {
    return {};
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos) {
    return {};
  }
  ++p;
  while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  if (p >= json.size() || json[p] != '"') {
    return {};
  }
  ++p;
  std::string v;
  while (p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
      const char e = json[p++];
      if (e == 'n') {
        v.push_back('\n');
      } else if (e == 'r') {
        v.push_back('\r');
      } else if (e == 't') {
        v.push_back('\t');
      } else {
        v.push_back(e);
      }
      continue;
    }
    v.push_back(json[p++]);
  }
  return Widen(v);
}

}  // namespace

long long NoteCloudUnixNow() {
  return static_cast<long long>(time(nullptr));
}

long long NoteCloudUnixAfterHours(int hours) {
  return NoteCloudUnixNow() + static_cast<long long>(hours) * 3600LL;
}

NoteCloudSyncStore LoadNoteCloudSync() {
  NoteCloudSyncStore store;
  std::string json;
  if (!ReadFileUtf8(StorePath(), &json) || json.empty()) {
    return store;
  }
  auto parse_objects = [&](const char* section_key, bool links_section) {
    const std::string key = std::string("\"") + section_key + "\"";
    size_t sec = json.find(key);
    if (sec == std::string::npos) {
      return;
    }
    size_t arr = json.find('[', sec);
    if (arr == std::string::npos) {
      return;
    }
    size_t pos = arr + 1;
    const size_t arr_end = json.find(']', pos);
    if (arr_end == std::string::npos) {
      return;
    }
    while (pos < arr_end) {
      const size_t obj = json.find('{', pos);
      if (obj == std::string::npos || obj >= arr_end) {
        break;
      }
      const size_t end = json.find('}', obj);
      if (end == std::string::npos || end > arr_end) {
        break;
      }
      if (links_section) {
        NoteCloudLink link;
        link.path = NormPath(JsonStringAfter(json, obj, "path"));
        link.note_id = JsonIntAfter(json, obj, "note_id");
        link.sync = JsonBoolAfter(json, obj, "sync");
        if (!link.path.empty() && link.note_id > 0) {
          store.links.push_back(std::move(link));
        }
      } else {
        NoteCloudPendingDelete pd;
        pd.note_id = JsonIntAfter(json, obj, "note_id");
        pd.delete_after = JsonLongAfter(json, obj, "delete_after");
        if (pd.note_id > 0 && pd.delete_after > 0) {
          store.pending_deletes.push_back(pd);
        }
      }
      pos = end + 1;
    }
  };
  parse_objects("links", true);
  parse_objects("pending_deletes", false);
  return store;
}

bool SaveNoteCloudSync(const NoteCloudSyncStore& store) {
  std::ostringstream out;
  out << "{\n  \"links\": [\n";
  for (size_t i = 0; i < store.links.size(); ++i) {
    const auto& L = store.links[i];
    out << "    {\"path\":\"" << Esc(Narrow(L.path)) << "\",\"note_id\":"
        << L.note_id << ",\"sync\":" << (L.sync ? "true" : "false") << "}";
    if (i + 1 < store.links.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ],\n  \"pending_deletes\": [\n";
  for (size_t i = 0; i < store.pending_deletes.size(); ++i) {
    const auto& d = store.pending_deletes[i];
    out << "    {\"note_id\":" << d.note_id << ",\"delete_after\":"
        << d.delete_after << "}";
    if (i + 1 < store.pending_deletes.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n}\n";
  return AtomicWriteUtf8File(StorePath(), out.str());
}

const NoteCloudLink* FindNoteCloudLink(const NoteCloudSyncStore& store,
                                       const std::wstring& path) {
  const std::wstring key = NormPath(path);
  for (const auto& L : store.links) {
    if (_wcsicmp(L.path.c_str(), key.c_str()) == 0) {
      return &L;
    }
  }
  return nullptr;
}

NoteCloudLink* FindNoteCloudLinkMut(NoteCloudSyncStore* store,
                                    const std::wstring& path) {
  if (!store) {
    return nullptr;
  }
  const std::wstring key = NormPath(path);
  for (auto& L : store->links) {
    if (_wcsicmp(L.path.c_str(), key.c_str()) == 0) {
      return &L;
    }
  }
  return nullptr;
}

void NoteCloudUpsertLink(NoteCloudSyncStore* store, const std::wstring& path,
                         int note_id, bool sync) {
  if (!store || path.empty() || note_id <= 0) {
    return;
  }
  if (auto* L = FindNoteCloudLinkMut(store, path)) {
    L->note_id = note_id;
    L->sync = sync;
    return;
  }
  NoteCloudLink L;
  L.path = NormPath(path);
  L.note_id = note_id;
  L.sync = sync;
  store->links.push_back(std::move(L));
}

void NoteCloudScheduleDelete(NoteCloudSyncStore* store, int note_id,
                             long long delete_after_unix) {
  if (!store || note_id <= 0 || delete_after_unix <= 0) {
    return;
  }
  for (auto& d : store->pending_deletes) {
    if (d.note_id == note_id) {
      d.delete_after = delete_after_unix;
      return;
    }
  }
  store->pending_deletes.push_back(
      NoteCloudPendingDelete{note_id, delete_after_unix});
}

int NoteCloudSweepPendingDeletes(NoteCloudSyncStore* store) {
  if (!store) {
    return 0;
  }
  const long long now = NoteCloudUnixNow();
  int deleted = 0;
  std::vector<NoteCloudPendingDelete> keep;
  keep.reserve(store->pending_deletes.size());
  for (const auto& d : store->pending_deletes) {
    if (d.note_id <= 0) {
      continue;
    }
    if (d.delete_after > 0 && now >= d.delete_after) {
      if (VaultHttpLoggedIn() && VaultHttpDeleteNote(d.note_id)) {
        ++deleted;
        continue;
      }
      // Not logged in / failed — keep and retry next launch.
      keep.push_back(d);
      continue;
    }
    keep.push_back(d);
  }
  store->pending_deletes = std::move(keep);
  return deleted;
}
