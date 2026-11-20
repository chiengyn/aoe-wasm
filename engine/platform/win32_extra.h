// Win32 declarations used by the real-WinMain platform modules that the recovered headers don't provide.
#pragma once
#include "port_prefix.h"

typedef short SHORT;
typedef struct tagCREATESTRUCTA {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    LPCSTR lpszName, lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTA;

#ifndef MAKELPARAM
#define MAKELPARAM(l, h) ((LPARAM) (DWORD) MAKELONG(l, h))
#endif
#ifndef WM_CREATE
#define WM_CREATE 0x0001
#endif
#ifndef WM_KILLFOCUS
#define WM_KILLFOCUS 0x0008
#endif
#ifndef WM_SHOWWINDOW
#define WM_SHOWWINDOW 0x0018
#endif
#ifndef WM_SETCURSOR
#define WM_SETCURSOR 0x0020
#endif
#ifndef GWL_ID
#define GWL_ID (-12)
#endif
#ifndef MK_LBUTTON
#define MK_LBUTTON 0x0001
#endif
#ifndef MK_RBUTTON
#define MK_RBUTTON 0x0002
#endif
#ifndef VK_MBUTTON
#define VK_MBUTTON 0x04
#endif
#ifndef VK_CAPITAL
#define VK_CAPITAL 0x14
#endif
#ifndef VK_NUMLOCK
#define VK_NUMLOCK 0x90
#endif
#ifndef VK_SCROLL
#define VK_SCROLL 0x91
#endif
#ifndef VK_F10
#define VK_F10 0x79
#endif
#ifndef WHITE_BRUSH
#define WHITE_BRUSH 0
#endif
#ifndef SYSTEM_FONT
#define SYSTEM_FONT 13
#endif

extern "C" {
HGDIOBJ GetStockObject(int);
HGDIOBJ SelectObject(HDC, HGDIOBJ);
BOOL DeleteObject(HGDIOBJ);
HBRUSH CreateSolidBrush(COLORREF);
HPEN CreatePen(int, int, COLORREF);
int FillRect(HDC, const RECT*, HBRUSH);
BOOL TextOutA(HDC, int, int, LPCSTR, int);
BOOL GetTextExtentPoint32A(HDC, LPCSTR, int, LPSIZE);
COLORREF SetTextColor(HDC, COLORREF);
COLORREF SetBkColor(HDC, COLORREF);
int SetBkMode(HDC, int);
BOOL MoveToEx(HDC, int, int, LPPOINT);
BOOL LineTo(HDC, int, int);
HDC GetDC(HWND);
int ReleaseDC(HWND, HDC);
UINT GetCaretBlinkTime(void);
}
