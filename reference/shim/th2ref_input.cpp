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

extern "C" unsigned long th2ref_current_tick(void);

/* Scripted pointer.  MUS_RenewMouse reads the real device three ways -
 * GetCursorPos, ScreenToClient and GetAsyncKeyState - and all three are
 * redirected here by the prelude, so the function itself runs untouched and
 * all of its rect and trigger bookkeeping still happens. */
static int  g_mouse_x = 0, g_mouse_y = 0;
static bool g_mouse_l = false, g_mouse_r = false;

namespace {

enum Kind { EV_KEY, EV_MOVE, EV_LCLICK, EV_RCLICK };

struct Event {
    unsigned long tick;
    Kind          kind;
    int           offset;   /* EV_KEY: byte offset inside KEY_STRUCT */
    int           x, y;     /* EV_MOVE */
    unsigned long hold;
};

const int MAX_EVENTS = 4096;
Event  g_events[MAX_EVENTS];
int    g_count  = 0;
bool   g_loaded = false;

/* Only the keys a trace script plausibly needs.  Anything else in the
 * script is reported rather than ignored, so a typo fails loudly. */
struct Named { const char *name; int offset; };
#define K(f) { #f, (int)(size_t)&(((KEY_STRUCT*)0)->f) }
const Named g_keys[] = {
    K(enter), K(space), K(esc), K(bs), K(ctrl), K(shift), K(alt),
    K(up), K(down), K(left), K(right),
    K(pup), K(pdown), K(home), K(end),
};
#undef K

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
    while (fgets(line, sizeof line, f) && g_count < MAX_EVENTS) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        unsigned long tick = 0, hold = 1;
        char name[64] = {0};
        int a = 0, b = 0;
        int n = sscanf(line, "%lu %63s %d %d", &tick, name, &a, &b);
        if (n < 2) continue;

        if (!_stricmp(name, "move") && n >= 4) {
            g_events[g_count].tick = tick; g_events[g_count].kind = EV_MOVE;
            g_events[g_count].x = a; g_events[g_count].y = b;
            g_events[g_count].hold = 1; ++g_count;
            continue;
        }
        if (!_stricmp(name, "lclick") || !_stricmp(name, "rclick")) {
            g_events[g_count].tick = tick;
            g_events[g_count].kind = name[0] == 'l' || name[0] == 'L' ? EV_LCLICK : EV_RCLICK;
            g_events[g_count].hold = (n >= 3 && a > 0) ? (unsigned long)a : 2;
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
        ++g_count;
    }
    fclose(f);
    fprintf(stderr, "th2ref: %d input events loaded\n", g_count);
}

}  /* namespace */

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

    for (int i = 0; i < g_count; ++i) {
        const Event &e = g_events[i];
        /* a move is a state change, so it applies from its tick onwards */
        if (e.kind == EV_MOVE) {
            if (now >= e.tick) { g_mouse_x = e.x; g_mouse_y = e.y; }
            continue;
        }
        if (now < e.tick || now >= e.tick + e.hold) continue;
        switch (e.kind) {
        case EV_LCLICK: g_mouse_l = true; break;
        case EV_RCLICK: g_mouse_r = true; break;
        case EV_KEY:
            btn[e.offset] = 1;                   /* level, for the whole hold */
            if (now == e.tick) {                 /* edge, only on the first */
                trg[e.offset]  = 1;
                btrg[e.offset] = 1;
            }
            break;
        default: break;
        }
    }
}

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
