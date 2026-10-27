#!/usr/bin/env python3
"""Validate R3D surface shaders (a package's shaders/*.glsl) with glslangValidator.

Usage: python3 -I .claude/skills/shaders/check_surface_shader.py <shader.glsl>...   (exit 1 if any fails)

A surface shader isn't a complete GLSL file. R3D's parser (submodules/r3d/src/common/r3d_rshade.h)
moves its uniforms into a block, turns `varying` into `out` (vertex) / `in` (fragment), and builds
each stage from the shared code plus only that stage's entry point, inside the templates
shaders/include/user/scene.vert / scene.frag. The checker does the same with stub declarations of
each stage's built-ins (types as in those templates), then compiles:
  vertex stage, fragment stage, fragment stage with DEPTH defined (the depth / pre-pass variant).
It catches syntax and type errors, a built-in used in the wrong stage, and names strict front
ends reject (NVIDIA accepts more than AMD / Intel). If R3D's templates change their built-ins,
update VERT_BUILTINS / FRAG_BUILTINS.
"""
import os, re, subprocess, sys, tempfile

VERT_BUILTINS = """
uniform mat4 MATRIX_MODEL, MATRIX_INV_VIEW, MATRIX_VIEW_PROJECTION; uniform mat3 MATRIX_NORMAL;
vec3 POSITION, EMISSION, NORMAL, INSTANCE_POSITION, INSTANCE_SCALE; vec2 TEXCOORD;
vec4 COLOR, TANGENT, INSTANCE_ROTATION, INSTANCE_COLOR, INSTANCE_CUSTOM;
int FRAME_INDEX; float TIME;
"""
FRAG_BUILTINS = """
in vec3 vPosition;
#define POSITION vPosition
vec2 TEXCOORD; vec3 TANGENT, BITANGENT, NORMAL;
vec3 ALBEDO, EMISSION, NORMAL_MAP; float ALPHA, OCCLUSION, ROUGHNESS, METALNESS, SPECULAR;
int FRAME_INDEX; float TIME;
out vec4 stubOut;
"""
VARYING = re.compile(r'^(\s*)((?:flat|smooth|noperspective)\s+)?varying\b')


def entry_spans(src):
    """(name, start, end) of `void vertex()` / `void fragment()`, skipping comments."""
    spans, i, n = [], 0, len(src)
    while i < n:
        if src.startswith('//', i):
            i = src.find('\n', i); i = n if i < 0 else i
        elif src.startswith('/*', i):
            i = src.find('*/', i); i = n if i < 0 else i + 2
        else:
            m = re.compile(r'void\s+(vertex|fragment)\s*\(\s*\)').match(src, i)
            if m and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == '_')):
                j, depth = src.index('{', m.end()), 0
                while True:
                    if src[j] == '{': depth += 1
                    elif src[j] == '}':
                        depth -= 1
                        if depth == 0: break
                    j += 1
                spans.append((m.group(1), i, j + 1))
                i = j + 1
            else:
                i += 1
    return spans


def stage_source(src, stage, defines=()):
    # Drop the other stage's entry point (newlines kept, so line numbers stay the source's)
    for name, a, b in reversed(entry_spans(src)):
        if name != stage:
            src = src[:a] + re.sub(r'[^\n]', ' ', src[a:b]) + src[b:]
    lines = []
    for line in src.split('\n'):
        if re.match(r'\s*#pragma\s', line):
            line = ''
        else:
            line = VARYING.sub(lambda m: m.group(1) + (m.group(2) or '') + ('out' if stage == 'vertex' else 'in'), line)
        lines.append(line)
    has_entry = any(name == stage for name, _, _ in entry_spans(src))
    head = '#version 330 core\n' + ''.join('#define %s\n' % d for d in defines) + (VERT_BUILTINS if stage == 'vertex' else FRAG_BUILTINS) + '#line 1\n'
    call = (stage + '();') if has_entry else ''
    tail = '\nvoid main() { %s %s }\n' % (call, 'gl_Position = vec4(POSITION, 1.0);' if stage == 'vertex'
                                          else 'stubOut = vec4(ALBEDO, ALPHA);')
    return head + '\n'.join(lines) + tail


def main(paths):
    status = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in paths:
            src = open(path, encoding='utf-8').read()
            errors = []
            for stage, ext, defines in (('vertex', 'vert', ()), ('fragment', 'frag', ()), ('fragment', 'frag', ('DEPTH',))):
                out = os.path.join(tmp, 'stub.' + ext)
                with open(out, 'w') as f:
                    f.write(stage_source(src, stage, defines))
                # (glslangValidator's -D needs -l, so the defines go into the source, as R3D does)
                r = subprocess.run(['glslangValidator', '-S', ext, out], capture_output=True, text=True)
                if r.returncode != 0:
                    label = stage + ''.join(' (%s)' % d for d in defines)
                    found = ['%s: %s' % (label, l.strip()) for l in (r.stdout + r.stderr).splitlines()
                             if l.startswith('ERROR') and 'compilation' not in l]
                    # anything else that failed (a usage error, a crash) is shown whole
                    errors += found or ['%s: exit %d: %s' % (label, r.returncode, (r.stdout + r.stderr).strip())]
            if errors:
                status = 1
                print('FAIL  ' + path)
                for e in errors:
                    print('      ' + e)
            else:
                print('ok    ' + path)
    return status


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))
