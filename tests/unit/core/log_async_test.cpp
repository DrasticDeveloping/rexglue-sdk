/**
 * @file        core/log_async_test.cpp
 * @brief       Tests for log_async (the asynchronous loggers) and TakeSlowLogWrite
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Why these exist: in Swap Force Online's run 20261106-211124 (P1m) one file write of the dedicated
 * server stalled about 16 s under the sink lock; every thread that logged waited on it, the network
 * pump included, and all clients dropped the server. With log_async a log call only queues the line.
 *
 * Each case starts and ends with ShutdownLogging, because InitLogging decides the mode once.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/async_logger.h>
#include <spdlog/sinks/base_sink.h>

#include <fmt/format.h>

#include <rex/logging.h>

#if defined(_WIN32)
#include <spdlog/details/windows_include.h>  // the exit watchdog's child process
#endif

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

int64_t MsBetween(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration_cast<milliseconds>(b - a).count();
}

// Holds every line until Release(), then keeps them. Its first line marks when the sink blocked.
class LatchSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  void Release() {
    {
      std::lock_guard lock(latch_mutex_);
      released_ = true;
      released_at_ = Clock::now();
    }
    cv_.notify_all();
  }
  bool WaitEntered(milliseconds bound) {
    std::unique_lock lock(latch_mutex_);
    return cv_.wait_for(lock, bound, [&] { return entered_; });
  }
  int64_t BlockedMs() {
    std::lock_guard lock(latch_mutex_);
    return MsBetween(entered_at_, released_at_);
  }
  Clock::time_point EnteredAt() {
    std::lock_guard lock(latch_mutex_);
    return entered_at_;
  }
  std::vector<std::string> Lines() {
    std::lock_guard lock(lines_mutex_);
    return lines_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    {
      std::unique_lock lock(latch_mutex_);
      if (!entered_) {
        entered_ = true;
        entered_at_ = Clock::now();
        cv_.notify_all();
      }
      cv_.wait(lock, [&] { return released_; });
    }
    std::lock_guard lock(lines_mutex_);
    lines_.emplace_back(msg.payload.data(), msg.payload.size());
  }
  void flush_() override {}

 private:
  std::mutex latch_mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
  Clock::time_point entered_at_{};
  Clock::time_point released_at_{};
  std::mutex lines_mutex_;
  std::vector<std::string> lines_;
};

// The exit watchdog's stand-in in this process: a crash on the writer (LogUrgent there) arms it,
// and its default would end the test run 4 s later. Every Session installs this (and disarms).
std::atomic<int> g_watchdog_fired{0};
void CountWatchdog() {
  ++g_watchdog_fired;
}

// One logging setup per case: the mode, a queue large enough for the case (or the smallest, 1024
// lines, to fill it), a category of its own. `flush_level` off: sinks are flushed only by
// FlushLogging (the default flushes after every info line). `extra`: a sink the loggers have from
// InitLogging on (LogConfig::extra_sinks), so it also gets InitLogging's own lines.
struct Session {
  explicit Session(bool async, int32_t queue = 8192,
                   spdlog::level::level_enum flush_level = spdlog::level::info,
                   spdlog::sink_ptr extra = nullptr) {
    rex::SetExitWatchdogHookForTesting(&CountWatchdog);
    rex::ShutdownLogging();
    REXCVAR_SET(log_async, async);
    REXCVAR_SET(log_async_queue, queue);
    rex::LogConfig config;  // no file, no console
    config.default_level = spdlog::level::trace;
    config.flush_level = flush_level;
    if (extra)
      config.extra_sinks.push_back(extra);
    rex::InitLogging(config);
    category = rex::RegisterLogCategory("test.log_async");
    logger = rex::GetLogger(category);
    uint32_t ms = 0;
    const char* sink = nullptr;
    size_t queued = 0;
    rex::TakeSlowLogWrite(ms, sink, queued);  // a record from an earlier case
  }
  ~Session() {
    rex::ShutdownLogging();
    REXCVAR_SET(log_async, false);
    REXCVAR_SET(log_async_queue, 32768);
  }
  rex::LogCategoryId category{0};
  std::shared_ptr<spdlog::logger> logger;
};

// Keeps its lines; once armed, its flush blocks until Release() (the writer then holds its lock).
class FlushBlockSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  void Arm() { armed_ = true; }
  void Release() {
    {
      std::lock_guard lock(latch_mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }
  bool FlushEntered() {
    std::lock_guard lock(latch_mutex_);
    return entered_;
  }
  int Flushes() const { return flushes_.load(); }
  std::vector<std::string> Lines() {
    std::lock_guard lock(lines_mutex_);
    return lines_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::lock_guard lock(lines_mutex_);
    lines_.emplace_back(msg.payload.data(), msg.payload.size());
  }
  void flush_() override {
    if (armed_.load()) {
      std::unique_lock lock(latch_mutex_);
      entered_ = true;
      cv_.wait(lock, [&] { return released_; });
    }
    ++flushes_;
  }

 private:
  std::atomic<bool> armed_{false};
  std::atomic<int> flushes_{0};
  std::mutex latch_mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
  std::mutex lines_mutex_;
  std::vector<std::string> lines_;
};

// Keeps its lines (thread safe through base_sink's mutex; Lines() takes it too).
class CollectSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<std::string> Lines() {
    std::lock_guard lock(mutex_);
    return lines_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    lines_.emplace_back(msg.payload.data(), msg.payload.size());
  }
  void flush_() override {}

 private:
  std::vector<std::string> lines_;
};

bool Contains(const std::vector<std::string>& lines, const std::string& line) {
  for (const auto& l : lines) {
    if (l == line)
      return true;
  }
  return false;
}

// Turns a regression's hang into a failed check: runs `release` after `after` unless disarmed.
class Watchdog {
 public:
  Watchdog(milliseconds after, std::function<void()> release)
      : thread_([this, after, release = std::move(release)] {
          std::unique_lock lock(mutex_);
          if (!cv_.wait_for(lock, after, [&] { return done_; }))
            release();
        }) {}
  ~Watchdog() {
    {
      std::lock_guard lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  std::thread thread_;
};

// Waits until `count` reaches `n` (at most `bound`).
bool WaitCount(const std::atomic<int>& count, int n, milliseconds bound) {
  const auto until = Clock::now() + bound;
  while (count.load() < n) {
    if (Clock::now() >= until)
      return false;
    std::this_thread::sleep_for(milliseconds(1));
  }
  return true;
}

// The thread's lines, in order: "<tag> 0" .. "<tag> <n-1>".
bool InOrder(const std::vector<std::string>& lines, const std::string& tag, int n) {
  int next = 0;
  for (const auto& line : lines) {
    if (line.rfind(tag + " ", 0) != 0)
      continue;
    if (line != tag + " " + std::to_string(next))
      return false;
    ++next;
  }
  return next == n;
}

}  // namespace

TEST_CASE("log_async: a blocked sink does not hold the logging threads", "[log_async]") {
  Session session(true);
  REQUIRE(session.logger);
  auto latch = std::make_shared<LatchSink>();
  rex::AddSink(latch);

  constexpr int kLines = 2000;
  std::atomic<int64_t> slowest_thread_ms{0};
  auto writer = [&](const char* tag) {
    const auto began = Clock::now();
    for (int i = 0; i < kLines; ++i)
      session.logger->info("{} {}", tag, i);
    const int64_t took = MsBetween(began, Clock::now());
    int64_t seen = slowest_thread_ms.load();
    while (took > seen && !slowest_thread_ms.compare_exchange_weak(seen, took)) {
    }
  };

  // (a) 2 x 2000 lines while the sink holds its first line: every call only queues.
  std::thread a(writer, "A"), b(writer, "B");
  a.join();
  b.join();
  REQUIRE(latch->WaitEntered(milliseconds(2000)));
  CHECK(slowest_thread_ms.load() < 100);
  CHECK(latch->Lines().empty());

  // Hold the sink past the slow-write threshold (1000 ms), then let it go.
  while (MsBetween(latch->EnteredAt(), Clock::now()) < 1200)
    std::this_thread::sleep_for(milliseconds(5));
  latch->Release();

  // (b) FlushLogging returns only when the sink has every line, each thread's in order.
  rex::FlushLogging();
  const auto lines = latch->Lines();
  CHECK(lines.size() == 2 * kLines);
  CHECK(InOrder(lines, "A", kLines));
  CHECK(InOrder(lines, "B", kLines));

  // (c) The witness names the added sink and at least the blocked time, once, with the backlog
  // that waited behind the blocked line (the other 3999 lines, maybe FlushLogging's drain mark).
  uint32_t ms = 0;
  const char* sink = nullptr;
  size_t queued = 99;
  REQUIRE(rex::TakeSlowLogWrite(ms, sink, queued));
  CHECK(static_cast<int64_t>(ms) + 1 >= latch->BlockedMs());
  CHECK(latch->BlockedMs() >= 1200);
  CHECK(std::string(sink) == "added");
  CHECK(queued >= static_cast<size_t>(2 * kLines - 1));
  CHECK(queued <= static_cast<size_t>(2 * kLines + 1));
  CHECK_FALSE(rex::TakeSlowLogWrite(ms, sink, queued));
}

TEST_CASE("log_async: FlushLogging gives up after its bound when the writer is stuck", "[log_async]") {
  Session session(true);
  REQUIRE(session.logger);
  auto latch = std::make_shared<LatchSink>();
  rex::AddSink(latch);

  session.logger->info("stuck");
  REQUIRE(latch->WaitEntered(milliseconds(2000)));
  const auto began = Clock::now();
  rex::FlushLogging();  // must not wait for the latch (EndServer and the crash handler _Exit next)
  const int64_t took = MsBetween(began, Clock::now());
  CHECK(took >= 2900);
  CHECK(took < 6000);
  CHECK(latch->Lines().empty());
  latch->Release();
  rex::FlushLogging();
  CHECK(latch->Lines().size() == 1);
}

TEST_CASE("log_async off: a blocked sink holds the logging thread, as before", "[log_async]") {
  // (d) The P1m mechanism: a synchronous call waits for the sink.
  Session session(false);
  REQUIRE(session.logger);
  auto latch = std::make_shared<LatchSink>();
  rex::AddSink(latch);

  std::thread releaser([&] {
    if (latch->WaitEntered(milliseconds(5000)))
      std::this_thread::sleep_for(milliseconds(300));
    latch->Release();
  });
  const auto began = Clock::now();
  session.logger->info("held");
  const int64_t took = MsBetween(began, Clock::now());
  releaser.join();
  CHECK(took >= 290);
  CHECK(latch->Lines().size() == 1);
}

TEST_CASE("log_async: FlushLogging's bound covers a sink that blocks in its flush", "[log_async]") {
  // Sol's narrow round B, F1: the drain gets through, then the sink's flush stalls (a disk or a
  // virus scanner), while another thread keeps logging. The flush runs on the writer; FlushLogging
  // waits for it only until its 3 s deadline and never calls the sink itself.
  Session session(true, 8192, spdlog::level::off);
  REQUIRE(session.logger);
  auto sink = std::make_shared<FlushBlockSink>();
  rex::AddSink(sink);
  Watchdog watchdog(milliseconds(8000), [&] { sink->Release(); });

  session.logger->info("before");
  sink->Arm();
  std::atomic<bool> stop{false};
  std::atomic<int> later{0};
  std::thread producer([&] {
    while (!stop.load() && later.load() < 200) {
      session.logger->info("later {}", later.load());
      ++later;
      std::this_thread::sleep_for(milliseconds(5));
    }
  });
  const auto began = Clock::now();
  rex::FlushLogging();
  const int64_t took = MsBetween(began, Clock::now());
  CHECK(took >= 2900);
  CHECK(took < 4000);
  CHECK(sink->FlushEntered());
  CHECK(Contains(sink->Lines(), "before"));  // the drain itself got through

  // Once the flush returns, the next FlushLogging gets through at once (the writer counted as
  // stuck: 250 ms at most), with every line and a second flush.
  sink->Release();
  stop = true;
  producer.join();
  const auto again = Clock::now();
  rex::FlushLogging();
  CHECK(MsBetween(again, Clock::now()) < 1000);
  CHECK(sink->Lines().size() == static_cast<size_t>(1 + later.load()));
  CHECK(sink->Flushes() >= 2);
}

TEST_CASE("log_async: a fatal line does not wait for room behind a stuck writer", "[log_async]") {
  // Sol's narrow round B, F2: the writer is stuck in a sink and the queue (1024 lines) is full, so
  // an ordinary line would wait forever. The fatal path (REX_FATAL's LogUrgent, then FlushLogging)
  // returns within 500 ms + 3 s; the crash handler's lines after it within 250 ms.
  Session session(true, 1024, spdlog::level::off);
  REQUIRE(session.logger);
  auto latch = std::make_shared<LatchSink>();
  rex::AddSink(latch);
  Watchdog watchdog(milliseconds(10000), [&] { latch->Release(); });

  session.logger->info("stuck");
  REQUIRE(latch->WaitEntered(milliseconds(2000)));
  constexpr int kLines = 1100;
  std::atomic<int> produced{0};
  std::thread producer([&] {
    for (int i = 0; i < kLines; ++i) {
      session.logger->info("P {}", i);
      ++produced;
    }
  });
  REQUIRE(WaitCount(produced, 1024, milliseconds(5000)));
  std::this_thread::sleep_for(milliseconds(50));
  REQUIRE(produced.load() == 1024);  // the queue is full: the producer waits for room

  size_t missed0 = 0, displaced0 = 0;
  rex::UrgentLogCounts(missed0, displaced0);
  const auto began = Clock::now();
  rex::LogUrgent(session.category, spdlog::level::critical, "[FATAL] test fatal");
  const int64_t enqueue_ms = MsBetween(began, Clock::now());
  rex::FlushLogging();
  const int64_t fatal_ms = MsBetween(began, Clock::now());
  CHECK(enqueue_ms >= 450);
  CHECK(enqueue_ms < 1000);
  CHECK(fatal_ms < 4000);

  const auto crash = Clock::now();
  rex::LogUrgent(session.category, spdlog::level::err, "Crash: test crash");
  rex::FlushLogging();
  CHECK(MsBetween(crash, Clock::now()) < 600);

  size_t missed = 0, displaced = 0;
  rex::UrgentLogCounts(missed, displaced);
  CHECK(displaced - displaced0 == 2);  // each urgent line pushed the oldest queued line out
  CHECK(missed == missed0);

  latch->Release();
  producer.join();
  rex::FlushLogging();
  const auto lines = latch->Lines();
  CHECK(Contains(lines, "stuck"));
  CHECK(Contains(lines, "[FATAL] test fatal"));
  CHECK(Contains(lines, "Crash: test crash"));
  CHECK(lines.size() == static_cast<size_t>(1 + kLines + 2 - 2));
}

TEST_CASE("log_async: a crash handler's urgent lines wait for room only once", "[log_async]") {
  // Narrow round B review: a crash (not a REX_FATAL, so no drain has timed out yet) logs up to three
  // urgent lines before its FlushLogging. Only the first waits kUrgentRoomBound; the others go in at
  // once, so the handler takes about 500 ms + 3 s, not 1.5 s + 3 s.
  Session session(true, 1024, spdlog::level::off);
  REQUIRE(session.logger);
  auto latch = std::make_shared<LatchSink>();
  rex::AddSink(latch);
  Watchdog watchdog(milliseconds(10000), [&] { latch->Release(); });

  session.logger->info("stuck");
  REQUIRE(latch->WaitEntered(milliseconds(2000)));
  constexpr int kLines = 1100;
  std::atomic<int> produced{0};
  std::thread producer([&] {
    for (int i = 0; i < kLines; ++i) {
      session.logger->info("P {}", i);
      ++produced;
    }
  });
  REQUIRE(WaitCount(produced, 1024, milliseconds(5000)));
  std::this_thread::sleep_for(milliseconds(50));
  REQUIRE(produced.load() == 1024);

  size_t missed0 = 0, displaced0 = 0;
  rex::UrgentLogCounts(missed0, displaced0);
  const auto began = Clock::now();
  rex::LogUrgent(session.category, spdlog::level::err, "Crash: one");
  rex::LogUrgent(session.category, spdlog::level::err, "Crash: two");
  rex::LogUrgent(session.category, spdlog::level::info, "Where (crash): three");
  const int64_t lines_ms = MsBetween(began, Clock::now());
  rex::FlushLogging();
  CHECK(lines_ms >= 450);
  CHECK(lines_ms < 1000);
  CHECK(MsBetween(began, Clock::now()) < 4000);

  size_t missed = 0, displaced = 0;
  rex::UrgentLogCounts(missed, displaced);
  CHECK(displaced - displaced0 == 3);

  latch->Release();
  producer.join();
  rex::FlushLogging();
  const auto lines = latch->Lines();
  CHECK(Contains(lines, "Crash: one"));
  CHECK(Contains(lines, "Crash: two"));
  CHECK(Contains(lines, "Where (crash): three"));
  CHECK(lines.size() == static_cast<size_t>(1 + kLines + 3 - 3));
}

namespace {

// Runs `on_first` on the writer, inside this sink (its lock held), on its first line: a crash
// handler running on the crashing writer thread.
class CrashSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit CrashSink(std::function<void()> on_first) : on_first_(std::move(on_first)) {}

 protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    if (!fired_) {
      fired_ = true;
      on_first_();
    }
  }
  void flush_() override {}

 private:
  std::function<void()> on_first_;
  bool fired_ = false;
};

}  // namespace

TEST_CASE("log_async: a crash on the writer thread with a full queue does not deadlock",
          "[log_async]") {
  // Sol's narrow round B, F2: the writer is the queue's only consumer. Its crash handler's lines
  // go straight to the other sinks (not to the one it crashed in), and its FlushLogging returns.
  Session session(true, 1024, spdlog::level::off);
  REQUIRE(session.logger);
  auto collect = std::make_shared<CollectSink>();
  std::atomic<int> produced{0};
  std::atomic<bool> handled{false};
  std::atomic<int64_t> handler_ms{-1};
  std::atomic<int> produced_at_crash{0};
  auto crash = std::make_shared<CrashSink>([&] {
    // Wait (bounded) until the producer has filled the queue behind this line.
    WaitCount(produced, 1024, milliseconds(5000));
    std::this_thread::sleep_for(milliseconds(20));
    produced_at_crash = produced.load();
    const auto began = Clock::now();
    rex::LogUrgent(session.category, spdlog::level::err, "Crash: on the writer");
    rex::FlushLogging();
    handler_ms = MsBetween(began, Clock::now());
    handled = true;
  });
  rex::AddSink(collect);
  rex::AddSink(crash);
  size_t missed0 = 0, displaced0 = 0;
  rex::UrgentLogCounts(missed0, displaced0);

  session.logger->info("crash");
  constexpr int kLines = 1100;
  std::thread producer([&] {
    for (int i = 0; i < kLines; ++i) {
      session.logger->info("P {}", i);
      ++produced;
    }
  });
  const auto until = Clock::now() + milliseconds(10000);
  while (!handled.load() && Clock::now() < until)
    std::this_thread::sleep_for(milliseconds(5));
  REQUIRE(handled.load());
  CHECK(produced_at_crash.load() == 1024);  // the queue was full while the handler ran
  CHECK(handler_ms.load() < 200);
  CHECK(Contains(collect->Lines(), "Crash: on the writer"));  // written directly
  size_t missed = 0, displaced = 0;
  rex::UrgentLogCounts(missed, displaced);
  CHECK(missed - missed0 == 1);  // the sink it crashed in missed the line
  CHECK(displaced == displaced0);

  producer.join();
  rex::FlushLogging();
  CHECK(collect->Lines().size() == static_cast<size_t>(1 + 1 + kLines));
}

namespace {

// Blocks forever in its write or in its flush (whichever `where` says) until Release(): a disk or
// a virus scanner that never returns.
class StallSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  enum class Where { kLog, kFlush };
  explicit StallSink(Where where) : where_(where) {}
  void Release() {
    {
      std::lock_guard lock(latch_mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }
  bool Entered() {
    std::lock_guard lock(latch_mutex_);
    return entered_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg&) override {
    if (where_ == Where::kLog)
      Stall();
  }
  void flush_() override {
    if (where_ == Where::kFlush)
      Stall();
  }

 private:
  void Stall() {
    std::unique_lock lock(latch_mutex_);
    entered_ = true;
    cv_.wait(lock, [&] { return released_; });
  }
  const Where where_;
  std::mutex latch_mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
};

// The in-process watchdog test's hook: notes when the watchdog fired and lets the stuck sink go,
// where the real watchdog would have ended the process.
std::atomic<StallSink*> g_stall_to_release{nullptr};
std::atomic<int64_t> g_watchdog_fired_at{0};  // steady_clock ticks
void ReleaseOnWatchdog() {
  g_watchdog_fired_at = Clock::now().time_since_epoch().count();
  if (auto* stall = g_stall_to_release.load())
    stall->Release();
}

// Keeps each line formatted with its pattern (set_pattern), without the line end.
class FormatSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<std::string> Lines() {
    std::lock_guard lock(mutex_);
    return lines_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t buf;
    formatter_->format(msg, buf);
    std::string line(buf.data(), buf.size());
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    lines_.push_back(std::move(line));
  }
  void flush_() override {}

 private:
  std::vector<std::string> lines_;
};

// A healthy writer that is slower than the producers: each line takes `per_line` (busy wait).
class SlowSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit SlowSink(std::chrono::microseconds per_line) : per_line_(per_line) {}
  std::vector<std::string> Lines() {
    std::lock_guard lock(mutex_);
    return lines_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const auto until = Clock::now() + per_line_;
    while (Clock::now() < until) {
    }
    lines_.emplace_back(msg.payload.data(), msg.payload.size());
  }
  void flush_() override {}

 private:
  const std::chrono::microseconds per_line_;
  std::vector<std::string> lines_;
};

}  // namespace

TEST_CASE("log_async: the exit watchdog ends a crash on the writer stuck in a sink", "[log_async]") {
  // Sol's narrow round B round 2, M1: on the writer the crash line goes to the sinks directly, and
  // nothing else bounds that. The watchdog, armed before the first direct sink call, fires 4 s
  // later whether the sink blocks in its write or in its flush; the sink before it got the line.
  for (const auto where : {StallSink::Where::kLog, StallSink::Where::kFlush}) {
    INFO("the sink blocks in its " << (where == StallSink::Where::kLog ? "log" : "flush"));
    Session session(true, 1024, spdlog::level::off);
    REQUIRE(session.logger);
    auto collect = std::make_shared<CollectSink>();
    auto stall = std::make_shared<StallSink>(where);
    std::atomic<bool> handled{false};
    std::atomic<int64_t> began_at{0};
    auto crash = std::make_shared<CrashSink>([&] {
      began_at = Clock::now().time_since_epoch().count();
      rex::LogUrgent(session.category, spdlog::level::err, "Crash: on the writer");
      rex::FlushLogging();
      handled = true;
    });
    g_stall_to_release = stall.get();
    g_watchdog_fired_at = 0;
    rex::SetExitWatchdogHookForTesting(&ReleaseOnWatchdog);
    Watchdog guard(milliseconds(10000), [&] { stall->Release(); });  // a regression fails, not hangs
    rex::AddSink(collect);
    rex::AddSink(crash);
    rex::AddSink(stall);

    session.logger->info("crash");
    const auto until = Clock::now() + milliseconds(12000);
    while (!handled.load() && Clock::now() < until)
      std::this_thread::sleep_for(milliseconds(5));
    REQUIRE(handled.load());
    CHECK(stall->Entered());
    REQUIRE(g_watchdog_fired_at.load() != 0);
    const int64_t fired_ms = std::chrono::duration_cast<milliseconds>(
                                 Clock::duration(g_watchdog_fired_at.load() - began_at.load()))
                                 .count();
    CHECK(fired_ms >= 3990);
    CHECK(fired_ms < 4600);
    CHECK(Contains(collect->Lines(), "Crash: on the writer"));
    g_stall_to_release = nullptr;
    rex::SetExitWatchdogHookForTesting(&CountWatchdog);
  }
}

TEST_CASE("log_async: an armed crash path's waits end before the exit watchdog", "[log_async]") {
  // Sol's narrow round B round 3: once armed, the bounded waits share the watchdog's budget. A
  // REX_FATAL's FlushLogging that starts 1.5 s after the arm would take its full 3 s (4.5 s); it ends
  // 4 s - 150 ms - 250 ms after the arm instead, and the abort handler's line and FlushLogging after
  // it (same thread, writer stuck) end 150 ms before the watchdog, which has not fired then.
  Session session(true, 1024, spdlog::level::off);
  REQUIRE(session.logger);
  auto latch = std::make_shared<LatchSink>();
  rex::AddSink(latch);
  Watchdog guard(milliseconds(10000), [&] { latch->Release(); });

  session.logger->info("stuck");
  REQUIRE(latch->WaitEntered(milliseconds(2000)));
  const int fired0 = g_watchdog_fired.load();
  const auto armed = Clock::now();
  rex::ArmExitWatchdog();
  std::this_thread::sleep_for(milliseconds(1500));  // the handler's earlier work
  rex::FlushLogging();
  const int64_t first_ms = MsBetween(armed, Clock::now());
  rex::LogUrgent(session.category, spdlog::level::err, "Crash: abort");
  rex::FlushLogging();
  const int64_t second_ms = MsBetween(armed, Clock::now());
  CHECK(g_watchdog_fired.load() == fired0);
  CHECK(first_ms >= 3550);
  CHECK(first_ms < 3700);
  CHECK(second_ms >= 3800);
  CHECK(second_ms < 3950);
  CHECK(WaitCount(g_watchdog_fired, fired0 + 1, milliseconds(1000)));  // it was armed

  latch->Release();
  rex::FlushLogging();
  CHECK(Contains(latch->Lines(), "Crash: abort"));
}

TEST_CASE("log_async: if the exit watchdog cannot start, logging stays synchronous", "[log_async]") {
  // The root-cause analysis after Sol's narrow round B round 3 (R1, R4): async mode exists only
  // with a running watchdog, the one mechanism that bounds a crash whatever the sinks do. With its
  // start failing (injected), InitLogging creates no pool and the loggers stay plain synchronous
  // ones; a clean start afterwards turns async mode on again. (The warning line is checked in a
  // fresh child process below: after this program's first ShutdownLogging, rex::log::core()'s
  // cached category id no longer names a registered logger, so InitLogging's own lines are lost
  // here; the game initializes its logging once.)
  struct Injected {  // cleared however the case ends, so the later cases start async normally
    Injected() { rex::SetExitWatchdogStartFailureForTesting(true); }
    ~Injected() { rex::SetExitWatchdogStartFailureForTesting(false); }
  };
  auto injected = std::make_unique<Injected>();
  {
    auto collect = std::make_shared<CollectSink>();
    Session session(true, 8192, spdlog::level::info, collect);
    REQUIRE(session.logger);
    CHECK_FALSE(rex::LogAsyncActive());
    CHECK(dynamic_cast<spdlog::async_logger*>(session.logger.get()) == nullptr);
    // Synchronous: the line is in the sink when the call returns, with no FlushLogging.
    session.logger->info("sync line");
    CHECK(Contains(collect->Lines(), "sync line"));
  }
  injected.reset();
  {
    Session session(true);
    REQUIRE(session.logger);
    CHECK(rex::LogAsyncActive());
    CHECK(dynamic_cast<spdlog::async_logger*>(session.logger.get()) != nullptr);
  }
}

#if defined(_WIN32)

namespace {

// Writes each line straight to a file (WriteFile, no buffer in the process), so what it got before
// TerminateProcess is in the file.
class FileLineSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  explicit FileLineSink(const char* path)
      : file_(CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr)) {}
  ~FileLineSink() override {
    if (file_ != INVALID_HANDLE_VALUE)
      CloseHandle(file_);
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    std::string line(msg.payload.data(), msg.payload.size());
    line += '\n';
    DWORD written = 0;
    if (file_ != INVALID_HANDLE_VALUE)
      WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  }
  void flush_() override {}

 private:
  HANDLE file_;
};

constexpr const char* kWatchdogChildVar = "REX_LOG_WATCHDOG_CHILD";
constexpr const char* kRefusedChildVar = "REX_LOG_REFUSED_CHILD";

// What a child run of this test program left: its exit code, how long it ran, whether it ended
// within the wait, and the file its sink wrote.
struct ChildRun {
  bool ended = false;
  DWORD code = 0;
  int64_t took_ms = 0;
  std::string text;
};

// Runs this test program again as a child running only the hidden case `tag`, with the
// environment variable `var` naming the file its sink writes; waits at most `wait_ms`.
ChildRun RunChild(const wchar_t* tag, const char* var, DWORD wait_ms) {
  ChildRun run;
  wchar_t exe[MAX_PATH] = {};
  REQUIRE(GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
  char temp[MAX_PATH] = {};
  REQUIRE(GetTempPathA(MAX_PATH, temp) > 0);
  const std::string path = std::string(temp) + "rex_log_child_" + std::to_string(GetCurrentProcessId()) +
                           "_" + var + ".log";
  DeleteFileA(path.c_str());

  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE null_out = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inherit, OPEN_EXISTING, 0, nullptr);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = null_out;
  si.hStdOutput = null_out;
  si.hStdError = null_out;
  PROCESS_INFORMATION pi{};
  std::wstring command = std::wstring(L"\"") + exe + L"\" \"" + tag + L"\"";
  SetEnvironmentVariableA(var, path.c_str());
  const auto began = Clock::now();
  const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  SetEnvironmentVariableA(var, nullptr);
  if (null_out != INVALID_HANDLE_VALUE)
    CloseHandle(null_out);
  REQUIRE(created);

  const DWORD waited = WaitForSingleObject(pi.hProcess, wait_ms);
  run.took_ms = MsBetween(began, Clock::now());
  run.ended = waited == WAIT_OBJECT_0;
  if (!run.ended) {
    TerminateProcess(pi.hProcess, 99);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  GetExitCodeProcess(pi.hProcess, &run.code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(file, buf, sizeof(buf), &got, nullptr) && got > 0)
      run.text.append(buf, got);
    CloseHandle(file);
  }
  DeleteFileA(path.c_str());
  return run;
}

}  // namespace

// The child of the next case (hidden; does nothing unless started by it): a crash on the writer
// with a sink that never returns from its write. The real watchdog must end this process.
TEST_CASE("log_async: exit watchdog child", "[.][log_watchdog_child]") {
  char path[1024] = {};
  const DWORD n = GetEnvironmentVariableA(kWatchdogChildVar, path, sizeof(path));
  if (n == 0 || n >= sizeof(path)) {
    SUCCEED("not started as the watchdog child");
    return;
  }
  Session session(true, 1024, spdlog::level::off);
  REQUIRE(session.logger);
  rex::SetExitWatchdogHookForTesting(nullptr);  // the real end of the process
  auto file = std::make_shared<FileLineSink>(path);
  auto stall = std::make_shared<StallSink>(StallSink::Where::kLog);
  auto crash = std::make_shared<CrashSink>([&] {
    rex::LogUrgent(session.category, spdlog::level::err, "Crash: on the writer");
    rex::FlushLogging();
  });
  rex::AddSink(file);
  rex::AddSink(crash);
  rex::AddSink(stall);
  session.logger->info("crash");
  std::this_thread::sleep_for(milliseconds(20000));
  stall->Release();
  FAIL("the exit watchdog did not end the process");
}

TEST_CASE("log_async: the exit watchdog ends the process of a crash stuck on the writer",
          "[log_async]") {
  // M1 for real: this test program again, as a child running only the case above. It must end
  // with the watchdog's exit code 3, 4 s after the crash (plus its start), with the crash line in
  // the file the sink before the stuck one wrote.
  const ChildRun run = RunChild(L"[log_watchdog_child]", kWatchdogChildVar, 15000);
  CHECK(run.ended);
  CHECK(run.code == 3);
  CHECK(run.took_ms >= 4000);
  CHECK(run.took_ms < 7000);
  CHECK(run.text.find("Crash: on the writer\n") != std::string::npos);
}

// The child of the next case (hidden; does nothing unless started by it): a process whose exit
// watchdog cannot start (injected), initializing its logging once, as the game does.
TEST_CASE("log_async: watchdog start failure child", "[.][log_refused_child]") {
  char path[1024] = {};
  const DWORD n = GetEnvironmentVariableA(kRefusedChildVar, path, sizeof(path));
  if (n == 0 || n >= sizeof(path)) {
    SUCCEED("not started as the refused-start child");
    return;
  }
  rex::SetExitWatchdogStartFailureForTesting(true);
  auto file = std::make_shared<FileLineSink>(path);
  Session session(true, 8192, spdlog::level::info, file);
  REQUIRE(session.logger);
  session.logger->info("child: async {}, synchronous logger {}", rex::LogAsyncActive() ? 1 : 0,
                       dynamic_cast<spdlog::async_logger*>(session.logger.get()) ? 0 : 1);
}

TEST_CASE("log_async: a process whose exit watchdog cannot start logs synchronously and says so",
          "[log_async]") {
  // R1/R4 of the root-cause analysis, in a fresh process: no pool, synchronous loggers, the warning
  // through the synchronous sinks during InitLogging, and no `Log: asynchronous` line.
  const ChildRun run = RunChild(L"[log_refused_child]", kRefusedChildVar, 15000);
  CHECK(run.ended);
  CHECK(run.code == 0);
  CHECK(run.text.find("Log: the exit watchdog did not start; logging stays synchronous\n") !=
        std::string::npos);
  CHECK(run.text.find("Log: asynchronous") == std::string::npos);
  CHECK(run.text.find("child: async 0, synchronous logger 1\n") != std::string::npos);
}

#endif  // _WIN32

TEST_CASE("log_async: an urgent line never displaces an ordinary line while the writer keeps up",
          "[log_async]") {
  // Sol's narrow round B round 2, M2: the queue is full because two producers outrun a healthy
  // writer (block policy). An urgent line claims a slot atomically as soon as the writer frees
  // one; it never sees room, loses it to a producer and pushes an ordinary line out.
  Session session(true, 1024, spdlog::level::off);
  REQUIRE(session.logger);
  auto slow = std::make_shared<SlowSink>(std::chrono::microseconds(30));
  rex::AddSink(slow);

  size_t missed0 = 0, displaced0 = 0;
  rex::UrgentLogCounts(missed0, displaced0);
  constexpr int kLines = 20000;
  constexpr int kUrgent = 30;
  std::atomic<int> produced{0};
  auto producer = [&](const char* tag) {
    for (int i = 0; i < kLines; ++i) {
      session.logger->info("{} {}", tag, i);
      ++produced;
    }
  };
  std::thread a(producer, "A"), b(producer, "B");
  REQUIRE(WaitCount(produced, 3000, milliseconds(5000)));  // the queue (1024) is full by now
  int64_t slowest_ms = 0;
  for (int i = 0; i < kUrgent; ++i) {
    const auto began = Clock::now();
    rex::LogUrgent(session.category, spdlog::level::err, fmt::format("U {}", i));
    slowest_ms = std::max(slowest_ms, MsBetween(began, Clock::now()));
    std::this_thread::sleep_for(milliseconds(5));
  }
  const int produced_during = produced.load();
  a.join();
  b.join();
  rex::FlushLogging();

  size_t missed = 0, displaced = 0;
  rex::UrgentLogCounts(missed, displaced);
  CHECK(produced_during < 2 * kLines);  // the producers were still racing at the last urgent line
  CHECK(displaced == displaced0);
  CHECK(slowest_ms < 450);  // each got its slot long before the room deadline
  const auto lines = slow->Lines();
  CHECK(lines.size() == static_cast<size_t>(2 * kLines + kUrgent));
  CHECK(InOrder(lines, "A", kLines));
  CHECK(InOrder(lines, "B", kLines));
  CHECK(InOrder(lines, "U", kUrgent));
}

TEST_CASE("urgent lines carry the caller's source location, log_async off and on", "[log_async]") {
  // Sol's narrow round B round 2, M3: REXLOG_CAT_URGENT passes __FILE__, __LINE__ and __FUNCTION__
  // like REX_LOG_IMPL, to the synchronous log call, the queued line and the writer's direct write.
  for (const bool async : {false, true}) {
    INFO("log_async " << async);
    Session session(async);
    REQUIRE(session.logger);
    auto sink = std::make_shared<FormatSink>();
    sink->set_pattern("%s:%# %! %v");
    rex::AddSink(sink);

    const char* function = __FUNCTION__;
    const int line = __LINE__ + 1;
    REXLOG_CAT_URGENT(session.category, spdlog::level::err, "urgent {}", async ? "on" : "off");
    rex::FlushLogging();
    CHECK(Contains(sink->Lines(), fmt::format("log_async_test.cpp:{} {} urgent {}", line, function,
                                              async ? "on" : "off")));
  }

  // On the writer (a crash inside a sink): the direct write keeps the location too.
  Session session(true);
  REQUIRE(session.logger);
  auto sink = std::make_shared<FormatSink>();
  sink->set_pattern("%s:%# %! %v");
  std::string expected;
  std::atomic<bool> handled{false};
  auto crash = std::make_shared<CrashSink>([&] {
    const char* function = __FUNCTION__;
    const int line = __LINE__ + 1;
    REXLOG_CAT_URGENT(session.category, spdlog::level::err, "urgent on the writer");
    expected = fmt::format("log_async_test.cpp:{} {} urgent on the writer", line, function);
    handled = true;
  });
  rex::AddSink(sink);
  rex::AddSink(crash);
  session.logger->info("crash");
  rex::FlushLogging();
  REQUIRE(handled.load());
  CHECK(Contains(sink->Lines(), expected));
}
