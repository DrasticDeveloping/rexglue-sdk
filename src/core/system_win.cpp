/**
 * @file        core/system_win.cpp
 * @brief       Windows implementations of rex/system.h platform helpers.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#include <rex/string.h>
#include <rex/system.h>

namespace rex {

namespace {
std::atomic<bool> g_message_boxes_suppressed{false};
}  // namespace

void SetSimpleMessageBoxesSuppressed(bool suppressed) {
  g_message_boxes_suppressed.store(suppressed, std::memory_order_relaxed);
}

bool SimpleMessageBoxesSuppressed() {
  return g_message_boxes_suppressed.load(std::memory_order_relaxed);
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  // A dedicated server: no modal box nobody would close (the caller has logged the message).
  if (SimpleMessageBoxesSuppressed()) {
    return;
  }
  UINT flags = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
  const wchar_t* title = L"ReXGlue";
  switch (type) {
    case SimpleMessageBoxType::Help:
      flags |= MB_ICONINFORMATION;
      break;
    case SimpleMessageBoxType::Warning:
      flags |= MB_ICONWARNING;
      break;
    case SimpleMessageBoxType::Error:
      flags |= MB_ICONERROR;
      break;
  }
  auto wide = rex::string::to_utf16(message);
  ::MessageBoxW(nullptr, reinterpret_cast<const wchar_t*>(wide.c_str()), title, flags);
}

}  // namespace rex
