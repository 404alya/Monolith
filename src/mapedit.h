// Manual map edits: remove, move, turn, scale, duplicate and add objects, and whole groups (rename, delete, new,
// move, duplicate) as minimal text edits to the map's text (mapedit.c; each edit in the map-text file that holds the
// line: the main file or a part it imports), with undo / redo; the edit script of --apply-edits
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
    EDIT_MOVE_TO_FILE,      // the selected objects' whole lines (a `repeat` copy split out first) move to
                            // map-text file filePath, with the `group` / `use` they had (docs/code/mapedit.md
                            // "move-to-file"; the edit script only)
    EDIT_BARRIER_ADD,       // a new line `id N barrier x0 y0 z0 x1 y1 z1 [rot y p r]` (corners[], with
                            // hasRot ypr[]): after anchor's line (its file and group), or with anchor kind
                            // MS_OBJ_NONE at the end of the main map file's `# barriers` block (made at its
                            // end, with `group "barriers"`, when it has none); the new barrier is selected
    // Shapes and groups as units (docs/editor-tools.md 2.7, 3.2)
    EDIT_SHAPE_ADD,         // a new line `id N shape <shapeKind> pos[0] pos[1] pos[2] size[0..2]` after
                            // anchor's line (its group; the `use "default"` wrap as EDIT_ADD), or with
                            // anchor kind MS_OBJ_NONE at the end of the main map file; the new shape is selected
    EDIT_GROUP_RENAME,      // every `group "<groupName>"` line in every file becomes `group "<groupNew>"`
    EDIT_GROUP_DELETE,      // every object line of group groupName, its `group` lines, the comments right
                            // above deleted lines in its blocks, `use` lines left without objects, the
                            // deleted entities' paint entries
    EDIT_GROUP_NEW,         // the selection's whole lines into a new block `group "<groupNew>"` at the end of
                            // the file that holds them (move-to-file's scope rules)
    EDIT_GROUP_MOVE,        // the selection's whole lines to the end of group groupName's last block
    EDIT_GROUP_DUPLICATE,   // a copy of every object line of group groupName, shifted by delta[], in a new
                            // block `group "<groupNew>"` right after its last block
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
    double ypr[3];          // EDIT_SET_ROTATION (EDIT_BARRIER_ADD with hasRot): yaw, pitch, roll in degrees
                            // (R = Ry Rx Rz)
    const char *modelPath;  // EDIT_ADD: package-relative model file
    const char *filePath;   // EDIT_MOVE_TO_FILE: the package-relative map-text file the lines go to
    double pos[3];          // EDIT_ADD: where it stands (origin bottom)
    MS_ObjRef anchor;       // EDIT_ADD, EDIT_BARRIER_ADD: the new line goes right after this object's line
                            // (its group); EDIT_BARRIER_ADD with kind MS_OBJ_NONE: the main file's `# barriers`
    double corners[6];      // EDIT_BARRIER_ADD: x0 y0 z0 x1 y1 z1 (world; written with up to 4 decimals)
    bool hasRot;            // EDIT_BARRIER_ADD: `rot ypr[0] ypr[1] ypr[2]` too
    int shapeKind;          // EDIT_SHAPE_ADD: an MS_ShapeKind (pos[] = cx y0 cz, size[] = sx sy sz)
    const char *groupName;  // EDIT_GROUP_*: the group (by name, every statement of it)
    const char *groupNew;   // EDIT_GROUP_RENAME / NEW / DUPLICATE: the new name
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

// One part of an edit: bytes [offset, offset + oldLen) of map-text file `path` before it became
// newText. A step's hunks of one file are next to each other, in ascending order. created: the step
// made the file (move-to-file into a new part: offset 0, nothing old); undone, the working copy no
// longer has it (a file on disk stays: nothing is deleted)
typedef struct {
    char *path;             // package-relative, as MS_MapFile.path ("forest.map", "parts/lake.map")
    int offset;
    char *oldText, *newText;
    int oldLen, newLen;
    unsigned long long fileBefore, fileAfter;   // FNV-1a 64 of the whole file before / after the step
    bool created;
} EditHunk;

// A step's paint (docs/painting.md 10): the saved tiles of the strokes and the canvases they
// belong to. Owned by drawmode.c; mapedit.c only keeps it in the history and calls the hooks.
typedef struct PaintStep PaintStep;

// One manual edit, reversible: the hunks against the files as they were, the map text's hashes
// (scene->hash) before and after, and the selection (entity ids) to restore on undo / redo; the op,
// so a rebase can apply it again to another text (op.modelPath then points into modelPath, op.filePath
// into filePath)
typedef struct {
    EditOp op;
    char *modelPath, *filePath;
    MS_EntityId anchorId;   // EDIT_ADD, EDIT_BARRIER_ADD: the entity of op.anchor (a rebase finds it by this;
                            // stmt 0: none, EDIT_BARRIER_ADD's end of the main file)
    EditHunk *hunks;
    int hunkCount, hunkCap;
    unsigned long long hashBefore, hashAfter;
    MS_EntityId *selBefore, *selAfter;
    int selBeforeCount, selAfterCount, selAfterCap;
    char *what;             // "moved 2 objects +0.5 m x (line 120 split into 3 lines)" (heap, owned by the step)
    char *matName, *matKeys;    // EDIT_MATERIAL_SET: op.matName / matKeys point here
    char *paintMaterial, *paintAtlas;   // EDIT_PAINT_*: op.paint.material / atlas point here
    char *groupName, *groupNew;         // EDIT_GROUP_*: op.groupName / groupNew point here
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
// again, or its strokes replayed onto a PNG that changed on disk): 0 provided, 1 the step failed
// (why in MS_LastError()); release frees the paint when the step leaves the history; save writes
// the canvases and the map (CTRL+S anywhere and auto save: MapEditSaveAll) and returns
// MapEditSave's values with the same texts (0 / 1: MapEditor.note, 2: MS_LastError()).
typedef struct {
    void *user;
    bool (*undo)(void *user, PaintStep *p);
    bool (*redo)(void *user, PaintStep *p);
    unsigned char (*rebase)(void *user, PaintStep *p);
    void (*release)(void *user, PaintStep *p);
    long long (*bytes)(void *user, const PaintStep *p);    // its memory, for the history cap
    unsigned char (*save)(void *user);
} MapEditPaintHooks;

// One map-text file of the working copy: its text as the edits left it, and the file on disk as it
// was last read or written. Kept by package-relative path (the scene's file order can change after
// an edit); files[0] is the main map file.
typedef struct {
    char *path;                     // package-relative (MS_MapFile.path)
    char *text;                     // the working text (heap, NUL-terminated); NULL: the working copy
    int size;                       //   doesn't have the file (a new part whose move was undone)
    unsigned long long hash;        // FNV-1a 64 of text
    unsigned long long diskHash;    // of the file on disk when last read or written
    bool onDisk;                    // the file was on disk then (false: a new part, not saved yet)
} MapEditFile;

typedef struct {
    EditStep *undo, *redo;  // heap stacks, top = last
    int undoCount, undoCap, redoCount, redoCap;
    unsigned long long knownHash;   // the map text the stacks belong to (scene->hash)
    // The working copy (docs/live-editing.md 2): the map text the scene shows, one entry per
    // map-text file (files[]), the undo depth that matches the files on disk (-1 = none), the steps
    // a rebase could not apply again. It exists once MapEditBegin ran (edit mode, --edit-script):
    // from then on edits, undo and redo change it and go live (MS_EditSetText) instead of writing
    // the files. Without it (--apply-edits) every edit writes the files it changes and reloads.
    MapEditFile *files;
    int fileCount, fileCap;
    unsigned long long workHash;    // scene->hash while the scene shows the working copy
    unsigned int loadVersion;       // scene->loadVersion the working copy belongs to
    int savedAt;            // the undo depth both the files and the working copy have
    EditStep *lost;         // steps the file has beyond savedAt that the working copy undid (a new edit
    int lostCount, lostCap; // dropped the redo stack holding them); they count as unsaved
    EditStep *failed;
    int failedCount, failedCap;
    bool autoSave;          // edit_save = auto: every edit, undo and redo is also written at once
    MS_EditStats lastStats; // what the last live edit / undo / redo cost (MS_EditSetText)
    MapEditPaintHooks paintHooks;   // the draw mode's (docs/painting.md 10.2)
    int droppedSteps;       // steps the paint history cap took from the bottom of the undo stack
    int droppedUnsaved;     // ... of them the file doesn't have (they count as unsaved until a save)
    MonoMsg note;           // what the last call did when it did not fail (MapEditNote); failures
                            // are in MS_LastError() (docs/architecture.md "Errors and reports")
} MapEditor;

// The text of the last outcome that was no failure ("saved 3 edits to ...", the step's text); ""
// before the first. Valid until the next MapEdit* call.
const char *MapEditNote(const MapEditor *E);

// Applies op to the objects in sel[] (the selection; EDIT_ADD ignores it): the patch is made
// against the loaded map text and the edit goes onto the undo stack (the redo stack is dropped).
// With a working copy (MapEditBegin) the patched text goes live (MS_EditSetText; auto save: also
// written); without one the file on disk must still be the loaded text, it is written and
// reloaded (MS_Reload). A patch that would not load is never applied. Returns
//   0 applied: the note is the step's text
//   1 not applied: why in MS_LastError(); *outSel untouched (NULL)
//   2 applied, but the auto save failed: the note is the step's text + " (auto save: <reason>)"
// On 0 and 2 *outSel is a heap array of entity ids to select (free it).
unsigned char MapEditApply(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op,
                           MS_EntityId **outSel, int *outSelCount);
// A paint step: the stroke's tiles (no text), or with op (EDIT_PAINT_ADD / SET / REMOVE /
// EDIT_MATERIAL_SET) the text edit and the paint as one step. paint (never NULL) is the history's from then on (even
// when not pushed: released through the hooks). what: the HUD text ("painted 3 tiles of paint_12.png").
// Never auto-saved here (the draw mode saves at LMB up). The paint in the history is capped at 512
// MiB: beyond it the oldest steps are dropped (MapEditor.droppedSteps counts them). Returns
//   0 pushed: the note is the text, with " (oldest %d undo step%s dropped: ...)" when the cap dropped steps
//   1 not pushed, the paint released: why in MS_LastError()
unsigned char MapEditPushPaint(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op, PaintStep *paint,
                               const char *what, MS_EntityId **outSel, int *outSelCount);

// One more paint op onto the top step of the undo stack, which must be a paint step the working copy
// shows (a stroke going on onto a new face, the panel's face groups): the op goes live, the step's
// hunks become the text of both (one undo), and a rebase applies it after the step's own op. Returns
//   0 amended: the note is the op's text
//   1 not amended (the step and the text as they were): why in MS_LastError()
unsigned char MapEditAmendPaint(MapEditor *E, const MS_ObjRef *sel, int selCount, const EditOp *op);

// Undo / redo the last step (only while the file, or the working copy, is the one the step left / found).
// Returns
//   0 done: the note is "undone: <text>" / "redone: <text>"
//   1 not done: why in MS_LastError() ("nothing to undo", "undo does not apply to the map any more", ...)
//   2 done, but the auto save failed: the note + " (auto save: <reason>)"
// On 0 and 2 *outSel is the selection to restore (heap, free it).
unsigned char MapEditUndo(MapEditor *E, MS_EntityId **outSel, int *outSelCount);
unsigned char MapEditRedo(MapEditor *E, MS_EntityId **outSel, int *outSelCount);

// Frees the history, the working copy and the note
void MapEditFree(MapEditor *E);

// The working copy exists (MapEditBegin ran since the last load)
bool MapEditHasWork(const MapEditor *E);
// The scene shows the working copy again (MS_EditSetText with every file's working text: after a
// renderer restart, or to load provided textures again); E->workHash follows. MS_RELOADED or
// MS_RELOAD_FAILED (MS_LastError())
MS_ReloadResult MapEditShowWork(MapEditor *E);
// The scene's map-text files are the files on disk as the working copy last read or wrote them
bool MapEditSceneOnDisk(const MapEditor *E);
// The package path of the scene's map-text file `file` ("parts/lake.map"), "" for the main file (and
// out of range). A line is named the way dumps and the edit script name it ("line 41", "line
// parts/lake.map:41") with "%s%s%d" and MAPEDIT_LINE_AT(file, line)
const char *MapEditFilePath(int file);
#define MAPEDIT_LINE_AT(file, line) MapEditFilePath(file), MapEditFilePath(file)[0] ? ":" : "", (line)

// The working copy (docs/live-editing.md): MapEditBegin takes the loaded map text as the
// working copy (after a load); MapEditSave writes it to the map file (MS_EditMarkSaved);
// MapEditUnsaved counts the steps the file doesn't have; MapEditRebase applies the unsaved
// steps again after the file changed on disk
// (MapEditBegin drops the undo history; false for a compiled map or out of memory.) A rebase takes
// the reloaded file as the new base, applies each unsaved step's op again to its selection found
// by entity id (oldest first, each one live), puts the steps that no longer apply into failed[]
// and drops the history before the save point; steps undone past the save point can't be
// re-applied (they are reported as failed: the file keeps them). *outSel: the last step's
// selection (free it).
bool MapEditBegin(MapEditor *E);
// Returns
//   0 saved: the note is "saved %d edit%s to %s"
//   1 nothing to save, the file is up to date: the note is "nothing to save: %s is up to date"
//   2 not saved: MS_LastError() is "nothing to save" (no working copy), "not saved: %s changed on
//     disk ..." or "cannot write %s"
unsigned char MapEditSave(MapEditor *E);
// CTRL+S: the draw mode's canvases and the map (paintHooks.save), else MapEditSave; the same values
unsigned char MapEditSaveAll(MapEditor *E);
// The paint memory of the history (undo, redo and lost steps)
long long MapEditPaintBytes(const MapEditor *E);
// Every map-text file on disk is the one the working copy was last read from or written to (and a
// new part not saved yet is still not there)
bool MapEditDiskUnchanged(const MapEditor *E);
int MapEditUnsaved(const MapEditor *E);
// MapEditRebase returns
//   0 every unsaved step applied again: the note says how many
//   1 applied again, *failedCount could not be: the note "... %d could not be (listed in red)";
//     failed[] lists them
//   2 the map can't be edited any more, the unsaved edits dropped: MS_LastError(); *failedCount =
//     the steps dropped
//   3 out of memory, nothing done: MS_LastError()
// failed[] also gets the steps undone past the save point (with every code but 3).
unsigned char MapEditRebase(MapEditor *E, MS_EntityId **outSel, int *outSelCount, int *failedCount);
void MapEditClearFailed(MapEditor *E);
// Writes the working copy to `path` (closing with unsaved edits): the main file's text to path, and
// every other file that differs from disk to <path without ".map">/<its package path>
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
// The next free `id` of the loaded map (the largest one in any of its map-text files + 1)
int MapEditNextId(void);

// A short description of where an object comes from: "line 120 (copy 3 of 8, id 7)", "line
// parts/lake.map:41, id 7" for a line in a part
void MapEditDescribe(MS_ObjRef o, char *out, int outSize);

// What made each object (its line's statement), for the transform modal: what it can do
typedef enum {
    EDIT_CLASS_BOX = 0, EDIT_CLASS_BLOCK, EDIT_CLASS_OTHER_BOX,  // `box`, `block`, stairs / waffle
    EDIT_CLASS_MODEL, EDIT_CLASS_LIGHT, EDIT_CLASS_DIR_LIGHT,     // a directional light has no position
    EDIT_CLASS_NONE,                                            // not from a line of the map text
    EDIT_CLASS_BARRIER,                                         // `barrier` (edited like a `box` line)
    EDIT_CLASS_SHAPE,                                           // `shape <kind>` (edited like a `block` line)
} EditObjClass;
// A class whose line is two corners or a block (`box`, `block`, `barrier`, `shape`): moves, turns, scales
// and resizes
static inline bool EditClassBoxLike(int c)
{
    return c == EDIT_CLASS_BOX || c == EDIT_CLASS_BLOCK || c == EDIT_CLASS_BARRIER || c == EDIT_CLASS_SHAPE;
}
// classes[i] for objs[i] (each line read once; allocates only while reading the lines)
void MapEditClassify(const MS_ObjRef *objs, int count, unsigned char *classes);
// The transform an object of class cls (EditObjClass) gets from op (EDIT_MOVE, EDIT_ROTATE,
// EDIT_SET_ROTATION, EDIT_SCALE), with the same math MapEditApply writes the line with (before its
// rounding to the written decimals): the transform modal's preview. A box that becomes
// axis-aligned gets its permuted size and rotation 0, as written. False: it doesn't follow (*out = *in)
bool MapEditPreviewTransform(const EditOp *op, int cls, const MS_Transform *in, MS_Transform *out);

// --apply-edits <script>: runs an edit script against the loaded scene (a noGpu load of
// --map), writing the map-text files each edit changes after it. Returns the exit status (0 = every line
// worked). Format: docs/code/mapedit.md#edit-script.
int MapEditRunScript(const char *scriptPath);
// --edit-script <script>: the same language applied live to the loaded GPU scene (working copy,
// MS_EditSetText; nothing written unless the script says `save`), before --shot / --dump-* run.
// Also `save` and `disk <file>` (docs/live-editing.md 7.5). Returns the exit status.
int MapEditRunLiveScript(MapEditor *E, const char *scriptPath);

#endif
