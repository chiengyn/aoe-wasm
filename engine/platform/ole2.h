#pragma once
#include "windows.h"
struct IUnknown{virtual HRESULT QueryInterface(REFIID,void**)=0;virtual ULONG AddRef()=0;virtual ULONG Release()=0;};
