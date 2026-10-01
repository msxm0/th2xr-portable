#include "audio_browser.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// Two browser decoders, chosen per file:
//
//   WebCodecs AudioDecoder, where it takes Vorbis.  The Ogg pages are split
//   into packets here and fed a few dozen at a time, and every decoded
//   packet arrives as its own small task and is copied into per-channel
//   arrays sized from the stream's final granule position.  Nothing on the
//   main thread is bigger than one packet's copy.
//
//   decodeAudioData otherwise.  It decodes off-thread too, but then builds
//   the whole AudioBuffer in one main-thread task - 24-47 ms for a music
//   track, the longest task left in a traced session - and resamples it to
//   the context's rate.
//
// A head and the rest of the same file must come out of the same one, or
// the two halves would disagree about the rate; `method` pins it.

EM_JS(void, th2_audio_init_js, (), {
    if (Module.__th2Audio) {
        return;
    }
    var Context = window.AudioContext || window.webkitAudioContext;
    var audio = Module.__th2Audio = {
        context: Context ? new Context() : null,
        next: 1,
        pending: new Map(),
        // Unknown until asked, and asking needs a real stream's headers
        // (Chrome insists on the description even for isConfigSupported),
        // so the first Ogg file asks and goes to decodeAudioData; the files
        // after the answer follow it.
        webcodecs: undefined,
        asking: false,
    };

    // Ogg pages to Vorbis packets, walked lazily: a packet is cut out only
    // when the decoder is about to be fed it.  Splitting a whole file up
    // front made a view object per packet - seven thousand for a music track,
    // for every file read ahead - that the GC had to mark however little of
    // the file was ever decoded.  A page cut short - the tail of a head still
    // downloading - ends the walk; a packet running over it is left out
    // rather than decoded truncated.
    //
    // `view` returns the file's bytes as they are now.  They live in the
    // engine's memory, which outlasts the decode, so nothing is copied into
    // JS; the view is taken again on every call because a growing wasm heap
    // replaces the buffer under any view held across one.
    audio.oggReader = function (view) {
        var reader = {at: 0, page: null, segment: 0, start: 0, p: 0,
                      pieces: [], pieceBytes: 0};
        var bytes = view();
        var nextPage = function () {
            var at = reader.at;
            if (at + 27 > bytes.length || bytes[at] !== 0x4f
                || bytes[at + 1] !== 0x67 || bytes[at + 2] !== 0x67
                || bytes[at + 3] !== 0x53) {
                return false;
            }
            var segments = bytes[at + 26];
            var body = at + 27 + segments;
            if (body > bytes.length) {
                return false;
            }
            var size = 0;
            for (var i = 0; i < segments; ++i) {
                size += bytes[at + 27 + i];
            }
            if (body + size > bytes.length) {
                return false;
            }
            reader.page = at;
            reader.segment = 0;
            reader.start = body;
            reader.p = body;
            reader.at = body + size;
            return true;
        };
        reader.next = function () {
            bytes = view();
            while (true) {
                if (reader.page === null && !nextPage()) {
                    return null;
                }
                var segments = bytes[reader.page + 26];
                while (reader.segment < segments) {
                    var length = bytes[reader.page + 27 + reader.segment++];
                    reader.p += length;
                    if (length < 255) {
                        var piece = bytes.subarray(reader.start, reader.p);
                        reader.start = reader.p;
                        if (reader.pieces.length === 0) {
                            return piece;
                        }
                        reader.pieces.push(piece);
                        var joined = new Uint8Array(reader.pieceBytes + piece.length);
                        var o = 0;
                        for (var k = 0; k < reader.pieces.length; ++k) {
                            joined.set(reader.pieces[k], o);
                            o += reader.pieces[k].length;
                        }
                        reader.pieces = [];
                        reader.pieceBytes = 0;
                        return joined;
                    }
                }
                if (reader.start < reader.p) {
                    // Continues on the next page.
                    reader.pieces.push(bytes.subarray(reader.start, reader.p));
                    reader.pieceBytes += reader.p - reader.start;
                }
                reader.page = null;
            }
        };
        // The last complete page's granule and end-of-stream flag, found from
        // the end of the bytes rather than by walking every page.
        reader.lastGranule = -1;
        reader.eos = false;
        for (var at = bytes.length - 27; at >= 0; --at) {
            if (bytes[at] !== 0x4f || bytes[at + 1] !== 0x67
                || bytes[at + 2] !== 0x67 || bytes[at + 3] !== 0x53
                || bytes[at + 4] !== 0) {
                continue;
            }
            var segments = bytes[at + 26];
            var body = at + 27 + segments;
            if (body > bytes.length) {
                continue;
            }
            var size = 0;
            for (var i = 0; i < segments; ++i) {
                size += bytes[at + 27 + i];
            }
            if (body + size > bytes.length) {
                continue;
            }
            var low = (bytes[at + 6] | bytes[at + 7] << 8 | bytes[at + 8] << 16
                       | bytes[at + 9] << 24) >>> 0;
            var high = (bytes[at + 10] | bytes[at + 11] << 8
                        | bytes[at + 12] << 16 | bytes[at + 13] << 24) >>> 0;
            if (low === 0xffffffff && high === 0xffffffff) {
                continue;   // a page that completes no packet
            }
            // A whole file's last page ends where the bytes do, which is
            // also what tells a real page from "OggS" turning up inside the
            // audio data.  Only a head still downloading ends elsewhere, and
            // its end is not used.
            if (body + size === bytes.length) {
                reader.lastGranule = high * 4294967296 + low;
                reader.eos = (bytes[at + 5] & 4) !== 0;
                break;
            }
            if (reader.lastGranule < 0) {
                reader.lastGranule = high * 4294967296 + low;
            }
            if (bytes.length - at > 65536) {
                break;      // far enough back: a head's last complete page
            }
        }
        return reader;
    };

    // One decoded packet, interleaved, straight into the wasm buffer the
    // engine attached (th2_audio_attach).  Holding the decode in JS arrays
    // instead - sixty megabytes for a music track - was external memory the
    // GC answered with a major collection, 8-15 ms of main thread, at every
    // track change.  The buffer is sized from the stream's granules, which
    // is exact for a whole file; a decode that would overrun it fails over
    // to ffmpeg rather than write past it.
    audio.take = function (record, data) {
        var frames = data.numberOfFrames;
        var at = record.frames;
        if (record.state !== 0 || !record.dest
            || data.numberOfChannels !== record.channels
            || at + frames > record.allocated) {
            data.close();
            if (record.state === 0) {
                audio.fail(record);
            }
            return;
        }
        var first = (record.dest >> 2) + at * record.channels;
        // HEAPF32 is looked up here, not kept: a growing heap replaces it.
        data.copyTo(HEAPF32.subarray(first, first + frames * record.channels),
                    {planeIndex: 0, format: 'f32'});
        data.close();
        record.frames = at + frames;
    };

    audio.fail = function (record) {
        record.state = -1;
        if (record.decoder) {
            try { record.decoder.close(); } catch (e) {}
            record.decoder = null;
        }
        record.reader = null;
    };

    // Keeps a few dozen packets in flight, but only until the frames the
    // engine has asked for (th2_audio_want) are covered.  Read-ahead wants
    // two seconds of a track, not three minutes: decoding every file whole
    // made thousands of chunk and AudioData objects per track for the GC to
    // trace - 50 ms collections on the title screen once it read the first
    // scene ahead.  Driven by the decoder's own dequeue events as well as by
    // polls, so a decode asked for more keeps going between polls.
    audio.pump = function (record) {
        if (!record.decoder || record.state !== 0 || record.flushing) {
            return;
        }
        var batch = 48;
        while (record.reader && batch-- > 0
               && record.decoder.decodeQueueSize < 24
               && record.frames + record.decoder.decodeQueueSize * 2048
                      < record.want
               // Only what the engine's buffer has room for: a Vorbis packet
               // decodes to at most 4096 frames.  A buffer for the whole
               // stream has the slack for its last packet built in.
               && (record.allocated >= record.capacity
                   || record.frames + (record.decoder.decodeQueueSize + 1) * 4096
                          <= record.allocated)) {
            var packet = record.reader.next();
            if (!packet) {
                record.reader = null;   // every packet is in
                break;
            }
            record.decoder.decode(new EncodedAudioChunk({
                type: 'key', timestamp: record.next, data: packet}));
            ++record.next;
        }
        if (!record.reader) {
            record.flushing = true;
            record.decoder.flush().then(function () {
                // The last page's granule is where the stream ends; the
                // decoder pads its final block past it.  A looped track
                // with the padding left in clicks at the seam.
                if (record.end >= 0 && record.frames > record.end) {
                    record.frames = record.end;
                }
                record.state = 1;
                try { record.decoder.close(); } catch (e) {}
                record.decoder = null;
                record.reader = null;
            }, function () { audio.fail(record); });
        }
    };

    audio.vorbisConfig = function (headers) {
        if (headers.length < 3 || !headers[2] || headers[0][0] !== 1) {
            return null;
        }
        var id = headers[0];
        var channels = id[11];
        var rate = (id[12] | id[13] << 8 | id[14] << 16 | id[15] << 24) >>> 0;
        if (channels <= 0 || rate <= 0) {
            return null;
        }
        // Xiph lacing of the three header packets, as the Vorbis codec
        // registration asks for: count - 1, the first two sizes, then all
        // three.
        var lace = function (n, out) {
            while (n >= 255) { out.push(255); n -= 255; }
            out.push(n);
        };
        var head = [2];
        lace(headers[0].length, head);
        lace(headers[1].length, head);
        var description = new Uint8Array(head.length + headers[0].length
            + headers[1].length + headers[2].length);
        description.set(head, 0);
        var o = head.length;
        for (var k = 0; k < 3; ++k) {
            description.set(headers[k], o);
            o += headers[k].length;
        }
        return {codec: 'vorbis', sampleRate: rate, numberOfChannels: channels,
                description: description};
    };

    audio.startWebCodecs = function (record, view) {
        if (audio.webcodecs === false || typeof AudioDecoder === 'undefined') {
            return false;
        }
        var reader = audio.oggReader(view);
        var headers = [reader.next(), reader.next(), reader.next()];
        var config = headers[0] && headers[1] && headers[2]
            ? audio.vorbisConfig(headers) : null;
        if (!config) {
            return false;
        }
        // Not yet answered: ask, and try it anyway.  Where it is not
        // supported the decoder's error fails this one over to ffmpeg, at the
        // same native rate, and the answer sends later files to
        // decodeAudioData; where it is, the first track already avoids
        // decodeAudioData's one long completion task.
        if (audio.webcodecs === undefined && !audio.asking
            && AudioDecoder.isConfigSupported) {
            audio.asking = true;
            AudioDecoder.isConfigSupported(config).then(
                function (result) { audio.webcodecs = !!result.supported; },
                function () { audio.webcodecs = false; });
        }
        record.channels = config.numberOfChannels;
        record.rate = config.sampleRate;
        record.capacity = Math.max(0, reader.lastGranule) + 8192;
        record.dest = 0;      // until th2_audio_attach
        record.allocated = 0;
        record.want = 0;      // until th2_audio_want
        record.end = reader.eos && reader.lastGranule >= 0
            ? reader.lastGranule : -1;
        record.reader = reader;
        record.next = 3;    // packets fed, headers included
        record.flushing = false;
        try {
            record.decoder = new AudioDecoder({
                output: function (data) { audio.take(record, data); },
                error: function () { audio.fail(record); },
            });
            record.decoder.ondequeue = function () { audio.pump(record); };
            record.decoder.configure(config);
        } catch (e) {
            record.decoder = null;
            return false;
        }
        record.method = 1;
        audio.pump(record);
        return true;
    };
});

EM_JS(int, th2_audio_start_js, (const unsigned char* bytes, int size, int method), {
    var audio = Module.__th2Audio;
    if (!audio) {
        return 0;
    }
    // WebCodecs reads the file where the engine keeps it, until release.
    var view = function () { return HEAPU8.subarray(bytes, bytes + size); };
    var handle = audio.next++;
    var record = {state: 0, rate: 0, channels: 0, frames: 0, method: 0,
                  buffer: null, dest: 0, decoder: null};
    if (method !== 2 && audio.startWebCodecs(record, view)) {
        audio.pending.set(handle, record);
        return handle;
    }
    if (method === 1 || !audio.context) {
        return 0;
    }
    record.method = 2;
    record.rate = audio.context.sampleRate | 0;
    audio.pending.set(handle, record);
    // decodeAudioData detaches what it is given, so it alone gets a copy.
    var copy = new Uint8Array(size);
    copy.set(HEAPU8.subarray(bytes, bytes + size));
    audio.context.decodeAudioData(copy.buffer, function (decoded) {
        record.buffer = decoded;
        record.frames = decoded.length;
        record.channels = decoded.numberOfChannels;
        record.state = 1;
    }, function () { record.state = -1; });
    return handle;
});

// Frames a WebCodecs decode can produce, so the engine can give it a buffer;
// 0 for decodeAudioData, which keeps its own.
EM_JS(int, th2_audio_capacity_js, (int handle), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    return record && record.method === 1 ? record.capacity : 0;
});

EM_JS(void, th2_audio_attach_js, (int handle, float* dest, int frames), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    if (record) {
        record.dest = dest;
        record.allocated = frames;
        audio.pump(record);
    }
});

// The engine wants at least `frames` decoded (-1: all of it).
EM_JS(void, th2_audio_want_js, (int handle, int frames), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    if (!record || record.method !== 1) {
        return;
    }
    record.want = frames < 0 ? Infinity : Math.max(record.want, frames);
    audio.pump(record);
});

EM_JS(int, th2_audio_rate_of_js, (int handle), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    return record ? record.rate : 0;
});

EM_JS(int, th2_audio_method_of_js, (int handle), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    return record ? record.method : 0;
});

EM_JS(int, th2_audio_poll_js, (int handle, int* frames, int* channels), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    if (!record) {
        return -1;
    }
    if (record.method === 1 && record.state >= 0) {
        // WebCodecs: what has arrived so far, never past the stream's end.
        audio.pump(record);
        HEAP32[frames >> 2] = record.end >= 0
            ? Math.min(record.frames, record.end) : record.frames;
        HEAP32[channels >> 2] = record.channels;
        return record.state;
    }
    if (record.state === 0) {
        audio.pump(record);
        return 0;
    }
    if (record.state === 1) {
        HEAP32[frames >> 2] = record.frames;
        HEAP32[channels >> 2] = record.channels;
    }
    return record.state;
});

EM_JS(void, th2_audio_copy_js, (int handle, float* dest, int first, int count), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    if (!record || record.state !== 1) {
        return;
    }
    if (!record.buffer) {
        return;     // a WebCodecs decode is in the engine's buffer already
    }
    var channels = record.channels;
    var out = dest >> 2;
    for (var c = 0; c < channels; ++c) {
        var data = record.buffer.getChannelData(c);
        var at = out + c;
        for (var i = 0; i < count; ++i) {
            HEAPF32[at] = data[first + i];
            at += channels;
        }
    }
});

EM_JS(void, th2_audio_release_js, (int handle), {
    var audio = Module.__th2Audio;
    var record = audio && audio.pending.get(handle);
    if (record && record.decoder) {
        try { record.decoder.close(); } catch (e) {}
        record.decoder = null;
    }
    if (audio) {
        audio.pending.delete(handle);
    }
});

// Plain functions over the JS ones.  An EM_JS function is a JS import, so a
// call to it from another object file does not pull this one out of the
// static library - these do.
void th2_audio_init() { th2_audio_init_js(); }
int th2_audio_start(const unsigned char* bytes, int size, int method)
{
    return th2_audio_start_js(bytes, size, method);
}
int th2_audio_rate_of(int handle) { return th2_audio_rate_of_js(handle); }
int th2_audio_capacity(int handle) { return th2_audio_capacity_js(handle); }
void th2_audio_attach(int handle, float* dest, int frames)
{
    th2_audio_attach_js(handle, dest, frames);
}
void th2_audio_want(int handle, int frames) { th2_audio_want_js(handle, frames); }
int th2_audio_method_of(int handle) { return th2_audio_method_of_js(handle); }
int th2_audio_poll(int handle, int* frames, int* channels)
{
    return th2_audio_poll_js(handle, frames, channels);
}
void th2_audio_copy(int handle, float* dest, int first, int count)
{
    th2_audio_copy_js(handle, dest, first, count);
}
void th2_audio_release(int handle) { th2_audio_release_js(handle); }

#endif
