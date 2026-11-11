#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace tlr {

// Helpers for finding programs the node has installed, shared by Lightning.cpp
// and TeamViewer.cpp: registry text in either view, Add/Remove Programs
// entries, Start menu shortcuts, Program Files folders, version resources.

bool fileExists(const std::wstring& path);
bool startsWithNoCase(const std::wstring& s, const wchar_t* head);
bool endsWithNoCase(const std::wstring& s, const wchar_t* tail);
// dir\name, with the trailing separators of dir removed and / turned into \.
std::wstring joinPath(std::wstring dir, const wchar_t* name);
// "C:\x\a.exe,0" or "\"C:\x\a.exe\" /q" reduced to C:\x\a.exe.
std::wstring exeIn(const std::wstring& value);

// The two registry views. Index 0 is the view of this process, which under
// WOW64 is the 32-bit one (SOFTWARE\WOW6432Node); index 1 is the 64-bit view.
// On 32-bit Windows both reach the same key. A 64-bit installer writes the
// 64-bit view, a 32-bit installer the 32-bit one.
extern const REGSAM kRegViews[2];
bool isWow64();
// The key as regedit shows it, WOW6432Node included where the view implies it.
std::string regKeyText(HKEY root, const std::wstring& path, REGSAM view);
std::wstring regText(HKEY root, const std::wstring& path, const wchar_t* value, REGSAM view);
std::vector<std::wstring> regSubKeys(HKEY root, const std::wstring& path, REGSAM view);

// The target of a shortcut, through IShellLink; empty when it cannot be read.
std::wstring shortcutTarget(const std::wstring& lnk);
// The target of the first *.lnk under the Start menu (all users, then this
// user, recursively) whose file name contains nameContains and whose target
// exists and ends with targetEndsWith; how says which shortcut.
std::wstring startMenuTarget(const wchar_t* nameContains, const wchar_t* targetEndsWith, std::string& how);
// Every such shortcut's target in the order found, for a caller that chooses
// among several (a Start menu holding more than one Lightning generation).
struct ShortcutHit { std::wstring path; std::string how; };
std::vector<ShortcutHit> startMenuTargets(const wchar_t* nameContains, const wchar_t* targetEndsWith);
// The first <Program Files folder>\tail that exists, over Program Files (x86),
// Program Files and the 64-bit Program Files; empty when none does.
std::wstring programFilesFile(const wchar_t* tail);
// "7.1.0.109" from the file's version resource (product version, else file
// version); empty when the file has none.
std::string fileVersionText(const std::wstring& path);
// The first number of such a version text, 0 when there is none.
int versionMajor(const std::string& version);

} // namespace tlr
