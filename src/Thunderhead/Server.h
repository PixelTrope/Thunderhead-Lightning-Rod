#pragma once

#include "Net.h"
#include "Protocol.h"

#include <string>
#include <vector>

namespace tlr {

// Delivered to the window with PostMessage; the window owns and deletes it.
struct NetEvent {
    enum Type { Hello, Report, Ack, Pong, Disconnected, Info };
    Type type;
    int conn;               // connection id, -1 for Info
    std::string ip;
    Message msg;
    std::string text;       // Info text or disconnect reason
};

// One thread: answers ANNOUNCE on UDP, accepts agents on TCP, reads their messages.
class Server {
public:
    Server();
    ~Server();
    bool start(HWND window, UINT message, unsigned short tcpPort, const std::string& hostName, int intervalSec);
    void stop();
    bool send(int conn, const Message& m);     // from any thread
    void close(int conn);                      // from any thread
    void setInterval(int sec);                 // value sent in SET after HELLO
    unsigned short tcpPort() const { return tcpPort_; }

private:
    struct Conn {
        int id;
        SOCKET s;
        std::string ip;
        MessageParser parser;
        bool pendingClose;
    };
    static DWORD WINAPI threadMain(LPVOID p);
    void loop();
    void post(NetEvent::Type type, int conn, const std::string& ip, const Message& msg, const std::string& text);
    void dropConn(size_t index, const std::string& reason);
    Conn* findConn(int id);

    HWND window_;
    UINT message_;
    unsigned short tcpPort_;
    std::string hostName_;
    volatile long interval_;
    volatile long stopFlag_;
    SOCKET udp_, listen_;
    HANDLE thread_;
    CRITICAL_SECTION lock_;
    std::vector<Conn*> conns_;
    int nextId_;
};

} // namespace tlr
