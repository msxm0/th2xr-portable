#include "avg_char.hpp"

#include <algorithm>

namespace th2 {
namespace {

// CharLocateTable[9] - the nine standing positions, in the original's units.
constexpr int char_locate_table[9] = {
    -160 * 80 / 64, 0, 160 * 80 / 64,
    -192 * 80 / 64, 192 * 80 / 64,
    -300 * 80 / 64, 300 * 80 / 64,
    -400 * 80 / 64, 400 * 80 / 64,
};

// CharPosTable[12] is all zeroes in the shipped source, so the per-character
// nudge it would apply is nothing.  Kept because SetCharPos and
// SetCharPosShake both consult it and both skip it for the 90..99 and
// 190..199 pose ranges; without the table those two branches would look
// like dead code and get dropped.
constexpr int char_pos_table[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

constexpr int char_dy = 0;  // CHAR_DY

std::uint32_t drw_bld_of(int alpha)
{
    return drw_bld
        | (static_cast<std::uint32_t>(std::clamp(alpha, 0, 256)) << 16);
}

std::uint32_t drw_ami_of(int rate)
{
    return drw_ami | (static_cast<std::uint32_t>(std::clamp(rate, 0, 256)) << 16);
}

std::uint32_t drw_lpp_of(int direction, int rate)
{
    return (drw_lpp + static_cast<std::uint32_t>(direction))
        | (static_cast<std::uint32_t>(std::clamp(rate, 0, 256)) << 16);
}

}  // namespace

int char_locate(int locate)
{
    return char_locate_table[
        static_cast<std::size_t>(std::clamp(locate, 0, 8))];
}

int AvgChar::char_index(int char_no) const
{
    int i = 0;
    for (; i < max_char; ++i) {
        if (chars_[i].flag && chars_[i].cno == char_no) {
            break;
        }
    }
    return i;
}

int AvgChar::space_index() const
{
    int i = 0;
    for (; i < max_char; ++i) {
        if (!chars_[i].flag) {
            break;
        }
    }
    return i;
}

int AvgChar::locate_offset(int index) const
{
    const CharState& character = chars_[index];
    if (character.cno < 10) {
        if (!((90 <= character.pose && character.pose < 100)
              || (190 <= character.pose && character.pose < 200))) {
            return char_pos_table[static_cast<std::size_t>(character.cno)];
        }
    }
    return 0;
}

void AvgChar::set_char_pos(int index, int x)
{
    x += locate_offset(index);
    display_.set_graph_pos(
        grp_char + index, x, -char_dy, 0, 0,
        display_width, display_height + char_dy * 2);
}

void AvgChar::set_char_pos_shake(int shx, int shy, int disp)
{
    for (int i = 0; i < max_char; ++i) {
        CharState& character = chars_[i];
        if (!character.flag) {
            continue;
        }
        const int x = locate_offset(i);
        display_.set_graph_move(
            grp_char + i, char_locate(character.loc1) + shx + x,
            shy - char_dy);
        display_.set_graph_move(
            grp_spback + i + 1, char_locate(character.loc1) + shx + x,
            shy - char_dy);
        if (character.cond == char_cond_nomal && disp != -1) {
            // The only caller that passes ON is a sine shake, which is why
            // that is the one family where characters leave the plate and
            // move on their own.
            character.cut_mode = disp ? 2 : 0;
        }
    }
}

void AvgChar::set_char_bright_all(int r, int g, int b)
{
    for (int i = 0; i < max_char; ++i) {
        if (chars_[i].flag) {
            display_.set_graph_bright(grp_spback + i + 1, r, g, b);
        }
    }
}

void AvgChar::set_char(
    int char_no, int pose, int locate, int layer, int in_type,
    int bright, int alph, int frame)
{
    int index = char_index(char_no);

    // The equality check comes first: a call that changes nothing returns
    // without tearing the wash down.
    if (index != max_char) {
        const CharState& held = chars_[index];
        if (held.cno == char_no && held.pose == pose && held.loc1 == locate
            && held.fade1 == bright && held.layer == layer
            && held.alph1 == alph) {
            return;
        }
    }

    if (in_type != char_type_wait) {
        if (hooks_.reset_half_tone) {
            hooks_.reset_half_tone();
        }
        if (hooks_.novel_message_disp) {
            hooks_.novel_message_disp(false);
        }
    }

    if (index != max_char) {
        if (in_type == char_type_wait || in_type == char_type_direct) {
            CharState& character = chars_[index];
            character.flag = 1;
            if (in_type == char_type_wait) {
                character.cond = char_cond_wait;
                character.cut_mode = 0;
            } else {
                character.cond = char_cond_nomal;
                if (hooks_.load_char) {
                    hooks_.load_char(index, char_no, pose, in_type);
                }
                display_.set_graph_param(
                    grp_char + index, drw_bld_of(alph));
                character.cut_mode = 0;
            }
            character.type = in_type;
            character.cno = char_no;
            character.pose = pose;
            character.loc1 = locate;
            character.fade1 = bright;
            character.cnt = 0;
            character.max = frame;
            character.grp = 0;
            character.layer = layer;
            character.alph1 = alph;
        } else {
            // Already on screen and only the pose differs: that is a
            // cross-fade, not a fresh entrance.
            if (chars_[index].pose != pose) {
                set_char_pose(char_no, pose, char_type_cfade, frame);
            }
        }
        return;
    }

    index = space_index();
    if (index == max_char) {
        return;  // "これ以上、キャラクターを登録出来ません"
    }
    CharState& character = chars_[index];
    if (in_type == char_type_wait || in_type == char_type_direct) {
        character.flag = 1;
        if (in_type == char_type_wait) {
            character.cond = char_cond_wait;
            character.cut_mode = 0;
        } else {
            character.cut_mode = 0;
            character.cond = char_cond_nomal;
            if (hooks_.load_char) {
                hooks_.load_char(index, char_no, pose, in_type);
            }
            display_.set_graph_param(grp_char + index, drw_bld_of(alph));
        }
        character.type = in_type;
        character.cno = char_no;
        character.pose = pose;
        character.loc1 = locate;
        character.fade1 = bright;
        character.cnt = 0;
        character.max = frame;
        character.grp = 0;
        character.layer = layer;
        character.alph1 = alph;
    } else {
        character.flag = 1;
        character.cond = char_cond_in;
        character.type = in_type;
        character.cno = char_no;
        character.pose = pose;
        character.loc1 = locate;
        character.fade1 = bright;
        character.cnt = 0;
        character.max = frame;
        character.grp = 0;
        character.layer = layer;
        character.alph1 = alph;
        // Registered as already baked, which AVG_ControlChar's first loop
        // notices next frame and turns into a full plate rebuild.
        character.cut_mode = 1;
        if (hooks_.load_char) {
            hooks_.load_char(index, char_no, pose, in_type);
        }
    }
}

void AvgChar::release_char(int index)
{
    display_.reset_graph(grp_char + index);
    display_.release_bmp(bmp_char + index * 2);
    display_.release_bmp(bmp_char + index * 2 + 1);
    chars_[index] = CharState{};
}

void AvgChar::reset_char(int char_no, int out_type, int frame)
{
    const int index = char_index(char_no);
    if (index == max_char) {
        return;
    }
    if (out_type == char_type_wait) {
        chars_[index].type = char_type_wait2;
        return;
    }
    if (hooks_.reset_half_tone) {
        hooks_.reset_half_tone();
    }
    if (hooks_.novel_message_disp) {
        hooks_.novel_message_disp(false);
    }
    chars_[index].cond = char_cond_out;
    chars_[index].type = out_type;
    chars_[index].cnt = 0;
    chars_[index].max = frame;
}

void AvgChar::set_char_pose(int char_no, int pose, int in_type, int frame)
{
    const int index = char_index(char_no);
    if (index == max_char) {
        // "登録していないキャラクターです" - register it instead.
        set_char(char_no, pose, 1, 0, in_type, bright_neutral, 256, frame);
        return;
    }
    if (in_type == char_type_wait) {
        set_char(char_no, pose, chars_[index].loc1, chars_[index].layer,
                 char_type_wait, chars_[index].fade1, chars_[index].alph1,
                 frame);
        return;
    }
    if (chars_[index].pose == pose) {
        return;
    }
    CharState& character = chars_[index];
    character.cond = char_cond_pose;
    character.pose = pose;
    character.cnt = 0;
    character.max = frame;
    // The slot has two bitmaps and the dissolve runs between them, so a new
    // pose goes into whichever one is not currently showing.
    character.grp = character.grp ? 0 : 1;

    if (hooks_.reset_half_tone) {
        hooks_.reset_half_tone();
    }
    if (hooks_.novel_message_disp) {
        hooks_.novel_message_disp(false);
    }
    if (hooks_.load_char) {
        hooks_.load_char(index, char_no, pose, char_type_cfade);
    }
    display_.set_graph_bset(
        grp_char + index, bmp_char + index * 2 + !character.grp, 0);
}

void AvgChar::set_char_locate(int char_no, int locate, int frame)
{
    const int index = char_index(char_no);
    if (index == max_char) {
        return;
    }
    CharState& character = chars_[index];
    character.cond = char_cond_locate;
    character.loc2 = character.loc1;
    character.loc1 = locate;
    character.cnt = 0;
    character.max = frame;
    if (hooks_.reset_half_tone) {
        hooks_.reset_half_tone();
    }
    if (hooks_.novel_message_disp) {
        hooks_.novel_message_disp(false);
    }
}

void AvgChar::set_char_layer(int char_no, int layer)
{
    const int index = char_index(char_no);
    if (index == max_char) {
        return;
    }
    if (layer < -1 || layer > 5) {
        layer = std::clamp(layer, 0, 4);
    }
    chars_[index].layer = layer;
    display_.set_graph_layer(grp_char + index, lay_char + chars_[index].layer);
    if (hooks_.reset_half_tone) {
        hooks_.reset_half_tone();
    }
    if (hooks_.novel_message_disp) {
        hooks_.novel_message_disp(false);
    }
}

void AvgChar::set_char_bright(int char_no, int fade, int fade_count)
{
    const int index = char_index(char_no);
    if (index == max_char) {
        return;
    }
    CharState& character = chars_[index];
    if (fade_count <= 0) {
        character.cond = char_cond_nomal;
        character.fade2 = character.fade1;
        character.fade1 = fade;
        character.cnt = 0;
        character.max = fade_count;
        character.cut_mode = 0;
        display_.set_graph_bright(grp_char + index, fade, fade, fade);
    } else {
        character.cond = char_cond_bright;
        character.fade2 = character.fade1;
        character.fade1 = fade;
        character.cnt = 0;
        character.max = fade_count;
    }
    if (hooks_.reset_half_tone) {
        hooks_.reset_half_tone();
    }
    if (hooks_.novel_message_disp) {
        hooks_.novel_message_disp(false);
    }
}

void AvgChar::set_char_alph(int char_no, int alph, int fade_count)
{
    const int index = char_index(char_no);
    if (index == max_char) {
        return;
    }
    CharState& character = chars_[index];
    if (fade_count <= 0) {
        character.cond = char_cond_nomal;
        character.alph2 = character.alph1;
        character.alph1 = alph;
        character.cnt = 0;
        character.max = fade_count;
        character.cut_mode = 0;
        display_.set_graph_param(grp_char + index, drw_bld_of(alph));
    } else {
        character.cond = char_cond_alpha;
        character.alph2 = character.alph1;
        character.alph1 = alph;
        character.cnt = 0;
        character.max = fade_count;
    }
    if (hooks_.reset_half_tone) {
        hooks_.reset_half_tone();
    }
    if (hooks_.novel_message_disp) {
        hooks_.novel_message_disp(false);
    }
}

void AvgChar::open_char()
{
    for (int i = 0; i < max_char; ++i) {
        display_.set_graph_disp(grp_char + i, true);
    }
}

void AvgChar::close_char()
{
    for (int i = 0; i < max_char; ++i) {
        display_.set_graph_disp(grp_char + i, false);
    }
}

void AvgChar::set_back_release_char()
{
    for (int i = 0; i < max_char; ++i) {
        if (chars_[i].type == char_type_wait2) {
            release_char(i);
        }
    }
}

void AvgChar::set_back_char(int x, int y, int char_disp)
{
    // By layer, then by slot.  The slot order is not insertion order - a
    // freed slot is reused - so this is the walk that decides who occludes
    // whom in the plate.
    for (int l = 0; l < 5; ++l) {
        for (int i = 0; i < max_char; ++i) {
            CharState& character = chars_[i];
            if (character.layer != l) {
                continue;
            }
            if (!character.flag
                || !(character.cond == char_cond_nomal
                     || character.cond == char_cond_wait)) {
                continue;
            }
            character.cond = char_cond_nomal;
            if (hooks_.load_char) {
                hooks_.load_char(i, character.cno, character.pose,
                                 char_type_direct);
            }
            character.cut_mode = char_disp == 1 ? 1 : 0;
            display_.set_graph_param(
                grp_char + i, drw_bld_of(character.alph1));
            display_.set_graph_fade(grp_char + i, character.fade1);

            int mx = 0;
            int my = 0;
            display_.get_graph_move(grp_char + i, &mx, &my);
            display_.set_graph_move(grp_char + i, mx + x, my + y);
            int sx = 0;
            int sy = 0;
            display_.get_bmp_size(bmp_back, &sx, &sy);
            display_.set_graph_clip(grp_char + i, 0, 0, sx, sy);
            display_.set_graph_target(grp_char + i, bmp_back);
            display_.set_graph_move(grp_char + i, mx, my);
        }
    }
}

bool AvgChar::wait_char(int char_no) const
{
    const int index = char_index(char_no);
    if (index == max_char) {
        return false;
    }
    return chars_[index].cond != char_cond_nomal
        && chars_[index].cond != char_cond_wait;
}

void AvgChar::init_char()
{
    for (auto& character : chars_) {
        character = CharState{};
    }
    win_flag_ = 0;
}

void AvgChar::control_char()
{
    const bool sp = hooks_.level && hooks_.level();
    const bool ami = hooks_.ami && hooks_.ami();
    int disp = 0;

    if (hooks_.back_flag && hooks_.back_flag()) {
        // A character that starts animating while it is still in the plate
        // takes the whole plate back to the clean copy: it cannot be taken
        // out in place.  Suppressed while the background is scrolling or
        // zooming, because the plate is being rebuilt anyway.
        const bool scrolling = hooks_.back_scrolling && hooks_.back_scrolling();
        const bool zooming = hooks_.back_zooming && hooks_.back_zooming();
        if (!scrolling && !zooming) {
            for (int i = 0; i < max_char; ++i) {
                if (chars_[i].flag) {
                    if (chars_[i].cond != char_cond_nomal
                        && chars_[i].cut_mode) {
                        disp = 1;
                        break;
                    }
                }
            }
        }
        if (hooks_.back_redraw_take && hooks_.back_redraw_take()) {
            disp = 1;
        }
        if (disp) {
            if (hooks_.copy_back) {
                hooks_.copy_back();
            }
            if (hooks_.create_back_cscope) {
                hooks_.create_back_cscope();
            }
            for (int i = 0; i < max_char; ++i) {
                chars_[i].cut_mode = 0;
            }
        }

        // Anyone ordered above an animating character stays a layer too, or
        // the plate - drawn as the background - would cover the one that is
        // moving.
        for (int i = 0; i < max_char; ++i) {
            if (chars_[i].flag && chars_[i].cond != char_cond_nomal
                && chars_[i].cond != char_cond_wait) {
                for (int j = 0; j < max_char; ++j) {
                    if (chars_[j].flag && chars_[j].cond == char_cond_nomal
                        && chars_[i].cond != char_cond_wait) {
                        if (chars_[j].layer * max_char + j
                            > chars_[i].layer * max_char + i) {
                            chars_[j].disp = 1;
                        }
                    }
                }
            }
        }

        for (int l = 0; l < 5; ++l) {
            for (int i = 0; i < max_char; ++i) {
                CharState& character = chars_[i];
                if (!character.flag || character.layer != l) {
                    continue;
                }
                if (character.cond == char_cond_nomal && !character.disp) {
                    switch (character.cut_mode) {
                    case 0: {
                        display_.set_graph_disp(grp_char + i, true);
                        int sx = 0;
                        int sy = 0;
                        display_.get_bmp_size(bmp_back, &sx, &sy);
                        display_.set_graph_clip(grp_char + i, 0, 0, sx, sy);
                        display_.get_graph_move(grp_char + i, &sx, &sy);
                        int back_x = 0;
                        int back_y = 0;
                        if (hooks_.back_pos) {
                            hooks_.back_pos(&back_x, &back_y);
                        }
                        display_.set_graph_move(
                            grp_char + i, sx + back_x, sy + back_y);
                        display_.set_graph_target(grp_char + i, bmp_back);
                        display_.set_graph_move(grp_char + i, sx, sy);
                        character.cut_mode = 1;
                        break;
                    }
                    case 1:
                        display_.set_graph_disp(grp_char + i, false);
                        break;
                    case 2:
                        display_.set_graph_disp(grp_char + i, true);
                        break;
                    }
                } else {
                    if (character.cond != char_cond_wait) {
                        character.disp = 0;
                        character.cut_mode = 0;
                        display_.set_graph_disp(grp_char + i, true);
                    } else {
                        character.cond = char_cond_wait;
                    }
                }
            }
        }
    }

    for (int i = 0; i < max_char; ++i) {
        CharState& character = chars_[i];
        std::uint32_t param = drw_bld_of(character.alph1);
        int bright = character.fade1;

        int max = hooks_.eff_cnt ? hooks_.eff_cnt(character.max)
                                 : character.max;
        const int max2 = max;
        int cnt = character.cnt;
        if (!character.flag) {
            continue;
        }
        int rate = 0;
        int locate = 0;
        switch (character.cond) {
        case char_cond_nomal:
            break;
        case char_cond_wait:
            display_.set_graph_disp(grp_char + i, false);
            break;
        case char_cond_in:
            if (max) {
                switch (character.type) {
                case char_type_move:
                case char_type_move + 1:
                    // The walk on eases out of the wings: the remaining
                    // count is cubed, not the elapsed one.
                    cnt = max - cnt;
                    cnt = cnt * cnt * cnt;
                    max = max * max * max;
                    cnt = max - cnt;
                    locate = character.type == char_type_move ? -600 : 600;
                    rate = static_cast<int>(
                        static_cast<long long>(
                            char_locate(character.loc1) - locate)
                        * cnt / max);
                    set_char_pos(i, locate + rate);
                    break;
                case char_type_cfade:
                case char_type_wave:
                    if (character.alph1 != 256) {
                        rate = character.alph1 * cnt / max;
                        param = drw_bld_of(rate);
                    } else {
                        rate = 256 * cnt / max;
                        // With the effects setting on it is a plain blend;
                        // off, it is a dither mesh or a pattern wipe.
                        if (sp) {
                            param = drw_bld_of(rate);
                        } else if (ami) {
                            param = drw_ami_of(rate);
                        } else {
                            param = drw_lpp_of(dir_up, rate);
                        }
                    }
                    break;
                }
            }
            break;
        case char_cond_out:
            if (max) {
                switch (character.type) {
                case char_type_move:
                case char_type_move + 1:
                    // CHAR_COND_OUT cubes the remaining count and does not
                    // invert it again, which is what makes leaving the
                    // mirror of arriving.
                    cnt = max - cnt;
                    cnt = cnt * cnt * cnt;
                    max = max * max * max;
                    locate = character.type == char_type_move ? -600 : 600;
                    rate = static_cast<int>(
                        static_cast<long long>(
                            char_locate(character.loc1) - locate)
                        * cnt / max);
                    set_char_pos(i, locate + rate);
                    break;
                case char_type_cfade:
                case char_type_wave:
                    if (character.alph1 != 256) {
                        rate = character.alph1 - character.alph1 * cnt / max;
                        param = drw_bld_of(rate);
                    } else {
                        rate = 256 - 256 * cnt / max;
                        if (sp) {
                            param = drw_bld_of(rate);
                        } else if (ami) {
                            param = drw_ami_of(rate);
                        } else {
                            param = drw_lpp_of(dir_up, rate);
                        }
                    }
                    break;
                }
            }
            break;
        case char_cond_pose:
            if (max) {
                // The dissolve is a blend factor across the slot's two
                // bitmaps, so the character never becomes see-through.
                rate = 256 * cnt / max;
                if (character.alph1 != 256 || sp) {
                    display_.set_graph_param2(
                        grp_char + i, drw_bld_of(rate));
                } else if (ami) {
                    display_.set_graph_param2(
                        grp_char + i, drw_ami_of(rate));
                } else {
                    display_.set_graph_param2(
                        grp_char + i, drw_lpp_of(dir_up, rate));
                }
            }
            break;
        case char_cond_locate:
            if (max) {
                cnt = max - cnt;
                cnt = cnt * cnt * cnt;
                max = max * max * max;
                cnt = max - cnt;
                rate = static_cast<int>(
                    static_cast<long long>(
                        char_locate(character.loc1) - char_locate(character.loc2))
                    * cnt / max);
                set_char_pos(i, char_locate(character.loc2) + rate);
            } else {
                set_char_pos(i, char_locate(character.loc1));
            }
            break;
        case char_cond_bright:
            rate = max
                ? (character.fade2 * (max - cnt) + character.fade1 * cnt) / max
                : character.fade1;
            bright = rate;
            break;
        case char_cond_alpha:
            rate = max
                ? (character.alph2 * (max - cnt) + character.alph1 * cnt) / max
                : character.alph1;
            param = drw_bld_of(rate);
            break;
        default:
            break;
        }
        display_.set_graph_param(grp_char + i, param);
        display_.set_graph_fade(grp_char + i, bright);
        if (character.type == char_type_wave
            && character.cond != char_cond_nomal) {
            display_.set_graph_target(grp_char + i, bmp_back);
        }

        if (character.cond != char_cond_nomal
            && character.cond != char_cond_wait) {
            // The message window closes for the length of an animation and
            // is put back afterwards, remembered by a flag - so the text
            // does come back without the script printing another line.
            if (hooks_.window_cond && hooks_.window_cond()) {
                win_flag_ = 1;
                if (hooks_.close_window) {
                    hooks_.close_window();
                }
            }
            if (character.cnt >= max2) {
                if (win_flag_) {
                    win_flag_ = 0;
                    if (hooks_.open_window) {
                        hooks_.open_window();
                    }
                }
                if (character.cond == char_cond_out) {
                    release_char(i);
                    break;
                }
                if (character.type == char_type_wave) {
                    display_.set_graph_disp(grp_char + i, false);
                }
                if (character.cond == char_cond_pose) {
                    display_.reset_graph_bset(grp_char + i);
                    display_.release_bmp(
                        bmp_char + i * 2 + !character.grp);
                }
                character.cond = char_cond_nomal;
            }
            character.cnt++;
        }
    }
}

}  // namespace th2
