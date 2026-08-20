#include "../include/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

namespace jamcli_config_detail {

inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

inline std::string stripQuotes(std::string s) {
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\'' && s.back() == '\''))) {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

inline bool parseBool(const std::string& raw, bool fallback) {
    std::string v = raw;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "true" || v == "yes" || v == "1") return true;
    if (v == "false" || v == "no" || v == "0") return false;
    return fallback;
}

inline bool parseEnabledDisabled(const std::string& raw, bool fallback) {
    std::string v = raw;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); });
    if (v == "enabled" || v == "true" || v == "yes" || v == "1") return true;
    if (v == "disabled" || v == "false" || v == "no" || v == "0") return false;
    return fallback;
}

inline std::string configPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/jamcli/config.yaml";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/jamcli/config.yaml";
    }
    return "";
}

} // namespace jamcli_config_detail

JamcliConfig loadJamcliConfig(std::function<void(std::string)> debugCb) {
    using namespace jamcli_config_detail;
    JamcliConfig cfg;

    std::string path = configPath();
    if (path.empty()) return cfg;

    std::ifstream in(path);
    if (!in) return cfg;

    std::string section, line;
    while (std::getline(in, line)) {
        if (auto hashPos = line.find('#'); hashPos != std::string::npos) {
            line = line.substr(0, hashPos);
        }
        if (trim(line).empty()) continue;

        bool indented = !line.empty() && (line[0] == ' ' || line[0] == '\t');
        std::string content = trim(line);

        if (!indented) {
            if (!content.empty() && content.back() == ':') content.pop_back();
            section = trim(content);
            continue;
        }

        auto colon = content.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(content.substr(0, colon));
        std::string value = stripQuotes(trim(content.substr(colon + 1)));

        if (section == "network") {
            if (key == "interface") cfg.network.interface = value;
            else if (key == "upnp") cfg.network.upnp = parseBool(value, cfg.network.upnp);
            else if (key == "dht-bootstrap") cfg.network.dhtBootstrap = parseBool(value, cfg.network.dhtBootstrap);
            else if (key == "dht-bootstrap-node") cfg.network.dhtBootstrapNode = value;
            else if (key == "turn-server") cfg.network.turnServer = parseBool(value, cfg.network.turnServer);
            else if (key == "turn-server-url") cfg.network.turnServerUrl = value;
            else if (key == "turn-server-username") cfg.network.turnServerUsername = value;
            else if (key == "turn-server-password") cfg.network.turnServerPassword = value;
            else if (key == "turn-server-realm") cfg.network.turnServerRealm = value;
            else if (key == "dht-proxy") cfg.network.dhtProxy = parseBool(value, cfg.network.dhtProxy);
            else if (key == "local-peer-discovery") cfg.network.localPeerDiscovery = parseBool(value, cfg.network.localPeerDiscovery);
            else if (key == "jams") cfg.network.jams = parseEnabledDisabled(value, cfg.network.jams);
            else if (key == "jams-server") cfg.network.jamsServer = value;
            else if (key == "jams-user") cfg.network.jamsUser = value;
        } else if (section == "TURN") {
            if (key == "enable") cfg.turn.enable = parseBool(value, cfg.turn.enable);
            else if (key == "server") cfg.turn.server = value;
            else if (key == "username") cfg.turn.username = value;
            else if (key == "password") cfg.turn.password = value;
            else if (key == "realm") cfg.turn.realm = value;
        } else if (section == "Account") {
            if (key == "managerUri") cfg.account.managerUri = value;
            else if (key == "managerUsername") cfg.account.managerUsername = value;
            else if (key == "localInterface") cfg.account.localInterface = value;
        } else if (section == "video") {
            if (key == "use") cfg.video.use = parseBool(value, cfg.video.use);
        } else if (section == "audio") {
            if (key == "use") cfg.audio.use = parseBool(value, cfg.audio.use);
            else if (key == "device") cfg.audio.device = value;
            else if (key == "level") cfg.audio.level = value;
        } else if (section == "mic") {
            if (key == "use") cfg.mic.use = parseBool(value, cfg.mic.use);
            else if (key == "device") cfg.mic.device = value;
            else if (key == "level") cfg.mic.level = value;
        }
    }

    struct LabeledField { const char* label; std::string* val; };
    for (const auto& f : {LabeledField{"audio.device", &cfg.audio.device},
                          LabeledField{"audio.level", &cfg.audio.level},
                          LabeledField{"mic.device", &cfg.mic.device},
                          LabeledField{"mic.level", &cfg.mic.level}}) {
        if (*f.val != "as-is" && debugCb) {
            debugCb(std::string("config.yaml: ") + f.label + " = '" + *f.val +
                     "' is parsed but not applied yet.");
        }
    }

    return cfg;
}
