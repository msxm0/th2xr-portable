/* Forced in front of every translation unit of the reference build with
 * -include.  Nothing in the GPL tree is edited; everything the MSVC build
 * got from its own headers or its compiler is supplied here, in one place,
 * so it cannot rot when the sources are re-synced.
 */
#ifndef TH2REF_PRELUDE_H
#define TH2REF_PRELUDE_H

/* libstdc++'s bits/c++config.h unconditionally #undefs min and max, and it
 * is pulled in by anything that touches a C++ header - which on mingw
 * includes windows.h and math.h.  It has an include guard, so it only does
 * it once: pull it in here first, deliberately, and define the macros
 * afterwards where nothing will take them away again. */
#include <cstddef>
/* Every C++ header the sources use, pulled in *before* the macros exist -
 * otherwise they mangle std::min(a,b,comp)'s declaration, which takes three
 * arguments and is the one thing a two-argument function-like macro cannot
 * survive. */
#include <vector>
#include <algorithm>
#include <string>
#include <limits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* MSVC's windows.h defines these as function-like macros and the code
 * relies on it.  STD_StepLimit has a parameter called `max`, and
 * LIM(D,L,H) = min(max((D),(L)),(H)) only compiles because a function-like
 * macro expands solely when followed by '(' - so `max*(src-start)` stays the
 * parameter while `max((D),(L))` becomes the macro. */
#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

/* The virtual clock.  mmsystem.h is pulled in first so its declaration of
 * timeGetTime is parsed before the name becomes a macro - the same ordering
 * problem as min/max above.  Every caller is redirected at once: there are
 * four live clock sources in this engine (timeGetTime2, the TWait opcode's
 * direct call, STD_timeGetTime behind GetTime/WaitTime, and GetLocalTime
 * behind GetSystemTime) and patching them one at a time would miss some. */
#ifdef TH2REF_TRACE
#include <windows.h>
#include <mmsystem.h>
#include "th2ref_hooks.h"
#define timeGetTime() th2ref_time()
#endif

#endif /* TH2REF_PRELUDE_H */
