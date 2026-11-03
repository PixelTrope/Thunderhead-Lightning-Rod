#pragma once

#include <windows.h>
#include <cstdio>
#include <string>

namespace tlr {

// Appends timestamped lines to <dir>\<base>-YYYY-MM-DD.log, switching file at midnight.
class Logger {
public:
    Logger();
    ~Logger();
    void open(const std::wstring& dir, const std::wstring& base);
    void write(const std::string& line);      // UTF-8 text without newline
    void close();
private:
    void ensureFile(const SYSTEMTIME& st);
    CRITICAL_SECTION lock_;
    std::wstring dir_, base_;
    FILE* file_;
    int day_;
};

} // namespace tlr
