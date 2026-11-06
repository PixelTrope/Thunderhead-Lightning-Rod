#pragma once

#include <string>
#include <vector>

namespace tlr {

std::string  narrow(const std::wstring& s);   // UTF-16 -> UTF-8
std::wstring widen(const std::string& s);     // UTF-8 -> UTF-16
std::string  trim(const std::string& s);
std::wstring trim(const std::wstring& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string  format(const char* fmt, ...);
std::string  toLower(const std::string& s);
bool         parseBool(const std::string& s, bool def = false);
const char*  boolText(bool b);                // "yes" / "no"
std::string  stripIndirect(const std::string& s); // "@file,%id%;Text" -> "Text"
std::string  hexEncode(const std::string& bytes);  // two lower-case digits per byte
bool         hexDecode(const std::string& hex, std::string& bytes);

} // namespace tlr
