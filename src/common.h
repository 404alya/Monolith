// Shared includes, the current scene and process-wide data paths
#ifndef MONOLITH_COMMON_H
#define MONOLITH_COMMON_H

#include <r3d/r3d.h>
#include <raymath.h>
#include <rlgl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "monolith_scene.h"

// The editor edits and reaches the AI only on Linux (+ i3 / sway); everywhere else it is a viewer
// for benchmarking, viewing and debugging a map (the project-history skill, "The stated purpose").
// 0: the view-only build: the editing and request keys and flags are refused (the list: the
// header comment of src/main.c, "View-only build"). Checked with `if (!MONOLITH_EDITING)` at the
// entry points, so both builds compile every module; only launcher.c and classifier.c (POSIX) are
// left out of the other platforms' builds (nob.c); launcher.h gives stubs there.
#if defined(__linux__)
#   define MONOLITH_EDITING 1
#else
#   define MONOLITH_EDITING 0
#endif
// Text for people: static literals vs heap strings (MonoMsg, MonoStrf)
#include "msg.h"
// (raylib's keyboard / mouse calls go through the input shim: --input-script)
#include "input.h"

// The loaded map package (runtime/monolith_scene.h). Set by main() before the window
// opens; its contents change on hot reload (scene->version tells which load).
extern MS_Scene *scene;

// Data directory, with a trailing slash (see InitDataPaths in main.c)
extern char benchmarkDir[1024];

#endif
