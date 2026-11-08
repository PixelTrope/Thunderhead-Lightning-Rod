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
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <winnetwk.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <objbase.h>
#include <map>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace tlr;

namespace {

const UINT WM_NET = WM_APP + 1;
const UINT WM_UPDATE_DONE = WM_APP + 2;   // lParam: UpdateResult*
const UINT_PTR kTimerId = 1;

HINSTANCE g_inst = NULL;
HWND g_main = NULL, g_status = NULL;
HWND g_inventoryPane = NULL, g_staticPane = NULL, g_commandPane = NULL;
HWND g_list = NULL;                       // Client Inventory
HWND g_staticTitle = NULL, g_staticList = NULL, g_flags[5] = { NULL, NULL, NULL, NULL, NULL };
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

struct CommandDef { int id; const wchar_t* label; const char* verb; bool live; };

const CommandDef kCommands[] = {
    { IDC_CMD_START71,       L"Start Lightning 7.1", "START",   true  },
    { IDC_CMD_STOP,          L"Stop Lightning",      "STOP",    true  },
    { IDC_CMD_PING,          L"Lightning Rod Ping",  NULL,      true  },
    { IDC_CMD_NETPING,       L"Network Ping",        NULL,      true  },
    { IDC_CMD_RESTART_LR,    L"Restart LR",          "RESTART", true  },
    { IDC_CMD_EXIT_LR,       L"Exit LR",             "EXIT",    true  },
    { IDC_CMD_CHECK_ALL,     L"Check All (Ctrl-A)",  NULL,      true  },
    { IDC_CMD_CHECK_NONE,    L"Check None (Ctrl-D)", NULL,      true  },
    { IDC_CMD_CHECK_OUTDATED, L"Check outdated",     NULL,      true  },
    { IDC_CMD_UPDATE,        L"Update checked",      NULL,      true  },
    { IDC_CMD_RENAME,        L"Rename node",         NULL,      true  },
    { IDC_CMD_SHUTDOWN_NODE, L"Shutdown Node",       NULL,      true  },
    { IDC_CMD_ABORT_SHUTDOWN, L"Abort Shutdown",     NULL,      true  },
    { IDC_CMD_OPEN_SHARE,    L"Open Client's Shared Folder", NULL, true },
    { IDC_CMD_COPY,          L"Copy files or folder to Client", NULL, true },
    { IDC_CMD_PRIORITY,      L"Change Render Priority", NULL, true },
    { IDC_CMD_BATCH,         L"Run Remote Batch File", NULL, true },
    { IDC_CMD_WOL,           L"Send Wake On LAN Magic Packet", NULL, true },
    { IDC_CMD_VIEW_LOCAL_LOG, L"View Local Log",     NULL,      true  },
    { IDC_CMD_VIEW_REMOTE_LOG, L"View Remote Log(s)", NULL,     true  },
    { IDC_CMD_SET_TIME,      L"Set Client Date and Time", NULL, true  },
    { IDC_CMD_MUTE,          L"Mute Checked Clients", NULL,     true  },
    { IDC_CMD_UNMUTE,        L"Unmute Checked Clients", NULL,   true  },
    { IDC_CMD_REMOVE_OFFLINE, L"Remove Checked Offline Clients", NULL, true },
    { IDC_CMD_START61,       L"Start Lightning 6.1", NULL,      false },
    { IDC_CMD_START63,       L"Start Lightning 6.3", NULL,      false },
    { IDC_CMD_START_TV,      L"Start TeamViewer",    NULL,      false },
    { IDC_CMD_MESSAGE,       L"Display message",     NULL,      false },
    { IDC_CMD_SOUND,         L"Play sound",          NULL,      false },
};
const int kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);

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
    case ColStatus:       return n.updating ? L"Updating" : (n.online ? L"Online" : L"Offline");
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
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
}

// ---------------------------------------------------------------- static pane

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

    // Every field there is: the stored record, the connection, the HELLO, the
    // latest report (kept for an offline node), then any field of HELLO or
    // REPORT that has no row of its own, as a newer agent may send one.
    std::vector<std::pair<std::wstring, std::wstring> > rows;
    auto add = [&rows](const wchar_t* item, const std::wstring& value) { rows.push_back(std::make_pair(std::wstring(item), value)); };
    add(L"Name", widen(n->name));
    if (!h.get("name").empty() && h.get("name") != n->name) add(L"Reported name", widen(h.get("name")));
    add(L"MAC", widen(n->mac));
    add(L"IP", widen(nodeIp(*n)));
    add(L"Status", columnText(*n, ColStatus));
    add(L"Connected for", n->online ? durationText((long long)((tickMs() - n->onlineSinceMs) / 1000)) : std::wstring());
    add(L"Last report", columnText(*n, ColLastReport));
    add(L"Group", widen(n->group));
    add(L"Comment", widen(n->comment));
    add(L"Auto Shutdown", n->autoShutdown ? L"Yes" : L"No");
    add(L"Agent", widen(h.get("agent")));
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
                                              "chipset", "gpu", "vram_mb", "ip", "mac", "name" };
    static const char* const knownReport[] = { "cpu_pct", "cpu_temp_c", "disk_free_mb", "lightning", "lightning_cpu_pct",
                                               "mem_free_mb", "rendering", "uptime_s", "clock", "utc_ms" };
    for (std::map<std::string, std::string>::const_iterator it = h.fields.begin(); it != h.fields.end(); ++it) {
        bool known = false;
        for (size_t k = 0; k < sizeof(knownHello) / sizeof(knownHello[0]); ++k) if (it->first == knownHello[k]) known = true;
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
    default: return;
    }
    g_store->save();
    logEvent(format("%s %s=%s", n->name.c_str(), name, boolText(on)));
}

// The Auto Shutdown cell of a row was clicked: flip the flag, save it and repaint the row.
void toggleAutoShutdown(int row)
{
    NodeRecord* n = nodeAt(row);
    if (!n) return;
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

void checkOutdated()
{
    std::wstring file = pushFilePath();
    std::string version = fileVersion(file);
    if (version.empty()) {
        MessageBoxW(g_main, (L"No agent file with a version resource at\n" + file +
                    L"\n\nPut the LightningRod.exe to push beside Thunderhead.exe, or set agent_file in settings.txt.").c_str(),
                    L"Thunderhead", MB_OK | MB_ICONWARNING);
        return;
    }
    int outdated = 0, online = 0;
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    for (int row = 0; row < count; ++row) {
        NodeRecord* n = nodeAt(row);
        bool old = n && n->online && !n->updating && compareVersions(n->hello.get("agent"), version) < 0;
        if (n && n->online) ++online;
        if (old) ++outdated;
        setChecked(row, old);
    }
    updateStatusBar(widen(format("|  %d of %d online clients are older than %s", outdated, online, version.c_str())).c_str());
}

struct UpdateJob { std::string name; std::string ip; std::wstring source; std::wstring share; std::wstring dir; };
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
    std::vector<std::wstring> dirs;
    dirs.push_back(L"\\\\" + widen(job->ip) + L"\\" + job->share + L"\\");
    if (nameResolvesTo(job->name, job->ip)) dirs.push_back(L"\\\\" + widen(job->name) + L"\\" + job->share + L"\\");
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
    job->source = pushFilePath();
    job->share = widen(g_store->settings.shareName);
    job->dir = n->updateDir;
    HANDLE t = CreateThread(NULL, 0, fn, job, 0, NULL);
    if (t) { CloseHandle(t); return; }
    delete job;
    n->updating = false;
    refreshRow(n);
    logEvent(n->name + " update: could not start the " + what + " thread");
}

// A share that refused the credentials Windows offered gets the credential
// prompt, owned by the main window, and the staging is tried once more with
// what was typed. Refusals that arrive while a prompt is up wait their turn.
std::vector<std::pair<NodeRecord*, std::wstring> > g_credQueue;
bool g_credPrompting = false;

void runCredQueue()
{
    if (g_credPrompting) return;
    g_credPrompting = true;
    while (!g_credQueue.empty()) {
        NodeRecord* n = g_credQueue.front().first;
        std::wstring dir = g_credQueue.front().second;
        g_credQueue.erase(g_credQueue.begin());
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
        g_credQueue.push_back(std::make_pair(n, r->credDir));
        runCredQueue();
        return;
    }
    if (r->phase == 1 && r->ok && n->online) {
        n->updateDir = r->dir;
        n->updateStaged = true;
        Message m("UPDATE");
        if (g_server->send(n->conn, m)) {
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
    std::vector<NodeRecord*> targets = checkedNodes();
    int sent = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online || n->updating) continue;
        ++sent;
        n->updating = true;
        n->updateStaged = false;
        n->updateDir.clear();
        n->updatePrompted = false;
        n->updateStartMs = tickMs();
        logEvent(format("%s update: agent %s, staging %s from %s", n->name.c_str(), n->hello.get("agent").c_str(), version.c_str(), narrow(file).c_str()));
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
    int sent = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        if (g_server->send(n->conn, m)) {
            ++sent;
            logEvent(format("%s -> %s", m.oneLine().c_str(), n->name.c_str()));
            logNode(*n, std::string("-> ") + m.oneLine());
        }
    }
    updateStatusBar(widen(format("|  %s sent to %d of %d checked", m.verb.c_str(), sent, (int)targets.size())).c_str());
}

void sendCommand(const char* verb)
{
    sendCommandMessage(Message(verb));
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
        if (!g_server->send(n->conn, m)) {
            ++g_pingNoReply;
            logEvent("PING could not be sent to " + n->name);
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
// that of its connection, else the one its last HELLO carried, else its name
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
    int online = 0, sent = 0, old = 0;
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
        if (g_server->send(n->conn, m)) {
            ++sent;
            logEvent(format("%s -> %s", label.c_str(), n->name.c_str()));
            logNode(*n, "-> " + label);
        }
    }
    if (online == 0) { updateStatusBar(L"|  no online clients checked"); return; }
    std::string status = format("|  %s sent to %d of %d checked", label.c_str(), sent, (int)targets.size());
    if (old) status += format(", %d skipped: agent older than %s", old, minAgent);
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

// Asks for a priority class, confirms the two that carry a cost, and sends
// PRIORITY to every checked client, which applies it to its Lightning.exe.
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
    if (warning) {
        TASKDIALOG_BUTTON buttons[2] = { { 300, L"Yes, Proceed" }, { 301, L"No, Cancel" } };
        TASKDIALOGCONFIG cc;
        memset(&cc, 0, sizeof(cc));
        cc.cbSize = sizeof(cc);
        cc.hwndParent = g_main;
        cc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
        cc.pszWindowTitle = L"Change Render Priority";
        cc.pszMainIcon = TD_WARNING_ICON;
        cc.pszMainInstruction = L"Confirm the priority change";
        cc.pszContent = warning;
        cc.pButtons = buttons;
        cc.cButtons = 2;
        cc.nDefaultButton = 301;
        int answer = 0;
        if (FAILED(TaskDialogIndirect(&cc, &answer, NULL, NULL)) || answer != 300) return;
    }
    Message m("PRIORITY");
    m.set("level", std::string(level));
    sendCommandMessage(m);
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
            continue;
        }
        ++done;
        logEvent(format("WOL magic packet for %s (%s) sent to %s, UDP port 9", n->name.c_str(), n->mac.c_str(), where.c_str()));
        logNode(*n, "-> WOL magic packet to " + where);
    }
    closesocket(s);
    updateStatusBar(widen(format("|  WOL magic packet sent for %d of %d checked", done, (int)targets.size())).c_str());
}

// ---------------------------------------------------------------- remote batch file

// First agent version that knows BATCH.
const char* const kBatchAgent = "0.2.7";
// The text travels as two hexadecimal digits per byte inside one message, which
// may be 65536 bytes long.
const size_t kMaxBatchBytes = 30000;
const wchar_t* const kBatchTitle = L"Run Remote Batch File";

std::wstring g_batchText;       // text of the dialog, kept while Thunderhead runs
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
    const COMDLG_FILTERSPEC types[2] = { { L"Batch files (*.bat;*.cmd)", L"*.bat;*.cmd" }, { L"All files (*.*)", L"*.*" } };
    dlg->SetFileTypes(2, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"Select the batch file to load");
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
    std::wstring content = widen(format("No error checking and no check for harmful code is done on this batch file. It is sent to "
                                        "%d checked online client(s) and run there exactly as written, with the rights Lightning Rod "
                                        "has on each client.\n\n", g_batchOnline)) +
                           L"Are you ABSOLUTELY sure you want this batch file to run on the remote client(s) ?";
    TASKDIALOG_BUTTON buttons[2] = { { 300, L"Yes, Execute" }, { 301, L"No, Cancel" } };
    TASKDIALOGCONFIG cc;
    memset(&cc, 0, sizeof(cc));
    cc.cbSize = sizeof(cc);
    cc.hwndParent = owner;
    cc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    cc.pszWindowTitle = L"Execute Remote Batch File";
    cc.pszMainIcon = TD_WARNING_ICON;
    cc.pszMainInstruction = L"This batch file is not checked in any way";
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
    g_batchMaximized = IsDlgButtonChecked(dlg, IDC_BATCH_MAXIMIZED) == BST_CHECKED;
    g_batchFront = IsDlgButtonChecked(dlg, IDC_BATCH_FRONT) == BST_CHECKED;
}

INT_PTR CALLBACK batchDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM)
{
    HWND edit = GetDlgItem(dlg, IDC_BATCH_TEXT);
    switch (msg) {
    case WM_INITDIALOG: {
        SetDlgItemTextW(dlg, IDC_BATCH_INFO, widen(format("Text of the batch file to run on %d checked online client(s). Type it, "
                                                           "paste it, or load a file with Browse for file.", g_batchOnline)).c_str());
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
        CheckDlgButton(dlg, IDC_BATCH_MAXIMIZED, g_batchMaximized ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_BATCH_FRONT, g_batchFront ? BST_CHECKED : BST_UNCHECKED);
        SetFocus(edit);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BATCH_BROWSE: {
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
            if (trim(g_batchText).empty()) {
                MessageBoxW(dlg, L"The batch file text is empty.", kBatchTitle, MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            size_t bytes = narrow(g_batchText).size();
            if (bytes > kMaxBatchBytes) {
                MessageBoxW(dlg, widen(format("The text is %u bytes; a remote batch file may have %u at most.",
                                              (unsigned)bytes, (unsigned)kMaxBatchBytes)).c_str(), kBatchTitle, MB_OK | MB_ICONWARNING);
                return TRUE;
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

// Run Remote Batch File: asks for the text, confirms, and sends it to every
// checked online client, whose agent writes it to a file and runs it.
void runRemoteBatch()
{
    std::vector<NodeRecord*> targets = checkedNodes();
    g_batchOnline = 0;
    for (size_t i = 0; i < targets.size(); ++i) if (targets[i]->online) ++g_batchOnline;
    if (g_batchOnline == 0) {
        MessageBoxW(g_main, L"Check the online clients to run the batch file on first.", kBatchTitle, MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BATCH), g_main, batchDlgProc, 0) != IDOK) return;
    std::string utf8 = narrow(g_batchText);
    Message m("BATCH");
    m.set("text", hexEncode(utf8));
    m.set("maximized", std::string(boolText(g_batchMaximized)));
    m.set("front", std::string(boolText(g_batchFront)));
    sendVersioned(m, format("BATCH (%u bytes of text%s%s)", (unsigned)utf8.size(), g_batchMaximized ? ", maximized" : "",
                            g_batchFront ? ", to the front" : ""), kBatchAgent);
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
        startLogLoad(hwnd, v);
        return 0;
    }
    if (!v) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
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
        LogSource src;
        if (!n->ip.empty()) src.dirs.push_back(L"\\\\" + widen(n->ip) + L"\\" + widen(g_store->settings.shareName) + L"\\");
        src.name = n->name;
        src.ip = n->ip;
        src.share = widen(g_store->settings.shareName);
        src.pattern = L"LightningRod-*.log";
        std::string label = n->name.empty() ? n->mac : n->name;
        openLogView(widen(label) + L" log", src);
        ++opened;
        logEvent(format("View Remote Log: %s, \\\\%s\\%s\\LightningRod-*.log%s", label.c_str(),
                        n->ip.empty() ? n->name.c_str() : n->ip.c_str(), g_store->settings.shareName.c_str(),
                        n->online ? "" : " (offline, last known address)"));
    }
    updateStatusBar(widen(format("|  %d log window(s) opened", opened)).c_str());
}

// ---------------------------------------------------------------- rename

struct RenameRequest { std::wstring info; std::wstring name; bool reboot; };

INT_PTR CALLBACK renameDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
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
        paths->push_back("\\\\" + n->name + "\\" + g_store->settings.shareName + "\\");
    }
    if (paths->empty()) { delete paths; return; }
    logEvent(format("opening the shared folder of %u client(s)", (unsigned)paths->size()));
    HANDLE t = CreateThread(NULL, 0, openSharedFoldersThread, paths, 0, NULL);
    if (t) CloseHandle(t);
    else { logEvent("Open shared folder: could not start a thread"); delete paths; }
}

// ---------------------------------------------------------------- copy to clients

struct CopyJob {
    std::vector<std::wstring> sources;                        // files, or one folder
    std::vector<std::pair<std::string, std::wstring> > targets; // client name, \\NAME\<share>
};

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
        const std::string& name = job->targets[i].first;
        std::wstring to = job->targets[i].second;
        if (to.empty() || to[to.size() - 1] != L'\\') to += L'\\';
        if (cp.wnd) {
            SetWindowTextW(cp.client, widen(format("Client %s (%u of %u): %s", name.c_str(), (unsigned)i + 1, (unsigned)job->targets.size(), narrow(to).c_str())).c_str());
            SetWindowTextW(cp.file, L"Connecting...");
            copyProgressPump(cp);
        }
        // The share must answer before anything is copied; a refusal gets the credential prompt.
        if (GetFileAttributesW(to.c_str()) == INVALID_FILE_ATTRIBUTES) {
            DWORD e = GetLastError();
            if (e == ERROR_ACCESS_DENIED || e == ERROR_LOGON_FAILURE || e == ERROR_INVALID_PASSWORD || e == ERROR_SESSION_CREDENTIAL_CONFLICT)
                e = promptShareCredentials(cp.wnd ? cp.wnd : g_main, to) == NO_ERROR && GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES ? NO_ERROR : GetLastError();
            if (e != NO_ERROR) {
                lastError = format("%s is not reachable (%s)", narrow(to).c_str(), lastErrorText((int)e).c_str());
                logEvent(format("copy to %s failed: %s", name.c_str(), lastError.c_str()));
                ++clientsFailed;
                cp.done += perClient;
                copyProgressShowBytes(cp, cp.done);
                continue;
            }
        }
        std::string err;
        unsigned files = 0;
        ULONGLONG before = cp.done;
        bool ok = true;
        for (size_t k = 0; k < job->sources.size() && ok; ++k) ok = copyTree(job->sources[k], to, cp, err, files);
        filesAll += files;
        bytesAll += cp.done - before;
        if (cp.cancelled) logEvent(format("copy to %s cancelled after %u file(s), %s", name.c_str(), files, byteText(cp.done - before).c_str()));
        else if (ok) { logEvent(format("copied %u file(s), %s, to %s", files, byteText(cp.done - before).c_str(), name.c_str())); ++clientsDone; }
        else { lastError = err; logEvent(format("copy to %s failed after %u file(s): %s", name.c_str(), files, err.c_str())); ++clientsFailed; }
        if (!ok && !cp.cancelled) { cp.done = before + perClient; copyProgressShowBytes(cp, cp.done); }
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
    TASKDIALOG_BUTTON buttons[2] = { { 100, L"Files..." }, { 101, L"A folder..." } };
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
    tc.cButtons = 2;
    tc.nDefaultButton = 100;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&tc, &pressed, NULL, NULL)) || (pressed != 100 && pressed != 101)) return;
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
        job->targets.push_back(std::make_pair(n->name, L"\\\\" + widen(n->name) + L"\\" + widen(g_store->settings.shareName)));
    }
    if (job->targets.empty()) { delete job; return; }
    logEvent(format("copying %u item(s) from %s to %u client(s)", (unsigned)sources.size(),
                    narrow(sources[0]).c_str(), (unsigned)job->targets.size()));
    HANDLE t = CreateThread(NULL, 0, copyToClientsThread, job, 0, NULL);
    if (t) CloseHandle(t);
    else { logEvent("copy to clients: could not start a thread"); delete job; }
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
        if (g_server->send(n->conn, m)) {
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

void setOffline(NodeRecord* n, const std::string& reason)
{
    if (!n->online) return;
    n->online = false;
    n->pingPending = false;
    if (n->conn >= 0) g_server->close(n->conn);
    n->conn = -1;
    logEvent(format("%s offline: %s", n->name.c_str(), reason.c_str()));
    logNode(*n, "offline: " + reason);
    refreshRow(n);
    if (g_staticNode == n) showStaticPane(n);
}

void setIntervalAll(int sec)
{
    g_store->settings.intervalSec = sec;
    g_store->saveSettings();
    g_server->setInterval(sec);
    Message m("SET");
    m.set("interval", (long long)sec);
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i]->online) continue;
        if (g_server->send(nodes[i]->conn, m)) logNode(*nodes[i], "-> " + m.oneLine());
    }
    logEvent(format("report interval set to %d s", sec));
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

// ---------------------------------------------------------------- dialogs

INT_PTR CALLBACK intervalDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND combo = GetDlgItem(dlg, IDC_INTERVAL_COMBO);
        int current = (int)lp, selected = 0;
        for (int sec = 5, i = 0; sec <= 60; sec += 5, ++i) {
            wchar_t text[32];
            _snwprintf(text, 31, L"%d seconds", sec);
            text[31] = L'\0';
            SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
            SendMessageW(combo, CB_SETITEMDATA, (WPARAM)i, (LPARAM)sec);
            if (sec == current) selected = i;
        }
        SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            HWND combo = GetDlgItem(dlg, IDC_INTERVAL_COMBO);
            int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
            int sec = i < 0 ? 0 : (int)SendMessageW(combo, CB_GETITEMDATA, (WPARAM)i, 0);
            EndDialog(dlg, sec > 0 ? sec : 0);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- network events

void onNet(NetEvent* e)
{
    unsigned long long now = tickMs();
    switch (e->type) {
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
        if (n->online && n->conn != e->conn) g_server->close(n->conn);   // replaced by a new session
        n->online = true;
        n->conn = e->conn;
        n->ip = e->ip;
        n->hello = e->msg;
        n->report = Message();
        n->lastReportMs = now;
        n->onlineSinceMs = now;
        if (n->updating) {
            n->updating = false;
            logEvent(format("%s update finished: agent %s is back", n->name.c_str(), e->msg.get("agent").c_str()));
        }
        logEvent(format("%s online from %s, agent %s%s", n->name.c_str(), n->ip.c_str(),
                        e->msg.get("agent").c_str(), isNew ? " (new client)" : ""));
        logNode(*n, "<- " + e->msg.oneLine());
        logNode(*n, format("-> SET interval=%d", g_store->settings.intervalSec));
        refreshRow(n);
        if (g_staticNode == n) showStaticPane(n);
        break;
    }
    case NetEvent::Report: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        // A REPORT carries only the values that changed since the previous one.
        n->report.verb = e->msg.verb;
        for (std::map<std::string, std::string>::const_iterator it = e->msg.fields.begin(); it != e->msg.fields.end(); ++it)
            n->report.fields[it->first] = it->second;
        n->lastReportMs = now;
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
        if (cmd == "BATCH") {
            // Two acknowledgements per batch file: stage=started, then stage=finished.
            std::string file = e->msg.get("file");
            if (e->msg.get("stage") == "finished")
                text = format("BATCH %s finished on %s with exit code %s after %s s", file.c_str(),
                              n->name.c_str(), e->msg.get("exit").c_str(), e->msg.get("seconds").c_str());
            else if (ok) text = format("BATCH started on %s as batch\\%s", n->name.c_str(), file.c_str());
            else text = format("BATCH failed on %s: %s", n->name.c_str(), e->msg.get("error").c_str());
        } else if (cmd == "TIME") {
            long long delta = atoll(e->msg.get("delta_s").c_str());
            std::string was = format("%lld s %s", delta < 0 ? -delta : delta, delta < 0 ? "behind" : "ahead");
            if (!ok) text = format("TIME: %s clock %s, not set: %s", n->name.c_str(), was.c_str(), e->msg.get("error").c_str());
            else if (parseBool(e->msg.get("set")))
                text = format("TIME: %s clock was %s (its date %s), set to the host's time", n->name.c_str(), was.c_str(),
                              e->msg.get("date").c_str());
            else text = format("TIME: %s clock %s, left alone", n->name.c_str(), was.c_str());
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
                    if (g_server->send(n->conn, reboot)) {
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
                startUpdateThread(n, updateFinishThread, "finish");
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
        if (!n->online) continue;
        if (n->pingPending && now - n->pingSentMs > kPingWaitMs) {
            ++g_pingNoReply;
            setOffline(n, format("no report within %d s of PING", (int)(kPingWaitMs / 1000)));
            pingStatus();
            continue;
        }
        if (now - n->lastReportMs > limit) {
            setOffline(n, format("no report for %llu s", (now - n->lastReportMs) / 1000));
            continue;
        }
        refreshRow(n, ColLastReport);
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
    int flagWidth = (rc.right - 2 * margin) / 3;
    if (flagWidth < 120) flagWidth = 120;
    int x = margin;
    for (int i = 0; i < 5; ++i) {
        if (x + flagWidth > rc.right - margin && x > margin) { x = margin; y += line + 2; }
        MoveWindow(g_flags[i], x, y, flagWidth, line, TRUE);
        x += flagWidth;
    }
    y += line + 6;
    MoveWindow(g_staticList, margin, y, rc.right - 2 * margin, rc.bottom - y - margin, TRUE);
    int valueWidth = rc.right - 2 * margin - 112 - GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (valueWidth < 120) valueWidth = 120;
    SendMessageW(g_staticList, LVM_SETCOLUMNWIDTH, 1, (LPARAM)valueWidth);
}

// Command buttons: as many 200 px columns as the pane's width holds (200 px
// fits the longest label), widened to share the width, in as many rows as
// the count needs. The pane's height is set from commandPaneHeight() so that
// every row is visible; the details pane above it takes what is left.
const int kCmdMargin = 8, kCmdGap = 6, kCmdMinWidth = 200, kCmdHeight = 30;

int commandColumns(int width)
{
    int perRow = (width - 2 * kCmdMargin + kCmdGap) / (kCmdMinWidth + kCmdGap);
    return perRow < 1 ? 1 : perRow;
}

int commandPaneHeight(int width)
{
    int rows = (kCommandCount + commandColumns(width) - 1) / commandColumns(width);
    return 2 * kCmdMargin + rows * (kCmdHeight + kCmdGap) - kCmdGap;
}

void layoutCommandPane()
{
    RECT rc;
    GetClientRect(g_commandPane, &rc);
    int perRow = commandColumns(rc.right);
    int bw = (rc.right - 2 * kCmdMargin - (perRow - 1) * kCmdGap) / perRow;
    if (bw < kCmdMinWidth) bw = kCmdMinWidth;
    for (int i = 0; i < kCommandCount; ++i) {
        int x = kCmdMargin + (i % perRow) * (bw + kCmdGap);
        int y = kCmdMargin + (i / perRow) * (kCmdHeight + kCmdGap);
        MoveWindow(GetDlgItem(g_commandPane, kCommands[i].id), x, y, bw, kCmdHeight, TRUE);
    }
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
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
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

LRESULT onListCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:     return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT: {
        const NodeRecord* n = (const NodeRecord*)cd->nmcd.lItemlParam;
        if (n) drawAutoShutdownCell(cd->nmcd.hdc, (int)cd->nmcd.dwItemSpec, n->autoShutdown);
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

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: layout(); return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case IDM_FILE_EXIT: DestroyWindow(g_main); return 0;
        case IDM_NODE_REMOVE: removeCheckedOffline(); return 0;
        case IDM_SETTINGS_INTERVAL: {
            int sec = (int)DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_INTERVAL), g_main, intervalDlgProc,
                                           (LPARAM)g_store->settings.intervalSec);
            if (sec > 0 && sec != g_store->settings.intervalSec) setIntervalAll(sec);
            return 0;
        }
        case IDM_HELP_ABOUT:
            MessageBoxW(g_main, L"Thunderhead " TLR_VERSION_WSTRING L"\nBryce Lightning render farm console.\n\n"
                        L"Nodes run Lightning Rod " TLR_VERSION_WSTRING L".", L"About Thunderhead", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDC_CMD_CHECK_ALL: checkAll(true); return 0;
        case IDC_CMD_CHECK_NONE: checkAll(false); return 0;
        case IDC_CMD_CHECK_OUTDATED: checkOutdated(); return 0;
        case IDC_CMD_UPDATE: updateChecked(); return 0;
        case IDC_CMD_RENAME: renameChecked(); return 0;
        case IDC_CMD_OPEN_SHARE: openSharedFolders(); return 0;
        case IDC_CMD_COPY: copyToClients(); return 0;
        case IDC_CMD_PRIORITY: changeRenderPriority(); return 0;
        case IDC_CMD_BATCH: runRemoteBatch(); return 0;
        case IDC_CMD_WOL: sendWakeOnLan(); return 0;
        case IDC_CMD_PING: pingChecked(); return 0;
        case IDC_CMD_NETPING: networkPing(); return 0;
        case IDC_CMD_VIEW_LOCAL_LOG: viewLocalLog(); return 0;
        case IDC_CMD_VIEW_REMOTE_LOG: viewRemoteLogs(); return 0;
        case IDC_CMD_SET_TIME: setClientTime(); return 0;
        case IDC_CMD_MUTE: sendAudio("mute"); return 0;
        case IDC_CMD_UNMUTE: sendAudio("unmute"); return 0;
        case IDC_CMD_REMOVE_OFFLINE: removeCheckedOffline(); return 0;   // same as Node > Remove checked offline nodes
        case IDC_CMD_SHUTDOWN_NODE: shutdownChecked(); return 0;
        case IDC_CMD_ABORT_SHUTDOWN: sendPower("abort"); return 0;
        case IDC_FLAG_WOL: case IDC_FLAG_BOOT: case IDC_FLAG_AUTOLOGIN: case IDC_FLAG_AUTOROD: case IDC_FLAG_AUTOLIGHTNING:
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
    case WM_TIMER: if (wp == kTimerId) onTimer(); return 0;
    case WM_NET: { NetEvent* e = (NetEvent*)lp; onNet(e); delete e; return 0; }
    case WM_UPDATE_DONE: { UpdateResult* r = (UpdateResult*)lp; onUpdateDone(r); delete r; return 0; }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->hwndFrom == g_list) return onListNotify(hdr);
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
                 LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP | LVS_EX_CHECKBOXES);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
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
    g_flags[0] = createCheck(g_staticPane, IDC_FLAG_WOL, L"Wake On LAN");
    g_flags[1] = createCheck(g_staticPane, IDC_FLAG_BOOT, L"Boot");
    g_flags[2] = createCheck(g_staticPane, IDC_FLAG_AUTOLOGIN, L"Auto Login");
    g_flags[3] = createCheck(g_staticPane, IDC_FLAG_AUTOROD, L"Auto start Lightning Rod");
    g_flags[4] = createCheck(g_staticPane, IDC_FLAG_AUTOLIGHTNING, L"Auto start Lightning");
    g_staticList = createKeyValueList(g_staticPane, IDC_STATIC_LIST);
    ShowWindow(g_staticPane, SW_HIDE);

    // command pane
    for (int i = 0; i < kCommandCount; ++i) {
        HWND b = CreateWindowExW(0, L"BUTTON", kCommands[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
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
    g_events.open(g_dir + L"\\logs", L"thunderhead");
    CreateDirectoryW((g_dir + L"\\logs\\nodes").c_str(), NULL);
    logEvent("Thunderhead " TLR_VERSION_STRING " starting");

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
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
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 796, NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    createChildren(inst);
    layout();

    std::vector<NodeRecord*>& nodes = store.nodes();
    for (size_t i = 0; i < nodes.size(); ++i) refreshRow(nodes[i]);

    if (!server.start(g_main, WM_NET, (unsigned short)store.settings.tcpPort, hostName(), store.settings.intervalSec)) {
        MessageBoxW(g_main, L"The listening ports could not be opened. Another Thunderhead may be running; see the log.",
                    L"Thunderhead", MB_OK | MB_ICONERROR);
    }
    SetTimer(g_main, kTimerId, 1000, NULL);
    updateStatusBar();
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

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
        if (!g_edit && hotKeys && TranslateAcceleratorW(g_main, hotKeys, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (hotKeys) DestroyAcceleratorTable(hotKeys);

    server.stop();
    logEvent("Thunderhead stopped");
    for (std::map<std::string, Logger*>::iterator it = g_nodeLogs.begin(); it != g_nodeLogs.end(); ++it) delete it->second;
    g_nodeLogs.clear();
    netShutdown();
    DeleteObject(g_font);
    return 0;
}
