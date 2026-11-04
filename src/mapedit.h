// Manual map edits: remove, move, turn, scale, duplicate and add objects as minimal text edits to the
// map file (mapedit.c), with undo / redo; the edit script of --apply-edits
#ifndef MONOLITH_MAPEDIT_H
#define MONOLITH_MAPEDIT_H

#include "common.h"

typedef enum {
    EDIT_MOVE, EDIT_ROTATE, EDIT_REMOVE, EDIT_DUPLICATE, EDIT_ADD, EDIT_ID,
    EDIT_SCALE,             // factor[] about pivot[] (boxes: corners; models: scale and position)
    EDIT_SIZE,              // boxes: size[] about the box's centre (0 = keep that axis)
    EDIT_ID_REMOVE,         // the selection's lines lose their `id N` prefix (objects stay)
    EDIT_SET_ROTATION,      // ypr[] absolute, each object about its own centre / origin (reset: 0 0 0;
                            // docs/live-editing.md 11.9)
    EDIT_MATERIAL_SET,      // material matName: the keys in matKeys replace the same keys on its first
                            // line or are added there; "-key" removes a key (uv_faces, filter, albedo_map,
                            // paint_filter)
    // The paint layer (docs/painting.md 4.2, 10.3), on the selection (one object; EDIT_PAINT_SET with
    // a kept rectangle: several). ADD and SET take their faces away from the object's other entries
    // in the same material (an entry left without faces goes), so a face that joins a group leaves
    // its old one
    EDIT_PAINT_ADD,         // a `paint` statement in the file's paint block (sorted by material, entity,
                            // face / slot; the block is appended under `# paint (draw mode)` when the
                            // file has none): paint.material (NULL: the model's own, `paint own`),
                            // paint.faces or paint.slot, paint.rect; the material gets `paint
                            // "<paint.atlas>"` (and `paint_filter nearest` with paint.nearest) when it has
                            // no paint key (the built-in "default" without a line: a material line at the
                            // top of the block); the line gets an id when it has none (nothing is split)
    EDIT_PAINT_SET,         // the object's entry in paint.material with the rectangle paint.rect, else the
                            // one holding the first face of paint.faces (or paint.slot), gets paint.faces
                            // and paint.rect (rect[2] <= 0: kept); a
                            // `paint own` entry also takes paint.filter (-1 kept, 0 trilinear, 1 nearest)
    EDIT_PAINT_REMOVE,      // the object's entries in paint.material lose the faces of paint.faces (an
                            // entry left without faces goes) or the entry of paint.slot goes (faces 0 and
                            // slot -1: every entry of the object in that material)
} EditOpKind;

// Faces of a paint entry (bits of EditOp.paint.faces): +x -x +y -y +z -z
#define EDIT_PAINT_ALL_FACES 0x3F

typedef struct {
    EditOpKind kind;
    double delta[3];        // EDIT_MOVE: the shift; EDIT_DUPLICATE: where the copies go (metres)
    double degrees;         // EDIT_ROTATE: yaw (any angle)
    // EDIT_ROTATE beyond a yaw (docs/live-editing.md 7.3, 7.5, 11.9): with axis[] all 0 the turn
    // is R = Ry(degrees) Rx(pitch) Rz(roll); else `angle` degrees about axis[] (a unit vector).
    // Global (the turn composes on the left of a model's or box's rotation, and with hasPivot the
    // objects orbit pivot[]) or local (on the right: about each object's own axes, each about its
    // own centre / origin). Without hasPivot every object turns about its own centre (boxes) /
    // origin (models). A box whose rotation ends up axis-aligned is written as plain corners
    // (no `rot`), else with `rot`. Only `degrees` set (no pivot, axis, pitch, roll, local) is the
    // plain `turn <yaw>`.
    double pitch, roll;
    double axis[3], angle;
    bool local, hasPivot;
    double pivot[3];        // EDIT_ROTATE, EDIT_SCALE (world; without hasPivot: each object's own centre)
    double factor[3];       // EDIT_SCALE: per world axis, > 0 (box corners, block centre + sizes, model
                            // position + `scale`: each model axis takes the factor of the world axis
                            // nearest to it; local: factor[i] scales the models' own axis i)
    double size[3];         // EDIT_SIZE: box / block size in metres about its centre, 0 = keep
    double ypr[3];          // EDIT_SET_ROTATION: yaw, pitch, roll in degrees (R = Ry Rx Rz)
    const char *modelPath;  // EDIT_ADD: package-relative model file
    double pos[3];          // EDIT_ADD: where it stands (origin bottom)
    MS_ObjRef anchor;       // EDIT_ADD: the new line goes right after this object's line (its group)
    int id;                 // EDIT_ID: the id the selected objects' line gets (1..2147483647, unique in
                            // the file; one line only); 0 = each line without an id gets the next free one
    const char *matName;    // EDIT_MATERIAL_SET: the material
    const char *matKeys;    // its keys as written, e.g. "paint_filter nearest" ("-key" removes one)
    struct {                // EDIT_PAINT_ADD / SET / REMOVE
        const char *material;   // the material's name; NULL: the model slot's own material
        const char *atlas;      // EDIT_PAINT_ADD: the atlas path the material gets if it has no paint key
        unsigned char faces;    // boxes: EDIT_PAINT_* face bits; models: 0
        int slot;               // models: the slot; boxes: -1
        int rect[4];            // x y w h, texels of the atlas
        bool nearest;           // EDIT_PAINT_ADD: paint_filter nearest (on the own entry, or with the
                                // material's new paint key)
        signed char filter;     // EDIT_PAINT_SET on own entries: -1 kept, 0 trilinear, 1 nearest
    } paint;
} EditOp;

// One part of an edit: bytes [offset, offset + oldLen) of the file before it became newText
typedef struct {
    int offset;
    char *oldText, *newText;
    int oldLen, newLen;
} EditHunk;

// A step's paint (docs/painting.md 10): the saved tiles of the strokes and the canvases they
// belong to. Owned by drawmode.c; mapedit.c only keeps it in the history and calls the hooks.
typedef struct PaintStep PaintStep;

// One manual edit, reversible: the hunks against the file as it was, the file hashes before
// and after, and the selection (entity ids) to restore on undo / redo; the op, so a rebase can
// apply it again to another text (op.modelPath then points into modelPath)
typedef struct {
    EditOp op;
    char *modelPath;
    MS_EntityId anchorId;   // EDIT_ADD: the entity of op.anchor (a rebase finds it by this)
    EditHunk *hunks;
    int hunkCount, hunkCap;
    unsigned long long hashBefore, hashAfter;
    MS_EntityId *selBefore, *selAfter;
    int selBeforeCount, selAfterCount, selAfterCap;
    char what[200];         // "moved 2 objects +0.5 m x (line 120 split into 3 lines)"
    char *matName, *matKeys;    // EDIT_MATERIAL_SET: op.matName / matKeys point here
    char *paintMaterial, *paintAtlas;   // EDIT_PAINT_*: op.paint.material / atlas point here
    PaintStep *paint;       // NULL for text edits; strokes (no hunks), or a first stroke (hunks + paint)
    // More paint ops amended onto this step (MapEditAmendPaint: a stroke that went on onto a new face,
    // the panel's face groups): its hunks are the text of all of them, a rebase applies op then these
    struct EditMore *more;
    int moreCount, moreCap;
} EditStep;

// One amended op of a step (its strings owned here; op.paint.material / atlas point at them)
typedef struct EditMore {
    EditOp op;
    char *material, *atlas;
} EditMore;

// What mapedit.c calls for a step with paint (drawmode.c sets them; NULL: no draw mode). undo /
// redo swap the step's tiles (or images) with the canvas; they run before the step's text goes
// live, so the canvas the text's material loads is provided by then (and the other one runs when
// the text can't go live). rebase runs before a rebase applies the step again (the canvas provided
// again, or its strokes replayed onto a PNG that changed on disk; false: the step failed, why in
// msg); release frees the paint when the step leaves the history; save writes the canvases and
// the map (CTRL+S anywhere and auto save: MapEditSaveAll).
typedef struct {
    void *user;
    bool (*undo)(void *user, PaintStep *p);
    bool (*redo)(void *user, PaintStep *p);
    bool (*rebase)(void *user, PaintStep *p, char *msg, int msgSize);
    void (*release)(void *user, PaintStep *p);
    long long (*bytes)(void *user, const PaintStep *p);    // its memory, for the history cap
    bool (*save)(void *user, char *msg, int msgSize);
} MapEditPaintHooks;

typedef struct {
    EditStep *undo, *redo;  // heap stacks, top = last
    int undoCount, undoCap, redoCount, redoCap;
    unsigned long long knownHash;   // the map file the stacks belong to (FNV-1a 64)
    // The working copy (docs/live-editing.md 2; phase 1 switches the edits over to it): the
    // map text the scene shows, the file's hash as last read or written, the undo depth that
    // matches the file (-1 = none), the steps a rebase could not apply again
    // The working copy exists once MapEditBegin ran (edit mode, --edit-script): from then on edits,
    // undo and redo change it and go live (MS_EditSetText) instead of writing the file. Without it
    // (--apply-edits) every edit writes the map file and reloads it.
    char *work;
    int workSize;
    unsigned long long workHash;    // FNV-1a 64 of work (= scene->hash while the scene shows it)
    unsigned long long diskHash;
    unsigned int loadVersion;       // scene->loadVersion the working copy belongs to
    int savedAt;            // the undo depth both the file and the working copy have
    EditStep *lost;         // steps the file has beyond savedAt that the working copy undid (a new edit
    int lostCount, lostCap; // dropped the redo stack holding them); they count as unsaved
    EditStep *failed;
    int failedCount, failedCap;
    bool autoSave;          // edit_save = auto: every edit, undo and redo is also written at once
    MS_EditStats lastStats; // what the last live edit / undo / redo cost (MS_EditSetText)
    MapEditPaintHooks paintHooks;   // the draw mode's (docs/painting.md 10.2)
    int droppedSteps;       // steps the paint history cap took from the bottom of the undo stack
    int droppedUnsaved;     // ... of them the file doesn't have (they count as unsaved until a save)
} MapEditor;

typedef enum {
    EDIT_FAILED = 0,        // nothing written (msg says why)
    EDIT_DONE,              // written and reloaded; *sel / *selCount = the objects to select now
} EditResult;

// Applies op to the objects in sel[] (the selection; EDIT_ADD ignores it): the patch is made
// against the loaded map text and the edit goes onto the undo stack (the redo stack is dropped).
// With a working copy (MapEditBegin) the patched text goes live (MS_EditSetText; auto save: also
// written); without one the file on disk must still be the loaded text, it is written and
// reloaded (MS_Reload). A patch that would not load is never applied (EDIT_FAILED). On EDIT_DONE
// *outSel is a heap array of entity ids to select (free it), and msg says what changed.
EditResult MapEditApply(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op,
                        MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);
// A paint step: the stroke's tiles (no text), or with op (EDIT_PAINT_ADD / SET / REMOVE /
// EDIT_MATERIAL_SET) the text edit and the paint as one step. paint is the history's from then on (even on
// EDIT_FAILED: released through the hooks). what: the HUD text ("painted 3 tiles of paint_12.png").
// Never auto-saved here (the draw mode saves at LMB up). The paint in the history is capped at 512
// MiB: beyond it the oldest steps are dropped (msg says so; MapEditor.droppedSteps counts them).
EditResult MapEditPushPaint(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op, PaintStep *paint,
                            const char *what, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);

// One more paint op onto the top step of the undo stack, which must be a paint step the working copy
// shows (a stroke going on onto a new face, the panel's face groups): the op goes live, the step's
// hunks become the text of both (one undo), and a rebase applies it after the step's own op.
// EDIT_FAILED (msg says why) leaves the step and the text as they were.
EditResult MapEditAmendPaint(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op, char *msg, int msgSize);

// Undo / redo the last step (only while the file, or the working copy, is the one the step left / found)
EditResult MapEditUndo(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);
EditResult MapEditRedo(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);

// Call after every reload: when the map is no longer the file the stacks belong to (a request
// finished, another editor saved), the history is dropped: true then (msg says so)
bool MapEditSync(MapEditor *E, char *msg, int msgSize);
void MapEditFree(MapEditor *E);

// The working copy (docs/live-editing.md): MapEditBegin takes the loaded map text as the
// working copy (after a load); MapEditSave writes it to the map file (MS_EditMarkSaved);
// MapEditUnsaved counts the steps the file doesn't have; MapEditRebase applies the unsaved
// steps again after the file changed on disk (returns how many failed, msg says which)
// (MapEditBegin drops the undo history; false for a compiled map or out of memory.) A rebase takes
// the reloaded file as the new base, applies each unsaved step's op again to its selection found
// by entity id (oldest first, each one live), puts the steps that no longer apply into failed[]
// and drops the history before the save point; steps undone past the save point can't be
// re-applied (they are reported as failed: the file keeps them). *outSel: the last step's
// selection (free it).
bool MapEditBegin(MapEditor *E);
bool MapEditSave(MapEditor *E, char *msg, int msgSize);
// CTRL+S: the draw mode's canvases and the map (paintHooks.save), else MapEditSave
bool MapEditSaveAll(MapEditor *E, char *msg, int msgSize);
// The paint memory of the history (undo, redo and lost steps)
long long MapEditPaintBytes(const MapEditor *E);
// The map file on disk is the one the working copy was last read from or written to
bool MapEditDiskUnchanged(const MapEditor *E);
int MapEditUnsaved(const MapEditor *E);
int MapEditRebase(MapEditor *E, MS_EntityId **outSel, int *outSelCount, char *msg, int msgSize);
void MapEditClearFailed(MapEditor *E);
// Writes the working copy to `path` (closing with unsaved edits)
bool MapEditWriteWork(const MapEditor *E, const char *path);
// "38.4 ms (parse 23.9, bvh 8.1, hidden 1.2, meshes 0.9 (3 of 324 rebuilt), ...)" for the log
void MapEditStatsText(const MS_EditStats *st, char *out, int outSize);

// The world position an arrow step snaps from: box min corner, model / light position,
// rounded to 0.1 mm so that the float of a decimal coordinate gives back that decimal
void MapEditAnchor(MS_ObjRef o, double out[3]);

// The object's entity id as the HUD shows it: "id 12:0" for a line with `id 12` (decimal, as
// written there and in `select id 12`), else "id a7d95ba7:0 (implicit)" (hex, as --dump-entities
// and `select entity` write it); "" when the object has no entity
void MapEditIdText(MS_ObjRef o, char *out, int outSize);
// The next free `id` of the loaded map file (the largest one + 1)
int MapEditNextId(void);

// A short description of where an object comes from: "line 120 (copy 3 of 8, id 7)"
void MapEditDescribe(MS_ObjRef o, char *out, int outSize);

// What made each object (its line's statement), for the transform modal: what it can do
typedef enum {
    EDIT_CLASS_BOX = 0, EDIT_CLASS_BLOCK, EDIT_CLASS_OTHER_BOX,  // `box`, `block`, stairs / waffle
    EDIT_CLASS_MODEL, EDIT_CLASS_LIGHT, EDIT_CLASS_DIR_LIGHT,     // a directional light has no position
    EDIT_CLASS_NONE,                                            // not from a line of the map text
} EditObjClass;
// classes[i] for objs[i] (each line read once; allocates only while reading the lines)
void MapEditClassify(const MS_ObjRef *objs, int count, unsigned char *classes);
// The transform an object of class cls (EditObjClass) gets from op (EDIT_MOVE, EDIT_ROTATE,
// EDIT_SET_ROTATION, EDIT_SCALE), with the same math MapEditApply writes the line with (before its
// rounding to the written decimals): the transform modal's preview. A box that becomes
// axis-aligned gets its permuted size and rotation 0, as written. False: it doesn't follow (*out = *in)
bool MapEditPreviewTransform(const EditOp *op, int cls, const MS_Transform *in, MS_Transform *out);

// --apply-edits <script>: runs an edit script against the loaded scene (a noGpu load of
// --map), writing the map file after every edit. Returns the exit status (0 = every line
// worked). Format: the header comment of mapedit.c.
int MapEditRunScript(const char *scriptPath);
// --edit-script <script>: the same language applied live to the loaded GPU scene (working copy,
// MS_EditSetText; nothing written unless the script says `save`), before --shot / --dump-* run.
// Also `save` and `disk <file>` (docs/live-editing.md 7.5). Returns the exit status.
int MapEditRunLiveScript(MapEditor *E, const char *scriptPath);

#endif
