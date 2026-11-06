#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>

namespace tlr {

bool netInit();                 // WSAStartup
void netShutdown();             // WSACleanup
std::string ipText(const sockaddr_in& a);      // dotted address
std::string ipText(unsigned long netOrder);    // dotted address
bool setNonBlocking(SOCKET s, bool on);
bool sendAll(SOCKET s, const std::string& data);   // blocking; false on error
unsigned long long tickMs();    // milliseconds since boot, wrap-safe
unsigned long long utcNowMs();  // the system clock: milliseconds since 1970-01-01 00:00 UTC
std::string lastErrorText(int err);

} // namespace tlr
