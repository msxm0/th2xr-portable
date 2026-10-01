#pragma once

// Ogg Vorbis decoded by the browser instead of by ffmpeg (web build only).
//
// ffmpeg builds a stream's Huffman tables on open - about six milliseconds,
// atomic, and three times a frame's whole allowance for work nobody is
// waiting on.  Across a playthrough that is thirty seconds of rebuilding the
// same tables, because every voice line carries the same codebooks.  The
// browser decodes off this thread; what is left here is a copy, which the
// caller slices.
//
// A handle is polled until it reports ready (1) or failed (-1); on failure
// the caller decodes with ffmpeg instead.

#ifdef __EMSCRIPTEN__
void th2_audio_init();
// Starts a decode of `size` bytes of Ogg; 0 if the browser cannot take it.
// `method` 0 lets it choose; 1 (WebCodecs) or 2 (decodeAudioData) repeats
// the choice made for an earlier part of the same file.
int th2_audio_start(const unsigned char* bytes, int size, int method);
// The rate the handle's samples come out at, and which decoder it chose.
// Both are known as soon as it starts.
int th2_audio_rate_of(int handle);
int th2_audio_method_of(int handle);
// A WebCodecs decode writes interleaved floats straight into the engine's
// memory: capacity is how many frames the whole stream can produce (0 for a
// decoder that keeps its own buffer); attach hands it room for `frames` of
// them, which must stay put until replaced by another attach or the handle
// is released.  It decodes no further than that room.  th2_audio_copy is
// then not needed.
int th2_audio_capacity(int handle);
void th2_audio_attach(int handle, float* dest, int frames);
// How far a WebCodecs decode should go: at least `frames` (-1 for all).  It
// decodes only that far, and polls report frames as they arrive rather than
// only once the whole stream is done.
void th2_audio_want(int handle, int frames);
// 0 still working, 1 ready, -1 failed.
int th2_audio_poll(int handle, int* frames, int* channels);
// Interleaves frames [first, first + count) into dest (float samples).
void th2_audio_copy(int handle, float* dest, int first, int count);
void th2_audio_release(int handle);
#endif
