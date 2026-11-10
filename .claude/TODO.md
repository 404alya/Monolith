# The AI's list: bugs and deferred work found by Claude sessions (tools/ai-todo.sh)
6472901156218498333 - bug: R3D contrast clips dark values to black Linear contrast after tonemap (output.frag Adjustments). docs/known-issues.md, Open decisions 1
13271338723792542802 - bug: R3D texture path join can overflow a 256-byte buffer r3d_importer_texture.c, unpatched; the loader guards it. docs/known-issues.md, Open decisions 2
16697682306116340562 - bug: assimp leaks ~1.4 KB on a short glTF buffer file In assimp's own glTF code, unpatched. docs/known-issues.md, Open decisions 3
15668286884236237955 - task: check FBX / DAE external references Not verified yet. docs/known-issues.md, Open decisions 4
13588000311843348956 - task: check symlinks in a package that point outside it Not verified yet. docs/known-issues.md, Open decisions 4
1142123145578853017 - idea: i3 / sway for_window rule for monolith-claude windows Avoids a flash before the move to its workspace. docs/known-issues.md, Open decisions 5
4029415151538202736 - task: decide the Quaternius licence policy Developer's call. docs/known-issues.md, Open decisions 5
18394907280734056012 - task: decide medium/low shadow map size, low render scale Graphics preset values. docs/known-issues.md, Open decisions 5
16849537629031576708 - idea: skip volumetric fog lookups in uniform air Bit-identical first, then a closed form for lit rays. project-history skill, Private maps and the weed pre-pass
16845012953862367618 - idea: compile the game per file, in parallel Incremental build is one serial compile (~11 s). project-history skill, The build program: nob.c
14071567816549104739 - task: remove the editor-only Windows code launcher.c _WIN32 branch, wincmd_test.c, --win run, %APPDATA% path. project-history, The stated purpose
