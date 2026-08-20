#pragma once

#include <functional>
#include <string>

#include "types.h"

// Loads ~/.config/jamcli/config.yaml (or $XDG_CONFIG_HOME/jamcli/config.yaml)
// into a JamcliConfig. Missing file / missing keys simply keep the defaults.
// `debugCb`, when set, receives human-readable notices about parsed-but-not-
// yet-applied settings.
JamcliConfig loadJamcliConfig(std::function<void(std::string)> debugCb = {});
