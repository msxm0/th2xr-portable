/* Scripted input for the reference build.
 *
 * Replaces KEY_RenewKeybord and MUS_RenewMouse for trace runs.  Both of
 * those read the real devices, and the mouse is the subtler of the two: the
 * engine hit-tests the system bar against the live cursor position, so a
 * trace run with the pointer parked over a button would take a different
 * path from one with it elsewhere.  Leaving them uncalled and driving
 * KeyCond from a script is what makes a run repeatable.
 *
 * Script format, one event per line, '#' to end of line is a comment:
 *
 *     120 enter        # a single-frame press at tick 120
 *     300 esc
 *     420 ctrl 90      # held from tick 420 for 90 ticks
 *     30 every 30 lclick 3   # that click, every 30 ticks, forever
 *     30 every 30 lclick 3 until 9000   # ...on the ticks before 9000
 *
 * A route is hundreds of thousands of ticks of clicking; `every` is one event
 * whose firing is arithmetic.  A recorded run (reference/record.sh) is the
 * opposite - thousands of one-shot lines - which is why the events are
 * sorted and looked up by tick rather than scanned.
 *
 * A press sets both trg (the edge) and btn (the level) for its duration,
 * and btrg follows trg, which is what KEY_RenewKeybord would do for a key
 * held for one frame.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "keybord.h"
#include "mouse.h"
#ifndef ON
#define ON 1
#define OFF 0
#endif
#include "dispSprite.h"
extern ANIME_CONTROL AnimeControl[ANIME_CONTROL_MAX];

/* GM_Avg.cpp's WEATHER_STRUCT, layout copied for TH2REF_WEATHER_LOG. */
typedef struct { char flag; char type; float x, y; float ax, ay; unsigned long cnt; } TH2REF_WOBJ;
typedef struct { int flag; int wno; unsigned long cnt; TH2REF_WOBJ obj[200];
                 int reset; int reset_cnt; int noreset; float wind; int speed; int amount;
                 int twind; int tspeed; int tamount; } TH2REF_WEATHER;
extern TH2REF_WEATHER Weather;

extern "C" unsigned long th2ref_current_tick(void);

/* Scripted pointer.  MUS_RenewMouse reads the real device three ways -
 * GetCursorPos, ScreenToClient and GetAsyncKeyState - and all three are
 * redirected here by the prelude, so the function itself runs untouched and
 * all of its rect and trigger bookkeeping still happens. */
static int  g_mouse_x = 0, g_mouse_y = 0;
static bool g_mouse_l = false, g_mouse_r = false;

namespace {

enum Kind { EV_KEY, EV_MOVE, EV_LCLICK, EV_RCLICK, EV_MAPPICK };

struct Event {
    unsigned long tick;
    Kind          kind;
    int           offset;   /* EV_KEY: byte offset inside KEY_STRUCT */
    int           x, y;     /* EV_MOVE */
    unsigned long hold;
    unsigned long period;   /* 0 one-shot, else repeats every `period` */
    unsigned long until;    /* a repeat stops on this tick (0: never) */
};

/* Split and sorted once, so a tick costs a binary search and the events in
 * effect instead of a scan of every line - a recorded run is thousands of
 * pointer moves (reference/record.sh), which used to overrun a fixed table
 * and make every tick quadratic in the length of the run.  Kept exactly in
 * step with TraceScript in src/trace.cpp. */
std::vector<Event> g_moves;     /* by tick */
std::vector<Event> g_shots;     /* one-shots, by tick */
std::vector<Event> g_repeats;
unsigned long g_longest = 1;
bool   g_loaded = false;

/* Only the keys a trace script plausibly needs.  Anything else in the
 * script is reported rather than ignored, so a typo fails loudly. */
struct Named { const char *name; int offset; };
#define K(f) { #f, (int)(size_t)&(((KEY_STRUCT*)0)->f) }
/* The number row is n0..n9 in KEY_STRUCT but "num0".."num9" in a script, so
 * it needs the spelling given rather than taken from the field name.  These
 * are what AVG_ControlSelectWindow reads to pick a choice:
 *     if(GameKey.num[j]){ select = j-1; click = 1; }
 * and GameKey.num[j] is KeyCond.trg.kJ || KeyCond.trg.nJ.  Answering a choice
 * any other way means knowing where its text landed. */
#define KN(s, f) { s, (int)(size_t)&(((KEY_STRUCT*)0)->f) }
const Named g_keys[] = {
    K(enter), K(space), K(esc), K(bs), K(ctrl), K(shift), K(alt),
    K(up), K(down), K(left), K(right),
    K(pup), K(pdown), K(home), K(end),
    KN("num0", n0), KN("num1", n1), KN("num2", n2), KN("num3", n3),
    KN("num4", n4), KN("num5", n5), KN("num6", n6), KN("num7", n7),
    KN("num8", n8), KN("num9", n9),
};
#undef K
#undef KN

bool by_tick(const Event &a, const Event &b) { return a.tick < b.tick; }

void load(void)
{
    g_loaded = true;
    const char *path = getenv("TH2REF_INPUT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "th2ref: cannot open input script %s\n", path);
        return;
    }
    char line[512];
    size_t count = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;

        /* "... until <tick>" ends a repeat; cut off first, so the optional
         * hold before it still reads as the last number. */
        unsigned long until = 0;
        for (char *p = line; *p; ++p) {
            if (_strnicmp(p, "until", 5) == 0 && (p == line || p[-1] == ' ')) {
                until = strtoul(p + 5, NULL, 10);
                *p = 0;
                break;
            }
        }

        unsigned long tick = 0, hold = 1, period = 0;
        char name[64] = {0};
        int a = 0, b = 0;
        int n = sscanf(line, "%lu %63s %d %d", &tick, name, &a, &b);
        if (n < 2) continue;

        /* "<first> every <period> <what> [hold]" */
        if (!_stricmp(name, "every")) {
            char word[64] = {0};
            unsigned long pd = 0;
            int m = sscanf(line, "%*lu %*63s %lu %63s %d %d", &pd, word, &a, &b);
            if (m < 2 || pd == 0) {
                fprintf(stderr, "th2ref: bad 'every' in input script\n");
                continue;
            }
            if (!_stricmp(word, "move") || !_stricmp(word, "every")) {
                fprintf(stderr, "th2ref: '%s' cannot repeat\n", word);
                continue;
            }
            period = pd;
            strcpy(name, word);
            n = m;              /* args after the name, counted the same way */
        }
        if (until && !period) {
            fprintf(stderr, "th2ref: until only ends a repeat\n");
            continue;
        }

        Event e;
        memset(&e, 0, sizeof e);
        e.tick = tick;
        e.period = period;
        e.until = until;

        /* "mappick": stand on a map destination, whichever page it is on.
         *
         * A map is answered by the pointer being inside a destination's rect
         * when a click lands - AVG_ControlMapEvent reads MUS_GetMouseNo and
         * GameKey.click, and unlike AVG_ControlSelectWindow it has no number
         * key path.  The rects are per scene (MapEventCharPos[pos], offset
         * again when destinations overlap) and only the ones on the displayed
         * page are enabled, so no fixed coordinate answers every map in the
         * game.  Rather than carry a table of them, ask the engine: stand on
         * the first enabled destination, or on the page-forward arrow when
         * this page has none, and let the periodic click do the rest. */
        if (!_stricmp(name, "mappick")) {
            e.kind = EV_MAPPICK;
            e.hold = 1;
            if (!e.period) e.period = 1;
            g_repeats.push_back(e);
            ++count;
            continue;
        }
        if (!_stricmp(name, "move") && n >= 4) {
            e.kind = EV_MOVE; e.x = a; e.y = b; e.hold = 1; e.period = 0;
            g_moves.push_back(e);
            ++count;
            continue;
        }
        if (!_stricmp(name, "lclick") || !_stricmp(name, "rclick")) {
            e.kind = name[0] == 'l' || name[0] == 'L' ? EV_LCLICK : EV_RCLICK;
            e.hold = (n >= 3 && a > 0) ? (unsigned long)a : 2;
        } else {
            hold = (n >= 3 && a > 0) ? (unsigned long)a : 1;
            int offset = -1;
            for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; ++i) {
                if (!_stricmp(g_keys[i].name, name)) { offset = g_keys[i].offset; break; }
            }
            if (offset < 0) {
                fprintf(stderr, "th2ref: unknown key '%s' in input script\n", name);
                continue;
            }
            e.kind = EV_KEY;
            e.offset = offset;
            e.hold = hold ? hold : 1;
        }
        if (period && e.hold > period) {
            fprintf(stderr, "th2ref: repeat holds longer than its period\n");
            continue;
        }
        if (period) {
            g_repeats.push_back(e);
        } else {
            g_shots.push_back(e);
            if (e.hold > g_longest) g_longest = e.hold;
        }
        ++count;
    }
    fclose(f);
    /* Stable, so two moves on one tick keep their order and the later line
     * wins, as it always has. */
    std::stable_sort(g_moves.begin(), g_moves.end(), by_tick);
    std::stable_sort(g_shots.begin(), g_shots.end(), by_tick);
    fprintf(stderr, "th2ref: %u input events loaded\n", (unsigned)count);
}

}  /* namespace */

static void th2ref_map_pick(void);

extern "C" void th2ref_input(void)
{
    if (!g_loaded) load();

    /* Full control: whatever the devices are doing, the engine sees only
     * what the script says. */
    ZeroMemory(&KeyCond, sizeof(KeyCond));

    const unsigned long now = th2ref_current_tick();
    char *btn  = (char*)&KeyCond.btn;
    char *trg  = (char*)&KeyCond.trg;
    char *btrg = (char*)&KeyCond.btrg;

    g_mouse_l = g_mouse_r = false;
    int map_pick = 0;

    /* a move is a state change: the latest one at or before now holds */
    {
        Event probe; memset(&probe, 0, sizeof probe); probe.tick = now;
        std::vector<Event>::const_iterator it = std::upper_bound(
            g_moves.begin(), g_moves.end(), probe, by_tick);
        if (it != g_moves.begin()) {
            --it;
            g_mouse_x = it->x;
            g_mouse_y = it->y;
        }
    }

    const auto apply = [&](const Event &e, bool first) {
        switch (e.kind) {
        case EV_MAPPICK: map_pick = 1; break;
        case EV_LCLICK: g_mouse_l = true; break;
        case EV_RCLICK: g_mouse_r = true; break;
        case EV_KEY:
            btn[e.offset] = 1;                   /* level, for the whole hold */
            if (first) {                         /* edge, only on the first */
                trg[e.offset]  = 1;
                btrg[e.offset] = 1;
            }
            break;
        default: break;
        }
    };

    /* one-shots in effect started within the longest hold of now */
    {
        Event probe; memset(&probe, 0, sizeof probe);
        probe.tick = now + 1 > g_longest ? now + 1 - g_longest : 0;
        std::vector<Event>::const_iterator it = std::lower_bound(
            g_shots.begin(), g_shots.end(), probe, by_tick);
        for (; it != g_shots.end() && it->tick <= now; ++it) {
            if (now - it->tick < it->hold) apply(*it, now == it->tick);
        }
    }

    /* repeats: phase within a period, from the first tick until `until` */
    for (size_t i = 0; i < g_repeats.size(); ++i) {
        const Event &e = g_repeats[i];
        if (now < e.tick || (e.until && now >= e.until)) continue;
        const unsigned long phase = (now - e.tick) % e.period;
        if (phase < e.hold) apply(e, phase == 0);
    }
    if (map_pick) th2ref_map_pick();

    {
        static FILE *wlog = NULL;
        static int wopen = 0;
        if (!wopen) {
            wopen = 1;
            const char *path = getenv("TH2REF_WEATHER_LOG");
            if (path && *path) wlog = fopen(path, "w");
        }
        if (wlog && Weather.flag) {
            fprintf(wlog, "%lu n%d c%lu", now, Weather.amount, Weather.cnt);
            for (int k = 0; k < 3 && k < Weather.amount; ++k) {
                const TH2REF_WOBJ *o = &Weather.obj[k];
                unsigned int bx, by;
                memcpy(&bx, &o->x, 4); memcpy(&by, &o->y, 4);
                fprintf(wlog, " [t%d x%.4f y%.4f %08x %08x k%lu]", o->type, o->x, o->y, bx, by, o->cnt);
            }
            fputc('\n', wlog);
            fflush(wlog);
        }
    }

    /* TH2REF_MAP_LOG: the map's mouse, a line a frame while its layer is up.
     * `no` is MouseStruct.no as MUS_RenewMouse left it last frame - the
     * select that frame's AVG_ControlMapEvent read - and the flags are the
     * rects as that frame's step 3 left them. */
    static FILE *maplog = NULL;
    static int maplog_open = 0;
    if (!maplog_open) {
        maplog_open = 1;
        const char *path = getenv("TH2REF_MAP_LOG");
        if (path && *path) maplog = fopen(path, "w");
    }
    if (maplog && MUS_GetMouseLayer() == 10) {
        fprintf(maplog, "%lu mouse=%d,%d prev_no=%d flags=", now, g_mouse_x,
                g_mouse_y, MUS_GetMouseNo(-1));
        for (int n = 0; n < 18; ++n)
            fputc(MUS_GetMouseRectFlag(10, n) ? '1' : '0', maplog);
        /* the sprites as SPR_RenewSprite left them last frame */
        for (int j = 0; j < ANIME_CONTROL_MAX; ++j) {
            const ANIME_CONTROL *ac = &AnimeControl[j];
            if (!ac->flag) continue;
            fprintf(maplog, " ac%d:a%d,c%ld,n%ld,d%ld,m%ld", j, (int)(unsigned char)ac->ano,
                    ac->code_no, ac->count, ac->draw_count, ac->mode);
        }
        fputc('\n', maplog);
        fflush(maplog);
    }
}

/* Layer 10 is the map's own; AVG_ControlMapEvent sets it with
 * MUS_SetMouseLayer(10) and puts it back when it leaves, so this is inert
 * anywhere else in the game and cannot move the pointer during ordinary
 * scenes.  Rects 0..15 are the destinations, 16 and 17 the page arrows. */
static void th2ref_map_pick(void)
{
    if (MUS_GetMouseLayer() != 10) return;
    for (int n = 0; n < 16; ++n) {
        int sx, sy, w, h;
        if (!MUS_GetMouseRectFlag(10, n)) continue;
        if (!MUS_GetMouseRect(10, n, &sx, &sy, &w, &h)) continue;
        if (w <= 0 || h <= 0) continue;
        g_mouse_x = sx + w / 2;
        g_mouse_y = sy + h / 2;
        return;
    }
    /* Nothing selectable here: stand on the forward arrow and let the next
     * click turn the page. */
    int sx, sy, w, h;
    if (MUS_GetMouseRect(10, 17, &sx, &sy, &w, &h) && w > 0 && h > 0) {
        g_mouse_x = sx + w / 2;
        g_mouse_y = sy + h / 2;
    }
}

extern "C" int th2ref_mouse_level(void) { return g_mouse_l ? 1 : 0; }

extern "C" int th2ref_cursor_pos(POINT *p)
{
    if (p) { p->x = g_mouse_x; p->y = g_mouse_y; }
    return 1;
}

/* SetCursorPos, for MUS_SetMousePosRect: the pointer stays where the engine
 * put it until the script next moves it - the port does the same. */
extern "C" int th2ref_set_cursor_pos(int x, int y)
{
    g_mouse_x = x;
    g_mouse_y = y;
    return 1;
}

extern "C" short th2ref_async_key(int vk)
{
    /* 0x8001 is what the engine tests for: `GetAsyncKeyState(k)&0x8001` */
    if (vk == VK_LBUTTON) return g_mouse_l ? (short)0x8001 : 0;
    if (vk == VK_RBUTTON) return g_mouse_r ? (short)0x8001 : 0;
    return 0;
}
