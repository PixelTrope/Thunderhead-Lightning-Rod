#include "BryceScene.h"
#include "Net.h"
#include "StrUtil.h"

extern "C" {
#include "puff.h"
}

#include <windows.h>
#include <cstring>
#include <vector>

namespace tlr {

namespace {

const size_t kRecordBytes = 416;
const size_t kChunk = 1024 * 1024;
const size_t kOverlap = 4096;      // the record compresses to about 120 bytes

unsigned int be32(const unsigned char* p) { return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3]; }

float beFloat(const unsigned char* p)
{
    unsigned int u = be32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

// Tries to inflate a zlib stream starting at p into exactly kRecordBytes
// bytes that look like the render settings record.
bool recordAt(const unsigned char* p, size_t avail, unsigned char* record)
{
    if (avail < 8 || p[0] != 0x78) return false;
    if (((p[0] << 8) | p[1]) % 31 != 0) return false;   // zlib header check
    unsigned long destLen = kRecordBytes, srcLen = (unsigned long)(avail - 2);
    if (puff(record, &destLen, p + 2, &srcLen) != 0 || destLen != kRecordBytes) return false;
    // document width and height, then the render width and height, which the
    // record repeats as height and width at 0x18 and 0x1c
    unsigned int dw = be32(record), dh = be32(record + 4), rw = be32(record + 8), rh = be32(record + 12);
    return dw > 0 && dw < 32768 && dh > 0 && dh < 32768 && rw > 0 && rw < 32768 && rh > 0 && rh < 32768 &&
           be32(record + 0x18) == rh && be32(record + 0x1c) == rw;
}

} // namespace

bool readBryceScene(const std::wstring& path, BryceSceneInfo& out, std::string& error)
{
    out = BryceSceneInfo();
    error.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) { error = "cannot open: " + lastErrorText((int)GetLastError()); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart < 64) { CloseHandle(f); error = "not a Bryce scene (too small)"; return false; }
    char head[16];
    DWORD got = 0;
    if (!ReadFile(f, head, 16, &got, NULL) || got != 16 || memcmp(head, "Bryce_", 6) != 0) { CloseHandle(f); error = "not a Bryce scene (no Bryce_ header)"; return false; }
    unsigned char record[kRecordBytes];
    std::vector<unsigned char> buf;
    bool found = false;
    unsigned long long end = (unsigned long long)sz.QuadPart;
    while (!found && end > 0) {
        unsigned long long start = end > kChunk + kOverlap ? end - kChunk - kOverlap : 0;
        size_t len = (size_t)(end - start);
        buf.resize(len);
        LARGE_INTEGER pos;
        pos.QuadPart = (LONGLONG)start;
        if (!SetFilePointerEx(f, pos, NULL, FILE_BEGIN) || !ReadFile(f, &buf[0], (DWORD)len, &got, NULL) || got != len) { error = "read failed: " + lastErrorText((int)GetLastError()); break; }
        for (size_t i = 0; i + 8 < len && !found; ++i)
            if (buf[i] == 0x78 && recordAt(&buf[i], len - i, record)) found = true;
        if (start == 0) break;
        end = start + kOverlap;
    }
    CloseHandle(f);
    if (!found) { if (error.empty()) error = "no render settings record found in the scene"; return false; }
    out.docWidth = (int)be32(record);
    out.docHeight = (int)be32(record + 4);
    out.width = (int)be32(record + 8);     // the render size: what the frames and Bryce's own .bmp have
    out.height = (int)be32(record + 12);
    out.seconds = beFloat(record + 0x3c);
    out.fps = beFloat(record + 0x40);
    if (out.seconds < 0 || out.seconds > 1e6 || out.fps < 0 || out.fps > 1000) { out.seconds = 0; out.fps = 0; }
    out.lastFrame = out.seconds > 0 && out.fps > 0 ? (int)(out.seconds * out.fps + 0.5) : 0;
    return true;
}

bool writeSolidBmp(const std::wstring& path, int width, int height, unsigned char gray, std::string& error)
{
    error.clear();
    if (width <= 0 || height <= 0 || width > 32768 || height > 32768) { error = "bad size"; return false; }
    unsigned int rowBytes = ((unsigned int)width * 3 + 3) & ~3u;
    unsigned int pixelBytes = rowBytes * (unsigned int)height;
    unsigned char hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    unsigned int fileSize = 54 + pixelBytes, offBits = 54, infoSize = 40;
    memcpy(hdr + 2, &fileSize, 4);
    memcpy(hdr + 10, &offBits, 4);
    memcpy(hdr + 14, &infoSize, 4);
    memcpy(hdr + 18, &width, 4);
    memcpy(hdr + 22, &height, 4);
    unsigned short planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &pixelBytes, 4);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { error = lastErrorText((int)GetLastError()); return false; }
    DWORD wrote = 0;
    bool ok = WriteFile(f, hdr, 54, &wrote, NULL) && wrote == 54;
    std::vector<unsigned char> row(rowBytes, gray);
    for (unsigned int i = 0; i < rowBytes - (unsigned int)width * 3; ++i) row[rowBytes - 1 - i] = 0;   // padding
    for (int y = 0; y < height && ok; ++y) ok = WriteFile(f, &row[0], rowBytes, &wrote, NULL) && wrote == rowBytes;
    if (!ok) error = lastErrorText((int)GetLastError());
    CloseHandle(f);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

} // namespace tlr
