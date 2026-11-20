// User32 subset for the real-WinMain build: window classes and windows, message delivery, focus,
// capture, keyboard state, timers, paint regions, the predefined "edit"/"static" controls, cursor and
// message boxes. Browser input arrives through aoe_input_* and becomes ordinary WM_* messages.
#include "port_prefix.h"
#include "win32_game.h"
#include <emscripten.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#ifndef WM_SETTEXT
#define WM_SETTEXT 0x000C
#endif
#ifndef WM_GETTEXT
#define WM_GETTEXT 0x000D
#endif
#ifndef WM_GETTEXTLENGTH
#define WM_GETTEXTLENGTH 0x000E
#endif
#ifndef EM_LIMITTEXT
#define EM_LIMITTEXT 0x00C5
#endif
#ifndef EM_REPLACESEL
#define EM_REPLACESEL 0x00C2
#endif
#ifndef WM_ERASEBKGND
#define WM_ERASEBKGND 0x0014
#endif
#ifndef WM_NCCREATE
#define WM_NCCREATE 0x0081
#endif

struct WndClass { std::string name; WNDPROC proc; UINT style; HCURSOR cursor; };
static std::map<std::string, WndClass> classes;

static LRESULT edit_proc(HWND, UINT, WPARAM, LPARAM);
static LRESULT static_proc(HWND, UINT, WPARAM, LPARAM);

static std::string lower(const char* s) { std::string r = s ? s : ""; for (size_t i = 0; i < r.size(); i++) if (r[i] >= 'A' && r[i] <= 'Z') r[i] += 32; return r; }

static std::vector<AoeWindow*> windows;
static HWND focus_window, capture_window, active_window;
// Two key states, as on Windows: async_state is the physical state (GetAsyncKeyState), updated when the
// browser reports the event; key_state is the thread's state (GetKeyState, GetKeyboardState, TranslateMessage),
// updated as the game takes each input message from its queue (aoe_input_retrieved). Bit 0x80: down; bit 1:
// toggled (Caps Lock, Num Lock, Scroll Lock).
static BYTE key_state[256], async_state[256];
static POINT cursor_pos = {0, 0};
static HCURSOR current_cursor;
static int screen_width = 800, screen_height = 600;
static bool page_hidden;

// HWNDs are small integers: recovered code compares (lParam & 0xFFFF) with window handles, as on Windows 9x.
static unsigned next_handle = 0x40;
AoeWindow* aoe_window(HWND h) {
    if (!h) return 0;
    for (size_t i = 0; i < windows.size(); i++) if (windows[i]->handle == h) return windows[i];
    return 0;
}
const std::vector<AoeWindow*>& aoe_windows() { return windows; }
HCURSOR aoe_current_cursor() { return current_cursor; }
void aoe_set_screen_size(int w, int h) { screen_width = w; screen_height = h; }
int aoe_screen_width() { return screen_width; }
int aoe_screen_height() { return screen_height; }

// Screen position of a window's client origin.
static POINT origin(AoeWindow* w) {
    POINT p = {0, 0};
    for (; w; w = aoe_window(w->parent)) { p.x += w->x; p.y += w->y; }
    return p;
}

static void invalidate(AoeWindow* w, const RECT* r) {
    RECT full = {0, 0, w->w, w->h};
    RECT add = r ? *r : full;
    if (!w->invalid) { w->update = add; w->invalid = true; }
    else {
        w->update.left = std::min(w->update.left, add.left);
        w->update.top = std::min(w->update.top, add.top);
        w->update.right = std::max(w->update.right, add.right);
        w->update.bottom = std::max(w->update.bottom, add.bottom);
    }
}

// ---------------------------------------------------------------- timers
struct Timer { HWND hwnd; UINT_PTR id; UINT ms; TIMERPROC proc; double due; };
static std::vector<Timer> timers;

// Double-click time and rectangle (Windows defaults); see aoe_input_mouse.
static const UINT DOUBLE_CLICK_MS = 500;
static const int DOUBLE_CLICK_W = 4, DOUBLE_CLICK_H = 4;

extern "C" {

ATOM RegisterClassA(const WNDCLASSA* c) {
    if (!c || !c->lpszClassName) return 0;
    WndClass k;
    k.name = lower(c->lpszClassName);
    k.proc = c->lpfnWndProc;
    k.style = c->style;
    k.cursor = c->hCursor;
    classes[k.name] = k;
    return (ATOM) classes.size();
}

HWND CreateWindowExA(DWORD exstyle, LPCSTR cls, LPCSTR text, DWORD style, int x, int y, int w, int h, HWND parent,
                     HMENU menu, HINSTANCE, LPVOID param) {
    std::string name = lower(cls);
    WNDPROC proc = 0;
    if (name == "edit") proc = edit_proc;
    else if (name == "static") proc = static_proc;
    else if (classes.count(name)) proc = classes[name].proc;
    else { printf("[aoe] CreateWindowExA: unknown class %s\n", cls); return 0; }
    AoeWindow* wnd = new AoeWindow;
    next_handle += 4;
    if (next_handle > 0xfff0) next_handle = 0x44;
    wnd->handle = (HWND) (uintptr_t) next_handle;
    wnd->cls = name;
    wnd->proc = proc;
    wnd->parent = parent;
    wnd->style = style;
    wnd->exstyle = exstyle;
    // Top-level windows have no non-client area here: the client is the whole window.
    wnd->x = x == (int) 0x80000000 ? 0 : x;
    wnd->y = y == (int) 0x80000000 ? 0 : y;
    wnd->w = w == (int) 0x80000000 ? screen_width : w;
    wnd->h = h == (int) 0x80000000 ? screen_height : h;
    wnd->text = text ? text : "";
    wnd->visible = (style & WS_VISIBLE) != 0;
    wnd->id = (int) (intptr_t) menu;
    windows.push_back(wnd);
    CREATESTRUCTA cs;
    memset(&cs, 0, sizeof(cs));
    cs.lpCreateParams = param;
    cs.hwndParent = parent;
    cs.cx = wnd->w; cs.cy = wnd->h; cs.x = wnd->x; cs.y = wnd->y;
    cs.style = style; cs.lpszName = text; cs.lpszClass = cls; cs.dwExStyle = exstyle;
    if (!SendMessageA(wnd->handle, WM_NCCREATE, 0, (LPARAM) &cs) && proc != edit_proc && proc != static_proc) {
        // DefWindowProc returns TRUE for WM_NCCREATE; a FALSE from the class procedure aborts creation.
    }
    if (SendMessageA(wnd->handle, WM_CREATE, 0, (LPARAM) &cs) == -1) { DestroyWindow(wnd->handle); return 0; }
    if (wnd->visible) invalidate(wnd, 0);
    return wnd->handle;
}

BOOL DestroyWindow(HWND h) {
    AoeWindow* w = aoe_window(h);
    if (!w) return FALSE;
    for (size_t i = 0; i < windows.size(); i++)
        if (windows[i]->parent == h) { DestroyWindow(windows[i]->handle); i = (size_t) -1; }
    SendMessageA(h, WM_DESTROY, 0, 0);
    if (focus_window == h) focus_window = 0;
    if (capture_window == h) capture_window = 0;
    if (active_window == h) active_window = 0;
    for (size_t i = 0; i < timers.size(); i++) if (timers[i].hwnd == h) { timers.erase(timers.begin() + i); i--; }
    windows.erase(std::find(windows.begin(), windows.end(), w));
    delete w;
    return TRUE;
}

BOOL IsWindow(HWND h) { return aoe_window(h) != 0; }
BOOL IsWindowVisible(HWND h) { AoeWindow* w = aoe_window(h); return w && w->visible; }
BOOL IsWindowEnabled(HWND h) { AoeWindow* w = aoe_window(h); return w && w->enabled; }
BOOL EnableWindow(HWND h, BOOL enable) { AoeWindow* w = aoe_window(h); if (!w) return FALSE; bool was = !w->enabled; w->enabled = enable != 0; return was; }
HWND GetParent(HWND h) { AoeWindow* w = aoe_window(h); return w ? w->parent : 0; }

BOOL ShowWindow(HWND h, int command) {
    AoeWindow* w = aoe_window(h);
    if (!w) return FALSE;
    bool was = w->visible;
    bool show = command != SW_HIDE;
    w->visible = show;
    if (show != was) SendMessageA(h, WM_SHOWWINDOW, show, 0);
    if (show && !was) {
        invalidate(w, 0);
        if (!w->parent) {
            // First activation of the top-level window, as Windows does on SW_SHOW.
            active_window = h;
            SendMessageA(h, WM_ACTIVATEAPP, 1, 0);
            SendMessageA(h, WM_ACTIVATE, 1 /* WA_ACTIVE */, 0);
            if (!focus_window) SetFocus(h);
            SendMessageA(h, WM_SIZE, 0, MAKELPARAM(w->w, w->h));
        }
    }
    return was;
}

BOOL UpdateWindow(HWND h) {
    AoeWindow* w = aoe_window(h);
    if (w && w->invalid && w->visible) SendMessageA(h, WM_PAINT, 0, 0);
    return w != 0;
}

BOOL InvalidateRect(HWND h, const RECT* r, BOOL) {
    if (!h) { for (size_t i = 0; i < windows.size(); i++) invalidate(windows[i], 0); return TRUE; }
    AoeWindow* w = aoe_window(h);
    if (!w) return FALSE;
    invalidate(w, r);
    return TRUE;
}

BOOL ValidateRect(HWND h, const RECT*) {
    AoeWindow* w = aoe_window(h);
    if (!w) return FALSE;
    w->invalid = false;
    return TRUE;
}

BOOL GetUpdateRect(HWND h, LPRECT r, BOOL) {
    AoeWindow* w = aoe_window(h);
    if (!w || !w->invalid) { if (r) SetRect(r, 0, 0, 0, 0); return FALSE; }
    if (r) *r = w->update;
    return TRUE;
}

BOOL GetClientRect(HWND h, LPRECT r) {
    AoeWindow* w = aoe_window(h);
    if (!r) return FALSE;
    if (!w) { SetRect(r, 0, 0, screen_width, screen_height); return h == 0; }
    SetRect(r, 0, 0, w->w, w->h);
    return TRUE;
}

BOOL GetWindowRect(HWND h, LPRECT r) {
    AoeWindow* w = aoe_window(h);
    if (!r) return FALSE;
    if (!w) { SetRect(r, 0, 0, screen_width, screen_height); return h == 0; }
    POINT o = origin(w);
    SetRect(r, o.x, o.y, o.x + w->w, o.y + w->h);
    return TRUE;
}

BOOL ClientToScreen(HWND h, LPPOINT p) {
    AoeWindow* w = aoe_window(h);
    if (!p) return FALSE;
    if (w) { POINT o = origin(w); p->x += o.x; p->y += o.y; }
    return TRUE;
}

BOOL ScreenToClient(HWND h, LPPOINT p) {
    AoeWindow* w = aoe_window(h);
    if (!p) return FALSE;
    if (w) { POINT o = origin(w); p->x -= o.x; p->y -= o.y; }
    return TRUE;
}

BOOL MoveWindow(HWND h, int x, int y, int w, int height, BOOL repaint) {
    AoeWindow* wnd = aoe_window(h);
    if (!wnd) return FALSE;
    bool resized = wnd->w != w || wnd->h != height;
    wnd->x = x; wnd->y = y; wnd->w = w; wnd->h = height;
    if (resized) SendMessageA(h, WM_SIZE, 0, MAKELPARAM(w, height));
    if (repaint) invalidate(wnd, 0);
    return TRUE;
}

BOOL SetWindowPos(HWND h, HWND, int x, int y, int w, int height, UINT flags) {
    AoeWindow* wnd = aoe_window(h);
    if (!wnd) return FALSE;
    if (!(flags & 0x0002 /* SWP_NOMOVE */)) { wnd->x = x; wnd->y = y; }
    if (!(flags & 0x0001 /* SWP_NOSIZE */) && (wnd->w != w || wnd->h != height)) {
        wnd->w = w; wnd->h = height;
        SendMessageA(h, WM_SIZE, 0, MAKELPARAM(w, height));
    }
    if (flags & 0x0040 /* SWP_SHOWWINDOW */) ShowWindow(h, SW_SHOW);
    if (flags & 0x0080 /* SWP_HIDEWINDOW */) ShowWindow(h, SW_HIDE);
    return TRUE;
}

LONG GetWindowLongA(HWND h, int index) {
    AoeWindow* w = aoe_window(h);
    if (!w) return 0;
    switch (index) {
        case GWL_WNDPROC: return (LONG) (intptr_t) w->proc;
        case GWL_USERDATA: return w->user;
        case GWL_STYLE: return (LONG) w->style;
        case GWL_EXSTYLE: return (LONG) w->exstyle;
        case GWL_ID: return w->id;
    }
    return 0;
}

LONG SetWindowLongA(HWND h, int index, LONG value) {
    AoeWindow* w = aoe_window(h);
    if (!w) return 0;
    LONG old = GetWindowLongA(h, index);
    switch (index) {
        case GWL_WNDPROC: w->proc = (WNDPROC) (intptr_t) value; break;
        case GWL_USERDATA: w->user = value; break;
        case GWL_STYLE: w->style = (DWORD) value; break;
        case GWL_EXSTYLE: w->exstyle = (DWORD) value; break;
        case GWL_ID: w->id = value; break;
    }
    return old;
}

DWORD SetClassLongA(HWND h, int index, LONG value) {
    AoeWindow* w = aoe_window(h);
    if (!w || index != GCL_HCURSOR || !classes.count(w->cls)) return 0;
    HCURSOR old = classes[w->cls].cursor;
    classes[w->cls].cursor = (HCURSOR) (intptr_t) value;
    return (DWORD) (intptr_t) old;
}

BOOL SetWindowTextA(HWND h, LPCSTR text) { return SendMessageA(h, WM_SETTEXT, 0, (LPARAM) text) != 0; }
int GetWindowTextA(HWND h, LPSTR out, int capacity) { return (int) SendMessageA(h, WM_GETTEXT, capacity, (LPARAM) out); }
int GetWindowTextLengthA(HWND h) { return (int) SendMessageA(h, WM_GETTEXTLENGTH, 0, 0); }

LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    AoeWindow* w = aoe_window(h);
    if (!w || !w->proc) return 0;
    return w->proc(h, msg, wp, lp);
}

LRESULT CallWindowProcA(FARPROC proc, HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    return proc ? ((WNDPROC) proc)(h, msg, wp, lp) : 0;
}

LRESULT DispatchMessageA(const MSG* msg) {
    if (msg->message == WM_TIMER && msg->lParam) {
        ((TIMERPROC) (intptr_t) msg->lParam)(msg->hwnd, WM_TIMER, msg->wParam, GetTickCount());
        return 0;
    }
    return SendMessageA(msg->hwnd, msg->message, msg->wParam, msg->lParam);
}

LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    AoeWindow* w = aoe_window(h);
    switch (msg) {
        case WM_NCCREATE: return TRUE;
        case WM_PAINT: if (w) w->invalid = false; return 0; // BeginPaint/EndPaint validate
        case WM_ERASEBKGND: return 1;
        case WM_SETTEXT: if (w) w->text = lp ? (const char*) lp : ""; return TRUE;
        case WM_GETTEXT: {
            if (!w || !wp) return 0;
            int n = std::min((int) w->text.size(), (int) wp - 1);
            memcpy((char*) lp, w->text.data(), n);
            ((char*) lp)[n] = 0;
            return n;
        }
        case WM_GETTEXTLENGTH: return w ? (LRESULT) w->text.size() : 0;
        case WM_CLOSE: DestroyWindow(h); return 0;
        case WM_SYSCOMMAND: return 0;
        case WM_SETCURSOR: return 0;
    }
    return 0;
}

// Windows posts WM_QUIT when the main window is destroyed by the application itself.
HWND GetFocus(void) { return focus_window; }
HWND SetFocus(HWND h) {
    HWND old = focus_window;
    if (h == old) return old;
    if (h && !aoe_window(h)) return 0;
    focus_window = h;
    if (old && aoe_window(old)) SendMessageA(old, WM_KILLFOCUS, (WPARAM) h, 0);
    if (h) SendMessageA(h, WM_SETFOCUS, (WPARAM) old, 0);
    return old;
}

HWND GetCapture(void) { return capture_window; }
HWND SetCapture(HWND h) { HWND old = capture_window; capture_window = h; return old; }
BOOL ReleaseCapture(void) { capture_window = 0; return TRUE; }

HWND GetActiveWindow(void) { return active_window; }
HWND GetForegroundWindow(void) { return active_window; }
BOOL SetForegroundWindow(HWND) { return TRUE; }
BOOL BringWindowToTop(HWND) { return TRUE; }
HWND GetLastActivePopup(HWND h) { return h; }
HWND FindWindowA(LPCSTR, LPCSTR) { return 0; }
DWORD GetWindowThreadProcessId(HWND, LPDWORD pid) { if (pid) *pid = 1; return 1; }
BOOL IsIconic(HWND) { return page_hidden; }

short GetKeyState(int vk) {
    BYTE s = key_state[vk & 255];
    return (short) (((s & 0x80) ? 0x8000 : 0) | (s & 1));
}
short GetAsyncKeyState(int vk) { return (async_state[vk & 255] & 0x80) ? (short) 0x8000 : 0; }
BOOL GetKeyboardState(PBYTE out) { if (!out) return FALSE; memcpy(out, key_state, 256); return TRUE; }

BOOL GetCursorPos(LPPOINT p) { if (!p) return FALSE; *p = cursor_pos; return TRUE; }
// The browser cannot move the system pointer; the game's own cursor follows its logical position.
BOOL SetCursorPos(int x, int y) { cursor_pos.x = x; cursor_pos.y = y; return TRUE; }
// The Windows cursor (menus, lobby, waiting). The game sets it to NULL while it draws its own pointer
// (the in-game view); the page shows the matching browser cursor (Module.aoeCursor).
EM_JS(void, aoe_host_cursor, (const char* css), { if (Module['aoeCursor']) Module['aoeCursor'](UTF8ToString(css)); });
static const char* cursor_css(HCURSOR c) {
    switch ((uintptr_t) c) {
        case 0: return "none";
        case 32513: return "text";          // IDC_IBEAM
        case 32514: return "wait";          // IDC_WAIT
        case 32515: return "crosshair";     // IDC_CROSS
        case 32650: return "progress";      // IDC_APPSTARTING
        case 32648: return "not-allowed";   // IDC_NO
        default: return "default";          // IDC_ARROW and anything else
    }
}
HCURSOR SetCursor(HCURSOR c) {
    HCURSOR old = current_cursor;
    current_cursor = c;
    static const char* shown;
    const char* css = cursor_css(c);
    if (css != shown) { shown = css; aoe_host_cursor(css); }
    return old;
}
HCURSOR LoadCursorA(HINSTANCE, LPCSTR name) { return (HCURSOR) (uintptr_t) ((uintptr_t) name <= 0xffff ? (uintptr_t) name : 0x10000 + strlen(name)); }
HICON LoadIconA(HINSTANCE, LPCSTR) { return (HICON) 1; }
int ShowCursor(BOOL show) { static int count = 0; count += show ? 1 : -1; return count; }

UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, TIMERPROC proc) {
    for (size_t i = 0; i < timers.size(); i++)
        if (timers[i].hwnd == h && timers[i].id == id && h) { timers[i].ms = ms; timers[i].proc = proc; timers[i].due = emscripten_get_now() + ms; return id; }
    Timer t;
    t.hwnd = h; t.id = h ? id : timers.size() + 1; t.ms = ms ? ms : 1; t.proc = proc; t.due = emscripten_get_now() + t.ms;
    timers.push_back(t);
    return t.id;
}

BOOL KillTimer(HWND h, UINT_PTR id) {
    for (size_t i = 0; i < timers.size(); i++)
        if (timers[i].hwnd == h && timers[i].id == id) { timers.erase(timers.begin() + i); return TRUE; }
    return FALSE;
}

int GetSystemMetrics(int index) {
    switch (index) {
        case SM_CXSCREEN: return screen_width;
        case SM_CYSCREEN: return screen_height;
        case 2: case 20: return 16;  // SM_CXVSCROLL / SM_CYVSCROLL
        case 32: case 33: return 4;  // SM_CXFRAME / SM_CYFRAME
        case 4: return 19;           // SM_CYCAPTION
        case 43: return 3;           // SM_CMOUSEBUTTONS
        case 19: return 1;           // SM_MOUSEPRESENT
        case 36: return DOUBLE_CLICK_W; // SM_CXDOUBLECLK
        case 37: return DOUBLE_CLICK_H; // SM_CYDOUBLECLK
    }
    return 0;
}

BOOL SystemParametersInfoA(UINT action, UINT, PVOID out, UINT) {
    if (out && (action == 16 /* SPI_GETSCREENSAVEACTIVE */ || action == 83 /* SPI_GETLOWPOWERACTIVE */)) *(BOOL*) out = FALSE;
    return TRUE;
}
BOOL SetSysColors(int, const INT*, const COLORREF*) { return TRUE; }
UINT GetCaretBlinkTime(void) { return 530; }
UINT GetDoubleClickTime(void) { return DOUBLE_CLICK_MS; }

// Windows 98 standard scheme.
COLORREF GetSysColor(int index) {
    switch (index) {
        case 5: case 15 /* COLOR_WINDOW, COLOR_BTNFACE */: return index == 5 ? 0xffffff : 0xc0c0c0;
        case 8: case 18 /* COLOR_WINDOWTEXT, COLOR_BTNTEXT */: return 0;
        case 13 /* COLOR_HIGHLIGHT */: return 0x800000;
        case 14 /* COLOR_HIGHLIGHTTEXT */: return 0xffffff;
        case 16 /* COLOR_BTNSHADOW */: return 0x808080;
        case 20 /* COLOR_BTNHIGHLIGHT */: return 0xffffff;
    }
    return 0xc0c0c0;
}
BOOL MessageBeep(UINT) { return TRUE; }

// Clipboard: empty. (Paste support can come from the browser later.)
BOOL OpenClipboard(HWND) { return TRUE; }
BOOL CloseClipboard(void) { return TRUE; }
BOOL IsClipboardFormatAvailable(UINT) { return FALSE; }
HANDLE GetClipboardData(UINT) { return 0; }

// Text composition is handled by the browser; there is no Windows input method context.
HIMC ImmAssociateContext(HWND, HIMC) { return 0; }
BOOL ImmNotifyIME(HIMC, DWORD, DWORD, DWORD) { return TRUE; }
BOOL ImmReleaseContext(HWND, HIMC) { return TRUE; }
BOOL ImmSetOpenStatus(HIMC, BOOL) { return TRUE; }

// WinHelp: the page shows the converted help file (tools/convert_help.py) over the game. Commands used by
// the game: HELP_KEY (a keyword from a Help button), HELP_CONTENTS/HELP_FINDER, HELP_QUIT.
EM_JS(int, aoe_host_help, (const char* file, int command, const char* key), {
    if (!Module['aoeHelp']) return 0;
    Module['aoeHelp'](UTF8ToString(file), command, key ? UTF8ToString(key) : '');
    return 1;
});
BOOL WinHelpA(HWND, LPCSTR file, UINT command, ULONG_PTR data) {
    const UINT HELP_KEY_ = 0x0101, HELP_PARTIALKEY_ = 0x0105, HELP_MULTIKEY_ = 0x0201;
    const char* key = (command == HELP_KEY_ || command == HELP_PARTIALKEY_) && data ? (const char*) data : 0;
    if (command == HELP_MULTIKEY_) key = 0;
    if (!aoe_host_help(file ? file : "", (int) command, key)) {
        printf("[aoe] WinHelp not available: %s\n", file ? file : "");
        return FALSE;
    }
    return TRUE;
}

}

EM_JS(void, aoe_host_message_box, (const char* text, const char* title), {
    const t = UTF8ToString(text), c = UTF8ToString(title);
    if (Module['onMessageBox']) Module['onMessageBox'](c, t);
});

extern "C" int MessageBoxA(HWND, LPCSTR text, LPCSTR title, UINT) {
    printf("[aoe] MessageBox: %s: %s\n", title ? title : "", text ? text : "");
    aoe_host_message_box(text ? text : "", title ? title : "");
    return 1; // IDOK
}

// ---------------------------------------------------------------- keyboard translation (US layout)
static int to_ascii(UINT vk, bool shift, bool caps, bool ctrl) {
    if (ctrl) { if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 1; return 0; }
    if (vk >= 'A' && vk <= 'Z') return (shift != caps) ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') return shift ? ")!@#$%^&*("[vk - '0'] : vk;
    if (vk >= 0x60 && vk <= 0x69) return '0' + vk - 0x60; // numpad
    switch (vk) {
        case 0x20: return ' ';
        case 0x08: return 8;
        case 0x0D: return 13;
        case 0x1B: return 27;
        case 0x09: return 9;
        case 0x6A: return '*'; case 0x6B: return '+'; case 0x6D: return '-'; case 0x6E: return '.'; case 0x6F: return '/';
        case 0xBA: return shift ? ':' : ';';
        case 0xBB: return shift ? '+' : '=';
        case 0xBC: return shift ? '<' : ',';
        case 0xBD: return shift ? '_' : '-';
        case 0xBE: return shift ? '>' : '.';
        case 0xBF: return shift ? '?' : '/';
        case 0xC0: return shift ? '~' : '`';
        case 0xDB: return shift ? '{' : '[';
        case 0xDC: return shift ? '|' : '\\';
        case 0xDD: return shift ? '}' : ']';
        case 0xDE: return shift ? '"' : '\'';
    }
    return 0;
}

// Characters the browser produced for posted key presses (its keyboard layout), in posting order.
struct PendingChar { UINT vk; LPARAM lp; int ch; };
static std::vector<PendingChar> pending_chars;

extern "C" BOOL TranslateMessage(const MSG* msg) {
    if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return FALSE;
    bool shift = (key_state[VK_SHIFT] & 0x80) != 0, caps = (key_state[VK_CAPITAL] & 1) != 0;
    bool ctrl = (key_state[VK_CONTROL] & 0x80) != 0, alt = (key_state[VK_MENU] & 0x80) != 0;
    int browser_char = -1;
    for (size_t i = 0; i < pending_chars.size(); i++) {
        if (pending_chars[i].vk != msg->wParam || pending_chars[i].lp != msg->lParam) continue;
        browser_char = pending_chars[i].ch;
        pending_chars.erase(pending_chars.begin(), pending_chars.begin() + i + 1);
        break;
    }
    if (alt && !ctrl) return FALSE;
    int c = ctrl || browser_char <= 0 ? to_ascii(msg->wParam, shift, caps, ctrl && !alt) : browser_char;
    if (!c) return FALSE;
    PostMessageA(msg->hwnd, msg->message == WM_SYSKEYDOWN ? 0x0106 /* WM_SYSCHAR */ : WM_CHAR, c, msg->lParam);
    return TRUE;
}

// ---------------------------------------------------------------- synthesized messages
// Called by PeekMessage/GetMessage after the posted queue: WM_PAINT, then WM_TIMER (Windows priority).
bool aoe_user_synthesize(MSG* out, HWND hwnd, UINT first, UINT last, bool remove) {
    double now = emscripten_get_now();
    for (size_t i = 0; i < windows.size(); i++) {
        AoeWindow* w = windows[i];
        if (!w->invalid || !w->visible || (hwnd && w->handle != hwnd)) continue;
        if ((first || last) && (WM_PAINT < first || WM_PAINT > last)) continue;
        memset(out, 0, sizeof(*out));
        out->hwnd = w->handle;
        out->message = WM_PAINT;
        out->time = (DWORD) now;
        return true; // WM_PAINT stays pending until the window validates itself
    }
    if ((first || last) && (WM_TIMER < first || WM_TIMER > last)) return false;
    for (size_t i = 0; i < timers.size(); i++) {
        Timer& t = timers[i];
        if (t.due > now || (hwnd && t.hwnd != hwnd)) continue;
        memset(out, 0, sizeof(*out));
        out->hwnd = t.hwnd;
        out->message = WM_TIMER;
        out->wParam = t.id;
        out->lParam = (LPARAM) (intptr_t) t.proc;
        out->time = (DWORD) now;
        if (remove) t.due = now + t.ms;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------- browser input
static AoeWindow* top_level() {
    for (size_t i = 0; i < windows.size(); i++) if (!windows[i]->parent && windows[i]->visible) return windows[i];
    return 0;
}

// Deepest visible child of `w` under screen point p.
static AoeWindow* hit(AoeWindow* w, POINT p) {
    for (size_t i = windows.size(); i-- > 0;) {
        AoeWindow* c = windows[i];
        if (c->parent != w->handle || !c->visible) continue;
        POINT o = origin(c);
        if (p.x >= o.x && p.y >= o.y && p.x < o.x + c->w && p.y < o.y + c->h) return hit(c, p);
    }
    return w;
}

// wParam of mouse messages: buttons and modifiers held when the event happened.
static WPARAM mouse_keys() {
    WPARAM k = 0;
    if (async_state[VK_LBUTTON] & 0x80) k |= MK_LBUTTON;
    if (async_state[VK_RBUTTON] & 0x80) k |= MK_RBUTTON;
    if (async_state[VK_MBUTTON] & 0x80) k |= 0x0010;
    if (async_state[VK_SHIFT] & 0x80) k |= MK_SHIFT;
    if (async_state[VK_CONTROL] & 0x80) k |= MK_CONTROL;
    return k;
}

// win32_loop.cpp: queues browser input like hardware input.
void aoe_post_input(HWND hwnd, UINT message, WPARAM w, LPARAM l);

static void set_down(BYTE* state, int vk, bool down) { if (down) state[vk] |= 0x80; else state[vk] &= ~0x80; }

// Called when the game removes a browser input message from its queue: the thread's key state changes then.
void aoe_input_retrieved(const MSG& m) {
    switch (m.message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
            int vk = m.wParam & 255;
            bool down = m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN;
            bool extended = (m.lParam >> 24) & 1;
            if (down && !(key_state[vk] & 0x80) && (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)) key_state[vk] ^= 1;
            set_down(key_state, vk, down);
            // Generic modifiers also set their left/right variant (scan code 0x36 is the right Shift).
            if (vk == VK_SHIFT) set_down(key_state, ((m.lParam >> 16) & 255) == 0x36 ? 0xA1 : 0xA0, down);
            if (vk == VK_CONTROL) set_down(key_state, extended ? 0xA3 : 0xA2, down);
            if (vk == VK_MENU) set_down(key_state, extended ? 0xA5 : 0xA4, down);
            break;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: set_down(key_state, VK_LBUTTON, true); break;
        case WM_LBUTTONUP: set_down(key_state, VK_LBUTTON, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: set_down(key_state, VK_RBUTTON, true); break;
        case WM_RBUTTONUP: set_down(key_state, VK_RBUTTON, false); break;
        case 0x0207: case 0x0209: set_down(key_state, VK_MBUTTON, true); break; // WM_MBUTTONDOWN / DBLCLK
        case 0x0208: set_down(key_state, VK_MBUTTON, false); break;              // WM_MBUTTONUP
    }
}

extern "C" {

// Windows' double-click detection (the browser's pointer events carry no click count): a button press is a
// double click when the previous press was the same button in the same window, at most GetDoubleClickTime()
// earlier and inside the SM_CXDOUBLECLK x SM_CYDOUBLECLK rectangle around it. Only windows whose class has
// CS_DBLCLKS receive WM_xBUTTONDBLCLK; others get a plain button-down. The press after a double click
// starts a new sequence.
static struct { int button; HWND window; POINT at; double time; bool armed; } last_press = {-1, 0, {0, 0}, 0, false};

static bool wants_double_clicks(AoeWindow* w) {
    if (w->cls == "edit") return true; // the system Edit class registers CS_DBLCLKS
    std::map<std::string, WndClass>::iterator c = classes.find(w->cls);
    return c != classes.end() && (c->second.style & CS_DBLCLKS);
}

// kind: 0 move, 1 down, 2 up; button: 0 left, 1 middle, 2 right (DOM numbering). time: when the browser saw
// the event, on emscripten_get_now()'s clock (0 or NaN: now), so a busy frame doesn't change click timing.
EMSCRIPTEN_KEEPALIVE void aoe_input_mouse(int kind, int button, int x, int y, double time) {
    cursor_pos.x = x;
    cursor_pos.y = y;
    static const UINT down[3] = {WM_LBUTTONDOWN, 0x0207 /* WM_MBUTTONDOWN */, WM_RBUTTONDOWN};
    static const UINT up[3] = {WM_LBUTTONUP, 0x0208 /* WM_MBUTTONUP */, WM_RBUTTONUP};
    static const UINT dbl[3] = {WM_LBUTTONDBLCLK, 0x0209, WM_RBUTTONDBLCLK};
    static const int vk[3] = {VK_LBUTTON, VK_MBUTTON, VK_RBUTTON};
    button = button < 0 || button > 2 ? 0 : button;
    AoeWindow* root = top_level();
    if (!root) return;
    POINT p = {x, y};
    AoeWindow* target = capture_window && aoe_window(capture_window) ? aoe_window(capture_window) : hit(root, p);
    if (!target->enabled) target = root;
    UINT msg = WM_MOUSEMOVE;
    if (kind == 1) {
        async_state[vk[button]] |= 0x80;
        msg = down[button];
        double now = time > 0 ? time : emscripten_get_now();
        bool is_double = last_press.armed && last_press.button == button && last_press.window == target->handle
                         && now - last_press.time <= DOUBLE_CLICK_MS && abs(x - last_press.at.x) <= DOUBLE_CLICK_W / 2
                         && abs(y - last_press.at.y) <= DOUBLE_CLICK_H / 2;
        if (is_double && wants_double_clicks(target)) msg = dbl[button];
        last_press.button = button;
        last_press.window = target->handle;
        last_press.at = p;
        last_press.time = now;
        last_press.armed = !is_double;
    }
    if (kind == 2) { async_state[vk[button]] &= ~0x80; msg = up[button]; }
    POINT o = origin(target);
    aoe_post_input(target->handle, msg, mouse_keys(), MAKELPARAM(x - o.x, y - o.y));
}

// vk: Windows virtual-key code; scan: scan code; down: 1/0; extended: 1 for extended keys;
// ch: Windows-1252 character the browser produced for this press (0 if none).
EMSCRIPTEN_KEEPALIVE void aoe_input_key(int vk, int scan, int down, int extended, int ch) {
    vk &= 255;
    bool repeat = down && (async_state[vk] & 0x80);
    if (down && !repeat && (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)) async_state[vk] ^= 1;
    set_down(async_state, vk, down);
    bool alt = (async_state[VK_MENU] & 0x80) != 0;
    bool sys = (alt && !(async_state[VK_CONTROL] & 0x80)) || vk == VK_F10 || vk == VK_MENU;
    UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    LPARAM lp = 1 | ((scan & 0xff) << 16) | (extended ? 1 << 24 : 0) | (alt ? 1 << 29 : 0) | (repeat ? 1 << 30 : 0) | (down ? 0 : (3u << 30));
    HWND target = focus_window && aoe_window(focus_window) ? focus_window : (top_level() ? top_level()->handle : 0);
    if (!target) return;
    if (down && ch > 0) {
        PendingChar p = {(UINT) vk, lp, ch & 0xff};
        pending_chars.push_back(p);
        if (pending_chars.size() > 64) pending_chars.erase(pending_chars.begin());
    }
    aoe_post_input(target, msg, vk, lp);
}

// Corrects modifier state from the flags of a browser event, without generating messages. Only a state the
// page never saw change (a key pressed or released in another window) is corrected, in both key states; presses
// the page did see arrive as key messages and reach the thread state when the game reads them.
static void correct_modifier(int vk, bool down) {
    if (((async_state[vk] & 0x80) != 0) == down) return;
    set_down(async_state, vk, down);
    set_down(key_state, vk, down);
}
EMSCRIPTEN_KEEPALIVE void aoe_input_modifiers(int shift, int ctrl, int alt, int caps) {
    correct_modifier(VK_SHIFT, shift);
    correct_modifier(VK_CONTROL, ctrl);
    correct_modifier(VK_MENU, alt);
    if ((async_state[VK_CAPITAL] & 1) != (caps ? 1 : 0)) { async_state[VK_CAPITAL] ^= 1; key_state[VK_CAPITAL] ^= 1; }
}

// Structured window queries for tests: the n-th window of a class (0 if none), its text and state.
EMSCRIPTEN_KEEPALIVE HWND aoe_find_window(const char* cls, int n) {
    std::string want = lower(cls);
    for (size_t i = 0; i < windows.size(); i++) if (windows[i]->cls == want && n-- == 0) return windows[i]->handle;
    return 0;
}
EMSCRIPTEN_KEEPALIVE const char* aoe_window_text(HWND h) { AoeWindow* w = aoe_window(h); return w ? w->text.c_str() : ""; }
// field: 0 visible, 1 has focus, 2 x, 3 y, 4 width, 5 height
EMSCRIPTEN_KEEPALIVE int aoe_window_state(HWND h, int field) {
    AoeWindow* w = aoe_window(h);
    if (!w) return -1;
    switch (field) {
        case 0: return w->visible;
        case 1: return focus_window == h;
        case 2: return origin(w).x;
        case 3: return origin(w).y;
        case 4: return w->w;
        case 5: return w->h;
    }
    return -1;
}

// Diagnostics for tests: one line per window.
EMSCRIPTEN_KEEPALIVE void aoe_debug_windows(void) {
    for (size_t i = 0; i < windows.size(); i++) {
        AoeWindow* w = windows[i];
        printf("[aoe] hwnd=%p parent=%p class=%s rect=%d,%d %dx%d visible=%d focused=%d invalid=%d sel=%d-%d text=\"%s\"%s\n", w->handle, w->parent,
               w->cls.c_str(), w->x, w->y, w->w, w->h, w->visible, w->focused, w->invalid, w->sel_start, w->sel_end, w->text.c_str(),
               focus_window == w->handle ? " [focus]" : "");
    }
}

// Releases every held key/button (e.g. when the page loses focus), as Windows does on deactivation.
EMSCRIPTEN_KEEPALIVE void aoe_input_reset(void) {
    // Toggle states (bit 1) survive; held keys and buttons are released.
    for (int i = 0; i < 256; i++) { key_state[i] &= 1; async_state[i] &= 1; }
    capture_window = 0;
}

EMSCRIPTEN_KEEPALIVE void aoe_set_page_active(int active) {
    page_hidden = !active;
    AoeWindow* root = top_level();
    if (!root) return;
    if (!active) aoe_input_reset();
    PostMessageA(root->handle, WM_ACTIVATEAPP, active ? 1 : 0, 0);
    if (active) InvalidateRect(root->handle, 0, TRUE);
}

}

void aoe_child_clip_rects(HWND parent, std::vector<RECT>* out) {
    AoeWindow* p = aoe_window(parent);
    if (!p || !(p->style & WS_CLIPCHILDREN)) return;
    for (size_t i = 0; i < windows.size(); i++) {
        AoeWindow* c = windows[i];
        if (c->parent != parent || !c->visible) continue;
        POINT o = origin(c);
        RECT r = {o.x, o.y, o.x + c->w, o.y + c->h};
        out->push_back(r);
    }
}

bool aoe_paint_children() {
    bool painted = false;
    for (size_t i = 0; i < windows.size(); i++) {
        AoeWindow* w = windows[i];
        if (!w->parent || !w->invalid || !w->visible) continue;
        SendMessageA(w->handle, WM_PAINT, 0, 0);
        w->invalid = false;
        painted = true;
    }
    return painted;
}

// ---------------------------------------------------------------- predefined controls
static LRESULT static_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcA(h, msg, wp, lp); }

static void edit_set_sel(AoeWindow* w, int start, int end) {
    int n = (int) w->text.size();
    if (start == -1) { w->sel_start = w->sel_end = std::min(std::max(w->sel_end, 0), n); return; }
    if (end == -1) end = n;
    w->sel_start = std::min(std::max(start, 0), n);
    w->sel_end = std::min(std::max(end, 0), n);
}

static void edit_replace(AoeWindow* w, const std::string& with) {
    int a = std::min(w->sel_start, w->sel_end), b = std::max(w->sel_start, w->sel_end);
    std::string next = w->text.substr(0, a) + with + w->text.substr(b);
    if (w->limit && next.size() > w->limit) return; // Windows rejects input past the limit
    w->undo = w->text;
    w->text = next;
    w->sel_start = w->sel_end = a + (int) with.size();
    invalidate(w, 0);
}

static bool caret_visible() { return ((unsigned) (emscripten_get_now() / GetCaretBlinkTime()) & 1) == 0; }

// Paints like a Windows edit control: background and colors from WM_CTLCOLOREDIT (answered by the
// game's TEditPanel), text, selection in highlight colors, and a blinking caret while focused.
static void edit_paint(AoeWindow* w) {
    w->invalid = false;
    if (!w->visible) return;
    HDC dc = GetDC(w->handle);
    RECT rc = {0, 0, w->w, w->h};
    SetBkColor(dc, 0xffffff);
    SetTextColor(dc, 0);
    SetBkMode(dc, OPAQUE);
    HBRUSH brush = (HBRUSH) SendMessageA(w->parent, 0x0133 /* WM_CTLCOLOREDIT */, (WPARAM) dc, (LPARAM) w->handle);
    if (!SendMessageA(w->handle, WM_ERASEBKGND, (WPARAM) dc, 0)) FillRect(dc, &rc, brush ? brush : (HBRUSH) GetStockObject(WHITE_BRUSH));
    HGDIOBJ old_font = w->font ? SelectObject(dc, w->font) : 0;
    COLORREF text = SetTextColor(dc, 0), back = SetBkColor(dc, 0);
    SetTextColor(dc, text);
    SetBkColor(dc, back);
    int a = std::min(w->sel_start, w->sel_end), b = std::max(w->sel_start, w->sel_end);
    const char* s = w->text.c_str();
    SIZE pre, mid;
    GetTextExtentPoint32A(dc, s, a, &pre);
    GetTextExtentPoint32A(dc, s + a, b - a, &mid);
    SetBkMode(dc, TRANSPARENT);
    TextOutA(dc, 0, 0, s, a);
    if (b > a && w->focused) {
        RECT sel = {pre.cx, 0, pre.cx + mid.cx, w->h};
        HBRUSH highlight = CreateSolidBrush(0x800000); // COLOR_HIGHLIGHT
        FillRect(dc, &sel, highlight);
        DeleteObject(highlight);
        SetTextColor(dc, 0xffffff);
        TextOutA(dc, pre.cx, 0, s + a, b - a);
        SetTextColor(dc, text);
    } else TextOutA(dc, pre.cx, 0, s + a, b - a);
    TextOutA(dc, pre.cx + mid.cx, 0, s + b, (int) w->text.size() - b);
    if (w->focused && caret_visible()) {
        SIZE upto, line;
        GetTextExtentPoint32A(dc, s, w->sel_end, &upto);
        GetTextExtentPoint32A(dc, "Mg", 2, &line);
        HPEN pen = CreatePen(PS_SOLID, 1, text);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        MoveToEx(dc, upto.cx, 0, 0);
        LineTo(dc, upto.cx, std::min((int) line.cy, w->h));
        SelectObject(dc, old_pen);
        DeleteObject(pen);
    }
    if (old_font) SelectObject(dc, old_font);
    ReleaseDC(w->handle, dc);
}

// Called once per presented frame: repaint focused edit controls when the caret blinks.
void aoe_user_frame() {
    static bool shown = true;
    bool now = caret_visible();
    if (now == shown) return;
    shown = now;
    for (size_t i = 0; i < windows.size(); i++) if (windows[i]->focused && windows[i]->visible && windows[i]->cls == "edit") invalidate(windows[i], 0);
}

// Single-line edit control behavior used by TEditPanel (typing, selection, undo, limits).
static LRESULT edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    AoeWindow* w = aoe_window(h);
    if (!w) return 0;
    switch (msg) {
        case WM_SETTEXT: w->text = lp ? (const char*) lp : ""; w->sel_start = w->sel_end = 0; w->undo = w->text; invalidate(w, 0); return TRUE;
        case WM_SETFONT: w->font = (HFONT) wp; if (lp) invalidate(w, 0); return 0;
        case 0x0031 /* WM_GETFONT */: return (LRESULT) w->font;
        case EM_GETSEL:
            if (wp) *(DWORD*) wp = std::min(w->sel_start, w->sel_end);
            if (lp) *(DWORD*) lp = std::max(w->sel_start, w->sel_end);
            return MAKELPARAM(std::min(w->sel_start, w->sel_end), std::max(w->sel_start, w->sel_end));
        case EM_SETSEL: edit_set_sel(w, (int) wp, (int) lp); invalidate(w, 0); return 0;
        case EM_LIMITTEXT: w->limit = wp; return 0;
        case EM_REPLACESEL: edit_replace(w, lp ? (const char*) lp : ""); return 0;
        case EM_UNDO: case 0x0304 /* WM_UNDO */: { std::string t = w->text; w->text = w->undo; w->undo = t; edit_set_sel(w, (int) w->text.size(), -1); invalidate(w, 0); return TRUE; }
        case 0x00C6 /* EM_CANUNDO */: return w->undo != w->text;
        case WM_SETFOCUS: w->focused = true; invalidate(w, 0); return 0;
        case WM_KILLFOCUS: w->focused = false; invalidate(w, 0); return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: SetFocus(h); return 0;
        case WM_CHAR: {
            int c = (int) (wp & 0xff);
            if (c == 8) {
                if (w->sel_start == w->sel_end && w->sel_start > 0) w->sel_start--;
                edit_replace(w, "");
            } else if (c == 1) { // Ctrl+A
                edit_set_sel(w, 0, -1);
            } else if (c >= 32 || c >= 0x80) {
                if ((w->style & 0x2000 /* ES_NUMBER */) && (c < '0' || c > '9')) return 0;
                edit_replace(w, std::string(1, (char) c));
            }
            return 0;
        }
        case WM_KEYDOWN: {
            int n = (int) w->text.size();
            bool shift = (key_state[VK_SHIFT] & 0x80) != 0;
            int caret = w->sel_end, anchor = w->sel_start;
            switch (wp) {
                case VK_LEFT: caret = std::max(0, caret - 1); break;
                case VK_RIGHT: caret = std::min(n, caret + 1); break;
                case VK_HOME: caret = 0; break;
                case VK_END: caret = n; break;
                case VK_DELETE:
                    if (w->sel_start == w->sel_end && w->sel_end < n) w->sel_end++;
                    edit_replace(w, "");
                    return 0;
                default: return 0;
            }
            w->sel_end = caret;
            w->sel_start = shift ? anchor : caret;
            invalidate(w, 0);
            return 0;
        }
        case WM_PAINT: edit_paint(w); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}
