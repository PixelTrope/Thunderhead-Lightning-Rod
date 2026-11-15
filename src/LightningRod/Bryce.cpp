#include "Bryce.h"

#include "Installed.h"
#include "Net.h"
#include "StrUtil.h"

#include <tlhelp32.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <vector>

namespace tlr {

namespace {

const wchar_t* const kBryceExe = L"Bryce.exe";
const wchar_t* const kDazKey = L"SOFTWARE\\DAZ 3D";
const wchar_t* const kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

// "Bryce 7.1", "Bryce 7.0", "Bryce 6.3": the product keys and Add/Remove
// Programs entries of the program itself. "Bryce 7.0 Content" is the preset
// library and "Bryce Lightning 7.0" the render client; neither names Bryce.exe.
bool isBryceProgramKey(const std::wstring& name)
{
    if (!startsWithNoCase(name, L"Bryce ")) return false;
    if (name.size() < 7 || !iswdigit(name[6])) return false;
    std::wstring lower = name;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = (wchar_t)towlower(lower[i]);
    return lower.find(L"content") == std::wstring::npos && lower.find(L"lightning") == std::wstring::npos;
}

int compareVersionText(const std::string& a, const std::string& b)
{
    const char* pa = a.c_str();
    const char* pb = b.c_str();
    for (;;) {
        long na = strtol(pa, (char**)&pa, 10), nb = strtol(pb, (char**)&pb, 10);
        if (na != nb) return na < nb ? -1 : 1;
        if (*pa != '.' || *pb != '.') return 0;
        ++pa;
        ++pb;
    }
}

struct KeyVersion { std::wstring key; std::string version; };
bool newerFirst(const KeyVersion& a, const KeyVersion& b) { return compareVersionText(a.version, b.version) > 0; }

bool consider(BryceInstall& out, const std::wstring& path, const std::string& version, const std::string& found)
{
    if (!fileExists(path)) return false;
    out.path = path;
    out.version = version.empty() ? fileVersionText(path) : version;
    out.found = found;
    return true;
}

ULONGLONG ftToU64(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

struct WindowSearch { DWORD pid; HWND best; long area; };

BOOL CALLBACK findWindow(HWND w, LPARAM lp)
{
    WindowSearch* s = (WindowSearch*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != s->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER) != NULL) return TRUE;
    RECT rc;
    if (!GetWindowRect(w, &rc)) return TRUE;
    long area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > s->area) { s->area = area; s->best = w; }
    return TRUE;
}

} // namespace

bool findBryce(BryceInstall& out, std::string& error)
{
    out = BryceInstall();
    error.clear();
    // 1. The product keys, highest version first, in both registry views.
    for (int v = 0; v < 2; ++v) {
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, kDazKey, kRegViews[v]);
        std::vector<KeyVersion> keys;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (!isBryceProgramKey(subs[i])) continue;
            KeyVersion kv;
            kv.key = std::wstring(kDazKey) + L"\\" + subs[i];
            kv.version = narrow(regText(HKEY_LOCAL_MACHINE, kv.key, L"Version", kRegViews[v]));
            keys.push_back(kv);
        }
        std::sort(keys.begin(), keys.end(), newerFirst);
        for (size_t i = 0; i < keys.size(); ++i)
            if (consider(out, joinPath(regText(HKEY_LOCAL_MACHINE, keys[i].key, L"Location", kRegViews[v]), kBryceExe), keys[i].version,
                         "registry " + regKeyText(HKEY_LOCAL_MACHINE, keys[i].key, kRegViews[v]) + "\\Location")) return true;
    }
    // 2. The Add/Remove Programs entries.
    for (int v = 0; v < 2; ++v) {
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, kUninstallKey, kRegViews[v]);
        for (size_t i = 0; i < subs.size(); ++i) {
            if (!isBryceProgramKey(subs[i])) continue;
            std::wstring key = std::wstring(kUninstallKey) + L"\\" + subs[i];
            if (consider(out, joinPath(regText(HKEY_LOCAL_MACHINE, key, L"InstallLocation", kRegViews[v]), kBryceExe),
                         narrow(regText(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", kRegViews[v])),
                         "uninstall entry " + regKeyText(HKEY_LOCAL_MACHINE, key, kRegViews[v]))) return true;
        }
    }
    // 3. The Start menu shortcuts whose target is Bryce.exe (the Lightning and
    //    documentation shortcuts under the same folder point elsewhere).
    std::vector<ShortcutHit> hits = startMenuTargets(L"Bryce", kBryceExe);
    for (size_t i = 0; i < hits.size(); ++i) if (consider(out, hits[i].path, std::string(), hits[i].how)) return true;
    // 4. The default folders.
    if (consider(out, programFilesFile(L"DAZ 3D\\Bryce7\\Bryce.exe"), std::string(), "default folder")) return true;
    if (consider(out, programFilesFile(L"DAZ 3D\\Bryce\\Bryce.exe"), std::string(), "default folder")) return true;
    error = "Bryce 3D is not installed: no registry entry, Start menu shortcut or default folder names a Bryce.exe";
    return false;
}

DWORD findBrycePid()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, kBryceExe) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

std::wstring runningBrycePath()
{
    DWORD pid = findBrycePid();
    if (pid == 0) return std::wstring();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return std::wstring();
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    std::wstring out;
    if (Module32FirstW(snap, &me)) out = me.szExePath;
    CloseHandle(snap);
    return out;
}

HWND bryceWindow()
{
    WindowSearch s;
    s.pid = findBrycePid();
    s.best = NULL;
    s.area = 0;
    if (s.pid == 0) return NULL;
    EnumWindows(findWindow, (LPARAM)&s);
    return s.best;
}

bool startBryce(const std::wstring& scene, BryceInstall& inst, std::string& error)
{
    if (!findBryce(inst, error)) return false;
    if (findBrycePid() != 0) {
        std::wstring running = runningBrycePath();
        error = "already running" + (running.empty() ? std::string() : " (" + narrow(running) + ")");
        return false;
    }
    std::wstring dir = inst.path.substr(0, inst.path.rfind(L'\\'));
    std::wstring cmd = L"\"" + inst.path + L"\"";
    if (!scene.empty()) cmd += L" \"" + scene + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(inst.path.c_str(), &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
        error = lastErrorText((int)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool stopBryce(std::string& error)
{
    error.clear();
    DWORD pid = findBrycePid();
    if (pid == 0) { error = "not running"; return false; }
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) { error = lastErrorText((int)GetLastError()); return false; }
    bool ok = TerminateProcess(h, 1) != 0;
    if (ok) WaitForSingleObject(h, 5000);
    else error = lastErrorText((int)GetLastError());
    CloseHandle(h);
    return ok;
}

BryceState BryceMonitor::query()
{
    BryceState s;
    s.pid = findBrycePid();
    s.running = s.pid != 0;
    if (!s.running) { lastPid_ = 0; return s; }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, s.pid);
    if (h) {
        FILETIME create, exit, kernel, user, now;
        if (GetProcessTimes(h, &create, &exit, &kernel, &user)) {
            GetSystemTimeAsFileTime(&now);
            ULONGLONG wall = ftToU64(now);
            ULONGLONG cpu = ftToU64(kernel) + ftToU64(user);
            if (lastPid_ == s.pid && wall > lastWall_) {
                ULONGLONG elapsed = (wall - lastWall_) * (ULONGLONG)cores;
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
    return s;
}

} // namespace tlr
