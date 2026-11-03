#pragma once

#include "Protocol.h"

#include <string>
#include <vector>

namespace tlr {

// One node as Thunderhead knows it: the user-edited fields are saved in
// nodes.txt keyed by MAC; the live fields come from the agent's HELLO and REPORT.
struct NodeRecord {
    // persisted
    std::string mac;
    std::string name;
    std::string group;
    bool wol, boot, autoLogin, autoRod, autoLightning;
    std::string comment;
    // live
    bool online;
    int conn;                        // server connection id, -1 when offline
    std::string ip;
    Message hello;                   // static fields
    Message report;                  // latest REPORT
    unsigned long long lastReportMs; // tickMs() of the latest REPORT
    unsigned long long onlineSinceMs;

    NodeRecord()
        : wol(false), boot(false), autoLogin(false), autoRod(false), autoLightning(false),
          online(false), conn(-1), lastReportMs(0), onlineSinceMs(0) {}
};

struct Settings {
    int intervalSec;
    int tcpPort;
    Settings() : intervalSec(kDefaultReportInterval), tcpPort(kDefaultTcpPort) {}
};

class NodeStore {
public:
    explicit NodeStore(const std::wstring& dir);
    void load();
    void save();                      // nodes.txt
    void saveSettings();              // settings.txt

    NodeRecord* find(const std::string& mac);
    NodeRecord* findByConn(int conn);
    NodeRecord& add(const std::string& mac);
    void remove(const std::string& mac);
    std::vector<NodeRecord*>& nodes() { return nodes_; }

    Settings settings;
private:
    std::wstring dir_;
    std::vector<NodeRecord*> nodes_;
};

} // namespace tlr
