#include "Log.h"
#include "StrUtil.h"

namespace tlr {

Logger::Logger() : file_(NULL), day_(-1)
{
    InitializeCriticalSection(&lock_);
}

Logger::~Logger()
{
    close();
    DeleteCriticalSection(&lock_);
}

void Logger::open(const std::wstring& dir, const std::wstring& base)
{
    EnterCriticalSection(&lock_);
    dir_ = dir;
    base_ = base;
    CreateDirectoryW(dir_.c_str(), NULL);
    LeaveCriticalSection(&lock_);
}

void Logger::close()
{
    EnterCriticalSection(&lock_);
    if (file_) { fclose(file_); file_ = NULL; }
    day_ = -1;
    LeaveCriticalSection(&lock_);
}

void Logger::ensureFile(const SYSTEMTIME& st)
{
    int day = st.wYear * 10000 + st.wMonth * 100 + st.wDay;
    if (file_ && day == day_) return;
    if (file_) { fclose(file_); file_ = NULL; }
    wchar_t name[64];
    _snwprintf(name, 63, L"%s-%04d-%02d-%02d.log", base_.c_str(), st.wYear, st.wMonth, st.wDay);
    name[63] = L'\0';
    std::wstring path = dir_ + L"\\" + name;
    file_ = _wfopen(path.c_str(), L"ab");
    day_ = day;
}

void Logger::write(const std::string& line)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&lock_);
    if (!dir_.empty()) {
        ensureFile(st);
        if (file_) {
            fprintf(file_, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line.c_str());
            fflush(file_);
        }
    }
    LeaveCriticalSection(&lock_);
}

} // namespace tlr
