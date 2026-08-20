#include "../include/utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include <arpa/inet.h>      // htonl/ntohl
#include <fcntl.h>
#include <sys/socket.h>     // UNIX domain socket
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// ============================================================================
// Socket I/O Helpers
// ============================================================================
bool writeAll(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, p + off, len - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool readAll(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::read(fd, p + off, len - off);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

std::vector<uint8_t> decodeBase64(const std::string& input) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::array<int, 256> table{};
    table.fill(-1);
    for (size_t i = 0; i < chars.size(); ++i)
        table[static_cast<unsigned char>(chars[i])] = static_cast<int>(i);
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        if (std::isspace(c)) continue;
        int d = table[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

// ------------------------------------------------------------------
// Base64 encode (for vCard filename hashing)
// ------------------------------------------------------------------
static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& in) {
    std::string out;
    int i = 0, j = 0;
    unsigned char arr3[3], arr4[4];
    size_t len = in.size();
    for (size_t n = 0; n < len; ++n) {
        arr3[i++] = in[n];
        if (i == 3) {
            arr4[0] = (arr3[0] & 0xfc) >> 2;
            arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
            arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
            arr4[3] = arr3[2] & 0x3f;
            for (j = 0; j < 4; ++j)
                out += kBase64Chars[arr4[j]];
            i = 0;
        }
    }
    if (i) {
        for (j = i; j < 3; ++j) arr3[j] = '\0';
        arr4[0] = (arr3[0] & 0xfc) >> 2;
        arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
        arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
        arr4[3] = arr3[2] & 0x3f;
        for (j = 0; j < (i + 1); ++j)
            out += kBase64Chars[arr4[j]];
        while (i++ < 3)
            out += '=';
    }
    return out;
}

bool writeAvatarBase64ToTemp(const std::string& encoded, std::string& outPath) {
    if (encoded.empty()) return false;
    std::string data = encoded;
    if (data.rfind("data:", 0) == 0) {
        auto comma = data.find(',');
        if (comma == std::string::npos) return false;
        data.erase(0, comma + 1);
    }
    auto bytes = decodeBase64(data);
    if (bytes.empty()) return false;
    char tmpl[] = "/tmp/jamcli-avatar-XXXXXX";
    int fd = ::mkstemp(tmpl);
    if (fd < 0) return false;
    bool ok = writeAll(fd, bytes.data(), bytes.size());
    ::close(fd);
    if (!ok) {
        ::unlink(tmpl);
        return false;
    }
    outPath = tmpl;
    return true;
}

bool writeWireMsg(int fd, WireMsgType type, const void* payload, uint32_t len) {
    uint32_t hdr[2] = {htonl(static_cast<uint32_t>(type)), htonl(len)};
    if (!writeAll(fd, hdr, sizeof(hdr))) return false;
    if (len > 0 && !writeAll(fd, payload, len)) return false;
    return true;
}

bool writeWireControl(int fd, const std::string& line) {
    return writeWireMsg(fd, WireMsgType::CONTROL, line.data(), static_cast<uint32_t>(line.size()));
}

int bindUnixSocket(const std::string& path) {
    ::unlink(path.c_str());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// ============================================================================
// Misc helpers: shell escaping, external tool detection
// ============================================================================
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

bool commandExists(const std::string& name) {
    std::string cmd = "command -v " + shellQuote(name) + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string redirectStderrToDebugLogFile() {
    std::string path = "/tmp/jamcli-debug-" + std::to_string(::getpid()) + ".log";
    if (!std::freopen(path.c_str(), "w", stderr)) return "";
    return path;
}

std::string toLowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string fileExtension(const std::string& path) {
    auto dot = path.find_last_of('.');
    auto slash = path.find_last_of('/');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    return toLowerCopy(path.substr(dot + 1));
}

bool isImageExt(const std::string& ext) {
    static const std::set<std::string> exts = {
        "png", "jpg", "jpeg", "gif", "bmp", "webp", "tiff", "tif", "avif", "heic"
    };
    return exts.count(ext) != 0;
}

bool isVideoExt(const std::string& ext) {
    static const std::set<std::string> exts = {
        "mp4", "mov", "mkv", "webm", "avi", "m4v", "3gp", "ogv"
    };
    return exts.count(ext) != 0;
}

bool isAudioExt(const std::string& ext) {
    static const std::set<std::string> exts = {
        "ogg", "oga", "opus", "mp3", "wav", "flac", "aac", "m4a", "wma"
    };
    return exts.count(ext) != 0;
}

std::string truncateTitle(const std::string& name, size_t maxLen) {
    if (name.size() <= maxLen) return name;
    if (maxLen <= 1) return name.substr(0, maxLen);
    // Avoid cutting a UTF-8 sequence in half.
    size_t cut = maxLen - 1;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
    return name.substr(0, cut) + "\u2026"; // "…"
}

// ------------------------------------------------------------------
// Probe image dimensions using ffprobe.
// ------------------------------------------------------------------
static bool getImageDimensions(const std::string& path, int& w, int& h) {
    if (!commandExists("ffprobe")) return false;
    std::string cmd = "ffprobe -v error -select_streams v:0 "
                      "-show_entries stream=width,height "
                      "-of csv=s=x:p=0 " + shellQuote(path) + " 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buf[64] = {0};
    if (::fgets(buf, sizeof(buf), pipe) == nullptr) {
        ::pclose(pipe);
        return false;
    }
    ::pclose(pipe);
    std::string s = buf;
    if (!s.empty() && s.back() == '\n') s.pop_back();
    auto x = s.find('x');
    if (x == std::string::npos) return false;
    try {
        w = std::stoi(s.substr(0, x));
        h = std::stoi(s.substr(x + 1));
        return w > 0 && h > 0;
    } catch (...) {
        return false;
    }
}

// ------------------------------------------------------------------
// Compute timg -g argument so the longer side is ~maxPixels.
// Assumes a typical terminal cell is ~7px wide, ~14px tall.
// ------------------------------------------------------------------
static std::string computeTimgGrid(const std::string& path, int maxPixels) {
    int w = 0, h = 0;
    if (!getImageDimensions(path, w, h)) return "15x7";
    const double CELL_W = 7.0;
    const double CELL_H = 14.0;
    int cols = 0, rows = 0;
    if (w >= h) {
        cols = std::max(1, static_cast<int>(maxPixels / CELL_W));
        rows = std::max(1, static_cast<int>(cols * (h * CELL_W) / (w * CELL_H)));
    } else {
        rows = std::max(1, static_cast<int>(maxPixels / CELL_H));
        cols = std::max(1, static_cast<int>(rows * (w * CELL_H) / (h * CELL_W)));
    }
    return std::to_string(cols) + "x" + std::to_string(rows);
}

// ------------------------------------------------------------------
// Render inline image with timg.
// maxPixels > 0  -> limit longer side to maxPixels (default 120).
// maxPixels <= 0 -> real size (no grid limit).
// ------------------------------------------------------------------
bool renderInlineMedia(const std::string& path, const std::string& displayName, int maxPixels) {
    const std::string ext = fileExtension(displayName.empty() ? path : displayName);
    // Inline terminal rendering is intentionally limited to still images.
    // Audio/video files are handled by ffplay asynchronously.
    if (!isImageExt(ext)) return false;
    if (!commandExists("timg")) return false;

    // Do not let timg inherit jamcli's raw-mode stdin.
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int nullfd = ::open("/dev/null", O_RDWR);
        int ttyfd = ::open("/dev/tty", O_WRONLY);
        if (nullfd >= 0) {
            ::dup2(nullfd, STDIN_FILENO);
            ::close(nullfd);
        }
        if (ttyfd >= 0) {
            ::dup2(ttyfd, STDOUT_FILENO);
            ::close(ttyfd);
        }
        int errfd = ::open("/dev/null", O_WRONLY);
        if (errfd >= 0) {
            ::dup2(errfd, STDERR_FILENO);
            ::close(errfd);
        }
        if (maxPixels <= 0) {
            ::execlp("timg", "timg", path.c_str(), static_cast<char*>(nullptr));
        } else {
            std::string grid = computeTimgGrid(path, maxPixels);
            std::string gridArg = "-g" + grid;
            ::execlp("timg", "timg", gridArg.c_str(), path.c_str(), static_cast<char*>(nullptr));
        }
        // ::execlp("timg", "timg", "-g15x7", path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// ============================================================================
// vCard line parser (shared)
// ============================================================================
void parseVCardLine(const std::string& raw, std::map<std::string, std::string>& out) {
    auto colon = raw.find(':');
    if (colon == std::string::npos) return;
    std::string namePart = raw.substr(0, colon);
    std::string value = raw.substr(colon + 1);

    // Strip property parameters (e.g. FN;CHARSET=UTF-8  ->  FN)
    auto semi = namePart.find(';');
    std::string prop = (semi == std::string::npos) ? namePart
                                                   : namePart.substr(0, semi);

    if (prop == "FN") {
        out["displayName"] = value;
    } else if (prop == "PHOTO") {
        // vCard 3.0: PHOTO;ENCODING=b;TYPE=JPEG:base64data
        out["avatar"] = value;
    } else if (prop == "N") {
        // Fallback display name from structured name if FN absent
        if (out.find("displayName") == out.end()) {
            std::vector<std::string> parts;
            std::istringstream iss(value);
            std::string p;
            while (std::getline(iss, p, ';'))
                parts.push_back(p);
            if (parts.size() >= 2) {
                out["displayName"] =
                    (parts[1].empty() ? "" : parts[1] + " ") + parts[0];
            }
        }
    }
}

// ============================================================================
// Parse a vCard file from an arbitrary path (not the profiles/ subdir).
// ============================================================================
std::map<std::string, std::string> parseVCardFile(const std::string& path) {
    std::map<std::string, std::string> result;
    std::ifstream file(path);
    if (!file.is_open()) return result;
    std::string line, unfolded;
    while (std::getline(file, line)) {
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (!unfolded.empty()) unfolded += line.substr(1);
            continue;
        }
        if (!unfolded.empty()) {
            parseVCardLine(unfolded, result);
            unfolded.clear();
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "END:VCARD") break;
        unfolded = line;
    }
    if (!unfolded.empty()) parseVCardLine(unfolded, result);
    return result;
}

// ============================================================================
// vCard profile reader from daemon cache (~/.local/share/jami/.../profiles/)
// ============================================================================
std::map<std::string, std::string>
getContactProfileFromVCard(const std::string& accountId,
                           const std::string& uri) {
    std::map<std::string, std::string> result;
    const char* home = std::getenv("HOME");
    if (!home) return result;
    std::string path = std::string(home) + "/.local/share/jami/" + accountId +
                       "/profiles/" + base64Encode(uri) + ".vcf";
    std::ifstream file(path);
    if (!file.is_open()) {
        // Retry with bare hash (no jami: scheme)
        std::string bare = uri;
        if (bare.rfind("jami:", 0) == 0) bare.erase(0, 5);
        path = std::string(home) + "/.local/share/jami/" + accountId +
               "/profiles/" + base64Encode(bare) + ".vcf";
        file.open(path);
    }
    if (!file.is_open()) return result;
    std::string line, unfolded;
    while (std::getline(file, line)) {
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            // continuation of previous line
            if (!unfolded.empty())
                unfolded += line.substr(1);
            continue;
        }
        if (!unfolded.empty()) {
            parseVCardLine(unfolded, result);
            unfolded.clear();
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == "END:VCARD")
            break;
        unfolded = line;
    }
    if (!unfolded.empty()) parseVCardLine(unfolded, result);
    return result;
}

// ============================================================================
// Simple line-based YAML parser for flat key:value files.
// ============================================================================
std::map<std::string, std::string> parseSimpleYaml(const std::string& path) {
    std::map<std::string, std::string> result;
    std::ifstream file(path);
    if (!file.is_open()) return result;
    std::string line;
    while (std::getline(file, line)) {
        auto start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        auto end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty() || line[0] == '#') continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        auto keyEnd = key.find_last_not_of(" \t");
        if (keyEnd != std::string::npos) key = key.substr(0, keyEnd + 1);
        auto valStart = value.find_first_not_of(" \t");
        if (valStart != std::string::npos) value = value.substr(valStart);
        else value = "";
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        result[key] = value;
    }
    return result;
}

bool renderContactAvatar(const std::map<std::string, std::string>& details) {
    std::string encoded;
    for (const char* key : {"PHOTO", "AVATAR", "photo", "avatar",
                            "Account.photo", "Account.avatar",
                            "Account.profilePicture", "profilePicture"}) {
        auto it = details.find(key);
        if (it != details.end() && !it->second.empty()) {
            encoded = it->second;
            break;
        }
    }
    if (encoded.empty() || !commandExists("timg")) return false;
    std::string path;
    if (!writeAvatarBase64ToTemp(encoded, path)) return false;
    bool ok = renderInlineMedia(path, "avatar.jpg", 120);
    ::unlink(path.c_str());
    return ok;
}

bool playMediaAsync(const std::string& path, const std::string& displayName,
                    const std::string& rendererMode) {
    const std::string ext = fileExtension(displayName.empty() ? path : displayName);
    const bool isVideo = isVideoExt(ext);
    const bool isAudio = isAudioExt(ext);
    if (!isVideo && !isAudio) return false;
    const bool useMpv = rendererMode == "mpv" && commandExists("mpv");
    if (!useMpv && !commandExists("ffplay")) return false;
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int nullfd = ::open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            ::dup2(nullfd, STDIN_FILENO);
            ::close(nullfd);
        }
        const std::string title = displayName.empty() ? path : displayName;
        if (useMpv) {
            const std::string titleArg = "--title=" + title;
            if (isAudio) {
                ::execlp("mpv", "mpv", "--really-quiet", "--no-video", titleArg.c_str(),
                         path.c_str(), static_cast<char*>(nullptr));
            } else {
                ::execlp("mpv", "mpv", "--really-quiet", titleArg.c_str(),
                         path.c_str(), static_cast<char*>(nullptr));
            }
        } else if (isAudio) {
            ::execlp("ffplay", "ffplay", "-hide_banner", "-loglevel", "error",
                     "-autoexit", "-nodisp", path.c_str(),
                     static_cast<char*>(nullptr));
        } else {
            ::execlp("ffplay", "ffplay", "-hide_banner", "-loglevel", "error",
                     "-autoexit", "-window_title", title.c_str(),
                     path.c_str(), static_cast<char*>(nullptr));
        }
        ::_exit(127);
    }
    std::thread([pid] { ::waitpid(pid, nullptr, 0); }).detach();
    return true;
}

void printFileTitle(std::ostream& os, const std::string& path, const std::string& displayName) {
    std::string title = truncateTitle(displayName, 120);
    renderInlineMedia(path, displayName, 120);
    os << "[file] " << title << std::endl;
}
