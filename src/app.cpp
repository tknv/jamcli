#include "../include/app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jami/callmanager_interface.h>
#include <jami/configurationmanager_interface.h>
#include <jami/conversation_interface.h> // Swarm conversation API
#include <jami/datatransfer_interface.h> // Swarm file transfer
#include <jami/jami.h>
#include <jami/media_const.h> // libjami::Media::MediaAttributeKey / MediaAttributeValue
#include <jami/presencemanager_interface.h>
#include <jami/videomanager_interface.h> // libjami::VideoSignal::DecodingStarted / DecodingStopped, SinkTarget

#include <net/if.h>
#include <poll.h>

// ============================================================================
// Display name cache
//
// TerminalUI resolves @hash -> displayname for things it renders itself
// (the prompt, printIncomingMessage/printIncomingTagged/printOutgoingTagged).
// A lot of call sites in this file build a plain string ("Calling @" + hash)
// before handing it to ui_.printSystemMessage(), so TerminalUI never gets a
// chance to resolve it. This mirrors the same hash->name mapping here so
// those call sites can resolve it themselves before formatting the message.
//
// The vCard cache reading itself (base64Encode / getContactProfileFromVCard)
// lives in utils.cpp - this file just consumes it.
// ============================================================================
namespace {
std::mutex g_display_name_mutex;
std::unordered_map<std::string, std::string> g_display_names;
// UTF-8 helper
bool isUtf8ContinuationByte(char c) { return (c & 0xC0) == 0x80; }

size_t prevUtf8CharPos(const std::string &str, size_t pos) {
  if (pos == 0)
    return 0;
  size_t p = pos - 1;
  while (p > 0 && isUtf8ContinuationByte(str[p])) {
    p--;
  }
  return p;
}

size_t nextUtf8CharPos(const std::string &str, size_t pos) {
  if (pos >= str.size())
    return str.size();
  size_t p = pos + 1;
  while (p < str.size() && isUtf8ContinuationByte(str[p])) {
    p++;
  }
  return p;
}

void rememberDisplayName(const std::string &hash, const std::string &name) {
  if (hash.empty() || name.empty())
    return;
  std::lock_guard<std::mutex> lock(g_display_name_mutex);
  g_display_names[hash] = name;
}

std::string displayNameOrHash(const std::string &hash) {
  std::lock_guard<std::mutex> lock(g_display_name_mutex);
  auto it = g_display_names.find(hash);
  return it != g_display_names.end() ? it->second : hash;
}

std::string localTimeStamp() {
  const std::time_t now = std::time(nullptr);

  std::tm tm{};
  if (localtime_r(&now, &tm) == nullptr) {
    return {};
  }

  std::ostringstream os;
  os << std::put_time(&tm, "%a %b %e %H:%M:%S %Z %Y");
  return os.str();
}

std::string withLocalTimeStamp(const std::string &text) {
  return text + " - " + localTimeStamp();
}
} // namespace

// ============================================================================
// LibjamiWorker
// ============================================================================
LibjamiWorker::LibjamiWorker() : thread_(&LibjamiWorker::run, this) {}

LibjamiWorker::~LibjamiWorker() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
  }
  cv_.notify_all();
  if (thread_.joinable())
    thread_.join();
}

void LibjamiWorker::post(std::function<void()> task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(task));
  }
  cv_.notify_one();
}

void LibjamiWorker::run() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
      if (!running_ && queue_.empty())
        return;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    if (task)
      task();
  }
}

// ============================================================================
// JamcliApp
// ============================================================================
JamcliApp::JamcliApp() { ui_.setMode(UiMode::SETTING); }

JamcliApp::~JamcliApp() {
  running_ = false;
  if (composing_watcher_thread_.joinable())
    composing_watcher_thread_.join();
  raw_guard_.disable();

  std::promise<void> done;
  auto fut = done.get_future();
  libjami_worker_.post([this, &done] {
    local_video_.stopBlocking();
    if (!local_video_input_id_.empty()) {
      libjami::closeVideoInput(local_video_input_id_);
      local_video_input_id_.clear();
    }
    remote_video_.stopBlocking();
    done.set_value();
  });
  fut.wait();
  renderer_launcher_.stop();
  libjami::fini();
}

bool JamcliApp::init(const std::string &rendererMode, bool debugLog) {
  debug_log_ = debugLog;
  config_ = loadJamcliConfig([this](const std::string &msg) {
    ui_.printSystemMessage("[config] " + msg);
  });

  ::signal(SIGPIPE, SIG_IGN);
  ::signal(SIGTTIN, SIG_IGN);
  ::signal(SIGTTOU, SIG_IGN);

  remote_video_.setDebugCallback([this](const std::string &msg) {
    ui_.printSystemMessage("[peer video] " + msg);
  });
  local_video_.setDebugCallback([this](const std::string &msg) {
    ui_.printSystemMessage("[local video] " + msg);
  });
  renderer_launcher_.setDebugCallback([this](const std::string &msg) {
    ui_.printSystemMessage("[view] " + msg);
  });
  renderer_launcher_.setRenderer(rendererMode);

  if (debugLog) {
    std::string debugLogPath = redirectStderrToDebugLogFile();
    if (!debugLogPath.empty()) {
      ui_.printSystemMessage("[debug-log] libjami console log -> " +
                             debugLogPath);
    } else {
      ui_.printSystemMessage("[debug-log] could not redirect stderr to a file; "
                             "libjami's console log may overwrite this UI.");
    }
  }

  if (!libjami::init(debugLog ? libjami::InitFlag::LIBJAMI_FLAG_CONSOLE_LOG
                              : static_cast<libjami::InitFlag>(0))) {
    ui_.printSystemMessage("Failed to initialize libjami engine.");
    return false;
  }

  registerJamiSignalHandlers();

  if (!libjami::start()) {
    ui_.printSystemMessage("Failed to start libjami engine.");
    return false;
  }

  libjami::setDecodingAccelerated(false);

  auto account_list = libjami::getAccountList();
  if (account_list.empty()) {
    setupInitialAccount();
  } else {
    current_account_ = account_list.front();
    ui_.printSystemMessage("Loaded active account: " + current_account_);
    login();
  }

  return true;
}

bool JamcliApp::consumeAltESCEnterSequence(char first) {
  last_escape_was_shift_enter_ = false;
  last_escape_was_arrow_left_ = false;
  last_escape_was_arrow_right_ = false;
  if (first != '\x1b')
    return false;

  // Common terminal encodings for Shift+Enter:
  //   Kitty keyboard protocol: CSI 13;2u
  //   xterm modifyOtherKeys:   CSI 27;2;13~
  // Some terminals also use CSI 13;2~.
  //
  // Left/Right arrow keys show up as CSI cursor sequences ("\x1b[D" /
  // "\x1b[C"), or as SS3 sequences ("\x1bOD" / "\x1bOC") from terminals in
  // application-cursor-keys mode.
  std::string seq(1, first);
  char c = 0;
  for (int i = 0; i < 10; ++i) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    if (::poll(&pfd, 1, 40) <= 0)
      break;
    if (::read(STDIN_FILENO, &c, 1) != 1)
      break;
    seq.push_back(c);

    // Alt+Enter (Esc + Enter) を検知した場合は即座に抜ける
    if (seq.size() == 2 && (c == '\r' || c == '\n'))
      break;
    // seq[1] is the CSI ('[') or SS3 ('O') introducer - it happens to fall
    // inside the 0x40-0x7E "final byte" range too, but it isn't a
    // terminator, so it's excluded here. Without this, sequences like
    // "\x1b[D" or "\x1b[13;2u" were being cut short right after the '[',
    // and the remaining bytes ("D", "13;2u", ...) leaked into the input as
    // literal typed characters.
    if (seq.size() > 2 && c >= 0x40 && c <= 0x7E)
      break;
  }
  if (seq == "\x1b[13;2u" || seq == "\x1b[13;2~" || seq == "\x1b[27;2;13~" ||
      seq == "\x1b\r" || seq == "\x1b\n") {
    last_escape_was_shift_enter_ = true;
  } else if (seq == "\x1b[D" || seq == "\x1bOD") {
    last_escape_was_arrow_left_ = true;
  } else if (seq == "\x1b[C" || seq == "\x1bOC") {
    last_escape_was_arrow_right_ = true;
  }
  return true;
}

void JamcliApp::run() {
  startComposingWatcher();

  std::string line;
  while (running_) {
    ui_.redrawPrompt();

    if (!::isatty(STDIN_FILENO) || !raw_guard_.enable()) {
      // Not a real terminal (e.g. piped input/tests) - fall back to plain
      // line reading; typing indicator / push-to-talk key are unavailable.
      if (!std::getline(std::cin, line))
        break;
      processInputLine(line);
      continue;
    }

    line.clear();
    bool eof = false;
    bool havePendingByte = false;
    char pendingByte = 0;

    auto readByte = [&](char &out) -> ssize_t {
      if (havePendingByte) {
        out = pendingByte;
        havePendingByte = false;
        return 1;
      }
      return ::read(STDIN_FILENO, &out, 1);
    };

    while (true) {
      char c;
      ssize_t n = readByte(c);
      if (n < 0) {
        if (errno == EINTR) {
          // Interrupted by a signal (e.g. from a child process we
          // spawned to preview a received image/video) - not EOF,
          // just retry the read instead of quitting the app.
          continue;
        }
        eof = true; // a real read error
        break;
      }
      if (n == 0) {
        eof = true;
        break;
      } // real EOF (stdin closed)

      if (c == '\x1b') {
        if (consumeAltESCEnterSequence(c)) {
          if (last_escape_was_shift_enter_) {
            size_t pos = ui_.cursorPos();
            line.insert(pos, 1, '\n');
            ui_.appendInputChar('\n');
            last_local_keystroke_ = std::chrono::steady_clock::now();
            setLocalComposing(true);
          } else if (last_escape_was_arrow_left_) {
            size_t pos = ui_.cursorPos();
            if (pos > 0) {
              size_t prev = prevUtf8CharPos(line, pos);
              for (size_t i = 0; i < (pos - prev); ++i)
                ui_.moveCursorLeft();
            }
          } else if (last_escape_was_arrow_right_) {
            size_t pos = ui_.cursorPos();
            if (pos < line.size()) {
              size_t next = nextUtf8CharPos(line, pos);
              for (size_t i = 0; i < (next - pos); ++i)
                ui_.moveCursorRight();
            }
          }
        }
        continue;
      }

      if (c == '\r' || c == '\n') {
        if (line.empty())
          continue; // ignore Enter on an empty line
        std::cout << std::endl;
        break;
      }
      if (c == 0x04 && line.empty()) { // Ctrl-D on empty line
        eof = true;
        break;
      }
      if (c == 0x7f || c == 0x08) { // backspace/delete
        size_t pos = ui_.cursorPos();
        if (pos > 0) {
          size_t prev = prevUtf8CharPos(line, pos);
          size_t bytes_to_remove = pos - prev;
          line.erase(prev, bytes_to_remove);
          for (size_t i = 0; i < bytes_to_remove; ++i) {
            ui_.backspaceInput();
          }
          last_local_keystroke_ = std::chrono::steady_clock::now();
          setLocalComposing(true);
        }
        continue;
      }
      if (std::iscntrl(static_cast<unsigned char>(c))) {
        continue; // ignore other control characters
      }

      // Printable character. If more bytes are already sitting on stdin
      // (typical of a terminal paste), drain the run of printable
      // characters locally and redraw the input line ONCE for the whole
      // run, instead of once per character. Previously every pasted
      // character triggered its own synchronous clear+redraw of the line,
      // which for a long paste flooded the terminal with hundreds of
      // (partial) copies of the growing prompt/line.
      std::string burst(1, c);
      pollfd pfd{STDIN_FILENO, POLLIN, 0};
      while (::poll(&pfd, 1, 0) > 0) {
        char next;
        ssize_t n2 = ::read(STDIN_FILENO, &next, 1);
        if (n2 != 1)
          break;
        if (next == '\r' || next == '\n' || next == '\x1b' || next == 0x7f ||
            next == 0x08 || std::iscntrl(static_cast<unsigned char>(next))) {
          // Not part of the printable run - stash it so the next loop
          // iteration handles it through the normal single-byte path.
          pendingByte = next;
          havePendingByte = true;
          break;
        }
        burst.push_back(next);
      }

      {
        size_t pos = ui_.cursorPos();
        line.insert(pos, burst);
      }
      ui_.appendInputText(burst);
      last_local_keystroke_ = std::chrono::steady_clock::now();
      setLocalComposing(true);
    }

    raw_guard_.disable();
    if (eof)
      break;

    ui_.clearInput();
    setLocalComposing(false);
    processInputLine(line);
  }
}

// Background thread that clears our own "composing" state after a short
// idle period (the daemon has no per-keystroke timeout of its own).
void JamcliApp::startComposingWatcher() {
  composing_watcher_thread_ = std::thread([this] {
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (local_is_composing_ &&
          std::chrono::steady_clock::now() - last_local_keystroke_.load() >
              std::chrono::seconds(3)) {
        setLocalComposing(false);
      }
    }
  });
}

void JamcliApp::registerJamiSignalHandlers() {
  std::map<std::string, std::shared_ptr<libjami::CallbackWrapperBase>> handlers;

  // 1. Incoming Message Handler
  using IncomingMsg = libjami::ConfigurationSignal::IncomingAccountMessage;
  auto cb1 = libjami::exportable_callback<IncomingMsg>(
      std::function<typename IncomingMsg::cb_type>(
          [this](const std::string &accountId,
                 const std::string &conversationId,
                 const std::string &messageId,
                 const std::map<std::string, std::string> &payloads) {
            auto it = payloads.find("text/plain");
            if (it != payloads.end()) {
              handleIncomingTextPayload(it->second, companion_, conversationId,
                                        messageId);
            }
          }));
  handlers[cb1.first] = cb1.second;

  // 2. Incoming Call Handler
  using IncomingCall = libjami::CallSignal::IncomingCall;
  auto cb2 = libjami::exportable_callback<
      IncomingCall>(std::function<typename IncomingCall::cb_type>(
      [this](const std::string &accountId, const std::string &callId,
             const std::string &peerUri,
             const std::vector<std::map<std::string, std::string>> &mediaList) {
        std::lock_guard<std::mutex> lock(call_mutex_);
        pending_call_.callId = callId;
        pending_call_.peerUri = peerUri;
        pending_call_.active = true;

        bool hasVideo = false;
        for (const auto &media : mediaList) {
          auto it = media.find(libjami::Media::MediaAttributeKey::MEDIA_TYPE);
          if (it != media.end() &&
              it->second == libjami::Media::MediaAttributeValue::VIDEO) {
            hasVideo = true;
            break;
          }
        }
        pending_call_.type = hasVideo ? CallType::VIDEO : CallType::AUDIO;

        ui_.printSystemMessage("Incoming call from @" +
                               displayNameOrHash(peerUri) +
                               (hasVideo ? " (video)" : " (audio)"));
        ui_.startRingingAnimation(pending_call_.type, pending_call_.peerUri);
      }));
  handlers[cb2.first] = cb2.second;

  // 3. Call State Changed Handler
  using CallState = libjami::CallSignal::StateChange;
  auto cb3 = libjami::exportable_callback<CallState>(
      std::function<typename CallState::cb_type>(
          [this](const std::string &accountId, const std::string &callId,
                 const std::string &state, int code) {
            if (state == "CURRENT") {
              std::lock_guard<std::mutex> lock(call_mutex_);
              if (active_call_id_ == callId) {
                call_connected_ = true;
              }
              return;
            }
            if (state == "HANGUP" || state == "OVER") {
              bool wasThisCall = false;
              {
                std::lock_guard<std::mutex> lock(call_mutex_);
                if (pending_call_.callId == callId) {
                  pending_call_.active = false;
                  wasThisCall = true;
                }
                if (active_call_id_ == callId) {
                  active_call_id_.clear();
                  call_connected_ = false;
                  wasThisCall = true;
                }
              }
              if (wasThisCall) {
                ui_.stopRingingAnimation();
                ui_.printSystemMessage("Call ended.");
              }
              requestViewClose();
              requestVideoSinkTeardown();
              ui_.redrawPrompt();
            }
          }));
  handlers[cb3.first] = cb3.second;

  // 4. ConversationReady Handler
  using ConvReady = libjami::ConversationSignal::ConversationReady;
  auto cb4 = libjami::exportable_callback<ConvReady>(
      std::function<typename ConvReady::cb_type>(
          [this](const std::string &accountId,
                 const std::string &conversationId) {
            cacheConversationMembers(conversationId);
          }));
  handlers[cb4.first] = cb4.second;

  // 5. SwarmMessageReceived Handler
  using SwarmMsg = libjami::ConversationSignal::SwarmMessageReceived;
  auto cb5 = libjami::exportable_callback<SwarmMsg>(
      std::function<typename SwarmMsg::cb_type>(
          [this](const std::string &accountId,
                 const std::string &conversationId,
                 libjami::SwarmMessage message) {
            auto authorIt = message.body.find("author");
            std::string author = (authorIt != message.body.end())
                                     ? authorIt->second
                                     : companion_;

            // SwarmMessageReceived fires for every message written into the
            // conversation log, including our own outgoing ones (a swarm
            // conversation is a shared log all members - us included - are
            // subscribed to). Our own sends are already logged/printed
            // synchronously at the call site (logAndPrintOutgoingText/File),
            // so echoes authored by us must be dropped here. This check has
            // to happen before the companion-relabeling below, which would
            // otherwise overwrite a self-authored message's author with
            // companion_ and make it look like the peer sent it.
            //
            // NOTE: "author" here is the account's identity hash
            // (Account.username / own_username_), NOT current_account_
            // (the local API/account id) - those are two different values
            // in libjami, and comparing against the wrong one silently
            // never matches.
            if (!own_username_.empty() && author == own_username_)
              return;

            // In a 1:1 companion session, SwarmMessage may expose a URI/device
            // identity different from the peer id shown by /chat. Keep the CLI
            // display stable and avoid showing two identities for the same
            // peer.
            const std::string activeConversation =
                resolveConversationId(companion_);
            if (!companion_.empty() && !activeConversation.empty() &&
                conversationId == activeConversation) {
              author = companion_;
            }

            if (message.type == "text/plain") {
              auto it = message.body.find("body");
              if (it != message.body.end()) {
                handleIncomingTextPayload(it->second, author, conversationId,
                                          message.id);
              }
              return;
            }

            auto fileIdIt = message.body.find("fileId");
            if (fileIdIt != message.body.end() && !fileIdIt->second.empty()) {
              handleIncomingFileOffer(conversationId, message.id,
                                      fileIdIt->second, author, message.body);
            }
          }));
  handlers[cb5.first] = cb5.second;

  // 6. Video Decoding Started Handler (FIXED FOR REMOTE VIEW)
  using DecStarted = libjami::VideoSignal::DecodingStarted;
  auto cb6 = libjami::exportable_callback<DecStarted>(
      std::function<typename DecStarted::cb_type>(
          [this](const std::string &id, const std::string & /*shmPath*/, int w,
                 int h, bool isMixer) {
            ui_.printSystemMessage("[signal] DecodingStarted id='" + id + "' " +
                                   std::to_string(w) + "x" + std::to_string(h) +
                                   " isMixer=" + (isMixer ? "true" : "false"));

            if (isMixer)
              return;
            if (w <= 0 || h <= 0)
              return;

            libjami_worker_.post([this, id, w, h] {
              // Local camera preview check
              if (!local_video_input_id_.empty() &&
                  id == local_video_input_id_) {
                if (local_video_.start(id, w, h,
                                       "you (local preview) - talking to @" +
                                           local_preview_peer_label_,
                                       "local", renderer_launcher_.renderer(),
                                       renderer_launcher_.localListenFd())) {
                  ui_.printSystemMessage("Showing your own camera (" +
                                         std::to_string(w) + "x" +
                                         std::to_string(h) + ").");
                  reportRawRendererCommandIfNeeded("local", w, h);
                } else {
                  ui_.printSystemMessage(
                      "Could not show local camera preview: " +
                      local_video_.lastError());
                }
                return;
              }

              // FIX: Verification for Remote Video Stream.
              // DecodingStarted's `id` is a Video Sink ID (e.g., sink_...), NOT
              // the callId. Checking `id != expectedCallId` previously caused
              // remote streams to be dropped.
              bool hasActiveOrPendingCall = false;
              {
                std::lock_guard<std::mutex> lock(call_mutex_);
                hasActiveOrPendingCall =
                    !active_call_id_.empty() || pending_call_.active;
              }

              if (!hasActiveOrPendingCall) {
                ui_.printSystemMessage(
                    "[signal]   (ignored - no active or pending call)");
                return;
              }

              std::string peerLabel =
                  !companion_.empty() ? companion_ : pending_call_.peerUri;
              if (remote_video_.start(id, w, h, peerLabel, "remote",
                                      renderer_launcher_.renderer(),
                                      renderer_launcher_.remoteListenFd())) {
                active_remote_sink_id_ = id;
                ui_.printSystemMessage(
                    "Showing @" + displayNameOrHash(peerLabel) + "'s video (" +
                    std::to_string(w) + "x" + std::to_string(h) + ").");
                reportRawRendererCommandIfNeeded("remote", w, h);
              } else {
                ui_.printSystemMessage(
                    "Could not show @" + displayNameOrHash(peerLabel) +
                    "'s video: " + remote_video_.lastError());
              }
            });
          }));
  handlers[cb6.first] = cb6.second;

  // 7. Video Decoding Stopped Handler
  using DecStopped = libjami::VideoSignal::DecodingStopped;
  auto cb7 = libjami::exportable_callback<DecStopped>(
      std::function<typename DecStopped::cb_type>(
          [this](const std::string &id, const std::string & /*shmPath*/,
                 bool isMixer) {
            ui_.printSystemMessage(
                "[signal] DecodingStopped id='" + id +
                "' isMixer=" + (isMixer ? "true" : "false") +
                (id == active_remote_sink_id_ ? " (matches active sink)" : ""));

            if (id == active_remote_sink_id_) {
              remote_video_.requestStop();
              std::lock_guard<std::mutex> lock(call_mutex_);
              active_remote_sink_id_.clear();
            }
          }));
  handlers[cb7.first] = cb7.second;

  // 8. DataTransferEvent Handler
  using XferEvent = libjami::DataTransferSignal::DataTransferEvent;
  auto cb8 = libjami::exportable_callback<XferEvent>(
      std::function<typename XferEvent::cb_type>(
          [this](const std::string &accountId,
                 const std::string &conversationId,
                 const std::string &interactionId, const std::string &fileId,
                 int eventCode) {
            using Code = libjami::DataTransferEventCode;
            switch (static_cast<Code>(eventCode)) {
            case Code::finished:
              ui_.printSystemMessage("File transfer complete (id " + fileId +
                                     ").");
              // Terminal media work must not run inside the libjami callback.
              libjami_worker_.post(
                  [this, fileId] { markFileDownloadedAndPreview(fileId); });
              break;
            case Code::closed_by_peer:
              ui_.printSystemMessage("Peer closed the file transfer (id " +
                                     fileId + ").");
              break;
            case Code::timeout_expired:
              ui_.printSystemMessage("File transfer timed out (id " + fileId +
                                     ").");
              break;
            case Code::unsupported:
            case Code::invalid_pathname:
            case Code::unjoinable_peer:
              ui_.printSystemMessage("File transfer failed (id " + fileId +
                                     ").");
              break;
            default:
              break;
            }
            std::lock_guard<std::mutex> lock(call_mutex_);
            if (pending_file_.fileId == fileId) {
              pending_file_.active = false;
            }
          }));
  handlers[cb8.first] = cb8.second;

  // 9. ConversationRequestReceived Handler
  using ConvReqRecv = libjami::ConversationSignal::ConversationRequestReceived;
  auto cb9 = libjami::exportable_callback<ConvReqRecv>(
      std::function<typename ConvReqRecv::cb_type>(
          [this](const std::string &accountId,
                 const std::string &conversationId,
                 std::map<std::string, std::string> metadatas) {
            auto fromIt = metadatas.find("from");
            std::string from =
                (fromIt != metadatas.end()) ? fromIt->second : conversationId;
            ui_.printSystemMessage("Invitation received from @" +
                                   displayNameOrHash(from) +
                                   " (/list all to see it, /accept " + from +
                                   " or /ac " + from + " to accept).");
          }));
  handlers[cb9.first] = cb9.second;

  // 10. Presence Handler - drives /list (online) vs /list all
  using BuddyNotif = libjami::PresenceSignal::NewBuddyNotification;
  auto cb10 = libjami::exportable_callback<BuddyNotif>(
      std::function<typename BuddyNotif::cb_type>(
          [this](const std::string & /*accountId*/, const std::string &buddyUri,
                 int status, const std::string & /*lineStatus*/) {
            std::lock_guard<std::mutex> lock(presence_mutex_);
            if (status != 0)
              online_contacts_.insert(buddyUri);
            else
              online_contacts_.erase(buddyUri);
          }));
  handlers[cb10.first] = cb10.second;

  // 11. Composing status - drives the live "companion is typing" indicator.
  // Confirmed against the daemon's D-Bus XML: composingStatusChanged(accountId,
  // conversationUri, peer, status).
  using ComposingStatus = libjami::ConfigurationSignal::ComposingStatusChanged;
  auto cb11 = libjami::exportable_callback<ComposingStatus>(
      std::function<typename ComposingStatus::cb_type>(
          [this](const std::string & /*accountId*/,
                 const std::string &conversationId,
                 const std::string &contactUri, int status) {
            // The signal's contactId may be a URI (for example jami:<id>),
            // while jamcli stores the companion as the bare account id.
            // Prefer the conversation id for 1:1 filtering, then normalize
            // the peer only for display.
            const std::string activeConversation =
                resolveConversationId(companion_);
            if (companion_.empty())
              return;
            if (!activeConversation.empty() &&
                conversationId != activeConversation)
              return;

            std::string peer = contactUri;
            if (peer.rfind("jami:", 0) == 0)
              peer.erase(0, 5);
            if (peer.empty())
              peer = companion_;

            // For the selected 1:1 companion, use the configured companion id
            // consistently even if the daemon reports a URI/device variant.
            if (peer != companion_ && !activeConversation.empty())
              peer = companion_;

            if (status != 0) {
              if (!companion_is_typing_.exchange(true)) {
                ui_.startTypingAnimation(peer);
              }
            } else {
              if (companion_is_typing_.exchange(false)) {
                ui_.stopTypingAnimation();
              }
            }
          }));
  handlers[cb11.first] = cb11.second;

  // 12. Registered-name operation completion.
  using NameRegEnded = libjami::ConfigurationSignal::NameRegistrationEnded;
  auto cb12 = libjami::exportable_callback<NameRegEnded>(
      std::function<typename NameRegEnded::cb_type>(
          [this](const std::string &accountId, int status,
                 const std::string &name) {
            if (accountId != current_account_)
              return;
            static const char *const statusText[] = {
                "success", "wrong password", "invalid name", "already taken",
                "network error"};
            std::string result = (status >= 0 && status < 5)
                                     ? statusText[status]
                                     : "unknown error";
            if (status == 0)
              ui_.printSystemMessage("Registered name confirmed: " + name);
            else
              ui_.printSystemMessage("Name registration failed for '" + name +
                                     "': " + result);
          }));
  handlers[cb12.first] = cb12.second;

  libjami::registerSignalHandlers(handlers);
}

void JamcliApp::cacheConversationMembers(const std::string &conversationId) {
  if (current_account_.empty())
    return;

  auto members =
      libjami::getConversationMembers(current_account_, conversationId);
  if (members.size() != 2)
    return;

  for (const auto &member : members) {
    auto it = member.find("uri");
    if (it == member.end())
      continue;
    if (it->second == current_account_)
      continue;

    std::lock_guard<std::mutex> lock(conv_mutex_);
    uri_to_conversation_[it->second] = conversationId;
  }
}

std::string JamcliApp::resolveConversationId(const std::string &peerUri) {
  {
    std::lock_guard<std::mutex> lock(conv_mutex_);
    auto it = uri_to_conversation_.find(peerUri);
    if (it != uri_to_conversation_.end()) {
      return it->second;
    }
  }

  if (current_account_.empty())
    return "";

  for (const auto &conversationId :
       libjami::getConversations(current_account_)) {
    cacheConversationMembers(conversationId);
  }

  std::lock_guard<std::mutex> lock(conv_mutex_);
  auto it = uri_to_conversation_.find(peerUri);
  return (it != uri_to_conversation_.end()) ? it->second : "";
}

void JamcliApp::startLocalPreview(const std::string &peer) {
  stopLocalPreview();
  local_preview_peer_label_ = peer;

  local_video_input_id_ = libjami::openVideoInput("");
  if (local_video_input_id_.empty()) {
    ui_.printSystemMessage("Could not open the local camera for preview.");
    return;
  }
}

void JamcliApp::stopLocalPreview() {
  local_video_.requestStop();
  if (!local_video_input_id_.empty()) {
    std::string inputId = std::move(local_video_input_id_);
    std::thread([inputId] { libjami::closeVideoInput(inputId); }).detach();
  }
}

void JamcliApp::requestViewClose() { renderer_launcher_.requestStop(); }

void JamcliApp::requestVideoSinkTeardown() {
  stopLocalPreview();
  remote_video_.requestStop();
  std::lock_guard<std::mutex> lock(call_mutex_);
  active_remote_sink_id_.clear();
}

// ---- Message id / reply / delete plumbing ----

// Entry point for every incoming text/plain payload (both the legacy
// IncomingAccountMessage signal and the swarm SwarmMessageReceived signal).
// Recognizes our own "/del@ID" / "/delete@ID" control text, "/@ID text"
// reply markers, and the jamcli-only typing markers above, on top of the
// underlying message id/text logging.
void JamcliApp::handleIncomingTextPayload(const std::string &body,
                                          const std::string &peer,
                                          const std::string &conversationId,
                                          const std::string &interactionId) {

  std::string cmd = body;
  auto atPos = cmd.find('@');
  if ((cmd.rfind("/del@", 0) == 0 || cmd.rfind("/delete@", 0) == 0) &&
      atPos != std::string::npos) {
    std::string id = cmd.substr(atPos + 1);
    // strip trailing whitespace
    while (!id.empty() && std::isspace(static_cast<unsigned char>(id.back())))
      id.pop_back();
    handleDeleteCommand(id);
    return;
  }

  auto [replyId, text] = parseReplyMarker(body);
  std::string shownText =
      replyId.empty() ? body : ("(re: " + replyId + ") " + text);
  logAndPrintIncomingText(shownText, peer, conversationId, interactionId);
}

std::string JamcliApp::logAndPrintIncomingText(
    const std::string &text, const std::string &peer,
    const std::string &conversationId, const std::string &interactionId) {
  std::string id = id_ring_.next();
  MessageRecord rec;
  rec.id = id;
  rec.outgoing = false;
  rec.peer = peer;
  rec.text = text;
  rec.conversationId = conversationId;
  rec.interactionId = interactionId;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    message_log_[id] = rec;
  }
  ui_.printIncomingTagged(id, withLocalTimeStamp(text), peer);
  return id;
}

std::string JamcliApp::logAndPrintOutgoingText(
    const std::string &text, const std::string &peer,
    const std::string &conversationId, const std::string &replyToId) {
  std::string id = id_ring_.next();
  MessageRecord rec;
  rec.id = id;
  rec.outgoing = true;
  rec.peer = peer;
  rec.text = text;
  rec.conversationId = conversationId;
  rec.replyToId = replyToId;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    message_log_[id] = rec;
  }
  ui_.printOutgoingTagged(id, withLocalTimeStamp(text), peer);
  return id;
}

std::string JamcliApp::logAndPrintOutgoingFile(
    const std::string &path, const std::string &displayName,
    const std::string &peer, const std::string &conversationId,
    const std::string &fileId) {
  std::string id = id_ring_.next();
  MessageRecord rec;
  rec.id = id;
  rec.outgoing = true;
  rec.peer = peer;
  rec.isFile = true;
  rec.filePath = path;
  rec.fileName = displayName;
  rec.fileId = fileId;
  rec.conversationId = conversationId;
  rec.downloaded = true; // we already have the source file locally
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    message_log_[id] = rec;
  }
  ui_.printOutgoingTagged(id, "[file] " + truncateTitle(displayName, 120),
                          peer);
  if (isImageExt(fileExtension(displayName))) {
    ui_.printInlineImagePreview(path, displayName);
  }
  return id;
}

std::string JamcliApp::logAndPrintIncomingFile(
    const std::string &path, const std::string &displayName,
    const std::string &peer, const std::string &conversationId,
    const std::string &interactionId, const std::string &fileId) {
  std::string id = id_ring_.next();
  MessageRecord rec;
  rec.id = id;
  rec.outgoing = false;
  rec.peer = peer;
  rec.isFile = true;
  rec.filePath = path;
  rec.fileName = displayName;
  rec.fileId = fileId;
  rec.conversationId = conversationId;
  rec.interactionId = interactionId;
  rec.downloaded = false;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    message_log_[id] = rec;
  }
  ui_.printIncomingTagged(id, "\"" + truncateTitle(displayName, 120) + "\"",
                          peer);
  return id;
}

std::string JamcliApp::logAvatarFile(const std::string &path,
                                     const std::string &displayName,
                                     const std::string &peer) {
  std::string id = id_ring_.next();
  MessageRecord rec;
  rec.id = id;
  rec.outgoing = false;
  rec.peer = peer;
  rec.isFile = true;
  rec.filePath = path;
  rec.fileName = displayName;
  rec.conversationId = resolveConversationId(peer);
  rec.downloaded = true;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    message_log_[id] = rec;
  }
  return id;
}

// Marks a previously-logged received file as downloaded and shows its inline
// preview (or filename-only fallback) now that the bytes are on disk.
void JamcliApp::markFileDownloadedAndPreview(const std::string &fileId) {
  std::string id, path, name;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    for (auto &[key, rec] : message_log_) {
      if (rec.isFile && rec.fileId == fileId && !rec.outgoing) {
        rec.downloaded = true;
        id = rec.id;
        path = rec.filePath;
        name = rec.fileName.empty() ? path : rec.fileName;
        break;
      }
    }
  }
  if (!id.empty()) {
    const std::string ext = fileExtension(name);
    if (isImageExt(ext)) {
      ui_.printInlineImagePreview(path, name);
    } else if (isVideoExt(ext) || isAudioExt(ext)) {
      if (playMediaAsync(path, name, renderer_launcher_.renderer())) {
        ui_.printSystemMessage("Playing " + truncateTitle(name, 120) + ".");
      } else {
        ui_.printSystemMessage("Received " + truncateTitle(name, 120) +
                               ". ffplay/mpv not available.");
      }
    } else {
      printFileTitle(std::cout, path, name);
    }
  }
}

void JamcliApp::openLoggedFile(const std::string &id) {
  std::string path, name;
  bool found = false;
  bool downloaded = false;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    auto it = message_log_.find(id);
    if (it != message_log_.end() && !it->second.deleted && it->second.isFile) {
      found = true;
      downloaded = it->second.downloaded;
      path = it->second.filePath;
      name = it->second.fileName.empty() ? it->second.filePath
                                         : it->second.fileName;
    }
  }
  if (!found) {
    ui_.printSystemMessage("No such file message id: " + id);
    return;
  }
  if (!downloaded || path.empty()) {
    ui_.printSystemMessage("File " + id + " is not downloaded yet.");
    return;
  }

  const std::string ext = fileExtension(name);
  if (isImageExt(ext)) {
    // Real size: no 120px limit.  Temporarily restore canonical terminal
    // mode so timg can query the terminal size and output correctly.
    raw_guard_.withCanonicalMode([&] {
      if (!renderInlineMedia(path, name, 0)) {
        ui_.printSystemMessage("Could not display image (timg failed).");
      }
    });
    return;
  }
  if (isVideoExt(ext) || isAudioExt(ext)) {
    if (playMediaAsync(path, name, renderer_launcher_.renderer())) {
      ui_.printSystemMessage("Opening " + truncateTitle(name, 120) + ".");
    } else {
      ui_.printSystemMessage("Could not open " + truncateTitle(name, 120) +
                             ". ffplay/mpv not available.");
    }
    return;
  }
  ui_.printSystemMessage("File " + id + " is not a playable media file.");
}

// Parses a leading reply marker of the form "/@ID " or "*/@ID* " from a line,
// returning {referencedId, remainingText}. If there is no marker, referencedId
// is empty and remainingText is the original line.
std::pair<std::string, std::string>
JamcliApp::parseReplyMarker(const std::string &line) {
  std::string s = line;
  if (s.rfind("*/@", 0) == 0) {
    auto close = s.find('*', 3);
    if (close != std::string::npos) {
      std::string id = s.substr(3, close - 3);
      std::string rest = s.substr(close + 1);
      if (!rest.empty() && rest.front() == ' ')
        rest.erase(0, 1);
      return {id, rest};
    }
  }
  if (s.rfind("/@", 0) == 0) {
    std::istringstream iss(s);
    std::string token;
    iss >> token; // "/@ID"
    std::string id = token.substr(2);
    std::string rest;
    std::getline(iss, rest);
    if (!rest.empty() && rest.front() == ' ')
      rest.erase(0, 1);
    return {id, rest};
  }
  return {"", line};
}

void JamcliApp::handleReplyAndSend(const std::string &rawId,
                                   const std::string &text) {
  const std::string id = toUpperHexId(rawId);
  if (companion_.empty()) {
    ui_.printSystemMessage("Use /chat <user> first.");
    return;
  }
  std::string conversationId = resolveConversationId(companion_);
  if (conversationId.empty()) {
    ui_.printSystemMessage("No conversation with @" +
                           displayNameOrHash(companion_) + " yet.");
    return;
  }

  std::string quoted, replyToInteractionId;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    auto it = message_log_.find(id);
    if (it == message_log_.end() || it->second.deleted) {
      ui_.printSystemMessage("No such message id: " + id);
      return;
    }
    quoted = it->second.text.empty() ? ("[file] " + it->second.filePath)
                                     : it->second.text;
    replyToInteractionId =
        it->second.interactionId; // only real for received messages
  }

  // NOTE: we intentionally send only the plain text now. Embedding a
  // "/@id " marker in the wire text used to leak that literal text to the
  // peer (visible verbatim on any client that isn't this jamcli build).
  // The real swarm reply-to id is passed separately below when we have one;
  // when we don't (e.g. replying to our own earlier message), the peer
  // simply won't see a "replying to" annotation, which is the acceptable
  // trade-off.
  libjami::sendMessage(current_account_, conversationId, text,
                       replyToInteractionId, 0);
  logAndPrintOutgoingText(text + "  (re: " + id + " \"" + quoted + "\")",
                          companion_, conversationId, id);
}

void JamcliApp::handleDeleteCommand(const std::string &id) {
  MessageRecord rec;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(msg_mutex_);
    auto it = message_log_.find(id);
    if (it != message_log_.end() && !it->second.deleted) {
      found = true;
      it->second.deleted = true;
      rec = it->second;
    }
  }
  if (!found) {
    ui_.printSystemMessage("No such message id: " + id);
    return;
  }

  if (rec.isFile && !rec.downloaded) {
    // Not downloaded yet: cancel the transfer if it's the pending one, so the
    // file never lands on disk.
    std::lock_guard<std::mutex> lock(call_mutex_);
    if (pending_file_.active && pending_file_.fileId == rec.fileId) {
      libjami::cancelDataTransfer(current_account_,
                                  pending_file_.conversationId, rec.fileId);
      pending_file_.active = false;
    }
  }
  ui_.printSystemMessage(
      "Message " + id + " deleted." +
      (rec.isFile && !rec.downloaded ? " (file transfer canceled)" : ""));
}

// Send the real Jami composing state. The peer receives this through
// ConfigurationSignal::ComposingStatusChanged, not as a chat message.
void JamcliApp::setLocalComposing(bool composing) {
  if (mode_ != UiMode::COMPANION || companion_.empty())
    return;

  bool expected = !composing;
  if (!local_is_composing_.compare_exchange_strong(expected, composing)) {
    return; // already in the desired state, nothing to send
  }

  std::string conversationId = resolveConversationId(companion_);
  if (conversationId.empty()) {
    // Couldn't resolve the conversation yet (e.g. right after /chat,
    // before it's cached) - roll back so the very next keystroke
    // retries instead of leaving local_is_composing_ permanently
    // "stuck", which used to make the peer never see us typing.
    local_is_composing_.store(!composing);
    return;
  }

  std::string account = current_account_;
  fireAndForgetLibjamiCall([account, conversationId, composing] {
    libjami::setIsComposing(account, conversationId, composing);
  });
}

// Forcibly clears our composing flag and, if we were still mid-typing,
// tells the *previously* active companion we stopped composing.
//
// This must be called whenever the active companion/conversation changes
// (e.g. /chat to someone else). Without it, local_is_composing_ can stay
// stuck at true from the old conversation, so the very next keystroke in
// the new conversation calls setLocalComposing(true) -> exchange(true)
// returns true -> "no change" -> the composing signal for the new
// conversation is silently never sent, and our typing never shows up on
// the new peer's side.
void JamcliApp::resetLocalComposing() {
  if (!local_is_composing_.exchange(false))
    return; // wasn't composing
  if (companion_.empty())
    return;
  std::string conversationId = resolveConversationId(companion_);
  if (conversationId.empty())
    return;
  std::string account = current_account_;
  fireAndForgetLibjamiCall([account, conversationId] {
    libjami::setIsComposing(account, conversationId, false);
  });
}

// ---- Push-to-talk voice/video messages (/am, /vm, or bare 'm' key) ----
//
// /vm or /am starts recording. While recording, the same command stops and
// sends. /cancel or /c stops recording and discards the temporary file.
// Video recording also has a live ffplay preview fed from the same ffmpeg
// capture process, so the camera is opened only once.
void JamcliApp::startPushToTalk(CallType type) {
  if (recording_active_) {
    stopPushToTalkAndSend();
    return;
  }
  if (companion_.empty()) {
    ui_.printSystemMessage("Use /chat <user> first.");
    return;
  }
  if (!commandExists("ffmpeg")) {
    ui_.printSystemMessage(
        "ffmpeg not found - cannot record voice/video messages.");
    return;
  }
  const bool previewWithMpv =
      (renderer_launcher_.renderer() == "mpv") && commandExists("mpv");
  if (type == CallType::VIDEO && !previewWithMpv && !commandExists("ffplay")) {
    ui_.printSystemMessage(
        "ffplay/mpv not found - cannot show video recording preview.");
    return;
  }

  std::string ext = (type == CallType::VIDEO) ? "mp4" : "m4a";
  recording_path_ =
      "/tmp/jamcli-msg-" + std::to_string(::getpid()) + "-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "." + ext;
  recording_type_ = type;
  recording_preview_fifo_.clear();
  recording_preview_pid_ = -1;

  if (type == CallType::VIDEO) {
    recording_preview_fifo_ =
        "/tmp/jamcli-preview-" + std::to_string(::getpid()) + "-" +
        std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) +
        ".nut";
    ::unlink(recording_preview_fifo_.c_str());
    if (::mkfifo(recording_preview_fifo_.c_str(), 0600) != 0) {
      ui_.printSystemMessage("Could not create video preview pipe.");
      recording_preview_fifo_.clear();
      recording_type_ = CallType::NONE;
      recording_path_.clear();
      return;
    }

    // ffplay consumes the live NUT stream. stdin is /dev/null so it cannot
    // interfere with jamcli's terminal input.
    pid_t previewPid = ::fork();
    if (previewPid == 0) {
      int nullfd = ::open("/dev/null", O_RDONLY);
      if (nullfd >= 0) {
        ::dup2(nullfd, STDIN_FILENO);
        if (nullfd != STDIN_FILENO)
          ::close(nullfd);
      }
      const std::string title = "jamcli recording preview";
      if (previewWithMpv) {
        const std::string titleArg = "--title=" + title;
        ::execlp("mpv", "mpv", "--really-quiet", "--demuxer-lavf-format=nut",
                 titleArg.c_str(), recording_preview_fifo_.c_str(),
                 static_cast<char *>(nullptr));
      } else {
        ::execlp("ffplay", "ffplay", "-hide_banner", "-loglevel", "error",
                 "-autoexit", "-window_title", title.c_str(), "-f", "nut",
                 recording_preview_fifo_.c_str(), static_cast<char *>(nullptr));
      }
      ::_exit(127);
    }
    if (previewPid < 0) {
      ::unlink(recording_preview_fifo_.c_str());
      recording_preview_fifo_.clear();
      recording_type_ = CallType::NONE;
      recording_path_.clear();
      ui_.printSystemMessage("Could not start video preview.");
      return;
    }
    recording_preview_pid_ = previewPid;
  }

  pid_t pid = ::fork();
  if (pid == 0) {
    ::close(STDIN_FILENO);
    std::string logPath =
        "/tmp/jamcli-record-" + std::to_string(::getpid()) + ".log";
    std::freopen(logPath.c_str(), "w", stdout);
    std::freopen(logPath.c_str(), "a", stderr);

    std::string micDevice =
        (config_.mic.device != "as-is" && !config_.mic.device.empty())
            ? config_.mic.device
            : "default";
    if (type == CallType::AUDIO) {
      ::execlp("ffmpeg", "ffmpeg", "-y", "-f", "pulse", "-i", micDevice.c_str(),
               recording_path_.c_str(), static_cast<char *>(nullptr));
    } else {
      // One capture process writes the final MP4 and simultaneously
      // feeds the live preview through a NUT FIFO.
      const std::string teeSpec =
          "[f=mp4]" + recording_path_ + "|[f=nut]" + recording_preview_fifo_;
      ::execlp("ffmpeg", "ffmpeg", "-y", "-f", "v4l2", "-i", "/dev/video0",
               "-f", "pulse", "-i", micDevice.c_str(), "-map", "0:v:0", "-map",
               "1:a:0", "-c:v", "libx264", "-preset", "veryfast", "-tune",
               "zerolatency", "-c:a", "aac", "-f", "tee", teeSpec.c_str(),
               static_cast<char *>(nullptr));
    }
    std::fprintf(stderr, "jamcli: exec of ffmpeg failed\n");
    ::_exit(127);
  }
  if (pid < 0) {
    if (recording_preview_pid_ > 0) {
      ::kill(recording_preview_pid_, SIGTERM);
      ::waitpid(recording_preview_pid_, nullptr, 0);
      recording_preview_pid_ = -1;
    }
    if (!recording_preview_fifo_.empty()) {
      ::unlink(recording_preview_fifo_.c_str());
      recording_preview_fifo_.clear();
    }
    ui_.printSystemMessage("Could not start recording (fork failed).");
    recording_type_ = CallType::NONE;
    recording_path_.clear();
    return;
  }

  recording_pid_ = pid;
  recording_active_ = true;
  ui_.printSystemMessage(std::string("Recording ") +
                         (type == CallType::AUDIO ? "voice" : "video") +
                         " message... /" +
                         (type == CallType::AUDIO ? "am" : "vm") +
                         " again or m = send, /cancel = discard.");
}

void JamcliApp::stopPushToTalkAndSend() { finishPushToTalk(true); }

void JamcliApp::cancelPushToTalk() { finishPushToTalk(false); }

void JamcliApp::finishPushToTalk(bool send) {
  if (!recording_active_)
    return;
  recording_active_ = false;

  if (recording_pid_ > 0) {
    ::kill(recording_pid_, SIGINT); // finalize the recording cleanly
    int status = 0;
    for (int i = 0; i < 50; ++i) {
      if (::waitpid(recording_pid_, &status, WNOHANG) > 0) {
        recording_pid_ = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (recording_pid_ > 0) {
      ::kill(recording_pid_, SIGKILL);
      ::waitpid(recording_pid_, nullptr, 0);
      recording_pid_ = -1;
    }
  }

  if (recording_preview_pid_ > 0) {
    // ffplay normally exits on FIFO EOF after ffmpeg stops. Give it a
    // moment, then terminate it so cancel never leaves a preview behind.
    int status = 0;
    if (::waitpid(recording_preview_pid_, &status, WNOHANG) == 0) {
      ::kill(recording_preview_pid_, SIGTERM);
      ::waitpid(recording_preview_pid_, nullptr, 0);
    }
    recording_preview_pid_ = -1;
  }

  const std::string path = recording_path_;
  const CallType type = recording_type_;
  const std::string fifo = recording_preview_fifo_;
  recording_path_.clear();
  recording_preview_fifo_.clear();
  recording_type_ = CallType::NONE;
  if (!fifo.empty())
    ::unlink(fifo.c_str());

  if (send) {
    ui_.printSystemMessage("Recording stopped, sending...");
    sendFileToCompanion(path);
  } else {
    ::unlink(path.c_str());
    ui_.printSystemMessage(std::string("Recording canceled; ") +
                           (type == CallType::VIDEO ? "video" : "voice") +
                           " message was not sent.");
  }
}

void JamcliApp::handleIncomingFileOffer(
    const std::string &conversationId, const std::string &interactionId,
    const std::string &fileId, const std::string &author,
    const std::map<std::string, std::string> &body) {
  std::string path;
  int64_t total = 0, progress = 0;
  libjami::fileTransferInfo(current_account_, conversationId, fileId, path,
                            total, progress);

  auto nameIt = body.find("displayName");
  std::string displayName = (nameIt != body.end() && !nameIt->second.empty())
                                ? nameIt->second
                                : fileId;

  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    pending_file_ = {conversationId, interactionId, fileId, author,
                     displayName,    total,         true};
  }

  logAndPrintIncomingFile(path, displayName, author, conversationId,
                          interactionId, fileId);
  ui_.printSystemMessage("Incoming file '" + truncateTitle(displayName, 120) +
                         "' (" + std::to_string(total) + " bytes) from @" +
                         author +
                         ". Use /receive or /r to accept, /cancel to decline.");
}

void JamcliApp::acceptPendingFile() {
  FileTransferState xfer;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    if (!pending_file_.active) {
      ui_.printSystemMessage("No incoming call or file transfer to receive.");
      return;
    }
    xfer = pending_file_;
  }

  std::string savePath = "./" + xfer.displayName;
  bool ok = libjami::downloadFile(current_account_, xfer.conversationId,
                                  xfer.interactionId, xfer.fileId, savePath);
  if (ok) {
    ui_.printSystemMessage("Receiving '" +
                           truncateTitle(xfer.displayName, 120) + "' -> " +
                           savePath);
    std::lock_guard<std::mutex> lock(msg_mutex_);
    for (auto &[key, rec] : message_log_) {
      if (rec.isFile && rec.fileId == xfer.fileId && !rec.outgoing) {
        rec.filePath = savePath;
        break;
      }
    }
  } else {
    ui_.printSystemMessage("Could not start download for '" +
                           truncateTitle(xfer.displayName, 120) + "'.");
  }
}

void JamcliApp::sendFileToCompanion(const std::string &path) {
  if (companion_.empty()) {
    ui_.printSystemMessage("Use /chat <user> first.");
    return;
  }
  std::ifstream f(path);
  if (!f.good()) {
    ui_.printSystemMessage("File not found: " + path);
    return;
  }
  f.close();

  std::string conversationId = resolveConversationId(companion_);
  if (conversationId.empty()) {
    ui_.printSystemMessage("No conversation with @" +
                           displayNameOrHash(companion_) + " yet.");
    return;
  }

  auto slash = path.find_last_of('/');
  std::string displayName =
      (slash == std::string::npos) ? path : path.substr(slash + 1);
  displayName = truncateTitle(displayName, 120);

  libjami::sendFile(current_account_, conversationId, path, displayName, "");
  ui_.printSystemMessage("Sending '" + displayName + "' to @" +
                         displayNameOrHash(companion_) + "...");
  logAndPrintOutgoingFile(path, displayName, companion_, conversationId, "");
}

void JamcliApp::fireAndForgetLibjamiCall(std::function<void()> call) {
  std::thread(std::move(call)).detach();
}

void JamcliApp::cancelActive() {
  if (recording_active_) {
    cancelPushToTalk();
    return;
  }

  IncomingCallState incoming;
  std::string activeCall;
  bool connected = false;
  FileTransferState xfer;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    incoming = pending_call_;
    activeCall = active_call_id_;
    connected = call_connected_;
    xfer = pending_file_;
  }

  if (incoming.active) {
    ui_.stopRingingAnimation();
    libjami::refuse(current_account_, incoming.callId);
    std::lock_guard<std::mutex> lock(call_mutex_);
    pending_call_.active = false;
    ui_.printSystemMessage("Declined call from @" +
                           displayNameOrHash(incoming.peerUri));
    return;
  }

  if (!activeCall.empty() && !connected) {
    std::string account = current_account_;
    fireAndForgetLibjamiCall(
        [account, activeCall] { libjami::hangUp(account, activeCall); });
    ui_.printSystemMessage("Outgoing call canceled.");
    return;
  }

  if (!activeCall.empty() && connected) {
    ui_.printSystemMessage(
        "Call is already connected - use /hangup or /h to hang up.");
    return;
  }

  if (xfer.active) {
    libjami::cancelDataTransfer(current_account_, xfer.conversationId,
                                xfer.fileId);
    std::lock_guard<std::mutex> lock(call_mutex_);
    pending_file_.active = false;
    ui_.printSystemMessage("File transfer canceled.");
    return;
  }

  ui_.printSystemMessage("Nothing to cancel.");
}

// Ends the current companion chat: cancels every outstanding request
// (unlike cancelActive(), which stops at the first match) and returns
// to message mode.
void JamcliApp::endChat() {
  if (mode_ != UiMode::COMPANION) {
    ui_.printSystemMessage("Not in a chat. Use /chat <user> to start one.");
    return;
  }

  cancelAllPending();
  resetLocalComposing();

  ui_.printSystemMessage("Ended chat with @" + displayNameOrHash(companion_) +
                         ".");
  companion_.clear();
  mode_ = UiMode::MESSAGE;
  ui_.setMode(mode_);
}

// Cancels/declines/hangs-up every pending call, file transfer, and
// push-to-talk recording, without stopping at the first one it finds.
void JamcliApp::cancelAllPending() {
  if (recording_active_) {
    cancelPushToTalk();
  }

  IncomingCallState incoming;
  std::string activeCall;
  FileTransferState xfer;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    incoming = pending_call_;
    activeCall = active_call_id_;
    xfer = pending_file_;
  }

  if (incoming.active) {
    ui_.stopRingingAnimation();
    libjami::refuse(current_account_, incoming.callId);
    std::lock_guard<std::mutex> lock(call_mutex_);
    pending_call_.active = false;
    ui_.printSystemMessage("Declined call from @" +
                           displayNameOrHash(incoming.peerUri));
  }

  if (!activeCall.empty()) {
    libjami::hangUp(current_account_, activeCall);
    std::lock_guard<std::mutex> lock(call_mutex_);
    active_call_id_.clear();
    call_connected_ = false;
    ui_.printSystemMessage("Call ended.");
  }

  if (xfer.active) {
    libjami::cancelDataTransfer(current_account_, xfer.conversationId,
                                xfer.fileId);
    std::lock_guard<std::mutex> lock(call_mutex_);
    pending_file_.active = false;
    ui_.printSystemMessage("File transfer canceled.");
  }
}

void JamcliApp::hangupActive() {
  std::string activeCall;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    activeCall = active_call_id_;
  }

  if (activeCall.empty()) {
    ui_.printSystemMessage("No active call to hang up.");
    return;
  }

  ui_.printSystemMessage("Hanging up...");
  std::string account = current_account_;
  fireAndForgetLibjamiCall([this, account, activeCall] {
    libjami::hangUp(account, activeCall);
    ui_.printSystemMessage("Call hang up.");
  });
}

void JamcliApp::setLocalVideoMuted(bool mute) {
  std::string callId;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    callId = active_call_id_;
  }
  if (callId.empty()) {
    ui_.printSystemMessage("No active call.");
    return;
  }
  if (mute)
    local_video_.pause();
  else
    local_video_.resume();

  std::string account = current_account_;
  fireAndForgetLibjamiCall([account, callId, mute] {
    libjami::muteLocalMedia(account, callId,
                            libjami::Media::MediaAttributeValue::VIDEO, mute);
  });
  ui_.printSystemMessage(mute ? "Camera muted." : "Camera unmuted.");
}

void JamcliApp::setupInitialAccount() {
  ui_.printSystemMessage("No account found.");
  std::cout << "[1] Create new account\n"
            << "[2] Import account backup file\n"
            << "Select option (1/2): " << std::flush;

  std::string choice;
  std::getline(std::cin, choice);

  std::map<std::string, std::string> details;
  details["Account.type"] = "RING";

  if (choice == "2") {
    std::cout << "Enter path to backup archive (.gz): " << std::flush;
    std::string path;
    std::getline(std::cin, path);

    std::cout << "Enter backup password (if any): " << std::flush;
    std::string password;
    std::getline(std::cin, password);

    details["Account.archivePath"] = path;
    details["Account.archivePassword"] = password;
  } else {
    std::cout << "Enter alias/display name: " << std::flush;
    std::string alias;
    std::getline(std::cin, alias);
    details["Account.alias"] = alias;
  }

  current_account_ = libjami::addAccount(details);
  ui_.printSystemMessage("Account created/imported successfully: " +
                         current_account_);
  login();

  // Registering a public name is entirely optional at this point -
  // leaving it blank skips it, and it can always be done later with
  // /give-registerName <name>.
  if (choice != "2") {
    std::cout << "Register a public name now? Leave blank to skip: "
              << std::flush;
    std::string registerName;
    std::getline(std::cin, registerName);
    if (!registerName.empty()) {
      registerAccountName(registerName);
    }
  }
}

void JamcliApp::reportRawRendererCommandIfNeeded(const std::string &role, int w,
                                                 int h) {
  if (renderer_launcher_.renderer() != "raw")
    return;
  const std::string &path = (role == "local")
                                ? renderer_launcher_.localSocketPath()
                                : renderer_launcher_.remoteSocketPath();
  if (path.empty())
    return;
  char hostbuf[256] = {0};
  std::string host = (::gethostname(hostbuf, sizeof(hostbuf) - 1) == 0)
                         ? hostbuf
                         : "<this-host>";
  ui_.printSystemMessage("[raw] run this on your local machine to view the " +
                         role + " stream:");
  ui_.printSystemMessage(
      "  ssh " + host + " jamcli --raw-renderer " + role + " unix://" + path +
      " | ffplay -f rawvideo -pixel_format rgb0 -video_size " +
      std::to_string(w) + "x" + std::to_string(h) + " -i -");
}

void JamcliApp::login() {
  if (current_account_.empty()) {
    ui_.printSystemMessage("No valid account set.");
    return;
  }
  applyNetworkConfig();
  libjami::sendRegister(current_account_, true);
  mode_ = UiMode::MESSAGE;
  ui_.setMode(mode_);

  // Jami identity fields are deliberately kept separate:
  //   Account.username       = permanent Jami ID / public-key fingerprint
  //   Account.displayName    = editable profile display name
  //   Account.registeredName = optional name-server username
  // Do NOT use Account.username as the display name: on accounts with a
  // registered name it can be confused with the user-facing username.
  auto details = libjami::getAccountDetails(current_account_);
  if (auto it = details.find("Account.username"); it != details.end()) {
    own_username_ = it->second;
  }

  std::string hash;
  if (auto it = details.find("Account.username"); it != details.end())
    hash = it->second;
  own_username_ = hash;

  std::string displayName;
  for (const char *key : {"Account.displayName", "Account.alias"}) {
    auto it = details.find(key);
    if (it != details.end() && !it->second.empty()) {
      displayName = it->second;
      break;
    }
  }

  std::string registeredName;
  for (const char *key : {"Account.registeredName", "registeredName"}) {
    auto it = details.find(key);
    if (it != details.end() && !it->second.empty()) {
      registeredName = it->second;
      break;
    }
  }

  // ------------------------------------------------------------------
  // Load local config.yaml for additional account info
  // ------------------------------------------------------------------
  const char *home = std::getenv("HOME");
  if (home) {
    std::vector<std::string> configPaths = {
        std::string(home) + "/.local/share/jami/" + current_account_ +
            "/config.yaml",
        std::string(home) + "/.local/share/jami/" + current_account_ +
            "/config.yml",
        std::string(home) + "/.config/jami/" + current_account_ +
            "/config.yaml",
        std::string(home) + "/.config/jami/" + current_account_ + "/config.yml",
    };

    std::map<std::string, std::string> yaml;
    std::string foundPath;
    for (const auto &p : configPaths) {
      // ui_.printSystemMessage("[debug] trying config: " + p);
      yaml = parseSimpleYaml(p);
      if (!yaml.empty())
        break;
    }

    std::string yamlDisplayName =
        yaml.count("displayName") ? yaml["displayName"] : "";
    std::string yamlUsername = yaml.count("username") ? yaml["username"] : "";
    std::string yamlRegisteredName = yaml.count("Account.registeredName")
                                         ? yaml["Account.registeredName"]
                                         : "";
    std::string yamlInterface =
        yaml.count("interface") ? yaml["interface"] : "";
    std::string yamlDeviceName =
        yaml.count("Account.deviceName") ? yaml["Account.deviceName"] : "";

    if (displayName.empty() && !yamlDisplayName.empty())
      displayName = yamlDisplayName;
    if (hash.empty() && !yamlUsername.empty())
      hash = yamlUsername;
    if (registeredName.empty() && !yamlRegisteredName.empty())
      registeredName = yamlRegisteredName;

    ui_.printSystemMessage("Using interface: " + (yamlInterface.empty()
                                                      ? "<default>"
                                                      : yamlInterface));
    ui_.printSystemMessage("This hostname: " + (yamlDeviceName.empty()
                                                    ? "<unknown>"
                                                    : yamlDeviceName));
  }

  subscribeAllContactsPresence();

  ui_.printSystemMessage("Logged in.");
  ui_.printSystemMessage("Display Name: " +
                         (displayName.empty() ? "User" : displayName));
  ui_.printSystemMessage("Hash: " + (hash.empty() ? "<unknown>" : hash));
  if (!registeredName.empty())
    ui_.printSystemMessage("Registered Name: " + registeredName);

  // ------------------------------------------------------------------
  // Load own profile photo from profile.vcf and render inline
  // ------------------------------------------------------------------
  if (home) {
    std::string ownVCardPath = std::string(home) + "/.local/share/jami/" +
                               current_account_ + "/profile.vcf";
    auto ownProfile = parseVCardFile(ownVCardPath);
    if (debug_log_) {
      ui_.printSystemMessage("[debug] trying own vCard: " + ownVCardPath);
      ui_.printSystemMessage("[debug] vCard keys: " +
                             std::to_string(ownProfile.size()));
      for (const auto &[k, v] : ownProfile) {
        ui_.printSystemMessage("[debug vcard] " + k + " = " +
                               (v.size() > 60 ? v.substr(0, 60) + "..." : v));
      }
    }

    if (!ownProfile["avatar"].empty()) {
      std::string path;
      if (writeAvatarBase64ToTemp(ownProfile["avatar"], path)) {
        std::string extPath = path + ".jpg";
        ::rename(path.c_str(), extPath.c_str());
        // Render inline at login (terminal is still in canonical mode)
        if (renderInlineMedia(extPath, "avatar.jpg", 120)) {
          ui_.printSystemMessage("Photo: available");
        } else {
          ui_.printSystemMessage("Photo: available (render skipped)");
        }
        std::string avatarId =
            logAvatarFile(extPath, "avatar.jpg", own_username_);
        ui_.printSystemMessage("Photo: [avatar: " + avatarId + "]");
      } else {
        ui_.printSystemMessage("Photo: could not decode avatar data");
      }
    } else {
      ui_.printSystemMessage("Photo: none (no avatar in profile.vcf)");
    }
  } else {
    ui_.printSystemMessage("Photo: none (HOME not set)");
  }
}

void JamcliApp::applyNetworkConfig() {
  auto details = libjami::getAccountDetails(current_account_);

  // Account.localInterface: explicit interface names are applied only if
  // the interface currently exists. Otherwise leave the account detail
  // untouched so Jami falls back to its normal interface selection.
  std::string requestedInterface = config_.account.localInterface;
  if (requestedInterface == "as-is" || requestedInterface.empty())
    requestedInterface = config_.network.interface; // legacy alias

  if (requestedInterface != "as-is" && !requestedInterface.empty()) {
    if (::if_nametoindex(requestedInterface.c_str()) != 0) {
      details["Account.localInterface"] = requestedInterface;
    } else {
      ui_.printSystemMessage("[config] interface '" + requestedInterface +
                             "' not found; falling back to as-is.");
    }
  }

  details["Account.upnpEnabled"] = config_.network.upnp ? "true" : "false";
  details["Account.peerDiscovery"] =
      config_.network.localPeerDiscovery ? "true" : "false";
  details["Account.proxyEnabled"] = config_.network.dhtProxy ? "true" : "false";
  if (config_.network.dhtBootstrap && !config_.network.dhtBootstrapNode.empty())
    details["Account.hostname"] = config_.network.dhtBootstrapNode;

  // Canonical TURN.* keys. Legacy network.turn-server* values remain
  // supported for existing configurations.
  const bool turnEnable = config_.network.turnServer && config_.turn.enable;
  const std::string turnServer =
      (config_.turn.server == "turn.jami.net:3478" &&
       config_.network.turnServerUrl != "turn.jami.net:3478")
          ? config_.network.turnServerUrl
          : config_.turn.server;
  const std::string turnUsername =
      (config_.turn.username == "ring" &&
       config_.network.turnServerUsername != "ring")
          ? config_.network.turnServerUsername
          : config_.turn.username;
  const std::string turnPassword =
      (config_.turn.password == "ring" &&
       config_.network.turnServerPassword != "ring")
          ? config_.network.turnServerPassword
          : config_.turn.password;
  const std::string turnRealm = (config_.turn.realm == "ring" &&
                                 config_.network.turnServerRealm != "ring")
                                    ? config_.network.turnServerRealm
                                    : config_.turn.realm;

  details["TURN.enable"] = turnEnable ? "true" : "false";
  details["TURN.server"] = turnServer;
  details["TURN.username"] = turnUsername;
  details["TURN.password"] = turnPassword;
  details["TURN.realm"] = turnRealm;

  // Canonical Account.managerUri / Account.managerUsername. Legacy
  // network.jams* values are accepted as a compatibility fallback.
  std::string managerUri = config_.account.managerUri;
  std::string managerUsername = config_.account.managerUsername;
  if (managerUri.empty() && config_.network.jams)
    managerUri = config_.network.jamsServer;
  if (managerUsername.empty() && config_.network.jams)
    managerUsername = config_.network.jamsUser;

  if (!managerUri.empty()) {
    if (managerUri.rfind("http://", 0) != 0 &&
        managerUri.rfind("https://", 0) != 0)
      managerUri = "https://" + managerUri;
    details["Account.managerUri"] = managerUri;
  }
  if (!managerUsername.empty())
    details["Account.managerUsername"] = managerUsername;

  libjami::setAccountDetails(current_account_, details);
  ui_.printSystemMessage(
      "[config] network/account settings applied to account " +
      current_account_ + ".");
}

void JamcliApp::logout() {
  if (mode_ == UiMode::SETTING) {
    ui_.printSystemMessage("Already in setting mode.");
    return;
  }

  std::cout << "Export account backup file before logout? (y/N): "
            << std::flush;
  std::string resp;
  std::getline(std::cin, resp);

  if (resp == "y" || resp == "Y") {
    std::cout << "Enter destination backup archive path (.gz): " << std::flush;
    std::string dest_path;
    std::getline(std::cin, dest_path);

    std::cout << "Enter archive password: " << std::flush;
    std::string pwd;
    std::getline(std::cin, pwd);

    libjami::exportToFile(current_account_, dest_path, pwd);
    ui_.printSystemMessage("Account backup exported to: " + dest_path);
  }

  resetLocalComposing();
  libjami::sendRegister(current_account_, false);
  current_account_.clear();
  own_username_.clear();
  companion_.clear();
  mode_ = UiMode::SETTING;
  ui_.setMode(mode_);
  ui_.printSystemMessage("Logged out. Returned to setting mode '#'");
}

// ---- Command handlers (one per slash command, uniform signature) ----
// All commands are matched case-insensitively (see processInputLine()):
// the dispatcher lowercases the command word before lookup, so these
// bodies never need to care about the case the user actually typed.
void JamcliApp::cmdLogin(std::istringstream &) {
  if (mode_ == UiMode::SETTING) {
    auto accs = libjami::getAccountList();
    if (accs.empty()) {
      setupInitialAccount();
    } else {
      current_account_ = accs.front();
      login();
    }
  } else {
    ui_.printSystemMessage("Already logged in.");
  }
}

void JamcliApp::cmdMe(std::istringstream &) { login(); }
void JamcliApp::cmdLogout(std::istringstream &) { logout(); }

void JamcliApp::cmdList(std::istringstream &iss) {
  std::string arg;
  iss >> arg;
  listUsers(toLowerCopy(arg) == "all");
}

void JamcliApp::cmdAdd(std::istringstream &iss) {
  std::string user_hash;
  iss >> user_hash;
  if (!user_hash.empty()) {
    std::string account = current_account_;
    fireAndForgetLibjamiCall(
        [account, user_hash] { libjami::addContact(account, user_hash); });
    ui_.printSystemMessage("Added user hash: " + user_hash);
  } else {
    ui_.printSystemMessage("Usage: /add <user hash>");
  }
}

void JamcliApp::cmdAccept(std::istringstream &iss) {
  std::string user;
  iss >> user;
  if (user.empty()) {
    ui_.printSystemMessage("Usage: /accept <user> (or /ac <user>)");
  } else {
    acceptInvitation(user);
  }
}

void JamcliApp::cmdChat(std::istringstream &iss) {
  if (mode_ == UiMode::SETTING) {
    ui_.printSystemMessage("Login required first.");
    return;
  }
  std::string target;
  iss >> target;
  if (target.empty()) {
    ui_.printSystemMessage("Usage: /chat @<ID> (for example /chat @02)");
    return;
  }
  target = resolveContactAlias(target);
  if (target.empty()) {
    ui_.printSystemMessage(
        "Unknown contact alias. Use /list first (aliases are @1..@ff).");
    return;
  }
  resetLocalComposing(); // notify the old companion before switching
  companion_ = target;
  mode_ = UiMode::COMPANION;
  ui_.setMode(mode_, displayNameOrHash(companion_));

  if (resolveConversationId(companion_).empty()) {
    ui_.printSystemMessage(
        "Note: no swarm conversation with @" + displayNameOrHash(companion_) +
        " found yet. "
        "Messages will fail until the contact request is accepted.");
  }
}

void JamcliApp::cmdAddImg(std::istringstream &iss) {
  std::string path = restOfLine(iss);
  if (path.empty()) {
    ui_.printSystemMessage("Usage: /add-img <file path>");
  } else {
    updateOwnAvatar(path);
  }
}

void JamcliApp::cmdGiveDisplayName(std::istringstream &iss) {
  std::string displayName = restOfLine(iss);
  if (displayName.empty()) {
    ui_.printSystemMessage("Usage: /give-displayName <displayName>");
  } else {
    setOwnDisplayName(displayName);
  }
}

void JamcliApp::cmdGiveRegisterName(std::istringstream &iss) {
  std::string registerName = restOfLine(iss);
  if (registerName.empty()) {
    ui_.printSystemMessage("Usage: /give-registerName <registerName>");
  } else {
    registerAccountName(registerName);
  }
}

void JamcliApp::cmdAudioCall(std::istringstream &) {
  initiateCall(CallType::AUDIO);
}

void JamcliApp::cmdVideoCall(std::istringstream &) {
  initiateCall(CallType::VIDEO);
}

void JamcliApp::cmdReceive(std::istringstream &) {
  bool hasIncomingCall = false;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    hasIncomingCall = pending_call_.active;
  }
  if (hasIncomingCall) {
    acceptPendingCall();
  } else {
    acceptPendingFile();
  }
}

void JamcliApp::cmdCancel(std::istringstream &) { cancelActive(); }

void JamcliApp::cmdHangup(std::istringstream &) { hangupActive(); }

void JamcliApp::cmdMute(std::istringstream &) { setLocalVideoMuted(true); }

void JamcliApp::cmdUnmute(std::istringstream &) { setLocalVideoMuted(false); }

void JamcliApp::cmdSend(std::istringstream &iss) {
  std::string path = restOfLine(iss);
  if (path.empty()) {
    ui_.printSystemMessage("Usage: /send <file path>");
  } else {
    sendFileToCompanion(path);
  }
}

void JamcliApp::cmdAudioMessage(std::istringstream &) {
  startPushToTalk(CallType::AUDIO);
}

void JamcliApp::cmdVideoMessage(std::istringstream &) {
  startPushToTalk(CallType::VIDEO);
}

void JamcliApp::cmdEndChat(std::istringstream &) { endChat(); }

void JamcliApp::cmdQuit(std::istringstream &) { handleQuit(); }

// Trailing free-text argument (e.g. a file path or a name), with the
// single separating space stripped. Shared by every command whose
// argument may itself contain spaces.
std::string JamcliApp::restOfLine(std::istringstream &iss) {
  std::string rest;
  std::getline(iss, rest);
  if (!rest.empty() && rest.front() == ' ')
    rest.erase(0, 1);
  return rest;
}

// Handles the commands whose name isn't a fixed word but a fixed prefix
// followed by caller-supplied data (a hex message id, or reply text).
// Returns true if `cmdLower` matched one of these and was handled.
bool JamcliApp::dispatchPrefixCommand(const std::string &cmdLower,
                                      const std::string &rawLine) {
  // /open<ID> or /o<ID>: reopen a previously logged file. ID case does
  // not matter - it is normalized to upper hex below either way.
  if ((cmdLower.rfind("/open", 0) == 0 || cmdLower.rfind("/o", 0) == 0) &&
      cmdLower.size() >= 4) {
    const size_t prefixLen = (cmdLower.rfind("/open", 0) == 0) ? 5 : 2;
    const std::string id = toUpperHexId(cmdLower.substr(prefixLen));
    if (id.size() == 2 && std::isxdigit(static_cast<unsigned char>(id[0])) &&
        std::isxdigit(static_cast<unsigned char>(id[1]))) {
      openLoggedFile(id);
    } else {
      ui_.printSystemMessage(
          "Usage: /open<ID> or /o<ID> (for example /open0B or /o0b)");
    }
    return true;
  }

  // /del@<ID> or /delete@<ID>: mark a message deleted, and tell the peer.
  if (cmdLower.rfind("/del@", 0) == 0 || cmdLower.rfind("/delete@", 0) == 0) {
    auto atPos = cmdLower.find('@');
    std::string id = (atPos == std::string::npos)
                         ? ""
                         : toUpperHexId(cmdLower.substr(atPos + 1));
    if (id.empty()) {
      ui_.printSystemMessage("Usage: /del@<ID> or /delete@<ID>");
    } else {
      handleDeleteCommand(id);
      // Best-effort: tell the peer too, so their copy is marked deleted as
      // well.
      if (!companion_.empty()) {
        std::string conversationId = resolveConversationId(companion_);
        if (!conversationId.empty()) {
          libjami::sendMessage(current_account_, conversationId, "/del@" + id,
                               "", 0);
        }
      }
    }
    return true;
  }

  // /#<Jami conversation commit ID> <text>: reply directly to the
  // actual Jami swarm commit. The commit ID is sent through libjami's
  // reply-to parameter and is therefore not exposed as literal text to the
  // peer.
  if (cmdLower.rfind("/#", 0) == 0) {
    std::istringstream iss(rawLine.substr(2));
    std::string commitId;
    iss >> commitId;
    std::string text;
    std::getline(iss, text);
    if (!text.empty() && text.front() == ' ')
      text.erase(0, 1);

    if (commitId.empty() || text.empty()) {
      ui_.printSystemMessage("[*] Usage: /#<Jami commit ID> <reply text>");
      return true;
    }
    if (companion_.empty()) {
      ui_.printSystemMessage("[*] Use /chat <user> first.");
      return true;
    }
    const std::string conversationId = resolveConversationId(companion_);
    if (conversationId.empty()) {
      ui_.printSystemMessage("[*] No conversation with @" +
                             displayNameOrHash(companion_) + " yet.");
      return true;
    }

    // Jami API: flag 0 + non-empty commitId means reply to that commit.
    libjami::sendMessage(current_account_, conversationId, text, commitId, 0);
    logAndPrintOutgoingText(text, companion_, conversationId, commitId);
    setLocalComposing(false);
    return true;
  }

  // /@<ID> <text> or */@<ID>* <text>: reply to a local jamcli message id.
  if (cmdLower.rfind("/@", 0) == 0 || cmdLower.rfind("*/@", 0) == 0) {
    auto [id, text] = parseReplyMarker(rawLine);
    id = toUpperHexId(id);
    if (id.empty() || text.empty()) {
      ui_.printSystemMessage("Usage: /@<ID> <text> (reply to a message)");
    } else {
      handleReplyAndSend(id, text);
    }
    return true;
  }

  return false;
}

std::string JamcliApp::toUpperHexId(std::string id) {
  std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return id;
}

void JamcliApp::processInputLine(const std::string &line) {
  if (line.empty())
    return;

  if (line[0] == '/') {
    // /shell executes in the same working directory/environment as jamcli.
    if (toLowerCopy(line.substr(0, 6)) == "/shell" &&
        (line.size() == 6 ||
         std::isspace(static_cast<unsigned char>(line[6])))) {
      std::string command = line.substr(6);
      while (!command.empty() &&
             std::isspace(static_cast<unsigned char>(command.front())))
        command.erase(command.begin());
      if (command.empty()) {
        ui_.printSystemMessage("[*] Usage: /shell <command>");
        return;
      }

      FILE *pipe = ::popen(command.c_str(), "r");
      if (!pipe) {
        ui_.printSystemMessage("[*] shell: failed to execute command: " +
                               command);
        return;
      }
      char buffer[4096];
      bool printed = false;
      while (std::fgets(buffer, sizeof(buffer), pipe)) {
        std::string out(buffer);
        if (!out.empty() && out.back() == '\n')
          out.pop_back();
        if (!out.empty() && out.back() == '\r')
          out.pop_back();
        ui_.printSystemMessage(out);
        printed = true;
      }
      const int status = ::pclose(pipe);
      if (!printed)
        ui_.printSystemMessage("(no output)");
      if (status == -1)
        ui_.printSystemMessage("shell: could not obtain exit status");
      else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        ui_.printSystemMessage("shell: exit " +
                               std::to_string(WEXITSTATUS(status)));
      else if (WIFSIGNALED(status))
        ui_.printSystemMessage("shell: terminated by signal " +
                               std::to_string(WTERMSIG(status)));
      return;
    }

    if (toLowerCopy(line) == "/help") {
      static const char *const help[] = {"/login",
                                         "/logout",
                                         "/me",
                                         "/list [all]",
                                         "/add <user hash>",
                                         "/accept <user>  (/ac)",
                                         "/chat @<ID>",
                                         "/add-img <file>",
                                         "/give-displayName <name>",
                                         "/give-registerName <name>",
                                         "/audio-call  (/acal)",
                                         "/video-call  (/vcal)",
                                         "/receive  (/r)",
                                         "/cancel  (/c)",
                                         "/hangup  (/h)",
                                         "/mute",
                                         "/unmute",
                                         "/send <file>",
                                         "/am",
                                         "/vm",
                                         "/e  (/end)",
                                         "/quit  (/q)",
                                         "/#<Jami commit ID> <reply text>",
                                         "/@<local ID> <reply text>",
                                         "/del@<local ID>",
                                         "/open<ID>  (/o<ID>)",
                                         "/shell <command>",
                                         "/help"};
      for (const char *entry : help)
        ui_.printSystemMessage(entry);
      return;
    }

    // Fixed-name commands, matched case-insensitively via cmdLower.
    // A plain array of {names, handler} keeps this a linear, easy to
    // step-through table rather than a long if/else-if chain.
    struct CommandEntry {
      std::vector<std::string> names; // lower-case
      void (JamcliApp::*handler)(std::istringstream &);
    };
    static const CommandEntry kCommands[] = {
        {{"/login"}, &JamcliApp::cmdLogin},
        {{"/logout"}, &JamcliApp::cmdLogout},
        {{"/list"}, &JamcliApp::cmdList},
        {{"/add"}, &JamcliApp::cmdAdd},
        {{"/accept", "/ac"}, &JamcliApp::cmdAccept},
        {{"/chat"}, &JamcliApp::cmdChat},
        {{"/add-img"}, &JamcliApp::cmdAddImg},
        {{"/give-displayname"}, &JamcliApp::cmdGiveDisplayName},
        {{"/give-registername"}, &JamcliApp::cmdGiveRegisterName},
        {{"/audio-call", "/acal"}, &JamcliApp::cmdAudioCall},
        {{"/video-call", "/vcal"}, &JamcliApp::cmdVideoCall},
        {{"/receive", "/r"}, &JamcliApp::cmdReceive},
        {{"/cancel", "/c"}, &JamcliApp::cmdCancel},
        {{"/hangup", "/h"}, &JamcliApp::cmdHangup},
        {{"/mute", "/m"}, &JamcliApp::cmdMute},
        {{"/unmute", "/um"}, &JamcliApp::cmdUnmute},
        {{"/send"}, &JamcliApp::cmdSend},
        {{"/am"}, &JamcliApp::cmdAudioMessage},
        {{"/vm"}, &JamcliApp::cmdVideoMessage},
        {{"/e", "/end"}, &JamcliApp::cmdEndChat},
        {{"/quit", "/q"}, &JamcliApp::cmdQuit},
        {{"/me"}, &JamcliApp::cmdMe},
    };

    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    const std::string cmdLower = toLowerCopy(cmd);

    for (const auto &entry : kCommands) {
      if (std::find(entry.names.begin(), entry.names.end(), cmdLower) !=
          entry.names.end()) {
        (this->*entry.handler)(iss);
        return;
      }
    }

    if (dispatchPrefixCommand(cmdLower, line))
      return;

    ui_.printSystemMessage("Unknown command: " + cmd);
  } else {
    if (mode_ == UiMode::COMPANION) {
      if (!companion_.empty()) {
        std::string conversationId = resolveConversationId(companion_);
        if (conversationId.empty()) {
          ui_.printSystemMessage("No conversation with @" +
                                 displayNameOrHash(companion_) + " yet.");
        } else {
          libjami::sendMessage(current_account_, conversationId, line, "", 0);
          logAndPrintOutgoingText(line, companion_, conversationId);
          setLocalComposing(false); // sending clears the composing state
        }
      }
    } else {
      ui_.printSystemMessage(
          "Use /chat <user> to enter companion mode before sending messages.");
    }
  }
}

bool JamcliApp::isOnline(const std::string &uri) {
  std::lock_guard<std::mutex> lock(presence_mutex_);
  return online_contacts_.count(uri) != 0;
}

std::string JamcliApp::contactIdFromMap(
    const std::map<std::string, std::string> &contact) const {
  auto it = contact.find("id");
  if (it == contact.end())
    it = contact.find("uri");
  if (it == contact.end())
    return {};
  std::string id = it->second;
  if (id.rfind("jami:", 0) == 0)
    id.erase(0, 5);
  return id;
}

std::string JamcliApp::aliasForIndex(size_t index) {
  if (index == 0 || index > 0xFF)
    return {};
  std::ostringstream os;
  os << std::hex << std::nouppercase << index;
  return os.str();
}

std::string JamcliApp::resolveContactAlias(std::string target) {
  if (target.empty())
    return {};
  if (target.front() == '@')
    target.erase(0, 1);
  if (target.empty() || target.size() > 2)
    return {};
  for (char c : target) {
    if (!std::isxdigit(static_cast<unsigned char>(c)))
      return {};
  }
  unsigned long n = 0;
  try {
    n = std::stoul(target, nullptr, 16);
  } catch (...) {
    return {};
  }
  if (n == 0 || n > 0xFF)
    return {};
  std::lock_guard<std::mutex> lock(contact_alias_mutex_);
  auto it = contact_aliases_.find(static_cast<unsigned>(n));
  return it == contact_aliases_.end() ? std::string{} : it->second;
}

void JamcliApp::listUsers(bool show_all) {
  if (current_account_.empty())
    return;

  const std::string account = current_account_;

  fireAndForgetLibjamiCall([this, account, show_all] {
    try {
      auto contacts = libjami::getContacts(account);
      ui_.printSystemMessage(show_all ? "--- All Contacts ---"
                                      : "--- Online Confirmed Contacts ---");
      {
        std::lock_guard<std::mutex> lock(contact_alias_mutex_);
        contact_aliases_.clear();
      }
      size_t alias = 1;
      int shown = 0;
      for (const auto &contact : contacts) {
        std::string user_id = contactIdFromMap(contact);
        if (user_id.empty())
          continue;
        if (alias > 0xFF)
          break;
        // Confirmed / Pending
        auto confirmedIt = contact.find("confirmed");
        const bool isConfirmed =
            confirmedIt != contact.end() && confirmedIt->second == "true";
        // Online / Offline
        const bool isOnlineNow = isOnline(user_id);

        // /list: confirmed + online only
        // /list all: confirmed + offline also included
        if (!show_all && (!isConfirmed || !isOnlineNow))
          continue;

        // @1 ... @ff
        const std::string shortAlias = aliasForIndex(alias);
        {
          std::lock_guard<std::mutex> lock(contact_alias_mutex_);
          contact_aliases_[static_cast<unsigned>(alias)] = user_id;
        }

        // Contact profile
        auto details = libjami::getContactDetails(account, user_id);
        // Fetch Swarm Member Profile
        std::map<std::string, std::string> memberProfile;
        auto convIt = contact.find("conversationId");

        if (convIt != contact.end() && !convIt->second.empty()) {
          try {
            auto members =
                libjami::getConversationMembers(account, convIt->second);
            for (const auto &m : members) {
              auto uriIt = m.find("uri");
              if (uriIt != m.end() && uriIt->second == user_id) {
                memberProfile = m;
                break; // Found the specific contact's swarm profile
              }
            }
          } catch (...) {
            // Ignore gracefully if the swarm data isn't ready
          }
        }

        std::map<std::string, std::string> vcardProfile =
            getContactProfileFromVCard(account, user_id);

        if (debug_log_) {
          ui_.printSystemMessage(
              "[DEBUG] contact keys: " + std::to_string(contact.size()) +
              ", details keys: " + std::to_string(details.size()) +
              ", memberProfile keys: " + std::to_string(memberProfile.size()) +
              ", vcardProfile keys: " + std::to_string(vcardProfile.size()));
          for (const auto &[key, value] : contact) {
            ui_.printSystemMessage(
                "[DEBUG contact] " + key + " = " +
                (value.size() > 120 ? value.substr(0, 120) + "..." : value));
          }
          for (const auto &[key, value] : details) {
            ui_.printSystemMessage(
                "[DEBUG details] " + key + " = " +
                (value.size() > 120 ? value.substr(0, 120) + "..." : value));
          }
          for (const auto &[key, value] : memberProfile) {
            ui_.printSystemMessage(
                "[DEBUG memberProfile] " + key + " = " +
                (value.size() > 120 ? value.substr(0, 120) + "..." : value));
          }
          for (const auto &[key, value] : vcardProfile) {
            ui_.printSystemMessage(
                "[DEBUG vcard] " + key + " = " +
                (value.size() > 120 ? value.substr(0, 120) + "..." : value));
          }
        }

        std::string displayName;
        for (const auto &source :
             {std::cref(vcardProfile), std::cref(memberProfile),
              std::cref(contact), std::cref(details)}) {
          const auto &info = source.get();
          for (const char *key :
               {"DISPLAY_NAME", "displayName", "display_name",
                "Account.displayName", "Account.alias", "alias"}) {
            auto it = info.find(key);
            if (it != info.end() && !it->second.empty()) {
              displayName = it->second;
              break;
            }
          }
          if (!displayName.empty())
            break;
        }
        if (displayName.empty() && debug_log_) {
          ui_.printSystemMessage(
              "[DEBUG] no display name found for @" + user_id +
              " (vcardProfile/memberProfile/contact/details all empty or "
              "missing a name key - check that a profile has been received "
              "from this peer yet).");
        }
        if (displayName.empty()) {
          displayName = "User";
        } else {
          rememberDisplayName(user_id, displayName);
          ui_.setDisplayName(user_id, displayName);
        }

        // Register Name
        std::string registeredName;
        for (const auto &source :
             {std::cref(vcardProfile), std::cref(memberProfile),
              std::cref(contact), std::cref(details)}) {
          const auto &info = source.get();
          for (const char *key :
               {"REGISTERED_NAME", "registeredName", "registered_name",
                "Account.registeredName"}) {
            auto it = info.find(key);
            if (it != info.end() && !it->second.empty()) {
              registeredName = it->second;
              break;
            }
          }
          if (!registeredName.empty())
            break;
        }

        /*
         * Status
         */
        std::string contactStatus = isConfirmed ? " [Confirmed]" : " [Pending]";
        contactStatus += isOnlineNow ? " [Online]" : " [Offline]";

        // ------------------------------------------------------------------
        // Confirmed user only: extract avatar and log it with a message id
        // so the user can /open<ID> it later.  We never render inline here
        // because timg from a detached thread races with the main input loop
        // and corrupts the terminal (was causing the app to close).
        // ------------------------------------------------------------------
        std::string avatarId;
        if (isConfirmed) {
          std::string avatarEncoded;
          for (const auto &source :
               {std::cref(vcardProfile), std::cref(memberProfile),
                std::cref(contact), std::cref(details)}) {
            const auto &info = source.get();
            for (const char *key :
                 {"PHOTO", "AVATAR", "photo", "avatar", "Account.photo",
                  "Account.avatar", "Account.profilePicture",
                  "profilePicture"}) {
              auto it = info.find(key);
              if (it != info.end() && !it->second.empty()) {
                avatarEncoded = it->second;
                break;
              }
            }
            if (!avatarEncoded.empty())
              break;
          }

          if (!avatarEncoded.empty()) {
            std::string path;
            if (writeAvatarBase64ToTemp(avatarEncoded, path)) {
              std::string extPath = path + ".jpg";
              ::rename(path.c_str(), extPath.c_str());
              avatarId = logAvatarFile(extPath, "avatar.jpg", user_id);
            }
          }
        }

        /*
         * Display:
         *
         * <@01> Display Name | Hash: xxxx |
         * Register Name: xxxx [Confirmed] [Online]
         */
        std::string line =
            "<@" + shortAlias + "> " + displayName + " | Hash: " + user_id;

        if (!registeredName.empty()) {
          line += " | Register Name: " + registeredName;
        }

        line += contactStatus;

        if (!avatarId.empty()) {
          line += " [avatar: " + avatarId + "]";
        }

        ui_.printSystemMessage(line);

        ++shown;
        ++alias;
      }

      if (shown == 0) {
        ui_.printSystemMessage(show_all ? "No contacts found."
                                        : "No confirmed contacts online.");
      }

      /*
       * /list all:
       * show pending conversation requests as well.
       */
      if (!show_all)
        return;

      auto requests = libjami::getConversationRequests(account);

      for (const auto &request : requests) {
        auto fromIt = request.find("from");
        if (fromIt != request.end()) {
          ui_.printSystemMessage("User: " + fromIt->second + " [Waiting]");
        }
      }

    } catch (const std::exception &e) {
      ui_.printSystemMessage(std::string("/list failed: ") + e.what());
    } catch (...) {
      ui_.printSystemMessage("/list failed: unknown error");
    }
  });
}

void JamcliApp::updateOwnAvatar(const std::string &path) {
  if (current_account_.empty()) {
    ui_.printSystemMessage("Login required first.");
    return;
  }
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    ui_.printSystemMessage("Avatar file not found: " + path);
    return;
  }
  if (!isImageExt(fileExtension(path))) {
    ui_.printSystemMessage("Unsupported avatar image type: " + path);
    return;
  }

  const std::string account = current_account_;
  const std::string type = fileExtension(path);
  fireAndForgetLibjamiCall([this, account, path, type] {
    // flag 0 means avatar is supplied as a filesystem path.
    libjami::updateProfile(account, "", path, type, "", 0);
    ui_.printSystemMessage("Avatar update requested: " + path);
  });
}

// /give-displayName <displayName>: local profile display name only. This
// is *not* registered on the network - it is just what /list shows peers
// via DISPLAY_NAME / Account.alias. No network round trip is required and
// it can be set (or changed) at any time, repeatedly.
void JamcliApp::setOwnDisplayName(const std::string &displayName) {
  if (current_account_.empty()) {
    ui_.printSystemMessage("Login required first.");
    return;
  }

  const std::string account = current_account_;
  // avatar left empty -> only the display name changes; see
  // updateOwnAvatar() for the avatar-only counterpart of this call.
  fireAndForgetLibjamiCall([this, account, displayName] {
    libjami::updateProfile(account, displayName, "", "", "", 0);
    ui_.printSystemMessage("Display name updated: " + displayName);
  });
}

// Returns the account's already-registered public name, or "" if none.
std::string JamcliApp::currentRegisteredName() {
  if (current_account_.empty())
    return "";
  auto details = libjami::getAccountDetails(current_account_);
  for (const char *key : {"Account.registeredName", "registeredName"}) {
    auto it = details.find(key);
    if (it != details.end() && !it->second.empty())
      return it->second;
  }
  return "";
}

// /give-registerName <registerName>: attempts to register the given name
// for the currently logged-in account on the Jami name service (a
// one-time, network operation - unlike setOwnDisplayName() above). Result
// arrives asynchronously via the NameRegistrationEnded signal handler.
void JamcliApp::registerAccountName(const std::string &registerName) {
  if (current_account_.empty()) {
    ui_.printSystemMessage("Login required first.");
    return;
  }

  const std::string account = current_account_;
  fireAndForgetLibjamiCall([this, account, registerName] {
    // This runs on a detached thread — the main input loop is never blocked.
    auto details = libjami::getAccountDetails(account);

    std::string registered;
    for (const char *key : {"Account.registeredName", "registeredName"}) {
      if (auto it = details.find(key);
          it != details.end() && !it->second.empty()) {
        registered = it->second;
        break;
      }
    }

    if (!registered.empty()) {
      ui_.printSystemMessage("Account already has a registered name: " +
                             registered);
      return;
    }

    if (!libjami::registerName(account, registerName, "", "")) {
      ui_.printSystemMessage("Failed to start name registration: " +
                             registerName);
      return;
    }

    ui_.printSystemMessage("Name registration requested: " + registerName);
  });
}

// Subscribes to presence updates for every current contact so /list can
// tell online from offline. Called once after login.
void JamcliApp::subscribeAllContactsPresence() {
  if (current_account_.empty())
    return;
  for (const auto &contact : libjami::getContacts(current_account_)) {
    auto it = contact.find("id");
    if (it == contact.end())
      it = contact.find("uri");
    if (it == contact.end())
      continue;
    libjami::subscribeBuddy(current_account_, it->second, true);
  }
}

void JamcliApp::acceptInvitation(const std::string &user) {
  if (current_account_.empty())
    return;

  for (const auto &req : libjami::getConversationRequests(current_account_)) {
    auto fromIt = req.find("from");
    auto idIt = req.find("id");
    if (fromIt != req.end() && idIt != req.end() && fromIt->second == user) {
      libjami::acceptConversationRequest(current_account_, idIt->second);
      ui_.printSystemMessage("Accepted invitation from @" +
                             displayNameOrHash(user) + ".");
      return;
    }
  }
  ui_.printSystemMessage("No pending invitation from @" +
                         displayNameOrHash(user) + ".");
}

void JamcliApp::initiateCall(CallType type) {
  if (companion_.empty()) {
    ui_.printSystemMessage("No active companion. Use /chat <user> first.");
    return;
  }

  namespace MediaKey = libjami::Media::MediaAttributeKey;
  namespace MediaVal = libjami::Media::MediaAttributeValue;

  std::vector<std::map<std::string, std::string>> mediaList;
  mediaList.push_back({
      {MediaKey::MEDIA_TYPE, MediaVal::AUDIO},
      {MediaKey::ENABLED, "true"},
      {MediaKey::MUTED, "false"},
      {MediaKey::SOURCE, ""},
      {MediaKey::LABEL, "audio_0"},
  });

  if (type == CallType::VIDEO) {
    std::string cameraMrl = "camera://" + libjami::getDefaultDevice();
    mediaList.push_back({
        {MediaKey::MEDIA_TYPE, MediaVal::VIDEO},
        {MediaKey::ENABLED, "true"},
        {MediaKey::MUTED, "false"},
        {MediaKey::SOURCE, cameraMrl},
        {MediaKey::LABEL, "video_0"},
    });
  }

  if (type == CallType::VIDEO) {
    renderer_launcher_.start();
  }

  std::string callId =
      libjami::placeCallWithMedia(current_account_, companion_, mediaList);
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    active_call_id_ = callId;
    call_connected_ = false;
  }
  ui_.printSystemMessage("Calling @" + displayNameOrHash(companion_) +
                         " (Call ID: " + callId + ")...");
  ui_.startRingingAnimation(type, companion_);

  if (type == CallType::VIDEO) {
    libjami_worker_.post(
        [this, peer = companion_] { startLocalPreview(peer); });
  }
}

void JamcliApp::acceptPendingCall() {
  IncomingCallState callToAccept;
  {
    std::lock_guard<std::mutex> lock(call_mutex_);
    if (!pending_call_.active) {
      ui_.printSystemMessage("No incoming call to accept.");
      return;
    }
    callToAccept = pending_call_;
    pending_call_.active = false;
    active_call_id_ = callToAccept.callId;
    call_connected_ = false;
  }

  if (callToAccept.type == CallType::VIDEO) {
    renderer_launcher_.start();
    libjami_worker_.post(
        [this, peer = callToAccept.peerUri] { startLocalPreview(peer); });
  }

  libjami::accept(current_account_, callToAccept.callId);
  ui_.printSystemMessage("Accepted call from @" +
                         displayNameOrHash(callToAccept.peerUri));
}

void JamcliApp::handleQuit() { running_ = false; }
