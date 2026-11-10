#include "Lightning.h"

#include "Installed.h"
#include "Net.h"
#include "StrUtil.h"

#include <tlhelp32.h>
#include <cstring>
#include <vector>

namespace tlr {

// Where each generation's installer leaves its traces. Generation 3 is
// verified on Bryce 7.1 / Lightning 7.1.0.109 installs: the product key
// HKLM\SOFTWARE\DAZ 3D\Bryce Lightning 7.0 (under SOFTWARE\WOW6432Node on
// 64-bit Windows) with Location and Version, the Add/Remove Programs entry
// "Bryce Lightning 7.0 <version>" with InstallLocation and DisplayVersion, the
// Start menu shortcut DAZ 3D\Bryce Lightning\Bryce Lightning 7.0.lnk, and the
// default folder DAZ 3D\BryceLightning7. The entries of generations 2 and 1
// are not yet known; their rules are empty and findLightning says so.
struct GenRule {
    int gen;
    const char* label;
    const wchar_t* productKey;       // HKLM key with Location and Version
    const wchar_t* uninstallPrefix;  // start of the Add/Remove Programs key name
    const wchar_t* shortcut;         // text in the Start menu shortcut's name
    const wchar_t* defaultTail;      // path below a Program Files folder
    int major;                       // first number of the file's version
};
const GenRule kRules[] = {
    { 3, "Lightning 3 (Bryce 7 and 7 Pro)", L"SOFTWARE\\DAZ 3D\\Bryce Lightning 7.0", L"Bryce Lightning 7", L"Bryce Lightning 7",
      L"DAZ 3D\\BryceLightning7\\Lightning.exe", 7 },
    { 2, "Lightning 2 (Bryce 6.x)", NULL, NULL, NULL, NULL, 6 },
    { 1, "Lightning 1 (Bryce 5.x)", NULL, NULL, NULL, NULL, 5 },
};
const wchar_t* const kLightningExe = L"Lightning.exe";
const wchar_t* const kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

static const GenRule* ruleFor(int gen)
{
    for (size_t i = 0; i < sizeof(kRules) / sizeof(kRules[0]); ++i) if (kRules[i].gen == gen) return &kRules[i];
    return NULL;
}

const char* lightningLabel(int gen)
{
    const GenRule* r = ruleFor(gen);
    return r ? r->label : "Lightning (unknown generation)";
}

bool findLightning(int gen, LightningInstall& out, std::string& error)
{
    out = LightningInstall();
    out.gen = gen;
    error.clear();
    const GenRule* rule = ruleFor(gen);
    if (!rule) { error = format("there is no Lightning generation %d", gen); return false; }
    if (!rule->productKey) {
        error = std::string(rule->label) + ": the registry entries of its installer are not yet known to this agent";
        return false;
    }
    std::wstring path;
    std::string version, found;
    // 1. The product key, in this process's registry view and in the other one.
    for (int v = 0; v < 2 && path.empty(); ++v) {
        std::wstring p = joinPath(regText(HKEY_LOCAL_MACHINE, rule->productKey, L"Location", kRegViews[v]), kLightningExe);
        if (!fileExists(p)) continue;
        path = p;
        version = narrow(regText(HKEY_LOCAL_MACHINE, rule->productKey, L"Version", kRegViews[v]));
        found = "registry " + regKeyText(HKEY_LOCAL_MACHINE, rule->productKey, kRegViews[v]) + "\\Location";
    }
    // 2. The Add/Remove Programs entry.
    for (int v = 0; v < 2 && path.empty(); ++v) {
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, kUninstallKey, kRegViews[v]);
        for (size_t i = 0; i < subs.size() && path.empty(); ++i) {
            if (!startsWithNoCase(subs[i], rule->uninstallPrefix)) continue;
            std::wstring key = std::wstring(kUninstallKey) + L"\\" + subs[i];
            std::wstring p = joinPath(regText(HKEY_LOCAL_MACHINE, key, L"InstallLocation", kRegViews[v]), kLightningExe);
            if (!fileExists(p)) continue;
            path = p;
            version = narrow(regText(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", kRegViews[v]));
            found = "uninstall entry " + regKeyText(HKEY_LOCAL_MACHINE, key, kRegViews[v]);
        }
    }
    // 3. The Start menu shortcut.  4. The default folder.
    if (path.empty()) path = startMenuTarget(rule->shortcut, kLightningExe, found);
    if (path.empty()) { path = programFilesFile(rule->defaultTail); if (!path.empty()) found = "default folder"; }
    if (path.empty()) {
        error = std::string(rule->label) + " is not installed: no registry entry, Start menu shortcut or default folder names it";
        return false;
    }
    // The file itself must be of the generation asked for: a Bryce 7 Lightning
    // carries 7.x in its version resource.
    std::string fileVersion = fileVersionText(path);
    if (version.empty()) version = fileVersion;
    out.path = path;
    out.version = version;
    out.found = found;
    int major = versionMajor(fileVersion.empty() ? version : fileVersion);
    if (major != 0 && major != rule->major) {
        error = format("%s is version %s, not a %d.x Lightning", narrow(path).c_str(),
                       (fileVersion.empty() ? version : fileVersion).c_str(), rule->major);
        return false;
    }
    return true;
}

// GetExtendedTcpTable arrived with Windows XP SP2; it is resolved at run time so
// the agent loads on every node and the structures are declared here because the
// Windows 7.1A SDK hides them below _WIN32_WINNT 0x0502.
namespace {

struct TcpRowOwnerPid {
    DWORD state, localAddr, localPort, remoteAddr, remotePort, owningPid;
};
struct TcpTableOwnerPid {
    DWORD numEntries;
    TcpRowOwnerPid table[1];
};
const DWORD kTcpTableOwnerPidAll = 5;
const DWORD kTcpStateEstablished = 5;

typedef DWORD (WINAPI *GetExtendedTcpTableFn)(PVOID, PDWORD, BOOL, ULONG, DWORD, ULONG);

GetExtendedTcpTableFn extendedTcpTable()
{
    static GetExtendedTcpTableFn fn = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE lib = LoadLibraryW(L"iphlpapi.dll");
        if (lib) fn = (GetExtendedTcpTableFn)GetProcAddress(lib, "GetExtendedTcpTable");
    }
    return fn;
}

bool ownsEstablishedConnection(DWORD pid)
{
    GetExtendedTcpTableFn fn = extendedTcpTable();
    if (!fn) return false;
    DWORD size = 0;
    fn(NULL, &size, FALSE, AF_INET, kTcpTableOwnerPidAll, 0);
    if (size == 0) return false;
    std::string buf(size + 256, '\0');
    if (fn(&buf[0], &size, FALSE, AF_INET, kTcpTableOwnerPidAll, 0) != NO_ERROR) return false;
    const TcpTableOwnerPid* table = (const TcpTableOwnerPid*)buf.data();
    for (DWORD i = 0; i < table->numEntries; ++i) {
        const TcpRowOwnerPid& r = table->table[i];
        if (r.owningPid == pid && r.state == kTcpStateEstablished) return true;
    }
    return false;
}

ULONGLONG ftToU64(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

} // namespace

DWORD findLightningPid()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Lightning.exe") == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

LightningState LightningMonitor::query()
{
    LightningState s;
    s.pid = findLightningPid();
    s.running = s.pid != 0;
    if (!s.running) { lastPid_ = 0; return s; }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    cores_ = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, s.pid);
    if (h) {
        FILETIME create, exit, kernel, user, now;
        if (GetProcessTimes(h, &create, &exit, &kernel, &user)) {
            GetSystemTimeAsFileTime(&now);
            ULONGLONG wall = ftToU64(now);
            ULONGLONG cpu = ftToU64(kernel) + ftToU64(user);
            if (lastPid_ == s.pid && wall > lastWall_) {
                ULONGLONG elapsed = (wall - lastWall_) * (ULONGLONG)cores_;
                ULONGLONG used = cpu > lastCpu_ ? cpu - lastCpu_ : 0;
                s.cpuPct = (int)((used * 100 + elapsed / 2) / elapsed);
                if (s.cpuPct > 100) s.cpuPct = 100;
            }
            lastPid_ = s.pid;
            lastWall_ = wall;
            lastCpu_ = cpu;
        }
        CloseHandle(h);
    }
    s.rendering = ownsEstablishedConnection(s.pid);
    return s;
}

std::wstring runningLightningPath()
{
    DWORD pid = findLightningPid();
    if (pid == 0) return std::wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return std::wstring();
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    std::wstring out;
    if (Module32FirstW(snap, &me)) out = me.szExePath;   // the first module is the executable
    CloseHandle(snap);
    return out;
}

bool startLightning(int gen, LightningInstall& inst, std::string& error)
{
    if (!findLightning(gen, inst, error)) return false;
    if (findLightningPid() != 0) {
        std::wstring running = runningLightningPath();
        error = "already running" + (running.empty() ? std::string() : " (" + narrow(running) + ")");
        return false;
    }
    return startLightningAt(inst.path, error);
}

bool startLightningAt(const std::wstring& path, std::string& error)
{
    error.clear();
    if (!fileExists(path)) { error = narrow(path) + " does not exist"; return false; }
    std::wstring dir = path.substr(0, path.rfind(L'\\'));
    std::wstring cmd = std::wstring(L"\"") + path + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(path.c_str(), &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
        error = lastErrorText((int)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool stopLightning(std::string& error)
{
    error.clear();
    DWORD pid = findLightningPid();
    if (pid == 0) { error = "not running"; return false; }
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) { error = lastErrorText((int)GetLastError()); return false; }
    bool ok = TerminateProcess(h, 1) != 0;
    if (ok) WaitForSingleObject(h, 3000);
    else error = lastErrorText((int)GetLastError());
    CloseHandle(h);
    return ok;
}

// Priority class names as Thunderhead sends them.
struct PriorityName { const char* name; DWORD cls; };
const PriorityName kPriorities[] = {
    { "RealTime",    REALTIME_PRIORITY_CLASS },
    { "High",        HIGH_PRIORITY_CLASS },
    { "AboveNormal", ABOVE_NORMAL_PRIORITY_CLASS },
    { "Normal",      NORMAL_PRIORITY_CLASS },
    { "BelowNormal", BELOW_NORMAL_PRIORITY_CLASS },
    { "Low",         IDLE_PRIORITY_CLASS },
};
const int kPriorityCount = sizeof(kPriorities) / sizeof(kPriorities[0]);

bool setLightningPriority(const std::string& level, std::string& applied, std::string& error)
{
    error.clear();
    applied.clear();
    DWORD cls = 0;
    for (int i = 0; i < kPriorityCount; ++i)
        if (_stricmp(level.c_str(), kPriorities[i].name) == 0) cls = kPriorities[i].cls;
    if (cls == 0) { error = "unknown priority " + level; return false; }
    DWORD pid = findLightningPid();
    if (pid == 0) { error = "not running"; return false; }
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { error = lastErrorText((int)GetLastError()); return false; }
    bool ok = SetPriorityClass(h, cls) != 0;
    if (!ok) {
        error = lastErrorText((int)GetLastError());
    } else {
        // Windows grants High in place of RealTime when the caller lacks the
        // increase-base-priority privilege, without an error.
        DWORD now = GetPriorityClass(h);
        for (int i = 0; i < kPriorityCount; ++i)
            if (kPriorities[i].cls == now) applied = kPriorities[i].name;
        if (now != cls) { ok = false; error = "Windows applied " + applied + " instead (RealTime needs administrator rights)"; }
    }
    CloseHandle(h);
    return ok;
}

} // namespace tlr
