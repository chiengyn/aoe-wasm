// Shared portable CRT behavior with evidence from the supplied retail executable.
#include "port_prefix.h"
#undef fopen
#include <unistd.h>
#include <string>
#include <sys/stat.h>
#include <emscripten.h>
static uint32_t random_state=1;
extern "C" {
int rand(void){random_state=random_state*214013u+2531011u;return (random_state>>16)&0x7fff;}
void srand(unsigned int seed){random_state=seed;}
long tell(int fd){return (long)lseek(fd,0,SEEK_CUR);}
long filelength(int fd){struct stat st;return fstat(fd,&st)?-1:(long)st.st_size;}
DWORD timeGetTime(void){return (uint32_t)emscripten_get_now();}
DWORD GetTickCount(void){return timeGetTime();}
DWORD GetCurrentDirectoryA(DWORD size,LPSTR out){return getcwd(out,size)?strlen(out):0;}
char*itoa(int n,char*out,int radix){if(radix<2||radix>36)return 0;char tmp[34];unsigned int v=(radix==10&&n<0)?0u-(unsigned)n:(unsigned)n;int i=0;do{tmp[i++]="0123456789abcdefghijklmnopqrstuvwxyz"[v%radix];v/=radix;}while(v);char*p=out;if(radix==10&&n<0)*p++='-';while(i)*p++=tmp[--i];*p=0;return out;}
char*strlwr(char*s){for(char*p=s;*p;p++)if(*p>='A'&&*p<='Z')*p+=32;return s;}
}
// The supplied English resource tables use single-byte Windows-1252.
static unsigned char upper1252(unsigned char c){if((c>='a'&&c<='z')||(c>=0xe0&&c<=0xf6)||(c>=0xf8&&c<=0xfe))return c-32;switch(c){case 0x9a:return 0x8a;case 0x9c:return 0x8c;case 0x9e:return 0x8e;case 0xff:return 0x9f;}return c;}
extern "C" {
LPSTR CharUpperA(LPSTR s){if((uintptr_t)s<=0xffff)return (LPSTR)(uintptr_t)upper1252((uintptr_t)s);for(unsigned char*p=(unsigned char*)s;*p;p++)*p=upper1252(*p);return s;}
unsigned char*_mbsupr(unsigned char*s){CharUpperA((char*)s);return s;}
unsigned char*_mbsinc(const unsigned char*s){return (unsigned char*)s+1;}
unsigned char*_mbsdec(const unsigned char*start,const unsigned char*current){return current>start?(unsigned char*)current-1:0;}
unsigned char*_mbsninc(const unsigned char*s,size_t count){while(count--&&*s)s++;return (unsigned char*)s;}
size_t _mbslen(const unsigned char*s){return strlen((const char*)s);}
int _mbscmp(const unsigned char*a,const unsigned char*b){return strcmp((const char*)a,(const char*)b);}
unsigned char*_mbschr(const unsigned char*s,unsigned int c){return (unsigned char*)strchr((const char*)s,c);}
unsigned char*_mbsstr(const unsigned char*a,const unsigned char*b){return (unsigned char*)strstr((const char*)a,(const char*)b);}
unsigned char*_mbscpy(unsigned char*a,const unsigned char*b){return (unsigned char*)strcpy((char*)a,(const char*)b);}
unsigned char*_mbscat(unsigned char*a,const unsigned char*b){return (unsigned char*)strcat((char*)a,(const char*)b);}
unsigned char*_mbsncat(unsigned char*a,const unsigned char*b,size_t n){return (unsigned char*)strncat((char*)a,(const char*)b,n);}
unsigned char*_mbsncpy(unsigned char*a,const unsigned char*b,size_t n){return (unsigned char*)strncpy((char*)a,(const char*)b,n);}
int _ismbcspace(unsigned int c){return c==' '||(c>=9&&c<=13);}
BOOL IsDBCSLeadByte(BYTE){return FALSE;}
}

extern "C" FILE* aoe_fopen(const char*path,const char*mode){
    if(!path||!mode)return 0;
    std::string normalized;
    for(const unsigned char*p=(const unsigned char*)path;*p;p++){
        unsigned char c=*p;if(c=='\\')c='/';if(c>='A'&&c<='Z')c+=32;normalized+=(char)c;
    }
    if(normalized.size()>1&&normalized[1]==':'){normalized.erase(1,1);normalized='/'+normalized;}
    return fopen(normalized.c_str(),mode);
}
#include <dirent.h>
#include <fnmatch.h>
#include <errno.h>
#include <stdarg.h>
struct FindState {DIR*directory;std::string folder,pattern;};
static std::string normalized_path(const char*path){std::string p=path;for(size_t i=0;i<p.size();i++){if(p[i]=='\\')p[i]='/';if(p[i]>='A'&&p[i]<='Z')p[i]+=32;}if(p.size()>1&&p[1]==':'){p.erase(1,1);p='/'+p;}return p;}
extern "C" {
int aoe_open(const char* path,int flags,...){
 int mode=0;if(flags&O_CREAT){va_list args;va_start(args,flags);mode=va_arg(args,int);va_end(args);}
 int fd=open(path,flags,mode);if(fd>=0)return fd;
 std::string normalized=normalized_path(path);return normalized==path?-1:open(normalized.c_str(),flags,mode);
}
int _findnext(intptr_t handle,_finddata_t*out){
    if(handle==-1||!handle||!out){errno=EBADF;return -1;}FindState*s=(FindState*)handle;
    dirent*entry;while((entry=readdir(s->directory))){if(fnmatch(s->pattern.c_str(),entry->d_name,0))continue;
        std::string name=s->folder+"/"+entry->d_name;struct stat st;if(stat(name.c_str(),&st))continue;
        memset(out,0,sizeof(*out));out->attrib=S_ISDIR(st.st_mode)?0x10:0x20;out->size=st.st_size;out->time_create=st.st_ctime;out->time_access=st.st_atime;out->time_write=st.st_mtime;strncpy(out->name,entry->d_name,259);return 0;
    }errno=ENOENT;return -1;
}
intptr_t _findfirst(const char*pattern,_finddata_t*out){
    std::string p=normalized_path(pattern);size_t slash=p.find_last_of('/');FindState*s=new FindState;s->folder=slash==std::string::npos?".":p.substr(0,slash);s->pattern=slash==std::string::npos?p:p.substr(slash+1);s->directory=opendir(s->folder.c_str());
    if(!s->directory){delete s;return -1;}intptr_t handle=(intptr_t)s;if(_findnext(handle,out)){closedir(s->directory);delete s;return -1;}return handle;
}
int _findclose(intptr_t handle){if(handle==-1||!handle){errno=EBADF;return -1;}FindState*s=(FindState*)handle;int result=closedir(s->directory);delete s;return result;}
int _mkdir(const char*path){return mkdir(normalized_path(path).c_str(),0777);}
}
