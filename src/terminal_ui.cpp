#include "../include/terminal_ui.h"
#include "../include/utils.h"

#include <array>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <vector>

#include <sys/ioctl.h>
#include <unistd.h>

namespace {
// Current terminal width in columns, used to figure out where the terminal
// will wrap a printed line on its own (so we can clear/redraw that many rows
// instead of assuming the input is always exactly as many rows as it has
// '\n' characters). Falls back to a sane default if the ioctl fails (e.g.
// output isn't actually a tty).
int terminalWidth() {
    struct winsize ws {};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    return 80;
}
} // namespace

// ============================================================================
// TerminalUI
// ============================================================================
TerminalUI::TerminalUI() : running_animation_(false) {}

TerminalUI::~TerminalUI() {
    stopRingingAnimation();
}

void TerminalUI::setMode(UiMode mode, const std::string& companion) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    mode_ = mode;
    companion_ = companion;
    updatePromptString();
}

void TerminalUI::setDisplayName(const std::string& hash, const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    if (name.empty()) {
        display_names_.erase(hash);
    } else {
        display_names_[hash] = name;
    }
    // If the name just arrived for the peer currently shown in the prompt,
    // refresh prompt_str_ and redraw right away instead of waiting for the
    // next mode change.
    if (mode_ == UiMode::COMPANION && companion_ == hash) {
        clearInputAreaUnlocked();
        updatePromptString();
        redrawPromptUnlocked();
    }
}

std::string TerminalUI::resolveDisplayName(const std::string& hash) const {
    auto it = display_names_.find(hash);
    return (it != display_names_.end()) ? it->second : hash;
}

void TerminalUI::startRingingAnimation(CallType type, const std::string& peer) {
    stopRingingAnimation();
    running_animation_ = true;
    animation_thread_ = std::thread(&TerminalUI::animLoop, this, type, peer);
}

void TerminalUI::stopRingingAnimation() {
    running_animation_ = false;
    if (animation_thread_.joinable()) {
        animation_thread_.join();
    }
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    status_text_.clear();
    redrawPromptUnlocked();
}

void TerminalUI::printIncomingMessage(const std::string& msg, const std::string& peer) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    std::cout << "< " << msg << " < @" << resolveDisplayName(peer) << std::endl;
    redrawPromptUnlocked();
}

void TerminalUI::printIncomingTagged(const std::string& id, const std::string& msg, const std::string& peer) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    std::cout << id << " < " << msg << " < @" << resolveDisplayName(peer) << std::endl;
    redrawPromptUnlocked();
}

void TerminalUI::printOutgoingTagged(const std::string& id, const std::string& msg, const std::string& peer) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    std::cout << id << " > <@" << resolveDisplayName(peer) << "> " << msg << std::endl;
    redrawPromptUnlocked();
}

void TerminalUI::printSystemMessage(const std::string& msg) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    std::cout << "[*] " << msg << std::endl;
    redrawPromptUnlocked();
}

void TerminalUI::printInlineImagePreview(const std::string& path, const std::string& displayName) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    std::cout << "\n" << std::flush;
    renderInlineMedia(path, displayName);
    std::cout << std::endl;
    redrawPromptUnlocked();
}

void TerminalUI::startTypingAnimation(const std::string& peer) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    if (typing_running_) return;
    typing_running_ = true;
    if (typing_thread_.joinable()) typing_thread_.join();
    typing_thread_ = std::thread(&TerminalUI::typingLoop, this, peer);
}

void TerminalUI::stopTypingAnimation() {
    {
        std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
        if (!typing_running_) return;
        typing_running_ = false;
    }
    if (typing_thread_.joinable()) typing_thread_.join();
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    status_text_.clear();
    redrawPromptUnlocked();
}

void TerminalUI::redrawPrompt() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    redrawPromptUnlocked();
}

void TerminalUI::setInputBuffer(const std::string& buf) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    input_buffer_ = buf;
    cursor_pos_ = input_buffer_.size();
}

void TerminalUI::appendInputChar(char c) {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    // Clear using the state as it currently is on screen, *before* mutating
    // the buffer - the escape codes we emit depend on where the cursor
    // visually is right now, not on the content we're about to have.
    clearInputAreaUnlocked();
    if (cursor_pos_ > input_buffer_.size()) cursor_pos_ = input_buffer_.size();
    input_buffer_.insert(input_buffer_.begin() + cursor_pos_, c);
    ++cursor_pos_;
    redrawPromptUnlocked();
}

void TerminalUI::appendInputText(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    if (cursor_pos_ > input_buffer_.size()) cursor_pos_ = input_buffer_.size();
    input_buffer_.insert(cursor_pos_, text);
    cursor_pos_ += text.size();
    redrawPromptUnlocked();
}

void TerminalUI::backspaceInput() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    if (cursor_pos_ == 0) return; // nothing before the cursor to delete
    clearInputAreaUnlocked();
    input_buffer_.erase(cursor_pos_ - 1, 1);
    --cursor_pos_;
    redrawPromptUnlocked();
}

void TerminalUI::clearInput() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    clearInputAreaUnlocked();
    input_buffer_.clear();
    cursor_pos_ = 0;
    redrawPromptUnlocked();
}

void TerminalUI::moveCursorLeft() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    if (cursor_pos_ == 0) return;
    clearInputAreaUnlocked();
    --cursor_pos_;
    redrawPromptUnlocked();
}

void TerminalUI::moveCursorRight() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    if (cursor_pos_ >= input_buffer_.size()) return;
    clearInputAreaUnlocked();
    ++cursor_pos_;
    redrawPromptUnlocked();
}

size_t TerminalUI::cursorPos() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    return cursor_pos_;
}

std::string TerminalUI::getPromptString() {
    std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
    return prompt_str_;
}

void TerminalUI::updatePromptString() {
    switch (mode_) {
        case UiMode::SETTING:
            prompt_str_ = "# ";
            break;
        case UiMode::MESSAGE:
            prompt_str_ = "> ";
            break;
        case UiMode::COMPANION:
            prompt_str_ = "<@" + resolveDisplayName(companion_) + "< ";
            break;
    }
}

void TerminalUI::clearCurrentLineUnlocked() const {
    std::cout << "\r\033[K" << std::flush;
}

size_t TerminalUI::computePrefixLength() const {
    size_t len = prompt_str_.size();
    if (!status_text_.empty()) {
        len += status_text_.size() + 3; // "[" + status + "] "
    }
    return len;
}

// Maps an offset into the conceptual "prefix + input_buffer_" string to the
// (row, col) it lands on once printed to a terminalWidth()-wide terminal,
// honoring both explicit '\n' in the input and the terminal's own line
// wrapping (a run of `width` characters fills a row without moving to the
// next one until the very next character is printed - matching how real
// terminals defer the wrap). This is what lets us clear/redraw exactly the
// rows currently on screen instead of just the explicit-'\n' line count,
// which is what caused the display to grow a new line per keystroke once
// typing reached the edge of the terminal.
TerminalUI::RowCol TerminalUI::offsetToRowCol(size_t absPos) const {
    int width = terminalWidth();
    if (width <= 0) width = 80;
    const size_t w = static_cast<size_t>(width);

    const size_t prefixLen = computePrefixLength();
    const std::string full = std::string(prefixLen, ' ') + input_buffer_;
    if (absPos > full.size()) absPos = full.size();

    size_t row = 0;
    size_t segStart = 0;
    for (size_t i = 0; i < absPos; ++i) {
        if (full[i] == '\n') {
            ++row;
            segStart = i + 1;
        }
    }
    const size_t segLen = absPos - segStart;
    size_t col = 0;
    if (segLen > 0) {
        row += (segLen - 1) / w;
        col = (segLen - 1) % w + 1;
    }
    return {row, col};
}

void TerminalUI::clearInputAreaUnlocked() const {
    // The terminal cursor is wherever the last redraw left it: on the row
    // matching cursor_pos_, which isn't necessarily the last row of the
    // input (left/right arrow can leave it mid-buffer). Move up to the
    // first row, clear every row the prefix+input currently occupies
    // on-screen, then return to the top so redrawPromptUnlocked() starts
    // from a clean slate.
    const size_t prefixLen = computePrefixLength();
    const RowCol cur = offsetToRowCol(prefixLen + cursor_pos_);
    const RowCol end = offsetToRowCol(prefixLen + input_buffer_.size());
    const size_t lines = end.row + 1;

    if (cur.row > 0) std::cout << "\033[" << cur.row << "A";
    std::cout << "\r";
    for (size_t i = 0; i < lines; ++i) {
        std::cout << "\033[K";
        if (i + 1 < lines) std::cout << "\033[1B\r";
    }
    if (lines > 1) std::cout << "\033[" << (lines - 1) << "A";
    std::cout << "\r" << std::flush;
}

void TerminalUI::redrawPromptUnlocked() const {
    if (!status_text_.empty()) {
        std::cout << "[" << status_text_ << "] ";
    }
    std::cout << prompt_str_ << input_buffer_;

    if (cursor_pos_ < input_buffer_.size()) {
        // Cursor isn't at the end (left/right arrow was used) - printing
        // above left the terminal cursor at the very end of the buffer, so
        // walk it back to where it logically belongs.
        const size_t prefixLen = computePrefixLength();
        const RowCol end = offsetToRowCol(prefixLen + input_buffer_.size());
        const RowCol cur = offsetToRowCol(prefixLen + cursor_pos_);
        if (end.row > cur.row) std::cout << "\033[" << (end.row - cur.row) << "A";
        std::cout << "\r";
        if (cur.col > 0) std::cout << "\033[" << cur.col << "C";
    }
    std::cout << std::flush;
}

void TerminalUI::animLoop(CallType type, std::string peer) {
    const std::vector<std::string> audio_frames = {
        "-_-", "-_(", "-((", "(((", "((-", "(_-"
    };
    const std::vector<std::string> video_frames = {
        "o_o", "_o ", "o  ", "  *", " **", "***", "** ", "*  ","  o", " o_"
    };

    const auto& frames = (type == CallType::AUDIO) ? audio_frames : video_frames;
    size_t idx = 0;
    (void)peer; // peer is already reflected in prompt_str_ via setMode(COMPANION, peer)

    while (running_animation_) {
        {
            std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
            clearInputAreaUnlocked();
            status_text_ = frames[idx];
            redrawPromptUnlocked();
        }
        idx = (idx + 1) % frames.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void TerminalUI::typingLoop(std::string peer) {
    static const std::array<std::string, 5> frames = {
        "  .", " ..", "...", ".. ", ".  "
    };
    size_t idx = 0;
    (void)peer; // peer is already reflected in prompt_str_ via setMode(COMPANION, peer)
    while (typing_running_) {
        {
            std::lock_guard<std::recursive_mutex> lock(ui_mutex_);
            clearInputAreaUnlocked();
            status_text_ = frames[idx];
            redrawPromptUnlocked();
        }
        idx = (idx + 1) % frames.size();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

// ============================================================================
// RawTerminalGuard
// ============================================================================
bool RawTerminalGuard::enable() {
    if (enabled_) return true;
    if (::tcgetattr(STDIN_FILENO, &orig_) != 0) return false;
    termios raw = orig_;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return false;
    enabled_ = true;
    return true;
}

void RawTerminalGuard::disable() {
    if (!enabled_) return;
    ::tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
    enabled_ = false;
}

bool RawTerminalGuard::enabled() const { return enabled_; }

void RawTerminalGuard::withCanonicalMode(const std::function<void()>& fn) {
    bool wasEnabled = enabled_;
    if (wasEnabled) disable();
    fn();
    if (wasEnabled) enable();
}

RawTerminalGuard::~RawTerminalGuard() { disable(); }
