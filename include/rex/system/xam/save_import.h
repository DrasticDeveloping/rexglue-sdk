/**
 * The saved-game import journal and the commit counter (Swap Force Online stage 1C, item 3).
 *
 * The import replaces one saved game's data file (the slot's `.xsg`) with a file handed over from
 * outside the game, so that a stop of the process at any point leaves the old file or the new one
 * under the live name, never a mix, and never touches the package's header. It is a protocol of
 * its own, under root/save-backup/import/<id>/, and shares nothing with ReplaceContent's journal
 * (save-backup/pending, save-backup/previous), which the plain game's own saves use unchanged.
 *
 * What the import claims and proves is process termination only (TerminateProcess, a crash, End
 * task): see SaveImportJournal::Import. Power loss is designed for (data flushed before every
 * rename that publishes it, write-through renames) but not claimed.
 *
 * Nothing here runs unless a caller asks for an import or a snapshot; recovery at start only looks
 * for root/save-backup/import, which nothing but an import creates.
 */

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/system/xtypes.h>

namespace rex::system::xam {

using Sha256 = std::array<uint8_t, 32>;

// ERROR_NOT_SUPPORTED: the import and the snapshot exist on Windows only.
constexpr X_RESULT kImportNotSupported = X_RESULT_FROM_WIN32(0x00000032L);

std::string Sha256Hex(const Sha256& sha);
bool ParseSha256Hex(std::string_view text, Sha256* out);
// SHA-256 through Windows CNG. False if CNG fails.
bool Sha256Of(std::span<const uint8_t> bytes, Sha256* out);
// Reads a whole file. A Win32 error, 0 on success.
uint32_t ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>* out);

// ---- Fault points (tests and dev only) ---------------------------------------------------------
// The hook is called at each named point of the import and of recovery. 0 continues; a non-zero
// Win32 error makes the operation at that point fail with it (where the point names an operation:
// a durable write's `.partial` and `.tmp`, a deletion's `.first` and `.txn`, `install`, `readback`
// (any non-zero is a mismatch), `prev.rename`, `volume` (any non-zero: not NTFS)); the status
// queries `stat.pending`, `stat.import`, `stat.txn`, a deletion's `.stat` and the snapshot's
// `snap.pending` and `snap.import`, and the folder
// listings `recover.enum` and a deletion's `.enum` (at each entry), fail with the value as their
// error; at the I- and R-points the value is ignored. The hook may end the process (TerminateProcess) or change files
// first. Without a hook every point is one relaxed load.
using ImportFaultHook = uint32_t (*)(const char* point);
void SetImportFaultHook(ImportFaultHook hook);
uint32_t RunImportFaultPoint(const char* point);

// ---- The transaction record ---------------------------------------------------------------------
// save-backup/import/<id>/txn: "format 1", "phase <p>", "seq <n>", "package <relative .xsg>",
// "header <relative header>", "old_sha <hex>", "new_sha <hex>", "header_sha <hex>", then
// "sha <hex>", the SHA-256 of the lines above it (each with its "\n").
struct ImportTxn {
  std::string phase;  // prepared, installed, committed
  uint32_t seq = 0;
  std::string package;  // relative to the content root, generic separators
  std::string header;
  Sha256 old_sha{};
  Sha256 new_sha{};
  Sha256 header_sha{};
};
std::string FormatImportTxn(const ImportTxn& txn);
// False when a line is missing or malformed or the checksum does not hold.
bool ParseImportTxn(std::string_view text, ImportTxn* out);

// ---- Building blocks (exposed for the tests) ---------------------------------------------------
// A durable write of `bytes` to `path`: whatever sits at path.tmp removed by name (DeleteFileW: a
// hard link's partner keeps its bytes), path.tmp created new (CREATE_NEW, refused when it has a
// second hard link), written (point <name>.partial after half), flushed, closed, reopened and
// hashed against `expected` (point <name>.tmp; `alias` too when not null), then MoveFileExW(tmp,
// path, REPLACE_EXISTING | WRITE_THROUGH). Any failure deletes the temp's name and returns its
// Win32 error; the destination is unchanged, and no other file is ever written through the temp.
uint32_t DurableWrite(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                      const Sha256& expected, const std::string& name, const char* alias = nullptr);
// Deletes one of the import's own folders (files only): every file but `txn` in turn (point
// <op>.first after the first), then point <op>.txn, then `txn`, then the empty folder. A missing
// folder is success; a folder whose status or listing fails is an error with nothing deleted.
// A Win32 error, 0 on success.
uint32_t DeleteImportFolder(const std::filesystem::path& dir, const std::string& op);

// ---- The journal --------------------------------------------------------------------------------
struct ImportReport {
  std::string reason;          // a refusal's or a failure's reason, for the caller's line
  bool cleanup_pending = false;  // success, step 8 failed (the folder stays committed)
  bool finished_earlier = false;  // step 0 finished a committed folder first
};

class SaveImportJournal {
 public:
  explicit SaveImportJournal(std::filesystem::path root);

  struct Request {
    std::string id;                       // PackageId of the package folder
    std::filesystem::path package_file;   // the live data file (absolute)
    std::filesystem::path header_file;    // the package's header (absolute; never written)
    std::filesystem::path staged;         // the file to install (absolute)
    Sha256 new_sha{};
    Sha256 base_sha{};
    uint32_t seq = 0;
    uint64_t file_size = 0;               // the exact length both files must have
    // Swap Force Online online_save_to_host (HostImportJournal only; the 1C journal ignores both): the
    // header's hash the caller bound, and the caller's publish: when set, the replace runs inside it,
    // under the slot write lock and the content manager's lock, right after the import's last checks; the
    // caller decides there whether its binding still holds (and records the install in the same breath)
    // and calls `replace` only if it does. Returning without a replace refuses the import ("binding
    // changed"). The caller must not take the content manager's lock or a slot lock inside it.
    Sha256 base_header_sha{};
    std::function<bool(const std::function<bool()>& replace)> publish;
  };
  // Runs `body` with the content manager's lock held; `body` gets whether the package is open.
  using LockedRunner = std::function<uint32_t(const std::function<uint32_t(bool open)>& body)>;

  // X_ERROR_SUCCESS: the committed record is durable and the live file is the new one (cleanup
  // may be pending, see the report). X_ERROR_IO_INCOMPLETE: the replace landed but the
  // transaction is not committed; the folder stays for recovery and the package is refused for the
  // rest of the process. Anything else is a refusal: the live file and the header are byte-equal to
  // before.
  X_RESULT Import(const Request& request, const LockedRunner& locked, ImportReport* report);

  // Settles every save-backup/import/<id>/ by the live file's hash against its record: finished
  // (live = new: moved to previous-import/<id>), did not happen (live = old, or no record:
  // deleted), or orphaned (anything else: moved to import-orphaned/<id>-<time>). Never writes the
  // live file or the header. Each outcome is one delete or one rename after the decision. A record
  // whose status cannot be read keeps its folder and refuses its package; a listing that fails
  // keeps every folder and refuses every import for the process. Never throws.
  void Recover();

  bool IsUnresolved(const std::string& id) const;

  // Whether neither the native journal's pending folder nor this journal's import folder exists
  // for `id`, each established by its own status query (points `snap.pending`, `snap.import`).
  // Only a plain not-found counts as absent; a status that cannot be read (or anything else at
  // the path) counts as present, so ContentManager::SnapshotSavedGame refuses then.
  bool FoldersAbsent(const std::string& id) const;

  std::filesystem::path ImportDir(const std::string& id) const;
  std::filesystem::path PreviousDir(const std::string& id) const;
  std::filesystem::path PendingDir(const std::string& id) const;  // the native journal's

 private:
  // Step 8's two operations on a committed folder: previous-import/<id> removed, import/<id>
  // renamed to it. A Win32 error, 0 on success.
  uint32_t FinishCommitted(const std::string& id, const char* op);
  X_RESULT Abort(const std::string& id, X_RESULT result, const std::string& reason,
                 ImportReport* report);
  void RecoverLocked();  // Recover's body, the lock held

  std::filesystem::path root_;
  mutable std::mutex mutex_;
  // X_ERROR_IO_INCOMPLETE in this process, or a folder recovery kept (its record unreadable).
  std::unordered_set<std::string> unresolved_;
  std::unordered_set<std::string> orphaned_;    // orphaned by this process's recovery
  bool recovery_incomplete_ = false;            // recovery could not list or finish: refuse all
};

// ==== Swap Force Online online_save_to_host ======================================================
// Everything below is used only by a process that a session armed (client/save_to_host.cpp Bind). A process
// that is never armed (the default, offline play, the setting off, a guest, the server) never reaches any
// of it: no lock, no folder, no wait, start-up included. The 1C journal above is the online branch's own.

// Armed for this process: set when a session binds the host's slot, cleared when the binding ends
// (ContentManager::DisarmSaveToHost).
void SetSaveToHostArmed();
bool SaveToHostArmed();
void ClearSaveToHostArmed();

// The slot write lock: a cross-process lock on one saved-game package, the file
// <content root>\save-backup\locks\<id>.lock opened with no sharing and delete-on-close, so it belongs to
// its handle (not to a thread) and ends with the process. Taken only in an armed process: by the host
// import (HostImportJournal, for its whole run), by a native write of a saved game, by the host import's
// recovery and by the game's load of the bound slot. A writer of another build does not take it; the host
// import refuses its save when the live file changed under it instead.
class SlotWriteLock {
 public:
  // The lock, waiting up to `wait` for another holder; nullptr when it was not obtained. Off Windows:
  // nullptr.
  static std::unique_ptr<SlotWriteLock> Acquire(const std::filesystem::path& content_root, const std::string& id,
                                                std::chrono::milliseconds wait);
  ~SlotWriteLock();
  SlotWriteLock(const SlotWriteLock&) = delete;
  SlotWriteLock& operator=(const SlotWriteLock&) = delete;

 private:
  explicit SlotWriteLock(void* handle) : handle_(handle) {}
  void* handle_ = nullptr;
};

// The host import (online_save_to_host): the host's staged save into its own live slot, journaled in its
// own folders (save-backup\host-import, host-previous-import, host-import-orphaned), never the 1C journal's.
// Its steps are the 1C journal's (old copies, new copy, prepared record, one replace of the data file,
// read-back, installed, committed, cleanup); the differences: the header is bound (an import never writes
// it, so the pair stays whole only while the live header is the bound one, byte for byte), the replace
// runs inside the caller's publish, and its recovery runs only when a session binds (Recover), each folder
// under its slot write lock, deciding by both files of the pair.
class HostImportJournal {
 public:
  explicit HostImportJournal(std::filesystem::path root);
  // As SaveImportJournal::Import; refused unless armed. The caller holds the package's slot write lock.
  X_RESULT Import(const SaveImportJournal::Request& request, const SaveImportJournal::LockedRunner& locked,
                  ImportReport* report);
  // Settles this journal's folders; a no-op unless armed (never at start-up).
  void Recover();
  std::filesystem::path ImportDir(const std::string& id) const;
  std::filesystem::path PreviousDir(const std::string& id) const;

 private:
  void RecoverLocked();
  uint32_t FinishCommitted(const std::string& id);
  X_RESULT Abort(const std::string& id, X_RESULT result, const std::string& reason, ImportReport* report);

  std::filesystem::path root_;
  std::mutex mutex_;
  std::unordered_set<std::string> unresolved_;
  std::unordered_set<std::string> orphaned_;
};

// ---- The commit counter -------------------------------------------------------------------------
// A per-package count the content manager raises in CloseContent's detaching lock acquisition when
// that close commits (a package CreateContent made, or one with a pending replacement), with the
// thread that raised it. A snapshot reads the count with the bytes; the closing thread asks which
// count it raised, so a commit's cause is bound to its own count.
class CommitCounter {
 public:
  uint64_t Raise(const std::string& id);
  uint64_t Current(const std::string& id) const;
  // The latest count the calling thread raised for `id`, 0 if none.
  uint64_t RaisedByThisThread(const std::string& id) const;

 private:
  struct ThreadRaise {
    std::thread::id thread;
    std::string id;
    uint64_t count;
  };
  static constexpr size_t kThreadRecords = 64;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, uint64_t> counts_;
  std::vector<ThreadRaise> raises_;  // newest last, at most kThreadRecords
};

}  // namespace rex::system::xam
