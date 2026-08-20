#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// ============================================================================
// Enums & Data Structures
// ============================================================================
enum class UiMode {
    SETTING,    // Prompt: #
    MESSAGE,    // Prompt: >
    COMPANION   // Prompt: > <@foo>
};

enum class CallType {
    NONE,
    AUDIO,
    VIDEO
};

struct IncomingCallState {
    std::string callId;
    std::string peerUri;
    CallType type {CallType::NONE};
    bool active {false};
};

struct FileTransferState {
    std::string conversationId;
    std::string interactionId;
    std::string fileId;
    std::string peerUri;
    std::string displayName;
    int64_t totalSize {0};
    bool active {false};
};

// ============================================================================
// Config file struct
// ============================================================================
struct JamcliConfig {
    struct {
        std::string interface {"as-is"};
        bool upnp {true};
        bool dhtBootstrap {true};
        std::string dhtBootstrapNode {"bootstrap.jami.net:4222"};
        bool turnServer {true};
        std::string turnServerUrl {"turn.jami.net:3478"};
        std::string turnServerUsername {"ring"};
        std::string turnServerPassword {"ring"};
        std::string turnServerRealm {"ring"};
        bool dhtProxy {false};
        bool localPeerDiscovery {true};
        bool jams {false};
        std::string jamsServer {"ns.jami.net"};
        std::string jamsUser;
    } network;

    // Canonical libjami account configuration.
    struct {
        std::string managerUri;
        std::string managerUsername;
        std::string localInterface {"as-is"};
    } account;

    struct {
        bool enable {true};
        std::string server {"turn.jami.net:3478"};
        std::string username {"ring"};
        std::string password {"ring"};
        std::string realm {"ring"};
    } turn;
    struct {
        bool use {true};
    } video;
    struct {
        bool use {true};
        std::string device {"as-is"};
        std::string level {"as-is"};
    } audio;
    struct {
        bool use {true};
        std::string device {"as-is"};
        std::string level {"as-is"};
    } mic;
};

// ----------------------------------------------------------------------------
// Message id ring: hex 01-FF, wrapping back to 01 (which then invalidates the
// previous message that used id "01", since the log map key gets overwritten).
// ----------------------------------------------------------------------------
class MessageIdRing {
public:
    std::string next() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (counter_ == 0 || counter_ > 0xFF) counter_ = 1;
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", counter_);
        ++counter_;
        return std::string(buf);
    }
private:
    std::mutex mutex_;
    unsigned counter_ {1};
};

struct MessageRecord {
    std::string id;                 // "01".."FF"
    bool outgoing {false};
    std::string peer;
    std::string text;               // plain text body (if any)
    bool isFile {false};
    std::string filePath;           // local path (sender) or download destination (receiver)
    std::string fileName;           // original/display filename (stable across transfer)
    std::string fileId;             // libjami transfer id (files only)
    std::string conversationId;
    std::string interactionId;      // real swarm interaction id, when known (used for reply-to)
    bool downloaded {false};        // true once a received file has finished downloading
    std::string replyToId;          // our own short id ("01".."FF") this message replies to
    bool deleted {false};
};
