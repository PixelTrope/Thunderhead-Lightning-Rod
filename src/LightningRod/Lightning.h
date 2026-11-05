#pragma once

#include <windows.h>
#include <string>

namespace tlr {

// Fixed location of the version 1 client; see docs/PROTOCOL.md.
extern const wchar_t* const kLightningPath;
// Where Lightning is installed on this node: Lightning is a 32-bit program, so
// it lives under Program Files (x86) on 64-bit Windows and under Program Files
// on 32-bit Windows. Falls back to kLightningPath when neither exists.
std::wstring lightningPath();

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
bool  startLightning(std::string& error);
bool  stopLightning(std::string& error);
// level is RealTime, High, AboveNormal, Normal, BelowNormal or Low (case does not
// matter); applied receives the class Windows reports afterwards.
bool  setLightningPriority(const std::string& level, std::string& applied, std::string& error);

} // namespace tlr
