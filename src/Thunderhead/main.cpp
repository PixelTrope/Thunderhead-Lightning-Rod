// Thunderhead: host console for the Bryce Lightning render farm.
// See docs/PROTOCOL.md for the wire protocol and files.
//
// Window layout: the left half is the Client Inventory (one row per node with a
// check box, the per-cycle values, Group and Comment editable in place); the top
// right is the static pane for the clicked node (HELLO data and the five flags);
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
    ColName, ColStatus, ColAgent, ColRendering, ColLightning, ColLightningCpu, ColCpuPct, ColMemFree, ColDiskFree,
    ColUptime, ColLastReport, ColGroup, ColComment, ColCount
};

struct ColumnDef { const wchar_t* title; int width; int align; };

const ColumnDef kColumns[ColCount] = {
    { L"Name",         130, LVCFMT_LEFT },
    { L"Status",        65, LVCFMT_LEFT },
    { L"Agent",         50, LVCFMT_LEFT },
    { L"Rendering",     70, LVCFMT_LEFT },
    { L"Lightning",     70, LVCFMT_LEFT },
    { L"Lightning %",   75, LVCFMT_RIGHT },
    { L"CPU %",         50, LVCFMT_RIGHT },
    { L"Mem free MB",   80, LVCFMT_RIGHT },
    { L"C: free MB",    75, LVCFMT_RIGHT },
    { L"Uptime",        90, LVCFMT_RIGHT },
    { L"Last report",   80, LVCFMT_RIGHT },
    { L"Group",        100, LVCFMT_LEFT },
    { L"Comment",      220, LVCFMT_LEFT },
};

// ---------------------------------------------------------------- command buttons

struct CommandDef { int id; const wchar_t* label; const char* verb; bool live; };

const CommandDef kCommands[] = {
    { IDC_CMD_START71,       L"Start Lightning 7.1", "START",   true  },
    { IDC_CMD_STOP,          L"Stop Lightning",      "STOP",    true  },
    { IDC_CMD_PING,          L"Ping",                "PING",    true  },
    { IDC_CMD_RESTART_LR,    L"Restart LR",          "RESTART", true  },
    { IDC_CMD_EXIT_LR,       L"Exit LR",             "EXIT",    true  },
    { IDC_CMD_CHECK_ALL,     L"Check all",           NULL,      true  },
    { IDC_CMD_CHECK_NONE,    L"Check none",          NULL,      true  },
    { IDC_CMD_CHECK_OUTDATED, L"Check outdated",     NULL,      true  },
    { IDC_CMD_UPDATE,        L"Update checked",      NULL,      true  },
    { IDC_CMD_START61,       L"Start Lightning 6.1", NULL,      false },
    { IDC_CMD_START63,       L"Start Lightning 6.3", NULL,      false },
    { IDC_CMD_RESTART_NODE,  L"Restart node",        NULL,      false },
    { IDC_CMD_SHUTDOWN_NODE, L"Shut down node",      NULL,      false },
    { IDC_CMD_START_TV,      L"Start TeamViewer",    NULL,      false },
    { IDC_CMD_MESSAGE,       L"Display message",     NULL,      false },
    { IDC_CMD_SOUND,         L"Play sound",          NULL,      false },
    { IDC_CMD_BATCH,         L"Run batch file",      NULL,      false },
    { IDC_CMD_WOL,           L"Wake On LAN",         NULL,      false },
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

std::wstring columnText(const NodeRecord& n, int col)
{
    const Message& r = n.report;
    bool live = n.online && !r.verb.empty();
    switch (col) {
    case ColName:         return widen(n.name.empty() ? n.mac : n.name);
    case ColStatus:       return n.updating ? L"Updating" : (n.online ? L"Online" : L"Offline");
    case ColAgent:        return widen(n.hello.get("agent"));
    case ColRendering:    return !live ? L"" : (parseBool(r.get("rendering")) ? L"Yes" : L"No");
    case ColLightning:    return !live ? L"" : (r.get("lightning") == "running" ? L"Running" : L"Stopped");
    case ColLightningCpu: return !live ? L"" : widen(r.get("lightning_cpu_pct"));
    case ColCpuPct:       return !live ? L"" : widen(r.get("cpu_pct"));
    case ColMemFree:      return !live ? L"" : widen(r.get("mem_free_mb"));
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

void refreshRow(NodeRecord* n, int onlyColumn = -1)
{
    int row = rowOf(n);
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
    }
    for (int col = 0; col < ColCount; ++col) {
        if (onlyColumn >= 0 && col != onlyColumn) continue;
        if (g_edit && row == g_editRow && col == g_editCol) continue;   // being edited
        setCell(g_list, row, col, columnText(*n, col));
    }
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
    std::wstring title = L"Static data: " + widen(n->name.empty() ? n->mac : n->name) +
                         (n->online ? L"" : L"  (offline, last known values)");
    SetWindowTextW(g_staticTitle, title.c_str());
    CheckDlgButton(g_staticPane, IDC_FLAG_WOL, n->wol ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_BOOT, n->boot ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_AUTOLOGIN, n->autoLogin ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_AUTOROD, n->autoRod ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_staticPane, IDC_FLAG_AUTOLIGHTNING, n->autoLightning ? BST_CHECKED : BST_UNCHECKED);

    struct Row { const wchar_t* item; std::wstring value; };
    Row rows[] = {
        { L"Name",       widen(n->name) },
        { L"MAC",        widen(n->mac) },
        { L"IP",         widen(n->ip.empty() ? h.get("ip") : n->ip) },
        { L"Agent",      widen(h.get("agent")) },
        { L"OS",         widen(h.get("os")) },
        { L"CPU",        widen(h.get("cpu")) },
        { L"MHz",        widen(h.get("cpu_mhz")) },
        { L"Cores",      widen(h.get("cores")) },
        { L"Memory MB",  widen(h.get("mem_mb")) },
        { L"C: MB",      widen(h.get("disk_total_mb")) },
        { L"Board",      widen(h.get("board")) },
        { L"Chipset",    widen(h.get("chipset")) },
        { L"GPU",        widen(h.get("gpu")) },
        { L"VRAM MB",    widen(h.get("vram_mb")) },
    };
    const int count = sizeof(rows) / sizeof(rows[0]);
    int have = (int)SendMessageW(g_staticList, LVM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        if (i >= have) {
            LVITEMW item;
            memset(&item, 0, sizeof(item));
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = (LPWSTR)rows[i].item;
            SendMessageW(g_staticList, LVM_INSERTITEMW, 0, (LPARAM)&item);
        }
        setCell(g_staticList, i, 1, rows[i].value);
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

struct UpdateJob { std::string name; std::string ip; std::wstring source; std::wstring share; };
struct UpdateResult { std::string name; bool ok; std::string text; };

// Worker thread: copies the agent over the node's share after the old agent has exited,
// then drops update.go so the update helper on the node starts the new one.
DWORD WINAPI updateCopyThread(LPVOID p)
{
    UpdateJob* job = (UpdateJob*)p;
    UpdateResult* res = new UpdateResult();
    res->name = job->name;
    res->ok = false;
    std::wstring dir = L"\\\\" + widen(job->ip) + L"\\" + job->share + L"\\";   // by address: clones share a name until renamed
    std::wstring staged = dir + L"LightningRod.new";
    std::wstring target = dir + L"LightningRod.exe";
    if (!CopyFileW(job->source.c_str(), staged.c_str(), FALSE)) {
        res->text = "copy to " + narrow(dir) + " failed: " + lastErrorText((int)GetLastError());
    } else {
        bool moved = false;
        for (int attempt = 0; attempt < 40 && !moved; ++attempt) {    // the old exe may still be closing
            moved = MoveFileExW(staged.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!moved) Sleep(500);
        }
        if (!moved) {
            res->text = "replace of LightningRod.exe failed: " + lastErrorText((int)GetLastError());
            DeleteFileW(staged.c_str());
        } else {
            res->ok = true;
            res->text = "LightningRod.exe replaced through " + narrow(dir);
        }
    }
    // Always signal, so the node restarts whatever agent file it has.
    HANDLE go = CreateFileW((dir + L"update.go").c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (go != INVALID_HANDLE_VALUE) CloseHandle(go);
    else res->text += "; update.go could not be written (" + lastErrorText((int)GetLastError()) + "), the node restarts the agent after 2 minutes";
    delete job;
    if (!PostMessageW(g_main, WM_UPDATE_DONE, 0, (LPARAM)res)) delete res;
    return 0;
}

void startUpdateCopy(NodeRecord* n)
{
    UpdateJob* job = new UpdateJob();
    job->name = n->name;
    job->ip = n->ip;
    job->source = pushFilePath();
    job->share = widen(g_store->settings.shareName);
    logEvent(n->name + " update: agent stopped, copying " + narrow(job->source));
    HANDLE t = CreateThread(NULL, 0, updateCopyThread, job, 0, NULL);
    if (t) CloseHandle(t);
    else { delete job; logEvent(n->name + " update: could not start the copy thread"); }
}

void onUpdateDone(UpdateResult* r)
{
    std::string text = r->name + " update: " + r->text;
    logEvent(text);
    NodeRecord* n = NULL;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i]->name == r->name) n = nodes[i];
    if (n && !r->ok) { n->updating = false; refreshRow(n); }
    updateStatusBar((L"|  " + widen(text)).c_str());
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
        Message m("UPDATE");
        if (g_server->send(n->conn, m)) {
            ++sent;
            n->updating = true;
            n->updateStartMs = tickMs();
            logEvent(format("UPDATE -> %s (agent %s, pushing %s)", n->name.c_str(), n->hello.get("agent").c_str(), version.c_str()));
            logNode(*n, "-> " + m.oneLine());
            refreshRow(n);
        }
    }
    updateStatusBar(widen(format("|  UPDATE sent to %d of %d checked", sent, (int)targets.size())).c_str());
}

// ---------------------------------------------------------------- commands

void sendCommand(const char* verb)
{
    std::vector<NodeRecord*> targets = checkedNodes();
    if (targets.empty()) { updateStatusBar(L"|  no clients checked"); return; }
    int sent = 0;
    for (size_t i = 0; i < targets.size(); ++i) {
        NodeRecord* n = targets[i];
        if (!n->online) continue;
        Message m(verb);
        if (g_server->send(n->conn, m)) {
            ++sent;
            logEvent(format("%s -> %s", verb, n->name.c_str()));
            logNode(*n, std::string("-> ") + m.oneLine());
        }
    }
    updateStatusBar(widen(format("|  %s sent to %d of %d checked", verb, sent, (int)targets.size())).c_str());
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
        n->report = e->msg;
        n->lastReportMs = now;
        logNode(*n, "<- " + e->msg.oneLine());
        refreshRow(n);
        break;
    }
    case NetEvent::Ack: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        logNode(*n, "<- " + e->msg.oneLine());
        bool ok = parseBool(e->msg.get("ok"));
        std::string text = format("%s %s on %s%s%s", e->msg.get("cmd").c_str(), ok ? "ok" : "failed",
                                  n->name.c_str(), ok ? "" : ": ", ok ? "" : e->msg.get("error").c_str());
        logEvent(text);
        updateStatusBar((L"|  " + widen(text)).c_str());
        break;
    }
    case NetEvent::Pong: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        logNode(*n, "<- PONG");
        logEvent("PONG from " + n->name);
        updateStatusBar((L"|  PONG from " + widen(n->name)).c_str());
        break;
    }
    case NetEvent::Disconnected: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (n) {
            setOffline(n, e->text);
            if (n->updating) startUpdateCopy(n);
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
        if (n->updating && !n->online && now - n->updateStartMs > 180000) {
            n->updating = false;
            logEvent(n->name + " update: the agent did not come back within 3 minutes");
            refreshRow(n);
        }
        if (!n->online) continue;
        if (now - n->lastReportMs > limit) {
            setOffline(n, format("no report for %llu s", (now - n->lastReportMs) / 1000));
            continue;
        }
        refreshRow(n, ColLastReport);
    }
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
    int valueWidth = rc.right - 2 * margin - 90 - GetSystemMetrics(SM_CXVSCROLL) - 4;
    if (valueWidth < 120) valueWidth = 120;
    SendMessageW(g_staticList, LVM_SETCOLUMNWIDTH, 1, (LPARAM)valueWidth);
}

void layoutCommandPane()
{
    RECT rc;
    GetClientRect(g_commandPane, &rc);
    const int margin = 8, gap = 6, bw = 150, bh = 30;
    int perRow = (rc.right - 2 * margin + gap) / (bw + gap);
    if (perRow < 1) perRow = 1;
    for (int i = 0; i < kCommandCount; ++i) {
        int x = margin + (i % perRow) * (bw + gap);
        int y = margin + (i / perRow) * (bh + gap);
        MoveWindow(GetDlgItem(g_commandPane, kCommands[i].id), x, y, bw, bh, TRUE);
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
    int left = rc.right / 2;
    int top = height / 2;
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

void onListNotify(NMHDR* hdr)
{
    switch (hdr->code) {
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
    case LVN_COLUMNCLICK:
        endEdit(true);
        break;
    }
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
        if (hdr->hwndFrom == g_list) onListNotify(hdr);
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
    col.cx = 90;
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
    wc.lpszClassName = L"ThunderheadMain";
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MENU);
    RegisterClassW(&wc);

    g_main = CreateWindowExW(0, L"ThunderheadMain", L"Thunderhead " TLR_VERSION_WSTRING,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 760, NULL, NULL, inst, NULL);
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

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    server.stop();
    logEvent("Thunderhead stopped");
    for (std::map<std::string, Logger*>::iterator it = g_nodeLogs.begin(); it != g_nodeLogs.end(); ++it) delete it->second;
    g_nodeLogs.clear();
    netShutdown();
    DeleteObject(g_font);
    return 0;
}
