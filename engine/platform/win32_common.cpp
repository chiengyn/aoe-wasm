// Win32 helpers shared by the probe build and the real-WinMain build (moved from menu_host.cpp).
#include "port_prefix.h"
#include <map>
#include <string>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

static std::map<void*,size_t> global_blocks;

extern "C" {
HGLOBAL GlobalAlloc(UINT flags,size_t size){if(flags&~(GMEM_MOVEABLE|GMEM_ZEROINIT))abort();void*p=flags&GMEM_ZEROINIT?calloc(1,size):malloc(size);if(p)global_blocks[p]=size;return p;}
LPVOID GlobalLock(HGLOBAL h){return global_blocks.count(h)?h:0;}
BOOL GlobalUnlock(HGLOBAL h){return FALSE;}
HGLOBAL GlobalHandle(LPCVOID p){return global_blocks.count((void*)p)?(void*)p:0;}
HGLOBAL GlobalFree(HGLOBAL h){if(!global_blocks.count(h))return h;global_blocks.erase(h);free(h);return 0;}
HGLOBAL GlobalReAlloc(HGLOBAL h,SIZE_T size,UINT flags){if(!global_blocks.count(h)||flags&~(GMEM_MOVEABLE|GMEM_ZEROINIT))return 0;size_t old=global_blocks[h];void*p=realloc(h,size);if(p){global_blocks.erase(h);global_blocks[p]=size;if((flags&GMEM_ZEROINIT)&&size>old)memset((char*)p+old,0,size-old);}return p;}
HFILE OpenFile(LPCSTR file,LPOFSTRUCT info,UINT style){if(style!=OF_READ)abort();std::string path=file;for(unsigned i=0;i<path.size();i++){if(path[i]=='\\')path[i]='/';if(path[i]>='A'&&path[i]<='Z')path[i]+=32;}int fd=open(path.c_str(),O_RDONLY);if(info){memset(info,0,sizeof(*info));info->cBytes=sizeof(*info);strncpy(info->szPathName,path.c_str(),sizeof(info->szPathName)-1);}return fd;}
UINT _lread(HFILE file,LPVOID out,UINT size){return read(file,out,size);}
LONG _hread(HFILE file,LPVOID out,LONG size){return size<0?-1:read(file,out,size);}
LONG _llseek(HFILE file,LONG offset,int origin){return lseek(file,offset,origin);}
HFILE _lclose(HFILE file){return close(file);}
DWORD GetTempPathA(DWORD size,LPSTR out){const char*path="/tmp/";if(size<6)return 6;memcpy(out,path,6);return 5;}
UINT GetTempFileNameA(LPCSTR path,LPCSTR prefix,UINT unique,LPSTR out){if(strlen(path)>200)return 0;if(unique){sprintf(out,"%s%.3s%04x.tmp",path,prefix,unique);return unique;}sprintf(out,"%s%.3sXXXXXX",path,prefix);int fd=mkstemp(out);if(fd<0)return 0;close(fd);return 1;}
// Composition is owned by the browser input, so no Windows HIMC exists.
HIMC ImmGetContext(HWND){return 0;}
BOOL SetRect(LPRECT r,int l,int t,int right,int bottom){if(!r)return FALSE;r->left=l;r->top=t;r->right=right;r->bottom=bottom;return TRUE;}
int MulDiv(int a,int b,int divisor){
    if(!divisor)return -1;
    int64_t product=(int64_t)a*b,denominator=divisor;
    bool negative=(product<0)!=(denominator<0);
    uint64_t magnitude=product<0?-product:product,den=denominator<0?-denominator:denominator;
    int64_t result=(magnitude+den/2)/den;if(negative)result=-result;
    return result>2147483647LL||result<-2147483648LL?-1:(int)result;
}
}
