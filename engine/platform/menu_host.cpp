// Browser host for the recovered menu classes. Geometry, palette shading,
// bevel drawing, strings and control states remain in the C++ game.
// Canvas font rasterization is a known fidelity limit, not Windows GDI parity.
#include "port_prefix.h"
#include "menu_host.h"
#include "../upstream/include/Shape.h"
#include "../upstream/include/drawarea.h"
#include "../upstream/include/asm_draw.h"
#include <emscripten.h>
#include <vector>
#include <algorithm>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <map>


enum { FONT=1, REGION=2, PALETTE=3, BRUSH=4, PEN=5 };
struct GdiObject {
    int kind; LOGFONTA font; RECT rect; COLORREF color; int width,style; std::vector<PALETTEENTRY> palette;
    GdiObject(int k):kind(k){memset(&font,0,sizeof(font));memset(&rect,0,sizeof(rect));}
};
struct MenuDC { GdiObject* font; GdiObject* region; COLORREF color; int bk; MenuDC():font(0),region(0),color(0),bk(TRANSPARENT){} };
static MenuDC menu_dc;
static GdiObject black_brush(BRUSH);
static GdiObject hollow_brush(BRUSH),white_pen(PEN),black_pen(PEN);
static HWND captured;
static POINT browser_pointer={-1,-1};
static HCURSOR current_cursor;
static LONG class_cursor;
static bool menu_closed;
// Actual edit-control state. Browser inputs are synchronized by the menu bridge;
// these handles never stand in for a DirectPlay or native application window.
struct EditWindow { std::string text; RECT rect; LONG user,proc; HFONT font; int start,end; bool visible; EditWindow():user(0),proc(0),font(0),start(0),end(0),visible(false){} };
static std::vector<EditWindow*> edits;
static HWND focused;
static EditWindow* edit(HWND h){EditWindow*p=(EditWindow*)h;if(std::find(edits.begin(),edits.end(),p)==edits.end())abort();return p;}
extern "C" EMSCRIPTEN_KEEPALIVE int menu_is_closed(){return menu_closed;}
extern int fog_next_shape;
void* wasm_menu_dc(){return &menu_dc;}

EM_JS(int,menu_measure,(const LOGFONTA* font,const char* text,int length,int field),{
    if(!Module['menuHost'])throw new Error('Menu font host is not installed');
    return Module['menuHost'].measure(font, text, length, field);
});
EM_JS(void,menu_text,(const LOGFONTA* font,const char* text,int length,int x,int y,unsigned color,const RECT* clip),{
    Module['menuHost'].text(font,text,length,x,y,color,clip);
});
EM_JS(void,wasm_menu_begin_frame,(),{Module['menuHost'].begin();});
EM_JS(void,menu_present,(const unsigned char* bits,int width,int height,int pitch,const PALETTEENTRY* pal),{
    Module['menuHost'].present(bits,width,height,pitch,pal);
});
EM_JS(void,menu_invalidate,(),{if(Module['menuHost'])Module['menuHost'].dirty=true;});
EM_JS(int,menu_hidden,(),{return typeof document==='undefined'?0:Number(document.hidden);});
EM_JS(void,menu_cursor,(int id),{if(!Module['menuHost'])throw new Error('No cursor host');Module['menuHost'].cursor(id);});
void wasm_menu_present(TDrawArea* area,void* palette){
    GdiObject* p=(GdiObject*)palette;
    if(!p||p->kind!=PALETTE||p->palette.size()!=256)abort();
    menu_present(area->Bits,area->Width,area->Height,area->Pitch,&p->palette[0]);
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void game_ui_pointer(int x,int y){browser_pointer.x=x;browser_pointer.y=y;}
BOOL GetCursorPos(LPPOINT p){if(!p)return FALSE;*p=browser_pointer;return TRUE;}
BOOL ScreenToClient(HWND,LPPOINT p){return p!=0;}
BOOL ClientToScreen(HWND,LPPOINT p){return p!=0;}
HRGN CreateRectRgn(int l,int t,int r,int b){GdiObject*p=new GdiObject(REGION);p->rect.left=l;p->rect.top=t;p->rect.right=r;p->rect.bottom=b;return p;}
BOOL DeleteObject(HGDIOBJ object){if(!object||object==&black_brush||object==&hollow_brush||object==&white_pen||object==&black_pen)return FALSE;delete (GdiObject*)object;return TRUE;}
HGDIOBJ GetStockObject(int index){switch(index){case BLACK_BRUSH:return &black_brush;case HOLLOW_BRUSH:hollow_brush.style=1;return &hollow_brush;case WHITE_PEN:white_pen.color=0xffffff;white_pen.width=1;return &white_pen;case BLACK_PEN:black_pen.color=0;black_pen.width=1;return &black_pen;}abort();}
HPEN CreatePen(int style,int width,COLORREF color){if(style!=PS_SOLID||width<0)abort();GdiObject*p=new GdiObject(PEN);p->style=style;p->width=width;p->color=color;return p;}
HFONT CreateFontIndirectA(const LOGFONTA* font){GdiObject*p=new GdiObject(FONT);p->font=*font;return p;}
HGDIOBJ SelectObject(HDC dc,HGDIOBJ object){MenuDC*d=(MenuDC*)dc;GdiObject* old=d->font;GdiObject*p=(GdiObject*)object;if(p&&p->kind!=FONT)abort();d->font=p;return old;}
BOOL GetTextMetricsA(HDC dc,TEXTMETRICA* out){MenuDC*d=(MenuDC*)dc;if(!d->font)return FALSE;memset(out,0,sizeof(*out));out->tmHeight=menu_measure(&d->font->font,"Mg",2,1);out->tmAscent=menu_measure(&d->font->font,"Mg",2,2);out->tmDescent=out->tmHeight-out->tmAscent;out->tmAveCharWidth=menu_measure(&d->font->font,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz",52,0)/52;out->tmMaxCharWidth=menu_measure(&d->font->font,"W",1,0);out->tmWeight=d->font->font.lfWeight;return TRUE;}
BOOL GetTextExtentPoint32A(HDC dc,LPCSTR text,int length,LPSIZE out){MenuDC*d=(MenuDC*)dc;if(!d->font){out->cx=out->cy=0;return FALSE;}out->cx=menu_measure(&d->font->font,text,length,0);out->cy=menu_measure(&d->font->font,text,length,1);return TRUE;}
int SelectClipRgn(HDC dc,HRGN region){((MenuDC*)dc)->region=(GdiObject*)region;return region?2:1;}
int SetBkMode(HDC dc,int mode){int old=((MenuDC*)dc)->bk;((MenuDC*)dc)->bk=mode;return old;}
COLORREF SetTextColor(HDC dc,COLORREF color){COLORREF old=((MenuDC*)dc)->color;((MenuDC*)dc)->color=color;return old;}
BOOL TextOutA(HDC dc,int x,int y,LPCSTR text,int length){MenuDC*d=(MenuDC*)dc;if(!d->font)return FALSE;if(d->bk!=TRANSPARENT)abort();menu_text(&d->font->font,text,length,x,y,d->color,d->region?&d->region->rect:0);return TRUE;}
HPALETTE CreatePalette(const LOGPALETTE* palette){GdiObject*p=new GdiObject(PALETTE);p->palette.assign(palette->palPalEntry,palette->palPalEntry+palette->palNumEntries);return p;}
UINT GetNearestPaletteIndex(HPALETTE palette,COLORREF color){GdiObject*p=(GdiObject*)palette;if(!p||p->kind!=PALETTE)return 0xffffffffu;int best=0,distance=0x7fffffff;for(unsigned i=0;i<p->palette.size();i++){int r=(color&255)-p->palette[i].peRed,g=((color>>8)&255)-p->palette[i].peGreen,b=((color>>16)&255)-p->palette[i].peBlue,d=r*r+g*g+b*b;if(d<distance){distance=d;best=i;}}return best;}
UINT GetPaletteEntries(HPALETTE palette,UINT start,UINT count,LPPALETTEENTRY out){GdiObject*p=(GdiObject*)palette;if(!p||p->kind!=PALETTE||start>=p->palette.size())return 0;count=std::min(count,(UINT)p->palette.size()-start);if(out)memcpy(out,&p->palette[start],count*sizeof(*out));return count;}
BOOL UpdateWindow(HWND){menu_invalidate();return TRUE;}
BOOL InvalidateRect(HWND,const RECT*,BOOL){menu_invalidate();return TRUE;}
BOOL IsIconic(HWND){return menu_hidden();}
HCURSOR SetCursor(HCURSOR cursor){menu_cursor((int)cursor);HCURSOR old=current_cursor;current_cursor=cursor;return old;}
DWORD SetClassLongA(HWND,int index,LONG value){if(index!=-12)abort();LONG old=class_cursor;class_cursor=value;return old;}
HWND CreateWindowExA(DWORD,LPCSTR cls,LPCSTR text,DWORD,int x,int y,int w,int h,HWND,HMENU,HINSTANCE,LPVOID){
    if(strcmp(cls,"edit"))abort();EditWindow*p=new EditWindow;p->text=text?text:"";p->rect.left=x;p->rect.top=y;p->rect.right=x+w;p->rect.bottom=y+h;edits.push_back(p);return p;
}
BOOL DestroyWindow(HWND h){EditWindow*p=edit(h);edits.erase(std::find(edits.begin(),edits.end(),p));if(focused==h)focused=0;delete p;return TRUE;}
BOOL ShowWindow(HWND h,int command){EditWindow*p=edit(h);bool was=p->visible;if(command!=SW_SHOW&&command!=SW_HIDE)abort();p->visible=command==SW_SHOW;return was;}
BOOL MoveWindow(HWND h,int x,int y,int w,int height,BOOL){EditWindow*p=edit(h);p->rect.left=x;p->rect.top=y;p->rect.right=x+w;p->rect.bottom=y+height;return TRUE;}
BOOL SetWindowTextA(HWND h,LPCSTR text){EditWindow*p=edit(h);p->text=text?text:"";p->start=p->end=0;return TRUE;}
int GetWindowTextA(HWND h,LPSTR out,int capacity){EditWindow*p=edit(h);if(capacity<=0)return 0;int len=std::min((int)p->text.size(),capacity-1);memcpy(out,p->text.data(),len);out[len]=0;return len;}
LONG GetWindowLongA(HWND h,int index){EditWindow*p=edit(h);if(index==GWL_USERDATA)return p->user;if(index==GWL_WNDPROC)return p->proc;abort();}
LONG SetWindowLongA(HWND h,int index,LONG value){EditWindow*p=edit(h);LONG old=GetWindowLongA(h,index);if(index==GWL_USERDATA)p->user=value;else p->proc=value;return old;}
HWND GetFocus(){return focused;}
HWND SetFocus(HWND h){HWND old=focused;focused=h;return old;}
LRESULT SendMessageA(HWND h,UINT message,WPARAM w,LPARAM l){
    if(message==WM_CLOSE){menu_closed=true;menu_invalidate();return 0;}
    EditWindow*p=edit(h);
    if(message==WM_SETFONT){p->font=(HFONT)w;return 0;}
    if(message==EM_SETSEL){p->start=std::min((int)w,(int)p->text.size());p->end=l==-1?p->text.size():std::min((int)l,(int)p->text.size());if((int)w==-1)p->start=p->end;return 0;}
    if(message==EM_GETSEL){if(w)*(DWORD*)w=p->start;if(l)*(DWORD*)l=p->end;return (p->end<<16)|(p->start&65535);}
    fprintf(stderr,"Unported edit message: %u\n",message);abort();
}
int DrawTextA(HDC dc,LPCSTR text,int length,LPRECT rect,UINT format){
    const UINT supported=DT_SINGLELINE|DT_WORDBREAK|DT_CENTER|DT_RIGHT|DT_VCENTER|DT_BOTTOM|DT_CALCRECT|DT_NOPREFIX;
    if(format&~supported)abort();if(length<0)length=strlen(text);
    std::vector<std::string> lines;std::string line;
    for(int i=0;i<length;i++){
        if(text[i]=='\r')continue;
        if(text[i]=='\n'&&!(format&DT_SINGLELINE)){lines.push_back(line);line.clear();continue;}
        line+=text[i];SIZE size;GetTextExtentPoint32A(dc,line.c_str(),line.size(),&size);
        if((format&DT_WORDBREAK)&&size.cx>rect->right-rect->left&&line.size()>1){
            size_t split=line.find_last_of(' ');if(split==std::string::npos||split==0)split=line.size()-1;
            lines.push_back(line.substr(0,split));line=line.substr(split+(line[split]==' '?1:0));
        }
    }
    lines.push_back(line);SIZE metrics;GetTextExtentPoint32A(dc,"Mg",2,&metrics);int height=metrics.cy*lines.size();
    if(format&DT_CALCRECT){int width=0;for(unsigned i=0;i<lines.size();i++){SIZE z;GetTextExtentPoint32A(dc,lines[i].c_str(),lines[i].size(),&z);width=std::max(width,(int)z.cx);}rect->right=rect->left+width;rect->bottom=rect->top+height;return height;}
    int y=rect->top;if(format&DT_VCENTER)y+=(rect->bottom-rect->top-height)/2;else if(format&DT_BOTTOM)y=rect->bottom-height;
    for(unsigned i=0;i<lines.size();i++){SIZE size;GetTextExtentPoint32A(dc,lines[i].c_str(),lines[i].size(),&size);int x=rect->left;if(format&DT_CENTER)x+=(rect->right-rect->left-size.cx)/2;else if(format&DT_RIGHT)x=rect->right-size.cx;TextOutA(dc,x,y,lines[i].c_str(),lines[i].size());y+=metrics.cy;}return height;
}
int DrawTextExA(HDC dc,LPSTR text,int length,LPRECT rect,UINT format,LPDRAWTEXTPARAMS params){
    // Native menu edits use DT_EDITCONTROL with SINGLELINE or WORDBREAK.
    // Multiline/wrapping is deliberately rejected until it has a real host.
    if(params||(format&~(0x2000u|0x20u|0x10u))||!(format&0x20))abort();
    if(length<0)length=strlen(text);SIZE size;GetTextExtentPoint32A(dc,text,length,&size);
    return TextOutA(dc,rect->left,rect->top,text,length)?size.cy:0;
}
HWND GetCapture(){return captured;}
HWND SetCapture(HWND window){HWND old=captured;captured=window;return old;}
BOOL ReleaseCapture(){captured=0;return TRUE;}
}

