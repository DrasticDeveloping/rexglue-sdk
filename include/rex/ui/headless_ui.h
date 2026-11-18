// The hidden window's UI switch (render_hidden_ui, ui/window.cpp).
//
// A game started with window_hidden (a dedicated server) still built a presenter, a swap chain on
// its never-shown window, an immediate drawer, the ImGui drawer with every dialog and overlay, and
// painted that UI on the UI thread for nobody. With render_hidden_ui=false a hidden window gets
// none of them: ReXApp::SetupPresentation creates the window (input, the clipboard owner and
// WM_CLOSE still need it) but no presenter surface, no immediate or ImGui drawer and no dialogs,
// and Runtime::Setup sets the graphics system up without presentation (its headless provider; the
// command processor is the same). What tolerates their absence:
// - the guest's XAM dialogs take the headless path (no ImGui drawer: xam_ui.cpp), so a dialog
//   never waits for a paint that never comes; the app may also set `headless` for the same;
// - a swap without a presenter closes its frame like the draw skip does (d3d12/command_processor
//   IssueSwap) instead of failing every frame;
// - the app's own drawers are null (OnCreateDialogs is not called): its dialogs are never made.
// A visible window always gets its UI: the switch acts on a hidden window only.
//
// Decided once, before the window opens, after the app's OnPreSetup (which may set window_hidden
// and render_hidden_ui): both settings are restart-only. Pure, so a unit test covers it.

#pragma once

namespace rex::ui::headless_ui {

// True: no presenter, swap chain, drawers or UI for this process.
inline bool Decide(bool render_hidden_ui, bool window_hidden) {
  return !render_hidden_ui && window_hidden;
}

}  // namespace rex::ui::headless_ui
