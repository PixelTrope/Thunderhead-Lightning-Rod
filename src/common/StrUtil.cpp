#include "StrUtil.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace tlr {

std::string narrow(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, NULL, NULL);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::wstring trim(const std::wstring& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        if (p == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

std::string format(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    return std::string(buf, (size_t)n);
}

std::string toLower(const std::string& s)
{
    std::string out(s);
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
    return out;
}

bool parseBool(const std::string& s, bool def)
{
    std::string v = toLower(trim(s));
    if (v == "yes" || v == "true" || v == "1" || v == "on") return true;
    if (v == "no" || v == "false" || v == "0" || v == "off") return false;
    return def;
}

const char* boolText(bool b)
{
    return b ? "yes" : "no";
}

std::string stripIndirect(const std::string& s)
{
    if (!s.empty() && s[0] == '@') {
        size_t p = s.rfind(';');
        if (p != std::string::npos) return trim(s.substr(p + 1));
    }
    return trim(s);
}

std::string hexEncode(const std::string& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (size_t i = 0; i < bytes.size(); ++i) {
        unsigned char c = (unsigned char)bytes[i];
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

bool hexDecode(const std::string& hex, std::string& bytes)
{
    bytes.clear();
    if (hex.size() % 2 != 0) return false;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = hex[i + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) { bytes.clear(); return false; }
            v = v * 16 + d;
        }
        bytes += (char)v;
    }
    return true;
}

} // namespace tlr
