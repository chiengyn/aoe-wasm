// Software DirectDraw for the real-WinMain build: the game's own full-screen DirectDraw path runs
// unmodified against system-memory 8-bit surfaces. The primary surface is the screen; when the game has
// drawn to it, the frame is handed to the browser (Module.aoePresent) before control returns to the
// browser (win32_loop.cpp).
#include "port_prefix.h"
#include "win32_game.h"
#include "ddraw.h"
#include <emscripten.h>
#include <algorithm>
#include <vector>

#define DD_OK 0
#define DDERR_UNSUPPORTED ((HRESULT) 0x80004001)
#define DDERR_INVALIDPARAMS ((HRESULT) 0x80070057)
#define DDERR_GENERIC ((HRESULT) 0x80004005)
#define DDERR_NOTFOUND ((HRESULT) 0x887600FF)

static RECT make_rect(LONG l, LONG t, LONG r, LONG b) { RECT x; x.left = l; x.top = t; x.right = r; x.bottom = b; return x; }

static HRESULT unsupported(const char* what) {
    static int reported;
    if (reported++ < 50) printf("[aoe] DirectDraw: unsupported %s\n", what);
    return DDERR_UNSUPPORTED;
}

static int display_w = 800, display_h = 600;
static bool primary_dirty;

struct SoftPalette;
struct SoftClipper;

struct SoftPalette : IDirectDrawPalette {
    ULONG refs;
    PALETTEENTRY entries[256];
    bool on_primary;
    SoftPalette() : refs(1), on_primary(false) { memset(entries, 0, sizeof(entries)); }
    HRESULT QueryInterface(REFIID, void** out) { if (out) *out = 0; return DDERR_UNSUPPORTED; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT GetCaps(LPDWORD caps) { if (caps) *caps = 0x4 | 0x400; return DD_OK; } // DDPCAPS_8BIT | ALLOW256
    HRESULT GetEntries(DWORD, DWORD base, DWORD count, LPPALETTEENTRY out) {
        if (!out || base + count > 256) return DDERR_INVALIDPARAMS;
        memcpy(out, entries + base, count * sizeof(*out));
        return DD_OK;
    }
    HRESULT Initialize(LPDIRECTDRAW, DWORD, LPPALETTEENTRY) { return DD_OK; }
    HRESULT SetEntries(DWORD, DWORD start, DWORD count, LPPALETTEENTRY in) {
        if (!in || start + count > 256) return DDERR_INVALIDPARAMS;
        memcpy(entries + start, in, count * sizeof(*in));
        if (on_primary) primary_dirty = true; // palette animation changes the displayed colors
        return DD_OK;
    }
};

struct SoftClipper : IDirectDrawClipper {
    ULONG refs;
    HWND hwnd;
    SoftClipper() : refs(1), hwnd(0) {}
    HRESULT QueryInterface(REFIID, void** out) { if (out) *out = 0; return DDERR_UNSUPPORTED; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT GetClipList(LPRECT, LPRGNDATA, LPDWORD) { return unsupported("Clipper::GetClipList"); }
    HRESULT GetHWnd(HWND* out) { if (out) *out = hwnd; return DD_OK; }
    HRESULT Initialize(LPDIRECTDRAW, DWORD) { return DD_OK; }
    HRESULT IsClipListChanged(WINBOOL* changed) { if (changed) *changed = FALSE; return DD_OK; }
    HRESULT SetClipList(LPRGNDATA, DWORD) { return unsupported("Clipper::SetClipList"); }
    HRESULT SetHWnd(DWORD, HWND h) { hwnd = h; return DD_OK; }
};

struct SoftSurface;
static SoftSurface* primary;
static AoeSurfaceView surface_view(void* owner);

struct SoftSurface : IDirectDrawSurface {
    ULONG refs;
    int width, height, pitch;
    std::vector<unsigned char> bits;
    bool is_primary, has_key;
    DWORD key_low, key_high;
    SoftPalette* palette;
    SoftClipper* clipper;
    HDC dc;
    DWORD caps;

    SoftSurface(int w, int h, bool prim, DWORD caps_in)
        : refs(1), width(w), height(h), pitch((w + 3) & ~3), bits(((w + 3) & ~3) * (h > 0 ? h : 1)), is_primary(prim),
          has_key(false), key_low(0), key_high(0), palette(0), clipper(0), dc(0), caps(caps_in) {}
    ~SoftSurface() {
        if (palette) palette->Release();
        if (clipper) clipper->Release();
        if (dc) aoe_delete_dc(dc);
        if (primary == this) primary = 0;
    }

    void describe(LPDDSURFACEDESC d) {
        DWORD size = d->dwSize ? d->dwSize : sizeof(DDSURFACEDESC);
        memset(d, 0, sizeof(DDSURFACEDESC));
        d->dwSize = size;
        d->dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        d->dwWidth = width;
        d->dwHeight = height;
        d->lPitch = pitch;
        d->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        d->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        d->ddpfPixelFormat.dwRGBBitCount = 8;
        d->ddsCaps.dwCaps = caps;
        if (has_key) { d->dwFlags |= DDSD_CKSRCBLT; d->ddckCKSrcBlt.dwColorSpaceLowValue = key_low; d->ddckCKSrcBlt.dwColorSpaceHighValue = key_high; }
    }

    // Rectangles of the destination that DirectDraw may not write (visible child windows under a clipper).
    void excluded(std::vector<RECT>* out) {
        if (is_primary && clipper && clipper->hwnd) aoe_child_clip_rects(clipper->hwnd, out);
    }

    // Copies src(sx,sy,w,h) to (dx,dy) with optional source color key and child-window clipping.
    void copy_from(SoftSurface* src, int dx, int dy, int sx, int sy, int w, int h, bool keyed, const RECT* stretch_dest) {
        if (stretch_dest) { stretch_from(src, *stretch_dest, sx, sy, w, h, keyed); return; }
        if (sx < 0) { dx -= sx; w += sx; sx = 0; }
        if (sy < 0) { dy -= sy; h += sy; sy = 0; }
        if (dx < 0) { sx -= dx; w += dx; dx = 0; }
        if (dy < 0) { sy -= dy; h += dy; dy = 0; }
        w = std::min(w, std::min(src->width - sx, width - dx));
        h = std::min(h, std::min(src->height - sy, height - dy));
        if (w <= 0 || h <= 0) return;
        std::vector<RECT> holes;
        excluded(&holes);
        bool same = src == this;
        std::vector<unsigned char> temp;
        const unsigned char* from = &src->bits[0];
        int from_pitch = src->pitch;
        if (same) { temp.assign(src->bits.begin(), src->bits.end()); from = &temp[0]; }
        for (int row = 0; row < h; row++) {
            const unsigned char* s = from + (sy + row) * from_pitch + sx;
            unsigned char* d = &bits[(dy + row) * pitch + dx];
            if (!keyed && holes.empty()) { memmove(d, s, w); continue; }
            for (int col = 0; col < w; col++) {
                if (keyed && s[col] >= src->key_low && s[col] <= src->key_high) continue;
                if (!holes.empty()) {
                    int x = dx + col, y = dy + row;
                    bool hidden = false;
                    for (size_t i = 0; i < holes.size() && !hidden; i++) hidden = x >= holes[i].left && x < holes[i].right && y >= holes[i].top && y < holes[i].bottom;
                    if (hidden) continue;
                }
                d[col] = s[col];
            }
        }
        if (is_primary) primary_dirty = true;
    }

    void stretch_from(SoftSurface* src, RECT dest, int sx, int sy, int sw, int sh, bool keyed) {
        int dw = dest.right - dest.left, dh = dest.bottom - dest.top;
        if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
        std::vector<unsigned char> copy(src->bits);
        for (int y = 0; y < dh; y++) {
            int ty = dest.top + y, fy = sy + y * sh / dh;
            if (ty < 0 || ty >= height || fy < 0 || fy >= src->height) continue;
            for (int x = 0; x < dw; x++) {
                int tx = dest.left + x, fx = sx + x * sw / dw;
                if (tx < 0 || tx >= width || fx < 0 || fx >= src->width) continue;
                unsigned char c = copy[fy * src->pitch + fx];
                if (keyed && c >= src->key_low && c <= src->key_high) continue;
                bits[ty * pitch + tx] = c;
            }
        }
        if (is_primary) primary_dirty = true;
    }

    void fill(RECT r, unsigned char color) {
        r.left = std::max<LONG>(0, r.left); r.top = std::max<LONG>(0, r.top);
        r.right = std::min<LONG>(width, r.right); r.bottom = std::min<LONG>(height, r.bottom);
        std::vector<RECT> holes;
        excluded(&holes);
        for (int y = r.top; y < r.bottom; y++)
            for (int x = r.left; x < r.right; x++) {
                bool hidden = false;
                for (size_t i = 0; i < holes.size() && !hidden; i++) hidden = x >= holes[i].left && x < holes[i].right && y >= holes[i].top && y < holes[i].bottom;
                if (!hidden) bits[y * pitch + x] = color;
            }
        if (is_primary) primary_dirty = true;
    }

    // IUnknown
    HRESULT QueryInterface(REFIID, void** out) { if (out) *out = 0; return DDERR_UNSUPPORTED; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG r = --refs; if (!r) delete this; return r; }

    HRESULT AddAttachedSurface(LPDIRECTDRAWSURFACE) { return unsupported("Surface::AddAttachedSurface"); }
    HRESULT AddOverlayDirtyRect(LPRECT) { return unsupported("Surface::AddOverlayDirtyRect"); }

    HRESULT Blt(LPRECT dest, LPDIRECTDRAWSURFACE source, LPRECT src_rect, DWORD flags, LPDDBLTFX fx) {
        RECT d = dest ? *dest : make_rect(0, 0, width, height);
        if (flags & DDBLT_COLORFILL) {
            if (!fx) return DDERR_INVALIDPARAMS;
            fill(d, (unsigned char) fx->dwFillColor);
            return DD_OK;
        }
        SoftSurface* src = (SoftSurface*) source;
        if (!src) return DDERR_INVALIDPARAMS;
        RECT s = src_rect ? *src_rect : make_rect(0, 0, src->width, src->height);
        int sw = s.right - s.left, sh = s.bottom - s.top, dw = d.right - d.left, dh = d.bottom - d.top;
        bool keyed = (flags & DDBLT_KEYSRC) && src->has_key;
        copy_from(src, d.left, d.top, s.left, s.top, sw, sh, keyed, (sw != dw || sh != dh) ? &d : 0);
        return DD_OK;
    }
    HRESULT BltBatch(LPDDBLTBATCH, DWORD, DWORD) { return unsupported("Surface::BltBatch"); }
    HRESULT BltFast(DWORD x, DWORD y, LPDIRECTDRAWSURFACE source, LPRECT src_rect, DWORD trans) {
        SoftSurface* src = (SoftSurface*) source;
        if (!src) return DDERR_INVALIDPARAMS;
        RECT s = src_rect ? *src_rect : make_rect(0, 0, src->width, src->height);
        copy_from(src, (int) x, (int) y, s.left, s.top, s.right - s.left, s.bottom - s.top, (trans & DDBLTFAST_SRCCOLORKEY) && src->has_key, 0);
        return DD_OK;
    }
    HRESULT DeleteAttachedSurface(DWORD, LPDIRECTDRAWSURFACE) { return unsupported("Surface::DeleteAttachedSurface"); }
    HRESULT EnumAttachedSurfaces(LPVOID, LPDDENUMSURFACESCALLBACK) { return unsupported("Surface::EnumAttachedSurfaces"); }
    HRESULT EnumOverlayZOrders(DWORD, LPVOID, LPDDENUMSURFACESCALLBACK) { return unsupported("Surface::EnumOverlayZOrders"); }
    HRESULT Flip(LPDIRECTDRAWSURFACE, DWORD) { return unsupported("Surface::Flip"); }
    HRESULT GetAttachedSurface(LPDDSCAPS, LPDIRECTDRAWSURFACE*) { return unsupported("Surface::GetAttachedSurface"); }
    HRESULT GetBltStatus(DWORD) { return DD_OK; } // blits complete synchronously
    HRESULT GetCaps(LPDDSCAPS out) { if (out) out->dwCaps = caps; return DD_OK; }
    HRESULT GetClipper(LPDIRECTDRAWCLIPPER* out) { if (!out) return DDERR_INVALIDPARAMS; *out = clipper; if (clipper) clipper->AddRef(); return clipper ? DD_OK : DDERR_NOCLIPPERATTACHED; }
    HRESULT GetColorKey(DWORD, LPDDCOLORKEY out) { if (!has_key) return DDERR_NOTFOUND; out->dwColorSpaceLowValue = key_low; out->dwColorSpaceHighValue = key_high; return DD_OK; }
    HRESULT GetDC(HDC* out) {
        if (!out) return DDERR_INVALIDPARAMS;
        if (!dc) dc = aoe_create_surface_dc(this, surface_view);
        *out = dc;
        return DD_OK;
    }
    HRESULT GetFlipStatus(DWORD) { return DD_OK; }
    HRESULT GetOverlayPosition(LPLONG, LPLONG) { return unsupported("Surface::GetOverlayPosition"); }
    HRESULT GetPalette(LPDIRECTDRAWPALETTE* out) { if (!out) return DDERR_INVALIDPARAMS; *out = palette; if (palette) palette->AddRef(); return palette ? DD_OK : DDERR_NOTFOUND; }
    HRESULT GetPixelFormat(LPDDPIXELFORMAT out) {
        memset(out, 0, sizeof(*out));
        out->dwSize = sizeof(*out);
        out->dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        out->dwRGBBitCount = 8;
        return DD_OK;
    }
    HRESULT GetSurfaceDesc(LPDDSURFACEDESC out) { if (!out) return DDERR_INVALIDPARAMS; describe(out); return DD_OK; }
    HRESULT Initialize(LPDIRECTDRAW, LPDDSURFACEDESC) { return DD_OK; }
    HRESULT IsLost() { return DD_OK; }
    HRESULT Lock(LPRECT rect, LPDDSURFACEDESC out, DWORD, HANDLE) {
        if (!out) return DDERR_INVALIDPARAMS;
        describe(out);
        out->lpSurface = &bits[0] + (rect ? rect->top * pitch + rect->left : 0);
        return DD_OK;
    }
    HRESULT ReleaseDC(HDC) { if (is_primary) primary_dirty = true; return DD_OK; }
    HRESULT Restore() { return DD_OK; }
    HRESULT SetClipper(LPDIRECTDRAWCLIPPER c) {
        if (clipper) clipper->Release();
        clipper = (SoftClipper*) c;
        if (clipper) clipper->AddRef();
        return DD_OK;
    }
    HRESULT SetColorKey(DWORD flags, LPDDCOLORKEY key) {
        if (!(flags & DDCKEY_SRCBLT)) return DD_OK;
        has_key = key != 0;
        if (key) { key_low = key->dwColorSpaceLowValue; key_high = (flags & DDCKEY_COLORSPACE) ? key->dwColorSpaceHighValue : key_low; }
        return DD_OK;
    }
    HRESULT SetOverlayPosition(LONG, LONG) { return unsupported("Surface::SetOverlayPosition"); }
    HRESULT SetPalette(LPDIRECTDRAWPALETTE p) {
        if (palette) { palette->on_primary = false; palette->Release(); }
        palette = (SoftPalette*) p;
        if (palette) { palette->AddRef(); palette->on_primary = is_primary; }
        if (is_primary) primary_dirty = true;
        return DD_OK;
    }
    HRESULT Unlock(LPVOID) { if (is_primary) primary_dirty = true; return DD_OK; }
    HRESULT UpdateOverlay(LPRECT, LPDIRECTDRAWSURFACE, LPRECT, DWORD, LPDDOVERLAYFX) { return unsupported("Surface::UpdateOverlay"); }
    HRESULT UpdateOverlayDisplay(DWORD) { return unsupported("Surface::UpdateOverlayDisplay"); }
    HRESULT UpdateOverlayZOrder(DWORD, LPDIRECTDRAWSURFACE) { return unsupported("Surface::UpdateOverlayZOrder"); }
};

static AoeSurfaceView surface_view(void* owner) {
    SoftSurface* s = (SoftSurface*) owner;
    AoeSurfaceView v;
    v.bits = &s->bits[0];
    v.width = s->width;
    v.height = s->height;
    v.pitch = s->pitch;
    // Surfaces without their own palette use the display palette, as DirectDraw does in 8-bit modes.
    SoftPalette* p = s->palette ? s->palette : (primary ? primary->palette : 0);
    v.palette = p ? p->entries : 0;
    return v;
}

struct SoftDirectDraw : IDirectDraw {
    ULONG refs;
    // The window given DDSCL_FULLSCREEN: DirectDraw sizes it to cover the screen on each display mode change.
    HWND fullscreen_wnd;
    SoftDirectDraw() : refs(1), fullscreen_wnd(0) {}
    HRESULT QueryInterface(REFIID, void** out) { if (out) *out = 0; return DDERR_UNSUPPORTED; }
    ULONG AddRef() { return ++refs; }
    ULONG Release() { ULONG r = --refs; if (!r) delete this; return r; }
    HRESULT Compact() { return DD_OK; }
    HRESULT CreateClipper(DWORD, LPDIRECTDRAWCLIPPER* out, IUnknown*) { *out = new SoftClipper; return DD_OK; }
    HRESULT CreatePalette(DWORD, LPPALETTEENTRY table, LPDIRECTDRAWPALETTE* out, IUnknown*) {
        SoftPalette* p = new SoftPalette;
        if (table) memcpy(p->entries, table, sizeof(p->entries));
        *out = p;
        return DD_OK;
    }
    HRESULT CreateSurface(LPDDSURFACEDESC desc, LPDIRECTDRAWSURFACE* out, IUnknown*) {
        if (!desc || !out) return DDERR_INVALIDPARAMS;
        bool prim = (desc->dwFlags & DDSD_CAPS) && (desc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
        if (prim && primary) return DDERR_GENERIC; // DDERR_PRIMARYSURFACEALREADYEXISTS
        int w = prim ? display_w : (int) desc->dwWidth, h = prim ? display_h : (int) desc->dwHeight;
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return DDERR_INVALIDPARAMS;
        DWORD caps = ((desc->dwFlags & DDSD_CAPS) ? desc->ddsCaps.dwCaps : DDSCAPS_OFFSCREENPLAIN) | DDSCAPS_SYSTEMMEMORY;
        SoftSurface* s = new SoftSurface(w, h, prim, caps);
        if (desc->dwFlags & DDSD_CKSRCBLT) s->SetColorKey(DDCKEY_SRCBLT | DDCKEY_COLORSPACE, &desc->ddckCKSrcBlt);
        if (prim) primary = s;
        *out = s;
        return DD_OK;
    }
    HRESULT DuplicateSurface(LPDIRECTDRAWSURFACE, LPDIRECTDRAWSURFACE*) { return unsupported("DirectDraw::DuplicateSurface"); }
    HRESULT EnumDisplayModes(DWORD, LPDDSURFACEDESC, LPVOID context, LPDDENUMMODESCALLBACK callback) {
        static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}};
        for (int i = 0; i < 4; i++) {
            DDSURFACEDESC d;
            memset(&d, 0, sizeof(d));
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
            d.dwWidth = modes[i][0];
            d.dwHeight = modes[i][1];
            d.lPitch = modes[i][0];
            d.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            d.ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
            d.ddpfPixelFormat.dwRGBBitCount = 8;
            if (!callback(&d, context)) break;
        }
        return DD_OK;
    }
    HRESULT EnumSurfaces(DWORD, LPDDSURFACEDESC, LPVOID, LPDDENUMSURFACESCALLBACK) { return unsupported("DirectDraw::EnumSurfaces"); }
    HRESULT FlipToGDISurface() { return DD_OK; }
    HRESULT GetCaps(LPDDCAPS driver, LPDDCAPS hel) {
        LPDDCAPS both[2] = {driver, hel};
        for (int i = 0; i < 2; i++) {
            if (!both[i]) continue;
            DWORD size = both[i]->dwSize ? both[i]->dwSize : sizeof(DDCAPS);
            memset(both[i], 0, size);
            both[i]->dwSize = size;
            both[i]->dwCaps = DDCAPS_BLT | DDCAPS_BLTCOLORFILL | DDCAPS_COLORKEY | DDCAPS_PALETTE;
            both[i]->dwCKeyCaps = DDCKEYCAPS_SRCBLT;
            both[i]->dwPalCaps = DDPCAPS_8BIT;
            both[i]->ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY | DDSCAPS_PALETTE;
        }
        return DD_OK;
    }
    HRESULT GetDisplayMode(LPDDSURFACEDESC out) {
        if (!out) return DDERR_INVALIDPARAMS;
        DWORD size = out->dwSize ? out->dwSize : sizeof(DDSURFACEDESC);
        memset(out, 0, sizeof(DDSURFACEDESC));
        out->dwSize = size;
        out->dwFlags = DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT;
        out->dwWidth = display_w;
        out->dwHeight = display_h;
        out->lPitch = display_w;
        out->ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        out->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
        out->ddpfPixelFormat.dwRGBBitCount = 8;
        return DD_OK;
    }
    HRESULT GetFourCCCodes(LPDWORD count, LPDWORD) { if (count) *count = 0; return DD_OK; }
    HRESULT GetGDISurface(LPDIRECTDRAWSURFACE*) { return unsupported("DirectDraw::GetGDISurface"); }
    HRESULT GetMonitorFrequency(LPDWORD f) { if (f) *f = 60; return DD_OK; }
    HRESULT GetScanLine(LPDWORD l) { if (l) *l = 0; return DD_OK; }
    HRESULT GetVerticalBlankStatus(WINBOOL* b) { if (b) *b = TRUE; return DD_OK; }
    HRESULT Initialize(GUID*) { return DD_OK; }
    HRESULT RestoreDisplayMode() { return DD_OK; }
    HRESULT SetCooperativeLevel(HWND h, DWORD flags) { fullscreen_wnd = (flags & DDSCL_FULLSCREEN) ? h : 0; return DD_OK; }
    HRESULT SetDisplayMode(DWORD w, DWORD h, DWORD bpp) {
        if (bpp != 8 || w < 320 || h < 200 || w > 4096 || h > 4096) return DDERR_UNSUPPORTED;
        display_w = w;
        display_h = h;
        aoe_set_screen_size(w, h);
        // As Windows does for an exclusive full-screen window: the game's window grows or shrinks with the mode.
        // The game reads its edges back (GetWindowRect) for edge scrolling, so a window left at the old size
        // scrolled the map before the pointer reached the right or bottom edge of a larger screen (the game
        // ignores the WM_SIZE this sends while it changes modes, as with the real one).
        if (fullscreen_wnd) SetWindowPos(fullscreen_wnd, 0, 0, 0, (int) w, (int) h, 0x0004 /* SWP_NOZORDER */ | 0x0010 /* SWP_NOACTIVATE */);
        return DD_OK;
    }
    HRESULT WaitForVerticalBlank(DWORD, HANDLE) { return DD_OK; }
};

extern "C" HRESULT DirectDrawCreate(GUID*, LPDIRECTDRAW* out, IUnknown*) {
    if (!out) return DDERR_INVALIDPARAMS;
    *out = new SoftDirectDraw;
    return DD_OK;
}

bool aoe_primary_view(AoeSurfaceView* out) {
    if (!primary) return false;
    if (out) *out = surface_view(primary);
    return true;
}

void aoe_mark_primary_dirty() { primary_dirty = true; }

// ---------------------------------------------------------------- presentation
EM_JS(void, aoe_host_present, (const unsigned char* bits, int width, int height, int pitch, const PALETTEENTRY* palette), {
    if (Module['aoePresent']) Module['aoePresent'](bits, width, height, pitch, palette);
});

static unsigned frames_presented;
extern "C" EMSCRIPTEN_KEEPALIVE unsigned aoe_frames_presented() { return frames_presented; }

// Copy of the last presented frame (indices + palette) for tests and screenshots.
extern "C" EMSCRIPTEN_KEEPALIVE int aoe_frame_info(int field) {
    if (!primary) return 0;
    switch (field) {
        case 0: return (int) (intptr_t) &primary->bits[0];
        case 1: return primary->width;
        case 2: return primary->height;
        case 3: return primary->pitch;
        case 4: return primary->palette ? (int) (intptr_t) primary->palette->entries : 0;
    }
    return 0;
}

void aoe_present_if_dirty() {
    aoe_user_frame();
    // Child windows (edit controls) paint into the screen surface before it is shown.
    if (aoe_paint_children()) primary_dirty = true;
    if (!primary || !primary_dirty) return;
    primary_dirty = false;
    frames_presented++;
    static PALETTEENTRY black[256];
    aoe_host_present(&primary->bits[0], primary->width, primary->height, primary->pitch, primary->palette ? primary->palette->entries : black);
}
