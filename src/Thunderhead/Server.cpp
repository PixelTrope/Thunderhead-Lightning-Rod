#include "Server.h"
#include "StrUtil.h"

#include <cstring>

namespace tlr {

Server::Server()
    : window_(NULL), message_(0), tcpPort_(0), interval_(kDefaultReportInterval), stopFlag_(0),
      udp_(INVALID_SOCKET), listen_(INVALID_SOCKET), thread_(NULL), nextId_(1)
{
    InitializeCriticalSection(&lock_);
}

Server::~Server()
{
    stop();
    DeleteCriticalSection(&lock_);
}

bool Server::start(HWND window, UINT message, unsigned short tcpPort, const std::string& hostName, int intervalSec)
{
    window_ = window;
    message_ = message;
    tcpPort_ = tcpPort;
    hostName_ = hostName;
    interval_ = intervalSec;

    udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (udp_ == INVALID_SOCKET || listen_ == INVALID_SOCKET) return false;
    BOOL yes = TRUE;
    setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));

    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(kDiscoveryPort);
    if (bind(udp_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        post(NetEvent::Info, -1, "", Message(), format("UDP port %d is in use (%s)", (int)kDiscoveryPort, lastErrorText(WSAGetLastError()).c_str()));
        return false;
    }
    a.sin_port = htons(tcpPort_);
    if (bind(listen_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR || listen(listen_, 16) == SOCKET_ERROR) {
        post(NetEvent::Info, -1, "", Message(), format("TCP port %d is in use (%s)", (int)tcpPort_, lastErrorText(WSAGetLastError()).c_str()));
        return false;
    }
    stopFlag_ = 0;
    thread_ = CreateThread(NULL, 0, threadMain, this, 0, NULL);
    return thread_ != NULL;
}

void Server::stop()
{
    if (!thread_) return;
    InterlockedExchange(&stopFlag_, 1);
    WaitForSingleObject(thread_, 5000);
    CloseHandle(thread_);
    thread_ = NULL;
    EnterCriticalSection(&lock_);
    for (size_t i = 0; i < conns_.size(); ++i) { closesocket(conns_[i]->s); delete conns_[i]; }
    conns_.clear();
    LeaveCriticalSection(&lock_);
    if (udp_ != INVALID_SOCKET) { closesocket(udp_); udp_ = INVALID_SOCKET; }
    if (listen_ != INVALID_SOCKET) { closesocket(listen_); listen_ = INVALID_SOCKET; }
}

void Server::setInterval(int sec)
{
    InterlockedExchange(&interval_, sec);
}

Server::Conn* Server::findConn(int id)
{
    for (size_t i = 0; i < conns_.size(); ++i)
        if (conns_[i]->id == id) return conns_[i];
    return NULL;
}

bool Server::send(int conn, const Message& m)
{
    std::string wire = m.encode();
    EnterCriticalSection(&lock_);
    Conn* c = findConn(conn);
    bool ok = c && !c->pendingClose && sendAll(c->s, wire);
    if (c && !ok) c->pendingClose = true;
    LeaveCriticalSection(&lock_);
    return ok;
}

void Server::close(int conn)
{
    EnterCriticalSection(&lock_);
    Conn* c = findConn(conn);
    if (c) c->pendingClose = true;
    LeaveCriticalSection(&lock_);
}

void Server::post(NetEvent::Type type, int conn, const std::string& ip, const Message& msg, const std::string& text)
{
    NetEvent* e = new NetEvent();
    e->type = type;
    e->conn = conn;
    e->ip = ip;
    e->msg = msg;
    e->text = text;
    if (!PostMessageW(window_, message_, 0, (LPARAM)e)) delete e;
}

void Server::dropConn(size_t index, const std::string& reason)
{
    Conn* c = conns_[index];
    closesocket(c->s);
    post(NetEvent::Disconnected, c->id, c->ip, Message(), reason);
    conns_.erase(conns_.begin() + index);
    delete c;
}

DWORD WINAPI Server::threadMain(LPVOID p)
{
    ((Server*)p)->loop();
    return 0;
}

void Server::loop()
{
    post(NetEvent::Info, -1, "", Message(), format("listening on UDP %d and TCP %d", (int)kDiscoveryPort, (int)tcpPort_));
    while (!stopFlag_) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(udp_, &rd);
        FD_SET(listen_, &rd);
        EnterCriticalSection(&lock_);
        for (size_t i = 0; i < conns_.size();) {
            if (conns_[i]->pendingClose) { dropConn(i, "closed by Thunderhead"); continue; }
            FD_SET(conns_[i]->s, &rd);
            ++i;
        }
        LeaveCriticalSection(&lock_);

        timeval tv; tv.tv_sec = 0; tv.tv_usec = 500000;
        int r = select(0, &rd, NULL, NULL, &tv);
        if (r < 0) { Sleep(100); continue; }
        if (r == 0) continue;

        if (FD_ISSET(udp_, &rd)) {
            char buf[4096];
            sockaddr_in from;
            int fromLen = sizeof(from);
            int n = recvfrom(udp_, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
            if (n > 0) {
                Message m;
                if (parseMessage(std::string(buf, (size_t)n), m) && m.verb == "ANNOUNCE") {
                    Message offer("OFFER");
                    offer.set("host", hostName_);
                    offer.set("port", (long long)tcpPort_);
                    std::string wire = offer.encode();
                    sendto(udp_, wire.data(), (int)wire.size(), 0, (sockaddr*)&from, sizeof(from));
                }
            }
        }

        if (FD_ISSET(listen_, &rd)) {
            sockaddr_in from;
            int fromLen = sizeof(from);
            SOCKET s = accept(listen_, (sockaddr*)&from, &fromLen);
            if (s != INVALID_SOCKET) {
                BOOL noDelay = TRUE;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
                Conn* c = new Conn();
                c->s = s;
                c->ip = ipText(from);
                c->pendingClose = false;
                EnterCriticalSection(&lock_);
                c->id = nextId_++;
                conns_.push_back(c);
                LeaveCriticalSection(&lock_);
            }
        }

        EnterCriticalSection(&lock_);
        for (size_t i = 0; i < conns_.size();) {
            Conn* c = conns_[i];
            if (!FD_ISSET(c->s, &rd)) { ++i; continue; }
            char buf[8192];
            int n = recv(c->s, buf, sizeof(buf), 0);
            if (n == 0) { dropConn(i, "closed by agent"); continue; }
            if (n < 0) { dropConn(i, lastErrorText(WSAGetLastError())); continue; }
            c->parser.feed(buf, (size_t)n);
            if (c->parser.overflow()) { dropConn(i, "oversized message"); continue; }
            Message m;
            bool dropped = false;
            while (c->parser.next(m)) {
                if (m.verb == "HELLO") {
                    Message set("SET");
                    set.set("interval", (long long)interval_);
                    if (!sendAll(c->s, set.encode())) { dropConn(i, "send failed"); dropped = true; break; }
                    post(NetEvent::Hello, c->id, c->ip, m, "");
                } else if (m.verb == "REPORT") {
                    post(NetEvent::Report, c->id, c->ip, m, "");
                } else if (m.verb == "ACK") {
                    post(NetEvent::Ack, c->id, c->ip, m, "");
                } else if (m.verb == "PONG") {
                    post(NetEvent::Pong, c->id, c->ip, m, "");
                } else {
                    post(NetEvent::Info, c->id, c->ip, m, "unknown verb " + m.verb);
                }
            }
            if (!dropped) ++i;
        }
        LeaveCriticalSection(&lock_);
    }
}

} // namespace tlr
