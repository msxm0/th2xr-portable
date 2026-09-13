/* Decoder stubs for the reference build.
 *
 * The real XviD and Ogg Vorbis sources ARE in the GPL release
 * (aquaplus_gpl/XViD and aquaplus_gpl/OGG) - this is a choice, not a
 * necessity, and it is reversible by building those two trees instead.
 *
 * A reference harness compares pictures and engine state.  It never listens
 * to the audio and never looks at a movie frame, and both subsystems are the
 * two places the engine would otherwise become non-deterministic: Ogg
 * playback position gates AVG_WaitSe / WaitVoice / WaitBGM, and movie
 * decoding gates AVG_WaitMovie.  Stubbing them is what lets a tick be a unit
 * of engine progress rather than a unit of real time.
 *
 * Each stub reports "finished immediately", so the engine's waits clear on
 * the tick after they are asked - the shared fiction both sides agree on.
 */
#include <windows.h>
#include "xvid_dec.h"
#include "oggDec.h"

int  XviDDec::Start_XviD(int w, int h, int csp)            { (void)w;(void)h;(void)csp; return 0; }
void XviDDec::Clear_XviD()                                  {}
int  XviDDec::DecodeXviD(BYTE* in, int insize, int* rest,
                         BYTE* out, DWORD pitch)
{
    (void)in; (void)insize; (void)out; (void)pitch;
    if (rest) *rest = 0;
    return XviD_DEC_END;
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
