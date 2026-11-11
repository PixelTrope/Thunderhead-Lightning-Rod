#pragma once

#include <windows.h>

namespace tlr {

// Thunderhead's two color schemes. The light scheme is Windows' own with the
// Client Inventory's near-black header and gray rows; Dark Mode reverses it:
// dark panes with light text, a light header with black captions, dark rows
// with white text and white rules. Everything that paints asks theme() for
// its colors, and setDarkMode() switches the scheme at run time.
struct Theme {
    bool dark;
    COLORREF windowBack;    // behind lists
    COLORREF paneBack;      // panes, dialogs, bars
    COLORREF text;
    COLORREF grayText;
    COLORREF editBack;      // text boxes
    COLORREF editText;
    COLORREF listBack;      // the list beyond its rows
    COLORREF headerBack;
    COLORREF headerText;
    COLORREF headerRule;
    COLORREF rowLight;      // even rows
    COLORREF rowMedium;     // odd rows
    COLORREF rowText;
    COLORREF columnRule;
    COLORREF pushFace;      // owner-drawn push buttons in dark mode
    COLORREF pushPressed;
    COLORREF pushEdge;
    COLORREF pushDefaultEdge;
    COLORREF switchOff;     // the Dark Mode slider's track
    COLORREF switchOn;
    COLORREF switchKnob;
    COLORREF bandMix;       // command pane: the color the group tints are mixed towards, and how much
    int bandPct;
    int facePct;
};

const Theme& theme();
void setDarkMode(bool dark);

// Cached brushes of the current scheme; valid until the scheme changes.
HBRUSH themeBrush(COLORREF color);

// For WM_CTLCOLORSTATIC, WM_CTLCOLORBTN, WM_CTLCOLOREDIT, WM_CTLCOLORLISTBOX
// and WM_CTLCOLORDLG: sets the DC's colors and returns the brush in Dark
// Mode, NULL in the light scheme so that the caller keeps its own handling.
HBRUSH themeCtlColor(UINT msg, HDC dc, HWND control);

// Dark or light title bar for a top-level window (Windows 10 1809 and later;
// no effect elsewhere).
void themeWindowFrame(HWND top);

// Dresses one control for the current scheme: push buttons become owner-drawn
// in Dark Mode and ordinary again in the light scheme, check boxes drop the
// visual style in Dark Mode so that their text takes the WM_CTLCOLORBTN color,
// combo boxes, edits, lists and scroll bars take Windows' dark control theme.
// Owner-drawn buttons the caller paints itself are left alone.
void themeControl(HWND control);
// themeControl for every child of top, recursively, and the frame.
void themeWindow(HWND top);

// Paints an owner-drawn push button that themeControl converted (or any push
// button of the caller's, in both schemes). Returns false for other items.
bool themeDrawPushButton(const DRAWITEMSTRUCT* di);

// The two-position slider: a rounded track, the knob at the left for off and
// at the right for on, the focus marked around the track.
void themeDrawSwitch(const DRAWITEMSTRUCT* di, bool on);

// One line for a dialog procedure's start: handles WM_INITDIALOG (frame and
// controls), the WM_CTLCOLOR messages and WM_DRAWITEM of converted push
// buttons. Returns the value to return from the procedure, 0 when the message
// is not the theme's.
INT_PTR themeDialogMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp);

} // namespace tlr
