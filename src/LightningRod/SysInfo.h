#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace tlr {

// Gathered once at start; sent in HELLO.
struct StaticInfo {
    std::string mac;        // XX-XX-XX-XX-XX-XX of the primary adapter
    std::string ip;         // dotted address of that adapter
    std::string name;       // computer name
    std::string os;         // product name, service pack, version numbers
    std::string cpu;        // processor name string
    int cpuMhz;
    int cores;              // logical processors
    int memMb;              // installed, as seen by the OS
    std::string board;      // baseboard manufacturer and product
    std::string chipset;    // host bridge description
    std::string gpu;        // display adapter description
    int vramMb;
    StaticInfo() : cpuMhz(0), cores(0), memMb(0), vramMb(0) {}
};

// Gathered every report.
struct DynamicInfo {
    int cpuPct;
    int memFreeMb;
    int diskTotalMb;
    int diskFreeMb;
    long long uptimeS;
    DynamicInfo() : cpuPct(0), memFreeMb(0), diskTotalMb(0), diskFreeMb(0), uptimeS(0) {}
};

// Whole-machine CPU use between two samples.
class CpuTracker {
public:
    CpuTracker() : have_(false) {}
    int sample();           // percent since last call; 0 on the first call
private:
    bool have_;
    ULONGLONG idle_, kernel_, user_;
};

StaticInfo  collectStatic();
DynamicInfo collectDynamic(CpuTracker& cpu);

// Directed broadcast address (network order) of every adapter with an address.
std::vector<unsigned long> broadcastAddresses();

// Registry helpers shared with Lightning.cpp.
std::string regString(HKEY root, const wchar_t* path, const wchar_t* value);
DWORD       regDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD def);

} // namespace tlr
