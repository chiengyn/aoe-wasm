// Room launch (browser port): the web lobby (web/index.html) starts the game straight into a multiplayer
// room, as a Microsoft Gaming Zone lobby launch did for the retail game. The page passes the room before
// WinMain runs (aoe_room_set); the main menu then runs the steps of the game's own Multiplayer screens
// (wasm_room_launch in scr_cj.cpp) and the room's password goes with the DirectPlay Open (win32_dplay.cpp).
// The page hears what happened through Module.aoeRoom(event, code):
//   "setup"   the room's game setup screen is shown (created or joined)
//   "failed"  creating or joining did not work; code is the DirectPlay result (0 if there was none)
//   "left"    the player is back on a menu screen after the room (left it, or the match ended)
// Linked into both builds; nothing happens unless the page set a room.
#include "port_prefix.h"
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace {
enum class Stage { none, pending, launching, in_room, done };
Stage stage = Stage::none;
bool host = false;
std::string player, room, password;
GUID instance;
HRESULT last_open = 0;

#ifdef __EMSCRIPTEN__
EM_JS(void, aoe_host_room, (const char* event, int code), {
    if (Module['aoeRoom']) Module['aoeRoom'](UTF8ToString(event), code);
});
#else
void aoe_host_room(const char*, int) {}
#endif

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}

// The session instance as the relay lists it (/api/rooms id): the GUID's 16 bytes in memory order.
static bool parse_instance(const char* hex, GUID* out) {
    unsigned char bytes[16];
    if (!hex || strlen(hex) != 32) return false;
    for (int i = 0; i < 16; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = (unsigned char) (hi << 4 | lo);
    }
    memcpy(out, bytes, 16);
    return true;
}

extern "C" {
// role: 1 creates the room, 0 joins the room with this instance. Strings are Windows-1252. The game's own
// limits apply: 15 characters for the player name (scr_mp.cpp), 40 for the game name (scr_cj.cpp).
EMSCRIPTEN_KEEPALIVE int aoe_room_set(int role, const char* player_name, const char* room_name, const char* room_password, const char* instance_hex) {
    if (stage != Stage::none || !player_name || !*player_name) return 0;
    host = role == 1;
    memset(&instance, 0, sizeof(instance));
    if (!host && !parse_instance(instance_hex, &instance)) return 0;
    if (host && (!room_name || !*room_name)) return 0;
    player.assign(player_name, strnlen(player_name, 15));
    room.assign(room_name ? room_name : "", room_name ? strnlen(room_name, 40) : 0);
    password = room_password ? room_password : "";
    stage = Stage::pending;
    return 1;
}
}

int wasm_room_pending() { return stage == Stage::pending; }

// Hands the room to wasm_room_launch once. player and room must hold 16 and 41 bytes.
int wasm_room_take(int* is_host, char* player_out, char* room_out, GUID* instance_out) {
    if (stage != Stage::pending) return 0;
    stage = Stage::launching;
    *is_host = host;
    strcpy(player_out, player.c_str());
    strcpy(room_out, room.c_str());
    *instance_out = instance;
    return 1;
}

// The password for DirectPlay Open while a room is being created or joined (the game's own dialogs never
// set one); null otherwise.
const char* wasm_room_password() { return stage == Stage::launching && !password.empty() ? password.c_str() : 0; }

void wasm_room_open_result(HRESULT result) { last_open = result; }

void wasm_room_failed() {
    if (stage != Stage::launching) return;
    stage = Stage::done;
    aoe_host_room("failed", (int) last_open);
}

// Every screen shown (TScreenPanel::set_focus).
void wasm_room_screen(const char* name) {
    if (!name) return;
    if (stage == Stage::launching && strcmp(name, "MP Setup Screen") == 0) {
        stage = Stage::in_room;
        aoe_host_room("setup", 0);
    } else if (stage == Stage::in_room &&
               (strcmp(name, "Join Screen") == 0 || strcmp(name, "MP Startup Screen") == 0 || strcmp(name, "Main Menu") == 0)) {
        stage = Stage::done;
        aoe_host_room("left", 0);
    }
}
