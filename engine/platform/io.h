#pragma once
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>
#define _INC_IO 1
extern "C" int aoe_open(const char*,int,...);
#define _open aoe_open
#define _close close
#define _read read
#define _write write
#define _lseek lseek
#define _access access
extern "C" long filelength(int);
extern "C" long tell(int);
#define _filelength filelength
#define _tell tell
struct _finddata_t{unsigned attrib;long time_create,time_access,time_write;unsigned long size;char name[260];};
extern "C" intptr_t _findfirst(const char*,struct _finddata_t*);
extern "C" int _findnext(intptr_t,struct _finddata_t*);
extern "C" int _findclose(intptr_t);
