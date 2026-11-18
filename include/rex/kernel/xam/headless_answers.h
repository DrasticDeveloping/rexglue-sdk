// The guest's XAM dialogs answered with no UI (xam_ui.cpp): the `headless` setting, or a process
// with no ImGui drawer (a hidden window with render_hidden_ui off, rex/ui/headless_ui.h). Each such
// answer is logged at warning level (`XAM: headless answer: ...`: the dialog, its title, text,
// buttons or default text, and the answer given), counted here and kept as the last one, so a
// process nobody looks at can say afterwards which dialogs it answered for the game. Not counted:
// the device selector, which is answered at once with or without a UI.

#pragma once

#include <cstdint>
#include <string>

namespace rex::kernel::xam {

// Answers given with no UI so far (any thread).
uint32_t HeadlessAnswerCount();

// The last one, as logged ("" before the first).
std::string LastHeadlessAnswer();

}  // namespace rex::kernel::xam
