#pragma once
#include <string.h>
extern "C" unsigned char* _mbsinc(const unsigned char*);
extern "C" unsigned char* _mbsdec(const unsigned char*,const unsigned char*);
extern "C" size_t _mbslen(const unsigned char*);
extern "C" unsigned char* _mbsncpy(unsigned char*,const unsigned char*,size_t);

extern "C" {
int _mbscmp(const unsigned char*,const unsigned char*);unsigned char*_mbschr(const unsigned char*,unsigned int);
unsigned char*_mbsstr(const unsigned char*,const unsigned char*);unsigned char*_mbsninc(const unsigned char*,size_t);
unsigned char*_mbscpy(unsigned char*,const unsigned char*);unsigned char*_mbsncat(unsigned char*,const unsigned char*,size_t);
unsigned char*_mbsupr(unsigned char*);int _ismbcspace(unsigned int);
}

extern "C" unsigned char*_mbscat(unsigned char*,const unsigned char*);
