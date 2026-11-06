// The hidden game's draw skip and the GPU work counters (render_hidden_draw, gpu_draw_counters).
//
// A game started with window_hidden (a dedicated server, an export run) still did all of its GPU
// work for a picture nobody sees. With render_hidden_draw=false the D3D12 command processor skips
// the host GPU work that cannot reach guest memory: draws without memexport, the resolves'
// copies and clears, and the swap's present. Packet, register, fence and interrupt processing
// stay as they are, and so does the guest frame count (CommandProcessor::guest_frame_count(),
// counted in ExecutePacketType3_XE_SWAP after IssueSwap returns). What a skipped draw or resolve
// changes that the guest could see, and what keeps it correct, is written where each one is
// skipped (d3d12/command_processor.cpp).
//
// One path the skip cannot cover: with memexport readback on (readback_memexport, the default),
// a memexport draw (always drawn) copies its export ranges from the GPU's copy of guest memory
// back into guest memory. If such a range overlaps a resolve destination, the full path carries
// the resolve's bytes into guest memory that way, and under the skip (no resolve) the CPU's older
// bytes come back instead. So the skip is trusted only while gpu_draw_counters shows memexport 0.
//
// The decisions are pure functions here so the unit tests cover them without a GPU.

#pragma once

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rex::graphics::draw_skip {

enum class Mode {
  kDraw,  // draw as before (render_hidden_draw on, the default, or a visible window)
  kSkip,  // hidden window with render_hidden_draw off: skip what cannot reach guest memory
  // render_hidden_draw off, but resolve readback (readback_resolve / d3d12_readback_resolve) is
  // on at startup: the settings say this game reads render-to-texture results on the CPU, and a
  // skipped draw leaves nothing true to read back, so the skip is refused and everything is drawn.
  kRefusedReadback,
};

// Decided once, when the command processor sets up its context: render_hidden_draw and
// window_hidden are restart-only, and a host occlusion query or a frame must not be cut in half
// by a change. Resolve readback can be turned on later (it is hot-reloadable); a skip that is
// already on stays on, and IssueCopy warns once that the readback is ignored until a restart.
inline Mode Decide(bool render_hidden_draw, bool window_hidden, bool resolve_readback_on) {
  if (render_hidden_draw || !window_hidden) {
    return Mode::kDraw;
  }
  return resolve_readback_on ? Mode::kRefusedReadback : Mode::kSkip;
}

// One draw under the skip. A draw whose vertex or pixel shader exports to memory (memexport)
// writes guest memory the game may read (directly with readback_memexport, the default, or
// through later GPU work), so it is always drawn; any other draw only writes render targets.
inline bool SkipDraw(bool skipping, bool memexport_used) {
  return skipping && !memexport_used;
}

// Counted on the command processor thread (readbacks: on the calling guest thread, kept
// separately as an atomic by the command processor and copied in here for the line).
struct Counters {
  uint64_t draws = 0;           // IssueDraw calls that are draws (not EDRAM copies)
  uint64_t memexport = 0;       // ... of which the vertex or pixel shader uses memexport
  uint64_t copies = 0;          // IssueCopy calls (resolves)
  uint64_t swaps = 0;           // XE_SWAP packets (IssueSwap calls)
  uint64_t zpd = 0;             // EVENT_WRITE_ZPD packets (occlusion query begin and end)
  uint64_t viz = 0;             // VIZ_QUERY packets (visibility query begin and end)
  uint64_t readbacks = 0;       // ReadBackResolvedMemory calls
  uint64_t skipped_draws = 0;   // draws whose host GPU work the skip left out
  uint64_t skipped_copies = 0;  // resolves whose host GPU work the skip left out
};

// The gpu_draw_counters line, one per window (about 10 s), in the gpu log:
//   GPU: draws 8123 memexport 0 copies 1210 swaps 600 zpd 0 viz 0 readbacks 0 skipped 0/0 in 10.0 s
// "skipped" is draws/copies; both are 0 unless the skip is on.
inline std::string FormatCounters(const Counters& c, double seconds) {
  return fmt::format(
      "GPU: draws {} memexport {} copies {} swaps {} zpd {} viz {} readbacks {} skipped {}/{} "
      "in {:.1f} s",
      c.draws, c.memexport, c.copies, c.swaps, c.zpd, c.viz, c.readbacks, c.skipped_draws,
      c.skipped_copies, seconds);
}

// The window the counters cover.
constexpr double kCounterWindowSeconds = 10.0;

}  // namespace rex::graphics::draw_skip
