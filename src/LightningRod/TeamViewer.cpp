#include "TeamViewer.h"

#include "Net.h"
#include "StrUtil.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <cstring>
#include <cwctype>
#include <vector>

namespace tlr {

namespace {

const wchar_t* const kExe = L"TeamViewer.exe";

bool fileExists(const std::wstring& path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool endsWithNoCase(const std::wstring& s, const wchar_t* tail)
{
    size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

bool startsWithNoCase(const std::wstring& s, const wchar_t* head)
{
    size_t n = wcslen(head);
    return s.size() >= n && _wcsnicmp(s.c_str(), head, n) == 0;
}

std::wstring withExe(std::wstring dir)
{
    while (!dir.empty() && (dir[dir.size() - 1] == L'\\' || dir[dir.size() - 1] == L'/')) dir.erase(dir.size() - 1);
    return dir.empty() ? dir : dir + L"\\" + kExe;
}

// A value such as "C:\x\TeamViewer.exe,0" or "\"C:\x\TeamViewer.exe\" /flag"
// reduced to the path of the executable in it.
std::wstring exeIn(const std::wstring& value)
{
    std::wstring s = trim(value);
    if (s.empty()) return s;
    if (s[0] == L'"') {
        size_t q = s.find(L'"', 1);
        return q == std::wstring::npos ? std::wstring() : s.substr(1, q - 1);
    }
    size_t e = s.find(L".exe");
    if (e != std::wstring::npos) return s.substr(0, e + 4);
    size_t comma = s.rfind(L',');
    return comma == std::wstring::npos ? s : s.substr(0, comma);
}

// Both registry views on 64-bit Windows: TeamViewer 15 has a 32-bit and a
// 64-bit build and registers under the view of its own bitness. A 32-bit
// Windows ignores the view flags.
const REGSAM kViews[] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
const char* const kViewNames[] = { "", " (32-bit view)" };

std::string viewText(int view) { return kViewNames[view]; }

std::wstring regText(HKEY root, const std::wstring& path, const wchar_t* value, REGSAM view)
{
    HKEY key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS) return std::wstring();
    wchar_t buf[1024];
    DWORD type = 0, size = sizeof(buf) - sizeof(wchar_t);
    std::wstring out;
    if (RegQueryValueExW(key, value, NULL, &type, (BYTE*)buf, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        buf[size / sizeof(wchar_t)] = L'\0';
        out = buf;
        if (type == REG_EXPAND_SZ) {
            wchar_t ex[1024];
            DWORD n = ExpandEnvironmentStringsW(buf, ex, 1024);
            if (n > 0 && n <= 1024) out = ex;
        }
    }
    RegCloseKey(key);
    return trim(out);
}

std::vector<std::wstring> regSubKeys(HKEY root, const std::wstring& path, REGSAM view)
{
    std::vector<std::wstring> out;
    HKEY key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | view, &key) != ERROR_SUCCESS) return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(key);
    return out;
}

// The product key: HKLM\SOFTWARE\TeamViewer\InstallationDirectory in the
// current releases, HKLM\SOFTWARE\TeamViewer\VersionN\InstallationDirectory in
// releases 6 to 10.
std::wstring fromProductKey(std::string& how)
{
    const std::wstring base = L"SOFTWARE\\TeamViewer";
    for (int v = 0; v < 2; ++v) {
        std::wstring p = withExe(regText(HKEY_LOCAL_MACHINE, base, L"InstallationDirectory", kViews[v]));
        if (fileExists(p)) { how = "registry HKLM\\SOFTWARE\\TeamViewer" + viewText(v); return p; }
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, base, kViews[v]);
        for (size_t i = 0; i < subs.size(); ++i) {
            p = withExe(regText(HKEY_LOCAL_MACHINE, base + L"\\" + subs[i], L"InstallationDirectory", kViews[v]));
            if (fileExists(p)) { how = "registry HKLM\\SOFTWARE\\TeamViewer\\" + narrow(subs[i]) + viewText(v); return p; }
        }
    }
    return std::wstring();
}

// Add/Remove Programs: every entry whose key name starts with TeamViewer
// ("TeamViewer", "TeamViewer 9", "TeamViewer Host"), for the machine and for
// this user.
std::wstring fromUninstallEntries(std::string& how)
{
    const std::wstring base = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    const HKEY roots[] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
    const char* const rootNames[] = { "HKLM", "HKCU" };
    for (int r = 0; r < 2; ++r) {
        for (int v = 0; v < 2; ++v) {
            std::vector<std::wstring> subs = regSubKeys(roots[r], base, kViews[v]);
            for (size_t i = 0; i < subs.size(); ++i) {
                if (!startsWithNoCase(subs[i], L"TeamViewer")) continue;
                std::wstring key = base + L"\\" + subs[i];
                std::wstring p = withExe(regText(roots[r], key, L"InstallLocation", kViews[v]));
                if (!fileExists(p)) p = exeIn(regText(roots[r], key, L"DisplayIcon", kViews[v]));
                if (!fileExists(p) || !endsWithNoCase(p, kExe)) {
                    std::wstring un = exeIn(regText(roots[r], key, L"UninstallString", kViews[v]));
                    size_t cut = un.find_last_of(L"\\/");
                    p = cut == std::wstring::npos ? std::wstring() : withExe(un.substr(0, cut));
                }
                if (fileExists(p)) { how = format("uninstall entry %s\\...\\Uninstall\\%s%s", rootNames[r], narrow(subs[i]).c_str(), viewText(v).c_str()); return p; }
            }
        }
    }
    return std::wstring();
}

std::wstring fromAppPaths(std::string& how)
{
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\TeamViewer.exe";
    for (int v = 0; v < 2; ++v) {
        std::wstring p = exeIn(regText(HKEY_LOCAL_MACHINE, key, NULL, kViews[v]));
        if (fileExists(p)) { how = "registry App Paths\\TeamViewer.exe" + viewText(v); return p; }
    }
    return std::wstring();
}

// The target of a shortcut; empty when it cannot be read.
std::wstring shortcutTarget(const std::wstring& lnk)
{
    IShellLinkW* link = NULL;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&link)) || !link) return std::wstring();
    IPersistFile* file = NULL;
    std::wstring out;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && file) {
        if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ))) {
            wchar_t path[MAX_PATH] = L"";
            WIN32_FIND_DATAW fd;
            if (SUCCEEDED(link->GetPath(path, MAX_PATH, &fd, SLGP_UNCPRIORITY))) out = path;
        }
        file->Release();
    }
    link->Release();
    return out;
}

// Every *.lnk whose name contains TeamViewer under folder, recursively; the
// first whose target exists and is an executable is taken.
std::wstring teamViewerShortcut(const std::wstring& folder, std::wstring& lnkName)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring out;
    std::vector<std::wstring> dirs;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = folder + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs.push_back(full); continue; }
        if (!endsWithNoCase(fd.cFileName, L".lnk")) continue;
        std::wstring lower = fd.cFileName;
        for (size_t i = 0; i < lower.size(); ++i) lower[i] = (wchar_t)towlower(lower[i]);
        if (lower.find(L"teamviewer") == std::wstring::npos) continue;
        std::wstring target = shortcutTarget(full);
        if (fileExists(target) && endsWithNoCase(target, L".exe")) { out = target; lnkName = fd.cFileName; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (size_t i = 0; i < dirs.size() && out.empty(); ++i) out = teamViewerShortcut(dirs[i], lnkName);
    return out;
}

std::wstring fromStartMenu(std::string& how)
{
    const int folders[] = { CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS };
    const char* const names[] = { "all users", "this user" };
    for (int i = 0; i < 2; ++i) {
        wchar_t base[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, folders[i], NULL, SHGFP_TYPE_CURRENT, base))) continue;
        std::wstring lnk, p = teamViewerShortcut(base, lnk);
        if (!p.empty()) { how = format("Start menu shortcut %s (%s)", narrow(lnk).c_str(), names[i]); return p; }
    }
    return std::wstring();
}

std::wstring fromProgramFiles(std::string& how)
{
    const wchar_t* const vars[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432" };
    for (int i = 0; i < 3; ++i) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(vars[i], base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        std::wstring p = std::wstring(base, n) + L"\\TeamViewer\\" + kExe;
        if (fileExists(p)) { how = "Program Files folder"; return p; }
    }
    return std::wstring();
}

struct WindowSearch { std::vector<DWORD> pids; HWND best; long area; };

BOOL CALLBACK findWindow(HWND w, LPARAM lp)
{
    WindowSearch* s = (WindowSearch*)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) != NULL) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    bool ours = false;
    for (size_t i = 0; i < s->pids.size() && !ours; ++i) ours = s->pids[i] == pid;
    if (!ours) return TRUE;
    RECT r;
    if (!GetWindowRect(w, &r)) return TRUE;
    long area = (r.right - r.left) * (r.bottom - r.top);
    if (area > s->area) { s->area = area; s->best = w; }
    return TRUE;
}

std::vector<DWORD> teamViewerPids()
{
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { if (_wcsicmp(pe.szExeFile, kExe) == 0) pids.push_back(pe.th32ProcessID); } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

} // namespace

std::wstring teamViewerPath(std::string& how)
{
    how.clear();
    std::wstring p = fromProductKey(how);
    if (p.empty()) p = fromUninstallEntries(how);
    if (p.empty()) p = fromAppPaths(how);
    if (p.empty()) p = fromStartMenu(how);
    if (p.empty()) p = fromProgramFiles(how);
    return p;
}

bool teamViewerRunning()
{
    return !teamViewerPids().empty();
}

bool startTeamViewer(std::wstring& path, std::string& how, bool& wasRunning, std::string& error)
{
    error.clear();
    path = teamViewerPath(how);
    if (path.empty()) { error = "TeamViewer is not installed: no registry entry, Start menu shortcut or Program Files folder names it"; return false; }
    wasRunning = teamViewerRunning();
    std::wstring dir = path.substr(0, path.rfind(L'\\'));
    std::wstring cmd = L"\"" + path + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(path.c_str(), &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
        error = narrow(path) + ": " + lastErrorText((int)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

HWND teamViewerWindow()
{
    WindowSearch s;
    s.pids = teamViewerPids();
    s.best = NULL;
    s.area = 0;
    if (s.pids.empty()) return NULL;
    EnumWindows(findWindow, (LPARAM)&s);
    return s.best;
}

} // namespace tlr
