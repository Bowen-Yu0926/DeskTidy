#include "notes_cloud_sync.h"

#include "paths_util.h"

#include <Windows.h>

#include <chrono>
#include <condition_variable>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

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

int JsonIntAfter(const std::string& json, size_t from, size_t limit,
                 const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos || p + needle.size() > limit) {
    return 0;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos || p >= limit) {
    return 0;
  }
  ++p;
  while (p < limit && p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  return p < limit ? atoi(json.c_str() + p) : 0;
}

long long JsonLongAfter(const std::string& json, size_t from, size_t limit,
                        const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos || p + needle.size() > limit) {
    return 0;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos || p >= limit) {
    return 0;
  }
  ++p;
  while (p < limit && p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  return p < limit ? _atoi64(json.c_str() + p) : 0;
}

bool JsonBoolAfter(const std::string& json, size_t from, size_t limit,
                   const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos || p + needle.size() > limit) {
    return false;
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos || p >= limit) {
    return false;
  }
  ++p;
  while (p < limit && p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  return p < limit && p < json.size() && json[p] == 't';
}

std::wstring JsonStringAfter(const std::string& json, size_t from, size_t limit,
                             const char* key) {
  const std::string needle = std::string("\"") + key + "\"";
  size_t p = json.find(needle, from);
  if (p == std::string::npos || p + needle.size() > limit) {
    return {};
  }
  p = json.find(':', p + needle.size());
  if (p == std::string::npos || p >= limit) {
    return {};
  }
  ++p;
  while (p < limit && p < json.size() && (json[p] == ' ' || json[p] == '\t')) {
    ++p;
  }
  if (p >= limit || p >= json.size() || json[p] != '"') {
    return {};
  }
  ++p;
  std::string v;
  while (p < limit && p < json.size() && json[p] != '"') {
    if (json[p] == '\\' && p + 1 < json.size()) {
      ++p;
      const char e = json[p++];
      if (e == 'n') {
        v.push_back('\n');
      } else if (e == 'r') {
        v.push_back('\r');
      } else if (e == 't') {
        v.push_back('\t');
      } else if (e == 'b') {
        v.push_back('\b');
      } else if (e == 'f') {
        v.push_back('\f');
      } else if (e == 'u' && p + 4 <= json.size()) {
        unsigned code = 0;
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
          const char hc = json[p + k];
          code <<= 4;
          if (hc >= '0' && hc <= '9') {
            code |= static_cast<unsigned>(hc - '0');
          } else if (hc >= 'a' && hc <= 'f') {
            code |= static_cast<unsigned>(hc - 'a' + 10);
          } else if (hc >= 'A' && hc <= 'F') {
            code |= static_cast<unsigned>(hc - 'A' + 10);
          } else {
            ok = false;
            break;
          }
        }
        if (ok) {
          p += 4;
          if (code < 0x80) {
            v.push_back(static_cast<char>(code));
          } else if (code < 0x800) {
            v.push_back(static_cast<char>(0xC0 | (code >> 6)));
            v.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          } else {
            v.push_back(static_cast<char>(0xE0 | (code >> 12)));
            v.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            v.push_back(static_cast<char>(0x80 | (code & 0x3F)));
          }
        } else {
          v.push_back('u');
        }
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
  auto find_unquoted = [&](char target, size_t start, size_t limit) -> size_t {
    bool in_str = false;
    for (size_t i = start; i < limit && i < json.size(); ++i) {
      const char c = json[i];
      if (in_str) {
        if (c == '\\' && i + 1 < json.size()) {
          ++i;
        } else if (c == '"') {
          in_str = false;
        }
      } else {
        if (c == '"') {
          in_str = true;
        } else if (c == target) {
          return i;
        }
      }
    }
    return std::string::npos;
  };
  auto parse_objects = [&](const char* section_key, bool links_section) {
    const std::string key = std::string("\"") + section_key + "\"";
    size_t sec = json.find(key);
    if (sec == std::string::npos) {
      return;
    }
    size_t arr = find_unquoted('[', sec + key.size(), json.size());
    if (arr == std::string::npos) {
      return;
    }
    size_t pos = arr + 1;
    const size_t arr_end = find_unquoted(']', pos, json.size());
    if (arr_end == std::string::npos) {
      return;
    }
    while (pos < arr_end) {
      const size_t obj = find_unquoted('{', pos, arr_end);
      if (obj == std::string::npos || obj >= arr_end) {
        break;
      }
      const size_t end = find_unquoted('}', obj + 1, arr_end);
      if (end == std::string::npos || end > arr_end) {
        break;
      }
      if (links_section) {
        NoteCloudLink link;
        link.path = NormPath(JsonStringAfter(json, obj, end, "path"));
        link.note_id = JsonIntAfter(json, obj, end, "note_id");
        link.sync = JsonBoolAfter(json, obj, end, "sync");
        if (!link.path.empty() && link.note_id > 0) {
          // Dedupe on load: each local path must own at most one link, and each
          // note_id must be bound to exactly one local file. A duplicated
          // binding is how sync can cross-write one note's body into another's
          // file (the reported "谷歌邮箱备用码 ↔ 产品问题清单" mix-up).
          bool dup_path = false;
          for (const auto& prev : store.links) {
            if (_wcsicmp(prev.path.c_str(), link.path.c_str()) == 0) {
              dup_path = true;
              break;
            }
          }
          bool dup_id = false;
          for (const auto& prev : store.links) {
            if (prev.note_id == link.note_id) {
              dup_id = true;
              break;
            }
          }
          if (!dup_path && !dup_id) {
            store.links.push_back(std::move(link));
          }
        }
      } else {
        NoteCloudPendingDelete pd;
        pd.note_id = JsonIntAfter(json, obj, end, "note_id");
        pd.delete_after = JsonLongAfter(json, obj, end, "delete_after");
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
  const std::wstring norm = NormPath(path);
  // A note_id must map to exactly one local file, otherwise sync can push one
  // file's body into the other's cloud note (content cross-write). Re-bind by
  // dropping any other path that currently claims this note_id.
  for (auto it = store->links.begin(); it != store->links.end();) {
    if (it->note_id == note_id && it->path != norm) {
      it = store->links.erase(it);
      continue;
    }
    ++it;
  }
  if (auto* L = FindNoteCloudLinkMut(store, path)) {
    L->note_id = note_id;
    L->sync = sync;
    return;
  }
  NoteCloudLink L;
  L.path = norm;
  L.note_id = note_id;
  L.sync = sync;
  store->links.push_back(std::move(L));
}

bool NoteCloudRetargetLink(NoteCloudSyncStore* store,
                           const std::wstring& old_path,
                           const std::wstring& new_path) {
  if (!store || old_path.empty() || new_path.empty()) {
    return false;
  }
  // Compare/store in the same normalised form NoteCloudUpsertLink uses,
  // otherwise a moved link could stop matching its own file.
  const std::wstring old_norm = NormPath(old_path);
  const std::wstring new_norm = NormPath(new_path);
  if (old_norm == new_norm) {
    return false;
  }
  bool moved = false;
  for (auto& L : store->links) {
    if (_wcsicmp(L.path.c_str(), old_norm.c_str()) == 0) {
      L.path = new_norm;
      moved = true;
    }
  }
  return moved;
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

// --- DeskNote cloud-body upload queue -------------------------------------
// Process-scoped state. The worker thread outlives any NotesWindow, so the
// queue cannot live on the window (it used to, and a window teardown would
// have stranded queued bodies).
//
// A note that is edited while offline/not-logged-in has no disk copy — the
// online (`online://`) mirror is memory-only. To avoid losing that edit on
// process exit, the pending queue is mirrored to a small JSON file whenever it
// changes; on startup any leftover entries are re-loaded so they are still
// uploaded once the session is restored.
namespace {
std::mutex g_push_mutex;
std::condition_variable g_push_cv;
std::map<int, std::pair<std::wstring, std::wstring>> g_push_queue;
// Bodies removed from the retry queue but still being uploaded. Keep these in
// the persisted snapshot until the server confirms success; otherwise an edit
// to another note during an upload would rewrite the file without this batch.
std::map<int, std::pair<std::wstring, std::wstring>> g_push_inflight;
bool g_push_worker_started = false;
// Set by NotifyPushWorker so a wakeup that lands between "queue drained" and
// "start waiting" is not lost (a plain notify_one would be).
bool g_push_wake = false;

std::wstring PushPendingPath() {
  return JoinPath(NativeDataDir(), L"notes_push_pending.json");
}

void PersistPushPendingLocked() {
  if (g_push_queue.empty() && g_push_inflight.empty()) {
    DeleteFileW(PushPendingPath().c_str());
    return;
  }
  std::map<int, std::pair<std::wstring, std::wstring>> snapshot = g_push_inflight;
  for (const auto& kv : g_push_queue) {
    snapshot[kv.first] = kv.second;  // queued edits are newer than in-flight
  }
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& kv : snapshot) {
    if (!first) {
      out << ",";
    }
    first = false;
    out << "\"" << kv.first << "\":{\"title\":\""
        << Esc(Narrow(kv.second.first)) << "\",\"body\":\""
        << Esc(Narrow(kv.second.second)) << "\"}";
  }
  out << "}";
  AtomicWriteUtf8File(PushPendingPath(), out.str());
}

void LoadPushPending() {
  std::lock_guard<std::mutex> lock(g_push_mutex);
  std::string json;
  if (!ReadFileUtf8(PushPendingPath(), &json) || json.empty()) {
    return;
  }
  auto skip_ws = [&](size_t* p) {
    while (*p < json.size() &&
           (json[*p] == ' ' || json[*p] == '\t' || json[*p] == '\r' ||
            json[*p] == '\n' || json[*p] == ',')) {
      ++*p;
    }
  };
  size_t pos = json.find('{');
  if (pos == std::string::npos) {
    return;
  }
  ++pos;
  for (;;) {
    skip_ws(&pos);
    if (pos >= json.size() || json[pos] == '}') {
      break;
    }
    if (json[pos] != '"') {
      break;  // malformed top-level key; don't guess and corrupt recovery
    }
    // Parse the top-level key while respecting escaped quotes/backslashes.
    ++pos;
    std::string token;
    while (pos < json.size()) {
      if (json[pos] == '\\' && pos + 1 < json.size()) {
        token.push_back(json[pos++]);
        token.push_back(json[pos++]);
        continue;
      }
      if (json[pos] == '"') {
        break;
      }
      token.push_back(json[pos++]);
    }
    if (pos >= json.size() || json[pos] != '"') {
      break;
    }
    ++pos;
    skip_ws(&pos);
    if (pos >= json.size() || json[pos++] != ':') {
      break;
    }
    skip_ws(&pos);
    if (pos >= json.size() || json[pos] != '{') {
      break;
    }
    const size_t obj_start = pos++;
    int depth = 1;
    bool in_string = false;
    for (; pos < json.size() && depth > 0; ++pos) {
      const char c = json[pos];
      if (in_string) {
        if (c == '\\' && pos + 1 < json.size()) {
          ++pos;
        } else if (c == '"') {
          in_string = false;
        }
      } else if (c == '"') {
        in_string = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
      }
    }
    if (depth != 0) {
      break;
    }
    const size_t obj_end = pos;
    const bool is_number =
        !token.empty() && token.size() <= 10 &&
        token.find_first_not_of("0123456789") == std::string::npos;
    const int id = is_number ? std::atoi(token.c_str()) : 0;
    if (id > 0 && g_push_queue.count(id) == 0) {
      std::wstring title = JsonStringAfter(json, obj_start, obj_end, "title");
      std::wstring body_w = JsonStringAfter(json, obj_start, obj_end, "body");
      g_push_queue.emplace(id, std::make_pair(title, body_w));
    }
  }
}
}  // namespace

void NoteCloudPushEnqueue(int note_id, const std::wstring& title,
                          const std::wstring& body) {
  if (note_id <= 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_push_mutex);
    g_push_queue[note_id] = std::make_pair(title, body);
    // A new edit must be able to wake an idle worker. The retry wait below
    // only wakes on g_push_wake (never on the queue), so a freshly-enqueued
    // batch would otherwise sit until the next notify or the 1h timeout.
    g_push_wake = true;
    PersistPushPendingLocked();
  }
  g_push_cv.notify_one();
}

std::map<int, std::pair<std::wstring, std::wstring>> NoteCloudPushTakeAll() {
  std::map<int, std::pair<std::wstring, std::wstring>> batch;
  {
    std::lock_guard<std::mutex> lock(g_push_mutex);
    batch.swap(g_push_queue);
    // The batch is now "in flight": keep it in the persisted snapshot so an
    // edit to a different note while this upload runs cannot rewrite the file
    // without this pending body. Committed ones are dropped in CommitBatch;
    // failed ones move back to the queue in Requeue.
    for (const auto& kv : batch) {
      g_push_inflight[kv.first] = kv.second;
    }
    PersistPushPendingLocked();
  }
  return batch;
}

void NoteCloudPushRequeue(
    const std::map<int, std::pair<std::wstring, std::wstring>>& batch) {
  std::lock_guard<std::mutex> lock(g_push_mutex);
  for (const auto& kv : batch) {
    // emplace, not operator[]: never overwrite a newer body that was queued
    // while this batch was in flight (latest-wins).
    g_push_queue.emplace(kv.first, kv.second);
    // No longer in flight — it is queued again and will be retried.
    g_push_inflight.erase(kv.first);
  }
  PersistPushPendingLocked();
}

bool NoteCloudPushMarkWorkerStarted() {
  std::lock_guard<std::mutex> lock(g_push_mutex);
  const bool start = !g_push_worker_started;
  g_push_worker_started = true;
  return start;
}

// Call after a batch uploaded successfully: drop the on-disk pending mirror so
// a future crash recovery does not re-upload an already-committed body.
void NoteCloudPushCommitBatch(const std::map<int, std::pair<std::wstring,
                               std::wstring>>& uploaded) {
  std::lock_guard<std::mutex> lock(g_push_mutex);
  for (const auto& kv : uploaded) {
    const auto it = g_push_inflight.find(kv.first);
    if (it != g_push_inflight.end() && it->second == kv.second) {
      g_push_inflight.erase(it);
    }
  }
  PersistPushPendingLocked();
}

// Reload any bodies that were queued but not yet uploaded when a previous run
// exited. Returns true if anything was recovered (caller should start the
// worker and wake it).
bool NoteCloudPushRecoverPending() {
  LoadPushPending();
  std::lock_guard<std::mutex> lock(g_push_mutex);
  const bool recovered = !g_push_queue.empty();
  if (recovered) {
    g_push_wake = true;
  }
  return recovered;
}

void NoteCloudNotifyPushWorker() {
  {
    std::lock_guard<std::mutex> lock(g_push_mutex);
    // Latch the wakeup: the worker may be between TakeAll() and wait_for(),
    // where a bare notify_one would be lost and the batch would sit for the
    // full retry interval.
    g_push_wake = true;
  }
  g_push_cv.notify_one();
}

void NoteCloudPushWaitForRetry(unsigned ms) {
  std::unique_lock<std::mutex> lock(g_push_mutex);
  // Wake ONLY on a latched notify (new edit / login). The predicate must not
  // test !g_push_queue.empty(): the worker calls this AFTER re-queueing a
  // failed batch, so a queue-based predicate is instantly true and the wait
  // returns without sleeping — turning "offline, not logged in, or server
  // down" into a 100%-CPU spin that never honours the backoff. A new edit
  // still wakes us because NoteCloudPushEnqueue sets g_push_wake.
  g_push_cv.wait_for(lock, std::chrono::milliseconds(ms),
                     [] { return g_push_wake; });
  g_push_wake = false;
}
