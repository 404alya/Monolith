#!/usr/bin/env python3
# profile_report.py -- summarise a perf profile of monolith (tools/profile.sh runs it)
#
# Run with `python3 -I tools/profile_report.py <perf script output> [options]` (stdlib only).
# The input is `perf script -F comm,tid,time,period,ip,sym,symoff,dso` of a `perf record -e
# cycles:u --call-graph dwarf` run (the `benchmarking` skill, "CPU profiling"). Samples are
# weighted by their period (cycles): perf's frequency mode starts with periods of 1-1000
# cycles, which must not count like a steady-state sample.
#
#   --finish-site SYM+0xOFF  return address of the headless bench's glFinish() call (profile.sh
#                            reads it from objdump): driver time called from there is "glFinish"
#   --select REGEX           parts whose samples the detail tables cover (default ^frame$)
#   --frame-ms MS            the bench report's "CPU until finish" (All column): the "frame"
#                            part is scaled to it, so every row gets ms per frame
#   --frames N               frames the run drew (bench: views x (60 warmup + frames)); without
#                            --frame-ms (a bench whose frames are all reused has no CPU rows)
#                            the rows get kilocycles per frame instead
#   --top N                  rows per table (default 25)
#   --root SYM               also print the call tree under SYM (repeatable, e.g. R3D_End)
#   --depth N                depth of those trees (default 3)
#   --folded FILE            also write folded stacks (root;...;leaf cycles) of the selected
#                            samples, the input of flame-graph viewers
#   --binary PATH --sites N  for the N functions of PATH (the profiled monolith) that spend the
#                            most driver time: the driver time per call site, as file:line
#                            (addr2line, with the inline chain)
#
# Parts: a sample inside MS_Load / MS_Reload goes to the first of LOAD_PARTS whose pattern matches
# a frame of its stack ("load: x" / "reload: x"), any other to the first of PARTS; what nothing
# matches is "frame". "wait" is a sleep or yield (glFinish's wait under __GL_YIELD=USLEEP), "kernel" a
# sample whose IP is a kernel address (cycles:u still lands there on AMD at syscall entry and
# exit, as skid). Driver code has no symbols: its frames are named after the DSO
# ("[libnvidia-glcore]").
import argparse, collections, os, re, subprocess, sys

WAIT_RE = re.compile(r'^(usleep|__nanosleep|clock_nanosleep|nanosleep|sched_yield|__sched_yield|'
                     r'poll|__poll|pthread_cond_wait|___pthread_cond_wait|__futex_abstimed_wait\w*)$')
# (part, pattern), most specific first; matched against every frame of a stack
LOAD_PARTS = [
    ('parse', r'ParseScene|ReadCompiled|ReadWholeFile|InitData'),
    ('models (assimp)', r'LoadModelFiles|CheckModelFiles|aiImport\w*|Assimp::\w*'),
    ('resolve models', r'ResolveModels'),
    ('collider BVH', r'BuildColliderBvh'),
    ('geometry', r'FinishParse|FinishGeometry'),
    ('entities', r'BuildEntities'),
    ('hidden faces', r'FindHiddenFaces'),
    ('box meshes', r'UploadBoxes'),
    ('surface shaders', r'R3D_LoadSurfaceShader'),
    ('textures', r'GetTexture'),
    ('sky + ambient', r'R3D_GenProceduralSky|R3D_GenAmbientMap|R3D_LoadCubemap'),
    ('model groups', r'BuildModelGroups'),
    ('GPU other', r'UploadGpu'),
    ('shadow maps', r'R3D_LoadShadowMap'),
    ('free old map', r'FreeData'),
    ('watch + hash', r'SetWatch|CombinedHash|Fnv1a64Update|GetFileModTime'),
]
PARTS = [
    ('init', r'InitWindow|R3D_Init|MS_InitRenderer|_dl_start|_dl_init|dl_main|_dl_start_user'),
    ('exit', r'CloseWindow|R3D_Close|FreeMainHeap|__run_exit_handlers|_dl_fini'),
    ('cache: AO volume', r'BakeOcclusionVolume'),
    ('cache: fog sun visibility', r'R3D_BakeFogSunVisibility'),
    # R3D compiles a shader variant on its first use, in the first frame that draws with it
    ('lazy shader compile', r'load_shader|link_shader|compile_shader|r3d_shader_load_\w+'),
]

def ShortDso(dso):
    base = os.path.basename(dso)
    if dso.startswith('[') or base == '[unknown]': return dso
    return re.sub(r'\.so[.\d]*$', '', base)

def Samples(path):
    """Yields (period, frames), frames leaf first as (sym, dso, symoff); inlined frames get the
    DSO of the function they were inlined into."""
    head = re.compile(r'^\s*(\S+)\s+(\d+)\s+([\d.]+):\s+(\d+)\s*$')
    frame = re.compile(r'^\s+([0-9a-f]+)\s+(.*?)\s+\((.*)\)\s*$')
    def Done(period, frames):
        for i in range(len(frames) - 2, -1, -1):
            if frames[i][3]: frames[i][1] = frames[i + 1][1]
        return period, [(s, d, o) for s, d, o, _ in frames]
    period, frames = None, []
    with open(path, errors='replace') as f:
        for line in f:
            m = head.match(line)
            if m:
                if period is not None: yield Done(period, frames)
                period, frames = int(m.group(4)), []
                continue
            m = frame.match(line)
            if m and period is not None:
                ip, sym, dso = m.groups()
                inlined = sym.endswith(' (inlined)') or dso == 'inlined'
                sym = sym.replace(' (inlined)', '')
                off = re.search(r'\+(0x[0-9a-f]+)$', sym)
                sym = sym[:off.start()] if off else sym
                d = '[kernel]' if int(ip, 16) >= 0xffff800000000000 else ShortDso(dso)
                if sym == '[unknown]': sym = '[%s]' % d
                frames.append([sym, d, int(off.group(1), 16) if off else None, inlined])
    if period is not None: yield Done(period, frames)

def Driver(d): return d.startswith(('libnvidia', 'libGL'))

def PrintSites(a, drvBy, drvSite, sel, Fmt):
    """Driver time per call site of the top --sites callers, resolved with nm + addr2line."""
    addr = {}
    for line in subprocess.run(['nm', a.binary], capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'tT': addr.setdefault(f[2], int(f[0], 16))
    callers = [c for c, _ in drvBy.most_common() if c in addr][:a.sites]
    sites = [(c, o) for (c, o) in drvSite if c in callers]
    # perf prints a caller frame at (return address - 1), inside the call instruction
    pcs = ['%x' % (addr[c] + o) for c, o in sites]
    out = subprocess.run(['addr2line', '-e', a.binary, '-f', '-i', '-a'] + pcs, capture_output=True, text=True).stdout
    where, cur = {}, None
    lines = out.splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith('0x'):
            cur = int(lines[i], 16); where[cur] = []; i += 1; continue
        fn, loc = lines[i], lines[i + 1] if i + 1 < len(lines) else '?'
        where[cur].append('%s %s' % (fn, loc.split(' (')[0].replace(os.getcwd() + '/', '')))
        i += 2
    for c in callers:
        print('\n## Driver time by call site in %s (share of the selection)' % c)
        rows = sorted(((v, o) for (cc, o), v in drvSite.items() if cc == c), reverse=True)
        for v, o in rows[:12]:
            if v < 0.002 * sel: break
            chain = where.get(addr[c] + o, ['?'])
            print('  %s  +0x%x  %s' % (Fmt(v, sel), o, '  <  '.join(chain[:3])))

def main():
    ap = argparse.ArgumentParser(description='Summarise a perf profile of monolith (see the header comment).')
    ap.add_argument('script')
    ap.add_argument('--finish-site')
    ap.add_argument('--select', default='^frame$')
    ap.add_argument('--frame-ms', type=float)
    ap.add_argument('--frames', type=int)
    ap.add_argument('--top', type=int, default=25)
    ap.add_argument('--root', action='append', default=[])
    ap.add_argument('--depth', type=int, default=3)
    ap.add_argument('--folded')
    ap.add_argument('--binary')
    ap.add_argument('--sites', type=int, default=0)
    a = ap.parse_args()
    finish = None
    if a.finish_site:
        fs, fo = a.finish_site.split('+')
        finish = (fs, int(fo, 16))
    parts = [(n, re.compile(r'^(%s)$' % p)) for n, p in PARTS]
    loadParts = [(n, re.compile(r'^(%s)$' % p)) for n, p in LOAD_PARTS]
    select = re.compile(a.select)

    total = 0
    partTotal = collections.Counter()
    bucket = collections.Counter()
    dsoSelf = collections.Counter()
    symSelf = collections.Counter()
    symIncl = collections.Counter()
    drvBy = collections.Counter()
    drvSite = collections.Counter()   # (caller, symoff) -> driver time
    trees = {r: collections.Counter() for r in a.root}
    rootTotal = collections.Counter()
    folded = collections.Counter() if a.folded else None

    for period, frames in Samples(a.script):
        if not frames: continue
        total += period
        syms = [s for s, _, _ in frames]
        leafSym, leafDso, _ = frames[0]
        # the first frame outside the driver and the GL dispatch libraries
        caller = next((f for f in frames if not Driver(f[1])), None)
        part = None
        if any(WAIT_RE.match(s) for s in syms): part = 'wait'
        elif finish and any(Driver(d) for _, d, _ in frames):
            # glFinish: the first frame of ours is the finish site (the driver polls the clock
            # through the vdso and libc while it waits, so those frames come first)
            own = next((f for f in frames if f[1] == 'monolith'), None)
            if own and own[0] == finish[0] and own[2] is not None and abs(own[2] - finish[1]) <= 1: part = 'glFinish'
        loader = 'reload' if 'MS_Reload' in syms else 'load' if ('MS_Load' in syms or 'RestartRenderer' in syms) else None
        if part is None and not loader:
            part = next((n for n, r in parts if any(r.match(s) for s in syms)), None)
        if part is None:
            if loader:
                sub = next((n for n, r in loadParts if any(r.match(s) for s in syms)), 'other')
                part = '%s: %s' % (loader, sub)
        if part is None: part = 'kernel' if leafDso == '[kernel]' else 'frame'
        partTotal[part] += period
        if not select.search(part): continue
        bucket[part] += period
        if folded is not None: folded[';'.join(reversed(syms))] += period
        dsoSelf[leafDso] += period
        symSelf[(leafSym, leafDso)] += period
        for s in set(syms): symIncl[s] += period
        if Driver(leafDso):
            drvBy[caller[0] if caller else '(stack ends in the driver)'] += period
            if caller and caller[2] is not None and caller[1] == 'monolith': drvSite[(caller[0], caller[2])] += period
        for r in a.root:
            if r in syms:
                i = len(syms) - 1 - syms[::-1].index(r)   # outermost occurrence
                path = tuple(reversed(syms[max(0, i - a.depth):i]))
                rootTotal[r] += period
                for k in range(1, len(path) + 1): trees[r][path[:k]] += period

    if not total: sys.exit('profile_report: no samples')
    sel = sum(bucket.values())
    # ms per frame: the "frame" part is the bench's "CPU until finish"
    msPerCycle = None
    if a.frame_ms and a.frames and partTotal['frame']:
        msPerCycle = a.frame_ms * a.frames / partTotal['frame']

    def Fmt(v, base):
        s = '%6.2f%%  %7.3f Gc' % (100.0 * v / base, v / 1e9)
        if msPerCycle: s += '  %7.4f ms/frame' % (v * msPerCycle / a.frames)
        elif a.frames: s += '  %7.1f kc/frame' % (v / 1e3 / a.frames)
        return s

    print('# profile_report: %s' % os.path.basename(a.script))
    print('\n## Parts (share of every sample; Gc = 10^9 cycles)')
    for k, v in sorted(partTotal.items(), key=lambda kv: -kv[1]):
        print('  %-34s %s' % (k, Fmt(v, total)))
    if not sel: sys.exit('profile_report: no samples in the parts matching %s' % a.select)
    print('\n## Selected: parts matching %s, %s of every sample' % (a.select, Fmt(sel, total).strip()))
    print('\n## By DSO (self, share of the selection)')
    for k, v in dsoSelf.most_common(): print('  %-34s %s' % (k, Fmt(v, sel)))
    if drvBy:
        print('\n## Driver self time by the function that called into it (share of the selection)')
        for k, v in drvBy.most_common(a.top): print('  %s  %s' % (Fmt(v, sel), k))
    if a.binary and a.sites and drvSite:
        PrintSites(a, drvBy, drvSite, sel, Fmt)
    print('\n## Top %d symbols by self time (share of the selection)' % a.top)
    for (s, d), v in symSelf.most_common(a.top): print('  %s  %-46s %s' % (Fmt(v, sel), s[:46], d))
    print('\n## Top %d symbols by inclusive time (share of the selection)' % a.top)
    for s, v in symIncl.most_common(a.top): print('  %s  %s' % (Fmt(v, sel), s))
    for r in a.root:
        print('\n## Call tree under %s (inclusive, share of the selection; %s)' % (r, Fmt(rootTotal[r], sel).strip()))
        t = trees[r]
        def Walk(prefix):
            kids = sorted((p for p in t if len(p) == len(prefix) + 1 and p[:len(prefix)] == prefix), key=lambda p: -t[p])
            for p in kids:
                if t[p] < 0.003 * sel: continue
                print('  %s  %s%s' % (Fmt(t[p], sel), '  ' * len(prefix), p[-1]))
                Walk(p)
        Walk(())
    if folded is not None:
        with open(a.folded, 'w') as f:
            for k, v in folded.most_common(): f.write('%s %d\n' % (k, v))

if __name__ == '__main__':
    main()
