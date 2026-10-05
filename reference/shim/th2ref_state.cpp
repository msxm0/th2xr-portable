/* The state trace for the reference build.
 *
 * Pixels tell you two runs disagree; they don't tell you why, and a screen
 * that is black for two hundred frames tells you nothing at all.  This
 * writes one line per tick naming the handful of integers the whole AVG
 * machine turns on - the program counter, the message state machine, the
 * background fade counter, the half tone - so a divergence can be read off
 * as "their fd_cnt is 40/120 and ours is 0" rather than guessed at from a
 * PSNR curve.
 *
 * Our side writes the same columns from the same fields (src/trace.cpp), so
 * the two files diff directly.
 *
 * This is the one shim translation unit that includes the engine's own
 * headers: the fields have to be read out of the real structures, at the
 * real offsets, or the trace would be describing something else.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mm_std.h"
#include "main.h"
#include "escript.h"
#include "GM_Avg.h"
#include "GM_avgBack.h"
#include "GM_avgMsg.h"
#include "disp.h"
#include "mouse.h"

#include "th2ref_hooks.h"

static FILE *g_state = 0;
static int   g_tried = 0;

/* One character per glyph: alph2/16, which is 0..16, in base 36 - so a fully
 * faded-in character is 'g' and an invisible one '0'.  Compact enough to sit
 * in a per-tick line and to diff by eye.
 *
 * A frame draws several text objects and only TXT_WINDOW - the message - is
 * worth recording.  DrawGraphText reports the slot it is about to draw, which
 * is the only identification that actually holds: comparing str against
 * NovelMessage.str never matches (the engine does not hand that buffer
 * through), and "keep the longest one this tick" silently interleaves three
 * different objects into one column, which reads like a bug that is not
 * there and hides one that is.
 */
#define TH2REF_TEXT_MAX 1024
static char g_text[TH2REF_TEXT_MAX + 1];
static int  g_text_len = 0;
static int  g_slot = -1;
static int  g_slot_fresh = 0;
static int  g_recording = 0;
static int  g_txt_cnt = 0;
static int  g_txt_step = 0;

extern "C" void th2ref_text_slot(int slot)
{
    /* Only DrawGraphText calls this, and it calls it immediately before the
     * TXT_DrawTextEx that draws the object.  The freshness flag is what
     * separates those from the measuring calls: DSP_GetTextDispH and friends
     * go straight to TXT_DrawTextEx with text_cnt and step_cnt forced to -1,
     * bypassing DrawGraphText entirely.  Recording those as if they were
     * draws reported ts->cnt and ts->step as -1 when they were nothing of
     * the kind, and mixed their glyphs in with the real ones. */
    g_slot = slot;
    g_slot_fresh = 1;
}

extern "C" void th2ref_text_begin(int text_cnt, int step_cnt)
{
    g_recording = (g_slot_fresh && g_slot == TXT_WINDOW);
    g_slot_fresh = 0;
    if (g_recording) {
        g_text_len = 0;
        g_txt_cnt = text_cnt;
        g_txt_step = step_cnt;
    }
}

extern "C" void th2ref_text_glyph(int alph2)
{
    if (!g_recording || g_text_len >= TH2REF_TEXT_MAX) return;
    int v = alph2 / 16;
    if (v < 0) v = 0;
    if (v > 16) v = 16;
    g_text[g_text_len++] = (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

/* ---------------------------------------------------------------- checkpoints
 *
 * Without these the harness is quadratic: every 300 tick window replays the
 * whole game from the title to reach its own start, so a sweep to 6000 ticks
 * costs 63000 and watching it means watching the opening twenty times.  With
 * them each window resumes from the one before it and costs its own 300 plus
 * the lead-in.
 *
 * A checkpoint is the engine's own save - the script position, ESC_FlagBuf
 * and AVG_SAVE_DATA - written by SAV_TraceSave, which is SAV_Save without
 * the thumbnail capture that would disturb GRP_WORK.  What that save does
 * NOT carry is the clock: GlobalCount, and the harness's own tick.  Those go
 * in a sidecar, because a resumed run has to answer to the same absolute
 * tick numbers as the straight-through run it is standing in for, or the
 * scripted input fires at the wrong moments and nothing lines up.
 *
 * Whether the engine's save is a faithful checkpoint at all is not something
 * to take on trust - SAV_Load reconstructs the scene rather than restoring
 * it, and anything the reconstruction does not reach comes back wrong.  That
 * is what loop.sh's --verify-checkpoint pass is for: run straight through,
 * run again resumed, and require the two state traces to agree tick for tick
 * before any checkpoint is used for real work.
 */
void SAV_TraceSave( const char *fname );
int  SAV_TraceLoad( const char *fname );

static long getenv_long(const char *name, long dflt)
{
    const char *v = getenv(name);
    return (v && *v) ? atol(v) : dflt;
}

/* Written next to the .sav.  Text, one line, so a stale or truncated one
 * fails to parse rather than resuming at a plausible-looking wrong tick. */
static void checkpoint_write_meta(const char *sav, unsigned long tick)
{
    char path[512];
    _snprintf(path, sizeof(path), "%s.meta", sav);
    path[sizeof(path) - 1] = 0;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%lu %d %d\n", tick, GlobalCount, GlobalCount2);
    fclose(f);
}

static int checkpoint_read_meta(const char *sav, unsigned long *tick,
                                int *gc, int *gc2)
{
    char path[512];
    _snprintf(path, sizeof(path), "%s.meta", sav);
    path[sizeof(path) - 1] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long t = 0;
    int a = 0, b = 0;
    int got = fscanf(f, "%lu %d %d", &t, &a, &b);
    fclose(f);
    if (got != 3) return 0;
    *tick = t; *gc = a; *gc2 = b;
    return 1;
}

/* Called at the top of every state line, before the line is written, so the
 * first line a resumed run emits describes the loaded state and can be
 * compared directly with the same tick of a straight-through run. */
static void checkpoint_save_message(const char *file);
static void checkpoint_load_message(const char *file);

static void checkpoint_resume(void)
{
    static int done = -1;
    static const char *file = 0;
    static long trigger = 0;

    if (done < 0) {
        file = getenv("TH2REF_RESUME_FILE");
        if (!file || !*file) { done = 1; return; }
        /* The tick to load at.  The engine has to be past the title and into
         * the opening script for AVG_LoadWindow to have a window to rebuild,
         * and that is exactly what the lead-in the harness already runs is
         * for, so the default is one tick past it. */
        trigger = getenv_long("TH2REF_RESUME_TRIGGER", 236);
        done = 0;
        th2ref_hold_dump(1);
    }
    if (done) return;
    if ((long)th2ref_current_tick() < trigger) return;

    unsigned long tick = 0;
    int gc = 0, gc2 = 0;
    if (!checkpoint_read_meta(file, &tick, &gc, &gc2)) {
        fprintf(stderr, "th2ref: no checkpoint sidecar for %s\n", file);
        exit(2);
    }
    if (!SAV_TraceLoad(file)) {
        fprintf(stderr, "th2ref: cannot read checkpoint %s\n", file);
        exit(2);
    }
    /* After SAV_TraceLoad, which would otherwise overwrite it. */
    checkpoint_load_message(file);
    GlobalCount  = gc;
    GlobalCount2 = gc2;
    th2ref_set_tick(tick);
    th2ref_hold_dump(0);
    done = 1;
}

/* The message machine, saved beside the checkpoint.
 *
 * SAV_TraceSave is SAV_Save without the thumbnail, and the engine's save is
 * not a snapshot: AVG_SAVE_DATA carries ms_step1/ms_count/ms_kstep and the
 * load rebuilds the rest, so a resumed run lands near where the save was
 * taken rather than on it.  Ours lands somewhere else again, and the two
 * sides then drift apart and re-sync for the whole window - which is not a
 * difference between the engines at all, only between their saves.  Both
 * sides now restore the machine exactly, so a resumed run continues a linear
 * one instead of merely resembling it.
 *
 * NOVEL_MESSEGE and the half tone struct are plain data - no pointers - so
 * they survive being written by one process and read by another.  Anything
 * holding a pointer would not, which is why this is not simply a memcpy of
 * every global the state dump reads. */
static void snap_path(const char *file, char *out, size_t n)
{
    snprintf(out, n, "%s.msg", file);
}

extern "C" int th2ref_eopr_size(void);
extern "C" void *th2ref_eopr_data(void);

static void checkpoint_save_message(const char *file)
{
    char path[512];
    snap_path(file, path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "th2ref: cannot write %s\n", path);
        return;
    }
    fwrite(&NovelMessage, sizeof NovelMessage, 1, f);
    fwrite(&HalfTone, sizeof HalfTone, 1, f);
    /* EOprFlag: without it the resumed run re-enters the opcode it was
     * parked on and runs its set-up again, which throws the restored message
     * away and starts the next one. */
    {
        const int n = th2ref_eopr_size();
        fwrite(th2ref_eopr_data(), 1, (size_t)n, f);
    }
    fclose(f);
}

static void checkpoint_load_message(const char *file)
{
    char path[512];
    snap_path(file, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "th2ref: no %s - checkpoint predates the message"
                        " snapshot\n", path);
        exit(2);
    }
    if (fread(&NovelMessage, sizeof NovelMessage, 1, f) != 1
        || fread(&HalfTone, sizeof HalfTone, 1, f) != 1) {
        fprintf(stderr, "th2ref: short read on %s\n", path);
        exit(2);
    }
    {
        const int n = th2ref_eopr_size();
        if (fread(th2ref_eopr_data(), 1, (size_t)n, f) != (size_t)n) {
            fprintf(stderr, "th2ref: short read on %s (EOprFlag)\n", path);
            exit(2);
        }
    }
    fclose(f);
}

static void checkpoint_save(void)
{
    static int done = -1;
    static const char *file = 0;
    static long at = 0;

    if (done < 0) {
        file = getenv("TH2REF_SAVE_FILE");
        if (!file || !*file) { done = 1; return; }
        at = getenv_long("TH2REF_SAVE_AT", -1);
        done = (at < 0) ? 1 : 0;
    }
    if (done) return;
    if ((long)th2ref_current_tick() != at) return;

    SAV_TraceSave(file);
    checkpoint_save_message(file);
    checkpoint_write_meta(file, th2ref_current_tick());
    done = 1;
}

/* Hold the reference at a window boundary without killing it.
 *
 * The engine's save is not a snapshot, and no amount of injecting state makes
 * a resumed reference match a straight-through one: its control pass
 * reconciles the message against restored graphics a frame later and lands on
 * the save's reduced state whatever it was handed.  So the reference is never
 * resumed at all.  It runs once, in order, and simply waits here while our
 * side is rebuilt and re-run against the trace it has already written - which
 * is fixed, because the reference does not change.  TH2REF_PAUSE_FILE holds
 * the tick to run to; writing a larger number into it lets the run continue.
 */
static void pause_at_window(void)
{
    static long target = -2;
    static const char *file = 0;
    if (target == -2) {
        file = getenv("TH2REF_PAUSE_FILE");
        target = (file && *file) ? getenv_long("TH2REF_PAUSE_AT", -1) : -1;
    }
    if (target < 0) return;
    while ((long)th2ref_current_tick() >= target) {
        FILE *f = fopen(file, "r");
        long want = -1;
        if (f) {
            if (fscanf(f, "%ld", &want) != 1) want = -1;
            fclose(f);
        }
        if (want > target) { target = want; return; }
        Sleep(50);
    }
}

/* Per-tick click probe.  The earlier version hung off AVG_GetHitKey, which
 * sits behind a short-circuit and so is not called every frame - a missing
 * line meant "no click" OR "not polled" and could not tell them apart.  This
 * runs once per tick from the dump, so a missing line means no click. */
static void note_click(void)
{
    static FILE *log = NULL;
    static int   tried = 0;
    static unsigned long lo = 0, hi = 0;
    if (!tried) {
        const char *path = getenv("TH2REF_CLICK_LOG");
        const char *rng  = getenv("TH2REF_CLICK_RANGE");
        tried = 1;
        if (rng && *rng) sscanf(rng, "%lu:%lu", &lo, &hi);
        if (path && *path) log = fopen(path, "w");
    }
    if (!log) return;
    const unsigned long now = th2ref_current_tick();
    /* Inside the window of interest log every tick, so a missing click is
     * distinguishable from a tick that was never examined; outside it, only
     * the clicks, to keep the file small. */
    const int inside = (hi && now >= lo && now <= hi);
    if (!inside && !GameKey.click) return;
    fprintf(log, "%lu click=%d level=%d no=%d\n", now,
            GameKey.click, th2ref_mouse_level(), MUS_GetMouseNo(-1));
    fflush(log);
}

extern "C" void th2ref_dump_state(void)
{
    th2ref_tick_done(MainWindow.draw_flag);
    checkpoint_resume();
    pause_at_window();
    note_click();
    if (!g_tried) {
        g_tried = 1;
        const char *path = getenv("TH2REF_STATE");
        if (path && *path) {
            g_state = fopen(path, "w");
            if (g_state) {
                /* Column names on line one, so the file reads on its own and
                 * a diff points at a field rather than an ordinal. */
                fprintf(g_state,
                        "tick script pc "
                        "msg_flag msg_disp msg_step1 msg_step2 msg_count "
                        "msg_kstep msg_max "
                        "tone_tstep tone_tcount "
                        "bk_bno bk_fd_flag bk_fd_type bk_fd_cnt bk_fd_max "
                        "bk_sc_flag bk_sc_cnt bk_sc_max "
                        "bk_sk_flag bk_sk_cnt "
                        "bk_br_flag bk_br_cnt "
                        "avg_msg_cut avg_auto avg_frame avg_wait avg_level "
                        "avg_msg_wait avg_msg_page avg_half_tone "
                        "txt_cnt txt_step global_count text\n");
            }
        }
    }
    if (!g_state) return;
    /* A resumed run's lead-in ticks are bookkeeping, not the run: they carry
     * raw tick numbers from the title screen and writing them would leave the
     * trace with two disjoint tick ranges in one file. */
    if (th2ref_dump_held()) return;

    const DWORD pc = EXEC_LangInfo ? EXEC_LangInfo->pc : 0;

    fprintf(g_state,
            "%lu %s %lu "
            "%d %d %d %d %d %d %d "
            "%d %d "
            "%d %d %d %d %d "
            "%d %d %d "
            "%d %d "
            "%d %d "
            "%ld %d %ld %ld %ld %ld %ld %ld %d %d %d %s\n",
            th2ref_current_tick(),
            NowLangFileName[0] ? NowLangFileName : "-",
            (unsigned long)pc,
            (int)NovelMessage.flag, (int)NovelMessage.disp,
            (int)NovelMessage.step1, (int)NovelMessage.step2,
            (int)NovelMessage.count, (int)NovelMessage.kstep,
            (int)NovelMessage.max,
            HalfTone.tstep, HalfTone.tcount,
            BackStruct.bno,
            BackStruct.fd_flag, BackStruct.fd_type,
            BackStruct.fd_cnt, BackStruct.fd_max,
            BackStruct.sc_flag, BackStruct.sc_cnt, BackStruct.sc_max,
            BackStruct.sk_flag, BackStruct.sk_cnt,
            BackStruct.br_flag, BackStruct.br_cnt,
            Avg.msg_cut, Avg.auto_flag, Avg.frame,
            Avg.wait, Avg.level, Avg.msg_wait, Avg.msg_page, Avg.half_tone,
            g_txt_cnt, g_txt_step, GlobalCount,
            (g_text[g_text_len] = 0, g_text_len ? g_text : "-"));
    /* Reset for the next tick: a frame that draws no message must report
     * none, not last frame's. */
    g_text_len = 0;
    fflush(g_state);

    /* One-shot dump of the message source; see the note on our side. */
    {
        const char *at = getenv("TH2REF_STRTICK");
        if (at && th2ref_current_tick() == (unsigned long)atol(at)) {
            const char *path = getenv("TH2REF_STRFILE");
            char *str = DSP_GetTextStr(TXT_WINDOW);
            if (path && str) {
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(str, 1, strlen(str), f); fclose(f); }
            }
        }
    }

    /* After the line, so the checkpoint and the last state line it can be
     * checked against describe the same instant. */
    checkpoint_save();
}
