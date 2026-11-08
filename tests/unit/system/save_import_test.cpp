/**
 * The saved-game import journal (Swap Force Online stage 1C, item 3; the game's docs/online-1C.md
 * sections 3.3, 5.4 and 6): every named fault point, each run in a child process of this test
 * binary that ends itself with TerminateProcess at that point, then a new content manager on the
 * same folder; the racing kill; the post-install policy; cleanup pending; error injection; the
 * binding; recovery's orphans; the snapshot's refusals and the commit counter.
 *
 * A child is this binary run with the hidden test "save import child" and the environment variable
 * REX_SAVE_IMPORT_CHILD = "<mode>|<root>|<point>|<arg>". A child that reaches its armed point exits
 * with kHitCode; one that never reaches it exits with Catch's own code, and the case fails.
 *
 * Everything runs in a fresh folder under the temp directory; nothing outside it is touched.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/string.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/save_import.h>

#if REX_PLATFORM_WIN32
#include <windows.h>

namespace fs = std::filesystem;
using namespace rex::system::xam;
using rex::X_RESULT;
using rex::system::XContentType;

namespace {

constexpr uint64_t kXuid = 0xE000000000000001ull;
constexpr uint64_t kOtherXuid = 0xE000000000000002ull;
constexpr uint32_t kTitle = 0x415608E9;
constexpr uint64_t kSize = 102400;
constexpr DWORD kHitCode = 0x5AFE;
constexpr const char* kChildEnv = "REX_SAVE_IMPORT_CHILD";
constexpr const char* kId = "E000000000000001_415608E9_00000001_SWAPFORCE_1";

std::vector<uint8_t> Content(uint8_t seed) {
  std::vector<uint8_t> v(kSize);
  for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint8_t>(i * 31u + seed * 7u + (i >> 9));
  return v;
}

Sha256 ShaOf(const std::vector<uint8_t>& bytes) {
  Sha256 sha{};
  REQUIRE(Sha256Of(bytes, &sha));
  return sha;
}

void PutFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
  fs::create_directories(path.parent_path());
  FILE* f = nullptr;
  _wfopen_s(&f, path.c_str(), L"wb");
  REQUIRE(f != nullptr);
  fwrite(bytes.data(), 1, bytes.size(), f);
  fclose(f);
}

std::vector<uint8_t> GetFile(const fs::path& path) {
  std::vector<uint8_t> bytes;
  if (ReadWholeFile(path, &bytes) != 0) bytes.clear();
  return bytes;
}

// The test hook: records points, runs an action, stops the process or fails an operation.
struct HookState {
  std::string stop_at;
  std::string fail_at;
  uint32_t fail_code = 0;
  bool fail_always = false;  // keep failing after the first time
  std::function<void(const char*)> on_point;
  std::vector<std::string> seen;
};
HookState g_hook;

uint32_t TestHook(const char* point) {
  g_hook.seen.emplace_back(point);
  if (g_hook.on_point) g_hook.on_point(point);
  if (!g_hook.stop_at.empty() && g_hook.stop_at == point) {
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), kHitCode);
  }
  if (!g_hook.fail_at.empty() && g_hook.fail_at == point) {
    const uint32_t code = g_hook.fail_code;
    if (!g_hook.fail_always) g_hook.fail_at.clear();
    return code;
  }
  return 0;
}

struct HookGuard {
  HookGuard() {
    g_hook = HookState{};
    SetImportFaultHook(&TestHook);
  }
  ~HookGuard() {
    SetImportFaultHook(nullptr);
    g_hook = HookState{};
  }
};

// A fresh content root with one profile's slot 1 (and optionally a second profile).
struct Slot {
  fs::path root;
  fs::path live;
  fs::path header;
  std::vector<uint8_t> header_bytes;

  static fs::path FreshRoot() {
    static int counter = 0;
    fs::path r = fs::temp_directory_path() /
                 ("rex_save_import_" + std::to_string(GetCurrentProcessId()) + "_" +
                  std::to_string(counter++));
    std::error_code ec;
    fs::remove_all(r, ec);
    fs::create_directories(r);
    return r;
  }

  static fs::path LiveOf(const fs::path& root, uint64_t xuid) {
    char x[17];
    std::snprintf(x, sizeof x, "%016llX", static_cast<unsigned long long>(xuid));
    return root / x / "415608E9" / "00000001" / "SWAPFORCE_1" / "SWAPFORCE_1.xsg";
  }
  static fs::path HeaderOf(const fs::path& root, uint64_t xuid) {
    char x[17];
    std::snprintf(x, sizeof x, "%016llX", static_cast<unsigned long long>(xuid));
    return root / x / "415608E9" / "Headers" / "00000001" / "SWAPFORCE_1.header";
  }
  static std::vector<uint8_t> HeaderBytes(uint64_t header_xuid) {
    XCONTENT_AGGREGATE_DATA data{};
    data.device_id = 1;
    data.content_type = XContentType::kSavedGame;
    data.set_display_name(u"Slot 1");
    data.set_file_name("SWAPFORCE_1");
    data.xuid = header_xuid;
    data.title_id = kTitle;
    std::vector<uint8_t> bytes(sizeof(data));
    std::memcpy(bytes.data(), &data, sizeof(data));
    return bytes;
  }

  // An existing root (a child's), or a fresh one with content seed 1 and the owner's header.
  explicit Slot(fs::path existing = {}, uint64_t header_xuid = kXuid) {
    root = existing.empty() ? FreshRoot() : existing;
    live = LiveOf(root, kXuid);
    header = HeaderOf(root, kXuid);
    if (existing.empty()) {
      header_bytes = HeaderBytes(header_xuid);
      PutFile(live, Content(1));
      PutFile(header, header_bytes);
    } else {
      header_bytes = GetFile(header);
    }
  }

  SaveImportBinding Binding() const {
    SaveImportBinding b;
    b.xuid = kXuid;
    b.title_id = kTitle;
    b.file_name = "SWAPFORCE_1";
    b.package_file = live;
    b.header_file = header;
    b.file_size = kSize;
    return b;
  }

  fs::path Stage(const std::vector<uint8_t>& bytes, const std::string& name) const {
    const fs::path p = root / "staged" / (name + ".xsg");
    PutFile(p, bytes);
    return p;
  }

  fs::path ImportDir() const { return root / "save-backup" / "import" / kId; }
  fs::path PreviousDir() const { return root / "save-backup" / "previous-import" / kId; }
  fs::path OrphanRoot() const { return root / "save-backup" / "import-orphaned"; }

  // One import of `bytes` over whatever the live file holds now.
  X_RESULT Import(ContentManager& cm, const std::vector<uint8_t>& bytes, uint32_t seq,
                  ImportReport* report = nullptr) const {
    const fs::path staged = Stage(bytes, "seq" + std::to_string(seq));
    return cm.ImportSavedGame(Binding(), staged, ShaOf(bytes), ShaOf(GetFile(live)), seq, report);
  }

  ~Slot() = default;
};

void RemoveRoot(const fs::path& root) {
  std::error_code ec;
  fs::remove_all(root, ec);
}

bool HasOrphan(const Slot& slot) {
  std::error_code ec;
  if (!fs::is_directory(slot.OrphanRoot(), ec)) return false;
  for (const auto& e : fs::directory_iterator(slot.OrphanRoot(), ec)) {
    if (e.path().filename().string().rfind(kId, 0) == 0) return true;
  }
  return false;
}

// Runs this binary's hidden child test with the given spec; its exit code.
DWORD RunChild(const std::string& spec) {
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"save import child\"";
  SetEnvironmentVariableA(kChildEnv, spec.c_str());
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi);
  SetEnvironmentVariableA(kChildEnv, nullptr);
  if (!ok) return 0xFFFFFFFF;
  DWORD code = 0xFFFFFFFE;
  if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  else TerminateProcess(pi.hProcess, 0xFFFFFFFD);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code;
}

std::string Spec(const char* mode, const Slot& slot, const std::string& point,
                 const std::string& arg = "") {
  return std::string(mode) + "|" + rex::path_to_utf8(slot.root) + "|" + point + "|" + arg;
}

// A new content manager recovers; then the live file is `want`, the header byte-equal, no import
// folder left.
void ExpectSettled(const Slot& slot, const std::vector<uint8_t>& want) {
  { ContentManager cm(nullptr, slot.root); }
  CHECK(GetFile(slot.live) == want);
  CHECK(GetFile(slot.header) == slot.header_bytes);
  CHECK_FALSE(fs::exists(slot.ImportDir()));
}

// Holds a file open without FILE_SHARE_DELETE (a scanner; a recovery that cannot delete).
struct Holder {
  HANDLE h = INVALID_HANDLE_VALUE;
  explicit Holder(const fs::path& p) {
    h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  }
  ~Holder() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
  }
};

// ---- The child ---------------------------------------------------------------------------------

void ChildMain(const std::string& spec) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t bar = spec.find('|', start);
    parts.push_back(spec.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  parts.resize(4);
  const std::string& mode = parts[0];
  const fs::path root = rex::to_path(parts[1]);
  const std::string& point = parts[2];
  const std::string& arg = parts[3];
  SetImportFaultHook(&TestHook);
  g_hook.stop_at = point;
  Slot slot(root);

  if (mode == "recover") {
    ContentManager cm(nullptr, root);  // recovery runs in the constructor
    return;
  }
  if (mode == "import") {
    ContentManager cm(nullptr, root);
    std::unique_ptr<Holder> hold;
    if (arg == "hold") hold = std::make_unique<Holder>(slot.live);
    if (arg == "third") {
      g_hook.on_point = [&](const char* p) {
        if (std::strcmp(p, "I6") == 0) PutFile(slot.live, Content(9));
      };
    }
    slot.Import(cm, GetFile(root / "staged" / "child.xsg"), 50);
    return;
  }
  if (mode == "race") {
    ContentManager cm(nullptr, root);
    const auto a = GetFile(root / "staged" / "a.xsg");
    const auto b = GetFile(root / "staged" / "b.xsg");
    std::atomic<bool> armed{false};
    g_hook.stop_at.clear();
    g_hook.on_point = [&](const char* p) {
      if (std::strcmp(p, "install") == 0) armed.store(true, std::memory_order_release);
    };
    std::thread killer([&] {
      std::mt19937 rng(static_cast<uint32_t>(GetTickCount64() ^ GetCurrentProcessId()));
      const auto delay = std::chrono::microseconds(std::uniform_int_distribution<int>(0, 2000)(rng));
      while (!armed.load(std::memory_order_acquire)) std::this_thread::yield();
      const auto until = std::chrono::steady_clock::now() + delay;
      while (std::chrono::steady_clock::now() < until) {
      }
      TerminateProcess(GetCurrentProcess(), kHitCode);
    });
    for (uint32_t seq = 100;; ++seq) {
      const auto now = GetFile(slot.live);
      if (slot.Import(cm, now == a ? b : a, seq) != X_ERROR_SUCCESS) {
        TerminateProcess(GetCurrentProcess(), 3);  // an import failed: not a sample of the replace
      }
    }
  }
}

// The clean stop of an import at `point` (child), then recovery: live = want.
void StopImportAt(const std::string& point, bool want_new, const std::string& arg = "") {
  Slot slot;
  const auto old_bytes = Content(1);
  const auto new_bytes = Content(2);
  slot.Stage(new_bytes, "child");
  INFO("point " << point);
  REQUIRE(RunChild(Spec("import", slot, point, arg)) == kHitCode);
  ExpectSettled(slot, want_new ? new_bytes : old_bytes);
  RemoveRoot(slot.root);
}

}  // namespace

TEST_CASE("save import child", "[.][save_import_child]") {
  char buf[2048] = {};
  if (GetEnvironmentVariableA(kChildEnv, buf, sizeof buf) == 0) return;
  ChildMain(buf);
}

// ---- The record ---------------------------------------------------------------------------------

TEST_CASE("import txn record round trip and checksum", "[save_import]") {
  ImportTxn txn;
  txn.phase = "installed";
  txn.seq = 7;
  txn.package = "E000000000000001/415608E9/00000001/SWAPFORCE_1/SWAPFORCE_1.xsg";
  txn.header = "E000000000000001/415608E9/Headers/00000001/SWAPFORCE_1.header";
  txn.old_sha.fill(0x11);
  txn.new_sha.fill(0x22);
  txn.header_sha.fill(0x33);
  const std::string text = FormatImportTxn(txn);
  ImportTxn back;
  REQUIRE(ParseImportTxn(text, &back));
  CHECK(back.phase == "installed");
  CHECK(back.seq == 7);
  CHECK(back.package == txn.package);
  CHECK(back.new_sha == txn.new_sha);
  // One changed byte anywhere breaks the checksum; a truncated record does not parse.
  for (size_t i = 0; i < text.size(); i += 17) {
    std::string bad = text;
    bad[i] = bad[i] == 'x' ? 'y' : 'x';
    CHECK_FALSE(ParseImportTxn(bad, &back));
  }
  CHECK_FALSE(ParseImportTxn(text.substr(0, text.size() - 1), &back));
  CHECK_FALSE(ParseImportTxn("", &back));
}

// ---- Success, sequence of imports ----------------------------------------------------------------

TEST_CASE("import replaces the live file and keeps the header", "[save_import]") {
  HookGuard hook;
  Slot slot;
  ContentManager cm(nullptr, slot.root);
  ImportReport report;
  REQUIRE(slot.Import(cm, Content(2), 1, &report) == X_ERROR_SUCCESS);
  CHECK(GetFile(slot.live) == Content(2));
  CHECK(GetFile(slot.header) == slot.header_bytes);
  CHECK_FALSE(fs::exists(slot.ImportDir()));
  CHECK(GetFile(slot.PreviousDir() / "old.xsg") == Content(1));
  REQUIRE(slot.Import(cm, Content(3), 2, &report) == X_ERROR_SUCCESS);
  CHECK(GetFile(slot.live) == Content(3));
  CHECK(GetFile(slot.PreviousDir() / "old.xsg") == Content(2));
  CHECK_FALSE(report.cleanup_pending);
  RemoveRoot(slot.root);
}

// ---- Every named fault point, a clean stop (5.4) -----------------------------------------------

TEST_CASE("import stopped before the install keeps the old file", "[save_import][fault]") {
  for (const char* point :
       {"I1", "old.xsg.partial", "old.xsg.tmp", "I2", "I3", "old.header.partial", "old.header.tmp",
        "I4", "new.xsg.partial", "new.xsg.tmp", "I5", "txn.prepared.partial", "txn.prepared.tmp",
        "I6", "install"}) {
    StopImportAt(point, false);
  }
}

TEST_CASE("import stopped at or after the install keeps the new file", "[save_import][fault]") {
  for (const char* point : {"I7", "readback", "txn.installed.partial", "txn.installed.tmp", "I8",
                            "txn.committed.partial", "txn.committed.tmp", "I9", "I10",
                            "prev.rename"}) {
    StopImportAt(point, true);
  }
}

TEST_CASE("import stopped inside the old previous-import's removal", "[save_import][fault]") {
  HookGuard hook;
  for (const char* point : {"prev.first", "prev.txn"}) {
    Slot slot;
    {
      ContentManager cm(nullptr, slot.root);
      REQUIRE(slot.Import(cm, Content(2), 1) == X_ERROR_SUCCESS);  // a previous-import exists
    }
    slot.Stage(Content(3), "child");
    INFO("point " << point);
    REQUIRE(RunChild(Spec("import", slot, point)) == kHitCode);
    ExpectSettled(slot, Content(3));
    RemoveRoot(slot.root);
  }
}

TEST_CASE("import stopped inside an abort keeps the old file", "[save_import][fault]") {
  // The live file held without share-delete: the replace is refused four times, then the abort.
  for (const char* point : {"abort.first", "abort.txn"}) StopImportAt(point, false, "hold");
}

// ---- Recovery interrupted (R-points, recovery-side deletions), once and twice -------------------

namespace {
// A first stop at `first` (an import child), then `stops` recovery children stopped at `point`,
// each shown hit; then a recovery that finishes: live = want.
void StopRecoveryAt(const std::string& first, const std::string& point, int stops, bool with_previous,
                    bool want_new) {
  HookGuard hook;
  Slot slot;
  std::vector<uint8_t> old_bytes = Content(1);
  if (with_previous) {
    ContentManager cm(nullptr, slot.root);
    REQUIRE(slot.Import(cm, Content(2), 1) == X_ERROR_SUCCESS);
    old_bytes = Content(2);
  }
  const auto new_bytes = Content(3);
  slot.Stage(new_bytes, "child");
  INFO("first " << first << " point " << point << " stops " << stops);
  REQUIRE(RunChild(Spec("import", slot, first)) == kHitCode);
  for (int i = 0; i < stops; ++i) REQUIRE(RunChild(Spec("recover", slot, point)) == kHitCode);
  ExpectSettled(slot, want_new ? new_bytes : old_bytes);
  RemoveRoot(slot.root);
}
}  // namespace

TEST_CASE("recovery stopped once at each point repeats its decision", "[save_import][fault]") {
  StopRecoveryAt("I7", "R1", 1, false, true);
  StopRecoveryAt("I6", "R1", 1, false, false);
  StopRecoveryAt("I7", "R2", 1, false, true);
  StopRecoveryAt("I9", "R2", 1, false, true);
  StopRecoveryAt("I6", "discard.first", 1, false, false);
  StopRecoveryAt("I6", "discard.txn", 1, false, false);
  StopRecoveryAt("I7", "prev-r.first", 1, true, true);
  StopRecoveryAt("I7", "prev-r.txn", 1, true, true);
}

TEST_CASE("recovery stopped twice in a row repeats its decision", "[save_import][fault]") {
  // Stage 7's addition 2: only the points before the terminal folder operation.
  StopRecoveryAt("I7", "R1", 2, false, true);
  StopRecoveryAt("I9", "R2", 2, false, true);
  StopRecoveryAt("I6", "discard.first", 2, false, false);
  StopRecoveryAt("I6", "discard.txn", 2, false, false);
  StopRecoveryAt("I7", "prev-r.first", 2, true, true);
  StopRecoveryAt("I7", "prev-r.txn", 2, true, true);
}

TEST_CASE("recovery stopped after its folder operation (R3) is done", "[save_import][fault]") {
  Slot slot;
  slot.Stage(Content(2), "child");
  REQUIRE(RunChild(Spec("import", slot, "I7")) == kHitCode);
  REQUIRE(RunChild(Spec("recover", slot, "R3")) == kHitCode);
  CHECK_FALSE(fs::exists(slot.ImportDir()));  // the first recovery removed it
  for (int i = 0; i < 2; ++i) {
    HookGuard hook;
    { ContentManager cm(nullptr, slot.root); }
    CHECK(g_hook.seen.empty());  // nothing left to recover: no point reached
    CHECK(GetFile(slot.live) == Content(2));
    CHECK(GetFile(slot.header) == slot.header_bytes);
  }
  RemoveRoot(slot.root);
}

// ---- The racing kill (the replace in progress; samples it, D23) ---------------------------------

TEST_CASE("a kill racing the replace leaves the old or the new file", "[save_import][fault][race]") {
  Slot slot;
  const auto a = Content(1);
  const auto b = Content(2);
  slot.Stage(a, "a");
  slot.Stage(b, "b");
  // 500 runs, bounded by 45 s so the test stays inside its time limit; at least 100 must run.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
  int runs = 0;
  for (; runs < 500 && std::chrono::steady_clock::now() < deadline; ++runs) {
    REQUIRE(RunChild(Spec("race", slot, "")) == kHitCode);
    { ContentManager cm(nullptr, slot.root); }
    const auto live = GetFile(slot.live);
    REQUIRE((live == a || live == b));
    REQUIRE(GetFile(slot.header) == slot.header_bytes);
    REQUIRE_FALSE(fs::exists(slot.ImportDir()));
  }
  INFO("runs " << runs);
  CHECK(runs >= 100);
  RemoveRoot(slot.root);
}

// ---- After the replace: one policy (D24; stage 7's addition 1) ----------------------------------

TEST_CASE("a failure after the replace is not committed and is settled at restart",
          "[save_import][post_install]") {
  for (const char* point : {"txn.installed.tmp", "txn.committed.tmp", "readback"}) {
    HookGuard hook;
    Slot slot;
    INFO("point " << point);
    {
      ContentManager cm(nullptr, slot.root);
      g_hook.fail_at = point;
      g_hook.fail_code = ERROR_DISK_FULL;
      ImportReport report;
      REQUIRE(slot.Import(cm, Content(2), 1, &report) == X_ERROR_IO_INCOMPLETE);
      CHECK(fs::exists(slot.ImportDir()));
      CHECK(GetFile(slot.live) == Content(2));  // the replace landed
      // A second import of the package in the same process is refused, nothing changed.
      ImportReport second;
      CHECK(slot.Import(cm, Content(3), 2, &second) != X_ERROR_SUCCESS);
      CHECK(second.reason == "unresolved");
      CHECK(GetFile(slot.live) == Content(2));
    }
    ExpectSettled(slot, Content(2));  // recovery: finished
    RemoveRoot(slot.root);
  }
}

// ---- Cleanup pending (step 8) -----------------------------------------------------------------

TEST_CASE("a failed cleanup is a committed import, finished later", "[save_import][cleanup]") {
  for (const char* point : {"prev.first", "prev.rename"}) {
    HookGuard hook;
    Slot slot;
    INFO("point " << point);
    ContentManager cm(nullptr, slot.root);
    REQUIRE(slot.Import(cm, Content(2), 1) == X_ERROR_SUCCESS);  // previous-import exists
    g_hook.fail_at = point;
    g_hook.fail_code = ERROR_ACCESS_DENIED;
    ImportReport report;
    REQUIRE(slot.Import(cm, Content(3), 2, &report) == X_ERROR_SUCCESS);
    CHECK(report.cleanup_pending);
    CHECK(GetFile(slot.live) == Content(3));
    CHECK(fs::exists(slot.ImportDir()));
    // Failing again: the next import refuses "cleanup pending", nothing changed.
    g_hook.fail_at = point;
    g_hook.fail_always = true;
    ImportReport refused;
    CHECK(slot.Import(cm, Content(4), 3, &refused) != X_ERROR_SUCCESS);
    CHECK(refused.reason == "cleanup pending");
    CHECK(GetFile(slot.live) == Content(3));
    // Then the next import finishes the folder first and proceeds.
    g_hook.fail_at.clear();
    g_hook.fail_always = false;
    ImportReport next;
    REQUIRE(slot.Import(cm, Content(4), 4, &next) == X_ERROR_SUCCESS);
    CHECK(next.finished_earlier);
    CHECK(GetFile(slot.live) == Content(4));
    CHECK_FALSE(fs::exists(slot.ImportDir()));
    RemoveRoot(slot.root);
  }
}

TEST_CASE("a committed folder with cleanup pending is finished by recovery", "[save_import][cleanup]") {
  HookGuard hook;
  Slot slot;
  {
    ContentManager cm(nullptr, slot.root);
    REQUIRE(slot.Import(cm, Content(2), 1) == X_ERROR_SUCCESS);
    g_hook.fail_at = "prev.first";
    g_hook.fail_code = ERROR_ACCESS_DENIED;
    ImportReport report;
    REQUIRE(slot.Import(cm, Content(3), 2, &report) == X_ERROR_SUCCESS);
    REQUIRE(report.cleanup_pending);
  }
  ExpectSettled(slot, Content(3));
  RemoveRoot(slot.root);
}

// ---- Refusals up to and including step 5: nothing changed -----------------------------------------

namespace {
void ExpectUnchanged(const Slot& slot, const std::vector<uint8_t>& live) {
  CHECK(GetFile(slot.live) == live);
  CHECK(GetFile(slot.header) == slot.header_bytes);
  CHECK_FALSE(fs::exists(slot.ImportDir()));
}
}  // namespace

TEST_CASE("refusals change nothing", "[save_import][errors]") {
  HookGuard hook;
  const auto old_bytes = Content(1);
  const auto new_bytes = Content(2);

  SECTION("staged hash differs") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    const auto staged = slot.Stage(new_bytes, "x");
    ImportReport r;
    CHECK(cm.ImportSavedGame(slot.Binding(), staged, ShaOf(Content(5)), ShaOf(old_bytes), 1, &r) !=
          X_ERROR_SUCCESS);
    CHECK(r.reason == "staged hash");
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("staged length differs") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    std::vector<uint8_t> short_bytes(new_bytes.begin(), new_bytes.end() - 1);
    const auto staged = slot.Stage(short_bytes, "x");
    ImportReport r;
    CHECK(cm.ImportSavedGame(slot.Binding(), staged, ShaOf(short_bytes), ShaOf(old_bytes), 1, &r) !=
          X_ERROR_SUCCESS);
    CHECK(r.reason == "staged length");
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("base hash differs") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    const auto staged = slot.Stage(new_bytes, "x");
    ImportReport r;
    CHECK(cm.ImportSavedGame(slot.Binding(), staged, ShaOf(new_bytes), ShaOf(Content(7)), 1, &r) !=
          X_ERROR_SUCCESS);
    CHECK(r.reason == "base differs");
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("a native pending folder") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    fs::create_directories(slot.root / "save-backup" / "pending" / kId);
    ImportReport r;
    CHECK(slot.Import(cm, new_bytes, 1, &r) != X_ERROR_SUCCESS);
    CHECK(r.reason == "native pending");
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("a header with another profile's xuid") {
    Slot slot({}, kOtherXuid);
    ContentManager cm(nullptr, slot.root);
    ImportReport r;
    CHECK(slot.Import(cm, new_bytes, 1, &r) != X_ERROR_SUCCESS);
    CHECK(r.reason == "profile");
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("a bound path that differs") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    // Another profile's slot, whose header carries the owner's xuid.
    const fs::path other_live = Slot::LiveOf(slot.root, kOtherXuid);
    const fs::path other_header = Slot::HeaderOf(slot.root, kOtherXuid);
    PutFile(other_live, Content(8));
    PutFile(other_header, Slot::HeaderBytes(kXuid));
    auto binding = slot.Binding();
    binding.header_file = other_header;
    const auto staged = slot.Stage(new_bytes, "x");
    ImportReport r;
    CHECK(cm.ImportSavedGame(binding, staged, ShaOf(new_bytes), ShaOf(old_bytes), 1, &r) !=
          X_ERROR_SUCCESS);
    CHECK(r.reason == "bound path");
    auto to_other = slot.Binding();
    to_other.xuid = kOtherXuid;
    to_other.package_file = other_live;
    to_other.header_file = other_header;
    ImportReport r2;
    CHECK(cm.ImportSavedGame(to_other, staged, ShaOf(new_bytes), ShaOf(Content(8)), 1, &r2) !=
          X_ERROR_SUCCESS);  // the header's xuid (the owner's) would redirect it
    CHECK(GetFile(other_live) == Content(8));
    CHECK(GetFile(other_header) == Slot::HeaderBytes(kXuid));
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("a volume that is not NTFS") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    g_hook.fail_at = "volume";
    g_hook.fail_code = 1;
    ImportReport r;
    CHECK(slot.Import(cm, new_bytes, 1, &r) != X_ERROR_SUCCESS);
    CHECK(r.reason == "not NTFS");
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("a write failure in step 2 and in step 3") {
    for (const char* point : {"old.xsg.tmp", "old.header.tmp", "new.xsg.tmp", "txn.prepared.tmp"}) {
      Slot slot;
      ContentManager cm(nullptr, slot.root);
      g_hook.fail_at = point;
      g_hook.fail_code = ERROR_DISK_FULL;
      INFO("point " << point);
      CHECK(slot.Import(cm, new_bytes, 1) == X_RESULT_FROM_WIN32(ERROR_DISK_FULL));
      ExpectUnchanged(slot, old_bytes);
      RemoveRoot(slot.root);
    }
  }
  SECTION("the install refused (the live file held without share-delete)") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    ImportReport r;
    {
      Holder hold(slot.live);
      REQUIRE(hold.h != INVALID_HANDLE_VALUE);
      CHECK(slot.Import(cm, new_bytes, 1, &r) != X_ERROR_SUCCESS);
    }
    CHECK(r.reason == "install refused");
    CHECK(std::count(g_hook.seen.begin(), g_hook.seen.end(), std::string("install")) == 4);
    ExpectUnchanged(slot, old_bytes);
    RemoveRoot(slot.root);
  }
  SECTION("the live file replaced between step 0 and step 5 (stage 7's addition 3)") {
    Slot slot;
    ContentManager cm(nullptr, slot.root);
    g_hook.on_point = [&](const char* p) {
      if (std::strcmp(p, "I6") == 0) PutFile(slot.live, Content(9));
    };
    ImportReport r;
    CHECK(slot.Import(cm, new_bytes, 1, &r) != X_ERROR_SUCCESS);
    CHECK(r.reason == "base differs");
    ExpectUnchanged(slot, Content(9));
    RemoveRoot(slot.root);
  }
}

TEST_CASE("an open content refuses the import", "[save_import][errors]") {
  Slot slot;
  SaveImportJournal journal(slot.root);
  SaveImportJournal::Request rq;
  rq.id = kId;
  rq.package_file = slot.live;
  rq.header_file = slot.header;
  rq.staged = slot.Stage(Content(2), "x");
  rq.new_sha = ShaOf(Content(2));
  rq.base_sha = ShaOf(Content(1));
  rq.seq = 1;
  rq.file_size = kSize;
  ImportReport r;
  CHECK(journal.Import(rq, [](const std::function<uint32_t(bool)>& body) { return body(true); }, &r) !=
        X_ERROR_SUCCESS);
  CHECK(r.reason == "open");
  ExpectUnchanged(slot, Content(1));
  // Open only at step 5's check: aborted, the folder deleted.
  int calls = 0;
  CHECK(journal.Import(rq, [&](const std::function<uint32_t(bool)>& body) { return body(++calls > 1); },
                       &r) != X_ERROR_SUCCESS);
  CHECK(r.reason == "open");
  ExpectUnchanged(slot, Content(1));
  RemoveRoot(slot.root);
}

TEST_CASE("a base that changes to a third file, stopped in the abort, is orphaned",
          "[save_import][errors][fault]") {
  Slot slot;
  slot.Stage(Content(2), "child");
  REQUIRE(RunChild(Spec("import", slot, "abort.txn", "third")) == kHitCode);
  {
    ContentManager cm(nullptr, slot.root);  // recovery: neither old nor new: orphaned
    CHECK(GetFile(slot.live) == Content(9));
    CHECK(HasOrphan(slot));
    CHECK_FALSE(fs::exists(slot.ImportDir()));
    ImportReport r;
    CHECK(slot.Import(cm, Content(3), 2, &r) != X_ERROR_SUCCESS);  // refused for the rest of the run
    CHECK(r.reason == "orphaned");
    CHECK(GetFile(slot.live) == Content(9));
  }
  CHECK(GetFile(slot.header) == slot.header_bytes);
  RemoveRoot(slot.root);
}

TEST_CASE("an import over an unresolved transaction is refused (stage 7's addition 4)",
          "[save_import][errors][fault]") {
  HookGuard hook;
  Slot slot;
  slot.Stage(Content(2), "child");
  REQUIRE(RunChild(Spec("import", slot, "I6")) == kHitCode);
  {
    Holder hold(slot.ImportDir() / "txn");  // recovery cannot delete the folder
    REQUIRE(hold.h != INVALID_HANDLE_VALUE);
    ContentManager cm(nullptr, slot.root);
    CHECK(fs::exists(slot.ImportDir()));
    ImportReport r;
    CHECK(slot.Import(cm, Content(3), 2, &r) != X_ERROR_SUCCESS);
    CHECK(r.reason == "unresolved");
    CHECK(GetFile(slot.live) == Content(1));
  }
  ContentManager cm(nullptr, slot.root);
  CHECK_FALSE(fs::exists(slot.ImportDir()));
  CHECK(slot.Import(cm, Content(3), 3) == X_ERROR_SUCCESS);
  CHECK(GetFile(slot.live) == Content(3));
  RemoveRoot(slot.root);
}

// ---- Recovery's orphans: the live file never written --------------------------------------------

TEST_CASE("recovery orphans a damaged record or a third live file", "[save_import][recovery]") {
  SECTION("a damaged txn checksum") {
    Slot slot;
    slot.Stage(Content(2), "child");
    REQUIRE(RunChild(Spec("import", slot, "I7")) == kHitCode);
    auto txn = GetFile(slot.ImportDir() / "txn");
    txn[8] ^= 0x01;
    PutFile(slot.ImportDir() / "txn", txn);
    { ContentManager cm(nullptr, slot.root); }
    CHECK(GetFile(slot.live) == Content(2));
    CHECK(HasOrphan(slot));
    CHECK_FALSE(fs::exists(slot.ImportDir()));
    RemoveRoot(slot.root);
  }
  SECTION("a live file replaced while a transaction is unfinished") {
    Slot slot;
    slot.Stage(Content(2), "child");
    REQUIRE(RunChild(Spec("import", slot, "I6")) == kHitCode);
    PutFile(slot.live, Content(9));
    { ContentManager cm(nullptr, slot.root); }
    CHECK(GetFile(slot.live) == Content(9));
    CHECK(GetFile(slot.header) == slot.header_bytes);
    CHECK(HasOrphan(slot));
    RemoveRoot(slot.root);
  }
}

// ---- Two profiles: only the bound slot changes (import-two-profiles (a)) -------------------------

TEST_CASE("an import changes only the bound profile's slot", "[save_import]") {
  HookGuard hook;
  Slot slot;
  const fs::path other_live = Slot::LiveOf(slot.root, kOtherXuid);
  const fs::path other_header = Slot::HeaderOf(slot.root, kOtherXuid);
  PutFile(other_live, Content(8));
  PutFile(other_header, Slot::HeaderBytes(kOtherXuid));
  ContentManager cm(nullptr, slot.root);
  REQUIRE(slot.Import(cm, Content(2), 1) == X_ERROR_SUCCESS);
  CHECK(GetFile(slot.live) == Content(2));
  CHECK(GetFile(other_live) == Content(8));
  CHECK(GetFile(other_header) == Slot::HeaderBytes(kOtherXuid));
  RemoveRoot(slot.root);
}

// ---- The snapshot and the commit counter ----------------------------------------------------------

TEST_CASE("the snapshot refuses while a journal folder exists or the size is wrong", "[save_import][snapshot]") {
  Slot slot;
  ContentManager cm(nullptr, slot.root);
  XCONTENT_AGGREGATE_DATA data{};
  std::memcpy(&data, slot.header_bytes.data(), sizeof(data));
  std::vector<uint8_t> bytes;
  uint64_t count = 99;
  REQUIRE(cm.SnapshotSavedGame(kXuid, data, "SWAPFORCE_1.xsg", kSize, &bytes, &count) == X_ERROR_SUCCESS);
  CHECK(bytes == Content(1));
  CHECK(count == 0);  // no committing close in this process
  CHECK(cm.SnapshotSavedGame(kXuid, data, "SWAPFORCE_1.xsg", kSize - 1, &bytes, &count) ==
        X_ERROR_ACCESS_DENIED);
  fs::create_directories(slot.root / "save-backup" / "pending" / kId);
  CHECK(cm.SnapshotSavedGame(kXuid, data, "SWAPFORCE_1.xsg", kSize, &bytes, &count) ==
        X_ERROR_ACCESS_DENIED);
  fs::remove(slot.root / "save-backup" / "pending" / kId);
  fs::create_directories(slot.ImportDir());
  CHECK(cm.SnapshotSavedGame(kXuid, data, "SWAPFORCE_1.xsg", kSize, &bytes, &count) ==
        X_ERROR_ACCESS_DENIED);
  fs::remove(slot.ImportDir());
  CHECK(cm.CommitCountRaisedByThisThread(kXuid, data) == 0);
  RemoveRoot(slot.root);
}

TEST_CASE("the commit counter counts per package and records the raising thread", "[save_import][snapshot]") {
  CommitCounter counter;
  CHECK(counter.Current("a") == 0);
  CHECK(counter.Raise("a") == 1);
  CHECK(counter.Raise("a") == 2);
  CHECK(counter.Raise("b") == 1);
  CHECK(counter.Current("a") == 2);
  CHECK(counter.RaisedByThisThread("a") == 2);
  uint64_t other_seen = 99, other_raised = 0;
  std::thread t([&] {
    other_seen = counter.RaisedByThisThread("a");  // another thread raised none
    other_raised = counter.Raise("a");
  });
  t.join();
  CHECK(other_seen == 0);
  CHECK(other_raised == 3);
  CHECK(counter.RaisedByThisThread("a") == 2);  // this thread's own, not the latest
  CHECK(counter.Current("a") == 3);
}

#endif  // REX_PLATFORM_WIN32
