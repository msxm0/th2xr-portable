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

/* True when TH2REF_NOTEXT is set.  The reference composites text into the
 * same buffer as the art and we keep it on a separate high-resolution layer,
 * so a pixel comparison has to leave it out of both. */
int th2ref_text_hidden(void);

/* One line per tick of the integers the AVG machine turns on - the program
 * counter, the message state machine, the background fade and scroll and
 * shake counters, the half tone.  Written to TH2REF_STATE.  Pixels say two
 * runs disagree; this says which counter disagreed first. */
void th2ref_dump_state(void);

/* Checkpoint support.  th2ref_set_tick moves the virtual clock to the tick a
 * save was taken at, so a resumed run keeps the same absolute schedule as
 * the straight-through one it has to agree with; th2ref_hold_dump suppresses
 * the frame dump while the lead-in runs, so the ticks spent reaching the
 * load point never reach the trace. */
void th2ref_set_tick(unsigned long tick);
void th2ref_hold_dump(int hold);
int  th2ref_dump_held(void);

/* A sound that lasts a fixed number of ticks.
 *
 * The decoders are stubbed, so nothing is ever really played; but the engine
 * decides a sound has finished by asking DirectSound whether the buffer has
 * drained, and that answer comes on a wall clock.  Under a virtual clock
 * that makes the length of a wait depend on how fast the machine is, which
 * is the one thing the harness cannot have.  So the buffer's status comes
 * from the tick counter instead: a sound reports PCM_PLAY for
 * TH2REF_PCM_TICKS ticks (default 50) after it is started, and PCM_STOP
 * after that - except a looping one, which never stops, exactly as a real
 * loop would not.
 *
 * Reporting PCM_STOP immediately, which is what stubbing alone did, meant
 * SEW, VW and MW never waited for anything and the whole class of
 * sound-gated waits went untested. */
void th2ref_pcm_play(int handle, int repeat);
void th2ref_pcm_stop(int handle);
int  th2ref_pcm_status(int handle);       /* PCM_STOP 0 / PCM_PLAY 1 */
/* The same fiction for the movie: XviD_DEC_CONTINUE until the run is up. */
int  th2ref_movie_decoding(void);
void th2ref_movie_start(void);

/* The per-glyph alpha of the message text.
 *
 * TXT_DrawTextEx computes, for every character it emits,
 *
 *     alph2 = LIM(text_cnt - cnt2, 0, 16) * 16;
 *     if(step < step_cnt) alph2 = 256;
 *
 * and that vector is the typewriter.  No scalar in the state trace can stand
 * in for it: NovelMessage.count and .kstep can agree on both sides while the
 * formula that turns them into per-character alpha disagrees, which is
 * exactly how a bug that re-fades already-revealed letters hid from a trace
 * that dumped only the counters and a picture with the text taken out of it.
 *
 * begin() is called once per TXT_DrawTextEx and says whether this text
 * object is the message window - the only one worth recording; glyph() is
 * called for each character, outside the cnt_flag guard, so it records even
 * when nothing is being drawn. */
void th2ref_text_slot(int slot);
void th2ref_text_begin(int text_cnt, int step_cnt);
void th2ref_text_glyph(int alph2);

/* Replaces KEY_RenewKeybord and MUS_RenewMouse: fills KeyCond from a script
 * keyed by tick, and leaves the real devices unread. */
void th2ref_input(void);

/* MUS_RenewMouse reads the real pointer three ways.  Redirecting these lets
 * the function itself run untouched, so all of its rect and trigger
 * bookkeeping still happens - on scripted coordinates. */
/* Probe: AVG_GetHitKey is `click && MUS_GetMouseNo(-1)==-1`, and the port
 * models only the click.  Records which rect the cursor was over on the
 * frames a click actually lands, so the one that suppresses a message hit
 * can be identified. */
void  th2ref_note_hitkey(int click, int rect_no);

/* Audio as events rather than samples.  Nothing is decoded on this side, so
 * there is no waveform to compare - what can be compared is which sound the
 * engine asked for, on which channel, on which tick.  `name` is the resolved
 * voice file where one exists, NULL otherwise. */
void  th2ref_note_audio(const char *kind, int a, int b, int c, int d,
                        const char *name);
/* The scripted left-button level this tick, straight from the input table -
 * so a dropped click can be pinned on the shim or on the engine. */
int   th2ref_mouse_level(void);

int   th2ref_cursor_pos(POINT *p);
short th2ref_async_key(int vk);

#ifdef __cplusplus
}
#endif
#endif
