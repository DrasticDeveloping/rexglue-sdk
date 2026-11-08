/**
 * @file        core/logging.cpp
 * @brief       Logging infrastructure implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/async_logger.h>
#include <spdlog/details/null_mutex.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/logging/sink.h>
#include <rex/platform.h>
#include <rex/platform/env.h>

#if REX_PLATFORM_WIN32
#include <spdlog/details/windows_include.h>  // TerminateProcess (the exit watchdog)
#include <spdlog/sinks/msvc_sink.h>
#else
#include <spdlog/sinks/stdout_sinks.h>
#endif

REXCVAR_DEFINE_STRING(log_level, "info", "Log",
                      "Global log level: trace, debug, info, warn, error, critical, off")
    .allowed({"trace", "debug", "info", "warn", "error", "critical", "off"});

REXCVAR_DEFINE_STRING(log_file, "", "Log", "Log file path (empty = auto sequential naming)");

REXCVAR_DEFINE_BOOL(log_verbose, false, "Log", "Enable verbose logging (sets level to trace)")
    .debug_only();

REXCVAR_DEFINE_BOOL(log_noisy, false, "Log", "Enable noisy/high-frequency log macros");

REXCVAR_DEFINE_INT32(log_flush_interval, 0, "Log", "Periodic flush interval in seconds (0 = off)")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Asynchronous logging (Swap Force Online, 2026-11-06). With it on, a log call only queues the line
// and one writer thread runs the sinks (file, debug, capture), so the calling thread never waits on
// a disk write. Why: the dedicated server's synchronous file flush stalled about 16 s under the sink
// lock in run 20261106-211124 (P1m); its network pump thread waited on that lock to log one line,
// and every client dropped the server on its 10 s peer timeout. Safe: the default is off, so the
// other games and offline play keep today's synchronous loggers; the online client turns it on for
// its dedicated server (--log_async=true in the server's command line), and a client switches to
// the same mode at run time at its first online session (EnableAsyncLogging below; the owner's
// decision of 2026-11-07, "1 but only online"), so play before any Host or Join keeps the
// synchronous loggers. Lines from all threads
// keep one order (one writer) and keep the time and thread of the call; FlushLogging and
// ShutdownLogging drain the queue (bounded at 3 s) so the lines before a hard exit reach the disk.
// It takes effect only with a running exit watchdog: if that cannot start, the loggers stay
// synchronous for the process (the exit invariant, "Asynchronous mode" below).
REXCVAR_DEFINE_BOOL(log_async, false, "Log",
                    "Queue log lines to one writer thread; the calling thread never waits on a sink")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// How many lines the asynchronous queue holds before a logging thread waits for room (the block
// overflow policy: no line is ever dropped, because the test runner grades log lines). 32768 lines
// is about 100 s of the server's busiest second (326 lines/s in P1m) and about 13 MB, allocated
// once when log_async is on. Read only with log_async; no effect otherwise.
REXCVAR_DEFINE_INT32(log_async_queue, 32768, "Log",
                     "Lines the async queue holds before a caller waits (block policy)")
    .range(1024, 1048576)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_max_file_size_mb, 5, "Log", "Max log file size in MB before rotation")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_max_files, 20, "Log", "Max number of rotated log files to keep")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {

namespace {

// --- The player's data out of every log line ---------------------------------------------------
// Logs end up in public problem reports, so no line may carry the player's user folder, user name
// or the machine's name (2026-10-30). Every sink gets the lines through RedactingSink.

struct Redactions {
  std::string profile;  // the user folder, e.g. C:\Users\<name> (HOME elsewhere)
  std::vector<std::pair<std::string, std::string_view>> words;  // a name and what it becomes
};

const Redactions& GetRedactions() {
  static const Redactions redactions = [] {
    Redactions r;
    auto env = [](const char* name) {
      const char* value = std::getenv(name);
      return value ? std::string(value) : std::string();
    };
#if REX_PLATFORM_WIN32
    r.profile = env("USERPROFILE");
    const std::string user = env("USERNAME"), computer = env("COMPUTERNAME");
#else
    r.profile = env("HOME");
    const std::string user = env("USER"), computer = env("HOSTNAME");
#endif
    // Short names would match ordinary words.
    if (user.size() >= 3)
      r.words.emplace_back(user, "<user>");
    if (computer.size() >= 3 && computer != user)
      r.words.emplace_back(computer, "<computer>");
    return r;
  }();
  return redactions;
}

char FoldChar(char c) {
  return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool MatchesAt(std::string_view text, size_t at, std::string_view what) {
  if (what.empty() || at + what.size() > text.size())
    return false;
  for (size_t i = 0; i < what.size(); ++i) {
    if (FoldChar(text[at + i]) != FoldChar(what[i]))
      return false;
  }
  return true;
}

bool IsWordChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Every occurrence of `what` (case and slashes ignored; with `whole_word`, not inside a longer
// word) becomes `with`.
void ReplaceAll(std::string& text, std::string_view what, std::string_view with, bool whole_word) {
  for (size_t at = 0; at + what.size() <= text.size();) {
    const bool word_ok = !whole_word || ((at == 0 || !IsWordChar(text[at - 1])) &&
                                         (at + what.size() == text.size() || !IsWordChar(text[at + what.size()])));
    if (MatchesAt(text, at, what) && word_ok) {
      text.replace(at, what.size(), with);
      at += with.size();
    } else {
      ++at;
    }
  }
}

// Any other user's folder: the part after \Users\ (or /home/) up to the next separator or quote.
void ReplaceUserFolders(std::string& text) {
  for (std::string_view marker : {"\\users\\", "\\home\\"}) {
    for (size_t at = 0; at + marker.size() <= text.size(); ++at) {
      if (!MatchesAt(text, at, marker))
        continue;
      const size_t begin = at + marker.size();
      size_t end = begin;
      while (end < text.size() && text[end] != '\\' && text[end] != '/' && text[end] != '\'' && text[end] != '"')
        ++end;
      const std::string_view name(text.data() + begin, end - begin);
      if (name.empty() || name == "<user>" || name == "Public" || name == "Default" || name == "All Users")
        continue;
      text.replace(begin, end - begin, "<user>");
    }
  }
}

// --- The slow-write witness ---------------------------------------------------------------------
// The longest sink write or flush over kSlowWriteMs since the last TakeSlowLogWrite, and which sink
// it was. P1m's 16 s stall could not be pinned to a sink from the logs alone; this names it. Only
// RedactingSink writes it, after the target returns; no log call happens here.
constexpr int64_t kSlowWriteMs = 1000;
std::mutex g_slow_mutex;
uint32_t g_slow_ms = 0;
const char* g_slow_sink = nullptr;
size_t g_slow_queued = 0;  // the log_async queue's length when that write ended: the backlog
// The log_async pool (see "Asynchronous mode" below), readable on any thread without g_mutex.
std::atomic<spdlog::details::thread_pool*> g_pool_raw{nullptr};

// True on the log_async writer thread (set when the pool starts it). A line logged there must not
// be queued: the writer is the queue's only consumer, so with the block policy a full queue would
// wait forever (see LogUrgent).
thread_local bool t_log_writer = false;
// The RedactingSink this thread is inside (its lock held), or null. A crash handler that runs on
// the writer while it is inside a sink (LogUrgent) writes to every other sink directly, not to this
// one: locking it again on the same thread would deadlock.
thread_local spdlog::sinks::sink* t_inside_sink = nullptr;

struct InsideSink {
  explicit InsideSink(spdlog::sinks::sink* sink) : previous(t_inside_sink) { t_inside_sink = sink; }
  ~InsideSink() { t_inside_sink = previous; }
  InsideSink(const InsideSink&) = delete;
  InsideSink& operator=(const InsideSink&) = delete;
  spdlog::sinks::sink* previous;
};

void NoteSinkCall(const char* label, std::chrono::steady_clock::time_point began) {
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - began)
                         .count();
  if (ms <= kSlowWriteMs)
    return;
  // The queue's length now is what piled up behind this write. Read here, not in TakeSlowLogWrite
  // (up to a frame_stats window later, when the writer has long emptied the queue). Safe on the
  // writer thread: it does not hold the queue's mutex while it is inside a sink.
  auto* pool = g_pool_raw.load(std::memory_order_acquire);
  const size_t queued = pool ? pool->queue_size() : 0;
  std::lock_guard lock(g_slow_mutex);
  if (static_cast<uint32_t>(ms) > g_slow_ms) {
    g_slow_ms = static_cast<uint32_t>(ms);
    g_slow_sink = label;
    g_slow_queued = queued;
  }
}

class RedactingSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  // `label` names the sink in the slow-write witness: debug, console, file, capture or added.
  RedactingSink(spdlog::sink_ptr target, const char* label)
      : target_(std::move(target)), label_(label) {}

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const InsideSink inside(this);
    const std::string text = RedactUserData(std::string_view(msg.payload.data(), msg.payload.size()));
    spdlog::details::log_msg redacted = msg;
    redacted.payload = spdlog::string_view_t(text.data(), text.size());
    if (target_->should_log(redacted.level)) {
      const auto began = std::chrono::steady_clock::now();
      target_->log(redacted);
      NoteSinkCall(label_, began);
    }
  }
  void flush_() override {
    const InsideSink inside(this);
    const auto began = std::chrono::steady_clock::now();
    target_->flush();
    NoteSinkCall(label_, began);
  }
  // The pattern is the target's (SetConsolePattern, SetFilePattern).
  void set_pattern_(const std::string& pattern) override { target_->set_pattern(pattern); }
  void set_formatter_(std::unique_ptr<spdlog::formatter> formatter) override {
    target_->set_formatter(std::move(formatter));
  }

 private:
  spdlog::sink_ptr target_;
  const char* label_;
};

spdlog::sink_ptr Redacted(spdlog::sink_ptr sink, const char* label) {
  return sink ? std::make_shared<RedactingSink>(std::move(sink), label) : sink;
}

// The sinks AddSink was given and what the loggers got instead, for RemoveSink.
std::vector<std::pair<spdlog::sink_ptr, spdlog::sink_ptr>> g_added_sinks;

spdlog::sink_ptr AddedSink(const spdlog::sink_ptr& sink) {
  for (auto& [given, redacted] : g_added_sinks) {
    if (given == sink)
      return redacted;
  }
  // ReXApp's in-memory log ring (rex_app.cpp, AddSink) is told apart from the app's own sinks.
  const bool capture = dynamic_cast<LogCaptureSink*>(sink.get()) != nullptr;
  auto redacted = Redacted(sink, capture ? "capture" : "added");
  g_added_sinks.emplace_back(sink, redacted);
  return redacted;
}

std::vector<LogCategoryEntry> g_registry;
spdlog::sink_ptr g_console_sink;
spdlog::sink_ptr g_file_sink;
spdlog::sink_ptr g_early_sink;
std::vector<spdlog::sink_ptr> g_extra_sinks;
bool g_early_initialized = false;
bool g_initialized = false;
// Timed, so that FlushLogging can give up at its deadline when another thread holds it (another
// FlushLogging waiting for a stuck writer).
std::timed_mutex g_mutex;
// The level lock (2026-11-07, amendment an): guards g_registry's storage and each entry's logger
// and has_explicit_level against the level setters (SetCategoryLevel, SetAllLevels, SetRootLevel,
// the log_level callback) and GetLogger, which take this lock alone. Whoever changes those fields
// holds g_mutex first, then this, and only around the change itself (a push_back, a pointer swap, a
// set_level): never across a sink call or a logger's construction. Why its own lock: g_mutex is
// held for a whole synchronous FlushLogging (24 s in a disk stall, P1n), and the setters must not
// wait on that (the log_level callback runs under the cvar registry's lock); before the run-time
// switch the setters took no lock at all, so offline play waits on nothing new. Lock order: g_mutex,
// then this; nothing is taken while it is held.
std::mutex g_level_mutex;
LogConfig g_config;

// The loggers by category id for GetLoggerRaw, readable on any thread without g_mutex. Written
// (release) under g_mutex and g_level_mutex wherever a category's logger is set (SetEntryLogger
// and SetEntryLoggerLocked: InitLoggingEarly, InitLogging, RegisterLogCategory,
// RegisterLogSubcategory, EnableAsyncLogging) and cleared by ShutdownLogging, so it always names
// the same logger as g_registry. One slot per possible id (LogCategoryId is 16 bits):
// zero-initialized static storage (constinit, no dynamic init), so only the page that holds the
// used slots is ever touched. Why: EnableAsyncLogging replaces loggers
// while other threads log, and reading g_registry[id].logger (a shared_ptr) while it is assigned is
// a data race; the table also takes GetLoggerRaw off g_registry, which a registration can
// reallocate. Without a switch GetLoggerRaw returns the same pointer as before, from the same
// moments on, through one acquire load (a plain load on x64).
using LoggerTable = std::array<std::atomic<spdlog::logger*>, 65536>;
constinit LoggerTable g_raw{};

// The table GetLoggerRaw reads: g_raw until EnableAsyncLogging publishes a complete replacement
// with one release store (Sol, narrow round on the switch, F1). Swapping category by category let a
// thread see category A asynchronous and category B still synchronous, so its queued line through A
// could reach a shared sink after its later synchronous line through B. With one publication a
// thread sees the old table or the new one, never a mix, and since the pointer only ever moves
// forward (old -> new, once per process), a thread that saw the new table never sees the old one
// again: its lines stay in order across categories. The replacement is allocated once and never
// freed (like g_retired): a reader may still be loading a slot from it. Writers (SetEntryLoggerLocked)
// hold g_mutex and g_level_mutex and write the current table.
std::atomic<LoggerTable*> g_table{&g_raw};

// The synchronous loggers EnableAsyncLogging replaced. Allocated once, never freed (also not at
// static teardown): a thread that took the old pointer from GetLoggerRaw just before the swap may
// still be inside its log call, a GetLogger holder keeps using it, spdlog's default logger stays
// the old "core" one (EnableAsyncLogging never calls spdlog::drop or set_default_logger), and
// spdlog's own map keeps them (EnableAsyncLogging does not touch it either; see there).
// Under g_mutex; one set of loggers per switch, and a process switches at most once (unit tests:
// once per drained ShutdownLogging).
std::vector<std::shared_ptr<spdlog::logger>>* g_retired = nullptr;

// Every category logger is asynchronous now (InitLogging with log_async, or EnableAsyncLogging after
// its swaps); EnableAsyncLogging's fast path. Set under g_mutex, cleared by ShutdownLogging.
std::atomic<bool> g_loggers_async{false};

// Sets a category's logger in g_registry and in g_raw together, so the two never drift apart.
// Caller holds g_mutex and g_level_mutex; `id` is a registered entry.
void SetEntryLoggerLocked(size_t id, std::shared_ptr<spdlog::logger> logger) {
  spdlog::logger* raw = logger.get();
  g_registry[id].logger = std::move(logger);
  (*g_table.load(std::memory_order_relaxed))[id].store(raw, std::memory_order_release);
}

// The same, taking g_level_mutex itself. Caller holds g_mutex (and not g_level_mutex); the logger is
// made before the call, so its construction is outside the level lock.
void SetEntryLogger(size_t id, std::shared_ptr<spdlog::logger> logger) {
  std::lock_guard level_lock(g_level_mutex);
  SetEntryLoggerLocked(id, std::move(logger));
}

// --- Asynchronous mode (log_async) ---------------------------------------------------------------
// THE EXIT INVARIANT (the root-cause analysis after Sol's narrow round B round 3; docs/online-1B.md
// amendment aj, widened by the run-time switch of amendment an). In async mode every exit path
// (crash, fatal error, abort) ends within a bound, whatever the sinks, the queue and the writer do.
// One mechanism guarantees it, and it has one precondition:
// - Async mode has two entries and one start sequence: InitLogging with log_async (S, from its
//   start) and EnableAsyncLogging (a client, at its first online session; one-way, it stays on for
//   the rest of the process). Both run the same sequence under g_mutex (MakeAsyncLocked, then
//   PublishAsyncLocked; StartAsyncLocked for InitLogging): the watchdog's thread, then the pool,
//   then armable (g_exit_watchdog_live), then the drain mark; only after the publish does any logger
//   turn asynchronous. EnableAsyncLogging also builds every asynchronous logger between the two
//   (on the made, unpublished pool), so a failure there publishes nothing either. Before a switch
//   nothing of it exists (no watchdog thread, no pool, nothing armable): play before any session is
//   the plain synchronous logging of every other game, crashes included.
// - The bound is the exit watchdog's: kExitBound (4 s) after the first ArmExitWatchdog, then
//   TerminateProcess (std::_Exit elsewhere) with exit code 3. Nothing else guarantees the bound.
// - Async mode exists only while the watchdog runs. If the watchdog cannot start (or the pool or,
//   at a switch, an asynchronous logger cannot be made), async mode is refused for the process
//   (g_log_async_refused): no pool, the loggers stay the same synchronous objects, one warning
//   through the synchronous sinks, and every later EnableAsyncLogging returns false at once. The
//   watchdog is never stopped while a pool exists: its thread lives until the process ends, and
//   ShutdownLogging stops it from being armed only after the pool is gone (never for a pool left to
//   the exit behind a stuck writer).
//   ArmExitWatchdog does nothing without async mode. Between PublishAsyncLocked and the one store
//   of g_table, every logger still reads synchronous while the watchdog is already armable: a crash
//   then is bounded too, and nothing on its path depends on the queue (LogUrgent logs plainly
//   through a synchronous logger).
// - A logger the switch replaced is never freed (g_retired) and stays synchronous: a thread that
//   took its pointer just before the switch writes that one line to the sinks itself, within the
//   bound like any synchronous crash line once armed. GetLoggerRaw reads the published table
//   (g_table: g_raw before the switch, the replacement after), never g_registry.
// - Every crash, fatal or abort entry arms it before its first ordinary log call (a line before the
//   arm is urgent): REX_FATAL, REX_FATAL_CAT, REX_FATAL_FN, REX_FATAL_IF and REX_ASSERT in a debug
//   build (logging/assert.h), the portal's OnCrash and OnAbort (dev/write_watch.cpp), WriteOnWriter
//   below, and the kernel's dead ends: FiberEntryPoint's std::terminate (kernel/crt/threading.cpp),
//   XamShowDirtyDiscErrorUI's exit(1) (kernel/xam/xam_ui.cpp: two urgent lines, a dialog that waits
//   for OK, then the arm), KeBugCheckEx (kernel/xboxkrnl/xboxkrnl_debug.cpp) and an undecodable MMIO
//   access (system/mmio_handler.cpp). Every other abort reaches OnAbort through SIGABRT; the SDK's
//   other aborts (rex_assert_fail, FatalError) write only to stderr. The game's own Fatal() (a
//   failed disc check right after InitLogging) logs an urgent line, arms after its message box,
//   flushes and calls std::exit(2).
// - Deliberate exits. Their exit tails are bounded by themselves, without the watchdog: EndServer
//   and ReXApp::OnClosing (urgent lines, FlushLogging, _Exit) and the input script's quit (an urgent
//   line, FlushLogging, _Exit). Ordinary lines logged before those tails are outside the bound and
//   can still wait on a full queue behind a stuck writer: OnClosing's TerminateTitle (its debug line
//   in kernel_state.cpp) and the lines of EndServer's session drain (the audit's retained
//   exclusions). Bounded by arming it: HalReturnToFirmware (the title's shutdown: an urgent line, the
//   arm, FlushLogging, then exit(0), whose static teardown joins the log writer). A client after its
//   switch has the same exit paths (the same executable; the S-only EndServer aside) and the same
//   retained exclusion (OnClosing's TerminateTitle line). Never in async mode: the portal's export
//   runs (one ordinary line on the launcher's synchronous loggers, FlushLogging, _Exit); they quit
//   without a session, so no switch can precede them.
// - Every wait on an armed path is best effort under the watchdog's deadline (CapToExit): an urgent
//   line's room wait, FlushLogging, DrainQueue. Delivery to the sinks is never the bound.
// - The watchdog thread takes no lock, allocates nothing and logs nothing.
//
// The queue and its one writer thread; set by PublishAsyncLocked (the first InitLogging when
// log_async is on, or EnableAsyncLogging), null otherwise (then every logger is the plain synchronous
// spdlog::logger, as before). g_pool_raw
// (defined with the slow-write witness above) is the same pool for NoteSinkCall, which runs on any
// thread without g_mutex.
// Changed under g_mutex and g_pool_mutex; LogUrgent copies it under g_pool_mutex alone (held only
// for the copy), since a fatal line must not wait for g_mutex (FlushLogging holds it up to 3 s).
std::shared_ptr<spdlog::details::thread_pool> g_pool;
std::mutex g_pool_mutex;

using Clock = std::chrono::steady_clock;

// FlushLogging's and ShutdownLogging's bound, from the call to the return: the queue's lines
// written and every sink flushed by the writer, or the caller gives up.
constexpr std::chrono::milliseconds kDrainBound{3000};
// After a drain timed out the writer counts as stuck until a later mark gets through: the next
// drain waits only this long (a crash handler's FlushLogging after REX_FATAL's), and an urgent line
// does not wait for room at all.
constexpr std::chrono::milliseconds kStuckGrace{250};
// How long an urgent line (LogUrgent) waits for room in a full queue before it pushes the oldest
// queued line out (overrun_oldest), so a fatal line costs at most this plus FlushLogging's bound.
constexpr std::chrono::milliseconds kUrgentRoomBound{500};
std::atomic<bool> g_writer_stuck{false};
// An urgent line's room wait ran out with the queue still full: the later urgent lines (a crash
// handler logs up to three before its FlushLogging) skip the wait until a drain gets through again
// (cleared with g_writer_stuck). Kept apart from g_writer_stuck, which would also cut the next
// FlushLogging to kStuckGrace.
std::atomic<bool> g_urgent_no_room{false};
std::atomic<size_t> g_urgent_missed{0};  // urgent lines a sink missed (LogUrgent on the writer)
// Serializes every post with the discard_new policy on the pool (LogUrgent's attempts and its
// flush, DrainQueue's marks), so the pool's discard counter tells an urgent line whether its own
// attempt was dropped: then a full queue is waited out without ever displacing an ordinary line
// (Sol's narrow round B round 2, M2). Held only around one non-blocking post and always taken with
// a deadline. Lock order: g_mutex before it (DrainQueue); LogUrgent never takes g_mutex.
std::timed_mutex g_urgent_mutex;

// --- The exit watchdog (log_async) --------------------------------------------------------------
// A crash or fatal error with log_async ends the process at most kExitBound after its first line,
// whatever the sinks do (Sol's narrow round B round 2, M1). Why: on the writer thread itself the
// crash lines are written to the sinks directly (WriteOnWriter), and a sink that stalls there (a
// disk, a virus scanner: P1m's 16 s file stall) would hold the process forever, never reaching
// abort/_Exit. Once armed, the bounded waits share one budget (CapToExit): each ends kExitMargin
// before the watchdog's deadline at the latest, so a crash path reaches abort/WER before the
// watchdog fires however its waits add up (three urgent lines' room waits, a drain that succeeds
// late, a concurrent FlushLogging holding g_mutex: Sol's narrow round B round 3); the watchdog only
// ends one stuck in direct sink I/O or anything else unbounded. Its thread is started before the pool
// (MakeAsyncLocked; the invariant above) and touches only atomics, a sleep and
// TerminateProcess/_Exit: no log lock, no allocation, no loader lock. Synchronous logging (log_async
// off and no switch: the other games, offline play before any session, and a process whose
// watchdog did not start) has no pool and no watchdog thread, so ArmExitWatchdog does nothing there
// and crashes behave as before.
constexpr std::chrono::milliseconds kExitBound{4000};
constexpr int kExitWatchdogCode = 3;  // std::abort's exit code on Windows
std::atomic<int64_t> g_exit_deadline{0};  // steady_clock ticks of the deadline; 0 = not armed
std::atomic<void (*)()> g_exit_hook{nullptr};  // unit tests only (SetExitWatchdogHookForTesting)
std::atomic<void (*)()> g_switch_hook{nullptr};  // unit tests only (SetSwitchPublishHookForTesting)
std::atomic<bool> g_exit_watchdog_running{false};
// Async mode is in effect, so ArmExitWatchdog arms: set by PublishAsyncLocked (InitLogging or
// EnableAsyncLogging) once the watchdog runs and the pool exists, before any logger is
// asynchronous; cleared by ShutdownLogging only after the pool
// is gone (never when a stuck writer's pool is left to the exit). The invariant's "never stopped
// while a pool exists".
std::atomic<bool> g_exit_watchdog_live{false};
// The watchdog failed to start, or the pool (MakeAsyncLocked) or a switch's asynchronous loggers
// (EnableAsyncLogging) could not be made: async mode is refused for the rest of the process, at
// InitLogging and at every later EnableAsyncLogging (unit tests clear it:
// SetExitWatchdogStartFailureForTesting).
std::atomic<bool> g_log_async_refused{false};
std::atomic<bool> g_exit_watchdog_fail_for_testing{false};  // SetExitWatchdogStartFailureForTesting
// How long before the watchdog's deadline every bounded wait of an armed path ends: room for the
// abort/WER after the last drain. And the deadline this thread's first FlushLogging since the arm
// was cut for: that one ends kStuckGrace earlier still, so the abort handler's FlushLogging after a
// REX_FATAL's (the same thread: abort raises SIGABRT on it) keeps its grace.
constexpr std::chrono::milliseconds kExitMargin{150};
thread_local int64_t t_exit_reserved = 0;

// `d`, or earlier once the watchdog is armed: kExitMargin plus `reserve` before its deadline (a time
// already past then: the waits that take it try once).
Clock::time_point CapToExit(Clock::time_point d, Clock::duration reserve = Clock::duration::zero()) {
  const int64_t armed = g_exit_deadline.load(std::memory_order_acquire);
  if (armed == 0)
    return d;
  return std::min(d, Clock::time_point{Clock::duration(armed)} - kExitMargin - reserve);
}

[[noreturn]] void EndProcessNow() {
#if REX_PLATFORM_WIN32
  TerminateProcess(GetCurrentProcess(), kExitWatchdogCode);
#endif
  std::_Exit(kExitWatchdogCode);
}

void ExitWatchdogMain() {
  for (;;) {
    int64_t armed = g_exit_deadline.load(std::memory_order_acquire);
    if (armed == 0) {
      g_exit_deadline.wait(0, std::memory_order_acquire);
      continue;
    }
    const Clock::time_point due{Clock::duration(armed)};
    const auto now = Clock::now();
    if (now < due) {
      // Short steps, so a test's disarm and re-arm (SetExitWatchdogHookForTesting) is seen.
      std::this_thread::sleep_for(std::min<Clock::duration>(due - now, std::chrono::milliseconds(20)));
      continue;
    }
    if (auto hook = g_exit_hook.load(std::memory_order_acquire)) {
      hook();
      g_exit_deadline.compare_exchange_strong(armed, 0, std::memory_order_acq_rel);
      continue;
    }
    EndProcessNow();
  }
}

// Starts the watchdog's thread once per process (MakeAsyncLocked, under g_mutex, before the
// pool). False if the thread could not be started (or a unit test injects that failure).
bool StartExitWatchdog() {
  if (g_exit_watchdog_fail_for_testing.load(std::memory_order_acquire))
    return false;
  if (g_exit_watchdog_running.load(std::memory_order_acquire))
    return true;
  try {
    std::thread(ExitWatchdogMain).detach();
  } catch (...) {
    return false;
  }
  g_exit_watchdog_running.store(true, std::memory_order_release);
  return true;
}

// The drain mark: a private async logger on the same queue whose only sink notes the last mark the
// writer handled. One writer takes the queue in order, so when the writer reaches a mark, every line
// queued before it has been written. A mark can also flush every sink, on the writer, before it is
// noted: so the caller never calls a sink itself (the writer may be inside it, holding its lock, or
// a disk flush may stall) and waits only on the note, until its deadline. The mark uses discard_new,
// so posting it never waits on a full queue (a stuck writer cannot hang FlushLogging past its bound).
class DrainMarkSink final : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
 public:
  std::atomic<uint64_t> seen{0};  // the sequence number of the last mark handled

  // The sinks the next flushing mark flushes (DrainQueue, under g_mutex).
  void SetTargets(std::vector<spdlog::sink_ptr> targets) {
    std::lock_guard lock(targets_mutex_);
    targets_ = std::move(targets);
  }

 protected:
  // The payload is "<sequence number> <1 to flush, else 0>" (DrainQueue).
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const char* begin = msg.payload.data();
    const char* end = begin + msg.payload.size();
    uint64_t seq = 0;
    const auto [next, ec] = std::from_chars(begin, end, seq);
    if (ec != std::errc())
      return;
    if (end - next >= 2 && next[1] == '1') {
      std::vector<spdlog::sink_ptr> targets;
      {
        std::lock_guard lock(targets_mutex_);
        targets = targets_;
      }
      for (auto& sink : targets) {
        try {
          sink->flush();
        } catch (...) {
        }
      }
    }
    seen.store(seq, std::memory_order_release);
  }
  void flush_() override {}

 private:
  std::mutex targets_mutex_;
  std::vector<spdlog::sink_ptr> targets_;
};

std::shared_ptr<DrainMarkSink> g_mark_sink;      // per pool
std::shared_ptr<spdlog::async_logger> g_mark_logger;
uint64_t g_mark_seq = 0;  // per pool, under g_mutex: the last mark posted

// What MakeAsyncLocked (or StartAsyncLocked, or EnableAsyncLogging) did.
enum class AsyncStart { kStarted, kNoWatchdog, kNoQueue, kNoLoggers };

// Async mode's parts, made but not yet published (MakeAsyncLocked, then PublishAsyncLocked).
struct AsyncParts {
  std::shared_ptr<spdlog::details::thread_pool> pool;
  std::shared_ptr<DrainMarkSink> mark_sink;
  std::shared_ptr<spdlog::async_logger> mark_logger;
};

// The first half of async mode's one start sequence, for both entries (InitLogging with log_async,
// and EnableAsyncLogging at a client's first online session), so the invariant's order exists in
// one place: the exit watchdog's thread first, then the pool and the drain mark, made but not
// published (kStarted). On a failure nothing is published (no pool, nothing armable, no logger
// changed) and async mode is refused for the rest of the process (g_log_async_refused): the
// watchdog's thread could not start (kNoWatchdog), or the queue (log_async_queue lines, about 13 MB
// by default) or its writer thread could not be made (kNoQueue; not worth a retry). Before
// 2026-11-07 a throwing pool construction escaped InitLogging; it is caught now. A pool made here
// and never published is destroyed with `parts` (its idle writer is joined); the watchdog's thread
// then stays parked, never armed. Caller holds g_mutex, and g_pool is null.
AsyncStart MakeAsyncLocked(AsyncParts& parts) {
  if (!StartExitWatchdog()) {
    g_log_async_refused.store(true, std::memory_order_release);
    return AsyncStart::kNoWatchdog;
  }
  const size_t queue = static_cast<size_t>(std::max<int32_t>(1024, REXCVAR_GET(log_async_queue)));
  try {
    parts.pool =
        std::make_shared<spdlog::details::thread_pool>(queue, 1, [] { t_log_writer = true; });
    parts.mark_sink = std::make_shared<DrainMarkSink>();
    parts.mark_logger = std::make_shared<spdlog::async_logger>(
        "rex.log.drain", parts.mark_sink, parts.pool, spdlog::async_overflow_policy::discard_new);
    parts.mark_logger->set_level(spdlog::level::trace);
    parts.mark_logger->flush_on(spdlog::level::off);
  } catch (...) {
    parts = {};
    g_log_async_refused.store(true, std::memory_order_release);
    return AsyncStart::kNoQueue;
  }
  return AsyncStart::kStarted;
}

// The second half: publishes what MakeAsyncLocked made, the pool first, then armable, then the
// drain mark. Cannot throw (moves and atomic stores). Caller holds g_mutex, not g_level_mutex.
void PublishAsyncLocked(AsyncParts& parts) noexcept {
  {
    std::lock_guard pool_lock(g_pool_mutex);
    g_pool = std::move(parts.pool);
  }
  // Armable from here (the watchdog runs; no logger is asynchronous yet, so nothing can have needed
  // it before).
  g_exit_watchdog_live.store(true, std::memory_order_release);
  g_pool_raw.store(g_pool.get(), std::memory_order_release);
  g_writer_stuck.store(false, std::memory_order_release);
  g_urgent_no_room.store(false, std::memory_order_release);
  g_mark_seq = 0;
  g_mark_sink = std::move(parts.mark_sink);
  g_mark_logger = std::move(parts.mark_logger);
}

// Both halves at once, for InitLogging (its rebuild then makes the loggers asynchronous). Caller
// holds g_mutex, and g_pool is null.
AsyncStart StartAsyncLocked() {
  AsyncParts parts;
  const AsyncStart start = MakeAsyncLocked(parts);
  if (start == AsyncStart::kStarted)
    PublishAsyncLocked(parts);
  return start;
}

// The warning of a refused start, logged after g_mutex is released (the loggers are synchronous
// then, so it goes straight to the sinks).
void WarnAsyncRefused(AsyncStart start) {
  if (start == AsyncStart::kNoWatchdog)
    REXLOG_WARN("Log: the exit watchdog did not start; logging stays synchronous");
  else if (start == AsyncStart::kNoQueue)
    REXLOG_WARN("Log: the log queue could not be made; logging stays synchronous");
  else if (start == AsyncStart::kNoLoggers)
    REXLOG_WARN("Log: the asynchronous loggers could not be made; logging stays synchronous");
}

// Every logger's sinks, each once. Caller holds g_mutex.
std::vector<spdlog::sink_ptr> DistinctSinks() {
  std::vector<spdlog::sink_ptr> sinks;
  for (auto& entry : g_registry) {
    if (!entry.logger)
      continue;
    for (auto& sink : entry.logger->sinks()) {
      if (sink && std::find(sinks.begin(), sinks.end(), sink) == sinks.end())
        sinks.push_back(sink);
    }
  }
  return sinks;
}

// Waits (polling every 1 ms, until `deadline`) until the writer has handled every line queued
// before the call and, with `flush`, flushed every sink after them. True when it has, or when
// logging is synchronous. False on the writer itself (it cannot wait for itself) and when the
// deadline passed; then the writer counts as stuck (kStuckGrace). Once the exit watchdog is armed
// the deadline ends within the exit budget (CapToExit); a drain cut by it also counts the writer as
// stuck, which costs nothing then (the process is ending). Caller holds g_mutex.
bool DrainQueue(Clock::time_point deadline, bool flush) {
  if (!g_pool || !g_mark_logger || !g_mark_sink)
    return true;
  if (t_log_writer)
    return false;
  deadline = CapToExit(deadline);
  if (g_writer_stuck.load(std::memory_order_acquire))
    deadline = std::min(deadline, Clock::now() + kStuckGrace);
  if (flush)
    g_mark_sink->SetTargets(DistinctSinks());
  uint64_t first = 0;  // this call's first mark: any mark from it on proves the earlier lines
  bool post = true;
  size_t lost = 0;
  for (;;) {
    if (post) {
      // With g_urgent_mutex, like every discard_new post (LogUrgent counts its own drops). If an
      // urgent line holds it until the deadline, no mark goes in and the drain times out.
      std::unique_lock urgent(g_urgent_mutex, deadline);
      if (urgent.owns_lock()) {
        lost = g_pool->discard_counter() + g_pool->overrun_counter();
        const uint64_t seq = ++g_mark_seq;
        g_mark_logger->log(spdlog::level::info, "{} {}", seq, flush ? 1 : 0);
        if (first == 0)
          first = seq;
        post = false;
      }
    }
    if (first != 0 && g_mark_sink->seen.load(std::memory_order_acquire) >= first) {
      g_writer_stuck.store(false, std::memory_order_release);
      g_urgent_no_room.store(false, std::memory_order_release);
      return true;
    }
    if (Clock::now() >= deadline) {
      g_writer_stuck.store(true, std::memory_order_release);
      return false;
    }
    // A full queue drops a mark (discard_new), and an urgent line can push a queued one out
    // (overrun_oldest): then post another.
    if (g_pool->discard_counter() + g_pool->overrun_counter() != lost)
      post = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// LogUrgent on the log_async writer itself: a crash or a fatal error inside a sink (the crash
// handler runs on the crashing thread). Queuing would wait forever on a full queue (the writer is
// the only consumer) and the line would never be written anyway, so it goes straight to the
// logger's sinks and is flushed, except to the sink the writer is inside (its lock is held by this
// thread): that sink misses the line, which is counted (UrgentLogCounts). The lines still queued
// are lost; the process is ending. Nothing bounds a direct sink call, so the exit watchdog is armed
// first (Sol's narrow round B round 2, M1): if a sink blocks in its write or flush here, the process
// ends kExitBound after the arm anyway; the delivery to the sinks before it is best effort.
void WriteOnWriter(spdlog::logger& logger, spdlog::level::level_enum level, std::string_view text,
                   const spdlog::source_loc& loc) {
  ArmExitWatchdog();
  const spdlog::details::log_msg msg(loc, logger.name(), level,
                                     spdlog::string_view_t(text.data(), text.size()));
  bool missed = false;
  for (auto& sink : logger.sinks()) {
    if (!sink)
      continue;
    if (sink.get() == t_inside_sink) {
      missed = true;
      continue;
    }
    if (!sink->should_log(level))
      continue;
    try {
      sink->log(msg);
      sink->flush();
    } catch (...) {
    }
  }
  if (missed)
    g_urgent_missed.fetch_add(1, std::memory_order_relaxed);
}

std::filesystem::path NextSequentialLogPath(const std::filesystem::path& logs_dir,
                                            std::string_view app_name) {
  std::filesystem::create_directories(logs_dir);

  int max_seq = 0;
  std::string prefix = std::string(app_name) + "_";
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    if (!entry.is_regular_file())
      continue;
    auto stem = entry.path().stem().string();
    if (stem.starts_with(prefix)) {
      auto num_str = stem.substr(prefix.size());
      int num = 0;
      auto [ptr, parse_ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), num);
      if (parse_ec == std::errc() && ptr == num_str.data() + num_str.size())
        max_seq = std::max(max_seq, num);
    }
  }

  return logs_dir / fmt::format("{}_{:03d}.log", app_name, max_seq + 1);
}

std::vector<spdlog::sink_ptr> BuildDefaultSinks() {
  std::vector<spdlog::sink_ptr> sinks;
  if (g_early_sink)
    sinks.push_back(g_early_sink);
  if (g_console_sink)
    sinks.push_back(g_console_sink);
  if (g_file_sink)
    sinks.push_back(g_file_sink);
  for (auto& s : g_extra_sinks)
    sinks.push_back(s);
  return sinks;
}

// Build sinks for a specific category (handles per-category sinks from config)
std::vector<spdlog::sink_ptr> BuildCategorySinks(const std::string& name) {
  auto it = g_config.category_sinks.find(name);
  if (it != g_config.category_sinks.end()) {
    if (g_config.category_sinks_exclusive) {
      // Replace default sinks entirely
      return it->second;
    }
    // Additive: default sinks + category-specific sinks
    auto sinks = BuildDefaultSinks();
    for (auto& s : it->second)
      sinks.push_back(s);
    return sinks;
  }
  return BuildDefaultSinks();
}

// Resolve per-category level from config, or return default
spdlog::level::level_enum ResolveCategoryLevel(const std::string& name) {
  auto it = g_config.category_levels.find(name);
  if (it != g_config.category_levels.end())
    return it->second;
  return g_config.default_level;
}

// Create a logger and register it
std::shared_ptr<spdlog::logger> CreateCategoryLogger(const std::string& name) {
  auto sinks = BuildCategorySinks(name);
  // Async mode: the late loggers queue to the writer (block policy: a full queue makes the caller
  // wait rather than drop a line), also a category registered after EnableAsyncLogging. The early
  // loggers (InitLoggingEarly) stay synchronous.
  std::shared_ptr<spdlog::logger> logger;
  if (g_pool)
    logger = std::make_shared<spdlog::async_logger>(name, sinks.begin(), sinks.end(), g_pool,
                                                    spdlog::async_overflow_policy::block);
  else
    logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_level(ResolveCategoryLevel(name));
  // On an async logger this flush runs on the writer, after the line (backend_flush_), so the
  // file still gets every info line at once.
  logger->flush_on(g_config.flush_level);
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace

std::string RedactUserData(std::string_view line) {
  std::string text(line);
  const Redactions& r = GetRedactions();
  if (r.profile.size() >= 4)
    ReplaceAll(text, r.profile, "%USERPROFILE%", false);
  ReplaceUserFolders(text);
  for (const auto& [word, with] : r.words)
    ReplaceAll(text, word, with, true);
  return text;
}

void InitLoggingEarly() {
  std::lock_guard lock(g_mutex);
  if (g_early_initialized || g_initialized)
    return;

#if REX_PLATFORM_WIN32
  auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
  auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
#endif
  sink->set_level(spdlog::level::trace);
  sink->set_pattern("[%l] [%n] %v");
  g_early_sink = Redacted(sink, "debug");

  g_config.default_level = kDefaultLogLevel;

  // Create loggers for any categories already registered during static init
  for (size_t i = 0; i < g_registry.size(); ++i) {
    auto& entry = g_registry[i];
    if (!entry.name.empty() && !entry.logger)
      SetEntryLogger(i, CreateCategoryLogger(entry.name));
  }

  // Set default logger to "core" if registered
  for (auto& entry : g_registry) {
    if (entry.name == "core") {
      spdlog::set_default_logger(entry.logger);
      break;
    }
  }
  g_early_initialized = true;
}

void InitLogging(const LogConfig& config) {
  std::unique_lock lock(g_mutex);

  if (g_initialized) {
    g_config = config;
    std::lock_guard level_lock(g_level_mutex);  // set_level, flush_on: no sink call
    for (auto& entry : g_registry) {
      if (!entry.logger)
        continue;
      entry.logger->set_level(ResolveCategoryLevel(entry.name));
      entry.logger->flush_on(config.flush_level);
      if (g_config.category_levels.count(entry.name))
        entry.has_explicit_level = true;
    }
    return;
  }

  g_config = config;

  // Early sink handling:
  //   Windows: the early msvc_sink is the persistent debug channel for GUI
  //     apps and does not conflict with the stdout console sink, so keep it.
  //   Non-Windows: drop the early stdout sink unconditionally so file-only
  //     configs don't leak to stdout and console configs don't duplicate.
#if !REX_PLATFORM_WIN32
  if (g_early_sink) {
    for (auto& entry : g_registry) {
      if (entry.logger)
        std::erase(entry.logger->sinks(), g_early_sink);
    }
    g_early_sink.reset();
  }
#endif

  // Console sink (stdout, colored). Intended for console-subsystem processes.
  if (config.log_to_console) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.console_pattern);
    g_console_sink = Redacted(sink, "console");
  }

  // File sink (rotating) with sequential naming fallback
  std::string resolved_path;
  if (config.log_file) {
    resolved_path = config.log_file;
  } else if (!config.app_name.empty()) {
    auto log_dir = config.log_dir.empty() ? std::filesystem::current_path() / "logs"
                                          : std::filesystem::path(config.log_dir);
    resolved_path = NextSequentialLogPath(log_dir, config.app_name).string();
  }
  if (!resolved_path.empty()) {
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        resolved_path, static_cast<size_t>(REXCVAR_GET(log_max_file_size_mb)) * 1024 * 1024,
        static_cast<size_t>(REXCVAR_GET(log_max_files)), false);
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = Redacted(sink, "file");
  }

  g_extra_sinks.clear();
  for (auto& sink : config.extra_sinks)
    g_extra_sinks.push_back(AddedSink(sink));

  // log_async: one writer thread, so the lines of all threads keep one order; stamps and thread ids
  // are taken at the call. The pool is this module's own (not spdlog's global one), so nothing else
  // in the process can replace it under the loggers. StartAsyncLocked starts the exit watchdog first
  // (the invariant at the top of the async section): before the pool and before any logger turns
  // asynchronous, and before any crash can need it (a thread started in a crash handler could fail
  // or take the loader lock). If it cannot start, or the pool cannot be made, async mode is refused
  // for the process: no pool, synchronous loggers, and one warning through them (below).
  AsyncStart async_start = AsyncStart::kStarted;
  if (REXCVAR_GET(log_async) && !g_pool && !g_log_async_refused.load(std::memory_order_acquire))
    async_start = StartAsyncLocked();

  // Rebuild all loggers with new sinks (asynchronous ones when the pool was just made)
  for (size_t i = 0; i < g_registry.size(); ++i) {
    auto& entry = g_registry[i];
    if (entry.name.empty())
      continue;
    if (entry.logger)
      spdlog::drop(entry.name);
    std::shared_ptr<spdlog::logger> logger = CreateCategoryLogger(entry.name);
    std::lock_guard level_lock(g_level_mutex);
    SetEntryLoggerLocked(i, std::move(logger));
    if (g_config.category_levels.count(entry.name))
      entry.has_explicit_level = true;
  }
  if (g_pool)
    g_loggers_async.store(true, std::memory_order_release);

  // Set default logger to "core"
  for (auto& entry : g_registry) {
    if (entry.name == "core") {
      spdlog::set_default_logger(entry.logger);
      break;
    }
  }
  g_initialized = true;

  // Periodic flush
  int flush_interval = REXCVAR_GET(log_flush_interval);
  if (flush_interval > 0)
    spdlog::flush_every(std::chrono::seconds(flush_interval));

  // Outside g_mutex: the first use of rex::log::core() registers it, which takes g_mutex.
  const size_t async_queue =
      g_pool ? static_cast<size_t>(std::max<int32_t>(1024, REXCVAR_GET(log_async_queue))) : 0;
  lock.unlock();
  if (async_queue)
    REXLOG_INFO("Log: asynchronous, queue {} lines, one writer", async_queue);
  WarnAsyncRefused(async_start);
}

void InitLogging(const char* log_file, spdlog::level::level_enum level) {
  LogConfig config;
  config.log_file = log_file;
  config.default_level = level;
  InitLogging(config);
}

// The run-time entry into async mode (the owner's decision of 2026-11-07 after P1n, run
// 20261107-014744-log-stall: "1 but only online"; docs/online-1B.md amendment an). A client's
// synchronous file sink stalled 24 s on a disk stall, its network pump waited on it to log one line,
// and the client dropped the session; S, asynchronous from InitLogging, stayed up. Offline play
// before any session must stay exactly as it is, so log_async stays off for clients and the game
// calls this once its first session starts. One-way: it never switches back (that would join the
// writer, which can hang behind a stuck sink, and would need a drain producers cannot overtake).
//
// Design A of the review: every category gets a new async_logger with the old one's sinks, level
// and flush level, published to g_raw (release); the synchronous loggers it replaces go to
// g_retired and are never freed, so a pointer taken before the swap stays valid and logs
// synchronously (spdlog's async_logger is final and gets its pool only in its constructor, so a
// logger cannot become asynchronous in place). Never spdlog::drop, which resets spdlog's default
// logger when the name is "core" while a concurrent spdlog::error may read it, and never
// set_default_logger (not safe against default_logger_raw): the default logger stays the old "core"
// logger, alive in g_retired. spdlog's own map is not touched either (since the review of
// 2026-11-07; no register_or_replace): that takes spdlog's map lock, which spdlog's periodic flush
// (flush_every, log_flush_interval > 0) holds while it flushes the synchronous loggers, so in a
// disk stall the switch would wait on a sink. Nothing in the SDK, the portal or the game reads that
// map (no spdlog::get or apply_all); it keeps the retired loggers, through which a periodic flush
// still flushes the same sinks on spdlog's own thread, as before the switch.
//
// The order (the review of 2026-11-07): the watchdog, the pool and the mark are made
// (MakeAsyncLocked), then every asynchronous logger is built on that unpublished pool, and only
// when all exist is anything published: the pool and armable (PublishAsyncLocked), then, under
// g_level_mutex, a replacement table (a copy of the current one holding every asynchronous logger,
// the level copied there, so a concurrent level change is never lost) published with one release
// store of g_table. A failure anywhere before the publish (watchdog, queue, a logger out
// of memory) changes nothing, refuses async mode for the process and logs one warning through the
// synchronous loggers.
//
// It never calls a sink and never waits for one: g_mutex is taken with a 3 s deadline (a
// synchronous FlushLogging stuck in a sink holds it), g_level_mutex is never held across a sink
// call, and its one line is queued after the locks are released. The exception: a refused start's
// warning goes through the synchronous loggers, as at InitLogging. Order: the lines of one thread
// to one category keep their order across the switch (a line that took the old pointer is written
// before its call returns; the slot never goes back). Across categories a thread's lines keep their
// order too: every category switches at the same instant (the one store of g_table), so no line is
// queued through a switched category while a later one is written through a category that is not
// switched yet (unit tests U1b and U1c).
bool EnableAsyncLogging() {
  if (g_loggers_async.load(std::memory_order_acquire))
    return true;
  // Never on the writer (it exists only in async mode) and never after a refused start.
  if (t_log_writer || g_log_async_refused.load(std::memory_order_acquire))
    return false;
  std::unique_lock lock(g_mutex, Clock::now() + kDrainBound);
  if (!lock.owns_lock())
    return false;  // no mark: a later call (the next session) tries again
  if (g_loggers_async.load(std::memory_order_acquire))
    return true;
  if (!g_initialized || g_pool || g_log_async_refused.load(std::memory_order_acquire))
    return false;
  // Room for the replaced loggers first, so the swaps below cannot fail on it.
  try {
    if (!g_retired)
      g_retired = new std::vector<std::shared_ptr<spdlog::logger>>();
    g_retired->reserve(g_retired->size() + g_registry.size());
  } catch (...) {
    return false;
  }
  AsyncParts parts;
  AsyncStart start = MakeAsyncLocked(parts);
  struct Swap {
    size_t id;
    std::shared_ptr<spdlog::logger> old;
    std::shared_ptr<spdlog::logger> async;
  };
  std::vector<Swap> swaps;
  LoggerTable* next = nullptr;  // the replacement table, published once below
  if (start == AsyncStart::kStarted) {
    try {
      next = new LoggerTable();  // zero-initialized slots
      swaps.reserve(g_registry.size());
      // g_registry's loggers and their sink lists change only under g_mutex, which is held.
      for (size_t id = 0; id < g_registry.size(); ++id) {
        std::shared_ptr<spdlog::logger> old = g_registry[id].logger;
        if (!old)
          continue;
        // The sinks as the old logger has them now: a category's own AddSink, the capture ring, a
        // test's stall sink and a replaced console sink all come along. No existing sink list is
        // changed. Level and flush level are copied at the publish below.
        auto async = std::make_shared<spdlog::async_logger>(old->name(), old->sinks().begin(),
                                                            old->sinks().end(), parts.pool,
                                                            spdlog::async_overflow_policy::block);
        swaps.push_back(Swap{id, std::move(old), std::move(async)});
      }
    } catch (...) {
      // Out of memory: nothing was published. Refused for the process, like a failed queue.
      g_log_async_refused.store(true, std::memory_order_release);
      start = AsyncStart::kNoLoggers;
    }
  }
  if (start != AsyncStart::kStarted) {
    delete next;    // never published, so no reader can hold it
    swaps.clear();  // never published
    parts = {};     // a made pool is destroyed here (its idle writer is joined)
    lock.unlock();
    WarnAsyncRefused(start);
    return false;
  }
  PublishAsyncLocked(parts);
  {
    // The swaps (nothing here allocates or throws: g_retired was reserved above). The new table
    // starts as a copy of the current one (writers hold g_mutex, which is held here, so it cannot
    // change under the copy), takes every async logger, and is then published with ONE release
    // store: GetLoggerRaw sees every category switch at the same instant (see g_table).
    std::lock_guard level_lock(g_level_mutex);
    LoggerTable* current = g_table.load(std::memory_order_relaxed);
    for (size_t id = 0; id < g_registry.size(); ++id)
      (*next)[id].store((*current)[id].load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (Swap& swap : swaps) {
      swap.async->set_level(swap.old->level());  // the level now (a log_level change is kept)
      swap.async->flush_on(swap.old->flush_level());
      (*next)[swap.id].store(swap.async.get(), std::memory_order_relaxed);
      g_retired->push_back(std::move(swap.old));
      g_registry[swap.id].logger = std::move(swap.async);
    }
    if (void (*hook)() = g_switch_hook.load(std::memory_order_acquire))
      hook();  // unit tests: every category must still read synchronous here
    g_table.store(next, std::memory_order_release);
  }
  g_loggers_async.store(true, std::memory_order_release);
  const size_t queue = static_cast<size_t>(std::max<int32_t>(1024, REXCVAR_GET(log_async_queue)));
  lock.unlock();
  // Marks the switch in the log (another thread's line can be queued just before it).
  REXLOG_INFO("Log: asynchronous from now on, queue {} lines, one writer", queue);
  return true;
}

void ShutdownLogging() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized && !g_early_initialized)
    return;

  // log_async: the queued lines first, then every sink flushed, both on the writer (bounded). A
  // writer that does not get through in time is stuck in a sink; then neither a flush request (it
  // would wait for room) nor joining the writer is safe, so the pool is left to the process exit.
  bool drained = true;
  if (g_pool) {
    drained = DrainQueue(Clock::now() + kDrainBound, true);
  } else {
    for (auto& entry : g_registry)
      if (entry.logger)
        entry.logger->flush();
  }

  spdlog::shutdown();
  std::vector<LogCategoryEntry> registry;
  {
    std::lock_guard level_lock(g_level_mutex);
    LoggerTable* table = g_table.load(std::memory_order_relaxed);
    for (size_t i = 0; i < g_registry.size(); ++i) {
      (*table)[i].store(nullptr, std::memory_order_release);
      g_raw[i].store(nullptr, std::memory_order_release);
    }
    g_table.store(&g_raw, std::memory_order_release);  // a switched table is kept, never freed
    registry.swap(g_registry);
  }
  g_loggers_async.store(false, std::memory_order_release);
  registry.clear();  // the loggers are freed outside the level lock
  g_console_sink.reset();
  g_file_sink.reset();
  g_early_sink.reset();
  g_extra_sinks.clear();
  g_initialized = false;
  g_early_initialized = false;

  // Destroying the pool queues its stop and joins the writer after the lines still queued.
  g_pool_raw.store(nullptr, std::memory_order_release);
  g_mark_logger.reset();
  g_mark_sink.reset();
  std::shared_ptr<spdlog::details::thread_pool> pool;
  {
    std::lock_guard pool_lock(g_pool_mutex);
    pool = std::move(g_pool);
  }
  if (pool && !drained) {
    // Never joined. The pool still exists, so the watchdog stays armable (a writer that wakes and
    // crashes is still bounded).
    new std::shared_ptr<spdlog::details::thread_pool>(std::move(pool));
    return;
  }
  pool.reset();
  // The pool is gone (its writer joined): only now does ArmExitWatchdog stop arming (the invariant:
  // the watchdog is never stopped while a pool exists). An armed deadline is never taken back.
  g_exit_watchdog_live.store(false, std::memory_order_release);
}

// The callers _Exit right after (EndServer, ReXApp::OnClosing, the crash handler, the REX_FATAL
// macros); they need the lines on disk when this returns. Synchronous loggers write on the calling
// thread, so a flush is enough. With log_async a flush request would only be queued (spdlog's async
// flush does not wait), so a drain mark goes into the queue and the writer, on reaching it (every
// earlier line written), flushes every sink and notes it; this thread waits only for the note, at
// most 3 s from the call (the wait for g_mutex included; kStuckGrace once a drain found the writer
// stuck), and never calls a sink itself: the writer may be inside one, holding its lock, and a later
// producer's line may keep it there. Once the exit watchdog is armed, the whole call also ends
// within the exit budget (CapToExit), the thread's first call since the arm kStuckGrace earlier
// (t_exit_reserved). No flush request is queued: with the block policy that could wait without
// bound on a full queue behind a stuck writer. On the writer itself (a crash inside a sink) it
// returns at once: LogUrgent wrote and flushed the crash lines directly, and the queue cannot be
// drained by its only consumer.
void FlushLogging() {
  if (t_log_writer)
    return;
  Clock::duration reserve = Clock::duration::zero();
  const int64_t armed = g_exit_deadline.load(std::memory_order_acquire);
  if (armed != 0 && t_exit_reserved != armed) {
    t_exit_reserved = armed;
    reserve = kStuckGrace;
  }
  auto deadline = CapToExit(Clock::now() + kDrainBound, reserve);
  if (g_writer_stuck.load(std::memory_order_acquire))
    deadline = std::min(deadline, Clock::now() + kStuckGrace);
  std::unique_lock lock(g_mutex, deadline);
  if (!lock.owns_lock())
    return;
  if (!g_pool) {
    for (auto& entry : g_registry)
      if (entry.logger)
        entry.logger->flush();
    return;
  }
  DrainQueue(deadline, true);
}

// A fatal error's or a crash report's line. Synchronous loggers: an ordinary log call, with the
// caller's source location (as REX_LOG_IMPL passes it). log_async: the line must not wait without
// bound for room in the queue (the block policy of the ordinary lines) behind a stuck writer. It
// goes in through a twin of the category's logger (same name, same sinks, so the line looks like
// any other) with discard_new: each attempt claims a free slot under the queue's own lock or is
// dropped, atomically (so an ordinary producer can never take the slot between a room check and
// the enqueue: Sol's narrow round B round 2, M2). The attempts repeat, yielding between them,
// until kUrgentRoomBound has passed (one attempt once a drain found the writer stuck or an earlier
// urgent line's wait ran out, until a drain gets through); only then does the line go in with
// overrun_oldest, pushing the oldest queued line out (counted in the pool's overrun counter,
// UrgentLogCounts). Once the exit watchdog is armed the wait also ends within the exit budget, with
// kStuckGrace left for the abort handler's FlushLogging (CapToExit). On the writer itself it is
// written directly (WriteOnWriter).
void LogUrgent(LogCategoryId category, spdlog::level::level_enum level, std::string_view text,
               spdlog::source_loc loc) {
  spdlog::logger* logger = GetLoggerRaw(category);
  if (!logger || !logger->should_log(level))
    return;
  const spdlog::string_view_t payload(text.data(), text.size());
  if (t_log_writer) {
    WriteOnWriter(*logger, level, text, loc);
    return;
  }
  std::shared_ptr<spdlog::details::thread_pool> pool;
  {
    std::lock_guard pool_lock(g_pool_mutex);
    pool = g_pool;
  }
  if (!pool || !dynamic_cast<spdlog::async_logger*>(logger)) {
    logger->log(loc, level, payload);
    return;
  }
  auto twin_of = [&](spdlog::async_overflow_policy policy) {
    auto twin = std::make_shared<spdlog::async_logger>(logger->name(), logger->sinks().begin(),
                                                       logger->sinks().end(), pool, policy);
    twin->set_level(spdlog::level::trace);
    twin->flush_on(spdlog::level::off);  // flushed by hand below: an automatic one would post too
    return twin;
  };
  const bool wait = !g_writer_stuck.load(std::memory_order_acquire) &&
                    !g_urgent_no_room.load(std::memory_order_acquire);
  const auto deadline =
      CapToExit(Clock::now() + (wait ? kUrgentRoomBound : std::chrono::milliseconds(0)), kStuckGrace);
  const auto claim = twin_of(spdlog::async_overflow_policy::discard_new);
  for (;;) {
    {
      // A deadline in the past still tries the lock once.
      std::unique_lock urgent(g_urgent_mutex, deadline);
      if (urgent.owns_lock()) {
        const size_t dropped = pool->discard_counter();
        claim->log(loc, level, payload);
        if (pool->discard_counter() == dropped) {
          // In. The flush the logger's flush_on would have queued, also discard_new under the
          // lock: if there is no room for it, the caller's FlushLogging flushes.
          if (level >= logger->flush_level())
            claim->flush();
          return;
        }
      }
    }
    if (Clock::now() >= deadline)
      break;
    std::this_thread::yield();
  }
  if (wait)
    g_urgent_no_room.store(true, std::memory_order_release);
  // No room until the deadline: the writer is stuck or far behind. No flush is posted after it (it
  // would push out a second line); the caller's FlushLogging follows.
  twin_of(spdlog::async_overflow_policy::overrun_oldest)->log(loc, level, payload);
}

void ArmExitWatchdog() {
  // Async mode only (its watchdog runs then); synchronous logging keeps the plain crash behavior.
  if (!g_exit_watchdog_live.load(std::memory_order_acquire))
    return;
  int64_t expected = 0;
  int64_t due = (Clock::now() + kExitBound).time_since_epoch().count();
  if (due == 0)
    due = 1;
  // The first arm wins: a later crash line never moves the deadline.
  if (g_exit_deadline.compare_exchange_strong(expected, due, std::memory_order_acq_rel))
    g_exit_deadline.notify_one();
}

void SetSwitchPublishHookForTesting(void (*hook)()) {
  g_switch_hook.store(hook, std::memory_order_release);
}

// Only from inside the publish hook (EnableAsyncLogging's thread holds g_mutex and g_level_mutex
// there, so g_registry cannot change and no lock is taken here), or from a test thread when nothing
// else logs or registers.
void LoggerModeCountsForTesting(size_t& sync, size_t& async) {
  sync = 0;
  async = 0;
  LoggerTable* table = g_table.load(std::memory_order_acquire);
  for (size_t id = 0; id < g_registry.size(); ++id) {
    if (!g_registry[id].logger)
      continue;
    spdlog::logger* logger = (*table)[id].load(std::memory_order_acquire);
    if (!logger)
      continue;
    if (dynamic_cast<spdlog::async_logger*>(logger))
      ++async;
    else
      ++sync;
  }
}

void SetExitWatchdogHookForTesting(void (*hook)()) {
  g_exit_hook.store(hook, std::memory_order_release);
  g_exit_deadline.store(0, std::memory_order_release);
}

void SetExitWatchdogStartFailureForTesting(bool fail) {
  g_exit_watchdog_fail_for_testing.store(fail, std::memory_order_release);
  g_log_async_refused.store(false, std::memory_order_release);
}

bool LogAsyncActive() {
  std::lock_guard pool_lock(g_pool_mutex);
  return g_pool != nullptr;
}

bool ExitWatchdogArmedForTesting() {
  return g_exit_deadline.load(std::memory_order_acquire) != 0;
}

void UrgentLogCounts(size_t& missed_sink, size_t& displaced) {
  missed_sink = g_urgent_missed.load(std::memory_order_relaxed);
  std::shared_ptr<spdlog::details::thread_pool> pool;
  {
    std::lock_guard pool_lock(g_pool_mutex);
    pool = g_pool;
  }
  displaced = pool ? pool->overrun_counter() : 0;
}

bool TakeSlowLogWrite(uint32_t& ms, const char*& sink, size_t& queued) {
  {
    std::lock_guard lock(g_slow_mutex);
    if (g_slow_ms == 0)
      return false;
    ms = g_slow_ms;
    sink = g_slow_sink ? g_slow_sink : "unknown";
    queued = g_slow_queued;
    g_slow_ms = 0;
    g_slow_sink = nullptr;
    g_slow_queued = 0;
  }
  return true;
}

LogCategoryId RegisterLogCategory(const char* name) {
  std::lock_guard lock(g_mutex);

  // Check for duplicates
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = name;
  std::shared_ptr<spdlog::logger> logger;
  if (g_initialized || g_early_initialized)
    logger = CreateCategoryLogger(name);
  std::lock_guard level_lock(g_level_mutex);  // a push_back may move g_registry
  g_registry.push_back(std::move(entry));
  if (logger)
    SetEntryLoggerLocked(id, std::move(logger));
  return LogCategoryId{id};
}

LogCategoryId RegisterLogSubcategory(const char* name, LogCategoryId parent) {
  std::lock_guard lock(g_mutex);

  std::string parent_name;
  if (parent.id < g_registry.size())
    parent_name = g_registry[parent.id].name;
  std::string full_name = parent_name + "." + name;

  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == full_name)
      return LogCategoryId{static_cast<uint16_t>(i)};
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = full_name;
  entry.parent = parent;
  std::shared_ptr<spdlog::logger> logger;
  if (g_initialized || g_early_initialized) {
    logger = CreateCategoryLogger(full_name);
    if (g_config.category_levels.count(full_name))
      entry.has_explicit_level = true;
  }
  std::lock_guard level_lock(g_level_mutex);  // a push_back may move g_registry
  g_registry.push_back(std::move(entry));
  if (logger)
    SetEntryLoggerLocked(id, std::move(logger));
  return LogCategoryId{id};
}

std::optional<LogCategoryId> FindCategory(const std::string& name) {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }
  return std::nullopt;
}

std::span<const LogCategoryEntry> GetAllCategories() {
  // No lock: only safe to call from main thread or after init.
  return {g_registry.data(), g_registry.size()};
}

// The macros' fast path, on any thread without a lock: one acquire load from g_raw (see there), the
// same pointer g_registry holds. The pointer stays valid for the process even if
// EnableAsyncLogging replaces the category's logger right after (g_retired); only ShutdownLogging
// (unit tests) frees loggers.
spdlog::logger* GetLoggerRaw(LogCategoryId category) {
  if (!g_initialized && !g_early_initialized)
    InitLoggingEarly();
  return (*g_table.load(std::memory_order_acquire))[category.id].load(std::memory_order_acquire);
}

// Under g_level_mutex (since 2026-11-07): EnableAsyncLogging may assign the entry on another
// thread. Not g_mutex, which a synchronous FlushLogging holds across its sink flush.
std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category) {
  if (!g_initialized && !g_early_initialized)
    InitLoggingEarly();
  std::lock_guard lock(g_level_mutex);
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger;
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return GetLogger(rex::log::core());
}

namespace {

// The level setters' bodies; caller holds g_level_mutex. The public ones and the log_level callback
// take it (since 2026-11-07), because EnableAsyncLogging copies each logger's level and replaces the
// logger under it: Host runs the switch on its own thread while the UI thread may change levels.
// Only g_level_mutex, never g_mutex, so a setter never waits on a sink (the review of 2026-11-07);
// the mutex is not recursive, hence the split.
void SetCategoryLevelLocked(LogCategoryId category, spdlog::level::level_enum level) {
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->set_level(level);
    }
  }
}

void SetRootLevelLocked(LogCategoryId root, spdlog::level::level_enum level) {
  SetCategoryLevelLocked(root, level);
  for (auto& entry : g_registry) {
    if (entry.parent.has_value() && entry.parent->id == root.id && !entry.has_explicit_level &&
        entry.logger)
      entry.logger->set_level(level);
  }
}

}  // namespace

void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level) {
  std::lock_guard lock(g_level_mutex);
  SetCategoryLevelLocked(category, level);
}

void SetAllLevels(spdlog::level::level_enum level) {
  std::lock_guard lock(g_level_mutex);
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->set_level(level);
  }
}

void SetRootLevel(LogCategoryId root, spdlog::level::level_enum level) {
  std::lock_guard lock(g_level_mutex);
  SetRootLevelLocked(root, level);
}

void RegisterLogLevelCallback() {
  rex::cvar::RegisterChangeCallback("log_level", [](std::string_view, std::string_view value) {
    if (auto level = ParseLogLevel(std::string(value))) {
      // Under the cvar registry's lock: g_level_mutex only, held for set_level calls alone.
      std::lock_guard lock(g_level_mutex);
      for (size_t i = 0; i < g_registry.size(); ++i) {
        auto& entry = g_registry[i];
        if (!entry.parent.has_value() && entry.logger && !entry.has_explicit_level)
          SetRootLevelLocked(LogCategoryId{static_cast<uint16_t>(i)}, *level);
      }
    }
  });
}

// AddSink, RemoveSink and ReplaceConsoleSink change the loggers' sink lists, which the log_async
// writer reads without a lock: they run only before other threads log (ReXApp after InitLogging,
// OnPostInitLogging), as before. With log_async they first let the writer finish what is queued.
// EnableAsyncLogging copies the sink lists they made into the new loggers (a sink added after a
// switch would not reach the retired synchronous loggers; nothing adds one that late).
void AddSink(spdlog::sink_ptr given) {
  std::lock_guard lock(g_mutex);
  DrainQueue(Clock::now() + kDrainBound, false);
  const auto sink = AddedSink(given);
  g_extra_sinks.push_back(sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->sinks().push_back(sink);
  }
}

void AddSink(LogCategoryId category, spdlog::sink_ptr given) {
  std::lock_guard lock(g_mutex);
  DrainQueue(Clock::now() + kDrainBound, false);
  const auto sink = AddedSink(given);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->sinks().push_back(sink);
    }
  }
}

void RemoveSink(spdlog::sink_ptr given) {
  std::lock_guard lock(g_mutex);
  DrainQueue(Clock::now() + kDrainBound, false);
  const auto sink = AddedSink(given);
  std::erase(g_extra_sinks, sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      std::erase(entry.logger->sinks(), sink);
  }
}

void RemoveSink(LogCategoryId category, spdlog::sink_ptr given) {
  std::lock_guard lock(g_mutex);
  DrainQueue(Clock::now() + kDrainBound, false);
  const auto sink = AddedSink(given);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      std::erase(logger->sinks(), sink);
    }
  }
}

void ReplaceConsoleSink(spdlog::sink_ptr given) {
  std::lock_guard lock(g_mutex);
  DrainQueue(Clock::now() + kDrainBound, false);
  const auto sink = Redacted(given, "console");
  for (auto& entry : g_registry) {
    if (!entry.logger)
      continue;
    auto& sinks = entry.logger->sinks();
    if (g_console_sink)
      std::erase(sinks, g_console_sink);
    if (sink)
      sinks.push_back(sink);
  }
  g_console_sink = sink;
}

void SetConsolePattern(const std::string& pattern) {
  if (g_console_sink)
    g_console_sink->set_pattern(pattern);
}

void SetFilePattern(const std::string& pattern) {
  if (g_file_sink)
    g_file_sink->set_pattern(pattern);
}

// ==========================================================================
// CLI Helpers
// ==========================================================================

std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str) {
  static const std::unordered_map<std::string, spdlog::level::level_enum> level_map = {
      {"trace", spdlog::level::trace},  {"debug", spdlog::level::debug},
      {"info", spdlog::level::info},    {"warn", spdlog::level::warn},
      {"warning", spdlog::level::warn}, {"error", spdlog::level::err},
      {"err", spdlog::level::err},      {"critical", spdlog::level::critical},
      {"off", spdlog::level::off},
  };

  std::string lower = level_str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto it = level_map.find(lower);
  if (it != level_map.end())
    return it->second;
  return std::nullopt;
}

spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level) {
  return ParseLogLevel(level_str).value_or(default_level);
}

LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  LogConfig config;
  config.log_file = log_file;

  // Build-type default
  config.default_level = kDefaultLogLevel;

  // Environment variable
  if (auto env_level = rex::platform::env::get("REX_LOG_LEVEL")) {
    if (auto level = ParseLogLevel(*env_level))
      config.default_level = *level;
  } else if (auto env_level2 = rex::platform::env::get("SPDLOG_LEVEL")) {
    if (auto level = ParseLogLevel(*env_level2))
      config.default_level = *level;
  }

  // CLI global level overrides environment
  if (!cli_level.empty()) {
    if (auto level = ParseLogLevel(cli_level))
      config.default_level = *level;
  }

  // Per-category CLI levels (string-keyed)
  for (const auto& [cat_name, level_str] : category_levels) {
    if (level_str.empty())
      continue;
    if (auto level = ParseLogLevel(level_str)) {
      config.category_levels[cat_name] = *level;
    }
  }

  return config;
}

std::map<std::string, std::string> ParseCategoryLevelsFromConfig(
    const std::filesystem::path& config_path) {
  std::map<std::string, std::string> result;
  if (!std::filesystem::exists(config_path))
    return result;

  try {
    toml::table config;
    try {
      config = toml::parse_file(config_path.string());
    } catch (const toml::parse_error&) {
      // A key set twice makes the file invalid; read it as LoadConfig does (cvar.cpp).
      std::ifstream in(config_path, std::ios::binary);
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      config = toml::parse(rex::cvar::CommentOutRepeatedKeys(text), config_path.string());
    }
    auto* log_table = config["log"].as_table();
    if (!log_table)
      return result;
    auto* levels_table = (*log_table)["levels"].as_table();
    if (!levels_table)
      return result;

    for (const auto& [key, value] : *levels_table) {
      if (value.is_string())
        result[std::string(key)] = value.as_string()->get();
    }
  } catch (const toml::parse_error&) {}

  return result;
}

}  // namespace rex
