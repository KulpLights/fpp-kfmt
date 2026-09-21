#!/bin/bash
set -e

# Nothing to undo. The install only runs make inside the plugin directory, and
# FPP removes that directory itself, so there are no services, timers, cron
# entries, symlinks or files outside the plugin to reverse. Clearing the build
# output keeps a later reinstall from starting on stale objects; make clean is
# a no-op if the build never ran, so this is safe to run twice.
cd "$(dirname "$0")/.."
make clean 2>/dev/null || true

exit 0
