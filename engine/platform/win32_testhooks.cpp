// Read-only queries of the running game for automated tests (tests/test_native_*.mjs). They report what
// the game's own objects hold; nothing here changes game state (aoe_test_ptrclear works on a drawing area of
// its own).
#include "port_prefix.h"
// Read access to protected fields in this file only (access specifiers do not change the object layout).
#define protected public
#include "../upstream/include/basegame.h"
#include "../upstream/include/world.h"
#include "../upstream/include/player.h"
#include "../upstream/include/obj_list.h"
#include "../upstream/include/com_hand.h"
#include "../upstream/include/game_globals.h"
#include "../upstream/include/tribegam.h"
#include "../upstream/include/scr_game.h"
#include "../upstream/include/diam_map.h"
#include "../upstream/include/view.h"
#include "../upstream/include/scenario.h"
#include "../upstream/include/drawarea.h"
#include "../upstream/include/Shape.h"
#include <emscripten.h>

extern "C" {

// field: 0 prog_mode, 1 world_time (ms), 2 comm current_turn, 3 comm OutOfSyncFlag, 4 world player count,
// 5 my comm player number (Me), 6 world curr_player
EMSCRIPTEN_KEEPALIVE int aoe_test_state(int field) {
    RGE_Base_Game* game = rge_base_game;
    if (!game) return -1;
    RGE_Game_World* world = game->world;
    TCommunications_Handler* comm = game->comm_handler;
    switch (field) {
        case 0: return game->prog_mode;
        case 1: return world ? (int) world->world_time : -1;
        case 2: return comm ? (int) comm->current_turn : -1;
        case 3: return comm ? comm->OutOfSyncFlag : -1;
        case 4: return world ? world->player_num : -1;
        case 5: return comm ? (int) comm->Me : -1;
        case 6: return world ? world->curr_player : -1;
    }
    return -1;
}

// Attribute `attribute` of world player `player` times 1000 (0 food, 1 wood, 2 stone, 3 gold, ...), or
// with attribute -1 the number of the player's objects.
EMSCRIPTEN_KEEPALIVE int aoe_test_player(int player, int attribute) {
    RGE_Base_Game* game = rge_base_game;
    if (!game || !game->world || player < 0 || player >= game->world->player_num) return -1;
    RGE_Player* p = game->world->players[player];
    if (!p) return -1;
    if (attribute < 0) return p->objects ? p->objects->number_of_objects : 0;
    if (attribute >= p->attribute_num) return -1;
    return (int) (p->attributes[attribute] * 1000.0f);
}

// The in-game minimap (RGE_Diamond_Map) and the main view it outlines. field: 0/1 player view_x/view_y x1000,
// 2/3 main view size in tiles (max_col_num/max_row_num), 4..7 minimap view_rect left/top/right/bottom,
// 8/9 minimap pnl_x/pnl_y, 10 map_image_start_y, 11 map_image_hgt, 12 map_tile_row_cnt, 13/14 map_wid/map_hgt,
// 15 map_row_ratio x1000, 16 map_col_ratio x1000, 17/18 main view pnl_wid/pnl_hgt, 19 minimap image y of the
// view's centre tile (same formula as draw_tile).
EMSCRIPTEN_KEEPALIVE int aoe_test_minimap(int field) {
    TRIBE_Game* game = (TRIBE_Game*) rge_base_game;
    if (!game || !game->game_screen || !game->game_screen->map_view || !game->game_screen->main_view) return -99999;
    RGE_Diamond_Map* m = game->game_screen->map_view;
    RGE_View* v = game->game_screen->main_view;
    RGE_Player* p = m->player;
    if (!p) return -99998;
    switch (field) {
        case 0: return (int) (p->view_x * 1000);
        case 1: return (int) (p->view_y * 1000);
        case 2: return v->max_col_num;
        case 3: return v->max_row_num;
        case 4: return m->view_rect.left;
        case 5: return m->view_rect.top;
        case 6: return m->view_rect.right;
        case 7: return m->view_rect.bottom;
        case 8: return m->pnl_x;
        case 9: return m->pnl_y;
        case 10: return m->map_image_start_y;
        case 11: return m->map_image_hgt;
        case 12: return m->map_tile_row_cnt;
        case 13: return m->map_wid;
        case 14: return m->map_hgt;
        case 15: return (int) (m->map_row_ratio * 1000);
        case 16: return (int) (m->map_col_ratio * 1000);
        case 17: return v->pnl_wid;
        case 18: return v->pnl_hgt;
        case 19: {
            int col = (int) p->view_x, row = (int) p->view_y;
            return m->pnl_y + m->map_image_start_y + m->map_tile_rows[m->map_wid + row - col - 1].image_line;
        }
    }
    return -99997;
}

// Scenario files made by the retail game: load one through the game's own loaders (the scenario list's
// header reader and the full scenario information reader). field 0: header read (1/0); 1: the scenario's
// internal Version x100, or -1 if it could not be read.
EMSCRIPTEN_KEEPALIVE int aoe_test_scenario(const char* name, int field) {
    RGE_Base_Game* game = rge_base_game;
    if (!game || !name) return -1;
    if (field == 0) {
        RGE_Scenario_Header* header = game->get_scenario_header((char*) name, 0);
        if (!header) return 0;
        delete header;
        return 1;
    }
    RGE_Scenario* scenario = game->get_scenario_info((char*) name, 0);
    if (!scenario) return -1;
    int version = (int) (scenario->Version * 100 + 0.5f);
    delete scenario;
    return version;
}

// Thread (queue-synchronized) and physical state of a virtual key: bit 0 GetKeyState down, bit 1
// GetAsyncKeyState down, bit 2 GetKeyState toggled.
EMSCRIPTEN_KEEPALIVE int aoe_test_keys(int vk) {
    short sync = GetKeyState(vk), physical = GetAsyncKeyState(vk);
    return ((sync & 0x8000) ? 1 : 0) | ((physical & 0x8000) ? 2 : 0) | ((sync & 1) ? 4 : 0);
}

// The current player's selection: field 0 sel_count, 1 master id of the selected object (-1 if none),
// 2 number of different objects in sel_list (sel_count can count one object more than once).
EMSCRIPTEN_KEEPALIVE int aoe_test_selection(int field) {
    RGE_Base_Game* game = rge_base_game;
    if (!game || !game->world || game->world->curr_player < 0) return -1;
    RGE_Player* p = game->world->players[game->world->curr_player];
    if (!p) return -1;
    if (field == 0) return p->sel_count;
    if (field == 2) {
        int distinct = 0;
        for (int i = 0; i < 25; i++) {
            if (!p->sel_list[i]) continue;
            bool seen = false;
            for (int j = 0; j < i && !seen; j++) seen = p->sel_list[j] == p->sel_list[i];
            distinct += !seen;
        }
        return distinct;
    }
    return p->selected_obj && p->selected_obj->master_obj ? p->selected_obj->master_obj->id : -1;
}

// The n-th object of the current player that the main view drew in its last frame: field 0/1 screen x/y of
// the middle of its sprite, 2 master id; -1 if there is no such object.
EMSCRIPTEN_KEEPALIVE int aoe_test_displayed(int n, int field) {
    TRIBE_Game* game = (TRIBE_Game*) rge_base_game;
    if (!game || !game->world || !game->game_screen || !game->game_screen->main_view) return -1;
    RGE_View* view = game->game_screen->main_view;
    RGE_Player* p = game->world->players[game->world->curr_player];
    if (!p || !p->objects || !view->prior_objs) return -1;
    for (RGE_Object_Node* node = p->objects->list; node; node = node->next) {
        RGE_Static_Object* o = node->node;
        if (!o || !view->Object_Was_Displayed(o->id, false)) continue;
        DClipInfo_Node* d = view->prior_objs->LocateIDbyDrawLevel(o->id, 6, 0x28, 0);
        if (!d || !d->Shape || n-- > 0) continue;
        switch (field) {
            case 0: return view->render_rect.left + d->Draw_X + d->Shape->Width / 2;
            case 1: return view->render_rect.top + d->Draw_Y + d->Shape->Height / 2;
            case 2: return o->master_obj ? o->master_obj->id : -1;
        }
        return -1;
    }
    return -1;
}

// TDrawArea::PtrClear on a new DirectDraw area that was never locked (the game's text panels clear their
// image buffers like this, then copy them with colour key 1). Returns how many of the w*h pixels hold the
// clear colour 1 afterwards; -1 if no DirectDraw system is running.
EMSCRIPTEN_KEEPALIVE int aoe_test_ptrclear(int w, int h) {
    RGE_Base_Game* game = rge_base_game;
    if (!game || !game->draw_system || game->draw_system->DrawType != 2) return -1;
    TDrawArea* area = new TDrawArea((char*) "PtrClear test", 0);
    if (!area->Init(game->draw_system, w, h, 0, 0)) { delete area; return -1; }
    area->PtrClear(0, 1);
    int count = 0;
    if (area->Bits == 0 && area->Lock((char*) "PtrClear test", 1)) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) count += area->Bits[y * area->Pitch + x] == 1;
        area->Unlock((char*) "PtrClear test");
    } else count = -2; // PtrClear left the area locked
    delete area;
    return count;
}

}
