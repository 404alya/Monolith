// --dump-scene: the whole loaded scene as stable text (dumpscene.c)
#ifndef MONOLITH_DUMPSCENE_H
#define MONOLITH_DUMPSCENE_H

#include "common.h"

// Writes boxes (hex floats, as --dump-boxes), model files, model instances (transform and
// world bounds as hex floats), model colliders, materials and lights of `s` to `path`.
// diff: without indices and map lines, references by name (--dump-scene-diff), so the
// dumps of a map before and after inserting lines diff to just the changed objects
bool DumpScene(const MS_Scene *s, const char *path, bool diff);

#endif
