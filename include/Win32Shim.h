#pragma once
// Win32 spoken by Qt - the names MinerGUI.cpp calls, on Linux.
//
// MinerGUI.cpp is 14,000 lines and every one of its windows is a Win32
// window: registered classes, a WNDPROC each, WM_ messages, owner-drawn
// BUTTONs, one EDIT, one rich edit. It draws every pixel itself through the
// ten GDI+ calls GdiplusShim.h answers to, so nothing of the LOOK depends on
// Windows; what depends on it is the plumbing - how a click becomes
// WM_LBUTTONDOWN, how a window gets its size, how a timer ticks. This header
// and src/linux/Win32Shim*.cpp are that plumbing, on Qt 6.
//
// The rule (docs/linux-port.md): the Linux build answers to the names the
// code calls. Nothing in MinerGUI.cpp changes for it. On Windows this file
// is never seen - <windows.h> is the real one there; on Linux the folder
// include/win32/ holds a Windows.h, gdiplus.h, dwmapi.h ... that each come
// here, and the build puts that folder first on the include path.
//
// What this is not: Windows. It covers what Bedrock's windows do and says
// so where it cuts a corner. A function that is here is one MinerGUI.cpp
// calls (about 120 functions, 230 constants and 35 types), and its
// behaviour is what MinerGUI relies on, not what the documentation lists.

#ifdef _WIN32
#error "Win32Shim.h is for the Linux build - Windows has windows.h"
#endif

#include <cstddef>
#include <cstdint>
#include <cwchar>

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef int BOOL;
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef int INT;
typedef long LONG;           // MSVC's spelling: what std::max(long, LONG) in MinerGUI wants
typedef unsigned long ULONG;
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef intptr_t LONG_PTR;
typedef uintptr_t ULONG_PTR;
typedef uintptr_t UINT_PTR;
typedef intptr_t INT_PTR;
typedef uintptr_t DWORD_PTR;
typedef UINT_PTR WPARAM;
typedef LONG_PTR LPARAM;
typedef LONG_PTR LRESULT;
typedef int32_t HRESULT;
typedef wchar_t WCHAR;
typedef char CHAR;
typedef const wchar_t* LPCWSTR;
typedef wchar_t* LPWSTR;
typedef wchar_t* PWSTR;
typedef const char* LPCSTR;
typedef char* LPSTR;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef BOOL* LPBOOL;
typedef BYTE* LPBYTE;
typedef DWORD COLORREF;
typedef DWORD LCID;
typedef void* HANDLE;
typedef void* HGLOBAL;
typedef void* HINSTANCE;
typedef void* HMODULE;
typedef void* HMENU;
typedef void* HMONITOR;
typedef void* HDROP;
typedef void* HRGN;

// The handles a window and its drawing surface are known by. Opaque here;
// the shim's own sources know what lies behind them.
typedef struct HWND__* HWND;
typedef struct HDC__* HDC;

// GDI objects: one base, so DeleteObject and SelectObject take any of them
// and a (HBITMAP) cast on what SelectObject hands back is a plain downcast.
struct HGDIOBJ__ {
    enum Kind { Bitmap, Brush, Font, Pen, Icon, Cursor, Stock };
    explicit HGDIOBJ__(Kind k) : kind(k) {}
    virtual ~HGDIOBJ__() = default;
    Kind kind;
};
typedef HGDIOBJ__* HGDIOBJ;
// The kinds, each a class of its own so a (HBITMAP) cast on what
// SelectObject hands back is a plain downcast; what they carry (a QImage,
// a QFont) is the shim's business, in classes derived from these.
struct HBITMAP__ : HGDIOBJ__ { HBITMAP__() : HGDIOBJ__(Bitmap) {} };
struct HBRUSH__ : HGDIOBJ__ { HBRUSH__() : HGDIOBJ__(Brush) {} };
struct HFONT__ : HGDIOBJ__ { HFONT__() : HGDIOBJ__(Font) {} };
struct HPEN__ : HGDIOBJ__ { HPEN__() : HGDIOBJ__(Pen) {} };
struct HICON__ : HGDIOBJ__ { HICON__() : HGDIOBJ__(Icon) {} };
typedef HBITMAP__* HBITMAP;
typedef HBRUSH__* HBRUSH;
typedef HFONT__* HFONT;
typedef HPEN__* HPEN;
typedef HICON__* HICON;
typedef HICON__* HCURSOR;

struct GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
};
typedef GUID CLSID;
typedef GUID IID;
typedef const GUID& REFIID;
typedef const GUID& REFKNOWNFOLDERID;
typedef GUID KNOWNFOLDERID;

struct RECT { LONG left, top, right, bottom; };
typedef RECT* LPRECT;
typedef const RECT* LPCRECT;
struct POINT { LONG x, y; };
typedef POINT* LPPOINT;
struct POINTL { LONG x, y; };
struct SIZE { LONG cx, cy; };

struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
    // The Qt event the message was made from, so a control can be handed
    // the original when the message reaches it (see DispatchMessage). Not
    // a Windows field; nothing outside the shim reads it.
    void* qt_;
};
typedef MSG* LPMSG;

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (*MONITORENUMPROC)(HMONITOR, HDC, LPRECT, LPARAM);
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);

struct WNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
};
// Application.cpp registers its overlay through the A spelling
struct WNDCLASSEXA {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
    HICON hIconSm;
};

struct PAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
};

struct MINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
};

struct MONITORINFO {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
};
typedef MONITORINFO* LPMONITORINFO;
struct MONITORINFOEXA : MONITORINFO { CHAR szDevice[32]; };
struct MONITORINFOEXW : MONITORINFO { WCHAR szDevice[32]; };

struct WINDOWPLACEMENT {
    UINT length;
    UINT flags;
    UINT showCmd;
    POINT ptMinPosition;
    POINT ptMaxPosition;
    RECT rcNormalPosition;
};

struct TRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};

struct DRAWITEMSTRUCT {
    UINT CtlType;
    UINT CtlID;
    UINT itemID;
    UINT itemAction;
    UINT itemState;
    HWND hwndItem;
    HDC hDC;
    RECT rcItem;
    ULONG_PTR itemData;
};

struct NMHDR {
    HWND hwndFrom;
    UINT_PTR idFrom;
    UINT code;
};

struct CHARRANGE { LONG cpMin, cpMax; };

struct TEXTRANGEW {
    CHARRANGE chrg;
    LPWSTR lpstrText;
};

struct ENLINK {
    NMHDR nmhdr;
    UINT msg;
    WPARAM wParam;
    LPARAM lParam;
    CHARRANGE chrg;
};

struct CHARFORMAT2W {
    UINT cbSize;
    DWORD dwMask;
    DWORD dwEffects;
    LONG yHeight;
    LONG yOffset;
    COLORREF crTextColor;
    BYTE bCharSet;
    BYTE bPitchAndFamily;
    WCHAR szFaceName[32];
    WORD wWeight;
    SHORT sSpacing;
    COLORREF crBackColor;
    LCID lcid;
    DWORD dwReserved;
    SHORT sStyle;
    WORD wKerning;
    BYTE bUnderlineType;
    BYTE bAnimation;
    BYTE bRevAuthor;
    BYTE bUnderlineColor;
};

struct GETTEXTEX {
    DWORD cb;
    DWORD flags;
    UINT codepage;
    LPCSTR lpDefaultChar;
    LPBOOL lpUsedDefChar;
};

struct GETTEXTLENGTHEX {
    DWORD flags;
    UINT codepage;
};

struct OPENFILENAMEW {
    DWORD lStructSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCWSTR lpstrFilter;
    LPWSTR lpstrCustomFilter;
    DWORD nMaxCustFilter;
    DWORD nFilterIndex;
    LPWSTR lpstrFile;
    DWORD nMaxFile;
    LPWSTR lpstrFileTitle;
    DWORD nMaxFileTitle;
    LPCWSTR lpstrInitialDir;
    LPCWSTR lpstrTitle;
    DWORD Flags;
    WORD nFileOffset;
    WORD nFileExtension;
    LPCWSTR lpstrDefExt;
    LPARAM lCustData;
    void* lpfnHook;
    LPCWSTR lpTemplateName;
};

#pragma pack(push, 2)
struct BITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth;
    LONG biHeight;
    WORD biPlanes;
    WORD biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG biXPelsPerMeter;
    LONG biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
};
#pragma pack(pop)
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
};
typedef BITMAPINFO* LPBITMAPINFO;

struct SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
};

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define TRUE 1
#define FALSE 0
#define CALLBACK
#define WINAPI
#define APIENTRY
#define MAX_PATH 260
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x08

#define LOWORD(l) ((WORD)(((DWORD_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xffff))
#define LOBYTE(w) ((BYTE)(((DWORD_PTR)(w)) & 0xff))
#define HIBYTE(w) ((BYTE)((((DWORD_PTR)(w)) >> 8) & 0xff))
#define MAKELONG(a, b) ((LONG)(((WORD)(((DWORD_PTR)(a)) & 0xffff)) | ((DWORD)((WORD)(((DWORD_PTR)(b)) & 0xffff))) << 16))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(((DWORD_PTR)(a)) & 0xff)) | ((WORD)((BYTE)(((DWORD_PTR)(b)) & 0xff))) << 8))
#define MAKELPARAM(l, h) ((LPARAM)(DWORD)MAKELONG(l, h))
#define MAKEWPARAM(l, h) ((WPARAM)(DWORD)MAKELONG(l, h))
#define MAKELRESULT(l, h) ((LRESULT)(DWORD)MAKELONG(l, h))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define GET_KEYSTATE_WPARAM(wParam) (LOWORD(wParam))
#define MAKEINTRESOURCEW(i) ((LPCWSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCEA(i) ((LPCSTR)((ULONG_PTR)((WORD)(i))))
#define MAKEINTRESOURCE MAKEINTRESOURCEA
#define IS_INTRESOURCE(p) ((((ULONG_PTR)(p)) >> 16) == 0)

#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(rgb) (LOBYTE(rgb))
#define GetGValue(rgb) (LOBYTE(((WORD)(rgb)) >> 8))
#define GetBValue(rgb) (LOBYTE((rgb) >> 16))

#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_ACCESSDENIED ((HRESULT)0x80070005)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Window messages
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_DRAWITEM 0x002B
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NOTIFY 0x004E
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_NCLBUTTONDBLCLK 0x00A3
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_CAPTURECHANGED 0x0215
#define WM_EXITSIZEMOVE 0x0232   // never sent here: a Qt window is moved by the window manager
#define WM_DROPFILES 0x0233
#define WM_MOUSELEAVE 0x02A3
#define WM_DPICHANGED 0x02E0
#define WM_HOTKEY 0x0312
#define WM_PRINTCLIENT 0x0318
#define WM_USER 0x0400

// Virtual keys
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE

// ShowWindow
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10

// WM_SIZE
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2

// SetWindowPos
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOSENDCHANGING 0x0400
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)-1)
#define HWND_NOTOPMOST ((HWND)-2)

// Window styles
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_SIZEBOX WS_THICKFRAME
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_POPUPWINDOW (WS_POPUP | WS_BORDER | WS_SYSMENU)
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_TRANSPARENT 0x00000020L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_LAYERED 0x00080000L
#define WS_EX_NOACTIVATE 0x08000000L
#define CW_USEDEFAULT ((int)0x80000000)

// Class styles
#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008

// Edit control
#define ES_LEFT 0x0000L
#define ES_CENTER 0x0001L
#define ES_RIGHT 0x0002L
#define ES_MULTILINE 0x0004L
#define ES_AUTOVSCROLL 0x0040L
#define ES_AUTOHSCROLL 0x0080L
#define ES_NOHIDESEL 0x0100L
#define ES_READONLY 0x0800L
#define ES_WANTRETURN 0x1000L
#define ES_NUMBER 0x2000L
#define EM_GETSEL 0x00B0
#define EM_SETSEL 0x00B1
#define EM_GETRECT 0x00B2
#define EM_SETRECT 0x00B3
#define EM_GETLINECOUNT 0x00BA
#define EM_REPLACESEL 0x00C2
#define EM_LIMITTEXT 0x00C5
#define EM_SETMARGINS 0x00D3
#define EM_GETMARGINS 0x00D4
#define EM_POSFROMCHAR 0x00D6
#define EM_CHARFROMPOS 0x00D7
#define EC_LEFTMARGIN 0x0001
#define EC_RIGHTMARGIN 0x0002
#define EN_SETFOCUS 0x0100
#define EN_KILLFOCUS 0x0200
#define EN_CHANGE 0x0300
#define EN_UPDATE 0x0400

// Rich edit (richedit.h)
#define EM_EXGETSEL (WM_USER + 52)
#define EM_EXLIMITTEXT (WM_USER + 53)
#define EM_EXSETSEL (WM_USER + 55)
#define EM_GETCHARFORMAT (WM_USER + 58)
#define EM_GETEVENTMASK (WM_USER + 59)
#define EM_SETBKGNDCOLOR (WM_USER + 67)
#define EM_SETCHARFORMAT (WM_USER + 68)
#define EM_SETEVENTMASK (WM_USER + 69)
#define EM_GETTEXTRANGE (WM_USER + 75)
#define EM_AUTOURLDETECT (WM_USER + 91)
#define EM_GETTEXTEX (WM_USER + 94)
#define EM_GETTEXTLENGTHEX (WM_USER + 95)
#define EM_SETTEXTEX (WM_USER + 97)
#define EN_LINK 0x070B
#define ENM_NONE 0x00000000
#define ENM_CHANGE 0x00000001
#define ENM_LINK 0x04000000
#define SCF_SELECTION 0x0001
#define SCF_WORD 0x0002
#define SCF_ALL 0x0004
#define CFM_BOLD 0x00000001
#define CFM_ITALIC 0x00000002
#define CFM_UNDERLINE 0x00000004
#define CFM_STRIKEOUT 0x00000008
#define CFM_PROTECTED 0x00000010
#define CFM_LINK 0x00000020
#define CFM_SIZE 0x80000000
#define CFM_COLOR 0x40000000
#define CFM_FACE 0x20000000
#define CFM_OFFSET 0x10000000
#define CFM_CHARSET 0x08000000
#define CFM_HIDDEN 0x00000100
#define CFM_BACKCOLOR 0x04000000
#define CFE_BOLD 0x0001
#define CFE_ITALIC 0x0002
#define CFE_UNDERLINE 0x0004
#define CFE_STRIKEOUT 0x0008
#define CFE_PROTECTED 0x0010
#define CFE_LINK 0x0020
#define CFE_AUTOCOLOR 0x40000000
#define CFE_HIDDEN 0x0100
#define GT_DEFAULT 0
#define GT_USECRLF 1
#define GTL_DEFAULT 0
#define GTL_USECRLF 1
#define GTL_PRECISE 2
#define GTL_CLOSE 4
#define GTL_NUMCHARS 8
#define GTL_NUMBYTES 16
#define MSFTEDIT_CLASS L"RICHEDIT50W"

// Button
#define BS_PUSHBUTTON 0x00000000L
#define BS_OWNERDRAW 0x0000000BL
#define BN_CLICKED 0
#define ODT_MENU 1
#define ODT_LISTBOX 2
#define ODT_COMBOBOX 3
#define ODT_BUTTON 4
#define ODT_STATIC 5
#define ODA_DRAWENTIRE 0x0001
#define ODA_SELECT 0x0002
#define ODA_FOCUS 0x0004
#define ODS_SELECTED 0x0001
#define ODS_GRAYED 0x0002
#define ODS_DISABLED 0x0004
#define ODS_CHECKED 0x0008
#define ODS_FOCUS 0x0010

// Mouse
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define WHEEL_DELTA 120
#define TME_LEAVE 0x00000002
#define TME_HOVER 0x00000001
#define HTERROR (-2)
#define HTTRANSPARENT (-1)
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTLEFT 10
#define HTRIGHT 11
#define HTTOP 12
#define HTTOPLEFT 13
#define HTTOPRIGHT 14
#define HTBOTTOM 15
#define HTBOTTOMLEFT 16
#define HTBOTTOMRIGHT 17
#define HTCLOSE 20
#define MA_ACTIVATE 1
#define MA_ACTIVATEANDEAT 2
#define MA_NOACTIVATE 3
#define MA_NOACTIVATEANDEAT 4

// Cursors and icons
#define IDC_ARROW MAKEINTRESOURCE(32512)
#define IDC_IBEAM MAKEINTRESOURCE(32513)
#define IDC_WAIT MAKEINTRESOURCE(32514)
#define IDC_CROSS MAKEINTRESOURCE(32515)
#define IDC_SIZEALL MAKEINTRESOURCE(32646)
#define IDC_NO MAKEINTRESOURCE(32648)
#define IDC_HAND MAKEINTRESOURCE(32649)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define LR_DEFAULTCOLOR 0x00000000
#define LR_LOADFROMFILE 0x00000010
#define LR_DEFAULTSIZE 0x00000040
#define LR_SHARED 0x00008000
#define ICON_SMALL 0
#define ICON_BIG 1

// System metrics and parameters
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80
#define SPI_GETWORKAREA 0x0030
#define MONITOR_DEFAULTTONULL 0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002
#define MONITORINFOF_PRIMARY 0x00000001

// GetWindowLongPtr
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define GWLP_ID (-12)
#define GWLP_HWNDPARENT (-8)

// Layered windows
#define LWA_COLORKEY 0x00000001
#define LWA_ALPHA 0x00000002

// PeekMessage
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001

// RedrawWindow
#define RDW_INVALIDATE 0x0001
#define RDW_ERASE 0x0004
#define RDW_UPDATENOW 0x0100
#define RDW_ALLCHILDREN 0x0080

// Hotkeys
#define MOD_ALT 0x0001
#define MOD_CONTROL 0x0002
#define MOD_SHIFT 0x0004
#define MOD_WIN 0x0008
#define MOD_NOREPEAT 0x4000

// Clipboard
#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_DIB 8
#define CF_UNICODETEXT 13
#define CF_HDROP 15
#define GMEM_FIXED 0x0000
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)

// File dialogs
#define OFN_READONLY 0x00000001
#define OFN_OVERWRITEPROMPT 0x00000002
#define OFN_HIDEREADONLY 0x00000004
#define OFN_NOCHANGEDIR 0x00000008
#define OFN_PATHMUSTEXIST 0x00000800
#define OFN_FILEMUSTEXIST 0x00001000
#define OFN_EXPLORER 0x00080000

// Fonts
#define FW_DONTCARE 0
#define FW_NORMAL 400
#define FW_SEMIBOLD 600
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define ANSI_CHARSET 0
#define OUT_DEFAULT_PRECIS 0
#define OUT_TT_PRECIS 4
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_QUALITY 0
#define ANTIALIASED_QUALITY 4
#define CLEARTYPE_QUALITY 5
#define DEFAULT_PITCH 0
#define FIXED_PITCH 1
#define VARIABLE_PITCH 2
#define FF_DONTCARE (0 << 4)
#define FF_ROMAN (1 << 4)
#define FF_SWISS (2 << 4)
#define FF_MODERN (3 << 4)

// GDI
#define SRCCOPY 0x00CC0020
#define BI_RGB 0
#define DIB_RGB_COLORS 0
#define TRANSPARENT 1
#define OPAQUE 2
#define PS_SOLID 0
#define NULL_BRUSH 5
#define HOLLOW_BRUSH NULL_BRUSH
#define BLACK_BRUSH 4
#define WHITE_BRUSH 0
#define PW_CLIENTONLY 0x00000001
#define PW_RENDERFULLCONTENT 0x00000002

// Message boxes
#define MB_OK 0x00000000L
#define MB_ICONHAND 0x00000010L
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONASTERISK 0x00000040L
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_ICONWARNING 0x00000030L

// WINDOWPLACEMENT
#define WPF_SETMINPOSITION 0x0001
#define WPF_RESTORETOMAXIMIZED 0x0002

// Dialog ids
#define IDOK 1
#define IDCANCEL 2

// MapVirtualKey
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2

// DPI
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
typedef HANDLE DPI_AWARENESS_CONTEXT;

// Sleep and time
#define INFINITE 0xFFFFFFFF

// Known folders (shlobj.h)
extern const GUID FOLDERID_Documents;
extern const GUID FOLDERID_Pictures;

// ---------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------

// Window classes and windows
BOOL RegisterClassExW(const WNDCLASSEXW* wc);
BOOL RegisterClassExA(const WNDCLASSEXA* wc);
BOOL UnregisterClassW(LPCWSTR className, HINSTANCE inst);
HWND CreateWindowExW(DWORD exStyle, LPCWSTR className, LPCWSTR title, DWORD style, int x, int y,
                     int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
HWND CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR title, DWORD style, int x, int y,
                     int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param);
inline HWND CreateWindowW(LPCWSTR className, LPCWSTR title, DWORD style, int x, int y, int w, int h,
                          HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    return CreateWindowExW(0, className, title, style, x, y, w, h, parent, menu, inst, param);
}
BOOL DestroyWindow(HWND hwnd);
BOOL ShowWindow(HWND hwnd, int cmd);
BOOL UpdateWindow(HWND hwnd);
BOOL InvalidateRect(HWND hwnd, const RECT* rect, BOOL erase);
BOOL RedrawWindow(HWND hwnd, const RECT* rect, HRGN region, UINT flags);
BOOL MoveWindow(HWND hwnd, int x, int y, int w, int h, BOOL repaint);
BOOL SetWindowPos(HWND hwnd, HWND after, int x, int y, int w, int h, UINT flags);
BOOL GetWindowRect(HWND hwnd, RECT* rect);
BOOL GetClientRect(HWND hwnd, RECT* rect);
BOOL ClientToScreen(HWND hwnd, POINT* pt);
BOOL ScreenToClient(HWND hwnd, POINT* pt);
BOOL AdjustWindowRectEx(RECT* rect, DWORD style, BOOL menu, DWORD exStyle);
BOOL AdjustWindowRectExForDpi(RECT* rect, DWORD style, BOOL menu, DWORD exStyle, UINT dpi);
BOOL SetWindowTextW(HWND hwnd, LPCWSTR text);
int GetWindowTextW(HWND hwnd, LPWSTR out, int max);
int GetWindowTextLengthW(HWND hwnd);
LONG_PTR GetWindowLongPtrW(HWND hwnd, int index);
LONG_PTR SetWindowLongPtrW(HWND hwnd, int index, LONG_PTR value);
#define GetWindowLongPtr GetWindowLongPtrW
#define SetWindowLongPtr SetWindowLongPtrW
BOOL GetWindowPlacement(HWND hwnd, WINDOWPLACEMENT* wp);
BOOL SetWindowPlacement(HWND hwnd, const WINDOWPLACEMENT* wp);
BOOL SetLayeredWindowAttributes(HWND hwnd, COLORREF key, BYTE alpha, DWORD flags);
HWND SetFocus(HWND hwnd);
HWND GetFocus();
BOOL SetForegroundWindow(HWND hwnd);
HWND GetForegroundWindow();
HWND GetActiveWindow();
BOOL IsWindowVisible(HWND hwnd);
BOOL IsWindow(HWND hwnd);
BOOL IsIconic(HWND hwnd);
BOOL IsZoomed(HWND hwnd);
HWND GetParent(HWND hwnd);
int GetClassNameW(HWND hwnd, LPWSTR out, int max);
int GetDlgCtrlID(HWND hwnd);
BOOL EnableWindow(HWND hwnd, BOOL enable);
HWND WindowFromPoint(POINT pt);
BOOL EnumWindows(WNDENUMPROC proc, LPARAM lParam);
DWORD GetWindowThreadProcessId(HWND hwnd, DWORD* pid);
BOOL PrintWindow(HWND hwnd, HDC hdc, UINT flags);

// Messages
LRESULT DefWindowProcW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT DefWindowProcA(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT SendMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT SendMessageA(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL PostMessageW(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
BOOL PostMessageA(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#define SendMessage SendMessageW
#define PostMessage PostMessageW
#define DefWindowProc DefWindowProcW
void PostQuitMessage(int code);
BOOL PeekMessageW(MSG* msg, HWND hwnd, UINT first, UINT last, UINT remove);
BOOL GetMessageW(MSG* msg, HWND hwnd, UINT first, UINT last);
BOOL TranslateMessage(const MSG* msg);
LRESULT DispatchMessageW(const MSG* msg);
BOOL IsDialogMessageW(HWND hwnd, MSG* msg);
#define PeekMessage PeekMessageW
#define GetMessage GetMessageW
#define DispatchMessage DispatchMessageW
#define IsDialogMessage IsDialogMessageW

// Painting
HDC BeginPaint(HWND hwnd, PAINTSTRUCT* ps);
BOOL EndPaint(HWND hwnd, const PAINTSTRUCT* ps);
HDC GetDC(HWND hwnd);
int ReleaseDC(HWND hwnd, HDC hdc);
HDC CreateCompatibleDC(HDC hdc);
BOOL DeleteDC(HDC hdc);
HBITMAP CreateCompatibleBitmap(HDC hdc, int w, int h);
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj);
BOOL DeleteObject(HGDIOBJ obj);
HGDIOBJ GetStockObject(int which);
BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD rop);
int GetDIBits(HDC hdc, HBITMAP bmp, UINT start, UINT lines, LPVOID bits, BITMAPINFO* info, UINT usage);
int GetObjectW(HGDIOBJ obj, int size, LPVOID out);
#define GetObject GetObjectW
HBRUSH CreateSolidBrush(COLORREF colour);
HPEN CreatePen(int style, int width, COLORREF colour);
HFONT CreateFontW(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                  DWORD underline, DWORD strikeOut, DWORD charSet, DWORD outPrecision,
                  DWORD clipPrecision, DWORD quality, DWORD pitchAndFamily, LPCWSTR face);
HFONT CreateFontA(int height, int width, int escapement, int orientation, int weight, DWORD italic,
                  DWORD underline, DWORD strikeOut, DWORD charSet, DWORD outPrecision,
                  DWORD clipPrecision, DWORD quality, DWORD pitchAndFamily, LPCSTR face);
COLORREF SetTextColor(HDC hdc, COLORREF colour);
COLORREF SetBkColor(HDC hdc, COLORREF colour);
int SetBkMode(HDC hdc, int mode);
int FillRect(HDC hdc, const RECT* rect, HBRUSH brush);
BOOL TextOutA(HDC hdc, int x, int y, LPCSTR text, int length);
BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom);

// Rectangles and points
BOOL PtInRect(const RECT* rect, POINT pt);
BOOL OffsetRect(RECT* rect, int dx, int dy);
BOOL SetRect(RECT* rect, int left, int top, int right, int bottom);
BOOL IsRectEmpty(const RECT* rect);
BOOL IntersectRect(RECT* out, const RECT* a, const RECT* b);
BOOL EqualRect(const RECT* a, const RECT* b);

// Mouse, keyboard, timers, hotkeys
HWND SetCapture(HWND hwnd);
BOOL ReleaseCapture();
HWND GetCapture();
BOOL TrackMouseEvent(TRACKMOUSEEVENT* tme);
BOOL GetCursorPos(POINT* pt);
HCURSOR SetCursor(HCURSOR cursor);
HCURSOR LoadCursorW(HINSTANCE inst, LPCWSTR name);
HCURSOR LoadCursorA(HINSTANCE inst, LPCSTR name);
#define LoadCursor LoadCursorA
HICON LoadIconW(HINSTANCE inst, LPCWSTR name);
HICON LoadIconA(HINSTANCE inst, LPCSTR name);
#define LoadIcon LoadIconA
HANDLE LoadImageW(HINSTANCE inst, LPCWSTR name, UINT type, int cx, int cy, UINT flags);
SHORT GetKeyState(int vk);
SHORT GetAsyncKeyState(int vk);
UINT MapVirtualKeyW(UINT code, UINT type);
UINT GetCaretBlinkTime();
UINT_PTR SetTimer(HWND hwnd, UINT_PTR id, UINT ms, void* proc);
BOOL KillTimer(HWND hwnd, UINT_PTR id);
BOOL RegisterHotKey(HWND hwnd, int id, UINT modifiers, UINT vk);
BOOL UnregisterHotKey(HWND hwnd, int id);
DWORD GetTickCount();
uint64_t GetTickCount64();
void Sleep(DWORD ms);
BOOL MessageBeep(UINT type);
BOOL Beep(DWORD frequency, DWORD ms);

// Screens
int GetSystemMetrics(int index);
BOOL SystemParametersInfoW(UINT action, UINT param, LPVOID out, UINT winIni);
HMONITOR MonitorFromWindow(HWND hwnd, DWORD flags);
HMONITOR MonitorFromPoint(POINT pt, DWORD flags);
BOOL GetMonitorInfoW(HMONITOR monitor, MONITORINFO* info);
BOOL GetMonitorInfoA(HMONITOR monitor, MONITORINFO* info);
#define GetMonitorInfo GetMonitorInfoW
BOOL EnumDisplayMonitors(HDC hdc, const RECT* clip, MONITORENUMPROC proc, LPARAM lParam);
UINT GetDpiForSystem();
UINT GetDpiForWindow(HWND hwnd);
BOOL SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT context);
int MulDiv(int number, int numerator, int denominator);

// Shell, dialogs, clipboard
HINSTANCE ShellExecuteW(HWND hwnd, LPCWSTR verb, LPCWSTR file, LPCWSTR params, LPCWSTR dir, int show);
int MessageBoxW(HWND hwnd, LPCWSTR text, LPCWSTR title, UINT type);
int MessageBoxA(HWND hwnd, LPCSTR text, LPCSTR title, UINT type);
BOOL GetOpenFileNameW(OPENFILENAMEW* ofn);
BOOL GetSaveFileNameW(OPENFILENAMEW* ofn);
HRESULT SHGetKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* out);
void CoTaskMemFree(void* p);
void* CoTaskMemAlloc(size_t bytes);
BOOL OpenClipboard(HWND hwnd);
BOOL CloseClipboard();
BOOL EmptyClipboard();
BOOL IsClipboardFormatAvailable(UINT format);
HANDLE GetClipboardData(UINT format);
HANDLE SetClipboardData(UINT format, HANDLE data);
UINT DragQueryFileW(HDROP drop, UINT index, LPWSTR out, UINT max);
HGLOBAL GlobalAlloc(UINT flags, size_t bytes);
LPVOID GlobalLock(HGLOBAL mem);
BOOL GlobalUnlock(HGLOBAL mem);
HGLOBAL GlobalFree(HGLOBAL mem);
size_t GlobalSize(HGLOBAL mem);

// Modules, errors, strings
HMODULE GetModuleHandleW(LPCWSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
#define GetModuleHandle GetModuleHandleA
HMODULE LoadLibraryW(LPCWSTR name);
BOOL FreeLibrary(HMODULE module);
DWORD GetLastError();
void SetLastError(DWORD error);
int MultiByteToWideChar(UINT codePage, DWORD flags, LPCSTR src, int srcLen, LPWSTR dst, int dstLen);
int WideCharToMultiByte(UINT codePage, DWORD flags, LPCWSTR src, int srcLen, LPSTR dst, int dstLen,
                        LPCSTR defaultChar, LPBOOL usedDefault);
DWORD GetCurrentProcessId();
HRESULT DwmSetWindowAttribute(HWND hwnd, DWORD attribute, LPCVOID value, DWORD size);

// ---------------------------------------------------------------------------
// The shim's own doors, for the GDI+ shim and the Linux application. Not
// Windows names; nothing in MinerGUI.cpp calls them.
// ---------------------------------------------------------------------------
class QPainter;
class QImage;
class QWidget;
namespace bedrock_shim {
// The painter behind a DC: the window's while it paints, the bitmap's for a
// memory DC, a scratch one for a DC that has nothing to draw on (measuring)
QPainter* painterFor(HDC hdc);
// The picture behind a DC's selected bitmap, or null
QImage* imageFor(HDC hdc);
// A packed DIB, as the clipboard hands it over, read into a picture
QImage imageFromDib(const BITMAPINFOHEADER* header, const void* bits);
// The widget behind a handle, for the application's own needs
QWidget* widgetFor(HWND hwnd);
// Registers a picture the binary carries as the icon LoadImageW hands out
void setApplicationIcon(const QImage& icon);
// Registers the faces the binary carries (EmbeddedData: Selawik, D-DIN) and
// makes them answer to the names the window asks for - Segoe UI,
// Bahnschrift, Consolas. Before the first window; returns how many loaded.
int registerFonts();
}  // namespace bedrock_shim
