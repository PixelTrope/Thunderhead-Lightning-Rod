#pragma once

#include <windows.h>
#include <string>

namespace tlr {

// One installed Lightning, by generation as Thunderhead numbers them:
// 3 = Bryce 7 and 7 Pro, 2 = Bryce 6.x, 1 = Bryce 5.x.
struct LightningInstall {
    int gen;
    std::wstring path;      // the Lightning.exe
    std::string version;    // from the installer's registry entry, else from the file's version resource
    std::string found;      // the source that named the path
    LightningInstall() : gen(0) {}
};

// "Lightning 3 (Bryce 7 and 7 Pro)" and so on.
const char* lightningLabel(int gen);
// Finds the installed Lightning of one generation through the traces its
// installer leaves; see Lightning lookup in docs/PROTOCOL.md. False with
// error set when the generation's rules are not yet known, when nothing
// names an existing Lightning.exe, or when the file found is of another
// generation.
bool findLightning(int gen, LightningInstall& out, std::string& error);
// Finds and starts the installed Lightning of one generation. inst receives
// what was found even when the start fails, as when Lightning already runs.
bool startLightning(int gen, LightningInstall& inst, std::string& error);
// Starts the given Lightning.exe from its own folder.
bool startLightningAt(const std::wstring& path, std::string& error);
// Full path of the running Lightning.exe; empty when none runs.
std::wstring runningLightningPath();

struct LightningState {
    bool running;
    DWORD pid;
    int cpuPct;         // share of the whole machine since the last query
    bool rendering;     // owns an established TCP connection
    LightningState() : running(false), pid(0), cpuPct(0), rendering(false) {}
};

class LightningMonitor {
public:
    LightningMonitor() : lastPid_(0), lastWall_(0), lastCpu_(0), cores_(1) {}
    LightningState query();
private:
    DWORD lastPid_;
    ULONGLONG lastWall_;    // 100 ns units
    ULONGLONG lastCpu_;     // kernel + user, 100 ns units
    int cores_;
};

DWORD findLightningPid();
bool  stopLightning(std::string& error);
// level is RealTime, High, AboveNormal, Normal, BelowNormal or Low (case does not
// matter); applied receives the class Windows reports afterwards.
bool  setLightningPriority(const std::string& level, std::string& applied, std::string& error);

} // namespace tlr
