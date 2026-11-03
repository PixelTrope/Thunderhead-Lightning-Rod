// Thunderhead: host console for the Bryce Lightning render farm.
// See docs/PROTOCOL.md for the wire protocol and files.

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
const UINT_PTR kTimerId = 1;

HINSTANCE g_inst = NULL;
HWND g_main = NULL, g_list = NULL, g_status = NULL;
std::wstring g_dir;
NodeStore* g_store = NULL;
Server* g_server = NULL;
Logger g_events;
std::map<std::string, Logger*> g_nodeLogs;

// ---------------------------------------------------------------- columns

enum Column {
    ColName, ColStatus, ColRendering, ColLightning, ColLightningCpu, ColGroup, ColIp, ColAgent, ColOs,
    ColCpu, ColMhz, ColCores, ColCpuPct, ColMem, ColMemFree, ColDisk, ColDiskFree, ColGpu, ColVram,
    ColBoard, ColChipset, ColWol, ColBoot, ColAutoLogin, ColAutoRod, ColAutoLightning, ColLastReport,
    ColUptime, ColComment, ColMac, ColCount
};

struct ColumnDef { const wchar_t* title; int width; int align; };

const ColumnDef kColumns[ColCount] = {
    { L"Name",            110, LVCFMT_LEFT },
    { L"Status",           60, LVCFMT_LEFT },
    { L"Rendering",        70, LVCFMT_LEFT },
    { L"Lightning",        70, LVCFMT_LEFT },
    { L"Lightning %",      75, LVCFMT_RIGHT },
    { L"Group",            80, LVCFMT_LEFT },
    { L"IP",              100, LVCFMT_LEFT },
    { L"Agent",            50, LVCFMT_LEFT },
    { L"OS",              220, LVCFMT_LEFT },
    { L"CPU",             220, LVCFMT_LEFT },
    { L"MHz",              50, LVCFMT_RIGHT },
    { L"Cores",            45, LVCFMT_RIGHT },
    { L"CPU %",            50, LVCFMT_RIGHT },
    { L"Mem MB",           60, LVCFMT_RIGHT },
    { L"Mem free MB",      80, LVCFMT_RIGHT },
    { L"C: MB",            60, LVCFMT_RIGHT },
    { L"C: free MB",       75, LVCFMT_RIGHT },
    { L"GPU",             180, LVCFMT_LEFT },
    { L"VRAM MB",          65, LVCFMT_RIGHT },
    { L"Board",           180, LVCFMT_LEFT },
    { L"Chipset",         220, LVCFMT_LEFT },
    { L"WOL",              40, LVCFMT_LEFT },
    { L"Boot",             40, LVCFMT_LEFT },
    { L"Auto login",       70, LVCFMT_LEFT },
    { L"Auto Rod",         65, LVCFMT_LEFT },
    { L"Auto Lightning",   90, LVCFMT_LEFT },
    { L"Last report",      80, LVCFMT_RIGHT },
    { L"Uptime",           90, LVCFMT_RIGHT },
    { L"Comment",         200, LVCFMT_LEFT },
    { L"MAC",             120, LVCFMT_LEFT },
};

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
    const Message& h = n.hello;
    const Message& r = n.report;
    switch (col) {
    case ColName:         return widen(n.name.empty() ? n.mac : n.name);
    case ColStatus:       return n.online ? L"Online" : L"Offline";
    case ColRendering:    return !n.online || r.verb.empty() ? L"" : (parseBool(r.get("rendering")) ? L"Yes" : L"No");
    case ColLightning:    return !n.online || r.verb.empty() ? L"" : (r.get("lightning") == "running" ? L"Running" : L"Stopped");
    case ColLightningCpu: return !n.online || r.verb.empty() ? L"" : widen(r.get("lightning_cpu_pct"));
    case ColGroup:        return widen(n.group);
    case ColIp:           return widen(n.ip);
    case ColAgent:        return widen(h.get("agent"));
    case ColOs:           return widen(h.get("os"));
    case ColCpu:          return widen(h.get("cpu"));
    case ColMhz:          return widen(h.get("cpu_mhz"));
    case ColCores:        return widen(h.get("cores"));
    case ColCpuPct:       return !n.online || r.verb.empty() ? L"" : widen(r.get("cpu_pct"));
    case ColMem:          return widen(h.get("mem_mb"));
    case ColMemFree:      return !n.online || r.verb.empty() ? L"" : widen(r.get("mem_free_mb"));
    case ColDisk:         return widen(r.get("disk_total_mb"));
    case ColDiskFree:     return !n.online || r.verb.empty() ? L"" : widen(r.get("disk_free_mb"));
    case ColGpu:          return widen(h.get("gpu"));
    case ColVram:         return widen(h.get("vram_mb"));
    case ColBoard:        return widen(h.get("board"));
    case ColChipset:      return widen(h.get("chipset"));
    case ColWol:          return n.wol ? L"Yes" : L"No";
    case ColBoot:         return n.boot ? L"Yes" : L"No";
    case ColAutoLogin:    return n.autoLogin ? L"Yes" : L"No";
    case ColAutoRod:      return n.autoRod ? L"Yes" : L"No";
    case ColAutoLightning: return n.autoLightning ? L"Yes" : L"No";
    case ColLastReport: {
        if (!n.online || n.lastReportMs == 0) return L"";
        long long ago = (long long)((tickMs() - n.lastReportMs) / 1000);
        wchar_t buf[32];
        _snwprintf(buf, 31, L"%lld s ago", ago);
        buf[31] = L'\0';
        return buf;
    }
    case ColUptime:       return !n.online || r.verb.empty() ? L"" : durationText(atoll(r.get("uptime_s").c_str()));
    case ColComment:      return widen(n.comment);
    case ColMac:          return widen(n.mac);
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
        std::wstring text = columnText(*n, col);
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.iSubItem = col;
        item.pszText = (LPWSTR)text.c_str();
        SendMessageW(g_list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&item);
    }
}

void updateStatusBar(const std::wstring& extra = L"")
{
    int total = 0, online = 0, rendering = 0;
    std::vector<NodeRecord*>& nodes = g_store->nodes();
    for (size_t i = 0; i < nodes.size(); ++i) {
        ++total;
        if (nodes[i]->online) ++online;
        if (nodes[i]->online && parseBool(nodes[i]->report.get("rendering"))) ++rendering;
    }
    wchar_t buf[256];
    _snwprintf(buf, 255, L"%d nodes, %d online, %d rendering  |  interval %d s  |  UDP %d, TCP %d  %s",
               total, online, rendering, g_store->settings.intervalSec, (int)kDiscoveryPort,
               (int)g_server->tcpPort(), extra.c_str());
    buf[255] = L'\0';
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
}

std::vector<NodeRecord*> selectedNodes()
{
    std::vector<NodeRecord*> out;
    int row = -1;
    for (;;) {
        row = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)row, LVNI_SELECTED);
        if (row < 0) break;
        LVITEMW item;
        memset(&item, 0, sizeof(item));
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&item)) out.push_back((NodeRecord*)item.lParam);
    }
    return out;
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
}

void sendCommand(const char* verb)
{
    std::vector<NodeRecord*> sel = selectedNodes();
    int sent = 0;
    for (size_t i = 0; i < sel.size(); ++i) {
        NodeRecord* n = sel[i];
        if (!n->online) continue;
        Message m(verb);
        if (g_server->send(n->conn, m)) {
            ++sent;
            logEvent(format("%s -> %s", verb, n->name.c_str()));
            logNode(*n, std::string("-> ") + m.oneLine());
        }
    }
    updateStatusBar(widen(format("|  %s sent to %d node(s)", verb, sent)));
}

// ---------------------------------------------------------------- dialogs

INT_PTR CALLBACK nodeDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static NodeRecord* node = NULL;
    switch (msg) {
    case WM_INITDIALOG:
        node = (NodeRecord*)lp;
        SetDlgItemTextW(dlg, IDC_NODE_NAME, widen(node->name).c_str());
        SetDlgItemTextW(dlg, IDC_NODE_MAC, widen(node->mac).c_str());
        SetDlgItemTextW(dlg, IDC_NODE_GROUP, widen(node->group).c_str());
        CheckDlgButton(dlg, IDC_NODE_WOL, node->wol ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NODE_BOOT, node->boot ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NODE_AUTOLOGIN, node->autoLogin ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NODE_AUTOROD, node->autoRod ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dlg, IDC_NODE_AUTOLIGHTNING, node->autoLightning ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dlg, IDC_NODE_COMMENT, widen(node->comment).c_str());
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            wchar_t buf[2048];
            GetDlgItemTextW(dlg, IDC_NODE_GROUP, buf, 2048);
            node->group = narrow(trim(std::wstring(buf)));
            GetDlgItemTextW(dlg, IDC_NODE_COMMENT, buf, 2048);
            std::wstring comment = buf;
            for (size_t i = 0; i < comment.size(); ++i)
                if (comment[i] == L'\r' || comment[i] == L'\n') comment[i] = L' ';
            node->comment = narrow(trim(comment));
            node->wol = IsDlgButtonChecked(dlg, IDC_NODE_WOL) == BST_CHECKED;
            node->boot = IsDlgButtonChecked(dlg, IDC_NODE_BOOT) == BST_CHECKED;
            node->autoLogin = IsDlgButtonChecked(dlg, IDC_NODE_AUTOLOGIN) == BST_CHECKED;
            node->autoRod = IsDlgButtonChecked(dlg, IDC_NODE_AUTOROD) == BST_CHECKED;
            node->autoLightning = IsDlgButtonChecked(dlg, IDC_NODE_AUTOLIGHTNING) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

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

// ---------------------------------------------------------------- events

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
        logEvent(format("%s online from %s, agent %s%s", n->name.c_str(), n->ip.c_str(),
                        e->msg.get("agent").c_str(), isNew ? " (new node)" : ""));
        logNode(*n, "<- " + e->msg.oneLine());
        logNode(*n, format("-> SET interval=%d", g_store->settings.intervalSec));
        refreshRow(n);
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
        updateStatusBar(L"|  " + widen(text));
        break;
    }
    case NetEvent::Pong: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (!n) break;
        logNode(*n, "<- PONG");
        logEvent("PONG from " + n->name);
        updateStatusBar(L"|  PONG from " + widen(n->name));
        break;
    }
    case NetEvent::Disconnected: {
        NodeRecord* n = g_store->findByConn(e->conn);
        if (n) setOffline(n, e->text);
        break;
    }
    case NetEvent::Info:
        logEvent(e->text);
        updateStatusBar(L"|  " + widen(e->text));
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
        if (!n->online) continue;
        if (now - n->lastReportMs > limit) {
            setOffline(n, format("no report for %llu s", (now - n->lastReportMs) / 1000));
            continue;
        }
        refreshRow(n, ColLastReport);
    }
    updateStatusBar();
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

void editSelected()
{
    std::vector<NodeRecord*> sel = selectedNodes();
    if (sel.empty()) return;
    NodeRecord* n = sel[0];
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_NODE), g_main, nodeDlgProc, (LPARAM)n) == IDOK) {
        g_store->save();
        refreshRow(n);
        logEvent(format("%s edited: group=%s wol=%s boot=%s autologin=%s autostart_rod=%s autostart_lightning=%s",
                        n->name.c_str(), n->group.c_str(), boolText(n->wol), boolText(n->boot), boolText(n->autoLogin),
                        boolText(n->autoRod), boolText(n->autoLightning)));
    }
}

void removeSelected()
{
    std::vector<NodeRecord*> sel = selectedNodes();
    std::vector<NodeRecord*> offline;
    for (size_t i = 0; i < sel.size(); ++i)
        if (!sel[i]->online) offline.push_back(sel[i]);
    if (offline.empty()) {
        MessageBoxW(g_main, L"Select one or more offline nodes to remove.", L"Thunderhead", MB_OK | MB_ICONINFORMATION);
        return;
    }
    std::wstring q = widen(format("Remove %d offline node(s) and their saved settings?", (int)offline.size()));
    if (MessageBoxW(g_main, q.c_str(), L"Thunderhead", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    for (size_t i = 0; i < offline.size(); ++i) {
        int row = rowOf(offline[i]);
        if (row >= 0) SendMessageW(g_list, LVM_DELETEITEM, (WPARAM)row, 0);
        logEvent("removed node " + offline[i]->name + " " + offline[i]->mac);
        g_store->remove(offline[i]->mac);
    }
    g_store->save();
    updateStatusBar();
}

void onCommand(int id)
{
    switch (id) {
    case IDM_FILE_EXIT: DestroyWindow(g_main); break;
    case IDM_NODE_START: sendCommand("START"); break;
    case IDM_NODE_STOP: sendCommand("STOP"); break;
    case IDM_NODE_PING: sendCommand("PING"); break;
    case IDM_NODE_EDIT: editSelected(); break;
    case IDM_NODE_REMOVE: removeSelected(); break;
    case IDM_SETTINGS_INTERVAL: {
        int sec = (int)DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_INTERVAL), g_main, intervalDlgProc,
                                       (LPARAM)g_store->settings.intervalSec);
        if (sec > 0 && sec != g_store->settings.intervalSec) setIntervalAll(sec);
        break;
    }
    case IDM_HELP_ABOUT:
        MessageBoxW(g_main, L"Thunderhead " TLR_VERSION_WSTRING L"\nBryce Lightning render farm console.\n\n"
                    L"Nodes run Lightning Rod " TLR_VERSION_WSTRING L".", L"About Thunderhead", MB_OK | MB_ICONINFORMATION);
        break;
    }
}

void layout()
{
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sb;
    GetWindowRect(g_status, &sb);
    int sbHeight = sb.bottom - sb.top;
    MoveWindow(g_list, 0, 0, rc.right, rc.bottom - sbHeight, TRUE);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: layout(); return 0;
    case WM_COMMAND: onCommand(LOWORD(wp)); return 0;
    case WM_TIMER: if (wp == kTimerId) onTimer(); return 0;
    case WM_NET: { NetEvent* e = (NetEvent*)lp; onNet(e); delete e; return 0; }
    case WM_NOTIFY: {
        NMHDR* hdr = (NMHDR*)lp;
        if (hdr->hwndFrom == g_list) {
            if (hdr->code == NM_DBLCLK) { editSelected(); return 0; }
            if (hdr->code == LVN_KEYDOWN && ((NMLVKEYDOWN*)lp)->wVKey == VK_RETURN) { editSelected(); return 0; }
        }
        break;
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
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return CreateFontIndirectW(&ncm.lfMessageFont);
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
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
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    if (!netInit()) { MessageBoxW(NULL, L"Winsock failed to start.", L"Thunderhead", MB_OK | MB_ICONERROR); return 1; }

    NodeStore store(g_dir);
    store.load();
    g_store = &store;
    Server server;
    g_server = &server;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ThunderheadMain";
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_MENU);
    RegisterClassW(&wc);

    g_main = CreateWindowExW(0, L"ThunderheadMain", L"Thunderhead " TLR_VERSION_WSTRING,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 700, NULL, NULL, inst, NULL);
    if (!g_main) return 1;

    g_list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                             0, 0, 100, 100, g_main, NULL, inst, NULL);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_HEADERDRAGDROP);
    HFONT font = uiFont();
    SendMessageW(g_list, WM_SETFONT, (WPARAM)font, TRUE);
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
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)font, TRUE);
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
    DeleteObject(font);
    return 0;
}
