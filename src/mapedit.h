// Manual map edits: remove, move, turn, duplicate and add objects as minimal text edits to the
// map file (mapedit.c), with undo / redo; the edit script of --apply-edits
#ifndef MONOLITH_MAPEDIT_H
#define MONOLITH_MAPEDIT_H

#include "common.h"

typedef enum { EDIT_MOVE, EDIT_ROTATE, EDIT_REMOVE, EDIT_DUPLICATE, EDIT_ADD } EditOpKind;

typedef struct {
    EditOpKind kind;
    double delta[3];        // EDIT_MOVE: the shift; EDIT_DUPLICATE: where the copies go (metres)
    double degrees;         // EDIT_ROTATE: yaw; models any angle, boxes multiples of 90
    const char *modelPath;  // EDIT_ADD: package-relative model file
    double pos[3];          // EDIT_ADD: where it stands (origin bottom)
    MS_ObjRef anchor;       // EDIT_ADD: the new line goes right after this object's line (its group)
} EditOp;

// One part of an edit: bytes [offset, offset + oldLen) of the file before it became newText
typedef struct {
    int offset;
    char *oldText, *newText;
    int oldLen, newLen;
} EditHunk;

// One manual edit, reversible: the hunks against the file as it was, the file hashes before
// and after, and the selection (entity ids) to restore on undo / redo
typedef struct {
    EditHunk *hunks;
    int hunkCount, hunkCap;
    unsigned long long hashBefore, hashAfter;
    MS_EntityId *selBefore, *selAfter;
    int selBeforeCount, selAfterCount, selAfterCap;
    char what[200];         // "moved 2 objects +0.5 m x (line 120 split into 3 lines)"
} EditStep;

typedef struct {
    EditStep *undo, *redo;  // heap stacks, top = last
    int undoCount, undoCap, redoCount, redoCap;
    unsigned long long knownHash;   // the map file the stacks belong to (FNV-1a 64)
} MapEditor;

typedef enum {
    EDIT_FAILED = 0,        // nothing written (msg says why)
    EDIT_DONE,              // written and reloaded; *sel / *selCount = the objects to select now
} EditResult;

// Applies op to the objects in sel[] (the selection; EDIT_ADD ignores it): the patch is made
// against the loaded map text, the file on disk must still be that text, it is written and
// reloaded (MS_Reload) and the edit goes onto the undo stack (the redo stack is dropped).
// A patch that would not load is never left on disk (reverted, EDIT_FAILED). On EDIT_DONE
// *outSel is a heap array of entity ids to select (free it), and msg says what changed.
EditResult MapEditApply(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op,
                        MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);
// Undo / redo the last step (only while the file is the one the step left / found)
EditResult MapEditUndo(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);
EditResult MapEditRedo(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);

// Call after every reload: when the map is no longer the file the stacks belong to (a request
// finished, another editor saved), the history is dropped: true then (msg says so)
bool MapEditSync(MapEditor *E, char *msg, int msgSize);
void MapEditFree(MapEditor *E);

// The world position an arrow step snaps from: box min corner, model / light position,
// rounded to 0.1 mm so that the float of a decimal coordinate gives back that decimal
void MapEditAnchor(MS_ObjRef o, double out[3]);

// A short description of where an object comes from: "line 120 (copy 3 of 8, id 7)"
void MapEditDescribe(MS_ObjRef o, char *out, int outSize);

// --apply-edits <script>: runs an edit script against the loaded scene (a noGpu load of
// --map), writing the map file after every edit. Returns the exit status (0 = every line
// worked). Format: the header comment of mapedit.c.
int MapEditRunScript(const char *scriptPath);

#endif
