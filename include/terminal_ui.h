#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <termios.h>

#include "types.h"

// ============================================================================
// Terminal & Animation UI Controller
// ============================================================================
class TerminalUI {
public:
    TerminalUI();
    ~TerminalUI();

    void setMode(UiMode mode, const std::string& companion = "");

    void startRingingAnimation(CallType type, const std::string& peer);
    void stopRingingAnimation();

    void printIncomingMessage(const std::string& msg, const std::string& peer);

    // Message-id tagged variants used once a message has been logged.
    void printIncomingTagged(const std::string& id, const std::string& msg, const std::string& peer);
    void printOutgoingTagged(const std::string& id, const std::string& msg, const std::string& peer);

    void printSystemMessage(const std::string& msg);

    // Render received/sent still images on a fresh left-aligned terminal line.
    // Keep the terminal output under the same UI mutex as prompt redraws; timg
    // writes directly to /dev/tty and otherwise can race with the input thread.
    void printInlineImagePreview(const std::string& path, const std::string& displayName);

    // ---- Typing indicator (shown while the companion is composing) ----
    void startTypingAnimation(const std::string& peer);
    void stopTypingAnimation();

    void redrawPrompt();
    void setInputBuffer(const std::string& buf);

    // Raw-mode line editing helpers: keep input_buffer_ in sync and repaint.
    void appendInputChar(char c);
    void backspaceInput();
    void clearInput();

    // Move the edit cursor within input_buffer_ (left/right arrow support).
    // No-ops at the start/end of the buffer.
    void moveCursorLeft();
    void moveCursorRight();
    size_t cursorPos();

    std::string getPromptString();
    void setDisplayName(const std::string& hash, const std::string& name);
    void appendInputText(const std::string& text);

private:
    // 0-based (row, col) of a position once printed to a terminalWidth()-wide
    // terminal; row 0 is the first (prompt) line. Used to figure out how many
    // on-screen rows the current prefix+input occupies (including rows
    // created purely by terminal line-wrap, not just explicit '\n') so we
    // clear/redraw exactly that many rows and reposition the cursor
    // correctly when it isn't at the end of the buffer.
    struct RowCol { size_t row; size_t col; };

    void updatePromptString();
    void clearCurrentLineUnlocked() const;
    void clearInputAreaUnlocked() const;
    void redrawPromptUnlocked() const;
    void animLoop(CallType type, std::string peer);
    void typingLoop(std::string peer);
    size_t computePrefixLength() const;
    RowCol offsetToRowCol(size_t absPos) const;

    std::recursive_mutex ui_mutex_;
    UiMode mode_ {UiMode::SETTING};
    std::string companion_;
    std::string prompt_str_ {"# "};
    std::string input_buffer_;
    size_t cursor_pos_ {0};
    std::string status_text_;
    std::unordered_map<std::string, std::string> display_names_;
    std::string resolveDisplayName(const std::string& hash) const;
    std::atomic<bool> running_animation_ {false};
    std::thread animation_thread_;
    std::atomic<bool> typing_running_ {false};
    std::thread typing_thread_;
};

// ============================================================================
// RawTerminalGuard
// Switches stdin to non-canonical, non-echoing mode so we can read keystrokes
// one at a time (needed for the live "companion is typing" indicator and for
// the push-to-talk 'm' key used by /am and /vm). ISIG is left enabled so
// Ctrl-C etc. keep working normally. Any code that needs a normal
// std::getline-style prompt (passwords, backup paths, ...) should wrap that
// section with withCanonicalMode().
// ============================================================================
class RawTerminalGuard {
public:
    bool enable();
    void disable();
    bool enabled() const;

    // Temporarily restores the normal terminal (with echo) for a synchronous
    // std::getline-based prompt, then re-enables raw mode afterwards.
    void withCanonicalMode(const std::function<void()>& fn);

    ~RawTerminalGuard();

private:
    termios orig_{};
    bool enabled_ {false};
};
