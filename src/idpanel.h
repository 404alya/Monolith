// The id panel (F5, spectator mode): every explicit `id N` of the map with its line, kind,
// object count, model file or material and group; fly to and select, remove an id prefix,
// give one (idpanel.c; docs/live-editing.md 8.1)
#ifndef MONOLITH_IDPANEL_H
#define MONOLITH_IDPANEL_H

#include "common.h"
#include "designer.h"
#include "editmode.h"
#include "spectator.h"
#include "ui.h"

typedef struct {
    int id;                         // N of `id N`
    int line;                       // map line
    int stmt;                       // statement index (MS_Ref.prim)
    int firstEntity;                // its objects are entities firstEntity .. firstEntity + objects - 1
    char kind[16];                  // box primitive name, "model", "light" (bounded: a keyword)
    int objects;                    // objects the line makes
    const char *what;               // model file, material name or light type (scene strings, valid until the
                                    // next version; NULL = none)
    const char *group;              // group name, NULL = none
    BoundingBox bounds;             // of its objects
} IdEntry;

typedef struct {
    bool open;
    bool interactive;               // a real window: the panel frees / captures the cursor
    int freezeFrames;               // game keys and camera stay frozen this many frames after closing
    UiList list;
    IdEntry *items;                 // heap, rebuilt when scene->version (or the scene) changes
    int itemCount, itemCap;
    const MS_Scene *scene;          // the scene the items describe (NULL: rebuild; a renderer restart)
    unsigned int version;
    MS_ObjRef *objs;                // scratch: one entry's objects for an edit (heap, grows)
    int objCap;
    int visibleRows;                // rows that fit (UiListDraw), for PGUP / PGDN
} IdPanel;

void IdPanelOpen(IdPanel *p, const char *filter);   // F5, --id-panel [filter] (filter: NULL = none)
void IdPanelClose(IdPanel *p);
bool IdPanelCapturesInput(const IdPanel *p);        // open, or just closed
// Input while it captures it (F5 opening is main.c's; ESC / F5 close it). Edits (remove, give) go
// through the edit mode (live, undoable); ENTER flies the spectator to the objects, selects them
// and closes the panel. EDITMODE_RELOADED: the scene changed (re-apply the environment).
EditModeResult IdPanelUpdate(IdPanel *p, EditMode *m, Designer *d, Spectator *spec);
void IdPanelDraw(IdPanel *p, int screenW, int screenH);
void IdPanelFree(IdPanel *p);

#endif
