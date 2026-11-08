#include "SysInfo.h"

#include "Net.h"
#include "StrUtil.h"

#include <iphlpapi.h>
#include <cstdio>
#include <cstring>

namespace tlr {

// ---------------------------------------------------------------- registry

std::string regString(HKEY root, const wchar_t* path, const wchar_t* value)
{
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return std::string();
    wchar_t buf[1024];
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    std::string out;
    if (RegQueryValueExW(key, value, NULL, &type, (BYTE*)buf, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ)) {
        buf[size / sizeof(wchar_t)] = L'\0';
        out = narrow(buf);
    }
    RegCloseKey(key);
    return trim(out);
}

DWORD regDword(HKEY root, const wchar_t* path, const wchar_t* value, DWORD def)
{
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return def;
    DWORD data = def, type = 0, size = sizeof(data);
    if (RegQueryValueExW(key, value, NULL, &type, (BYTE*)&data, &size) != ERROR_SUCCESS || type != REG_DWORD) data = def;
    RegCloseKey(key);
    return data;
}

// Reads a REG_QWORD, REG_DWORD or REG_BINARY (4 or 8 bytes) as a number.
static unsigned long long regNumber(HKEY key, const wchar_t* value)
{
    BYTE buf[16];
    DWORD type = 0, size = sizeof(buf);
    if (RegQueryValueExW(key, value, NULL, &type, buf, &size) != ERROR_SUCCESS) return 0;
    if (type == REG_DWORD || (type == REG_BINARY && size == 4)) { DWORD v; memcpy(&v, buf, 4); return v; }
    if (type == REG_QWORD || (type == REG_BINARY && size == 8)) { unsigned long long v; memcpy(&v, buf, 8); return v; }
    return 0;
}

// Reads a REG_BINARY holding UTF-16 text, as some display drivers store it.
static std::string regBinaryText(HKEY key, const wchar_t* value)
{
    wchar_t buf[512];
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    if (RegQueryValueExW(key, value, NULL, &type, (BYTE*)buf, &size) != ERROR_SUCCESS) return std::string();
    if (type != REG_BINARY && type != REG_SZ) return std::string();
    buf[size / sizeof(wchar_t)] = L'\0';
    return trim(narrow(buf));
}

static std::vector<std::wstring> subKeys(HKEY root, const wchar_t* path)
{
    std::vector<std::wstring> out;
    HKEY key;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(key);
    return out;
}

// ---------------------------------------------------------------- network

struct AdapterPick {
    std::string mac, ip;
    std::vector<unsigned long> broadcasts;
};

static AdapterPick pickAdapter()
{
    AdapterPick pick;
    ULONG size = 0;
    if (GetAdaptersInfo(NULL, &size) != ERROR_BUFFER_OVERFLOW || size == 0) return pick;
    std::vector<BYTE> buf(size);
    IP_ADAPTER_INFO* info = (IP_ADAPTER_INFO*)&buf[0];
    if (GetAdaptersInfo(info, &size) != NO_ERROR) return pick;

    int bestScore = -1;
    for (IP_ADAPTER_INFO* a = info; a; a = a->Next) {
        unsigned long ip = inet_addr(a->IpAddressList.IpAddress.String);
        unsigned long mask = inet_addr(a->IpAddressList.IpMask.String);
        if (ip == 0 || ip == INADDR_NONE || a->AddressLength != 6) continue;
        pick.broadcasts.push_back(ip | ~mask);
        int score = 0;
        if (a->Type == MIB_IF_TYPE_ETHERNET) score = 2;
        else if (a->Type == 71 /* IEEE 802.11 */) score = 1;
        if (a->GatewayList.IpAddress.String[0] && strcmp(a->GatewayList.IpAddress.String, "0.0.0.0") != 0) score += 4;
        if (score > bestScore) {
            bestScore = score;
            pick.mac = format("%02X-%02X-%02X-%02X-%02X-%02X", a->Address[0], a->Address[1], a->Address[2],
                              a->Address[3], a->Address[4], a->Address[5]);
            pick.ip = a->IpAddressList.IpAddress.String;
        }
    }
    return pick;
}

std::vector<unsigned long> broadcastAddresses()
{
    return pickAdapter().broadcasts;
}

// ---------------------------------------------------------------- OS and CPU

typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);

static std::string osDescription()
{
    OSVERSIONINFOW v;
    memset(&v, 0, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!fn || fn(&v) != 0) GetVersionExW(&v);

    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string product = regString(HKEY_LOCAL_MACHINE, key, L"ProductName");
    std::string csd = regString(HKEY_LOCAL_MACHINE, key, L"CSDVersion");
    if (product.empty()) product = "Windows";
    std::string out = product;
    if (!csd.empty()) out += " " + csd;
    out += format(" (%lu.%lu.%lu)", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return out;
}

// ---------------------------------------------------------------- SMBIOS baseboard

typedef UINT (WINAPI *GetSystemFirmwareTableFn)(DWORD, DWORD, PVOID, DWORD);

static std::string smbiosString(const BYTE* strings, const BYTE* end, int index)
{
    if (index <= 0) return std::string();
    const BYTE* p = strings;
    for (int i = 1; p < end; ++i) {
        size_t len = strnlen((const char*)p, (size_t)(end - p));
        if (i == index) return trim(std::string((const char*)p, len));
        p += len + 1;
        if (p < end && *p == 0) break;    // double zero ends the string set
    }
    return std::string();
}

static std::string baseboardFromSmbios()
{
    GetSystemFirmwareTableFn fn = (GetSystemFirmwareTableFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemFirmwareTable");
    if (!fn) return std::string();
    const DWORD sig = 'RSMB';
    UINT size = fn(sig, 0, NULL, 0);
    if (size == 0) return std::string();
    std::vector<BYTE> buf(size);
    if (fn(sig, 0, &buf[0], size) != size) return std::string();
    if (size < 8) return std::string();
    const BYTE* p = &buf[8];              // skip RawSMBIOSData header
    const BYTE* end = &buf[0] + size;
    while (p + 4 <= end) {
        BYTE type = p[0], len = p[1];
        if (len < 4) break;
        const BYTE* strings = p + len;
        const BYTE* q = strings;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        const BYTE* next = q + 2;
        if (type == 2 && len >= 6) {
            std::string maker = smbiosString(strings, end, p[4]);
            std::string product = smbiosString(strings, end, p[5]);
            std::string out = maker;
            if (!product.empty()) out += (out.empty() ? "" : " ") + product;
            return out;
        }
        if (type == 127) break;
        p = next;
    }
    return std::string();
}

static std::string baseboard()
{
    std::string out = baseboardFromSmbios();
    if (!out.empty()) return out;
    const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    std::string maker = regString(HKEY_LOCAL_MACHINE, key, L"BaseBoardManufacturer");
    std::string product = regString(HKEY_LOCAL_MACHINE, key, L"BaseBoardProduct");
    out = maker;
    if (!product.empty()) out += (out.empty() ? "" : " ") + product;
    return out;
}

// ---------------------------------------------------------------- chipset (PCI host bridge)

static std::string chipset()
{
    std::string best;
    int bestScore = 0;
    const wchar_t* root = L"SYSTEM\\CurrentControlSet\\Enum\\PCI";
    std::vector<std::wstring> devices = subKeys(HKEY_LOCAL_MACHINE, root);
    for (size_t i = 0; i < devices.size(); ++i) {
        std::wstring devPath = std::wstring(root) + L"\\" + devices[i];
        std::vector<std::wstring> instances = subKeys(HKEY_LOCAL_MACHINE, devPath.c_str());
        for (size_t j = 0; j < instances.size(); ++j) {
            std::wstring instPath = devPath + L"\\" + instances[j];
            std::string cls = toLower(regString(HKEY_LOCAL_MACHINE, instPath.c_str(), L"Class"));
            std::string desc = stripIndirect(regString(HKEY_LOCAL_MACHINE, instPath.c_str(), L"DeviceDesc"));
            if (desc.empty()) continue;
            std::string low = toLower(desc);
            int score = 0;
            if (low.find("host bridge") != std::string::npos) score += 4;
            if (low.find("root complex") != std::string::npos) score += 4;
            if (low.find("dram controller") != std::string::npos) score += 4;
            if (low.find("host") != std::string::npos) score += 1;
            if (cls == "system") score += 1;
            size_t n = instances[j].size();
            if (n >= 3 && instances[j].compare(n - 3, 3, L"&00") == 0) score += 2;   // bus 0 device 0 function 0
            if (score > bestScore) { bestScore = score; best = desc; }
        }
    }
    return best;
}

// ---------------------------------------------------------------- display adapter

static bool virtualAdapter(const std::string& desc)
{
    std::string low = toLower(desc);
    return low.find("remote") != std::string::npos || low.find("teamviewer") != std::string::npos ||
           low.find("mirror") != std::string::npos || low.find("basic display") != std::string::npos ||
           low.find("basic render") != std::string::npos || low.find("virtual") != std::string::npos ||
           low.find("rdp") != std::string::npos || low.find("vnc") != std::string::npos;
}

static void displayAdapter(std::string& gpu, int& vramMb)
{
    gpu.clear();
    vramMb = 0;
    // Prefer the adapter with the most memory; on a node there is one, on a
    // workstation with an integrated and a discrete GPU this picks the discrete one.
    const wchar_t* cls = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}";
    std::vector<std::wstring> subs = subKeys(HKEY_LOCAL_MACHINE, cls);
    for (size_t i = 0; i < subs.size(); ++i) {
        if (subs[i].size() != 4) continue;    // 0000, 0001, ...
        std::wstring path = std::wstring(cls) + L"\\" + subs[i];
        std::string desc = stripIndirect(regString(HKEY_LOCAL_MACHINE, path.c_str(), L"DriverDesc"));
        if (desc.empty() || virtualAdapter(desc)) continue;
        int mb = 0;
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
            unsigned long long bytes = regNumber(key, L"HardwareInformation.qwMemorySize");
            if (bytes == 0) bytes = regNumber(key, L"HardwareInformation.MemorySize");
            mb = (int)(bytes / (1024 * 1024));
            RegCloseKey(key);
        }
        if (gpu.empty() || mb > vramMb) { gpu = desc; vramMb = mb; }
    }
    if (vramMb > 0) return;
    // Windows XP keeps the hardware information under Control\Video.
    const wchar_t* video = L"SYSTEM\\CurrentControlSet\\Control\\Video";
    std::vector<std::wstring> guids = subKeys(HKEY_LOCAL_MACHINE, video);
    for (size_t i = 0; i < guids.size(); ++i) {
        std::wstring path = std::wstring(video) + L"\\" + guids[i] + L"\\0000";
        HKEY key;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        std::string adapter = regBinaryText(key, L"HardwareInformation.AdapterString");
        unsigned long long bytes = regNumber(key, L"HardwareInformation.qwMemorySize");
        if (bytes == 0) bytes = regNumber(key, L"HardwareInformation.MemorySize");
        RegCloseKey(key);
        if (bytes == 0 || virtualAdapter(adapter)) continue;
        int mb = (int)(bytes / (1024 * 1024));
        if (mb > vramMb) {
            vramMb = mb;
            if (gpu.empty() && !adapter.empty()) gpu = adapter;
        }
    }
}

// ---------------------------------------------------------------- collection

void collectAddress(StaticInfo& s)
{
    AdapterPick a = pickAdapter();
    s.mac = a.mac;
    s.ip = a.ip;
}

StaticInfo collectStatic()
{
    StaticInfo s;
    collectAddress(s);

    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &len)) s.name = narrow(name);

    s.os = osDescription();

    const wchar_t* cpuKey = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
    s.cpu = regString(HKEY_LOCAL_MACHINE, cpuKey, L"ProcessorNameString");
    // collapse the runs of spaces Intel puts in the name string
    std::string cpu;
    for (size_t i = 0; i < s.cpu.size(); ++i)
        if (s.cpu[i] != ' ' || cpu.empty() || cpu[cpu.size() - 1] != ' ') cpu += s.cpu[i];
    s.cpu = trim(cpu);
    s.cpuMhz = (int)regDword(HKEY_LOCAL_MACHINE, cpuKey, L"~MHz", 0);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s.cores = (int)si.dwNumberOfProcessors;

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) s.memMb = (int)(ms.ullTotalPhys / (1024 * 1024));

    // GetPhysicallyInstalledSystemMemory reads the firmware's SMBIOS memory
    // tables, so it counts what is fitted rather than what a 32-bit kernel can
    // address. It exists from Vista on and is looked up at run time for XP.
    typedef BOOL (WINAPI *InstalledMemFn)(PULONGLONG);
    InstalledMemFn installedMem = (InstalledMemFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetPhysicallyInstalledSystemMemory");
    ULONGLONG installedKb = 0;
    if (installedMem && installedMem(&installedKb)) s.memInstalledMb = (int)(installedKb / 1024);

    ULARGE_INTEGER freeBytes, totalBytes;
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &totalBytes, NULL)) s.diskTotalMb = (int)(totalBytes.QuadPart / (1024 * 1024));

    s.board = baseboard();
    s.chipset = chipset();
    displayAdapter(s.gpu, s.vramMb);
    return s;
}

static ULONGLONG ftToU64(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

int CpuTracker::sample()
{
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0;
    ULONGLONG i = ftToU64(idle), k = ftToU64(kernel), u = ftToU64(user);
    int pct = 0;
    if (have_) {
        ULONGLONG total = (k - kernel_) + (u - user_);      // kernel time includes idle
        ULONGLONG busy = total - (i - idle_);
        if (total > 0) pct = (int)((busy * 100 + total / 2) / total);
    }
    idle_ = i; kernel_ = k; user_ = u; have_ = true;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

DynamicInfo collectDynamic(CpuTracker& cpu)
{
    DynamicInfo d;
    d.cpuPct = cpu.sample();

    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) d.memFreeMb = (int)(ms.ullAvailPhys / (1024 * 1024));

    ULARGE_INTEGER freeBytes, totalBytes;
    if (GetDiskFreeSpaceExW(L"C:\\", &freeBytes, &totalBytes, NULL))
        d.diskFreeMb = (int)(freeBytes.QuadPart / (1024 * 1024));

    d.uptimeS = (long long)(tickMs() / 1000);
    return d;
}

} // namespace tlr
