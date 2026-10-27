#!/usr/bin/env python3
"""Validate R3D's own shaders (submodules/r3d/shaders) with glslangValidator.

Usage: python3 -I .claude/skills/shaders/check_r3d_shaders.py [shader path relative to shaders/ ...]
       (no argument: every .vert / .frag; exit 1 if any fails)

R3D embeds its shaders at build time (CMake embed_shaders + scripts/glsl_processor.py, which
resolves #include <...> from shaders/include) and adds `#define` lines after `#version` when it
compiles them (src/modules/r3d_shader.c). This does the same: includes resolved with R3D's own
script, then each shader compiled once per define set R3D uses for it (VARIANTS below; anything
not listed gets the runtime limits only). The scene templates (scene/*) are compiled without
user code, as R3D does for a material without a surface shader. Surface shaders in a map
package: check_surface_shader.py. If r3d_shader.c gains a define or a variant, update VARIANTS.
"""
import os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.abspath(__file__))
R3D = os.path.normpath(os.path.join(ROOT, '../../../submodules/r3d'))
SHADERS = os.path.join(R3D, 'shaders')


def runtime_limits():
    """The UBO capacities R3D formats into MAX_* defines, from CMakeLists.txt. The probe limits go
    into every variant (only array sizes); MAX_LIGHTS_FORWARD only where R3D sets it (the forward
    programs, VARIANTS' 'LIGHTS'), because ubo/light.glsl switches to the forward light array on it."""
    caps = dict(re.findall(r'(R3D_SHADER_\w+_UBO_CAP)\s+(\d+)', open(os.path.join(R3D, 'CMakeLists.txt')).read()))
    return (['MAX_ILLUMINATION_PROBES ' + caps['R3D_SHADER_PROBE_ILLUMINATION_UBO_CAP'],
             'MAX_REFLECTION_PROBES ' + caps['R3D_SHADER_PROBE_REFLECTION_UBO_CAP']],
            'MAX_LIGHTS_FORWARD ' + caps['R3D_SHADER_LIGHT_FORWARD_UBO_CAP'])


QUALITY = [['QUALITY_PRESET %d' % q] for q in range(4)]
# Define sets per shader, as in r3d_shader.c's load_* functions (scene programs: VS / FS defines)
VARIANTS = {
    'prepare/pyramid.frag': [['PYRAMID'], ['DOWNSAMPLE']],
    'post/fxaa.frag': QUALITY,
    'post/smaa.vert': QUALITY, 'post/smaa.frag': QUALITY,
    'prepare/smaa_edge_detection.vert': QUALITY, 'prepare/smaa_edge_detection.frag': QUALITY,
    'prepare/smaa_blending_weigths.vert': QUALITY, 'prepare/smaa_blending_weigths.frag': QUALITY,
    'scene/scene.vert': [['STAGE_VERT', v] for v in ('GEOMETRY', 'UNLIT', 'DEPTH', 'DEPTH_CUBE', 'DECAL')]
                        + [['STAGE_VERT', 'FORWARD', 'LIGHTS'], ['STAGE_VERT', 'PROBE', 'PROBE_FORWARD', 'LIGHTS'],
                           ['STAGE_VERT', 'PROBE', 'PROBE_UNLIT']],
    'scene/geometry.frag': [['STAGE_FRAG', 'GEOMETRY'], ['STAGE_FRAG', 'GEOMETRY', 'GEOMETRY_PREPASS', 'R3D_NO_ALPHA_TEST']],
    'scene/forward.frag': [['STAGE_FRAG', 'FORWARD', 'LIGHTS'], ['STAGE_FRAG', 'PROBE', 'PROBE_FORWARD', 'LIGHTS']],
    'scene/unlit.frag': [['STAGE_FRAG', 'UNLIT'], ['STAGE_FRAG', 'PROBE', 'PROBE_UNLIT']],
    'scene/depth.frag': [['STAGE_FRAG', 'DEPTH']],
    'scene/depth_cube.frag': [['STAGE_FRAG', 'DEPTH_CUBE']],
    'scene/decal.frag': [['STAGE_FRAG', 'DECAL']],
}


def main(args):
    if args:
        rels = args
    else:
        rels = sorted(os.path.relpath(os.path.join(d, f), SHADERS)
                      for d, _, fs in os.walk(SHADERS) for f in fs
                      if f.endswith(('.vert', '.frag')) and '/include' not in d and '/external' not in d)
    limits, lights = runtime_limits()
    status = 0
    with tempfile.TemporaryDirectory() as tmp:
        for rel in rels:
            pre = subprocess.run([sys.executable, '-I', os.path.join(R3D, 'scripts/glsl_processor.py'),
                                  '-I', os.path.join(SHADERS, 'include'), os.path.join(SHADERS, rel)],
                                 capture_output=True, text=True)
            if pre.returncode != 0 or 'Include not found' in pre.stdout + pre.stderr:
                print('FAIL  %s\n      includes: %s' % (rel, (pre.stdout + pre.stderr).strip()[:300]))
                status = 1
                continue
            code, ext = pre.stdout, rel.rsplit('.', 1)[1]
            errors = []
            for defines in VARIANTS.get(rel, [[]]):
                body = re.sub(r'(#version[^\n]*\n)',
                              lambda m: m.group(1) + ''.join('#define %s\n' % (lights if d == 'LIGHTS' else d)
                                                             for d in limits + defines), code, count=1)
                out = os.path.join(tmp, 'shader.' + ext)
                with open(out, 'w') as f:
                    f.write(body)
                r = subprocess.run(['glslangValidator', '-S', ext, out], capture_output=True, text=True)
                if r.returncode != 0:
                    found = [l.strip() for l in (r.stdout + r.stderr).splitlines()
                             if l.startswith('ERROR') and 'compilation' not in l]
                    errors += ['[%s] %s' % (' '.join(defines) or 'no defines', e)
                               for e in found or ['exit %d: %s' % (r.returncode, (r.stdout + r.stderr).strip())]]
            if errors:
                status = 1
                print('FAIL  ' + rel)
                for e in errors[:6]:
                    print('      ' + e)
            else:
                print('ok    %s (%d variant%s)' % (rel, len(VARIANTS.get(rel, [[]])), '' if len(VARIANTS.get(rel, [[]])) == 1 else 's'))
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
