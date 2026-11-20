// Internal interfaces between the platform modules of the real-WinMain build.
#pragma once
#include "port_prefix.h"
#include "win32_extra.h"
#include <string>
#include <vector>

struct AoeWindow {
    HWND handle;
    std::string cls, text, undo;
    WNDPROC proc;
    HWND parent;
    DWORD style, exstyle;
    int x, y, w, h;          // relative to the parent's client area (screen for top-level windows)
    bool visible, enabled, invalid, focused;
    RECT update;
    LONG user;
    int id;
    HFONT font;              // edit controls: WM_SETFONT
    int sel_start, sel_end;
    size_t limit;
    AoeWindow() : handle(0), proc(0), parent(0), style(0), exstyle(0), x(0), y(0), w(0), h(0), visible(false), enabled(true),
                  invalid(false), focused(false), user(0), id(0), font(0), sel_start(0), sel_end(0), limit(0) {
        update.left = update.top = update.right = update.bottom = 0;
    }
};

// win32_user.cpp
AoeWindow* aoe_window(HWND h);
const std::vector<AoeWindow*>& aoe_windows();
HCURSOR aoe_current_cursor();
void aoe_set_screen_size(int w, int h);
int aoe_screen_width();
int aoe_screen_height();
bool aoe_user_synthesize(MSG* out, HWND hwnd, UINT first, UINT last, bool remove);
void aoe_user_frame();
bool aoe_paint_children();   // sends WM_PAINT to invalid visible child windows; true if any painted
// Visible child windows of `parent` that DirectDraw blits must not cover (WS_CLIPCHILDREN), in screen coordinates.
void aoe_child_clip_rects(HWND parent, std::vector<RECT>* out);

// win32_gdi.cpp
struct AoeSurfaceView { unsigned char* bits; int width, height, pitch; const PALETTEENTRY* palette; };
HDC aoe_create_surface_dc(void* owner, AoeSurfaceView (*view)(void* owner));
void aoe_delete_dc(HDC dc);
const LOGFONTA* aoe_font_info(HFONT font);

// win32_media.cpp
void aoe_media_poll();   // posts MM_MCINOTIFY when a MIDI file ends

// win32_ddraw.cpp
bool aoe_primary_view(AoeSurfaceView* out);   // the screen as the game last drew it
void aoe_mark_primary_dirty();
void aoe_present_if_dirty();
