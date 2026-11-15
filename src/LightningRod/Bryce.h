#pragma once

#include <windows.h>
#include <string>

namespace tlr {

// The full Bryce 3D on a node: the program the Bryce 3D Network Render Host
// runs. Found as Lightning is found (see Bryce 3D on the host in
// docs/PROTOCOL.md): the installer's product key under HKLM\SOFTWARE\DAZ 3D
// named "Bryce <version>" (Bryce 7.1 on a 7.1.0.109 install, Bryce 7.0 and
// Bryce 6.3 on the Windows XP export; the "Bryce 7.0 Content" key is not a
// program), the Add/Remove Programs entry "Bryce <version> ...", the Start
// menu shortcut whose target is Bryce.exe, and the default folders
// DAZ 3D\Bryce7 and DAZ 3D\Bryce under each Program Files folder.
struct BryceInstall {
    std::wstring path;      // the Bryce.exe
    std::string version;    // from the installer's registry entry, else from the file's version resource
    std::string found;      // the source that named the path
};

bool findBryce(BryceInstall& out, std::string& error);
// Starts the installed Bryce from its own folder, with scene (a full path, may
// be empty) as its one argument, as a double-click on a .br7 would. inst
// receives what was found even when the start fails, as when Bryce already runs.
bool startBryce(const std::wstring& scene, BryceInstall& inst, std::string& error);
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
