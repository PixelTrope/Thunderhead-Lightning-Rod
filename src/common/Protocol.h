#pragma once

#include <map>
#include <string>

namespace tlr {

// Fixed numbers of the version 1 protocol; see docs/PROTOCOL.md.
const unsigned short kDiscoveryPort         = 42001; // UDP, Thunderhead listens
const unsigned short kDefaultTcpPort        = 42002; // TCP, Thunderhead listens
const int            kAnnounceIntervalSec   = 5;
const int            kDefaultReportInterval = 5;
const int            kMissedReportsOffline  = 3;
const int            kOfflineGraceSec       = 2;
const size_t         kMaxMessageBytes       = 65536;

// One message: a verb line followed by key=value lines and an empty line.
struct Message {
    std::string verb;
    std::map<std::string, std::string> fields;

    Message() {}
    explicit Message(const std::string& v) : verb(v) {}

    const std::string& get(const std::string& key) const;
    int  getInt(const std::string& key, int def = 0) const;
    bool has(const std::string& key) const { return fields.find(key) != fields.end(); }
    void set(const std::string& key, const std::string& value);
    void set(const std::string& key, long long value);

    std::string encode() const;      // wire form
    std::string oneLine() const;     // compact form for logs
};

// Parses a complete message held in text. Returns false if no verb.
bool parseMessage(const std::string& text, Message& out);

// Accumulates stream bytes and yields complete messages.
class MessageParser {
public:
    void   feed(const char* data, size_t len);
    bool   next(Message& out);
    bool   overflow() const { return buffer_.size() > kMaxMessageBytes; }
    void   clear() { buffer_.clear(); }
private:
    std::string buffer_;
};

} // namespace tlr
