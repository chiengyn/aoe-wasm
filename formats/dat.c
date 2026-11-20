/* AoE / RoR VER 3.7 serialized world reader, written for this port.
 * Little-endian file offsets are never dereferenced as saved Windows pointers.
 * Layout provenance: pinned aoe1-decomp serialization routines; see README.
 * Exported records contain only 32-bit fields (float fields documented in JS).
 */
#include <stdint.h>
#include <stddef.h>
#define NG 4096
#define NU 16384
#define NC 64
#define NT 65536
#define ND 32768
typedef struct { int32_t present,id,offset,name,slp,layer,color,frames,angles;
  float speed,duration,pause; int32_t flags,mirror,delta_start,delta_count,loaded,color_flag; } Graphic;
typedef struct { int32_t graphic,x,y,facet; } Delta;
typedef struct { int32_t type,id,action,group,object,terrain,move,work,work2,carry; } Task;
typedef struct { int32_t slot,id,civ,type,offset,name,name_len,language,group,hp;
  float los,speed; int32_t enabled,stand,death,undead,walk,run,attack,construction,task_start,task_count,train_at;
  float train_time; int32_t icon; } Unit;
typedef struct { int32_t name,unit_start,unit_count,slots,culture; } Civ;
typedef struct { const uint8_t *b; uint32_t n,p; int error; } Reader;
static Graphic graphics[NG]; static Unit units[NU]; static Civ civs[NC];
static Task tasks[NT]; static Delta deltas[ND];
/* counts: graphic slots, units, civs, tasks, deltas, effects, techs, maps. */
static int32_t counts[8], positions[10], last_error, error_offset, stage;
static uint32_t take(Reader*r,uint32_t n) {
  if(r->error)return 0;
  if(n>r->n-r->p){r->error=-2;return 0;}
  uint32_t p=r->p;r->p+=n;return p;
}
static uint32_t u8(Reader*r){uint32_t p=take(r,1);return r->error?0:r->b[p];}
static uint32_t u16(Reader*r){uint32_t p=take(r,2);return r->error?0:(uint32_t)r->b[p]|((uint32_t)r->b[p+1]<<8);}
static int32_t i16(Reader*r){return (int16_t)u16(r);}
static uint32_t u32(Reader*r){uint32_t p=take(r,4);return r->error?0:(uint32_t)r->b[p]|((uint32_t)r->b[p+1]<<8)|((uint32_t)r->b[p+2]<<16)|((uint32_t)r->b[p+3]<<24);}
static float f32(Reader*r){union{uint32_t u;float f;}v;v.u=u32(r);return v.f;}
static int count(Reader*r,int n,int limit){if(n<0||n>limit){r->error=-3;return 0;}return n;}
static int c16(Reader*r,int max){return count(r,i16(r),max);}
static int c32(Reader*r,int max){return count(r,(int32_t)u32(r),max);}
static int flag(Reader*r,uint32_t p,int i){return !r->error&&(r->b[p+4*i]||r->b[p+4*i+1]||r->b[p+4*i+2]||r->b[p+4*i+3]);}
static void checkpoint(Reader*r,int s){if(!r->error){stage=s;positions[s]=r->p;}}
static void read_graphic(Reader*r,int index){
  Graphic*g=&graphics[index];g->present=1;g->offset=r->p;g->name=take(r,21);take(r,13);
  g->slp=(int32_t)u32(r);g->loaded=u8(r);g->color_flag=u8(r);g->layer=u8(r);g->color=i16(r);take(r,9);
  g->delta_count=c16(r,ND-counts[4]);take(r,2);int angle_sound=u8(r);
  g->frames=c16(r,8192);g->angles=c16(r,256);g->speed=f32(r);g->duration=f32(r);g->pause=f32(r);
  g->flags=u8(r);g->id=i16(r);g->mirror=u8(r);g->delta_start=counts[4];
  for(int j=0;j<g->delta_count&&!r->error;j++){
    Delta*d=&deltas[counts[4]++];d->graphic=i16(r);take(r,6);d->x=i16(r);d->y=i16(r);d->facet=i16(r);take(r,2);
  }
  if(angle_sound)take(r,(uint32_t)g->angles*12);
}
static void read_unit(Reader*r,int civ,int slot){
  if(counts[1]>=NU){r->error=-3;return;}Unit*u=&units[counts[1]++];
  u->slot=slot;u->civ=civ;u->offset=r->p;u->type=u8(r);u->name_len=c16(r,4096);u->id=i16(r);u->language=u16(r);
  take(r,2);u->group=i16(r);u->stand=i16(r);u->death=i16(r);u->undead=i16(r);take(r,1);
  u->hp=i16(r);u->los=f32(r);take(r,19);u->icon=i16(r);take(r,3);u->enabled=u8(r);
  /* Placement terrains, clearance, elevation/fog, movement restriction. */
  take(r,8+8+2+2+1);
  /* Resource capacity/decay, interaction flags, minimap mode/color. */
  take(r,2+4+5+4+1);
  /* Help IDs, four behavior flags, selection effect/color, outline, stores. */
  take(r,12+4+2+12+21);
  int damage=u8(r);take(r,(uint32_t)damage*5+6);u->name=take(r,u->name_len);take(r,2);
  u->speed=0;u->walk=u->run=u->attack=u->construction=u->train_at=-1;u->train_time=0;u->task_start=counts[3];u->task_count=0;
  int type=u->type;
  if(type==10||type==90)return;
  if(type!=20&&type!=25&&type!=30&&type!=40&&type!=50&&type!=60&&type!=70&&type!=80){r->error=-4;return;}
  u->speed=f32(r);if(type<30)return;
  u->walk=i16(r);u->run=i16(r);take(r,13);if(type<40)return;
  take(r,20);u->task_count=c16(r,NT-counts[3]);
  for(int j=0;j<u->task_count&&!r->error;j++){
    Task*t=&tasks[counts[3]++];t->type=i16(r);t->id=i16(r);take(r,1);t->action=i16(r);t->group=i16(r);t->object=i16(r);t->terrain=i16(r);
    take(r,8+12+1+4+2+4+3);t->move=i16(r);t->work=i16(r);t->work2=i16(r);t->carry=i16(r);take(r,4);
  }
  if(type<50)return;
  take(r,1);int weapons=c16(r,1024);take(r,weapons*4);int armors=c16(r,1024);take(r,armors*4);
  take(r,2+12+4+1+2+12+1+4);u->attack=i16(r);take(r,12);
  if(type==60)take(r,9);
  if(type<70)return;
  take(r,18);u->train_time=i16(r);u->train_at=i16(r);take(r,3);
  if(type==80){u->construction=i16(r);take(r,14);}
}
int32_t dat_parse(const uint8_t*b,uint32_t n){
  Reader r={b,n,0,0};for(int i=0;i<8;i++)counts[i]=0;for(int i=0;i<10;i++)positions[i]=0;
  last_error=0;error_offset=0;stage=0;
  static const uint8_t magic[8]={'V','E','R',' ','3','.','7',0};
  if(n<8){r.error=-2;goto done;}for(int i=0;i<8;i++)if(b[i]!=magic[i]){r.error=-1;goto done;}take(&r,8);
  checkpoint(&r,1);int terrain=c16(&r,256),size=c16(&r,256);uint32_t ptr=take(&r,terrain*4);
  for(int i=0;i<terrain&&!r.error;i++)if(flag(&r,ptr,i))take(&r,size*4);
  int colors=c16(&r,256);take(&r,colors*36);
  checkpoint(&r,2);int sounds=c16(&r,8192);
  for(int i=0;i<sounds&&!r.error;i++){take(&r,4);int entries=c16(&r,4096);take(&r,4+entries*19);}
  checkpoint(&r,3);counts[0]=c16(&r,NG);ptr=take(&r,counts[0]*4);
  for(int i=0;i<counts[0]&&!r.error;i++){graphics[i].present=0;if(flag(&r,ptr,i))read_graphic(&r,i);}
  checkpoint(&r,4);take(&r,36304); /* Serialized RGE_Map size, NOT a file offset. */
  checkpoint(&r,5);counts[7]=c32(&r,4096);take(&r,4+counts[7]*72);
  for(int i=0;i<counts[7]&&!r.error;i++){
    take(&r,36);int lands=c32(&r,8192);take(&r,4+lands*44);
    int terrains=c32(&r,8192);take(&r,4+terrains*24);
    int objects=c32(&r,8192);take(&r,4+objects*44);
    int elevations=c32(&r,8192);take(&r,4+elevations*24);
  }
  checkpoint(&r,6);counts[5]=c32(&r,8192);
  for(int i=0;i<counts[5]&&!r.error;i++){take(&r,31);int commands=c16(&r,8192);take(&r,commands*11);}
  checkpoint(&r,7);counts[2]=c16(&r,NC);
  for(int c=0;c<counts[2]&&!r.error;c++){
    Civ*v=&civs[c];take(&r,1);v->name=take(&r,20);int resources=c16(&r,4096);take(&r,2+resources*4);v->culture=u8(&r);
    v->slots=c16(&r,8192);uint32_t pointers=take(&r,v->slots*4);v->unit_start=counts[1];
    for(int u=0;u<v->slots&&!r.error;u++)if(flag(&r,pointers,u))read_unit(&r,c,u);
    v->unit_count=counts[1]-v->unit_start;
  }
  checkpoint(&r,8);counts[6]=c16(&r,8192);
  for(int i=0;i<counts[6]&&!r.error;i++){take(&r,52);int len=c16(&r,4096);take(&r,len);}
  checkpoint(&r,9);if(!r.error&&r.p!=n)r.error=-5;
done:
  last_error=r.error;error_offset=r.p;return r.error;
}
int32_t dat_count(int32_t k){return k>=0&&k<8&&!last_error?counts[k]:-1;}
int32_t dat_position(int32_t k){return k>=0&&k<10?positions[k]:-1;}
int32_t dat_error_offset(void){return error_offset;}
int32_t dat_stage(void){return stage;}
const Graphic*dat_graphic(int32_t i){return !last_error&&i>=0&&i<counts[0]?&graphics[i]:0;}
const Unit*dat_unit(int32_t i){return !last_error&&i>=0&&i<counts[1]?&units[i]:0;}
const Civ*dat_civ(int32_t i){return !last_error&&i>=0&&i<counts[2]?&civs[i]:0;}
const Task*dat_task(int32_t i){return !last_error&&i>=0&&i<counts[3]?&tasks[i]:0;}
const Delta*dat_delta(int32_t i){return !last_error&&i>=0&&i<counts[4]?&deltas[i]:0;}
/* Same facet-index calculation as RGE_Sprite::get_facetindex. Output packs
 * mirror in bit 30; -1 rejects caller indices instead of wrapping silently. */
int32_t dat_frame(int32_t id,int32_t facet,int32_t frame){
  const Graphic*g=dat_graphic(id);if(!g||!g->present||facet<0||facet>=g->angles||frame<0||frame>=g->frames)return -1;
  int index=frame,mirror=0;
  if(g->mirror&&g->angles==2)mirror=facet;
  else if(g->mirror&&(facet>g->mirror||facet<(g->angles>>2))){
    int i=g->angles>>1;i=facet>i?i-facet+g->angles:i-facet;index+=(i-(g->angles>>2))*g->frames;mirror=1;
  }else index+=(g->mirror?facet-(g->angles>>2):facet)*g->frames;
  if(index<0||index>=0x40000000)return -1;return index|(mirror<<30);
}
