#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <sys/types.h> // pid_t

#include "config.h"
#include "terminal_ui.h"
#include "types.h"
#include "utils.h"
#include "video.h"

// ============================================================================
// LibjamiWorker
//
// Runs libjami API calls (and their callbacks) serially on a single
// dedicated thread, so the terminal input loop and Jami signal handlers
// never block on daemon calls or race with each other.
// ============================================================================
class LibjamiWorker {
public:
  LibjamiWorker();
  ~LibjamiWorker();

  void post(std::function<void()> task);

private:
  void run();

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> queue_;
  bool running_{true};
  std::thread thread_;
};

// ============================================================================
// JamcliApp
// ============================================================================
class JamcliApp {
public:
  JamcliApp();
  ~JamcliApp();
  bool init(const std::string &rendererMode, bool debugLog = false);
  bool consumeAltESCEnterSequence(char first);
  void run();

  // Background thread that clears our own "composing" state after a short
  // idle period (the daemon has no per-keystroke timeout of its own).
  void startComposingWatcher();

private:
  bool debug_log_ = false;
  void registerJamiSignalHandlers();
  void cacheConversationMembers(const std::string &conversationId);
  std::string resolveConversationId(const std::string &peerUri);
  // Applies a single conversationInfos key ("title" / "description" /
  // "avatar") to whatever conversation the current companion_ points at
  // (used by /add-group-discription, /add-group-photo; /add-group-name has
  // its own body so it can also refresh the prompt - see cmdAddGroupName).
  void updateActiveGroupInfo(const std::string &key, const std::string &value,
                             const std::string &label);
  // True if conversationId has more (or fewer) than exactly 2 members -
  // i.e. it's a group rather than a plain 1:1 contact chat (those always
  // have exactly 2 members - see cacheConversationMembers()). When true and
  // infosOut is non-null, also fills it with conversationInfos()
  // (title/description/avatar).
  bool isGroupConversation(const std::string &conversationId,
                           std::map<std::string, std::string> *infosOut = nullptr);
  // Shared by /add-group-member and /delete-group-member: resolves `arg`
  // (an @<ID> alias or a raw contact hash) and adds/removes it as a member
  // of whatever group companion_ currently points at.
  void addOrRemoveGroupMember(const std::string &arg, bool add);
  void startLocalPreview(const std::string &peer);
  void stopLocalPreview();
  void requestViewClose();
  void requestVideoSinkTeardown();

  // ---- Message id / reply / delete plumbing ----

  // Entry point for every incoming text/plain payload (both the legacy
  // IncomingAccountMessage signal and the swarm SwarmMessageReceived signal).
  // Recognizes our own "/del@ID" / "/delete@ID" control text, "/@ID text"
  // reply markers, and the jamcli-only typing markers above, on top of the
  // underlying message id/text logging.
  void handleIncomingTextPayload(const std::string &body,
                                 const std::string &peer,
                                 const std::string &conversationId,
                                 const std::string &interactionId);

  std::string logAndPrintIncomingText(const std::string &text,
                                      const std::string &peer,
                                      const std::string &conversationId,
                                      const std::string &interactionId);

  std::string logAndPrintOutgoingText(const std::string &text,
                                      const std::string &peer,
                                      const std::string &conversationId,
                                      const std::string &replyToId = "");

  std::string logAndPrintOutgoingFile(const std::string &path,
                                      const std::string &displayName,
                                      const std::string &peer,
                                      const std::string &conversationId,
                                      const std::string &fileId);

  std::string logAndPrintIncomingFile(const std::string &path,
                                      const std::string &displayName,
                                      const std::string &peer,
                                      const std::string &conversationId,
                                      const std::string &interactionId,
                                      const std::string &fileId);

  // Marks a previously-logged received file as downloaded and shows its inline
  // preview (or filename-only fallback) now that the bytes are on disk.
  void markFileDownloadedAndPreview(const std::string &fileId);

  void openLoggedFile(const std::string &id);

  // Parses a leading reply marker of the form "/@ID " or "*/@ID* " from a line,
  // returning {referencedId, remainingText}. If there is no marker,
  // referencedId is empty and remainingText is the original line.
  std::pair<std::string, std::string> parseReplyMarker(const std::string &line);

  void handleReplyAndSend(const std::string &id, const std::string &text);

  void handleDeleteCommand(const std::string &id);

  // Send the real Jami composing state. The peer receives this through
  // ConfigurationSignal::ComposingStatusChanged, not as a chat message.
  void setLocalComposing(bool composing);

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
  void resetLocalComposing();

  // ---- Push-to-talk voice/video messages (/am, /vm, or bare 'm' key) ----
  //
  // /vm or /am starts recording. While recording, the same command stops and
  // sends. /cancel or /c stops recording and discards the temporary file.
  // Video recording also has a live ffplay preview fed from the same ffmpeg
  // capture process, so the camera is opened only once.
  void startPushToTalk(CallType type);
  void stopPushToTalkAndSend();
  void cancelPushToTalk();
  void finishPushToTalk(bool send);

  void handleIncomingFileOffer(const std::string &conversationId,
                               const std::string &interactionId,
                               const std::string &fileId,
                               const std::string &author,
                               const std::map<std::string, std::string> &body);
  void acceptPendingFile();
  void sendFileToCompanion(const std::string &path);

  static void fireAndForgetLibjamiCall(std::function<void()> call);

  void cancelActive();

  // Ends the current companion chat: cancels every outstanding request
  // (unlike cancelActive(), which stops at the first match) and returns
  // to message mode.
  void endChat();

  // Cancels/declines/hangs-up every pending call, file transfer, and
  // push-to-talk recording, without stopping at the first one it finds.
  void cancelAllPending();
  void hangupActive();
  void setLocalVideoMuted(bool mute);
  void setupInitialAccount();
  void reportRawRendererCommandIfNeeded(const std::string &role, int w, int h);
  void login();
  void applyNetworkConfig();
  void logout();

  // ---- Command handlers (one per slash command, uniform signature) ----
  // All commands are matched case-insensitively (see processInputLine()):
  // the dispatcher lowercases the command word before lookup, so these
  // bodies never need to care about the case the user actually typed.
  void cmdLogin(std::istringstream &);
  void cmdLogout(std::istringstream &);
  void cmdList(std::istringstream &iss);
  void cmdAdd(std::istringstream &iss);
  void cmdAccept(std::istringstream &iss);
  void cmdDeleteHash(std::istringstream &iss);
  void cmdCreateGroup(std::istringstream &iss);
  void cmdDeleteGroup(std::istringstream &iss);
  void cmdChat(std::istringstream &iss);
  void cmdAddImg(std::istringstream &iss);
  void cmdGiveDisplayName(std::istringstream &iss);
  void cmdGiveRegisterName(std::istringstream &iss);
  // ---- Group conversation metadata (call while /chat'd into the group) ----
  void cmdAddGroupName(std::istringstream &iss);
  void cmdAddGroupDescription(std::istringstream &iss);
  void cmdAddGroupPhoto(std::istringstream &iss);
  void cmdMember(std::istringstream &);
  void cmdAddGroupMember(std::istringstream &iss);
  void cmdDeleteGroupMember(std::istringstream &iss);
  void cmdAudioCall(std::istringstream &);
  void cmdVideoCall(std::istringstream &);
  void cmdReceive(std::istringstream &);
  void cmdCancel(std::istringstream &);
  void cmdHangup(std::istringstream &);
  void cmdMute(std::istringstream &);
  void cmdUnmute(std::istringstream &);
  void cmdSend(std::istringstream &iss);
  void cmdAudioMessage(std::istringstream &);
  void cmdVideoMessage(std::istringstream &);
  void cmdEndChat(std::istringstream &);
  void cmdQuit(std::istringstream &);
  void cmdMe(std::istringstream &);
  // /flush: clears the local id_ring_/message_log_ bookkeeping used for
  // /open<ID>, /del@<ID>, /@<ID> replies, etc. Purely local housekeeping -
  // it does not touch the daemon, contacts, or conversations. After this,
  // the next logged message/file/avatar starts numbering from the first id
  // again.
  void cmdFlush(std::istringstream &);

  // Trailing free-text argument (e.g. a file path or a name), with the
  // single separating space stripped. Shared by every command whose
  // argument may itself contain spaces.
  static std::string restOfLine(std::istringstream &iss);

  // Handles the commands whose name isn't a fixed word but a fixed prefix
  // followed by caller-supplied data (a hex message id, or reply text).
  // Returns true if `cmdLower` matched one of these and was handled.
  bool dispatchPrefixCommand(const std::string &cmdLower,
                             const std::string &rawLine);

  static std::string toUpperHexId(std::string id);

  void processInputLine(const std::string &line);

  bool isOnline(const std::string &uri);

  std::string
  contactIdFromMap(const std::map<std::string, std::string> &contact) const;

  static std::string aliasForIndex(size_t index);

  std::string resolveContactAlias(std::string target);

  void listUsers(bool show_all);

  void updateOwnAvatar(const std::string &path);
  std::string logAvatarFile(const std::string &path,
                            const std::string &displayName,
                            const std::string &peer);

  // /give-displayName <displayName>: local profile display name only. This
  // is *not* registered on the network - it is just what /list shows peers
  // via DISPLAY_NAME / Account.alias. No network round trip is required and
  // it can be set (or changed) at any time, repeatedly.
  void setOwnDisplayName(const std::string &displayName);

  // Returns the account's already-registered public name, or "" if none.
  std::string currentRegisteredName();

  // /give-registerName <registerName>: attempts to register the given name
  // for the currently logged-in account on the Jami name service (a
  // one-time, network operation - unlike setOwnDisplayName() above). Result
  // arrives asynchronously via the NameRegistrationEnded signal handler.
  void registerAccountName(const std::string &registerName);

  // Subscribes to presence updates for every current contact so /list can
  // tell online from offline. Called once after login.
  void subscribeAllContactsPresence();

  void acceptInvitation(const std::string &user);

  void initiateCall(CallType type);

  void acceptPendingCall();

  void handleQuit();

  JamcliConfig config_;
  TerminalUI ui_;
  RendererLauncher renderer_launcher_;
  PeerVideoRenderer remote_video_;
  PeerVideoRenderer local_video_;
  LibjamiWorker libjami_worker_;

  std::string current_account_;
  // Our own conversation identity hash (Account.username), as embedded in
  // the "author" field of our own SwarmMessage commits. NOT the same value
  // as current_account_, which is only the local API/account id - see
  // login()/logout() for where this is kept in sync.
  std::string own_username_;
  std::string companion_;
  UiMode mode_{UiMode::SETTING};
  bool running_{true};

  std::mutex call_mutex_;
  IncomingCallState pending_call_;
  std::string active_call_id_;
  bool call_connected_{false};
  std::string active_remote_sink_id_;
  std::string local_video_input_id_;
  std::string local_preview_peer_label_;
  FileTransferState pending_file_;

  std::mutex conv_mutex_;
  std::map<std::string, std::string> uri_to_conversation_;

  // ---- Message ids / reply / delete ----
  MessageIdRing id_ring_;
  std::mutex msg_mutex_;
  std::map<std::string, MessageRecord> message_log_; // key: 2-hex-digit id

  // ---- Presence (/list vs /list all) ----
  std::mutex presence_mutex_;
  std::set<std::string> online_contacts_;

  // /list contact aliases: @1..@ff. The aliases are intentionally ephemeral
  // and are rebuilt whenever /list is run.
  std::mutex contact_alias_mutex_;
  std::map<unsigned, std::string> contact_aliases_;
  std::recursive_mutex list_avatar_ui_mutex_;

  // ---- Typing indicator ----
  std::atomic<bool> companion_is_typing_{false};
  // Written on the main input thread, read on composing_watcher_thread_ -
  // must be atomic to avoid a data race between the two.
  std::atomic<std::chrono::steady_clock::time_point> last_local_keystroke_{
      std::chrono::steady_clock::now()};
  std::atomic<bool> local_is_composing_{false};

  // ---- Raw terminal input / push-to-talk ----
  RawTerminalGuard raw_guard_;
  std::atomic<bool> recording_active_{false};
  CallType recording_type_{CallType::NONE};
  pid_t recording_pid_{-1};
  pid_t recording_preview_pid_{-1};
  std::string recording_path_;
  std::string recording_preview_fifo_;
  bool last_escape_was_shift_enter_{false};
  bool last_escape_was_arrow_left_ {false};
  bool last_escape_was_arrow_right_ {false};
  std::thread composing_watcher_thread_;
};
