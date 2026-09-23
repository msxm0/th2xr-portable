/* Instrumentation for the reference build.
 *
 * Three things, all of them things the harness needs rather than things the
 * engine wanted: a virtual clock, a framebuffer dump, and (later) scripted
 * input.  Nothing here changes what the engine computes - it changes only
 * where time comes from and whether the result is written to disk.
 */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "th2ref_hooks.h"

/* Bmp.h's pixel formats.  Repeated here rather than included, because this
 * file must not drag in the engine's headers - the prelude redefines
 * timeGetTime and this is the one translation unit that has to call the
 * real one. */
typedef struct { unsigned char b, g, r, a; } TH2REF_RGB32;
typedef struct { unsigned char b, g, r;    } TH2REF_RGB24;
typedef struct { TH2REF_RGB32 *buf; int sx, sy; } TH2REF_BMP_T;
typedef struct { TH2REF_RGB24 *buf; int sx, sy; } TH2REF_BMP_F;
typedef struct { unsigned short *buf; char *alp; int sx, sy; } TH2REF_BMP_H;

static unsigned long g_tick = 0;
static const char   *g_dir  = 0;
static long          g_first = -1;   /* first tick to dump, -1 = none */
static long          g_last  = -1;

extern "C" {

unsigned long th2ref_current_tick(void) { return g_tick; }

/* Checkpoints move the clock.  A run resumed from a save at tick 3000 spends
 * its first couple of hundred ticks getting the engine as far as the opening
 * script - the lead-in - and then loads, at which point it has to start
 * calling itself tick 3000 or every absolute schedule the harness keeps
 * (scripted input, the dump window, the state trace) would be off by the
 * length of the lead-in.  g_hold keeps those lead-in ticks from writing
 * frames of their own on the way past. */
static int g_hold = 0;

void th2ref_set_tick(unsigned long tick) { g_tick = tick; }
void th2ref_hold_dump(int hold)          { g_hold = hold; }
int  th2ref_dump_held(void)              { return g_hold; }

int th2ref_text_hidden(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("TH2REF_NOTEXT");
        cached = (v && *v && *v != '0') ? 1 : 0;
    }
    return cached;
}

unsigned long th2ref_time(void)
{
    /* Exactly the step MAIN_Loop adds to next_time (1000/frame, integer
     * divided, = 16 at 60fps).  Matching it is what keeps `skip` at zero so
     * no draw is ever dropped - see th2ref_hooks.h. */
    return g_tick * (1000UL / 60UL);
}

void th2ref_advance_tick(void)
{
    if (!g_dir) {
        g_dir = getenv("TH2REF_DUMP");
        const char *a = getenv("TH2REF_FROM");
        const char *b = getenv("TH2REF_TO");
        g_first = a ? atol(a) : 0;
        g_last  = b ? atol(b) : -1;
        if (!g_dir) g_dir = "";
    }
    ++g_tick;
}

/* How long a stubbed sound or movie pretends to run.  One number for both,
 * because the point is not to be right about any particular asset - nothing
 * is being decoded - but to be the same number on every machine and every
 * run, so a wait costs a fixed count of ticks. */
static unsigned long pcm_ticks(void)
{
    static long cached = -1;
    if (cached < 0) {
        const char *v = getenv("TH2REF_PCM_TICKS");
        cached = (v && *v) ? atol(v) : 50;
        if (cached < 0) cached = 0;
    }
    return (unsigned long)cached;
}

/* Flat arrays rather than a map: handles are small indices into the engine's
 * own object table, and this file is compiled without the engine's headers. */
#define TH2REF_PCM_MAX 256
static unsigned long g_pcm_start[TH2REF_PCM_MAX];
static char          g_pcm_live[TH2REF_PCM_MAX];
static char          g_pcm_loop[TH2REF_PCM_MAX];
static unsigned long g_movie_start = 0;
static char          g_movie_live  = 0;

void th2ref_pcm_play(int handle, int repeat)
{
    if (handle < 0 || handle >= TH2REF_PCM_MAX) return;
    g_pcm_start[handle] = g_tick;
    g_pcm_live[handle]  = 1;
    /* ClSoundDS::play: 1 plays once, 0 asks DirectSound for DSBPLAY_LOOPING,
     * anything else runs the notify-thread path, which also plays once. */
    g_pcm_loop[handle]  = (repeat == 0);
}

void th2ref_pcm_stop(int handle)
{
    if (handle < 0 || handle >= TH2REF_PCM_MAX) return;
    g_pcm_live[handle] = 0;
}

int th2ref_pcm_status(int handle)
{
    if (handle < 0 || handle >= TH2REF_PCM_MAX) return 0;   /* PCM_STOP */
    if (!g_pcm_live[handle]) return 0;
    if (g_pcm_loop[handle]) return 1;                       /* PCM_PLAY */
    if (g_tick - g_pcm_start[handle] >= pcm_ticks()) {
        g_pcm_live[handle] = 0;
        return 0;
    }
    return 1;
}

void th2ref_note_hitkey(int click, int rect_no)
{
    static FILE *log = NULL;
    static int   tried = 0;
    if (!click) return;               /* only the frames a click lands on */
    if (!tried) {
        const char *path = getenv("TH2REF_HITKEY_LOG");
        tried = 1;
        if (path && *path) log = fopen(path, "w");
    }
    if (!log) return;
    fprintf(log, "%lu %d %d\n", g_tick, click, rect_no);
    fflush(log);
}

void th2ref_movie_start(void)
{
    g_movie_start = g_tick;
    g_movie_live  = 1;
}

/* Separate from the sound's, and defaulting to zero, because the two fail
 * differently: a sound that keeps playing is polled once a frame, while the
 * movie player decodes in a loop until the stream yields something other
 * than CONTINUE - and inside that loop the tick cannot advance, so a
 * tick-based "not finished yet" never becomes "finished".  Set
 * TH2REF_MOVIE_TICKS only if the player is verified to give the frame loop
 * back between calls. */
static unsigned long movie_ticks(void)
{
    static long cached = -1;
    if (cached < 0) {
        const char *v = getenv("TH2REF_MOVIE_TICKS");
        cached = (v && *v) ? atol(v) : 0;
        if (cached < 0) cached = 0;
    }
    return (unsigned long)cached;
}

int th2ref_movie_decoding(void)
{
    if (!g_movie_live) return 0;
    if (g_tick - g_movie_start >= movie_ticks()) {
        g_movie_live = 0;
        return 0;
    }
    return 1;
}

/* Frames go out delta-coded and deflated.
 *
 * Raw they are 800*600*3 = 1.37MB each, which caps how long a window can be
 * before it stops fitting on the disk: a 12000 tick run is 17GB a side, and
 * that cap is the reason the harness used to compare in 300 tick windows and
 * replay the whole game to reach each one.  Consecutive frames of a visual
 * novel differ in the few hundred glyphs the typewriter has just drawn, so
 * subtracting the previous frame leaves a mostly-zero buffer that deflates
 * about eight to one - measured 8.2x over 120 frames of a busy stretch,
 * which is what makes a 12000 tick window fit in about 2GB.
 *
 * The subtraction is per byte, mod 256, so it is exactly reversible and no
 * pixel is approximated anywhere: this is a storage format, not a codec, and
 * a comparison that tolerates two levels of difference must not be fed
 * anything that introduces any.
 *
 * Key frames carry the picture itself rather than a difference, and one is
 * written whenever there is no predecessor to subtract (the first tick of a
 * window, or after a gap) and every TH2REF_KEY_PERIOD ticks after that, so
 * asking for a single tick costs at most that many decodes rather than a
 * walk from the start of the run.
 */
#define TH2REF_KEY_PERIOD 120

static unsigned char *g_prev = 0;      /* last frame dumped, for the delta */
static size_t         g_prev_size = 0;
static long           g_prev_tick = -1;

static void write_frame(const char *path, const unsigned char *bgr,
                        int w, int h)
{
    const size_t n = (size_t)w * (size_t)h * 3;
    const int key = (g_prev_tick != (long)g_tick - 1) || !g_prev
                 || g_prev_size != n || (g_tick % TH2REF_KEY_PERIOD) == 0;

    unsigned char *payload = (unsigned char*)malloc(n);
    if (!payload) return;
    if (key) {
        memcpy(payload, bgr, n);
    } else {
        for (size_t i = 0; i < n; ++i) {
            payload[i] = (unsigned char)(bgr[i] - g_prev[i]);
        }
    }

    uLongf cap = compressBound((uLong)n);
    unsigned char *packed = (unsigned char*)malloc(cap);
    if (!packed) { free(payload); return; }
    /* Level 1.  The gain from here to level 6 is a few percent of a file
     * that is already a twentieth of the run's disk budget, and it is paid
     * for in wall clock on every one of twelve thousand frames. */
    if (compress2(packed, &cap, payload, (uLong)n, 1) != Z_OK) {
        free(packed); free(payload); return;
    }
    free(payload);

    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "TH2REFZ %d %d 24 %d %lu\n", w, h, key ? 1 : 0,
                (unsigned long)n);
        fwrite(packed, 1, (size_t)cap, f);
        fclose(f);
    }
    free(packed);

    if (g_prev_size != n) {
        free(g_prev);
        g_prev = (unsigned char*)malloc(n);
        g_prev_size = g_prev ? n : 0;
    }
    if (g_prev) {
        memcpy(g_prev, bgr, n);
        g_prev_tick = (long)g_tick;
    }
}

void th2ref_note_audio(const char *kind, int a, int b, int c, int d,
                       const char *name)
{
    static FILE *log = NULL;
    static int   tried = 0;
    if (!tried) {
        const char *path = getenv("TH2REF_AUDIO_LOG");
        tried = 1;
        if (path && *path) log = fopen(path, "w");
    }
    if (!log) return;
    fprintf(log, "%lu %s %d %d %d %d %s\n", g_tick, kind, a, b, c, d,
            name ? name : "-");
    fflush(log);
}

/* One CRC per frame.  Storing every frame of a route is a hundred gigabytes
 * and change; a checksum per tick is eight bytes, so the whole run can be
 * compared and only the neighbourhood of a mismatch needs real pictures. */
static FILE *frame_hash_file(void)
{
    static FILE *log = NULL;
    static int   tried = 0;
    if (!tried) {
        const char *path = getenv("TH2REF_FRAME_HASH");
        tried = 1;
        if (path && *path) log = fopen(path, "w");
    }
    return log;
}

void th2ref_dump_frame(void *vram, int draw_mode)
{
    if (!vram) return;
    if (g_hold) return;                 /* still in a resumed run's lead-in */
    /* The .binz range gates only the pictures now - the checksum is taken on
     * every frame, which is what makes a whole-route comparison affordable. */
    /* Storing every frame of a route is over a hundred gigabytes a side, so
     * the pictures are sampled - TH2REF_FRAME_STEP ticks apart - while the
     * checksum above still covers every frame. */
    static long step = -1, base = 0;
    if (step < 0) {
        const char *v = getenv("TH2REF_FRAME_STEP");
        const char *b = getenv("TH2REF_FRAME_BASE");
        step = (v && *v) ? atol(v) : 1;
        if (step < 1) step = 1;
        base = (b && *b) ? atol(b) : 0;
    }
    /* Sampled on the *port's* phase, not ours: this clock leads by the
     * title-screen lead-in, so sampling both at multiples of the step would
     * pick two sets of frames that never correspond.  TH2REF_FRAME_BASE is
     * that lead-in. */
    if ((((long)g_tick - base) % step) != 0) return;
    const int store = g_dir && g_dir[0]
                   && (long)g_tick >= g_first
                   && (g_last < 0 || (long)g_tick <= g_last);
    FILE *hash = frame_hash_file();
    if (!store && !hash) return;

    int w = 0, h = 0;
    switch (draw_mode) {
    case 32: w = ((TH2REF_BMP_T*)vram)->sx; h = ((TH2REF_BMP_T*)vram)->sy; break;
    case 24: w = ((TH2REF_BMP_F*)vram)->sx; h = ((TH2REF_BMP_F*)vram)->sy; break;
    case 16: w = ((TH2REF_BMP_H*)vram)->sx; h = ((TH2REF_BMP_H*)vram)->sy; break;
    default: return;
    }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return;

    unsigned char *out = (unsigned char*)malloc((size_t)w * h * 3);
    if (!out) return;

    for (int y = 0; y < h; ++y) {
        unsigned char *dst = out + (size_t)y * w * 3;
        if (draw_mode == 32) {
            const TH2REF_RGB32 *s = ((TH2REF_BMP_T*)vram)->buf + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
                dst[x*3+0] = s[x].b; dst[x*3+1] = s[x].g; dst[x*3+2] = s[x].r;
            }
        } else if (draw_mode == 24) {
            const TH2REF_RGB24 *s = ((TH2REF_BMP_F*)vram)->buf + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
                dst[x*3+0] = s[x].b; dst[x*3+1] = s[x].g; dst[x*3+2] = s[x].r;
            }
        } else {
            /* RGB565, widened so every dump is one format downstream. */
            const unsigned short *s = ((TH2REF_BMP_H*)vram)->buf + (size_t)y * w;
            for (int x = 0; x < w; ++x) {
                unsigned short p = s[x];
                dst[x*3+0] = (unsigned char)(( p        & 0x1f) << 3);
                dst[x*3+1] = (unsigned char)(((p >> 5)  & 0x3f) << 2);
                dst[x*3+2] = (unsigned char)(((p >> 11) & 0x1f) << 3);
            }
        }
    }

    /* Written under a temporary name and renamed into place: the harness
     * stops this process the moment the state trace reaches the last tick,
     * and a frame caught half written is a file the comparison then reads as
     * a short one and refuses.  Rename is atomic on the same directory, so a
     * .bin that exists is always complete. */
    if (hash) {
        const unsigned long sum =
            crc32(crc32(0L, Z_NULL, 0), out, (uInt)((size_t)w * h * 3));
        fprintf(hash, "%lu %08lx %d %d\n", g_tick, sum, w, h);
        fflush(hash);
    }
    if (store) {
        char path[512], temp[512];
        sprintf(temp, "%s/f%06lu.tmp", g_dir, g_tick);
        sprintf(path, "%s/f%06lu.binz", g_dir, g_tick);
        write_frame(temp, out, w, h);
        remove(path);
        rename(temp, path);
    }
    free(out);
}

}  /* extern "C" */
