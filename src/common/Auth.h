#pragma once

#include <string>

namespace tlr {

// Host authentication; see docs/PROTOCOL.md under Authentication.
//
// Both programs hold the same secret, farm.key, a line of 64 hexadecimal
// digits beside the executable. Thunderhead creates its own at first start;
// an agent gets its copy when Thunderhead writes the file into the node's
// shared folder, which Windows allows only to a host that holds the
// credentials of that share. Every OFFER and every HELLO then carries an
// HMAC-SHA-256 over a challenge token the other side chose, so that an agent connects
// only to a host that holds the key and Thunderhead accepts only an agent
// that holds it.
const wchar_t* const kFarmKeyFile = L"farm.key";
const size_t kFarmKeyBytes = 32;              // 64 hexadecimal digits
const size_t kChallengeBytes = 16;                // 32 hexadecimal digits

std::string sha256Hex(const std::string& data);
std::string hmacSha256Hex(const std::string& key, const std::string& data);
std::string randomHex(size_t bytes);          // CryptGenRandom; empty on failure
bool equalsConstantTime(const std::string& a, const std::string& b);

// The proofs as both sides compute them. challenge token is the agent's ANNOUNCE challenge token
// for the OFFER, the host's OFFER challenge token for the HELLO; port is decimal text.
std::string offerAuth(const std::string& key, const std::string& challenge, const std::string& host, const std::string& port);
std::string helloAuth(const std::string& key, const std::string& challenge, const std::string& mac);

// farm.key: the first line, trimmed, in lower case; empty when the file is
// missing or does not hold kFarmKeyBytes * 2 hexadecimal digits. path may be
// a UNC path. writeKeyFile writes a temporary file and renames it into place.
std::string readKeyFile(const std::wstring& path);
bool writeKeyFile(const std::wstring& path, const std::string& key, std::string& error);
bool validKey(const std::string& key);

} // namespace tlr
