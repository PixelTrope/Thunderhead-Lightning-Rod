#pragma once

#include <windows.h>
#include <string>

namespace tlr {

// Where TeamViewer is installed on this node, whatever its version and
// wherever its installer put it. Looked for, in this order: the TeamViewer
// product key (HKLM\SOFTWARE\TeamViewer and its VersionN subkeys of the older
// releases, InstallationDirectory), the Add/Remove Programs entries whose name
// starts with TeamViewer (InstallLocation, DisplayIcon, UninstallString),
// App Paths\TeamViewer.exe, the TeamViewer shortcuts of the Start menu for all
// users and for this user, then TeamViewer\TeamViewer.exe under each Program
// Files folder. Both registry views are read on 64-bit Windows. how receives
// which source gave the path. Empty when nothing is found.
std::wstring teamViewerPath(std::string& how);

// Whether a TeamViewer.exe (the program, not its service) is running.
bool teamViewerRunning();

// Starts the installed TeamViewer as a double-click on its shortcut would.
// When TeamViewer is already running, the new copy hands over to the running
// one and that one shows its window, which is TeamViewer's own behaviour.
// wasRunning says whether a TeamViewer.exe existed before the start. Returns
// false with error set when none is installed or Windows refuses the start.
bool startTeamViewer(std::wstring& path, std::string& how, bool& wasRunning, std::string& error);

// The largest visible top-level window of a TeamViewer.exe process, or NULL.
HWND teamViewerWindow();

} // namespace tlr
