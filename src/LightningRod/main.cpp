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
#include "Bryce.h"
#include "BryceScene.h"
#include "Sound.h"
#include "Auth.h"
#include "resource.h"

#include <commctrl.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <lm.h>
#include <objbase.h>
#include <cstring>
#include <cwchar>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace tlr;

namespace {

const UINT WM_AGENT_UPDATE = WM_APP + 1;
const UINT WM_SHOW_MESSAGE = WM_APP + 3;    // lParam: ShownMessage* from the agent thread; the window thread opens the window and owns it
// A node whose NIC has not finished DHCP when the agent starts at logon reads
// no address, 127.x or an APIPA 169.254.x.y one, or lists no adapter with a
// working address at all. The agent then re-reads the adapters every kIpWaitMs
// until a working address appears, sounding the alarm at every re-read; from
// the kIpTries-th re-read on, its window shows the network error as well. It
// never gives up: a lease that arrives late is picked up at the next re-read.
const int kIpTries = 4;
const DWORD kIpWaitMs = 60000;
// Announcing this long without an acceptable OFFER (no Thunderhead on an
// announced sub-net) sounds the alarm, and again every kAnnounceAlarmMs.
const unsigned long long kAnnounceAlarmMs = 60000;
// While announcing or in a session the adapters are re-read this often, so
// that a lease lost, renewed with another address, or arriving late is noticed.
const unsigned long long kNetRereadMs = 30000;
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
bool g_renderHost = false;       // Bryce 3D Network Render Host (B3NDHR) mode: render_host= in settings.txt, reported in HELLO and REPORT
std::wstring g_renderBin;        // RENDER.BIN, the folder the host writes the frames to: render_bin= in settings.txt; empty = <this folder>\RENDER.BIN
volatile long g_rehello = 0;     // set by the window thread: send HELLO again with the new values
// The network stack of the Settings dialog: every adapter with a working
// address, which sub-nets the agent announces on, and the preferred adapter,
// whose address and MAC identify the node and whose sub-net's host is chosen
// first. net_preferred= holds that adapter's GUID (empty: the best adapter by
// type and gateway), net_off= the GUIDs of the adapters not announced on.
std::string g_netPreferred;
std::vector<std::string> g_netOff;
volatile long g_renet = 0;       // set by the window thread: the network settings changed
const unsigned long long kOfferHoldMs = 1500;   // an OFFER from another announced sub-net waits this long for one from the preferred sub-net

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
        else if (key == "render_host") g_renderHost = parseBool(value);
        else if (key == "render_bin") g_renderBin = widen(value);
        else if (key == "net_preferred") g_netPreferred = toLower(value);
        else if (key == "net_off") {
            std::vector<std::string> ids = split(value, ',');
            for (size_t k = 0; k < ids.size(); ++k) if (!trim(ids[k]).empty()) g_netOff.push_back(toLower(trim(ids[k])));
        }
    }
    if (g_relayIp.empty()) g_relay = false;
}

std::string joinIds(const std::vector<std::string>& ids)
{
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) out += (i ? "," : "") + ids[i];
    return out;
}

void saveSettings()
{
    std::string text = std::string("relay=") + boolText(g_relay) + "\r\nrelay_ip=" + g_relayIp + "\r\n" +
                       "render_host=" + boolText(g_renderHost) + "\r\n" + "render_bin=" + narrow(g_renderBin) + "\r\n" +
                       "net_preferred=" + g_netPreferred + "\r\n" + "net_off=" + joinIds(g_netOff) + "\r\n";
    if (!writeTextFile(settingsFile(), text)) logLine("settings.txt could not be written: " + lastErrorText((int)GetLastError()));
}

void netSnapshot(std::string& preferred, std::vector<std::string>& off)
{
    EnterCriticalSection(&g_settingsLock);
    preferred = g_netPreferred;
    off = g_netOff;
    LeaveCriticalSection(&g_settingsLock);
}

bool netIsOff(const std::vector<std::string>& off, const std::string& id)
{
    for (size_t i = 0; i < off.size(); ++i) if (off[i] == id) return true;
    return false;
}

// The Settings dialog's network values: saved, and the agent thread told to
// re-read the adapters, announce again where needed and send HELLO again.
void applyNetSettings(const std::string& preferred, const std::vector<std::string>& off)
{
    EnterCriticalSection(&g_settingsLock);
    bool changed = preferred != g_netPreferred || off != g_netOff;
    g_netPreferred = preferred;
    g_netOff = off;
    if (changed) saveSettings();
    LeaveCriticalSection(&g_settingsLock);
    if (changed) {
        logLine("network settings changed: preferred adapter " + (preferred.empty() ? std::string("automatic") : preferred) +
                ", not announced on " + (off.empty() ? std::string("none") : joinIds(off)));
        InterlockedExchange(&g_renet, 1);
    }
}

// One adapter as the Details windows of both programs show it and as HELLO
// carries it: address/prefix, Windows network category, whether the agent
// announces on it, the adapter's description and MAC, and which of them is
// the preferred one and the identity (the address and MAC the node reports).
std::string netText(const NetAdapter& n, const std::string& identityIp, const std::string& preferred, const std::vector<std::string>& off)
{
    std::string s = format("%s/%d, %s, %s, %s, %s", n.ip.c_str(), n.prefix(), n.category.c_str(),
                           netIsOff(off, n.id) ? "not announced" : "announcing", n.desc.c_str(), n.mac.c_str());
    if (n.id == preferred) s += ", preferred";
    if (n.ip == identityIp) s += ", identity";
    return s;
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

bool renderHostSetting()
{
    EnterCriticalSection(&g_settingsLock);
    bool on = g_renderHost;
    LeaveCriticalSection(&g_settingsLock);
    return on;
}

// ---- RENDER.BIN -------------------------------------------------------------
// The folder the Bryce 3D Network Render Host writes the frames to. The default
// is RENDER.BIN in this agent's folder, the shared one, so that it is reachable
// as \\node\LightningRod\RENDER.BIN; the Settings dialog or Thunderhead
// (RENDERHOST render_bin=) can name another folder on this node.
std::wstring renderBinDefault() { return exeDirectory() + L"\\RENDER.BIN"; }

std::wstring renderBinSetting()
{
    EnterCriticalSection(&g_settingsLock);
    std::wstring p = g_renderBin;
    LeaveCriticalSection(&g_settingsLock);
    return p.empty() ? renderBinDefault() : p;
}

bool renderBinExists()
{
    DWORD a = GetFileAttributesW(renderBinSetting().c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The path below this agent's folder when RENDER.BIN lies inside it (what
// Thunderhead appends to the share), empty when it lies elsewhere.
std::string renderBinInShare()
{
    std::wstring p = renderBinSetting(), base = exeDirectory() + L"\\";
    if (p.size() > base.size() && _wcsnicmp(p.c_str(), base.c_str(), base.size()) == 0) return narrow(p.substr(base.size()));
    return std::string();
}

// Creates a folder and the folders above it; true when it exists afterwards.
bool createFolder(const std::wstring& path)
{
    int r = SHCreateDirectoryExW(NULL, path.c_str(), NULL);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

// Sets the RENDER.BIN folder (empty for the default), saves it, and creates it
// when this node is the render host; says whether it changed.
bool applyRenderBin(const std::wstring& path, bool& changed)
{
    std::wstring p = trim(path);
    if (p == renderBinDefault()) p.clear();
    EnterCriticalSection(&g_settingsLock);
    changed = g_renderBin != p;
    g_renderBin = p;
    saveSettings();
    LeaveCriticalSection(&g_settingsLock);
    if (changed) logLine("RENDER.BIN folder set to " + narrow(renderBinSetting()));
    if (!renderHostSetting()) return true;
    if (renderBinExists()) return true;
    bool ok = createFolder(renderBinSetting());
    logLine("RENDER.BIN folder " + narrow(renderBinSetting()) + (ok ? " created" : " could not be created: " + lastErrorText((int)GetLastError())));
    return ok;
}

// Sets the B3NDHR mode (Settings dialog on the window thread, RENDERHOST from
// Thunderhead on the agent thread), saves it, and says whether it changed.
bool applyRenderHostSetting(bool on)
{
    EnterCriticalSection(&g_settingsLock);
    bool changed = g_renderHost != on;
    g_renderHost = on;
    saveSettings();
    LeaveCriticalSection(&g_settingsLock);
    if (changed) logLine(std::string("Bryce 3D Network Render Host mode ") + (on ? "on" : "off"));
    if (on && !renderBinExists()) {
        bool ok = createFolder(renderBinSetting());
        logLine("RENDER.BIN folder " + narrow(renderBinSetting()) + (ok ? " created" : " could not be created: " + lastErrorText((int)GetLastError())));
    }
    return changed;
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

// The relay destination as the Settings dialog and RELAYSET accept it: a dotted
// IPv4 address, or the Windows XP client's computer name (letters, digits,
// hyphens and dots, 63 characters at most), which the file-sharing path
// \\NAME\share resolves on this node by NetBIOS or DNS. A name keeps the relay
// working when the client's DHCP lease changes its address.
bool validRelayTarget(const std::string& s)
{
    if (validIpText(s)) return true;
    if (s.empty() || s.size() > 63) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    return s[0] != '-' && s[0] != '.';
}

// This node's own computer name, for the own-address checks of the relay settings.
std::string ownComputerName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
    return GetComputerNameW(name, &len) ? narrow(name) : std::string();
}

// True when a RELAY's dest= names the destination this node is set to relay
// to: the same text (names compare without case), or an address the configured
// name resolves to, or a name resolving to the configured address.
bool destMatchesRelay(const std::string& dest, const std::string& relayTo)
{
    if (_stricmp(dest.c_str(), relayTo.c_str()) == 0) return true;
    std::string a = dest, b = relayTo;
    for (int side = 0; side < 2; ++side) {
        const std::string& name = side == 0 ? b : a;
        const std::string& addr = side == 0 ? a : b;
        if (validIpText(name) || !validIpText(addr)) continue;
        addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        if (getaddrinfo(name.c_str(), NULL, &hints, &res) != 0 || !res) continue;
        bool found = false;
        for (addrinfo* p = res; p && !found; p = p->ai_next) {
            char text[32] = "";
            const sockaddr_in* sin = (const sockaddr_in*)p->ai_addr;
            if (p->ai_family == AF_INET && sin) {
                const unsigned char* bb = (const unsigned char*)&sin->sin_addr;
                _snprintf_s(text, sizeof(text), _TRUNCATE, "%u.%u.%u.%u", bb[0], bb[1], bb[2], bb[3]);
                found = addr == text;
            }
        }
        freeaddrinfo(res);
        if (found) return true;
    }
    return false;
}

// Sets the relay values as the Settings dialog or Thunderhead's RELAYSET asks,
// saves them, and has HELLO sent again when they changed; off keeps the last
// destination for the dialog. Returns the error text, empty when applied.
std::string applyRelaySetting(bool on, const std::string& ip, const std::string& ownIp)
{
    if (on && !validRelayTarget(ip)) return "relay_ip must be an IPv4 address of four numbers or the client's computer name";
    if (on && ip == ownIp) return "relay_ip " + ip + " is this node's own address";
    if (on && _stricmp(ip.c_str(), ownComputerName().c_str()) == 0) return "relay_ip " + ip + " is this node's own name";
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
    if (t->op == "probe") {
        // Whether the destination share opens from here. On a thread like the
        // copies, because Windows takes minutes to give up on an address that
        // has no host, and the agent must keep reporting meanwhile.
        if (GetFileAttributesW(destRoot.c_str()) == INVALID_FILE_ATTRIBUTES) t->error = narrow(destRoot) + ": " + lastErrorText((int)GetLastError());
        else t->ok = true;
    } else if (t->op == "push") {
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
// the agent update. Each of the agent's events plays the default file of
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
    bool keyed;                      // farm.key is present beside the agent
    bool hostAuthenticated;          // the OFFER of the current session proved the key
    int intervalSec;
    SYSTEMTIME lastReportTime;
    bool haveReport;
    StaticInfo info;
    std::vector<NetAdapter> nets;    // every adapter with a working address, as last read
    Message report;
    AgentStatus() : connected(false), waitingIp(false), ipTry(0), keyed(false), hostAuthenticated(false), intervalSec(kDefaultReportInterval), haveReport(false) { memset(&lastReportTime, 0, sizeof(lastReportTime)); }
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
        logLine(format("BATCH %s: window found%s%s", b.file.c_str(), b.maximized ? ", maximized" : "",
                       b.front ? ", brought to the front" : ""));
    } else if (tickMs() - b.startMs > 30000) {
        b.wantWindow = false;
        logLine("BATCH " + b.file + ": window not found within 30 s, left as Windows opened it");
    }
}

// Writes the text as batch\batch-YYYYMMDD-HHMMSS.bat beside the agent and starts
// it with cmd.exe in a console window of its own on the node's desktop, as when
// the file is double-clicked, with the agent's folder (the shared one) as current
// directory. Output shows in that window, a pause waits for a key there, and the
// window closes when the batch file ends. maximized opens the window maximized;
// front brings it above the other windows, which would otherwise keep a window
// opened by a background program behind them with only its taskbar button showing.
// The real System32: through the Sysnative alias when this 32-bit process runs
// on a 64-bit Windows, otherwise GetSystemDirectory. Empty on failure.
std::wstring nativeSystemDirectory()
{
    BOOL wow64 = FALSE;
    typedef BOOL (WINAPI* IsWow64ProcessFn)(HANDLE, PBOOL);
    IsWow64ProcessFn fn = (IsWow64ProcessFn)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (fn && fn(GetCurrentProcess(), &wow64) && wow64) {
        wchar_t win[MAX_PATH];
        UINT n = GetWindowsDirectoryW(win, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring sysnative = std::wstring(win, n) + L"\\Sysnative";
            if (GetFileAttributesW((sysnative + L"\\cmd.exe").c_str()) != INVALID_FILE_ATTRIBUTES) return sysnative;
        }
    }
    wchar_t sys[MAX_PATH];
    UINT sn = GetSystemDirectoryW(sys, MAX_PATH);
    if (sn == 0 || sn >= MAX_PATH) return std::wstring();
    return std::wstring(sys, sn);
}

bool startBatch(const std::string& utf8, const std::string& kind, const std::string& exeName, bool maximized, bool front,
                RunningBatch& out, std::string& err)
{
    // kind=bat and kind=ps1 arrive as text and are written to batch\ and run in a
    // console window of their own (cmd.exe, or powershell.exe with the execution
    // policy bypassed for that run); kind=exe names a file already in this folder
    // (the shared one), run from its own folder with no window of the agent's
    // making, so maximized applies and front cannot (its title is not known).
    std::wstring dir = exeDirectory(), exe, cmd, title, workDir = dir;
    std::string display;
    DWORD flags = 0;
    std::vector<wchar_t> titleBuf;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (kind == "exe") {
        if (exeName.empty() || exeName.find(':') != std::string::npos || exeName[0] == '\\' || exeName[0] == '/' ||
            exeName.find("..") != std::string::npos) {
            err = "the executable must be a name or a relative path inside the shared folder";
            return false;
        }
        std::wstring rel = widen(exeName);
        for (size_t i = 0; i < rel.size(); ++i) if (rel[i] == L'/') rel[i] = L'\\';
        exe = dir + L"\\" + rel;
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { err = narrow(rel) + " does not exist in " + narrow(dir); return false; }
        cmd = L"\"" + exe + L"\"";
        workDir = exe.substr(0, exe.rfind(L'\\'));
        display = narrow(rel);
    } else {
        bool ps = kind == "ps1";
        std::wstring folder = dir + L"\\batch", ext = ps ? L".ps1" : L".bat";
        if (!CreateDirectoryW(folder.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            err = "batch folder: " + lastErrorText((int)GetLastError());
            return false;
        }
        // Both readers want CR LF line ends; cmd.exe reads the OEM code page and
        // PowerShell 2.0 reads UTF-8 when the byte order mark says so.
        std::wstring wide = widen(utf8), text;
        for (size_t i = 0; i < wide.size(); ++i) {
            if (wide[i] == L'\r') continue;
            if (wide[i] == L'\n') text += L"\r\n"; else text += wide[i];
        }
        if (text.size() < 2 || text.compare(text.size() - 2, 2, L"\r\n") != 0) text += L"\r\n";
        std::string bytes;
        if (ps) {
            bytes = "\xEF\xBB\xBF" + narrow(text);
        } else {
            int n = WideCharToMultiByte(CP_OEMCP, 0, text.data(), (int)text.size(), NULL, 0, NULL, NULL);
            bytes.assign((size_t)n, '\0');
            if (n) WideCharToMultiByte(CP_OEMCP, 0, text.data(), (int)text.size(), &bytes[0], n, NULL, NULL);
        }

        SYSTEMTIME t;
        GetLocalTime(&t);
        std::string base = format("batch-%04d%02d%02d-%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond), name;
        HANDLE f = INVALID_HANDLE_VALUE;
        for (int i = 0; i < 100 && f == INVALID_HANDLE_VALUE; ++i) {
            name = i == 0 ? base : format("%s-%d", base.c_str(), i + 1);
            f = CreateFileW((folder + L"\\" + widen(name) + ext).c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) break;
        }
        if (f == INVALID_HANDLE_VALUE) { err = "script file: " + lastErrorText((int)GetLastError()); return false; }
        DWORD wrote = 0;
        bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &wrote, NULL) && wrote == bytes.size();
        DWORD writeErr = GetLastError();
        CloseHandle(f);
        if (!ok) { err = "script file: " + lastErrorText((int)writeErr); return false; }

        // The 64-bit shells on a 64-bit Windows: this agent is a 32-bit program,
        // and the System32 it sees is the redirected SysWOW64, whose cmd.exe and
        // powershell.exe would run the script 32-bit, with the 32-bit System32
        // and the WOW6432Node registry in place of the real ones. Sysnative is
        // the alias through which a 32-bit process reaches the real System32
        // (Vista and later); on a 32-bit Windows it does not exist and System32 is used.
        std::wstring sysDir = nativeSystemDirectory();
        if (sysDir.empty()) { err = "system directory: " + lastErrorText((int)GetLastError()); return false; }
        std::wstring script = folder + L"\\" + widen(name) + ext;
        if (ps) {
            exe = sysDir + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
            cmd = L"\"" + exe + L"\" -NoProfile -ExecutionPolicy Bypass -File \"" + script + L"\"";
        } else {
            exe = sysDir + L"\\cmd.exe";
            // cmd.exe removes the outer pair of quotes and runs what is between them.
            cmd = L"\"" + exe + L"\" /d /c \"\"" + script + L"\"\"";
        }
        display = "batch\\" + name + narrow(ext);
        title = std::wstring(L"Thunderhead ") + (ps ? L"script " : L"batch file ") + widen(name) + ext;
        titleBuf.assign(title.begin(), title.end());
        titleBuf.push_back(L'\0');
        si.lpTitle = &titleBuf[0];
        flags = CREATE_NEW_CONSOLE;
    }
    if (maximized) {
        si.dwFlags |= STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMAXIMIZED;
    }
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), &cmdBuf[0], NULL, NULL, FALSE, flags, NULL, workDir.c_str(), &si, &pi)) {
        err = narrow(exe.substr(exe.rfind(L'\\') + 1)) + " could not be started: " + lastErrorText((int)GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    if (kind != "exe") logLine(format("BATCH %s runs through %s", display.c_str(), narrow(exe).c_str()));
    out.process = pi.hProcess;
    out.file = display;
    out.startMs = tickMs();
    out.title = title;
    out.maximized = maximized;
    out.front = front;
    out.wantWindow = !title.empty() && (maximized || front);
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

// ---- Display Message ------------------------------------------------------
// MESSAGE from Thunderhead opens a window on this node's screen: the title in
// its caption, the text in a large font, an OK button beside the usual caption
// buttons and, when timed, a line counting down to the moment it closes by
// itself. The agent thread hands the window thread a ShownMessage; when the
// window closes, the window thread queues the outcome, which pollMessages
// reports to Thunderhead in a second ACK.
struct ShownMessage {
    std::wstring title, text;
    int timeoutSec;                  // 0: stays until dismissed
    unsigned long long startMs;
    HWND window, textCtl, countdown, okButton;
    HFONT textFont, uiFontHandle;
    ShownMessage() : timeoutSec(0), startMs(0), window(NULL), textCtl(NULL), countdown(NULL), okButton(NULL), textFont(NULL), uiFontHandle(NULL) {}
};
struct MessageOutcome { std::string title; std::string closed; long long seconds; };   // closed: ok, close or timeout
CRITICAL_SECTION g_messageLock;
std::vector<MessageOutcome> g_messageOutcomes;   // windows closed and not yet acknowledged
void openMessageWindow(ShownMessage* sm);        // window thread

void takeMessageOutcomes(std::vector<MessageOutcome>& out)
{
    EnterCriticalSection(&g_messageLock);
    out.swap(g_messageOutcomes);
    g_messageOutcomes.clear();
    LeaveCriticalSection(&g_messageLock);
}

// Writes a fresh neutral 50 % gray image, at the scene's render resolution,
// beside the scene under the scene's name, replacing whatever is there: with
// "Image with Scene Open/Save" on, Bryce then opens the scene showing this
// image instead of starting (or resuming) a render. Text for the log and ACK.
std::string grayImageBeside(const std::wstring& scene)
{
    if (scene.empty()) return "no scene to protect (none given and no job.txt)";
    if (GetFileAttributesW(scene.c_str()) == INVALID_FILE_ATTRIBUTES) return "scene " + narrow(scene) + " does not exist";
    BryceSceneInfo info;
    std::string err;
    if (!readBryceScene(scene, info, err)) return "resolution of " + narrow(scene) + " not read: " + err;
    size_t slash = scene.find_last_of(L"\\/");
    size_t dot = scene.rfind(L'.');
    std::wstring bmp = (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? scene : scene.substr(0, dot)) + L".bmp";
    bool had = GetFileAttributesW(bmp.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!writeSolidBmp(bmp, info.width, info.height, 128, err)) return narrow(bmp) + " could not be written: " + err;
    return format("%dx%d gray written as %s%s", info.width, info.height, narrow(bmp).c_str(), had ? " (replaced the one there)" : "");
}

class Agent {
public:
    Agent() : udp_(INVALID_SOCKET), tcp_(INVALID_SOCKET), intervalSec_(kDefaultReportInterval), nextReport_(0)
    {
        tvWindowWanted_ = false;
        tvStartMs_ = 0;
        helloSoundPending_ = false;
        hostAuthenticated_ = false;
        lastKeyPollMs_ = 0;
        lastRejectLogMs_ = 0;
        bryceWindowWanted_ = false;
        bryceStartMs_ = 0;
        hostAddrN_ = 0;
        InitializeCriticalSection(&lock_);
    }
    ~Agent() { DeleteCriticalSection(&lock_); }

    bool init()
    {
        // The installed Lightning of each generation goes into HELLO, so that
        // Thunderhead shows where Run Lightning would land. IShellLink, for the
        // Start menu fallback, wants COM on this thread, as does the Network
        // List Manager that names each adapter's network category.
        {
            HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
            std::string pref;
            std::vector<std::string> off;
            netSnapshot(pref, off);
            info_ = collectStatic(pref, nets_);
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
            // The full Bryce 3D of each generation as well, for the Bryce 3D Network Render Host role.
            for (int g = 3; g >= 1; --g) {
                BryceInstall bi;
                std::string err;
                if (findBryce(g, bi, err)) {
                    info_.brycePath[g] = narrow(bi.path);
                    info_.bryceVersion[g] = bi.version;
                    logLine(format("%s: %s, version %s, found through %s", bryceLabel(g), info_.brycePath[g].c_str(),
                                   bi.version.empty() ? "unknown" : bi.version.c_str(), bi.found.c_str()));
                } else {
                    logLine(err);
                }
            }
            if (SUCCEEDED(co)) CoUninitialize();
        }
        status_.info = info_;
        status_.nets = nets_;
        loadKey();
        logLine(format("Lightning Rod %s starting: %s mac=%s ip=%s, SMB1 relay %s, farm.key %s, render host %s", TLR_VERSION_STRING,
                       info_.name.c_str(), info_.mac.c_str(), info_.ip.c_str(), relayText().c_str(),
                       key_.empty() ? "absent (Thunderhead writes it into this folder through the share)" : "present",
                       renderHostSetting() ? "on" : "off"));
        logNetworks();
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
        for (bool first = true;; first = false) {
            if (!networkReady()) { acquireAddress(); logNetworks(); }
            else if (first) logNetworks();
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

    // ---- networks --------------------------------------------------------
    // Re-reads the adapters and the identity address under the Settings in
    // force. COM is needed for the network categories; the agent thread holds
    // none between calls.
    void rereadNetworks()
    {
        std::string pref;
        std::vector<std::string> off;
        netSnapshot(pref, off);
        HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        collectAddress(info_, pref, nets_);
        if (SUCCEEDED(co)) CoUninitialize();
        EnterCriticalSection(&lock_);
        status_.info = info_;
        status_.nets = nets_;
        LeaveCriticalSection(&lock_);
        notify();
    }

    void logNetworks()
    {
        std::string pref;
        std::vector<std::string> off;
        netSnapshot(pref, off);
        if (nets_.empty()) { logLine("networks: no adapter holds a working address"); return; }
        for (size_t i = 0; i < nets_.size(); ++i) logLine(format("network %u: ", (unsigned)i + 1) + netText(nets_[i], info_.ip, pref, off));
        const NetAdapter* p = pickAdapter(nets_, pref);
        if (!pref.empty() && p && p->id != pref)
            logLine("the preferred adapter " + pref + " holds no working address; the best adapter identifies the node instead");
    }

    // True when the node can work: the identity address is valid and at least
    // one adapter holds a working address (the two differ when the adapter list
    // came back empty while a plain address read still succeeded).
    bool networkReady() const { return validAddress(info_.ip) && !nets_.empty(); }

    // A key of the adapter list as last read, to notice a change between re-reads.
    std::string netsKey() const
    {
        std::string k;
        for (size_t i = 0; i < nets_.size(); ++i) k += nets_[i].id + "=" + nets_[i].ip + "/" + nets_[i].mask + ";";
        return k;
    }

    // The announced adapter whose sub-net holds the address; NULL when none does.
    const NetAdapter* announcedSubnetOf(unsigned long addrN) const
    {
        std::string pref;
        std::vector<std::string> off;
        netSnapshot(pref, off);
        for (size_t i = 0; i < nets_.size(); ++i)
            if (!netIsOff(off, nets_[i].id) && nets_[i].sameSubnet(addrN)) return &nets_[i];
        return NULL;
    }

    // ---- address ---------------------------------------------------------
    // Returns once an adapter holds a working address. Until then the adapters
    // are re-read every kIpWaitMs, without limit, the alarm sounding before
    // each wait; from the kIpTries-th re-read on the window shows the network
    // error too. When a working address then appears, Lightning, which started
    // at logon as well and most likely bound the bad address, is stopped and
    // started again. Nothing is announced from a bad address.
    void acquireAddress()
    {
        int tries = 0;
        while (!networkReady()) {
            if (tries == 0) {
                // The alarm must be heard: the node has no network, so no AUDIO unmute
                // from Thunderhead can reach it, and a mute left from an earlier session
                // would silence it. The Logon sound, started by the window thread a
                // moment ago, is given its 4.4 s first; a new sound replaces a playing one.
                bool muted = false;
                std::string method, err;
                if (setAudioMute(false, muted, method, err)) logLine("playback device unmuted for the network alarm via " + method);
                else logLine("playback device could not be unmuted for the network alarm: " + err);
                Sleep(4500);
            }
            playEventSound(SoundLrBadIp);   // once per unworkable reading: the start-up one and each re-read
            ++tries;
            const char* why = nets_.empty() ? (validAddress(info_.ip) ? ", no adapter holds a working address" : "") : "";
            if (tries == kIpTries)
                logLine(format("no working address after %d re-reads (last read \"%s\"%s); network error shown, re-reading goes on every %u s",
                               kIpTries, info_.ip.c_str(), why, (unsigned)(kIpWaitMs / 1000)));
            else
                logLine(format("no working address (read \"%s\"%s); waiting %u s, then re-reading, try %d",
                               info_.ip.c_str(), why, (unsigned)(kIpWaitMs / 1000), tries));
            EnterCriticalSection(&lock_);
            status_.waitingIp = true;
            status_.ipTry = tries;
            status_.badIp = info_.ip;
            LeaveCriticalSection(&lock_);
            notify();
            Sleep(kIpWaitMs);
            rereadNetworks();
        }
        if (tries == 0) return;
        logLine(format("working address %s (mac %s) after %d re-read(s)", info_.ip.c_str(), info_.mac.c_str(), tries));
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
    }

    // ---- announcing ------------------------------------------------------
    // ---- farm.key -------------------------------------------------------
    // The shared secret of docs/PROTOCOL.md, Authentication: farm.key beside the
    // agent, put there by Thunderhead through the shared folder (or by hand).
    // With it the agent connects only to a host whose OFFER proves it; without
    // it the agent connects to any host but refuses every command except SET
    // and PING until the file appears, and then reconnects so that the host is
    // proved as well.
    void loadKey()
    {
        std::string key = readKeyFile(exeDirectory() + L"\\" + kFarmKeyFile);
        bool changed = key != key_;
        key_ = key;
        EnterCriticalSection(&lock_);
        status_.keyed = !key_.empty();
        LeaveCriticalSection(&lock_);
        if (changed) notify();
    }

    // Re-reads farm.key every few seconds while none is held; true when it appeared.
    bool pollKey()
    {
        if (!key_.empty()) return false;
        unsigned long long now = tickMs();
        if (now - lastKeyPollMs_ < 5000) return false;
        lastKeyPollMs_ = now;
        loadKey();
        if (key_.empty()) return false;
        logLine("farm.key appeared in " + narrow(exeDirectory()));
        return true;
    }

    void sendAnnounce()
    {
        Message m("ANNOUNCE");
        m.set("mac", info_.mac);
        m.set("name", info_.name);
        m.set("agent", TLR_VERSION_STRING);
        announceChallenge_ = randomHex(kChallengeBytes);
        m.set("challenge", announceChallenge_);
        std::string wire = m.encode();
        // The directed broadcast of every announced sub-net; the limited
        // broadcast only when no adapter is announced on, so that the node can
        // still be found.
        std::string pref;
        std::vector<std::string> off;
        netSnapshot(pref, off);
        std::vector<unsigned long> targets;
        for (size_t i = 0; i < nets_.size(); ++i) if (!netIsOff(off, nets_[i].id)) targets.push_back(nets_[i].broadcast());
        if (targets.empty()) targets.push_back(INADDR_BROADCAST);
        for (size_t i = 0; i < targets.size(); ++i) {
            sockaddr_in to;
            memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = targets[i];
            to.sin_port = htons(kDiscoveryPort);
            sendto(udp_, wire.data(), (int)wire.size(), 0, (sockaddr*)&to, sizeof(to));
        }
    }

    // An OFFER is taken from a host on an announced sub-net only. One from the
    // preferred adapter's sub-net (the identity address) is connected to at
    // once; one from another announced sub-net is held for kOfferHoldMs so that
    // a host on the preferred sub-net, answering the same ANNOUNCE, wins.
    void announcePhase()
    {
        unsigned long long nextAnnounce = 0;
        bool logged = false;
        bool havePending = false;
        Message pending;
        sockaddr_in pendingFrom;
        memset(&pendingFrom, 0, sizeof(pendingFrom));
        unsigned long long pendingSinceMs = 0;
        std::string pendingVia;
        unsigned long long startMs = tickMs();
        unsigned long long nextAlarm = startMs + kAnnounceAlarmMs;
        unsigned long long nextReread = startMs + kNetRereadMs;
        while (tcp_ == INVALID_SOCKET) {
            unsigned long long now = tickMs();
            pollKey();
            if (InterlockedExchange(&g_renet, 0)) {
                rereadNetworks();
                logNetworks();
                havePending = false;
                nextAnnounce = 0;
            }
            // The adapters are re-read every kNetRereadMs: a lease lost takes the
            // node back to the address wait; a changed one is announced from at once.
            if (now >= nextReread) {
                nextReread = now + kNetRereadMs;
                std::string before = netsKey();
                rereadNetworks();
                if (!networkReady()) {
                    logLine(format("working address lost while announcing (read \"%s\"); waiting for one", info_.ip.c_str()));
                    return;   // run() goes back to acquireAddress
                }
                if (netsKey() != before) {
                    logLine("adapters changed while announcing");
                    logNetworks();
                    havePending = false;
                    nextAnnounce = 0;
                }
            }
            // No host on an announced sub-net has answered since announcing began:
            // the alarm, then again every kAnnounceAlarmMs.
            if (now >= nextAlarm) {
                nextAlarm = now + kAnnounceAlarmMs;
                logLine(format("no Thunderhead on an announced sub-net for %llu s; alarm", (now - startMs) / 1000));
                playEventSound(SoundLrBadIp);
            }
            if (havePending && now - pendingSinceMs >= kOfferHoldMs) {
                havePending = false;
                connectOffer(pending, pendingFrom, "sub-net of " + pendingVia + ", no OFFER from the preferred sub-net within " + format("%u", (unsigned)kOfferHoldMs) + " ms");
                continue;
            }
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
                const NetAdapter* via = announcedSubnetOf(from.sin_addr.s_addr);
                if (!via) {
                    // The host's sub-net is not among the announced ones as last read:
                    // the adapters are re-read before the OFFER is judged, since a lease
                    // that arrived after the last read is the usual reason.
                    std::string before = netsKey();
                    rereadNetworks();
                    if (netsKey() != before) { logLine("adapters changed; re-read for an OFFER"); logNetworks(); }
                    via = announcedSubnetOf(from.sin_addr.s_addr);
                }
                if (!via) {
                    if (now - lastRejectLogMs_ > 60000) {
                        lastRejectLogMs_ = now;
                        logLine(format("OFFER from %s (%s) port %d ignored: not on an announced sub-net", ipText(from).c_str(), m.get("host").c_str(), port));
                    }
                    continue;
                }
                // A held key must be proved by the OFFER: auth= over the challenge token of this
                // agent's ANNOUNCE. An OFFER that fails is ignored; the file is re-read
                // in case the key was replaced on disk.
                if (!key_.empty()) {
                    bool proved = m.has("auth") && !announceChallenge_.empty() &&
                                  equalsConstantTime(m.get("auth"), offerAuth(key_, announceChallenge_, m.get("host"), format("%d", port)));
                    if (!proved) {
                        if (now - lastRejectLogMs_ > 60000) {
                            lastRejectLogMs_ = now;
                            logLine(format("OFFER from %s (%s) port %d rejected: %s", ipText(from).c_str(), m.get("host").c_str(), port,
                                           m.has("auth") ? "its farm.key is not this node's" : "it carries no proof of the farm key (Thunderhead before 0.4.0?)"));
                        }
                        loadKey();
                        continue;
                    }
                }
                if (via->ip == info_.ip) {
                    havePending = false;
                    connectOffer(m, from, "preferred sub-net");
                } else if (!havePending) {
                    havePending = true;
                    pending = m;
                    pendingFrom = from;
                    pendingSinceMs = now;
                    pendingVia = via->desc;
                }
            }
        }
    }

    // Connects to the host an acceptable OFFER came from and sends HELLO.
    void connectOffer(const Message& m, const sockaddr_in& from, const std::string& why)
    {
        int port = m.getInt("port", kDefaultTcpPort);
        sockaddr_in host = from;
        host.sin_port = htons((unsigned short)port);
        hostAuthenticated_ = !key_.empty();   // an OFFER reaches here only when it proved a held key
        offerChallenge_ = m.has("challenge") ? m.get("challenge") : m.get("nonce");   // nonce= from a 0.4.0 host
        logLine(format("OFFER from %s (%s) port %d%s, %s", ipText(from).c_str(), m.get("host").c_str(), port,
                       hostAuthenticated_ ? ", host authenticated" : ", unauthenticated: no farm.key here yet", why.c_str()));
        SOCKET s = connectWithTimeout(host, 3000);
        if (s == INVALID_SOCKET) { logLine("connect failed"); return; }
        tcp_ = s;
        hostAddrN_ = from.sin_addr.s_addr;
        parser_.clear();
        sent_ = Message();
        if (!sendHello()) { dropSession("hello send failed"); return; }
        std::string hostIp = ipText(from);
        logLine(format("connected to Thunderhead at %s:%d", hostIp.c_str(), port));
        EnterCriticalSection(&lock_);
        status_.connected = true;
        status_.hostName = m.get("host");
        status_.hostIp = hostIp;
        status_.hostMac = macOf(hostIp, info_);
        status_.hostAuthenticated = hostAuthenticated_;
        status_.haveReport = false;
        status_.report = Message();
        LeaveCriticalSection(&lock_);
        notify();
        nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
        cpu_.sample();
        lightning_.query();
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
        m.set("bryce3_path", info_.brycePath[3]);
        m.set("bryce3_version", info_.bryceVersion[3]);
        m.set("bryce2_path", info_.brycePath[2]);
        m.set("bryce2_version", info_.bryceVersion[2]);
        m.set("bryce1_path", info_.brycePath[1]);
        m.set("bryce1_version", info_.bryceVersion[1]);
        m.set("relay", boolText(relay));
        m.set("relay_ip", relay ? relayIp : std::string());
        m.set("render_host", boolText(renderHostSetting()));
        m.set("render_bin", narrow(renderBinSetting()));
        m.set("render_bin_exists", boolText(renderBinExists()));
        {
            std::string pref;
            std::vector<std::string> off;
            netSnapshot(pref, off);
            m.set("net_count", (long long)nets_.size());
            for (size_t i = 0; i < nets_.size(); ++i) m.set(format("net%u", (unsigned)i + 1), netText(nets_[i], info_.ip, pref, off));
        }
        m.set("keyed", boolText(!key_.empty()));
        if (!key_.empty() && !offerChallenge_.empty()) {
            m.set("challenge", offerChallenge_);
            m.set("auth", helloAuth(key_, offerChallenge_, info_.mac));
        }
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
        full.set("render_host", boolText(renderHostSetting()));
        full.set("render_bin", narrow(renderBinSetting()));
        full.set("render_bin_exists", boolText(renderBinExists()));
        BryceState b = bryce_.query();
        full.set("bryce", b.running ? "running" : "stopped");
        full.set("bryce_cpu_pct", (long long)b.cpuPct);   // the B3NDHR mode, so that a change made here reaches Thunderhead within a report
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
        if (key_.empty() && m.verb != "SET" && m.verb != "PING") {
            // Unpaired: the host was not proved, so nothing it asks for is done.
            logLine(m.verb + " refused: no farm.key on this node yet, the host is not authenticated");
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", "no");
            ack.set("error", "not paired: this node holds no farm.key; Thunderhead writes it into the shared folder once that folder opens from the host");
            sendMessage(ack);
            return;
        }
        if (m.verb == "SET") {
            int iv = m.getInt("interval", intervalSec_);
            if (iv < 1) iv = 1;
            if (iv > 3600) iv = 3600;
            intervalSec_ = iv;
            // phase: the slot Thunderhead dealt this node, as the delay before the
            // first report of the new schedule; later reports follow at the
            // interval from that one, which keeps the slot. The full interval
            // when Thunderhead sends none.
            int phase = m.getInt("phase", -1);
            if (phase < 0 || phase > intervalSec_ * 1000) phase = intervalSec_ * 1000;
            nextReport_ = tickMs() + (unsigned long long)phase;
            logLine(format("report interval %d s, next report in %d ms", intervalSec_, phase));
            EnterCriticalSection(&lock_);
            status_.intervalSec = intervalSec_;
            LeaveCriticalSection(&lock_);
            notify();
            if (helloSoundPending_) { helloSoundPending_ = false; playEventSound(SoundLrHello); }
        } else if (m.verb == "BRYCE") {
            // The full Bryce 3D on this node (the Bryce 3D Network Render Host):
            // action=start opens the installed Bryce of generation gen= (3 = Bryce 7
            // and 7 Pro, 2 = 6.x, 1 = 5.x; 3 when absent), with scene= (a path inside
            // this folder, the shared one) as a double-click on the file would, and
            // brings its window to the front; output=, first= and last= describe the
            // render job and are kept in job.txt beside the agent, the output folder
            // created when it lies inside this folder; action=stop ends whichever
            // Bryce.exe runs; action=front brings its window to the front. The ACK
            // carries what was found.
            std::string action = trim(m.get("action")), scene = trim(m.get("scene")), err;
            int gen = m.getInt("gen", 3);
            bool restarted = false;
            std::string prefsNote, imageNote;
            BryceInstall bi;
            bool ok = false;
            std::wstring scenePath;
            if (action == "start") {
                if (!scene.empty()) {
                    if (scene.find(':') != std::string::npos || scene[0] == '\\' || scene[0] == '/' || scene.find("..") != std::string::npos) {
                        err = "the scene must be a name or a relative path inside the shared folder";
                    } else {
                        std::wstring rel = widen(scene);
                        for (size_t i = 0; i < rel.size(); ++i) if (rel[i] == L'/') rel[i] = L'\\';
                        scenePath = exeDirectory() + L"\\" + rel;
                        if (GetFileAttributesW(scenePath.c_str()) == INVALID_FILE_ATTRIBUTES) err = scene + " does not exist in " + narrow(exeDirectory());
                    }
                }
                if (err.empty()) {
                    std::string output = trim(m.get("output"));
                    if (!output.empty() && output.find("..") == std::string::npos && output.compare(0, 2, "\\\\") != 0) {
                        // relative: inside this folder; absolute (a drive letter): anywhere on this node, RENDER.BIN as a rule
                        std::wstring dir = output.find(':') == std::string::npos && output[0] != '\\' ? exeDirectory() + L"\\" + widen(output) : widen(output);
                        if (!createFolder(dir)) logLine("BRYCE: output folder " + narrow(dir) + " could not be created: " + lastErrorText((int)GetLastError()));
                        else logLine("BRYCE: output folder " + narrow(dir));
                    }
                    if (!scene.empty()) {
                        SYSTEMTIME t;
                        GetLocalTime(&t);
                        std::string job = "scene=" + scene + "\r\noutput=" + output + "\r\nfirst=" + trim(m.get("first")) + "\r\nlast=" + trim(m.get("last")) +
                                          format("\r\nstarted=%04d-%02d-%02d %02d:%02d:%02d\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
                        if (!writeTextFile(exeDirectory() + L"\\job.txt", job)) logLine("job.txt could not be written: " + lastErrorText((int)GetLastError()));
                    }
                    // The neutral gray image beside the scene Bryce is about to open, written
                    // here on the node every time, whatever Thunderhead did: with no scene
                    // given, Bryce launches to its previous state, so the scene of the last
                    // job (job.txt) is the one it opens.
                    std::wstring protect = scenePath;
                    if (protect.empty()) {
                        std::vector<std::string> lines = split(readTextFile(exeDirectory() + L"\\job.txt"), '\n');
                        for (size_t i = 0; i < lines.size(); ++i)
                            if (lines[i].compare(0, 6, "scene=") == 0 && lines[i].size() > 6) protect = exeDirectory() + L"\\" + widen(trim(lines[i].substr(6)));
                    }
                    imageNote = grayImageBeside(protect);
                    ok = startBryce(gen, scenePath, !scenePath.empty(), bi, err, restarted, prefsNote);
                    if (ok) { bryceWindowWanted_ = true; bryceStartMs_ = tickMs(); }
                }
            } else if (action == "stop") {
                ok = stopBryce(err);
                bryceWindowWanted_ = false;
            } else if (action == "front") {
                HWND w = bryceWindow();
                if (w) { bringToFront(w); ok = true; } else err = findBrycePid() ? "Bryce has no visible window yet" : "Bryce is not running";
            } else {
                err = "unknown action \"" + action + "\"";
            }
            if (action != "start") { std::string ignore; findBryce(gen, bi, ignore); }   // what the ACK names
            std::string where = bi.path.empty() ? std::string()
                : format(" [%s, version %s, found through %s]", narrow(bi.path).c_str(), bi.version.empty() ? "unknown" : bi.version.c_str(), bi.found.c_str());
            logLine(format("BRYCE %s %s%s%s: %s%s%s%s%s", action.c_str(), bryceLabel(gen), scene.empty() ? "" : " scene ", scene.c_str(), ok ? "ok" : "failed",
                           restarted ? " (the running Bryce was ended first)" : "", err.empty() ? "" : ": ", err.c_str(), where.c_str()));
            if (!prefsNote.empty()) logLine("BRYCE: Image with Scene Open/Save: " + prefsNote);
            if (!imageNote.empty()) logLine("BRYCE: gray image: " + imageNote);
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("action", action);
            ack.set("gen", (long long)gen);
            ack.set("restarted", boolText(restarted));
            ack.set("prefs", prefsNote);
            ack.set("image", imageNote);
            ack.set("scene", scene);
            ack.set("path", narrow(bi.path));
            ack.set("version", bi.version);
            ack.set("found", bi.found);
            sendMessage(ack);
            if (ok && action == "start") {
                for (int i = 0; i < 20 && bryceWindowWanted_; ++i) {
                    tryBryceWindow();
                    if (bryceWindowWanted_) Sleep(100);
                }
            }
            sendReport();
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
        } else if (m.verb == "SHARE") {
            // Create Remote Shared Folder in Thunderhead: action=create path= name=
            // creates the folder on this node and shares it under the name at share
            // level for everyone (the folder's own permissions still apply). Needs
            // administrator rights, which Windows reports as access denied otherwise.
            std::string action = trim(m.get("action")), path = trim(m.get("path")), name = trim(m.get("name")), err;
            bool ok = false, existed = false;
            if (action != "create") err = "unknown action \"" + action + "\"";
            else if (path.size() < 3 || path[1] != ':' || path[2] != '\\' || path.find("..") != std::string::npos) err = "path must be a full local path";
            else if (name.empty() || name.size() > 80 || name.find_first_of("\\/:*?\"<>|") != std::string::npos) err = "share name invalid";
            else if (!createFolder(widen(path))) err = "folder could not be created: " + lastErrorText((int)GetLastError());
            else {
                std::wstring wpath = widen(path), wname = widen(name);
                while (wpath.size() > 3 && wpath[wpath.size() - 1] == L'\\') wpath.erase(wpath.size() - 1);
                SHARE_INFO_2 si;
                memset(&si, 0, sizeof(si));
                si.shi2_netname = (LPWSTR)wname.c_str();
                si.shi2_type = STYPE_DISKTREE;
                si.shi2_remark = (LPWSTR)L"Created by Thunderhead";
                si.shi2_permissions = ACCESS_ALL;
                si.shi2_max_uses = (DWORD)-1;
                si.shi2_path = (LPWSTR)wpath.c_str();
                DWORD bad = 0;
                NET_API_STATUS st = NetShareAdd(NULL, 2, (LPBYTE)&si, &bad);
                if (st == NERR_Success) ok = true;
                else if (st == NERR_DuplicateShare) {
                    // The name exists: fine when it already points at this folder.
                    SHARE_INFO_2* have = NULL;
                    if (NetShareGetInfo(NULL, (LPWSTR)wname.c_str(), 2, (LPBYTE*)&have) == NERR_Success && have) {
                        std::wstring hp = have->shi2_path ? have->shi2_path : L"";
                        while (hp.size() > 3 && hp[hp.size() - 1] == L'\\') hp.erase(hp.size() - 1);
                        if (_wcsicmp(hp.c_str(), wpath.c_str()) == 0) { ok = true; existed = true; }
                        else err = "share " + name + " exists already and points at " + narrow(hp);
                        NetApiBufferFree(have);
                    } else {
                        err = "share " + name + " exists already";
                    }
                } else if (st == ERROR_ACCESS_DENIED) err = "access denied: creating a share needs administrator rights on this node";
                else err = format("NetShareAdd failed: %s", lastErrorText((int)st).c_str());
            }
            logLine(format("SHARE %s %s as %s: %s%s%s", action.c_str(), path.c_str(), name.c_str(), ok ? (existed ? "ok, was shared already" : "ok") : "failed",
                           err.empty() ? "" : ": ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("action", action);
            ack.set("path", path);
            ack.set("name", name);
            ack.set("existed", boolText(existed));
            sendMessage(ack);
        } else if (m.verb == "RENDERHOST") {
            // Toggle B3D Net Render Host in Thunderhead: set=yes|no becomes this
            // node's B3NDHR mode, saved for the next start; a report follows at once.
            std::string set = trim(m.get("set")), err;
            bool ok = set == "yes" || set == "no";
            if (!ok) err = "set must be yes or no";
            else {
                if (m.has("render_bin") && !trim(m.get("render_bin")).empty()) {
                    bool changed = false;
                    EnterCriticalSection(&g_settingsLock);
                    g_renderHost = set == "yes";   // so that applyRenderBin creates the folder for a host
                    LeaveCriticalSection(&g_settingsLock);
                    if (!applyRenderBin(widen(m.get("render_bin")), changed)) err = "RENDER.BIN folder could not be created";
                }
                applyRenderHostSetting(set == "yes");
            }
            logLine(format("RENDERHOST set=%s render_bin=%s: %s%s%s", set.c_str(), narrow(renderBinSetting()).c_str(), ok ? "ok" : "failed", err.empty() ? "" : ": ", err.c_str()));
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("render_host", boolText(renderHostSetting()));
            ack.set("render_bin", narrow(renderBinSetting()));
            ack.set("render_bin_exists", boolText(renderBinExists()));
            ack.set("render_bin_in_share", renderBinInShare());
            sendMessage(ack);
            notify();
            sendReport();
            nextReport_ = tickMs() + (unsigned long long)intervalSec_ * 1000;
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
            // relay\<job>. All three run on a thread and are acknowledged when
            // they end; dest= must be the address set in Settings.
            std::string op = trim(m.get("op")), job = trim(m.get("job")), dest = trim(m.get("dest")), share = trim(m.get("share")), err;
            bool relay;
            std::string relayIp;
            relaySnapshot(relay, relayIp);
            if (!relay) err = "this agent is not set to act as an SMB1 relay";
            else if (!destMatchesRelay(dest, relayIp)) err = "this agent relays to " + relayIp + ", not to " + dest;
            else if (share.empty() || share.find_first_of("\\/") != std::string::npos) err = "share name missing";
            else if (op != "probe" && op != "push" && op != "pull") err = "unknown op \"" + op + "\"";
            else if (op != "probe" && (job.empty() || job.find_first_of("\\/:.") != std::string::npos)) err = "job must be a plain name";
            if (err.empty()) {
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
                    logLine(format("RELAY %s%s%s to \\\\%s\\%s\\ started", op.c_str(), job.empty() ? "" : " job ", job.c_str(), dest.c_str(), share.c_str()));
                    return;   // the ACK follows from pollRelays when the work has ended
                }
                err = "could not start the relay thread";
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
        } else if (m.verb == "MESSAGE") {
            // Display Message in Thunderhead: title= for the caption, text= the
            // message as hexadecimal digits of its UTF-8 bytes (as BATCH), sound= a
            // name in the sounds folder played as the window opens (empty for a
            // silent message), timeout= seconds after which the window closes by
            // itself, 0 for until dismissed. This ACK says the window was opened;
            // pollMessages sends a second one, stage=closed, when it has closed.
            std::string text, title = trim(m.get("title")), sound = trim(m.get("sound")), err, soundErr;
            int timeout = m.getInt("timeout", 0);
            if (timeout < 0) timeout = 0;
            bool ok = hexDecode(m.get("text"), text);
            if (!ok) err = "text is not hexadecimal";
            else if (title.empty()) { ok = false; err = "title missing"; }
            if (ok) {
                ShownMessage* sm = new ShownMessage();
                sm->title = widen(title);
                sm->text = widen(text);
                sm->timeoutSec = timeout;
                if (!g_window || !PostMessageW(g_window, WM_SHOW_MESSAGE, 0, (LPARAM)sm)) { delete sm; ok = false; err = "the window thread could not be reached"; }
            }
            if (ok && !sound.empty()) {
                std::wstring path = soundDir() + L"\\" + widen(sound);
                if (sound.find_first_of("\\/:") != std::string::npos) soundErr = "sound must be a name in the sounds folder";
                else if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) soundErr = sound + " is not in " + narrow(soundDir());
                else soundPlay(path, soundErr);
            }
            std::string when = timeout > 0 ? format("closes after %d s", timeout) : std::string("until dismissed");
            std::string soundNote = sound.empty() ? std::string(", silent") : ", sound " + sound + (soundErr.empty() ? "" : " not played: " + soundErr);
            logLine(ok ? format("MESSAGE \"%s\" opened, %u character(s), %s%s", title.c_str(), (unsigned)widen(text).size(), when.c_str(), soundNote.c_str())
                       : "MESSAGE failed: " + err);
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("stage", "opened");
            ack.set("ok", boolText(ok));
            ack.set("error", err);
            ack.set("title", title);
            ack.set("sound", sound);
            ack.set("sound_error", soundErr);
            ack.set("timeout", (long long)timeout);
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
            // HELLO again, so that both sides play their HELLO sounds and Thunderhead
            // lights the row, then a report with every value rather than only the
            // changed ones, and the report timer restarts from here. Agents from
            // 0.2.11 to 0.3.6 answered with the report alone; older ones sent PONG.
            if (!sendHello()) { dropSession("hello send failed"); return; }
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
            // kind=bat|ps1 with text= as hexadecimal digits of its UTF-8 bytes, or
            // kind=exe with file= inside the shared folder; bat when kind is absent.
            // This ACK says whether it was started; pollBatches sends a second one
            // when it has ended.
            std::string kind = trim(m.get("kind")), file = trim(m.get("file")), text, err;
            if (kind.empty()) kind = "bat";
            RunningBatch b;
            bool ok = true;
            if (kind == "exe") { if (file.empty()) { ok = false; err = "no file name"; } }
            else if (kind == "bat" || kind == "ps1") { ok = hexDecode(m.get("text"), text) && !text.empty(); if (!ok) err = "no text, or not hexadecimal"; }
            else { ok = false; err = "unknown kind \"" + kind + "\""; }
            bool maximized = parseBool(m.get("maximized")), front = parseBool(m.get("front"));
            if (ok) ok = startBatch(text, kind, file, maximized, front, b, err);
            if (ok) batches_.push_back(b);
            std::string size = kind == "exe" ? std::string() : format(", %u bytes of text", (unsigned)text.size());
            logLine(ok ? format("BATCH %s started as %s%s%s%s", kind.c_str(), b.file.c_str(), size.c_str(),
                                maximized ? ", maximized" : "", front ? ", to the front" : "")
                       : "BATCH " + kind + " failed: " + err);
            Message ack("ACK");
            ack.set("cmd", m.verb);
            ack.set("kind", kind);
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

    // Looks for Bryce's window once BRYCE has started it, brings it to the
    // front, and gives up 60 s after the start (Bryce takes a while to open).
    void tryBryceWindow()
    {
        if (!bryceWindowWanted_) return;
        HWND w = bryceWindow();
        if (w) {
            bringToFront(w);
            bryceWindowWanted_ = false;
            logLine("BRYCE: window found, brought to the front");
        } else if (tickMs() - bryceStartMs_ > 60000) {
            bryceWindowWanted_ = false;
            logLine("BRYCE: window not found within 60 s, left as Windows opened it");
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
    // Acknowledges every message window closed since the last pass.
    bool pollMessages()
    {
        std::vector<MessageOutcome> done;
        takeMessageOutcomes(done);
        for (size_t i = 0; i < done.size(); ++i) {
            const char* how = done[i].closed == "ok" ? "with OK" : done[i].closed == "timeout" ? "by its timer" : "with the Close button";
            logLine(format("MESSAGE \"%s\" closed %s after %lld s", done[i].title.c_str(), how, done[i].seconds));
            Message ack("ACK");
            ack.set("cmd", "MESSAGE");
            ack.set("stage", "closed");
            ack.set("ok", boolText(true));
            ack.set("error", std::string());
            ack.set("title", done[i].title);
            ack.set("closed", done[i].closed);
            ack.set("seconds", done[i].seconds);
            if (!sendMessage(ack)) return false;
        }
        return true;
    }

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
            logLine(format("BATCH %s finished with exit code %u after %lld s", b.file.c_str(), (unsigned)code, seconds));
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
            if (t->op == "probe")
                logLine(format("RELAY probe \\\\%s\\%s\\: %s%s%s", t->dest.c_str(), t->share.c_str(), t->ok ? "opens" : "failed",
                               t->error.empty() ? "" : ": ", t->error.c_str()));
            else
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
        unsigned long long nextReread = tickMs() + kNetRereadMs;
        while (tcp_ != INVALID_SOCKET) {
            tryTeamViewerWindow();
            tryBryceWindow();
            if (tickMs() >= nextReread) {
                // A lease renewed with another address, or lost, ends the session: the
                // node announces again from the new address, or waits for one.
                nextReread = tickMs() + kNetRereadMs;
                std::string oldIp = info_.ip;
                rereadNetworks();
                if (!networkReady()) { dropSession(format("working address lost (read \"%s\"); waiting for one", info_.ip.c_str()).c_str()); return; }
                if (info_.ip != oldIp) { logNetworks(); dropSession(("address changed from " + oldIp + " to " + info_.ip + "; announcing again").c_str()); return; }
            }
            if (!pollBatches()) { dropSession("batch result send failed"); return; }
            if (!pollMessages()) { dropSession("message result send failed"); return; }
            if (!pollRelays()) { dropSession("relay result send failed"); return; }
            if (InterlockedExchange(&g_rehello, 0)) {
                if (!sendHello()) { dropSession("hello send failed"); return; }
                logLine("HELLO sent again: SMB1 relay " + relayText());
            }
            if (InterlockedExchange(&g_renet, 0)) {
                // A new identity adapter changes the address and MAC the node reports,
                // and a host outside the announced sub-nets is no longer wanted: either
                // way the session ends and the node announces again. Otherwise the new
                // network list goes out in a fresh HELLO.
                std::string oldIp = info_.ip;
                rereadNetworks();
                logNetworks();
                if (info_.ip != oldIp) { dropSession("network settings changed the identity adapter; announcing again"); return; }
                if (!announcedSubnetOf(hostAddrN_)) { dropSession("network settings exclude the host's sub-net; announcing again"); return; }
                if (!sendHello()) { dropSession("hello send failed"); return; }
                logLine("HELLO sent again: network settings changed");
            }
            if (pollKey()) {
                // This session began without a key, so the host was never proved;
                // the next OFFER has to prove it.
                dropSession("farm.key arrived; reconnecting to authenticate the host");
                return;
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
    std::vector<NetAdapter> nets_;        // every adapter with a working address, as last read
    unsigned long hostAddrN_;             // the host's address (network order) of the current session
    CpuTracker cpu_;
    LightningMonitor lightning_;
    BryceMonitor bryce_;
    bool bryceWindowWanted_;              // BRYCE start: Bryce's window is still to be brought to the front
    unsigned long long bryceStartMs_;
    ThermalReader thermal_;
    MessageParser parser_;
    Message sent_;                   // every report value as last sent to this host
    std::vector<RunningBatch> batches_;   // batch files started by BATCH and not yet ended
    std::vector<RelayTask*> relays_;      // RELAY copies running and not yet acknowledged
    bool tvWindowWanted_;                 // TEAMVIEWER started TeamViewer and its window is still to be found
    unsigned long long tvStartMs_;
    bool helloSoundPending_;              // HELLO sent; the Proximity Connection sound plays when Thunderhead's SET answers it
    std::string key_;                     // farm.key; empty while unpaired
    std::string announceChallenge_;           // challenge token of the latest ANNOUNCE, which the OFFER has to sign
    std::string offerChallenge_;              // challenge token of the OFFER this session came from, signed in HELLO
    bool hostAuthenticated_;              // the OFFER of this session proved the key
    unsigned long long lastKeyPollMs_;    // tickMs() of the last re-read of farm.key while unpaired
    unsigned long long lastRejectLogMs_;  // tickMs() of the last "OFFER rejected" line
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
    } else {
        // The label as well: the network rows shift the rows below them when
        // an adapter comes or goes.
        memset(&it, 0, sizeof(it));
        it.iSubItem = 0;
        it.pszText = (LPWSTR)item;
        SendMessageW(g_list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
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
               (s.hostMac.empty() ? L"" : L", " + widen(s.hostMac)) + L")" +
               (s.hostAuthenticated ? L", host authenticated" : L", host not authenticated: commands refused until farm.key arrives");
    } else if (s.waitingIp) {
        host = widen(format("%sNo working IP address (read \"%s\"%s): re-reading in %u s, try %d",
                            s.ipTry >= kIpTries ? "Network Error: " : "", s.badIp.c_str(),
                            s.nets.empty() && validAddress(s.badIp) ? ", no adapter holds a working address" : "",
                            (unsigned)(kIpWaitMs / 1000), s.ipTry));
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
    setRow(row++, L"Bryce 3D",            s.haveReport ? (r.get("bryce") == "running" ? L"Running" : L"Stopped") : L"");
    setRow(row++, L"Bryce 3D CPU %",      s.haveReport ? widen(r.get("bryce_cpu_pct")) : L"");
    setRow(row++, L"Clock",               s.haveReport ? widen(r.get("clock")) : L"");
    setRow(row++, L"UTC ms since 1970",   s.haveReport ? widen(r.get("utc_ms")) : L"");
    setRow(row++, L"Sent once at connect", L"");
    setRow(row++, L"Name",                widen(i.name));
    setRow(row++, L"IP",                  widen(i.ip));
    setRow(row++, L"MAC",                 widen(i.mac));
    {
        std::string pref;
        std::vector<std::string> off;
        netSnapshot(pref, off);
        if (s.nets.empty()) setRow(row++, L"Networks", L"no adapter holds a working address");
        for (size_t k = 0; k < s.nets.size(); ++k) {
            std::wstring label = widen(format("Network %u", (unsigned)k + 1));
            setRow(row++, label.c_str(), widen(netText(s.nets[k], i.ip, pref, off)));
        }
    }
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
    for (int g = 3; g >= 1; --g)
        setRow(row++, widen(std::string(bryceLabel(g))).c_str(), i.brycePath[g].empty() ? std::wstring(L"not installed")
                                        : widen(i.bryceVersion[g].empty() ? i.brycePath[g] : i.bryceVersion[g] + " at " + i.brycePath[g]));
    setRow(row++, L"Sounds",              widen(format("%u file(s) in ", (unsigned)soundFilesIn(soundDir()).size())) + soundDir());
    setRow(row++, L"SMB1 relay",          widen(relayText()));
    setRow(row++, L"Net Render Host",     renderHostSetting() ? L"On: this node is the Bryce 3D Network Render Host (B3NDHR)" : L"Off");
    setRow(row++, L"RENDER.BIN",          renderBinSetting() + (renderBinExists() ? L"  (exists)" : L"  (missing; created when this node is the host)"));
    setRow(row++, L"Authentication",      s.keyed ? L"farm.key present: only a host that proves it is obeyed"
                                                  : L"no farm.key in " + exeDirectory() + L": Thunderhead writes it through the shared folder; commands are refused until then");
    while ((int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0) > row) SendMessageW(g_list, LVM_DELETEITEM, (WPARAM)row, 0);
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
// The B3NDHR switch of the Settings dialog: a two-position slider drawn as an
// owner-drawn button, the knob at the left for off and at the right for on,
// the track metallic blue when on. The value edited sits in g_dlgRenderHost
// until OK applies it.
bool g_dlgRenderHost = false;
std::vector<NetAdapter> g_dlgNets;   // the adapters the Settings dialog lists, in list order

void fillNetworkControls(HWND dlg)
{
    HWND list = GetDlgItem(dlg, IDC_NET_LIST);
    HWND combo = GetDlgItem(dlg, IDC_NET_PREFERRED);
    AgentStatus s;
    g_agent->snapshot(s);
    g_dlgNets = s.nets;
    std::string pref;
    std::vector<std::string> off;
    netSnapshot(pref, off);
    SendMessageW(list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
    const wchar_t* titles[3] = { L"Announce on (adapter)", L"Address", L"Network" };
    const int widths[3] = { 230, 110, 60 };
    for (int c = 0; c < 3; ++c) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)titles[c];
        col.cx = widths[c];
        col.iSubItem = c;
        SendMessageW(list, LVM_INSERTCOLUMNW, (WPARAM)c, (LPARAM)&col);
    }
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"Automatic: the best adapter (Ethernet with a gateway first)");
    int selected = 0;
    for (size_t k = 0; k < g_dlgNets.size(); ++k) {
        const NetAdapter& n = g_dlgNets[k];
        std::wstring desc = widen(n.desc), addr = widen(format("%s/%d", n.ip.c_str(), n.prefix())), cat = widen(n.category);
        LVITEMW it;
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = (int)k;
        it.pszText = (LPWSTR)desc.c_str();
        SendMessageW(list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = (LPWSTR)addr.c_str(); SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)k, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = (LPWSTR)cat.c_str();  SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)k, (LPARAM)&it);
        LVITEMW st;
        memset(&st, 0, sizeof(st));
        st.stateMask = LVIS_STATEIMAGEMASK;
        st.state = INDEXTOSTATEIMAGEMASK(netIsOff(off, n.id) ? 1 : 2);
        SendMessageW(list, LVM_SETITEMSTATE, (WPARAM)k, (LPARAM)&st);
        std::wstring entry = desc + L"  (" + widen(n.ip) + L")";
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)entry.c_str());
        if (n.id == pref) selected = (int)k + 1;
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
}

// The network values as the dialog shows them; false with a message when they
// cannot be applied.
bool readNetworkControls(HWND dlg, std::string& pref, std::vector<std::string>& off)
{
    HWND list = GetDlgItem(dlg, IDC_NET_LIST);
    int sel = (int)SendMessageW(GetDlgItem(dlg, IDC_NET_PREFERRED), CB_GETCURSEL, 0, 0);
    pref = sel > 0 && sel <= (int)g_dlgNets.size() ? g_dlgNets[sel - 1].id : std::string();
    off.clear();
    int on = 0;
    for (size_t k = 0; k < g_dlgNets.size(); ++k) {
        UINT state = (UINT)SendMessageW(list, LVM_GETITEMSTATE, (WPARAM)k, LVIS_STATEIMAGEMASK);
        bool checked = state == INDEXTOSTATEIMAGEMASK(2);
        if (checked) ++on; else off.push_back(g_dlgNets[k].id);
    }
    if (!g_dlgNets.empty() && on == 0) {
        MessageBoxW(dlg, L"Check at least one network to announce on, or Thunderhead cannot find this node.", L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!pref.empty() && netIsOff(off, pref)) {
        MessageBoxW(dlg, L"The preferred adapter must be one of the networks announced on.", L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
        return false;
    }
    return true;
}

void drawRenderHostSwitch(const DRAWITEMSTRUCT* di)
{
    RECT rc = di->rcItem;
    HDC dc = di->hDC;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    int h = rc.bottom - rc.top, w = rc.right - rc.left;
    int trackH = h - 4, trackW = w - 4;
    RECT track = { rc.left + 2, rc.top + 2, rc.left + 2 + trackW, rc.top + 2 + trackH };
    COLORREF fill = g_dlgRenderHost ? RGB(50, 82, 123) : RGB(170, 170, 170);
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ oldB = SelectObject(dc, b), oldP = SelectObject(dc, pen);
    RoundRect(dc, track.left, track.top, track.right, track.bottom, trackH, trackH);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(b);
    DeleteObject(pen);
    int knob = trackH - 4;
    int kx = g_dlgRenderHost ? track.right - 2 - knob : track.left + 2;
    HBRUSH white = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HPEN gray = CreatePen(PS_SOLID, 1, RGB(96, 96, 96));
    oldB = SelectObject(dc, white);
    oldP = SelectObject(dc, gray);
    Ellipse(dc, kx, track.top + 2, kx + knob, track.top + 2 + knob);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(gray);
    if (di->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
}

void setRenderHostSwitch(HWND dlg, bool on)
{
    g_dlgRenderHost = on;
    SetDlgItemTextW(dlg, IDC_RENDERHOST_STATE, on ? L"On" : L"Off");
    InvalidateRect(GetDlgItem(dlg, IDC_RENDERHOST_SWITCH), NULL, TRUE);
}

INT_PTR CALLBACK settingsDlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lp;
        if (di->CtlID == IDC_RENDERHOST_SWITCH) { drawRenderHostSwitch(di); return TRUE; }
        break;
    }
    case WM_INITDIALOG: {
        bool relay;
        std::string ip;
        relaySnapshot(relay, ip);
        setRenderHostSwitch(dlg, renderHostSetting());
        SetDlgItemTextW(dlg, IDC_RENDERBIN_PATH, renderBinSetting().c_str());
        SetDlgItemTextW(dlg, IDC_RENDERBIN_NOTE, renderBinExists() ? L"The folder exists." : L"The folder does not exist yet; it is created when this node is the host.");
        CheckDlgButton(dlg, IDC_RELAY_CHECK, relay ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dlg, IDC_RELAY_IP, widen(ip).c_str());
        SendDlgItemMessageW(dlg, IDC_RELAY_IP, EM_LIMITTEXT, 63, 0);
        EnableWindow(GetDlgItem(dlg, IDC_RELAY_IP), relay);
        SetDlgItemTextW(dlg, IDC_RELAY_INFO,
                        L"Thunderhead copies files for that client into this node's shared folder, and this agent copies them on "
                        L"to \\\\<address>\\LightningRod\\ over SMB1, which Windows 10 no longer speaks to Windows XP. "
                        L"This node must be able to open that share with its own logon.");
        SetDlgItemTextW(dlg, IDC_NET_INFO,
                        L"Networks: the agent announces on the checked sub-nets and connects only to a Thunderhead on one of them; "
                        L"a host on the preferred adapter's sub-net is chosen first. The preferred adapter's address and MAC identify "
                        L"this node to Thunderhead. Kept for the next start.");
        fillNetworkControls(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_RENDERHOST_SWITCH) { setRenderHostSwitch(dlg, !g_dlgRenderHost); return TRUE; }
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
                if (!validRelayTarget(ip)) {
                    MessageBoxW(dlg, L"Enter the Windows XP client's computer name, for example LATITUDE, or its IP address as four numbers, for example 42.32.22.156.",
                                L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
                if (ip == s.info.ip || _stricmp(ip.c_str(), s.info.name.c_str()) == 0) {
                    MessageBoxW(dlg, L"That is this node itself. Enter the name or address of the Windows XP client to relay to.",
                                L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            }
            wchar_t binBuf[1024];
            GetDlgItemTextW(dlg, IDC_RENDERBIN_PATH, binBuf, 1024);
            std::wstring bin = trim(std::wstring(binBuf));
            if (bin.empty()) bin = renderBinDefault();
            if (bin.size() < 3 || bin[1] != L':' || bin[2] != L'\\') {
                MessageBoxW(dlg, L"Enter the RENDER.BIN folder as a full path on this node, for example C:\\LightningRod\\RENDER.BIN.",
                            L"Lightning Rod Settings", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            std::string netPref;
            std::vector<std::string> netOff;
            if (!readNetworkControls(dlg, netPref, netOff)) return TRUE;
            applyRelaySetting(on, ip, s.info.ip);   // the checks above make it succeed
            applyRenderHostSetting(g_dlgRenderHost);   // reaches Thunderhead with the next report
            bool binChanged = false;
            applyRenderBin(bin, binChanged);
            applyNetSettings(netPref, netOff);
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
    case WM_SHOW_MESSAGE: openMessageWindow((ShownMessage*)lp); return 0;
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

// ---- message windows ------------------------------------------------------
// One top-level window per MESSAGE, class LightningRodMessage: an ordinary
// overlapped window (Minimize, Maximize or Restore and Close in its caption)
// with the text, a countdown line and an OK button. Enter presses OK and
// Escape closes, through IsDialogMessage in the message loop. The window owns
// its ShownMessage and frees it when it is destroyed.
const wchar_t* const kMessageClass = L"LightningRodMessage";
const UINT_PTR kMessageTimer = 1;
const int kMessageOk = 1;

void queueMessageOutcome(ShownMessage* sm, const char* closed)
{
    MessageOutcome o;
    o.title = narrow(sm->title);
    o.closed = closed;
    o.seconds = (long long)((tickMs() - sm->startMs) / 1000);
    EnterCriticalSection(&g_messageLock);
    g_messageOutcomes.push_back(o);
    LeaveCriticalSection(&g_messageLock);
}

void closeMessageWindow(ShownMessage* sm, const char* closed)
{
    KillTimer(sm->window, kMessageTimer);
    queueMessageOutcome(sm, closed);
    DestroyWindow(sm->window);
}

void layoutMessageWindow(ShownMessage* sm)
{
    RECT rc;
    GetClientRect(sm->window, &rc);
    int w = rc.right, h = rc.bottom, margin = 14, bh = 28, bw = 96;
    MoveWindow(sm->textCtl, margin, margin, w - 2 * margin, h - 3 * margin - bh, TRUE);
    MoveWindow(sm->countdown, margin, h - margin - bh + 6, w - 3 * margin - bw, bh - 6, TRUE);
    MoveWindow(sm->okButton, w - margin - bw, h - margin - bh, bw, bh, TRUE);
}

void refreshCountdown(ShownMessage* sm)
{
    if (sm->timeoutSec <= 0) { SetWindowTextW(sm->countdown, L"Stays until dismissed"); return; }
    long long left = sm->timeoutSec - (long long)((tickMs() - sm->startMs) / 1000);
    if (left < 0) left = 0;
    SetWindowTextW(sm->countdown, widen(format("Closes by itself in %lld s", left)).c_str());
}

LRESULT CALLBACK messageWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    ShownMessage* sm = (ShownMessage*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_SIZE: if (sm) layoutMessageWindow(sm); return 0;
    case WM_CTLCOLORSTATIC: SetBkMode((HDC)wp, TRANSPARENT); return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case DM_GETDEFID: return MAKELONG(kMessageOk, DC_HASDEFID);   // Enter presses OK, through IsDialogMessage
    case WM_COMMAND:
        if (sm && LOWORD(wp) == kMessageOk) { closeMessageWindow(sm, "ok"); return 0; }
        if (sm && LOWORD(wp) == IDCANCEL) { closeMessageWindow(sm, "close"); return 0; }   // Escape
        return 0;
    case WM_TIMER:
        if (sm && wp == kMessageTimer) {
            refreshCountdown(sm);
            if (sm->timeoutSec > 0 && tickMs() - sm->startMs >= (unsigned long long)sm->timeoutSec * 1000) closeMessageWindow(sm, "timeout");
        }
        return 0;
    case WM_CLOSE: if (sm) closeMessageWindow(sm, "close"); else DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if (sm) {
            if (sm->textFont) DeleteObject(sm->textFont);
            if (sm->uiFontHandle) DeleteObject(sm->uiFontHandle);
            delete sm;
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool isMessageWindow(HWND top)
{
    wchar_t cls[64];
    return top && GetClassNameW(top, cls, 64) && wcscmp(cls, kMessageClass) == 0;
}

// Opens the window of a MESSAGE on the window thread; sm belongs to the window from here on.
void openMessageWindow(ShownMessage* sm)
{
    static bool registered = false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = messageWndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_LR_APP));
        wc.lpszClassName = kMessageClass;
        registered = RegisterClassW(&wc) != 0;
    }
    int w = 520, h = 300, x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    sm->startMs = tickMs();
    sm->window = CreateWindowExW(WS_EX_APPWINDOW, kMessageClass, sm->title.c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, NULL, NULL, inst, NULL);
    if (!sm->window) {
        logLine("MESSAGE window could not be created: " + lastErrorText((int)GetLastError()));
        queueMessageOutcome(sm, "close");
        delete sm;
        return;
    }
    SetWindowLongPtrW(sm->window, GWLP_USERDATA, (LONG_PTR)sm);
    sm->uiFontHandle = uiFont();
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    if (GetObjectW(sm->uiFontHandle, sizeof(lf), &lf)) { lf.lfHeight = lf.lfHeight * 8 / 5; sm->textFont = CreateFontIndirectW(&lf); }
    sm->textCtl = CreateWindowExW(0, L"STATIC", sm->text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, sm->window, NULL, inst, NULL);
    sm->countdown = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, sm->window, NULL, inst, NULL);
    sm->okButton = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, sm->window, (HMENU)(INT_PTR)kMessageOk, inst, NULL);
    SendMessageW(sm->textCtl, WM_SETFONT, (WPARAM)(sm->textFont ? sm->textFont : sm->uiFontHandle), TRUE);
    SendMessageW(sm->countdown, WM_SETFONT, (WPARAM)sm->uiFontHandle, TRUE);
    SendMessageW(sm->okButton, WM_SETFONT, (WPARAM)sm->uiFontHandle, TRUE);
    layoutMessageWindow(sm);
    refreshCountdown(sm);
    SetTimer(sm->window, kMessageTimer, 1000, NULL);
    ShowWindow(sm->window, SW_SHOWNORMAL);
    bringToFront(sm->window);
    SetFocus(sm->okButton);
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
    InitializeCriticalSection(&g_messageLock);
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
        HWND top = m.hwnd ? GetAncestor(m.hwnd, GA_ROOT) : NULL;   // a message window takes Enter, Escape and Tab as a dialog does
        if (isMessageWindow(top) && IsDialogMessageW(top, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    logLine("window closed, agent stopping");
    g_window = NULL;
    soundShutdown();
    netShutdown();
    ExitProcess(0);
}
