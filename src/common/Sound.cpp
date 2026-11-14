#include "Sound.h"
#include "Net.h"       // lastErrorText
#include "StrUtil.h"

#include <mmsystem.h>
#include <algorithm>
#include <cstdio>
#include <map>

namespace tlr {

const wchar_t* const kSoundCategories[kSoundCategoryCount] = {
    L"Background", L"Balloon", L"Battery Critical", L"Battery Low", L"Critical Stop", L"Default", L"Ding", L"Error",
    L"Exclamation", L"Feed Discovered", L"Foreground", L"Hardware Fail", L"Hardware Insert", L"Hardware Remove",
    L"Information Bar", L"Logoff Sound", L"Logon", L"Menu Command", L"Message Nudge", L"Minimize", L"Navigation Start",
    L"Notify Calendar", L"Notify Email", L"Notify Messaging", L"Notify System Generic", L"Notify", L"Pop-up Blocked",
    L"Print complete", L"Proximity Connection", L"Proximity Notification", L"Recycle", L"Restore", L"Ringin", L"Ringout",
    L"Shutdown", L"Startup", L"Unlock", L"User Account Control",
};

const SoundEventDef kSoundEvents[kSoundEventCount] = {
    { SoundThOpen,          L"TH", L"App Opens",                           L"Startup",                "th_open" },
    { SoundThHello,         L"TH", L"App Receives HELLO from Client",      L"Proximity Notification", "th_hello" },
    { SoundThClose,         L"TH", L"App Closed or Closes",                L"Shutdown",               "th_close" },
    { SoundThClientOffline, L"TH", L"Client that was Online went Offline", L"Minimize",               "th_offline" },
    { SoundLrOpen,          L"LR", L"App Opens",                           L"Logon",                  "logon" },
    { SoundLrHello,         L"LR", L"App Sends HELLO to Host",             L"Proximity Connection",   "hello" },
    { SoundLrClose,         L"LR", L"App Closed or Closes",                L"Logoff Sound",           "logoff" },
    { SoundLrReport,        L"LR", L"App Sends Report to Host",            L"Navigation Start",       "report" },
    { SoundLrBadIp,         L"LR", L"App Finds No Workable IP Address",   L"Battery Low",            "badip" },
};

std::wstring soundDefaultFile(const wchar_t* category)
{
    return std::wstring(kSoundFilePrefix) + category + L".wav";
}

int soundCategoryIndex(const std::wstring& category)
{
    for (int i = 0; i < kSoundCategoryCount; ++i)
        if (_wcsicmp(kSoundCategories[i], category.c_str()) == 0) return i;
    return -1;
}

const SoundEventDef* soundEventOfCategory(const wchar_t* category)
{
    for (int i = 0; i < kSoundEventCount; ++i)
        if (_wcsicmp(kSoundEvents[i].category, category) == 0) return &kSoundEvents[i];
    return NULL;
}

namespace {

// One lock over the player state; the MCI device is one per process anyway.
struct Lock {
    CRITICAL_SECTION cs;
    Lock() { InitializeCriticalSection(&cs); }
    ~Lock() { DeleteCriticalSection(&cs); }
} g_lock;

struct Hold {
    Hold() { EnterCriticalSection(&g_lock.cs); }
    ~Hold() { LeaveCriticalSection(&g_lock.cs); }
};

bool g_muted = false;

// .wav files held in memory for PlaySound(SND_MEMORY): the bytes, and the size
// and write time they were read at, so that a replaced file is read again.
struct WavData { std::vector<char> bytes; ULONGLONG size; FILETIME written; };
std::map<std::wstring, WavData*> g_wavs;
const ULONGLONG kMaxHeldWav = 8u << 20;   // larger files play from disk

// The MCI device: one alias, reopened for every sound. generation changes at
// every open and close, so that a watcher thread started for one sound does
// nothing to a later one.
const wchar_t* const kAlias = L"tlrsound";
bool g_mciOpen = false;
unsigned g_mciGeneration = 0;

std::wstring extensionOf(const std::wstring& path)
{
    size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && slash > dot)) return std::wstring();
    std::wstring ext = path.substr(dot);
    for (size_t i = 0; i < ext.size(); ++i) ext[i] = (wchar_t)towlower(ext[i]);
    return ext;
}

bool isWav(const std::wstring& path) { return extensionOf(path) == L".wav"; }

std::string mciErrorText(MCIERROR e)
{
    wchar_t buf[256];
    if (mciGetErrorStringW(e, buf, 256)) return narrow(buf);
    return format("MCI error %u", (unsigned)e);
}

MCIERROR mci(const std::wstring& command, std::wstring* answer = NULL)
{
    wchar_t ret[128] = L"";
    MCIERROR e = mciSendStringW(command.c_str(), ret, 128, NULL);
    if (answer) *answer = ret;
    return e;
}

// Lock held.
void mciCloseLocked()
{
    if (g_mciOpen) {
        mci(std::wstring(L"stop ") + kAlias);
        mci(std::wstring(L"close ") + kAlias);
        g_mciOpen = false;
    }
    ++g_mciGeneration;
}

// Lock held. Opens path on the alias, the MPEGVideo device first (it decodes
// .mp3 on every Windows from XP on), then whatever MCI picks by extension.
bool mciOpenLocked(const std::wstring& path, std::string& error)
{
    mciCloseLocked();
    std::wstring quoted = L"\"" + path + L"\"";
    MCIERROR e = mci(L"open " + quoted + L" type mpegvideo alias " + kAlias);
    if (e != 0) {
        MCIERROR e2 = mci(L"open " + quoted + L" alias " + kAlias);
        if (e2 != 0) { error = mciErrorText(e); return false; }
    }
    g_mciOpen = true;
    ++g_mciGeneration;
    return true;
}

// Lock held. True once the device reports stopped, or no longer answers.
bool mciEndedLocked()
{
    std::wstring mode;
    if (mci(std::wstring(L"status ") + kAlias + L" mode", &mode) != 0) return true;
    return mode == L"stopped";
}

// Closes the device when the sound of generation gen has ended; ends at once
// when another sound has taken the device over.
DWORD WINAPI mciWatchThread(LPVOID p)
{
    unsigned gen = (unsigned)(UINT_PTR)p;
    for (;;) {
        Sleep(100);
        Hold h;
        if (g_mciGeneration != gen) return 0;
        if (mciEndedLocked()) { mciCloseLocked(); return 0; }
    }
}

bool fileInfo(const std::wstring& path, ULONGLONG& size, FILETIME& written, std::string& error)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        error = narrow(path) + ": " + lastErrorText((int)GetLastError());
        return false;
    }
    size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    written = fa.ftLastWriteTime;
    return true;
}

// Lock held. The held copy of a .wav, read now when it is not held yet or the
// file has changed; NULL without error for a file too large to hold.
WavData* heldWavLocked(const std::wstring& path, std::string& error)
{
    ULONGLONG size = 0;
    FILETIME written;
    if (!fileInfo(path, size, written, error)) return NULL;
    if (size > kMaxHeldWav || size == 0) return NULL;
    std::map<std::wstring, WavData*>::iterator it = g_wavs.find(path);
    if (it != g_wavs.end() && it->second->size == size && CompareFileTime(&it->second->written, &written) == 0) return it->second;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { error = narrow(path) + ": cannot be opened"; return NULL; }
    WavData* d = new WavData();
    d->bytes.resize((size_t)size);
    size_t got = fread(&d->bytes[0], 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) { delete d; error = narrow(path) + ": short read"; return NULL; }
    d->size = size;
    d->written = written;
    if (it != g_wavs.end()) {
        PlaySoundW(NULL, NULL, 0);   // a copy that may still be playing is not freed under the player
        delete it->second;
        g_wavs.erase(it);
    }
    g_wavs[path] = d;
    return d;
}

bool playWav(const std::wstring& path, bool wait, std::string& error)
{
    const void* memory = NULL;
    {
        Hold h;
        mciCloseLocked();   // one sound at a time, whatever device the previous one used
        WavData* d = heldWavLocked(path, error);
        if (!d && !error.empty()) return false;
        if (d) memory = &d->bytes[0];
    }
    DWORD flags = SND_NODEFAULT | (wait ? SND_SYNC : SND_ASYNC);
    BOOL ok = memory ? PlaySoundW((LPCWSTR)memory, NULL, flags | SND_MEMORY)
                     : PlaySoundW(path.c_str(), NULL, flags | SND_FILENAME);
    if (!ok) { error = narrow(path) + ": PlaySound refused the file (no playback device, or not a wave file)"; return false; }
    return true;
}

bool playMci(const std::wstring& path, DWORD waitMs, std::string& error)
{
    unsigned gen = 0;
    {
        Hold h;
        PlaySoundW(NULL, NULL, 0);
        if (!mciOpenLocked(path, error)) return false;
        MCIERROR e = mci(std::wstring(L"play ") + kAlias + L" from 0");
        if (e != 0) { error = narrow(path) + ": " + mciErrorText(e); mciCloseLocked(); return false; }
        gen = g_mciGeneration;
    }
    if (waitMs == 0) {
        HANDLE t = CreateThread(NULL, 0, mciWatchThread, (LPVOID)(UINT_PTR)gen, 0, NULL);
        if (t) CloseHandle(t);
        return true;
    }
    DWORD start = GetTickCount();
    for (;;) {
        Sleep(50);
        Hold h;
        if (g_mciGeneration != gen) return true;    // taken over by a later sound
        if (mciEndedLocked() || GetTickCount() - start >= waitMs) { mciCloseLocked(); return true; }
    }
}

} // namespace

void soundSetMuted(bool muted)
{
    g_muted = muted;
    if (muted) soundStop();
}

bool soundMuted() { return g_muted; }

bool soundFileSupported(const std::wstring& path)
{
    std::wstring ext = extensionOf(path);
    return ext == L".wav" || ext == L".mp3";
}

std::vector<std::wstring> soundFilesIn(const std::wstring& dir)
{
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (soundFileSupported(fd.cFileName)) out.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    struct Less { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
    std::sort(out.begin(), out.end(), Less());
    return out;
}

bool soundPlay(const std::wstring& path, std::string& error)
{
    if (g_muted) return true;
    return isWav(path) ? playWav(path, false, error) : playMci(path, 0, error);
}

bool soundPlayAndWait(const std::wstring& path, DWORD maxMs, std::string& error)
{
    if (g_muted) return true;
    if (maxMs == 0) maxMs = 1;
    return isWav(path) ? playWav(path, true, error) : playMci(path, maxMs, error);
}

void soundStop()
{
    Hold h;
    PlaySoundW(NULL, NULL, 0);
    mciCloseLocked();
}

void soundShutdown()
{
    soundStop();
    Hold h;
    for (std::map<std::wstring, WavData*>::iterator it = g_wavs.begin(); it != g_wavs.end(); ++it) delete it->second;
    g_wavs.clear();
}

} // namespace tlr
