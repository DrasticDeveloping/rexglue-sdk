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
#include <cstdint>
#include <filesystem>
#include <functional>
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
// queries `stat.pending`, `stat.import`, `stat.txn` and a deletion's `.stat`, and the folder
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
// A durable write of `bytes` to `path`: path.tmp created, written (point <name>.partial after half),
// flushed, closed, reopened and hashed against `expected` (point <name>.tmp; `alias` too when not
// null), then MoveFileExW(tmp, path, REPLACE_EXISTING | WRITE_THROUGH). Any failure deletes the
// temp and returns its Win32 error; the destination is unchanged.
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
