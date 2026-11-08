/**
 * The saved-game import journal and the commit counter (Swap Force Online stage 1C, item 3; the
 * design is docs/online-1C.md section 3.3 in the game's repository). See save_import.h.
 *
 * The steps of an import (a fault point after each, I1 to I11; I11 is the caller's):
 *   0. checks (no write but finishing a committed folder);
 *   1. make import/<id>/                                                      I1
 *   2. durable write of old.xsg (point old.xsg.tmp is I2), I3; of old.header, I4
 *   3. durable write of new.xsg                                               I5
 *   4. durable write of txn, phase prepared                                   I6
 *   5. under the lock: not open, no pending/<id>, the live hash still old; one MoveFileExW replace
 *      of new.xsg over the live file (retried three times 100 ms apart; no copy fallback)  I7
 *   6. read-back of the live file and the header's hash; txn phase installed  I8
 *   7. txn phase committed (success only after this rename returned)          I9
 *   8. previous-import/<id> removed, I10, import/<id> renamed to it.
 * Refusals up to and including step 5 leave the live file and the header byte-equal; failures in
 * step 6 or 7 return X_ERROR_IO_INCOMPLETE and keep the folder (one policy, design D24); a failure
 * in step 8 is success with cleanup pending.
 */

#include <rex/system/xam/save_import.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace rex::system::xam {

namespace {

std::atomic<ImportFaultHook> g_fault_hook{nullptr};

constexpr const char* kBackupDir = "save-backup";

// Win32 errors the journal's own refusals use (the caller logs the report's reason).
constexpr uint32_t kErrMismatch = 0x0000000DL;      // ERROR_INVALID_DATA
constexpr uint32_t kErrBusy = 0x000000AAL;          // ERROR_BUSY (open, pending, unresolved)
constexpr uint32_t kErrNotSupported = 0x00000032L;  // ERROR_NOT_SUPPORTED (volume)

// A path below the content root, with generic separators (the record's form).
std::string RelativeGeneric(const std::filesystem::path& root, const std::filesystem::path& p) {
  std::string s = rex::path_to_utf8(p.lexically_relative(root));
  std::replace(s.begin(), s.end(), '\\', '/');
  return s;
}

}  // namespace

std::string Sha256Hex(const Sha256& sha) {
  std::string s;
  s.reserve(64);
  for (uint8_t b : sha) s += fmt::format("{:02x}", b);
  return s;
}

bool ParseSha256Hex(std::string_view text, Sha256* out) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    unsigned v = 0;
    for (size_t k = 0; k < 2; ++k) {
      const char c = text[i * 2 + k];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
      else return false;
    }
    (*out)[i] = static_cast<uint8_t>(v);
  }
  return true;
}

void SetImportFaultHook(ImportFaultHook hook) { g_fault_hook.store(hook, std::memory_order_release); }

uint32_t RunImportFaultPoint(const char* point) {
  const ImportFaultHook hook = g_fault_hook.load(std::memory_order_acquire);
  return hook ? hook(point) : 0;
}

std::string FormatImportTxn(const ImportTxn& txn) {
  std::string body = fmt::format(
      "format 1\nphase {}\nseq {}\npackage {}\nheader {}\nold_sha {}\nnew_sha {}\nheader_sha {}\n",
      txn.phase, txn.seq, txn.package, txn.header, Sha256Hex(txn.old_sha), Sha256Hex(txn.new_sha),
      Sha256Hex(txn.header_sha));
  Sha256 sum{};
  Sha256Of(std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()), &sum);
  return body + "sha " + Sha256Hex(sum) + "\n";
}

bool ParseImportTxn(std::string_view text, ImportTxn* out) {
  const size_t sum_at = text.rfind("sha ", text.size() >= 2 ? text.size() - 2 : 0);
  if (sum_at == std::string_view::npos || (sum_at != 0 && text[sum_at - 1] != '\n')) return false;
  const std::string_view body = text.substr(0, sum_at);
  std::string_view sum_line = text.substr(sum_at + 4);
  if (sum_line.empty() || sum_line.back() != '\n') return false;
  sum_line.remove_suffix(1);
  Sha256 want{}, got{};
  if (!ParseSha256Hex(sum_line, &want)) return false;
  if (!Sha256Of(std::span(reinterpret_cast<const uint8_t*>(body.data()), body.size()), &got) ||
      got != want) {
    return false;
  }
  // The body's eight lines, in order.
  static constexpr const char* kKeys[] = {"format", "phase",   "seq",     "package",
                                          "header", "old_sha", "new_sha", "header_sha"};
  std::string_view rest = body;
  ImportTxn txn;
  for (const char* key : kKeys) {
    const size_t nl = rest.find('\n');
    if (nl == std::string_view::npos) return false;
    const std::string_view line = rest.substr(0, nl);
    rest.remove_prefix(nl + 1);
    const std::string_view k(key);
    if (line.size() <= k.size() || line.substr(0, k.size()) != k || line[k.size()] != ' ') {
      return false;
    }
    const std::string_view value = line.substr(k.size() + 1);
    if (k == "format") {
      if (value != "1") return false;
    } else if (k == "phase") {
      if (value != "prepared" && value != "installed" && value != "committed") return false;
      txn.phase = std::string(value);
    } else if (k == "seq") {
      if (value.empty() || value.size() > 10) return false;
      uint64_t n = 0;
      for (char c : value) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + uint64_t(c - '0');
      }
      if (n > 0xFFFFFFFFull) return false;
      txn.seq = static_cast<uint32_t>(n);
    } else if (k == "package") {
      txn.package = std::string(value);
    } else if (k == "header") {
      txn.header = std::string(value);
    } else if (k == "old_sha") {
      if (!ParseSha256Hex(value, &txn.old_sha)) return false;
    } else if (k == "new_sha") {
      if (!ParseSha256Hex(value, &txn.new_sha)) return false;
    } else if (!ParseSha256Hex(value, &txn.header_sha)) {
      return false;
    }
  }
  if (!rest.empty()) return false;
  *out = std::move(txn);
  return true;
}

uint64_t CommitCounter::Raise(const std::string& id) {
  std::lock_guard lock(mutex_);
  const uint64_t count = ++counts_[id];
  raises_.push_back({std::this_thread::get_id(), id, count});
  if (raises_.size() > kThreadRecords) raises_.erase(raises_.begin());
  return count;
}

uint64_t CommitCounter::Current(const std::string& id) const {
  std::lock_guard lock(mutex_);
  auto it = counts_.find(id);
  return it == counts_.end() ? 0 : it->second;
}

uint64_t CommitCounter::RaisedByThisThread(const std::string& id) const {
  std::lock_guard lock(mutex_);
  const auto self = std::this_thread::get_id();
  for (auto it = raises_.rbegin(); it != raises_.rend(); ++it) {
    if (it->thread == self && it->id == id) return it->count;
  }
  return 0;
}

#if REX_PLATFORM_WIN32

bool Sha256Of(std::span<const uint8_t> bytes, Sha256* out) {
  // BCryptHash (Windows 10+) in one call; the algorithm pseudo-handle needs no open or close.
  return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                   const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()),
                                   out->data(), ULONG(out->size())));
}

uint32_t ReadWholeFile(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError();
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size) || size.QuadPart > (int64_t(1) << 30)) {
    const uint32_t err = size.QuadPart > (int64_t(1) << 30) ? kErrMismatch : GetLastError();
    CloseHandle(h);
    return err;
  }
  out->assign(size_t(size.QuadPart), 0);
  size_t done = 0;
  while (done < out->size()) {
    DWORD got = 0;
    const DWORD want = DWORD(std::min<size_t>(out->size() - done, 1u << 20));
    if (!ReadFile(h, out->data() + done, want, &got, nullptr)) {
      const uint32_t err = GetLastError();
      CloseHandle(h);
      return err;
    }
    if (got == 0) break;
    done += got;
  }
  CloseHandle(h);
  if (done != out->size()) return kErrMismatch;
  return 0;
}

namespace {

// The SHA-256 and length of a file; false if it cannot be read.
bool HashFile(const std::filesystem::path& path, Sha256* sha, uint64_t* size = nullptr) {
  std::vector<uint8_t> bytes;
  if (ReadWholeFile(path, &bytes) != 0) return false;
  if (size) *size = bytes.size();
  return Sha256Of(bytes, sha);
}

std::filesystem::path TmpOf(const std::filesystem::path& path) {
  std::filesystem::path tmp = path;
  tmp += L".tmp";
  return tmp;
}

bool WriteAll(HANDLE h, const uint8_t* data, size_t size) {
  size_t done = 0;
  while (done < size) {
    DWORD wrote = 0;
    const DWORD want = DWORD(std::min<size_t>(size - done, 1u << 20));
    if (!WriteFile(h, data + done, want, &wrote, nullptr) || wrote == 0) return false;
    done += wrote;
  }
  return true;
}

// The volume a path is on and whether it is NTFS.
bool VolumeOf(const std::filesystem::path& path, std::wstring* volume, bool* ntfs) {
  wchar_t vol[MAX_PATH + 1] = {};
  if (!GetVolumePathNameW(path.c_str(), vol, MAX_PATH)) return false;
  wchar_t fs[MAX_PATH + 1] = {};
  if (!GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, nullptr, fs, MAX_PATH)) return false;
  *volume = vol;
  std::transform(volume->begin(), volume->end(), volume->begin(), ::towlower);
  *ntfs = _wcsicmp(fs, L"NTFS") == 0;
  return true;
}

std::string TimeStamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &now);
  return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

}  // namespace

uint32_t DurableWrite(const std::filesystem::path& path, std::span<const uint8_t> bytes,
                      const Sha256& expected, const std::string& name, const char* alias) {
  const auto tmp = TmpOf(path);
  auto fail = [&](uint32_t err) {
    DeleteFileW(tmp.c_str());
    return err ? err : kErrMismatch;
  };
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return fail(GetLastError());
  const size_t half = bytes.size() / 2;
  if (!WriteAll(h, bytes.data(), half)) {
    const uint32_t err = GetLastError();
    CloseHandle(h);
    return fail(err);
  }
  if (uint32_t err = RunImportFaultPoint((name + ".partial").c_str())) {
    CloseHandle(h);
    return fail(err);
  }
  if (!WriteAll(h, bytes.data() + half, bytes.size() - half) || !FlushFileBuffers(h)) {
    const uint32_t err = GetLastError();
    CloseHandle(h);
    return fail(err);
  }
  CloseHandle(h);
  Sha256 got{};
  uint64_t size = 0;
  if (!HashFile(tmp, &got, &size) || size != bytes.size() || got != expected) {
    return fail(kErrMismatch);
  }
  if (uint32_t err = RunImportFaultPoint((name + ".tmp").c_str())) return fail(err);
  if (alias) {
    if (uint32_t err = RunImportFaultPoint(alias)) return fail(err);
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return fail(GetLastError());
  }
  return 0;
}

uint32_t DeleteImportFolder(const std::filesystem::path& dir, const std::string& op) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return 0;
  std::vector<std::filesystem::path> files;
  bool has_txn = false;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (entry.path().filename() == L"txn") {
      has_txn = true;
    } else {
      files.push_back(entry.path());
    }
  }
  if (ec) return ERROR_ACCESS_DENIED;
  std::sort(files.begin(), files.end());
  bool first = true;
  for (const auto& f : files) {
    if (!DeleteFileW(f.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return GetLastError();
    if (first) {
      first = false;
      if (uint32_t err = RunImportFaultPoint((op + ".first").c_str())) return err;
    }
  }
  if (uint32_t err = RunImportFaultPoint((op + ".txn").c_str())) return err;
  if (has_txn) {
    const auto txn = dir / L"txn";
    if (!DeleteFileW(txn.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return GetLastError();
  }
  if (!RemoveDirectoryW(dir.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return GetLastError();
  return 0;
}

SaveImportJournal::SaveImportJournal(std::filesystem::path root) : root_(std::move(root)) {}

std::filesystem::path SaveImportJournal::ImportDir(const std::string& id) const {
  return root_ / kBackupDir / "import" / rex::to_path(id);
}
std::filesystem::path SaveImportJournal::PreviousDir(const std::string& id) const {
  return root_ / kBackupDir / "previous-import" / rex::to_path(id);
}
std::filesystem::path SaveImportJournal::PendingDir(const std::string& id) const {
  return root_ / kBackupDir / "pending" / rex::to_path(id);
}

bool SaveImportJournal::IsUnresolved(const std::string& id) const {
  std::lock_guard lock(mutex_);
  return unresolved_.count(id) != 0;
}

uint32_t SaveImportJournal::FinishCommitted(const std::string& id, const char* op) {
  if (uint32_t err = DeleteImportFolder(PreviousDir(id), op)) return err;
  const bool recovery = std::string_view(op) == "prev-r";
  RunImportFaultPoint(recovery ? "R2" : "I10");
  if (!recovery) {
    if (uint32_t err = RunImportFaultPoint("prev.rename")) return err;
  }
  std::error_code ec;
  std::filesystem::create_directories(PreviousDir(id).parent_path(), ec);
  if (!MoveFileExW(ImportDir(id).c_str(), PreviousDir(id).c_str(), MOVEFILE_WRITE_THROUGH)) {
    return GetLastError();
  }
  return 0;
}

X_RESULT SaveImportJournal::Abort(const std::string& id, X_RESULT result, const std::string& reason,
                                  ImportReport* report) {
  if (uint32_t err = DeleteImportFolder(ImportDir(id), "abort")) {
    REXSYS_ERROR("Content: the refused import of {} left its folder ({}); the next start settles it",
                 id, err);
  }
  if (report) report->reason = reason;
  return result;
}

X_RESULT SaveImportJournal::Import(const Request& rq, const LockedRunner& locked,
                                   ImportReport* report) {
  std::lock_guard serial(mutex_);
  ImportReport scratch;
  if (!report) report = &scratch;
  auto refuse = [&](uint32_t err, const char* reason) {
    report->reason = reason;
    return X_RESULT_FROM_WIN32(err);
  };

  // ---- Step 0: checks. ----
  if (unresolved_.count(rq.id)) return refuse(kErrBusy, "unresolved");
  if (orphaned_.count(rq.id)) return refuse(kErrBusy, "orphaned");
  std::vector<uint8_t> staged;
  Sha256 sha{};
  if (ReadWholeFile(rq.staged, &staged) != 0) return refuse(ERROR_FILE_NOT_FOUND, "staged unreadable");
  if (staged.size() != rq.file_size) return refuse(kErrMismatch, "staged length");
  if (!Sha256Of(staged, &sha) || sha != rq.new_sha) return refuse(kErrMismatch, "staged hash");
  std::vector<uint8_t> live;
  if (ReadWholeFile(rq.package_file, &live) != 0) return refuse(ERROR_FILE_NOT_FOUND, "live unreadable");
  if (live.size() != rq.file_size) return refuse(kErrMismatch, "live length");
  Sha256 live_sha{};
  if (!Sha256Of(live, &live_sha)) return refuse(kErrMismatch, "live hash");
  {
    std::wstring staged_vol, live_vol;
    bool staged_ntfs = false, live_ntfs = false;
    if (!VolumeOf(rq.staged, &staged_vol, &staged_ntfs) ||
        !VolumeOf(rq.package_file, &live_vol, &live_ntfs)) {
      return refuse(kErrNotSupported, "volume unknown");
    }
    if (staged_vol != live_vol) return refuse(kErrNotSupported, "another volume");
    if (!live_ntfs || RunImportFaultPoint("volume") != 0) return refuse(kErrNotSupported, "not NTFS");
  }
  std::error_code ec;
  if (std::filesystem::exists(PendingDir(rq.id), ec)) return refuse(kErrBusy, "native pending");
  if (std::filesystem::exists(ImportDir(rq.id), ec)) {
    // Only a committed import whose cleanup failed (live = its new file) is finished here.
    std::vector<uint8_t> text;
    ImportTxn old_txn;
    const bool ok = ReadWholeFile(ImportDir(rq.id) / L"txn", &text) == 0 &&
                    ParseImportTxn(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()),
                                   &old_txn);
    if (!ok || old_txn.phase != "committed" || old_txn.new_sha != live_sha) {
      return refuse(kErrBusy, "unresolved");
    }
    if (FinishCommitted(rq.id, "prev") != 0) return refuse(kErrBusy, "cleanup pending");
    report->finished_earlier = true;
    REXSYS_INFO("Content: the import of {} seq {} was finished", rq.id, old_txn.seq);
  }
  if (live_sha != rq.base_sha) return refuse(kErrMismatch, "base differs");
  {
    const uint32_t open_check = locked([](bool open) { return open ? kErrBusy : 0u; });
    if (open_check) return refuse(open_check, "open");
  }
  std::vector<uint8_t> header;
  if (ReadWholeFile(rq.header_file, &header) != 0) return refuse(ERROR_FILE_NOT_FOUND, "header unreadable");
  Sha256 header_sha{};
  if (!Sha256Of(header, &header_sha)) return refuse(kErrMismatch, "header hash");

  // ---- Steps 1 to 4: the folder, the copies, the prepared record. ----
  const auto dir = ImportDir(rq.id);
  std::filesystem::create_directories(dir, ec);
  if (ec || !std::filesystem::is_directory(dir, ec)) return refuse(ERROR_ACCESS_DENIED, "folder");
  RunImportFaultPoint("I1");
  if (uint32_t err = DurableWrite(dir / L"old.xsg", live, rq.base_sha, "old.xsg", "I2")) {
    return Abort(rq.id, X_RESULT_FROM_WIN32(err), "old copy", report);
  }
  RunImportFaultPoint("I3");
  if (uint32_t err = DurableWrite(dir / L"old.header", header, header_sha, "old.header")) {
    return Abort(rq.id, X_RESULT_FROM_WIN32(err), "header copy", report);
  }
  RunImportFaultPoint("I4");
  if (uint32_t err = DurableWrite(dir / L"new.xsg", staged, rq.new_sha, "new.xsg")) {
    return Abort(rq.id, X_RESULT_FROM_WIN32(err), "new copy", report);
  }
  RunImportFaultPoint("I5");
  ImportTxn txn;
  txn.phase = "prepared";
  txn.seq = rq.seq;
  txn.package = RelativeGeneric(root_, rq.package_file);
  txn.header = RelativeGeneric(root_, rq.header_file);
  txn.old_sha = rq.base_sha;
  txn.new_sha = rq.new_sha;
  txn.header_sha = header_sha;
  auto write_txn = [&](const char* phase) {
    txn.phase = phase;
    const std::string text = FormatImportTxn(txn);
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    Sha256 want{};
    Sha256Of(bytes, &want);
    return DurableWrite(dir / L"txn", bytes, want, std::string("txn.") + phase);
  };
  if (uint32_t err = write_txn("prepared")) {
    return Abort(rq.id, X_RESULT_FROM_WIN32(err), "prepared record", report);
  }
  RunImportFaultPoint("I6");

  // ---- Step 5: the install, under the lock. ----
  const auto new_file = dir / L"new.xsg";
  enum class Check { kNone, kOpen, kPending, kBase };
  Check failed = Check::kNone;
  uint32_t rename_err = 0;
  for (int attempt = 0; attempt < 4; ++attempt) {
    if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    failed = Check::kNone;
    rename_err = locked([&](bool open) -> uint32_t {
      if (open) {
        failed = Check::kOpen;
        return kErrBusy;
      }
      std::error_code lec;
      if (std::filesystem::exists(PendingDir(rq.id), lec)) {
        failed = Check::kPending;
        return kErrBusy;
      }
      Sha256 now{};
      if (!HashFile(rq.package_file, &now) || now != rq.base_sha) {
        failed = Check::kBase;
        return kErrMismatch;
      }
      if (uint32_t err = RunImportFaultPoint("install")) return err;
      if (!MoveFileExW(new_file.c_str(), rq.package_file.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return GetLastError();
      }
      return 0;
    });
    if (rename_err == 0 || failed != Check::kNone) break;
  }
  if (failed == Check::kOpen) return Abort(rq.id, X_RESULT_FROM_WIN32(kErrBusy), "open", report);
  if (failed == Check::kPending) return Abort(rq.id, X_RESULT_FROM_WIN32(kErrBusy), "native pending", report);
  if (failed == Check::kBase) return Abort(rq.id, X_RESULT_FROM_WIN32(kErrMismatch), "base differs", report);
  if (rename_err) return Abort(rq.id, X_RESULT_FROM_WIN32(rename_err), "install refused", report);
  RunImportFaultPoint("I7");

  // ---- Steps 6 and 7: after the replace, one policy (X_ERROR_IO_INCOMPLETE, folder kept). ----
  auto incomplete = [&](const char* reason) {
    unresolved_.insert(rq.id);
    report->reason = reason;
    REXSYS_ERROR("Content: the import of {} seq {} is installed but not committed ({}); the next "
                 "start settles it",
                 rq.id, rq.seq, reason);
    return X_ERROR_IO_INCOMPLETE;
  };
  {
    Sha256 back{};
    uint64_t back_size = 0;
    const bool read_ok = HashFile(rq.package_file, &back, &back_size);
    const bool injected = RunImportFaultPoint("readback") != 0;
    if (!read_ok || injected || back_size != rq.file_size || back != rq.new_sha) {
      return incomplete("read-back");
    }
    Sha256 header_now{};
    if (!HashFile(rq.header_file, &header_now) || header_now != header_sha) {
      return incomplete("header changed");
    }
  }
  if (write_txn("installed") != 0) return incomplete("installed record");
  RunImportFaultPoint("I8");
  if (write_txn("committed") != 0) return incomplete("committed record");
  RunImportFaultPoint("I9");

  // ---- Step 8: cleanup; a failure here is still a committed import. ----
  if (FinishCommitted(rq.id, "prev") != 0) {
    report->cleanup_pending = true;
    REXSYS_WARN("Content: the import of {} seq {} is committed; its cleanup is pending", rq.id, rq.seq);
  }
  return X_ERROR_SUCCESS;
}

void SaveImportJournal::Recover() {
  std::lock_guard serial(mutex_);
  const auto import_root = root_ / kBackupDir / "import";
  std::error_code ec;
  if (!std::filesystem::is_directory(import_root, ec)) return;
  std::vector<std::filesystem::path> dirs;
  for (const auto& entry : std::filesystem::directory_iterator(import_root, ec)) {
    if (entry.is_directory(ec)) dirs.push_back(entry.path());
  }
  for (const auto& dir : dirs) {
    const std::string id = rex::path_to_utf8(dir.filename());
    enum class Decision { kFinish, kDiscard, kOrphan } decision = Decision::kOrphan;
    ImportTxn txn;
    std::vector<uint8_t> text;
    const auto txn_path = dir / L"txn";
    if (!std::filesystem::exists(txn_path, ec)) {
      decision = Decision::kDiscard;  // a stop before I6: the install never ran
    } else if (ReadWholeFile(txn_path, &text) == 0 &&
               ParseImportTxn(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()),
                              &txn)) {
      Sha256 live{};
      if (HashFile(root_ / rex::to_path(txn.package), &live)) {
        if (live == txn.new_sha) decision = Decision::kFinish;
        else if (live == txn.old_sha) decision = Decision::kDiscard;
      }
    }
    switch (decision) {
      case Decision::kFinish:
        REXSYS_INFO("Content: the import of {} seq {} was finished", id, txn.seq);
        break;
      case Decision::kDiscard:
        REXSYS_INFO("Content: the import of {} seq {} did not happen; the version before it stays", id,
                    txn.seq);
        break;
      case Decision::kOrphan:
        break;
    }
    RunImportFaultPoint("R1");
    uint32_t err = 0;
    std::filesystem::path orphan;
    if (decision == Decision::kFinish) {
      err = FinishCommitted(id, "prev-r");
    } else if (decision == Decision::kDiscard) {
      err = DeleteImportFolder(dir, "discard");
    } else {
      orphan = root_ / kBackupDir / "import-orphaned" / rex::to_path(id + "-" + TimeStamp());
      std::filesystem::create_directories(orphan.parent_path(), ec);
      if (!MoveFileExW(dir.c_str(), orphan.c_str(), MOVEFILE_WRITE_THROUGH)) err = GetLastError();
      orphaned_.insert(id);
    }
    if (err) {
      REXSYS_ERROR("Content: could not settle the import folder {} ({}); imports of it are refused "
                   "until it is gone",
                   rex::path_to_utf8(dir), err);
      continue;
    }
    if (decision == Decision::kOrphan) {
      REXSYS_ERROR("Content: the import of {} matches neither its old nor its new file; the slot is "
                   "left as it is and the folder kept as {}",
                   id, rex::path_to_utf8(orphan));
    }
    RunImportFaultPoint("R3");
  }
}

#else  // !REX_PLATFORM_WIN32

bool Sha256Of(std::span<const uint8_t>, Sha256*) { return false; }
uint32_t ReadWholeFile(const std::filesystem::path&, std::vector<uint8_t>*) { return kErrNotSupported; }
uint32_t DurableWrite(const std::filesystem::path&, std::span<const uint8_t>, const Sha256&,
                      const std::string&, const char*) {
  return kErrNotSupported;
}
uint32_t DeleteImportFolder(const std::filesystem::path&, const std::string&) { return kErrNotSupported; }
SaveImportJournal::SaveImportJournal(std::filesystem::path root) : root_(std::move(root)) {}
std::filesystem::path SaveImportJournal::ImportDir(const std::string& id) const {
  return root_ / kBackupDir / "import" / id;
}
std::filesystem::path SaveImportJournal::PreviousDir(const std::string& id) const {
  return root_ / kBackupDir / "previous-import" / id;
}
std::filesystem::path SaveImportJournal::PendingDir(const std::string& id) const {
  return root_ / kBackupDir / "pending" / id;
}
bool SaveImportJournal::IsUnresolved(const std::string&) const { return false; }
X_RESULT SaveImportJournal::Import(const Request&, const LockedRunner&, ImportReport* report) {
  if (report) report->reason = "not supported";
  return kImportNotSupported;
}
void SaveImportJournal::Recover() {}
uint32_t SaveImportJournal::FinishCommitted(const std::string&, const char*) { return kErrNotSupported; }
X_RESULT SaveImportJournal::Abort(const std::string&, X_RESULT result, const std::string&, ImportReport*) {
  return result;
}

#endif

}  // namespace rex::system::xam
