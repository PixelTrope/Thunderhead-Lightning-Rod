#include "Auth.h"
#include "StrUtil.h"

#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <cstring>

namespace tlr {

namespace {

// SHA-256 as FIPS 180-4 describes it, written out here so that the XP agent
// and the Windows 10 host compute it with the same code and no dependency.
struct Sha256 {
    unsigned int h[8];
    unsigned char block[64];
    size_t blockLen;
    unsigned long long total;

    Sha256() { reset(); }

    void reset()
    {
        static const unsigned int init[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
        memcpy(h, init, sizeof(h));
        blockLen = 0;
        total = 0;
    }

    static unsigned int rotr(unsigned int x, int n) { return (x >> n) | (x << (32 - n)); }

    void compress(const unsigned char* p)
    {
        static const unsigned int k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
        unsigned int w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((unsigned int)p[i * 4] << 24) | ((unsigned int)p[i * 4 + 1] << 16) | ((unsigned int)p[i * 4 + 2] << 8) | (unsigned int)p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            unsigned int s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            unsigned int s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        unsigned int a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            unsigned int S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            unsigned int ch = (e & f) ^ (~e & g);
            unsigned int t1 = hh + S1 + ch + k[i] + w[i];
            unsigned int S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
            unsigned int t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const unsigned char* data, size_t len)
    {
        total += len;
        while (len > 0) {
            size_t take = 64 - blockLen;
            if (take > len) take = len;
            memcpy(block + blockLen, data, take);
            blockLen += take;
            data += take;
            len -= take;
            if (blockLen == 64) { compress(block); blockLen = 0; }
        }
    }

    void finish(unsigned char out[32])
    {
        unsigned long long bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (blockLen != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = (unsigned char)(bits >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = (unsigned char)(h[i] >> 24);
            out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
            out[i * 4 + 2] = (unsigned char)(h[i] >> 8);
            out[i * 4 + 3] = (unsigned char)h[i];
        }
    }
};

std::string sha256Raw(const std::string& data)
{
    Sha256 s;
    s.update((const unsigned char*)data.data(), data.size());
    unsigned char out[32];
    s.finish(out);
    return std::string((const char*)out, 32);
}

std::string winError(int err)
{
    char buf[512];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, (DWORD)err,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf, sizeof(buf), NULL);
    std::string text = n ? std::string(buf, n) : std::string();
    while (!text.empty() && (text[text.size() - 1] == '\r' || text[text.size() - 1] == '\n' || text[text.size() - 1] == ' ')) text.erase(text.size() - 1);
    return format("%s (%d)", text.c_str(), err);
}

std::string readSmallFile(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::string();
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return std::string(buf, n);
}

} // namespace

std::string sha256Hex(const std::string& data) { return hexEncode(sha256Raw(data)); }

std::string hmacSha256Hex(const std::string& key, const std::string& data)
{
    std::string k = key.size() > 64 ? sha256Raw(key) : key;
    k.resize(64, '\0');
    std::string ipad(64, '\0'), opad(64, '\0');
    for (size_t i = 0; i < 64; ++i) { ipad[i] = (char)(k[i] ^ 0x36); opad[i] = (char)(k[i] ^ 0x5c); }
    return hexEncode(sha256Raw(opad + sha256Raw(ipad + data)));
}

std::string randomHex(size_t bytes)
{
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) return std::string();
    std::string buf(bytes, '\0');
    bool ok = CryptGenRandom(prov, (DWORD)bytes, (BYTE*)&buf[0]) != 0;
    CryptReleaseContext(prov, 0);
    return ok ? hexEncode(buf) : std::string();
}

bool equalsConstantTime(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

std::string offerAuth(const std::string& key, const std::string& challenge, const std::string& host, const std::string& port)
{
    return hmacSha256Hex(key, "OFFER\n" + challenge + "\n" + host + "\n" + port);
}

std::string helloAuth(const std::string& key, const std::string& challenge, const std::string& mac)
{
    return hmacSha256Hex(key, "HELLO\n" + challenge + "\n" + mac);
}

bool validKey(const std::string& key)
{
    if (key.size() != kFarmKeyBytes * 2) return false;
    for (size_t i = 0; i < key.size(); ++i) {
        char c = key[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string readKeyFile(const std::wstring& path)
{
    std::string text = readSmallFile(path);
    size_t nl = text.find_first_of("\r\n");
    if (nl != std::string::npos) text.erase(nl);
    std::string key = toLower(trim(text));
    return validKey(key) ? key : std::string();
}

bool writeKeyFile(const std::wstring& path, const std::string& key, std::string& error)
{
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { error = "create: " + winError((int)GetLastError()); return false; }
    std::string text = key + "\r\n";
    DWORD wrote = 0;
    bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, NULL) && wrote == text.size();
    DWORD err = GetLastError();
    CloseHandle(f);
    if (!ok) { DeleteFileW(tmp.c_str()); error = "write: " + winError((int)err); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = "rename: " + winError((int)GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

} // namespace tlr
