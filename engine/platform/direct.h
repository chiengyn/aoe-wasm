#pragma once
#include <unistd.h>
#include <sys/stat.h>
extern "C" int _mkdir(const char*);
#define _getcwd getcwd
#define _chdir chdir

extern "C" {int _getdrive(void);int _chdrive(int);void _splitpath(const char*,char*,char*,char*,char*);}
