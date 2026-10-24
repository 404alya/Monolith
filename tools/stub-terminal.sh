#!/bin/sh
# stub-terminal.sh -- a fake terminal for tests of the request launcher (src/launcher.c).
# Use it as MONOLITH_TERMINAL=tools/stub-terminal.sh: instead of opening a window and
# starting Claude Code it only writes the argv it was given, one argument per line, to
# $MONOLITH_STUB_LOG (if set) and exits. Nothing appears on screen.
#   MONOLITH_STUB_OPENED=<file>   append one line per "window" (time + pid), to count them
#   MONOLITH_STUB_RUN=<file>      run the command after -e (how the launcher passes it to an
#                                 unknown terminal) like a terminal would, output to <file>,
#                                 and stay until it exits. With MONOLITH_CLAUDE set to
#                                 tools/stub-coordinator.sh this is a coordinator without a
#                                 window and without Claude (tools/test-coordinator.sh)
if [ -n "$MONOLITH_STUB_LOG" ]; then
    printf '%s\n' "$@" > "$MONOLITH_STUB_LOG"
fi
if [ -n "$MONOLITH_STUB_OPENED" ]; then
    echo "$(date '+%H:%M:%S.%N') $$" >> "$MONOLITH_STUB_OPENED"
fi
if [ -n "$MONOLITH_STUB_RUN" ]; then
    while [ $# -gt 0 ] && [ "$1" != "-e" ]; do shift; done
    [ $# -gt 0 ] && shift
    exec "$@" >> "$MONOLITH_STUB_RUN" 2>&1 < /dev/null
fi
exit 0
