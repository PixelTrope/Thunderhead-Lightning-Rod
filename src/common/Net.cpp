#include "Net.h"
#include "StrUtil.h"

namespace tlr {

bool netInit()
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void netShutdown()
{
    WSACleanup();
}

std::string ipText(unsigned long netOrder)
{
    in_addr a;
    a.s_addr = netOrder;
    const char* s = inet_ntoa(a);
    return s ? std::string(s) : std::string();
}

std::string ipText(const sockaddr_in& a)
{
    return ipText(a.sin_addr.s_addr);
}

bool setNonBlocking(SOCKET s, bool on)
{
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool sendAll(SOCKET s, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(10); continue; }
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

typedef ULONGLONG (WINAPI *GetTickCount64Fn)();

unsigned long long tickMs()
{
    static GetTickCount64Fn fn = (GetTickCount64Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTickCount64");
    static DWORD last = 0;
    static unsigned long long high = 0;
    if (fn) return fn();
    DWORD now = GetTickCount();
    if (now < last) high += 0x100000000ULL;   // 49.7-day wrap
    last = now;
    return high + now;
}

std::string lastErrorText(int err)
{
    char* buf = NULL;
    DWORD n = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             NULL, (DWORD)err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, NULL);
    std::string out = n && buf ? trim(std::string(buf, n)) : format("error %d", err);
    if (buf) LocalFree(buf);
    return out;
}

} // namespace tlr
