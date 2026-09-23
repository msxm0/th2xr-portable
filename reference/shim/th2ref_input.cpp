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
 *
 * A route is hundreds of thousands of ticks of clicking, which as one line
 * per click overruns the table below and makes every tick scan every event.
 * `every` is one event whose firing is arithmetic, so neither happens.
 *
 * A press sets both trg (the edge) and btn (the level) for its duration,
 * and btrg follows trg, which is what KEY_RenewKeybord would do for a key
 * held for one frame.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
};

const int MAX_EVENTS = 4096;
Event  g_events[MAX_EVENTS];
int    g_count  = 0;
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
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
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

        /* Loud, because a script that is silently half-loaded still runs:
         * the reference simply stops clicking, the port does not, and the
         * divergence looks like an engine bug rather than a full table. */
        if (g_count >= MAX_EVENTS) {
            fprintf(stderr, "th2ref: more than %d input events - use 'every'\n",
                    MAX_EVENTS);
            break;
        }

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
            g_events[g_count].tick = tick;
            g_events[g_count].kind = EV_MAPPICK;
            g_events[g_count].hold = 1;
            g_events[g_count].period = period ? period : 1;
            ++g_count;
            continue;
        }
        if (!_stricmp(name, "move") && n >= 4) {
            g_events[g_count].tick = tick; g_events[g_count].kind = EV_MOVE;
            g_events[g_count].x = a; g_events[g_count].y = b;
            g_events[g_count].hold = 1; g_events[g_count].period = 0;
            ++g_count;
            continue;
        }
        if (!_stricmp(name, "lclick") || !_stricmp(name, "rclick")) {
            g_events[g_count].tick = tick;
            g_events[g_count].kind = name[0] == 'l' || name[0] == 'L' ? EV_LCLICK : EV_RCLICK;
            g_events[g_count].hold = (n >= 3 && a > 0) ? (unsigned long)a : 2;
            g_events[g_count].period = period;
            if (period && g_events[g_count].hold > period) {
                fprintf(stderr, "th2ref: repeat holds longer than its period\n");
                continue;
            }
            ++g_count;
            continue;
        }
        hold = (n >= 3 && a > 0) ? (unsigned long)a : 1;
        int offset = -1;
        for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; ++i) {
            if (!_stricmp(g_keys[i].name, name)) { offset = g_keys[i].offset; break; }
        }
        if (offset < 0) {
            fprintf(stderr, "th2ref: unknown key '%s' in input script\n", name);
            continue;
        }
        g_events[g_count].tick   = tick;
        g_events[g_count].kind   = EV_KEY;
        g_events[g_count].offset = offset;
        g_events[g_count].hold   = hold ? hold : 1;
        g_events[g_count].period = period;
        if (period && g_events[g_count].hold > period) {
            fprintf(stderr, "th2ref: repeat holds longer than its period\n");
            continue;
        }
        ++g_count;
    }
    fclose(f);
    fprintf(stderr, "th2ref: %d input events loaded\n", g_count);
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

    for (int i = 0; i < g_count; ++i) {
        const Event &e = g_events[i];
        /* a move is a state change, so it applies from its tick onwards */
        if (e.kind == EV_MOVE) {
            if (now >= e.tick) { g_mouse_x = e.x; g_mouse_y = e.y; }
            continue;
        }
        if (now < e.tick) continue;
        /* one-shot: distance from its tick; repeat: phase within a period */
        const unsigned long phase = e.period ? (now - e.tick) % e.period
                                             : now - e.tick;
        if (phase >= e.hold) continue;
        if (e.kind == EV_MAPPICK) { map_pick = 1; continue; }
        switch (e.kind) {
        case EV_LCLICK: g_mouse_l = true; break;
        case EV_RCLICK: g_mouse_r = true; break;
        case EV_KEY:
            btn[e.offset] = 1;                   /* level, for the whole hold */
            if (phase == 0) {                    /* edge, only on the first */
                trg[e.offset]  = 1;
                btrg[e.offset] = 1;
            }
            break;
        default: break;
        }
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

extern "C" short th2ref_async_key(int vk)
{
    /* 0x8001 is what the engine tests for: `GetAsyncKeyState(k)&0x8001` */
    if (vk == VK_LBUTTON) return g_mouse_l ? (short)0x8001 : 0;
    if (vk == VK_RBUTTON) return g_mouse_r ? (short)0x8001 : 0;
    return 0;
}
