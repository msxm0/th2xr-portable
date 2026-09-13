#include "avg_back.hpp"

#include <algorithm>
#include <cstdlib>

namespace th2 {
namespace {

constexpr int DISP_X = 800;
constexpr int DISP_Y = 600;

}  // namespace

void AvgBack::init()
{
    back_ = {};
}

// ---------------------------------------------------------------- chain ---



void AvgBack::control_back()
{
    // void AVG_ControlBack(void), minus the branches nothing reaches:
    //
    //     AVG_ControlSpChar();
    //     AVG_ControlNoise();
    //     AVG_ControlBackFade();
    //     if(ChipBackStruct.flag){ ...chip... }else{
    //         AVG_ControlBackChange();
    //         AVG_ControlBackScroll();
    //         AVG_ControlBackCScope();
    //     }
    //     AVG_ControlShake();
    //     AVG_ControlRipple();
    //
    // ControlNoise, ControlBackCScope and ControlRipple have opcodes but no
    // uses - measured zero across all 123,413 retail instructions - and the
    // chip branch needs a Chip* opcode, of which there are none.
    control_back_fade();
    control_back_change();
    control_back_scroll();
    control_shake();
    // AVG_ColtrolFade is called from AVG_System's tail rather than from
    // AVG_ControlBack, but it is one more counter in the same pass.
    control_fade();
}

// --------------------------------------------------- AVG_ControlBackChange -

void AvgBack::control_back_change()
{
    int rate = 0;
    int back_max = eff_cnt(back_.fd_max);
    const int sp = level() ? 1 : 0;
    int x = 0;
    int y = 0;

    if (back_.fd_type == bak_fade) {
        if (back_.r != bright_neutral || back_.g != bright_neutral
            || back_.b != bright_neutral) {
            back_max *= 2;
        }
    }
    if (back_.fd_type == bak_lasterin && back_.fd_max == -2 && sp) {
        back_max *= 2;
    }

    if (!back_.fd_flag) {
        return;
    }
    back_.fd_cnt++;

    if (back_.fd_cnt >= back_max) {
        back_.fd_flag = 0;

        for (int i = 1; i < 4; ++i) {
            display_.reset_graph(grp_back + i);
        }
        display_.release_bmp(bmp_back + 1);

        display_.reset_graph_bset(grp_back);
        display_.set_graph(grp_back, bmp_back, lay_back, true, check_none);
        set_back_pos(back_.x, back_.y);

        if (back_.fd_type == bak_fade) {
            set_back_fade_direct(bright_neutral, bright_neutral, bright_neutral);
        }
        display_.set_graph_bright(grp_back, back_.r, back_.g, back_.b);
        return;
    }

    rate = 256 * back_.fd_cnt / back_max;
    switch (back_.fd_type) {
    case bak_fade:
        if (back_.r != bright_neutral || back_.g != bright_neutral
            || back_.b != bright_neutral) {
            if (rate < 128) {
                display_.set_graph_bright(
                    grp_back + 1, 128 - rate, 128 - rate, 128 - rate);
            } else {
                display_.set_graph_disp(grp_back + 1, false);
                display_.set_graph_disp(grp_back, true);
                display_.set_graph_bright(
                    grp_back, rate - 128, rate - 128, rate - 128);
            }
        } else {
            display_.set_graph_bright(grp_back, rate / 2, rate / 2, rate / 2);
        }
        break;
    case bak_cfade:
        if (sp) display_.set_graph_param(grp_back, DRW_BLD(rate));
        else    display_.set_graph_param(grp_back, DRW_AMI(rate));
        break;
    case bak_cfade_up:
        if (sp) display_.set_graph_param(grp_back, DRW_LCF(dir_up, rate));
        else    display_.set_graph_param(grp_back, DRW_LPP(dir_up, rate));
        break;
    case bak_cfade_do:
        if (sp) display_.set_graph_param(grp_back, DRW_LCF(dir_do, rate));
        else    display_.set_graph_param(grp_back, DRW_LPP(dir_do, rate));
        break;
    case bak_cfade_ri:
        if (sp) display_.set_graph_param(grp_back, DRW_LCF(dir_ri, rate));
        else    display_.set_graph_param(grp_back, DRW_LPP(dir_ri, rate));
        break;
    case bak_cfade_le:
        if (sp) display_.set_graph_param(grp_back, DRW_LCF(dir_le, rate));
        else    display_.set_graph_param(grp_back, DRW_LPP(dir_le, rate));
        break;
    case bak_cfade_ce:
        if (sp) display_.set_graph_param(grp_back, DRW_LCF(dir_ce, rate));
        else    display_.set_graph_param(grp_back, DRW_LPP(dir_ce, rate));
        break;
    case bak_cfade_ou:
        if (sp) display_.set_graph_param(grp_back, DRW_LCF(dir_ou, rate));
        else    display_.set_graph_param(grp_back, DRW_LPP(dir_ou, rate));
        break;
    case bak_dia1: case bak_dia2: case bak_dia3:
        if (sp) display_.set_graph_param(
                    grp_back, DRW_DIO(back_.fd_type - bak_dia1, rate));
        else    display_.set_graph_param(
                    grp_back, DRW_DIA(back_.fd_type - bak_dia1, rate));
        break;
    case bak_cfzoom1:
        rate = 256 * back_.fd_cnt / back_max;
        rate = 256 - rate * rate / 256;
        if (sp) display_.set_graph_param(grp_back, DRW_BLD(128 - rate / 2));
        else    display_.set_graph_param(grp_back, DRW_AMI(256 - rate));
        rate = 256 - 256 * back_.fd_cnt / back_max;
        rate = rate * rate / 256;
        display_.set_graph_zoom2(grp_back, DISP_X / 2, DISP_Y / 2, rate);
        break;
    case bak_cfzoom2:
        rate = 256 - 256 * back_.fd_cnt / back_max;
        rate = rate * rate / 256;
        rate = 256 - rate;
        if (sp) display_.set_graph_param(grp_back, DRW_BLD(rate));
        else    display_.set_graph_param(grp_back, DRW_AMI(rate));
        display_.set_graph_zoom2(grp_back + 1, DISP_X / 2, DISP_Y / 2, rate * 2);
        break;
    case bak_cfzoom3:
        rate = 256 - 256 * back_.fd_cnt / back_max;
        rate = rate * rate / 256;
        if (sp) display_.set_graph_param(grp_back + 1, DRW_BLD(rate));
        display_.set_graph_zoom2(
            grp_back + 1, DISP_X / 2, DISP_Y / 2, rate / 2 - 128);
        break;
    case bak_cfzoom4:
        rate = 256 - 256 * back_.fd_cnt / back_max;
        if (sp) display_.set_graph_param(grp_back, DRW_BLD(32 - rate / 8));
        rate = rate * rate / 256;
        display_.set_graph_zoom2(grp_back, DISP_X / 2, DISP_Y / 2, -rate / 2);
        break;
    case bak_kami:
        // The "paper tearing" wipe: the outgoing picture splits and the two
        // halves slide apart over the new one.
        x = DISP_X * rate / 256;
        display_.set_graph(grp_back, bmp_back, lay_back, true, check_none);
        if (x < 320) {
            display_.set_graph_pos(grp_back, 0, 0, 0, 0, x, DISP_Y);
            display_.set_graph(grp_back, bmp_back, lay_back, true, check_none);
            display_.set_graph_pos(
                grp_back + 4, DISP_X - x, 0, DISP_X - x, 0, x, DISP_Y);
        } else {
            display_.reset_graph(grp_back + 4);
            display_.set_graph_pos(grp_back, 0, 0, 0, 0, DISP_X, DISP_Y);
            display_.set_graph(grp_back, bmp_back, lay_back, true, check_none);
        }
        display_.set_graph(grp_back + 1, bmp_back + 1, lay_back + 2, true, check_none);
        display_.set_graph(grp_back + 2, bmp_back + 1, lay_back + 2, true, check_none);
        display_.set_graph(grp_back + 3, bmp_back + 1, lay_back + 1, true, check_none);
        display_.set_graph_pos(
            grp_back + 1, -x, 0, 0, 0, std::min(DISP_X, x * 2), DISP_Y);
        display_.set_graph_pos(grp_back + 2, x, 0, 0, 0, DISP_X, DISP_Y);
        display_.set_graph_pos(
            grp_back + 3, x, 0, std::min(DISP_X, x * 2), 0,
            std::max(0, DISP_X - x * 2), DISP_Y);
        if (sp) {
            display_.set_graph_param(grp_back + 1, DRW_BLD(128));
            display_.set_graph_param(grp_back + 2, DRW_BLD(128));
        } else {
            display_.set_graph_param(grp_back + 1, DRW_AMI(128));
            display_.set_graph_param(grp_back + 2, DRW_AMI(128));
        }
        break;
    case bak_slide_up: case bak_slide_do:
    case bak_slide_ri: case bak_slide_le:
        if (sp) {
            display_.set_graph_param(grp_back + 0, DRW_BLD(rate));
            display_.set_graph_param(grp_back + 1, DRW_BLD(rate));
        }
        rate = 256 - rate;
        y = DISP_Y - DISP_Y * rate * rate / (256 * 256);
        x = DISP_X - DISP_X * rate * rate / (256 * 256);
        switch (back_.fd_type) {
        case bak_slide_up:
            display_.set_graph_move(grp_back + 0, 0, y - DISP_Y);
            display_.set_graph_move(grp_back + 1, 0, y);
            break;
        case bak_slide_do:
            display_.set_graph_move(grp_back + 0, 0, -y + DISP_Y);
            display_.set_graph_move(grp_back + 1, 0, -y);
            break;
        case bak_slide_ri:
            display_.set_graph_move(grp_back + 0, -x + DISP_X, 0);
            display_.set_graph_move(grp_back + 1, -x, 0);
            break;
        case bak_slide_le:
            display_.set_graph_move(grp_back + 0, x - DISP_X, 0);
            display_.set_graph_move(grp_back + 1, x, 0);
            break;
        default:
            break;
        }
        break;
    case bak_lasterin:
        if (sp) {
            display_.set_graph_param(
                grp_back + 1, DRW_LST(std::min(255, rate), global_count() * 2));
            display_.set_graph_param(grp_back, DRW_BLD(rate));
        } else {
            display_.set_graph_param(grp_back, DRW_NIS(rate));
        }
        break;
    case bak_noise:
        display_.set_graph_param(grp_back, DRW_NIS(rate));
        break;
    case bak_piple:
        display_.set_graph_param(grp_back + 1, DRW_RPL(rate));
        display_.set_graph_param(grp_back, DRW_BLD(rate));
        break;
    case bak_tv:
        // The television switch-off: the picture collapses to a band and
        // then to a point, with an additive flash over it.
        rate = 150 * back_.fd_cnt / back_max;
        rate = 150 - rate;
        rate = rate * rate * rate / (150 * 150);
        rate = 150 - rate;
        if (rate < back_max * 120) {
            display_.set_graph_zoom(
                grp_back + 1, 0, (DISP_Y / 2) * rate / 120,
                DISP_X, DISP_Y - (DISP_Y / 2) * rate / 120 * 2);
        } else {
            display_.set_graph_disp(grp_back + 1, false);
        }
        {
            int rate2 = 150 * back_.fd_cnt / back_max;
            rate2 = std::max(0, rate2 - 110);
            display_.set_graph_zoom(
                grp_back + 2, rate2 * 10, (DISP_Y / 2) * rate / 150,
                DISP_X - rate2 * 20, DISP_Y - 299 * rate / 150 * 2);
            display_.set_graph_fade(
                grp_back + 2, 128 * back_.fd_cnt / back_max);
            display_.set_graph_param(grp_back + 2, DRW_ADD);
        }
        display_.set_graph_prim(
            grp_back, GraphType::flat, Poly::rect, lay_back, true);
        display_.set_graph_fade(grp_back, 0);
        display_.set_graph_pos_rect(grp_back, 0, 0, DISP_X, DISP_Y);
        break;
    case bak_roll:
        rate = 128 * back_.fd_cnt / back_max;
        display_.set_graph_roll(
            grp_back + 1, DISP_X / 2, DISP_Y / 2, rate, rate,
            0, 0, DISP_X, DISP_Y);
        display_.set_graph_param(grp_back + 1, DRW_BLD(32));
        display_.set_graph_fade(grp_back + 1, 128 - rate);
        display_.set_graph_disp(grp_back, false);
        break;
    default:
        // The pattern wipes: BMP_BACK+2 holds the mask and fd_vague is how
        // soft its edge is.
        display_.set_graph_bset(grp_back, bmp_back + 2, rate, back_.fd_vague);
        break;
    }
}

// ----------------------------------------------------- AVG_ControlBackFade -

void AvgBack::control_back_fade()
{
    const int back_max = eff_cnt(back_.br_fade);

    if (!back_.br_flag) {
        return;
    }
    back_.br_cnt++;

    // br_pat is -1 for a plain colour fade, which is the only kind the
    // retail scripts use; the pattern branch needs BMP_WORK+10 and a
    // GRP_WORK+20 mask, and AVG_SetBackFadePatern is never called.
    if (back_.br_cnt >= back_max) {
        back_.rr = back_.r = back_.er;
        back_.gg = back_.g = back_.eg;
        back_.bb = back_.b = back_.eb;
        back_.br_flag = 0;
    } else {
        const int cnt = back_.br_cnt;
        back_.rr = (back_.er * cnt + back_.r * (back_max - cnt)) / back_max;
        back_.gg = (back_.eg * cnt + back_.g * (back_max - cnt)) / back_max;
        back_.bb = (back_.eb * cnt + back_.b * (back_max - cnt)) / back_max;
    }
    display_.set_graph_bright(grp_back, back_.rr, back_.gg, back_.bb);
    // for(i=GRP_SCRIPT;i<GRP_ENDING;i++) DSP_SetGraphBright( i, ... ) - the
    // overlays darken with the background, which is what a fade to black
    // has to do to take them with it.
    for (int i = grp_script; i < grp_ending; ++i) {
        display_.set_graph_bright(i, back_.rr, back_.gg, back_.bb);
    }
    if (hooks_.set_char_bright) {
        hooks_.set_char_bright(back_.rr, back_.gg, back_.bb);
    }
}

// --------------------------------------------------- AVG_ControlBackScroll -

void AvgBack::control_back_scroll()
{
    int x, y, x2, y2, x3, y3, x4, y4;
    int w, h;
    int cnt, max;
    const int back_max = eff_cnt4(back_.sc_max);

    if (!back_.sc_flag) {
        return;
    }
    back_.sc_cnt++;
    if (back_.sc_cnt >= back_max) {
        back_.sc_flag = 0;
        // AVG_CopyBack(ON); AVG_SetBackChar( BackStruct.x, BackStruct.y, ON );
        // The characters were baked into the plate at the window's old
        // offset, so they have to be put back where they belong.
        copy_back(true);
        if (hooks_.set_back_char) {
            hooks_.set_back_char(back_.x, back_.y, true);
        }
        return;
    }

    cnt = back_.sc_cnt;
    max = back_max;
    switch (back_.sc_type) {
    case 0: case 3:
        break;
    case 1: case 4:      // ease in
        cnt = cnt * cnt;
        max = max * max;
        break;
    case 2: case 5:      // ease out
        cnt = max - cnt;
        cnt = cnt * cnt;
        max = max * max;
        cnt = max - cnt;
        break;
    default:
        break;
    }

    switch (back_.sc_type / 3) {
    case 0:
        x = (back_.sx * cnt + back_.x * (max - cnt)) / max;
        y = (back_.sy * cnt + back_.y * (max - cnt)) / max;
        w = (back_.sw * cnt + back_.w * (max - cnt)) / max;
        h = (back_.sh * cnt + back_.h * (max - cnt)) / max;
        static_cast<void>(w);
        static_cast<void>(h);
        display_.set_graph_pos(grp_back, 0, 0, x, y, DISP_X, DISP_Y);
        break;
    case 1: {
        int sx = 0;
        int sy = 0;
        display_.get_graph_bmp_size(grp_back, &sx, &sy);
        if (back_.sw == 0 || back_.sh == 0 || back_.w == 0 || back_.h == 0) {
            break;
        }
        x = -(back_.sx * DISP_X / back_.sw * cnt
              + back_.x * DISP_X / back_.w * (max - cnt)) / max;
        y = -(back_.sy * DISP_Y / back_.sh * cnt
              + back_.y * DISP_Y / back_.h * (max - cnt)) / max;
        w = (sx * DISP_X / back_.sw * cnt
             + sx * DISP_X / back_.w * (max - cnt)) / max;
        h = (sy * DISP_Y / back_.sh * cnt
             + sy * DISP_Y / back_.h * (max - cnt)) / max;
        display_.set_graph_pos_zoom(grp_back, x, y, w, h, 0, 0, sx, sy);
        break;
    }
    case 2:
        x  = (cnt * back_.sx  + (max - cnt) * back_.x)  / max;
        y  = (cnt * back_.sy  + (max - cnt) * back_.y)  / max;
        x2 = (cnt * back_.sx2 + (max - cnt) * back_.x2) / max;
        y2 = (cnt * back_.sy2 + (max - cnt) * back_.y2) / max;
        x3 = (cnt * back_.sx3 + (max - cnt) * back_.x3) / max;
        y3 = (cnt * back_.sy3 + (max - cnt) * back_.y3) / max;
        x4 = (cnt * back_.sx4 + (max - cnt) * back_.x4) / max;
        y4 = (cnt * back_.sy4 + (max - cnt) * back_.y4) / max;
        display_.set_graph_pos_poly(
            grp_back, 0, 0, DISP_X, 0, 0, DISP_Y, DISP_X, DISP_Y,
            x, y, x2, y2, x3, y3, x4, y4);
        break;
    default:
        break;
    }
}

// -------------------------------------------------------- AVG_ControlShake -

void AvgBack::control_shake()
{
    int x = 0;
    int y = 0;
    int cnt = 0;
    int cnt2 = 0;
    int w = 0;
    int h = 0;
    const int back_max = eff_cnt4(back_.sk_speed);
    const int pich = back_.sk_pich;

    if (!back_.sk_flag) {
        return;
    }
    back_.sk_cnt++;
    if (back_.sk_cnt >= back_max && back_.sk_speed != 0) {
        stop_shake();
        display_.set_graph_bmp(grp_back, bmp_back);
        display_.set_graph_bmp(grp_back + 1, bmp_backhalf);
        return;
    }

    switch (back_.sk_type) {
    case shake_sin:
    case shake_all_sin:
    case shake_txt_sin:
    case shake_sin_set:
    case shake_all_sin_set:
        cnt2 = back_.sk_cnt * back_.sk_swing / 8;
        if (back_max) {
            switch (back_.sk_dir) {
            case DIR_L: case DIR_DL: case DIR_UL: x = -COS(cnt2) * pich / 512 / 8; break;
            case DIR_U: case DIR_D:               x = 0; break;
            case DIR_R: case DIR_DR: case DIR_UR: x =  COS(cnt2) * pich / 512 / 8; break;
            default: break;
            }
            switch (back_.sk_dir) {
            case DIR_D: case DIR_DL: case DIR_DR: y =  COS(cnt2) * pich / 512 / 8; break;
            case DIR_L: case DIR_R:               y = 0; break;
            case DIR_U: case DIR_UL: case DIR_UR: y = -COS(cnt2) * pich / 512 / 8; break;
            default: break;
            }
            x = x * (back_max - back_.sk_cnt) / back_max;
            y = y * (back_max - back_.sk_cnt) / back_max;
        } else {
            switch (back_.sk_dir) {
            case DIR_L: case DIR_DL: case DIR_UL: x = -COS(cnt2) * pich / 4096; break;
            case DIR_U: case DIR_D:               x = 0; break;
            case DIR_R: case DIR_DR: case DIR_UR: x =  COS(cnt2) * pich / 4096; break;
            default: break;
            }
            switch (back_.sk_dir) {
            case DIR_D: case DIR_DL: case DIR_DR: y =  COS(cnt2) * pich / 4096; break;
            case DIR_L: case DIR_R:               y = 0; break;
            case DIR_U: case DIR_UL: case DIR_UR: y = -COS(cnt2) * pich / 4096; break;
            default: break;
            }
        }
        switch (back_.sk_type) {
        case shake_sin_set:
        case shake_sin:
            display_.set_graph_smove(grp_back,     back_.x - x, back_.y - y);
            display_.set_graph_smove(grp_back + 1, back_.x - x, back_.y - y);
            // The plate the shake moves has to be the clean one: BMP_BACK
            // has the characters baked into it and they are about to be
            // drawn live instead.
            display_.set_graph_bmp(grp_back,     bmp_back2);
            display_.set_graph_bmp(grp_back + 1, bmp_back2);
            // GRP_WORK is a black rectangle at layer 0, so the edge the
            // shake pulls away from is black rather than last frame.
            display_.set_graph_prim(
                grp_work, GraphType::flat, Poly::rect, 0, true);
            display_.set_graph_pos_rect(grp_work, 0, 0, DISP_X, DISP_Y);
            display_.set_graph_fade(grp_work, 0);
            if (hooks_.set_char_pos_shake) {
                hooks_.set_char_pos_shake(x, y, 1);
            }
            break;
        case shake_txt_sin:
            // DSP_SetTextMove( TXT_WINDOW, ShakeDx+x, ShakeDy+y ): the text
            // itself shakes and the picture does not.
            shake_text_dx_ = x;
            shake_text_dy_ = y;
            break;
        case shake_all_sin_set:
        case shake_all_sin:
            display_.set_graph_global_pos(x, y);
            break;
        default:
            break;
        }
        break;

    case shake_2ti:
    case shake_all_2ti:
    case shake_txt_2ti:
        cnt = (back_.sk_cnt % 2) * 2 - 1;
        switch (back_.sk_dir) {
        case DIR_L: case DIR_DL: case DIR_UL: x = -cnt * pich; break;
        case DIR_U: case DIR_D:               x = 0; break;
        case DIR_R: case DIR_DR: case DIR_UR: x =  cnt * pich; break;
        default: break;
        }
        switch (back_.sk_dir) {
        case DIR_D: case DIR_DL: case DIR_DR: y =  cnt * pich; break;
        case DIR_L: case DIR_R:               y = 0; break;
        case DIR_U: case DIR_UL: case DIR_UR: y = -cnt * pich; break;
        default: break;
        }
        switch (back_.sk_type) {
        case shake_2ti:
            display_.set_graph_smove(grp_back,     back_.x - x, back_.y - y);
            display_.set_graph_smove(grp_back + 1, back_.x - x, back_.y - y);
            display_.set_graph_prim(
                grp_work, GraphType::flat, Poly::rect, 0, true);
            display_.set_graph_pos_rect(grp_work, 0, 0, DISP_X, DISP_Y);
            display_.set_graph_fade(grp_work, 0);
            break;
        case shake_txt_2ti:
            shake_text_dx_ = x;
            shake_text_dy_ = y;
            break;
        case shake_all_2ti:
            display_.set_graph_global_pos(x, y);
            break;
        default:
            break;
        }
        break;

    case shake_rand:
    case shake_all_rand:
    case shake_txt_rand:
        // The direction is re-rolled every frame and never repeats.
        cnt = std::rand() % 8;
        while (back_.sk_dir == cnt) {
            cnt = std::rand() % 8;
        }
        back_.sk_dir = cnt;
        switch (back_.sk_dir) {
        case DIR_L: case DIR_DL: case DIR_UL: x = -pich; break;
        case DIR_U: case DIR_D:               x = 0; break;
        case DIR_R: case DIR_DR: case DIR_UR: x =  pich; break;
        default: break;
        }
        switch (back_.sk_dir) {
        case DIR_D: case DIR_DL: case DIR_DR: y =  pich; break;
        case DIR_L: case DIR_R:               y = 0; break;
        case DIR_U: case DIR_UL: case DIR_UR: y = -pich; break;
        default: break;
        }
        switch (back_.sk_type) {
        case shake_rand:
            display_.set_graph_smove(grp_back,     back_.x - x, back_.y - y);
            display_.set_graph_smove(grp_back + 1, back_.x - x, back_.y - y);
            display_.set_graph_prim(
                grp_work, GraphType::flat, Poly::rect, 0, true);
            display_.set_graph_pos_rect(grp_work, 0, 0, DISP_X, DISP_Y);
            display_.set_graph_fade(grp_work, 0);
            break;
        case shake_txt_rand:
            shake_text_dx_ = x;
            shake_text_dy_ = y;
            break;
        case shake_all_rand:
            display_.set_graph_global_pos(x, y);
            break;
        default:
            break;
        }
        break;

    case shake_roll:
        cnt = 256 - back_.sk_cnt * 256 / back_max;
        cnt = 256 - cnt * cnt / 256;
        if (back_.sk_dir % 2) {
            cnt = cnt * pich / 2 % 256;
        } else {
            cnt = 256 - cnt * pich / 2 % 256;
        }
        display_.set_graph_prim(
            grp_work, GraphType::flat, Poly::rect, 0, true);
        display_.set_graph_pos_rect(grp_work, 0, 0, DISP_X, DISP_Y);
        display_.set_graph_fade(grp_work, 0);
        display_.get_graph_bmp_size(grp_back, &w, &h);
        display_.set_graph_roll(grp_back, DISP_X / 2, DISP_Y / 2, 0, cnt, 0, 0, w, h);
        display_.get_graph_bmp_size(grp_back + 1, &w, &h);
        display_.set_graph_roll(grp_back + 1, DISP_X / 2, DISP_Y / 2, 0, cnt, 0, 0, w, h);
        break;

    case shake_roll_sin:
        cnt = back_.sk_cnt * back_.sk_swing / 8;
        y = -COS(cnt) * pich / 4096;
        if (back_max) {
            y = y * (back_max - back_.sk_cnt) / back_max;
        }
        display_.get_graph_bmp_size(grp_back, &w, &h);
        display_.set_graph_roll(
            grp_back, DISP_X / 2, DISP_Y / 2, 0, (256 + y) % 256, 0, 0, w, h);
        display_.get_graph_bmp_size(grp_back + 1, &w, &h);
        display_.set_graph_roll(
            grp_back + 1, DISP_X / 2, DISP_Y / 2, 0, (256 + y) % 256, 0, 0, w, h);
        break;

    case shake_roll_2ti:
        cnt = back_.sk_cnt % 4;
        switch (cnt) {
        case 0: y = -pich; break;
        case 3:
        case 1: y = 0; break;
        case 2: y = pich; break;
        default: break;
        }
        display_.get_graph_bmp_size(grp_back, &w, &h);
        display_.set_graph_roll(
            grp_back, DISP_X / 2, DISP_Y / 2, 0, (256 + y) % 256, 0, 0, w, h);
        display_.get_graph_bmp_size(grp_back + 1, &w, &h);
        display_.set_graph_roll(
            grp_back + 1, DISP_X / 2, DISP_Y / 2, 0, (256 + y) % 256, 0, 0, w, h);
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------- the setters -

bool AvgBack::set_shake(int type, int pich, int speed, int dir, int swing)
{
    // BOOL AVG_SetShake(...).  The first line is the whole of skipping: a
    // shake does not start at all while the key is down.
    if (hooks_.eff_cnt && hooks_.eff_cnt(1) == 0) {
        return false;
    }
    back_.sk_flag = 1;
    back_.sk_dir = dir;
    back_.sk_pich = pich;
    back_.sk_cnt = 0;
    back_.sk_type = type;
    back_.sk_speed = speed;
    back_.sk_swing = swing;

    if (back_.sk_type == shake_sin && speed == 0) {
        back_.sk_type = shake_sin_set;
    }
    if (back_.sk_type == shake_all_sin && speed == 0) {
        back_.sk_type = shake_all_sin_set;
    }
    if (back_.sk_type == shake_sin || back_.sk_type == shake_all_sin) {
        if (hooks_.reset_half_tone) hooks_.reset_half_tone();
        if (hooks_.novel_message_disp) hooks_.novel_message_disp(false);
    }
    shake_text_dx_ = 0;
    shake_text_dy_ = 0;
    return true;
}

bool AvgBack::wait_shake() const
{
    // BOOL AVG_WaitShake( void ), verbatim - a speed of zero is a shake that
    // never ends on its own, so it never holds the script either.
    if (back_.sk_speed == 0) return false;
    if (back_.sk_flag == 0) return false;
    return true;
}

void AvgBack::stop_shake()
{
    if (!back_.sk_flag) {
        return;
    }
    back_.sk_flag = 0;

    if (back_.sk_dir < 2 || back_.sk_type != shake_roll) {
        display_.set_graph_zoom2(grp_back, DISP_X / 2, DISP_Y / 2, 0);
        display_.set_graph_param(grp_back, DRW_NML);
        display_.set_graph_move(grp_back, back_.sx, back_.sy);
        display_.set_graph_smove(grp_back, back_.x, back_.y);

        display_.set_graph_zoom2(grp_back + 1, DISP_X / 2, DISP_Y / 2, 0);
        display_.set_graph_param(grp_back + 1, DRW_NML);
        display_.set_graph_move(grp_back + 1, back_.sx, back_.sy);
        display_.set_graph_smove(grp_back + 1, back_.x, back_.y);
    }
    display_.reset_graph(grp_work);
    shake_text_dx_ = 0;
    shake_text_dy_ = 0;

    if (back_.sk_type == shake_sin || back_.sk_type == shake_all_sin) {
        // SetCharPosShake( 0, 0, OFF ) is the only thing that puts a
        // character back into the plate once a sine shake has taken it out.
        if (hooks_.set_char_pos_shake) {
            hooks_.set_char_pos_shake(0, 0, 0);
        }
    }
    display_.set_graph_global_pos(0, 0);
}

void AvgBack::copy_back(bool sc)
{
    if (!back_.flag) {
        return;
    }
    if (sc) {
        switch (back_.sc_type / 3) {
        case 0: set_back_pos(back_.sx, back_.sy); break;
        case 1: set_back_pos_zoom(back_.sx, back_.sy, back_.sw, back_.sh); break;
        default: break;
        }
    } else {
        switch (back_.sc_type / 3) {
        case 0: set_back_pos(back_.x, back_.y); break;
        case 1: set_back_pos_zoom(back_.x, back_.y, back_.w, back_.h); break;
        default: break;
        }
    }
    // DSP_CopyBmp( BMP_BACK, BMP_BACK2 ): the clean plate back over the
    // baked one.  A bake cannot be undone in place.
    display_.copy_bmp(bmp_back, bmp_back2);
}

void AvgBack::set_back_pos(int x, int y)
{
    back_.zoom = 0;
    back_.x = x;
    back_.y = y;
    back_.w = DISP_X;
    back_.h = DISP_Y;

    back_.x2 = x + DISP_X;
    back_.y2 = y;
    back_.x3 = x;
    back_.y3 = y + DISP_Y;
    back_.x4 = x + DISP_X;
    back_.y4 = y + DISP_Y;

    display_.set_graph_pos(grp_back, 0, 0, x, y, DISP_X, DISP_Y);
}

void AvgBack::set_back_pos_zoom(int x, int y, int w, int h)
{
    int sx = 0;
    int sy = 0;
    display_.get_graph_bmp_size(grp_back, &sx, &sy);
    back_.zoom = 1;
    back_.x = x;
    back_.y = y;
    back_.w = w;
    back_.h = h;
    if (w == 0 || h == 0) {
        return;
    }
    display_.set_graph_pos_zoom(
        grp_back, -x * DISP_X / w, -y * DISP_Y / h,
        sx * DISP_X / w, sy * DISP_Y / h, 0, 0, sx, sy);
}

void AvgBack::set_back_scroll(int x, int y, int w, int h, int frame, int type)
{
    back_.sc_flag = 1;
    back_.sx = x;
    back_.sy = y;
    back_.sw = w;
    back_.sh = h;
    back_.sc_type = type;
    back_.sc_cnt = 0;
    back_.sc_max = frame;
}

void AvgBack::set_back_scroll_poly(int x1, int y1, int x2, int y2,
                                   int x3, int y3, int x4, int y4,
                                   int frame, int type)
{
    back_.sc_flag = 1;
    back_.sx = x1;   back_.sy = y1;
    back_.sx2 = x2;  back_.sy2 = y2;
    back_.sx3 = x3;  back_.sy3 = y3;
    back_.sx4 = x4;  back_.sy4 = y4;
    back_.sc_type = type;
    back_.sc_cnt = 0;
    back_.sc_max = frame;
}

void AvgBack::set_back_fade(int r, int g, int b, int fade)
{
    // void AVG_SetBackFade(int r,int g,int b, int fade ), verbatim.  It
    // takes the wash off and hides the text first, because the fade drives
    // GRP_BACK's brightness and the darkened copy would fight it.
    if (hooks_.reset_half_tone) hooks_.reset_half_tone();
    if (hooks_.novel_message_disp) hooks_.novel_message_disp(false);

    back_.br_flag = 1;
    back_.br_cnt = 0;
    back_.br_fade = fade;
    back_.br_pat = -1;
    back_.er = r;
    back_.eg = g;
    back_.eb = b;
}

void AvgBack::set_back_fade_direct(int r, int g, int b)
{
    back_.rr = back_.r = back_.er = r;
    back_.gg = back_.g = back_.eg = g;
    back_.bb = back_.b = back_.eb = b;
}

void AvgBack::reset_back_half_tone(int bak_no, int chg_type)
{
    // void AVG_ResetBackHalfTone( int bak_no, int chg_type ), verbatim.
    if (back_.flag && bak_no == back_.bno && chg_type == bak_direct) {
        return;
    }
    if (hooks_.reset_half_tone) hooks_.reset_half_tone();
    if (hooks_.novel_message_disp) hooks_.novel_message_disp(false);
}

void AvgBack::begin_back(int bak_no, int x, int y, int chg_type, int cg_flag,
                         int fd_max, int vague)
{
    // The bookkeeping half of AVG_SetBack.  Everything to do with finding,
    // decoding and toning the picture stays with the caller, which owns the
    // archive and the cache; what belongs here is the state the control
    // functions then run on.
    back_.flag = 1;
    back_.bno = bak_no;
    back_.cg_flag = cg_flag;
    back_.fd_type = chg_type;
    back_.fd_cnt = 0;
    back_.fd_max = fd_max;
    back_.fd_vague = vague;
    back_.fd_flag = chg_type == bak_direct ? 0 : 1;
    back_.sc_flag = 0;
    back_.sc_type = 0;
    back_.redraw = 1;
    set_back_pos(x, y);
}

void AvgBack::set_fade(int r, int g, int b, int disp, int fade)
{
    // void AVG_SetFade( int r, int g, int b, int disp, int fade ).  The
    // start of the ramp is wherever the last one left off, which is why a
    // flash that is interrupted does not jump.
    fade_.flag = 1;
    fade_.cnt = 0;
    fade_.sr = fade_.r;
    fade_.sg = fade_.g;
    fade_.sb = fade_.b;
    fade_.er = r;
    fade_.eg = g;
    fade_.eb = b;
    fade_.disp = disp;
    fade_.flash = 0;
    fade_.fade = fade;
    // The rest of AVG_SetFade freezes the screen: DSP_GetDispBmp captures it
    // into BMP_DISP, GRP_DISP draws that at LAY_BACK, and every graph
    // without DSP_GetGraphBrightFlag is hidden for the duration.  Ours tints
    // the composited frame instead, so there is nothing to hide - but the
    // two legs and their counters are the same.
}

void AvgBack::set_flash(int r, int g, int b, int fade1, int fade2)
{
    // void AVG_SetFlash( int r, int g, int b, int fade1, int fade2 ):
    //     AVG_SetFade( r, g, b, ON, fade1 );
    //     if(fade2<=0) fade2=1;
    //     FadeStruct.flash = fade2;
    set_fade(r, g, b, 1, fade1);
    if (fade2 <= 0) {
        fade2 = 1;
    }
    fade_.flash = fade2;
}

void AvgBack::control_fade()
{
    // void AVG_ColtrolFade( void ).
    const int max = eff_cnt(fade_.fade);
    if (!fade_.flag) {
        return;
    }
    fade_.cnt++;
    if (fade_.cnt >= max) {
        fade_.flag = 0;
        fade_.r = fade_.er;
        fade_.g = fade_.eg;
        fade_.b = fade_.eb;
        if (fade_.disp && fade_.flash) {
            // The return leg, started from inside the control function
            // rather than by anything outside it.
            set_fade(bright_neutral, bright_neutral, bright_neutral,
                     0, fade_.flash);
        }
    } else {
        fade_.r = (fade_.sr * (max - fade_.cnt) + fade_.er * fade_.cnt) / max;
        fade_.g = (fade_.sg * (max - fade_.cnt) + fade_.eg * fade_.cnt) / max;
        fade_.b = (fade_.sb * (max - fade_.cnt) + fade_.eb * fade_.cnt) / max;
    }
}

void AvgBack::open_back()
{
    display_.set_graph_disp(grp_back + 1, true);
}

void AvgBack::close_back()
{
    display_.set_graph_disp(grp_back + 1, false);
}

}  // namespace th2
