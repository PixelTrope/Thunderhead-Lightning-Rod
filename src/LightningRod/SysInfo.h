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
    int memMb;              // usable: physical memory the OS can address
    int memInstalledMb;     // fitted, from the firmware's SMBIOS tables; 0 when the firmware gives none
    std::string memType;    // DDR3, DDR4 ... from the SMBIOS memory device records; empty when the firmware gives none
    std::string lightning3Path;     // the installed Bryce 7 Lightning.exe (filled by main from Lightning.cpp); empty when none
    std::string lightning3Version;  // its version, from the installer's registry entry or the file
    int diskTotalMb;        // size of C:
    std::string board;      // baseboard manufacturer and product
    std::string chipset;    // host bridge description
    std::string gpu;        // display adapter description
    int vramMb;
    StaticInfo() : cpuMhz(0), cores(0), memMb(0), memInstalledMb(0), diskTotalMb(0), vramMb(0) {}
};

// Gathered every report.
struct DynamicInfo {
    int cpuPct;
    int memFreeMb;
    int diskFreeMb;
    long long uptimeS;
    DynamicInfo() : cpuPct(0), memFreeMb(0), diskFreeMb(0), uptimeS(0) {}
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
// Re-reads only mac and ip of s from the adapters, for the start-up wait on a
// node whose NIC has no lease yet.
void        collectAddress(StaticInfo& s);
DynamicInfo collectDynamic(CpuTracker& cpu);

// Directed broadcast address (network order) of every adapter with an address.
std::vector<unsigned long> broadcastAddresses();

// Registry helpers shared with Lightning.cpp.
std::string regString(HKEY root, const wchar_t* path, const wchar_t* value);
DWORD       regDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD def);

} // namespace tlr
