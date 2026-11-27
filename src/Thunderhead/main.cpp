// Thunderhead: host console for the Bryce Lightning render farm.
// See docs/PROTOCOL.md for the wire protocol and files.
//
// Window layout: the left half is the Client Inventory (one row per node with a
// check box, the per-cycle values, an Auto Shutdown check box, Group and Comment
// editable in place); the top
// right is the detail pane for the clicked node (stored data, HELLO data, the
// latest report and the five flags);
// the bottom right is the command pane, a grid of push buttons that act on every
// checked node.

#include "Net.h"
#include "Log.h"
#include "Protocol.h"
#include "StrUtil.h"
#include "Version.h"
#include "NodeStore.h"
#include "Server.h"
#include "Theme.h"
#include "Sound.h"
#include "Auth.h"
#include "BryceScene.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <winnetwk.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <objbase.h>
#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <deque>
#include <map>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace tlr;

namespace {

const UINT WM_NET = WM_APP + 1;
const UINT WM_UPDATE_DONE = WM_APP + 2;   // lParam: UpdateResult*
const UINT WM_THEME_CHANGED = WM_APP + 5; // to every other top-level window of this thread: dress for the current scheme
const UINT WM_SHARE_CHECKED = WM_APP + 6; // lParam: ShareResult* from a share check thread
const UINT WM_COPY_DONE = WM_APP + 7;     // lParam: CopyAfter* from the copy thread: the message to send once a copy job has ended
const UINT_PTR kTimerId = 1;
const UINT_PTR kNudgeTimerId = 2;   // 100 ms while a nudge SET is pending
const UINT_PTR kFlashTimerId = 3;   // 100 ms while a row is painted in the ping color

HINSTANCE g_inst = NULL;
HWND g_main = NULL, g_status = NULL;
HWND g_inventoryPane = NULL, g_staticPane = NULL, g_commandPane = NULL;
HWND g_list = NULL;                       // Client Inventory
HWND g_staticTitle = NULL, g_staticList = NULL, g_flags[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
HWND g_flagHeader = NULL, g_bootAlarm = NULL;   // the Info Flags header and the Boot on Alarm time picker
HWND g_edit = NULL;                       // in-place editor over a Group or Comment cell
int g_editRow = -1, g_editCol = -1;
bool g_editClosing = false;
NodeRecord* g_staticNode = NULL;          // node shown in the static pane
HFONT g_font = NULL;
std::wstring g_dir;
NodeStore* g_store = NULL;
Server* g_server = NULL;
Logger g_events;
std::map<std::string, Logger*> g_nodeLogs;
std::string g_farmKey;                    // farm.key beside Thunderhead.exe; see docs/PROTOCOL.md, Authentication
std::string g_pushVersion;                // version of the agent file to push; what Outdated is measured against

// SMB1 relay lookups; defined with the relay code below.
bool isRelay(const NodeRecord* n);
std::string relayTarget(const NodeRecord* n);
NodeRecord* nodeAtIp(const std::string& ip);
NodeRecord* relayFor(const NodeRecord* n);
NodeRecord* nodeOfRelayTarget(const NodeRecord* relay);
bool validIpText(const std::string& s);
// Shared-folder gate and authentication; defined with the share check code below.
std::string shareStateText(const NodeRecord* n);
std::string authText(const NodeRecord* n);
bool nodeOutdated(const NodeRecord* n);
bool commandAllowed(NodeRecord* n, const std::string& label);
bool sendToNode(NodeRecord* n, const Message& m, const std::string& label);
void createRemoteShare(const std::vector<NodeRecord*>& nodes);
std::wstring shareRootOf(const NodeRecord* n);

// The reply watch (sound events App Sends a Message to a Client, Client Did Not
// Receive or Answer a Command): every message that leaves for a client plays
// the send sound, except SET (the report-interval and spacing adjustments, which
// go out every few seconds); a message the client cannot receive (the gate refused it, or
// the send failed) plays the no-reply sound at once; a command that the agent
// answers with ACK is remembered in the node's pendingReplies and plays the
// no-reply sound when no ACK has come within kReplyWaitMs or the node goes
// offline first. SET is sent in the blind, PING has its own watch (kPingWaitMs,
// answered by HELLO, REPORT or PONG) and RELAY is awaited by relayRequest.
const unsigned long long kReplyWaitMs = 15000;
void logEvent(const std::string& text);
void logNode(const NodeRecord& n, const std::string& text);
void playEventSound(SoundEvent e);

bool verbExpectsAck(const std::string& verb) { return verb != "SET" && verb != "PING" && verb != "RELAY"; }

void expectReply(NodeRecord* n, const std::string& verb)
{
    if (verbExpectsAck(verb)) n->pendingReplies.push_back(std::make_pair(verb, tickMs()));
}

// The ACK with cmd= verb clears the oldest pending entry of that verb (BATCH and
// MESSAGE acknowledge twice; the first ACK is the receipt).
void replyArrived(NodeRecord* n, const std::string& verb)
{
    for (size_t i = 0; i < n->pendingReplies.size(); ++i)
        if (n->pendingReplies[i].first == verb) { n->pendingReplies.erase(n->pendingReplies.begin() + i); return; }
}

void noReply(NodeRecord* n, const std::string& what)
{
    logEvent(format("%s: %s", n->name.c_str(), what.c_str()));
    logNode(*n, what);
    playEventSound(SoundThNoReply);
}

// The one gate in front of every message Thunderhead sends to a client: nothing
// but SET, the report schedule, leaves this host for a node whose shared folder
// it has not read (see the shared-folder gate). Every send site goes through here.
bool sendGated(NodeRecord* n, const Message& m)
{
    if (m.verb != "SET" && !commandAllowed(n, m.oneLine())) { playEventSound(SoundThNoReply); return false; }
    if (!g_server->send(n->conn, m)) { noReply(n, m.verb + " could not be sent: its connection is gone"); return false; }
    playEventSound(SoundThSend);
    expectReply(n, m.verb);
    return true;
}
int shadeKey(const NodeRecord* n);
COLORREF rowShade(const NodeRecord* n, int row, COLORREF* textColor);
enum RowTint { TintNone, TintOffline, TintUnauthenticated, TintOutdated, TintRenderHost, TintRelay };
int rowTint(const NodeRecord* n);
bool isRenderHost(const NodeRecord* n);
bool clearAutoShutdownOfHost(NodeRecord* n);
std::wstring renderHostText(const NodeRecord* n);
std::wstring renderBinText(const NodeRecord* n);
NodeRecord* nodeAt(int row);
void fillRect(HDC dc, const RECT& rc, COLORREF color);

// ---------------------------------------------------------------- inventory columns

enum Column {
    ColName, ColIp, ColMac, ColStatus, ColAgent, ColRendering, ColLightning, ColLightningCpu, ColCpuPct, ColCpuTemp, ColMemFree,
    ColMemUsable, ColMemInstalled, ColDiskFree,
    ColUptime, ColLastReport, ColAutoShutdown, ColGroup, ColComment, ColCount
};

struct ColumnDef { const wchar_t* title; int width; int align; };

const ColumnDef kColumns[ColCount] = {
    { L"Name",         130, LVCFMT_LEFT },
    { L"IP",           100, LVCFMT_LEFT },
    { L"MAC",          118, LVCFMT_LEFT },
    { L"Status",        65, LVCFMT_LEFT },
    { L"Agent",         50, LVCFMT_LEFT },
    { L"Rendering",     70, LVCFMT_LEFT },
    { L"Lightning",     70, LVCFMT_LEFT },
    { L"Lightning %",   75, LVCFMT_RIGHT },
    { L"CPU %",         50, LVCFMT_RIGHT },
    { L"CPU temp",     110, LVCFMT_RIGHT },
    { L"Mem free MB",   80, LVCFMT_RIGHT },
    { L"Mem usable MB", 90, LVCFMT_RIGHT },
    { L"Mem installed MB", 105, LVCFMT_RIGHT },
    { L"C: free MB",    75, LVCFMT_RIGHT },
    { L"Uptime",        90, LVCFMT_RIGHT },
    { L"Last report",   80, LVCFMT_RIGHT },
    { L"Auto Shutdown", 95, LVCFMT_CENTER },
    { L"Group",        100, LVCFMT_LEFT },
    { L"Comment",      220, LVCFMT_LEFT },
};

// ---------------------------------------------------------------- command buttons

// The buttons sit in groups that mirror the Control menu, in the same order,
// each group on its own rows with a tint of its own on the buttons and the
// band behind them, so that the groups read without captions. A command with
// a parent is a nested one: the menu shows it in the parent's submenu, and the
// pane shows the parent as one button that opens a pop-up window holding the
// nested buttons (see commandPopup). Run Lightning has no nested commands; it
// asks the generation in a radio dialog, as Start B3D does.
struct CommandGroup { const wchar_t* name; COLORREF band; COLORREF face; };

const CommandGroup kGroups[] = {
    { L"Thunderhead Control",  RGB(196, 204, 220), RGB(170, 184, 210) },
    { L"List Control",         RGB(184, 226, 184), RGB(150, 208, 150) },
    { L"Node Control",         RGB(180, 206, 242), RGB(140, 180, 232) },
    { L"LightningRod Control", RGB(242, 216, 160), RGB(234, 196, 116) },
    { L"Comms",                RGB(220, 198, 238), RGB(200, 168, 228) },
    { L"Render Control",       RGB(242, 192, 186), RGB(232, 160, 150) },
};
enum { GroupThunderhead, GroupList, GroupNode, GroupRod, GroupComms, GroupRender, kGroupCount };

struct CommandDef { int id; const wchar_t* label; const char* verb; bool live; int group; int parent; };

const CommandDef kCommands[] = {
    { IDC_CMD_VIEW_LOCAL_LOG,  L"View Local Log",                 NULL,      true,  GroupThunderhead, 0 },
    { IDC_CMD_SETTINGS,        L"Settings",                       NULL,      true,  GroupThunderhead, 0 },
    { IDC_CMD_CHECK_ALL,       L"Check All (Ctrl-A)",             NULL,      true,  GroupList, 0 },
    { IDC_CMD_CHECK_NONE,      L"Check None (Ctrl-D)",            NULL,      true,  GroupList, 0 },
    { IDC_CMD_INVERT,          L"Invert Selections",              NULL,      true,  GroupList, 0 },
    { IDC_CMD_SELECT_GROUP,    L"Select Group",                   NULL,      true,  GroupList, 0 },
    { IDC_CMD_SEL_OFFLINE,     L"Offline (Red)",                  NULL,      true,  GroupList, IDC_CMD_SELECT_GROUP },
    { IDC_CMD_SEL_UNAUTH,      L"Unauthenticated (Orange)",       NULL,      true,  GroupList, IDC_CMD_SELECT_GROUP },
    { IDC_CMD_SEL_OUTDATED,    L"Outdated LR (Yellow)",           NULL,      true,  GroupList, IDC_CMD_SELECT_GROUP },
    { IDC_CMD_SEL_RELAY,       L"W7 Relay (SMB1><SMB2><SMB3) (Cyan)", NULL,  true,  GroupList, IDC_CMD_SELECT_GROUP },
    { IDC_CMD_SEL_HOST,        L"Bryce 3D Net Render Host (Blue)", NULL,     true,  GroupList, IDC_CMD_SELECT_GROUP },
    { IDC_CMD_WOL,             L"Send Wake On LAN Magic Packet",  NULL,      true,  GroupNode, 0 },
    { IDC_CMD_SHUTDOWN_NODE,   L"Shutdown Node",                  NULL,      true,  GroupNode, 0 },
    { IDC_CMD_ABORT_SHUTDOWN,  L"Abort Shutdown",                 NULL,      true,  GroupNode, 0 },
    { IDC_CMD_COMMAND_FILE,    L"Command File",                   NULL,      true,  GroupNode, 0 },
    { IDC_CMD_BATCH,           L"Run Remote File",                NULL,      true,  GroupNode, IDC_CMD_COMMAND_FILE },
    { IDC_CMD_OPEN_SHARE,      L"Open Client's Shared Folder",    NULL,      true,  GroupNode, IDC_CMD_COMMAND_FILE },
    { IDC_CMD_COPY,            L"Copy Files or Folder to Client", NULL,      true,  GroupNode, IDC_CMD_COMMAND_FILE },
    { IDC_CMD_UTILITIES,       L"Utilities",                      NULL,      true,  GroupNode, 0 },
    { IDC_CMD_NETPING,         L"Network Ping",                   NULL,      true,  GroupNode, IDC_CMD_UTILITIES },
    { IDC_CMD_RENAME,          L"Rename Node",                    NULL,      true,  GroupNode, IDC_CMD_UTILITIES },
    { IDC_CMD_SET_TIME,        L"Set Client Date and Time",       NULL,      true,  GroupNode, IDC_CMD_UTILITIES },
    { IDC_CMD_START_TV,        L"Start TeamViewer",               NULL,      true,  GroupNode, IDC_CMD_UTILITIES },
    { IDC_CMD_PING,            L"Ping LR",                        NULL,      true,  GroupRod, 0 },
    { IDC_CMD_RESTART_LR,      L"Restart LR",                     "RESTART", true,  GroupRod, 0 },
    { IDC_CMD_UPDATE,          L"Update LR",                      NULL,      true,  GroupRod, 0 },
    { IDC_CMD_EXIT_LR,         L"Exit LR",                        NULL,      true,  GroupRod, 0 },   // confirmed per client by exitChecked
    { IDC_CMD_CONFIGURE,       L"Configure Client",               NULL,      true,  GroupRod, 0 },
    { IDC_CMD_TOGGLE_RENDERHOST, L"Bryce 3D Net Render Host Toggle", NULL,   true,  GroupRod, IDC_CMD_CONFIGURE },
    { IDC_CMD_SET_RELAY,       L"Relay Settings",                 NULL,      true,  GroupRod, IDC_CMD_CONFIGURE },
    { IDC_CMD_VIEW_REMOTE_LOG, L"View Remote Log",                NULL,      true,  GroupRod, IDC_CMD_CONFIGURE },
    { IDC_CMD_REMOVE_OFFLINE,  L"Delist Offline Clients",         NULL,      true,  GroupRod, IDC_CMD_CONFIGURE },
    { IDC_CMD_MESSAGE,         L"Display Message",                NULL,      true,  GroupComms, 0 },
    { IDC_CMD_SOUND,           L"Play Sound",                     NULL,      true,  GroupComms, 0 },
    { IDC_CMD_MUTE,            L"Mute Checked Clients",           NULL,      true,  GroupComms, 0 },
    { IDC_CMD_UNMUTE,          L"Unmute Checked Clients",         NULL,      true,  GroupComms, 0 },
    { IDC_CMD_BRYCE3D,         L"Bryce 3D",                       NULL,      true,  GroupRender, 0 },
    { IDC_CMD_BRYCE_START,     L"Start B3D",                      NULL,      true,  GroupRender, IDC_CMD_BRYCE3D },
    { IDC_CMD_RENDER_JOB,      L"Render Job on Host",             NULL,      true,  GroupRender, IDC_CMD_BRYCE3D },
    { IDC_CMD_BRYCE_STOP,      L"Stop B3D",                       NULL,      true,  GroupRender, IDC_CMD_BRYCE3D },
    { IDC_CMD_PRIORITY,        L"Change Render Priority",         NULL,      true,  GroupRender, 0 },
    { IDC_CMD_RUN_LIGHTNING,   L"Run Lightning",                  NULL,      true,  GroupRender, 0 },
    { IDC_CMD_STOP,            L"Stop Lightning",                 "STOP",    true,  GroupRender, 0 },
};
const int kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

const CommandDef* commandDef(int id)
{
    for (int i = 0; i < kCommandCount; ++i) if (kCommands[i].id == id) return &kCommands[i];
    return NULL;
}
// Whether the command opens a pop-up of nested commands (drawn with a double border).
bool hasNested(int id)
{
    for (int i = 0; i < kCommandCount; ++i) if (kCommands[i].parent == id) return true;
    return false;
}

// ---------------------------------------------------------------- helpers

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.rfind(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::string hostName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &len) ? narrow(name) : std::string("Thunderhead");
}

void logEvent(const std::string& text)
{
    g_events.write(text);
}

void logNode(const NodeRecord& n, const std::string& text)
{
    std::string key = n.name.empty() ? n.mac : n.name;
    std::map<std::string, Logger*>::iterator it = g_nodeLogs.find(key);
    if (it == g_nodeLogs.end()) {
        Logger* l = new Logger();
        l->open(g_dir + L"\\logs\\nodes", widen(key));
        it = g_nodeLogs.insert(std::make_pair(key, l)).first;
    }
    it->second->write(text);
}

// ---------------------------------------------------------------- sounds

// Thunderhead's sounds live in sounds\ beside it: the repository's
// assets\sounds, which the build copies there. A category plays its default
// file, "ThunderheadLightningRod <Category>.wav", unless the custom scheme is
// on and assigns another file of that folder to it.
std::wstring soundDir() { return g_dir + L"\\" + kSoundFolder; }

std::wstring soundNameForCategory(const wchar_t* category)
{
    if (g_store && g_store->settings.customScheme) {
        std::map<std::string, std::string>::const_iterator it = g_store->settings.sounds.find(narrow(category));
        if (it != g_store->settings.sounds.end() && !it->second.empty()) return widen(it->second);
    }
    return soundDefaultFile(category);
}

std::wstring soundPathForEvent(SoundEvent e) { return soundDir() + L"\\" + soundNameForCategory(kSoundEvents[e].category); }

// Plays the sound of one of Thunderhead's own events; nothing while
// Thunderhead Audio is off. A failure is one line in the event log.
// The sounds are queued and played one after another on a thread of their own,
// each to its end (kEventSoundMaxMs at most), so that a command sent to ten
// clients is heard ten times; soundPlay, which replaces a sound still playing,
// would merge such a burst into one tick. The file is chosen at queueing time,
// so a scheme change in Settings is read on the window thread only.
const DWORD kEventSoundMaxMs = 3000;
const size_t kEventSoundQueueMax = 200;
struct EventSoundQueue { CRITICAL_SECTION lock; HANDLE wake; HANDLE thread; std::deque<std::pair<SoundEvent, std::wstring> > items; bool started; };
EventSoundQueue g_eventSounds = { {0}, NULL, NULL, std::deque<std::pair<SoundEvent, std::wstring> >(), false };

DWORD WINAPI eventSoundThread(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_eventSounds.wake, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_eventSounds.lock);
            if (g_eventSounds.items.empty()) { LeaveCriticalSection(&g_eventSounds.lock); break; }
            std::pair<SoundEvent, std::wstring> item = g_eventSounds.items.front();
            g_eventSounds.items.pop_front();
            LeaveCriticalSection(&g_eventSounds.lock);
            std::string err;
            if (!soundPlayAndWait(item.second, kEventSoundMaxMs, err)) logEvent(format("sound %s not played: %s", kSoundEvents[item.first].key, err.c_str()));   // logEvent is thread-safe
        }
    }
}

void playEventSound(SoundEvent e)
{
    if (!g_store || !g_store->settings.thAudio) return;
    if (!g_eventSounds.started) {
        g_eventSounds.started = true;
        InitializeCriticalSection(&g_eventSounds.lock);
        g_eventSounds.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_eventSounds.thread = CreateThread(NULL, 0, eventSoundThread, NULL, 0, NULL);
    }
    if (!g_eventSounds.thread) {   // no thread: the old way, one sound replacing the last
        std::string err;
        if (!soundPlay(soundPathForEvent(e), err)) logEvent(format("sound %s not played: %s", kSoundEvents[e].key, err.c_str()));
        return;
    }
    EnterCriticalSection(&g_eventSounds.lock);
    if (g_eventSounds.items.size() < kEventSoundQueueMax) g_eventSounds.items.push_back(std::make_pair(e, soundPathForEvent(e)));
    LeaveCriticalSection(&g_eventSounds.lock);
    SetEvent(g_eventSounds.wake);
}

std::wstring durationText(long long seconds)
{
    if (seconds < 0) seconds = 0;
    long long d = seconds / 86400, h = (seconds / 3600) % 24, m = (seconds / 60) % 60, s = seconds % 60;
    wchar_t buf[64];
    if (d > 0) _snwprintf(buf, 63, L"%lldd %02lld:%02lld:%02lld", d, h, m, s);
    else _snwprintf(buf, 63, L"%02lld:%02lld:%02lld", h, m, s);
    buf[63] = L'\0';
    return buf;
}

// cpu_temp_c as sent, in both scales; empty means the node has no ACPI thermal zone.
std::wstring tempText(const std::string& celsius)
{
    if (celsius.empty()) return L"n/a";
    double c = atof(celsius.c_str());
    wchar_t buf[48];
    _snwprintf(buf, 47, L"%.1f \u00B0C / %.1f \u00B0F", c, c * 9 / 5 + 32);
    buf[47] = L'\0';
    return buf;
}

// The address a node is reached at: that of its connection, else the one its
// HELLO carried (an offline node keeps the last one seen).
std::string nodeIp(const NodeRecord& n)
{
    return n.ip.empty() ? n.hello.get("ip") : n.ip;
}

// mem_installed_mb from HELLO: blank from an agent that does not send it, n/a
// where the node's OS cannot supply it (XP), else the figure.
std::wstring memInstalledText(const Message& h)
{
    if (!h.has("mem_installed_mb")) return L"";
    return h.get("mem_installed_mb") == "0" ? std::wstring(L"n/a") : widen(h.get("mem_installed_mb"));
}

// mem_type from HELLO (agents from 0.2.17): blank when absent, n/a when the
// node's firmware lists no memory devices.
// HELLO lightningN_path / lightningN_version: absent (an older agent) shows
// blank, an empty path "not installed", else "version at path".
std::wstring lightningInstallText(const Message& h, const char* prefix)
{
    std::string pathKey = std::string(prefix) + "_path", verKey = std::string(prefix) + "_version";
    if (!h.has(pathKey)) return std::wstring();
    std::string path = h.get(pathKey), ver = h.get(verKey);
    if (path.empty()) return L"not installed";
    return widen(ver.empty() ? path : ver + " at " + path);
}

std::wstring memTypeText(const Message& h)
{
    if (!h.has("mem_type")) return L"";
    return h.get("mem_type").empty() ? std::wstring(L"n/a") : widen(h.get("mem_type"));
}

std::wstring columnText(const NodeRecord& n, int col)
{
    const Message& r = n.report;
    bool live = n.online && !r.verb.empty();
    switch (col) {
    case ColName:         return widen(n.name.empty() ? n.mac : n.name);
    case ColIp:           return widen(nodeIp(n));
    case ColMac:          return widen(n.mac);
    case ColStatus:       return n.updating ? L"Updating" : n.online ? L"Online"
                               : (n.lastAnnounceMs && tickMs() - n.lastAnnounceMs < 12000) ? L"Announcing" : L"Offline";
    case ColAgent:        return widen(n.hello.get("agent"));
    case ColRendering:    return !live ? L"" : (parseBool(r.get("rendering")) ? L"Yes" : L"No");
    case ColLightning:    return !live ? L"" : (r.get("lightning") == "running" ? L"Running" : L"Stopped");
    case ColLightningCpu: return !live ? L"" : widen(r.get("lightning_cpu_pct"));
    case ColCpuPct:       return !live ? L"" : widen(r.get("cpu_pct"));
    case ColCpuTemp:      return !live ? L"" : tempText(r.get("cpu_temp_c"));
    case ColMemFree:      return !live ? L"" : widen(r.get("mem_free_mb"));
    case ColMemUsable:    return widen(n.hello.get("mem_mb"));
    case ColMemInstalled: return memInstalledText(n.hello);
    case ColDiskFree:     return !live ? L"" : widen(r.get("disk_free_mb"));
    case ColUptime:       return !live ? L"" : durationText(atoll(r.get("uptime_s").c_str()));
    case ColLastReport: {
        if (!n.online || n.lastReportMs == 0) return L"";
        long long ago = (long long)((tickMs() - n.lastReportMs) / 1000);
        wchar_t buf[32];
        _snwprintf(buf, 31, L"%lld s ago", ago);
        buf[31] = L'\0';
        return buf;
    }
    case ColAutoShutdown: return L"";   // painted as a check box by onListCustomDraw
    case ColGroup:        return widen(n.group);
    case ColComment:      return widen(n.comment);
    }
    return L"";
}

int rowOf(const NodeRecord* n)
{
    LVFINDINFOW fi;
    memset(&fi, 0, sizeof(fi));
    fi.flags = LVFI_PARAM;
    fi.lParam = (LPARAM)n;
    return (int)SendMessageW(g_list, LVM_FINDITEMW, (WPARAM)-1, (LPARAM)&fi);
}

NodeRecord* nodeAt(int row)
{
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_PARAM;
    item.iItem = row;
    return SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&item) ? (NodeRecord*)item.lParam : NULL;
}

void setCell(HWND list, int row, int col, const std::wstring& text)
{
    // Only a changed cell is written. LVM_SETITEMTEXT repaints the cell at
    // once, erasing it to the list's background first, so a cell rewritten with
    // the same text every second flashed white.
    wchar_t have[512];
    LVITEMW get;
    memset(&get, 0, sizeof(get));
    get.iSubItem = col;
    get.pszText = have;
    get.cchTextMax = 512;
    int len = (int)SendMessageW(list, LVM_GETITEMTEXTW, (WPARAM)row, (LPARAM)&get);
    if (len >= 0 && len < 511 && text.compare(have) == 0) return;
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.iSubItem = col;
    item.pszText = (LPWSTR)text.c_str();
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
}

int g_sortCol = -1;                       // column the inventory is sorted by, -1 = arrival order
bool g_sortAsc = true;
void applySort();

void refreshRow(NodeRecord* n, int onlyColumn = -1)
{
    int row = rowOf(n);
    bool inserted = false;
    if (row < 0) {
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
        item.lParam = (LPARAM)n;
        std::wstring name = columnText(*n, ColName);
        item.pszText = (LPWSTR)name.c_str();
        row = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&item);
        if (row < 0) return;
        onlyColumn = -1;
        inserted = true;
    }
    for (int col = 0; col < ColCount; ++col) {
        if (onlyColumn >= 0 && col != onlyColumn) continue;
        if (g_edit && row == g_editRow && col == g_editCol) continue;   // being edited
        setCell(g_list, row, col, columnText(*n, col));
    }
    // The cells a text update repaints take the shade of that moment; when the
    // condition behind the shade has changed since the row was last painted,
    // the whole row is repainted so that every cell shows the same shade.
    int key = shadeKey(n);
    if (key != n->shadeKey) {
        n->shadeKey = key;
        SendMessageW(g_list, LVM_REDRAWITEMS, (WPARAM)row, (LPARAM)row);
    }
    if (inserted) applySort();   // a new row takes its place in the current order
}

bool isChecked(int row)
{
    UINT state = (UINT)SendMessageW(g_list, LVM_GETITEMSTATE, (WPARAM)row, LVIS_STATEIMAGEMASK);
    return ((state >> 12) - 1) == 1;
}

void setChecked(int row, bool on)
{
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.stateMask = LVIS_STATEIMAGEMASK;
    item.state = INDEXTOSTATEIMAGEMASK(on ? 2 : 1);
    SendMessageW(g_list, LVM_SETITEMSTATE, (WPARAM)row, (LPARAM)&item);
}

std::vector<NodeRecord*> checkedNodes()
{
    std::vector<NodeRecord*> out;
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < count; ++row)
        if (isChecked(row)) { NodeRecord* n = nodeAt(row); if (n) out.push_back(n); }
    return out;
}

// ---------------------------------------------------------------- column sorting

int compareVersions(const std::string& a, const std::string& b);

// Numeric key of a cell, or false when the column is textual or the cell is blank.
bool sortNumber(const NodeRecord& n, int col, long long& out)
{
    const Message& r = n.report;
    bool live = n.online && !r.verb.empty();
    const char* key = NULL;
    switch (col) {
    case ColLightningCpu: key = "lightning_cpu_pct"; break;
    case ColCpuPct:       key = "cpu_pct"; break;
    case ColMemFree:      key = "mem_free_mb"; break;
    case ColMemUsable:
    case ColMemInstalled: {
        // static values from HELLO, present whether the node is online or not
        std::string v = n.hello.get(col == ColMemUsable ? "mem_mb" : "mem_installed_mb");
        if (v.empty()) return false;
        out = atoll(v.c_str());
        return true;
    }
    case ColDiskFree:     key = "disk_free_mb"; break;
    case ColUptime:       key = "uptime_s"; break;
    case ColLastReport:
        if (!n.online || n.lastReportMs == 0) return false;
        out = (long long)(tickMs() - n.lastReportMs);
        return true;
    case ColAutoShutdown: out = n.autoShutdown ? 1 : 0; return true;
    case ColIp: {
        // a.b.c.d as one number, so that 42.32.22.107 sorts before 42.32.22.158
        std::string ip = nodeIp(n);
        long long key = 0;
        int parts = 0, digits = 0;
        unsigned value = 0;
        for (size_t i = 0; i <= ip.size(); ++i) {
            char ch = i < ip.size() ? ip[i] : '.';
            if (ch >= '0' && ch <= '9') { value = value * 10 + (unsigned)(ch - '0'); ++digits; if (value > 255) return false; }
            else if (ch == '.' && digits > 0 && parts < 4) { key = (key << 8) | value; value = 0; digits = 0; ++parts; }
            else return false;
        }
        if (parts != 4) return false;
        out = key;
        return true;
    }
    case ColCpuTemp:
        if (!live || r.get("cpu_temp_c").empty()) return false;
        out = (long long)(atof(r.get("cpu_temp_c").c_str()) * 10 + 0.5);
        return true;
    default: return false;
    }
    if (!live || !r.has(key)) return false;
    out = atoll(r.get(key).c_str());
    return true;
}

// Ascending comparison of two rows by column: blank cells after filled ones,
// numbers by value, the Agent column by version, text without regard to case,
// ties by name.
int compareRows(const NodeRecord& a, const NodeRecord& b, int col)
{
    int c = 0;
    if (col == ColAgent) {
        std::string va = a.hello.get("agent"), vb = b.hello.get("agent");
        if (va.empty() != vb.empty()) c = va.empty() ? 1 : -1;
        else c = compareVersions(va, vb);
    } else {
        long long na = 0, nb = 0;
        bool ha = sortNumber(a, col, na), hb = sortNumber(b, col, nb);
        if (ha || hb) {
            if (ha != hb) c = ha ? -1 : 1;
            else c = na < nb ? -1 : (na > nb ? 1 : 0);
        } else {
            std::wstring ta = columnText(a, col), tb = columnText(b, col);
            if (ta.empty() != tb.empty()) c = ta.empty() ? 1 : -1;
            else c = _wcsicmp(ta.c_str(), tb.c_str());
        }
    }
    if (c == 0 && col != ColName) c = _wcsicmp(columnText(a, ColName).c_str(), columnText(b, ColName).c_str());
    return c;
}

int CALLBACK sortCallback(LPARAM a, LPARAM b, LPARAM)
{
    int c = compareRows(*(const NodeRecord*)a, *(const NodeRecord*)b, g_sortCol);
    return g_sortAsc ? c : -c;
}

void showSortArrow()
{
    HWND header = (HWND)SendMessageW(g_list, LVM_GETHEADER, 0, 0);
    if (!header) return;
    for (int i = 0; i < ColCount; ++i) {
        HDITEMW hi;
        memset(&hi, 0, sizeof(hi));
        hi.mask = HDI_FORMAT;
        if (!SendMessageW(header, HDM_GETITEMW, (WPARAM)i, (LPARAM)&hi)) continue;
        hi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sortCol) hi.fmt |= g_sortAsc ? HDF_SORTUP : HDF_SORTDOWN;
        SendMessageW(header, HDM_SETITEMW, (WPARAM)i, (LPARAM)&hi);
    }
}

void applySort()
{
    if (g_sortCol < 0) return;
    SendMessageW(g_list, LVM_SORTITEMS, 0, (LPARAM)sortCallback);
}

// A click on a column header sorts by that column, ascending first and then
// alternating with each further click on the same header.
void sortByColumn(int col)
{
    if (col < 0 || col >= ColCount) return;
    if (col == g_sortCol) g_sortAsc = !g_sortAsc;
    else { g_sortCol = col; g_sortAsc = true; }
    applySort();
    showSortArrow();
}

std::wstring g_statusExtra;
std::wstring g_statusText;    // what the status bar shows; the owner-drawn text in Dark Mode

// Without an argument the last event text stays; with one it is replaced.
void updateStatusBar(const wchar_t* extra = NULL)
{
    if (extra) g_statusExtra = extra;
    int total = 0, online = 0, rendering = 0, checked = 0;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        ++total;
        if (nodes[i]->online) ++online;
        if (nodes[i]->online && parseBool(nodes[i]->report.get("rendering"))) ++rendering;
    }
    int rows = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < rows; ++row) if (isChecked(row)) ++checked;
    wchar_t buf[256];
    _snwprintf(buf, 255, L"%d clients, %d online, %d rendering, %d checked  |  interval %d s  |  UDP %d, TCP %d  %s",
               total, online, rendering, checked, g_store->settings.intervalSec, (int)kDiscoveryPort,
               (int)g_server->tcpPort(), g_statusExtra.c_str());
    buf[255] = L'\0';
    g_statusText = buf;
    if (theme().dark) SendMessageW(g_status, SB_SETTEXTW, SBT_OWNERDRAW, (LPARAM)g_statusText.c_str());
    else SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
}

void drawStatusText(const DRAWITEMSTRUCT* di)
{
    const Theme& t = theme();
    RECT rc = di->rcItem;
    FillRect(di->hDC, &rc, themeBrush(t.paneBack));
    rc.left += 2;
    HGDIOBJ oldFont = SelectObject(di->hDC, g_font);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, t.text);
    DrawTextW(di->hDC, (const wchar_t*)di->itemData, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(di->hDC, oldFont);
}

// ---------------------------------------------------------------- static pane

void showStaticPane(NodeRecord* n);

// Boot on Alarm: the time of day beside the flag, kept in nodes.txt as
// boot_alarm=HH:MM (24-hour); the picker shows it with AM and PM. A node that
// has never had one shows 06:00 AM, which is saved only when the picker is
// changed. Like the flags, the value is stored and nothing acts on it yet.
bool parseBootAlarm(const std::string& text, int& hour, int& minute)
{
    if (text.size() != 5 || text[2] != ':') return false;
    hour = atoi(text.substr(0, 2).c_str());
    minute = atoi(text.substr(3, 2).c_str());
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

std::wstring bootAlarmText(const std::string& stored)
{
    int hour, minute;
    if (!parseBootAlarm(stored, hour, minute)) return widen(stored);
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    return widen(format("%02d:%02d %s", h12, minute, hour < 12 ? "AM" : "PM"));
}

void showBootAlarm(const NodeRecord* n)
{
    int hour = 6, minute = 0;
    if (!n->bootAlarm.empty() && !parseBootAlarm(n->bootAlarm, hour, minute)) { hour = 6; minute = 0; }
    SYSTEMTIME st;
    GetLocalTime(&st);
    st.wHour = (WORD)hour;
    st.wMinute = (WORD)minute;
    st.wSecond = 0;
    st.wMilliseconds = 0;
    SendMessageW(g_bootAlarm, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
}

// The picker changed: save the time for the node shown.
void bootAlarmChanged(const NMDATETIMECHANGE* c)
{
    NodeRecord* n = g_staticNode;
    if (!n || c->dwFlags != GDT_VALID) return;
    std::string value = format("%02d:%02d", c->st.wHour, c->st.wMinute);
    if (value == n->bootAlarm) return;
    n->bootAlarm = value;
    g_store->save();
    logEvent(format("%s boot_alarm=%s", n->name.c_str(), value.c_str()));
    if (g_staticNode == n) showStaticPane(n);
}

void showStaticPane(NodeRecord* n)
{
    g_staticNode = n;
    if (!n) { ShowWindow(g_staticPane, SW_HIDE); return; }
    const Message& h = n->hello;
    const Message& r = n->report;
    std::wstring title = L"Client details: " + widen(n->name.empty() ? n->mac : n->name) +
                         (n->online ? L"" : L"  (offline, last known values)");
    SetWindowTextW(g_staticTitle, title.c_str());
    CheckDlgButton(g_staticPane, IDC_FLAG_WOL, n->wol ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_BOOT, n->boot ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_AUTOLOGIN, n->autoLogin ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_AUTOROD, n->autoRod ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_AUTOLIGHTNING, n->autoLightning ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_RENDERHOST, n->renderHost ? BST_CHECKED : BST_UNCHECKED);
    showBootAlarm(n);

    // Every field there is: the stored record, the connection, the HELLO, the
    // latest report (kept for an offline node), then any field of HELLO or
    // REPORT that has no row of its own, as a newer agent may send one.
    std::vector<std::pair<std::wstring, std::wstring> > rows;
    auto add = [&rows](const wchar_t* item, const std::wstring& value) { rows.push_back(std::make_pair(std::wstring(item), value)); };
    add(L"Name", widen(n->name));
    if (!h.get("name").empty() && h.get("name") != n->name) add(L"Reported name", widen(h.get("name")));
    add(L"MAC", widen(n->mac));
    add(L"IP", widen(nodeIp(*n)));
    // Every adapter of the node with a working address, as an agent from 0.5.5
    // lists them: address/prefix, Windows network category, announced or not,
    // adapter, MAC, and which is preferred and the identity (the IP above).
    for (int k = 1; k <= h.getInt("net_count", 0); ++k) {
        std::wstring label = widen(format("Network %d", k));
        add(label.c_str(), widen(h.get(format("net%d", k))));
    }
    add(L"Status", columnText(*n, ColStatus));
    add(L"Connected for", n->online ? durationText((long long)((tickMs() - n->onlineSinceMs) / 1000)) : std::wstring());
    add(L"Last report", columnText(*n, ColLastReport));
    add(L"Group", widen(n->group));
    add(L"Comment", widen(n->comment));
    add(L"Auto Shutdown", n->autoShutdown ? L"Yes" : isRenderHost(n) ? L"No (never for a Net Render Host)" : L"No");
    add(L"Boot on Alarm time", n->bootAlarm.empty() ? std::wstring(L"not set") : bootAlarmText(n->bootAlarm));
    add(L"Agent", widen(h.get("agent")));
    add(L"Shared folder", widen(shareStateText(n)));
    add(L"Authentication", widen(authText(n)));
    add(L"Net Render Host", renderHostText(n));
    add(L"RENDER.BIN", renderBinText(n));
    add(L"OS", widen(h.get("os")));
    add(L"CPU", widen(h.get("cpu")));
    add(L"MHz", widen(h.get("cpu_mhz")));
    add(L"Cores", widen(h.get("cores")));
    add(L"Mem usable MB", widen(h.get("mem_mb")));
    add(L"Mem installed MB", memInstalledText(h));
    add(L"Memory type", memTypeText(h));
    add(L"C: MB", widen(h.get("disk_total_mb")));
    add(L"Board", widen(h.get("board")));
    add(L"Chipset", widen(h.get("chipset")));
    add(L"GPU", widen(h.get("gpu")));
    add(L"VRAM MB", widen(h.get("vram_mb")));
    add(L"Lightning 3 (B7 & 7 Pro)", lightningInstallText(h, "lightning3"));
    add(L"Lightning 2 (B6.x)", lightningInstallText(h, "lightning2"));
    add(L"Bryce 7 & 7 Pro", lightningInstallText(h, "bryce3"));
    add(L"Bryce 6.x", lightningInstallText(h, "bryce2"));
    add(L"Bryce 5.x", lightningInstallText(h, "bryce1"));
    add(L"Bryce 3D", !r.has("bryce") ? std::wstring() : (r.get("bryce") == "running" ? L"Running, CPU " + widen(r.get("bryce_cpu_pct")) + L" %" : L"Stopped"));
    if (h.has("relay")) {
        add(L"Relay Node", isRelay(n) ? L"Yes" : L"No");
        if (isRelay(n)) {
            NodeRecord* target = nodeOfRelayTarget(n);
            add(L"Relay To", widen(relayTarget(n) + (target ? " (" + (validIpText(relayTarget(n)) ? target->name : target->ip) + ")" : std::string()) +
                                   (n->relayProbe.empty() ? std::string() : "; its share " + n->relayProbe)));
        }
    }
    if (NodeRecord* via = relayFor(n)) add(L"Reached through relay", widen(via->name + " (" + via->ip + ")"));
    bool reported = !r.verb.empty();
    add(L"Rendering", !r.has("rendering") ? std::wstring() : (parseBool(r.get("rendering")) ? L"Yes" : L"No"));
    add(L"Lightning", !r.has("lightning") ? std::wstring() : (r.get("lightning") == "running" ? L"Running" : L"Stopped"));
    add(L"Lightning CPU %", widen(r.get("lightning_cpu_pct")));
    add(L"CPU %", widen(r.get("cpu_pct")));
    add(L"CPU temp", !reported ? std::wstring() : tempText(r.get("cpu_temp_c")));
    add(L"Mem free MB", widen(r.get("mem_free_mb")));
    add(L"C: free MB", widen(r.get("disk_free_mb")));
    add(L"Uptime", !r.has("uptime_s") ? std::wstring() : durationText(atoll(r.get("uptime_s").c_str())));
    add(L"Clock at last report", widen(r.get("clock")));
    if (n->haveClockOffset) {
        long long ms = n->clockOffsetMs, s = ms >= 0 ? (ms + 500) / 1000 : -((-ms + 500) / 1000);
        add(L"Clock offset", widen(format("%lld s %s this host", s < 0 ? -s : s, s < 0 ? "behind" : "ahead of")));
    } else {
        add(L"Clock offset", std::wstring());
    }
    static const char* const knownHello[] = { "agent", "os", "cpu", "cpu_mhz", "cores", "mem_mb", "mem_installed_mb", "mem_type", "disk_total_mb", "board",
                                              "chipset", "gpu", "vram_mb", "ip", "mac", "name", "lightning3_path", "lightning3_version",
                                              "lightning2_path", "lightning2_version", "relay", "relay_ip", "render_host", "render_bin", "render_bin_exists", "bryce3_path", "bryce3_version", "bryce2_path", "bryce2_version", "bryce1_path", "bryce1_version" };
    static const char* const knownReport[] = { "cpu_pct", "cpu_temp_c", "disk_free_mb", "lightning", "lightning_cpu_pct",
                                               "mem_free_mb", "rendering", "uptime_s", "clock", "utc_ms", "render_host", "render_bin", "render_bin_exists", "bryce", "bryce_cpu_pct" };
    for (std::map<std::string, std::string>::const_iterator it = h.fields.begin(); it != h.fields.end(); ++it) {
        bool known = false;
        for (size_t k = 0; k < sizeof(knownHello) / sizeof(knownHello[0]); ++k) if (it->first == knownHello[k]) known = true;
        if (it->first.compare(0, 3, "net") == 0) known = true;   // net_count and net1.. have their rows above
        if (!known) add((L"HELLO " + widen(it->first)).c_str(), widen(it->second));
    }
    for (std::map<std::string, std::string>::const_iterator it = r.fields.begin(); it != r.fields.end(); ++it) {
        bool known = false;
        for (size_t k = 0; k < sizeof(knownReport) / sizeof(knownReport[0]); ++k) if (it->first == knownReport[k]) known = true;
        if (!known) add((L"REPORT " + widen(it->first)).c_str(), widen(it->second));
    }
    const int count = (int)rows.size();
    int have = (int)SendMessageW(g_staticList, LVM_GETITEMCOUNT, 0, 0);
    while (have > count) SendMessageW(g_staticList, LVM_DELETEITEM, (WPARAM)--have, 0);
    for (int i = 0; i < count; ++i) {
        if (i >= have) {
            LVITEMW item;
            memset(&item, 0, sizeof(item));
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = (LPWSTR)rows[i].first.c_str();
            SendMessageW(g_staticList, LVM_INSERTITEMW, 0, (LPARAM)&item);
        } else {
            setCell(g_staticList, i, 0, rows[i].first);
        }
        setCell(g_staticList, i, 1, rows[i].second);
    }
    ShowWindow(g_staticPane, SW_SHOW);
}

void flagClicked(int id)
{
    NodeRecord* n = g_staticNode;
    if (!n) return;
    bool on = IsDlgButtonChecked(g_staticPane, id) == BST_CHECKED;
    const char* name = "";
    switch (id) {
    case IDC_FLAG_WOL:           n->wol = on;           name = "wol"; break;
    case IDC_FLAG_BOOT:          n->boot = on;          name = "boot"; break;
    case IDC_FLAG_AUTOLOGIN:     n->autoLogin = on;     name = "autologin"; break;
    case IDC_FLAG_AUTOROD:       n->autoRod = on;       name = "autostart_rod"; break;
    case IDC_FLAG_AUTOLIGHTNING: n->autoLightning = on; name = "autostart_lightning"; break;
    case IDC_FLAG_RENDERHOST:    n->renderHost = on;    name = "render_host"; break;
    default: return;
    }
    g_store->save();
    logEvent(format("%s %s=%s", n->name.c_str(), name, boolText(on)));
    if (id == IDC_FLAG_RENDERHOST) {
        if (clearAutoShutdownOfHost(n)) showStaticPane(n);
        refreshRow(n);   // the row's tint and its Auto Shutdown cell follow the flag
    }
}

// ---------------------------------------------------------------- Bryce 3D Network Render Host (B3NDHR)
// One client of the farm is the Bryce 3D Network Render Host, the machine that
// runs the full Bryce and hands the frames out. Three things say so: the Info
// Flag here (Thunderhead's record, kept in nodes.txt), the agent's own at-start
// setting (its Settings dialog, kept in its settings.txt), and the agent's
// report of that setting in HELLO and REPORT (render_host=). Toggle B3D Net
// Render Host flips the flag of every checked client and tells each online
// agent from 0.4.2 to set itself the same way; the row is tinted while either
// the flag or the report says so.
const char* const kRenderHostAgent = "0.4.2";

bool isRenderHost(const NodeRecord* n)
{
    if (!n) return false;
    if (n->renderHost) return true;
    const std::string& v = n->report.has("render_host") ? n->report.get("render_host") : n->hello.get("render_host");
    return parseBool(v);
}

// A Render Host is never shut down automatically: it holds RENDER.BIN and the
// clients' frames land there until the last one. Whatever makes a node a
// Render Host (the Info Flag, Toggle B3D Net Render Host, or the agent's own
// setting as HELLO or REPORT brings it) clears its Auto Shutdown flag, and the
// Auto Shutdown cell of a Render Host refuses the click (toggleAutoShutdown).
// Returns true when the flag was set and has been cleared.
bool clearAutoShutdownOfHost(NodeRecord* n)
{
    if (!n || !n->autoShutdown || !isRenderHost(n)) return false;
    n->autoShutdown = false;
    g_store->save();
    logEvent(format("%s auto_shutdown=no (cleared: a Net Render Host is never shut down automatically)", n->name.c_str()));
    return true;
}

// The RENDER.BIN folder the agent reports (REPORT first, then HELLO); empty from agents before 0.4.5.
std::string renderBinOf(const NodeRecord* n)
{
    return n->report.has("render_bin") ? n->report.get("render_bin") : n->hello.get("render_bin");
}

std::wstring renderBinText(const NodeRecord* n)
{
    std::string bin = renderBinOf(n);
    if (bin.empty()) return n->hello.get("agent").empty() ? std::wstring() : std::wstring(L"not reported (agent before 0.4.5)");
    std::string ex = n->report.has("render_bin_exists") ? n->report.get("render_bin_exists") : n->hello.get("render_bin_exists");
    return widen(bin + (parseBool(ex) ? "  (exists)" : "  (missing)"));
}

// The RENDER.BIN dialog of Toggle B3D Net Render Host: the folder as the agent
// reports it (or the installation default), editable; OK sends it with the
// toggle, Skip leaves this client untouched.
struct RenderBinDlgData { std::wstring info, path; };

INT_PTR CALLBACK renderBinDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    RenderBinDlgData* d = (RenderBinDlgData*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    switch (msg) {
    case WM_INITDIALOG:
        d = (RenderBinDlgData*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_RBIN_INFO, d->info.c_str());
        SetDlgItemTextW(dlg, IDC_RBIN_PATH, d->path.c_str());
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK && d) {
            wchar_t buf[1024];
            GetDlgItemTextW(dlg, IDC_RBIN_PATH, buf, 1024);
            std::wstring path = trim(std::wstring(buf));
            if (path.size() < 3 || path[1] != L':' || path[2] != L'\\') {
                MessageBoxW(dlg, L"Enter the folder as a full path on the host, for example C:\\LightningRod\\RENDER.BIN.", L"RENDER.BIN", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            d->path = path;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

std::wstring renderHostText(const NodeRecord* n)
{
    if (n->hello.get("agent").empty()) return std::wstring();
    bool reported = n->report.has("render_host") || n->hello.has("render_host");
    if (!reported) return L"not reported (agent before 0.4.2)";
    bool on = parseBool(n->report.has("render_host") ? n->report.get("render_host") : n->hello.get("render_host"));
    std::wstring text = on ? L"Yes" : L"No";
    if (on != n->renderHost) text += n->renderHost ? L" (the Info Flag here says Yes)" : L" (the Info Flag here says No)";
    return text;
}

void toggleRenderHost()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    int flagged = 0, told = 0, old = 0, skipped = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        std::wstring bin;
        if (!n->renderHost) {
            // Switching on: the RENDER.BIN folder, as the agent reports it or as the
            // installation lays it out, is confirmed or changed first.
            RenderBinDlgData d;
            std::string reported = renderBinOf(n);
            d.path = reported.empty() ? L"C:\\LightningRod\\RENDER.BIN" : widen(reported);
            d.info = widen(n->name) + L" becomes a Bryce 3D Network Render Host. Confirm the folder where it writes the frames: the default is RENDER.BIN in its "
                     L"shared Lightning Rod folder" + (reported.empty() ? L" (the agent has not reported its folder yet)" : L"") + L".";
            if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_RENDERBIN), g_main, renderBinDlgProc, (LPARAM)&d) != IDOK) {
                ++skipped;
                logEvent(n->name + ": Net Render Host toggle skipped");
                continue;
            }
            bin = d.path;
        }
        n->renderHost = !n->renderHost;
        logEvent(format("%s render_host=%s (Info Flag)%s%s", n->name.c_str(), boolText(n->renderHost), bin.empty() ? "" : ", RENDER.BIN ", narrow(bin).c_str()));
        if (n->renderHost) { ++flagged; clearAutoShutdownOfHost(n); }
        if (n->online) {
            if (compareVersions(n->hello.get("agent"), kRenderHostAgent) < 0) {
                ++old;
                logEvent(format("RENDERHOST not sent to %s: agent %s is older than %s, update it first", n->name.c_str(),
                                n->hello.get("agent").c_str(), kRenderHostAgent));
            } else {
                Message m("RENDERHOST");
                m.set("set", std::string(boolText(n->renderHost)));
                if (!bin.empty()) m.set("render_bin", narrow(bin));
                if (sendToNode(n, m, m.oneLine())) ++told;
            }
        }
        refreshRow(n);
        if (g_staticNode == n) showStaticPane(n);
    }
    g_store->save();
    std::string status = format("|  Net Render Host toggled on %d checked: %d now flagged, %d agent(s) told", (int)targets.size(), flagged, told);
    if (old) status += format(", %d skipped: agent older than %s", old, kRenderHostAgent);
    if (skipped) status += format(", %d skipped at the RENDER.BIN dialog", skipped);
    updateStatusBar(widen(status).c_str());
}

// The Auto Shutdown cell of a row was clicked: flip the flag, save it and repaint the row.
void toggleAutoShutdown(int row)
{
    NodeRecord* n = nodeAt(row);
    if (!n) return;
    if (!n->autoShutdown && isRenderHost(n)) {
        updateStatusBar((L"|  " + widen(n->name) + L" is a Net Render Host: it is never shut down automatically").c_str());
        logEvent(n->name + ": Auto Shutdown refused, a Net Render Host is never shut down automatically");
        return;
    }
    n->autoShutdown = !n->autoShutdown;
    g_store->save();
    logEvent(format("%s auto_shutdown=%s", n->name.c_str(), boolText(n->autoShutdown)));
    SendMessageW(g_list, LVM_REDRAWITEMS, (WPARAM)row, (LPARAM)row);
}

// ---------------------------------------------------------------- in-place editing

void endEdit(bool commit)
{
    if (!g_edit || g_editClosing) return;
    g_editClosing = true;
    NodeRecord* n = nodeAt(g_editRow);
    if (commit && n) {
        wchar_t buf[2048];
        GetWindowTextW(g_edit, buf, 2048);
        std::string value = narrow(trim(std::wstring(buf)));
        std::string& field = g_editCol == ColGroup ? n->group : n->comment;
        if (value != field) {
            field = value;
            g_store->save();
            logEvent(format("%s %s=%s", n->name.c_str(), g_editCol == ColGroup ? "group" : "comment", value.c_str()));
        }
    }
    HWND edit = g_edit;
    g_edit = NULL;
    DestroyWindow(edit);
    if (n) setCell(g_list, g_editRow, g_editCol, columnText(*n, g_editCol));
    g_editRow = g_editCol = -1;
    g_editClosing = false;
}

LRESULT CALLBACK editSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { endEdit(true); return 0; }
        if (wp == VK_ESCAPE) { endEdit(false); return 0; }
        break;
    case WM_CHAR:
        if (wp == VK_RETURN || wp == VK_ESCAPE) return 0;   // no beep
        break;
    case WM_KILLFOCUS: endEdit(true); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void beginEdit(int row, int col)
{
    endEdit(true);
    NodeRecord* n = nodeAt(row);
    if (!n || (col != ColGroup && col != ColComment)) return;
    RECT rc;
    rc.left = LVIR_BOUNDS;
    rc.top = col;
    if (!SendMessageW(g_list, LVM_GETSUBITEMRECT, (WPARAM)row, (LPARAM)&rc)) return;
    g_editRow = row;
    g_editCol = col;
    std::wstring text = columnText(*n, col);
    g_edit = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, g_list, NULL, g_inst, NULL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_edit, EM_SETSEL, 0, -1);
    SetWindowSubclass(g_edit, editSubclass, 1, 0);
    SetFocus(g_edit);
}

// ---------------------------------------------------------------- agent update

// The file to push: settings agent_file, or LightningRod.exe beside Thunderhead.exe.
std::wstring pushFilePath()
{
    if (!g_store->settings.agentFile.empty()) return widen(g_store->settings.agentFile);
    return g_dir + L"\\LightningRod.exe";
}

// A Windows XP node (HELLO os version 5.x) cannot run the standard build of the
// agent; it takes the XP build, XP\LightningRod.exe under Thunderhead's folder
// (the build-xp tree, toolset v141_xp). Empty when that file is absent.
bool isWindowsXp(const NodeRecord* n) { return n && n->hello.get("os").find("(5.") != std::string::npos; }

std::wstring pushFilePathFor(const NodeRecord* n)
{
    if (!isWindowsXp(n)) return pushFilePath();
    std::wstring xp = g_dir + L"\\XP\\LightningRod.exe";
    return GetFileAttributesW(xp.c_str()) == INVALID_FILE_ATTRIBUTES ? std::wstring() : xp;
}

// "major.minor.patch" from the file's version resource; empty if absent.
std::string fileVersion(const std::wstring& path)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return std::string();
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, &buf[0])) return std::string();
    VS_FIXEDFILEINFO* info = NULL;
    UINT len = 0;
    if (!VerQueryValueW(&buf[0], L"\\", (LPVOID*)&info, &len) || !info) return std::string();
    return format("%u.%u.%u", (unsigned)HIWORD(info->dwFileVersionMS), (unsigned)LOWORD(info->dwFileVersionMS),
                  (unsigned)HIWORD(info->dwFileVersionLS));
}

// Numeric comparison of dotted versions: <0, 0, >0.
int compareVersions(const std::string& a, const std::string& b)
{
    std::vector<std::string> pa = split(a, '.'), pb = split(b, '.');
    for (size_t i = 0; i < 3; ++i) {
        int x = i < pa.size() ? atoi(pa[i].c_str()) : 0;
        int y = i < pb.size() ? atoi(pb[i].c_str()) : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

// Rereads the version of the agent file to push, which Outdated is measured
// against, so that a Select Group of Outdated LR sees a file put there since start.
bool refreshPushVersion()
{
    std::wstring file = pushFilePath();
    std::string version = fileVersion(file);
    if (version.empty()) {
        MessageBoxW(g_main, (L"No agent file with a version resource at\n" + file +
                    L"\n\nPut the LightningRod.exe to push beside Thunderhead.exe, or set agent_file in Thunderhead.settings.txt.").c_str(),
                    L"Thunderhead", MB_OK | MB_ICONWARNING);
        return false;
    }
    g_pushVersion = version;
    return true;
}

// ---------------------------------------------------------------- SMB1 relay

// A Windows XP client's share speaks SMB1, which this host no longer does. An
// agent from kRelayAgent on a Windows 7 node whose Settings say Act as SMB1
// Relay for that client's address announces relay=yes and relay_ip= in HELLO.
// Every share operation of that client (update staging with the sounds folder,
// Copy Files or Folder, View Remote Log) then goes through it: the files are
// written to relay\<job>\ under the relay node's share, which this host can
// reach, and RELAY op=push has the relay copy them on over SMB1; RELAY op=pull
// fetches a file the other way. The client's commands still go to its own
// session, which needs no file sharing.
const char* const kRelayAgent = "0.3.0";

bool isRelay(const NodeRecord* n) { return n && parseBool(n->hello.get("relay")); }
// The relay's destination as its Settings hold it: the client's address, or
// (agents from 0.5.7) its computer name, which survives a changed DHCP lease.
std::string relayTarget(const NodeRecord* n) { return n ? n->hello.get("relay_ip") : std::string(); }

NodeRecord* nodeAtIp(const std::string& ip)
{
    if (ip.empty()) return NULL;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->ip == ip) return nodes[i];
    return NULL;
}

// True when the relay target text names node n: its current address, or its
// name without regard to case.
bool relayTargetIs(const std::string& target, const NodeRecord* n)
{
    if (target.empty() || !n) return false;
    if (!n->ip.empty() && target == n->ip) return true;
    return !n->name.empty() && _stricmp(target.c_str(), n->name.c_str()) == 0;
}

// The node a relay's destination names, by address or by name; NULL when none.
NodeRecord* nodeOfRelayTarget(const NodeRecord* relay)
{
    std::string target = relayTarget(relay);
    if (target.empty()) return NULL;
    if (NodeRecord* byIp = nodeAtIp(target)) return byIp;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i] != relay && relayTargetIs(target, nodes[i])) return nodes[i];
    return NULL;
}

// The online relay set to n's address or name, or NULL.
NodeRecord* relayFor(const NodeRecord* n)
{
    if (!n || (n->ip.empty() && n->name.empty())) return NULL;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* r = nodes[i];
        if (r != n && r->online && isRelay(r) && relayTargetIs(relayTarget(r), n) && compareVersions(r->hello.get("agent"), kRelayAgent) >= 0) return r;
    }
    return NULL;
}

// A worker thread's wait for the ACK of one RELAY job, keyed by job id; the
// main thread hands the ACK over in relayAckDelivered.
struct RelayWait { HANDLE done; Message ack; bool got; };
CRITICAL_SECTION g_relayLock;
std::map<std::string, RelayWait*> g_relayWaits;
unsigned g_relayJobSeq = 0;

std::string newRelayJob()
{
    EnterCriticalSection(&g_relayLock);
    std::string job = format("th%lu-%u", (unsigned long)GetCurrentProcessId(), ++g_relayJobSeq);
    LeaveCriticalSection(&g_relayLock);
    return job;
}

bool relayAckDelivered(const Message& ack)
{
    EnterCriticalSection(&g_relayLock);
    std::map<std::string, RelayWait*>::iterator it = g_relayWaits.find(ack.get("job"));
    bool found = it != g_relayWaits.end();
    if (found) { it->second->ack = ack; it->second->got = true; SetEvent(it->second->done); }
    LeaveCriticalSection(&g_relayLock);
    return found;
}

// Worker threads: sends m, which carries job=, to the relay's connection and
// waits for the ACK of that job, calling pump every 100 ms when one is given
// (the copy progress window). False with err on refusal, failure or timeout.
bool relayRequest(int relayConn, const Message& m, DWORD timeoutMs, Message& ack, std::string& err, void (*pump)(void*) = NULL, void* arg = NULL)
{
    std::string job = m.get("job");
    RelayWait* w = new RelayWait();
    w->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->got = false;
    EnterCriticalSection(&g_relayLock);
    g_relayWaits[job] = w;
    LeaveCriticalSection(&g_relayLock);
    bool sent = g_server->send(relayConn, m);
    playEventSound(sent ? SoundThSend : SoundThNoReply);   // soundPlay is safe from this worker thread
    DWORD start = GetTickCount();
    while (sent) {
        if (WaitForSingleObject(w->done, pump ? 100 : timeoutMs) == WAIT_OBJECT_0) break;
        if (GetTickCount() - start >= timeoutMs) break;
        if (pump) pump(arg);
    }
    EnterCriticalSection(&g_relayLock);
    g_relayWaits.erase(job);
    bool got = w->got;
    ack = w->ack;
    LeaveCriticalSection(&g_relayLock);
    CloseHandle(w->done);
    delete w;
    if (!sent) { err = "the relay's connection is gone"; return false; }
    if (!got) { err = format("no answer from the relay within %u s", (unsigned)(timeoutMs / 1000)); playEventSound(SoundThNoReply); return false; }
    if (!parseBool(ack.get("ok"))) { err = ack.get("error"); return false; }
    return true;
}

Message relayMessage(const char* op, const std::string& job, const std::string& destIp, const std::wstring& share)
{
    Message m("RELAY");
    m.set("op", std::string(op));
    m.set("job", job);
    m.set("dest", destIp);
    m.set("share", narrow(share));
    return m;
}

// relay\<job>\ under the relay node's share: where the files for a relayed client are staged.
std::wstring relayStageDir(const std::string& relayIp, const std::wstring& share, const std::string& job)
{
    return L"\\\\" + widen(relayIp) + L"\\" + share + L"\\relay\\" + widen(job) + L"\\";
}

// Creates the folders of a UNC path (ending with a backslash) below its share.
bool makeDirs(const std::wstring& path)
{
    size_t start = 0;
    if (path.compare(0, 2, L"\\\\") == 0) {
        size_t s1 = path.find(L'\\', 2);
        size_t s2 = s1 == std::wstring::npos ? std::wstring::npos : path.find(L'\\', s1 + 1);
        if (s2 == std::wstring::npos) return false;
        start = s2 + 1;
    }
    for (size_t i = start; i < path.size(); ++i) {
        if (path[i] != L'\\') continue;
        std::wstring part = path.substr(0, i);
        if (!CreateDirectoryW(part.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return true;
}

// Deletes a folder (ending with a backslash) with everything in it.
void removeTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(dir + fd.cFileName + L"\\");
            else DeleteFileW((dir + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring bare = dir;
    while (!bare.empty() && bare[bare.size() - 1] == L'\\') bare.erase(bare.size() - 1);
    RemoveDirectoryW(bare.c_str());
}

static std::string byteText(ULONGLONG bytes);

// ---------------------------------------------------------------- agent update

struct UpdateJob {
    std::string name; std::string ip; std::wstring source; std::wstring share; std::wstring dir; std::wstring sounds;
    bool viaRelay; int relayConn; std::string relayName, relayIp;   // the client's share is reached through a relay
    std::string key;                                                 // farm.key to place beside the agent
    std::wstring root;                                               // the share root the gate verified (see shareRootOf); empty when none
};
struct UpdateResult {
    std::string name; int phase; bool ok; std::string text; std::wstring dir;
    bool needCredentials;            // a share refused the credentials Windows offered
    std::wstring credDir;            // that share
};

// \\NAME\share is tried after \\IP\share, but only when NAME resolves to that
// address, because freshly cloned nodes share a name. Some nodes accept the
// host's credentials only under their name.
bool nameResolvesTo(const std::string& name, const std::string& ip)
{
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    addrinfo* res = NULL;
    if (getaddrinfo(name.c_str(), NULL, &hints, &res) != 0) return false;
    bool match = false;
    for (addrinfo* a = res; a; a = a->ai_next)
        if (a->ai_addr && ipText(*(const sockaddr_in*)a->ai_addr) == ip) match = true;
    freeaddrinfo(res);
    return match;
}

// Whether an error from a share means that it refused the credentials Windows
// offered, which the standard credential prompt is for.
bool isCredentialError(int err)
{
    return err == ERROR_ACCESS_DENIED || err == ERROR_LOGON_FAILURE || err == ERROR_INVALID_PASSWORD ||
           err == ERROR_BAD_USERNAME || err == ERROR_ACCOUNT_RESTRICTION || err == ERROR_LOGON_TYPE_NOT_GRANTED ||
           err == ERROR_SESSION_CREDENTIAL_CONFLICT || err == ERROR_NO_SUCH_USER;
}

// Shows the standard Windows credential prompt for \\NAME\share, the one
// Explorer shows for a shared folder without stored credentials, and connects
// the share with what is typed; "Remember my credentials" saves them in the
// Credential Manager. Returns NO_ERROR, ERROR_CANCELLED or another Windows
// error. dir may end with a backslash.
DWORD promptShareCredentials(HWND owner, const std::wstring& dir)
{
    std::wstring remote = dir;
    while (!remote.empty() && remote[remote.size() - 1] == L'\\') remote.erase(remote.size() - 1);
    NETRESOURCEW nr;
    memset(&nr, 0, sizeof(nr));
    nr.dwType = RESOURCETYPE_DISK;
    nr.lpRemoteName = (LPWSTR)remote.c_str();
    return WNetAddConnection3W(owner, &nr, NULL, NULL, CONNECT_INTERACTIVE | CONNECT_PROMPT | CONNECT_CMD_SAVECRED | CONNECT_TEMPORARY);
}

// Copies the files of Thunderhead's sounds folder to sounds\ under the node's
// share, those missing there or differing in size or write time (CopyFile
// keeps the write time), as part of the staging, so that an updated agent
// holds every file the host can ask it to play. Returns text for the log.
std::string copySoundsTo(const std::wstring& from, const std::wstring& shareDir)
{
    std::wstring to = shareDir + kSoundFolder;    // shareDir ends with a backslash
    if (!CreateDirectoryW(to.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return "; sounds folder not created on the node: " + lastErrorText((int)GetLastError());
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((from + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "; no sounds folder beside Thunderhead to copy";
    unsigned copied = 0, same = 0, failed = 0;
    std::string firstError;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring src = from + L"\\" + fd.cFileName, dst = to + L"\\" + fd.cFileName;
        WIN32_FILE_ATTRIBUTE_DATA have;
        if (GetFileAttributesExW(dst.c_str(), GetFileExInfoStandard, &have) &&
            have.nFileSizeLow == fd.nFileSizeLow && have.nFileSizeHigh == fd.nFileSizeHigh &&
            CompareFileTime(&have.ftLastWriteTime, &fd.ftLastWriteTime) == 0) { ++same; continue; }
        if (CopyFileW(src.c_str(), dst.c_str(), FALSE)) ++copied;
        else { ++failed; if (firstError.empty()) firstError = narrow(fd.cFileName) + ": " + lastErrorText((int)GetLastError()); }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::string text = format("; sounds: %u file(s) copied, %u already there", copied, same);
    if (failed) text += format(", %u failed (%s)", failed, firstError.c_str());
    return text;
}

// Writes farm.key into dir (ending with a backslash) unless the file there
// already holds the key. Returns text for the log, empty when nothing was done.
std::string copyKeyTo(const std::wstring& dir, const std::string& key, bool& written)
{
    written = false;
    if (key.empty()) return std::string();
    std::wstring path = dir + kFarmKeyFile;
    if (readKeyFile(path) == key) return std::string();
    std::string err;
    if (!writeKeyFile(path, key, err)) return "; farm.key not written: " + err;
    written = true;
    return "; farm.key written";
}

// Phase 1, worker thread: places the agent file beside the running agent as
// LightningRod.new while everything on the node is still running, so a share
// that cannot be written is reported before anything is stopped.
DWORD WINAPI updateStageThread(LPVOID p)
{
    UpdateJob* job = (UpdateJob*)p;
    UpdateResult* res = new UpdateResult();
    res->name = job->name;
    res->phase = 1;
    res->ok = false;
    res->needCredentials = false;
    if (job->viaRelay) {
        // The agent file and the sounds go to relay\<job> under the relay's
        // share; the relay copies them to the root of the client's share, where
        // the client's own helper swaps LightningRod.new in after UPDATE.
        std::string relayJob = newRelayJob(), err;
        std::wstring root = L"\\\\" + widen(job->relayIp) + L"\\" + job->share + L"\\";
        std::wstring stage = relayStageDir(job->relayIp, job->share, relayJob);
        if (!makeDirs(stage)) {
            int e = (int)GetLastError();
            err = "staging folder " + narrow(stage) + " on relay " + job->relayName + " could not be created: " + lastErrorText(e);
            if (isCredentialError(e)) { res->needCredentials = true; res->credDir = root; }
        } else if (!CopyFileW(job->source.c_str(), (stage + L"LightningRod.new").c_str(), FALSE)) {
            err = "copy to " + narrow(stage) + " failed: " + lastErrorText((int)GetLastError());
            removeTree(stage);
        } else {
            bool keyWritten = false;
            std::string sounds = copySoundsTo(job->sounds, stage) + copyKeyTo(stage, job->key, keyWritten);
            Message ack;
            if (relayRequest(job->relayConn, relayMessage("push", relayJob, job->ip, job->share), 10 * 60 * 1000, ack, err)) {
                res->ok = true;
                res->dir = L"\\\\" + widen(job->ip) + L"\\" + job->share + L"\\";
                res->text = format("agent staged as LightningRod.new in %s through relay %s (%s file(s), %s)%s", narrow(res->dir).c_str(),
                                   job->relayName.c_str(), ack.get("files").c_str(),
                                   byteText(_strtoui64(ack.get("bytes").c_str(), NULL, 10)).c_str(), sounds.c_str());
            } else {
                err = "relay " + job->relayName + " could not copy to \\\\" + job->ip + "\\" + narrow(job->share) + "\\: " + err;
                removeTree(stage);
            }
        }
        if (!res->ok) res->text = err;
        delete job;
        if (!PostMessageW(g_main, WM_UPDATE_DONE, 0, (LPARAM)res)) delete res;
        return 0;
    }
    std::vector<std::wstring> dirs;
    if (!job->root.empty()) dirs.push_back(job->root);   // the path the gate verified, and nothing else
    else {
        dirs.push_back(L"\\\\" + widen(job->ip) + L"\\" + job->share + L"\\");
        if (nameResolvesTo(job->name, job->ip)) dirs.push_back(L"\\\\" + widen(job->name) + L"\\" + job->share + L"\\");
    }
    std::string errors;
    for (size_t i = 0; i < dirs.size() && !res->ok; ++i) {
        if (CopyFileW(job->source.c_str(), (dirs[i] + L"LightningRod.new").c_str(), FALSE)) {
            res->ok = true;
            res->dir = dirs[i];
            res->text = "agent staged as LightningRod.new in " + narrow(dirs[i]);
        } else {
            int err = (int)GetLastError();
            errors += std::string(errors.empty() ? "" : "; ") + "copy to " + narrow(dirs[i]) + " failed: " + lastErrorText(err);
            if (!res->needCredentials && isCredentialError(err)) { res->needCredentials = true; res->credDir = dirs[i]; }
        }
    }
    if (res->ok) { bool keyWritten = false; res->text += copySoundsTo(job->sounds, res->dir) + copyKeyTo(res->dir, job->key, keyWritten); }
    if (!res->ok) res->text = errors;
    delete job;
    if (!PostMessageW(g_main, WM_UPDATE_DONE, 0, (LPARAM)res)) delete res;
    return 0;
}

// Phase 2, worker thread, after the old agent has closed its connection. An
// agent from 0.2.5 on swaps LightningRod.new in by itself as soon as the old
// file closes; for older agents Thunderhead does the swap over the share and
// then drops update.go, which their helper waits for.
DWORD WINAPI updateFinishThread(LPVOID p)
{
    UpdateJob* job = (UpdateJob*)p;
    UpdateResult* res = new UpdateResult();
    res->name = job->name;
    res->phase = 2;
    res->ok = false;
    res->needCredentials = false;
    std::wstring staged = job->dir + L"LightningRod.new";
    std::wstring target = job->dir + L"LightningRod.exe";
    bool swappedHere = false;
    for (int attempt = 0; attempt < 120 && !res->ok; ++attempt) {    // the old exe may still be closing
        if (GetFileAttributesW(staged.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
            res->ok = true;
            res->text = "the node swapped the staged agent in by itself";
        } else if (MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            res->ok = true;
            swappedHere = true;
            res->text = "LightningRod.exe replaced through " + narrow(job->dir);
        } else {
            Sleep(500);
        }
    }
    if (!res->ok) res->text = "replace of LightningRod.exe failed: " + lastErrorText((int)GetLastError());
    if (swappedHere || !res->ok) {
        // The helper of an agent before 0.2.5 starts LightningRod.exe when this file appears.
        HANDLE go = CreateFileW((job->dir + L"update.go").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (go != INVALID_HANDLE_VALUE) CloseHandle(go);
        else res->text += "; update.go could not be written (" + lastErrorText((int)GetLastError()) + ")";
    }
    delete job;
    if (!PostMessageW(g_main, WM_UPDATE_DONE, 0, (LPARAM)res)) delete res;
    return 0;
}

void startUpdateThread(NodeRecord* n, LPTHREAD_START_ROUTINE fn, const char* what)
{
    UpdateJob* job = new UpdateJob();
    job->name = n->name;
    job->ip = n->ip;
    job->source = pushFilePathFor(n);
    job->share = widen(g_store->settings.shareName);
    job->dir = n->updateDir;
    job->sounds = soundDir();
    job->key = g_farmKey;
    job->root = n->shareState == 1 ? shareRootOf(n) : std::wstring();
    NodeRecord* relay = relayFor(n);
    job->viaRelay = relay != NULL;
    job->relayConn = relay ? relay->conn : -1;
    if (relay) { job->relayName = relay->name; job->relayIp = relay->ip; }
    n->updateViaRelay = job->viaRelay;
    HANDLE t = CreateThread(NULL, 0, fn, job, 0, NULL);
    if (t) { CloseHandle(t); return; }
    delete job;
    n->updating = false;
    refreshRow(n);
    logEvent(n->name + " update: could not start the " + what + " thread");
}

// A share that refused the credentials Windows offered gets the credential
// prompt, owned by the main window, and the operation is tried once more with
// what was typed: an update stages again, a shared-folder gate check runs
// again. Refusals that arrive while a prompt is up wait their turn.
struct CredRequest { NodeRecord* n; std::wstring dir; bool update; };   // update: the staging asked; otherwise the gate's share check
std::vector<CredRequest> g_credQueue;
bool g_credPrompting = false;
void startShareCheck(NodeRecord* n, const char* why);

void runCredQueue()
{
    if (g_credPrompting) return;
    g_credPrompting = true;
    while (!g_credQueue.empty()) {
        CredRequest req = g_credQueue.front();
        g_credQueue.erase(g_credQueue.begin());
        NodeRecord* n = req.n;
        std::wstring dir = req.dir;
        if (!req.update) {
            // The gate: the share is checked again at once with what was typed;
            // a cancelled prompt leaves the minute retries running without one,
            // until the node's next HELLO or Open Client's Shared Folder.
            if (n->shareState == 1) continue;
            logEvent(n->name + ": " + narrow(dir) + " refused the credentials Windows offered; asking for them");
            DWORD e = promptShareCredentials(g_main, dir);
            if (e == NO_ERROR) {
                logEvent(n->name + ": connected to " + narrow(dir) + " with the credentials given; checking the shared folder again");
                n->shareChecking = false;
                startShareCheck(n, "credentials");
            } else {
                logEvent(n->name + ": " + (e == ERROR_CANCELLED ? std::string("the credential prompt was cancelled; the shared folder is checked again every minute, and the prompt returns at its next HELLO or with Open Client's Shared Folder")
                                                                : lastErrorText((int)e)));
            }
            continue;
        }
        if (!n->updating) continue;
        logEvent(n->name + " update: " + narrow(dir) + " refused the credentials Windows offered; asking for them");
        DWORD e = promptShareCredentials(g_main, dir);
        if (e == NO_ERROR) {
            logEvent(n->name + " update: connected to " + narrow(dir) + " with the credentials given, staging again");
            startUpdateThread(n, updateStageThread, "staging");
            continue;
        }
        logEvent(n->name + " update: " + (e == ERROR_CANCELLED ? std::string("the credential prompt was cancelled") : lastErrorText((int)e)));
        n->updating = false;
        n->updateStaged = false;
        refreshRow(n);
    }
    g_credPrompting = false;
}

void onUpdateDone(UpdateResult* r)
{
    std::string text = r->name + " update: " + r->text;
    logEvent(text);
    updateStatusBar((L"|  " + widen(text)).c_str());
    NodeRecord* n = NULL;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->name == r->name) n = nodes[i];
    if (!n || !n->updating) return;
    if (r->phase == 1 && !r->ok && r->needCredentials && !n->updatePrompted) {
        n->updatePrompted = true;
        CredRequest req; req.n = n; req.dir = r->credDir; req.update = true;
        g_credQueue.push_back(req);
        runCredQueue();
        return;
    }
    if (r->phase == 1 && r->ok && n->online) {
        n->updateDir = r->dir;
        n->updateStaged = true;
        Message m("UPDATE");
        if (sendGated(n, m)) {
            logEvent(format("UPDATE -> %s (agent %s)", n->name.c_str(), n->hello.get("agent").c_str()));
            logNode(*n, "-> " + m.oneLine());
            return;
        }
        logEvent(n->name + " update: UPDATE could not be sent");
    } else if (r->phase == 1 && r->ok) {
        logEvent(n->name + " update: the node went offline before UPDATE was sent");
    } else if (r->phase == 2 && r->ok) {
        return;   // Updating stays until the new agent connects
    }
    n->updating = false;
    n->updateStaged = false;
    refreshRow(n);
}

void updateChecked()
{
    std::wstring file = pushFilePath();
    std::string version = fileVersion(file);
    if (version.empty()) {
        MessageBoxW(g_main, (L"No agent file with a version resource at\n" + file).c_str(), L"Thunderhead", MB_OK | MB_ICONWARNING);
        return;
    }
    g_pushVersion = version;
    std::vector<NodeRecord*> targets = checkedNodes();
    int sent = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online || n->updating) continue;
        if (!commandAllowed(n, "UPDATE")) continue;   // the agent file goes into its share: no share, no update
        if (NodeRecord* relay = relayFor(n)) { if (!commandAllowed(relay, "UPDATE (as relay for " + n->name + ")")) continue; }
        std::wstring nodeFile = pushFilePathFor(n);
        std::string nodeVersion = nodeFile.empty() ? std::string() : fileVersion(nodeFile);
        if (nodeVersion.empty()) {
            logEvent(format("%s update not started: it runs %s, which needs the XP build XP\\LightningRod.exe under Thunderhead's folder, and that file is %s",
                            n->name.c_str(), n->hello.get("os").c_str(), nodeFile.empty() ? "absent" : "without a version resource"));
            continue;
        }
        ++sent;
        n->updating = true;
        n->updateStaged = false;
        n->updateDir.clear();
        n->updatePrompted = false;
        n->updateStartMs = tickMs();
        logEvent(format("%s update: agent %s, staging %s from %s", n->name.c_str(), n->hello.get("agent").c_str(), nodeVersion.c_str(), narrow(nodeFile).c_str()));
        if (NodeRecord* relay = relayFor(n)) logEvent(format("%s update: its share is reached through SMB1 relay %s", n->name.c_str(), relay->name.c_str()));
        refreshRow(n);
        startUpdateThread(n, updateStageThread, "staging");
    }
    updateStatusBar(widen(format("|  update started for %d of %d checked", sent, (int)targets.size())).c_str());
}

// ---------------------------------------------------------------- commands

void sendCommandMessage(const Message& m)
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    int sent = 0, blocked = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        if (!commandAllowed(n, m.oneLine())) { ++blocked; continue; }
        if (sendGated(n, m)) {
            ++sent;
            logEvent(format("%s -> %s", m.oneLine().c_str(), n->name.c_str()));
            logNode(*n, std::string("-> ") + m.oneLine());
        }
    }
    std::string status = format("|  %s sent to %d of %d checked", m.verb.c_str(), sent, (int)targets.size());
    if (blocked) status += format(", %d blocked: shared folder not readable", blocked);
    updateStatusBar(widen(status).c_str());
}

void sendCommand(const char* verb)
{
    sendCommandMessage(Message(verb));
}

// Exit LR: one warning per checked online client before its EXIT, naming the
// client and its address, since an agent that has exited cannot be reached
// again from here. "Yes, really End LR" sends it, "Skip this Client" goes on
// to the next, "Cancel All" (or closing the dialog) stops the round.
void exitChecked()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    int online = 0;
    for (size_t i = 0; i < targets.size(); ++i) if (targets[i]->online) ++online;
    if (online == 0) { updateStatusBar(L"|  no online clients checked"); return; }
    int sent = 0, skipped = 0, blocked = 0, left = online;
    bool cancelled = false;
    for (size_t i = 0; i < targets.size() && !cancelled; ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        --left;
        std::wstring instruction = L"End Lightning Rod on " + widen(n->name) + L" (" + widen(n->ip) + L")?";
        std::wstring content = L"Closing Lightning Rod cannot be undone from here. " + widen(n->name) +
                               L" will stop responding to Thunderhead: no reports, no commands, no update. "
                               L"To regain control, Lightning Rod has to be started again at the client, through a remote desktop "
                               L"session, or by restarting the client manually.";
        if (left) content += widen(format("\n\n%d more checked online client(s) follow this one.", left));
        TASKDIALOG_BUTTON buttons[3] = { { 901, L"Yes, really End LR" }, { 902, L"Skip this Client" }, { 903, L"Cancel All" } };
        TASKDIALOGCONFIG tc;
        memset(&tc, 0, sizeof(tc));
        tc.cbSize = sizeof(tc);
        tc.hwndParent = g_main;
        tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        tc.pszWindowTitle = L"Exit LR";
        tc.pszMainIcon = TD_WARNING_ICON;
        tc.pszMainInstruction = instruction.c_str();
        tc.pszContent = content.c_str();
        tc.pButtons = buttons;
        tc.cButtons = 3;
        tc.nDefaultButton = 902;   // Skip this Client
        int pressed = 0;
        if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || pressed == 903 || pressed == IDCANCEL) { cancelled = true; break; }
        if (pressed != 901) { ++skipped; logEvent(format("Exit LR: %s skipped", n->name.c_str())); continue; }
        if (!n->online) { logEvent(format("Exit LR: %s went offline before EXIT was sent", n->name.c_str())); continue; }
        Message m("EXIT");
        if (sendToNode(n, m, m.oneLine())) ++sent; else ++blocked;
    }
    std::string status = format("|  Exit LR: %d sent, %d skipped", sent, skipped);
    if (blocked) status += format(", %d not sent (shared folder not readable)", blocked);
    if (cancelled) status += format(", cancelled with %d not asked", left);
    updateStatusBar(widen(status).c_str());
}

// Lightning Rod Ping: every checked client is told to report at once. An agent from 0.2.11
// answers with a full REPORT, an older one with PONG; a client that sends
// neither within kPingWaitMs is marked offline by onTimer. A checked client
// that is offline already gets a line in the event log instead of a PING.
const char* const kPingReportAgent = "0.2.11";
const unsigned long long kPingWaitMs = 5000;
int g_pingChecked = 0, g_pingSent = 0, g_pingReplied = 0, g_pingNoReply = 0, g_pingOffline = 0;

void pingStatus()
{
    updateStatusBar(widen(format("|  PING: %d checked, %d sent, %d replied, %d no reply, %d offline", g_pingChecked, g_pingSent,
                                 g_pingReplied, g_pingNoReply, g_pingOffline)).c_str());
}

void pingChecked()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    g_pingChecked = (int)targets.size();
    g_pingSent = g_pingReplied = g_pingNoReply = g_pingOffline = 0;
    unsigned long long now = tickMs();
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) {
            ++g_pingOffline;
            logEvent(n->name + " is offline, not pinged");
            continue;
        }
        Message m("PING");
        if (!sendGated(n, m)) {
            ++g_pingNoReply;
            logEvent("PING not sent to " + n->name);
            continue;
        }
        ++g_pingSent;
        n->pingPending = true;
        n->pingSentMs = now;
        logEvent("PING -> " + n->name);
        logNode(*n, "-> PING");
    }
    pingStatus();
}

// Called for the first REPORT or PONG after a PING; what says which.
void pingReplied(NodeRecord* n, const std::string& what)
{
    n->pingPending = false;
    ++g_pingReplied;
    logEvent(format("%s from %s %llu ms after PING", what.c_str(), n->name.c_str(), tickMs() - n->pingSentMs));
    pingStatus();
}

// ---------------------------------------------------------------- text display box

// A pop-up with read-only, scrollable text and an OK button, for results that
// are too long for the status bar. The text may be appended to while the box
// is open through WM_TEXTBOX_APPEND; opened, when given, is called once the
// box exists, so that whatever fills it knows where to send its lines.
const UINT WM_TEXTBOX_APPEND = WM_APP + 4;   // wParam: job id, lParam: std::wstring* to append (deleted here)
struct TextBoxStart { std::wstring title, text; void (*opened)(HWND, void*); void* arg; unsigned job; };
HFONT g_textBoxFont = NULL;

INT_PTR CALLBACK textBoxDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    HWND edit = GetDlgItem(dlg, IDC_TEXTBOX_TEXT);
    switch (msg) {
    case WM_INITDIALOG: {
        TextBoxStart* s = (TextBoxStart*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)s->job);
        SetWindowTextW(dlg, s->title.c_str());
        if (!g_textBoxFont) {
            HDC dc = GetDC(dlg);
            g_textBoxFont = CreateFontW(-MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        FIXED_PITCH | FF_MODERN, L"Consolas");
            ReleaseDC(dlg, dc);
        }
        if (g_textBoxFont) SendMessageW(edit, WM_SETFONT, (WPARAM)g_textBoxFont, TRUE);
        SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
        SetWindowTextW(edit, s->text.c_str());
        if (s->opened) s->opened(dlg, s->arg);
        SetFocus(GetDlgItem(dlg, IDOK));
        return FALSE;
    }
    case WM_TEXTBOX_APPEND: {
        std::wstring* s = (std::wstring*)lp;
        if ((unsigned)GetWindowLongPtrW(dlg, GWLP_USERDATA) == (unsigned)wp) {
            int len = GetWindowTextLengthW(edit);
            SendMessageW(edit, EM_SETSEL, len, len);
            SendMessageW(edit, EM_REPLACESEL, FALSE, (LPARAM)s->c_str());
        }
        delete s;
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDOK); return TRUE; }
        break;
    }
    return FALSE;
}

// Shows the box and returns when OK closes it.
void showTextBox(const std::wstring& title, const std::wstring& text, void (*opened)(HWND, void*) = NULL, void* arg = NULL, unsigned job = 0)
{
    TextBoxStart s;
    s.title = title;
    s.text = text;
    s.opened = opened;
    s.arg = arg;
    s.job = job;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_TEXTBOX), g_main, textBoxDlgProc, (LPARAM)&s);
}

// ---------------------------------------------------------------- network ping

// Network Ping: a plain ICMP echo test of every checked client, three echo
// requests each as "ping -n 3" sends, to the address the client is known by:
// that of its connection, or for an offline client its last known address (kept
// in nodes.txt across Thunderhead restarts and shown in its IP column), else the
// one its last HELLO carried, else its name
// resolved. The clients are pinged at the same time, each on its own thread,
// and the lines appear in a text box as each one finishes; OK closes it. A box
// closed early loses nothing but the display: the threads finish on their own
// and the event log keeps the line naming the clients pinged.
const int kNetPingCount = 3;
const DWORD kNetPingTimeoutMs = 1000;
const int kNetPingBytes = 32;

struct NetPingJob {
    HWND dlg;
    unsigned id;
    volatile LONG remaining;             // clients still being pinged
    volatile LONG answered;              // clients that replied at least once
    int clients;
};
struct NetPingTarget { NetPingJob* job; std::string name, ip; };
struct NetPingStart { NetPingJob* job; std::vector<NetPingTarget*> targets; };
unsigned g_netPingId = 0;

std::string ipText(IPAddr a)   // IPAddr is in network order
{
    return format("%u.%u.%u.%u", (unsigned)(a & 255), (unsigned)((a >> 8) & 255), (unsigned)((a >> 16) & 255), (unsigned)((a >> 24) & 255));
}

std::string icmpStatusText(DWORD status)
{
    wchar_t buf[256];
    DWORD size = 255;
    if (GetIpErrorString(status, buf, &size) == NO_ERROR) return narrow(trim(buf));
    return lastErrorText((int)status);
}

void postNetPingText(NetPingJob* job, const std::string& text)
{
    std::wstring* s = new std::wstring(widen(text));
    if (!PostMessageW(job->dlg, WM_TEXTBOX_APPEND, job->id, (LPARAM)s)) delete s;   // the box is closed
}

std::string netPingDone(const NetPingJob* job)
{
    return format("Done: %d client(s), %d replied, %d without a reply.\r\n", job->clients, (int)job->answered,
                  job->clients - (int)job->answered);
}

DWORD WINAPI netPingThread(LPVOID param)
{
    NetPingTarget* t = (NetPingTarget*)param;
    NetPingJob* job = t->job;
    std::string ip = t->ip;
    if (ip.empty()) {   // never reported: try the name
        addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        if (getaddrinfo(t->name.c_str(), NULL, &hints, &res) == 0 && res) {
            ip = ipText(((sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
            freeaddrinfo(res);
        }
    }
    std::string out = t->name + (ip.empty() ? "" : " (" + ip + ")") + "\r\n";
    int received = 0;
    if (ip.empty()) {
        out += "  no address: the client has never reported and its name does not resolve\r\n";
    } else {
        IPAddr addr = inet_addr(ip.c_str());
        HANDLE h = IcmpCreateFile();
        if (h == INVALID_HANDLE_VALUE) {
            out += "  ICMP is not available: " + lastErrorText((int)GetLastError()) + "\r\n";
        } else {
            char data[kNetPingBytes];
            for (int i = 0; i < kNetPingBytes; ++i) data[i] = (char)('a' + i % 23);   // the pattern ping.exe sends
            std::vector<char> reply(sizeof(ICMP_ECHO_REPLY) + kNetPingBytes + 8);
            DWORD minRt = 0, maxRt = 0, sumRt = 0;
            for (int i = 0; i < kNetPingCount; ++i) {
                DWORD n = IcmpSendEcho(h, addr, data, sizeof(data), NULL, &reply[0], (DWORD)reply.size(), kNetPingTimeoutMs);
                ICMP_ECHO_REPLY* r = (ICMP_ECHO_REPLY*)&reply[0];
                if (n == 0) { out += "  " + icmpStatusText(GetLastError()) + "\r\n"; continue; }
                if (r->Status != IP_SUCCESS) { out += "  Reply from " + ipText(r->Address) + ": " + icmpStatusText(r->Status) + "\r\n"; continue; }
                DWORD rt = r->RoundTripTime;
                if (received == 0 || rt < minRt) minRt = rt;
                if (rt > maxRt) maxRt = rt;
                sumRt += rt;
                ++received;
                out += format("  Reply from %s: bytes=%u time%s TTL=%u\r\n", ipText(r->Address).c_str(), (unsigned)r->DataSize,
                              rt == 0 ? "<1ms" : format("=%ums", (unsigned)rt).c_str(), (unsigned)r->Options.Ttl);
            }
            IcmpCloseHandle(h);
            int lost = kNetPingCount - received;
            out += format("  %d sent, %d received, %d lost (%d%% loss)", kNetPingCount, received, lost, lost * 100 / kNetPingCount);
            if (received) out += format(", round trip min %u ms, avg %u ms, max %u ms", (unsigned)minRt, (unsigned)(sumRt / received), (unsigned)maxRt);
            out += "\r\n";
        }
    }
    out += "\r\n";
    postNetPingText(job, out);
    if (received) InterlockedIncrement(&job->answered);
    if (InterlockedDecrement(&job->remaining) == 0) {
        postNetPingText(job, netPingDone(job));
        delete job;
    }
    delete t;
    return 0;
}

// Called once the text box exists: the threads need its handle for their lines.
void startNetPing(HWND dlg, void* arg)
{
    NetPingStart* s = (NetPingStart*)arg;
    s->job->dlg = dlg;
    for (size_t i = 0; i < s->targets.size(); ++i) {
        HANDLE t = CreateThread(NULL, 0, netPingThread, s->targets[i], 0, NULL);
        if (t) { CloseHandle(t); continue; }
        postNetPingText(s->job, s->targets[i]->name + "\r\n  could not start a thread: " + lastErrorText((int)GetLastError()) + "\r\n\r\n");
        delete s->targets[i];
        if (InterlockedDecrement(&s->job->remaining) == 0) {
            postNetPingText(s->job, netPingDone(s->job));
            delete s->job;
            break;
        }
    }
}

void networkPing()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    NetPingStart s;
    s.job = new NetPingJob();
    s.job->dlg = NULL;
    s.job->id = ++g_netPingId;
    s.job->remaining = (LONG)targets.size();
    s.job->answered = 0;
    s.job->clients = (int)targets.size();
    std::string names;
    for (size_t i = 0; i < targets.size(); ++i) {
        NetPingTarget* t = new NetPingTarget();
        t->job = s.job;
        t->name = targets[i]->name.empty() ? targets[i]->mac : targets[i]->name;
        t->ip = nodeIp(*targets[i]);
        s.targets.push_back(t);
        names += (names.empty() ? "" : ", ") + t->name + (t->ip.empty() ? "" : " " + t->ip);
    }
    logEvent(format("Network Ping: %d echo requests each to %s", kNetPingCount, names.c_str()));
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string head = format("Network Ping at %02d:%02d:%02d: %d checked client(s), %d echo requests of %d bytes each, %u ms timeout.\r\n"
                              "Each client's lines appear when its requests are done.\r\n\r\n", st.wHour, st.wMinute, st.wSecond,
                              (int)targets.size(), kNetPingCount, kNetPingBytes, (unsigned)kNetPingTimeoutMs);
    showTextBox(L"Network Ping", widen(head), startNetPing, &s, s.job->id);
}

// First agent version that knows POWER; older agents ignore the verb.
const char* const kPowerAgent = "0.2.6";

// Sends m to every checked online client whose agent is at least minAgent.
// Older agents do not know the verb and are skipped with a line in the event
// log. label is what the logs and the status bar show for the message.
void sendVersioned(const Message& m, const std::string& label, const char* minAgent)
{
    std::vector<NodeRecord*> targets = checkedNodes();
    int online = 0, sent = 0, old = 0, blocked = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        ++online;
        std::string agent = n->hello.get("agent");
        if (compareVersions(agent, minAgent) < 0) {
            ++old;
            logEvent(format("%s not sent to %s: agent %s is older than %s, update it first",
                            label.c_str(), n->name.c_str(), agent.c_str(), minAgent));
            continue;
        }
        if (!commandAllowed(n, label)) { ++blocked; continue; }
        if (sendGated(n, m)) {
            ++sent;
            logEvent(format("%s -> %s", label.c_str(), n->name.c_str()));
            logNode(*n, "-> " + label);
        }
    }
    if (online == 0) { updateStatusBar(L"|  no online clients checked"); return; }
    std::string status = format("|  %s sent to %d of %d checked", label.c_str(), sent, (int)targets.size());
    if (old) status += format(", %d skipped: agent older than %s", old, minAgent);
    if (blocked) status += format(", %d blocked: shared folder not readable", blocked);
    updateStatusBar(widen(status).c_str());
}

void sendPower(const char* action)
{
    Message m("POWER");
    m.set("action", std::string(action));
    sendVersioned(m, m.oneLine(), kPowerAgent);
}

// Mute / Unmute Checked Clients: AUDIO action=mute|unmute sets the master mute
// of the client's default playback device; the ACK reports the state read back.
const char* const kAudioAgent = "0.2.16";

void sendAudio(const char* action)
{
    Message m("AUDIO");
    m.set("action", std::string(action));
    sendVersioned(m, m.oneLine(), kAudioAgent);
}

// One message to one online client, logged as the others are.
bool sendToNode(NodeRecord* n, const Message& m, const std::string& label)
{
    if (!n->online || !sendGated(n, m)) return false;
    logEvent(format("%s -> %s", label.c_str(), n->name.c_str()));
    logNode(*n, "-> " + label);
    return true;
}

// All Lightning Rods Audio (Settings): AUDIO unmute when the switch is on,
// mute when it is off, to a client as it connects and to every online client
// when the switch changes. Agents before kAudioAgent are passed over.
void sendAudioSwitch(NodeRecord* n, bool on)
{
    if (compareVersions(n->hello.get("agent"), kAudioAgent) < 0) return;
    Message m("AUDIO");
    m.set("action", std::string(on ? "unmute" : "mute"));
    sendToNode(n, m, m.oneLine());
}

void sendAudioAll(bool on)
{
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->online) sendAudioSwitch(nodes[i], on);
}

// Sounds on the agent: SOUND action=scheme names the files of the agent's own
// events (logon=, hello=, logoff=; empty for the category's default file), as
// the scheme here assigns them; SOUND action=play file=NAME plays a file of
// the agent's sounds folder. Agents from 0.2.21 know SOUND.
const char* const kSoundAgent = "0.2.21";

Message soundSchemeMessage()
{
    Message m("SOUND");
    m.set("action", std::string("scheme"));
    for (int e = 0; e < kSoundEventCount; ++e) {
        if (kSoundEvents[e].app[0] != L'L') continue;
        std::wstring name = soundNameForCategory(kSoundEvents[e].category);
        m.set(kSoundEvents[e].key, name == soundDefaultFile(kSoundEvents[e].category) ? std::string() : narrow(name));
    }
    return m;
}

void sendSoundScheme(NodeRecord* n)
{
    if (compareVersions(n->hello.get("agent"), kSoundAgent) < 0) return;
    Message m = soundSchemeMessage();
    sendToNode(n, m, m.oneLine());
}

void sendSoundSchemeAll()
{
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->online) sendSoundScheme(nodes[i]);
}

void sendPlaySound(const std::wstring& name)
{
    Message m("SOUND");
    m.set("action", std::string("play"));
    m.set("file", narrow(name));
    sendVersioned(m, m.oneLine(), kSoundAgent);
}

// Start TeamViewer: the agent finds the installed TeamViewer through the
// registry or the Start menu, whatever its version and folder, starts it and
// brings its window to the front. The ACK names the path and the source.
const char* const kTeamViewerAgent = "0.2.18";

void startTeamViewer()
{
    Message m("TEAMVIEWER");
    sendVersioned(m, m.oneLine(), kTeamViewerAgent);
}

// Run Lightning 3 / 2 / 1: START gen=N. The agent finds the installed Lightning
// of that generation through its installer's registry entries, the Start menu
// or the default folder (Lightning lookup in docs/PROTOCOL.md) and the ACK
// names the file, its version and the source. Agents before 0.2.19 ignore
// gen and start the Bryce 7 Lightning from its default folder, so the Bryce 6
// and Bryce 5 buttons go only to agents that know those generations' entries
// (0.2.20 for Lightning 2); older agents are skipped with an event log line.
const char* const kLightning2Agent = "0.2.20";

void runLightning(int gen)
{
    Message m("START");
    m.set("gen", (long long)gen);
    if (gen == 3) sendCommandMessage(m);
    else sendVersioned(m, m.oneLine(), kLightning2Agent);
}

// Run Lightning: asks which generation with one radio per Lightning, numbered
// as the agent numbers them (3 = Bryce 7 and 7 Pro, 2 = Bryce 6.x, 1 = Bryce
// 5.x, the last not yet supported and disabled), then START gen= to every
// checked online client.
HRESULT CALLBACK lightningDialogProc(HWND dlg, UINT msg, WPARAM, LPARAM, LONG_PTR)
{
    if (msg == TDN_CREATED) SendMessageW(dlg, TDM_ENABLE_RADIO_BUTTON, 601, FALSE);
    return S_OK;
}

void runLightningAsked()
{
    TASKDIALOG_BUTTON radios[3] = { { 603, L"3 (B7 && 7 Pro)" }, { 602, L"2 (B6.x)" }, { 601, L"1 (B5.x)" } };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = L"Run Lightning";
    tc.pszMainInstruction = L"Which Lightning is to be started on the checked clients?";
    tc.pszContent = L"The agent finds that generation's installed Lightning.exe and starts it; a generation that is not installed is reported in the event log.";
    tc.pRadioButtons = radios;
    tc.cRadioButtons = 3;
    tc.nDefaultRadioButton = 603;
    tc.pfCallback = lightningDialogProc;
    int pressed = 0, radio = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, &radio, NULL)) || pressed != IDOK) return;
    runLightning(radio - 600);
}

// Set Client Date and Time: every checked online client gets this host's clock
// and sets its own to it when its date differs from the host's or its time is
// kTimeToleranceSec or more from the host's; a clock closer than that is left
// alone. The ACK says what was found and done.
const char* const kTimeAgent = "0.2.12";
const int kTimeToleranceSec = 180;

void setClientTime()
{
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    Message m("TIME");
    m.set("utc_ms", format("%llu", utcNowMs()));
    m.set("date", format("%04d-%02d-%02d", lt.wYear, lt.wMonth, lt.wDay));
    m.set("tolerance", (long long)kTimeToleranceSec);
    sendVersioned(m, format("TIME (host clock %04d-%02d-%02d %02d:%02d:%02d)", lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond),
                  kTimeAgent);
}

// Shutdown Node, the one button for shutting down and restarting: asks which
// shutdown.exe action the checked clients are to run.
// The dialog is also the confirmation. Every action goes out with /f and /t 66,
// so Abort Shutdown can cancel it during the 66 seconds.
void shutdownChecked()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    int online = 0;
    for (size_t i = 0; i < targets.size(); ++i) if (targets[i]->online) ++online;
    if (online == 0) { updateStatusBar(L"|  no online clients checked"); return; }
    static const char* const kActions[3] = { "shutdown", "restart", "coldrestart" };
    TASKDIALOG_BUTTON radios[3] = { { 400, L"Shutdown (/s)" }, { 401, L"Restart (/r)" }, { 402, L"Cold restart (/g)" } };
    std::wstring content = widen(format("%d checked online client(s) will close all programs by force (/f) and act after "
                                        "66 seconds (/t 66). Abort Shutdown cancels the action during that time.", online));
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = L"Shutdown Node";
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = L"Select the action for the checked clients";
    tc.pszContent = content.c_str();
    tc.pRadioButtons = radios;
    tc.cRadioButtons = 3;
    tc.nDefaultRadioButton = 400;   // Shutdown
    tc.nDefaultButton = IDCANCEL;
    int pressed = 0, radio = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, &radio, NULL)) || pressed != IDOK || radio < 400 || radio > 402) return;
    sendPower(kActions[radio - 400]);
}

// Asks for a priority class and sends PRIORITY to every checked client, which
// applies it to its Lightning.exe. RealTime and High carry a cost, so each
// checked online client is confirmed on its own: "Yes, Proceed" sends it,
// "Skip This Client" goes on to the next, "Cancel All" (or closing the
// dialog) stops the round and leaves the clients not yet asked unchanged.
void changeRenderPriority()
{
    std::vector<NodeRecord*> nodes = checkedNodes();
    if (nodes.empty()) {
        MessageBoxW(g_main, L"Check the clients to change first.", L"Change Render Priority", MB_OK | MB_ICONINFORMATION);
        return;
    }
    static const char* const kLevels[6] = { "RealTime", "High", "AboveNormal", "Normal", "BelowNormal", "Low" };
    std::wstring labels[6];
    TASKDIALOG_BUTTON radios[6];
    for (int i = 0; i < 6; ++i) {
        labels[i] = widen(kLevels[i]);
        radios[i].nButtonID = 200 + i;
        radios[i].pszButtonText = labels[i].c_str();
    }
    std::wstring content = widen(format("The priority class of Lightning.exe is changed on %u checked client(s).", (unsigned)nodes.size()));
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = L"Change Render Priority";
    tc.pszMainInstruction = L"Select the priority for Lightning";
    tc.pszContent = content.c_str();
    tc.pRadioButtons = radios;
    tc.cRadioButtons = 6;
    tc.nDefaultRadioButton = 203;   // Normal
    int pressed = 0, radio = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, &radio, NULL)) || pressed != IDOK || radio < 200 || radio > 205) return;
    const char* level = kLevels[radio - 200];

    const wchar_t* warning = NULL;
    if (radio == 200) warning = L"Setting priority to Realtime may cause the render client to stop responding, Lightning Rod to stop reporting, or cause issues with Windows itself.  Are you ABSOLUTELY sure you want to proceed?";
    else if (radio == 201) warning = L"Setting priority to High may cause CPU temperatures to rise. Are you sure you want to proceed?";
    Message m("PRIORITY");
    m.set("level", std::string(level));
    if (!warning) { sendCommandMessage(m); return; }

    int online = 0;
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->online) ++online;
    if (online == 0) { updateStatusBar(L"|  no online clients checked"); return; }
    int sent = 0, skipped = 0, blocked = 0, left = online;
    bool cancelled = false;
    for (size_t i = 0; i < nodes.size() && !cancelled; ++i) {
        NodeRecord* n = nodes[i];
        if (!n->online) continue;
        --left;
        std::wstring instruction = L"Set Lightning to " + labels[radio - 200] + L" on " + widen(n->name) + L" (" + widen(n->ip) + L")?";
        std::wstring content = warning;
        if (left) content += widen(format("\n\n%d more checked online client(s) follow this one.", left));
        TASKDIALOG_BUTTON buttons[3] = { { 300, L"Yes, Proceed" }, { 301, L"Skip This Client" }, { 302, L"Cancel All" } };
        TASKDIALOGCONFIG cc;
        memset(&cc, 0, sizeof(cc));
        cc.cbSize = sizeof(cc);
        cc.hwndParent = g_main;
        cc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        cc.pszWindowTitle = L"Change Render Priority";
        cc.pszMainIcon = TD_WARNING_ICON;
        cc.pszMainInstruction = instruction.c_str();
        cc.pszContent = content.c_str();
        cc.pButtons = buttons;
        cc.cButtons = 3;
        cc.nDefaultButton = 301;   // Skip This Client
        int answer = 0;
        if (FAILED(TaskDialogIndirect(&cc, &answer, NULL, NULL)) || answer == 302 || answer == IDCANCEL) { cancelled = true; break; }
        if (answer != 300) { ++skipped; logEvent(format("PRIORITY %s: %s skipped", level, n->name.c_str())); continue; }
        if (!n->online) { logEvent(format("PRIORITY %s: %s went offline before it was sent", level, n->name.c_str())); continue; }
        if (!commandAllowed(n, m.oneLine())) { ++blocked; continue; }
        if (sendToNode(n, m, m.oneLine())) ++sent;
    }
    std::string status = format("|  PRIORITY %s: %d sent, %d skipped", level, sent, skipped);
    if (blocked) status += format(", %d blocked: shared folder not readable", blocked);
    if (cancelled) status += format(", cancelled with %d not asked", left);
    updateStatusBar(widen(status).c_str());
}

// ---------------------------------------------------------------- wake on LAN

// Parses "60-A4-4C-B5-16-1C" (hyphens or colons) into six bytes.
bool parseMac(const std::string& text, unsigned char mac[6])
{
    int count = 0, digits = 0;
    unsigned value = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        char c = i < text.size() ? text[i] : '-';
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d >= 0) {
            if (digits == 2) return false;
            value = value * 16 + (unsigned)d;
            ++digits;
        } else if (c == '-' || c == ':') {
            if (digits != 2 || count == 6) return false;
            mac[count++] = (unsigned char)value;
            value = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    return count == 6;
}

// Where a magic packet goes, in network byte order: the directed broadcast
// address of every IPv4 interface of this host that is up, then 255.255.255.255,
// which Windows sends on one interface only.
std::vector<unsigned long> wakeTargets(SOCKET s)
{
    std::vector<unsigned long> out;
    INTERFACE_INFO info[32];
    DWORD bytes = 0;
    if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, info, sizeof(info), &bytes, NULL, NULL) == 0) {
        for (size_t i = 0; i < bytes / sizeof(INTERFACE_INFO); ++i) {
            if (!(info[i].iiFlags & IFF_UP) || (info[i].iiFlags & IFF_LOOPBACK)) continue;
            unsigned long addr = info[i].iiAddress.AddressIn.sin_addr.s_addr;
            unsigned long mask = info[i].iiNetmask.AddressIn.sin_addr.s_addr;
            if (addr == 0 || mask == 0 || mask == 0xFFFFFFFFul) continue;
            unsigned long broadcast = addr | ~mask;
            bool known = false;
            for (size_t k = 0; k < out.size(); ++k) if (out[k] == broadcast) known = true;
            if (!known) out.push_back(broadcast);
        }
    }
    out.push_back(INADDR_BROADCAST);
    return out;
}

// Send Wake On LAN Magic Packet: broadcasts the magic packet of every checked
// client, online or not, to UDP port 9: six FF bytes, then the client's MAC
// sixteen times. The network adapter of a client that is switched off starts
// the client when it sees its own packet; a running client ignores it. Nothing
// comes back, so the only sign of success is the client going online.
void sendWakeOnLan()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    if (s == INVALID_SOCKET || setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes)) != 0) {
        std::string text = "WOL: no broadcast socket: " + lastErrorText(WSAGetLastError());
        if (s != INVALID_SOCKET) closesocket(s);
        logEvent(text);
        updateStatusBar((L"|  " + widen(text)).c_str());
        return;
    }
    std::vector<unsigned long> nets = wakeTargets(s);
    int done = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        unsigned char mac[6];
        if (!parseMac(n->mac, mac)) {
            logEvent(format("WOL magic packet not sent for %s: \"%s\" is not a MAC address", n->name.c_str(), n->mac.c_str()));
            continue;
        }
        char packet[102];
        memset(packet, 0xFF, 6);
        for (int k = 0; k < 16; ++k) memcpy(packet + 6 + k * 6, mac, 6);
        std::string where, error;
        for (size_t k = 0; k < nets.size(); ++k) {
            sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = nets[k];
            to.sin_port = htons(9);
            if (sendto(s, packet, (int)sizeof(packet), 0, (sockaddr*)&to, sizeof(to)) == (int)sizeof(packet))
                where += (where.empty() ? "" : ", ") + ipText(nets[k]);
            else error = lastErrorText(WSAGetLastError());
        }
        if (where.empty()) {
            logEvent(format("WOL magic packet for %s (%s) could not be sent: %s", n->name.c_str(), n->mac.c_str(), error.c_str()));
            playEventSound(SoundThNoReply);
            continue;
        }
        ++done;
        logEvent(format("WOL magic packet for %s (%s) sent to %s, UDP port 9", n->name.c_str(), n->mac.c_str(), where.c_str()));
        logNode(*n, "-> WOL magic packet to " + where);
        playEventSound(SoundThSend);   // one send sound per client woken, as for any other command
    }
    closesocket(s);
    updateStatusBar(widen(format("|  WOL magic packet sent for %d of %d checked", done, (int)targets.size())).c_str());
}

// ---------------------------------------------------------------- notice windows

// A notice of Thunderhead's own, shown the way the agents show a Display
// Message: an ordinary top-level window (Minimize, Maximize or Restore and
// Close in its caption) with the text, an OK button and a line counting down
// to the moment it closes by itself. Enter and Escape close it through
// IsDialogMessage in the message loop. Several may be open at once.
const wchar_t* const kNoticeClass = L"ThunderheadNotice";
const UINT_PTR kNoticeTimer = 1;
struct NoticeWindow { std::wstring text; int seconds; unsigned long long startMs; HWND textCtl, countdown, ok; };
HFONT g_noticeFont = NULL;

void layoutNotice(HWND hwnd, NoticeWindow* n)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom, margin = 14, bh = 28, bw = 96;
    MoveWindow(n->textCtl, margin, margin, w - 2 * margin, h - 3 * margin - bh, TRUE);
    MoveWindow(n->countdown, margin, h - margin - bh + 6, w - 3 * margin - bw, bh - 6, TRUE);
    MoveWindow(n->ok, w - margin - bw, h - margin - bh, bw, bh, TRUE);
}

void refreshNoticeCountdown(NoticeWindow* n)
{
    long long left = n->seconds - (long long)((tickMs() - n->startMs) / 1000);
    if (left < 0) left = 0;
    SetWindowTextW(n->countdown, widen(format("Closes by itself in %lld s", left)).c_str());
}

LRESULT CALLBACK noticeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    NoticeWindow* n = (NoticeWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        n = (NoticeWindow*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)n);
        if (!g_noticeFont) {
            LOGFONTW lf;
            memset(&lf, 0, sizeof(lf));
            if (GetObjectW(g_font, sizeof(lf), &lf)) { lf.lfHeight = lf.lfHeight * 7 / 5; g_noticeFont = CreateFontIndirectW(&lf); }
        }
        n->textCtl = CreateWindowExW(0, L"STATIC", n->text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
        n->countdown = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 10, 10, hwnd, NULL, g_inst, NULL);
        n->ok = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDOK, g_inst, NULL);
        SendMessageW(n->textCtl, WM_SETFONT, (WPARAM)(g_noticeFont ? g_noticeFont : g_font), TRUE);
        SendMessageW(n->countdown, WM_SETFONT, (WPARAM)g_font, TRUE);
        SendMessageW(n->ok, WM_SETFONT, (WPARAM)g_font, TRUE);
        n->startMs = tickMs();
        refreshNoticeCountdown(n);
        themeWindow(hwnd);
        SetTimer(hwnd, kNoticeTimer, 1000, NULL);
        return 0;
    }
    if (!n) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_THEME_CHANGED: themeWindow(hwnd); return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, themeBrush(theme().paneBack));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (HBRUSH b = themeCtlColor(msg, (HDC)wp, (HWND)lp)) return (LRESULT)b;
        break;
    case WM_DRAWITEM: return themeDrawPushButton((const DRAWITEMSTRUCT*)lp) ? TRUE : FALSE;
    case WM_SIZE: layoutNotice(hwnd, n); return 0;
    case WM_SETFOCUS: SetFocus(n->ok); return 0;
    case DM_GETDEFID: return MAKELONG(IDOK, DC_HASDEFID);   // Enter presses OK, through IsDialogMessage
    case WM_COMMAND: if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd); return 0;
    case WM_TIMER:
        if (wp == kNoticeTimer) {
            refreshNoticeCountdown(n);
            if (tickMs() - n->startMs >= (unsigned long long)n->seconds * 1000) DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kNoticeTimer);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete n;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool isNoticeWindow(HWND top)
{
    wchar_t cls[64];
    return top && GetClassNameW(top, cls, 64) && wcscmp(cls, kNoticeClass) == 0;
}

// Opens a notice over the main window; later notices step down and to the right.
void showNotice(const std::wstring& title, const std::wstring& text, int seconds)
{
    static bool registered = false;
    static int shown = 0;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = noticeProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
        wc.lpszClassName = kNoticeClass;
        registered = RegisterClassW(&wc) != 0;
    }
    NoticeWindow* n = new NoticeWindow();
    n->text = text;
    n->seconds = seconds;
    n->startMs = 0;
    n->textCtl = n->countdown = n->ok = NULL;
    RECT mr;
    GetWindowRect(g_main, &mr);
    int w = 560, h = 260, step = 30 * (shown++ % 6);
    int x = mr.left + ((mr.right - mr.left) - w) / 2 + step, y = mr.top + ((mr.bottom - mr.top) - h) / 2 + step;
    HWND hw = CreateWindowExW(0, kNoticeClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, g_inst, n);
    if (!hw) { logEvent("notice window could not be created: " + lastErrorText((int)GetLastError())); delete n; return; }
    ShowWindow(hw, SW_SHOW);
    SetFocus(n->ok);
}

// ---------------------------------------------------------------- remote file

// Run Remote File sends BATCH: a batch file or a PowerShell script as text,
// which the agent writes to its batch folder and runs in a console window, or
// the name of an executable already in the client's shared folder, which the
// agent runs from there. Agents from kBatchAgent run batch files; scripts and
// executables need kRemoteFileAgent.
const char* const kBatchAgent = "0.2.7";
const char* const kRemoteFileAgent = "0.3.4";
// The text travels as two hexadecimal digits per byte inside one message, which
// may be 65536 bytes long.
const size_t kMaxBatchBytes = 30000;
const wchar_t* const kBatchTitle = L"Run Remote File";
enum { KindBat, KindPs1, KindExe };
const char* const kKindNames[3] = { "bat", "ps1", "exe" };

std::wstring g_batchText;       // text of the dialog, kept while Thunderhead runs
int g_batchKind = KindBat;      // the File type radio
std::wstring g_batchExe;        // Executable: name or relative path inside the client's shared folder
bool g_batchMaximized = true;   // Start maximized: the console window on the client opens maximized
bool g_batchFront = true;       // Bring to front: it is put above the client's other windows
int g_batchOnline = 0;          // checked online clients when the dialog opened
HFONT g_batchFont = NULL;

std::wstring windowText(HWND w)
{
    int len = GetWindowTextLengthW(w);
    std::wstring s((size_t)len + 1, L'\0');
    int got = GetWindowTextW(w, &s[0], len + 1);
    s.resize((size_t)(got > 0 ? got : 0));
    return s;
}

// Reads a batch file for the text box: UTF-16 or UTF-8 when the file starts
// with that byte order mark, otherwise UTF-8 when the bytes are valid UTF-8,
// otherwise the ANSI code page of this machine. Lines come back ending in CR LF.
bool readBatchFile(const std::wstring& path, std::wstring& text, std::wstring& error)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { error = widen(lastErrorText((int)GetLastError())); return false; }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > (LONGLONG)(2 * kMaxBatchBytes + 4)) {
        CloseHandle(f);
        error = widen(format("The file is larger than the %u bytes of text a remote batch file may have.", (unsigned)kMaxBatchBytes));
        return false;
    }
    std::string bytes((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    bool ok = bytes.empty() || (ReadFile(f, &bytes[0], (DWORD)bytes.size(), &got, NULL) && got == bytes.size());
    DWORD err = GetLastError();
    CloseHandle(f);
    if (!ok) { error = widen(lastErrorText((int)err)); return false; }

    std::wstring raw;
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF && (unsigned char)bytes[1] == 0xFE) {
        raw.assign((const wchar_t*)(bytes.data() + 2), (bytes.size() - 2) / 2);
    } else {
        size_t skip = bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB &&
                      (unsigned char)bytes[2] == 0xBF ? 3 : 0;
        const char* data = bytes.data() + skip;
        int len = (int)(bytes.size() - skip);
        UINT cp = CP_UTF8;
        int n = len ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, len, NULL, 0) : 0;
        if (len && n == 0) { cp = CP_ACP; n = MultiByteToWideChar(CP_ACP, 0, data, len, NULL, 0); }
        raw.assign((size_t)n, L'\0');
        if (n) MultiByteToWideChar(cp, 0, data, len, &raw[0], n);
    }
    text.clear();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == L'\r') continue;
        if (raw[i] == L'\n') text += L"\r\n"; else text += raw[i];
    }
    return true;
}

bool browseBatchFile(HWND owner, std::wstring& path)
{
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        MessageBoxW(owner, L"The file dialog could not be created.", kBatchTitle, MB_OK | MB_ICONERROR);
        return false;
    }
    const COMDLG_FILTERSPEC batTypes[2] = { { L"Batch files (*.bat;*.cmd)", L"*.bat;*.cmd" }, { L"All files (*.*)", L"*.*" } };
    const COMDLG_FILTERSPEC ps1Types[2] = { { L"PowerShell scripts (*.ps1)", L"*.ps1" }, { L"All files (*.*)", L"*.*" } };
    dlg->SetFileTypes(2, g_batchKind == KindPs1 ? ps1Types : batTypes);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"Select the file to load");
    dlg->SetOkButtonLabel(L"Load");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = NULL;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; CoTaskMemFree(p); ok = true; }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// The reminder shown before anything is sent. Returns true for "Yes, Execute".
bool confirmBatch(HWND owner)
{
    std::wstring content = widen(format("No error checking and no check for harmful code is done on this file. It is run on "
                                        "%d checked online client(s) exactly as written, with the rights Lightning Rod "
                                        "has on each client.\n\n", g_batchOnline)) +
                           L"Are you ABSOLUTELY sure you want this file to run on the remote client(s) ?";
    TASKDIALOG_BUTTON buttons[2] = { { 300, L"Yes, Execute" }, { 301, L"No, Cancel" } };
    TASKDIALOGCONFIG cc;
    memset(&cc, 0, sizeof(cc));
    cc.cbSize = sizeof(cc);
    cc.hwndParent = owner;
    cc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cc.pszWindowTitle = L"Execute Remote File";
    cc.pszMainIcon = TD_WARNING_ICON;
    cc.pszMainInstruction = L"This file is not checked in any way";
    cc.pszContent = content.c_str();
    cc.pButtons = buttons;
    cc.cButtons = 2;
    cc.nDefaultButton = 301;
    int answer = 0;
    return SUCCEEDED(TaskDialogIndirect(&cc, &answer, NULL, NULL)) && answer == 300;
}

// Copies what the dialog holds into the globals that outlive it.
void keepBatchDialog(HWND dlg)
{
    g_batchText = windowText(GetDlgItem(dlg, IDC_BATCH_TEXT));
    g_batchExe = trim(windowText(GetDlgItem(dlg, IDC_BATCH_EXE)));
    if (IsDlgButtonChecked(dlg, IDC_BATCH_KIND_EXE) == BST_CHECKED) g_batchKind = KindExe;
    else if (IsDlgButtonChecked(dlg, IDC_BATCH_KIND_PS1) == BST_CHECKED) g_batchKind = KindPs1;
    else g_batchKind = KindBat;
    g_batchMaximized = IsDlgButtonChecked(dlg, IDC_BATCH_MAXIMIZED) == BST_CHECKED;
    g_batchFront = IsDlgButtonChecked(dlg, IDC_BATCH_FRONT) == BST_CHECKED;
}

// The File type radios: a batch file or a script is typed or loaded into the
// text box; an executable is named in its own box and the text box is grayed.
void batchKindChanged(HWND dlg, int kind)
{
    bool exe = kind == KindExe;
    EnableWindow(GetDlgItem(dlg, IDC_BATCH_TEXT), !exe);
    EnableWindow(GetDlgItem(dlg, IDC_BATCH_BROWSE), !exe);
    EnableWindow(GetDlgItem(dlg, IDC_BATCH_EXE_LABEL), exe);
    EnableWindow(GetDlgItem(dlg, IDC_BATCH_EXE), exe);
    std::string info = exe
        ? format("Executable to run on %d checked online client(s): a file already in each client's shared folder "
                 "(Copy Files or Folder to Client puts it there).", g_batchOnline)
        : format("Text of the %s to run on %d checked online client(s). Type it, paste it, or load a file with Browse for file.",
                 kind == KindPs1 ? "PowerShell script" : "batch file", g_batchOnline);
    SetDlgItemTextW(dlg, IDC_BATCH_INFO, widen(info).c_str());
}

INT_PTR CALLBACK batchDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    HWND edit = GetDlgItem(dlg, IDC_BATCH_TEXT);
    switch (msg) {
    case WM_INITDIALOG: {
        if (!g_batchFont) {
            HDC dc = GetDC(dlg);
            g_batchFont = CreateFontW(-MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                      FIXED_PITCH | FF_MODERN, L"Consolas");
            ReleaseDC(dlg, dc);
        }
        if (g_batchFont) SendMessageW(edit, WM_SETFONT, (WPARAM)g_batchFont, TRUE);
        SendMessageW(edit, EM_SETLIMITTEXT, kMaxBatchBytes, 0);
        SetWindowTextW(edit, g_batchText.c_str());
        SetDlgItemTextW(dlg, IDC_BATCH_EXE, g_batchExe.c_str());
        SendDlgItemMessageW(dlg, IDC_BATCH_EXE, EM_LIMITTEXT, 200, 0);
        CheckRadioButton(dlg, IDC_BATCH_KIND_BAT, IDC_BATCH_KIND_EXE, IDC_BATCH_KIND_BAT + g_batchKind);
        CheckDlgButton(dlg, IDC_BATCH_MAXIMIZED, g_batchMaximized ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_BATCH_FRONT, g_batchFront ? BST_CHECKED : BST_UNCHECKED);
        batchKindChanged(dlg, g_batchKind);
        SetFocus(g_batchKind == KindExe ? GetDlgItem(dlg, IDC_BATCH_EXE) : edit);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BATCH_KIND_BAT:
        case IDC_BATCH_KIND_PS1:
        case IDC_BATCH_KIND_EXE:
            batchKindChanged(dlg, LOWORD(wp) - IDC_BATCH_KIND_BAT);
            return TRUE;
        case IDC_BATCH_BROWSE: {
            keepBatchDialog(dlg);
            std::wstring path, text, error;
            if (!browseBatchFile(dlg, path)) return TRUE;
            if (!readBatchFile(path, text, error)) {
                MessageBoxW(dlg, (path + L"\n\n" + error).c_str(), kBatchTitle, MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            SetWindowTextW(edit, text.c_str());
            SetFocus(edit);
            return TRUE;
        }
        case IDC_BATCH_EXECUTE: {
            keepBatchDialog(dlg);
            if (g_batchKind == KindExe) {
                if (g_batchExe.empty()) {
                    MessageBoxW(dlg, L"Enter the name of the executable in the client's shared folder.", kBatchTitle, MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                if (g_batchExe.find(L':') != std::wstring::npos || g_batchExe[0] == L'\\' || g_batchExe[0] == L'/' ||
                    g_batchExe.find(L"..") != std::wstring::npos) {
                    MessageBoxW(dlg, L"Give the executable as a name or a path relative to the shared folder, for example tools\\setup.exe.",
                                kBatchTitle, MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            } else {
                if (trim(g_batchText).empty()) {
                    MessageBoxW(dlg, g_batchKind == KindPs1 ? L"The script text is empty." : L"The batch file text is empty.", kBatchTitle, MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                size_t bytes = narrow(g_batchText).size();
                if (bytes > kMaxBatchBytes) {
                    MessageBoxW(dlg, widen(format("The text is %u bytes; a remote file may have %u at most.",
                                                  (unsigned)bytes, (unsigned)kMaxBatchBytes)).c_str(), kBatchTitle, MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            }
            if (confirmBatch(dlg)) EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            keepBatchDialog(dlg);
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Run Remote File: asks for the file, confirms, and sends it to every checked
// online client. A batch file or script goes as text to every client at once;
// an executable is checked first in each client's shared folder, which is
// reachable from here unless the client sits behind an SMB1 relay, and a
// client without the file gets a notice instead of the command.
void runRemoteFile()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    g_batchOnline = 0;
    for (size_t i = 0; i < targets.size(); ++i) if (targets[i]->online) ++g_batchOnline;
    if (g_batchOnline == 0) {
        MessageBoxW(g_main, L"Check the online clients to run the file on first.", kBatchTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BATCH), g_main, batchDlgProc, 0) != IDOK) return;
    Message m("BATCH");
    m.set("kind", std::string(kKindNames[g_batchKind]));
    m.set("maximized", std::string(boolText(g_batchMaximized)));
    m.set("front", std::string(boolText(g_batchFront)));
    if (g_batchKind != KindExe) {
        std::string utf8 = narrow(g_batchText);
        m.set("text", hexEncode(utf8));
        sendVersioned(m, format("BATCH %s (%u bytes of text%s%s)", kKindNames[g_batchKind], (unsigned)utf8.size(),
                                g_batchMaximized ? ", maximized" : "", g_batchFront ? ", to the front" : ""),
                      g_batchKind == KindBat ? kBatchAgent : kRemoteFileAgent);
        return;
    }
    std::string file = narrow(g_batchExe);
    m.set("file", file);
    std::string label = "BATCH exe " + file + (g_batchMaximized ? ", maximized" : "") + (g_batchFront ? ", to the front" : "");
    int sent = 0, missing = 0, old = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        std::string agent = n->hello.get("agent");
        if (compareVersions(agent, kRemoteFileAgent) < 0) {
            ++old;
            logEvent(format("%s not sent to %s: agent %s is older than %s, update it first", label.c_str(), n->name.c_str(), agent.c_str(), kRemoteFileAgent));
            continue;
        }
        if (!relayFor(n) && !n->ip.empty()) {
            std::wstring path = shareRootOf(n) + g_batchExe;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                ++missing;
                logEvent(format("%s not sent to %s: %s does not exist", label.c_str(), n->name.c_str(), narrow(path).c_str()));
                showNotice(kBatchTitle, L"File does not exist on " + widen(n->name) + L":\n" + path +
                           L"\n\nUse the 'Copy Files or Folder to Client' button to place it, and then try again.", 10);
                continue;
            }
        }
        if (sendToNode(n, m, label)) ++sent;
    }
    std::string status = format("|  %s sent to %d of %d checked", label.c_str(), sent, (int)targets.size());
    if (missing) status += format(", %d without the file", missing);
    if (old) status += format(", %d skipped: agent older than %s", old, kRemoteFileAgent);
    updateStatusBar(widen(status).c_str());
}

// ---------------------------------------------------------------- log viewer

// View Local Log and View Remote Log(s): a top-level window per log with a
// read-only, scrollable text control holding the file. The file is read on a
// worker thread, so a share that does not answer stalls only its own window.
// Refresh, or F5 in the text, reads it again; the view opens scrolled to the
// end; Ctrl+A selects everything for copying. Nothing can be typed into it.
const UINT WM_LOG_LOADED = WM_APP + 3;   // lParam: LogText*
const int IDC_LOG_TEXT = 2501, IDC_LOG_REFRESH = 2502, IDC_LOG_INFO = 2503;
const unsigned long long kLogShowBytes = 4u << 20;   // at most the last 4 MB of a file are shown

// What a log window reads: the newest file matching pattern in the first of
// dirs that has one. For a remote log, \\NAME\share\ is appended to dirs on
// the worker thread when name resolves to ip, as the agent update does.
struct LogSource {
    std::vector<std::wstring> dirs;
    std::string name, ip;
    std::wstring share, pattern;
    bool viaRelay;                   // the client's share is SMB1: RELAY op=pull through the relay
    int relayConn;
    std::string relayName, relayIp;
    LogSource() : viaRelay(false), relayConn(-1) {}
};
struct LogView { std::wstring title; LogSource src; HWND text, refresh, info; bool loading, prompted; };
struct LogJob { HWND window; LogSource src; };
struct LogText { bool ok; std::wstring file, text, info; bool needCredentials; std::wstring credDir; };
HFONT g_logFont = NULL;

// The newest file matching pattern in dir, by name, which carries the date.
std::wstring newestFile(const std::wstring& dir, const std::wstring& pattern, std::string& err, int& code)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { code = (int)GetLastError(); err = lastErrorText(code); return std::wstring(); }
    std::wstring best;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (best.empty() || _wcsicmp(fd.cFileName, best.c_str()) > 0))
            best = fd.cFileName;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty()) { code = ERROR_FILE_NOT_FOUND; err = "no such file"; return std::wstring(); }
    return dir + best;
}

// Reads a UTF-8 log file into text with CR LF line ends for the edit control.
// A file above kLogShowBytes is cut to its last kLogShowBytes at a line end.
bool readLogFile(const std::wstring& path, std::wstring& text, std::wstring& info, std::string& err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { err = lastErrorText((int)GetLastError()); return false; }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { err = lastErrorText((int)GetLastError()); CloseHandle(h); return false; }
    unsigned long long total = (unsigned long long)size.QuadPart, skip = 0;
    if (total > kLogShowBytes) {
        skip = total - kLogShowBytes;
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)skip;
        SetFilePointerEx(h, pos, NULL, FILE_BEGIN);
    }
    std::string bytes((size_t)(total - skip), '\0');
    size_t got = 0;
    while (got < bytes.size()) {
        DWORD n = 0;
        if (!ReadFile(h, &bytes[got], (DWORD)(bytes.size() - got), &n, NULL)) {
            err = lastErrorText((int)GetLastError());
            CloseHandle(h);
            return false;
        }
        if (n == 0) break;
        got += n;
    }
    CloseHandle(h);
    bytes.resize(got);
    if (skip) {
        size_t nl = bytes.find('\n');
        bytes = nl == std::string::npos ? std::string() : bytes.substr(nl + 1);
    }
    std::string fixed;
    fixed.reserve(bytes.size() + bytes.size() / 16);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r') continue;
        if (bytes[i] == '\n') fixed += "\r\n"; else fixed += bytes[i];
    }
    text = widen(fixed);
    if (skip) text = L"[the first " + widen(format("%llu", skip)) + L" bytes of the file are not shown]\r\n" + text;
    SYSTEMTIME st;
    GetLocalTime(&st);
    info = widen(format("%llu bytes, read at %02d:%02d:%02d", total, st.wHour, st.wMinute, st.wSecond));
    return true;
}

DWORD WINAPI logLoadThread(LPVOID p)
{
    LogJob* job = (LogJob*)p;
    HWND window = job->window;
    LogText* res = new LogText();
    res->ok = false;
    res->needCredentials = false;
    if (job->src.viaRelay) {
        std::string relayJob = newRelayJob(), err;
        Message m = relayMessage("pull", relayJob, job->src.ip, job->src.share);
        m.set("pattern", narrow(job->src.pattern));
        Message ack;
        std::wstring stage = relayStageDir(job->src.relayIp, job->src.share, relayJob);
        if (!relayRequest(job->src.relayConn, m, 2 * 60 * 1000, ack, err)) {
            res->text = widen("relay " + job->src.relayName + ": " + err);
        } else {
            std::wstring file = stage + widen(ack.get("file"));
            if (readLogFile(file, res->text, res->info, err)) {
                res->ok = true;
                res->file = L"\\\\" + widen(job->src.ip) + L"\\" + job->src.share + L"\\" + widen(ack.get("file"));
                res->info += L", through relay " + widen(job->src.relayName);
            } else {
                res->text = widen("the relayed copy " + narrow(file) + " could not be read: " + err);
            }
            removeTree(stage);
        }
        delete job;
        if (!PostMessageW(window, WM_LOG_LOADED, 0, (LPARAM)res)) delete res;
        return 0;
    }
    std::vector<std::wstring> dirs = job->src.dirs;
    if (!job->src.name.empty() && (job->src.ip.empty() || nameResolvesTo(job->src.name, job->src.ip)))
        dirs.push_back(L"\\\\" + widen(job->src.name) + L"\\" + job->src.share + L"\\");
    std::string errors;
    for (size_t i = 0; i < dirs.size() && !res->ok; ++i) {
        std::string err;
        int code = 0;
        std::wstring file = newestFile(dirs[i], job->src.pattern, err, code);
        if (file.empty()) {
            errors += (errors.empty() ? "" : "\r\n") + narrow(dirs[i] + job->src.pattern) + ": " + err;
            if (!res->needCredentials && isCredentialError(code)) { res->needCredentials = true; res->credDir = dirs[i]; }
        } else if (readLogFile(file, res->text, res->info, err)) {
            res->ok = true;
            res->file = file;
        } else {
            errors += (errors.empty() ? "" : "\r\n") + narrow(file) + ": " + err;
        }
    }
    if (!res->ok) res->text = widen(errors.empty() ? std::string("nowhere to look for the log") : errors);
    delete job;
    if (!PostMessageW(window, WM_LOG_LOADED, 0, (LPARAM)res)) delete res;   // the window is gone
    return 0;
}

void startLogLoad(HWND hwnd, LogView* v)
{
    if (v->loading) return;
    v->loading = true;
    SetWindowTextW(v->info, L"reading...");
    LogJob* job = new LogJob();
    job->window = hwnd;
    job->src = v->src;
    HANDLE t = CreateThread(NULL, 0, logLoadThread, job, 0, NULL);
    if (t) CloseHandle(t);
    else { delete job; v->loading = false; SetWindowTextW(v->info, L"could not start a thread"); }
}

// F5 in the text reads the file again; Ctrl+A selects everything.
LRESULT CALLBACK logEditProc(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_KEYDOWN && wp == VK_F5) { PostMessageW(GetParent(w), WM_COMMAND, IDC_LOG_REFRESH, 0); return 0; }
    if (msg == WM_CHAR && wp == 1) { SendMessageW(w, EM_SETSEL, 0, -1); return 0; }
    return DefSubclassProc(w, msg, wp, lp);
}

void layoutLogView(HWND hwnd, LogView* v)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int bar = 32, margin = 6;
    MoveWindow(v->text, 0, 0, rc.right, rc.bottom - bar, TRUE);
    MoveWindow(v->refresh, margin, rc.bottom - bar + 4, 100, 24, TRUE);
    MoveWindow(v->info, margin + 110, rc.bottom - bar + 8, rc.right - margin - 116, 20, TRUE);
}

LRESULT CALLBACK logViewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LogView* v = (LogView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_CREATE) {
        v = (LogView*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)v);
        v->text = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                                  ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                  0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_TEXT, g_inst, NULL);
        SendMessageW(v->text, EM_SETLIMITTEXT, 0, 0);   // no limit beyond memory
        if (!g_logFont) {
            HDC dc = GetDC(hwnd);
            g_logFont = CreateFontW(-MulDiv(10, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    FIXED_PITCH | FF_MODERN, L"Consolas");
            ReleaseDC(hwnd, dc);
        }
        SendMessageW(v->text, WM_SETFONT, (WPARAM)g_logFont, TRUE);
        SetWindowSubclass(v->text, logEditProc, 1, 0);
        v->refresh = CreateWindowExW(0, L"BUTTON", L"Refresh (F5)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                     0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_REFRESH, g_inst, NULL);
        SendMessageW(v->refresh, WM_SETFONT, (WPARAM)g_font, TRUE);
        v->info = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS,
                                  0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_LOG_INFO, g_inst, NULL);
        SendMessageW(v->info, WM_SETFONT, (WPARAM)g_font, TRUE);
        v->loading = false;
        themeWindow(hwnd);
        startLogLoad(hwnd, v);
        return 0;
    }
    if (!v) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_THEME_CHANGED: themeWindow(hwnd); return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, themeBrush(theme().paneBack));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        if (HBRUSH b = themeCtlColor(msg, (HDC)wp, (HWND)lp)) return (LRESULT)b;
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->hwndItem == v->refresh) return themeDrawPushButton(di) ? TRUE : FALSE;
        break;
    }
    case WM_SIZE: layoutLogView(hwnd, v); return 0;
    case WM_SETFOCUS: SetFocus(v->text); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_LOG_REFRESH) startLogLoad(hwnd, v);
        return 0;
    case WM_LOG_LOADED: {
        LogText* r = (LogText*)lp;
        v->loading = false;
        if (!r->ok && r->needCredentials && !v->prompted) {
            v->prompted = true;
            DWORD e = promptShareCredentials(hwnd, r->credDir);
            if (e == NO_ERROR) { delete r; startLogLoad(hwnd, v); return 0; }
            r->text += e == ERROR_CANCELLED ? std::wstring(L"\r\n\r\nThe credential prompt was cancelled.")
                                            : L"\r\n\r\n" + widen(lastErrorText((int)e));
        }
        SetWindowTextW(v->text, r->text.c_str());
        if (r->ok) {
            SetWindowTextW(hwnd, (v->title + L"   " + r->file).c_str());
            SetWindowTextW(v->info, (r->file + L"   " + r->info).c_str());
            int len = GetWindowTextLengthW(v->text);
            SendMessageW(v->text, EM_SETSEL, len, len);
            SendMessageW(v->text, EM_SCROLLCARET, 0, 0);
        } else {
            SetWindowTextW(hwnd, (v->title + L"   not read").c_str());
            SetWindowTextW(v->info, L"the log could not be read; the text gives the reason");
        }
        delete r;
        return 0;
    }
    case WM_DESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete v;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void openLogView(const std::wstring& title, const LogSource& src)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = logViewProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(IDI_APP));
        wc.lpszClassName = L"ThunderheadLogView";
        RegisterClassW(&wc);
        registered = true;
    }
    LogView* v = new LogView();
    v->title = title;
    v->src = src;
    v->text = v->refresh = v->info = NULL;
    v->loading = false;
    v->prompted = false;
    HWND w = CreateWindowExW(0, L"ThunderheadLogView", (title + L"   reading").c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1000, 600, NULL, NULL, g_inst, v);
    if (!w) { logEvent("log window could not be created: " + lastErrorText((int)GetLastError())); return; }
    ShowWindow(w, SW_SHOW);
}

// View Local Log: the current Thunderhead event log, logs\thunderhead-YYYY-MM-DD.log.
void viewLocalLog()
{
    LogSource src;
    src.dirs.push_back(g_dir + L"\\logs\\");
    src.pattern = L"thunderhead-*.log";
    openLogView(L"Thunderhead log", src);
}

// View Remote Log(s): the agent's own log of every checked client, which it
// writes as LightningRod-YYYY-MM-DD.log beside itself, in the root of the
// node's share. The share is read under the address the client last reported
// from and under its name, as the update does. A client that is offline is
// tried as well, since its share may answer when its agent does not.
void viewRemoteLogs()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    int opened = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (n->ip.empty() && n->name.empty()) { logEvent(n->mac + " has no name or address yet; log not opened"); continue; }
        if (!commandAllowed(n, "View Remote Log")) continue;
        if (NodeRecord* relay = relayFor(n)) { if (!commandAllowed(relay, "View Remote Log (as relay for " + n->name + ")")) continue; }
        LogSource src;
        src.dirs.push_back(shareRootOf(n));
        src.name = n->name;
        src.ip = n->ip;
        src.share = widen(g_store->settings.shareName);
        src.pattern = L"LightningRod-*.log";
        NodeRecord* relay = relayFor(n);
        if (relay) {
            src.viaRelay = true;
            src.relayConn = relay->conn;
            src.relayName = relay->name;
            src.relayIp = relay->ip;
            src.dirs.clear();
        }
        std::string label = n->name.empty() ? n->mac : n->name;
        openLogView(widen(label) + L" log", src);
        ++opened;
        logEvent(format("View Remote Log: %s, \\\\%s\\%s\\LightningRod-*.log%s%s", label.c_str(),
                        n->ip.empty() ? n->name.c_str() : n->ip.c_str(), g_store->settings.shareName.c_str(),
                        relay ? (" through relay " + relay->name).c_str() : "",
                        n->online ? "" : " (offline, last known address)"));
    }
    updateStatusBar(widen(format("|  %d log window(s) opened", opened)).c_str());
}

// ---------------------------------------------------------------- rename

struct RenameRequest { std::wstring info; std::wstring name; bool reboot; };

INT_PTR CALLBACK renameDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    static RenameRequest* req = NULL;
    switch (msg) {
    case WM_INITDIALOG:
        req = (RenameRequest*)lp;
        SetDlgItemTextW(dlg, IDC_RENAME_INFO, req->info.c_str());
        SetDlgItemTextW(dlg, IDC_RENAME_NAME, req->name.c_str());
        SendDlgItemMessageW(dlg, IDC_RENAME_NAME, EM_LIMITTEXT, 15, 0);
        SendDlgItemMessageW(dlg, IDC_RENAME_NAME, EM_SETSEL, 0, -1);
        CheckDlgButton(dlg, IDC_RENAME_REBOOT, req->reboot ? BST_CHECKED : BST_UNCHECKED);
        SetFocus(GetDlgItem(dlg, IDC_RENAME_NAME));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[64];
            GetDlgItemTextW(dlg, IDC_RENAME_NAME, buf, 64);
            req->name = trim(std::wstring(buf));
            req->reboot = IsDlgButtonChecked(dlg, IDC_RENAME_REBOOT) == BST_CHECKED;
            if (req->name.empty()) { MessageBoxW(dlg, L"Enter a name.", L"Rename node", MB_OK | MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDABORT) { EndDialog(dlg, LOWORD(wp)); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- Set Relay Address

// The relay values of a Windows 7 client, set from here instead of at the
// node's screen: one dialog per checked online Windows 7 client, in list
// order, with its current values; OK sends RELAYSET when something changed,
// Skip leaves the client as it is, Cancel All ends the pass. Clients on other
// Windows versions and agents before kRelaySetAgent are passed over with a
// line in the event log.
const char* const kRelaySetAgent = "0.3.1";

struct RelayDlgData { std::wstring info; bool on; std::string ip; std::string ownIp, ownName; };

bool isWindows7(const NodeRecord* n)
{
    return n->hello.get("os").find("(6.1.") != std::string::npos;   // Windows 7, Windows Embedded Standard 7
}

bool validIpText(const std::string& s)
{
    std::vector<std::string> parts = split(s, '.');
    if (parts.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (parts[i].empty() || parts[i].size() > 3) return false;
        for (size_t k = 0; k < parts[i].size(); ++k) if (parts[i][k] < '0' || parts[i][k] > '9') return false;
        if (atoi(parts[i].c_str()) > 255) return false;
    }
    return true;
}

// A relay destination: an IPv4 address or a computer name (letters, digits,
// hyphens and dots, 63 characters at most), as agents from 0.5.7 accept it.
bool validRelayTargetText(const std::string& s)
{
    if (validIpText(s)) return true;
    if (s.empty() || s.size() > 63 || s[0] == '-' || s[0] == '.') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return true;
}

INT_PTR CALLBACK relayDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    RelayDlgData* d = (RelayDlgData*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_DRAWITEM && ((const DRAWITEMSTRUCT*)lp)->CtlID == IDC_RELAY_SWITCH) {
        themeDrawSwitch((const DRAWITEMSTRUCT*)lp, d && d->on);
        return TRUE;
    }
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    switch (msg) {
    case WM_INITDIALOG:
        d = (RelayDlgData*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_RELAY_INFO, d->info.c_str());
        SetDlgItemTextW(dlg, IDC_RELAY_STATE, d->on ? L"On" : L"Off");
        SetDlgItemTextW(dlg, IDC_RELAY_DEST, widen(d->ip).c_str());
        SendDlgItemMessageW(dlg, IDC_RELAY_DEST, EM_LIMITTEXT, 63, 0);
        EnableWindow(GetDlgItem(dlg, IDC_RELAY_DEST), d->on);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RELAY_SWITCH && d) {
            d->on = !d->on;
            SetDlgItemTextW(dlg, IDC_RELAY_STATE, d->on ? L"On" : L"Off");
            InvalidateRect(GetDlgItem(dlg, IDC_RELAY_SWITCH), NULL, TRUE);
            EnableWindow(GetDlgItem(dlg, IDC_RELAY_DEST), d->on);
            if (d->on) { SetFocus(GetDlgItem(dlg, IDC_RELAY_DEST)); SendDlgItemMessageW(dlg, IDC_RELAY_DEST, EM_SETSEL, 0, -1); }
            return TRUE;
        }
        if (LOWORD(wp) == IDOK && d) {
            wchar_t buf[64];
            GetDlgItemTextW(dlg, IDC_RELAY_DEST, buf, 64);
            std::string ip = trim(narrow(buf));
            if (d->on && !validRelayTargetText(ip)) {
                MessageBoxW(dlg, L"Enter the destination as the Windows XP client's computer name, for example LATITUDE, or as an IP address of four numbers, for example 42.32.22.156. A name keeps the relay working when the client's DHCP address changes (agents from 0.5.7).", L"Set Relay Address", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (d->on && (ip == d->ownIp || _stricmp(ip.c_str(), d->ownName.c_str()) == 0)) {
                MessageBoxW(dlg, L"That is the client itself. Enter the name or address of the Windows XP client it relays to.", L"Set Relay Address", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (d->on) d->ip = ip;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDABORT) { EndDialog(dlg, LOWORD(wp)); return TRUE; }
        break;
    }
    return FALSE;
}

void setRelayAddress()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    int sent = 0, shown = 0, skipped = 0, passed = 0;
    bool cancelled = false;
    for (size_t i = 0; i < targets.size() && !cancelled; ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) { logEvent(format("Set Relay Address: %s is offline, passed over", n->name.c_str())); ++passed; continue; }
        if (!isWindows7(n)) { logEvent(format("Set Relay Address: %s runs %s, not Windows 7, passed over", n->name.c_str(), n->hello.get("os").c_str())); ++passed; continue; }
        std::string agent = n->hello.get("agent");
        if (compareVersions(agent, kRelaySetAgent) < 0) {
            logEvent(format("Set Relay Address: %s has agent %s, older than %s, passed over; update it first", n->name.c_str(), agent.c_str(), kRelaySetAgent));
            ++passed;
            continue;
        }
        RelayDlgData d;
        d.info = L"Client " + widen(n->name) + L"  (" + widen(n->ip) + L")";
        d.on = isRelay(n);
        d.ip = relayTarget(n);
        d.ownIp = n->ip;
        d.ownName = n->name;
        bool wasOn = d.on;
        std::string wasIp = d.ip;
        ++shown;
        INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_RELAY), g_main, relayDlgProc, (LPARAM)&d);
        if (r == IDABORT) { cancelled = true; break; }
        if (r != IDOK) { ++skipped; continue; }
        if (d.on == wasOn && (!d.on || d.ip == wasIp)) { logEvent(format("Set Relay Address: %s unchanged", n->name.c_str())); continue; }
        if (!n->online) { logEvent(format("Set Relay Address: %s went offline before RELAYSET was sent", n->name.c_str())); continue; }
        Message m("RELAYSET");
        m.set("relay", boolText(d.on));
        m.set("relay_ip", d.on ? d.ip : std::string());
        if (sendToNode(n, m, m.oneLine())) ++sent;
    }
    updateStatusBar(widen(format("|  Set Relay Address: %d dialog(s), %d sent, %d skipped, %d passed over%s", shown, sent, skipped, passed,
                                 cancelled ? ", cancelled" : "")).c_str());
}

// ---------------------------------------------------------------- Display Message

// A message window on a client's screen: one dialog per checked online
// client, in list order, naming the client. The radio buttons are the sound
// categories, each of which titles the message "Message from Thunderhead:
// <Category>" and plays the category's file from the client's own sounds
// folder (the custom scheme's assignment when there is one), and Silent
// message, titled "Message from Thunderhead" with no sound. The text box holds
// the message text (it may be empty), the switch chooses Until Dismissed or
// Timed with the seconds beside it; the choices carry over from one dialog to
// the next. Send sends MESSAGE, Skip leaves the client without one, Cancel All
// ends the pass. Agents before kMessageAgent are passed over. The agent
// answers when the window opens and again when it closes (see onNet).
const char* const kMessageAgent = "0.3.3";
const int kMessageSilent = kSoundCategoryCount;     // the radio after the categories
const int kMessageMaxSeconds = 86400;

struct MessageDlgData { std::wstring info; int choice; std::wstring text; bool timed; int seconds; };

std::wstring messageTitle(int choice)
{
    if (choice == kMessageSilent) return L"Message from Thunderhead";
    return std::wstring(L"Message from Thunderhead: ") + kSoundCategories[choice];
}

RECT dialogRect(HWND dlg, int x, int y, int w, int h)
{
    RECT r = { x, y, x + w, y + h };
    MapDialogRect(dlg, &r);
    return r;
}

INT_PTR CALLBACK messageDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    MessageDlgData* d = (MessageDlgData*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_DRAWITEM && ((const DRAWITEMSTRUCT*)lp)->CtlID == IDC_MSG_TIMED_SWITCH) {
        themeDrawSwitch((const DRAWITEMSTRUCT*)lp, d && d->timed);
        return TRUE;
    }
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    switch (msg) {
    case WM_INITDIALOG: {
        d = (MessageDlgData*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_MSG_INFO, d->info.c_str());
        // The radios: three columns of thirteen, the categories in their order,
        // Silent message last; placed after the first line in the tab order.
        const int perColumn = (kMessageSilent + 1 + 2) / 3;
        HWND after = GetDlgItem(dlg, IDC_MSG_INFO);
        for (int i = 0; i <= kMessageSilent; ++i) {
            RECT r = dialogRect(dlg, 10 + 150 * (i / perColumn), 36 + 13 * (i % perColumn), 145, 11);
            const wchar_t* label = i == kMessageSilent ? L"Silent message" : kSoundCategories[i];
            HWND b = CreateWindowExW(0, L"BUTTON", label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0),
                                     r.left, r.top, r.right - r.left, r.bottom - r.top, dlg, (HMENU)(INT_PTR)(IDC_MSG_RADIO_FIRST + i), g_inst, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
            SetWindowPos(b, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            themeControl(b);
            after = b;
        }
        CheckRadioButton(dlg, IDC_MSG_RADIO_FIRST, IDC_MSG_RADIO_FIRST + kMessageSilent, IDC_MSG_RADIO_FIRST + d->choice);
        SetDlgItemTextW(dlg, IDC_MSG_TEXT, d->text.c_str());
        SendDlgItemMessageW(dlg, IDC_MSG_TEXT, EM_LIMITTEXT, 4000, 0);
        SetDlgItemTextW(dlg, IDC_MSG_TIMED_STATE, d->timed ? L"Timed" : L"Until Dismissed");
        SetDlgItemInt(dlg, IDC_MSG_SECONDS, (UINT)d->seconds, FALSE);
        SendDlgItemMessageW(dlg, IDC_MSG_SECONDS, EM_LIMITTEXT, 5, 0);
        EnableWindow(GetDlgItem(dlg, IDC_MSG_SECONDS), d->timed);
        SetFocus(GetDlgItem(dlg, IDC_MSG_TEXT));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_MSG_TIMED_SWITCH && d) {
            d->timed = !d->timed;
            SetDlgItemTextW(dlg, IDC_MSG_TIMED_STATE, d->timed ? L"Timed" : L"Until Dismissed");
            InvalidateRect(GetDlgItem(dlg, IDC_MSG_TIMED_SWITCH), NULL, TRUE);
            EnableWindow(GetDlgItem(dlg, IDC_MSG_SECONDS), d->timed);
            if (d->timed) { SetFocus(GetDlgItem(dlg, IDC_MSG_SECONDS)); SendDlgItemMessageW(dlg, IDC_MSG_SECONDS, EM_SETSEL, 0, -1); }
            return TRUE;
        }
        if (LOWORD(wp) == IDOK && d) {
            for (int i = 0; i <= kMessageSilent; ++i)
                if (IsDlgButtonChecked(dlg, IDC_MSG_RADIO_FIRST + i) == BST_CHECKED) d->choice = i;
            d->text = windowText(GetDlgItem(dlg, IDC_MSG_TEXT));
            if (d->timed) {
                BOOL okNum = FALSE;
                UINT sec = GetDlgItemInt(dlg, IDC_MSG_SECONDS, &okNum, FALSE);
                if (!okNum || sec < 1 || sec > (UINT)kMessageMaxSeconds) {
                    MessageBoxW(dlg, L"Enter the seconds the message stays on the client's screen, 1 to 86400.", L"Display Message", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                d->seconds = (int)sec;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDABORT) { EndDialog(dlg, LOWORD(wp)); return TRUE; }
        break;
    }
    return FALSE;
}

void displayMessage()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    static MessageDlgData last;
    static bool lastSet = false;
    if (!lastSet) {
        int def = soundCategoryIndex(L"Default");
        last.choice = def < 0 ? 0 : def;
        last.timed = false;
        last.seconds = 30;
        lastSet = true;
    }
    int sent = 0, shown = 0, skipped = 0, passed = 0;
    bool cancelled = false;
    for (size_t i = 0; i < targets.size() && !cancelled; ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) { logEvent(format("Display Message: %s is offline, passed over", n->name.c_str())); ++passed; continue; }
        std::string agent = n->hello.get("agent");
        if (compareVersions(agent, kMessageAgent) < 0) {
            logEvent(format("Display Message: %s has agent %s, older than %s, passed over; update it first", n->name.c_str(), agent.c_str(), kMessageAgent));
            ++passed;
            continue;
        }
        MessageDlgData d = last;
        d.info = L"Client " + widen(n->name) + L"  (" + widen(n->ip) + L")" + widen(format("   %d of %d checked", (int)i + 1, (int)targets.size()));
        ++shown;
        INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_MESSAGE), g_main, messageDlgProc, (LPARAM)&d);
        if (r == IDABORT) { cancelled = true; break; }
        if (r != IDOK) { ++skipped; continue; }
        last = d;
        if (!n->online) { logEvent(format("Display Message: %s went offline before MESSAGE was sent", n->name.c_str())); continue; }
        std::wstring title = messageTitle(d.choice);
        Message m("MESSAGE");
        m.set("title", narrow(title));
        m.set("sound", d.choice == kMessageSilent ? std::string() : narrow(soundNameForCategory(kSoundCategories[d.choice])));
        m.set("text", hexEncode(narrow(d.text)));
        m.set("timeout", (long long)(d.timed ? d.seconds : 0));
        std::string when = d.timed ? format("closes after %d s", d.seconds) : std::string("until dismissed");
        std::string label = format("MESSAGE \"%s\", %u character(s), %s", narrow(title).c_str(), (unsigned)d.text.size(), when.c_str());
        if (sendToNode(n, m, label)) ++sent;
    }
    updateStatusBar(widen(format("|  Display Message: %d dialog(s), %d sent, %d skipped, %d passed over%s", shown, sent, skipped, passed,
                                 cancelled ? ", cancelled" : "")).c_str());
}

// One dialog per checked online client, in list order; Skip moves on, Stop ends the pass.
// Opens \NAME\<share_name>\ of every checked client in Windows Explorer. The
// name is used rather than the address so that the window title shows the node.
// ShellExecute does not return until the name is resolved and the share found,
// which takes up to a minute for a node that is off, so the opens run on a
// thread of their own and the window stays usable.
DWORD WINAPI openSharedFoldersThread(LPVOID param)
{
    std::vector<std::string>* paths = (std::vector<std::string>*)param;
    for (size_t i = 0; i < paths->size(); ++i) {
        const std::string& path = (*paths)[i];
        HINSTANCE r = ShellExecuteW(NULL, L"open", widen(path).c_str(), NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) logEvent(format("open %s failed (ShellExecute %d)", path.c_str(), (int)(INT_PTR)r));
        else logEvent("opened " + path);
    }
    delete paths;
    return 0;
}

void openSharedFolders()
{
    std::vector<NodeRecord*> nodes = checkedNodes();
    if (nodes.empty()) { logEvent("Open shared folder: no clients checked"); return; }
    std::vector<std::string>* paths = new std::vector<std::string>();
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (n->name.empty()) { logEvent(format("%s has no name yet; shared folder not opened", n->mac.c_str())); continue; }
        if (NodeRecord* relay = relayFor(n)) {
            logEvent(format("%s: its share is SMB1, reached through relay %s; it cannot be opened from this host", n->name.c_str(), relay->name.c_str()));
            continue;
        }
        paths->push_back(narrow(shareRootOf(n)));
        // The folder opening in Explorer is where the credentials get typed; the
        // share is checked again every few seconds for a while so that the row
        // clears as soon as they are in.
        n->shareRetryUntilMs = tickMs() + 120000;
        n->sharePrompted = false;   // the next refused check prompts again, should Explorer's own prompt not be used
        if (n->shareState != 1) n->shareNextMs = tickMs() + 5000;
    }
    if (paths->empty()) { delete paths; return; }
    logEvent(format("opening the shared folder of %u client(s)", (unsigned)paths->size()));
    HANDLE t = CreateThread(NULL, 0, openSharedFoldersThread, paths, 0, NULL);
    if (t) CloseHandle(t);
    else { logEvent("Open shared folder: could not start a thread"); delete paths; }
}

// ---------------------------------------------------------------- copy to clients

struct CopyTarget {
    std::string name;
    std::wstring share;                 // \\NAME\<share>
    std::wstring shareName;
    bool viaRelay;                      // an SMB1 share: staged under the relay's share, then RELAY op=push
    int relayConn;
    std::string relayName, relayIp, destIp;
    std::wstring subdir;                // folder below the share to copy into, created first; empty for the share itself
    CopyTarget() : viaRelay(false), relayConn(-1) {}
};

struct CopyJob {
    std::vector<std::wstring> sources;  // files, or one folder
    std::vector<CopyTarget> targets;
    // A message to send to one node when the whole job has ended without a
    // failure (the render job's BRYCE start); empty verb for none.
    std::string afterMac, afterLabel;
    Message after;
};

struct CopyAfter { std::string mac, label; bool ok; Message after; };

// The copy progress window: Thunderhead's own, because the Windows shell
// dialogs (SHFileOperation, IFileOperation, IProgressDialog) all hold their
// window back for about a second so that a short copy shows nothing at all,
// and most copies to a client share are short. This one appears as soon as
// the copy starts, follows the bytes of every file, and stays with the result
// until closed. It lives on the copy thread, which pumps its messages from
// the CopyFileEx callback and between files.
struct CopyProgress {
    HWND wnd, client, file, bar, bytes, button;
    ULONGLONG total;       // bytes over the whole job, all clients
    ULONGLONG done;        // bytes of the files finished so far
    ULONGLONG fileBase;    // done at the start of the current file
    bool cancelled;
    bool finished;         // the button then reads Close
    CopyProgress() : wnd(NULL), client(NULL), file(NULL), bar(NULL), bytes(NULL), button(NULL),
                     total(0), done(0), fileBase(0), cancelled(false), finished(false) {}
};

static std::string byteText(ULONGLONG bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024) return format("%.2f GB", bytes / (1024.0 * 1024 * 1024));
    if (bytes >= 1024ULL * 1024) return format("%.1f MB", bytes / (1024.0 * 1024));
    return format("%u KB", (unsigned)((bytes + 1023) / 1024));
}

static LRESULT CALLBACK copyProgressProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CopyProgress* cp = (CopyProgress*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); return 0;
    case WM_THEME_CHANGED: themeWindow(hwnd); return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, themeBrush(theme().paneBack));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (HBRUSH b = themeCtlColor(msg, (HDC)wp, (HWND)lp)) return (LRESULT)b;
        break;
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (cp && di->hwndItem == cp->button) return themeDrawPushButton(di) ? TRUE : FALSE;
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL && cp) {
            if (cp->finished) DestroyWindow(hwnd);
            else if (!cp->cancelled) { cp->cancelled = true; SetWindowTextW(cp->file, L"Stopping after the current file..."); EnableWindow(cp->button, FALSE); }
        }
        return 0;
    case WM_CLOSE: SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void copyProgressPump(CopyProgress& cp)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); return; }
        if (!cp.wnd || !IsDialogMessageW(cp.wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
}

static void copyPump(void* cp) { copyProgressPump(*(CopyProgress*)cp); }

static void copyProgressShowBytes(CopyProgress& cp, ULONGLONG at)
{
    if (!cp.wnd) return;
    ULONGLONG total = cp.total ? cp.total : 1;
    if (at > total) at = total;
    SendMessageW(cp.bar, PBM_SETPOS, (WPARAM)(at * 1000 / total), 0);
    SetWindowTextW(cp.bytes, widen(format("%s of %s (%u%%)", byteText(at).c_str(), byteText(cp.total).c_str(), (unsigned)(at * 100 / total))).c_str());
}

static void copyProgressOpen(CopyProgress& cp)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = copyProgressProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
        wc.lpszClassName = L"ThunderheadCopyProgress";
        registered = RegisterClassW(&wc) != 0;
    }
    const int w = 520, h = 190, margin = 12, line = 20;
    RECT owner;
    GetWindowRect(g_main, &owner);
    int x = owner.left + ((owner.right - owner.left) - w) / 2, y = owner.top + ((owner.bottom - owner.top) - h) / 2;
    cp.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ThunderheadCopyProgress", L"Copy files or folder to Client",
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, g_main, NULL, g_inst, &cp);
    if (!cp.wnd) return;
    RECT rc;
    GetClientRect(cp.wnd, &rc);
    int cw = rc.right - 2 * margin, cy = margin;
    cp.client = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, margin, cy, cw, line, cp.wnd, NULL, g_inst, NULL); cy += line + 4;
    cp.file = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS, margin, cy, cw, line, cp.wnd, NULL, g_inst, NULL); cy += line + 6;
    cp.bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, margin, cy, cw, 18, cp.wnd, NULL, g_inst, NULL); cy += 18 + 6;
    cp.bytes = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, margin, cy, cw, line, cp.wnd, NULL, g_inst, NULL); cy += line + 8;
    cp.button = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, rc.right - margin - 90, rc.bottom - margin - 26, 90, 26, cp.wnd, (HMENU)IDCANCEL, g_inst, NULL);
    SendMessageW(cp.bar, PBM_SETRANGE32, 0, 1000);
    HWND texts[] = { cp.client, cp.file, cp.bytes, cp.button };
    for (int i = 0; i < 4; ++i) SendMessageW(texts[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    themeWindow(cp.wnd);
    copyProgressShowBytes(cp, 0);
    ShowWindow(cp.wnd, SW_SHOWNORMAL);
    UpdateWindow(cp.wnd);
    copyProgressPump(cp);
}

static DWORD CALLBACK copyProgressRoutine(LARGE_INTEGER, LARGE_INTEGER transferred, LARGE_INTEGER, LARGE_INTEGER,
                                          DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
{
    CopyProgress* cp = (CopyProgress*)data;
    copyProgressShowBytes(*cp, cp->fileBase + (ULONGLONG)transferred.QuadPart);
    copyProgressPump(*cp);
    return cp->cancelled ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}

static std::wstring leafName(std::wstring path)
{
    while (path.size() > 1 && path[path.size() - 1] == L'\\') path.erase(path.size() - 1);
    size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? path : path.substr(cut + 1);
}

static bool isDot(const wchar_t* name) { return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0; }

// Bytes in a file, or in every file under a folder.
static ULONGLONG treeSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    ULONGLONG sum = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (!isDot(fd.cFileName)) sum += treeSize(path + L"\\" + fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    return sum;
}

// Copies the file or folder src into destDir as destDir<leaf>, folders
// recursively, existing files overwritten, each file through CopyFileEx so
// that the window follows the bytes. Stops at the first failure (err set) or
// when the user has cancelled.
static bool copyTree(const std::wstring& src, const std::wstring& destDir, CopyProgress& cp, std::string& err, unsigned& files)
{
    std::wstring dest = destDir + leafName(src);
    DWORD attr = GetFileAttributesW(src.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { err = narrow(src) + " not found"; return false; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (!CreateDirectoryW(dest.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = "cannot create " + narrow(dest) + ": " + lastErrorText((int)GetLastError());
            return false;
        }
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return true;
        bool ok = true;
        do { if (!isDot(fd.cFileName)) ok = copyTree(src + L"\\" + fd.cFileName, dest + L"\\", cp, err, files); }
        while (ok && !cp.cancelled && FindNextFileW(h, &fd));
        FindClose(h);
        return ok;
    }
    if (cp.wnd) SetWindowTextW(cp.file, dest.c_str());
    cp.fileBase = cp.done;
    ULONGLONG size = treeSize(src);
    copyProgressPump(cp);
    if (cp.cancelled) return false;
    BOOL cancel = FALSE;
    if (!CopyFileExW(src.c_str(), dest.c_str(), copyProgressRoutine, &cp, &cancel, 0)) {
        DWORD e = GetLastError();
        if (cp.cancelled || e == ERROR_REQUEST_ABORTED) { cp.cancelled = true; return false; }
        err = narrow(leafName(src)) + ": " + lastErrorText((int)e);
        return false;
    }
    cp.done += size;
    ++files;
    return true;
}

// One progress window over the whole job: the first line names the client,
// the second the file being written, the bar counts bytes over every client,
// Cancel stops after the current file, and the result stays on screen until
// Close. A share that refuses the connection gets the standard credential
// prompt once. Runs on its own thread because an unreachable client can take
// a minute; the main window keeps running.
DWORD WINAPI copyToClientsThread(LPVOID param)
{
    CopyJob* job = (CopyJob*)param;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CopyProgress cp;
    ULONGLONG perClient = 0;
    for (size_t k = 0; k < job->sources.size(); ++k) perClient += treeSize(job->sources[k]);
    cp.total = perClient * job->targets.size();
    copyProgressOpen(cp);
    unsigned clientsDone = 0, clientsFailed = 0, filesAll = 0;
    ULONGLONG bytesAll = 0;
    std::string lastError;
    for (size_t i = 0; i < job->targets.size() && !cp.cancelled; ++i) {
        const CopyTarget& tg = job->targets[i];
        const std::string& name = tg.name;
        // A relayed client's files go to relay\<job> under the relay's share first.
        std::string relayJob;
        std::wstring to = tg.share, probe;
        if (tg.viaRelay) { relayJob = newRelayJob(); to = relayStageDir(tg.relayIp, tg.shareName, relayJob); }
        if (to.empty() || to[to.size() - 1] != L'\\') to += L'\\';
        probe = tg.viaRelay ? L"\\\\" + widen(tg.relayIp) + L"\\" + tg.shareName + L"\\" : to;
        if (!tg.subdir.empty()) to += tg.subdir + L"\\";   // the share itself is probed; the folder below it is made after
        if (cp.wnd) {
            SetWindowTextW(cp.client, widen(format("Client %s (%u of %u): %s%s", name.c_str(), (unsigned)i + 1, (unsigned)job->targets.size(),
                                                   tg.viaRelay ? ("\\\\" + tg.destIp + "\\" + narrow(tg.shareName) + "\\ through relay " + tg.relayName).c_str() : narrow(to).c_str(), "")).c_str());
            SetWindowTextW(cp.file, L"Connecting...");
            copyProgressPump(cp);
        }
        // The share must answer before anything is copied; a refusal gets the credential prompt.
        if (GetFileAttributesW(probe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED || e == ERROR_LOGON_FAILURE || e == ERROR_INVALID_PASSWORD || e == ERROR_SESSION_CREDENTIAL_CONFLICT)
                e = promptShareCredentials(cp.wnd ? cp.wnd : g_main, probe) == NO_ERROR && GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES ? NO_ERROR : GetLastError();
            if (e != NO_ERROR) {
                lastError = format("%s is not reachable (%s)", narrow(probe).c_str(), lastErrorText((int)e).c_str());
                logEvent(format("copy to %s failed: %s", name.c_str(), lastError.c_str()));
                ++clientsFailed;
                cp.done += perClient;
                copyProgressShowBytes(cp, cp.done);
                continue;
            }
        }
        if ((tg.viaRelay || !tg.subdir.empty()) && !makeDirs(to)) {
            lastError = format("staging folder %s could not be created (%s)", narrow(to).c_str(), lastErrorText((int)GetLastError()).c_str());
            logEvent(format("copy to %s failed: %s", name.c_str(), lastError.c_str()));
            ++clientsFailed;
            cp.done += perClient;
            copyProgressShowBytes(cp, cp.done);
            continue;
        }
        std::string err;
        unsigned files = 0;
        ULONGLONG before = cp.done;
        bool ok = true;
        for (size_t k = 0; k < job->sources.size() && ok; ++k) ok = copyTree(job->sources[k], to, cp, err, files);
        if (tg.viaRelay) {
            if (ok && !cp.cancelled) {
                // The relay copies the staged files on over SMB1; Cancel waits for it, since the relay has no abort.
                if (cp.wnd) {
                    SetWindowTextW(cp.file, widen("Relay " + tg.relayName + " is copying to \\\\" + tg.destIp + "\\" + narrow(tg.shareName) + "\\ ...").c_str());
                    copyProgressPump(cp);
                }
                Message ack;
                std::string rerr;
                ok = relayRequest(tg.relayConn, relayMessage("push", relayJob, tg.destIp, tg.shareName), 30 * 60 * 1000, ack, rerr, copyPump, &cp);
                if (!ok) { err = "relay " + tg.relayName + ": " + rerr; removeTree(to); }
            } else {
                removeTree(to);
            }
        }
        filesAll += files;
        bytesAll += cp.done - before;
        if (cp.cancelled) logEvent(format("copy to %s cancelled after %u file(s), %s", name.c_str(), files, byteText(cp.done - before).c_str()));
        else if (ok) { logEvent(format("copied %u file(s), %s, to %s", files, byteText(cp.done - before).c_str(), name.c_str())); ++clientsDone; }
        else { lastError = err; logEvent(format("copy to %s failed after %u file(s): %s", name.c_str(), files, err.c_str())); ++clientsFailed; }
        if (!ok && !cp.cancelled) { cp.done = before + perClient; copyProgressShowBytes(cp, cp.done); }
    }
    if (!job->after.verb.empty()) {
        // The follow-up goes out as soon as the copying has ended, not when the
        // progress window is closed.
        CopyAfter* ca = new CopyAfter();
        ca->mac = job->afterMac;
        ca->label = job->afterLabel;
        ca->ok = !cp.cancelled && clientsFailed == 0;
        ca->after = job->after;
        if (!PostMessageW(g_main, WM_COPY_DONE, 0, (LPARAM)ca)) delete ca;
    }
    if (cp.wnd) {
        cp.finished = true;
        std::string head = cp.cancelled ? "Cancelled" : clientsFailed ? format("Finished with %u failure(s)", clientsFailed) : "Finished";
        SetWindowTextW(cp.client, widen(format("%s: %u file(s), %s, copied to %u of %u client(s)", head.c_str(), filesAll, byteText(bytesAll).c_str(),
                                              clientsDone, (unsigned)job->targets.size())).c_str());
        SetWindowTextW(cp.file, widen(lastError.empty() ? std::string(cp.cancelled ? "Stopped at your request." : "The event log lists every client.") : "Last error: " + lastError).c_str());
        if (!cp.cancelled && !clientsFailed) copyProgressShowBytes(cp, cp.total);
        SetWindowTextW(cp.button, L"Close");
        EnableWindow(cp.button, TRUE);
        SetFocus(cp.button);
        MSG m;
        while (GetMessageW(&m, NULL, 0, 0) > 0) { if (!IsDialogMessageW(cp.wnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    }
    CoUninitialize();
    delete job;
    return 0;
}

// Create Remote Shared Folder: a folder on each checked online client, created
// and shared under one name by the agent (SHARE action=create; agents from
// kShareAgent, administrator rights on the client). The new share is opened
// from here in Windows Explorer when the agent reports success.
const char* const kShareAgent = "0.4.6";
struct ShareDlgData { std::wstring info, path, name; };

INT_PTR CALLBACK shareDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    ShareDlgData* d = (ShareDlgData*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    switch (msg) {
    case WM_INITDIALOG:
        d = (ShareDlgData*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_SHARE_INFO, d->info.c_str());
        SetDlgItemTextW(dlg, IDC_SHARE_PATH, d->path.c_str());
        SetDlgItemTextW(dlg, IDC_SHARE_NAME, d->name.c_str());
        SendDlgItemMessageW(dlg, IDC_SHARE_NAME, EM_LIMITTEXT, 80, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SHARE_PATH && HIWORD(wp) == EN_KILLFOCUS && d) {
            // The share name follows the folder's last part until typed by hand.
            wchar_t buf[1024], name[128];
            GetDlgItemTextW(dlg, IDC_SHARE_PATH, buf, 1024);
            GetDlgItemTextW(dlg, IDC_SHARE_NAME, name, 128);
            std::wstring path = trim(std::wstring(buf));
            while (!path.empty() && path[path.size() - 1] == L'\\') path.erase(path.size() - 1);
            size_t slash = path.find_last_of(L"\\/");
            if (name[0] == L'\0' && slash != std::wstring::npos && slash + 1 < path.size()) SetDlgItemTextW(dlg, IDC_SHARE_NAME, path.substr(slash + 1).c_str());
            return TRUE;
        }
        if (LOWORD(wp) == IDOK && d) {
            wchar_t buf[1024];
            GetDlgItemTextW(dlg, IDC_SHARE_PATH, buf, 1024);
            d->path = trim(std::wstring(buf));
            GetDlgItemTextW(dlg, IDC_SHARE_NAME, buf, 1024);
            d->name = trim(std::wstring(buf));
            if (d->path.size() < 3 || d->path[1] != L':' || d->path[2] != L'\\' || d->path.find(L"..") != std::wstring::npos) {
                MessageBoxW(dlg, L"Enter the folder as a full local path on the client, for example D:\\RENDER.BIN.", L"Create Remote Shared Folder", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (d->name.empty() || d->name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
                MessageBoxW(dlg, L"Enter a share name without \\ / : * ? \" < > |.", L"Create Remote Shared Folder", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void createRemoteShare(const std::vector<NodeRecord*>& nodes)
{
    int online = 0;
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->online) ++online;
    if (online == 0) { updateStatusBar(L"|  no online clients checked"); return; }
    static std::wstring lastPath = L"D:\\RENDER.BIN", lastName = L"RENDER.BIN";
    ShareDlgData d;
    d.info = widen(format("The folder is created and shared on %d checked online client(s); an existing share of the same name and folder is kept.", online));
    d.path = lastPath;
    d.name = lastName;
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SHARE), g_main, shareDlgProc, (LPARAM)&d) != IDOK) return;
    lastPath = d.path;
    lastName = d.name;
    Message m("SHARE");
    m.set("action", std::string("create"));
    m.set("path", narrow(d.path));
    m.set("name", narrow(d.name));
    int sent = 0, old = 0;
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (!n->online) continue;
        if (compareVersions(n->hello.get("agent"), kShareAgent) < 0) {
            ++old;
            logEvent(format("SHARE not sent to %s: agent %s is older than %s, update it first", n->name.c_str(), n->hello.get("agent").c_str(), kShareAgent));
            continue;
        }
        if (sendToNode(n, m, m.oneLine())) ++sent;
    }
    std::string status = format("|  SHARE create %s as %s sent to %d of %d online checked", narrow(d.path).c_str(), narrow(d.name).c_str(), sent, online);
    if (old) status += format(", %d skipped: agent older than %s", old, kShareAgent);
    updateStatusBar(widen(status).c_str());
}

// Asks for files or a folder, lets the user pick them, and copies them to the
// shared folder of every checked client.
void copyToClients()
{
    std::vector<NodeRecord*> nodes = checkedNodes();
    if (nodes.empty()) {
        MessageBoxW(g_main, L"Check the clients to copy to first.", L"Copy files or folder to Client", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring content = widen(format("The selection is copied to \\\\NAME\\%s\\ of %u checked client(s).",
                                        g_store->settings.shareName.c_str(), (unsigned)nodes.size()));
    TASKDIALOG_BUTTON buttons[3] = { { 100, L"Files..." }, { 101, L"A folder..." }, { 102, L"Create Remote Shared Folder...\nCreate a folder on each checked client and share it" } };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = L"Copy files or folder to Client";
    tc.pszMainInstruction = L"What do you want to copy?";
    tc.pszContent = content.c_str();
    tc.pButtons = buttons;
    tc.cButtons = 3;
    tc.nDefaultButton = 100;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || (pressed != 100 && pressed != 101 && pressed != 102)) return;
    if (pressed == 102) { createRemoteShare(nodes); return; }
    bool folder = pressed == 101;

    std::vector<std::wstring> sources;
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        MessageBoxW(g_main, L"The file dialog could not be created.", L"Copy files or folder to Client", MB_OK | MB_ICONERROR);
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_ALLOWMULTISELECT);
    dlg->SetOptions(opts);
    dlg->SetTitle(folder ? L"Select the folder to copy" : L"Select the files to copy");
    dlg->SetOkButtonLabel(L"Copy");
    if (SUCCEEDED(dlg->Show(g_main))) {
        IShellItemArray* items = NULL;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = NULL;
                if (FAILED(items->GetItemAt(i, &item)) || !item) continue;
                PWSTR path = NULL;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) { sources.push_back(path); CoTaskMemFree(path); }
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    if (sources.empty()) return;

    CopyJob* job = new CopyJob;
    job->sources = sources;
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (n->name.empty()) { logEvent(format("%s has no name yet; nothing copied to it", n->mac.c_str())); continue; }
        if (!commandAllowed(n, "copy")) continue;   // writing into its share is control of the node
        if (NodeRecord* relay = relayFor(n)) { if (!commandAllowed(relay, "copy (as relay for " + n->name + ")")) continue; }
        CopyTarget tg;
        tg.name = n->name;
        tg.shareName = widen(g_store->settings.shareName);
        tg.share = shareRootOf(n);
        tg.destIp = n->ip;
        if (NodeRecord* relay = relayFor(n)) {
            tg.viaRelay = true;
            tg.relayConn = relay->conn;
            tg.relayName = relay->name;
            tg.relayIp = relay->ip;
            logEvent(format("copy to %s goes through SMB1 relay %s", n->name.c_str(), relay->name.c_str()));
        }
        job->targets.push_back(tg);
    }
    if (job->targets.empty()) { delete job; return; }
    logEvent(format("copying %u item(s) from %s to %u client(s)", (unsigned)sources.size(),
                    narrow(sources[0]).c_str(), (unsigned)job->targets.size()));
    HANDLE t = CreateThread(NULL, 0, copyToClientsThread, job, 0, NULL);
    if (t) CloseHandle(t);
    else { logEvent("copy to clients: could not start a thread"); delete job; }
}

// ---------------------------------------------------------------- Bryce 3D on the host
// Start Bryce on Host, Stop Bryce on Host and Render Job on Host act on the
// Bryce 3D Network Render Hosts, every online client that is flagged as one or
// reports as one, not on the checked rows. The agent finds the installed Bryce
// as it finds Lightning (BRYCE, see docs/PROTOCOL.md under Bryce 3D on the
// host); agents before kBryceAgent are passed over with a line in the log.
const char* const kBryceAgent = "0.4.3";

// The Bryce generations as Lightning's are numbered: 3 = Bryce 7 and 7 Pro, 2 = 6.x, 1 = 5.x.
const char* bryceGenLabel(int gen)
{
    return gen == 3 ? "Bryce 7 & 7 Pro" : gen == 2 ? "Bryce 6.x" : gen == 1 ? "Bryce 5.x" : "Bryce";
}

// Start Bryce 3D: asks which Bryce, then BRYCE action=start gen= to every
// render host. Returns 0 when cancelled.
int askBryceGeneration()
{
    TASKDIALOG_BUTTON radios[3] = { { 503, L"Bryce 7 && 7 Pro" }, { 502, L"Bryce 6.x" }, { 501, L"Bryce 5.x" } };
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    tc.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    tc.pszWindowTitle = L"Start B3D";
    tc.pszMainInstruction = L"Which Bryce 3D is to be started on the render host(s)?";
    tc.pszContent = L"The agent finds that version's installed Bryce.exe and starts it; a version that is not installed is reported in the event log.";
    tc.pRadioButtons = radios;
    tc.cRadioButtons = 3;
    tc.nDefaultRadioButton = 503;
    int pressed = 0, radio = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, &radio, NULL)) || pressed != IDOK) return 0;
    return radio - 500;
}

std::vector<NodeRecord*> renderHostsOnline()
{
    std::vector<NodeRecord*> out;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (!n->online || !isRenderHost(n)) continue;
        if (!commandAllowed(n, "Bryce host control")) continue;   // an unverified host is not controlled
        if (NodeRecord* relay = relayFor(n)) { if (!commandAllowed(relay, "Bryce host control (as relay for " + n->name + ")")) continue; }
        out.push_back(n);
    }
    return out;
}

// BRYCE to one host, version-checked; what the log and status bar say.
bool sendBryce(NodeRecord* n, const Message& m, const std::string& label)
{
    if (compareVersions(n->hello.get("agent"), kBryceAgent) < 0) {
        logEvent(format("%s not sent to %s: agent %s is older than %s, update it first", label.c_str(), n->name.c_str(),
                        n->hello.get("agent").c_str(), kBryceAgent));
        return false;
    }
    return sendToNode(n, m, label);
}

void bryceOnHosts(const char* action)
{
    std::vector<NodeRecord*> hosts = renderHostsOnline();
    if (hosts.empty()) {
        updateStatusBar(L"|  no online client is flagged as Bryce 3D Network Render Host (Bryce 3D Net Render Host Toggle)");
        logEvent(format("BRYCE %s: no online client is a Bryce 3D Network Render Host", action));
        return;
    }
    Message m("BRYCE");
    m.set("action", std::string(action));
    if (strcmp(action, "start") == 0) {
        int gen = askBryceGeneration();
        if (gen == 0) return;
        m.set("gen", (long long)gen);
    }
    int sent = 0;
    for (size_t i = 0; i < hosts.size(); ++i) if (sendBryce(hosts[i], m, m.oneLine())) ++sent;
    updateStatusBar(widen(format("|  BRYCE %s sent to %d of %d render host(s)", action, sent, (int)hosts.size())).c_str());
}

void onCopyDone(CopyAfter* ca)
{
    NodeRecord* n = g_store->find(ca->mac);
    if (!n) return;
    if (!ca->ok) { logEvent(format("%s not sent to %s: the copy did not finish cleanly", ca->label.c_str(), n->name.c_str())); return; }
    if (!n->online) { logEvent(format("%s not sent to %s: it went offline during the copy", ca->label.c_str(), n->name.c_str())); return; }
    sendBryce(n, ca->after, ca->label);
}

// The Windows file dialog for one file or one folder; false when cancelled.
// startIn, when given, is the folder the dialog opens in (a UNC path works).
bool pickPath(HWND owner, bool folder, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec, std::wstring& out,
              const std::wstring& startIn = std::wstring())
{
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST);
    dlg->SetOptions(opts);
    dlg->SetTitle(title);
    if (!folder && filterName) {
        COMDLG_FILTERSPEC filters[2] = { { filterName, filterSpec }, { L"All files", L"*.*" } };
        dlg->SetFileTypes(2, filters);
    }
    if (!startIn.empty()) {
        IShellItem* start = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(startIn.c_str(), NULL, IID_PPV_ARGS(&start))) && start) { dlg->SetFolder(start); start->Release(); }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = NULL;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) { out = path; CoTaskMemFree(path); ok = true; }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

struct RenderJobData {
    std::vector<NodeRecord*> hosts;
    std::vector<std::wstring> bins;   // each host's RENDER.BIN as reported (empty when not reported)
    std::vector<std::wstring> roots;  // each host's share root as the gate verified it (shareRootOf)
    int host;
    int gen;                 // which Bryce opens the scene: 3, 2 or 1
    bool existing;           // Use existing job on host: scene is a path under the host's share, nothing is copied
    std::wstring scene, textures, output;
    std::wstring sceneRel;   // existing mode: the scene's path below the share root, as the agent takes it
    std::string first, last;
    BryceSceneInfo info;     // read from the scene file: resolution, frame rate, length
    bool infoOk;
    std::wstring infoPath;   // the scene the info belongs to
    RenderJobData() : host(0), gen(3), existing(false), infoOk(false) {}
};
std::wstring g_jobScene, g_jobTextures;   // remembered while Thunderhead runs
bool g_jobExisting = false;

std::wstring fileTitle(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.rfind(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

std::wstring fileName(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// Reads the chosen scene's render settings: the info line shows resolution,
// frame rate and length, and empty frame fields take 0 and the last frame.
void showSceneFacts(HWND dlg, RenderJobData* d, const std::wstring& path)
{
    if (path.empty()) return;
    if (d->infoOk && d->infoPath == path) return;
    std::string err;
    d->infoOk = readBryceScene(path, d->info, err);
    d->infoPath = path;
    std::wstring line;
    if (!d->infoOk) {
        line = L"Scene not readable: " + widen(err);
    } else if (d->info.animation()) {
        line = widen(format("Scene: %d x %d, %g fps, %g s: frames 0 to %d (%d images)", d->info.width, d->info.height, d->info.fps, d->info.seconds,
                            d->info.lastFrame, d->info.lastFrame + 1));
        wchar_t a[32], b[32];
        GetDlgItemTextW(dlg, IDC_JOB_FIRST, a, 32);
        GetDlgItemTextW(dlg, IDC_JOB_LAST, b, 32);
        if (a[0] == L'\0' && b[0] == L'\0') { SetDlgItemTextW(dlg, IDC_JOB_FIRST, L"0"); SetDlgItemTextW(dlg, IDC_JOB_LAST, widen(format("%d", d->info.lastFrame)).c_str()); }
    } else {
        line = widen(format("Scene: %d x %d, a still image (no animation length set)", d->info.width, d->info.height));
    }
    SetDlgItemTextW(dlg, IDC_JOB_INFO, (widen(format("%u online Bryce 3D Network Render Host(s).  ", (unsigned)d->hosts.size())) + line).c_str());
}

// The texture controls serve the copy only; an existing job is taken as it lies.
void applyJobMode(HWND dlg, const RenderJobData* d)
{
    EnableWindow(GetDlgItem(dlg, IDC_JOB_TEXTURES), !d->existing);
    EnableWindow(GetDlgItem(dlg, IDC_JOB_TEXTURES_BROWSE), !d->existing);
    SetDlgItemTextW(dlg, IDOK, d->existing ? L"Start Bryce" : L"Copy and Start Bryce");
}

INT_PTR CALLBACK renderJobDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    RenderJobData* d = (RenderJobData*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    switch (msg) {
    case WM_INITDIALOG: {
        d = (RenderJobData*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_JOB_INFO, widen(format("%u online Bryce 3D Network Render Host(s). The scene is opened in Bryce on the host chosen below.",
                                                        (unsigned)d->hosts.size())).c_str());
        for (size_t i = 0; i < d->hosts.size(); ++i)
            SendDlgItemMessageW(dlg, IDC_JOB_HOST, CB_ADDSTRING, 0, (LPARAM)widen(d->hosts[i]->name + "  (" + d->hosts[i]->ip + ")").c_str());
        SendDlgItemMessageW(dlg, IDC_JOB_HOST, CB_SETCURSEL, 0, 0);
        CheckRadioButton(dlg, IDC_JOB_MODE_COPY, IDC_JOB_MODE_EXISTING, d->existing ? IDC_JOB_MODE_EXISTING : IDC_JOB_MODE_COPY);
        SetDlgItemTextW(dlg, IDC_JOB_SCENE, d->scene.c_str());
        SetDlgItemTextW(dlg, IDC_JOB_TEXTURES, d->textures.c_str());
        SetDlgItemTextW(dlg, IDC_JOB_OUTPUT, d->output.c_str());
        CheckRadioButton(dlg, IDC_JOB_GEN3, IDC_JOB_GEN1, d->gen == 2 ? IDC_JOB_GEN2 : d->gen == 1 ? IDC_JOB_GEN1 : IDC_JOB_GEN3);
        applyJobMode(dlg, d);
        return TRUE;
    }
    case WM_COMMAND:
        if ((LOWORD(wp) == IDC_JOB_MODE_COPY || LOWORD(wp) == IDC_JOB_MODE_EXISTING) && d) {
            bool existing = LOWORD(wp) == IDC_JOB_MODE_EXISTING;
            if (existing != d->existing) { d->existing = existing; SetDlgItemTextW(dlg, IDC_JOB_SCENE, L""); applyJobMode(dlg, d); }
            return TRUE;
        }
        if (LOWORD(wp) == IDC_JOB_HOST && HIWORD(wp) == CBN_SELCHANGE && d) {
            // Another host: its RENDER.BIN becomes the output folder, and a chosen existing job no longer applies.
            int h = (int)SendDlgItemMessageW(dlg, IDC_JOB_HOST, CB_GETCURSEL, 0, 0);
            if (h >= 0 && h < (int)d->bins.size() && !d->bins[h].empty()) SetDlgItemTextW(dlg, IDC_JOB_OUTPUT, d->bins[h].c_str());
            if (d->existing) SetDlgItemTextW(dlg, IDC_JOB_SCENE, L"");
            return TRUE;
        }
        if (LOWORD(wp) == IDC_JOB_SCENE_BROWSE && d) {
            std::wstring path;
            int h = (int)SendDlgItemMessageW(dlg, IDC_JOB_HOST, CB_GETCURSEL, 0, 0);
            if (d->existing) {
                // The host's jobs folder, through the share the gate verified.
                if (h < 0 || h >= (int)d->roots.size()) return TRUE;
                if (relayFor(d->hosts[h])) {
                    MessageBoxW(dlg, L"This host's shared folder is reached through an SMB1 relay and cannot be browsed from here.", L"Render Job on Host", MB_OK | MB_ICONINFORMATION);
                    return TRUE;
                }
                std::wstring startIn = d->roots[h] + L"jobs";
                if (GetFileAttributesW(startIn.c_str()) == INVALID_FILE_ATTRIBUTES) startIn = d->roots[h];
                if (pickPath(dlg, false, L"Select the scene in the host's jobs folder", L"Bryce scenes (*.br7;*.br6;*.br5)", L"*.br7;*.br6;*.br5", path, startIn)) {
                    SetDlgItemTextW(dlg, IDC_JOB_SCENE, path.c_str());
                    showSceneFacts(dlg, d, path);
                }
                return TRUE;
            }
            if (pickPath(dlg, false, L"Select the Bryce scene", L"Bryce scenes (*.br7;*.br6;*.br5)", L"*.br7;*.br6;*.br5", path)) {
                SetDlgItemTextW(dlg, IDC_JOB_SCENE, path.c_str());
                showSceneFacts(dlg, d, path);
                wchar_t out[512];
                GetDlgItemTextW(dlg, IDC_JOB_OUTPUT, out, 512);
                if (out[0] == L'\0') {
                    bool known = h >= 0 && h < (int)d->bins.size() && !d->bins[h].empty();
                    SetDlgItemTextW(dlg, IDC_JOB_OUTPUT, known ? d->bins[h].c_str() : L"RENDER.BIN");
                }
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDC_JOB_TEXTURES_BROWSE) {
            std::wstring path;
            if (pickPath(dlg, true, L"Select the texture folder", NULL, NULL, path)) SetDlgItemTextW(dlg, IDC_JOB_TEXTURES, path.c_str());
            return TRUE;
        }
        if (LOWORD(wp) == IDOK && d) {
            wchar_t buf[1024];
            d->host = (int)SendDlgItemMessageW(dlg, IDC_JOB_HOST, CB_GETCURSEL, 0, 0);
            GetDlgItemTextW(dlg, IDC_JOB_SCENE, buf, 1024);   d->scene = trim(std::wstring(buf));
            GetDlgItemTextW(dlg, IDC_JOB_TEXTURES, buf, 1024); d->textures = trim(std::wstring(buf));
            GetDlgItemTextW(dlg, IDC_JOB_OUTPUT, buf, 1024);  d->output = trim(std::wstring(buf));
            GetDlgItemTextW(dlg, IDC_JOB_FIRST, buf, 1024);   d->first = trim(narrow(buf));
            GetDlgItemTextW(dlg, IDC_JOB_LAST, buf, 1024);    d->last = trim(narrow(buf));
            d->gen = IsDlgButtonChecked(dlg, IDC_JOB_GEN2) == BST_CHECKED ? 2 : IsDlgButtonChecked(dlg, IDC_JOB_GEN1) == BST_CHECKED ? 1 : 3;
            d->existing = IsDlgButtonChecked(dlg, IDC_JOB_MODE_EXISTING) == BST_CHECKED;
            if (d->host < 0 || d->host >= (int)d->hosts.size()) { MessageBoxW(dlg, L"Choose the render host.", L"Render Job on Host", MB_OK | MB_ICONWARNING); return TRUE; }
            DWORD a = d->scene.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(d->scene.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
                MessageBoxW(dlg, d->existing ? L"Browse for the scene file in the host's jobs folder." : L"Enter or browse for the scene file.", L"Render Job on Host", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            // A typed path has not been read yet; the frame fields may still be empty.
            if (!d->infoOk || d->infoPath != d->scene) {
                showSceneFacts(dlg, d, d->scene);
                GetDlgItemTextW(dlg, IDC_JOB_FIRST, buf, 1024);   d->first = trim(narrow(buf));
                GetDlgItemTextW(dlg, IDC_JOB_LAST, buf, 1024);    d->last = trim(narrow(buf));
            }
            if (d->existing) {
                // The file must lie under the host's share; the agent gets the path below it.
                const std::wstring& rootPath = d->roots[d->host];
                if (d->scene.size() <= rootPath.size() || _wcsnicmp(d->scene.c_str(), rootPath.c_str(), rootPath.size()) != 0) {
                    MessageBoxW(dlg, (L"The scene must lie inside the host's shared folder, " + rootPath + L", for Bryce on the host to open it.").c_str(), L"Render Job on Host", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                d->sceneRel = d->scene.substr(rootPath.size());
                d->textures.clear();
            } else if (!d->textures.empty()) {
                DWORD t = GetFileAttributesW(d->textures.c_str());
                if (t == INVALID_FILE_ATTRIBUTES || !(t & FILE_ATTRIBUTE_DIRECTORY)) { MessageBoxW(dlg, L"The texture folder does not exist. Leave it empty or browse for a folder.", L"Render Job on Host", MB_OK | MB_ICONWARNING); return TRUE; }
            }
            if (d->output.empty()) d->output = (d->host < (int)d->bins.size() && !d->bins[d->host].empty()) ? d->bins[d->host] : L"RENDER.BIN";
            if (d->first.empty() != d->last.empty() || (!d->first.empty() && atoi(d->last.c_str()) < atoi(d->first.c_str()))) {
                MessageBoxW(dlg, L"Enter both the first and the last frame, the last not before the first, or leave both empty.", L"Render Job on Host", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void appendJobRecord(const std::string& line)
{
    FILE* f = _wfopen((g_dir + L"\\jobs.txt").c_str(), L"ab");
    if (!f) { logEvent("jobs.txt could not be written"); return; }
    fwrite(line.data(), 1, line.size(), f);
    fwrite("\r\n", 1, 2, f);
    fclose(f);
}

// Render Job on Host. Copy from source: the scene (and its texture folder) go
// to jobs\<scene name>\ in the host's shared folder through the copy machinery,
// after asking when the scene is already there, and when the copy has ended
// cleanly the host is sent BRYCE action=start with the scene, output folder
// and frame range. Use existing job on host: the chosen file, already under
// the host's share, is started where it lies and nothing is copied. Both are
// recorded in jobs.txt.
void renderJob()
{
    RenderJobData d;
    d.hosts = renderHostsOnline();
    if (d.hosts.empty()) {
        MessageBoxW(g_main, L"No online client is flagged as Bryce 3D Network Render Host. Check the host and use Bryce 3D Net Render Host Toggle first.",
                    L"Render Job on Host", MB_OK | MB_ICONINFORMATION);
        return;
    }
    d.host = 0;
    d.gen = 3;
    d.existing = g_jobExisting;
    d.scene = d.existing ? std::wstring() : g_jobScene;
    d.textures = g_jobTextures;
    for (size_t i = 0; i < d.hosts.size(); ++i) { d.bins.push_back(widen(renderBinOf(d.hosts[i]))); d.roots.push_back(shareRootOf(d.hosts[i])); }
    d.output = d.bins[0].empty() ? L"RENDER.BIN" : d.bins[0];   // the host's RENDER.BIN; relative means inside its shared folder
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_RENDERJOB), g_main, renderJobDlgProc, (LPARAM)&d) != IDOK) return;
    g_jobExisting = d.existing;
    if (!d.existing) { g_jobScene = d.scene; g_jobTextures = d.textures; }
    NodeRecord* n = d.hosts[d.host];
    if (!n->online) { logEvent(n->name + ": went offline before the render job started"); return; }
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string frames = (d.first.empty() ? "?" : d.first) + "-" + (d.last.empty() ? "?" : d.last);

    // The image Bryce shows instead of rendering when it opens the scene with
    // "Image with Scene Open/Save" on: always a fresh neutral 50 % gray one at
    // the scene's resolution, replacing whatever lies there, because a partly
    // rendered frame left beside the scene makes Bryce resume rendering it on
    // load whatever the preference says.
    std::string imageNote;
    if (d.existing) {
        std::wstring base = fileTitle(d.sceneRel);
        std::wstring dir = d.scene.substr(0, d.scene.find_last_of(L"\\/") + 1);
        std::wstring bmp = dir + base + L".bmp";
        bool had = GetFileAttributesW(bmp.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!d.infoOk) imageNote = std::string("the scene's resolution could not be read, so no gray image was written") + (had ? " (the image beside the scene stays)" : "");
        else {
            std::string err;
            if (writeSolidBmp(bmp, d.info.width, d.info.height, 128, err)) imageNote = format("gray %dx%d image written as %s%s", d.info.width, d.info.height, narrow(base + L".bmp").c_str(), had ? ", replacing the one there" : "");
            else imageNote = "gray image could not be written: " + err;
        }
        Message m("BRYCE");
        m.set("action", std::string("start"));
        m.set("gen", (long long)d.gen);
        m.set("scene", narrow(d.sceneRel));
        m.set("output", narrow(d.output));
        m.set("first", d.first);
        m.set("last", d.last);
        std::string record = format("%04d-%02d-%02d %02d:%02d:%02d  host=%s (%s)  bryce=%s  existing=%s  output=%s  frames=%s  resolution=%dx%d  image: %s",
                                    t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n->name.c_str(), n->ip.c_str(), bryceGenLabel(d.gen),
                                    narrow(d.scene).c_str(), narrow(d.output).c_str(), frames.c_str(), d.info.width, d.info.height, imageNote.c_str());
        appendJobRecord(record);
        logEvent("render job: " + record);
        sendBryce(n, m, "BRYCE start (existing job " + narrow(base) + ")");
        return;
    }

    std::wstring base = fileTitle(d.scene);
    std::wstring subdir = L"jobs\\" + base;
    // An earlier copy of the same scene is overwritten only with leave.
    if (!relayFor(n)) {
        std::wstring dest = d.roots[d.host] + subdir + L"\\" + fileName(d.scene);
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES) {
            TASKDIALOG_BUTTON buttons[2] = { { 100, L"Overwrite" }, { 101, L"Cancel the copy" } };
            std::wstring content = L"The scene is already on " + widen(n->name) + L" as " + dest + L". Overwrite it (and the texture folder beside it), or cancel?";
            TASKDIALOGCONFIG tc;
            memset(&tc, 0, sizeof(tc));
            tc.cbSize = sizeof(tc);
            tc.hwndParent = g_main;
            tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
            tc.pszWindowTitle = L"Render Job on Host";
            tc.pszMainInstruction = L"The scene exists on the host already";
            tc.pszContent = content.c_str();
            tc.pButtons = buttons;
            tc.cButtons = 2;
            tc.nDefaultButton = 101;
            int pressed = 0;
            if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || pressed != 100) {
                logEvent(format("render job cancelled: %s exists on %s already", narrow(dest).c_str(), n->name.c_str()));
                return;
            }
        }
    }
    CopyJob* job = new CopyJob;
    job->sources.push_back(d.scene);
    if (!d.textures.empty()) job->sources.push_back(d.textures);
    {
        // Always the gray image, never the one beside the source: the copy overwrites what the job folder holds.
        if (!d.infoOk) imageNote = "the scene's resolution could not be read, so no gray image was made";
        else {
            CreateDirectoryW((g_dir + L"\\jobs").c_str(), NULL);
            std::wstring gray = g_dir + L"\\jobs\\" + base + L".bmp";
            std::string err;
            if (writeSolidBmp(gray, d.info.width, d.info.height, 128, err)) { job->sources.push_back(gray); imageNote = format("gray %dx%d image made and copied as %s", d.info.width, d.info.height, narrow(base + L".bmp").c_str()); }
            else imageNote = "gray image could not be made: " + err;
        }
    }
    CopyTarget tg;
    tg.name = n->name;
    tg.shareName = widen(g_store->settings.shareName);
    tg.share = shareRootOf(n);
    tg.destIp = n->ip;
    tg.subdir = subdir;
    if (NodeRecord* relay = relayFor(n)) {
        tg.viaRelay = true;
        tg.relayConn = relay->conn;
        tg.relayName = relay->name;
        tg.relayIp = relay->ip;
    }
    job->targets.push_back(tg);
    Message m("BRYCE");
    m.set("action", std::string("start"));
    m.set("gen", (long long)d.gen);
    m.set("scene", narrow(subdir + L"\\" + fileName(d.scene)));
    m.set("output", narrow(d.output));
    m.set("first", d.first);
    m.set("last", d.last);
    job->after = m;
    job->afterMac = n->mac;
    job->afterLabel = "BRYCE start (render job " + narrow(base) + ")";
    std::string record = format("%04d-%02d-%02d %02d:%02d:%02d  host=%s (%s)  bryce=%s  scene=%s  textures=%s  copied_to=\\\\%s\\%s\\%s  output=%s  frames=%s  resolution=%dx%d  image: %s",
                                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n->name.c_str(), n->ip.c_str(), bryceGenLabel(d.gen), narrow(d.scene).c_str(),
                                d.textures.empty() ? "(none)" : narrow(d.textures).c_str(), n->name.c_str(), g_store->settings.shareName.c_str(),
                                narrow(subdir).c_str(), narrow(d.output).c_str(), frames.c_str(), d.info.width, d.info.height, imageNote.c_str());
    appendJobRecord(record);
    logEvent("render job: " + record);
    HANDLE th = CreateThread(NULL, 0, copyToClientsThread, job, 0, NULL);
    if (th) CloseHandle(th);
    else { logEvent("render job: could not start the copy thread"); delete job; }
}

void renameChecked()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    int sent = 0, shown = 0;
    static bool rebootDefault = true;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        ++shown;
        RenameRequest req;
        req.info = L"Node " + widen(n->name) + L"   MAC " + widen(n->mac) + L"   IP " + widen(n->ip) +
                   widen(format("   (%d of %d)", shown, (int)targets.size()));
        req.name = widen(n->name);
        req.reboot = rebootDefault;
        INT_PTR r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_RENAME), g_main, renameDlgProc, (LPARAM)&req);
        if (r == IDABORT) break;
        if (r != IDOK) continue;
        rebootDefault = req.reboot;
        if (!n->online) continue;          // went offline while the dialog was up
        Message m("RENAME");
        m.set("name", narrow(req.name));
        if (sendGated(n, m)) {
            ++sent;
            n->rebootAfterRename = req.reboot;
            logEvent(format("RENAME -> %s (%s): new name %s%s", n->name.c_str(), n->mac.c_str(), narrow(req.name).c_str(),
                            req.reboot ? ", reboot after" : ""));
            logNode(*n, "-> " + m.oneLine());
        }
    }
    updateStatusBar(widen(format("|  RENAME sent to %d client(s)", sent)).c_str());
}

void checkAll(bool on)
{
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < count; ++row) setChecked(row, on);
    updateStatusBar();
}

// Invert Selections: every row's check box is flipped.
void invertChecked()
{
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < count; ++row) setChecked(row, !isChecked(row));
    updateStatusBar();
}

// Select Group: the check boxes are set from scratch, on for the rows painted
// in the chosen tint and off for every other, whatever was checked before. The
// tint is the one the row shows (rowTint, with its precedence), so the group
// selected is the group seen: a render host that is offline is in Offline,
// not in Bryce 3D Net Render Host.
void selectGroup(int tint, const wchar_t* label)
{
    if (tint == TintOutdated && !refreshPushVersion()) return;
    int chosen = 0;
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < count; ++row) {
        bool in = rowTint(nodeAt(row)) == tint;
        setChecked(row, in);
        if (in) ++chosen;
    }
    updateStatusBar(widen(format("|  Select Group: %d client(s) in %s", chosen, narrow(label).c_str())).c_str());
}

void setOffline(NodeRecord* n, const std::string& reason)
{
    if (!n->online) return;
    n->online = false;
    n->pingPending = false;
    if (n->conn >= 0) g_server->close(n->conn);
    n->conn = -1;
    logEvent(format("%s offline: %s", n->name.c_str(), reason.c_str()));
    logNode(*n, "offline: " + reason);
    if (!n->pendingReplies.empty()) {
        std::string verbs;
        for (size_t i = 0; i < n->pendingReplies.size(); ++i) verbs += (i ? ", " : "") + n->pendingReplies[i].first;
        n->pendingReplies.clear();
        noReply(n, "went offline before answering " + verbs);
    }
    playEventSound(SoundThClientOffline);
    refreshRow(n);
    if (g_staticNode == n) showStaticPane(n);
}

// ---------------------------------------------------------------- report spacing
// Identical units booted together send HELLO within a second or two of each other
// and, left alone, keep reporting in that bunch for the whole session, because an
// agent times its next report from the one it has just sent. Thunderhead therefore
// answers every HELLO with the interval and a phase: the delay in milliseconds
// before the agent's first report, chosen as the middle of the widest gap between
// the reports expected from the other online nodes. The agent's own re-arming
// then keeps the node in that slot. Agents before 0.3.2 ignore the phase.
const char* const kPhaseAgent = "0.3.2";

// Where a node's next report falls within the interval, counted from now; -1 unknown.
int reportPhaseOf(const NodeRecord* n, unsigned long long now, long long intervalMs)
{
    if (n->nextReportMs == 0) return -1;
    long long d = (long long)(n->nextReportMs - now) % intervalMs;
    if (d < 0) d += intervalMs;
    return (int)d;
}

// The middle of the widest gap between the other online nodes' slots; the whole
// interval when this node is the only one.
int freeReportPhase(const NodeRecord* self, unsigned long long now)
{
    long long intervalMs = (long long)g_store->settings.intervalSec * 1000;
    std::vector<long long> slots;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i] == self || !nodes[i]->online) continue;
        int p = reportPhaseOf(nodes[i], now, intervalMs);
        if (p >= 0) slots.push_back(p);
    }
    if (slots.empty()) return (int)intervalMs;
    std::sort(slots.begin(), slots.end());
    long long bestStart = slots.back(), bestGap = slots.front() + intervalMs - slots.back();   // the gap across the wrap
    for (size_t i = 1; i < slots.size(); ++i) {
        long long gap = slots[i] - slots[i - 1];
        if (gap > bestGap) { bestGap = gap; bestStart = slots[i - 1]; }
    }
    return (int)((bestStart + bestGap / 2) % intervalMs);
}

// SET interval= phase= to one node, and the slot it is then expected to keep.
void sendSchedule(NodeRecord* n, int phaseMs, unsigned long long now)
{
    Message m("SET");
    m.set("interval", (long long)g_store->settings.intervalSec);
    m.set("phase", (long long)phaseMs);
    if (!g_server->send(n->conn, m)) { noReply(n, "SET could not be sent: its connection is gone"); return; }
    // No send sound for SET: the report-spacing nudges send one every few
    // seconds on a farm of this size, which would make the event continuous.
    logNode(*n, "-> " + m.oneLine());
    bool knowsPhase = compareVersions(n->hello.get("agent"), kPhaseAgent) >= 0;
    n->nextReportMs = now + (unsigned long long)(knowsPhase ? phaseMs : g_store->settings.intervalSec * 1000);
}

// Reports that arrive in the same clock second are nudged apart: the first one
// in a second is left alone, the second, third and fourth get 1, 2 and 3 s
// respectively, any further ones are left for the next interval. The nudge is
// given by sending the node SET interval= phase=interval that many seconds after
// its report; the agent times its next report from the SET, so the one interval
// then running is longer by the nudge and only that one, and the node's slot
// has moved by the nudge and stays there. Works with every agent, since the SET
// itself re-times the agent.
void scheduleNudge(NodeRecord* n, int place, unsigned long long now)
{
    if (n->nudgeDueMs) return;
    int nudgeSec = place - 1;
    n->nudgeDueMs = now + (unsigned long long)nudgeSec * 1000;
    static const char* const ordinal[] = { "", "first", "second", "third", "fourth" };
    logEvent(format("%s: %s report in the same clock second, its next interval is nudged +%d s", n->name.c_str(), ordinal[place], nudgeSec));
    SetTimer(g_main, kNudgeTimerId, 100, NULL);
}

void onNudgeTimer()
{
    unsigned long long now = tickMs();
    bool pending = false;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (!n->nudgeDueMs) continue;
        if (!n->online) { n->nudgeDueMs = 0; continue; }
        if (now < n->nudgeDueMs) { pending = true; continue; }
        n->nudgeDueMs = 0;
        sendSchedule(n, g_store->settings.intervalSec * 1000, now);
    }
    if (!pending) KillTimer(g_main, kNudgeTimerId);
}

void setIntervalAll(int sec)
{
    g_store->settings.intervalSec = sec;
    g_store->saveSettings();
    // Every online node is re-anchored by this SET, so the slots are dealt out
    // evenly in one pass instead of one gap at a time.
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    std::vector<NodeRecord*> online;
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->online) online.push_back(nodes[i]);
    unsigned long long now = tickMs();
    for (size_t i = 0; i < online.size(); ++i) {
        online[i]->nudgeDueMs = 0;
        sendSchedule(online[i], (int)((unsigned long long)sec * 1000 * (i + 1) / online.size()), now);
    }
    logEvent(online.empty() ? format("report interval set to %d s", sec)
                            : format("report interval set to %d s, %u node(s) spaced %d ms apart",
                                     sec, (unsigned)online.size(), (int)(sec * 1000 / (int)online.size())));
    updateStatusBar();
}

void removeCheckedOffline()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    std::vector<NodeRecord*> offline;
    for (size_t i = 0; i < targets.size(); ++i)
        if (!targets[i]->online) offline.push_back(targets[i]);
    if (offline.empty()) {
        MessageBoxW(g_main, L"Check one or more offline clients to remove.", L"Thunderhead", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring q = widen(format("Remove %d offline client(s) and their saved settings?", (int)offline.size()));
    if (MessageBoxW(g_main, q.c_str(), L"Thunderhead", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    endEdit(false);
    for (size_t i = 0; i < offline.size(); ++i) {
        if (g_staticNode == offline[i]) showStaticPane(NULL);
        int row = rowOf(offline[i]);
        if (row >= 0) SendMessageW(g_list, LVM_DELETEITEM, (WPARAM)row, 0);
        logEvent("removed client " + offline[i]->name + " " + offline[i]->mac);
        g_store->remove(offline[i]->mac);
    }
    g_store->save();
    updateStatusBar();
}

// ---------------------------------------------------------------- Play Sound

struct PlaySoundDlg { std::vector<std::wstring> files; };

std::wstring selectedSound(HWND dlg, const PlaySoundDlg* d)
{
    int i = (int)SendDlgItemMessageW(dlg, IDC_SOUND_LIST, LB_GETCURSEL, 0, 0);
    return (d && i >= 0 && i < (int)d->files.size()) ? d->files[i] : std::wstring();
}

// Plays a file here, on the host; a word when Thunderhead Audio is off.
void previewSound(HWND owner, const std::wstring& path, const wchar_t* title)
{
    if (path.empty()) return;
    if (soundMuted()) {
        MessageBoxW(owner, L"Thunderhead Audio is off (Settings), so nothing is heard here.", title, MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::string err;
    if (!soundPlay(path, err)) MessageBoxW(owner, widen(err).c_str(), title, MB_OK | MB_ICONWARNING);
}

INT_PTR CALLBACK playSoundDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    PlaySoundDlg* d = (PlaySoundDlg*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    switch (msg) {
    case WM_INITDIALOG: {
        d = (PlaySoundDlg*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        HWND list = GetDlgItem(dlg, IDC_SOUND_LIST);
        SendMessageW(list, WM_SETFONT, (WPARAM)g_font, TRUE);
        for (size_t i = 0; i < d->files.size(); ++i) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)d->files[i].c_str());
        if (!d->files.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
        int checkedOnline = 0;
        std::vector<NodeRecord*> targets = checkedNodes();
        for (size_t i = 0; i < targets.size(); ++i) if (targets[i]->online) ++checkedOnline;
        std::wstring info = widen(format("%u sound(s).  Play to Clients reaches %d checked online client(s).", (unsigned)d->files.size(), checkedOnline));
        if (soundMuted()) info += L"  Thunderhead Audio is off: Play Here is silent.";
        SetDlgItemTextW(dlg, IDC_SOUND_INFO, (info + L"\n" + soundDir()).c_str());
        return TRUE;
    }
    case WM_COMMAND:
        if ((LOWORD(wp) == IDC_SOUND_LIST && HIWORD(wp) == LBN_DBLCLK) || LOWORD(wp) == IDC_SOUND_PLAY_HERE) {
            std::wstring name = selectedSound(dlg, d);
            if (!name.empty()) previewSound(dlg, soundDir() + L"\\" + name, L"Play Sound");
            return TRUE;
        }
        if (LOWORD(wp) == IDC_SOUND_PLAY_CLIENTS) {
            std::wstring name = selectedSound(dlg, d);
            if (!name.empty()) sendPlaySound(name);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// Play Sound: the files of the sounds folder; one is played here or sent to
// the checked clients, which play their own copy of it.
void openPlaySound()
{
    PlaySoundDlg d;
    d.files = soundFilesIn(soundDir());
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SOUND), g_main, playSoundDlgProc, (LPARAM)&d);
}

// ---------------------------------------------------------------- sound scheme (Settings)

// The Default scheme plays each category's own file; the Custom scheme plays
// the file assigned to a category in the Settings dialog's scheme section,
// its default where none is assigned.
struct SchemeDlg {
    bool custom;
    std::map<std::string, std::string> sounds;   // category -> file name in the sounds folder
};

std::wstring schemeFileOf(const SchemeDlg* d, int cat)
{
    if (d->custom) {
        std::map<std::string, std::string>::const_iterator it = d->sounds.find(narrow(kSoundCategories[cat]));
        if (it != d->sounds.end() && !it->second.empty()) return widen(it->second);
    }
    return soundDefaultFile(kSoundCategories[cat]);
}

int selectedCategory(HWND dlg)
{
    return (int)SendDlgItemMessageW(dlg, IDC_SCHEME_LIST, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

void selectCategory(HWND dlg, int cat)
{
    LVITEMW item;
    memset(&item, 0, sizeof(item));
    item.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    item.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendDlgItemMessageW(dlg, IDC_SCHEME_LIST, LVM_SETITEMSTATE, (WPARAM)cat, (LPARAM)&item);
    SendDlgItemMessageW(dlg, IDC_SCHEME_LIST, LVM_ENSUREVISIBLE, (WPARAM)cat, FALSE);
}

void fillSchemeList(HWND dlg, const SchemeDlg* d)
{
    HWND list = GetDlgItem(dlg, IDC_SCHEME_LIST);
    int count = (int)SendMessageW(list, LVM_GETITEMCOUNT, 0, 0);
    for (int cat = 0; cat < kSoundCategoryCount; ++cat) {
        if (cat >= count) {
            LVITEMW item;
            memset(&item, 0, sizeof(item));
            item.mask = LVIF_TEXT;
            item.iItem = cat;
            item.pszText = (LPWSTR)kSoundCategories[cat];
            SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&item);
            const SoundEventDef* ev = soundEventOfCategory(kSoundCategories[cat]);
            setCell(list, cat, 1, ev ? std::wstring(ev->app) + L": " + ev->label : std::wstring());
        }
        std::wstring file = schemeFileOf(d, cat);
        bool isDefault = file == soundDefaultFile(kSoundCategories[cat]);
        bool exists = GetFileAttributesW((soundDir() + L"\\" + file).c_str()) != INVALID_FILE_ATTRIBUTES;
        setCell(list, cat, 2, file + (isDefault ? L"" : L"   (custom)") + (exists ? L"" : L"   [missing]"));
    }
    CheckRadioButton(dlg, IDC_SCHEME_DEFAULT, IDC_SCHEME_CUSTOM, d->custom ? IDC_SCHEME_CUSTOM : IDC_SCHEME_DEFAULT);
    EnableWindow(GetDlgItem(dlg, IDC_SCHEME_RESET), d->custom);
}

bool browseSoundFile(HWND owner, std::wstring& path)
{
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        MessageBoxW(owner, L"The file dialog could not be created.", L"Sound Schemes", MB_OK | MB_ICONERROR);
        return false;
    }
    const COMDLG_FILTERSPEC types[3] = { { L"Sound files (*.wav;*.mp3)", L"*.wav;*.mp3" }, { L"Wave files (*.wav)", L"*.wav" }, { L"MP3 files (*.mp3)", L"*.mp3" } };
    dlg->SetFileTypes(3, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"Select the sound file for the category");
    dlg->SetOkButtonLabel(L"Assign");
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = NULL;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = NULL;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; CoTaskMemFree(p); ok = true; }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// Whether name is one of the default scheme's own files, which are never overwritten.
bool isDefaultSoundName(const std::wstring& name)
{
    for (int cat = 0; cat < kSoundCategoryCount; ++cat)
        if (_wcsicmp(soundDefaultFile(kSoundCategories[cat]).c_str(), name.c_str()) == 0) return true;
    return false;
}

// Browse: a .wav or .mp3 for the selected category. A file from outside the
// sounds folder is copied into it first, so that the folder stays the one
// source of every sound, here and, with the next agent update, on the nodes.
void assignBrowsed(HWND dlg, SchemeDlg* d)
{
    int cat = selectedCategory(dlg);
    if (cat < 0) { MessageBoxW(dlg, L"Select the category first.", L"Sound Schemes", MB_OK | MB_ICONINFORMATION); return; }
    std::wstring path;
    if (!browseSoundFile(dlg, path)) return;
    if (!soundFileSupported(path)) { MessageBoxW(dlg, L"Only .wav and .mp3 files are played.", L"Sound Schemes", MB_OK | MB_ICONWARNING); return; }
    std::wstring name = leafName(path), dest = soundDir() + L"\\" + name;
    if (_wcsicmp(path.c_str(), dest.c_str()) != 0) {
        if (isDefaultSoundName(name)) {
            MessageBoxW(dlg, (name + L" is the name of a default scheme file, which is not replaced. Rename the file and assign it again.").c_str(),
                        L"Sound Schemes", MB_OK | MB_ICONWARNING);
            return;
        }
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES &&
            MessageBoxW(dlg, (name + L" is in the sounds folder already. Replace it with the selected file?").c_str(), L"Sound Schemes",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        CreateDirectoryW(soundDir().c_str(), NULL);
        if (!CopyFileW(path.c_str(), dest.c_str(), FALSE)) {
            MessageBoxW(dlg, widen("The file could not be copied into the sounds folder: " + lastErrorText((int)GetLastError())).c_str(),
                        L"Sound Schemes", MB_OK | MB_ICONERROR);
            return;
        }
    }
    d->sounds[narrow(kSoundCategories[cat])] = narrow(name);
    d->custom = true;
    fillSchemeList(dlg, d);
    selectCategory(dlg, cat);
}

// The scheme list's columns and colors; the colors follow the scheme in force.
void schemeListColors(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_SCHEME_LIST);
    const Theme& t = theme();
    SendMessageW(list, LVM_SETBKCOLOR, 0, (LPARAM)t.editBack);
    SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, (LPARAM)t.editBack);
    SendMessageW(list, LVM_SETTEXTCOLOR, 0, (LPARAM)t.editText);
}

void setupSchemeList(HWND dlg, const SchemeDlg* d)
{
    HWND list = GetDlgItem(dlg, IDC_SCHEME_LIST);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessageW(list, WM_SETFONT, (WPARAM)g_font, TRUE);
    schemeListColors(dlg);
    const wchar_t* titles[3] = { L"Category", L"Event", L"File in the sounds folder" };
    const int widths[3] = { 150, 220, 340 };
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)titles[i];
        col.cx = widths[i];
        col.iSubItem = i;
        SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    SetDlgItemTextW(dlg, IDC_SCHEME_INFO, L"Browse assigns a .wav or .mp3 file to the selected category of the Custom scheme.");
    fillSchemeList(dlg, d);
    selectCategory(dlg, 0);
}

// The scheme section's buttons and radios; true when the command was one of them.
bool schemeCommand(HWND dlg, int id, SchemeDlg* d)
{
    if (id == IDC_SCHEME_DEFAULT || id == IDC_SCHEME_CUSTOM) { d->custom = id == IDC_SCHEME_CUSTOM; fillSchemeList(dlg, d); return true; }
    if (id == IDC_SCHEME_BROWSE) { assignBrowsed(dlg, d); return true; }
    if (id == IDC_SCHEME_RESET) {
        int cat = selectedCategory(dlg);
        if (cat >= 0) { d->sounds.erase(narrow(kSoundCategories[cat])); fillSchemeList(dlg, d); selectCategory(dlg, cat); }
        return true;
    }
    if (id == IDC_SCHEME_PLAY) {
        int cat = selectedCategory(dlg);
        if (cat >= 0) previewSound(dlg, soundDir() + L"\\" + schemeFileOf(d, cat), L"Settings");
        return true;
    }
    return false;
}

// Saves a changed scheme in settings.txt and, since the agents' own events
// follow it, tells every online agent the new one.
void applySoundScheme(const SchemeDlg& d)
{
    if (d.custom == g_store->settings.customScheme && d.sounds == g_store->settings.sounds) return;
    g_store->settings.customScheme = d.custom;
    g_store->settings.sounds = d.sounds;
    g_store->saveSettings();
    logEvent(d.custom ? format("sound scheme: custom, %u assignment(s)", (unsigned)d.sounds.size()) : std::string("sound scheme: default"));
    sendSoundSchemeAll();
}

// ---------------------------------------------------------------- dialogs

// What the Settings dialog edits. The Dark Mode switch takes effect as it is
// flipped, so that the scheme can be judged; Cancel puts the old one back.
struct SettingsDlgData { int sec; bool dark; bool darkAtStart; bool thAudio; bool lrAudio; SchemeDlg scheme; };

void applyTheme(bool dark);

INT_PTR CALLBACK intervalDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsDlgData* d = (SettingsDlgData*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (msg == WM_DRAWITEM) {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->CtlID == IDC_DARK_SWITCH) { themeDrawSwitch(di, d && d->dark); return TRUE; }
        if (di->CtlID == IDC_TH_AUDIO_SWITCH) { themeDrawSwitch(di, d && d->thAudio); return TRUE; }
        if (di->CtlID == IDC_LR_AUDIO_SWITCH) { themeDrawSwitch(di, d && d->lrAudio); return TRUE; }
    }
    if (INT_PTR r = themeDialogMessage(dlg, msg, wp, lp)) return r;
    switch (msg) {
    case WM_INITDIALOG: {
        d = (SettingsDlgData*)lp;
        SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)d);
        SetDlgItemTextW(dlg, IDC_DARK_STATE, d->dark ? L"On" : L"Off");
        SetDlgItemTextW(dlg, IDC_TH_AUDIO_STATE, d->thAudio ? L"On" : L"Off");
        SetDlgItemTextW(dlg, IDC_LR_AUDIO_STATE, d->lrAudio ? L"On" : L"Off");
        HWND combo = GetDlgItem(dlg, IDC_INTERVAL_COMBO);
        int current = d->sec, selected = 0;
        for (int sec = 5, i = 0; sec <= 60; sec += 5, ++i) {
            wchar_t text[32];
            _snwprintf(text, 31, L"%d seconds", sec);
            text[31] = L'\0';
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
            SendMessageW(combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)sec);
            if (sec == current) selected = i;
        }
        SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
        setupSchemeList(dlg, &d->scheme);
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* h = (NMHDR*)lp;
        if (d && h->idFrom == IDC_SCHEME_LIST && h->code == NM_DBLCLK) {
            int cat = selectedCategory(dlg);
            if (cat >= 0) previewSound(dlg, soundDir() + L"\\" + schemeFileOf(&d->scheme, cat), L"Settings");
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        if (d && schemeCommand(dlg, LOWORD(wp), &d->scheme)) return TRUE;
        if (LOWORD(wp) == IDC_DARK_SWITCH && d) {
            d->dark = !d->dark;
            SetDlgItemTextW(dlg, IDC_DARK_STATE, d->dark ? L"On" : L"Off");
            applyTheme(d->dark);
            themeWindow(dlg);
            schemeListColors(dlg);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_TH_AUDIO_SWITCH && d) {
            d->thAudio = !d->thAudio;
            SetDlgItemTextW(dlg, IDC_TH_AUDIO_STATE, d->thAudio ? L"On" : L"Off");
            InvalidateRect(GetDlgItem(dlg, IDC_TH_AUDIO_SWITCH), NULL, TRUE);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_LR_AUDIO_SWITCH && d) {
            d->lrAudio = !d->lrAudio;
            SetDlgItemTextW(dlg, IDC_LR_AUDIO_STATE, d->lrAudio ? L"On" : L"Off");
            InvalidateRect(GetDlgItem(dlg, IDC_LR_AUDIO_SWITCH), NULL, TRUE);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK && d) {
            HWND combo = GetDlgItem(dlg, IDC_INTERVAL_COMBO);
            int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
            d->sec = i < 0 ? 0 : (int)SendMessageW(combo, CB_GETITEMDATA, (WPARAM)i, 0);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- network events

// ---------------------------------------------------------------- shared-folder gate, pairing, ping flash
// No command goes to a node whose shared folder this host has not read: the
// farm's security is Windows file sharing, so a host that cannot open
// \\node\LightningRod has no business commanding the node. SET, the report
// schedule, is the one message outside the gate, since it instructs nothing. A
// node's share is checked on a worker thread when its HELLO arrives, every
// minute while it fails, every ten minutes while it passes, and every few
// seconds for two minutes after Open Client's Shared Folder, where the
// credentials get typed. A readable share also receives farm.key, which pairs
// the agent (see Authentication). A node reached through an SMB1 relay is
// checked by the relay's RELAY op=probe instead and paired by a push through it.

struct ShareJob { std::string mac, name, ip; std::wstring share; std::string key; bool viaRelay; int relayConn; std::string relayName, relayIp; bool fresh; };
struct ShareResult { std::string mac; bool viaRelay; bool ok; std::wstring dir; std::string error; bool credential; std::wstring credDir; bool keyWritten; std::string keyError; };   // credDir: the path that refused the credentials

DWORD WINAPI shareCheckThread(LPVOID p)
{
    ShareJob* job = (ShareJob*)p;
    ShareResult* res = new ShareResult();
    res->mac = job->mac;
    res->viaRelay = job->viaRelay;
    res->ok = false;
    res->credential = false;
    res->keyWritten = false;
    if (job->viaRelay) {
        // Pairing only: farm.key is staged under relay\<job> on the relay's share
        // and RELAY op=push carries it on over SMB1. Reachability came from the probe.
        std::string relayJob = newRelayJob(), err;
        std::wstring stage = relayStageDir(job->relayIp, job->share, relayJob);
        bool written = false;
        if (!makeDirs(stage)) {
            err = "staging folder " + narrow(stage) + " on relay " + job->relayName + " could not be created: " + lastErrorText((int)GetLastError());
        } else {
            std::string note = copyKeyTo(stage, job->key, written);
            Message ack;
            if (!written) { err = note.size() > 2 ? note.substr(2) : std::string("no key to write"); removeTree(stage); }
            else if (!relayRequest(job->relayConn, relayMessage("push", relayJob, job->ip, job->share), 2 * 60 * 1000, ack, err)) removeTree(stage);
            else res->keyWritten = true;
        }
        res->ok = true;
        res->keyError = err;
        delete job;
        if (!PostMessageW(g_main, WM_SHARE_CHECKED, 0, (LPARAM)res)) delete res;
        return 0;
    }
    // One path only, the one the file transfers use: \\NAME\share when the name
    // resolves to the node's address, \\IP\share otherwise (freshly cloned nodes
    // share a name). No falling back from one to the other: a folder that opens
    // by address but not by name would still fail every copy, which goes by name.
    std::vector<std::wstring> dirs;
    if (!job->name.empty() && nameResolvesTo(job->name, job->ip)) dirs.push_back(L"\\\\" + widen(job->name) + L"\\" + job->share + L"\\");
    else dirs.push_back(L"\\\\" + widen(job->ip) + L"\\" + job->share + L"\\");
    if (job->fresh) {
        // A connection left from before (a credential since removed, or Explorer's
        // session) would answer in place of a fresh logon; it is dropped first so
        // that the check authenticates anew with what Windows holds now.
        std::wstring remote = dirs[0].substr(0, dirs[0].size() - 1);
        WNetCancelConnection2W(remote.c_str(), 0, TRUE);
    }
    std::string errors;
    for (size_t i = 0; i < dirs.size() && !res->ok; ++i) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dirs[i] + L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); res->ok = true; res->dir = dirs[i]; break; }
        int err = (int)GetLastError();
        errors += std::string(errors.empty() ? "" : "; ") + narrow(dirs[i]) + ": " + lastErrorText(err);
        if (isCredentialError(err)) { res->credential = true; res->credDir = dirs[i]; }
    }
    if (res->ok) {
        std::string note = copyKeyTo(res->dir, job->key, res->keyWritten);
        if (!res->keyWritten && note.size() > 2) res->keyError = note.substr(2);
    } else {
        res->error = errors;
    }
    delete job;
    if (!PostMessageW(g_main, WM_SHARE_CHECKED, 0, (LPARAM)res)) delete res;
    return 0;
}

// The UNC root of a client's shared folder, ending with a backslash: the path
// the gate verified while it stands, otherwise the name path, which is where
// Explorer asks for the credentials. Every file operation goes through this,
// so that what the gate checked is what the transfers use.
std::wstring shareRootOf(const NodeRecord* n)
{
    if (n->shareState == 1 && !n->sharePath.empty() && n->sharePath[n->sharePath.size() - 1] == L'\\') return n->sharePath;
    return L"\\\\" + widen(n->name.empty() ? n->ip : n->name) + L"\\" + widen(g_store->settings.shareName) + L"\\";
}

std::string shareStateText(const NodeRecord* n)
{
    if (n->shareState == 1) return "readable from this host: " + (n->sharePath.empty() ? std::string("through its relay") : narrow(n->sharePath));
    if (n->shareState == 2) return "not readable from this host (commands and reports refused): " + n->shareError;
    return n->online ? std::string("being checked (commands and reports refused until it passes)") : std::string("not checked yet");
}

const char* const kAuthAgent = "0.4.0";   // first agent version that knows farm.key

std::string authText(const NodeRecord* n)
{
    std::string agent = n->hello.get("agent");
    if (agent.empty()) return std::string();
    if (compareVersions(agent, kAuthAgent) < 0) return "none: agent " + agent + " is older than " + kAuthAgent + "; update it";
    if (n->authenticated) return "authenticated: its HELLO proved the farm key";
    if (n->keyed) return "not authenticated: the agent holds a farm.key but did not prove it";
    return "not paired yet: the agent holds no farm.key; it is written to its shared folder once that is readable, and the agent then reconnects";
}

bool nodeOutdated(const NodeRecord* n)
{
    if (!n->online || n->updating) return false;
    std::string against = g_pushVersion.empty() ? std::string(TLR_VERSION_STRING) : g_pushVersion;
    return compareVersions(n->hello.get("agent"), against) < 0;
}

bool commandAllowed(NodeRecord* n, const std::string& label)
{
    if (n->shareState == 1) return true;
    logEvent(format("%s not sent to %s: its shared folder is %s. Use Open Client's Shared Folder and enter the credentials Windows asks for",
                    label.c_str(), n->name.c_str(), shareStateText(n).c_str()));
    return false;
}

void startShareCheck(NodeRecord* n, const char* why)
{
    if (n->shareChecking) return;
    unsigned long long now = tickMs();
    if (NodeRecord* relay = relayFor(n)) {
        // The relay probes the SMB1 share; relayProbeResult takes the answer. The
        // relay itself must have passed the gate first, and the probe that its
        // own verification sends covers this node as well.
        if (relay->shareState != 1 || compareVersions(relay->hello.get("agent"), kRelayAgent) < 0) { n->shareNextMs = now + 15000; return; }
        Message probe = relayMessage("probe", std::string(), n->ip, widen(g_store->settings.shareName));
        if (!sendToNode(relay, probe, probe.oneLine())) { n->shareNextMs = now + 15000; return; }
        n->shareChecking = true;
        n->shareNextMs = now + 60000;
        return;
    }
    if (n->ip.empty()) { n->shareNextMs = now + 60000; return; }
    ShareJob* job = new ShareJob();
    job->mac = n->mac;
    job->name = n->name;
    job->ip = n->ip;
    job->share = widen(g_store->settings.shareName);
    job->key = g_farmKey;
    job->viaRelay = false;
    job->relayConn = -1;
    job->fresh = strcmp(why, "HELLO") == 0 || strcmp(why, "announce") == 0;   // a connecting node is checked with a fresh logon
    HANDLE t = CreateThread(NULL, 0, shareCheckThread, job, 0, NULL);
    if (!t) { delete job; n->shareNextMs = now + 60000; return; }
    CloseHandle(t);
    n->shareChecking = true;
    n->shareNextMs = now + 60000;
    logNode(*n, format("share check (%s%s) of %s's shared folder", why, job->fresh ? ", fresh logon" : "", n->name.c_str()));
}

// farm.key to a node behind an SMB1 relay, on a thread, through the relay.
void pairThroughRelay(NodeRecord* target, NodeRecord* relay)
{
    if (g_farmKey.empty() || relay->conn < 0) return;
    ShareJob* job = new ShareJob();
    job->mac = target->mac;
    job->name = target->name;
    job->ip = target->ip;
    job->share = widen(g_store->settings.shareName);
    job->key = g_farmKey;
    job->viaRelay = true;
    job->relayConn = relay->conn;
    job->relayName = relay->name;
    job->relayIp = relay->ip;
    job->fresh = false;
    HANDLE t = CreateThread(NULL, 0, shareCheckThread, job, 0, NULL);
    if (!t) { delete job; return; }
    CloseHandle(t);
    logEvent(format("%s: farm.key is being pushed to its share through relay %s", target->name.c_str(), relay->name.c_str()));
}

// What follows a HELLO once the node's share has been read: the audio switch,
// the sound scheme, and for a relay the probe of the share it relays to.
void shareVerified(NodeRecord* n)
{
    if (!n->online) return;
    if (n->helloFollowupPending) {
        n->helloFollowupPending = false;
        sendAudioSwitch(n, g_store->settings.lrAudio);   // All Lightning Rods Audio
        sendSoundScheme(n);                              // the files of the agent's own events
    }
    if (isRelay(n) && compareVersions(n->hello.get("agent"), kRelayAgent) >= 0) {
        Message probe = relayMessage("probe", std::string(), relayTarget(n), widen(g_store->settings.shareName));
        if (sendToNode(n, probe, probe.oneLine())) {
            if (NodeRecord* target = nodeOfRelayTarget(n)) { target->shareChecking = true; target->shareNextMs = tickMs() + 60000; }
        }
    }
    if (!n->keyed && compareVersions(n->hello.get("agent"), kAuthAgent) >= 0 && n->shareState == 1 && !n->sharePath.empty())
        logEvent(format("%s: the agent holds no farm.key yet; it reads the one written to its share and reconnects with authentication", n->name.c_str()));
}

void onShareChecked(ShareResult* r)
{
    NodeRecord* n = g_store->find(r->mac);
    if (!n) return;
    if (r->viaRelay) {
        if (r->keyWritten) logEvent(format("%s: farm.key pushed to its share through the relay", n->name.c_str()));
        else logEvent(format("%s: farm.key could not be pushed through the relay: %s", n->name.c_str(), r->keyError.c_str()));
        return;
    }
    n->shareChecking = false;
    int prev = n->shareState;
    unsigned long long now = tickMs();
    if (r->ok) {
        n->shareState = 1;
        n->sharePath = r->dir;
        n->shareError.clear();
        n->shareNextMs = now + 10 * 60 * 1000;
        if (prev != 1) logEvent(format("%s: shared folder %s is readable from this host%s", n->name.c_str(), narrow(r->dir).c_str(),
                                       r->keyWritten ? "; farm.key written to it" : ""));
        else if (r->keyWritten) logEvent(format("%s: farm.key written to %s", n->name.c_str(), narrow(r->dir).c_str()));
        if (!r->keyError.empty()) logEvent(format("%s: farm.key could not be written to %s: %s", n->name.c_str(), narrow(r->dir).c_str(), r->keyError.c_str()));
        if (n->reportsRefused) { n->reportsRefused = false; logEvent(format("%s: its reports are accepted again from the next one", n->name.c_str())); }
        shareVerified(n);
    } else {
        bool changed = prev != 2 || n->shareError != r->error;   // a new reason (the firewall opened, the share now refuses the logon) is logged as well
        n->shareState = 2;
        n->shareError = r->error;
        n->report = Message();   // nothing of an untrusted client is shown live
        n->shareNextMs = now + (now < n->shareRetryUntilMs ? 5000 : 60000);
        if (changed) logEvent(format("%s: shared folder not readable from this host (%s); no command goes to it until it is. "
                                     "%s",
                                     n->name.c_str(), r->error.c_str(),
                                     r->credential ? "The credential prompt follows" : "Check that the folder is shared and reachable, then use Open Client's Shared Folder"));
    }
    refreshRow(n);
    if (g_staticNode == n) showStaticPane(n);
    // A share that refused the credentials Windows offered gets the standard
    // credential prompt, once per HELLO (and again after Open Client's Shared
    // Folder): the farm's access challenge is Windows file sharing, so the
    // refusal is put to the operator rather than only logged.
    if (!r->ok && r->credential && !n->sharePrompted) {
        n->sharePrompted = true;
        CredRequest req; req.n = n; req.dir = r->credDir; req.update = false;
        g_credQueue.push_back(req);
        runCredQueue();
    }
}

// The relay's answer to RELAY op=probe stands in for the share check of the
// SMB1 node it relays to.
void relayProbeResult(NodeRecord* target, NodeRecord* relay, bool ok, const std::string& error)
{
    target->shareChecking = false;
    int prev = target->shareState;
    unsigned long long now = tickMs();
    target->shareState = ok ? 1 : 2;
    target->sharePath = ok ? L"\\\\" + widen(target->ip) + L"\\" + widen(g_store->settings.shareName) + L"\\ (SMB1, through relay " + widen(relay->name) + L")" : std::wstring();
    target->shareError = ok ? std::string() : error + " (probed by relay " + relay->name + ")";
    target->shareNextMs = now + (ok ? 10 * 60 * 1000 : 60000);
    if (prev != target->shareState) {
        if (ok) logEvent(format("%s: its SMB1 share opens from relay %s; commands may go to it", target->name.c_str(), relay->name.c_str()));
        else logEvent(format("%s: its SMB1 share does not open from relay %s (%s); no command goes to it until it does", target->name.c_str(), relay->name.c_str(), error.c_str()));
    }
    if (ok) {
        if (target->online && !target->keyed) pairThroughRelay(target, relay);
        shareVerified(target);
    }
    refreshRow(target);
    if (g_staticNode == target) showStaticPane(target);
}

// The row of a node that answered Lightning Rod Ping is painted in the ping
// color for one second; a 100 ms timer runs while any row is lit.
void startPingFlash(NodeRecord* n, unsigned long long now)
{
    n->pingFlashUntilMs = now + 1000;
    int row = rowOf(n);
    if (row >= 0) SendMessageW(g_list, LVM_REDRAWITEMS, (WPARAM)row, (LPARAM)row);
    SetTimer(g_main, kFlashTimerId, 100, NULL);
}

void onFlashTimer()
{
    unsigned long long now = tickMs();
    bool active = false;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (!n->pingFlashUntilMs) continue;
        if (now < n->pingFlashUntilMs) { active = true; continue; }
        n->pingFlashUntilMs = 0;
        int row = rowOf(n);
        if (row >= 0) SendMessageW(g_list, LVM_REDRAWITEMS, (WPARAM)row, (LPARAM)row);
    }
    if (!active) KillTimer(g_main, kFlashTimerId);
}

void onNet(NetEvent* e)
{
    unsigned long long now = tickMs();
    switch (e->type) {
    case NetEvent::Announce: {
        // Every ANNOUNCE is answered by the server thread; here it only shows a
        // known node as Announcing. The node is found by its MAC, or by its name
        // when the ANNOUNCE carries no MAC (an agent before 0.5.6 whose adapter
        // list came back empty). A node that keeps announcing without connecting
        // either sees no host on a sub-net it announces on (its log says which)
        // or holds another farm.key: the key is written to its shared folder when
        // that is readable, and it reconnects by itself.
        NodeRecord* n = e->msg.get("mac").empty() ? NULL : g_store->find(e->msg.get("mac"));
        if (!n) n = g_store->findByName(e->msg.get("name"));
        if (!n) break;   // a new node joins the inventory with its HELLO
        n->lastAnnounceMs = now;
        if (n->online) break;
        if (!e->ip.empty() && n->ip != e->ip) { n->ip = e->ip; g_store->save(); }   // the last known address outlives the session
        ++n->announcesUnanswered;
        if (n->announcesUnanswered == 4 || n->announcesUnanswered % 120 == 0) {
            logEvent(format("%s announces from %s (agent %s) but has not connected (%d announcements): it accepts no OFFER from this host, "
                            "because its announced sub-nets do not include this host's or its farm.key differs; its log says which; "
                            "the key is written to its shared folder when that is readable",
                            n->name.c_str(), n->ip.c_str(), e->msg.get("agent").c_str(), n->announcesUnanswered));
            startShareCheck(n, "announce");
        }
        refreshRow(n, ColStatus);
        refreshRow(n, ColIp);
        break;
    }
    case NetEvent::Hello: {
        std::string mac = e->msg.get("mac");
        if (mac.empty()) { g_server->close(e->conn); break; }
        NodeRecord* n = g_store->find(mac);
        bool isNew = n == NULL;
        if (!n) n = &g_store->add(mac);
        std::string name = e->msg.get("name");
        bool renamed = !name.empty() && name != n->name;
        if (renamed) n->name = name;
        if (isNew || renamed) g_store->save();
        // An agent sends HELLO again within its session when its Settings changed
        // (the SMB1 relay values); that renews the static fields and nothing else.
        bool again = n->online && n->conn == e->conn;
        if (n->online && n->conn != e->conn) g_server->close(n->conn);   // replaced by a new session
        n->online = true;
        n->conn = e->conn;
        if (n->ip != e->ip) { n->ip = e->ip; g_store->save(); }   // the last known address outlives the session
        n->hello = e->msg;
        if (!again) n->report = Message();
        clearAutoShutdownOfHost(n);   // the agent's own Render Host setting counts
        n->lastReportMs = now;
        if (!again) { n->nextReportMs = 0; n->nudgeDueMs = 0; }
        n->arrivalSec = utcNowMs() / 1000;   // a HELLO occupies its clock second like a report
        if (!again) n->onlineSinceMs = now;
        if (n->updating) {
            n->updating = false;
            logEvent(format("%s update finished: agent %s is back", n->name.c_str(), e->msg.get("agent").c_str()));
        }
        n->authenticated = e->authenticated;
        n->keyed = parseBool(e->msg.get("keyed"));
        n->announcesUnanswered = 0;
        // An agent from 0.4.0 answers Lightning Rod Ping with HELLO (then a full REPORT).
        bool pinged = again && n->pingPending;
        if (pinged) { pingReplied(n, "HELLO"); startPingFlash(n, now); }
        else if (again) logEvent(format("%s sent HELLO again (agent %s)", n->name.c_str(), e->msg.get("agent").c_str()));
        else logEvent(format("%s online from %s, agent %s%s; authentication: %s", n->name.c_str(), n->ip.c_str(),
                             e->msg.get("agent").c_str(), isNew ? " (new client)" : "", authText(n).c_str()));
        logNode(*n, "<- " + e->msg.oneLine());
        // The SET that answers HELLO: the interval and the node's report slot. A
        // renewed HELLO keeps the slot the node already holds.
        int phase = again ? reportPhaseOf(n, now, (long long)g_store->settings.intervalSec * 1000) : -1;
        if (phase < 0) phase = freeReportPhase(n, now);
        sendSchedule(n, phase, now);
        playEventSound(SoundThHello);                        // every HELLO, the renewed one as well
        if (isRelay(n)) {
            NodeRecord* target = nodeOfRelayTarget(n);
            logEvent(format("%s acts as SMB1 relay to %s%s", n->name.c_str(), relayTarget(n).c_str(),
                            target ? (" (" + (validIpText(relayTarget(n)) ? target->name : target->ip) + ")").c_str() : " (no known client at that address or name)"));
            n->relayProbe.clear();
            if (target && g_staticNode == target) showStaticPane(target);
        } else if (NodeRecord* via = relayFor(n)) {
            logEvent(format("%s: share operations go through SMB1 relay %s", n->name.c_str(), via->name.c_str()));
        }
        if (!again) {
            // Nothing but SET goes to the node until its shared folder has been
            // read from this host: the audio switch, the sound scheme and the
            // relay probe follow from onShareChecked (see the shared-folder gate).
            n->helloFollowupPending = true;
            n->shareState = 0;
            n->shareError.clear();
            n->shareRetryUntilMs = 0;
            n->sharePrompted = false;   // a connecting node gets the credential prompt again if its share refuses the logon
            startShareCheck(n, "HELLO");
        }
        refreshRow(n);
        if (g_staticNode == n) showStaticPane(n);
        break;
    }
    case NetEvent::Report: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        if (n->shareState != 1) {
            // Untrusted until its shared folder opens from here: the report keeps
            // the session alive and nothing more; its values are not taken and the
            // live columns stay blank. The first refusal goes to the event log.
            n->lastReportMs = now;
            n->nextReportMs = now + (unsigned long long)g_store->settings.intervalSec * 1000;
            n->arrivalSec = utcNowMs() / 1000;
            n->report = Message();
            logNode(*n, "<- " + e->msg.oneLine() + "  (discarded: shared folder not readable from this host)");
            if (!n->reportsRefused) {
                n->reportsRefused = true;
                logEvent(format("%s: its reports are discarded until its shared folder is readable from this host", n->name.c_str()));
            }
            refreshRow(n);
            if (g_staticNode == n) showStaticPane(n);
            break;
        }
        // A REPORT carries only the values that changed since the previous one.
        n->report.verb = e->msg.verb;
        for (std::map<std::string, std::string>::const_iterator it = e->msg.fields.begin(); it != e->msg.fields.end(); ++it)
            n->report.fields[it->first] = it->second;
        if (e->msg.has("render_host")) clearAutoShutdownOfHost(n);
        n->lastReportMs = now;
        n->nextReportMs = now + (unsigned long long)g_store->settings.intervalSec * 1000;
        // Report spacing: the place of this report among those that arrived in
        // the same clock second; the second to fourth are nudged.
        unsigned long long sec = utcNowMs() / 1000;
        int place = 1;
        std::vector<NodeRecord*>& all = g_store->nodes();
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i] != n && all[i]->online && all[i]->arrivalSec == sec) ++place;
        n->arrivalSec = sec;
        if (place >= 2 && place <= 4) scheduleNudge(n, place, now);
        logNode(*n, "<- " + e->msg.oneLine());
        if (e->msg.has("utc_ms")) {
            n->clockOffsetMs = (long long)(_strtoui64(e->msg.get("utc_ms").c_str(), NULL, 10) - utcNowMs());
            n->haveClockOffset = true;
        }
        if (n->pingPending) pingReplied(n, "report");
        refreshRow(n);
        if (g_staticNode == n) showStaticPane(n);
        break;
    }
    case NetEvent::Ack: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        logNode(*n, "<- " + e->msg.oneLine());
        bool ok = parseBool(e->msg.get("ok"));
        std::string cmd = e->msg.get("cmd"), text;
        replyArrived(n, cmd);
        if (cmd == "BATCH") {
            // Two acknowledgements per batch file: stage=started, then stage=finished.
            std::string file = e->msg.get("file");
            if (e->msg.get("stage") == "finished")
                text = format("BATCH %s finished on %s with exit code %s after %s s", file.c_str(),
                              n->name.c_str(), e->msg.get("exit").c_str(), e->msg.get("seconds").c_str());
            else if (ok) text = format("BATCH%s%s started on %s: %s", e->msg.has("kind") ? " " : "", e->msg.get("kind").c_str(), n->name.c_str(), file.c_str());
            else text = format("BATCH failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
        } else if (cmd == "TIME") {
            long long delta = atoll(e->msg.get("delta_s").c_str());
            std::string was = format("%lld s %s", delta < 0 ? -delta : delta, delta < 0 ? "behind" : "ahead");
            if (!ok) text = format("TIME: %s clock %s, not set: %s", n->name.c_str(), was.c_str(), e->msg.get("error").c_str());
            else if (parseBool(e->msg.get("set")))
                text = format("TIME: %s clock was %s (its date %s), set to the host's time", n->name.c_str(), was.c_str(),
                              e->msg.get("date").c_str());
            else text = format("TIME: %s clock %s, left alone", n->name.c_str(), was.c_str());
        } else if (cmd == "START" && e->msg.has("gen")) {
            std::string ver = e->msg.get("version"), path = e->msg.get("path");
            std::string what = "Lightning " + e->msg.get("gen") + (ver.empty() ? "" : " " + ver) + (path.empty() ? "" : " at " + path);
            if (ok) text = format("START ok on %s: %s, found through %s", n->name.c_str(), what.c_str(), e->msg.get("found").c_str());
            else text = format("START failed on %s: %s%s", n->name.c_str(), e->msg.get("error").c_str(),
                               path.empty() ? "" : (" (" + what + ", found through " + e->msg.get("found") + ")").c_str());
        } else if (cmd == "TEAMVIEWER") {
            if (ok) text = format("TEAMVIEWER ok on %s: started %s, found through %s%s", n->name.c_str(),
                                  e->msg.get("path").c_str(), e->msg.get("found").c_str(),
                                  parseBool(e->msg.get("running")) ? ", TeamViewer was already running" : "");
            else text = format("TEAMVIEWER failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
        } else if (cmd == "BRYCE") {
            std::string action = e->msg.get("action"), scene = e->msg.get("scene"), ver = e->msg.get("version"), path = e->msg.get("path");
            std::string what = std::string(bryceGenLabel(e->msg.getInt("gen", 3))) + (ver.empty() ? "" : " " + ver) + (path.empty() ? "" : " at " + path);
            if (ok) text = format("BRYCE %s ok on %s: %s%s%s%s%s%s", action.c_str(), n->name.c_str(), what.c_str(),
                                  e->msg.get("found").empty() ? "" : ", found through ", e->msg.get("found").c_str(),
                                  scene.empty() ? "" : "; scene ", scene.c_str(),
                                  parseBool(e->msg.get("restarted")) ? " (the Bryce that was open was ended first)" : "");
            if (!e->msg.get("prefs").empty()) logEvent(format("%s: Image with Scene Open/Save: %s", n->name.c_str(), e->msg.get("prefs").c_str()));
            if (!e->msg.get("image").empty()) logEvent(format("%s: gray image: %s", n->name.c_str(), e->msg.get("image").c_str()));
            else text = format("BRYCE %s failed on %s: %s%s%s", action.c_str(), n->name.c_str(), e->msg.get("error").c_str(),
                               path.empty() ? "" : (" (" + what + ")").c_str(), "");
        } else if (cmd == "SHARE") {
            std::string path = e->msg.get("path"), name = e->msg.get("name");
            if (ok) {
                text = format("SHARE create ok on %s: \\\\%s\\%s -> %s%s", n->name.c_str(), n->name.c_str(), name.c_str(), path.c_str(),
                              parseBool(e->msg.get("existed")) ? " (was shared already)" : "");
                // Checked by opening it from here, as the shared folders are.
                std::vector<std::string>* paths = new std::vector<std::string>();
                paths->push_back("\\\\" + n->name + "\\" + name + "\\");
                HANDLE t = CreateThread(NULL, 0, openSharedFoldersThread, paths, 0, NULL);
                if (t) CloseHandle(t); else delete paths;
            } else {
                text = format("SHARE create failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
            }
        } else if (cmd == "RENDERHOST") {
            bool hostOn = parseBool(e->msg.get("render_host"));
            std::string bin = e->msg.get("render_bin"), inShare = e->msg.get("render_bin_in_share");
            bool exists = parseBool(e->msg.get("render_bin_exists"));
            if (ok) text = format("RENDERHOST ok on %s: Net Render Host %s%s%s%s", n->name.c_str(), hostOn ? "on" : "off",
                                  bin.empty() ? "" : ", RENDER.BIN ", bin.c_str(), bin.empty() ? "" : exists ? " (exists)" : " (missing)");
            else text = format("RENDERHOST failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
            if (ok && hostOn && !bin.empty()) {
                // The folder is checked by opening it: through the share when it lies
                // inside the agent's folder, otherwise it cannot be reached from here.
                if (!inShare.empty() && exists) {
                    std::vector<std::string>* paths = new std::vector<std::string>();
                    paths->push_back(narrow(shareRootOf(n)) + inShare + "\\");
                    HANDLE t = CreateThread(NULL, 0, openSharedFoldersThread, paths, 0, NULL);
                    if (t) CloseHandle(t); else delete paths;
                } else if (!exists) {
                    logEvent(format("%s: RENDER.BIN %s does not exist and could not be created on the node", n->name.c_str(), bin.c_str()));
                } else {
                    logEvent(format("%s: RENDER.BIN %s lies outside its shared folder, so it cannot be opened from this host", n->name.c_str(), bin.c_str()));
                }
            }
        } else if (cmd == "RELAYSET") {
            if (ok) text = format("RELAYSET ok on %s: relay %s%s%s", n->name.c_str(), parseBool(e->msg.get("relay")) ? "on" : "off",
                                  parseBool(e->msg.get("relay")) ? " to " : "", e->msg.get("relay_ip").c_str());
            else text = format("RELAYSET failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
        } else if (cmd == "RELAY") {
            std::string op = e->msg.get("op"), dest = e->msg.get("dest");
            if (op == "probe") {
                n->relayProbe = ok ? "opens from there" : "does not open from there: " + e->msg.get("error");
                text = ok ? format("RELAY probe ok on %s: \\\\%s\\%s\\ opens from there", n->name.c_str(), dest.c_str(), g_store->settings.shareName.c_str())
                          : format("RELAY probe failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
                if (g_staticNode == n) showStaticPane(n);
                if (NodeRecord* target = nodeAtIp(dest)) relayProbeResult(target, n, ok, e->msg.get("error"));
            } else {
                relayAckDelivered(e->msg);   // the worker thread waiting for this job goes on
                std::string file = e->msg.get("file");
                text = ok ? format("RELAY %s ok on %s: %s file(s), %s %s \\\\%s\\%s\\%s%s", op.c_str(), n->name.c_str(), e->msg.get("files").c_str(),
                                   byteText(_strtoui64(e->msg.get("bytes").c_str(), NULL, 10)).c_str(), op == "pull" ? "from" : "to",
                                   dest.c_str(), g_store->settings.shareName.c_str(), file.empty() ? "" : ": ", file.c_str())
                          : format("RELAY %s failed on %s: %s", op.c_str(), n->name.c_str(), e->msg.get("error").c_str());
            }
        } else if (cmd == "MESSAGE") {
            // Two acknowledgements per message: stage=opened, then stage=closed.
            std::string title = e->msg.get("title");
            if (e->msg.get("stage") == "closed") {
                std::string how = e->msg.get("closed");
                text = format("MESSAGE \"%s\" closed on %s %s after %s s", title.c_str(), n->name.c_str(),
                              how == "ok" ? "with OK" : how == "timeout" ? "by its timer" : "with the Close button", e->msg.get("seconds").c_str());
            } else if (ok) {
                std::string soundErr = e->msg.get("sound_error");
                text = format("MESSAGE \"%s\" shown on %s%s%s", title.c_str(), n->name.c_str(), soundErr.empty() ? "" : ", sound not played: ", soundErr.c_str());
            } else {
                text = format("MESSAGE failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
            }
        } else if (cmd == "SOUND") {
            std::string action = e->msg.get("action"), file = e->msg.get("file");
            if (ok) text = format("SOUND %s ok on %s%s%s", action.c_str(), n->name.c_str(), file.empty() ? "" : ": ", file.c_str());
            else text = format("SOUND %s failed on %s: %s", action.c_str(), n->name.c_str(), e->msg.get("error").c_str());
        } else if (cmd == "AUDIO") {
            std::string action = e->msg.get("action"), method = e->msg.get("method");
            if (ok) text = format("AUDIO %s ok on %s: audio %s%s%s", action.c_str(), n->name.c_str(),
                                  parseBool(e->msg.get("muted")) ? "muted" : "unmuted",
                                  method.empty() ? "" : " via ", method.c_str());
            else text = format("AUDIO %s failed on %s: %s", action.c_str(), n->name.c_str(), e->msg.get("error").c_str());
        } else {
            if (cmd == "POWER") cmd += " " + e->msg.get("action");
            text = format("%s %s on %s%s%s", cmd.c_str(), ok ? "ok" : "failed",
                          n->name.c_str(), ok ? "" : ": ", ok ? "" : e->msg.get("error").c_str());
        }
        logEvent(text);
        updateStatusBar((L"|  " + widen(text)).c_str());
        if (e->msg.get("cmd") == "UPDATE" && !ok && n->updating) {
            n->updating = false;
            n->updateStaged = false;
            refreshRow(n);
        }
        if (e->msg.get("cmd") == "RENAME") {
            if (ok) {
                std::string newName = e->msg.get("name");
                if (!newName.empty() && newName != n->name) {
                    logEvent(format("%s will be %s after its reboot", n->name.c_str(), newName.c_str()));
                    n->name = newName;          // the per-node log switches to the new name from here
                    g_store->save();
                    refreshRow(n);
                    if (g_staticNode == n) showStaticPane(n);
                }
                if (n->rebootAfterRename) {
                    Message reboot("REBOOT");
                    if (sendGated(n, reboot)) {
                        logEvent("REBOOT -> " + n->name + " (after rename)");
                        logNode(*n, "-> " + reboot.oneLine());
                    }
                }
            }
            n->rebootAfterRename = false;
        }
        break;
    }
    case NetEvent::Pong: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        logNode(*n, "<- PONG");
        if (n->pingPending)
            pingReplied(n, format("PONG (agent %s is older than %s, no report)", n->hello.get("agent").c_str(), kPingReportAgent));
        else logEvent("PONG from " + n->name);
        break;
    }
    case NetEvent::Disconnected: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (n) {
            setOffline(n, e->text);
            if (n->updating && n->updateStaged) {
                if (n->updateViaRelay) logEvent(n->name + " update: staged through a relay; the node swaps the agent in by itself, waiting for it to reconnect");
                else startUpdateThread(n, updateFinishThread, "finish");
            } else if (n->updating) {
                n->updating = false;
                refreshRow(n);
                logEvent(n->name + " update: the node went offline while the agent was being staged");
            }
        }
        break;
    }
    case NetEvent::Info:
        logEvent(e->text);
        updateStatusBar((L"|  " + widen(e->text)).c_str());
        break;
    }
    updateStatusBar();
}

void onTimer()
{
    unsigned long long now = tickMs();
    unsigned long long limit = (unsigned long long)g_store->settings.intervalSec * kMissedReportsOffline * 1000 +
                               (unsigned long long)kOfflineGraceSec * 1000;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        NodeRecord* n = nodes[i];
        if (n->updating && !n->online && now - n->updateStartMs > 600000) {
            n->updating = false;
            n->updateStaged = false;
            logEvent(n->name + " update: the agent did not come back within 10 minutes");
            refreshRow(n);
        }
        if (!n->online) {
            if (n->lastAnnounceMs && now - n->lastAnnounceMs < 14000) refreshRow(n, ColStatus);   // Announcing ages into Offline
            continue;
        }
        if (n->shareChecking && now >= n->shareNextMs) n->shareChecking = false;   // a check that never answered
        if (!n->shareChecking && now >= n->shareNextMs) startShareCheck(n, n->shareState == 1 ? "periodic" : "retry");
        if (n->pingPending && now - n->pingSentMs > kPingWaitMs) {
            ++g_pingNoReply;
            noReply(n, format("no answer within %d s of PING", (int)(kPingWaitMs / 1000)));
            setOffline(n, format("no report within %d s of PING", (int)(kPingWaitMs / 1000)));
            pingStatus();
            continue;
        }
        // The reply watch: a command the agent should have acknowledged by now.
        for (size_t i = 0; i < n->pendingReplies.size();) {
            if (now - n->pendingReplies[i].second > kReplyWaitMs) {
                std::string verb = n->pendingReplies[i].first;
                n->pendingReplies.erase(n->pendingReplies.begin() + i);
                noReply(n, format("no ACK to %s within %d s", verb.c_str(), (int)(kReplyWaitMs / 1000)));
            } else ++i;
        }
        if (now - n->lastReportMs > limit) {
            setOffline(n, format("no report for %llu s", (now - n->lastReportMs) / 1000));
            continue;
        }
        refreshRow(n, ColLastReport);
    }
    static int pushVersionAge = 0;
    if (++pushVersionAge >= 60) {   // the agent file beside Thunderhead may have been replaced
        pushVersionAge = 0;
        std::string v = fileVersion(pushFilePath());
        if (v != g_pushVersion) { g_pushVersion = v; InvalidateRect(g_list, NULL, FALSE); }
    }
    if (g_staticNode) showStaticPane(g_staticNode);   // Connected for, Last report
    updateStatusBar();
}

// ---------------------------------------------------------------- layout

void layoutStaticPane()
{
    RECT rc;
    GetClientRect(g_staticPane, &rc);
    const int margin = 6, line = 18;
    int y = margin;
    MoveWindow(g_staticTitle, margin, y, rc.right - 2 * margin, line, TRUE);
    y += line + 4;
    MoveWindow(g_flagHeader, margin, y, rc.right - 2 * margin, line, TRUE);
    y += line + 2;
    int flagWidth = (rc.right - 2 * margin) / 3;
    if (flagWidth < 120) flagWidth = 120;
    const int pickerWidth = 96, pickerHeight = line + 4;
    // Row one: Wake On LAN, Boot on Alarm and its time; row two: the other three.
    MoveWindow(g_flags[0], margin, y, flagWidth, line, TRUE);
    const int alarmFlagWidth = 104;   // "Boot on Alarm" and its box; the picker follows it directly
    MoveWindow(g_flags[1], margin + flagWidth, y, alarmFlagWidth, line, TRUE);
    MoveWindow(g_bootAlarm, margin + flagWidth + alarmFlagWidth + 4, y - 2, pickerWidth, pickerHeight, TRUE);
    y += line + 4;
    for (int i = 2; i < 5; ++i) MoveWindow(g_flags[i], margin + flagWidth * (i - 2), y, flagWidth, line, TRUE);
    y += line + 4;
    MoveWindow(g_flags[5], margin, y, rc.right - 2 * margin, line, TRUE);   // row three: the render host flag
    y += line + 6;
    MoveWindow(g_staticList, margin, y, rc.right - 2 * margin, rc.bottom - y - margin, TRUE);
    int valueWidth = rc.right - 2 * margin - 112 - GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (valueWidth < 120) valueWidth = 120;
    SendMessageW(g_staticList, LVM_SETCOLUMNWIDTH, 1, (LPARAM)valueWidth);
}

// Command buttons: as many 200 px columns as the pane's width holds (200 px
// fits the longest label), widened to share the width. Every group starts a
// new row and gets a band of its own colour behind its rows. The pane's height
// comes from commandPaneHeight() so that every row is visible; the details
// pane above it takes what is left.
const int kCmdMargin = 6, kCmdBandPad = 4, kCmdGap = 4, kCmdGroupGap = 4, kCmdMinWidth = 200, kCmdHeight = 26;
bool drawCommandButton(const DRAWITEMSTRUCT* di);
void fillRect(HDC dc, const RECT& rc, COLORREF color);

struct CommandLayout {
    RECT buttons[kCommandCount];
    RECT bands[kGroupCount];
    int height;
};

int commandColumns(int width)
{
    int avail = width - 2 * (kCmdMargin + kCmdBandPad);
    int perRow = (avail + kCmdGap) / (kCmdMinWidth + kCmdGap);
    return perRow < 1 ? 1 : perRow;
}

CommandLayout commandLayout(int width)
{
    CommandLayout out;
    int cols = commandColumns(width);
    int x0 = kCmdMargin + kCmdBandPad;
    int bw = (width - 2 * x0 - (cols - 1) * kCmdGap) / cols;
    if (bw < kCmdMinWidth) bw = kCmdMinWidth;
    int y = kCmdMargin + kCmdBandPad;
    for (int g = 0; g < kGroupCount; ++g) {
        int n = 0;
        RECT band = { kCmdMargin, y - kCmdBandPad, width - kCmdMargin, 0 };
        for (int i = 0; i < kCommandCount; ++i) {
            if (kCommands[i].group != g || kCommands[i].parent) continue;   // nested commands live in their parent's pop-up
            RECT r = { x0 + (n % cols) * (bw + kCmdGap), y + (n / cols) * (kCmdHeight + kCmdGap), 0, 0 };
            r.right = r.left + bw;
            r.bottom = r.top + kCmdHeight;
            out.buttons[i] = r;
            ++n;
        }
        int rows = (n + cols - 1) / cols;
        y += rows * (kCmdHeight + kCmdGap) - kCmdGap;
        band.bottom = y + kCmdBandPad;
        out.bands[g] = band;
        y = band.bottom + kCmdGroupGap + kCmdBandPad;
    }
    out.height = out.bands[kGroupCount - 1].bottom + kCmdMargin;
    return out;
}

int commandPaneHeight(int width)
{
    return commandLayout(width - 2 * GetSystemMetrics(SM_CXEDGE)).height + 2 * GetSystemMetrics(SM_CYEDGE);
}

void layoutCommandPane()
{
    RECT rc;
    GetClientRect(g_commandPane, &rc);
    CommandLayout cl = commandLayout(rc.right);
    for (int i = 0; i < kCommandCount; ++i) {
        if (kCommands[i].parent) continue;
        const RECT& r = cl.buttons[i];
        MoveWindow(GetDlgItem(g_commandPane, kCommands[i].id), r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    }
    InvalidateRect(g_commandPane, NULL, TRUE);
}

// pct parts of a to 100 - pct parts of b
COLORREF mixColor(COLORREF a, COLORREF b, int pct)
{
    return RGB((GetRValue(a) * pct + GetRValue(b) * (100 - pct)) / 100,
               (GetGValue(a) * pct + GetGValue(b) * (100 - pct)) / 100,
               (GetBValue(a) * pct + GetBValue(b) * (100 - pct)) / 100);
}

// The bands behind the button groups.
void paintCommandPaneBackground(HDC dc)
{
    RECT rc;
    const Theme& t = theme();
    GetClientRect(g_commandPane, &rc);
    FillRect(dc, &rc, themeBrush(t.paneBack));
    CommandLayout cl = commandLayout(rc.right);
    for (int g = 0; g < kGroupCount; ++g) {
        HBRUSH b = CreateSolidBrush(mixColor(kGroups[g].band, t.bandMix, t.bandPct));
        FillRect(dc, &cl.bands[g], b);
        DeleteObject(b);
    }
}

// ---------------------------------------------------------------- nested command pop-ups
// A parent button (Select Group, Command File, Utilities, Configure Client,
// Bryce 3D) opens a small pop-up window under itself holding its nested
// commands as buttons of the same colour, one per row. A click on one posts
// the command to the main window and closes the pop-up; so does a click
// anywhere else (the pop-up closes when it is deactivated). One pop-up at a
// time.
HWND g_commandPopup = NULL;
int g_commandPopupParent = 0;
const wchar_t* const kCommandPopupClass = L"ThunderheadCommandPopup";
const int kPopupPad = 4;

void closeCommandPopup()
{
    if (g_commandPopup) DestroyWindow(g_commandPopup);
}

LRESULT CALLBACK commandPopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= IDC_CMD_FIRST && id <= IDC_CMD_LAST) {
            PostMessageW(g_main, WM_COMMAND, (WPARAM)id, 0);
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case WM_CTLCOLORBTN:
        if (HBRUSH b = themeCtlColor(msg, (HDC)wp, (HWND)lp)) return (LRESULT)b;
        break;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        const CommandDef* def = commandDef(g_commandPopupParent);
        const Theme& t = theme();
        COLORREF band = def ? mixColor(kGroups[def->group].band, t.bandMix, t.bandPct) : t.paneBack;
        fillRect((HDC)wp, rc, band);
        HBRUSH edge = CreateSolidBrush(mixColor(band, t.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 60));
        FrameRect((HDC)wp, &rc, edge);
        DeleteObject(edge);
        return 1;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON && drawCommandButton(di)) return TRUE;
        break;
    }
    case WM_DESTROY:
        g_commandPopup = NULL;
        g_commandPopupParent = 0;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void openCommandPopup(int parentId)
{
    if (g_commandPopup && g_commandPopupParent == parentId) { closeCommandPopup(); return; }
    closeCommandPopup();
    HWND parentButton = GetDlgItem(g_commandPane, parentId);
    if (!parentButton) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = commandPopupProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = kCommandPopupClass;
        RegisterClassW(&wc);
        registered = true;
    }
    std::vector<const CommandDef*> items;
    for (int i = 0; i < kCommandCount; ++i) if (kCommands[i].parent == parentId) items.push_back(&kCommands[i]);
    if (items.empty()) return;
    RECT pr;
    GetWindowRect(parentButton, &pr);
    int bw = pr.right - pr.left;
    if (bw < kCmdMinWidth) bw = kCmdMinWidth;
    int w = bw + 2 * kPopupPad, h = (int)items.size() * (kCmdHeight + kCmdGap) - kCmdGap + 2 * kPopupPad;
    // Under the parent button, raised above it when the screen ends first.
    HMONITOR mon = MonitorFromRect(&pr, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = pr.left, y = pr.bottom + 1;
    if (y + h > mi.rcWork.bottom) y = pr.top - 1 - h;
    if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
    g_commandPopupParent = parentId;
    g_commandPopup = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kCommandPopupClass, L"", WS_POPUP | WS_BORDER,
                                     x, y, w, h, g_main, NULL, g_inst, NULL);
    if (!g_commandPopup) { g_commandPopupParent = 0; return; }
    for (size_t i = 0; i < items.size(); ++i) {
        HWND b = CreateWindowExW(0, L"BUTTON", items[i]->label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 kPopupPad, kPopupPad + (int)i * (kCmdHeight + kCmdGap), bw, kCmdHeight,
                                 g_commandPopup, (HMENU)(INT_PTR)items[i]->id, g_inst, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        EnableWindow(b, items[i]->live);
    }
    ShowWindow(g_commandPopup, SW_SHOWNA);
    SetActiveWindow(g_commandPopup);
    SetFocus(GetDlgItem(g_commandPopup, items[0]->id));
}

// An owner-drawn command button: the group's tint on the face, a darker edge
// of the same hue, the face pressed darker and disabled paler with grey text.
// In Dark Mode the tints are mixed towards near-black and the edge and the
// pressed face go lighter instead. A button that opens a pop-up of nested
// commands gets a second, inner frame two pixels inside the edge: the double
// border says that more options lie behind it.
bool drawCommandButton(const DRAWITEMSTRUCT* di)
{
    const CommandDef* def = commandDef((int)di->CtlID);
    if (!def) return false;
    const CommandGroup& g = kGroups[def->group];
    const Theme& t = theme();
    bool disabled = (di->itemState & ODS_DISABLED) != 0, pressed = (di->itemState & ODS_SELECTED) != 0;
    COLORREF band = mixColor(g.band, t.bandMix, t.bandPct), base = mixColor(g.face, t.bandMix, t.facePct);
    COLORREF towards = t.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
    COLORREF face = disabled ? mixColor(base, band, 35) : pressed ? mixColor(base, towards, 86) : base;
    COLORREF edge = mixColor(base, towards, disabled ? 80 : 62);
    RECT rc = di->rcItem;
    HBRUSH b = CreateSolidBrush(face);
    FillRect(di->hDC, &rc, b);
    DeleteObject(b);
    b = CreateSolidBrush(edge);
    FrameRect(di->hDC, &rc, b);
    bool nested = hasNested(def->id);
    if (nested) {
        RECT inner = rc;
        InflateRect(&inner, -3, -3);
        FrameRect(di->hDC, &inner, b);
    }
    DeleteObject(b);
    wchar_t text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    HGDIOBJ oldFont = SelectObject(di->hDC, g_font);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? t.grayText : t.text);
    RECT tr = rc;
    if (pressed) OffsetRect(&tr, 1, 1);
    DrawTextW(di->hDC, text, -1, &tr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(di->hDC, oldFont);
    if (di->itemState & ODS_FOCUS) {
        RECT fr = rc;
        InflateRect(&fr, nested ? -5 : -3, nested ? -5 : -3);
        DrawFocusRect(di->hDC, &fr);
    }
    return true;
}

void layout()
{
    endEdit(true);
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(g_status, &sb);
    int height = rc.bottom - (sb.bottom - sb.top);
    // Client Inventory: two thirds of the width; details above the command
    // buttons in the remaining third. The command pane is as tall as its rows
    // need, the details pane keeps at least kDetailsMinHeight.
    const int kDetailsMinHeight = 160;
    int left = rc.right * 2 / 3;
    int top = height - commandPaneHeight(rc.right - left);
    if (top < kDetailsMinHeight) top = kDetailsMinHeight;
    MoveWindow(g_inventoryPane, 0, 0, left, height, TRUE);
    MoveWindow(g_staticPane, left, 0, rc.right - left, top, TRUE);
    MoveWindow(g_commandPane, left, top, rc.right - left, height - top, TRUE);
    RECT inner;
    GetClientRect(g_inventoryPane, &inner);
    MoveWindow(g_list, 0, 0, inner.right, inner.bottom, TRUE);
    layoutStaticPane();
    layoutCommandPane();
}

// ---------------------------------------------------------------- window procedures

LRESULT CALLBACK paneProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
        return SendMessageW(g_main, msg, wp, lp);
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (HBRUSH b = themeCtlColor(msg, (HDC)wp, (HWND)lp)) return (LRESULT)b;
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_ERASEBKGND: {
        if (hwnd == g_commandPane) { paintCommandPaneBackground((HDC)wp); return 1; }
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, themeBrush(hwnd == g_inventoryPane ? theme().windowBack : theme().paneBack));
        return 1;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON && hwnd == g_commandPane && drawCommandButton(di)) return TRUE;
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Paints the Auto Shutdown cell of one row as a check box after the list has drawn
// the row, using the list's own state images so it matches the row check box.
void drawAutoShutdownCell(HDC dc, int row, bool on)
{
    RECT rc;
    rc.top = ColAutoShutdown;
    rc.left = LVIR_BOUNDS;
    if (!SendMessageW(g_list, LVM_GETSUBITEMRECT, (WPARAM)row, (LPARAM)&rc)) return;
    HIMAGELIST il = (HIMAGELIST)SendMessageW(g_list, LVM_GETIMAGELIST, LVSIL_STATE, 0);
    int cx = 0, cy = 0;
    if (!il || !ImageList_GetIconSize(il, &cx, &cy)) return;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    ImageList_Draw(il, on ? 1 : 0, dc, rc.left + (rc.right - rc.left - cx) / 2, rc.top + (rc.bottom - rc.top - cy) / 2, ILD_TRANSPARENT);
    RestoreDC(dc, saved);
}

// The Client Inventory's appearance: a near-black header with white captions,
// rows shaded light and medium gray in turn, and a black rule between the
// columns (the reverse in Dark Mode; the colors are the scheme's), all painted
// through custom draw so that the list keeps its own selection, check boxes
// and in-place editing. The Client Details list shares the header painting.

HWND headerOf(HWND list) { return list ? (HWND)SendMessageW(list, LVM_GETHEADER, 0, 0) : NULL; }
HWND listHeader() { return headerOf(g_list); }

void fillRect(HDC dc, const RECT& rc, COLORREF color)
{
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

// The vertical rules at the right edge of every column, between top and
// bottom in the list's client coordinates. The edges come from the list's own
// column order, column widths and scroll position, which is how the list lays
// its cells out; the header window's item rectangles were used before, and
// during a horizontal scroll the header moves a moment after the rows repaint,
// which left rules at the old positions inside the data.
void drawColumnRules(HDC dc, int top, int bottom)
{
    int x;
    RECT first;
    first.left = LVIR_BOUNDS;
    if (SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0) > 0 && SendMessageW(g_list, LVM_GETITEMRECT, 0, (LPARAM)&first)) x = first.left;
    else x = -GetScrollPos(g_list, SB_HORZ);
    int order[ColCount];
    if (!SendMessageW(g_list, LVM_GETCOLUMNORDERARRAY, (WPARAM)ColCount, (LPARAM)order))
        for (int i = 0; i < ColCount; ++i) order[i] = i;
    HPEN pen = CreatePen(PS_SOLID, 1, theme().columnRule);
    HGDIOBJ old = SelectObject(dc, pen);
    for (int k = 0; k < ColCount; ++k) {
        x += (int)SendMessageW(g_list, LVM_GETCOLUMNWIDTH, (WPARAM)order[k], 0);
        MoveToEx(dc, x - 1, top, NULL);
        LineTo(dc, x - 1, bottom);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

// The check box column of one row, which the list leaves in its own
// background color: painted over in the row's shade after the list has drawn
// the row, with the check box put back from the list's state images.
void drawCheckBoxCell(HDC dc, int row, COLORREF shade)
{
    RECT bounds, label;
    bounds.left = LVIR_BOUNDS;
    label.left = LVIR_LABEL;
    if (!SendMessageW(g_list, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&bounds)) return;
    if (!SendMessageW(g_list, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&label)) return;
    RECT rc = bounds;
    rc.right = label.left;
    if (rc.right <= rc.left) return;
    fillRect(dc, rc, shade);
    HIMAGELIST il = (HIMAGELIST)SendMessageW(g_list, LVM_GETIMAGELIST, LVSIL_STATE, 0);
    int cx = 0, cy = 0;
    if (!il || !ImageList_GetIconSize(il, &cx, &cy)) return;
    UINT state = (UINT)SendMessageW(g_list, LVM_GETITEMSTATE, (WPARAM)row, LVIS_STATEIMAGEMASK);
    int image = (int)((state & LVIS_STATEIMAGEMASK) >> 12) - 1;
    if (image < 0) return;
    ImageList_Draw(il, image, dc, rc.left + (rc.right - rc.left - cx) / 2, rc.top + (rc.bottom - rc.top - cy) / 2, ILD_TRANSPARENT);
}

// The shade of a row: the alternating gray of the scheme, or that gray tinted
// to state the node's condition. Strongest first: the ping flash, solid for a
// second after the node answered Lightning Rod Ping; offline, red; shared
// folder not readable from this host, orange; agent older than the one to
// push, yellow; the Bryce 3D Network Render Host, metallic blue; a Windows 7
// client acting as SMB1 relay, turquoise.
// The conditions rowShade paints, as one number, so that a change is seen.
int shadeKey(const NodeRecord* n)
{
    if (!n) return 0;
    return (n->online ? 1 : 0) | (n->shareState == 1 ? 2 : 0) | (nodeOutdated(n) ? 4 : 0) | (isRelay(n) ? 8 : 0) |
           ((n->pingFlashUntilMs && tickMs() < n->pingFlashUntilMs) ? 16 : 0) | (isRenderHost(n) ? 32 : 0);
}

// The condition a row's tint states, strongest first; the ping flash is a
// momentary overlay, not a condition. Select Group selects by this.
int rowTint(const NodeRecord* n)
{
    if (!n) return TintNone;
    if (!n->online) return TintOffline;
    if (n->shareState != 1) return TintUnauthenticated;
    if (nodeOutdated(n)) return TintOutdated;
    if (isRenderHost(n)) return TintRenderHost;
    if (isRelay(n)) return TintRelay;
    return TintNone;
}

COLORREF rowShade(const NodeRecord* n, int row, COLORREF* textColor)
{
    const Theme& t = theme();
    COLORREF base = row % 2 == 0 ? t.rowLight : t.rowMedium;
    if (textColor) *textColor = t.rowText;
    if (!n) return base;
    if (n->pingFlashUntilMs && tickMs() < n->pingFlashUntilMs) {
        if (textColor) *textColor = RGB(255, 255, 255);
        return RGB(128, 0, 168);   // R 50 %, G 0 %, B 66 %
    }
    int pct = t.dark ? 30 : 22;
    switch (rowTint(n)) {
    case TintOffline:         return mixColor(RGB(255, 0, 0), base, pct);
    case TintUnauthenticated: return mixColor(RGB(255, 140, 0), base, pct);
    case TintOutdated:        return mixColor(RGB(255, 220, 0), base, pct);
    case TintRenderHost:      return mixColor(RGB(50, 82, 123), base, pct + 10);   // metallic blue: the Bryce 3D Network Render Host
    case TintRelay:           return mixColor(RGB(64, 224, 208), base, pct);
    }
    return base;
}

// The part of the list's client area below its last row (or below the header
// when the list is empty); false when there is none.
bool blankBelowRows(RECT& rc)
{
    GetClientRect(g_list, &rc);
    HWND header = listHeader();
    if (header) {
        RECT hr;
        GetWindowRect(header, &hr);
        MapWindowPoints(NULL, g_list, (POINT*)&hr, 2);
        if (hr.bottom > rc.top) rc.top = hr.bottom;
    }
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    if (count > 0) {
        RECT last;
        last.left = LVIR_BOUNDS;
        if (SendMessageW(g_list, LVM_GETITEMRECT, (WPARAM)(count - 1), (LPARAM)&last) && last.bottom > rc.top) rc.top = last.bottom;
    }
    return rc.top < rc.bottom;
}

LRESULT onListCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT: {
        // The row's shade goes through clrTextBk only: the list paints it under
        // the text it draws, so the two always agree. Painting the row here
        // ourselves blanked rows under the pointer, because the hot-tracking
        // repaint hands the handler a DC covering the whole row while the
        // list itself redraws only part of it.
        int row = (int)cd->nmcd.dwItemSpec;
        const NodeRecord* n = (const NodeRecord*)cd->nmcd.lItemlParam;
        COLORREF text;
        cd->clrTextBk = rowShade(n, row, &text);
        cd->clrText = text;
        return CDRF_NOTIFYPOSTPAINT;
    }
    case CDDS_ITEMPOSTPAINT: {
        int row = (int)cd->nmcd.dwItemSpec;
        const NodeRecord* n = (const NodeRecord*)cd->nmcd.lItemlParam;
        drawCheckBoxCell(cd->nmcd.hdc, row, rowShade(n, row, NULL));
        if (n && !isRenderHost(n)) drawAutoShutdownCell(cd->nmcd.hdc, row, n->autoShutdown);   // a Render Host has no Auto Shutdown
        RECT rc;
        rc.left = LVIR_BOUNDS;
        if (SendMessageW(g_list, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&rc)) drawColumnRules(cd->nmcd.hdc, rc.top, rc.bottom);
        return CDRF_DODEFAULT;
    }
    case CDDS_POSTPAINT: {
        // The stretch below the last row, which no item paints.
        RECT rc;
        if (blankBelowRows(rc)) drawColumnRules(cd->nmcd.hdc, rc.top, rc.bottom);
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

// The header, painted item by item: dark face, a gray rule at each column's
// right edge, the caption in white with the column's alignment, and the sort
// arrow as a small white triangle above the caption. The list forwards the
// header's custom draw notifications to this window.
LRESULT onHeaderCustomDraw(NMCUSTOMDRAW* cd)
{
    HWND header = cd->hdr.hwndFrom;
    switch (cd->dwDrawStage) {
    case CDDS_PREPAINT: {
        RECT rc;
        GetClientRect(header, &rc);
        fillRect(cd->hdc, rc, theme().headerBack);
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    }
    case CDDS_ITEMPREPAINT: {
        int col = (int)cd->dwItemSpec;
        RECT rc = cd->rc;
        fillRect(cd->hdc, rc, theme().headerBack);
        RECT rule = rc;
        rule.left = rc.right - 1;
        fillRect(cd->hdc, rule, theme().headerRule);
        wchar_t text[64] = L"";
        HDITEMW hi;
        memset(&hi, 0, sizeof(hi));
        hi.mask = HDI_TEXT | HDI_FORMAT;
        hi.pszText = text;
        hi.cchTextMax = 64;
        SendMessageW(header, HDM_GETITEMW, (WPARAM)col, (LPARAM)&hi);
        if (hi.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
            int cx = (rc.left + rc.right) / 2, y = rc.top + 1;
            POINT up[3]   = { { cx - 4, y + 4 }, { cx + 5, y + 4 }, { cx, y - 1 } };
            POINT down[3] = { { cx - 4, y },     { cx + 5, y },     { cx, y + 5 } };
            HBRUSH white = CreateSolidBrush(theme().headerText);
            HGDIOBJ oldBrush = SelectObject(cd->hdc, white);
            HGDIOBJ oldPen = SelectObject(cd->hdc, GetStockObject(NULL_PEN));
            Polygon(cd->hdc, (hi.fmt & HDF_SORTUP) ? up : down, 3);
            SelectObject(cd->hdc, oldPen);
            SelectObject(cd->hdc, oldBrush);
            DeleteObject(white);
        }
        RECT tr = rc;
        tr.left += 6;
        tr.right -= 7;
        if (cd->uItemState & CDIS_SELECTED) OffsetRect(&tr, 1, 1);
        UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        UINT just = hi.fmt & HDF_JUSTIFYMASK;
        fmt |= just == HDF_RIGHT ? DT_RIGHT : just == HDF_CENTER ? DT_CENTER : DT_LEFT;
        HGDIOBJ oldFont = SelectObject(cd->hdc, g_font);
        int oldMode = SetBkMode(cd->hdc, TRANSPARENT);
        COLORREF oldColor = SetTextColor(cd->hdc, theme().headerText);
        DrawTextW(cd->hdc, text, -1, &tr, fmt);
        SetTextColor(cd->hdc, oldColor);
        SetBkMode(cd->hdc, oldMode);
        SelectObject(cd->hdc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    case CDDS_POSTPAINT: {
        // The blank stretch right of the last column, which the header paints
        // after its items.
        RECT rc;
        GetClientRect(header, &rc);
        int count = (int)SendMessageW(header, HDM_GETITEMCOUNT, 0, 0);
        for (int i = 0; i < count; ++i) {
            RECT ir;
            if (SendMessageW(header, HDM_GETITEMRECT, (WPARAM)i, (LPARAM)&ir) && ir.right > rc.left) rc.left = ir.right;
        }
        if (rc.left < rc.right) fillRect(cd->hdc, rc, theme().headerBack);
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

LRESULT onListNotify(NMHDR* hdr)
{
    switch (hdr->code) {
    case NM_CUSTOMDRAW:
        return onListCustomDraw((NMLVCUSTOMDRAW*)hdr);
    case NM_CLICK: {
        NMITEMACTIVATE* ia = (NMITEMACTIVATE*)hdr;
        LVHITTESTINFO ht;
        memset(&ht, 0, sizeof(ht));
        ht.pt = ia->ptAction;
        SendMessageW(g_list, LVM_SUBITEMHITTEST, 0, (LPARAM)&ht);
        if (ht.iItem >= 0 && (ht.iSubItem == ColGroup || ht.iSubItem == ColComment) && (ht.flags & LVHT_ONITEM)) {
            beginEdit(ht.iItem, ht.iSubItem);
        } else {
            endEdit(true);
            if (ht.iItem >= 0 && ht.iSubItem == ColAutoShutdown && (ht.flags & LVHT_ONITEM)) toggleAutoShutdown(ht.iItem);
        }
        if (ht.iItem >= 0) showStaticPane(nodeAt(ht.iItem));
        break;
    }
    case LVN_ITEMCHANGED: {
        NMLISTVIEW* lv = (NMLISTVIEW*)hdr;
        if ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK) updateStatusBar();
        if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) showStaticPane(nodeAt(lv->iItem));
        break;
    }
    case LVN_BEGINSCROLL:
        endEdit(true);
        break;
    case LVN_COLUMNCLICK:
        endEdit(true);
        sortByColumn(((NMLISTVIEW*)hdr)->iSubItem);
        break;
    }
    return 0;
}

// Dresses every window for the scheme: the main window and its children, the
// two lists' base colors, the status bar, and every other top-level window of
// this thread (log views, the copy progress) through WM_THEME_CHANGED.
BOOL CALLBACK themeOtherWindow(HWND h, LPARAM)
{
    if (h != g_main) SendMessageW(h, WM_THEME_CHANGED, 0, 0);
    return TRUE;
}

void applyTheme(bool dark)
{
    setDarkMode(dark);
    const Theme& t = theme();
    themeWindow(g_main);
    SendMessageW(g_list, LVM_SETBKCOLOR, 0, (LPARAM)t.listBack);
    SendMessageW(g_staticList, LVM_SETBKCOLOR, 0, (LPARAM)t.editBack);
    SendMessageW(g_staticList, LVM_SETTEXTBKCOLOR, 0, (LPARAM)t.editBack);
    SendMessageW(g_staticList, LVM_SETTEXTCOLOR, 0, (LPARAM)t.editText);
    SendMessageW(g_status, SB_SETBKCOLOR, 0, (LPARAM)(t.dark ? t.paneBack : CLR_DEFAULT));
    updateStatusBar();
    EnumThreadWindows(GetCurrentThreadId(), themeOtherWindow, 0);
    RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

// Settings: the report interval and the Dark Mode switch, from the button and
// the menu alike. The switch acts at once; Cancel restores the scheme.
void openSettings()
{
    SettingsDlgData d;
    d.sec = g_store->settings.intervalSec;
    d.dark = d.darkAtStart = g_store->settings.darkMode;
    d.thAudio = g_store->settings.thAudio;
    d.lrAudio = g_store->settings.lrAudio;
    d.scheme.custom = g_store->settings.customScheme;
    d.scheme.sounds = g_store->settings.sounds;
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_INTERVAL), g_main, intervalDlgProc, (LPARAM)&d) != IDOK) {
        if (theme().dark != d.darkAtStart) applyTheme(d.darkAtStart);
        return;
    }
    if (d.sec > 0 && d.sec != g_store->settings.intervalSec) setIntervalAll(d.sec);
    if (d.dark != g_store->settings.darkMode) {
        g_store->settings.darkMode = d.dark;
        g_store->saveSettings();
        logEvent(d.dark ? "Dark Mode on" : "Dark Mode off");
    }
    if (theme().dark != d.dark) applyTheme(d.dark);
    if (d.thAudio != g_store->settings.thAudio) {
        g_store->settings.thAudio = d.thAudio;
        g_store->saveSettings();
        soundSetMuted(!d.thAudio);
        logEvent(d.thAudio ? "Thunderhead Audio on" : "Thunderhead Audio off");
    }
    if (d.lrAudio != g_store->settings.lrAudio) {
        g_store->settings.lrAudio = d.lrAudio;
        g_store->saveSettings();
        logEvent(d.lrAudio ? "All Lightning Rods Audio on: AUDIO unmute to every online client"
                           : "All Lightning Rods Audio off: AUDIO mute to every online client");
        sendAudioAll(d.lrAudio);
    }
    applySoundScheme(d.scheme);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: layout(); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDM_FILE_EXIT: SendMessageW(g_main, WM_CLOSE, 0, 0); return 0;
        case IDC_CMD_SETTINGS: openSettings(); return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(g_main, L"Thunderhead " TLR_VERSION_WSTRING L"\nBryce Lightning render farm console.\n\n"
                        L"Nodes run Lightning Rod " TLR_VERSION_WSTRING L".", L"About Thunderhead", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDC_CMD_CHECK_ALL: checkAll(true); return 0;
        case IDC_CMD_CHECK_NONE: checkAll(false); return 0;
        case IDC_CMD_INVERT: invertChecked(); return 0;
        case IDC_CMD_SEL_OFFLINE: selectGroup(TintOffline, L"Offline (Red)"); return 0;
        case IDC_CMD_SEL_UNAUTH: selectGroup(TintUnauthenticated, L"Unauthenticated (Orange)"); return 0;
        case IDC_CMD_SEL_OUTDATED: selectGroup(TintOutdated, L"Outdated LR (Yellow)"); return 0;
        case IDC_CMD_SEL_RELAY: selectGroup(TintRelay, L"W7 Relay (Cyan)"); return 0;
        case IDC_CMD_SEL_HOST: selectGroup(TintRenderHost, L"Bryce 3D Net Render Host (Blue)"); return 0;
        case IDC_CMD_SELECT_GROUP: case IDC_CMD_COMMAND_FILE: case IDC_CMD_UTILITIES: case IDC_CMD_CONFIGURE: case IDC_CMD_BRYCE3D:
            // From the menu these are submenus and never arrive; from the pane the parent button opens its pop-up.
            openCommandPopup(id);
            return 0;
        case IDC_CMD_UPDATE: updateChecked(); return 0;
        case IDC_CMD_RENAME: renameChecked(); return 0;
        case IDC_CMD_OPEN_SHARE: openSharedFolders(); return 0;
        case IDC_CMD_COPY: copyToClients(); return 0;
        case IDC_CMD_PRIORITY: changeRenderPriority(); return 0;
        case IDC_CMD_BATCH: runRemoteFile(); return 0;
        case IDC_CMD_WOL: sendWakeOnLan(); return 0;
        case IDC_CMD_PING: pingChecked(); return 0;
        case IDC_CMD_NETPING: networkPing(); return 0;
        case IDC_CMD_VIEW_LOCAL_LOG: viewLocalLog(); return 0;
        case IDC_CMD_VIEW_REMOTE_LOG: viewRemoteLogs(); return 0;
        case IDC_CMD_SET_TIME: setClientTime(); return 0;
        case IDC_CMD_RUN_LIGHTNING: runLightningAsked(); return 0;
        case IDC_CMD_EXIT_LR: exitChecked(); return 0;
        case IDC_CMD_START_TV: startTeamViewer(); return 0;
        case IDC_CMD_MUTE: sendAudio("mute"); return 0;
        case IDC_CMD_UNMUTE: sendAudio("unmute"); return 0;
        case IDC_CMD_SOUND: openPlaySound(); return 0;
        case IDC_CMD_REMOVE_OFFLINE: removeCheckedOffline(); return 0;
        case IDC_CMD_SET_RELAY: setRelayAddress(); return 0;
        case IDC_CMD_TOGGLE_RENDERHOST: toggleRenderHost(); return 0;
        case IDC_CMD_RENDER_JOB: renderJob(); return 0;
        case IDC_CMD_BRYCE_START: bryceOnHosts("start"); return 0;
        case IDC_CMD_BRYCE_STOP: bryceOnHosts("stop"); return 0;
        case IDC_CMD_MESSAGE: displayMessage(); return 0;
        case IDC_CMD_SHUTDOWN_NODE: shutdownChecked(); return 0;
        case IDC_CMD_ABORT_SHUTDOWN: sendPower("abort"); return 0;
        case IDC_FLAG_WOL: case IDC_FLAG_BOOT: case IDC_FLAG_AUTOLOGIN: case IDC_FLAG_AUTOROD: case IDC_FLAG_AUTOLIGHTNING: case IDC_FLAG_RENDERHOST:
            if (HIWORD(wp) == BN_CLICKED) flagClicked(id);
            return 0;
        }
        if (id >= IDC_CMD_FIRST && id <= IDC_CMD_LAST) {
            for (int i = 0; i < kCommandCount; ++i)
                if (kCommands[i].id == id && kCommands[i].verb) { sendCommand(kCommands[i].verb); break; }
            return 0;
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerId) onTimer(); else if (wp == kNudgeTimerId) onNudgeTimer(); else if (wp == kFlashTimerId) onFlashTimer();
        return 0;
    case WM_NET: { NetEvent* e = (NetEvent*)lp; onNet(e); delete e; return 0; }
    case WM_UPDATE_DONE: { UpdateResult* r = (UpdateResult*)lp; onUpdateDone(r); delete r; return 0; }
    case WM_SHARE_CHECKED: { ShareResult* r = (ShareResult*)lp; onShareChecked(r); delete r; return 0; }
    case WM_COPY_DONE: { CopyAfter* r = (CopyAfter*)lp; onCopyDone(r); delete r; return 0; }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->hwndFrom == g_list) return onListNotify(hdr);
        if (hdr->hwndFrom == g_bootAlarm && hdr->code == DTN_DATETIMECHANGE) { bootAlarmChanged((const NMDATETIMECHANGE*)hdr); return 0; }
        if (hdr->code == NM_CUSTOMDRAW && (hdr->hwndFrom == listHeader() || hdr->hwndFrom == headerOf(g_staticList)))
            return onHeaderCustomDraw((NMCUSTOMDRAW*)hdr);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, themeBrush(theme().paneBack));
        return 1;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->hwndItem == g_status) { drawStatusText(di); return TRUE; }
        break;
    }
    case WM_CLOSE: {
        // The Shutdown sound plays to its end (5 s at most) before the window goes.
        std::string err;
        if (g_store->settings.thAudio && !soundPlayAndWait(soundPathForEvent(SoundThClose), 5000, err))
            logEvent(format("sound %s not played: %s", kSoundEvents[SoundThClose].key, err.c_str()));
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kTimerId);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HFONT uiFont()
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return CreateFontIndirectW(&ncm.lfMessageFont);
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// The in-place Group/Comment editor is a child of the list, so the list gets
// its WM_CTLCOLOREDIT; in Dark Mode it answers with the scheme's colors.
LRESULT CALLBACK listColorSubclass(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_CTLCOLOREDIT) {
        if (HBRUSH b = themeCtlColor(msg, (HDC)wp, (HWND)lp)) return (LRESULT)b;
    }
    if (msg == WM_NOTIFY) {
        // A column width or order change: the list repaints its rows, but not
        // the blank stretch below them, where the rules of the old column
        // edges would otherwise stay beside the new ones.
        const NMHDR* n = (const NMHDR*)lp;
        if (n->hwndFrom == headerOf(w) &&
            (n->code == HDN_ITEMCHANGEDW || n->code == HDN_ITEMCHANGEDA || n->code == HDN_ENDTRACKW || n->code == HDN_ENDTRACKA ||
             n->code == HDN_ENDDRAG || n->code == HDN_DIVIDERDBLCLICKW || n->code == HDN_DIVIDERDBLCLICKA)) {
            LRESULT r = DefSubclassProc(w, msg, wp, lp);
            RECT rc;
            if (blankBelowRows(rc)) InvalidateRect(w, &rc, TRUE);
            return r;
        }
    }
    if (msg == WM_ERASEBKGND) {
        // The list erased the invalid area to its background color and painted
        // the row shade over it a moment later, which showed as a white flash in
        // every cell rewritten by a text update (Last report, once a second).
        // Erasing each visible row in the shade the custom draw will paint it
        // in, and only the rest in the background color, leaves nothing to flash.
        HDC dc = (HDC)wp;
        RECT client;
        GetClientRect(w, &client);
        FillRect(dc, &client, themeBrush(theme().listBack));
        int count = (int)SendMessageW(w, LVM_GETITEMCOUNT, 0, 0);
        int top = (int)SendMessageW(w, LVM_GETTOPINDEX, 0, 0);
        int perPage = (int)SendMessageW(w, LVM_GETCOUNTPERPAGE, 0, 0) + 1;
        for (int row = top; row < count && row < top + perPage; ++row) {
            RECT rc;
            rc.left = LVIR_BOUNDS;
            if (!SendMessageW(w, LVM_GETITEMRECT, (WPARAM)row, (LPARAM)&rc)) continue;
            rc.left = client.left;
            rc.right = client.right;
            fillRect(dc, rc, rowShade(nodeAt(row), row, NULL));
        }
        return 1;
    }
    return DefSubclassProc(w, msg, wp, lp);
}

HWND createPane(HINSTANCE inst, int bgColor)
{
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"ThunderheadPane", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                             0, 0, 10, 10, g_main, NULL, inst, NULL);
    SetClassLongPtrW(h, GCLP_HBRBACKGROUND, (LONG_PTR)GetSysColorBrush(bgColor));
    return h;
}

HWND createStatic(HWND parent, int id, const wchar_t* text)
{
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

HWND createCheck(HWND parent, int id, const wchar_t* text)
{
    HWND h = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 10, 10,
                             parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

HWND createKeyValueList(HWND parent, int id)
{
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER | LVS_SINGLESEL,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(h, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (LPWSTR)L"Item";
    col.cx = 112;
    col.iSubItem = 0;
    SendMessageW(h, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Value";
    col.cx = 300;
    col.iSubItem = 1;
    SendMessageW(h, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    return h;
}

void createChildren(HINSTANCE inst)
{
    WNDCLASSW pc;
    memset(&pc, 0, sizeof(pc));
    pc.lpfnWndProc = paneProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursor(NULL, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    pc.lpszClassName = L"ThunderheadPane";
    RegisterClassW(&pc);

    g_inventoryPane = createPane(inst, COLOR_WINDOW);
    g_staticPane = createPane(inst, COLOR_BTNFACE);
    g_commandPane = createPane(inst, COLOR_BTNFACE);

    // Client Inventory
    g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                             0, 0, 10, 10, g_inventoryPane, NULL, inst, NULL);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                 LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP | LVS_EX_CHECKBOXES);   // shading and rules come from onListCustomDraw
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SetWindowSubclass(g_list, listColorSubclass, 1, 0);
    for (int i = 0; i < ColCount; ++i) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT | LVCF_SUBITEM;
        col.pszText = (LPWSTR)kColumns[i].title;
        col.cx = kColumns[i].width;
        col.fmt = kColumns[i].align;
        col.iSubItem = i;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }

    // static pane
    g_staticTitle = createStatic(g_staticPane, IDC_STATIC_TITLE, L"");
    g_flagHeader = createStatic(g_staticPane, IDC_FLAG_HEADER, L"Info Flags (does not change any setting or function)");
    g_flags[0] = createCheck(g_staticPane, IDC_FLAG_WOL, L"Wake On LAN");
    g_flags[1] = createCheck(g_staticPane, IDC_FLAG_BOOT, L"Boot on Alarm");
    g_bootAlarm = CreateWindowExW(0, DATETIMEPICK_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_TIMEFORMAT | DTS_UPDOWN,
                                 0, 0, 10, 10, g_staticPane, (HMENU)(INT_PTR)IDC_BOOT_ALARM, inst, NULL);
    SendMessageW(g_bootAlarm, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_bootAlarm, DTM_SETFORMATW, 0, (LPARAM)L"hh:mm tt");
    g_flags[2] = createCheck(g_staticPane, IDC_FLAG_AUTOLOGIN, L"Auto Login");
    g_flags[3] = createCheck(g_staticPane, IDC_FLAG_AUTOROD, L"Auto start Lightning Rod");
    g_flags[4] = createCheck(g_staticPane, IDC_FLAG_AUTOLIGHTNING, L"Auto start Lightning");
    g_flags[5] = createCheck(g_staticPane, IDC_FLAG_RENDERHOST, L"Bryce 3D Network Render Host (B3NDHR)");
    g_staticList = createKeyValueList(g_staticPane, IDC_STATIC_LIST);
    ShowWindow(g_staticPane, SW_HIDE);

    // command pane: the top-level commands; nested ones appear in their parent's pop-up
    for (int i = 0; i < kCommandCount; ++i) {
        if (kCommands[i].parent) continue;
        HWND b = CreateWindowExW(0, L"BUTTON", kCommands[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 0, 0, 10, 10, g_commandPane, (HMENU)(INT_PTR)kCommands[i].id, inst, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        EnableWindow(b, kCommands[i].live);
    }

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show)
{
    g_inst = inst;
    g_dir = exeDirectory();
    InitializeCriticalSection(&g_relayLock);
    srand((unsigned)GetTickCount());
    g_events.open(g_dir + L"\\logs", L"thunderhead");
    CreateDirectoryW((g_dir + L"\\logs\\nodes").c_str(), NULL);
    logEvent("Thunderhead " TLR_VERSION_STRING " starting");
    // farm.key: created once, beside Thunderhead.exe; written to every client's
    // shared folder as that is found readable (see docs/PROTOCOL.md, Authentication).
    g_farmKey = readKeyFile(g_dir + L"\\" + kFarmKeyFile);
    if (g_farmKey.empty()) {
        std::string err;
        g_farmKey = randomHex(kFarmKeyBytes);
        if (g_farmKey.empty() || !writeKeyFile(g_dir + L"\\" + kFarmKeyFile, g_farmKey, err)) {
            logEvent("farm.key could not be created: " + err + "; OFFERs go out unsigned and no agent that holds a key will connect");
            g_farmKey.clear();
        } else {
            logEvent("farm.key created beside Thunderhead.exe; agents that hold no key receive it through their shared folders");
        }
    } else {
        logEvent("farm.key loaded");
    }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_DATE_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   // file dialogs
    if (!netInit()) { MessageBoxW(NULL, L"Winsock failed to start.", L"Thunderhead", MB_OK | MB_ICONERROR); return 1; }

    NodeStore store(g_dir);
    store.load();
    g_store = &store;
    Server server;
    g_server = &server;
    g_font = uiFont();

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = L"ThunderheadMain";
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MENU);
    RegisterClassW(&wc);

    g_main = CreateWindowExW(0, L"ThunderheadMain", L"Thunderhead " TLR_VERSION_WSTRING,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 830, NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    createChildren(inst);
    layout();

    std::vector<NodeRecord*>& nodes = store.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) refreshRow(nodes[i]);

    g_pushVersion = fileVersion(pushFilePath());
    if (!server.start(g_main, WM_NET, (unsigned short)store.settings.tcpPort, hostName(), g_farmKey)) {
        MessageBoxW(g_main, L"The listening ports could not be opened. Another Thunderhead may be running; see the log.",
                    L"Thunderhead", MB_OK | MB_ICONERROR);
    }
    SetTimer(g_main, kTimerId, 1000, NULL);
    applyTheme(store.settings.darkMode);   // also the first updateStatusBar()
    soundSetMuted(!store.settings.thAudio);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    playEventSound(SoundThOpen);

    // Hot keys while the main window has the focus: Ctrl+A checks every client,
    // Ctrl+D checks none. They arrive as WM_COMMAND with the button's id. The
    // in-place Group/Comment editor keeps its own Ctrl+A (select all text).
    ACCEL accel[] = {
        { FCONTROL | FVIRTKEY, 'A', IDC_CMD_CHECK_ALL },
        { FCONTROL | FVIRTKEY, 'D', IDC_CMD_CHECK_NONE },
    };
    HACCEL hotKeys = CreateAcceleratorTableW(accel, 2);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        HWND top = m.hwnd ? GetAncestor(m.hwnd, GA_ROOT) : NULL;   // a notice window takes Enter, Escape and Tab as a dialog does
        if (isNoticeWindow(top) && IsDialogMessageW(top, &m)) continue;
        if (!g_edit && hotKeys && TranslateAcceleratorW(g_main, hotKeys, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (hotKeys) DestroyAcceleratorTable(hotKeys);

    server.stop();
    logEvent("Thunderhead stopped");
    for (std::map<std::string, Logger*>::iterator it = g_nodeLogs.begin(); it != g_nodeLogs.end(); ++it) delete it->second;
    g_nodeLogs.clear();
    soundShutdown();
    netShutdown();
    DeleteObject(g_font);
    return 0;
}
