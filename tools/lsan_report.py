#!/usr/bin/env python3
"""lsan_report.py <lsan.supp> <log dir> -- classifies LeakSanitizer reports (tools/leakcheck.sh).

Reads every LeakSanitizer log in <log dir> (files named <run>.<pid>, written through
LSAN_OPTIONS=log_path=<log dir>/<run>) and sorts each leak into
  - "ours": the allocating frame is in the program (our code, raylib, R3D, assimp: all
    linked statically) or in anything not listed in lsan.supp, and
  - "system": the allocating frame is in a driver / system library listed in lsan.supp.
The allocating frame is the first stack frame after the allocator itself: malloc & co.
(frame #0), operator new, strdup and every libc frame are skipped, so memory that libc
allocates for us (fopen, strdup) still counts as ours. Prints one line per run and the
full stack of every leak of ours; exits 1 if there is any.
"""
import os
import re
import sys

# "#3 0x7f.. in func (/usr/lib/libX.so+0x12)" (no debug info), or symbolized with debug info:
# "#1 0x55.. in func /path/file.c:12:5" (always the program: only it is built with -g)
FRAME = re.compile(r"^\s*#(\d+) 0x[0-9a-f]+(?: in (.*?))?\s+\((.+?)\+0x[0-9a-f]+\)")
FRAME_SRC = re.compile(r"^\s*#(\d+) 0x[0-9a-f]+ in (\S+)(?: .*)?$")
HEADER = re.compile(r"^(Direct|Indirect) leak of (\d+) byte\(s\) in (\d+) object\(s\) allocated from:")
SKIP_FUNC = re.compile(r"^(operator new|operator new\[\]|__interceptor_|malloc|calloc|realloc|reallocarray|"
                       r"posix_memalign|aligned_alloc|memalign|valloc|strdup|strndup|__strdup|__strndup)")


def load_patterns(path):
    pats = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("leak:"):
                pats.append(line[5:].strip())
    return pats


def classify(frames, pats):
    """frames: list of (function or None, module). Returns ('ours'|'system', deciding frame)."""
    for func, module in frames[1:]:
        base = os.path.basename(module)
        if func and SKIP_FUNC.match(func):
            continue
        if base.startswith("libc.so") or base.startswith("libm.so"):
            continue
        if any(p in base for p in pats):
            return "system", (func, module)
        return "ours", (func, module)
    return "ours", frames[0] if frames else (None, "?")


def parse(path):
    leaks, cur = [], None
    with open(path, errors="replace") as f:
        for line in f:
            h = HEADER.match(line)
            if h:
                cur = {"kind": h.group(1), "bytes": int(h.group(2)), "objects": int(h.group(3)), "frames": [], "text": [line.rstrip()]}
                leaks.append(cur)
                continue
            if cur is None:
                continue
            m = FRAME.match(line)
            if m:
                cur["frames"].append((m.group(2), m.group(3)))
                cur["text"].append(line.rstrip())
                continue
            m = FRAME_SRC.match(line)
            if m:
                cur["frames"].append((m.group(2), "<program>"))
                cur["text"].append(line.rstrip())
            elif not line.strip():
                cur = None
    return leaks


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    pats = load_patterns(sys.argv[1])
    logdir = sys.argv[2]
    runs = {}
    for name in sorted(os.listdir(logdir)):
        m = re.match(r"^(.*)\.(\d+)$", name)
        if not m or name.endswith(".out"):
            continue
        runs.setdefault(m.group(1), []).extend(parse(os.path.join(logdir, name)))
    # runs.txt (written by leakcheck.sh): every run, so the clean ones are listed too
    order = []
    rt = os.path.join(logdir, "runs.txt")
    if os.path.exists(rt):
        with open(rt) as f:
            order = [l.split()[0] for l in f if l.strip()]
    for r in order:
        runs.setdefault(r, [])
    ours_total = 0
    details = []
    for run, leaks in sorted(runs.items(), key=lambda kv: order.index(kv[0]) if kv[0] in order else len(order)):
        ours = [l for l in leaks if classify(l["frames"], pats)[0] == "ours"]
        sysl = [l for l in leaks if classify(l["frames"], pats)[0] == "system"]
        ob = sum(l["bytes"] for l in ours)
        sb = sum(l["bytes"] for l in sysl)
        print(f"  {run:28s} ours: {len(ours):3d} leak(s) {ob:8d} B   driver/system (suppressed): {len(sysl):3d} ({sb} B)")
        ours_total += len(ours)
        for l in ours:
            details.append(f"--- {run}:\n" + "\n".join(l["text"]))
    if details:
        print("\nLeaks of ours (allocating frame outside the libraries in lsan.supp):")
        for d in details:
            print(d)
    return 1 if ours_total else 0


if __name__ == "__main__":
    sys.exit(main())
