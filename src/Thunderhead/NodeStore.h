#pragma once

#include "Protocol.h"

#include <map>
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
    bool updating;                   // update in progress: staging, then UPDATE sent, until the new agent connects
    bool updateStaged;               // LightningRod.new is on the node and UPDATE has been sent
    bool updatePrompted;             // the credential prompt for its share has been shown in this update
    std::wstring updateDir;          // the share path the staging used
    unsigned long long updateStartMs;
    bool rebootAfterRename;          // send REBOOT when the RENAME is acknowledged
    bool pingPending;                // PING sent, no report or PONG yet
    bool haveClockOffset;            // a report with utc_ms has arrived
    long long clockOffsetMs;         // the node's clock minus this host's, at the latest such report
    unsigned long long pingSentMs;   // tickMs() of that PING

    NodeRecord()
        : wol(false), boot(false), autoLogin(false), autoRod(false), autoLightning(false), autoShutdown(false),
          online(false), conn(-1), lastReportMs(0), onlineSinceMs(0), updating(false), updateStaged(false), updatePrompted(false), updateStartMs(0),
          rebootAfterRename(false), pingPending(false), haveClockOffset(false), clockOffsetMs(0), pingSentMs(0) {}
};

struct Settings {
    int intervalSec;
    int tcpPort;
    std::string agentFile;    // LightningRod.exe to push; empty = beside Thunderhead.exe
    std::string shareName;    // share on each node that maps to the agent's folder
    bool darkMode;            // Thunderhead's Dark Mode (Settings)
    bool thAudio;             // Thunderhead Audio switch (Settings): Thunderhead's own event sounds
    bool lrAudio;             // All Lightning Rods Audio switch (Settings): AUDIO unmute or mute to every connecting client
    bool customScheme;        // sound_scheme=custom: the assignments in sounds apply; otherwise every category plays its default file
    std::map<std::string, std::string> sounds;   // category -> file name in the sounds folder (custom scheme); absent = default
    Settings() : intervalSec(kDefaultReportInterval), tcpPort(kDefaultTcpPort), shareName("LightningRod"), darkMode(false),
                 thAudio(true), lrAudio(true), customScheme(false) {}
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
