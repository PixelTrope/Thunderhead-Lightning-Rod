#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace tlr {

// One adapter with an address, as the network stack of the agent's Settings
// lists it. id is the adapter's GUID string, the stable key the settings use.
struct NetAdapter {
    std::string id;         // {GUID} of the adapter
    std::string desc;       // the adapter's description
    std::string mac;        // XX-XX-XX-XX-XX-XX
    std::string ip, mask;   // dotted
    unsigned long ipN, maskN;   // the same in network order
    bool gateway;           // has a default gateway
    int type;               // 2 Ethernet, 1 wireless, 0 other
    std::string category;   // Windows network profile: Private, Public, Domain; n/a where Windows has none (XP)
    NetAdapter() : ipN(0), maskN(0), gateway(false), type(0) {}
    unsigned long broadcast() const { return ipN | ~maskN; }
    bool sameSubnet(unsigned long addrN) const { return maskN && (addrN & maskN) == (ipN & maskN); }
    int prefix() const;     // bits in the mask
};

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
    std::string lightning2Path;     // the installed Bryce 6.x Lightning.exe, likewise; empty when none
    std::string lightning2Version;
    std::string brycePath[4];       // the installed full Bryce 3D of each generation, by generation (3 = Bryce 7 and 7 Pro, 2 = 6.x, 1 = 5.x); empty when none
    std::string bryceVersion[4];
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

StaticInfo  collectStatic(const std::string& preferredId, std::vector<NetAdapter>& nets);
// Every adapter that holds a working address (not 0.0.0.0, loopback or an
// APIPA 169.254 one), with its Windows network category. COM must be
// initialised on the calling thread for the category; without it, or on XP,
// the category reads n/a.
std::vector<NetAdapter> listAdapters();
// The adapter the agent identifies itself by: the one whose id is preferredId
// when it holds a working address, else the best by type and gateway.
const NetAdapter* pickAdapter(const std::vector<NetAdapter>& nets, const std::string& preferredId);
// Re-reads only mac and ip of s from the adapters, for the start-up wait on a
// node whose NIC has no lease yet; nets receives the current list.
void        collectAddress(StaticInfo& s, const std::string& preferredId, std::vector<NetAdapter>& nets);
DynamicInfo collectDynamic(CpuTracker& cpu);

// Registry helpers shared with Lightning.cpp.
std::string regString(HKEY root, const wchar_t* path, const wchar_t* value);
DWORD       regDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD def);

} // namespace tlr
