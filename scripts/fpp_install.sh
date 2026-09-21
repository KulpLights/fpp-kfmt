#!/bin/bash
set -e

# Build from the repo root, which is one level up now that this script lives in
# scripts/. Deriving it from $0 keeps the script runnable from any cwd.
cd "$(dirname "$0")/.."
make

# No restartFlag: the plugin declares FPP_PLUGIN_SUPPORTS_UNLOAD and the Plugin
# Manager asks fppd to load it as soon as this script finishes, so asking the
# user to restart would interrupt a running show for nothing.
