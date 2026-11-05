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
    bool autoShutdown;               // Auto Shutdown column; stored only, the shutdown daemon is not designed yet
    std::string comment;
    // live
    bool online;
    int conn;                        // server connection id, -1 when offline
    std::string ip;
    Message hello;                   // static fields
    Message report;                  // latest REPORT
    unsigned long long lastReportMs; // tickMs() of the latest REPORT
    unsigned long long onlineSinceMs;
    bool updating;                   // UPDATE sent, waiting for the new agent to connect
    unsigned long long updateStartMs;
    bool rebootAfterRename;          // send REBOOT when the RENAME is acknowledged

    NodeRecord()
        : wol(false), boot(false), autoLogin(false), autoRod(false), autoLightning(false), autoShutdown(false),
          online(false), conn(-1), lastReportMs(0), onlineSinceMs(0), updating(false), updateStartMs(0),
          rebootAfterRename(false) {}
};

struct Settings {
    int intervalSec;
    int tcpPort;
    std::string agentFile;    // LightningRod.exe to push; empty = beside Thunderhead.exe
    std::string shareName;    // share on each node that maps to the agent's folder
    Settings() : intervalSec(kDefaultReportInterval), tcpPort(kDefaultTcpPort), shareName("LightningRod") {}
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
