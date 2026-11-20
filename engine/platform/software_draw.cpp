// Portable indexed renderer for the recovered Asm_draw/Drawarea_asm routines.
// Compare against the user's retail executable with tests/test_drawing_oracle.py.
// Spans, palette indices and shadow tables stay native; no RGBA approximation.
#include "port_prefix.h"
#include "../upstream/include/drawarea.h"
#include "../upstream/include/asm_draw.h"
#include <emscripten.h>
#include <algorithm>
#include <stdint.h>
#include <vector>
#include "draw_mask_lanes.h"
static unsigned char** rows;
static VSpan_Node** heads;
static int min_x,min_y,max_x,max_y;
static unsigned char xform;
static unsigned char* xlate;
static uint32_t masks[4],values[4];
extern "C" {
void ASMSet_Shadowing(unsigned long a,unsigned long b,unsigned long c,unsigned long d){masks[0]=masks[2]=a;values[0]=values[2]=b;masks[1]=masks[3]=c;values[1]=values[3]=d;}
void ASMSet_Color_Xform(int color){xform=color;}
int ASMGet_Color_Xform(){return xform;}
void ASMSet_Xlate_Table(void* table){xlate=(unsigned char*)table;}
void ASMSet_Surface_Info(void* offsets,void* h,void*,long l,long t,long r,long b){rows=(unsigned char**)offsets;heads=(VSpan_Node**)h;min_x=l;min_y=t;max_x=r;max_y=b;}
void ASMDraw_HLine(long a,long b,long y,unsigned long color){
 if(y<min_y||y>max_y)return;if(a>b)std::swap(a,b);a=std::max(a,(long)min_x);b=std::min(b,(long)max_x);
 for(VSpan_Node*n=heads[y];n;n=n->Next){int l=std::max(a,(long)n->StartPx),r=std::min(b,(long)n->EndPx);if(l<=r)memset(rows[y]+l,color,r-l+1);}
}
static bool visible(int x,int y){for(VSpan_Node*n=heads[y];n;n=n->Next){if(x<n->StartPx)return false;if(x<=n->EndPx)return true;}return false;}
static unsigned read16(const unsigned char*p){return p[0]|(p[1]<<8);}
static unsigned read32(const unsigned char*p){return read16(p)|(read16(p+2)<<16);}
// Opcode length and payload are shared by normal decoding and extended 4E skip.
static int decode(const unsigned char*&p,int&kind,const unsigned char*&data,bool flip){
 unsigned c=*p++,op=c&15;int count=0;kind=op;data=0;
 if((c&3)==0){count=c>>2;kind=0;}
 else if((c&3)==1){count=c>>2;kind=1;}
 else if(op==2||op==3){count=((c&240)<<4)|*p++;kind=op==2?0:1;}
 else if(op==6||op==7||op==10||op==11){count=c>>4;if(!count)count=*p++;}
 else if(op==14){kind=c;switch(c){case 0x1e:count=1;break;case 0x2e:count=*p++;break;case 0x3e:case 0x4e:{if((c==0x3e)==flip){int skip;const unsigned char*q;count=decode(p,skip,q,flip);}break;}default:abort();}}
 else if(op==15){kind=15;return 0;}else abort();
 data=p;if(kind==0||kind==6)p+=count;else if(kind==7||kind==10)++p;
 return count;
}
void ASMDraw_Sprite(void* input,long x,long y,long width,long height,long offsets,long outline,int flags){
 const unsigned char*base=(unsigned char*)input;
 if(y+height-1<min_y||y>max_y||x>max_x||x+width-1<min_x)return;
 int first=0,last=height;if(y+height-1>max_y)last=max_y-y+1;
 // Preserve the original top-clipping behavior (destination starts at zero).
 int dest_y=y;if(y<min_y){first=min_y-y;dest_y=0;}
 for(int row=first;row<last;++row,++dest_y){
  if(!heads[dest_y])continue;unsigned left=read16(base+outline+row*4);if(left&0x8000)continue;
  const unsigned char*p=base+read32(base+offsets+row*4);int px=left;
  for(int instructions=0;instructions<width*4+1024;++instructions){
   int kind;const unsigned char*data;int count=decode(p,kind,data,(flags&2)!=0);if(kind==15)break;
   if(kind==0||kind==6||kind==7||kind==10||kind==11){
    const bool flip=(flags&2)!=0;int first_x=x+(flip?width-1-px:px),last_x=first_x+(flip?-count+1:count-1);
    // Rows clip to the span list only. As in the assembly, min_x/max_x only reject a sprite lying wholly
    // outside them: the view narrows them to one scroll strip while a tile's spans cover two strips.
    for(VSpan_Node*span=heads[dest_y];span;span=span->Next){
     int l=std::max(std::min(first_x,last_x),span->StartPx),r=std::min(std::max(first_x,last_x),span->EndPx);
     if(l>r)continue;
     int start=flip?r:l,length=r-l+1,alignment=(uintptr_t)(rows[dest_y]+start)&3;
     int base_length=length<8?length:8+length%4,extra=length-base_length,prefix=flip?(alignment+1)%4:(-alignment)&3;
     int kind_index=kind==6?1:(kind==7||kind==10)?2:kind==11?3:0;
     for(int i=0;i<length;i++){
      int dx=start+(flip?-i:i),source_index=flip?first_x-dx:dx-first_x;
      unsigned char*dst=rows[dest_y]+dx;unsigned color;
      if(kind==11){if(!xlate)abort();color=xlate[*dst];}else color=data[(kind==0||kind==6)?source_index:0];
      if(kind==6||kind==10)color|=xform;
      if(flags&1){
       // Original optimized codelets sometimes reuse the low WORD for short
       // prefixes/tails. A simple address%4 pattern differs from the EXE.
       int lane;if(extra&&i>=prefix&&i<prefix+extra)lane=flip?3-(i-prefix)%4:(i-prefix)%4;
       else lane=mask_lanes[kind_index][flip][base_length][alignment][i-(i>=prefix+extra?extra:0)];
       unsigned m=(masks[dest_y&3]>>(lane*8))&255,v=(values[dest_y&3]>>(lane*8))&255;
       color=(kind==11&&!flip)?((color|v)&m):((color&m)|v);
      }
      *dst=color;
     }
    }
   }
   // Outline pixels are disabled by default, as in the assembly state.
   if(kind==0x3e&&(flags&2))px-=count;else px+=count;
  }
 }
}
}

// The recovered routines lock and unlock only DirectDraw areas (DrawType 2); software areas keep Bits.
static bool uses_ddraw(TDrawArea* a){return a->DrawSystem&&a->DrawSystem->DrawType==2;}

// The assembly locks the destination (on DirectDraw areas) and leaves it locked: RGE_View::view_function_terrain
// draws the sprites into it next and unlocks it at the end.
void TDrawArea::PtrSpanCopy(TDrawArea* dest,long x,long y,tagRECT* r,unsigned char** spans){
 if(!r||r->left<0||r->left>r->right||r->right>=Width||r->top<0||r->top>r->bottom||r->bottom>=Height||x<0||x>=dest->Width||y<0||y>=dest->Height)return;
 int count=std::min((long)(r->bottom-r->top+1),(long)dest->Height-y);
 if(uses_ddraw(this)&&!dest->Lock((char*)"PtrSpanCopy",1))return;
 for(int row=0;row<count;row++)for(VSpan_Node*n=(VSpan_Node*)spans[r->top+row];n;n=n->Next)
  memcpy((unsigned char*)dest->CurDisplayOffsets[y+row]+x+n->StartPx,(unsigned char*)CurDisplayOffsets[r->top+row]+r->left+n->StartPx,n->EndPx-n->StartPx+1);
}
void TDrawArea::OverlayMemCopy(tagRECT&src,tagRECT&dst,int dx,int dy,int ox,int oy){
 if(!dx&&!dy)return;
 // Locked for the copy and unlocked after it on DirectDraw areas, as in the assembly.
 bool ddraw=uses_ddraw(this);if(ddraw&&!Lock((char*)"OverlayMemCopy",1))return;
 int count=src.bottom-src.top+1,words=(src.right-src.left+4)/4,step=dy<0?-1:1;
 int sy=oy+(dy<0?src.bottom:src.top),ty=oy+(dy<0?dst.bottom:dst.top);
 bool reverse=!dy&&dx<0;int sx=ox+(reverse?src.right-3:src.left),tx=ox+(reverse?dst.right-3:dst.left);
 for(int row=0;row<count;row++,sy+=step,ty+=step)for(int w=0;w<words;w++){
  uint32_t value;int off=reverse?-4*w:4*w;memcpy(&value,(unsigned char*)CurDisplayOffsets[sy]+sx+off,4);memcpy((unsigned char*)CurDisplayOffsets[ty]+tx+off,&value,4);
 }
 if(ddraw)Unlock((char*)"OverlayMemCopy");
}

// Test-only surface harness. Its byte layout is documented in the oracle test.
extern "C" EMSCRIPTEN_KEEPALIVE const unsigned char* drawing_probe(const unsigned char* input){
 const int32_t*v=(const int32_t*)input;static std::vector<unsigned char> pixels;static std::vector<unsigned char*> offsets;static std::vector<VSpan_Node> nodes;static std::vector<VSpan_Node*> h,t;
 int w=v[1],height=v[2],pitch=v[3],align=v[4];pixels.assign(height*pitch+16,205);offsets.resize(height);nodes.resize(height*2);h.resize(height);t.resize(height);
 for(int y=0;y<height;y++){offsets[y]=&pixels[y*pitch+8+align];VSpan_Node*a=&nodes[y*2],*b=a+1;a->StartPx=v[19];a->EndPx=v[20];b->StartPx=v[21];b->EndPx=v[22];a->Prev=0;b->Next=0;a->Next=b->StartPx<=b->EndPx?b:0;b->Prev=a;h[y]=a;t[y]=a->Next?b:a;}
 unsigned char table[256];for(int i=0;i<256;i++)table[i]=(i*37+11)&255;
 ASMSet_Surface_Info(&offsets[0],&h[0],&t[0],v[10],v[11],v[12],v[13]);ASMSet_Color_Xform(v[14]);ASMSet_Shadowing(v[15],v[16],v[17],v[18]);ASMSet_Xlate_Table(table);
 if(v[0]==0)ASMDraw_Sprite((void*)(input+24*4),v[5],v[6],v[7],v[8],0,v[8]*4,v[9]);else ASMDraw_HLine(v[5],v[7],v[6],v[14]);
 return &pixels[0];
}

// Software equivalent of the recovered inclusive indexed-surface clear. Like the assembly, it rejects a
// rectangle that lies outside the area, clamps the rest, and on DirectDraw areas locks the surface for the
// fill and unlocks it afterwards (Unlock even when the caller had it locked, as the original does). Rows are
// written through CurDisplayOffsets, which follow the view's scroll offsets.
void TDrawArea::PtrClear(tagRECT*rect,int color){
 tagRECT r;if(rect)r=*rect;else {r.left=r.top=0;r.right=Width;r.bottom=Height;}
 if(r.right<0||r.left>r.right||r.left>=Width||r.bottom<0||r.top>r.bottom||r.top>=Height)return;
 int left=std::max(0,(int)r.left),top=std::max(0,(int)r.top),right=std::min((int)Width-1,(int)r.right),bottom=std::min((int)Height-1,(int)r.bottom);
 bool ddraw=uses_ddraw(this);
 if(ddraw&&!Lock((char*)"PtrClear",1))return;
 for(int y=top;y<=bottom;y++)memset((unsigned char*)CurDisplayOffsets[y]+left,color,right-left+1);
 if(ddraw)Unlock((char*)"PtrClear");
}
