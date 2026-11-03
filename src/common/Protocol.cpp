#include "Protocol.h"
#include "StrUtil.h"

#include <cstdlib>

namespace tlr {

static const std::string kEmpty;

const std::string& Message::get(const std::string& key) const
{
    std::map<std::string, std::string>::const_iterator it = fields.find(key);
    return it == fields.end() ? kEmpty : it->second;
}

int Message::getInt(const std::string& key, int def) const
{
    const std::string& v = get(key);
    if (v.empty()) return def;
    return atoi(v.c_str());
}

void Message::set(const std::string& key, const std::string& value)
{
    std::string clean;
    clean.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
        if (value[i] != '\r' && value[i] != '\n') clean += value[i];
    fields[key] = clean;
}

void Message::set(const std::string& key, long long value)
{
    fields[key] = format("%lld", value);
}

std::string Message::encode() const
{
    std::string out = verb;
    out += '\n';
    for (std::map<std::string, std::string>::const_iterator it = fields.begin(); it != fields.end(); ++it) {
        out += it->first;
        out += '=';
        out += it->second;
        out += '\n';
    }
    out += '\n';
    return out;
}

std::string Message::oneLine() const
{
    std::string out = verb;
    for (std::map<std::string, std::string>::const_iterator it = fields.begin(); it != fields.end(); ++it) {
        out += ' ';
        out += it->first;
        out += '=';
        out += it->second;
    }
    return out;
}

bool parseMessage(const std::string& text, Message& out)
{
    out.verb.clear();
    out.fields.clear();
    std::vector<std::string> lines = split(text, '\n');
    bool haveVerb = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty()) {
            if (haveVerb) break;
            continue;
        }
        if (!haveVerb) {
            out.verb = line;
            haveVerb = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.fields[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return haveVerb;
}

void MessageParser::feed(const char* data, size_t len)
{
    buffer_.append(data, len);
}

bool MessageParser::next(Message& out)
{
    // A message ends at the first empty line after at least one non-empty line.
    size_t pos = 0;
    bool seenContent = false;
    while (pos < buffer_.size()) {
        size_t nl = buffer_.find('\n', pos);
        if (nl == std::string::npos) return false;
        std::string line = buffer_.substr(pos, nl - pos);
        if (!line.empty() && line[line.size() - 1] == '\r') line.erase(line.size() - 1);
        if (line.empty()) {
            if (seenContent) {
                std::string text = buffer_.substr(0, nl + 1);
                buffer_.erase(0, nl + 1);
                return parseMessage(text, out);
            }
            buffer_.erase(0, nl + 1);   // stray blank line before any content
            pos = 0;
            continue;
        }
        seenContent = true;
        pos = nl + 1;
    }
    return false;
}

} // namespace tlr
