// --tri-report: where a map's triangles are, and what the draw distance leaves out (trireport.c)
#ifndef MONOLITH_TRIREPORT_H
#define MONOLITH_TRIREPORT_H

#include "common.h"

// Writes the triangle report of the uploaded scene `s` to `path`: totals, per viewpoint the
// triangles within their draw distance at `detailDistance` (0 = everything), then groups,
// materials, map lines and draw ranges, most triangles first
bool WriteTriReport(const MS_Scene *s, const char *path, float detailDistance);

#endif
