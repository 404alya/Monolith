// The groups panel (F6, spectator mode): every group of the map by name, sorted by area, with its
// object count, files and lines; select and fly to a group, rename, delete, duplicate it, make a new
// group from the selection or put the selection into one (groupspanel.c; docs/editor-tools.md 3.4)
#ifndef MONOLITH_GROUPSPANEL_H
#define MONOLITH_GROUPSPANEL_H

#include "common.h"
#include "designer.h"
#include "editmode.h"
#include "spectator.h"
#include "ui.h"

// One row: an area header (the text before ": " shared by several groups; its groups follow it) or
// a group by name (every `group` statement with that name)
typedef struct {
    bool header;                    // an area row
    const char *name;               // the group's name (a scene string, valid until the next version);
                                    // a header: the first of its groups' names (its area is its start)
    int areaLen;                    // the area's length in name (the text before ": "), 0: none
    bool inArea;                    // a group under a header row (drawn indented)
    int statements;                 // group statements with this name (a header: its groups)
    int objects;                    // objects (boxes, models, lights, barriers, ...)
    int firstFile, files;           // the map-text file of its first object (MS_MapFileAt), how many hold some
    int firstLine, lastLine;        // its objects' lines in firstFile (0: none)
    BoundingBox bounds;             // of its objects but lights (min > max: none)
    int first, count;               // a header: its groups are items first .. first + count - 1
} GroupEntry;

typedef struct {
    bool open;
    bool interactive;               // a real window: the panel frees / captures the cursor
    int freezeFrames;               // editor keys and camera stay frozen this many frames after closing
    UiList list;
    GroupEntry *items;              // heap, rebuilt when scene->version (or the scene) changes
    int itemCount, itemCap;
    const MS_Scene *scene;          // the scene the items describe (NULL: rebuild)
    unsigned int version;
    MS_ObjRef *objs;                // scratch: a row's objects for ENTER (heap, grows)
    int objCap;
    const char **names;             // scratch: a row's group names for an edit (heap, grows)
    int nameCap;
    int visibleRows;                // rows that fit (UiListDraw), for PGUP / PGDN
    // The text field: CTRL+R renames the row's group, CTRL+N names a new group for the selection
    int field;                      // 0 none, 1 rename, 2 new group
    char *text;                     // what is typed (heap)
    int textLen, textCap;
    char *fieldGroup;               // rename: the group's name as it was (heap)
    // DELETE asks for a second press within 3 s: the row (its name, and whether it is an area)
    char *armName;                  // heap; NULL: nothing armed
    bool armArea;
    float armTime;
} GroupsPanel;

void GroupsPanelOpen(GroupsPanel *p, const char *filter);   // F6, --groups-panel [filter] (NULL: none)
void GroupsPanelClose(GroupsPanel *p);
bool GroupsPanelCapturesInput(const GroupsPanel *p);         // open, or just closed
// Input while it captures it (F6 opening is main.c's; ESC / F6 close it, ESC first closes a text
// field). Edits go through the edit mode (live, undoable); ENTER flies the spectator to the group
// (an area row: all its groups), selects it and closes the panel. EDITMODE_RELOADED: the scene changed.
EditModeResult GroupsPanelUpdate(GroupsPanel *p, EditMode *m, Designer *d, Spectator *spec, float dt);
void GroupsPanelDraw(GroupsPanel *p, const Designer *d, int screenW, int screenH);
void GroupsPanelFree(GroupsPanel *p);

#endif
