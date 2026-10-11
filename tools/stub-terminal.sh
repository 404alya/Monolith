#!/bin/sh
# stub-terminal.sh -- a fake terminal for tests of the request launcher (src/launcher.c).
# Use it as MONOLITH_TERMINAL=tools/stub-terminal.sh: instead of opening a window and
# starting Claude Code it only writes the argv it was given, one argument per line, to
# $MONOLITH_STUB_LOG (if set) and exits. Nothing is started, nothing appears on screen.
if [ -n "$MONOLITH_STUB_LOG" ]; then
    printf '%s\n' "$@" > "$MONOLITH_STUB_LOG"
fi
exit 0
