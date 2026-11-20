// Diagnostic entry points only. Missing desktop APIs fail closed in the JS loader.
#include <emscripten.h>
#include "../upstream/include/main.h"
#include "../upstream/include/tribegam.h"
#include "../upstream/include/tworld.h"
#include "../upstream/include/mst_play.h"
#include "../upstream/include/m_s_obj.h"
#include "../upstream/include/game_globals.h"
#include "../upstream/include/com_hand.h"
#include "../upstream/include/world_info.h"
#include "../upstream/include/map.h"
#include "../upstream/include/player.h"
#include "../upstream/include/obj_list.h"
#include "../upstream/include/stat_obj.h"
#include "../upstream/include/Command.h"
#include "../upstream/include/tcommand.h"
#include "../upstream/include/t_b_obj.h"
#include "../upstream/include/tplayer.h"
#include "../upstream/include/tm_co_ob.h"
#include "../upstream/include/pathsys.h"
#include "../upstream/include/visible.h"
#include "../upstream/include/gameinfo.h"
#include "../upstream/include/taimdmd.h"
#include "../upstream/include/sprite.h"
#include "../upstream/include/drawarea.h"
#include "../upstream/include/tech.h"
class ProbeGame:public TRIBE_Game {public:ProbeGame(RGE_Prog_Info*p):TRIBE_Game(p,0){} void attach(RGE_Game_World*w){world=w;} int local(unsigned seed){random_game_seed=seed;random_map_seed=seed;player_game_info=new RGE_Game_Info((char*)"/profile.nfo");player_game_info->add_new_person((char*)"WASM test");player_game_info->set_current_person(0);return setup_comm();} void world_graphics(){setup_shapes();prog_info->interface_style=2;} int ui_program(int mode){int old=prog_mode;set_prog_mode(mode);return old;} int ui_state(int f){return f?master_obj_id:game_mode;} int graphics(int w,int h){
 draw_system=new TDrawSystem;draw_system->DrawType=1;draw_system->ScreenMode=2;
 draw_area=new TDrawArea((char*)"Browser game",1);draw_area->Init(draw_system,w,h,0,0);draw_system->DrawArea=draw_area;
 map_save_area=new TDrawArea((char*)"Browser minimap",1);map_save_area->Init(draw_system,w,h,0,0);
 return setup_fonts()&&setup_chat();
 } void notification(int n,long a,long b,long c,long d){printf("notify %d %ld %ld %ld %ld\n",n,a,b,c,d);TRIBE_Game::notification(n,a,b,c,d);}};
static ProbeGame* game;
static TRIBE_World* world;
static RGE_Prog_Info info;
static int diagnostic_clock;
static int local_player=1;
int wasm_capture_orders=0;
EM_JS(void,wasm_send_order,(const void* data,int size),{if(!Module['nativeCommand'])throw new Error('Native command transport missing');Module['nativeCommand'](Array.from(HEAPU8.subarray(data,data+size)));});
int wasm_capture_order(const void* data,int size){if(!wasm_capture_orders)return 0;wasm_send_order(data,size);return 1;}
TRIBE_Game* wasm_simulation_game(){return game;}
int wasm_game_graphics(int w,int h){return game&&game->graphics(w,h);}
void wasm_world_graphics(){if(game)game->world_graphics();}
int wasm_ui_program(int mode){return game?game->ui_program(mode):0;}
int wasm_game_ui_state(int f){return game?game->ui_state(f):0;}
extern int ENABLE_COMPRESSION;
extern int do_debug_timeGetTime;
extern "C" {
EMSCRIPTEN_KEEPALIVE const char* engine_probe_string(int id){static char buffer[256];LoadStringA(0,id,buffer,sizeof(buffer));return buffer;}
EMSCRIPTEN_KEEPALIVE const char* engine_probe_ai_name(){if(!world||world->player_num<3)return "";TRIBE_Player*p=(TRIBE_Player*)world->players[2];return p->playerAI?p->playerAI->buildAI.buildListName():"";}
EMSCRIPTEN_KEEPALIVE const char* engine_probe_ai_status(int field){if(!world||world->player_num<3||field<1||field>6)return "";return ((TRIBE_Player*)world->players[2])->aiStatus(field);}
EMSCRIPTEN_KEEPALIVE int engine_probe_load(const char* file) {
    if (game) return -1;
    memset(&info,0,sizeof(info));
    strcpy(info.prog_name,"AoE WASM engine probe");
    strcpy(info.prog_version,"port-in-progress");
    strcpy(info.registry_key,"AoE-WASM-Probe");
    strcpy(info.data_dir,"data2/");strcpy(info.ai_dir,"data2/");
    strcpy(info.sounds_dir,"sound/");strcpy(info.save_dir,"savegame/");
    info.main_wid=800;info.main_hgt=600;info.max_players=8;
    puts("probe: constructing game");
    game=new ProbeGame(&info);
    game->setMapType(TRIBE_Game::AllWater);
    printf("probe: game constructor status %d\n",game->get_error_code());
    puts("probe: constructing world");
    world=new TRIBE_World;
    game->attach(world);
    ENABLE_COMPRESSION=0;
    puts("probe: reading uncompressed DAT through recovered engine");
    return world->init((char*)file,0,0);
}
EMSCRIPTEN_KEEPALIVE int engine_probe_count(int field) {
    if(!world)return -1;
    switch(field){case 0:return world->master_player_num;case 1:return world->sprite_num;case 2:return world->sound_num;case 3:return world->terrain_num;}
    return -1;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_units(int civ) {
    if(!world||civ<0||civ>=world->master_player_num||!world->master_players[civ])return -1;
    return world->master_players[civ]->master_object_num;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_hp(int civ,int id) {
    int n=engine_probe_units(civ);if(id<0||id>=n)return -1;
    RGE_Master_Static_Object* unit=world->master_players[civ]->master_objects[id];
    return unit?unit->hp:-1;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_new_game(unsigned seed,int computer) {
    if(!game||!world)return -1;
    game->setNumberPlayers(2);
    puts("probe: constructing local communication handler");
    if(!game->local(seed))return -2;
    for(int i=1;i<=8;i++){comm->SetPlayerHumanity(i,i<=2?((computer&&i==2)?ME_COMPUTER:ME_HUMAN):ME_ABSENT);game->setPlayerID(i,i);}
    RGE_Player_Info setup;memset(&setup,0,sizeof(setup));
    setup.player_num=2;setup.map_width=72;setup.map_height=72;setup.map_type=game->mapType();
    for(int i=0;i<2;i++){strcpy(setup.name[i],i?"Player 2":"Player 1");setup.type[i]=(computer&&i==1)?4:0;setup.tribe[i]=i+1;setup.color[i]=i;}
    puts("probe: generating random game");int result=world->new_game(setup,1);world->update_mutual_allies();world->set_map_visible(0);world->set_map_fog(1);numberPathingIterations=2500;do_fixed_update=1;fixed_update_time=50;comm->MultiplayerGameStart();return result;
}
EMSCRIPTEN_KEEPALIVE int engine_mp_option(int field,int value){
    if(!game||!world||world->player_num)return 0;
    switch(field){
    case 0:game->setMapSize((TRIBE_Game::MapSize)value);break;
    case 1:game->setMapType((TRIBE_Game::MapType)value);break;
    case 2:game->setVictoryType((TRIBE_Game::VictoryType)value,game->victoryAmount());break;
    case 3:game->setVictoryType(game->victoryType(),value);break;
    case 4:game->setResourceLevel((TRIBE_Game::ResourceLevel)value);break;
    case 5:game->setStartingAge((TRIBE_Game::Age)value);break;
    case 6:game->setPopLimit(value);break;
    case 7:game->setRandomizePositions(value);break;
    case 8:game->setFullVisibility(value);break;
    case 9:game->setFullTechTree(value);break;
    case 10:game->setAllowCheatCodes(value);break;
    case 11:game->setMpPathFinding(value);break;
    case 12:game->setDifficulty(value);break;
    case 13:game->setDeathMatch(value);break;
    default:return 0;
    }return 1;
}
static RGE_Player_Info mp_setup;
EMSCRIPTEN_KEEPALIVE int engine_mp_player(int slot,const char*name,int civ,int color,int team){
    if(!game||!world||world->player_num||slot<1||slot>8||civ<1||civ>16||color<1||color>8||team<1||team>5||strlen(name)>32)return 0;
    strcpy(mp_setup.name[slot-1],name);mp_setup.type[slot-1]=0;mp_setup.tribe[slot-1]=civ;mp_setup.color[slot-1]=color-1;
    game->setCivilization(slot-1,civ);game->setPlayerColor(slot-1,color);game->setPlayerTeam(slot-1,team);return 1;
}
EMSCRIPTEN_KEEPALIVE int engine_mp_start(unsigned seed,int players,int me){
    if(!game||!world||world->player_num||players<2||players>8||me<1||me>players)return 0;
    local_player=me;game->setNumberPlayers(players);game->setSinglePlayerGame(0);game->setMultiplayerGame(1);
    if(!game->local(seed))return 0;
    for(int i=1;i<=8;i++){comm->SetPlayerHumanity(i,i<=players?ME_HUMAN:ME_ABSENT);game->setPlayerID(i,i);}
    const int sizes[]={72,96,120,144,200,250};int size=game->mapSize();if(size<0||size>5)return 0;
    mp_setup.player_num=players;mp_setup.map_width=mp_setup.map_height=sizes[size];mp_setup.map_type=game->mapType();
    int result=world->new_game(mp_setup,1);world->update_mutual_allies();world->set_map_visible(game->fullVisibility());world->set_map_fog(1);
    numberPathingIterations=2500;do_fixed_update=1;fixed_update_time=50;comm->MultiplayerGameStart();return result;
}
EMSCRIPTEN_KEEPALIVE void engine_mp_pause(int value){if(game){if(value)comm->RequestPauseGame(0);else comm->RequestResumeGame(0);}}
EMSCRIPTEN_KEEPALIVE int engine_mp_players(){return world?world->player_num-1:0;}
EMSCRIPTEN_KEEPALIVE int engine_mp_checksum(int field){long a,b,c;if(!game||!game->GetWorldChecksums(a,b,c))return 0;return field==0?a:field==1?b:c;}
EMSCRIPTEN_KEEPALIVE void engine_mp_perspective(int player){if(world&&player>0&&player<world->player_num)world->curr_player=player;}
EMSCRIPTEN_KEEPALIVE int engine_mp_visible(int player,int x,int y){if(!world||player<1||player>=world->player_num||x<0||y<0||x>=world->map->map_width||y>=world->map->map_height)return -1;return world->players[player]->visible->get_visible(x,y);}
EMSCRIPTEN_KEEPALIVE int engine_mp_research(int player,int tech){if(!world||player<1||player>=world->player_num)return -1;TRIBE_Player_Tech*t=((TRIBE_Player*)world->players[player])->tech_tree;return tech>=0&&tech<t->tech_player_tree_num?t->get_tech_state(tech):-1;}
EMSCRIPTEN_KEEPALIVE int engine_mp_research_location(int tech){return world&&world->tech&&tech>=0&&tech<world->tech->tech_tree_num?world->tech->tech_tree[tech].build_obj_id:-1;}
EMSCRIPTEN_KEEPALIVE int engine_mp_packet(const unsigned char*bytes,int size){if(!world||size<1||size>1024)return 0;void*order=malloc(size);memcpy(order,bytes,size);world->commands->do_command(order);free(order);return 1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_objects(int player){if(!world||player<0||player>=world->player_num)return -1;return world->players[player]->objects->number_of_objects+world->players[player]->sleeping_objects->number_of_objects;}
EMSCRIPTEN_KEEPALIVE int engine_probe_object_id(int player,int index){
    if(!world||player<0||player>=world->player_num||index<0)return -1;
    RGE_Player*p=world->players[player];RGE_Object_List*list=p->objects;
    if(index>=list->number_of_objects){index-=list->number_of_objects;list=p->sleeping_objects;}
    RGE_Object_Node*n=list->list;for(int i=0;n&&i<index;i++)n=n->next;return n?n->node->id:-1;
}
EMSCRIPTEN_KEEPALIVE double engine_probe_object_value(int id,int field){
    RGE_Static_Object*o=world?world->object(id):0;if(!o)return -1;
    switch(field){case 0:return o->master_obj->id;case 1:return o->world_x;case 2:return o->world_y;case 3:return o->hp;case 4:return o->object_state;case 5:return o->owner->id;case 6:return o->master_obj->object_group;case 7:return o->attribute_amount_held;case 8:return o->attribute_type_held;
    case 9:return o->sprite?o->sprite->id:-1;case 10:return o->sprite_list?o->sprite_list->frame(o->sprite):0;case 11:return o->facet;case 12:return o->world_z;case 13:return o->master_obj->hp;case 14:return o->master_obj->master_type;
    case 15:return o->tile?o->tile->screen_xpos+o->screen_x_offset:-1;case 16:return o->tile?o->tile->screen_ypos+o->tile->height*world->map->tile_half_height+o->screen_y_offset:-1;
    case 17:return o->inside_obj?o->inside_obj->id:-1;}return -1;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_clock(int deterministic){diagnostic_clock=deterministic!=0;do_debug_timeGetTime=debug_timeGetTime_on=diagnostic_clock;return diagnostic_clock;}
EMSCRIPTEN_KEEPALIVE int engine_probe_tick(int count){
    if(!world||count<0||count>10000)return -1;
    for(int i=0;i<count;i++){
        // Offline acceleration must advance the AI's wall-clock retry timers.
        // Use the recovered engine's own deterministic debug clock, only when
        // explicitly requested by tests. Browser sessions use real timeGetTime.
        if(diagnostic_clock){if((unsigned)debug_timeGetTime_time<world->world_time)debug_timeGetTime_time=world->world_time;debug_timeGetTime_cnt=0;}
        world->update();
    }
    return world->world_time;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_move(int id,float x,float y){RGE_Static_Object*o=world?world->object(id):0;if(!o)return 0;world->commands->command_move(&o,1,0,x,y);return 1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_train(int id,int type){RGE_Static_Object*o=world?world->object(id):0;if(!o)return 0;((TRIBE_Command*)world->commands)->command_queue(o,type,1);return 1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_work(int id,int target){RGE_Static_Object*o=world?world->object(id):0;RGE_Static_Object*t=world?world->object(target):0;if(!o||!t)return 0;world->commands->command_work(&o,1,t,t->world_x,t->world_y);return 1;}
EMSCRIPTEN_KEEPALIVE double engine_probe_resource(int player,int resource){if(!world||player<0||player>=world->player_num||resource<0||resource>=world->players[player]->attribute_num)return -1;return world->players[player]->attributes[resource];}
EMSCRIPTEN_KEEPALIVE double engine_probe_unit(int civ,int id,int field){
    if(id<0||id>=engine_probe_units(civ))return -1;
    RGE_Master_Static_Object*u=world->master_players[civ]->master_objects[id];if(!u)return -1;
    switch(field){case 0:return u->id;case 1:return u->master_type;case 2:return u->hp;case 3:return u->object_group;case 4:return u->los;case 5:return u->available;}return -1;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_placement(int player,int type,float x,float y){
    if(!world||player<1||player>=world->player_num)return -1;RGE_Player*p=world->players[player];if(type<0||type>=p->master_object_num||!p->master_objects[type])return -1;
    return p->master_objects[type]->check_placement(p,x,y,0,1,1,1,1,1,1);
}
EMSCRIPTEN_KEEPALIVE int engine_probe_build(int id,int type,float x,float y){RGE_Static_Object*o=world?world->object(id):0;if(!o)return 0;((TRIBE_Command*)world->commands)->command_build(&o,1,type,x,y);return 1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_tile(int x,int y,int field){if(!world||x<0||y<0||x>=world->map->map_width||y>=world->map->map_height)return -1;RGE_Tile*t=&world->map->map_row_offset[y][x];switch(field){case 0:return t->terrain_type;case 1:return t->height;case 2:return t->tile_type;case 3:return world->players[1]->visible->get_visible(x,y);}return -1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_ai(int field){if(!world||world->player_num<3)return -1;TRIBE_Player*p=(TRIBE_Player*)world->players[2];switch(field){case 0:return p->type;case 1:return world->currentUpdateComputerPlayer;case 2:return p->playerAI?p->playerAI->buildAI.buildListLength():-1;case 3:return p->playerAI?p->playerAI->firstUpdate:-1;case 4:return world->game_state;case 5:return p->playerAI?p->playerAI->buildAI.numberItemsBuilt():-1;}return -1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_rand(unsigned seed,int count) {srand(seed);int r=0;for(int i=0;i<count;++i)r=rand();return r;}
EMSCRIPTEN_KEEPALIVE int engine_probe_options(int mapType,int difficulty){
    if(!game||!world||world->player_num>0||mapType<0||mapType>8||difficulty<0||difficulty>4)return 0;
    game->setMapType((TRIBE_Game::MapType)mapType);game->setDifficulty(difficulty);game->setVictoryType(TRIBE_Game::VictoryConquest,0);return 1;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_map_info(int field){if(!world||!world->map)return -1;RGE_Map*m=world->map;switch(field){case 0:return m->map_width;case 1:return m->map_height;case 2:return m->tile_half_width;case 3:return m->tile_half_height;case 4:return m->elev_height;}return -1;}
// Same terrain-frame selection as RGE_View::get_tile_picture, without creating
// a Windows view or asking the legacy TShape loader to create a graphics device.
EMSCRIPTEN_KEEPALIVE int engine_probe_ground(int x,int y,int field){
    if(!world||x<0||y<0||x>=world->map->map_width||y>=world->map->map_height)return -1;
    RGE_Map*m=world->map;RGE_Tile*t=&m->map_row_offset[y][x];int terrain=t->terrain_type;
    if(m->terrain_types[terrain].terrain_to_draw>=0)terrain=m->terrain_types[terrain].terrain_to_draw;
    if(terrain>=32||t->tile_type>=19)return -1;RGE_Tile_Set*s=&m->terrain_types[terrain];RGE_TOB_Picts*p=&s->tiles[t->tile_type];int variant=0;
    if(p->count>1&&(s->rows>1||s->cols>1))variant=(s->rows>0?y%s->rows:0)*s->cols+(s->cols>0?x%s->cols:0);
    if(variant>=p->count)variant=0;
    switch(field){case 0:return s->resource_id;case 1:return p->count?p->shape_index+p->animations*variant:-1;case 2:return t->screen_xpos;case 3:return t->screen_ypos;case 4:return terrain;case 5:return t->border_type;case 6:return t->border_shape;}return -1;
}
EMSCRIPTEN_KEEPALIVE int engine_probe_player_unit(int player,int id){if(!world||player<1||player>=world->player_num)return 0;RGE_Player*p=world->players[player];return id>=0&&id<p->master_object_num&&p->master_objects[id]?p->master_objects[id]->available:0;}
EMSCRIPTEN_KEEPALIVE int engine_probe_stop(int id){RGE_Static_Object*o=world?world->object(id):0;if(!o)return 0;world->commands->command_stop(&o,1);return 1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_research(int id,int tech){RGE_Static_Object*o=world?world->object(id):0;if(!o)return 0;((TRIBE_Command*)world->commands)->command_research(o,tech);return 1;}
EMSCRIPTEN_KEEPALIVE int engine_probe_research_state(int tech){if(!world||world->player_num<2)return -1;return ((TRIBE_Player*)world->players[1])->researchState(tech);}
EMSCRIPTEN_KEEPALIVE int engine_probe_player_state(int player){if(!world||player<1||player>=world->player_num)return -1;return world->players[player]->game_status;}
}
