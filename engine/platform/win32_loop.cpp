// Win32 message queue shared by the probe and the real-WinMain ("game") builds.
//
// The original RGE_Base_Game::run() is an endless PeekMessage/GetMessage loop. In the game build
// (AOE_GAME_LOOP) these calls yield to the browser through Emscripten Asyncify, so the recovered
// loop runs unmodified. The probe build never enters run(); it only posts messages.
#include "port_prefix.h"
#include <deque>
#include <sys/stat.h>
#include <emscripten.h>

static std::deque<MSG> posted_messages;
static std::deque<bool> posted_input; // parallel to posted_messages: true for mouse/keyboard input from the browser

static bool matches(const MSG& msg, HWND hwnd, UINT first, UINT last) {
    if (hwnd && msg.hwnd != hwnd) return false;
    if (first == 0 && last == 0) return true;
    return msg.message >= first && msg.message <= last;
}

// Remove (or copy) the first queued message that passes the Win32 filter. *input tells whether it is
// browser input.
static bool take_message(MSG* out, HWND hwnd, UINT first, UINT last, bool remove, bool* input) {
    for (size_t i = 0; i < posted_messages.size(); i++) {
        if (!matches(posted_messages[i], hwnd, first, last)) continue;
        *out = posted_messages[i];
        *input = posted_input[i];
        if (remove) { posted_messages.erase(posted_messages.begin() + i); posted_input.erase(posted_input.begin() + i); }
        return true;
    }
    return false;
}

static void post(HWND hwnd, UINT message, WPARAM w, LPARAM l, bool input) {
    MSG msg;
    memset(&msg, 0, sizeof(msg));
    msg.hwnd = hwnd;
    msg.message = message;
    msg.wParam = w;
    msg.lParam = l;
    msg.time = GetTickCount();
    posted_messages.push_back(msg);
    posted_input.push_back(input);
}

extern "C" {

BOOL PostMessageA(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    post(hwnd, message, w, l, false);
    return TRUE;
}

#ifdef AOE_GAME_LOOP
// win32_user.cpp: WM_PAINT/WM_TIMER generated when the posted queue is empty (Windows priority order).
extern "C++" bool aoe_user_synthesize(MSG* out, HWND hwnd, UINT first, UINT last, bool remove);
// win32_ddraw.cpp: show the screen surface if the game drew to it.
extern "C++" void aoe_present_if_dirty();
// win32_media.cpp: MIDI end-of-file notifications.
extern "C++" void aoe_media_poll();

// win32_user.cpp: the thread's key state follows input messages as they leave the queue.
extern "C++" void aoe_input_retrieved(const MSG& msg);

// Mouse and keyboard input from the browser (win32_user.cpp), queued like hardware input.
extern "C++" void aoe_post_input(HWND hwnd, UINT message, WPARAM w, LPARAM l) { post(hwnd, message, w, l, true); }

static bool next_message(MSG* out, HWND hwnd, UINT first, UINT last, bool remove) {
    aoe_media_poll();
    bool input = false;
    if (take_message(out, hwnd, first, last, remove, &input)) {
        if (input && remove) aoe_input_retrieved(*out);
        return true;
    }
    return aoe_user_synthesize(out, hwnd, first, last, remove);
}

// Browser events are queued by the host (Phase 1). Until then only messages the game posts itself
// (and WM_QUIT) reach the queue.
EMSCRIPTEN_KEEPALIVE void aoe_post_message(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    PostMessageA(hwnd, message, w, l);
}

void PostQuitMessage(int exit_code) {
    PostMessageA(0, WM_QUIT, (WPARAM) exit_code, 0);
}

// An empty queue hands control back to the browser for one task before reporting "no message",
// so RGE_Base_Game::run() calls handle_idle() at most once per browser task.
BOOL PeekMessageA(MSG* out, HWND hwnd, UINT first, UINT last, UINT flags) {
    bool remove = (flags & PM_REMOVE) != 0;
    if (next_message(out, hwnd, first, last, remove)) return TRUE;
    aoe_present_if_dirty();
    emscripten_sleep(0);
    return next_message(out, hwnd, first, last, remove) ? TRUE : FALSE;
}

// Blocks (by yielding) until a matching message arrives. Returns FALSE only for WM_QUIT.
BOOL GetMessageA(MSG* out, HWND hwnd, UINT first, UINT last) {
    while (!next_message(out, hwnd, first, last, true)) {
        aoe_present_if_dirty();
        emscripten_sleep(4);
    }
    return out->message == WM_QUIT ? FALSE : TRUE;
}

#endif

}

#ifdef AOE_GAME_LOOP
// Command.cpp routes orders through this probe hook (C++ linkage); 0 keeps the original path.
int wasm_capture_order(const void*, int) { return 0; }
#endif

#ifdef AOE_GAME_LOOP
// Harness self-test of the yielding queue: returns a bit mask of passed checks (all = 0x1f).
extern "C" EMSCRIPTEN_KEEPALIVE int aoe_loop_selftest() {
    MSG msg;
    int passed = 0;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {}
    if (!PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) passed |= 1;             // empty queue yields, then FALSE
    PostMessageA((HWND) 7, WM_USER, 11, 22);
    if (PeekMessageA(&msg, 0, 0, 0, 0) && msg.message == WM_USER) passed |= 2; // PM_NOREMOVE keeps it
    if (PeekMessageA(&msg, (HWND) 7, WM_USER, WM_USER, PM_REMOVE) && msg.wParam == 11 && msg.lParam == 22) passed |= 4;
    PostMessageA(0, WM_USER + 1, 0, 0);
    if (GetMessageA(&msg, 0, 0, 0) && msg.message == WM_USER + 1) passed |= 8;
    PostQuitMessage(3);
    if (!GetMessageA(&msg, 0, 0, 0) && msg.message == WM_QUIT && msg.wParam == 3) passed |= 16;
    return passed;
}

extern "C" int __stdcall WinMain(HINSTANCE, HINSTANCE, char*, int);
EM_JS(void, aoe_game_exit, (int code), { if (Module['onGameExit']) Module['onGameExit'](code); });

// argv[1..] become the Windows command line (e.g. "WING NODXCHECK NOSOUND NOSTARTUP").
int main(int argc, char** argv) {
    static char cmd_line[256];
    cmd_line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd_line, " ", sizeof(cmd_line) - strlen(cmd_line) - 1);
        strncat(cmd_line, argv[i], sizeof(cmd_line) - strlen(cmd_line) - 1);
    }
    // C:\ exists on every Windows machine; the game writes debug logs there (e.g. c:\aoeWVlog.txt) without
    // checking that fopen succeeded.
    mkdir("/c", 0777);
    printf("[aoe] WinMain starting: \"%s\"\n", cmd_line);
    int code = WinMain((HINSTANCE) 1, 0, cmd_line, SW_SHOW);
    printf("[aoe] WinMain returned %d\n", code);
    aoe_game_exit(code);
    // The process ends here; the module is built with EXIT_RUNTIME=0, so global destructors don't run.
    // (~TPanelSystem frees panel list nodes that ~TPanel already freed through removePanel, a double free
    // the Windows CRT never detected at process exit.)
    fflush(0);
    return code;
}
#endif
