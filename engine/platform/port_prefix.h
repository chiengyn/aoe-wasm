#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <math.h>
#include <fcntl.h>
#include "windows.h"
#define _iobuf _IO_FILE
#define _INC_STDIO 1
#define _INC_STDLIB 1
#define _INC_STRING 1
#define _INC_MATH 1
#define _stricmp strcasecmp
#define stricmp strcasecmp
#define _strnicmp strncasecmp
#define strnicmp strncasecmp
#define _snprintf snprintf
#define _vsnprintf vsnprintf
#define _O_BINARY 0
#define _O_RDONLY O_RDONLY
#define _O_RDWR O_RDWR
#define _O_WRONLY O_WRONLY
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _S_IREAD 0400
#define _S_IWRITE 0200
extern "C" int unlink(const char*);
#define _unlink unlink
#include "io.h"
extern "C" char* itoa(int,char*,int);
#define _itoa itoa
extern "C" char* strlwr(char*);
#define _strlwr strlwr

extern "C" char*_pgmptr;

// Windows asset paths are normalized inside the isolated WASM filesystem.
extern "C" FILE* aoe_fopen(const char*,const char*);
#define fopen aoe_fopen
