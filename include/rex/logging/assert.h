/**
 * @file        rex/logging/assert.h
 * @brief       Fatal error and assertion macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/macros.h>

#include <cassert>
#include <cstdlib>

/* =========================================================================
   Fatal Error Macros
   ========================================================================= */

// Each one arms the exit watchdog (::rex::ArmExitWatchdog), logs its [FATAL] line through
// ::rex::LogUrgent (REXLOG_CAT_URGENT) and calls ::rex::FlushLogging() before std::abort(): with
// synchronous loggers that is the log call and the flush they always were (no watchdog runs); with
// log_async the line never waits without bound for queue room behind a stuck writer (at most
// 500 ms, then it pushes the oldest queued line out), FlushLogging waits for the writer to write
// and flush it (at most 3 s), so the line is on disk before the abort, also before an app installs
// its abort handler, and the watchdog ends the process 4 s after the arm if anything still hangs.

/** Log a critical error to Core and abort. */
#define REX_FATAL(fmt, ...)                                         \
  do {                                                              \
    ::rex::ArmExitWatchdog();                                       \
    REXLOG_CAT_URGENT(::rex::log::core(), spdlog::level::critical,  \
                      "[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__);   \
    ::rex::FlushLogging();                                          \
    std::abort();                                                   \
  } while (0)

/** Log a critical error to a specific category and abort. */
#define REX_FATAL_CAT(cat, fmt, ...)                                \
  do {                                                              \
    ::rex::ArmExitWatchdog();                                       \
    REXLOG_CAT_URGENT(cat, spdlog::level::critical,                 \
                      "[FATAL] " fmt __VA_OPT__(, ) __VA_ARGS__);   \
    ::rex::FlushLogging();                                          \
    std::abort();                                                   \
  } while (0)

/** Log a critical error with function name and abort. */
#define REX_FATAL_FN(fmt, ...)                                                \
  do {                                                                        \
    ::rex::ArmExitWatchdog();                                                 \
    REXLOG_CAT_URGENT(::rex::log::core(), spdlog::level::critical,            \
                      "[FATAL] {}: " fmt, __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__); \
    ::rex::FlushLogging();                                                    \
    std::abort();                                                             \
  } while (0)

/** Check condition and abort with fatal error if false. */
#define REX_FATAL_IF(cond, fmt, ...)                                  \
  do {                                                                \
    if (!(cond)) {                                                    \
      ::rex::ArmExitWatchdog();                                       \
      REXLOG_CAT_URGENT(::rex::log::core(), spdlog::level::critical,  \
                        "[FATAL] {}: check failed: " #cond " - " fmt, \
                        __FUNCTION__ __VA_OPT__(, ) __VA_ARGS__);     \
      ::rex::FlushLogging();                                          \
      std::abort();                                                   \
    }                                                                 \
  } while (0)

/* =========================================================================
   Assertion Macros
   ========================================================================= */

/** Log error and assert (debug-only crash). */
#ifdef NDEBUG
// assert() does nothing here: the line is an ordinary one and the code goes on, as before.
#define REX_ASSERT(cond, msg)                                \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      assert(cond);                                          \
    }                                                        \
  } while (0)
#else
// A debug build's assert() aborts, so this is an abort entry (the exit invariant, logging.cpp's
// async section): it arms the exit watchdog before its line, logs it through the urgent path and
// flushes, as REX_FATAL does. Synchronous logging: the same line and a flush. With log_async the
// process then ends 4 s after the arm even if the assertion dialog is answered with Ignore.
#define REX_ASSERT(cond, msg)                                                                \
  do {                                                                                       \
    if (!(cond)) {                                                                           \
      ::rex::ArmExitWatchdog();                                                              \
      REXLOG_CAT_URGENT(::rex::log::core(), spdlog::level::err, "Assertion failed: {} - {}", \
                        #cond, msg);                                                         \
      ::rex::FlushLogging();                                                                 \
      assert(cond);                                                                          \
    }                                                                                        \
  } while (0)
#endif

/** Log error and return a value if condition fails. */
#define REX_ASSERT_RET(cond, msg, retval)                    \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return retval;                                         \
    }                                                        \
  } while (0)

/** Log error and return void if condition fails. */
#define REX_ASSERT_RET_VOID(cond, msg)                       \
  do {                                                       \
    if (!(cond)) {                                           \
      REXLOG_ERROR("Assertion failed: {} - {}", #cond, msg); \
      return;                                                \
    }                                                        \
  } while (0)
