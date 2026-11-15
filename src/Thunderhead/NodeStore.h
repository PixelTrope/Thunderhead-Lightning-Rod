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
    bool wol, boot, autoLogin, autoRod, autoLightning;   // the Info Flags of Client details: stored only, they change no setting or function
    bool renderHost;                 // the sixth flag, Bryce 3D Network Render Host (B3NDHR): Thunderhead's record of the role; the agent's own setting travels in HELLO and REPORT
    std::string bootAlarm;           // Boot on Alarm time of day, "HH:MM" in 24-hour form; empty when never set
    bool autoShutdown;               // Auto Shutdown column; stored only, the shutdown daemon is not designed yet
    std::string comment;
    // live
    bool online;
    int conn;                        // server connection id, -1 when offline
    std::string ip;
    Message hello;                   // static fields
    Message report;                  // latest REPORT
    unsigned long long lastReportMs; // tickMs() of the latest REPORT
    unsigned long long nextReportMs; // tickMs() when its next REPORT is due: the slot the node holds in the interval (0 unknown)
    unsigned long long nudgeDueMs;   // tickMs() when the nudge SET is to be sent (0 none): see report spacing
    unsigned long long arrivalSec;   // clock second (UTC seconds) in which its latest REPORT or HELLO arrived
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
    bool updateViaRelay;             // the running update was staged through an SMB1 relay: the node swaps the file in itself
    std::string relayProbe;          // a relay's answer to RELAY op=probe: "reachable" or the error
    // Authentication and the shared-folder gate (docs/PROTOCOL.md, Authentication)
    bool authenticated;              // its HELLO proved the farm key
    bool keyed;                      // the agent says it holds a farm.key (HELLO keyed=yes; absent from agents before 0.4.0)
    int shareState;                  // the host's view of \node\share: 0 not checked yet, 1 readable, 2 not readable
    std::string shareError;          // why, when shareState is 2
    std::wstring sharePath;          // the UNC path that opened, when shareState is 1
    bool shareChecking;              // a check thread is running for it
    unsigned long long shareNextMs;  // tickMs() when the next check is due (0 at once)
    unsigned long long shareRetryUntilMs;   // while before this, failed checks are repeated every few seconds (after Open Client's Shared Folder)
    bool helloFollowupPending;       // the messages that follow HELLO (audio switch, sound scheme, relay probe) wait for the share check
    unsigned long long lastAnnounceMs;      // tickMs() of its latest ANNOUNCE
    int announcesUnanswered;         // ANNOUNCEs since its last HELLO
    unsigned long long pingFlashUntilMs;    // the row is painted in the ping color until then (0 none)
    int shadeKey;                    // the shade conditions as last painted (see shadeKey in main.cpp); -1 never

    NodeRecord()
        : wol(false), boot(false), autoLogin(false), autoRod(false), autoLightning(false), renderHost(false), autoShutdown(false),
          online(false), conn(-1), lastReportMs(0), nextReportMs(0), nudgeDueMs(0), arrivalSec(0), onlineSinceMs(0), updating(false), updateStaged(false), updatePrompted(false), updateStartMs(0),
          rebootAfterRename(false), pingPending(false), haveClockOffset(false), clockOffsetMs(0), pingSentMs(0), updateViaRelay(false),
          authenticated(false), keyed(false), shareState(0), shareChecking(false), shareNextMs(0), shareRetryUntilMs(0), helloFollowupPending(false),
          lastAnnounceMs(0), announcesUnanswered(0), pingFlashUntilMs(0), shadeKey(-1) {}
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
