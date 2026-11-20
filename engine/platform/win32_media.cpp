// Multimedia for the real-WinMain build: DirectSound (sound effects), the mixer (the game's sound and music
// volume sliders), MCI (MIDI music; there is no CD audio) and MCIWnd (intro videos).
//
// Timing stays here: whether a sound buffer is still playing, a MIDI file's position and its end
// notification (MM_MCINOTIFY), and a video's mode. The browser only renders: every call is forwarded to
// Module.aoeMedia (web/native-media.mjs), which is optional. Without it the game runs silently with the
// same timing.
#include "port_prefix.h"
#include "win32_game.h"
#include "mmsystem.h"
#include "dsound.h"
#include <emscripten.h>
#include <fcntl.h>
#include <unistd.h>
#include <map>
#include <string>
#include <vector>

#define MCI_NOTIFY_SUCCESSFUL 1
#define MCI_NOTIFY_SUPERSEDED 2
#define MCI_NOTIFY_ABORTED 4
#define MCIERR_INVALID_DEVICE_ID 257
#define MCIERR_UNRECOGNIZED_COMMAND 261
#define MCIERR_FILE_NOT_FOUND 275
#define MCIERR_DEVICE_NOT_INSTALLED 306
#define MMSYSERR_BADDEVICEID 2
#define MIXERR_INVALLINE 1024
#define MIXERR_INVALCONTROL 1025
#ifndef WM_ERASEBKGND
#define WM_ERASEBKGND 0x0014
#endif
// The PCM part of WAVEFORMATEX (everything before cbSize).
#define PCM_FORMAT_BYTES 16

static double now_ms() { return emscripten_get_now(); }

// ---------------------------------------------------------------- browser bridge (all optional)
EM_JS(void, aoe_media_sound_data, (int id, const unsigned char* data, int bytes, int rate, int channels, int bits), {
    const m = Module['aoeMedia']; if (m) m.soundData(id, HEAPU8.slice(data, data + bytes), rate, channels, bits);
});
EM_JS(void, aoe_media_sound_duplicate, (int id, int source), { const m = Module['aoeMedia']; if (m) m.soundDuplicate(id, source); });
EM_JS(void, aoe_media_sound_play, (int id, int loop, int volume, int pan, int frequency), {
    const m = Module['aoeMedia']; if (m) m.soundPlay(id, !!loop, volume, pan, frequency);
});
EM_JS(void, aoe_media_sound_update, (int id, int volume, int pan, int frequency), {
    const m = Module['aoeMedia']; if (m) m.soundUpdate(id, volume, pan, frequency);
});
EM_JS(void, aoe_media_sound_stop, (int id), { const m = Module['aoeMedia']; if (m) m.soundStop(id); });
EM_JS(void, aoe_media_sound_release, (int id), { const m = Module['aoeMedia']; if (m) m.soundRelease(id); });
EM_JS(void, aoe_media_volume, (int line, double gain), { const m = Module['aoeMedia']; if (m) m.volume(line, gain); });
EM_JS(void, aoe_media_midi_play, (int id, const unsigned char* data, int bytes, double from_ms), {
    const m = Module['aoeMedia']; if (m) m.midiPlay(id, HEAPU8.slice(data, data + bytes), from_ms);
});
EM_JS(void, aoe_media_midi_stop, (int id), { const m = Module['aoeMedia']; if (m) m.midiStop(id); });
// Video size (width << 16 | height) of a converted intro video, 0 if the browser has none for this file.
EM_JS(int, aoe_media_video_open, (const char* path), {
    const m = Module['aoeMedia']; return m ? m.videoOpen(UTF8ToString(path)) | 0 : 0;
});
EM_JS(void, aoe_media_video_play, (int x, int y, int w, int h, int screen_w, int screen_h), {
    const m = Module['aoeMedia']; if (m) m.videoPlay(x, y, w, h, screen_w, screen_h);
});
EM_JS(void, aoe_media_video_pause, (int paused), { const m = Module['aoeMedia']; if (m) m.videoPause(!!paused); });
EM_JS(void, aoe_media_video_close, (), { const m = Module['aoeMedia']; if (m) m.videoClose(); });

static bool read_file(const char* path, std::vector<unsigned char>* out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    unsigned char chunk[65536];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) out->insert(out->end(), chunk, chunk + n);
    close(fd);
    return true;
}

// ---------------------------------------------------------------- DirectSound
static HRESULT ds_unsupported(const char* what) {
    static int reported;
    if (reported++ < 50) printf("[aoe] DirectSound: unsupported %s\n", what);
    return DSERR_UNSUPPORTED;
}

struct SoundData {
    std::vector<unsigned char> bytes;
    int refs;
    SoundData() : refs(1) {}
};

struct SoftSoundBuffer : IDirectSoundBuffer {
    ULONG refs;
    int id;
    bool primary;
    SoundData* data;
    WAVEFORMATEX format;
    LONG volume, pan;
    DWORD frequency;          // 0: the format's own rate
    bool playing, looping;
    double started;           // now_ms() when Play started (from position 0)
    static int next_id;

    SoftSoundBuffer(bool primary_in, DWORD bytes, const WAVEFORMATEX* fmt)
        : refs(1), id(++next_id), primary(primary_in), data(primary_in ? 0 : new SoundData), volume(0), pan(0), frequency(0),
          playing(false), looping(false), started(0) {
        memset(&format, 0, sizeof(format));
        if (fmt) memcpy(&format, fmt, PCM_FORMAT_BYTES);
        else { format.wFormatTag = WAVE_FORMAT_PCM; format.nChannels = 2; format.nSamplesPerSec = 22050; format.wBitsPerSample = 16;
               format.nBlockAlign = 4; format.nAvgBytesPerSec = 88200; }
        if (data) data->bytes.assign(bytes, format.wBitsPerSample == 8 ? 0x80 : 0);
    }
    ~SoftSoundBuffer() {
        aoe_media_sound_release(id);
        if (data && !--data->refs) delete data;
    }
    DWORD rate() const { return frequency ? frequency : format.nSamplesPerSec; }
    double duration_ms() const {
        if (!data || !format.nBlockAlign || !rate()) return 0;
        return (double) (data->bytes.size() / format.nBlockAlign) * 1000.0 / rate();
    }
    bool still_playing() {
        if (playing && !looping && now_ms() - started >= duration_ms()) playing = false;
        return playing;
    }

    HRESULT __stdcall QueryInterface(REFIID, void** out) { if (out) *out = 0; return E_NOINTERFACE; }
    ULONG __stdcall AddRef() { return ++refs; }
    ULONG __stdcall Release() { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT __stdcall GetCaps(LPDSBCAPS caps) {
        if (!caps) return DSERR_INVALIDPARAM;
        caps->dwFlags = primary ? DSBCAPS_PRIMARYBUFFER : DSBCAPS_STATIC | DSBCAPS_CTRLDEFAULT;
        caps->dwBufferBytes = data ? (DWORD) data->bytes.size() : 0;
        caps->dwUnlockTransferRate = 0;
        caps->dwPlayCpuOverhead = 0;
        return DS_OK;
    }
    HRESULT __stdcall GetCurrentPosition(LPDWORD play, LPDWORD write) {
        DWORD pos = 0;
        if (data && still_playing() && format.nBlockAlign) {
            double frames = (now_ms() - started) * rate() / 1000.0;
            size_t total = data->bytes.size() / format.nBlockAlign;
            pos = total ? (DWORD) (((size_t) frames % total) * format.nBlockAlign) : 0;
        }
        if (play) *play = pos;
        if (write) *write = pos;
        return DS_OK;
    }
    HRESULT __stdcall GetFormat(LPWAVEFORMATEX out, DWORD size, LPDWORD written) {
        DWORD n = size < sizeof(format) ? size : sizeof(format);
        if (out) memcpy(out, &format, n);
        if (written) *written = n;
        return DS_OK;
    }
    HRESULT __stdcall GetVolume(LPLONG out) { if (out) *out = volume; return DS_OK; }
    HRESULT __stdcall GetPan(LPLONG out) { if (out) *out = pan; return DS_OK; }
    HRESULT __stdcall GetFrequency(LPDWORD out) { if (out) *out = rate(); return DS_OK; }
    HRESULT __stdcall GetStatus(LPDWORD status) {
        if (!status) return DSERR_INVALIDPARAM;
        *status = still_playing() ? DSBSTATUS_PLAYING | (looping ? DSBSTATUS_LOOPING : 0) : 0;
        return DS_OK;
    }
    HRESULT __stdcall Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) { return DSERR_ALREADYINITIALIZED; }
    // The buffer is plain memory; Lock maps it in one piece (no wrap-around for these static buffers).
    HRESULT __stdcall Lock(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD) {
        if (!data) return ds_unsupported("Lock on the primary buffer");
        size_t size = data->bytes.size();
        if (offset > size) return DSERR_INVALIDPARAM;
        if (bytes > size - offset) bytes = (DWORD) (size - offset);
        if (p1) *p1 = size ? &data->bytes[offset] : 0;
        if (n1) *n1 = bytes;
        if (p2) *p2 = 0;
        if (n2) *n2 = 0;
        return DS_OK;
    }
    HRESULT __stdcall Unlock(LPVOID, DWORD, LPVOID, DWORD) {
        if (data) aoe_media_sound_data(id, data->bytes.empty() ? 0 : &data->bytes[0], (int) data->bytes.size(),
                                       (int) format.nSamplesPerSec, format.nChannels, format.wBitsPerSample);
        return DS_OK;
    }
    HRESULT __stdcall Play(DWORD, DWORD, DWORD flags) {
        if (primary) { playing = true; looping = true; return DS_OK; }
        if (!still_playing()) started = now_ms();
        playing = true;
        looping = (flags & DSBPLAY_LOOPING) != 0;
        aoe_media_sound_play(id, looping, volume, pan, (int) rate());
        return DS_OK;
    }
    HRESULT __stdcall SetCurrentPosition(DWORD pos) {
        // Only rewinding is used (before Play); a restart takes effect with the next Play.
        if (pos == 0 && playing) { started = now_ms(); if (!primary) { aoe_media_sound_stop(id); playing = false; } }
        return DS_OK;
    }
    HRESULT __stdcall SetFormat(LPCWAVEFORMATEX fmt) {
        if (!fmt) return DSERR_INVALIDPARAM;
        memcpy(&format, fmt, PCM_FORMAT_BYTES);
        return DS_OK;
    }
    HRESULT __stdcall SetVolume(LONG v) {
        volume = v < -10000 ? -10000 : v > 0 ? 0 : v;
        if (playing && !primary) aoe_media_sound_update(id, volume, pan, (int) rate());
        return DS_OK;
    }
    HRESULT __stdcall SetPan(LONG p) {
        pan = p < -10000 ? -10000 : p > 10000 ? 10000 : p;
        if (playing && !primary) aoe_media_sound_update(id, volume, pan, (int) rate());
        return DS_OK;
    }
    HRESULT __stdcall SetFrequency(DWORD f) {
        frequency = f;
        if (playing && !primary) aoe_media_sound_update(id, volume, pan, (int) rate());
        return DS_OK;
    }
    HRESULT __stdcall Stop() {
        if (playing && !primary) aoe_media_sound_stop(id);
        playing = false;
        return DS_OK;
    }
    HRESULT __stdcall Restore() { return DS_OK; }
};
int SoftSoundBuffer::next_id;

struct SoftDirectSound : IDirectSound {
    ULONG refs;
    SoftDirectSound() : refs(1) {}
    HRESULT __stdcall QueryInterface(REFIID, void** out) { if (out) *out = 0; return E_NOINTERFACE; }
    ULONG __stdcall AddRef() { return ++refs; }
    ULONG __stdcall Release() { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT __stdcall CreateSoundBuffer(LPCDSBUFFERDESC desc, LPLPDIRECTSOUNDBUFFER out, IUnknown*) {
        if (!desc || !out) return DSERR_INVALIDPARAM;
        bool primary = (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) != 0;
        if (!primary) {
            const WAVEFORMATEX* f = desc->lpwfxFormat;
            if (!f || f->wFormatTag != WAVE_FORMAT_PCM || (f->wBitsPerSample != 8 && f->wBitsPerSample != 16) || !f->nChannels
                || f->nChannels > 2 || !desc->dwBufferBytes)
                return DSERR_BADFORMAT;
        }
        *out = new SoftSoundBuffer(primary, desc->dwBufferBytes, desc->lpwfxFormat);
        return DS_OK;
    }
    HRESULT __stdcall GetCaps(LPDSCAPS caps) {
        if (!caps) return DSERR_INVALIDPARAM;
        DWORD size = caps->dwSize;
        memset(caps, 0, sizeof(*caps));
        caps->dwSize = size;
        caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT | DSCAPS_SECONDARYMONO
                        | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT | DSCAPS_CONTINUOUSRATE;
        caps->dwMinSecondarySampleRate = 100;
        caps->dwMaxSecondarySampleRate = 100000;
        caps->dwMaxHwMixingAllBuffers = caps->dwMaxHwMixingStaticBuffers = caps->dwMaxHwMixingStreamingBuffers = 64;
        return DS_OK;
    }
    HRESULT __stdcall DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER original, LPLPDIRECTSOUNDBUFFER out) {
        SoftSoundBuffer* src = static_cast<SoftSoundBuffer*>(original);
        if (!src || !out || src->primary) return DSERR_INVALIDPARAM;
        SoftSoundBuffer* copy = new SoftSoundBuffer(false, 0, &src->format);
        delete copy->data;
        copy->data = src->data;
        src->data->refs++;
        copy->volume = src->volume; copy->pan = src->pan; copy->frequency = src->frequency;
        aoe_media_sound_duplicate(copy->id, src->id);
        *out = copy;
        return DS_OK;
    }
    HRESULT __stdcall SetCooperativeLevel(HWND, DWORD) { return DS_OK; }
    HRESULT __stdcall Compact() { return DS_OK; }
    HRESULT __stdcall GetSpeakerConfig(LPDWORD out) { if (out) *out = DSSPEAKER_STEREO; return DS_OK; }
    HRESULT __stdcall SetSpeakerConfig(DWORD) { return DS_OK; }
    HRESULT __stdcall Initialize(LPCGUID) { return DSERR_ALREADYINITIALIZED; }
};

// ---------------------------------------------------------------- mixer
// One mixer with three source lines; their volume controls are the game's sound and music sliders.
enum { LINE_WAVE = 1, LINE_SYNTH = 2, LINE_CD = 3 };
static DWORD line_volume[4] = {0, 0xffff, 0xffff, 0xffff};

static DWORD line_for_component(DWORD type) {
    if (type == MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT) return LINE_WAVE;
    if (type == MIXERLINE_COMPONENTTYPE_SRC_SYNTHESIZER) return LINE_SYNTH;
    if (type == MIXERLINE_COMPONENTTYPE_SRC_COMPACTDISC) return LINE_CD;
    return 0;
}

// ---------------------------------------------------------------- MCI sequencer (MIDI files)
// Length of a Standard MIDI File in milliseconds (tempo changes applied); 0 if it isn't one.
static double midi_length_ms(const std::vector<unsigned char>& f) {
    size_t n = f.size();
    if (n < 14 || memcmp(&f[0], "MThd", 4)) return 0;
    int tracks = f[10] << 8 | f[11], division = f[12] << 8 | f[13];
    bool smpte = (division & 0x8000) != 0;   // frames per second x ticks per frame instead of ticks per quarter
    if (!smpte && !division) return 0;
    std::map<unsigned long, unsigned long> tempo;   // tick -> microseconds per quarter note
    unsigned long last_tick = 0;
    size_t pos = 8 + (f[4] << 24 | f[5] << 16 | f[6] << 8 | f[7]);
    for (int t = 0; t < tracks && pos + 8 <= n; t++) {
        size_t len = (size_t) f[pos + 4] << 24 | f[pos + 5] << 16 | f[pos + 6] << 8 | f[pos + 7];
        bool is_track = !memcmp(&f[pos], "MTrk", 4);
        size_t p = pos + 8, end = pos + 8 + len < n ? pos + 8 + len : n;
        pos = pos + 8 + len;
        if (!is_track) continue;
        unsigned long tick = 0;
        unsigned char status = 0;
        while (p < end) {
            unsigned long delta = 0;
            do delta = delta << 7 | (f[p] & 0x7f); while (f[p++] & 0x80 && p < end);
            tick += delta;
            if (p >= end) break;
            unsigned char b = f[p];
            if (b == 0xff) {
                if (p + 2 > end) break;
                unsigned char type = f[p + 1];
                p += 2;
                unsigned long mlen = 0;
                do mlen = mlen << 7 | (f[p] & 0x7f); while (f[p++] & 0x80 && p < end);
                if (type == 0x51 && mlen == 3 && p + 3 <= end) tempo[tick] = f[p] << 16 | f[p + 1] << 8 | f[p + 2];
                p += mlen;
                if (type == 0x2f) break;
            } else if (b == 0xf0 || b == 0xf7) {
                p++;
                unsigned long slen = 0;
                do slen = slen << 7 | (f[p] & 0x7f); while (f[p++] & 0x80 && p < end);
                p += slen;
            } else {
                if (b & 0x80) { status = b; p++; }
                if (!status) break;
                int kind = status & 0xf0;
                p += (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
            }
        }
        if (tick > last_tick) last_tick = tick;
    }
    if (smpte) {
        int fps = -(signed char) f[12], ticks = f[13];
        return fps > 0 && ticks ? last_tick * 1000.0 / (fps == 29 ? 29.97 : fps) / ticks : 0;
    }
    double ms = 0;
    unsigned long at = 0, us = 500000;
    for (std::map<unsigned long, unsigned long>::iterator i = tempo.begin(); i != tempo.end() && i->first < last_tick; ++i) {
        ms += (double) (i->first - at) * us / division / 1000.0;
        at = i->first;
        us = i->second;
    }
    return ms + (double) (last_tick - at) * us / division / 1000.0;
}

struct MciDevice {
    std::vector<unsigned char> file;
    double length, position;   // ms; position is valid while not playing
    double started;            // now_ms() - position at the last start
    bool playing;
    HWND notify;               // window for MM_MCINOTIFY, 0 if none pending
    int notify_plays;          // plays the pending notification answers (see mci_play)
};
static std::map<MCIDEVICEID, MciDevice> mci_devices;
static MCIDEVICEID next_device = 1;

static double mci_position(MciDevice& d) {
    if (!d.playing) return d.position;
    double p = now_ms() - d.started;
    return p < d.length ? p : d.length;
}

static void mci_notify(MCIDEVICEID id, MciDevice& d, WPARAM result) {
    // One message per play command the game counted (TMusic_System::notify_waiting): the last carries the result.
    for (; d.notify && d.notify_plays > 1; d.notify_plays--) PostMessageA(d.notify, MM_MCINOTIFY, MCI_NOTIFY_SUPERSEDED, id);
    if (d.notify) PostMessageA(d.notify, MM_MCINOTIFY, result, id);
    d.notify = 0;
    d.notify_plays = 0;
}

static void mci_halt(MCIDEVICEID id, MciDevice& d) {
    if (d.playing) { d.position = mci_position(d); d.playing = false; aoe_media_midi_stop(id); }
}

// Posts the end-of-file notifications (called from the message loop).
static void mci_poll() {
    for (std::map<MCIDEVICEID, MciDevice>::iterator i = mci_devices.begin(); i != mci_devices.end(); ++i) {
        MciDevice& d = i->second;
        if (d.playing && now_ms() - d.started >= d.length) {
            d.playing = false;
            d.position = d.length;
            aoe_media_midi_stop(i->first);
            mci_notify(i->first, d, MCI_NOTIFY_SUCCESSFUL);
        }
    }
}

// ---------------------------------------------------------------- MCIWnd (intro videos)
#define MCIWNDM_GETMODEA (WM_USER + 106)
#define MCIWNDM_GETPALETTE (WM_USER + 126)
enum VideoMode { VIDEO_STOPPED, VIDEO_PLAYING, VIDEO_PAUSED };
static HWND video_window;
static VideoMode video_mode;

static LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case MCIWNDM_GETMODEA: {
            const char* mode = video_mode == VIDEO_PLAYING ? "playing" : video_mode == VIDEO_PAUSED ? "paused" : "stopped";
            if (l && w) { strncpy((char*) l, mode, w); ((char*) l)[w - 1] = 0; }
            return video_mode == VIDEO_PLAYING ? 526 : video_mode == VIDEO_PAUSED ? 529 : 525;   // MCI_MODE_*
        }
        case MCIWNDM_GETPALETTE: return 0;   // the browser shows the video in true color
        case MCI_PLAY: {
            AoeWindow* wnd = aoe_window(hwnd);
            if (wnd) {
                POINT p = {0, 0};
                ClientToScreen(hwnd, &p);
                aoe_media_video_play(p.x, p.y, wnd->w, wnd->h, aoe_screen_width(), aoe_screen_height());
            }
            video_mode = VIDEO_PLAYING;
            return 0;
        }
        case MCI_PAUSE: if (video_mode == VIDEO_PLAYING) { video_mode = VIDEO_PAUSED; aoe_media_video_pause(1); } return 0;
        case MCI_RESUME: if (video_mode == VIDEO_PAUSED) { video_mode = VIDEO_PLAYING; aoe_media_video_pause(0); } return 0;
        case MCI_STOP: video_mode = VIDEO_STOPPED; aoe_media_video_close(); return 0;
        case WM_CLOSE: DestroyWindow(hwnd); return 0;
        case WM_DESTROY:
            if (hwnd == video_window) { video_window = 0; video_mode = VIDEO_STOPPED; aoe_media_video_close(); }
            return 0;
        case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcA(hwnd, msg, w, l);
}

// ---------------------------------------------------------------- per-frame work
void aoe_media_poll() { mci_poll(); }

extern "C" {

// The page reports the end of the video (or that it couldn't be played).
EMSCRIPTEN_KEEPALIVE void aoe_media_video_ended() {
    if (video_mode != VIDEO_STOPPED) video_mode = VIDEO_STOPPED;
}

// Video for Windows: the game checks for the Indeo 4.1 codec before playing intro videos. The browser plays
// converted copies (tools/prepare_native.py), so a codec is "installed" when the page has any videos.
EM_JS(int, aoe_media_has_videos, (), { const m = Module['aoeMedia']; return m && m.hasVideos && m.hasVideos() ? 1 : 0; });
BOOL ICInfo(DWORD, DWORD, void*) { return aoe_media_has_videos(); }

HWND MCIWndCreateA(HWND parent, HINSTANCE instance, DWORD style, LPCSTR file) {
    int size = aoe_media_video_open(file ? file : "");
    if (!size) return 0;
    static bool registered;
    if (!registered) {
        WNDCLASSA c;
        memset(&c, 0, sizeof(c));
        c.lpfnWndProc = video_proc;
        c.lpszClassName = "MCIWndClass";
        RegisterClassA(&c);
        registered = true;
    }
    if (video_window) DestroyWindow(video_window);
    video_mode = VIDEO_STOPPED;
    video_window = CreateWindowExA(0, "MCIWndClass", file, style & ~0xffffu, 0, 0, size >> 16, size & 0xffff, parent, 0, instance, 0);
    return video_window;
}

HRESULT WINAPI DirectSoundCreate(LPCGUID, LPDIRECTSOUND* out, LPUNKNOWN) {
    if (!out) return DSERR_INVALIDPARAM;
    *out = new SoftDirectSound;
    return DS_OK;
}

UINT mixerGetNumDevs() { return 1; }
MMRESULT mixerOpen(HMIXER* out, UINT id, DWORD_PTR, DWORD_PTR, DWORD) {
    if (id != 0) return MMSYSERR_BADDEVICEID;
    if (out) *out = (HMIXER) 1;
    return 0;
}
MMRESULT mixerClose(HMIXER) { return 0; }

MMRESULT mixerGetLineInfoA(HANDLE, MIXERLINEA* line, DWORD flags) {
    if (!line || (flags & 0xf) != MIXER_GETLINEINFOF_COMPONENTTYPE) return MIXERR_INVALLINE;
    DWORD id = line_for_component(line->dwComponentType);
    if (!id) return MIXERR_INVALLINE;
    DWORD type = line->dwComponentType, size = line->cbStruct;
    memset(line, 0, size < sizeof(*line) ? size : sizeof(*line));
    line->cbStruct = size;
    line->dwComponentType = type;
    line->dwLineID = id;
    line->dwSource = id - 1;
    line->cChannels = 1;
    line->cControls = 1;
    strcpy(line->szShortName, id == LINE_WAVE ? "Wave" : id == LINE_SYNTH ? "Synth" : "CD");
    strcpy(line->szName, line->szShortName);
    return 0;
}

MMRESULT mixerGetLineControlsA(HANDLE, MIXERLINECONTROLSA* controls, DWORD) {
    if (!controls || !controls->pamxctrl || controls->dwLineID < LINE_WAVE || controls->dwLineID > LINE_CD) return MIXERR_INVALLINE;
    if (controls->dwControlType != MIXERCONTROL_CONTROLTYPE_VOLUME) return MIXERR_INVALCONTROL;
    MIXERCONTROLA* c = controls->pamxctrl;
    memset(c, 0, sizeof(*c));
    c->cbStruct = sizeof(*c);
    c->dwControlID = controls->dwLineID;
    c->dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
    c->Bounds.dwMinimum = 0;
    c->Bounds.dwMaximum = 0xffff;
    c->Metrics.cSteps = 0xffff;
    strcpy(c->szShortName, "Volume");
    strcpy(c->szName, "Volume");
    return 0;
}

MMRESULT mixerGetControlDetailsA(HANDLE, MIXERCONTROLDETAILS* d, DWORD) {
    if (!d || !d->paDetails || d->dwControlID < LINE_WAVE || d->dwControlID > LINE_CD) return MIXERR_INVALCONTROL;
    ((MIXERCONTROLDETAILS_UNSIGNED*) d->paDetails)->dwValue = line_volume[d->dwControlID];
    return 0;
}

MMRESULT mixerSetControlDetails(HANDLE, MIXERCONTROLDETAILS* d, DWORD) {
    if (!d || !d->paDetails || d->dwControlID < LINE_WAVE || d->dwControlID > LINE_CD) return MIXERR_INVALCONTROL;
    DWORD v = ((MIXERCONTROLDETAILS_UNSIGNED*) d->paDetails)->dwValue;
    line_volume[d->dwControlID] = v > 0xffff ? 0xffff : v;
    aoe_media_volume((int) d->dwControlID, line_volume[d->dwControlID] / 65535.0);
    return 0;
}

MCIERROR mciSendCommandA(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR param) {
    if (msg == MCI_OPEN) {
        MCI_OPEN_PARMSA* open_parms = (MCI_OPEN_PARMSA*) param;
        if (!open_parms) return MCIERR_UNRECOGNIZED_COMMAND;
        DWORD type = (DWORD) (uintptr_t) open_parms->lpstrDeviceType;
        bool by_id = (flags & MCI_OPEN_TYPE_ID) != 0;
        bool sequencer = by_id ? type == MCI_DEVTYPE_SEQUENCER
                               : open_parms->lpstrDeviceType && !strcasecmp(open_parms->lpstrDeviceType, "sequencer");
        // No CD audio: the game then plays its MIDI files (RGE_Base_Game::setup_music_system).
        if (!sequencer) return MCIERR_DEVICE_NOT_INSTALLED;
        if (!(flags & MCI_OPEN_ELEMENT) || !open_parms->lpstrElementName) return MCIERR_UNRECOGNIZED_COMMAND;
        MciDevice d;
        if (!read_file(open_parms->lpstrElementName, &d.file)) return MCIERR_FILE_NOT_FOUND;
        d.length = midi_length_ms(d.file);
        d.position = d.started = 0;
        d.playing = false;
        d.notify = 0;
        d.notify_plays = 0;
        MCIDEVICEID device = next_device++;
        mci_devices[device] = d;
        open_parms->wDeviceID = device;
        return 0;
    }
    std::map<MCIDEVICEID, MciDevice>::iterator it = mci_devices.find(id);
    if (it == mci_devices.end()) return MCIERR_INVALID_DEVICE_ID;
    MciDevice& d = it->second;
    switch (msg) {
        case MCI_PLAY: {
            MCI_PLAY_PARMS* p = (MCI_PLAY_PARMS*) param;
            if (p && (flags & MCI_FROM)) { mci_halt(id, d); d.position = p->dwFrom; }
            if ((flags & MCI_NOTIFY) && p) {
                if (d.notify) mci_notify(id, d, MCI_NOTIFY_SUPERSEDED);
                d.notify = (HWND) p->dwCallback;
                d.notify_plays = 1;
            } else if (d.notify) {
                // A play without MCI_NOTIFY resumes after a pause; the game still counts it as a play
                // awaiting a notification, so the pending one answers both.
                d.notify_plays++;
            }
            if (!d.playing) {
                if (d.position >= d.length) d.position = 0;
                d.started = now_ms() - d.position;
                d.playing = true;
                aoe_media_midi_play(id, d.file.empty() ? 0 : &d.file[0], (int) d.file.size(), d.position);
            }
            return 0;
        }
        case MCI_PAUSE: mci_halt(id, d); return 0;
        case MCI_RESUME:
            if (!d.playing) { d.started = now_ms() - d.position; d.playing = true;
                              aoe_media_midi_play(id, d.file.empty() ? 0 : &d.file[0], (int) d.file.size(), d.position); }
            return 0;
        case MCI_STOP: mci_halt(id, d); mci_notify(id, d, MCI_NOTIFY_ABORTED); return 0;
        case MCI_SEEK: {
            MCI_SEEK_PARMS* p = (MCI_SEEK_PARMS*) param;
            mci_halt(id, d);
            d.position = (flags & MCI_TO) && p ? (p->dwTo < d.length ? p->dwTo : d.length) : 0;
            return 0;
        }
        case MCI_SET: return 0;   // the sequencer's time format stays milliseconds
        case MCI_STATUS: {
            MCI_STATUS_PARMS* p = (MCI_STATUS_PARMS*) param;
            if (!p || !(flags & MCI_STATUS_ITEM)) return MCIERR_UNRECOGNIZED_COMMAND;
            if (p->dwItem == MCI_STATUS_POSITION) p->dwReturn = (DWORD) mci_position(d);
            else if (p->dwItem == MCI_STATUS_LENGTH) p->dwReturn = (DWORD) d.length;
            else if (p->dwItem == MCI_STATUS_MODE) p->dwReturn = d.playing ? 526 : 525;
            else return MCIERR_UNRECOGNIZED_COMMAND;
            return 0;
        }
        case MCI_CLOSE:
            mci_halt(id, d);
            mci_notify(id, d, MCI_NOTIFY_ABORTED);
            mci_devices.erase(it);
            return 0;
    }
    return MCIERR_UNRECOGNIZED_COMMAND;
}

// Dsutil.c's WAV streaming (music from .wav files with the STREAMMUSIC option, or when MIDI is unavailable).
// The supplied game has no WAV music, so streaming reports "unavailable" and the game uses its MIDI files.
void ds_stream_init(void*, LPDIRECTSOUND, LPDIRECTSOUNDBUFFER) {}
void ds_stream_exit(void) {}
int ds_stream_file(char* name, int, long) { printf("[aoe] WAV music streaming is not available: %s\n", name ? name : ""); return 0; }
int ds_stream_volume(long) { return 0; }
int ds_stream_pause(void) { return 0; }
int ds_stream_resume(void) { return 0; }
int ds_stream_stop(void) { return 0; }
unsigned int ds_stream_messages(void*, unsigned int, unsigned int, long) { return 0; }

BOOL mciGetErrorStringA(MCIERROR error, LPSTR out, UINT size) {
    if (!out || !size) return FALSE;
    const char* text = error == MCIERR_DEVICE_NOT_INSTALLED ? "The specified device is not installed."
                       : error == MCIERR_FILE_NOT_FOUND ? "The specified file cannot be found."
                       : error == MCIERR_INVALID_DEVICE_ID ? "Invalid device ID."
                                                            : "The specified command is not supported.";
    strncpy(out, text, size);
    out[size - 1] = 0;
    return TRUE;
}

}
