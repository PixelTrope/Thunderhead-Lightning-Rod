#include "Theme.h"

#include <uxtheme.h>
#include <map>
#include <string>

namespace tlr {

namespace {

Theme g_theme;
bool g_themeSet = false;
std::map<COLORREF, HBRUSH> g_brushes;

// Window property on a push button that themeControl made owner-drawn: its
// original button type plus one (BS_PUSHBUTTON is 0).
const wchar_t* const kOrigTypeProp = L"ThunderheadThemeOrigType";

void setLight(Theme& t)
{
    t.dark = false;
    t.windowBack = GetSysColor(COLOR_WINDOW);
    t.paneBack = GetSysColor(COLOR_BTNFACE);
    t.text = GetSysColor(COLOR_BTNTEXT);
    t.grayText = GetSysColor(COLOR_GRAYTEXT);
    t.editBack = GetSysColor(COLOR_WINDOW);
    t.editText = GetSysColor(COLOR_WINDOWTEXT);
    t.listBack = GetSysColor(COLOR_WINDOW);
    t.headerBack = RGB(28, 28, 28);
    t.headerText = RGB(255, 255, 255);
    t.headerRule = RGB(96, 96, 96);
    t.rowLight = RGB(232, 232, 232);
    t.rowMedium = RGB(200, 200, 200);
    t.rowText = RGB(0, 0, 0);
    t.columnRule = RGB(0, 0, 0);
    t.pushFace = GetSysColor(COLOR_BTNFACE);
    t.pushPressed = GetSysColor(COLOR_BTNSHADOW);
    t.pushEdge = GetSysColor(COLOR_BTNSHADOW);
    t.pushDefaultEdge = GetSysColor(COLOR_HIGHLIGHT);
    t.switchOff = RGB(160, 160, 160);
    t.switchOn = RGB(0, 120, 215);
    t.switchKnob = RGB(255, 255, 255);
    t.bandMix = RGB(255, 255, 255);
    t.bandPct = 100;
    t.facePct = 100;
}

void setDark(Theme& t)
{
    t.dark = true;
    t.windowBack = RGB(32, 32, 32);
    t.paneBack = RGB(43, 43, 43);
    t.text = RGB(240, 240, 240);
    t.grayText = RGB(150, 150, 150);
    t.editBack = RGB(30, 30, 30);
    t.editText = RGB(230, 230, 230);
    t.listBack = RGB(40, 40, 40);
    t.headerBack = RGB(232, 232, 232);
    t.headerText = RGB(0, 0, 0);
    t.headerRule = RGB(150, 150, 150);
    t.rowLight = RGB(48, 48, 48);
    t.rowMedium = RGB(72, 72, 72);
    t.rowText = RGB(240, 240, 240);
    t.columnRule = RGB(255, 255, 255);
    t.pushFace = RGB(62, 62, 62);
    t.pushPressed = RGB(45, 45, 45);
    t.pushEdge = RGB(120, 120, 120);
    t.pushDefaultEdge = RGB(0, 120, 215);
    t.switchOff = RGB(110, 110, 110);
    t.switchOn = RGB(0, 120, 215);
    t.switchKnob = RGB(240, 240, 240);
    t.bandMix = RGB(24, 24, 24);
    t.bandPct = 28;
    t.facePct = 40;
}

std::wstring className(HWND h)
{
    wchar_t name[64] = L"";
    GetClassNameW(h, name, 64);
    return name;
}

bool isClass(HWND h, const wchar_t* name)
{
    return _wcsicmp(className(h).c_str(), name) == 0;
}

BOOL CALLBACK themeChild(HWND h, LPARAM)
{
    themeControl(h);
    return TRUE;
}

} // namespace

const Theme& theme()
{
    if (!g_themeSet) setDarkMode(false);
    return g_theme;
}

void setDarkMode(bool dark)
{
    if (dark) setDark(g_theme); else setLight(g_theme);
    g_themeSet = true;
    for (std::map<COLORREF, HBRUSH>::iterator it = g_brushes.begin(); it != g_brushes.end(); ++it) DeleteObject(it->second);
    g_brushes.clear();
}

HBRUSH themeBrush(COLORREF color)
{
    std::map<COLORREF, HBRUSH>::iterator it = g_brushes.find(color);
    if (it != g_brushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(color);
    g_brushes[color] = b;
    return b;
}

HBRUSH themeCtlColor(UINT msg, HDC dc, HWND control)
{
    const Theme& t = theme();
    if (!t.dark) return NULL;
    bool textBox = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX ||
                   (control && (isClass(control, L"Edit") || isClass(control, L"ListBox") || isClass(control, L"ComboBox")));
    COLORREF back = textBox ? t.editBack : t.paneBack;
    COLORREF fore = textBox ? t.editText : t.text;
    if (control && !IsWindowEnabled(control)) fore = t.grayText;
    SetBkMode(dc, OPAQUE);
    SetBkColor(dc, back);
    SetTextColor(dc, fore);
    return themeBrush(back);
}

void themeWindowFrame(HWND top)
{
    typedef HRESULT (WINAPI *Fn)(HWND, DWORD, LPCVOID, DWORD);
    static Fn fn = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE lib = LoadLibraryW(L"dwmapi.dll");
        if (lib) fn = (Fn)GetProcAddress(lib, "DwmSetWindowAttribute");
    }
    if (!fn || !top) return;
    BOOL dark = theme().dark ? TRUE : FALSE;
    const DWORD kImmersiveDarkMode = 20, kImmersiveDarkModeBefore20H1 = 19;
    if (FAILED(fn(top, kImmersiveDarkMode, &dark, sizeof(dark)))) fn(top, kImmersiveDarkModeBefore20H1, &dark, sizeof(dark));
}

void themeControl(HWND h)
{
    const Theme& t = theme();
    std::wstring cls = className(h);
    if (_wcsicmp(cls.c_str(), L"Button") == 0) {
        LONG style = GetWindowLongW(h, GWL_STYLE);
        LONG type = style & BS_TYPEMASK;
        LONG orig = (LONG)(INT_PTR)GetPropW(h, kOrigTypeProp);
        LONG origType = orig ? orig - 1 : type;
        if (origType == BS_PUSHBUTTON || origType == BS_DEFPUSHBUTTON) {
            if (t.dark && !orig) {
                SetPropW(h, kOrigTypeProp, (HANDLE)(INT_PTR)(origType + 1));
                SendMessageW(h, BM_SETSTYLE, (WPARAM)((style & 0xFFFF & ~BS_TYPEMASK) | BS_OWNERDRAW), TRUE);
            } else if (!t.dark && orig) {
                RemovePropW(h, kOrigTypeProp);
                SendMessageW(h, BM_SETSTYLE, (WPARAM)((style & 0xFFFF & ~BS_TYPEMASK) | origType), TRUE);
            }
        } else if (origType == BS_CHECKBOX || origType == BS_AUTOCHECKBOX || origType == BS_RADIOBUTTON ||
                   origType == BS_AUTORADIOBUTTON || origType == BS_3STATE || origType == BS_AUTO3STATE || origType == BS_GROUPBOX) {
            // Without the visual style the check box text takes WM_CTLCOLORBTN's color.
            SetWindowTheme(h, t.dark ? L"" : NULL, t.dark ? L"" : NULL);
        }
    } else if (_wcsicmp(cls.c_str(), L"ComboBox") == 0) {
        SetWindowTheme(h, t.dark ? L"DarkMode_CFD" : NULL, NULL);
    } else if (_wcsicmp(cls.c_str(), L"Edit") == 0 || _wcsicmp(cls.c_str(), L"SysListView32") == 0 ||
               _wcsicmp(cls.c_str(), L"SysTreeView32") == 0 || _wcsicmp(cls.c_str(), L"ScrollBar") == 0 ||
               _wcsicmp(cls.c_str(), L"ListBox") == 0) {
        SetWindowTheme(h, t.dark ? L"DarkMode_Explorer" : NULL, NULL);
    }
    InvalidateRect(h, NULL, TRUE);
}

void themeWindow(HWND top)
{
    if (!top) return;
    themeWindowFrame(top);
    EnumChildWindows(top, themeChild, 0);
    RedrawWindow(top, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

bool themeDrawPushButton(const DRAWITEMSTRUCT* di)
{
    if (di->CtlType != ODT_BUTTON) return false;
    const Theme& t = theme();
    bool disabled = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0;
    LONG orig = (LONG)(INT_PTR)GetPropW(di->hwndItem, kOrigTypeProp);
    bool isDefault = orig == BS_DEFPUSHBUTTON + 1 || (di->itemState & ODS_DEFAULT) != 0;
    RECT rc = di->rcItem;
    FillRect(di->hDC, &rc, themeBrush(pressed ? t.pushPressed : t.pushFace));
    FrameRect(di->hDC, &rc, themeBrush(isDefault ? t.pushDefaultEdge : t.pushEdge));
    if (isDefault) {
        RECT inner = rc;
        InflateRect(&inner, -1, -1);
        FrameRect(di->hDC, &inner, themeBrush(t.pushDefaultEdge));
    }
    wchar_t text[128] = L"";
    GetWindowTextW(di->hwndItem, text, 128);
    HFONT font = (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0);
    HGDIOBJ oldFont = font ? SelectObject(di->hDC, font) : NULL;
    int oldMode = SetBkMode(di->hDC, TRANSPARENT);
    COLORREF oldColor = SetTextColor(di->hDC, disabled ? t.grayText : t.text);
    RECT tr = rc;
    if (pressed) OffsetRect(&tr, 1, 1);
    DrawTextW(di->hDC, text, -1, &tr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
    SetTextColor(di->hDC, oldColor);
    SetBkMode(di->hDC, oldMode);
    if (oldFont) SelectObject(di->hDC, oldFont);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, -3, -3);
        DrawFocusRect(di->hDC, &fr);
    }
    return true;
}

void themeDrawSwitch(const DRAWITEMSTRUCT* di, bool on)
{
    const Theme& t = theme();
    RECT rc = di->rcItem;
    FillRect(di->hDC, &rc, themeBrush(t.paneBack));
    int h = rc.bottom - rc.top, w = rc.right - rc.left;
    int th = h < 20 ? h : 20;
    int tw = w < th * 2 ? w : th * 2;
    RECT track;
    track.left = rc.left + (w - tw) / 2;
    track.top = rc.top + (h - th) / 2;
    track.right = track.left + tw;
    track.bottom = track.top + th;
    COLORREF fill = (di->itemState & ODS_DISABLED) ? t.grayText : on ? t.switchOn : t.switchOff;
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldBrush = SelectObject(di->hDC, brush), oldPen = SelectObject(di->hDC, pen);
    RoundRect(di->hDC, track.left, track.top, track.right, track.bottom, th, th);
    int d = th - 4;
    int kx = on ? track.right - 2 - d : track.left + 2;
    HBRUSH knob = CreateSolidBrush(t.switchKnob);
    HPEN knobPen = CreatePen(PS_SOLID, 1, t.switchKnob);
    SelectObject(di->hDC, knob);
    SelectObject(di->hDC, knobPen);
    Ellipse(di->hDC, kx, track.top + 2, kx + d, track.top + 2 + d);
    SelectObject(di->hDC, oldPen);
    SelectObject(di->hDC, oldBrush);
    DeleteObject(knobPen);
    DeleteObject(knob);
    DeleteObject(pen);
    DeleteObject(brush);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = track;
        InflateRect(&fr, 3, 3);
        DrawFocusRect(di->hDC, &fr);
    }
}

INT_PTR themeDialogMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG:
        themeWindow(dlg);
        return 0;   // the dialog's own initialization follows
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        return (INT_PTR)themeCtlColor(msg, (HDC)wp, (HWND)lp);
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON && GetPropW(di->hwndItem, kOrigTypeProp)) return themeDrawPushButton(di) ? TRUE : 0;
        return 0;
    }
    }
    return 0;
}

} // namespace tlr
