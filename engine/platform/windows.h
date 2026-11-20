#pragma once
// Portable Win32 ABI declarations. Implementations are supplied by this port.
#include <stddef.h>
#include <stdint.h>
#define __cdecl
#define __stdcall
#define __fastcall
#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINAPIV
#define FAR
#define NEAR
#define PASCAL
#define CONST const
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define interface struct
#define PURE =0
#define THIS void
#define THIS_
#define STDMETHOD(m) virtual HRESULT m
#define STDMETHOD_(t,m) virtual t m
#define DECLARE_INTERFACE_(n,b) struct n: public b
#define DECLARE_INTERFACE(n) struct n
#define STDAPI HRESULT
#define STDAPI_(t) t
#define STDMETHODCALLTYPE
#define STDMETHODVCALLTYPE
#define STDAPICALLTYPE
#define DECLSPEC_IMPORT
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define MAKE_HRESULT(a,b,c) ((HRESULT)(((uint32_t)(a)<<31)|((b)<<16)|(c)))
#define DEFINE_GUID(n,a,b,c,d,e,f,g,h,i,j,k) extern "C" const GUID n
#define S_OK 0
#define S_FALSE 1
#define E_FAIL ((HRESULT)0x80004005)
#define SUCCEEDED(x) ((HRESULT)(x)>=0)
#define FAILED(x) ((HRESULT)(x)<0)
typedef unsigned char BYTE,*LPBYTE;typedef unsigned short WORD,*LPWORD;
typedef unsigned long DWORD,*LPDWORD,ULONG,*PULONG;typedef long LONG,*LPLONG,HRESULT;
typedef int BOOL,*LPBOOL;typedef unsigned int UINT,*LPUINT;typedef int INT;typedef unsigned short USHORT;
typedef void VOID,*LPVOID,*PVOID;typedef const void *LPCVOID;
typedef char CHAR,*LPSTR,*PSTR;typedef const char*LPCSTR;typedef unsigned short WCHAR,*LPWSTR;typedef const WCHAR*LPCWSTR;
typedef void* HANDLE;typedef HANDLE HWND,HDC,HFONT,HPALETTE,HBITMAP,HBRUSH,HPEN,HGDIOBJ,HINSTANCE,HMODULE,HCURSOR,HICON,HMENU,HGLOBAL,HLOCAL,HKEY,HRSRC;
typedef unsigned long COLORREF;typedef unsigned int WPARAM;typedef long LPARAM,LRESULT;typedef unsigned long UINT_PTR,ULONG_PTR,DWORD_PTR;
struct tagRECT {LONG left,top,right,bottom;};typedef tagRECT RECT,*LPRECT;typedef const RECT*LPCRECT;
struct tagPOINT{LONG x,y;};typedef tagPOINT POINT,*LPPOINT;
struct tagSIZE{LONG cx,cy;};typedef tagSIZE SIZE,*LPSIZE;
struct _GUID{DWORD Data1;WORD Data2,Data3;BYTE Data4[8];};typedef _GUID GUID,IID,CLSID;typedef GUID*LPGUID;typedef const GUID& REFGUID;typedef const IID& REFIID;typedef const CLSID& REFCLSID;
struct tagPALETTEENTRY{BYTE peRed,peGreen,peBlue,peFlags;};typedef tagPALETTEENTRY PALETTEENTRY,*LPPALETTEENTRY;
struct tagLOGPALETTE{WORD palVersion,palNumEntries;PALETTEENTRY palPalEntry[1];};typedef tagLOGPALETTE LOGPALETTE,*LPLOGPALETTE;
struct tagMSG{HWND hwnd;UINT message;WPARAM wParam;LPARAM lParam;DWORD time;POINT pt;};typedef tagMSG MSG,*LPMSG;
typedef LRESULT (*WNDPROC)(HWND,UINT,WPARAM,LPARAM);
struct tagWNDCLASSA{UINT style;WNDPROC lpfnWndProc;int cbClsExtra,cbWndExtra;HINSTANCE hInstance;HICON hIcon;HCURSOR hCursor;HBRUSH hbrBackground;LPCSTR lpszMenuName,lpszClassName;};typedef tagWNDCLASSA WNDCLASS;
struct _CRITICAL_SECTION{void*DebugInfo;LONG LockCount,RecursionCount;HANDLE OwningThread,LockSemaphore;ULONG_PTR SpinCount;};typedef _CRITICAL_SECTION CRITICAL_SECTION;
struct _SYSTEMTIME{WORD wYear,wMonth,wDayOfWeek,wDay,wHour,wMinute,wSecond,wMilliseconds;};typedef _SYSTEMTIME SYSTEMTIME;
struct _FILETIME{DWORD dwLowDateTime,dwHighDateTime;};typedef _FILETIME FILETIME;
struct tagRGBQUAD{BYTE rgbBlue,rgbGreen,rgbRed,rgbReserved;};typedef tagRGBQUAD RGBQUAD;
struct tagBITMAPINFOHEADER{DWORD biSize;LONG biWidth,biHeight;WORD biPlanes,biBitCount;DWORD biCompression,biSizeImage;LONG biXPelsPerMeter,biYPelsPerMeter;DWORD biClrUsed,biClrImportant;};typedef tagBITMAPINFOHEADER BITMAPINFOHEADER;
struct tagBITMAPINFO{BITMAPINFOHEADER bmiHeader;RGBQUAD bmiColors[1];};typedef tagBITMAPINFO BITMAPINFO;
#define RGB(r,g,b) ((COLORREF)((BYTE)(r)|((WORD)(BYTE)(g)<<8)|((DWORD)(BYTE)(b)<<16)))
#define LOWORD(x) ((WORD)(x))
#define HIWORD(x) ((WORD)((DWORD)(x)>>16))
#define LOBYTE(x) ((BYTE)(x))
#define HIBYTE(x) ((BYTE)((WORD)(x)>>8))
#define MAKELONG(a,b) ((LONG)((WORD)(a)|((DWORD)(WORD)(b)<<16)))
#define MB_OK 0
#define MB_ICONEXCLAMATION 0x30
#define MB_YESNO 4
#define IDYES 6
#define WM_USER 0x400
extern "C" {
DWORD GetTickCount(void);void Sleep(DWORD);int MessageBoxA(HWND,LPCSTR,LPCSTR,UINT);
BOOL SetRect(LPRECT,int,int,int,int);BOOL GetCursorPos(LPPOINT);BOOL InvalidateRect(HWND,LPCRECT,BOOL);
int LoadStringA(HINSTANCE,UINT,LPSTR,int);BOOL PostMessageA(HWND,UINT,WPARAM,LPARAM);
}
#define MessageBox MessageBoxA
#define LoadString LoadStringA
#define PostMessage PostMessageA
typedef HANDLE* LPHANDLE;
#define ZeroMemory(p,n) memset(p,0,n)
#define CopyMemory(d,s,n) memcpy(d,s,n)
#define FillMemory(d,n,v) memset(d,v,n)
typedef HANDLE HRGN;typedef BITMAPINFOHEADER*LPBITMAPINFOHEADER;typedef int HFILE;
#define BLACK_PEN 7
#define WHITE_PEN 6
#define NULL_BRUSH 5
#define TRANSPARENT 1
#define OPAQUE 2
#define DT_TOP 0
#define DT_LEFT 0
#define DT_CENTER 1
#define DT_RIGHT 2
#define DT_VCENTER 4
#define DT_BOTTOM 8
#define DT_WORDBREAK 0x10
#define DT_SINGLELINE 0x20
#define DT_EXPANDTABS 0x40
#define DT_CALCRECT 0x400
#define DT_NOPREFIX 0x800
#define VK_BACK 8
#define VK_TAB 9
#define VK_RETURN 13
#define VK_SHIFT 16
#define VK_CONTROL 17
#define VK_MENU 18
#define VK_ESCAPE 27
#define VK_SPACE 32
#define VK_PRIOR 33
#define VK_NEXT 34
#define VK_END 35
#define VK_HOME 36
#define VK_LEFT 37
#define VK_UP 38
#define VK_RIGHT 39
#define VK_DOWN 40
#define VK_INSERT 45
#define VK_DELETE 46
extern "C" {
int SelectClipRgn(HDC,HRGN);HGDIOBJ GetStockObject(int);COLORREF SetTextColor(HDC,COLORREF);COLORREF SetBkColor(HDC,COLORREF);int SetBkMode(HDC,int);
BOOL MoveToEx(HDC,int,int,POINT*);BOOL LineTo(HDC,int,int);BOOL Rectangle(HDC,int,int,int,int);BOOL Ellipse(HDC,int,int,int,int);
BOOL TextOutA(HDC,int,int,LPCSTR,int);int DrawTextA(HDC,LPCSTR,int,LPRECT,UINT);BOOL GetTextExtentPoint32A(HDC,LPCSTR,int,SIZE*);
BOOL ClientToScreen(HWND,POINT*);BOOL ScreenToClient(HWND,POINT*);BOOL SetCursorPos(int,int);HCURSOR SetCursor(HCURSOR);
BOOL UpdateWindow(HWND);BOOL MessageBeep(UINT);short GetKeyState(int);short GetAsyncKeyState(int);HWND SetCapture(HWND);BOOL ReleaseCapture(void);
}
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_OUTOFMEMORY ((HRESULT)0x8007000e)
#define E_POINTER ((HRESULT)0x80004003)
#define E_ACCESSDENIED ((HRESULT)0x80070005)
extern const GUID GUID_NULL;
inline BOOL IsEqualGUID(REFGUID a,REFGUID b){return memcmp(&a,&b,16)==0;}
extern "C" DWORD GetTempPathA(DWORD,LPSTR);
typedef BOOL WINBOOL;typedef char*PCHAR;typedef DWORD*PDWORD;typedef ULONG*PULONG;typedef void(*FARPROC)();typedef DWORD LCID,LCTYPE;typedef unsigned long HIMC;typedef BYTE*PBYTE;typedef const char*LPCCH;typedef WORD ATOM;
struct tagDRAWTEXTPARAMS{UINT cbSize;int iTabLength,iLeftMargin,iRightMargin;UINT uiLengthDrawn;};typedef tagDRAWTEXTPARAMS DRAWTEXTPARAMS,*LPDRAWTEXTPARAMS;
struct tagCOPYDATASTRUCT{ULONG_PTR dwData;DWORD cbData;PVOID lpData;};typedef tagCOPYDATASTRUCT COPYDATASTRUCT;
struct _SECURITY_ATTRIBUTES{DWORD nLength;LPVOID lpSecurityDescriptor;BOOL bInheritHandle;};typedef _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES,*LPSECURITY_ATTRIBUTES;
struct _OFSTRUCT{BYTE cBytes,fFixedDisk;WORD nErrCode,Reserved1,Reserved2;CHAR szPathName[128];};typedef _OFSTRUCT OFSTRUCT,*LPOFSTRUCT;
#pragma pack(push,2)
struct tagBITMAPFILEHEADER{WORD bfType;DWORD bfSize;WORD bfReserved1,bfReserved2;DWORD bfOffBits;};typedef tagBITMAPFILEHEADER BITMAPFILEHEADER;
#pragma pack(pop)
#include "win_constants.h"
typedef UINT MMRESULT,MCIDEVICEID;typedef DWORD MCIERROR;typedef HANDLE HMIXER;typedef HMIXER*LPHMIXER;
typedef const GUID*LPCGUID;struct IUnknown;typedef IUnknown*LPUNKNOWN;
struct IDirectSound;typedef IDirectSound*LPDIRECTSOUND;struct IDirectDraw;typedef IDirectDraw*LPDIRECTDRAW;
struct _devicemodeA;typedef _devicemodeA DEVMODEA;typedef tagWNDCLASSA WNDCLASSA;
typedef void(*TIMERPROC)(HWND,UINT,UINT_PTR,DWORD);
typedef size_t SIZE_T;typedef HANDLE* PHKEY;typedef WORD*PWORD;typedef BITMAPINFO*LPBITMAPINFO;typedef const LOGPALETTE*LPCLOGPALETTE;
typedef DWORD REGSAM;
#include "win_functions.h"
#undef MAKEINTRESOURCE
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define GENERIC_READ 0x80000000UL
#define FILE_SHARE_READ 1
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 0x80
#define PAGE_READONLY 2
#define FILE_MAP_READ 4
#define MEM_DECOMMIT 0x4000
#define LOCALE_USER_DEFAULT 0x0400
#define NORM_IGNORECASE 1
#define KEY_ALL_ACCESS 0xf003f
#define KEY_QUERY_VALUE 1
#define REG_SZ 1
#define REG_DWORD 4
struct _MEMORYSTATUS{DWORD dwLength,dwMemoryLoad,dwTotalPhys,dwAvailPhys,dwTotalPageFile,dwAvailPageFile,dwTotalVirtual,dwAvailVirtual;};typedef _MEMORYSTATUS MEMORYSTATUS;
struct tagLOGFONTA{LONG lfHeight,lfWidth,lfEscapement,lfOrientation,lfWeight;BYTE lfItalic,lfUnderline,lfStrikeOut,lfCharSet,lfOutPrecision,lfClipPrecision,lfQuality,lfPitchAndFamily;CHAR lfFaceName[32];};typedef tagLOGFONTA LOGFONTA;
struct tagTEXTMETRICA{LONG tmHeight,tmAscent,tmDescent,tmInternalLeading,tmExternalLeading,tmAveCharWidth,tmMaxCharWidth,tmWeight,tmOverhang,tmDigitizedAspectX,tmDigitizedAspectY;BYTE tmFirstChar,tmLastChar,tmDefaultChar,tmBreakChar,tmItalic,tmUnderlined,tmStruckOut,tmPitchAndFamily,tmCharSet;};typedef tagTEXTMETRICA TEXTMETRICA;
struct _WIN32_FIND_DATAA{DWORD dwFileAttributes;FILETIME ftCreationTime,ftLastAccessTime,ftLastWriteTime;DWORD nFileSizeHigh,nFileSizeLow,dwReserved0,dwReserved1;CHAR cFileName[260],cAlternateFileName[14];};typedef _WIN32_FIND_DATAA WIN32_FIND_DATAA;
extern "C" {
COLORREF GetSysColor(int);DWORD GetCurrentDirectoryA(DWORD,LPSTR);DWORD GetLastError(void);void GlobalMemoryStatus(MEMORYSTATUS*);
BOOL SystemParametersInfoA(UINT,UINT,PVOID,UINT);HFILE OpenFile(LPCSTR,LPOFSTRUCT,UINT);BOOL CloseHandle(HANDLE);BOOL FreeLibrary(HMODULE);
BOOL PeekMessageA(MSG*,HWND,UINT,UINT,UINT);int GetSystemMetrics(int);int GetDeviceCaps(HDC,int);BOOL GetTextMetricsA(HDC,TEXTMETRICA*);HFONT CreateFontIndirectA(const LOGFONTA*);
UINT GetDriveTypeA(LPCSTR);HANDLE CreateMutexA(SECURITY_ATTRIBUTES*,BOOL,LPCSTR);BOOL ReleaseMutex(HANDLE);BOOL IsDBCSLeadByte(BYTE);void OutputDebugStringA(LPCSTR);
UINT GetTempFileNameA(LPCSTR,LPCSTR,UINT,LPSTR);BOOL FindClose(HANDLE);HANDLE FindFirstFileA(LPCSTR,WIN32_FIND_DATAA*);BOOL FindNextFileA(HANDLE,WIN32_FIND_DATAA*);BOOL FileTimeToSystemTime(const FILETIME*,SYSTEMTIME*);
BOOL UnmapViewOfFile(LPCVOID);BOOL VirtualFree(LPVOID,size_t,DWORD);
}

#define _HRESULT_TYPEDEF_(x) ((HRESULT)(x))
#define DECLSPEC_SELECTANY
#define REFIID const IID&
#define LPSECURITY_ATTRIBUTES SECURITY_ATTRIBUTES*
typedef unsigned long ULONG32;
typedef struct _LARGE_INTEGER{long long QuadPart;} LARGE_INTEGER;
typedef struct _ULARGE_INTEGER{unsigned long long QuadPart;} ULARGE_INTEGER;
typedef struct _RGNDATA RGNDATA,*LPRGNDATA;
extern "C" {
HCURSOR LoadCursorA(HINSTANCE,LPCSTR);BOOL BringWindowToTop(HWND);LONG GetWindowLongA(HWND,int);LONG SetWindowLongA(HWND,int,LONG);DWORD SetClassLongA(HWND,int,LONG);
LRESULT CallWindowProcA(FARPROC,HWND,UINT,WPARAM,LPARAM);
HANDLE CreateFileA(LPCSTR,DWORD,DWORD,SECURITY_ATTRIBUTES*,DWORD,DWORD,HANDLE);
HANDLE CreateFileMappingA(HANDLE,SECURITY_ATTRIBUTES*,DWORD,DWORD,DWORD,LPCSTR);
LPVOID MapViewOfFile(HANDLE,DWORD,DWORD,DWORD,size_t);
}

#define HFILE_ERROR (-1)
#define LMEM_ZEROINIT 0x40
#define LPTR 0x40
#define LMEM_FIXED 0
#define GMEM_FIXED 0
#define CONST_VTBL const
#define DECLSPEC_ALIGN(x) __attribute__((aligned(x)))
typedef void *HMONITOR;
typedef struct tagBITMAPCOREHEADER{DWORD bcSize;WORD bcWidth,bcHeight,bcPlanes,bcBitCount;} BITMAPCOREHEADER;
#pragma pack(push,1)
typedef struct tagRGBTRIPLE{BYTE rgbtBlue,rgbtGreen,rgbtRed;} RGBTRIPLE;
#pragma pack(pop)
extern "C" {HMODULE GetModuleHandleA(LPCSTR);LPVOID LockResource(HGLOBAL);}

#define DECLARE_HANDLE(n) typedef HANDLE n

typedef float FLOAT;

#define GetObject GetObjectA
#define __MINGW_NAME_AW(n) n##A
#define VER_PLATFORM_WIN32_NT 2
#define VER_PLATFORM_WIN32_WINDOWS 1
#define CLSCTX_INPROC_SERVER 1
typedef struct tagBITMAP{LONG bmType,bmWidth,bmHeight,bmWidthBytes;WORD bmPlanes,bmBitsPixel;LPVOID bmBits;} BITMAP;
typedef struct _OSVERSIONINFOA{DWORD dwOSVersionInfoSize,dwMajorVersion,dwMinorVersion,dwBuildNumber,dwPlatformId;CHAR szCSDVersion[128];} OSVERSIONINFOA;
extern "C" {
HRSRC FindResourceA(HMODULE,LPCSTR,LPCSTR);HGLOBAL LoadResource(HMODULE,HRSRC);HLOCAL LocalAlloc(UINT,size_t);HGLOBAL GlobalAlloc(UINT,size_t);
int SetStretchBltMode(HDC,int);int GetDIBits(HDC,HBITMAP,UINT,UINT,LPVOID,LPBITMAPINFO,UINT);
BOOL GetVersionExA(OSVERSIONINFOA*);HRESULT CoInitialize(LPVOID);void CoUninitialize(void);HRESULT CoCreateInstance(REFCLSID,LPUNKNOWN,DWORD,REFIID,LPVOID*);
}

extern "C" FARPROC GetProcAddress(HMODULE,LPCSTR);
