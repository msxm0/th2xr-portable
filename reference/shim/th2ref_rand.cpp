/* TH2REF_RAND_LOG: every rand() the engine makes, run-length collapsed per
 * (tick, caller), so the port can be taught to draw the same numbers in the
 * same order.  Linked with -Wl,--wrap=rand; inert without the variable. */
#include <stdio.h>
#include <stdlib.h>

extern "C" int __real_rand(void);
extern "C" unsigned long th2ref_current_tick(void);

static FILE *g_log;
static int g_open;
static unsigned long g_tick = ~0ul;
static void *g_caller;
static unsigned long g_run;
static int g_first;
static unsigned long long g_total;

static void flush_run(void)
{
    if (g_log && g_run)
        fprintf(g_log, "%lu %p %lu %d %llu\n", g_tick, g_caller, g_run, g_first,
                g_total - g_run);
    g_run = 0;
}

extern "C" int __wrap_rand(void)
{
    if (!g_open) {
        g_open = 1;
        const char *p = getenv("TH2REF_RAND_LOG");
        if (p && *p) g_log = fopen(p, "w");
    }
    int v = __real_rand();
    if (g_log) {
        void *caller = __builtin_return_address(0);
        unsigned long tick = th2ref_current_tick();
        if (tick != g_tick || caller != g_caller) {
            flush_run();
            g_tick = tick;
            g_caller = caller;
            g_first = v;
            fflush(g_log);
        }
        ++g_run;
    }
    ++g_total;
    return v;
}
