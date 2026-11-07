/**
 * @file        rex/logging/api.h
 * @brief       Logging system function declarations and CVAR declarations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/logging/types.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include <rex/cvar.h>

// Logging CVAR declarations (defined in logging.cpp)
REXCVAR_DECLARE(std::string, log_level);
REXCVAR_DECLARE(std::string, log_file);
REXCVAR_DECLARE(bool, log_verbose);
REXCVAR_DECLARE(bool, log_noisy);
REXCVAR_DECLARE(int32_t, log_flush_interval);
REXCVAR_DECLARE(bool, log_async);
REXCVAR_DECLARE(int32_t, log_async_queue);
REXCVAR_DECLARE(int32_t, log_max_file_size_mb);
REXCVAR_DECLARE(int32_t, log_max_files);

namespace rex {

/* =========================================================================
   Log Level Guidelines
   =========================================================================
   TRACE    - Per-instruction, per-iteration detail (massive output)
   DEBUG    - Development info, function entry/exit, intermediate state
   INFO     - Normal operational events, progress updates
   WARN     - Recoverable issues, fallback behaviors, unsupported features
   ERROR    - Serious problems affecting functionality
   CRITICAL - Fatal errors, memory corruption, unrecoverable state
   ========================================================================= */

/**
 * Initialize the logging system with full configuration.
 *
 * Creates shared sinks and loggers for all built-in categories.
 * Safe to call multiple times; subsequent calls update levels only.
 *
 * @param config  Logging configuration.
 */
void InitLogging(const LogConfig& config);

/**
 * Initialize logging with simple parameters (convenience overload).
 *
 * @param log_file  Path to log file, or nullptr for no file logging.
 * @param level     Default log level for all categories.
 */
void InitLogging(const char* log_file = nullptr,
                 spdlog::level::level_enum level = spdlog::level::info);

/**
 * Early-phase logging initialization (before config is loaded).
 * Creates a platform debug sink (OutputDebugString on Windows, stdout elsewhere)
 * so log lines emitted before InitLogging() is called are captured.
 */
void InitLoggingEarly();

/**
 * Flush all loggers and shut down the logging system.
 */
void ShutdownLogging();

/**
 * Flush all loggers without tearing down the logging system.
 *
 * Use before a hard process exit (std::_Exit) when other threads may still be
 * logging: unlike ShutdownLogging it leaves the registry and sinks intact, so a
 * concurrent logger cannot hit freed state.
 *
 * With log_async it waits until the writer thread has written every line queued
 * before the call and flushed every sink after them, so the lines are on disk when
 * it returns; at most 3 s from the call (250 ms once a drain found the writer
 * stuck), and it never calls a sink itself. On the writer thread it returns at once.
 */
void FlushLogging();

/**
 * Logs one line of a fatal error or a crash report (REX_FATAL*, REXLOG_URGENT_*).
 *
 * With synchronous loggers it is an ordinary log call. With log_async it never waits
 * without bound for room in the queue (ordinary lines do, the block policy): it keeps
 * trying to claim a free slot (atomically, under the queue's lock) for at most 500 ms
 * (one try once a drain found the writer stuck), and only then pushes the oldest
 * queued line out. On the writer thread itself (a crash inside a sink) it arms the
 * exit watchdog (ArmExitWatchdog), then writes and flushes the line directly to every
 * sink except the one the writer is inside. `loc` is the caller's source location
 * (REXLOG_CAT_URGENT passes __FILE__, __LINE__, __FUNCTION__, as the ordinary macros
 * do). Call FlushLogging after it before the process ends.
 */
void LogUrgent(LogCategoryId category, spdlog::level::level_enum level, std::string_view text,
               spdlog::source_loc loc = {});

/**
 * Arms the exit watchdog: with log_async, the process ends (TerminateProcess on Windows,
 * _Exit elsewhere, exit code 3) 4 s after the first call, whatever the log sinks or any
 * other code still do then. For crash and fatal paths only (the REX_FATAL macros, crash
 * handlers; LogUrgent on the log writer arms it itself). Once armed, LogUrgent's room wait
 * and FlushLogging share that budget: each ends 150 ms before the deadline at the latest
 * (a thread's first FlushLogging 250 ms earlier still, for the abort handler's after it),
 * so the watchdog only ends a path stuck in a sink. The first call sets the deadline;
 * later calls keep it. Does nothing with synchronous logging (no pool): log_async off,
 * or a process whose watchdog could not start (InitLogging starts the watchdog before
 * the pool; if it cannot, log_async counts as off for the process, the loggers stay
 * synchronous and it logs `Log: the exit watchdog did not start; logging stays
 * synchronous`).
 */
void ArmExitWatchdog();

/**
 * Unit tests only: what the exit watchdog runs at its deadline instead of ending the
 * process (null: end it), and disarms it. After the hook ran, it can be armed again.
 */
void SetExitWatchdogHookForTesting(void (*hook)());

/**
 * Unit tests only: with `fail`, the next InitLogging's watchdog start fails as if its
 * thread could not be created (log_async then counts as off). Every call also clears
 * an earlier failure's "off for the process", so a later InitLogging tries again.
 */
void SetExitWatchdogStartFailureForTesting(bool fail);

/**
 * Whether the loggers are asynchronous now: log_async was on at InitLogging and its
 * exit watchdog started, so the queue and its writer thread exist.
 */
bool LogAsyncActive();

/**
 * What LogUrgent could not deliver whole, since the start: `missed_sink`, lines a sink
 * missed because the writer was inside it; `displaced`, queued lines an urgent line
 * pushed out of a full queue (this log_async queue; 0 when logging is synchronous).
 */
void UrgentLogCounts(size_t& missed_sink, size_t& displaced);

/**
 * The slowest sink write or flush over 1000 ms since the last call, if any.
 *
 * Every sink is timed around its write and its flush (the redacting wrapper).
 * Returns false when none took that long. Otherwise fills in the time, the sink
 * ("debug", "console", "file", "capture" or "added" for a sink given to AddSink
 * or LogConfig::extra_sinks), and how many lines the log_async queue held when
 * that write ended, i.e. the backlog behind it (0 when logging is synchronous),
 * and clears the record.
 */
bool TakeSlowLogWrite(uint32_t& ms, const char*& sink, size_t& queued);

/**
 * Register a new log category at runtime.
 *
 * The returned handle can be used with REXLOG_CAT_* macros and all
 * category-accepting API functions. The category's logger is created
 * with the current default sinks and any matching entries from the
 * stored LogConfig::category_levels and LogConfig::category_sinks.
 *
 * Thread-safe but intended to be called during startup.
 *
 * @param name  Human-readable category name (e.g. "app.network").
 * @return      Handle for the new category.
 */
LogCategoryId RegisterLogCategory(const char* name);

LogCategoryId RegisterLogSubcategory(const char* name, LogCategoryId parent);
void SetRootLevel(LogCategoryId root, spdlog::level::level_enum level);

/**
 * Look up a category by name.
 *
 * @param name  Category name to search for (case-sensitive).
 * @return      Category handle, or std::nullopt if not found.
 */
std::optional<LogCategoryId> FindCategory(const std::string& name);

/**
 * Get a read-only view of all registered categories.
 *
 * @return  Span over the registry entries in registration order.
 */
std::span<const LogCategoryEntry> GetAllCategories();

/**
 * Get the raw logger pointer for a category (zero overhead).
 *
 * This is the fast path used by logging macros. Returns nullptr if
 * the category is not yet initialized.
 *
 * @param category  Category handle.
 * @return          Raw pointer to the spdlog logger (not owning).
 */
spdlog::logger* GetLoggerRaw(LogCategoryId category);

/**
 * Get the shared logger pointer for a category.
 *
 * Prefer GetLoggerRaw() in hot paths. This overload is useful when
 * you need to hold the logger or manipulate its sinks.
 *
 * @param category  Category handle.
 * @return          Shared pointer to the spdlog logger.
 */
std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category);

/**
 * Get the default (Core) logger.
 *
 * @return  Shared pointer to the Core category logger.
 */
std::shared_ptr<spdlog::logger> GetLogger();

/**
 * Set the log level for a specific category at runtime.
 *
 * @param category  Category handle.
 * @param level     New log level.
 */
void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level);

/**
 * Set the log level for all registered categories.
 *
 * @param level  New log level.
 */
void SetAllLevels(spdlog::level::level_enum level);

/**
 * Register a CVAR change callback for the "log_level" CVAR.
 * Call this after InitLogging() to enable runtime level changes.
 */
void RegisterLogLevelCallback();

/**
 * Add a sink to all current and future loggers.
 *
 * Call it only before other threads log (the loggers' sink lists are not locked
 * against the log_async writer); with log_async it first waits for the queue.
 *
 * @param sink  Shared pointer to the spdlog sink.
 */
void AddSink(spdlog::sink_ptr sink);

/**
 * Add a sink to a specific category's logger only.
 *
 * @param category  Category handle.
 * @param sink      Shared pointer to the spdlog sink.
 */
void AddSink(LogCategoryId category, spdlog::sink_ptr sink);

/**
 * Remove a sink from all loggers.
 *
 * @param sink  The sink to remove (matched by pointer identity).
 */
void RemoveSink(spdlog::sink_ptr sink);

/**
 * Remove a sink from a specific category's logger.
 *
 * @param category  Category handle.
 * @param sink      The sink to remove (matched by pointer identity).
 */
void RemoveSink(LogCategoryId category, spdlog::sink_ptr sink);

/**
 * A log line with the player's data taken out, as every sink gets it: the user folder (USERPROFILE,
 * HOME) becomes %USERPROFILE%, any other user's folder after \Users\ or /home/ <user>, and the
 * user's and the machine's names <user> and <computer> (logs end up in public reports).
 *
 * @param line  The message.
 * @return      The message without them.
 */
std::string RedactUserData(std::string_view line);

/**
 * Replace the global console sink on every registered logger with `sink`.
 * Pass nullptr to remove the console sink without replacement.
 *
 * @param sink  New console sink, or nullptr to remove.
 */
void ReplaceConsoleSink(spdlog::sink_ptr sink);

/**
 * Update the format pattern on the stdout console sink.
 *
 * @param pattern  spdlog pattern string.
 */
void SetConsolePattern(const std::string& pattern);

/**
 * Update the format pattern on the file sink.
 *
 * @param pattern  spdlog pattern string.
 */
void SetFilePattern(const std::string& pattern);

/**
 * Parse a log level string to spdlog level enum.
 *
 * Accepts: "trace", "debug", "info", "warn"/"warning", "error"/"err",
 * "critical", "off". Case-insensitive.
 *
 * @param level_str  Level name string.
 * @return           Parsed level, or std::nullopt if invalid.
 */
std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str);

/**
 * Parse log level from string, returning a default on failure.
 *
 * @param level_str      Level name string.
 * @param default_level  Fallback level if parsing fails.
 * @return               Parsed level or default_level.
 */
spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level);

/**
 * Build a LogConfig from CLI arguments and environment variables.
 *
 * Precedence: CLI args > environment (REX_LOG_LEVEL) > build-type default.
 *
 * @param log_file         Path to log file, or nullptr.
 * @param cli_level        Global level from CLI (empty string = not set).
 * @param category_levels  Per-category level overrides from CLI.
 * @return                 Populated LogConfig.
 */
LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels);

std::map<std::string, std::string> ParseCategoryLevelsFromConfig(
    const std::filesystem::path& config_path);

}  // namespace rex
