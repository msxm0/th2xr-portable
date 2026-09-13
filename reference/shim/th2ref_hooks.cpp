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

/* Written as a headerless 24-bit BGR blob plus a one-line sidecar, so the
 * comparison tool needs no image library and nothing can silently re-encode
 * the pixels on the way out. */
static void write_frame(const char *path, const unsigned char *bgr,
                        int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "TH2REF %d %d 24\n", w, h);
    fwrite(bgr, 1, (size_t)w * (size_t)h * 3, f);
    fclose(f);
}

void th2ref_dump_frame(void *vram, int draw_mode)
{
    if (!g_dir || !g_dir[0] || !vram) return;
    if ((long)g_tick < g_first) return;
    if (g_last >= 0 && (long)g_tick > g_last) return;

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

    char path[512];
    sprintf(path, "%s/f%06lu.bin", g_dir, g_tick);
    write_frame(path, out, w, h);
    free(out);
}

}  /* extern "C" */
