#pragma once

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

std::map<std::string, std::string> parseSimpleYaml(const std::string& path);

// ============================================================================
// vCard Helpers
// ============================================================================
std::string base64Encode(const std::string& in);
void parseVCardLine(const std::string& raw, std::map<std::string, std::string>& out);
std::map<std::string, std::string> parseVCardFile(const std::string& path);
std::map<std::string, std::string>
getContactProfileFromVCard(const std::string& accountId,
                           const std::string& uri);

// ============================================================================
// Socket I/O Helpers
// ============================================================================
bool writeAll(int fd, const void* buf, size_t len);
bool readAll(int fd, void* buf, size_t len);

// Small base64 decoder used for Jami profile avatars returned by getContactDetails().
std::vector<uint8_t> decodeBase64(const std::string& input);

bool writeAvatarBase64ToTemp(const std::string& encoded, std::string& outPath);

bool readFileAsBase64(const std::string& path, std::string& outBase64);

enum class WireMsgType : uint32_t {
    FRAME   = 1,
    CONTROL = 2,
};

bool writeWireMsg(int fd, WireMsgType type, const void* payload, uint32_t len);
bool writeWireControl(int fd, const std::string& line);
int bindUnixSocket(const std::string& path);

// ============================================================================
// Misc helpers: shell escaping, external tool detection
// ============================================================================
std::string shellQuote(const std::string& s);

// Returns true if `name` resolves to something runnable via PATH.
bool commandExists(const std::string& name);

// libjami's own internal logger (enabled by --debug-log) always writes raw,
// unsynchronized bytes straight to stderr from its own background thread -
// there is no public API to redirect it. Those writes race with jamcli's
// own ANSI cursor control on stdout (see TerminalUI::redrawPromptUnlocked())
// and bury the prompt under log spam, which is why the CLI used to appear to
// vanish entirely once --debug-log was passed. Moving stderr to a plain file
// before libjami::init() keeps the two apart; the daemon just inherits the
// redirected fd. Returns the log file path, or "" if the redirect failed.
std::string redirectStderrToDebugLogFile();

std::string toLowerCopy(std::string s);
std::string fileExtension(const std::string& path);
bool isImageExt(const std::string& ext);
bool isVideoExt(const std::string& ext);
bool isAudioExt(const std::string& ext);

// Truncates a filename to at most `maxLen` characters (UTF-8 byte-safe truncation
// with an ellipsis marker), used as the "title" shown for a sent/received file.
std::string truncateTitle(const std::string& name, size_t maxLen = 120);

// Renders an inline preview of an image/video using `timg` if it is available,
// keeping the aspect ratio and fitting the longer side within ~250px. Terminal
// output is character-cell based rather than pixel based, so we approximate a
// 250x250px bounding box as a 42x21 character-cell box (~6x12px per cell) and
// let timg do the aspect-preserving fit (it never crops, only letterboxes).
// Returns true if a preview was actually drawn.
// bool renderInlineMedia(const std::string& path, const std::string& displayName);
bool renderInlineMedia(const std::string& path, const std::string& displayName,
                       int maxPixels = 120);

bool renderContactAvatar(const std::map<std::string, std::string>& details);

// Plays received audio/video without blocking the libjami callback or the
// main terminal input loop. `rendererMode` mirrors the app's --renderer
// setting: when it's "mpv", both received videos and audio are played with
// mpv instead of ffplay (audio uses --no-video since there is nothing to
// render).
bool playMediaAsync(const std::string& path, const std::string& displayName,
                     const std::string& rendererMode);

// Prints a file message: inline preview via timg when possible/safe, otherwise
// just the (length-limited) filename as the title.
void printFileTitle(std::ostream& os, const std::string& path, const std::string& displayName);
