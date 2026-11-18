/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <filesystem>
#include <string_view>

#include <rex/platform.h>
#include <rex/string.h>

namespace rex {

#if REX_PLATFORM_ANDROID
bool InitializeAndroidSystemForApplicationContext();
void ShutdownAndroidSystem();
#endif

// The URL must include the protocol.
void LaunchWebBrowser(const std::string_view url);
void LaunchFileExplorer(const std::filesystem::path& path);

enum class SimpleMessageBoxType {
  Help,
  Warning,
  Error,
};

// This is expected to block the caller until the message box is closed. Suppressed (below), it
// shows nothing and returns at once; every caller logs the message itself.
void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message);

// For a process nobody sits at (a dedicated server): ShowSimpleMessageBox shows no box, so a
// fatal path logs and exits instead of blocking on a modal box. Any thread; set it early.
void SetSimpleMessageBoxesSuppressed(bool suppressed);
bool SimpleMessageBoxesSuppressed();

}  // namespace rex
