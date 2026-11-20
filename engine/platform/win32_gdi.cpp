// GDI subset for the real-WinMain build. Device contexts draw straight into 8-bit surfaces (DirectDraw
// surface DCs, window DCs on the primary surface). Colors map to palette indices the way GDI does on a
// palettized display. Glyphs come from the browser's fonts (Module.aoeFonts), thresholded to 1 bit
// because the game's Windows fonts drew without antialiasing on 8-bit surfaces.
#include "port_prefix.h"
#include "win32_game.h"
#include <emscripten.h>
#include <algorithm>
#include <string>
#include <vector>

enum GdiKind { G_FONT = 1, G_REGION, G_PALETTE, G_BRUSH, G_PEN };
struct GdiObj {
    int kind;
    LOGFONTA font;
    RECT rect;
    COLORREF color;
    bool hollow;
    std::vector<PALETTEENTRY> entries;
    GdiObj(int k) : kind(k), color(0), hollow(false) { memset(&font, 0, sizeof(font)); memset(&rect, 0, sizeof(rect)); }
};
static std::vector<GdiObj*> objects;
static GdiObj* obj(HGDIOBJ h) { for (size_t i = 0; i < objects.size(); i++) if (objects[i] == h) return objects[i]; return 0; }
static GdiObj* make(int kind) { GdiObj* o = new GdiObj(kind); objects.push_back(o); return o; }

struct Dc {
    enum Kind { Surface, Window, Info } kind;
    void* owner;
    AoeSurfaceView (*view)(void*);
    HWND window;
    GdiObj *font, *brush, *pen, *palette, *clip;
    COLORREF text_color, bk_color;
    int bk_mode;
    POINT pos;
    Dc(Kind k) : kind(k), owner(0), view(0), window(0), font(0), brush(0), pen(0), palette(0), clip(0), text_color(0),
                 bk_color(0xffffff), bk_mode(OPAQUE) { pos.x = pos.y = 0; }
};
static std::vector<Dc*> dcs;
static Dc* dc_of(HDC h) { for (size_t i = 0; i < dcs.size(); i++) if (dcs[i] == (Dc*) h) return dcs[i]; return 0; }

static GdiObj* stock(int index) {
    static GdiObj* table[20];
    if (index < 0 || index >= 20) return 0;
    if (!table[index]) {
        GdiObj* o = 0;
        switch (index) {
            case WHITE_BRUSH: o = make(G_BRUSH); o->color = 0xffffff; break;
            case 1: o = make(G_BRUSH); o->color = 0xc0c0c0; break;   // LTGRAY_BRUSH
            case 2: o = make(G_BRUSH); o->color = 0x808080; break;   // GRAY_BRUSH
            case 3: o = make(G_BRUSH); o->color = 0x404040; break;   // DKGRAY_BRUSH
            case BLACK_BRUSH: o = make(G_BRUSH); o->color = 0; break;
            case HOLLOW_BRUSH: o = make(G_BRUSH); o->hollow = true; break;
            case WHITE_PEN: o = make(G_PEN); o->color = 0xffffff; break;
            case BLACK_PEN: o = make(G_PEN); o->color = 0; break;
            case 8: o = make(G_PEN); o->hollow = true; break;        // NULL_PEN
            case DEFAULT_PALETTE: {
                o = make(G_PALETTE);
                // The 20 static Windows colors.
                static const unsigned char sys[20][3] = {{0,0,0},{128,0,0},{0,128,0},{128,128,0},{0,0,128},{128,0,128},{0,128,128},{192,192,192},{192,220,192},{166,202,240},
                                                         {255,251,240},{160,160,164},{128,128,128},{255,0,0},{0,255,0},{255,255,0},{0,0,255},{255,0,255},{0,255,255},{255,255,255}};
                for (int i = 0; i < 20; i++) { PALETTEENTRY e = {sys[i][0], sys[i][1], sys[i][2], 0}; o->entries.push_back(e); }
                break;
            }
            default: { // fonts: SYSTEM_FONT, ANSI_VAR_FONT, ...
                o = make(G_FONT);
                o->font.lfHeight = 16;
                o->font.lfWeight = 700;
                strcpy(o->font.lfFaceName, "Arial");
            }
        }
        table[index] = o;
    }
    return table[index];
}

// ---------------------------------------------------------------- color mapping
static bool view_of(Dc* d, AoeSurfaceView* v, int* ox, int* oy) {
    *ox = *oy = 0;
    if (d->kind == Dc::Surface && d->view) { *v = d->view(d->owner); return v->bits != 0; }
    if (d->kind == Dc::Window) {
        if (!aoe_primary_view(v)) return false;
        AoeWindow* w = aoe_window(d->window);
        for (; w; w = aoe_window(w->parent)) { *ox += w->x; *oy += w->y; }
        return true;
    }
    return false;
}

static int nearest(const PALETTEENTRY* pal, int count, COLORREF c) {
    int r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, best = 0, dist = 0x7fffffff;
    for (int i = 0; i < count; i++) {
        int dr = r - pal[i].peRed, dg = g - pal[i].peGreen, db = b - pal[i].peBlue, d = dr * dr + dg * dg + db * db;
        if (d < dist) { dist = d; best = i; if (!d) break; }
    }
    return best;
}

static int color_index(Dc* d, const AoeSurfaceView& v, COLORREF c) {
    if ((c >> 24) == 1) return c & 255;                     // PALETTEINDEX
    if (d->palette && !d->palette->entries.empty() && (c >> 24) == 2)
        return nearest(&d->palette->entries[0], d->palette->entries.size(), c & 0xffffff);
    if (v.palette) return nearest(v.palette, 256, c & 0xffffff);
    return (c & 0xffffff) ? 255 : 0;
}

// Effective clip in surface coordinates.
static RECT clip_rect(Dc* d, const AoeSurfaceView& v, int ox, int oy) {
    RECT r = {0, 0, v.width, v.height};
    if (d->kind == Dc::Window) {
        AoeWindow* w = aoe_window(d->window);
        if (w) { r.left = std::max<LONG>(r.left, ox); r.top = std::max<LONG>(r.top, oy); r.right = std::min<LONG>(r.right, ox + w->w); r.bottom = std::min<LONG>(r.bottom, oy + w->h); }
    }
    if (d->clip) {
        r.left = std::max<LONG>(r.left, d->clip->rect.left + ox); r.top = std::max<LONG>(r.top, d->clip->rect.top + oy);
        r.right = std::min<LONG>(r.right, d->clip->rect.right + ox); r.bottom = std::min<LONG>(r.bottom, d->clip->rect.bottom + oy);
    }
    return r;
}

static void fill(const AoeSurfaceView& v, RECT r, const RECT& clip, int index) {
    r.left = std::max(r.left, clip.left); r.top = std::max(r.top, clip.top);
    r.right = std::min(r.right, clip.right); r.bottom = std::min(r.bottom, clip.bottom);
    for (int y = r.top; y < r.bottom; y++) memset(v.bits + y * v.pitch + r.left, index, std::max<LONG>(0, r.right - r.left));
}

// ---------------------------------------------------------------- fonts (browser rasterizer)
// LOGFONTA: lfHeight +0, lfWeight +16, lfItalic +20, lfUnderline +21, lfFaceName +28 (32 bytes).
// Text is Windows-1252. Hosts receive {height, weight, italic, underline, face} and a JS string.
EM_JS(int, aoe_font_measure, (const LOGFONTA* font, const char* text, int length, int* out), {
    const fonts = Module['aoeFonts'];
    if (!fonts) throw new Error('aoeFonts host missing');
    const m = fonts.measure(Module['aoeDecodeFont'](font), Module['aoeDecodeText'](text, length));
    HEAP32[out >> 2] = m.width; HEAP32[(out >> 2) + 1] = m.height; HEAP32[(out >> 2) + 2] = m.ascent;
    return 1;
});
// Writes a width*height 0/1 mask; returns 0 if `capacity` is too small.
EM_JS(int, aoe_font_raster, (const LOGFONTA* font, const char* text, int length, unsigned char* mask, int capacity, int* out), {
    const r = Module['aoeFonts'].raster(Module['aoeDecodeFont'](font), Module['aoeDecodeText'](text, length));
    HEAP32[out >> 2] = r.width; HEAP32[(out >> 2) + 1] = r.height; HEAP32[(out >> 2) + 2] = r.ascent;
    if (r.width * r.height > capacity) return 0;
    HEAPU8.set(r.mask.subarray(0, r.width * r.height), mask);
    return 1;
});
EM_JS(void, aoe_font_install_decoders, (), {
    const high = '\u20ac\u0081\u201a\u0192\u201e\u2026\u2020\u2021\u02c6\u2030\u0160\u2039\u0152\u008d\u017d\u008f\u0090\u2018\u2019\u201c\u201d\u2022\u2013\u2014\u02dc\u2122\u0161\u203a\u0153\u009d\u017e\u0178';
    Module['aoeDecodeText'] = (ptr, length) => {
        let s = '';
        for (let i = 0; i < length; i++) { const b = HEAPU8[ptr + i]; s += b >= 0x80 && b < 0xa0 ? high[b - 0x80] : String.fromCharCode(b); }
        return s;
    };
    Module['aoeDecodeFont'] = ptr => {
        let face = '';
        for (let i = 0; i < 32 && HEAPU8[ptr + 28 + i]; i++) face += String.fromCharCode(HEAPU8[ptr + 28 + i]);
        return {height: HEAP32[ptr >> 2], weight: HEAP32[(ptr + 16) >> 2], italic: !!HEAPU8[ptr + 20], underline: !!HEAPU8[ptr + 21], face};
    };
});
static struct InstallFontDecoders { InstallFontDecoders() { aoe_font_install_decoders(); } } install_font_decoders;

static const LOGFONTA* font_of(Dc* d) { return d->font ? &d->font->font : &stock(SYSTEM_FONT)->font; }

static SIZE measure(Dc* d, const char* text, int length) {
    int m[3] = {0, 0, 0};
    aoe_font_measure(font_of(d), text, length, m);
    SIZE s = {m[0], m[1]};
    return s;
}

static void draw_text(Dc* d, int x, int y, const char* text, int length) {
    AoeSurfaceView v;
    int ox, oy;
    if (length <= 0 || !view_of(d, &v, &ox, &oy)) return;
    static std::vector<unsigned char> mask(1 << 16);
    int m[3];
    while (!aoe_font_raster(font_of(d), text, length, &mask[0], mask.size(), m)) mask.resize(m[0] * m[1]);
    RECT clip = clip_rect(d, v, ox, oy);
    int px = x + ox, py = y + oy;
    if (d->bk_mode == OPAQUE) {
        RECT box = {px, py, px + m[0], py + m[1]};
        fill(v, box, clip, color_index(d, v, d->bk_color));
    }
    int index = color_index(d, v, d->text_color);
    for (int row = 0; row < m[1]; row++) {
        int sy = py + row;
        if (sy < clip.top || sy >= clip.bottom) continue;
        for (int col = 0; col < m[0]; col++) {
            int sx = px + col;
            if (sx >= clip.left && sx < clip.right && mask[row * m[0] + col]) v.bits[sy * v.pitch + sx] = (unsigned char) index;
        }
    }
}

const LOGFONTA* aoe_font_info(HFONT f) { GdiObj* o = obj(f); return o && o->kind == G_FONT ? &o->font : &stock(SYSTEM_FONT)->font; }

HDC aoe_create_surface_dc(void* owner, AoeSurfaceView (*view)(void*)) {
    Dc* d = new Dc(Dc::Surface);
    d->owner = owner;
    d->view = view;
    dcs.push_back(d);
    return (HDC) d;
}

void aoe_delete_dc(HDC h) {
    Dc* d = dc_of(h);
    if (!d) return;
    dcs.erase(std::find(dcs.begin(), dcs.end(), d));
    delete d;
}

// drawarea.cpp's software (DrawType 1) branch references this; the game build uses DirectDraw surfaces.
void* wasm_menu_dc() { return 0; }

extern "C" {

HDC GetDC(HWND h) {
    Dc* d = new Dc(h || aoe_primary_view(0) ? Dc::Window : Dc::Info);
    d->window = h;
    dcs.push_back(d);
    return (HDC) d;
}
int ReleaseDC(HWND, HDC h) { aoe_delete_dc(h); return 1; }
HDC CreateICA(LPCSTR, LPCSTR, LPCSTR, const DEVMODEA*) { Dc* d = new Dc(Dc::Info); dcs.push_back(d); return (HDC) d; }
BOOL DeleteDC(HDC h) { aoe_delete_dc(h); return TRUE; }

int GetDeviceCaps(HDC, int index) {
    switch (index) {
        case 8: return aoe_screen_width();   // HORZRES
        case 10: return aoe_screen_height(); // VERTRES
        case 12: return 8;                   // BITSPIXEL
        case 14: return 1;                   // PLANES
        case 38: return 0x0100;              // RASTERCAPS: RC_PALETTE
        case 88: case 90: return 96;         // LOGPIXELSX/Y
        case 104: return 256;                // SIZEPALETTE
        case 106: return 20;                 // NUMRESERVED
        case 108: return 18;                 // COLORRES
    }
    return 0;
}

HGDIOBJ GetStockObject(int index) { return (HGDIOBJ) stock(index); }

HFONT CreateFontIndirectA(const LOGFONTA* f) { GdiObj* o = make(G_FONT); o->font = *f; return (HFONT) o; }
HBRUSH CreateSolidBrush(COLORREF c) { GdiObj* o = make(G_BRUSH); o->color = c; return (HBRUSH) o; }
HPEN CreatePen(int style, int, COLORREF c) { GdiObj* o = make(G_PEN); o->color = c; o->hollow = style == 5 /* PS_NULL */; return (HPEN) o; }
HRGN CreateRectRgn(int l, int t, int r, int b) { GdiObj* o = make(G_REGION); SetRect(&o->rect, l, t, r, b); return (HRGN) o; }

HPALETTE CreatePalette(const LOGPALETTE* p) {
    GdiObj* o = make(G_PALETTE);
    o->entries.assign(p->palPalEntry, p->palPalEntry + p->palNumEntries);
    return (HPALETTE) o;
}

BOOL DeleteObject(HGDIOBJ h) {
    GdiObj* o = obj(h);
    if (!o) return FALSE;
    for (int i = 0; i < 20; i++) if (o == stock(i)) return TRUE; // stock objects are never deleted
    for (size_t i = 0; i < dcs.size(); i++) {
        if (dcs[i]->font == o) dcs[i]->font = 0;
        if (dcs[i]->brush == o) dcs[i]->brush = 0;
        if (dcs[i]->pen == o) dcs[i]->pen = 0;
        if (dcs[i]->palette == o) dcs[i]->palette = 0;
        if (dcs[i]->clip == o) dcs[i]->clip = 0;
    }
    objects.erase(std::find(objects.begin(), objects.end(), o));
    delete o;
    return TRUE;
}

HGDIOBJ SelectObject(HDC h, HGDIOBJ object) {
    Dc* d = dc_of(h);
    GdiObj* o = obj(object);
    if (!d || !o) return 0;
    GdiObj* old = 0;
    switch (o->kind) {
        case G_FONT: old = d->font; d->font = o; break;
        case G_BRUSH: old = d->brush; d->brush = o; break;
        case G_PEN: old = d->pen; d->pen = o; break;
        case G_REGION: d->clip = o; return (HGDIOBJ) 2; // SIMPLEREGION
        case G_PALETTE: old = d->palette; d->palette = o; break;
    }
    if (!old) old = o->kind == G_FONT ? stock(SYSTEM_FONT) : o->kind == G_BRUSH ? stock(WHITE_BRUSH) : o->kind == G_PEN ? stock(BLACK_PEN) : 0;
    return (HGDIOBJ) old;
}

int SelectClipRgn(HDC h, HRGN region) {
    Dc* d = dc_of(h);
    if (!d) return 0;
    GdiObj* o = obj(region);
    d->clip = o && o->kind == G_REGION ? o : 0;
    return d->clip ? 2 : 1;
}

int GetObjectA(HGDIOBJ h, int size, LPVOID out) {
    GdiObj* o = obj(h);
    if (!o) return 0;
    if (o->kind == G_PALETTE && size >= 2) { *(WORD*) out = (WORD) o->entries.size(); return 2; }
    if (o->kind == G_FONT && size >= (int) sizeof(LOGFONTA)) { memcpy(out, &o->font, sizeof(LOGFONTA)); return sizeof(LOGFONTA); }
    return 0;
}

COLORREF SetTextColor(HDC h, COLORREF c) { Dc* d = dc_of(h); if (!d) return 0; COLORREF old = d->text_color; d->text_color = c; return old; }
COLORREF SetBkColor(HDC h, COLORREF c) { Dc* d = dc_of(h); if (!d) return 0; COLORREF old = d->bk_color; d->bk_color = c; return old; }
int SetBkMode(HDC h, int mode) { Dc* d = dc_of(h); if (!d) return 0; int old = d->bk_mode; d->bk_mode = mode; return old; }

BOOL GetTextMetricsA(HDC h, TEXTMETRICA* out) {
    Dc* d = dc_of(h);
    if (!d || !out) return FALSE;
    int m[3];
    aoe_font_measure(font_of(d), "Mg", 2, m);
    int avg[3];
    aoe_font_measure(font_of(d), "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ", 52, avg);
    int wide[3];
    aoe_font_measure(font_of(d), "W", 1, wide);
    memset(out, 0, sizeof(*out));
    out->tmHeight = m[1];
    out->tmAscent = m[2];
    out->tmDescent = m[1] - m[2];
    out->tmAveCharWidth = (avg[0] + 26) / 52;
    out->tmMaxCharWidth = wide[0];
    out->tmWeight = font_of(d)->lfWeight;
    out->tmItalic = font_of(d)->lfItalic;
    out->tmFirstChar = 32;
    out->tmLastChar = 255;
    out->tmDefaultChar = '?';
    out->tmBreakChar = ' ';
    out->tmPitchAndFamily = 0x22; // variable pitch, swiss
    out->tmCharSet = 0;
    return TRUE;
}

BOOL GetTextExtentPoint32A(HDC h, LPCSTR text, int length, LPSIZE out) {
    Dc* d = dc_of(h);
    if (!d || !out) return FALSE;
    *out = measure(d, text, length);
    return TRUE;
}

BOOL TextOutA(HDC h, int x, int y, LPCSTR text, int length) {
    Dc* d = dc_of(h);
    if (!d) return FALSE;
    draw_text(d, x, y, text, length);
    return TRUE;
}

int FillRect(HDC h, const RECT* r, HBRUSH brush) {
    Dc* d = dc_of(h);
    GdiObj* b = obj(brush);
    if (!d || !r) return 0;
    if ((uintptr_t) brush > 0 && (uintptr_t) brush <= 31) b = 0; // COLOR_* + 1 system brushes: treat as black
    if (b && b->hollow) return 1;
    AoeSurfaceView v;
    int ox, oy;
    if (!view_of(d, &v, &ox, &oy)) return 1;
    RECT box = {r->left + ox, r->top + oy, r->right + ox, r->bottom + oy};
    fill(v, box, clip_rect(d, v, ox, oy), color_index(d, v, b ? b->color : 0));
    return 1;
}

BOOL MoveToEx(HDC h, int x, int y, LPPOINT old) {
    Dc* d = dc_of(h);
    if (!d) return FALSE;
    if (old) *old = d->pos;
    d->pos.x = x; d->pos.y = y;
    return TRUE;
}

// Bresenham line that excludes its last point, as GDI does.
BOOL LineTo(HDC h, int x, int y) {
    Dc* d = dc_of(h);
    if (!d) return FALSE;
    AoeSurfaceView v;
    int ox, oy;
    GdiObj* pen = d->pen ? d->pen : stock(BLACK_PEN);
    if (!pen->hollow && view_of(d, &v, &ox, &oy)) {
        RECT clip = clip_rect(d, v, ox, oy);
        int index = color_index(d, v, pen->color);
        int x0 = d->pos.x, y0 = d->pos.y, dx = abs(x - x0), dy = -abs(y - y0), sx = x0 < x ? 1 : -1, sy = y0 < y ? 1 : -1, err = dx + dy;
        while (x0 != x || y0 != y) {
            int px = x0 + ox, py = y0 + oy;
            if (px >= clip.left && px < clip.right && py >= clip.top && py < clip.bottom) v.bits[py * v.pitch + px] = (unsigned char) index;
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
    d->pos.x = x; d->pos.y = y;
    return TRUE;
}

// ---------------------------------------------------------------- DrawText
static int draw_text_lines(Dc* d, const char* text, int length, LPRECT rect, UINT format) {
    if (length < 0) length = strlen(text);
    std::vector<std::string> lines;
    std::string line;
    int width = rect->right - rect->left;
    for (int i = 0; i < length; i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n' && !(format & DT_SINGLELINE)) { lines.push_back(line); line.clear(); continue; }
        if (c == '&' && !(format & DT_NOPREFIX)) { if (i + 1 < length && text[i + 1] == '&') i++; else continue; }
        line += c;
        if ((format & DT_WORDBREAK) && !(format & DT_SINGLELINE) && measure(d, line.c_str(), line.size()).cx > width && line.size() > 1) {
            size_t split = line.find_last_of(' ');
            if (split == std::string::npos || split == 0) { lines.push_back(line.substr(0, line.size() - 1)); line = line.substr(line.size() - 1); }
            else { lines.push_back(line.substr(0, split)); line = line.substr(split + 1); }
        }
    }
    lines.push_back(line);
    int line_height = measure(d, "Mg", 2).cy, height = line_height * (int) lines.size();
    if (format & DT_CALCRECT) {
        int widest = 0;
        for (size_t i = 0; i < lines.size(); i++) widest = std::max(widest, (int) measure(d, lines[i].c_str(), lines[i].size()).cx);
        rect->right = rect->left + widest;
        rect->bottom = rect->top + height;
        return height;
    }
    int y = rect->top;
    if ((format & DT_SINGLELINE) && (format & DT_VCENTER)) y += (rect->bottom - rect->top - height) / 2;
    else if ((format & DT_SINGLELINE) && (format & DT_BOTTOM)) y = rect->bottom - height;
    GdiObj* saved_clip = d->clip;
    GdiObj box(G_REGION);
    if (!(format & 0x100 /* DT_NOCLIP */)) {
        box.rect = *rect;
        if (saved_clip) {
            box.rect.left = std::max(box.rect.left, saved_clip->rect.left); box.rect.top = std::max(box.rect.top, saved_clip->rect.top);
            box.rect.right = std::min(box.rect.right, saved_clip->rect.right); box.rect.bottom = std::min(box.rect.bottom, saved_clip->rect.bottom);
        }
        d->clip = &box;
    }
    for (size_t i = 0; i < lines.size(); i++) {
        int w = measure(d, lines[i].c_str(), lines[i].size()).cx, x = rect->left;
        if (format & DT_CENTER) x += (width - w) / 2;
        else if (format & DT_RIGHT) x = rect->right - w;
        draw_text(d, x, y, lines[i].c_str(), lines[i].size());
        y += line_height;
    }
    d->clip = saved_clip;
    return height;
}

int DrawTextA(HDC h, LPCSTR text, int length, LPRECT rect, UINT format) {
    Dc* d = dc_of(h);
    return d && text && rect ? draw_text_lines(d, text, length, rect, format) : 0;
}

int DrawTextExA(HDC h, LPSTR text, int length, LPRECT rect, UINT format, LPDRAWTEXTPARAMS) {
    return DrawTextA(h, text, length, rect, format & ~0x2000u /* DT_EDITCONTROL */);
}

// ---------------------------------------------------------------- palettes
HPALETTE SelectPalette(HDC h, HPALETTE p, BOOL) {
    Dc* d = dc_of(h);
    if (!d) return 0;
    GdiObj* old = d->palette;
    GdiObj* o = obj(p);
    d->palette = o && o->kind == G_PALETTE ? o : 0;
    return (HPALETTE) (old ? old : stock(DEFAULT_PALETTE));
}

UINT RealizePalette(HDC h) { Dc* d = dc_of(h); return d && d->palette ? (UINT) d->palette->entries.size() : 0; }

UINT GetPaletteEntries(HPALETTE p, UINT start, UINT count, LPPALETTEENTRY out) {
    GdiObj* o = obj(p);
    if (!o || o->kind != G_PALETTE) return 0;
    if (!out) return o->entries.size();
    if (start >= o->entries.size()) return 0;
    count = std::min(count, (UINT) o->entries.size() - start);
    memcpy(out, &o->entries[start], count * sizeof(*out));
    return count;
}

UINT SetPaletteEntries(HPALETTE p, UINT start, UINT count, const PALETTEENTRY* in) {
    GdiObj* o = obj(p);
    if (!o || o->kind != G_PALETTE || start >= o->entries.size()) return 0;
    count = std::min(count, (UINT) o->entries.size() - start);
    memcpy(&o->entries[start], in, count * sizeof(*in));
    return count;
}

BOOL ResizePalette(HPALETTE p, UINT count) {
    GdiObj* o = obj(p);
    if (!o || o->kind != G_PALETTE) return FALSE;
    PALETTEENTRY black = {0, 0, 0, 0};
    o->entries.resize(count, black);
    return TRUE;
}

UINT GetNearestPaletteIndex(HPALETTE p, COLORREF c) {
    GdiObj* o = obj(p);
    if (!o || o->kind != G_PALETTE || o->entries.empty()) return 0xffffffffu;
    return nearest(&o->entries[0], o->entries.size(), c & 0xffffff);
}

UINT GetSystemPaletteEntries(HDC, UINT start, UINT count, LPPALETTEENTRY out) {
    AoeSurfaceView v;
    if (!out) return 256;
    for (UINT i = 0; i < count && start + i < 256; i++) {
        if (aoe_primary_view(&v) && v.palette) out[i] = v.palette[start + i];
        else { PALETTEENTRY e = {0, 0, 0, 0}; out[i] = e; }
    }
    return count;
}

}
