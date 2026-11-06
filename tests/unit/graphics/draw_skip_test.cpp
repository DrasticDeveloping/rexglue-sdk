#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/draw_skip.h>

using namespace rex::graphics::draw_skip;

TEST_CASE("draw skip acts only for a hidden window with render_hidden_draw off", "[draw_skip]") {
  // render_hidden_draw on (the default) draws, hidden or not.
  REQUIRE(Decide(true, false, false) == Mode::kDraw);
  REQUIRE(Decide(true, true, false) == Mode::kDraw);
  REQUIRE(Decide(true, true, true) == Mode::kDraw);
  // A visible window always draws, whatever the setting says.
  REQUIRE(Decide(false, false, false) == Mode::kDraw);
  REQUIRE(Decide(false, false, true) == Mode::kDraw);
  // Hidden with the setting off: the skip.
  REQUIRE(Decide(false, true, false) == Mode::kSkip);
}

TEST_CASE("draw skip is refused when resolve readback is on", "[draw_skip]") {
  REQUIRE(Decide(false, true, true) == Mode::kRefusedReadback);
}

TEST_CASE("memexport draws are never skipped", "[draw_skip]") {
  REQUIRE(SkipDraw(true, false));
  REQUIRE_FALSE(SkipDraw(true, true));
  REQUIRE_FALSE(SkipDraw(false, false));
  REQUIRE_FALSE(SkipDraw(false, true));
}

TEST_CASE("counter line names every counter", "[draw_skip]") {
  Counters c;
  c.draws = 8123;
  c.memexport = 2;
  c.copies = 1210;
  c.swaps = 600;
  c.zpd = 4;
  c.viz = 6;
  c.readbacks = 0;
  c.skipped_draws = 8000;
  c.skipped_copies = 1210;
  REQUIRE(FormatCounters(c, 10.04) ==
          "GPU: draws 8123 memexport 2 copies 1210 swaps 600 zpd 4 viz 6 readbacks 0 skipped "
          "8000/1210 in 10.0 s");
  REQUIRE(FormatCounters(Counters{}, 12.5) ==
          "GPU: draws 0 memexport 0 copies 0 swaps 0 zpd 0 viz 0 readbacks 0 skipped 0/0 in 12.5 s");
}
