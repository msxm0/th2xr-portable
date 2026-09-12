// Checks on the display layer's arithmetic - the part that keeps being
// wrong.  Everything here is against my_inc2/DISP.CPP rather than against
// what the numbers ought to look like.

#include "dsp.hpp"

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* what)
{
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

void check_near(float got, float want, const char* what, float slack = 1.5f)
{
    if (std::fabs(got - want) > slack) {
        std::printf("FAIL: %s (got %.2f want %.2f)\n", what, got, want);
        ++failures;
    }
}

// A plain 800x600 graph over a whole bitmap, as DSP_SetGraph leaves one.
th2::Graph whole_screen()
{
    th2::Graph graph;
    graph.flag = true;
    graph.disp = true;
    graph.dw = graph.sw = th2::display_width;
    graph.dh = graph.sh = th2::display_height;
    return graph;
}

void test_plain()
{
    const auto geometry = resolve_geometry(whole_screen(), 0, 0, 0, 0);
    check(geometry.poly == th2::Poly::rect, "plain graph stays a rect");
    check_near(geometry.destination.w, 800.0f, "plain width");
    check_near(geometry.destination.h, 600.0f, "plain height");
    check(geometry.red == 255 && geometry.green == 255
              && geometry.blue == 255,
          "neutral brightness is an unchanged modulation");
    check(!geometry.clipped, "no clip rect by default");
}

// DSP_SetGraphGlobalPos shifts every destination, and the clip with it.
void test_global_offset()
{
    auto graph = whole_screen();
    graph.clip = SDL_Rect{10, 20, 100, 200};
    const auto geometry = resolve_geometry(graph, 7, -5, 0, 0);
    check_near(geometry.destination.x, 7.0f, "global x reaches destination");
    check_near(geometry.destination.y, -5.0f, "global y reaches destination");
    check(geometry.clipped, "clip rect is used");
    check(geometry.clip.x == 17 && geometry.clip.y == 15,
          "clip travels with the global offset");
}

// int sx = gs->sx - BmpSet[gs->bno].pos.x - the bitmap's own origin comes
// off every source coordinate.
void test_source_origin()
{
    auto graph = whole_screen();
    graph.sx = 30;
    graph.sy = 40;
    const auto geometry = resolve_geometry(graph, 0, 0, 10, 15, 128, 128, 128);
    check_near(geometry.source.x, 20.0f, "source x loses the bitmap origin");
    check_near(geometry.source.y, 25.0f, "source y loses the bitmap origin");
}

// if(gs->zoom){ dx = cx + ((gs->dx - cx)*(zoom+256))>>8; ... }
// A zoom of 256 is exactly double, about (cx, cy), and the global offset is
// added after the multiply rather than before it.
void test_zoom_about_centre()
{
    auto graph = whole_screen();
    graph.zoom = 256;
    graph.cx = 400;
    graph.cy = 300;
    const auto geometry = resolve_geometry(graph, 0, 0, 0, 0);
    // (0 - 400) * 512 >> 8 = -800, so 400 - 800 = -400.
    check_near(geometry.destination.x, -400.0f, "zoom x about the centre");
    check_near(geometry.destination.y, -300.0f, "zoom y about the centre");
    check_near(geometry.destination.w, 1600.0f, "zoom doubles the width");
    check_near(geometry.destination.h, 1200.0f, "zoom doubles the height");

    // Zero zoom leaves the rectangle alone.
    graph.zoom = 0;
    const auto plain = resolve_geometry(graph, 0, 0, 0, 0);
    check_near(plain.destination.x, 0.0f, "no zoom leaves x");
    check_near(plain.destination.w, 800.0f, "no zoom leaves the width");
}

void test_zoom_adds_global_after_scaling()
{
    auto graph = whole_screen();
    graph.zoom = 256;
    graph.cx = 0;
    graph.cy = 0;
    const auto geometry = resolve_geometry(graph, 11, 13, 0, 0);
    // dx is 0, so the scale leaves it at 0 and the global offset lands
    // whole - not doubled, which is what putting it in first would do.
    check_near(geometry.destination.x, 11.0f, "global offset is not scaled");
    check_near(geometry.destination.y, 13.0f, "global offset is not scaled");
}

// DSP_SetGraphBright below 128 darkens; 128 is unchanged.  The rasteriser
// treats 128 as neutral, so it has to come out as a modulation of 255.
void test_brightness()
{
    auto graph = whole_screen();
    graph.r = graph.g = graph.b = 64;
    const auto half = resolve_geometry(graph, 0, 0, 0, 0);
    check_near(half.red, 127.0f, "half brightness is half a modulation", 2.0f);

    // The global brightness multiplies in unless brt_flag is set.
    graph.r = graph.g = graph.b = 128;
    const auto dimmed = resolve_geometry(graph, 0, 0, 0, 0, 64, 64, 64);
    check_near(dimmed.red, 127.0f, "global brightness applies", 2.0f);
    graph.brt_flag = true;
    const auto exempt = resolve_geometry(graph, 0, 0, 0, 0, 64, 64, 64);
    check_near(exempt.red, 255.0f, "brt_flag ignores the global", 2.0f);
}

// REV_W 0x10 / REV_H 0x20.
void test_mirroring()
{
    auto graph = whole_screen();
    graph.rparam = 0x10;
    const auto flipped = resolve_geometry(graph, 0, 0, 0, 0);
    check(flipped.flip_x && !flipped.flip_y, "REV_W mirrors horizontally");
    graph.rparam = 0x20;
    const auto other = resolve_geometry(graph, 0, 0, 0, 0);
    check(!other.flip_x && other.flip_y, "REV_H mirrors vertically");
}

// The DRW_ selector packs the mode low and its parameter high.
void test_blend_selector()
{
    const std::uint32_t bld = th2::drw_bld | (128u << 16);
    check(th2::draw_mode_of(bld) == th2::drw_bld, "DRW_BLD mode decodes");
    check(th2::draw_param_of(bld) == 128, "DRW_BLD alpha decodes");
    check(th2::draw_alpha_of(bld) == 128, "DRW_BLD carries its alpha");
    check(th2::draw_alpha_of(th2::drw_nml) == 256,
          "a mode without an alpha draws solid");
    check(th2::blend_of(th2::drw_add) == SDL_BLENDMODE_ADD,
          "DRW_ADD is an additive blend");
}

// DSP_SetGraphRoll builds the quad by hand out of SinTbl.  The engine's COS
// is the sine - COS(0) is zero, SIN(0) is one - so rate 0 is the *upright*
// rectangle, not rate 64.  Expectations here are worked through from the
// formula in DISP.CPP rather than from what a rotation "should" look like,
// which is how the quarter-turn error got in last time.
void test_roll_corners()
{
    // The geometry setters touch no renderer, so a null one is enough.
    th2::Display display(nullptr);

    // rate 0: COS=0, SIN=4096, sw2=400, sh2=300 about (400, 300).
    //   x1 = 400 + (0*300 - 4096*400)/4096 = 0
    //   y1 = 300 + (-0*400 - 4096*300)/4096 = 0
    //   x2 = 400 + (0*300 + 4096*400)/4096 - 1 = 799
    //   y4 = 300 + (0*400 + 4096*300)/4096 - 1 = 599
    display.set_graph_roll(0, 400, 300, 0, 0, 0, 0, 800, 600);
    const th2::Graph& upright = display.graph(0);
    check(upright.poly == th2::Poly::poly4, "a roll makes a quad");
    check(upright.dx == 0 && upright.dy == 0, "rate 0 corner 1 is the origin");
    check(upright.dx2 == 799 && upright.dy2 == 0, "rate 0 corner 2");
    check(upright.dx3 == 0 && upright.dy3 == 599, "rate 0 corner 3");
    check(upright.dx4 == 799 && upright.dy4 == 599, "rate 0 corner 4");

    // rate 64: COS=4096, SIN=0 - a quarter turn, so the quad is 600 across
    // and 800 down and corner 1 leaves the screen.
    display.set_graph_roll(0, 400, 300, 0, 64, 0, 0, 800, 600);
    const th2::Graph& quarter = display.graph(0);
    check(quarter.dx == 700 && quarter.dy == -100,
          "rate 64 is a quarter turn, not the identity");

    // rate 128: COS=0, SIN=-4096 - half a turn, corners swapped end for end.
    display.set_graph_roll(0, 400, 300, 0, 128, 0, 0, 800, 600);
    const th2::Graph& half = display.graph(0);
    check(half.dx == 800 && half.dy == 600, "half a turn swaps corner 1");
    check(half.dx4 == -1 && half.dy4 == -1, "half a turn swaps corner 4");

    // A zoom widens the quad: +256 is double, so the half-spans double too.
    display.set_graph_roll(0, 400, 300, 256, 0, 0, 0, 800, 600);
    const th2::Graph& zoomed = display.graph(0);
    check(zoomed.dx == -400 && zoomed.dx2 == 1199,
          "a roll zoom of +256 doubles the span");

    // The source quad is clamped to the bitmap; with no bitmap bound that
    // is a zero extent, which is what DSP_SetGraphPosPoly's LIM does.
    check(zoomed.sx == 0 && zoomed.sx2 == 0,
          "source corners clamp to the bitmap size");
}

// DSP_SetGraphPosZoom drops back to a plain blit when the scale is one, and
// DSP_SetGraphZoom2(0) turns the zoom off entirely.
void test_zoom_setters_degrade()
{
    th2::Display display(nullptr);
    display.set_graph_pos_zoom(0, 0, 0, 800, 600, 0, 0, 800, 600);
    check(display.graph(0).poly == th2::Poly::rect,
          "a one-to-one zoom is a plain blit");
    display.set_graph_pos_zoom(0, 0, 0, 400, 300, 0, 0, 800, 600);
    check(display.graph(0).poly == th2::Poly::zoom,
          "a real scale stays a zoom");
    display.set_graph_zoom2(0, 400, 300, 0);
    check(display.graph(0).poly == th2::Poly::rect
              && display.graph(0).zoom == 0,
          "zoom2 of nothing turns it off");
    display.set_graph_zoom3(0, 400, 300, 200);
    check(display.graph(0).zoom == 256,
          "zoom3 is a percentage: 200% is +256");
}

// DSP_SetGraphClip( gno, dx<0, ... ) clears the clip rather than setting a
// negative one.
void test_clip_clears_on_negative()
{
    th2::Display display(nullptr);
    display.set_graph_clip(0, 10, 10, 100, 100);
    check(display.graph(0).clip.has_value(), "clip is set");
    display.set_graph_clip(0, -1, 0, 0, 0);
    check(!display.graph(0).clip.has_value(), "a negative x clears the clip");
}

}  // namespace

int main()
{
    test_plain();
    test_global_offset();
    test_source_origin();
    test_zoom_about_centre();
    test_zoom_adds_global_after_scaling();
    test_brightness();
    test_mirroring();
    test_blend_selector();
    test_roll_corners();
    test_zoom_setters_degrade();
    test_clip_clears_on_negative();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("display geometry ok\n");
    return 0;
}
