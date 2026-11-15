#include "Server.h"
#include "Auth.h"
#include "StrUtil.h"

#include <cstring>

namespace tlr {

// An OFFER challenge token is good for this long; an agent connects within milliseconds
// of the OFFER, and a renewed HELLO on an authenticated connection is not
// checked again, so the window only has to outlast a slow connect.
const unsigned long long kChallengeLifeMs = 10 * 60 * 1000;

Server::Server()
    : window_(NULL), message_(0), tcpPort_(0), stopFlag_(0),
      udp_(INVALID_SOCKET), listen_(INVALID_SOCKET), thread_(NULL), nextId_(1)
{
    InitializeCriticalSection(&lock_);
}

Server::~Server()
{
    stop();
    DeleteCriticalSection(&lock_);
}

bool Server::start(HWND window, UINT message, unsigned short tcpPort, const std::string& hostName, const std::string& farmKey)
{
    window_ = window;
    message_ = message;
    tcpPort_ = tcpPort;
    hostName_ = hostName;
    farmKey_ = farmKey;

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

void Server::post(NetEvent::Type type, int conn, const std::string& ip, const Message& msg, const std::string& text, bool authenticated)
{
    NetEvent* e = new NetEvent();
    e->type = type;
    e->conn = conn;
    e->ip = ip;
    e->msg = msg;
    e->text = text;
    e->authenticated = authenticated;
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

// OFFER to an ANNOUNCE: host and port as before, plus challenge token=, a fresh value
// the agent has to echo in its HELLO proof, and auth=, this host's proof of the
// farm key over the challenge token the agent put in its ANNOUNCE (agents before 0.4.0
// send none and ignore both fields).
void Server::answerAnnounce(const Message& m, const sockaddr_in& from)
{
    unsigned long long now = tickMs();
    for (std::map<std::string, unsigned long long>::iterator it = offeredChallenges_.begin(); it != offeredChallenges_.end();) {
        if (now - it->second > kChallengeLifeMs) offeredChallenges_.erase(it++); else ++it;
    }
    Message offer("OFFER");
    offer.set("host", hostName_);
    offer.set("port", (long long)tcpPort_);
    std::string challenge = randomHex(kChallengeBytes);
    if (!challenge.empty()) {
        offeredChallenges_[challenge] = now;
        offer.set("challenge", challenge);
        offer.set("nonce", challenge);   // the field's name in 0.4.0 agents; dropped once none remain
        std::string theirs = m.has("challenge") ? m.get("challenge") : m.get("nonce");
        if (!farmKey_.empty() && !theirs.empty()) offer.set("auth", offerAuth(farmKey_, theirs, hostName_, format("%d", (int)tcpPort_)));
    }
    std::string wire = offer.encode();
    sendto(udp_, wire.data(), (int)wire.size(), 0, (sockaddr*)&from, sizeof(from));
    post(NetEvent::Announce, -1, ipText(from), m, "");
}

// A HELLO that carries challenge token= and auth= is checked: the challenge token must be one this
// host put in an OFFER within kChallengeLifeMs, and auth must be the proof of
// the farm key over it and the agent's MAC. A HELLO without the fields comes
// from an agent before 0.4.0 or from one that holds no farm.key yet; it is
// accepted as unauthenticated and the window decides what it may do. A
// renewed HELLO on a connection already authenticated is not checked again.
bool Server::verifyHello(Conn* c, const Message& m, std::string& why)
{
    if (c->authenticated) return true;
    if (!m.has("auth")) return true;   // unauthenticated, not refused
    std::string challenge = m.has("challenge") ? m.get("challenge") : m.get("nonce");   // nonce= from 0.4.0 agents
    if (farmKey_.empty()) { why = "this host has no farm key"; return false; }
    if (offeredChallenges_.find(challenge) == offeredChallenges_.end()) { why = "its challenge token is not one this host offered"; return false; }
    if (!equalsConstantTime(m.get("auth"), helloAuth(farmKey_, challenge, m.get("mac")))) { why = "its farm.key is not this host's"; return false; }
    c->authenticated = true;
    return true;
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
                if (parseMessage(std::string(buf, (size_t)n), m) && m.verb == "ANNOUNCE") answerAnnounce(m, from);
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
                c->authenticated = false;
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
            while (!dropped && c->parser.next(m)) {
                if (m.verb == "HELLO") {
                    std::string why;
                    if (!verifyHello(c, m, why)) {
                        // A refused HELLO ends the connection; the agent goes back to
                        // announcing, and its OFFERs keep failing its own check as well
                        // until the two farm.key files agree.
                        post(NetEvent::Info, -1, c->ip, m, format("HELLO from %s (%s, agent %s) refused: %s", m.get("name").c_str(), c->ip.c_str(),
                                                                   m.get("agent").c_str(), why.c_str()));
                        dropConn(i, "authentication failed");
                        dropped = true;
                        break;
                    }
                    // The SET that answers HELLO is sent by the window thread, which
                    // knows the report slots of the other nodes (see sendSchedule).
                    post(NetEvent::Hello, c->id, c->ip, m, "", c->authenticated);
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
