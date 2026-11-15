#pragma once

#include "Net.h"
#include "Protocol.h"

#include <map>
#include <string>
#include <vector>

namespace tlr {

// Delivered to the window with PostMessage; the window owns and deletes it.
struct NetEvent {
    enum Type { Announce, Hello, Report, Ack, Pong, Disconnected, Info };
    Type type;
    int conn;               // connection id, -1 for Announce and Info
    std::string ip;
    Message msg;
    std::string text;       // Info text or disconnect reason
    bool authenticated;     // Hello: the HELLO proved the farm key (see Authentication)
};

// One thread: answers ANNOUNCE on UDP, accepts agents on TCP, reads their messages.
class Server {
public:
    Server();
    ~Server();
    // farmKey is the shared secret of docs/PROTOCOL.md, Authentication: every
    // OFFER carries a proof of it over the agent's challenge token, and a HELLO that
    // carries a proof is checked against the challenge token this host put in its OFFER.
    bool start(HWND window, UINT message, unsigned short tcpPort, const std::string& hostName, const std::string& farmKey);
    void stop();
    bool send(int conn, const Message& m);     // from any thread
    void close(int conn);                      // from any thread
    unsigned short tcpPort() const { return tcpPort_; }

private:
    struct Conn {
        int id;
        SOCKET s;
        std::string ip;
        MessageParser parser;
        bool pendingClose;
        bool authenticated;   // a HELLO on this connection proved the key
    };
    static DWORD WINAPI threadMain(LPVOID p);
    void loop();
    void post(NetEvent::Type type, int conn, const std::string& ip, const Message& msg, const std::string& text, bool authenticated = false);
    void dropConn(size_t index, const std::string& reason);
    Conn* findConn(int id);
    void answerAnnounce(const Message& m, const sockaddr_in& from);
    bool verifyHello(Conn* c, const Message& m, std::string& why);

    HWND window_;
    UINT message_;
    unsigned short tcpPort_;
    std::string hostName_;
    std::string farmKey_;
    volatile long stopFlag_;
    SOCKET udp_, listen_;
    HANDLE thread_;
    CRITICAL_SECTION lock_;
    std::vector<Conn*> conns_;
    int nextId_;
    std::map<std::string, unsigned long long> offeredChallenges_;   // challenge token put in an OFFER -> tickMs() when; server thread only
};

} // namespace tlr
