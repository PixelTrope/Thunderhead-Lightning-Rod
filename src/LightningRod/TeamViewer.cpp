#include "TeamViewer.h"

#include "Installed.h"
#include "Net.h"
#include "StrUtil.h"

#include <tlhelp32.h>
#include <cstring>
#include <vector>

namespace tlr {

namespace {

const wchar_t* const kExe = L"TeamViewer.exe";

// The product key: HKLM\SOFTWARE\TeamViewer\InstallationDirectory in the
// current releases, HKLM\SOFTWARE\TeamViewer\VersionN\InstallationDirectory in
// releases 6 to 10. TeamViewer 15 has a 32-bit and a 64-bit build, each
// registering in the view of its own bitness, so both views are read.
std::wstring fromProductKey(std::string& how)
{
    const std::wstring base = L"SOFTWARE\\TeamViewer";
    for (int v = 0; v < 2; ++v) {
        std::wstring p = joinPath(regText(HKEY_LOCAL_MACHINE, base, L"InstallationDirectory", kRegViews[v]), kExe);
        if (fileExists(p)) { how = "registry " + regKeyText(HKEY_LOCAL_MACHINE, base, kRegViews[v]); return p; }
        std::vector<std::wstring> subs = regSubKeys(HKEY_LOCAL_MACHINE, base, kRegViews[v]);
        for (size_t i = 0; i < subs.size(); ++i) {
            p = joinPath(regText(HKEY_LOCAL_MACHINE, base + L"\\" + subs[i], L"InstallationDirectory", kRegViews[v]), kExe);
            if (fileExists(p)) { how = "registry " + regKeyText(HKEY_LOCAL_MACHINE, base + L"\\" + subs[i], kRegViews[v]); return p; }
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
    for (int r = 0; r < 2; ++r) {
        for (int v = 0; v < 2; ++v) {
            std::vector<std::wstring> subs = regSubKeys(roots[r], base, kRegViews[v]);
            for (size_t i = 0; i < subs.size(); ++i) {
                if (!startsWithNoCase(subs[i], L"TeamViewer")) continue;
                std::wstring key = base + L"\\" + subs[i];
                std::wstring p = joinPath(regText(roots[r], key, L"InstallLocation", kRegViews[v]), kExe);
                if (!fileExists(p)) p = exeIn(regText(roots[r], key, L"DisplayIcon", kRegViews[v]));
                if (!fileExists(p) || !endsWithNoCase(p, kExe)) {
                    std::wstring un = exeIn(regText(roots[r], key, L"UninstallString", kRegViews[v]));
                    size_t cut = un.find_last_of(L"\\/");
                    p = cut == std::wstring::npos ? std::wstring() : joinPath(un.substr(0, cut), kExe);
                }
                if (fileExists(p)) { how = "uninstall entry " + regKeyText(roots[r], key, kRegViews[v]); return p; }
            }
        }
    }
    return std::wstring();
}

std::wstring fromAppPaths(std::string& how)
{
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\TeamViewer.exe";
    for (int v = 0; v < 2; ++v) {
        std::wstring p = exeIn(regText(HKEY_LOCAL_MACHINE, key, NULL, kRegViews[v]));
        if (fileExists(p)) { how = "registry " + regKeyText(HKEY_LOCAL_MACHINE, key, kRegViews[v]); return p; }
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
    if (p.empty()) p = startMenuTarget(L"TeamViewer", L".exe", how);
    if (p.empty()) { p = programFilesFile(L"TeamViewer\\TeamViewer.exe"); if (!p.empty()) how = "Program Files folder"; }
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
