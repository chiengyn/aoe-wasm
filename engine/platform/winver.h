#pragma once
#include "windows.h"
struct VS_FIXEDFILEINFO{DWORD dwSignature,dwStrucVersion,dwFileVersionMS,dwFileVersionLS,dwProductVersionMS,dwProductVersionLS,dwFileFlagsMask,dwFileFlags,dwFileOS,dwFileType,dwFileSubtype,dwFileDateMS,dwFileDateLS;};
extern "C" {DWORD GetFileVersionInfoSizeA(LPCSTR,DWORD*);BOOL GetFileVersionInfoA(LPCSTR,DWORD,DWORD,LPVOID);BOOL VerQueryValueA(LPCVOID,LPCSTR,LPVOID*,UINT*);}
