#pragma once

#include <map>
#include <string>
#include <utility>
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
// Follow a renamed/moved local file so its cloud link keeps working. Without
// this the link keeps the stale path, PathIsCloudSynced() goes false, saves
// stop being pushed, and the note silently drops out of the online section.
// Returns true when a link was moved (caller should persist the store).
bool NoteCloudRetargetLink(NoteCloudSyncStore* store,
                           const std::wstring& old_path,
                           const std::wstring& new_path);
void NoteCloudScheduleDelete(NoteCloudSyncStore* store, int note_id,
                             long long delete_after_unix);

long long NoteCloudUnixNow();
long long NoteCloudUnixAfterHours(int hours);

// --- DeskNote cloud-body upload queue -------------------------------------
// Process-scoped (the detached worker outlives any window), so it lives in
// notes_cloud_sync.cpp rather than on NotesWindow. Latest-wins per note id.
void NoteCloudPushEnqueue(int note_id, const std::wstring& title,
                          const std::wstring& body);
// Take everything queued so far; the caller owns the batch.
std::map<int, std::pair<std::wstring, std::wstring>> NoteCloudPushTakeAll();
// Put a batch back without clobbering newer bodies queued meanwhile.
void NoteCloudPushRequeue(
    const std::map<int, std::pair<std::wstring, std::wstring>>& batch);
// Returns true exactly once, when the worker should be started.
bool NoteCloudPushMarkWorkerStarted();
// Call after a batch uploaded successfully: drop those entries from the on-disk
// pending mirror so a later crash recovery never re-uploads a committed body.
void NoteCloudPushCommitBatch(
    const std::map<int, std::pair<std::wstring, std::wstring>>& uploaded);
// Reload any bodies queued but not yet uploaded when a previous run exited.
// Returns true if anything was recovered (caller should start the worker).
bool NoteCloudPushRecoverPending();
// Wake the worker so a parked/backing-off batch retries now.
void NoteCloudNotifyPushWorker();
// Sleep up to `ms`, waking early on a notify. Caller must hold no lock.
void NoteCloudPushWaitForRetry(unsigned ms);
