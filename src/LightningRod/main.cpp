// Lightning Rod: node agent for the Bryce Lightning render farm.
// See docs/PROTOCOL.md for the wire protocol.

#include "Net.h"
#include "Log.h"
#include "Protocol.h"
#include "StrUtil.h"
#include "Version.h"
#include "SysInfo.h"
#include "Lightning.h"

#include <cstring>
#include <vector>

using namespace tlr;

namespace {

Logger g_log;

void logLine(const std::string& s) { g_log.write(s); }

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.rfind(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
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

class Agent {
public:
    Agent() : udp_(INVALID_SOCKET), tcp_(INVALID_SOCKET), intervalSec_(kDefaultReportInterval) {}

    bool init()
    {
        info_ = collectStatic();
        logLine(format("Lightning Rod %s starting: %s mac=%s ip=%s", TLR_VERSION_STRING,
                       info_.name.c_str(), info_.mac.c_str(), info_.ip.c_str()));
        logLine(format("os=%s cpu=%s %d MHz cores=%d mem=%d MB board=%s chipset=%s gpu=%s vram=%d MB",
                       info_.os.c_str(), info_.cpu.c_str(), info_.cpuMhz, info_.cores, info_.memMb,
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

private:
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
                hostText_ = ipText(from) + ":" + format("%d", port);
                parser_.clear();
                if (!sendHello()) { dropSession("hello send failed"); continue; }
                logLine("connected to Thunderhead at " + hostText_);
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
        return sendMessage(m);
    }

    void dropSession(const char* why)
    {
        logLine(format("session ended: %s", why));
        if (tcp_ != INVALID_SOCKET) { closesocket(tcp_); tcp_ = INVALID_SOCKET; }
        parser_.clear();
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
    std::string hostText_;
};

} // namespace

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    HANDLE single = CreateMutexW(NULL, TRUE, L"Local\\LightningRodAgent");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_log.open(exeDirectory(), L"LightningRod");
    if (!netInit()) { logLine("WSAStartup failed"); return 1; }

    Agent agent;
    if (!agent.init()) return 1;
    agent.run();
    netShutdown();
    return 0;
}
