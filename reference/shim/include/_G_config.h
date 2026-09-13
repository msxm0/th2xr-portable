/* Minimal stand-in for glibc's _G_config.h.
 *
 * libogg's ogg/os_types.h picks its integer types with
 *     #ifdef _WIN32
 *     #  ifndef __GNUC__   / * MSVC * /  ... __int64 ...
 *     #  else              / * Cygwin * /  #include <_G_config.h>
 * and a mingw cross-compiler is _WIN32 *and* __GNUC__, so it lands in the
 * Cygwin branch.  These are the five types it takes from there.
 */
#ifndef TH2REF__G_CONFIG_H
#define TH2REF__G_CONFIG_H
typedef long long          _G_int64_t;
typedef int                _G_int32_t;
typedef unsigned int       _G_uint32_t;
typedef short              _G_int16_t;
typedef unsigned short     _G_uint16_t;
#endif
