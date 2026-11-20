#include <emscripten.h>
#include "../platform/menu_host.h"
#include "../upstream/include/main.h"
#include "../upstream/include/tribegam.h"
#include "../upstream/include/drawarea.h"
#include "../upstream/include/game_globals.h"
#include "../upstream/include/Res_file.h"
#include "../upstream/include/scr_main.h"
#include "../upstream/include/scr_sing.h"
#include "../upstream/include/pnl_btn.h"
#include "../upstream/include/scr_mps.h"
#include "../upstream/include/com_hand.h"
#include "../upstream/include/gameinfo.h"
#include "../upstream/include/pnl_lst.h"
#include "../upstream/include/pnl_drop.h"
#include "../upstream/include/pnl_edit.h"
#include "../upstream/include/sceninfo.h"
#include "../upstream/include/scr_set_screen.h"
// TTextPanel::append_line(const char*,long) and wasm_lobby_role live in platform/port_glue.cpp.
extern int wasm_lobby_role;
EM_JS(void,lobby_action,(const char* kind,int slot,int value,const char* text),{
    if(Module['onLobbyAction'])Module['onLobbyAction'](UTF8ToString(kind),slot,value,text?UTF8ToString(text):'');
});
class BrowserComm:public TCommunications_Handler {
public:
    BrowserComm(RGE_Prog_Info*p):TCommunications_Handler(0,8,chat,p->game_guid){}
    void identity(int me,int host){Me=me;MeHost=host;Multiplayer=0;wasm_lobby_role=host;}
    void member(int slot,const char*name,int ready){SetPlayerHumanity(slot,*name?ME_HUMAN:ME_ABSENT);SetPlayerName(slot,(char*)name);unsigned oldMe=Me;int oldHost=MeHost;Me=slot;MeHost=1;SetMyReadiness(ready,0,0,0,0,0,0,0);Me=oldMe;MeHost=oldHost;}
};
class BrowserMPSetup:public TribeMPSetupScreen {
public:
    void sync(int ready,int canStart){i_am_ready=ready;myCivilization=myPlayerColor=myPlayerTeam=0;ready_button->set_state(ready);if(startButton)startButton->set_disabled(!canStart);if(gameSettingsButton)gameSettingsButton->set_disabled(ready);fillPlayers();}
    void add_chat(char*text){chatBox->word_wrap_append(text);chatBox->scroll(7,0,1);}
    const char* label(TPanel*p){if(p==ready_button)return "I'm Ready!";if(p==chatInput)return "Chat message";for(int i=0;i<8;i++){if(p==playerCivDrop[i])return "Civilization";if(p==playerNameDrop[i])return "Player slot";if(p==playerColor[i])return "Player color";if(p==playerTeam[i])return "Team";}return "";}
    virtual long action(TPanel*p,long action,unsigned long a,unsigned long b){
        if(p==startButton&&action==1){lobby_action("start",0,0,0);return 1;}
        if(p==cancelButton&&action==1){lobby_action("leave",0,0,0);return 1;}
        if(p==ready_button&&action==1){lobby_action("ready",0,!i_am_ready,0);return 1;}
        if(p==chatInput&&action==0){lobby_action("chat",0,0,chatInput->currentLine());chatInput->set_text((char*)"");return 1;}
        for(int i=0;i<8;i++){
            if(p==playerColor[i]&&action==1){lobby_action("color",i,playerColor[i]->get_id(),0);return 1;}
            if(p==playerTeam[i]&&action==1){lobby_action("team",i,playerTeam[i]->get_state()+1,0);return 1;}
            if(p==playerCivDrop[i]&&action==0){lobby_action("civ",i,playerCivDrop[i]->get_id(),0);return 1;}
            if(p==playerNameDrop[i]&&action==0){lobby_action("slot",i,playerNameDrop[i]->get_id(),0);return 1;}
        }
        if(p->panelName()&&!strcmp(p->panelName(),"Game Settings Screen")&&action){long r=TribeMPSetupScreen::action(p,action,a,b);lobby_action("settings",0,0,0);return r;}
        if(p==help_button||p==netInfoButton)return -1;
        return TribeMPSetupScreen::action(p,action,a,b);
    }
};
static BrowserComm* browser_comm;
static BrowserMPSetup* mp_screen;

class MenuGame:public TRIBE_Game {
public:
    MenuGame(RGE_Prog_Info* p):TRIBE_Game(p,0){}
    int init(int width,int height){
        draw_system=new TDrawSystem;
        draw_system->DrawType=1;
        draw_system->ScreenMode=2;
        draw_area=new TDrawArea((char*)"Browser menu",1);
        draw_area->Init(draw_system,width,height,0,0);
        draw_system->DrawArea=draw_area;
        input_enabled=1;
        return setup_fonts();
    }
    void show(){TEasy_Panel*p=(TEasy_Panel*)panel_system.currentPanel();wasm_menu_present(draw_area,p->palette);}
    int init_multiplayer(int me,int isHost){
        if(!player_game_info){player_game_info=new RGE_Game_Info((char*)"/menu-profile.nfo");player_game_info->add_new_person((char*)"Player 1");player_game_info->set_current_person(0);}
        if(!chat&&!setup_chat())return 0;
        if(!browser_comm)browser_comm=new BrowserComm(&menu_info_ref());
        comm=comm_handler=browser_comm;browser_comm->identity(me,isHost);
        if(!scenario_info)scenario_info=new RGE_Scenario_File_Info((char*)"/scenario/scenario.inf");
        setSinglePlayerGame(0);setMultiplayerGame(1);
        comm->SetMyPlayerName((char*)"Player 1");
        for(int i=1;i<=8;i++)comm->SetPlayerHumanity(i,i==1?ME_HUMAN:ME_ABSENT);
        return 1;
    }
    RGE_Prog_Info& menu_info_ref(){return *prog_info;}
};
static MenuGame* menu_game;
static RGE_Prog_Info menu_info;
static TButtonPanel* buttons[64];
static int button_count;
static TDropDownPanel* drops[40];static int drop_count;
static TEditPanel* edit_controls[8];static int edit_count;
static void draw_panel(TPanel*p){
    if(!p->visible||!p->active)return;
    p->draw();p->just_drawn=1;
    for(PanelNode*n=p->first_child_node;n;n=n->next_node)draw_panel(n->panel);
    p->just_drawn=0;
}
static void collect(TPanel*p){
    if(!p->visible||!p->active)return;
    for(PanelNode*n=p->first_child_node;n;n=n->next_node){
        if(!n->panel->visible||!n->panel->active)continue;
        if(n->panel->panel_type==6&&drop_count<40){drops[drop_count++]=(TDropDownPanel*)n->panel;continue;}
        TEditPanel*e=dynamic_cast<TEditPanel*>(n->panel);if(e&&edit_count<8){edit_controls[edit_count++]=e;continue;}
        if(n->panel->visible&&n->panel->active&&n->panel->panel_type==3&&button_count<64)buttons[button_count++]=(TButtonPanel*)n->panel;
        collect(n->panel);
    }
}
extern "C" {
EMSCRIPTEN_KEEPALIVE int menu_init(int width,int height){
    if(menu_game||!((width==640&&height==480)||(width==800&&height==600)||(width==1024&&height==768)))return 0;
    memset(&menu_info,0,sizeof(menu_info));strcpy(menu_info.prog_name,"Rise of Rome WASM menus");strcpy(menu_info.registry_key,"AoE-WASM-Menu");
    menu_info.main_wid=width;menu_info.main_hgt=height;menu_info.max_players=8;strcpy(menu_info.scenario_dir,"scenario/");
    menu_game=new MenuGame(&menu_info);
    puts("menu: fonts"); if(!menu_game->init(width,height))return 0; puts("menu: resources");
    RESFILE_open_new_resource_file((char*)"Interfac.drs",(char*)"tribe",(char*)"data2/",1);
    RESFILE_open_new_resource_file((char*)"Interfac.drs",(char*)"tribe",(char*)"data/",1);
    puts("menu: constructor"); TRIBE_Screen_Main_Menu*menu=new TRIBE_Screen_Main_Menu; puts("menu: focus");
    if(menu->error_code)return 0;
    panel_system.setCurrentPanel((char*)"Main Menu",0); puts("menu: ready");
    return 1;
}
EMSCRIPTEN_KEEPALIVE const char* menu_name(){return panel_system.currentPanel()?panel_system.currentPanel()->panelName():"";}
EMSCRIPTEN_KEEPALIVE int menu_cursor_resource(){return ((TEasy_Panel*)panel_system.currentPanel())->cursor_id;}
EMSCRIPTEN_KEEPALIVE int menu_draw(){
    if(!menu_game)return 0;
    TPanel*p=panel_system.currentPanel();wasm_menu_begin_frame();draw_panel(p);menu_game->show();
    button_count=drop_count=edit_count=0;collect(p);return button_count;
}
EMSCRIPTEN_KEEPALIVE const char* menu_button_text(int index){if(index<0||index>=button_count)return "";TButtonPanel*b=buttons[index];const char*label=mp_screen?mp_screen->label(b):"";return *label?label:b->text1[b->cur_state];}
EMSCRIPTEN_KEEPALIVE int menu_button_value(int index,int field){
    if(index<0||index>=button_count)return -1;TButtonPanel*b=buttons[index];
    switch(field){case 0:return b->pnl_x;case 1:return b->pnl_y;case 2:return b->pnl_wid;case 3:return b->pnl_hgt;case 4:return b->have_focus;case 5:return b->hotkey;case 6:return b->disabled;}return -1;
}
EMSCRIPTEN_KEEPALIVE int menu_focus(int index,int down){
    if(index<0||index>=button_count)return 0;TButtonPanel*b=buttons[index];b->parent_panel->set_curr_child(b);b->is_down=down;return 1;
}
EMSCRIPTEN_KEEPALIVE int menu_activate(int index){
    if(index<0||index>=button_count)return 0;
    TPanel*p=panel_system.currentPanel();TButtonPanel*b=buttons[index];
    if(b->disabled)return 0;
    if(strcmp(p->panelName(),"Main Menu")==0&&index==1){
        lobby_action("connect",0,0,0);return 2;
    }
    if(!strcmp(p->panelName(),"MP Setup Screen")||!strcmp(p->panelName(),"Game Settings Screen")){b->is_down=0;b->do_action();return 1;}
    // Only audited transitions are exposed so unfinished game setup cannot
    // crash or masquerade as a complete retail menu flow.
    bool supported=(strcmp(p->panelName(),"Main Menu")==0&&(index==0||index==4))
      ||(strcmp(p->panelName(),"Single Player Menu")==0&&(index==5||index==6));
    if(!supported)return -1;
    b->is_down=0;int result=p->action(b,1,0,0);button_count=0;collect(panel_system.currentPanel());panel_system.currentPanel()->handle_idle();return result;
}
EMSCRIPTEN_KEEPALIVE int menu_mp_open(int me,int isHost){if(me<1||me>8||mp_screen)return 0;if(!menu_game->init_multiplayer(me,isHost))return 0;mp_screen=new BrowserMPSetup;if(mp_screen->error_code)return 0;panel_system.setCurrentPanel((char*)"MP Setup Screen",0);return 1;}
EMSCRIPTEN_KEEPALIVE void menu_mp_member(int slot,const char*name,int civ,int color,int team,int ready){if(!mp_screen||slot<1||slot>8||strlen(name)>32)return;browser_comm->member(slot,name,ready);menu_game->setCivilization(slot-1,civ);menu_game->setPlayerColor(slot-1,color);menu_game->setPlayerTeam(slot-1,team);}
EMSCRIPTEN_KEEPALIVE void menu_mp_sync(int ready,int canStart){if(mp_screen){mp_screen->sync(ready,canStart);mp_screen->updateSummary();}}
EMSCRIPTEN_KEEPALIVE void menu_mp_chat(char*text){if(mp_screen&&strlen(text)<320)mp_screen->add_chat(text);}
EMSCRIPTEN_KEEPALIVE void menu_mp_leave(){if(mp_screen){panel_system.setCurrentPanel((char*)"Main Menu",0);panel_system.destroyPanel((char*)"Game Settings Screen");panel_system.destroyPanel((char*)"MP Setup Screen");mp_screen=0;wasm_lobby_role=-1;}}
EMSCRIPTEN_KEEPALIVE int menu_mp_setting(int f,int value,int write){
    if(!menu_game)return -1;
    switch(f){
    case 0:if(write)menu_game->setMapSize((TRIBE_Game::MapSize)value);return menu_game->mapSize();
    case 1:if(write)menu_game->setMapType((TRIBE_Game::MapType)value);return menu_game->mapType();
    case 2:if(write)menu_game->setVictoryType((TRIBE_Game::VictoryType)value,menu_game->victoryAmount());return menu_game->victoryType();
    case 3:if(write)menu_game->setVictoryType(menu_game->victoryType(),value);return menu_game->victoryAmount();
    case 4:if(write)menu_game->setResourceLevel((TRIBE_Game::ResourceLevel)value);return menu_game->resourceLevel();
    case 5:if(write)menu_game->setStartingAge((TRIBE_Game::Age)value);return menu_game->startingAge();
    case 6:if(write)menu_game->setPopLimit(value);return menu_game->popLimit();
    case 7:if(write)menu_game->setRandomizePositions(value);return menu_game->randomizePositions();
    case 8:if(write)menu_game->setFullVisibility(value);return menu_game->fullVisibility();
    case 9:if(write)menu_game->setFullTechTree(value);return menu_game->fullTechTree();
    case 10:if(write)menu_game->setAllowCheatCodes(value);return menu_game->allowCheatCodes();
    case 11:if(write)menu_game->setMpPathFinding(value);return menu_game->mpPathFinding();
    case 12:if(write)menu_game->setDifficulty(value);return menu_game->difficulty();
    case 13:if(write)menu_game->setDeathMatch(value);return menu_game->deathMatch();
    }return -1;
}
EMSCRIPTEN_KEEPALIVE int menu_drop_count(){return drop_count;}
EMSCRIPTEN_KEEPALIVE const char* menu_drop_text(int i,int line){return i>=0&&i<drop_count?drops[i]->get_text(line):"";}
EMSCRIPTEN_KEEPALIVE const char* menu_drop_label(int i){
    if(i<0||i>=drop_count)return "";TDropDownPanel*d=drops[i];
    TribeGameSettingsScreen*s=dynamic_cast<TribeGameSettingsScreen*>(panel_system.currentPanel());
    if(s){if(d==s->mapSizeDrop)return "Map Size";if(d==s->mapTypeDrop)return "Map Type";if(d==s->victoryTypeDrop)return "Victory Condition";if(d==s->resourcesDrop)return "Resources";if(d==s->ageDrop)return "Starting Age";if(d==s->difficultyDrop)return "Difficulty";if(d==s->pathingDrop)return "Path Finding";if(d==s->limitDrop)return "Population Limit";if(d==s->timeDrop)return "Time Limit";if(d==s->scoreDrop)return "Score";}
    return mp_screen?mp_screen->label(d):"";
}
EMSCRIPTEN_KEEPALIVE int menu_drop_value(int i,int f){if(i<0||i>=drop_count)return -1;TDropDownPanel*d=drops[i];switch(f){case 0:return d->pnl_x;case 1:return d->pnl_y;case 2:return d->pnl_wid;case 3:return d->pnl_hgt;case 4:return d->numberLines();case 5:return d->currentLineNumber();}return -1;}
EMSCRIPTEN_KEEPALIVE int menu_drop_select(int i,int line){if(i<0||i>=drop_count||line<0||line>=drops[i]->numberLines())return 0;TDropDownPanel*d=drops[i];d->setCurrentLineNumber(line);return d->parent_panel->action(d,0,0,0);}
EMSCRIPTEN_KEEPALIVE int menu_edit_count(){return edit_count;}
EMSCRIPTEN_KEEPALIVE const char* menu_edit_text(int i){return i>=0&&i<edit_count?edit_controls[i]->get_text():"";}
EMSCRIPTEN_KEEPALIVE int menu_edit_value(int i,int f){if(i<0||i>=edit_count)return -1;TEditPanel*e=edit_controls[i];switch(f){case 0:return e->pnl_x;case 1:return e->pnl_y;case 2:return e->pnl_wid;case 3:return e->pnl_hgt;case 4:return e->fixed_len;}return -1;}
EMSCRIPTEN_KEEPALIVE void menu_edit_input(int i,char*text,int submit){if(i<0||i>=edit_count)return;TEditPanel*e=edit_controls[i];if(strlen(text)>(unsigned)e->fixed_len)return;e->set_text(text);if(submit)e->parent_panel->action(e,0,0,0);}
}
