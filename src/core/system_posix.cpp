/**
 * @file        core/system_posix.cpp
 * @brief       POSIX implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <atomic>
#include <cstdio>

#include <rex/system.h>

namespace rex {

namespace {
std::atomic<bool> g_message_boxes_suppressed{false};
}  // namespace

// Here the "box" is a stderr line and never blocks; kept for the same API (rex/system.h).
void SetSimpleMessageBoxesSuppressed(bool suppressed) {
  g_message_boxes_suppressed.store(suppressed, std::memory_order_relaxed);
}

bool SimpleMessageBoxesSuppressed() {
  return g_message_boxes_suppressed.load(std::memory_order_relaxed);
}

// TODO(tomc): add linux support for showing a native message box
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* level = "INFO";
  switch (type) {
    case SimpleMessageBoxType::Help:
      level = "INFO";
      break;
    case SimpleMessageBoxType::Warning:
      level = "WARNING";
      break;
    case SimpleMessageBoxType::Error:
      level = "ERROR";
      break;
  }
  std::fprintf(stderr, "[%s] %.*s\n", level, static_cast<int>(message.size()), message.data());
  std::fflush(stderr);
}

}  // namespace rex
