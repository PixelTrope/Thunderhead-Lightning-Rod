#include "Installed.h"

#include "StrUtil.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <cstdlib>
#include <cstring>
#include <cwctype>

namespace tlr {

bool fileExists(const std::wstring& path)
{
    if (path.empty()) return false;
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool startsWithNoCase(const std::wstring& s, const wchar_t* head)
{
    size_t n = wcslen(head);
    return s.size() >= n && _wcsnicmp(s.c_str(), head, n) == 0;
}

bool endsWithNoCase(const std::wstring& s, const wchar_t* tail)
{
    size_t n = wcslen(tail);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, tail) == 0;
}

std::wstring joinPath(std::wstring dir, const wchar_t* name)
{
    dir = trim(dir);
    for (size_t i = 0; i < dir.size(); ++i) if (dir[i] == L'/') dir[i] = L'\\';
    while (!dir.empty() && dir[dir.size() - 1] == L'\\') dir.erase(dir.size() - 1);
    return dir.empty() ? dir : dir + L"\\" + name;
}

std::wstring exeIn(const std::wstring& value)
{
    std::wstring s = trim(value);
    if (s.empty()) return s;
    if (s[0] == L'"') {
        size_t q = s.find(L'"', 1);
        return q == std::wstring::npos ? std::wstring() : s.substr(1, q - 1);
    }
    std::wstring lower = s;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = (wchar_t)towlower(lower[i]);
    size_t e = lower.find(L".exe");
    if (e != std::wstring::npos) return s.substr(0, e + 4);
    size_t comma = s.rfind(L',');
    return comma == std::wstring::npos ? s : s.substr(0, comma);
}

// ---------------------------------------------------------------- registry

const REGSAM kRegViews[2] = { 0, KEY_WOW64_64KEY };

bool isWow64()
{
    typedef BOOL (WINAPI *Fn)(HANDLE, PBOOL);
    static int cached = -1;
    if (cached < 0) {
        BOOL wow = FALSE;
        Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
        cached = (fn && fn(GetCurrentProcess(), &wow) && wow) ? 1 : 0;
    }
    return cached == 1;
}

std::string regKeyText(HKEY root, const std::wstring& path, REGSAM view)
{
    std::string r = root == HKEY_LOCAL_MACHINE ? "HKLM\\" : root == HKEY_CURRENT_USER ? "HKCU\\" : "";
    std::wstring p = path;
    if (view != KEY_WOW64_64KEY && isWow64() && startsWithNoCase(p, L"SOFTWARE\\")) p = L"SOFTWARE\\WOW6432Node\\" + p.substr(9);
    return r + narrow(p);
}

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

// ---------------------------------------------------------------- shortcuts

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

static void shortcutsUnder(const std::wstring& folder, const std::wstring& nameLower, const wchar_t* targetEndsWith,
                           const char* who, std::vector<ShortcutHit>& hits)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> dirs;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = folder + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { dirs.push_back(full); continue; }
        if (!endsWithNoCase(fd.cFileName, L".lnk")) continue;
        std::wstring lower = fd.cFileName;
        for (size_t i = 0; i < lower.size(); ++i) lower[i] = (wchar_t)towlower(lower[i]);
        if (lower.find(nameLower) == std::wstring::npos) continue;
        std::wstring target = shortcutTarget(full);
        if (!fileExists(target) || !endsWithNoCase(target, targetEndsWith)) continue;
        ShortcutHit hit;
        hit.path = target;
        hit.how = format("Start menu shortcut %s (%s)", narrow(fd.cFileName).c_str(), who);
        hits.push_back(hit);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (size_t i = 0; i < dirs.size(); ++i) shortcutsUnder(dirs[i], nameLower, targetEndsWith, who, hits);
}

std::vector<ShortcutHit> startMenuTargets(const wchar_t* nameContains, const wchar_t* targetEndsWith)
{
    std::vector<ShortcutHit> hits;
    std::wstring nameLower = nameContains;
    for (size_t i = 0; i < nameLower.size(); ++i) nameLower[i] = (wchar_t)towlower(nameLower[i]);
    const int folders[] = { CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS };
    const char* const names[] = { "all users", "this user" };
    for (int i = 0; i < 2; ++i) {
        wchar_t base[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, folders[i], NULL, SHGFP_TYPE_CURRENT, base))) continue;
        shortcutsUnder(base, nameLower, targetEndsWith, names[i], hits);
    }
    return hits;
}

std::wstring startMenuTarget(const wchar_t* nameContains, const wchar_t* targetEndsWith, std::string& how)
{
    std::vector<ShortcutHit> hits = startMenuTargets(nameContains, targetEndsWith);
    if (hits.empty()) return std::wstring();
    how = hits[0].how;
    return hits[0].path;
}

// ---------------------------------------------------------------- files

std::wstring programFilesFile(const wchar_t* tail)
{
    // A 32-bit process sees ProgramFiles as Program Files (x86) on 64-bit Windows;
    // the other two variables exist only there and cover an install either side.
    const wchar_t* const vars[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"ProgramW6432" };
    for (int i = 0; i < 3; ++i) {
        wchar_t base[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(vars[i], base, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) continue;
        std::wstring p = std::wstring(base, n) + L"\\" + tail;
        if (fileExists(p)) return p;
    }
    return std::wstring();
}

std::string fileVersionText(const std::wstring& path)
{
    DWORD handle = 0, size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return std::string();
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, &data[0])) return std::string();
    VS_FIXEDFILEINFO* fixed = NULL;
    UINT len = 0;
    if (!VerQueryValueW(&data[0], L"\\", (LPVOID*)&fixed, &len) || !fixed || len < sizeof(VS_FIXEDFILEINFO)) return std::string();
    DWORD ms = fixed->dwProductVersionMS, ls = fixed->dwProductVersionLS;
    if (ms == 0 && ls == 0) { ms = fixed->dwFileVersionMS; ls = fixed->dwFileVersionLS; }
    if (ms == 0 && ls == 0) return std::string();
    return format("%u.%u.%u.%u", (unsigned)HIWORD(ms), (unsigned)LOWORD(ms), (unsigned)HIWORD(ls), (unsigned)LOWORD(ls));
}

int versionMajor(const std::string& version)
{
    return atoi(version.c_str());
}

} // namespace tlr
