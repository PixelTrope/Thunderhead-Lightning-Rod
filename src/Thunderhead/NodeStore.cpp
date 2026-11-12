#include "NodeStore.h"
#include "StrUtil.h"

#include <windows.h>
#include <cstdio>

namespace tlr {

namespace {

std::string readFile(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

bool writeFile(const std::wstring& path, const std::string& text)
{
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace

NodeStore::NodeStore(const std::wstring& dir) : dir_(dir) {}

void NodeStore::load()
{
    // settings.txt
    std::vector<std::string> lines = split(readFile(dir_ + L"\\settings.txt"), '\n');
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "interval") settings.intervalSec = atoi(value.c_str());
        else if (key == "tcp_port") settings.tcpPort = atoi(value.c_str());
        else if (key == "agent_file") settings.agentFile = value;
        else if (key == "share_name") settings.shareName = value;
        else if (key == "dark_mode") settings.darkMode = atoi(value.c_str()) != 0;
        else if (key == "th_audio") settings.thAudio = atoi(value.c_str()) != 0;
        else if (key == "lr_audio") settings.lrAudio = atoi(value.c_str()) != 0;
        else if (key == "sound_scheme") settings.customScheme = toLower(value) == "custom";
        else if (key.compare(0, 6, "sound.") == 0 && !value.empty()) settings.sounds[key.substr(6)] = value;
    }
    if (settings.shareName.empty()) settings.shareName = "LightningRod";
    if (settings.intervalSec < 5) settings.intervalSec = kDefaultReportInterval;
    if (settings.tcpPort < 1 || settings.tcpPort > 65535) settings.tcpPort = kDefaultTcpPort;

    // nodes.txt
    for (size_t i = 0; i < nodes_.size(); ++i) delete nodes_[i];
    nodes_.clear();
    lines = split(readFile(dir_ + L"\\nodes.txt"), '\n');
    NodeRecord* cur = NULL;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[' && line[line.size() - 1] == ']') {
            std::string mac = trim(line.substr(1, line.size() - 2));
            cur = mac.empty() ? NULL : &add(mac);
            continue;
        }
        if (!cur) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (key == "name") cur->name = value;
        else if (key == "group") cur->group = value;
        else if (key == "wol") cur->wol = parseBool(value);
        else if (key == "boot") cur->boot = parseBool(value);
        else if (key == "autologin") cur->autoLogin = parseBool(value);
        else if (key == "autostart_rod") cur->autoRod = parseBool(value);
        else if (key == "autostart_lightning") cur->autoLightning = parseBool(value);
        else if (key == "auto_shutdown") cur->autoShutdown = parseBool(value);
        else if (key == "comment") cur->comment = value;
    }
}

void NodeStore::save()
{
    std::string out = "# Thunderhead node database. One block per node, keyed by MAC.\r\n";
    for (size_t i = 0; i < nodes_.size(); ++i) {
        const NodeRecord& n = *nodes_[i];
        out += "\r\n[" + n.mac + "]\r\n";
        out += "name=" + n.name + "\r\n";
        out += "group=" + n.group + "\r\n";
        out += std::string("wol=") + boolText(n.wol) + "\r\n";
        out += std::string("boot=") + boolText(n.boot) + "\r\n";
        out += std::string("autologin=") + boolText(n.autoLogin) + "\r\n";
        out += std::string("autostart_rod=") + boolText(n.autoRod) + "\r\n";
        out += std::string("autostart_lightning=") + boolText(n.autoLightning) + "\r\n";
        out += std::string("auto_shutdown=") + boolText(n.autoShutdown) + "\r\n";
        out += "comment=" + n.comment + "\r\n";
    }
    writeFile(dir_ + L"\\nodes.txt", out);
}

void NodeStore::saveSettings()
{
    std::string out = format("interval=%d\r\ntcp_port=%d\r\n", settings.intervalSec, settings.tcpPort);
    out += "agent_file=" + settings.agentFile + "\r\n";
    out += "share_name=" + settings.shareName + "\r\n";
    out += format("dark_mode=%d\r\n", settings.darkMode ? 1 : 0);
    out += format("th_audio=%d\r\n", settings.thAudio ? 1 : 0);
    out += format("lr_audio=%d\r\n", settings.lrAudio ? 1 : 0);
    out += std::string("sound_scheme=") + (settings.customScheme ? "custom" : "default") + "\r\n";
    for (std::map<std::string, std::string>::const_iterator it = settings.sounds.begin(); it != settings.sounds.end(); ++it)
        out += "sound." + it->first + "=" + it->second + "\r\n";
    writeFile(dir_ + L"\\settings.txt", out);
}

NodeRecord* NodeStore::find(const std::string& mac)
{
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i]->mac == mac) return nodes_[i];
    return NULL;
}

NodeRecord* NodeStore::findByConn(int conn)
{
    if (conn < 0) return NULL;
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i]->conn == conn) return nodes_[i];
    return NULL;
}

NodeRecord& NodeStore::add(const std::string& mac)
{
    NodeRecord* existing = find(mac);
    if (existing) return *existing;
    NodeRecord* n = new NodeRecord();
    n->mac = mac;
    nodes_.push_back(n);
    return *n;
}

void NodeStore::remove(const std::string& mac)
{
    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i]->mac == mac) {
            delete nodes_[i];
            nodes_.erase(nodes_.begin() + i);
            return;
        }
    }
}

} // namespace tlr
