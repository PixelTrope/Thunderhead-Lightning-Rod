#include "Bryce.h"

#include "Installed.h"
#include "Net.h"
#include "StrUtil.h"

#include <tlhelp32.h>
#include <shlobj.h>
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

// Where each generation's installer leaves its traces. Generation 3 is
// verified on a Bryce 7.1 (7.1.0.109) install: product key "Bryce 7.1",
// Add/Remove Programs key "Bryce 7.1 7.1.0.109", shortcut DAZ 3D\Bryce7\
// Bryce 7.1.lnk, folder DAZ 3D\Bryce7. Generation 2 is verified on the
// registry export of a Windows XP node: product key "Bryce 6.3" with Location
// ...\DAZ 3D\Bryce. Generation 1 follows the same pattern by assumption:
// Bryce 5.5 was DAZ's, Bryce 5 Corel's; its keys and folders are not yet
// verified on a node, so the file's own version decides.
struct GenRule {
    int gen;
    const char* label;
    const wchar_t* keyPrefix;        // start of the product key's and the Add/Remove Programs key's name
    const wchar_t* defaults[3];      // paths below a Program Files folder, NULL-terminated
    int major;                       // first number of the generation's versions
};
const GenRule kRules[] = {
    { 3, "Bryce 7 and 7 Pro", L"Bryce 7", { L"DAZ 3D\\Bryce7\\Bryce.exe", L"DAZ 3D\\Bryce 7\\Bryce.exe", NULL }, 7 },
    { 2, "Bryce 6.x",         L"Bryce 6", { L"DAZ 3D\\Bryce\\Bryce.exe", L"DAZ 3D\\Bryce 6\\Bryce.exe", NULL }, 6 },
    { 1, "Bryce 5.x",         L"Bryce 5", { L"DAZ 3D\\Bryce 5.5\\Bryce.exe", L"DAZ\\Bryce 5.5\\Bryce.exe", L"Corel\\Bryce 5\\Bryce.exe" }, 5 },
};
const size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

const GenRule* ruleFor(int gen)
{
    for (size_t i = 0; i < kRuleCount; ++i) if (kRules[i].gen == gen) return &kRules[i];
    return NULL;
}

// "Bryce 7.1", "Bryce 7.0", "Bryce 6.3": the product keys and Add/Remove
// Programs entries of the program itself, by generation. "Bryce 7.0 Content"
// is the preset library and "Bryce Lightning 7.0" the render client; neither
// names Bryce.exe, and the Lightning key does not start with the prefix anyway.
bool isBryceProgramKey(const std::wstring& name, const wchar_t* prefix)
{
    if (!startsWithNoCase(name, prefix)) return false;
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

// A candidate is taken when the file exists and its version's first number is
// the generation's (a file without a version resource passes); a file of
// another generation is remembered as the error when nothing fits.
struct Search {
    const GenRule* rule;
    BryceInstall* out;
    std::string rejected;
    bool done;
    Search(const GenRule* r, BryceInstall* o) : rule(r), out(o), done(false) {}
    bool consider(const std::wstring& path, const std::string& version, const std::string& found)
    {
        if (done || !fileExists(path)) return false;
        std::string fileVersion = fileVersionText(path);
        const std::string& effective = fileVersion.empty() ? version : fileVersion;
        int major = versionMajor(effective);
        if (major != 0 && major != rule->major) {
            if (rejected.empty()) rejected = format("%s is version %s, not a %d.x Bryce", narrow(path).c_str(), effective.c_str(), rule->major);
            return false;
        }
        out->path = path;
        out->version = version.empty() ? fileVersion : version;
        out->found = found;
        done = true;
        return true;
    }
};

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

const char* bryceLabel(int gen)
{
    const GenRule* r = ruleFor(gen);
    return r ? r->label : "Bryce (unknown generation)";
}

bool findBryce(int gen, BryceInstall& out, std::string& error)
{
    out = BryceInstall();
    out.gen = gen;
    error.clear();
    const GenRule* rule = ruleFor(gen);
    if (!rule) { error = format("there is no Bryce generation %d", gen); return false; }
    Search s(rule, &out);
    // 1. The product keys, highest version first, in both registry views.
    for (int v = 0; v < 2 && !s.done; ++v) {
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, kDazKey, kRegViews[v]);
        std::vector<KeyVersion> keys;
        for (size_t i = 0; i < subs.size(); ++i) {
            if (!isBryceProgramKey(subs[i], rule->keyPrefix)) continue;
            KeyVersion kv;
            kv.key = std::wstring(kDazKey) + L"\\" + subs[i];
            kv.version = narrow(regText(HKEY_LOCAL_MACHINE, kv.key, L"Version", kRegViews[v]));
            keys.push_back(kv);
        }
        std::sort(keys.begin(), keys.end(), newerFirst);
        for (size_t i = 0; i < keys.size() && !s.done; ++i)
            s.consider(joinPath(regText(HKEY_LOCAL_MACHINE, keys[i].key, L"Location", kRegViews[v]), kBryceExe), keys[i].version,
                       "registry " + regKeyText(HKEY_LOCAL_MACHINE, keys[i].key, kRegViews[v]) + "\\Location");
    }
    // 2. The Add/Remove Programs entries.
    for (int v = 0; v < 2 && !s.done; ++v) {
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, kUninstallKey, kRegViews[v]);
        for (size_t i = 0; i < subs.size() && !s.done; ++i) {
            if (!isBryceProgramKey(subs[i], rule->keyPrefix)) continue;
            std::wstring key = std::wstring(kUninstallKey) + L"\\" + subs[i];
            s.consider(joinPath(regText(HKEY_LOCAL_MACHINE, key, L"InstallLocation", kRegViews[v]), kBryceExe),
                       narrow(regText(HKEY_LOCAL_MACHINE, key, L"DisplayVersion", kRegViews[v])),
                       "uninstall entry " + regKeyText(HKEY_LOCAL_MACHINE, key, kRegViews[v]));
        }
    }
    // 3. The Start menu shortcuts whose target is a Bryce.exe, told apart by the
    //    file's version (the Lightning and documentation shortcuts point elsewhere).
    std::vector<ShortcutHit> hits = startMenuTargets(L"Bryce", kBryceExe);
    for (size_t i = 0; i < hits.size() && !s.done; ++i) s.consider(hits[i].path, std::string(), hits[i].how);
    // 4. The default folders.
    for (int i = 0; i < 3 && !s.done && rule->defaults[i]; ++i) s.consider(programFilesFile(rule->defaults[i]), std::string(), "default folder");
    if (s.done) return true;
    error = std::string(rule->label) + " is not installed: "
          + (s.rejected.empty() ? std::string("no registry entry, Start menu shortcut or default folder names it") : s.rejected);
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

namespace {
const size_t kPrefsImageWithScene = 0x21F0;

std::vector<std::wstring> prefsCandidates(const std::wstring& bryceExe)
{
    std::vector<std::wstring> out;
    std::wstring dir = bryceExe.substr(0, bryceExe.rfind(L'\\'));
    out.push_back(dir + L"\\Bryce Prefs");
    // The VirtualStore copy: %LOCALAPPDATA%\VirtualStore\<path without the drive>
    wchar_t local[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local)) && dir.size() > 3 && dir[1] == L':')
        out.push_back(std::wstring(local) + L"\\VirtualStore\\" + dir.substr(3) + L"\\Bryce Prefs");
    return out;
}
} // namespace

bool ensureImageWithScene(const std::wstring& bryceExe, std::string& note)
{
    std::vector<std::wstring> paths = prefsCandidates(bryceExe);
    bool anyOn = false, anyFile = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        HANDLE h = CreateFileW(paths[i].c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) continue;
            // present but not writable: still read it, so the state is known
            h = CreateFileW(paths[i].c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h == INVALID_HANDLE_VALUE) { note += narrow(paths[i]) + ": " + lastErrorText((int)GetLastError()) + "; "; continue; }
            anyFile = true;
            LARGE_INTEGER off; off.QuadPart = (LONGLONG)kPrefsImageWithScene;
            unsigned char v = 0; DWORD got = 0;
            SetFilePointerEx(h, off, NULL, FILE_BEGIN);
            ReadFile(h, &v, 1, &got, NULL);
            CloseHandle(h);
            if (v == 1) { anyOn = true; note += narrow(paths[i]) + ": on; "; }
            else note += narrow(paths[i]) + ": off and not writable (" + lastErrorText((int)e) + "); ";
            continue;
        }
        anyFile = true;
        LARGE_INTEGER size;
        // The file is 16336 bytes on one Bryce 7.1 install and 14276 on another; the
        // byte sits at the same offset in both (the layouts agree around it).
        if (!GetFileSizeEx(h, &size) || size.QuadPart <= (LONGLONG)kPrefsImageWithScene) { CloseHandle(h); note += narrow(paths[i]) + ": unexpected size; "; continue; }
        LARGE_INTEGER off; off.QuadPart = (LONGLONG)kPrefsImageWithScene;
        unsigned char v = 0; DWORD got = 0, wrote = 0;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
        if (!ReadFile(h, &v, 1, &got, NULL) || got != 1) { CloseHandle(h); note += narrow(paths[i]) + ": read failed; "; continue; }
        if (v == 1) { anyOn = true; note += narrow(paths[i]) + ": already on; "; CloseHandle(h); continue; }
        unsigned char one = 1;
        SetFilePointerEx(h, off, NULL, FILE_BEGIN);
        bool ok = WriteFile(h, &one, 1, &wrote, NULL) && wrote == 1;
        DWORD e = GetLastError();
        CloseHandle(h);
        if (ok) { anyOn = true; note += narrow(paths[i]) + ": was off, turned on; "; }
        else note += narrow(paths[i]) + ": was off, write failed (" + lastErrorText((int)e) + "); ";
    }
    if (!anyFile) { note = "no Bryce Prefs file yet (Bryce has not been run on this node); the preference is set by Bryce itself at its first exit"; return true; }
    if (note.size() > 2) note.erase(note.size() - 2);
    return anyOn;
}

bool startBryce(int gen, const std::wstring& scene, bool restart, BryceInstall& inst, std::string& error, bool& restarted, std::string& prefsNote)
{
    restarted = false;
    if (!findBryce(gen, inst, error)) return false;
    if (findBrycePid() != 0) {
        std::wstring running = runningBrycePath();
        if (!restart) {
            error = "a Bryce is already running" + (running.empty() ? std::string() : " (" + narrow(running) + ")");
            return false;
        }
        std::string stopErr;
        if (!stopBryce(stopErr)) { error = "the running Bryce" + (running.empty() ? std::string() : " (" + narrow(running) + ")") + " could not be ended: " + stopErr; return false; }
        for (int i = 0; i < 50 && findBrycePid() != 0; ++i) Sleep(100);   // the process entry goes a moment after the terminate
        restarted = true;
    }
    // Image with Scene Open/Save on, so that opening the scene shows its image instead of starting a render proof.
    ensureImageWithScene(inst.path, prefsNote);
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
