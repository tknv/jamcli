#include <cstring>
#include <iostream>
#include <string>

#include "../include/app.h"
#include "jamcli_version.h" // generated - see CMakeLists.txt / Makefile.standalone

namespace {

// --version: printed to stdout, exits 0, and - critically - runs before
// anything else (no libjami/app initialization). A version check should
// never block on, or fail because of, engine startup.
void printVersion() {
    std::cout << "jamcli " << JAMCLI_VERSION;
    if (std::strcmp(JAMCLI_GIT_COMMIT, "unknown") != 0) {
        std::cout << " (" << JAMCLI_GIT_COMMIT << ")";
    }
    std::cout << "\n";
}

void printUsage(const char* argv0) {
    std::cout
        << "Usage: " << argv0 << " [options]\n"
        << "\n"
        << "Options:\n"
        << "  --renderer <mode>   Video renderer to use mpv or raw to local socket and pass to remote SSH client (default: ffplay)\n"
        << "  --debug-log         Show libjami's internal console log\n"
        << "  --version            Print version information and exit\n"
        << "  --help               Print this help message and exit\n";
}

} // namespace

// ============================================================================
// Main
// ============================================================================
int main(int argc, char* argv[]) {
    std::string rendererMode = "ffplay";
    bool debugLog = false;

    // --version/--help take priority over every other flag and over each
    // other's position on the command line, and must never be delayed by
    // parsing/validating other options or by starting the engine.
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version") {
            printVersion();
            return 0;
        } else if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--renderer" && i + 1 < argc) {
            rendererMode = argv[++i];
        } else if (arg == "--debug-log") {
            // libjami's own internal logger writes straight to the console,
            // uncoordinated with jamcli's prompt/redraw locking - off by default
            // to keep the chat UI clean, pass --debug-log to get it back for
            // troubleshooting.
            debugLog = true;
        }
    }

    JamcliApp app;
    if (!app.init(rendererMode, debugLog)) {
        return 1;
    }
    app.run();
    return 0;
}
