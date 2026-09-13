/* Instrumentation for the reference build.  Declarations only; the bodies
 * live in shim/th2ref_hooks.cpp.  Enabled by -DTH2REF_TRACE.
 */
#ifndef TH2REF_HOOKS_H
#define TH2REF_HOOKS_H
#ifdef __cplusplus
extern "C" {
#endif

/* The virtual clock.  One tick is one frame; it advances at the top of
 * MAIN_Loop and nothing else may observe real time.
 *
 * It returns tick * (1000/60) - the same integer step MAIN_Loop adds to
 * next_time - rather than tick*1000/60.  If the two disagreed by the 0.67ms
 * the rounding loses, NowTime would creep ahead of next_time, `skip` would
 * grow, and after about a hundred frames skip_cnt would start dropping
 * draws.  Stepping by exactly 1000/frame keeps skip at zero forever, so
 * every logic tick gets exactly one MAIN_DrawControl.
 */
unsigned long th2ref_time(void);
void          th2ref_advance_tick(void);
unsigned long th2ref_current_tick(void);

/* Called from MAIN_DrawControl with the bitmap MAIN_DrawGraph just filled,
 * before any DirectDraw blit - so the capture has no window, no compositor
 * and no scaling in it. */
void th2ref_dump_frame(void *vram, int draw_mode);

#ifdef __cplusplus
}
#endif
#endif
