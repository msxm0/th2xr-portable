/* Decoder stubs for the reference build.
 *
 * They decode nothing, but they do not finish instantly either: see
 * th2ref_pcm_play in th2ref_hooks.h.  A stub that reports end-of-stream on
 * its first call makes every sound- and movie-gated wait in the engine a
 * no-op, and a wait that never waits is a wait that is never tested.
 *
 * The real XviD and Ogg Vorbis sources ARE in the GPL release
 * (aquaplus_gpl/XViD and aquaplus_gpl/OGG) - this is a choice, not a
 * necessity, and it is reversible by building those two trees instead.
 *
 * A reference harness compares pictures and engine state.  It never listens
 * to the audio and never looks at a movie frame, and both subsystems are the
 * two places the engine would otherwise become non-deterministic: the engine
 * decides a sound has finished by asking DirectSound whether the buffer has
 * drained (ClSoundDS::GetStatus), and that answer arrives on a wall clock,
 * so the same AVG_WaitSe would cost a different number of ticks on a fast
 * machine than on a slow one.  Building the real decoders would not fix
 * that - it would make it worse, because then real audio really would be
 * playing during a replay running seven times faster than realtime.  The
 * fix either way is to slave the audio clock to the tick counter, and
 * stubbing is the cheap version of it.
 *
 * Each stub reports a fixed run of TH2REF_PCM_TICKS ticks (default 50) and
 * then end-of-stream, so a wait costs the same count of ticks on every
 * machine and every run - the shared fiction both sides agree on.
 */
#include <windows.h>
#include "xvid_dec.h"
#include "oggDec.h"
#include "th2ref_hooks.h"

int  XviDDec::Start_XviD(int w, int h, int csp)
{
    (void)w; (void)h; (void)csp;
    th2ref_movie_start();
    return 0;
}
void XviDDec::Clear_XviD()                                  {}
int  XviDDec::DecodeXviD(BYTE* in, int insize, int* rest,
                         BYTE* out, DWORD pitch)
{
    (void)in; (void)insize; (void)out; (void)pitch;
    if (rest) *rest = 0;
    // Decodes nothing either way - out is left as it was, which is the black
    // frame the movie player cleared.  What changes is only how long the
    // engine is told the stream lasts, so AVG_WaitMovie has something to
    // wait for.
    return th2ref_movie_decoding() ? XviD_DEC_CONTINUE : XviD_DEC_END;
}

void OggDec::Start_ogg(void)  {}
void OggDec::Clear_ogg(void)  {}
BOOL OggDec::GetWaveformat(WAVEFORMATEX* wfx, char* buf)
{
    (void)buf;
    if (!wfx) return FALSE;
    ZeroMemory(wfx, sizeof(*wfx));
    wfx->wFormatTag      = WAVE_FORMAT_PCM;
    wfx->nChannels       = 2;
    wfx->nSamplesPerSec  = 44100;
    wfx->wBitsPerSample  = 16;
    wfx->nBlockAlign     = 4;
    wfx->nAvgBytesPerSec = 44100 * 4;
    return TRUE;
}
int OggDec::DecodeOGG(char* inmem, int insize, char* outmem, int outsize, int* done)
{
    (void)inmem; (void)insize; (void)outmem; (void)outsize;
    if (done) *done = 0;
    return 0;   /* OGG_DEC_END */
}
