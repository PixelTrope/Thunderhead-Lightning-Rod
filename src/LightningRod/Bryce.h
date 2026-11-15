#pragma once

#include <windows.h>
#include <string>

namespace tlr {

// The full Bryce 3D on a node, the program a Bryce 3D Network Render Host
// runs, by generation as Thunderhead numbers the Lightning generations:
// 3 = Bryce 7 and 7 Pro, 2 = Bryce 6.x, 1 = Bryce 5.x. Found as Lightning is
// found (see Bryce 3D on the host in docs/PROTOCOL.md): the installer's
// product key under HKLM\SOFTWARE\DAZ 3D named "Bryce <version>" (Bryce 7.1
// on a 7.1.0.109 install, Bryce 7.0 and Bryce 6.3 on the Windows XP export;
// "Bryce 7.0 Content" is not a program), the Add/Remove Programs entry
// "Bryce <version> ...", the Start menu shortcut whose target is Bryce.exe,
// and the generation's default folders under each Program Files folder.
struct BryceInstall {
    int gen;
    std::wstring path;      // the Bryce.exe
    std::string version;    // from the installer's registry entry, else from the file's version resource
    std::string found;      // the source that named the path
    BryceInstall() : gen(0) {}
};

// "Bryce 7 and 7 Pro" and so on.
const char* bryceLabel(int gen);
bool findBryce(int gen, BryceInstall& out, std::string& error);
// Finds and starts the installed Bryce of one generation from its own folder,
// with scene (a full path, may be empty) as its one argument, as a
// double-click on a .br7 would. inst receives what was found even when the
// start fails, as when a Bryce already runs.
bool startBryce(int gen, const std::wstring& scene, BryceInstall& inst, std::string& error);
// Ends the running Bryce.exe, whatever its generation.
bool stopBryce(std::string& error);
DWORD findBrycePid();
std::wstring runningBrycePath();
// The largest visible top-level window of the running Bryce, or NULL.
HWND bryceWindow();

struct BryceState {
    bool running;
    DWORD pid;
    int cpuPct;         // share of the whole machine since the last query
    BryceState() : running(false), pid(0), cpuPct(0) {}
};

class BryceMonitor {
public:
    BryceMonitor() : lastPid_(0), lastWall_(0), lastCpu_(0) {}
    BryceState query();
private:
    DWORD lastPid_;
    ULONGLONG lastWall_;    // 100 ns units
    ULONGLONG lastCpu_;     // kernel + user, 100 ns units
};

} // namespace tlr
