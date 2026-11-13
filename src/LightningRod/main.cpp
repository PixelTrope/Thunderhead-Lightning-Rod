// Lightning Rod: node agent for the Bryce Lightning render farm.
// See docs/PROTOCOL.md for the wire protocol.
//
// The network loop runs on a worker thread; the main thread owns a window that
// shows the Thunderhead being reported to, the agent's start and running time,
// and every value sent, refreshed after each report.

#include "Net.h"
#include "Log.h"
#include "Protocol.h"
#include "StrUtil.h"
#include "Version.h"
#include "SysInfo.h"
#include "Lightning.h"
#include "Thermal.h"
#include "Audio.h"
#include "TeamViewer.h"
#include "Sound.h"
#include "resource.h"

#include <commctrl.h>
#include <iphlpapi.h>
#include <objbase.h>
#include <cstring>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace tlr;

namespace {

const UINT WM_AGENT_UPDATE = WM_APP + 1;
const UINT WM_NETWORK_ERROR = WM_APP + 2;   // no valid address after kIpTries re-reads; the window shows the error box
// A node whose NIC has not finished DHCP when the agent starts at logon reads
// no address, 127.x or an APIPA 169.254.x.y one. The agent then re-reads the
// address every kIpWaitMs, up to kIpTries times, before it gives up.
const int kIpTries = 4;
const DWORD kIpWaitMs = 30000;
const UINT_PTR kClockTimer = 1;

Logger g_log;
HWND g_window = NULL;
HANDLE g_singleInstance = NULL;

void logLine(const std::string& s) { g_log.write(s); }

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring p(path, n);
    size_t slash = p.rfind(L'\\');
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

std::string readTextFile(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::string();
    std::string text;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return text;
}

bool writeTextFile(const std::wstring& path, const std::string& text)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

// ---- settings and the SMB1 relay ------------------------------------------
// settings.txt beside the agent holds the Settings dialog: relay=yes|no and
// relay_ip=, the Windows XP client this node relays file sharing for. Windows
// 10 no longer speaks SMB1 to an XP share, so Thunderhead copies the files for
// that client into this node's shared folder and asks this agent, through
// RELAY, to copy them on over SMB1 (or to fetch a file back). The window
// thread edits the values, the agent thread reads them, hence the lock.
CRITICAL_SECTION g_settingsLock;
bool g_relay = false;
std::string g_relayIp;
volatile long g_rehello = 0;     // set by the window thread: send HELLO again with the new values

std::wstring settingsFile() { return exeDirectory() + L"\\settings.txt"; }
std::wstring relayDir() { return exeDirectory() + L"\\relay"; }   // staging folders of RELAY jobs, relay\<job>

void loadSettings()
{
    std::vector<std::string> lines = split(readTextFile(settingsFile()), '\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t eq = lines[i].find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(lines[i].substr(0, eq)), value = trim(lines[i].substr(eq + 1));
        if (key == "relay") g_relay = parseBool(value);
        else if (key == "relay_ip") g_relayIp = value;
    }
    if (g_relayIp.empty()) g_relay = false;
}

void saveSettings()
{
    std::string text = std::string("relay=") + boolText(g_relay) + "\r\nrelay_ip=" + g_relayIp + "\r\n";
    if (!writeTextFile(settingsFile(), text)) logLine("settings.txt could not be written: " + lastErrorText((int)GetLastError()));
}

void relaySnapshot(bool& relay, std::string& ip)
{
    EnterCriticalSection(&g_settingsLock);
    relay = g_relay;
    ip = g_relayIp;
    LeaveCriticalSection(&g_settingsLock);
}

std::string relayText()
{
    bool relay;
    std::string ip;
    relaySnapshot(relay, ip);
    return relay ? "to " + ip : std::string("off");
}

// A dotted IPv4 address with four parts, as the Settings dialog accepts it.
bool validIpText(const std::string& s)
{
    std::vector<std::string> parts = split(s, '.');
    if (parts.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (parts[i].empty() || parts[i].size() > 3) return false;
        for (size_t k = 0; k < parts[i].size(); ++k) if (parts[i][k] < '0' || parts[i][k] > '9') return false;
        if (atoi(parts[i].c_str()) > 255) return false;
    }
    return true;
}

// Sets the relay values as the Settings dialog or Thunderhead's RELAYSET asks,
// saves them, and has HELLO sent again when they changed; off keeps the last
// address for the dialog. Returns the error text, empty when applied.
std::string applyRelaySetting(bool on, const std::string& ip, const std::string& ownIp)
{
    if (on && !validIpText(ip)) return "relay_ip must be an IPv4 address of four numbers";
    if (on && ip == ownIp) return "relay_ip " + ip + " is this node's own address";
    bool was;
    std::string wasIp;
    relaySnapshot(was, wasIp);
    EnterCriticalSection(&g_settingsLock);
    g_relay = on;
    if (on) g_relayIp = ip;
    saveSettings();
    LeaveCriticalSection(&g_settingsLock);
    if (was != on || (on && ip != wasIp)) {
        logLine("SMB1 relay set " + relayText());
        InterlockedExchange(&g_rehello, 1);
    }
    return std::string();
}

// Copies every file under src (which ends with a backslash) to dst (likewise),
// folders included, existing files overwritten, and stops at the first failure.
bool copyTreeTo(const std::wstring& src, const std::wstring& dst, unsigned& files, unsigned long long& bytes, std::string& err)
{
    if (!CreateDirectoryW(dst.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        err = narrow(dst) + ": " + lastErrorText((int)GetLastError());
        return false;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((src + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { err = narrow(src) + ": " + lastErrorText((int)GetLastError()); return false; }
    bool ok = true;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copyTreeTo(src + fd.cFileName + L"\\", dst + fd.cFileName + L"\\", files, bytes, err);
        } else if (CopyFileW((src + fd.cFileName).c_str(), (dst + fd.cFileName).c_str(), FALSE)) {
            ++files;
            bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        } else {
            err = narrow(dst + fd.cFileName) + ": " + lastErrorText((int)GetLastError());
            ok = false;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

// Deletes a folder (ending with a backslash) with everything in it.
void removeTree(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) removeTree(dir + fd.cFileName + L"\\");
            else DeleteFileW((dir + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring bare = dir;
    while (!bare.empty() && bare[bare.size() - 1] == L'\\') bare.erase(bare.size() - 1);
    RemoveDirectoryW(bare.c_str());
}

// One RELAY job, run on a thread of its own so that reports go on while a
// copy over SMB1 takes its time; the agent thread sends the ACK when it ends.
struct RelayTask {
    HANDLE thread;
    std::string op, job, dest, share, pattern;
    bool ok;
    std::string error, file;
    unsigned files;
    unsigned long long bytes;
    RelayTask() : thread(NULL), ok(false), files(0), bytes(0) {}
};

DWORD WINAPI relayTaskThread(LPVOID p)
{
    RelayTask* t = (RelayTask*)p;
    std::wstring destRoot = L"\\\\" + widen(t->dest) + L"\\" + widen(t->share) + L"\\";
    std::wstring stage = relayDir() + L"\\" + widen(t->job) + L"\\";
    if (t->op == "push") {
        // Everything Thunderhead placed under relay\<job> goes to the root of the
        // destination share, folders and all; the staging folder goes afterwards.
        if (GetFileAttributesW(stage.c_str()) == INVALID_FILE_ATTRIBUTES) {
            t->error = "nothing staged in " + narrow(stage);
        } else {
            t->ok = copyTreeTo(stage, destRoot, t->files, t->bytes, t->error);
            removeTree(stage);
        }
    } else if (t->op == "pull") {
        // The newest file matching the pattern, by name, from the destination
        // share into relay\<job>, where Thunderhead reads it over this node's share.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((destRoot + widen(t->pattern)).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            t->error = narrow(destRoot + widen(t->pattern)) + ": " + lastErrorText((int)GetLastError());
        } else {
            std::wstring best;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (best.empty() || _wcsicmp(fd.cFileName, best.c_str()) > 0)) best = fd.cFileName;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            if (best.empty()) t->error = narrow(destRoot + widen(t->pattern)) + ": no such file";
            else {
                CreateDirectoryW(relayDir().c_str(), NULL);
                CreateDirectoryW(stage.c_str(), NULL);
                if (CopyFileW((destRoot + best).c_str(), (stage + best).c_str(), FALSE)) {
                    WIN32_FILE_ATTRIBUTE_DATA fa;
                    if (GetFileAttributesExW((stage + best).c_str(), GetFileExInfoStandard, &fa))
                        t->bytes = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                    t->files = 1;
                    t->file = narrow(best);
                    t->ok = true;
                } else {
                    t->error = narrow(destRoot + best) + ": " + lastErrorText((int)GetLastError());
                }
            }
        }
    }
    return 0;
}

// ---- sounds ---------------------------------------------------------------
// The agent's sounds live in sounds\ beside it, put there by Thunderhead with
// the agent update. Each of the agent's three events plays the default file of
// its category unless Thunderhead's SOUND scheme message named another file;
// the names are kept in sounds\scheme.txt so that the Logon sound at the next
// start honours them as well. The agent thread sets them, the window thread
// reads them, hence the lock.
CRITICAL_SECTION g_soundLock;
std::wstring g_soundNames[kSoundEventCount];   // by event; empty = the category's default file

std::wstring soundDir() { return exeDirectory() + L"\\" + kSoundFolder; }
std::wstring schemeFile() { return soundDir() + L"\\scheme.txt"; }

void loadSoundScheme()
{
    std::vector<std::string> lines = split(readTextFile(schemeFile()), '\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        size_t eq = lines[i].find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(lines[i].substr(0, eq)), value = trim(lines[i].substr(eq + 1));
        for (int e = 0; e < kSoundEventCount; ++e)
            if (key == kSoundEvents[e].key) g_soundNames[e] = widen(value);
    }
}

void saveSoundScheme()
{
    std::string text;
    for (int e = 0; e < kSoundEventCount; ++e)
        if (kSoundEvents[e].app[0] == L'L') text += std::string(kSoundEvents[e].key) + "=" + narrow(g_soundNames[e]) + "\r\n";
    CreateDirectoryW(soundDir().c_str(), NULL);
    if (!writeTextFile(schemeFile(), text)) logLine("scheme.txt could not be written: " + lastErrorText((int)GetLastError()));
}

// Takes the file names of the agent's events from a SOUND scheme message:
// logon=, hello=, logoff=, each a name in the sounds folder or empty for the
// default. Returns what was set, for the log.
std::string setSoundScheme(const Message& m)
{
    std::string text;
    EnterCriticalSection(&g_soundLock);
    for (int e = 0; e < kSoundEventCount; ++e) {
        if (kSoundEvents[e].app[0] != L'L' || !m.has(kSoundEvents[e].key)) continue;
        std::string name = trim(m.get(kSoundEvents[e].key));
        if (name.find_first_of("\\/:") != std::string::npos) name.clear();   // a name, never a path
        g_soundNames[e] = widen(name);
        text += std::string(text.empty() ? "" : ", ") + kSoundEvents[e].key + "=" + (name.empty() ? "default" : name);
    }
    saveSoundScheme();
    LeaveCriticalSection(&g_soundLock);
    return text;
}

std::wstring soundFileFor(SoundEvent e)
{
    EnterCriticalSection(&g_soundLock);
    std::wstring name = g_soundNames[e];
    LeaveCriticalSection(&g_soundLock);
    if (name.empty()) name = soundDefaultFile(kSoundEvents[e].category);
    return soundDir() + L"\\" + name;
}

// logSuccess is false for the report sound, which plays every interval and
// would otherwise fill the log; a failure is always logged.
void playEventSound(SoundEvent e, bool logSuccess = true)
{
    std::string err;
    std::wstring file = soundFileFor(e);
    if (soundPlay(file, err)) { if (logSuccess) logLine(format("sound %s: %s", kSoundEvents[e].key, narrow(file).c_str())); }
    else logLine(format("sound %s failed: %s", kSoundEvents[e].key, err.c_str()));
}

// The Logoff sound, played to its end (4 s at most) before the process ends.
void playCloseSound()
{
    std::string err;
    std::wstring file = soundFileFor(SoundLrClose);
    if (soundPlayAndWait(file, 4000, err)) logLine(format("sound %s: %s", kSoundEvents[SoundLrClose].key, narrow(file).c_str()));
    else logLine(format("sound %s failed: %s", kSoundEvents[SoundLrClose].key, err.c_str()));
    soundShutdown();
}

// What the window shows; copied out of the agent under its lock.
struct AgentStatus {
    bool connected;
    bool waitingIp;                  // start-up: no valid address yet
    int ipTry;                       // re-reads done so far
    std::string badIp;               // what was read, empty when the NIC had no address
    std::string hostName, hostIp, hostMac;
    int intervalSec;
    SYSTEMTIME lastReportTime;
    bool haveReport;
    StaticInfo info;
    Message report;
    AgentStatus() : connected(false), waitingIp(false), ipTry(0), intervalSec(kDefaultReportInterval), haveReport(false) { memset(&lastReportTime, 0, sizeof(lastReportTime)); }
};

// An address the agent can work from: not empty, not the 0.0.0.0 of a NIC
// without a lease, not loopback and not an APIPA address, which Windows
// assigns itself when no DHCP server answered.
bool validAddress(const std::string& ip)
{
    if (ip.empty() || ip == "0.0.0.0") return false;
    if (ip.compare(0, 4, "127.") == 0) return false;
    if (ip.compare(0, 8, "169.254.") == 0) return false;
    return true;
}

// Starts a new instance of this executable and lets go of the single-instance
// mutex so that it can run; the caller exits afterwards.
void relaunchSelf()
{
    if (g_singleInstance) { CloseHandle(g_singleInstance); g_singleInstance = NULL; }
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(path, n) + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(path, &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        logLine("relaunch failed: " + lastErrorText((int)GetLastError()));
    }
}

// MAC of a LAN address through ARP; the node's own MAC when the host is this machine.
std::string macOf(const std::string& ip, const StaticInfo& self)
{
    if (ip == self.ip || ip == "127.0.0.1") return self.mac;
    ULONG mac[2] = { 0, 0 };
    ULONG len = 6;
    if (SendARP(inet_addr(ip.c_str()), 0, mac, &len) != NO_ERROR || len != 6) return std::string();
    const BYTE* b = (const BYTE*)mac;
    return format("%02X-%02X-%02X-%02X-%02X-%02X", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// NetBIOS rules: 1 to 15 characters, letters, digits and hyphens, not all digits.
std::string validComputerName(const std::string& name)
{
    if (name.empty() || name.size() > 15) return "name must be 1 to 15 characters";
    bool digitsOnly = true;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool digit = c >= '0' && c <= '9';
        if (!letter && !digit && c != '-') return "name may contain only letters, digits and hyphens";
        if (!digit) digitsOnly = false;
    }
    if (digitsOnly) return "name cannot be all digits";
    if (name[0] == '-' || name[name.size() - 1] == '-') return "name cannot start or end with a hyphen";
    return std::string();
}

bool enablePrivilege(const wchar_t* name, std::string& err)
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        err = lastErrorText((int)GetLastError());
        return false;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    if (!ok) err = narrow(name) + ": " + lastErrorText((int)GetLastError());
    CloseHandle(token);
    return ok;
}

bool enableShutdownPrivilege(std::string& err) { return enablePrivilege(SE_SHUTDOWN_NAME, err); }

std::string localDateText(const SYSTEMTIME& t) { return format("%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay); }

// Runs Windows' own shutdown.exe with the given switches and waits for it. Its
// exit code is a Win32 error code: 0 when the request was accepted, otherwise
// for example 1116 when an abort finds no shutdown in progress.
bool runShutdownExe(const std::wstring& args, std::string& err)
{
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { err = "system directory: " + lastErrorText((int)GetLastError()); return false; }
    std::wstring exe = std::wstring(sys, n) + L"\\shutdown.exe";
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe.c_str(), &cmdBuf[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        err = "shutdown.exe could not be started: " + lastErrorText((int)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD code = 0;
    bool done = WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0 && GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (!done) { err = "shutdown.exe did not finish within 30 s"; return false; }
    if (code != 0) { err = format("shutdown.exe exit code %u: ", (unsigned)code) + lastErrorText((int)code); return false; }
    return true;
}

// A batch file sent by Thunderhead that is still running.
struct RunningBatch {
    HANDLE process;
    std::string file;                // name inside the batch folder
    unsigned long long startMs;
    std::wstring title;              // title of its console window
    bool maximized, front;           // what to do with the window once it is found
    bool wantWindow;                 // the window is still to be found
};

// Puts a window above all others and gives it the keyboard. Windows does not let
// a background process take the foreground by asking, so the window is first
// lifted through the topmost band, which needs no permission, and the request
// for the keyboard is then made while attached to the input of the thread that
// has the foreground now.
void bringToFront(HWND w)
{
    if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
    SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(w, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    HWND fg = GetForegroundWindow();
    DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    bool attached = other != 0 && other != me && AttachThreadInput(me, other, TRUE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    if (attached) AttachThreadInput(me, other, FALSE);
}

// Looks for the window of a batch file, once it is wanted maximized or in front,
// and gives up 30 s after the start. The window is found by its title alone,
// which carries the name of the batch file: its class is ConsoleWindowClass
// under the classic console host and another under Windows Terminal.
void tryWindow(RunningBatch& b)
{
    if (!b.wantWindow) return;
    HWND w = FindWindowW(NULL, b.title.c_str());
    if (w) {
        if (b.maximized && !IsZoomed(w)) ShowWindow(w, SW_SHOWMAXIMIZED);
        if (b.front) bringToFront(w);
        b.wantWindow = false;
        logLine(format("BATCH batch\\%s: window found%s%s", b.file.c_str(), b.maximized ? ", maximized" : "",
                       b.front ? ", brought to the front" : ""));
    } else if (tickMs() - b.startMs > 30000) {
        b.wantWindow = false;
        logLine("BATCH batch\\" + b.file + ": window not found within 30 s, left as Windows opened it");
    }
}

// Writes the text as batch\batch-YYYYMMDD-HHMMSS.bat beside the agent and starts
// it with cmd.exe in a console window of its own on the node's desktop, as when
// the file is double-clicked, with the agent's folder (the shared one) as current
// directory. Output shows in that window, a pause waits for a key there, and the
// window closes when the batch file ends. maximized opens the window maximized;
// front brings it above the other windows, which would otherwise keep a window
// opened by a background program behind them with only its taskbar button showing.
bool startBatch(const std::string& utf8, bool maximized, bool front, RunningBatch& out, std::string& err)
{
    std::wstring dir = exeDirectory(), folder = dir + L"\\batch";
    if (!CreateDirectoryW(folder.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        err = "batch folder: " + lastErrorText((int)GetLastError());
        return false;
    }
    // cmd.exe wants CR LF line ends and reads a batch file in the OEM code page.
    std::wstring wide = widen(utf8), text;
    for (size_t i = 0; i < wide.size(); ++i) {
        if (wide[i] == L'\r') continue;
        if (wide[i] == L'\n') text += L"\r\n"; else text += wide[i];
    }
    if (text.size() < 2 || text.compare(text.size() - 2, 2, L"\r\n") != 0) text += L"\r\n";
    int n = WideCharToMultiByte(CP_OEMCP, 0, text.data(), (int)text.size(), NULL, 0, NULL, NULL);
    std::string bytes((size_t)n, '\0');
    if (n) WideCharToMultiByte(CP_OEMCP, 0, text.data(), (int)text.size(), &bytes[0], n, NULL, NULL);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string base = format("batch-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond), name;
    HANDLE f = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 100 && f == INVALID_HANDLE_VALUE; ++i) {
        name = i == 0 ? base : format("%s-%d", base.c_str(), i + 1);
        f = CreateFileW((folder + L"\\" + widen(name) + L".bat").c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (f == INVALID_HANDLE_VALUE) { err = "batch file: " + lastErrorText((int)GetLastError()); return false; }
    DWORD wrote = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, NULL) && wrote == bytes.size();
    DWORD writeErr = GetLastError();
    CloseHandle(f);
    if (!ok) { err = "batch file: " + lastErrorText((int)writeErr); return false; }

    wchar_t sys[MAX_PATH];
    UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    if (sn == 0 || sn >= MAX_PATH) { err = "system directory: " + lastErrorText((int)GetLastError()); return false; }
    std::wstring exe = std::wstring(sys, sn) + L"\\cmd.exe";
    std::wstring bat = folder + L"\\" + widen(name) + L".bat";
    // cmd.exe removes the outer pair of quotes and runs what is between them.
    std::wstring cmd = L"\"" + exe + L"\" /d /c \"\"" + bat + L"\"\"";
    std::wstring title = L"Thunderhead batch file " + widen(name) + L".bat";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    std::vector<wchar_t> titleBuf(title.begin(), title.end());
    titleBuf.push_back(L'\0');
    si.lpTitle = &titleBuf[0];
    if (maximized) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMAXIMIZED;
    }
    if (!CreateProcessW(exe.c_str(), &cmdBuf[0], NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, dir.c_str(), &si, &pi)) {
        err = "cmd.exe could not be started: " + lastErrorText((int)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    out.process = pi.hProcess;
    out.file = name + ".bat";
    out.startMs = tickMs();
    out.title = title;
    out.maximized = maximized;
    out.front = front;
    out.wantWindow = maximized || front;
    // The window exists a moment after the process; a slow node gets more tries
    // from pollBatches.
    for (int i = 0; i < 20 && out.wantWindow; ++i) {
        tryWindow(out);
        if (out.wantWindow) Sleep(100);
    }
    return true;
}

// Connects with a timeout; returns INVALID_SOCKET on failure.
SOCKET connectWithTimeout(const sockaddr_in& addr, int timeoutMs)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    setNonBlocking(s, true);
    int r = connect(s, (const sockaddr*)&addr, sizeof(addr));
    if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
    fd_set wr, ex;
    FD_ZERO(&wr); FD_ZERO(&ex);
    FD_SET(s, &wr); FD_SET(s, &ex);
    timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(0, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex)) { closesocket(s); return INVALID_SOCKET; }
    setNonBlocking(s, false);
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&noDelay, sizeof(noDelay));
    return s;
}

// ---------------------------------------------------------------- agent

class Agent {
public:
    Agent() : udp_(INVALID_SOCKET), tcp_(INVALID_SOCKET), intervalSec_(kDefaultReportInterval), nextReport_(0)
    {
        tvWindowWanted_ = false;
        tvStartMs_ = 0;
        helloSoundPending_ = false;
        InitializeCriticalSection(&lock_);
    }
    ~Agent() { DeleteCriticalSection(&lock_); }

    bool init()
    {
        info_ = collectStatic();
        // The installed Lightning of each generation goes into HELLO, so that
        // Thunderhead shows where Run Lightning would land. IShellLink, for the
        // Start menu fallback, wants COM on this thread.
        {
            HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            const int gens[] = { 3, 2 };
            for (int g = 0; g < 2; ++g) {
                LightningInstall li;
                std::string err;
                std::string& path = gens[g] == 3 ? info_.lightning3Path : info_.lightning2Path;
                std::string& version = gens[g] == 3 ? info_.lightning3Version : info_.lightning2Version;
                if (findLightning(gens[g], li, err)) {
                    path = narrow(li.path);
                    version = li.version;
                    logLine(format("%s: %s, version %s, found through %s", lightningLabel(gens[g]), path.c_str(),
                                   version.empty() ? "unknown" : version.c_str(), li.found.c_str()));
                } else {
                    logLine(err);
                }
            }
            if (SUCCEEDED(co)) CoUninitialize();
        }
        status_.info = info_;
        logLine(format("Lightning Rod %s starting: %s mac=%s ip=%s, SMB1 relay %s", TLR_VERSION_STRING,
                       info_.name.c_str(), info_.mac.c_str(), info_.ip.c_str(), relayText().c_str()));
        logLine(format("os=%s cpu=%s %d MHz cores=%d mem=%d MB usable, %d MB installed, type %s disk=%d MB board=%s chipset=%s gpu=%s vram=%d MB",
                       info_.os.c_str(), info_.cpu.c_str(), info_.cpuMhz, info_.cores, info_.memMb, info_.memInstalledMb,
                       info_.memType.empty() ? "n/a" : info_.memType.c_str(), info_.diskTotalMb,
                       info_.board.c_str(), info_.chipset.c_str(), info_.gpu.c_str(), info_.vramMb));
        udp_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (udp_ == INVALID_SOCKET) { logLine("udp socket failed"); return false; }
        BOOL yes = TRUE;
        setsockopt(udp_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
        sockaddr_in any;
        memset(&any, 0, sizeof(any));
        any.sin_family = AF_INET;
        any.sin_addr.s_addr = INADDR_ANY;
        any.sin_port = 0;
        if (bind(udp_, (sockaddr*)&any, sizeof(any)) == SOCKET_ERROR) { logLine("udp bind failed"); return false; }
        return true;
    }

    void run()
    {
        if (!acquireAddress()) return;   // the window thread shows the error and relaunches
        for (;;) {
            if (tcp_ == INVALID_SOCKET) announcePhase();
            else sessionPhase();
        }
    }

    void snapshot(AgentStatus& out)
    {
        EnterCriticalSection(&lock_);
        out = status_;
        LeaveCriticalSection(&lock_);
    }

private:
    void notify()
    {
        if (g_window) PostMessageW(g_window, WM_AGENT_UPDATE, 0, 0);
    }

    // ---- address ---------------------------------------------------------
    // True once the adapter has a valid address. An invalid one at start is
    // re-read every kIpWaitMs up to kIpTries times; when a valid address then
    // appears, Lightning, which started at logon as well and most likely bound
    // the same bad address, is stopped and started again. After kIpTries
    // failures the window is told to show the network error box, and false is
    // returned so that nothing is announced from the bad address.
    bool acquireAddress()
    {
        int tries = 0;
        while (!validAddress(info_.ip)) {
            if (tries == kIpTries) {
                logLine(format("no valid IP address after %d re-reads (last read \"%s\"); network error shown", kIpTries, info_.ip.c_str()));
                if (g_window) PostMessageW(g_window, WM_NETWORK_ERROR, 0, 0);
                return false;
            }
            ++tries;
            logLine(format("no valid IP address (read \"%s\"); waiting %u s, then re-reading, try %d of %d",
                           info_.ip.c_str(), (unsigned)(kIpWaitMs / 1000), tries, kIpTries));
            EnterCriticalSection(&lock_);
            status_.waitingIp = true;
            status_.ipTry = tries;
            status_.badIp = info_.ip;
            LeaveCriticalSection(&lock_);
            notify();
            Sleep(kIpWaitMs);
            collectAddress(info_);
        }
        if (tries == 0) return true;
        logLine(format("valid IP address %s (mac %s) after %d re-read(s)", info_.ip.c_str(), info_.mac.c_str(), tries));
        EnterCriticalSection(&lock_);
        status_.waitingIp = false;
        status_.info = info_;
        LeaveCriticalSection(&lock_);
        notify();
        std::string err;
        std::wstring running = runningLightningPath();   // the same copy is started again, whatever its generation
        LightningInstall li;
        if (findLightningPid() == 0) {
            logLine("Lightning is not running; nothing to restart for the new address");
        } else if (!stopLightning(err)) {
            logLine("Lightning restart for the new address: stop failed: " + err);
        } else if (running.empty() ? !startLightning(3, li, err) : !startLightningAt(running, err)) {
            logLine("Lightning restart for the new address: start failed: " + err);
        } else {
            logLine("Lightning restarted so that it binds the new address");
        }
        return true;
    }

    // ---- announcing ------------------------------------------------------
    void sendAnnounce()
    {
        Message m("ANNOUNCE");
        m.set("mac", info_.mac);
        m.set("name", info_.name);
        m.set("agent", TLR_VERSION_STRING);
        std::string wire = m.encode();
        std::vector<unsigned long> targets = broadcastAddresses();
        targets.push_back(INADDR_BROADCAST);
        for (size_t i = 0; i < targets.size(); ++i) {
            sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = targets[i];
            to.sin_port = htons(kDiscoveryPort);
            sendto(udp_, wire.data(), (int)wire.size(), 0, (sockaddr*)&to, sizeof(to));
        }
    }

    void announcePhase()
    {
        unsigned long long nextAnnounce = 0;
        bool logged = false;
        while (tcp_ == INVALID_SOCKET) {
            unsigned long long now = tickMs();
            if (now >= nextAnnounce) {
                sendAnnounce();
                nextAnnounce = now + (unsigned long long)kAnnounceIntervalSec * 1000;
                if (!logged) { logLine("announcing, waiting for Thunderhead"); logged = true; }
            }
            long long waitMs = (long long)(nextAnnounce - now);
            if (waitMs > 1000) waitMs = 1000;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(udp_, &rd);
            timeval tv; tv.tv_sec = (long)(waitMs / 1000); tv.tv_usec = (long)((waitMs % 1000) * 1000);
            if (select(0, &rd, NULL, NULL, &tv) > 0 && FD_ISSET(udp_, &rd)) {
                char buf[4096];
                sockaddr_in from;
                int fromLen = sizeof(from);
                int n = recvfrom(udp_, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fromLen);
                if (n <= 0) continue;
                Message m;
                if (!parseMessage(std::string(buf, (size_t)n), m) || m.verb != "OFFER") continue;
                int port = m.getInt("port", kDefaultTcpPort);
                sockaddr_in host = from;
                host.sin_port = htons((unsigned short)port);
                logLine(format("OFFER from %s (%s) port %d", ipText(from).c_str(), m.get("host").c_str(), port));
                SOCKET s = connectWithTimeout(host, 3000);
                if (s == INVALID_SOCKET) { logLine("connect failed"); continue; }
                tcp_ = s;
                parser_.clear();
                sent_ = Message();
                if (!sendHello()) { dropSession("hello send failed"); continue; }
                std::string hostIp = ipText(from);
                logLine(format("connected to Thunderhead at %s:%d", hostIp.c_str(), port));
                EnterCriticalSection(&lock_);
                status_.connected = true;
                status_.hostName = m.get("host");
                status_.hostIp = hostIp;
                status_.hostMac = macOf(hostIp, info_);
                status_.haveReport = false;
                status_.report = Message();
                LeaveCriticalSection(&lock_);
                notify();
                nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
                cpu_.sample();
                lightning_.query();
            }
        }
    }

    // ---- session ---------------------------------------------------------
    bool sendMessage(const Message& m)
    {
        return sendAll(tcp_, m.encode());
    }

    bool sendHello()
    {
        bool relay;
        std::string relayIp;
        relaySnapshot(relay, relayIp);
        Message m("HELLO");
        m.set("mac", info_.mac);
        m.set("name", info_.name);
        m.set("agent", TLR_VERSION_STRING);
        m.set("ip", info_.ip);
        m.set("os", info_.os);
        m.set("cpu", info_.cpu);
        m.set("cpu_mhz", (long long)info_.cpuMhz);
        m.set("cores", (long long)info_.cores);
        m.set("mem_mb", (long long)info_.memMb);
        m.set("mem_installed_mb", (long long)info_.memInstalledMb);
        m.set("mem_type", info_.memType);
        m.set("disk_total_mb", (long long)info_.diskTotalMb);
        m.set("board", info_.board);
        m.set("chipset", info_.chipset);
        m.set("gpu", info_.gpu);
        m.set("vram_mb", (long long)info_.vramMb);
        m.set("lightning3_path", info_.lightning3Path);
        m.set("lightning3_version", info_.lightning3Version);
        m.set("lightning2_path", info_.lightning2Path);
        m.set("lightning2_version", info_.lightning2Version);
        m.set("relay", boolText(relay));
        m.set("relay_ip", relay ? relayIp : std::string());
        bool ok = sendMessage(m);
        // The sound of this event waits for Thunderhead's SET, which answers the
        // HELLO within the same exchange: by then a scheme message that names
        // another file for it may have arrived too. Every HELLO plays it, the
        // one sent again after a Settings change as well.
        if (ok) helloSoundPending_ = true;
        return ok;
    }

    bool sendReport()
    {
        DynamicInfo d = collectDynamic(cpu_);
        LightningState l = lightning_.query();
        Message full("REPORT");
        full.set("cpu_pct", (long long)d.cpuPct);
        full.set("mem_free_mb", (long long)d.memFreeMb);
        full.set("disk_free_mb", (long long)d.diskFreeMb);
        full.set("uptime_s", d.uptimeS);
        full.set("lightning", l.running ? "running" : "stopped");
        full.set("lightning_cpu_pct", (long long)l.cpuPct);
        full.set("rendering", boolText(l.rendering));
        // The node's clock: its local date and time as it shows them, and the
        // same instant in UTC milliseconds, from which Thunderhead works out how
        // far the clock is from its own.
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        full.set("clock", format("%s %02d:%02d:%02d", localDateText(lt).c_str(), lt.wHour, lt.wMinute, lt.wSecond));
        full.set("utc_ms", format("%llu", utcNowMs()));
        // ACPI thermal zone; empty when the board exposes none, which Thunderhead shows as n/a.
        int tenths = 0;
        bool haveTemp = thermal_.read(tenths);
        full.set("cpu_temp_c", haveTemp ? format("%d.%d", tenths / 10, tenths % 10) : std::string());
        static bool tempLogged = false;
        if (!tempLogged) {
            tempLogged = true;
            logLine(haveTemp ? format("CPU temperature from the ACPI thermal zone: %d.%d C", tenths / 10, tenths % 10)
                             : std::string("CPU temperature: no ACPI thermal zone reports a value, sending n/a"));
        }
        // Only values that differ from the last report sent in this session go on
        // the wire; the first report after HELLO therefore carries everything, and a
        // report with no values still tells Thunderhead the agent is alive.
        Message m("REPORT");
        for (std::map<std::string, std::string>::const_iterator it = full.fields.begin(); it != full.fields.end(); ++it) {
            std::map<std::string, std::string>::const_iterator old = sent_.fields.find(it->first);
            if (old == sent_.fields.end() || old->second != it->second) m.fields[it->first] = it->second;
        }
        bool ok = sendMessage(m);
        if (ok) {
            sent_ = full;
            EnterCriticalSection(&lock_);
            status_.report = full;
            status_.haveReport = true;
            status_.intervalSec = intervalSec_;
            GetLocalTime(&status_.lastReportTime);
            LeaveCriticalSection(&lock_);
            notify();
            playEventSound(SoundLrReport, false);   // every report sent, the regular ones and those PING asks for
        }
        return ok;
    }

    void dropSession(const char* why)
    {
        logLine(format("session ended: %s", why));
        if (tcp_ != INVALID_SOCKET) { closesocket(tcp_); tcp_ = INVALID_SOCKET; }
        parser_.clear();
        EnterCriticalSection(&lock_);
        status_.connected = false;
        LeaveCriticalSection(&lock_);
        notify();
    }

    void handle(const Message& m)
    {
        if (m.verb == "SET") {
            int iv = m.getInt("interval", intervalSec_);
            if (iv < 1) iv = 1;
            if (iv > 3600) iv = 3600;
            if (iv != intervalSec_) logLine(format("report interval %d s", iv));
            intervalSec_ = iv;
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
            EnterCriticalSection(&lock_);
            status_.intervalSec = intervalSec_;
            LeaveCriticalSection(&lock_);
            notify();
            if (helloSoundPending_) { helloSoundPending_ = false; playEventSound(SoundLrHello); }
        } else if (m.verb == "RELAYSET") {
            // Set Relay Address in Thunderhead: relay=yes|no and relay_ip= as the
            // Settings dialog would set them; HELLO follows again when they changed.
            bool on = parseBool(m.get("relay"));
            std::string ip = trim(m.get("relay_ip"));
            std::string err = applyRelaySetting(on, ip, info_.ip);
            logLine(format("RELAYSET relay=%s relay_ip=%s: %s%s%s", boolText(on), ip.c_str(), err.empty() ? "ok, now " : "failed",
                           err.empty() ? relayText().c_str() : ": ", err.c_str()));
            bool relayNow;
            std::string ipNow;
            relaySnapshot(relayNow, ipNow);
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(err.empty()));
            ack.set("error", err);
            ack.set("relay", boolText(relayNow));
            ack.set("relay_ip", relayNow ? ipNow : std::string());
            sendMessage(ack);
            notify();
        } else if (m.verb == "RELAY") {
            // SMB1 relay work for the Windows XP client this node is set to relay
            // to (Settings): op=probe answers whether \\dest\share\ can be opened
            // from here; op=push copies everything Thunderhead staged under
            // relay\<job> in this node's shared folder to the root of that share;
            // op=pull fetches the newest file matching pattern= from it into
            // relay\<job>. push and pull run on a thread and are acknowledged
            // when they end; dest= must be the address set in Settings.
            std::string op = trim(m.get("op")), job = trim(m.get("job")), dest = trim(m.get("dest")), share = trim(m.get("share")), err;
            bool relay;
            std::string relayIp;
            relaySnapshot(relay, relayIp);
            if (!relay) err = "this agent is not set to act as an SMB1 relay";
            else if (dest != relayIp) err = "this agent relays to " + relayIp + ", not to " + dest;
            else if (share.empty() || share.find_first_of("\\/") != std::string::npos) err = "share name missing";
            else if (op != "probe" && op != "push" && op != "pull") err = "unknown op \"" + op + "\"";
            else if (op != "probe" && (job.empty() || job.find_first_of("\\/:.") != std::string::npos)) err = "job must be a plain name";
            if (err.empty() && op == "probe") {
                std::wstring root = L"\\\\" + widen(dest) + L"\\" + widen(share) + L"\\";
                if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) err = narrow(root) + ": " + lastErrorText((int)GetLastError());
            }
            if (err.empty() && op != "probe") {
                RelayTask* t = new RelayTask();
                t->op = op;
                t->job = job;
                t->dest = dest;
                t->share = share;
                t->pattern = trim(m.get("pattern"));
                if (t->pattern.empty()) t->pattern = "*";
                t->thread = CreateThread(NULL, 0, relayTaskThread, t, 0, NULL);
                if (t->thread) {
                    relays_.push_back(t);
                    logLine(format("RELAY %s job %s to \\\\%s\\%s\\ started", op.c_str(), job.c_str(), dest.c_str(), share.c_str()));
                    return;   // the ACK follows from pollRelays when the copy has ended
                }
                err = "could not start the copy thread";
                delete t;
            }
            logLine(format("RELAY %s%s%s: %s%s%s", op.c_str(), job.empty() ? "" : " job ", job.c_str(), err.empty() ? "ok" : "failed",
                           err.empty() ? "" : ": ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(err.empty()));
            ack.set("error", err);
            ack.set("op", op);
            ack.set("job", job);
            ack.set("dest", dest);
            sendMessage(ack);
        } else if (m.verb == "SOUND") {
            // action=play file=NAME plays NAME from the agent's sounds folder on
            // the node's playback device and answers at once; action=scheme
            // names the files of the agent's own events (see setSoundScheme).
            std::string action = trim(m.get("action")), file = trim(m.get("file")), err;
            bool ok = false;
            if (action == "play") {
                if (file.empty() || file.find_first_of("\\/:") != std::string::npos) err = "file must be a name in the sounds folder";
                else {
                    std::wstring path = soundDir() + L"\\" + widen(file);
                    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) err = file + " is not in " + narrow(soundDir());
                    else ok = soundPlay(path, err);
                }
                logLine(format("SOUND play %s: %s%s%s", file.c_str(), ok ? "ok" : "failed", err.empty() ? "" : ": ", err.c_str()));
            } else if (action == "scheme") {
                std::string set = setSoundScheme(m);
                ok = true;
                logLine("SOUND scheme: " + (set.empty() ? std::string("nothing named") : set));
            } else {
                err = "unknown action \"" + action + "\"";
                logLine("SOUND failed: " + err);
            }
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("action", action);
            ack.set("file", file);
            sendMessage(ack);
        } else if (m.verb == "START" || m.verb == "STOP") {
            // START gen=3|2|1 (3 when absent) starts the installed Lightning of that
            // generation, found as docs/PROTOCOL.md describes under Lightning lookup;
            // the ACK carries what was found even when the start fails.
            std::string err;
            LightningInstall li;
            int gen = m.getInt("gen", 3);
            bool ok = m.verb == "START" ? startLightning(gen, li, err) : stopLightning(err);
            if (m.verb == "START") {
                std::string where = li.path.empty() ? std::string()
                    : format(" [%s, version %s, found through %s]", narrow(li.path).c_str(),
                             li.version.empty() ? "unknown" : li.version.c_str(), li.found.c_str());
                logLine(format("START %s: %s%s%s%s", lightningLabel(gen), ok ? "ok" : "failed", err.empty() ? "" : " ", err.c_str(), where.c_str()));
            } else {
                logLine(format("STOP Lightning: %s%s%s", ok ? "ok" : "failed", err.empty() ? "" : " ", err.c_str()));
            }
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            if (m.verb == "START") {
                ack.set("gen", (long long)gen);
                ack.set("path", narrow(li.path));
                ack.set("version", li.version);
                ack.set("found", li.found);
            }
            sendMessage(ack);
            sendReport();
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
        } else if (m.verb == "PRIORITY") {
            std::string level = trim(m.get("level")), applied, err;
            bool ok = setLightningPriority(level, applied, err);
            logLine(format("PRIORITY %s: %s%s%s", level.c_str(), ok ? "ok" : "failed", err.empty() ? "" : " ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("level", level);
            ack.set("applied", applied);
            sendMessage(ack);
        } else if (m.verb == "AUDIO") {
            // action=mute or unmute: the default playback device's master mute is
            // set, not toggled, and the ACK carries the state read back.
            std::string action = trim(m.get("action")), method, err;
            bool muted = false, ok = false;
            if (action != "mute" && action != "unmute") err = "unknown action \"" + action + "\"";
            else ok = setAudioMute(action == "mute", muted, method, err);
            logLine(format("AUDIO %s: %s%s%s%s", action.c_str(), ok ? "ok" : "failed",
                           method.empty() ? "" : (" via " + method).c_str(), err.empty() ? "" : ": ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("action", action);
            ack.set("muted", boolText(muted));
            ack.set("method", method);
            sendMessage(ack);
        } else if (m.verb == "TEAMVIEWER") {
            // Start TeamViewer: the installed copy, wherever its installer put it, is
            // found through the registry or the Start menu and started as its
            // shortcut would start it; its window is then brought to the front, as a
            // batch window is, since a window opened by a background program stays
            // behind the others. The ACK says where TeamViewer was found and whether
            // it was running before.
            std::wstring path;
            std::string how, err;
            bool wasRunning = false;
            bool ok = startTeamViewer(path, how, wasRunning, err);
            logLine(ok ? format("TEAMVIEWER: started %s, found through %s%s", narrow(path).c_str(), how.c_str(),
                                wasRunning ? ", TeamViewer was already running" : "")
                       : "TEAMVIEWER failed: " + err);
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("path", narrow(path));
            ack.set("found", how);
            ack.set("running", boolText(wasRunning));
            sendMessage(ack);
            if (ok) {
                tvWindowWanted_ = true;
                tvStartMs_ = tickMs();
                // The window exists a moment after the process; a slow node gets more
                // tries from sessionPhase.
                for (int i = 0; i < 20 && tvWindowWanted_; ++i) {
                    tryTeamViewerWindow();
                    if (tvWindowWanted_) Sleep(100);
                }
            }
        } else if (m.verb == "PING") {
            // Report at once, with every value rather than only the changed ones,
            // and restart the report timer from here. Agents before 0.2.11 sent PONG.
            sent_ = Message();
            sendReport();
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
        } else if (m.verb == "UPDATE") {
            // Thunderhead replaces LightningRod.exe through the node's share once this
            // process has exited. A copy of this executable, LightningRod.update.exe,
            // waits for that exit, then for update.go (or two minutes), and starts
            // whatever LightningRod.exe is then in the folder. No other tool is needed.
            std::wstring dir = exeDirectory();
            std::wstring helper = dir + L"\\LightningRod.update.exe";
            wchar_t self[MAX_PATH];
            DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
            bool ok = n > 0 && CopyFileW(self, helper.c_str(), FALSE) != 0;
            if (ok) {
                std::wstring cmd = L"\"" + helper + L"\" --helper " + widen(format("%lu", (unsigned long)GetCurrentProcessId()));
                std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                cmdBuf.push_back(L'\0');
                STARTUPINFOW si;
                PROCESS_INFORMATION pi;
                memset(&si, 0, sizeof(si));
                si.cb = sizeof(si);
                memset(&pi, 0, sizeof(pi));
                ok = CreateProcessW(helper.c_str(), &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi) != 0;
                if (ok) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
            }
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", ok ? "" : lastErrorText((int)GetLastError()));
            sendMessage(ack);
            if (!ok) { logLine("UPDATE: could not start the update helper"); return; }
            logLine("UPDATE received, helper started, stopping for replacement");
            playCloseSound();
            closesocket(tcp_);
            tcp_ = INVALID_SOCKET;
            if (g_singleInstance) { CloseHandle(g_singleInstance); g_singleInstance = NULL; }
            ExitProcess(0);
        } else if (m.verb == "RENAME") {
            std::string name = trim(m.get("name"));
            std::string err = validComputerName(name);
            bool ok = err.empty();
            if (ok) {
                std::wstring wname = widen(name);
                if (!SetComputerNameExW(ComputerNamePhysicalDnsHostname, wname.c_str())) {
                    ok = false;
                    err = lastErrorText((int)GetLastError());
                }
            }
            logLine(format("RENAME to %s: %s%s%s", name.c_str(), ok ? "ok, takes effect at reboot" : "failed",
                           err.empty() ? "" : " ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("name", name);
            sendMessage(ack);
        } else if (m.verb == "REBOOT" || m.verb == "SHUTDOWN") {
            bool reboot = m.verb == "REBOOT";
            std::string err;
            bool ok = enableShutdownPrivilege(err);
            if (ok && !InitiateSystemShutdownExW(NULL, (LPWSTR)L"Thunderhead", 5, TRUE, reboot ? TRUE : FALSE,
                                                 SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED)) {
                ok = false;
                err = lastErrorText((int)GetLastError());
            }
            logLine(format("%s: %s%s%s", m.verb.c_str(), ok ? "ok, in 5 s" : "failed", err.empty() ? "" : " ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            sendMessage(ack);
        } else if (m.verb == "BATCH") {
            // The text arrives as hexadecimal digits of its UTF-8 bytes. This ACK
            // says whether it was started; pollBatches sends a second one when
            // it has ended.
            std::string text, err;
            RunningBatch b;
            bool ok = hexDecode(m.get("text"), text) && !text.empty();
            if (!ok) err = "no batch text, or not hexadecimal";
            bool maximized = parseBool(m.get("maximized")), front = parseBool(m.get("front"));
            if (ok) ok = startBatch(text, maximized, front, b, err);
            if (ok) batches_.push_back(b);
            logLine(ok ? format("BATCH started in a console window as batch\\%s, %u bytes of text%s%s", b.file.c_str(),
                                (unsigned)text.size(), maximized ? ", maximized" : "", front ? ", to the front" : "")
                       : "BATCH failed: " + err);
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("stage", "started");
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("file", ok ? b.file : std::string());
            sendMessage(ack);
        } else if (m.verb == "TIME") {
            // The host's clock: utc_ms= its UTC time in milliseconds since 1970,
            // date= its local calendar date, tolerance= seconds. This clock is set
            // to the host's when the local date differs from the host's or the two
            // UTC times are tolerance or more apart; otherwise it is left alone.
            // The ACK says which, with delta_s= (this clock minus the host's,
            // before any change) and date= (this node's date before any change).
            unsigned long long hostMs = _strtoui64(m.get("utc_ms").c_str(), NULL, 10);
            int tol = m.getInt("tolerance", 180);
            long long deltaMs = (long long)(utcNowMs() - hostMs);
            SYSTEMTIME lt;
            GetLocalTime(&lt);
            std::string date = localDateText(lt), err;
            bool dateWrong = !m.get("date").empty() && m.get("date") != date;
            bool need = hostMs != 0 && (dateWrong || deltaMs >= (long long)tol * 1000 || deltaMs <= -(long long)tol * 1000);
            bool ok = hostMs != 0;
            if (!ok) err = "no utc_ms in the command";
            if (need) {
                ok = enablePrivilege(SE_SYSTEMTIME_NAME, err);
                if (ok) {
                    ULONGLONG t = (ULONGLONG)hostMs * 10000ULL + 116444736000000000ULL;
                    FILETIME ft;
                    ft.dwLowDateTime = (DWORD)t;
                    ft.dwHighDateTime = (DWORD)(t >> 32);
                    SYSTEMTIME st;
                    if (!FileTimeToSystemTime(&ft, &st) || !SetSystemTime(&st)) {
                        ok = false;
                        err = lastErrorText((int)GetLastError());
                    }
                }
            }
            long long deltaS = deltaMs >= 0 ? (deltaMs + 500) / 1000 : -((-deltaMs + 500) / 1000);
            logLine(format("TIME: this clock is %lld s from the host's%s, %s%s%s", deltaS, dateWrong ? ", the date differs" : "",
                           !need ? "left alone" : ok ? "set to the host's time" : "not set", err.empty() ? "" : ": ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("set", boolText(need && ok));
            ack.set("delta_s", (long long)deltaS);
            ack.set("date", date);
            sendMessage(ack);
            if (need && ok) {   // show the corrected clock at once
                sent_ = Message();
                sendReport();
                nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
            }
        } else if (m.verb == "POWER") {
            // Windows' shutdown.exe does the work. The three actions are its -s, -r
            // and -g switches, always with -f (close programs by force) and -t 66
            // (act after 66 seconds); abort is -a, which cancels a shutdown that is
            // still counting down. The dash form is the one every Windows version
            // from XP on accepts.
            std::string action = trim(m.get("action")), err;
            const wchar_t* args = NULL;
            if (action == "shutdown") args = L"-s -f -t 66";
            else if (action == "restart") args = L"-r -f -t 66";
            else if (action == "coldrestart") args = L"-g -f -t 66";
            else if (action == "abort") args = L"-a";
            bool ok = args != NULL;
            if (!ok) err = "unknown action " + action;
            else ok = runShutdownExe(args, err);
            logLine(format("POWER %s (shutdown.exe %s): %s%s%s", action.c_str(), args ? narrow(args).c_str() : "",
                           ok ? "ok" : "failed", err.empty() ? "" : " ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("action", action);
            sendMessage(ack);
        } else if (m.verb == "EXIT" || m.verb == "RESTART") {
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", "yes");
            ack.set("error", "");
            sendMessage(ack);
            logLine(m.verb == "EXIT" ? "EXIT received, stopping" : "RESTART received, relaunching");
            playCloseSound();
            closesocket(tcp_);
            tcp_ = INVALID_SOCKET;
            if (g_singleInstance) { CloseHandle(g_singleInstance); g_singleInstance = NULL; }
            if (m.verb == "RESTART") relaunchSelf();
            ExitProcess(0);
        } else {
            logLine("unknown verb from Thunderhead: " + m.verb);
        }
    }

    // Looks for the TeamViewer window once TEAMVIEWER has started it, brings it
    // to the front, and gives up 30 s after the start.
    void tryTeamViewerWindow()
    {
        if (!tvWindowWanted_) return;
        HWND w = teamViewerWindow();
        if (w) {
            bringToFront(w);
            tvWindowWanted_ = false;
            logLine("TEAMVIEWER: window found, brought to the front");
        } else if (tickMs() - tvStartMs_ > 30000) {
            tvWindowWanted_ = false;
            logLine("TEAMVIEWER: window not found within 30 s, left as Windows opened it");
        }
    }

    // Reports every batch file that has ended since the last pass with a second
    // ACK: stage=finished, the exit code of cmd.exe and the running time.
    bool pollBatches()
    {
        for (size_t i = 0; i < batches_.size(); ) {
            if (WaitForSingleObject(batches_[i].process, 0) != WAIT_OBJECT_0) { tryWindow(batches_[i]); ++i; continue; }
            RunningBatch b = batches_[i];
            batches_.erase(batches_.begin() + i);
            DWORD code = 0;
            GetExitCodeProcess(b.process, &code);
            CloseHandle(b.process);
            long long seconds = (long long)((tickMs() - b.startMs) / 1000);
            logLine(format("BATCH batch\\%s finished with exit code %u after %lld s", b.file.c_str(), (unsigned)code, seconds));
            Message ack("ACK");
            ack.set("cmd", "BATCH");
            ack.set("stage", "finished");
            ack.set("ok", boolText(code == 0));
            ack.set("error", code == 0 ? std::string() : format("exit code %u", (unsigned)code));
            ack.set("file", b.file);
            ack.set("exit", format("%u", (unsigned)code));
            ack.set("seconds", seconds);
            if (!sendMessage(ack)) return false;
        }
        return true;
    }

    // Acknowledges every RELAY push or pull that has ended since the last pass.
    bool pollRelays()
    {
        for (size_t i = 0; i < relays_.size(); ) {
            if (WaitForSingleObject(relays_[i]->thread, 0) != WAIT_OBJECT_0) { ++i; continue; }
            RelayTask* t = relays_[i];
            relays_.erase(relays_.begin() + i);
            CloseHandle(t->thread);
            logLine(format("RELAY %s job %s: %s, %u file(s), %llu bytes%s%s%s", t->op.c_str(), t->job.c_str(), t->ok ? "ok" : "failed",
                           t->files, t->bytes, t->file.empty() ? "" : ", ", t->file.c_str(), t->error.empty() ? "" : (": " + t->error).c_str()));
            Message ack("ACK");
            ack.set("cmd", "RELAY");
            ack.set("ok", boolText(t->ok));
            ack.set("error", t->error);
            ack.set("op", t->op);
            ack.set("job", t->job);
            ack.set("dest", t->dest);
            ack.set("files", (long long)t->files);
            ack.set("bytes", format("%llu", t->bytes));
            ack.set("file", t->file);
            bool sent = sendMessage(ack);
            delete t;
            if (!sent) return false;
        }
        return true;
    }

    void sessionPhase()
    {
        while (tcp_ != INVALID_SOCKET) {
            tryTeamViewerWindow();
            if (!pollBatches()) { dropSession("batch result send failed"); return; }
            if (!pollRelays()) { dropSession("relay result send failed"); return; }
            if (InterlockedExchange(&g_rehello, 0)) {
                if (!sendHello()) { dropSession("hello send failed"); return; }
                logLine("HELLO sent again: SMB1 relay " + relayText());
            }
            unsigned long long now = tickMs();
            if (now >= nextReport_) {
                if (!sendReport()) { dropSession("report send failed"); return; }
                nextReport_ = now + (unsigned long long)intervalSec_ * 1000;
            }
            long long waitMs = (long long)(nextReport_ - now);
            if (waitMs > 1000) waitMs = 1000;
            if (waitMs < 0) waitMs = 0;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(tcp_, &rd);
            FD_SET(udp_, &rd);
            timeval tv; tv.tv_sec = (long)(waitMs / 1000); tv.tv_usec = (long)((waitMs % 1000) * 1000);
            int r = select(0, &rd, NULL, NULL, &tv);
            if (r < 0) { dropSession("select failed"); return; }
            if (r == 0) continue;
            if (FD_ISSET(udp_, &rd)) {
                char buf[4096];
                sockaddr_in from;
                int fromLen = sizeof(from);
                recvfrom(udp_, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);   // stray OFFER; ignored
            }
            if (FD_ISSET(tcp_, &rd)) {
                char buf[8192];
                int n = recv(tcp_, buf, sizeof(buf), 0);
                if (n == 0) { dropSession("closed by Thunderhead"); return; }
                if (n < 0) { dropSession(lastErrorText(WSAGetLastError()).c_str()); return; }
                parser_.feed(buf, (size_t)n);
                if (parser_.overflow()) { dropSession("oversized message"); return; }
                Message m;
                while (tcp_ != INVALID_SOCKET && parser_.next(m)) handle(m);
            }
        }
    }

    SOCKET udp_, tcp_;
    StaticInfo info_;
    CpuTracker cpu_;
    LightningMonitor lightning_;
    ThermalReader thermal_;
    MessageParser parser_;
    Message sent_;                   // every report value as last sent to this host
    std::vector<RunningBatch> batches_;   // batch files started by BATCH and not yet ended
    std::vector<RelayTask*> relays_;      // RELAY copies running and not yet acknowledged
    bool tvWindowWanted_;                 // TEAMVIEWER started TeamViewer and its window is still to be found
    unsigned long long tvStartMs_;
    bool helloSoundPending_;              // HELLO sent; the Proximity Connection sound plays when Thunderhead's SET answers it
    int intervalSec_;
    unsigned long long nextReport_;
    CRITICAL_SECTION lock_;
    AgentStatus status_;
};

Agent* g_agent = NULL;

DWORD WINAPI agentThread(LPVOID)
{
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   // WMI, for the thermal zone
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    g_agent->run();
    return 0;
}

// ---------------------------------------------------------------- window

HWND g_hostText = NULL, g_timeText = NULL, g_reportText = NULL, g_list = NULL;
SYSTEMTIME g_started;
unsigned long long g_startedMs = 0;

std::wstring timeText(const SYSTEMTIME& st)
{
    wchar_t buf[64];
    _snwprintf(buf, 63, L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    buf[63] = L'\0';
    return buf;
}

std::wstring durationText(unsigned long long seconds)
{
    unsigned long long d = seconds / 86400, h = (seconds / 3600) % 24, m = (seconds / 60) % 60, s = seconds % 60;
    wchar_t buf[64];
    if (d > 0) _snwprintf(buf, 63, L"%llud %02llu:%02llu:%02llu", d, h, m, s);
    else _snwprintf(buf, 63, L"%02llu:%02llu:%02llu", h, m, s);
    buf[63] = L'\0';
    return buf;
}

void setRow(int row, const wchar_t* item, const std::wstring& value)
{
    int count = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    LVITEMW it;
    memset(&it, 0, sizeof(it));
    if (row >= count) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = (LPWSTR)item;
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    }
    memset(&it, 0, sizeof(it));
    it.iSubItem = 1;
    it.pszText = (LPWSTR)value.c_str();
    SendMessageW(g_list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
}

void refreshClock()
{
    unsigned long long running = (tickMs() - g_startedMs) / 1000;
    std::wstring text = L"Lightning Rod " TLR_VERSION_WSTRING L" started " + timeText(g_started) + L", running " + durationText(running);
    SetWindowTextW(g_timeText, text.c_str());
}

void refreshAll()
{
    AgentStatus s;
    g_agent->snapshot(s);

    std::wstring host;
    if (s.connected) {
        host = L"Reporting to Thunderhead on " + widen(s.hostName) + L"  (" + widen(s.hostIp) +
               (s.hostMac.empty() ? L"" : L", " + widen(s.hostMac)) + L")";
    } else if (s.waitingIp) {
        host = widen(format("No valid IP address (read \"%s\"): re-reading it in %u s, try %d of %d",
                            s.badIp.c_str(), (unsigned)(kIpWaitMs / 1000), s.ipTry, kIpTries));
    } else {
        host = L"Not connected: announcing, waiting for Thunderhead";
    }
    SetWindowTextW(g_hostText, host.c_str());

    std::wstring rep;
    if (s.connected && s.haveReport)
        rep = widen(format("Report interval %d s, last report sent ", s.intervalSec)) + timeText(s.lastReportTime);
    else if (s.connected)
        rep = widen(format("Report interval %d s, first report pending", s.intervalSec));
    else
        rep = L"";
    SetWindowTextW(g_reportText, rep.c_str());

    const Message& r = s.report;
    const StaticInfo& i = s.info;
    int row = 0;
    setRow(row++, L"CPU load %",          s.haveReport ? widen(r.get("cpu_pct")) : L"");
    setRow(row++, L"CPU temp C",          !s.haveReport ? std::wstring() : (r.get("cpu_temp_c").empty() ? std::wstring(L"n/a") : widen(r.get("cpu_temp_c"))));
    setRow(row++, L"Memory free MB",      s.haveReport ? widen(r.get("mem_free_mb")) : L"");
    setRow(row++, L"C: free MB",          s.haveReport ? widen(r.get("disk_free_mb")) : L"");
    setRow(row++, L"Uptime",              s.haveReport ? durationText((unsigned long long)atoll(r.get("uptime_s").c_str())) : L"");
    setRow(row++, L"Lightning",           s.haveReport ? (r.get("lightning") == "running" ? L"Running" : L"Stopped") : L"");
    setRow(row++, L"Lightning CPU %",     s.haveReport ? widen(r.get("lightning_cpu_pct")) : L"");
    setRow(row++, L"Rendering",           s.haveReport ? (parseBool(r.get("rendering")) ? L"Yes" : L"No") : L"");
    setRow(row++, L"Clock",               s.haveReport ? widen(r.get("clock")) : L"");
    setRow(row++, L"UTC ms since 1970",   s.haveReport ? widen(r.get("utc_ms")) : L"");
    setRow(row++, L"Sent once at connect", L"");
    setRow(row++, L"Name",                widen(i.name));
    setRow(row++, L"IP",                  widen(i.ip));
    setRow(row++, L"MAC",                 widen(i.mac));
    setRow(row++, L"Agent",               TLR_VERSION_WSTRING);
    setRow(row++, L"OS",                  widen(i.os));
    setRow(row++, L"CPU",                 widen(i.cpu));
    setRow(row++, L"MHz",                 widen(format("%d", i.cpuMhz)));
    setRow(row++, L"Cores",               widen(format("%d", i.cores)));
    setRow(row++, L"Memory usable MB",    widen(format("%d", i.memMb)));
    setRow(row++, L"Memory installed MB", i.memInstalledMb > 0 ? widen(format("%d", i.memInstalledMb)) : std::wstring(L"n/a"));
    setRow(row++, L"Memory type",         i.memType.empty() ? std::wstring(L"n/a") : widen(i.memType));
    setRow(row++, L"C: MB",               widen(format("%d", i.diskTotalMb)));
    setRow(row++, L"Board",               widen(i.board));
    setRow(row++, L"Chipset",             widen(i.chipset));
    setRow(row++, L"GPU",                 widen(i.gpu));
    setRow(row++, L"VRAM MB",             widen(format("%d", i.vramMb)));
    setRow(row++, L"Lightning 3 (B7 & 7 Pro)", i.lightning3Path.empty() ? std::wstring(L"not installed")
                                               : widen(i.lightning3Version.empty() ? i.lightning3Path : i.lightning3Version + " at " + i.lightning3Path));
    setRow(row++, L"Lightning 2 (B6.x)", i.lightning2Path.empty() ? std::wstring(L"not installed")
                                        : widen(i.lightning2Version.empty() ? i.lightning2Path : i.lightning2Version + " at " + i.lightning2Path));
    setRow(row++, L"Sounds",              widen(format("%u file(s) in ", (unsigned)soundFilesIn(soundDir()).size())) + soundDir());
    setRow(row++, L"SMB1 relay",          widen(relayText()));
    refreshClock();
}

void layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int margin = 8, line = 18;
    int y = margin;
    MoveWindow(g_hostText, margin, y, rc.right - 2 * margin, line, TRUE);   y += line + 2;
    MoveWindow(g_timeText, margin, y, rc.right - 2 * margin, line, TRUE);   y += line + 2;
    MoveWindow(g_reportText, margin, y, rc.right - 2 * margin, line, TRUE); y += line + 6;
    MoveWindow(g_list, margin, y, rc.right - 2 * margin, rc.bottom - y - margin, TRUE);
    int valueWidth = rc.right - 2 * margin - 150 - GetSystemMetrics(SM_CXVSCROLL);
    if (valueWidth < 100) valueWidth = 100;
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1, (LPARAM)valueWidth);
}

// Settings: Act as SMB1 Relay and the address of the Windows XP client relayed
// to. OK with a change saves settings.txt and has the agent send HELLO again,
// so that Thunderhead learns the new values at once.
INT_PTR CALLBACK settingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        bool relay;
        std::string ip;
        relaySnapshot(relay, ip);
        CheckDlgButton(dlg, IDC_RELAY_CHECK, relay ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dlg, IDC_RELAY_IP, widen(ip).c_str());
        SendDlgItemMessageW(dlg, IDC_RELAY_IP, EM_LIMITTEXT, 15, 0);
        EnableWindow(GetDlgItem(dlg, IDC_RELAY_IP), relay);
        SetDlgItemTextW(dlg, IDC_RELAY_INFO,
                        L"Thunderhead copies files for that client into this node's shared folder, and this agent copies them on "
                        L"to \\\\<address>\\LightningRod\\ over SMB1, which Windows 10 no longer speaks to Windows XP. "
                        L"This node must be able to open that share with its own logon.");
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RELAY_CHECK) {
            bool on = IsDlgButtonChecked(dlg, IDC_RELAY_CHECK) == BST_CHECKED;
            EnableWindow(GetDlgItem(dlg, IDC_RELAY_IP), on);
            if (on) SetFocus(GetDlgItem(dlg, IDC_RELAY_IP));
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            bool on = IsDlgButtonChecked(dlg, IDC_RELAY_CHECK) == BST_CHECKED;
            wchar_t buf[64];
            GetDlgItemTextW(dlg, IDC_RELAY_IP, buf, 64);
            std::string ip = trim(narrow(buf));
            AgentStatus s;
            g_agent->snapshot(s);
            if (on) {
                if (!validIpText(ip)) {
                    MessageBoxW(dlg, L"Enter the IP address of the Windows XP client as four numbers, for example 42.32.22.156.",
                                L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                if (ip == s.info.ip) {
                    MessageBoxW(dlg, L"That is this node's own address. Enter the address of the Windows XP client to relay to.",
                                L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            }
            applyRelaySetting(on, ip, s.info.ip);   // the checks above make it succeed
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: layout(hwnd); return 0;
    case WM_AGENT_UPDATE: refreshAll(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_LR_EXIT: PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_LR_SETTINGS:
            DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_LR_SETTINGS), hwnd, settingsDlgProc);
            refreshAll();
            return 0;
        case IDM_LR_ABOUT:
            MessageBoxW(hwnd, L"Lightning Rod " TLR_VERSION_WSTRING L"\nNode agent of the Bryce Lightning render farm.\n\n"
                        L"Reports to Thunderhead " TLR_VERSION_WSTRING L" and carries out its commands on this node.",
                        L"About Lightning Rod", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        return 0;
    case WM_NETWORK_ERROR:
        // MB_ICONERROR plays the Critical Stop sound; OK starts a fresh agent.
        SetWindowTextW(g_hostText, L"Network Error: Invalid IP address");
        MessageBoxW(hwnd, L"Network Error: Invalid IP address. Check network settings or hardware and restart Lightning Rod.",
                    L"Lightning Rod " TLR_VERSION_WSTRING, MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
        logLine("network error acknowledged, relaunching");
        relaunchSelf();
        ExitProcess(0);
    case WM_TIMER: if (wp == kClockTimer) refreshClock(); return 0;
    case WM_CLOSE: playCloseSound(); DestroyWindow(hwnd); return 0;
    case WM_DESTROY: KillTimer(hwnd, kClockTimer); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HFONT uiFont()
{
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return CreateFontIndirectW(&ncm.lfMessageFont);
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

bool createWindow(HINSTANCE inst, const std::string& nodeName)
{
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_LR_APP));
    wc.lpszClassName = L"LightningRodMain";
    wc.lpszMenuName = MAKEINTRESOURCEW(IDR_LR_MENU);
    RegisterClassW(&wc);

    std::wstring title = L"Lightning Rod " TLR_VERSION_WSTRING L" on " + widen(nodeName);
    g_window = CreateWindowExW(0, L"LightningRodMain", title.c_str(), WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 560, NULL, NULL, inst, NULL);
    if (!g_window) return false;
    HFONT font = uiFont();
    g_hostText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, g_window, NULL, inst, NULL);
    g_timeText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, g_window, NULL, inst, NULL);
    g_reportText = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, g_window, NULL, inst, NULL);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER,
                             0, 0, 0, 0, g_window, NULL, inst, NULL);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    SendMessageW(g_hostText, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_timeText, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_reportText, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)font, TRUE);
    LVCOLUMNW col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (LPWSTR)L"Item";
    col.cx = 150;
    col.iSubItem = 0;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (LPWSTR)L"Value";
    col.cx = 440;
    col.iSubItem = 1;
    SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    layout(g_window);
    return true;
}

// Update helper mode: wait for the old agent (pid) to exit, then for update.go or
// two minutes, then start LightningRod.exe from this folder.
int runUpdateHelper(DWORD pid)
{
    std::wstring dir = exeDirectory();
    HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (old) { WaitForSingleObject(old, 120000); CloseHandle(old); }
    std::wstring staged = dir + L"\\LightningRod.new";
    std::wstring exe = dir + L"\\LightningRod.exe";
    std::wstring go = dir + L"\\update.go";
    // Thunderhead 0.2.5 and later stage the new agent as LightningRod.new before
    // they send UPDATE, so it is swapped in here, locally, as soon as the old file
    // closes; nothing then depends on the network or on how fast this node is.
    // Older Thunderheads replace LightningRod.exe over the share after the
    // connection drops and then create update.go; that is waited for, at most
    // five minutes, after which whatever LightningRod.exe is present starts.
    for (int i = 0; i < 600; ++i) {
        if (GetFileAttributesW(staged.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (MoveFileExW(staged.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) break;
        } else if (GetFileAttributesW(go.c_str()) != INVALID_FILE_ATTRIBUTES) {
            break;
        }
        Sleep(500);
    }
    DeleteFileW(go.c_str());
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    for (int attempt = 0; attempt < 60; ++attempt) {       // the new file may still be closing
        if (CreateProcessW(exe.c_str(), &cmdBuf[0], NULL, NULL, FALSE, 0, NULL, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return 0;
        }
        Sleep(1000);
    }
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && strcmp(argv[1], "--helper") == 0) return runUpdateHelper((DWORD)strtoul(argv[2], NULL, 10));
    g_singleInstance = CreateMutexW(NULL, TRUE, L"Local\\LightningRodAgent");
    if (g_singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    GetLocalTime(&g_started);
    g_startedMs = tickMs();
    g_log.open(exeDirectory(), L"LightningRod");
    InitializeCriticalSection(&g_soundLock);
    InitializeCriticalSection(&g_settingsLock);
    loadSoundScheme();
    loadSettings();
    removeTree(relayDir() + L"\\");   // staging left by RELAY jobs of a previous run
    DeleteFileW((exeDirectory() + L"\\LightningRod.update.exe").c_str());   // left by the previous update
    DeleteFileW((exeDirectory() + L"\\update.go").c_str());
    DeleteFileW((exeDirectory() + L"\\LightningRod.new").c_str());
    if (!netInit()) { logLine("WSAStartup failed"); return 1; }

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    Agent agent;
    g_agent = &agent;
    if (!agent.init()) return 1;

    AgentStatus s;
    agent.snapshot(s);
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!createWindow(inst, s.info.name)) return 1;
    refreshAll();
    ShowWindow(g_window, SW_SHOWNORMAL);
    UpdateWindow(g_window);
    SetTimer(g_window, kClockTimer, 1000, NULL);
    playEventSound(SoundLrOpen);

    HANDLE thread = CreateThread(NULL, 0, agentThread, NULL, 0, NULL);
    if (!thread) { logLine("agent thread failed"); return 1; }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    logLine("window closed, agent stopping");
    g_window = NULL;
    soundShutdown();
    netShutdown();
    ExitProcess(0);
}
