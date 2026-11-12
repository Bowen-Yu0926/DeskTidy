#pragma once

#include <string>
#include <vector>

// Local file ↔ vault online-note link. 「转云 / 转本地」只改同步属性，文件仍是同一份。
struct NoteCloudLink {
  std::wstring path;  // normalized absolute path
  int note_id = 0;
  bool sync = false;  // true → 出现在在线区并参与同步
};

struct NoteCloudPendingDelete {
  int note_id = 0;
  // Unix seconds; DeskNote 下次启动扫描，到期则 VaultHttpDeleteNote。
  long long delete_after = 0;
};

struct NoteCloudSyncStore {
  std::vector<NoteCloudLink> links;
  std::vector<NoteCloudPendingDelete> pending_deletes;
};

NoteCloudSyncStore LoadNoteCloudSync();
bool SaveNoteCloudSync(const NoteCloudSyncStore& store);

const NoteCloudLink* FindNoteCloudLink(const NoteCloudSyncStore& store,
                                       const std::wstring& path);
NoteCloudLink* FindNoteCloudLinkMut(NoteCloudSyncStore* store,
                                    const std::wstring& path);

void NoteCloudUpsertLink(NoteCloudSyncStore* store, const std::wstring& path,
                         int note_id, bool sync);
void NoteCloudScheduleDelete(NoteCloudSyncStore* store, int note_id,
                             long long delete_after_unix);
// Deletes expired remote notes (best-effort). Returns how many deleted.
int NoteCloudSweepPendingDeletes(NoteCloudSyncStore* store);

long long NoteCloudUnixNow();
long long NoteCloudUnixAfterHours(int hours);
