// Lightning Rod: node agent for the Bryce Lightning render farm.
// See docs/PROTOCOL.md for the wire protocol.
//
// The network loop runs on a worker thread; the main thread owns a window that
// shows the Thunderhead being reported to, the agent's start and running time,
// and every value sent, refreshed after each report.

#include "Net.h"
#include "Log.h"
#include "Protocol.h"
#include "StrUtil.h"
#include "Version.h"
#include "SysInfo.h"
#include "Lightning.h"

#include <commctrl.h>
#include <iphlpapi.h>
#include <cstring>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace tlr;

namespace {

const UINT WM_AGENT_UPDATE = WM_APP + 1;
const UINT_PTR kClockTimer = 1;

Logger g_log;
HWND g_window = NULL;
HANDLE g_singleInstance = NULL;

void logLine(const std::string& s) { g_log.write(s); }

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.rfind(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

// What the window shows; copied out of the agent under its lock.
struct AgentStatus {
    bool connected;
    std::string hostName, hostIp, hostMac;
    int intervalSec;
    SYSTEMTIME lastReportTime;
    bool haveReport;
    StaticInfo info;
    Message report;
    AgentStatus() : connected(false), intervalSec(kDefaultReportInterval), haveReport(false) { memset(&lastReportTime, 0, sizeof(lastReportTime)); }
};

// MAC of a LAN address through ARP; the node's own MAC when the host is this machine.
std::string macOf(const std::string& ip, const StaticInfo& self)
{
    if (ip == self.ip || ip == "127.0.0.1") return self.mac;
    ULONG mac[2] = { 0, 0 };
    ULONG len = 6;
    if (SendARP(inet_addr(ip.c_str()), 0, mac, &len) != NO_ERROR || len != 6) return std::string();
    const BYTE* b = (const BYTE*)mac;
    return format("%02X-%02X-%02X-%02X-%02X-%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// Connects with a timeout; returns INVALID_SOCKET on failure.
SOCKET connectWithTimeout(const sockaddr_in& addr, int timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    setNonBlocking(s, true);
    int r = connect(s, (const sockaddr*)&addr, sizeof(addr));
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s, &wr); FD_SET(s, &ex);
    timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(0, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex)) { closesocket(s); return INVALID_SOCKET; }
    setNonBlocking(s, false);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    return s;
}

// ---------------------------------------------------------------- agent

class Agent {
public:
    Agent() : udp_(INVALID_SOCKET), tcp_(INVALID_SOCKET), intervalSec_(kDefaultReportInterval), nextReport_(0)
    {
        InitializeCriticalSection(&lock_);
    }
    ~Agent() { DeleteCriticalSection(&lock_); }

    bool init()
    {
        info_ = collectStatic();
        status_.info = info_;
        logLine(format("Lightning Rod %s starting: %s mac=%s ip=%s", TLR_VERSION_STRING,
                       info_.name.c_str(), info_.mac.c_str(), info_.ip.c_str()));
        logLine(format("os=%s cpu=%s %d MHz cores=%d mem=%d MB disk=%d MB board=%s chipset=%s gpu=%s vram=%d MB",
                       info_.os.c_str(), info_.cpu.c_str(), info_.cpuMhz, info_.cores, info_.memMb, info_.diskTotalMb,
                       info_.board.c_str(), info_.chipset.c_str(), info_.gpu.c_str(), info_.vramMb));
        udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_ == INVALID_SOCKET) { logLine("udp socket failed"); return false; }
        BOOL yes = TRUE;
        setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
        sockaddr_in any;
        memset(&any, 0, sizeof(any));
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = INADDR_ANY;
        any.sin_port = 0;
        if (bind(udp_, (sockaddr*)&any, sizeof(any)) == SOCKET_ERROR) { logLine("udp bind failed"); return false; }
        return true;
    }

    void run()
    {
        for (;;) {
            if (tcp_ == INVALID_SOCKET) announcePhase();
            else sessionPhase();
        }
    }

    void snapshot(AgentStatus& out)
    {
        EnterCriticalSection(&lock_);
        out = status_;
        LeaveCriticalSection(&lock_);
    }

private:
    void notify()
    {
        if (g_window) PostMessageW(g_window, WM_AGENT_UPDATE, 0, 0);
    }

    // ---- announcing ------------------------------------------------------
    void sendAnnounce()
    {
        Message m("ANNOUNCE");
        m.set("mac", info_.mac);
        m.set("name", info_.name);
        m.set("agent", TLR_VERSION_STRING);
        std::string wire = m.encode();
        std::vector<unsigned long> targets = broadcastAddresses();
        targets.push_back(INADDR_BROADCAST);
        for (size_t i = 0; i < targets.size(); ++i) {
            sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = targets[i];
            to.sin_port = htons(kDiscoveryPort);
            sendto(udp_, wire.data(), (int)wire.size(), 0, (sockaddr*)&to, sizeof(to));
        }
    }

    void announcePhase()
    {
        unsigned long long nextAnnounce = 0;
        bool logged = false;
        while (tcp_ == INVALID_SOCKET) {
            unsigned long long now = tickMs();
            if (now >= nextAnnounce) {
                sendAnnounce();
                nextAnnounce = now + (unsigned long long)kAnnounceIntervalSec * 1000;
                if (!logged) { logLine("announcing, waiting for Thunderhead"); logged = true; }
            }
            long long waitMs = (long long)(nextAnnounce - now);
            if (waitMs > 1000) waitMs = 1000;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(udp_, &rd);
            timeval tv; tv.tv_sec = (long)(waitMs / 1000); tv.tv_usec = (long)((waitMs % 1000) * 1000);
            if (select(0, &rd, NULL, NULL, &tv) > 0 && FD_ISSET(udp_, &rd)) {
                char buf[4096];
                sockaddr_in from;
                int fromLen = sizeof(from);
                int n = recvfrom(udp_, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
                if (n <= 0) continue;
                Message m;
                if (!parseMessage(std::string(buf, (size_t)n), m) || m.verb != "OFFER") continue;
                int port = m.getInt("port", kDefaultTcpPort);
                sockaddr_in host = from;
                host.sin_port = htons((unsigned short)port);
                logLine(format("OFFER from %s (%s) port %d", ipText(from).c_str(), m.get("host").c_str(), port));
                SOCKET s = connectWithTimeout(host, 3000);
                if (s == INVALID_SOCKET) { logLine("connect failed"); continue; }
                tcp_ = s;
                parser_.clear();
                if (!sendHello()) { dropSession("hello send failed"); continue; }
                std::string hostIp = ipText(from);
                logLine(format("connected to Thunderhead at %s:%d", hostIp.c_str(), port));
                EnterCriticalSection(&lock_);
                status_.connected = true;
                status_.hostName = m.get("host");
                status_.hostIp = hostIp;
                status_.hostMac = macOf(hostIp, info_);
                status_.haveReport = false;
                status_.report = Message();
                LeaveCriticalSection(&lock_);
                notify();
                nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
                cpu_.sample();
                lightning_.query();
            }
        }
    }

    // ---- session ---------------------------------------------------------
    bool sendMessage(const Message& m)
    {
        return sendAll(tcp_, m.encode());
    }

    bool sendHello()
    {
        Message m("HELLO");
        m.set("mac", info_.mac);
        m.set("name", info_.name);
        m.set("agent", TLR_VERSION_STRING);
        m.set("ip", info_.ip);
        m.set("os", info_.os);
        m.set("cpu", info_.cpu);
        m.set("cpu_mhz", (long long)info_.cpuMhz);
        m.set("cores", (long long)info_.cores);
        m.set("mem_mb", (long long)info_.memMb);
        m.set("disk_total_mb", (long long)info_.diskTotalMb);
        m.set("board", info_.board);
        m.set("chipset", info_.chipset);
        m.set("gpu", info_.gpu);
        m.set("vram_mb", (long long)info_.vramMb);
        return sendMessage(m);
    }

    bool sendReport()
    {
        DynamicInfo d = collectDynamic(cpu_);
        LightningState l = lightning_.query();
        Message m("REPORT");
        m.set("cpu_pct", (long long)d.cpuPct);
        m.set("mem_free_mb", (long long)d.memFreeMb);
        m.set("disk_free_mb", (long long)d.diskFreeMb);
        m.set("uptime_s", d.uptimeS);
        m.set("lightning", l.running ? "running" : "stopped");
        m.set("lightning_cpu_pct", (long long)l.cpuPct);
        m.set("rendering", boolText(l.rendering));
        bool ok = sendMessage(m);
        if (ok) {
            EnterCriticalSection(&lock_);
            status_.report = m;
            status_.haveReport = true;
            status_.intervalSec = intervalSec_;
            GetLocalTime(&status_.lastReportTime);
            LeaveCriticalSection(&lock_);
            notify();
        }
        return ok;
    }

    void dropSession(const char* why)
    {
        logLine(format("session ended: %s", why));
        if (tcp_ != INVALID_SOCKET) { closesocket(tcp_); tcp_ = INVALID_SOCKET; }
        parser_.clear();
        EnterCriticalSection(&lock_);
        status_.connected = false;
        LeaveCriticalSection(&lock_);
        notify();
    }

    void handle(const Message& m)
    {
        if (m.verb == "SET") {
            int iv = m.getInt("interval", intervalSec_);
            if (iv < 1) iv = 1;
            if (iv > 3600) iv = 3600;
            if (iv != intervalSec_) logLine(format("report interval %d s", iv));
            intervalSec_ = iv;
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
            EnterCriticalSection(&lock_);
            status_.intervalSec = intervalSec_;
            LeaveCriticalSection(&lock_);
            notify();
        } else if (m.verb == "START" || m.verb == "STOP") {
            std::string err;
            bool ok = m.verb == "START" ? startLightning(err) : stopLightning(err);
            logLine(format("%s Lightning: %s%s%s", m.verb.c_str(), ok ? "ok" : "failed",
                           err.empty() ? "" : " ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            sendMessage(ack);
            sendReport();
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
        } else if (m.verb == "PING") {
            sendMessage(Message("PONG"));
        } else if (m.verb == "EXIT" || m.verb == "RESTART") {
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", "yes");
            ack.set("error", "");
            sendMessage(ack);
            logLine(m.verb == "EXIT" ? "EXIT received, stopping" : "RESTART received, relaunching");
            closesocket(tcp_);
            tcp_ = INVALID_SOCKET;
            if (g_singleInstance) { CloseHandle(g_singleInstance); g_singleInstance = NULL; }
            if (m.verb == "RESTART") {
                wchar_t path[MAX_PATH];
                DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
                std::wstring cmd = L"\"" + std::wstring(path, n) + L"\"";
                std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                cmdBuf.push_back(L'\0');
                STARTUPINFOW si;
                PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof(si));
                si.cb = sizeof(si);
                memset(&pi, 0, sizeof(pi));
                if (CreateProcessW(path, &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                } else {
                    logLine("relaunch failed: " + lastErrorText((int)GetLastError()));
                }
            }
            ExitProcess(0);
        } else {
            logLine("unknown verb from Thunderhead: " + m.verb);
        }
    }

    void sessionPhase()
    {
        while (tcp_ != INVALID_SOCKET) {
            unsigned long long now = tickMs();
            if (now >= nextReport_) {
                if (!sendReport()) { dropSession("report send failed"); return; }
                nextReport_ = now + (unsigned long long)intervalSec_ * 1000;
            }
            long long waitMs = (long long)(nextReport_ - now);
            if (waitMs > 1000) waitMs = 1000;
            if (waitMs < 0) waitMs = 0;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(tcp_, &rd);
            FD_SET(udp_, &rd);
            timeval tv; tv.tv_sec = (long)(waitMs / 1000); tv.tv_usec = (long)((waitMs % 1000) * 1000);
            int r = select(0, &rd, NULL, NULL, &tv);
            if (r < 0) { dropSession("select failed"); return; }
            if (r == 0) continue;
            if (FD_ISSET(udp_, &rd)) {
                char buf[4096];
                sockaddr_in from;
                int fromLen = sizeof(from);
                recvfrom(udp_, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);   // stray OFFER; ignored
            }
            if (FD_ISSET(tcp_, &rd)) {
                char buf[8192];
                int n = recv(tcp_, buf, sizeof(buf), 0);
                if (n == 0) { dropSession("closed by Thunderhead"); return; }
                if (n < 0) { dropSession(lastErrorText(WSAGetLastError()).c_str()); return; }
                parser_.feed(buf, (size_t)n);
                if (parser_.overflow()) { dropSession("oversized message"); return; }
                Message m;
                while (tcp_ != INVALID_SOCKET && parser_.next(m)) handle(m);
            }
        }
    }

    SOCKET udp_, tcp_;
    StaticInfo info_;
    CpuTracker cpu_;
    LightningMonitor lightning_;
    MessageParser parser_;
    int intervalSec_;
    unsigned long long nextReport_;
    CRITICAL_SECTION lock_;
    AgentStatus status_;
};

Agent* g_agent = NULL;

DWORD WINAPI agentThread(LPVOID)
{
    g_agent->run();
    return 0;
}

// ---------------------------------------------------------------- window

HWND g_hostText = NULL, g_timeText = NULL, g_reportText = NULL, g_list = NULL;
SYSTEMTIME g_started;
unsigned long long g_startedMs = 0;

std::wstring timeText(const SYSTEMTIME& st)
{
    wchar_t buf[64];
    _snwprintf(buf, 63, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    buf[63] = L'\0';
    return buf;
}

std::wstring durationText(unsigned long long seconds)
{
    unsigned long long d = seconds / 86400, h = (seconds / 3600) % 24, m = (seconds / 60) % 60, s = seconds % 60;
    wchar_t buf[64];
    if (d > 0) _snwprintf(buf, 63, L"%llud %02llu:%02llu:%02llu", d, h, m, s);
    else _snwprintf(buf, 63, L"%02llu:%02llu:%02llu", h, m, s);
    buf[63] = L'\0';
    return buf;
}

void setRow(int row, const wchar_t* item, const std::wstring& value)
{
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    if (row >= count) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = (LPWSTR)item;
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    }
    memset(&it, 0, sizeof(it));
    it.iSubItem = 1;
    it.pszText = (LPWSTR)value.c_str();
    SendMessageW(g_list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

void refreshClock()
{
    unsigned long long running = (tickMs() - g_startedMs) / 1000;
    std::wstring text = L"Lightning Rod " TLR_VERSION_WSTRING L" started " + timeText(g_started) + L", running " + durationText(running);
    SetWindowTextW(g_timeText, text.c_str());
}

void refreshAll()
{
    AgentStatus s;
    g_agent->snapshot(s);

    std::wstring host;
    if (s.connected) {
        host = L"Reporting to Thunderhead on " + widen(s.hostName) + L"  (" + widen(s.hostIp) +
               (s.hostMac.empty() ? L"" : L", " + widen(s.hostMac)) + L")";
    } else {
        host = L"Not connected: announcing, waiting for Thunderhead";
    }
    SetWindowTextW(g_hostText, host.c_str());

    std::wstring rep;
    if (s.connected && s.haveReport)
        rep = widen(format("Report interval %d s, last report sent ", s.intervalSec)) + timeText(s.lastReportTime);
    else if (s.connected)
        rep = widen(format("Report interval %d s, first report pending", s.intervalSec));
    else
        rep = L"";
    SetWindowTextW(g_reportText, rep.c_str());

    const Message& r = s.report;
    const StaticInfo& i = s.info;
    int row = 0;
    setRow(row++, L"CPU load %",          s.haveReport ? widen(r.get("cpu_pct")) : L"");
    setRow(row++, L"Memory free MB",      s.haveReport ? widen(r.get("mem_free_mb")) : L"");
    setRow(row++, L"C: free MB",          s.haveReport ? widen(r.get("disk_free_mb")) : L"");
    setRow(row++, L"Uptime",              s.haveReport ? durationText((unsigned long long)atoll(r.get("uptime_s").c_str())) : L"");
    setRow(row++, L"Lightning",           s.haveReport ? (r.get("lightning") == "running" ? L"Running" : L"Stopped") : L"");
    setRow(row++, L"Lightning CPU %",     s.haveReport ? widen(r.get("lightning_cpu_pct")) : L"");
    setRow(row++, L"Rendering",           s.haveReport ? (parseBool(r.get("rendering")) ? L"Yes" : L"No") : L"");
    setRow(row++, L"Sent once at connect", L"");
    setRow(row++, L"Name",                widen(i.name));
    setRow(row++, L"IP",                  widen(i.ip));
    setRow(row++, L"MAC",                 widen(i.mac));
    setRow(row++, L"Agent",               TLR_VERSION_WSTRING);
    setRow(row++, L"OS",                  widen(i.os));
    setRow(row++, L"CPU",                 widen(i.cpu));
    setRow(row++, L"MHz",                 widen(format("%d", i.cpuMhz)));
    setRow(row++, L"Cores",               widen(format("%d", i.cores)));
    setRow(row++, L"Memory MB",           widen(format("%d", i.memMb)));
    setRow(row++, L"C: MB",               widen(format("%d", i.diskTotalMb)));
    setRow(row++, L"Board",               widen(i.board));
    setRow(row++, L"Chipset",             widen(i.chipset));
    setRow(row++, L"GPU",                 widen(i.gpu));
    setRow(row++, L"VRAM MB",             widen(format("%d", i.vramMb)));
    refreshClock();
}

void layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int margin = 8, line = 18;
    int y = margin;
    MoveWindow(g_hostText, margin, y, rc.right - 2 * margin, line, TRUE);   y += line + 2;
    MoveWindow(g_timeText, margin, y, rc.right - 2 * margin, line, TRUE);   y += line + 2;
    MoveWindow(g_reportText, margin, y, rc.right - 2 * margin, line, TRUE); y += line + 6;
    MoveWindow(g_list, margin, y, rc.right - 2 * margin, rc.bottom - y - margin, TRUE);
    int valueWidth = rc.right - 2 * margin - 150 - GetSystemMetrics(SM_CXVSCROLL);
    if (valueWidth < 100) valueWidth = 100;
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1, (LPARAM)valueWidth);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: layout(hwnd); return 0;
    case WM_AGENT_UPDATE: refreshAll(); return 0;
    case WM_TIMER: if (wp == kClockTimer) refreshClock(); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: KillTimer(hwnd, kClockTimer); PostQuitMessage(0); return 0;
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

bool createWindow(HINSTANCE inst, const std::string& nodeName)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LightningRodMain";
    RegisterClassW(&wc);

    std::wstring title = L"Lightning Rod " TLR_VERSION_WSTRING L" on " + widen(nodeName);
    g_window = CreateWindowExW(0, L"LightningRodMain", title.c_str(), WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 560, NULL, NULL, inst, NULL);
    if (!g_window) return false;
    HFONT font = uiFont();
    g_hostText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, g_window, NULL, inst, NULL);
    g_timeText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, g_window, NULL, inst, NULL);
    g_reportText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, g_window, NULL, inst, NULL);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER,
                             0, 0, 0, 0, g_window, NULL, inst, NULL);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessageW(g_hostText, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_timeText, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_reportText, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)font, TRUE);
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (LPWSTR)L"Item";
    col.cx = 150;
    col.iSubItem = 0;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Value";
    col.cx = 440;
    col.iSubItem = 1;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    layout(g_window);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    g_singleInstance = CreateMutexW(NULL, TRUE, L"Local\\LightningRodAgent");
    if (g_singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GetLocalTime(&g_started);
    g_startedMs = tickMs();
    g_log.open(exeDirectory(), L"LightningRod");
    if (!netInit()) { logLine("WSAStartup failed"); return 1; }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    Agent agent;
    g_agent = &agent;
    if (!agent.init()) return 1;

    AgentStatus s;
    agent.snapshot(s);
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!createWindow(inst, s.info.name)) return 1;
    refreshAll();
    ShowWindow(g_window, SW_SHOWNORMAL);
    UpdateWindow(g_window);
    SetTimer(g_window, kClockTimer, 1000, NULL);

    HANDLE thread = CreateThread(NULL, 0, agentThread, NULL, 0, NULL);
    if (!thread) { logLine("agent thread failed"); return 1; }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    logLine("window closed, agent stopping");
    g_window = NULL;
    netShutdown();
    ExitProcess(0);
}
