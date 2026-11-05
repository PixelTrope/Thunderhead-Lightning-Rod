#include "Lightning.h"

#include "Net.h"
#include "StrUtil.h"

#include <tlhelp32.h>
#include <cstring>
#include <vector>

namespace tlr {

const wchar_t* const kLightningPath = L"C:\\Program Files\\DAZ 3D\\BryceLightning7\\Lightning.exe";

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

bool startLightning(std::string& error)
{
    error.clear();
    if (findLightningPid() != 0) { error = "already running"; return false; }
    if (GetFileAttributesW(kLightningPath) == INVALID_FILE_ATTRIBUTES) {
        error = "not installed at " + narrow(kLightningPath);
        return false;
    }
    std::wstring dir(kLightningPath);
    dir = dir.substr(0, dir.rfind(L'\\'));
    std::wstring cmd = std::wstring(L"\"") + kLightningPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(kLightningPath, &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
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
