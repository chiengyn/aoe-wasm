#include <emscripten.h>
#include "../platform/menu_host.h"
#include "../upstream/include/scr_game.h"
#include "../upstream/include/Res_file.h"
#include "../upstream/include/pnl_edit.h"
#include "../upstream/include/dlg_smsg.h"
#include "../upstream/include/dlg_menu.h"
#include "../upstream/include/program_mode.h"
extern TRIBE_Game* wasm_simulation_game();
extern int wasm_game_graphics(int,int);
extern int wasm_capture_orders;
extern void wasm_world_graphics();
extern int wasm_ui_program(int);
extern int wasm_game_ui_state(int);
static TRIBE_Game* ui_game;
static bool world_render;
EM_JS(void,ui_event,(const char* kind,const char* value),{if(Module.gameUIEvent)Module.gameUIEvent(UTF8ToString(kind),UTF8ToString(value));});
static void draw_tree(TPanel*p){if(!p||!p->visible||!p->active)return;p->draw();p->just_drawn=1;for(PanelNode*n=p->first_child_node;n;n=n->next_node)draw_tree(n->panel);p->just_drawn=0;}
static void direct_text(TPanel*p){TMessagePanel*m=dynamic_cast<TMessagePanel*>(p);if(m&&m->ImageBuffer){delete m->ImageBuffer;m->ImageBuffer=0;m->ImageTextActive=0;}for(PanelNode*n=p->first_child_node;n;n=n->next_node)direct_text(n->panel);}
static void refresh_messages(TPanel*p){TMessagePanel*m=dynamic_cast<TMessagePanel*>(p);if(m)m->handle_idle();for(PanelNode*n=p->first_child_node;n;n=n->next_node)refresh_messages(n->panel);}
static TButtonPanel* modal_buttons[32];static int modal_count;
static void collect_buttons(TPanel*p){for(PanelNode*n=p->first_child_node;n;n=n->next_node){TPanel*c=n->panel;if(!c->active||!c->visible)continue;if(c->panel_type==3&&modal_count<32)modal_buttons[modal_count++]=(TButtonPanel*)c;collect_buttons(c);}}
static TPanel* modal(){return panel_system.modalPanel();}

class BrowserGameScreen:public TRIBE_Screen_Game {
public:
    BrowserGameScreen():TRIBE_Screen_Game(){
        allow_user_commands=1;
        // Canvas text is emitted each frame; use the recovered direct-draw path
        // instead of retaining a Windows GDI bitmap with cached glyphs.
        direct_text(this);
        if(time_panel->ImageBuffer){delete time_panel->ImageBuffer;time_panel->ImageBuffer=0;time_panel->ImageTextActive=0;}
    }
    TPanel* view(){return main_view;}
    TPanel* minimap(){return map_view;}
    void cancel_map_mouse(){map_view->end_scroll_view();map_view->release_mouse();}
    TRIBE_Main_View* battlefield(){return main_view;}
    RGE_Player* player(){return world->players[world->curr_player];}
    RGE_Static_Object* object(int id){return world->object(id);}
    void selection_changed(){object_panel->set_object(player()->selected_obj);object_changed();setup_buttons();}
    TButtonPanel* button(int i){return i>=0&&i<17?button_panel[i]:0;}
    int mode(){return wasm_game_ui_state(0);}
    int placing(){return wasm_game_ui_state(1);}
    void minimap_click(int x,int y){map_view->start_scroll_view(x,y);map_view->end_scroll_view();}
    void camera(float x,float y){player()->set_view_loc(x,y);}
    void refresh(){inven_panel->handle_idle();time_panel->handle_idle();pop_panel->handle_idle();object_panel->handle_idle();refresh_messages(this);setup_buttons();}
    long handle_paint(){hud();return 1;}
    long action(TPanel*from,long action,unsigned long a1,unsigned long a2){
        if(from&&(!strcmp(from->panelName(),"QuitGameDialog")||!strcmp(from->panelName(),"ResignDialog"))){
            bool quit=!strcmp(from->panelName(),"QuitGameDialog");
            panel_system.setCurrentPanel((char*)"Game Screen",0);
            panel_system.destroyPanel((char*)(quit?"QuitGameDialog":"ResignDialog"));
            if(action==0){if(quit)ui_event("leave","");else player()->command_resign(player()->id,0);}return 1;
        }
        return TRIBE_Screen_Game::action(from,action,a1,a2);
    }
    void menu(){command_menu();}
    void chat(){if(!modal())new TribeSendQuickMessageDialog(this,TribeSendQuickMessageDialog::AsChat);}
    void message(char*text){display_system_message(text);}
    void hud(){
        draw();just_drawn=1;
        for(PanelNode*n=first_child_node;n;n=n->next_node){TPanel*p=n->panel;if(!p->active||!p->visible)continue;if(p==main_view){if(world_render){main_view->render_terrain_mode=0;main_view->draw();}p->just_drawn=1;for(PanelNode*c=p->first_child_node;c;c=c->next_node)draw_tree(c->panel);p->just_drawn=0;}else draw_tree(p);}
        just_drawn=0;
    }
};
static BrowserGameScreen* screen;
static TButtonPanel* ui_button(int i){return modal()?(i>=0&&i<modal_count?modal_buttons[i]:0):(screen?screen->button(i):0);}
static TribeSendQuickMessageDialog* chat_dialog(){return dynamic_cast<TribeSendQuickMessageDialog*>(modal());}
static void close_chat(){panel_system.setCurrentPanel((char*)"Game Screen",0);panel_system.destroyPanel((char*)"Send Quick Message Dialog");}

extern "C" {
// Register graphics before loading the DAT, whose native map constructor loads
// terrain/border shapes immediately. Expansion resources take precedence.
EMSCRIPTEN_KEEPALIVE void game_ui_resources(){
    RESFILE_open_new_resource_file((char*)"Interfac.drs",(char*)"tribe",(char*)"data2/",1);
    RESFILE_open_new_resource_file((char*)"graphics.drs",(char*)"tribe",(char*)"data2/",1);
    RESFILE_open_new_resource_file((char*)"Interfac.drs",(char*)"tribe",(char*)"data/",1);
    RESFILE_open_new_resource_file((char*)"graphics.drs",(char*)"tribe",(char*)"data/",1);
    RESFILE_open_new_resource_file((char*)"Terrain.drs",(char*)"tribe",(char*)"data/",1);
    RESFILE_open_new_resource_file((char*)"Border.drs",(char*)"tribe",(char*)"data/",1);
}
EMSCRIPTEN_KEEPALIVE void game_ui_world(int enabled){world_render=enabled!=0;}
EMSCRIPTEN_KEEPALIVE int game_ui_init(int width,int height){
    ui_game=wasm_simulation_game();if(!ui_game||screen)return 0;
    if(!wasm_game_graphics(width,height))return 0;
    RESFILE_open_new_resource_file((char*)"Interfac.drs",(char*)"tribe",(char*)"data2/",1);
    RESFILE_open_new_resource_file((char*)"Interfac.drs",(char*)"tribe",(char*)"data/",1);
    if(world_render)wasm_world_graphics();
    screen=new BrowserGameScreen;
    panel_system.setCurrentPanel((char*)"Game Screen",0);return !screen->error_code;
}
EMSCRIPTEN_KEEPALIVE void game_ui_draw(){if(screen){wasm_menu_begin_frame();screen->hud();modal_count=0;if(modal())collect_buttons(modal());wasm_menu_present(screen->render_area,screen->palette);}}
EMSCRIPTEN_KEEPALIVE void game_ui_refresh(){if(screen)screen->refresh();}
EMSCRIPTEN_KEEPALIVE int game_ui_mouse(int kind,int button,int x,int y,int ctrl,int shift){
 if(!screen||modal())return 0;TPanel*v=screen->view();int old=wasm_ui_program(ProgModeGame);wasm_capture_orders=1;int result=0;
 if(kind==0)result=v->handle_mouse_down(button,x,y,ctrl,shift);
 else if(kind==1)result=v->handle_mouse_move(x,y,ctrl,shift);
 else if(kind==2)result=v->handle_mouse_up(button,x,y,ctrl,shift);
 else if(kind==3)result=v->handle_mouse_dbl_click(button,x,y,ctrl,shift);
 else if(kind==4){screen->battlefield()->end_scroll_view();v->release_mouse();}
 wasm_capture_orders=0;wasm_ui_program(old);screen->selection_changed();return result;
}
EMSCRIPTEN_KEEPALIVE int game_ui_map_mouse(int kind,int button,int x,int y,int ctrl,int shift){
 if(!screen||modal())return 0;TPanel*v=screen->minimap();int old=wasm_ui_program(ProgModeGame);wasm_capture_orders=1;int result=0;
 if(kind==0)result=v->handle_mouse_down(button,x,y,ctrl,shift);
 else if(kind==1)result=v->handle_mouse_move(x,y,ctrl,shift);
 else if(kind==2)result=v->handle_mouse_up(button,x,y,ctrl,shift);
 else if(kind==4)screen->cancel_map_mouse();
 wasm_capture_orders=0;wasm_ui_program(old);screen->selection_changed();return result;
}
EMSCRIPTEN_KEEPALIVE int game_ui_pick(int x,int y){if(!screen)return -1;RGE_Pick_Info info;return screen->battlefield()->pick1(0x29,2,x,y,&info,0,0)==0x34?info.object->id:-1;}
EMSCRIPTEN_KEEPALIVE int game_ui_object_pixel(int id,int axis){if(!screen)return -1;RGE_Static_Object*o=screen->object(id);if(!o||!o->tile)return -1;RGE_View*v=screen->battlefield();return axis?o->tile->screen_ypos+o->tile->height*v->elev_hgt+o->screen_y_offset-v->map_scr_y_offset:o->tile->screen_xpos+o->screen_x_offset-v->map_scr_x_offset;}
EMSCRIPTEN_KEEPALIVE void game_ui_scroll(float dx,float dy){if(screen){RGE_Player*p=screen->player();RGE_View*v=screen->battlefield();screen->camera(p->view_x+dx/v->tile_wid-dy/v->tile_hgt,p->view_y+dx/v->tile_wid+dy/v->tile_hgt);}}
EMSCRIPTEN_KEEPALIVE void game_ui_select(int id,int append){if(!screen)return;RGE_Player*p=screen->player();if(!append)p->unselect_object();RGE_Static_Object*o=screen->object(id);if(o)p->select_object(o);screen->selection_changed();}
EMSCRIPTEN_KEEPALIVE int game_ui_selected(int i){if(!screen)return -1;RGE_Player*p=screen->player();return i>=0&&i<25&&p->sel_list[i]?p->sel_list[i]->id:-1;}
EMSCRIPTEN_KEEPALIVE double game_ui_view(int f){if(!screen)return 0;RGE_Player*p=screen->player();return f?p->view_y:p->view_x;}
EMSCRIPTEN_KEEPALIVE int game_ui_mode(){return screen?screen->mode():0;}
EMSCRIPTEN_KEEPALIVE void game_ui_camera(float x,float y){if(screen)screen->camera(x,y);}
EMSCRIPTEN_KEEPALIVE void game_ui_minimap(int x,int y){if(screen)screen->minimap_click(x,y);}
EMSCRIPTEN_KEEPALIVE int game_ui_placing(){return screen?screen->placing():-1;}
EMSCRIPTEN_KEEPALIVE int game_ui_key(int key,int alt,int ctrl,int shift){
 if(!screen)return 0;wasm_capture_orders=1;int result=0;
 if(chat_dialog()){if(key==27){close_chat();result=1;}}
 else if(modal()){
  TButtonPanel*target=0;for(int i=0;i<modal_count;i++)if(!modal_buttons[i]->disabled&&modal_buttons[i]->hotkey==key)target=modal_buttons[i];
  if(target){target->do_action();result=1;}else result=modal()->handle_key_down(key,1,alt,ctrl,shift);
 }
 else if(key==13){screen->chat();result=1;}
 else if(key==121&&alt){screen->menu();result=1;}
 else {
  TButtonPanel*target=0;if(!alt&&!ctrl)for(int i=0;i<17;i++){TButtonPanel*b=screen->button(i);if(b&&b->active&&b->visible&&!b->disabled&&b->hotkey==key)target=b;}
  if(target){target->do_action();result=1;}else result=screen->key_down_action(key,1,alt,ctrl,shift);
 }
 wasm_capture_orders=0;screen->selection_changed();return result;
}
EMSCRIPTEN_KEEPALIVE int game_ui_button_count(){return modal()?modal_count:17;}
EMSCRIPTEN_KEEPALIVE const char* game_ui_modal(){return modal()?modal()->panelName():"";}
EMSCRIPTEN_KEEPALIVE int game_ui_modal_rect(int f){TPanel*p=modal();if(!p)return 0;return f==0?p->pnl_x:f==1?p->pnl_y:f==2?p->pnl_wid:p->pnl_hgt;}
EMSCRIPTEN_KEEPALIVE int game_ui_chat_rect(int f){TribeSendQuickMessageDialog*d=chat_dialog();if(!d)return 0;TEditPanel*p=d->chatInput;return f==0?p->pnl_x:f==1?p->pnl_y:f==2?p->pnl_wid:p->pnl_hgt;}
EMSCRIPTEN_KEEPALIVE void game_ui_chat_send(const char*text){if(!chat_dialog())return;char value[66];strncpy(value,text,65);value[65]=0;close_chat();if(*value)ui_event("chat",value);}
EMSCRIPTEN_KEEPALIVE void game_ui_message(const char*text){if(screen)screen->message((char*)text);}
EMSCRIPTEN_KEEPALIVE int game_ui_button(int i,int f){TButtonPanel*b=ui_button(i);if(!b)return -1;switch(f){case 0:return b->pnl_x;case 1:return b->pnl_y;case 2:return b->pnl_wid;case 3:return b->pnl_hgt;case 4:return b->active&&b->visible;case 5:return b->disabled;case 6:return b->get_id();case 7:return b->get_id2();case 8:return b->hotkey;}return -1;}
EMSCRIPTEN_KEEPALIVE const char* game_ui_button_text(int i){TButtonPanel*b=ui_button(i);return b&&b->text1[0]?b->text1[0]:"";}
EMSCRIPTEN_KEEPALIVE int game_ui_activate(int i){TButtonPanel*b=ui_button(i);if(!b||!b->active||b->disabled)return 0;if(modal()&&!strcmp(modal()->panelName(),"Menu Dialog")&&b->get_id()!=4&&b->get_id()!=6&&b->get_id()!=12)return -1;
 if(!modal()&&b->get_id()==13){screen->chat();return 1;}
 wasm_capture_orders=1;b->do_action();wasm_capture_orders=0;return 1;}
EMSCRIPTEN_KEEPALIVE int game_ui_rect(int panel,int field){if(!screen)return -1;TPanel*p=panel?screen->minimap():screen->view();switch(field){case 0:return p->pnl_x;case 1:return p->pnl_y;case 2:return p->pnl_wid;case 3:return p->pnl_hgt;}return -1;}
}
