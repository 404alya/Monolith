// The groups panel (docs/editor-tools.md 3.4): F6 in spectator mode, in or out of the edit mode.
//
// Lists every group of the map by name (every `group` statement with that name is one group),
// sorted by area (the text before ": ") then name, under an area row when the area has more than one
// group. ENTER selects and flies there; CTRL+R / DELETE / CTRL+D / CTRL+N / CTRL+G edit through the
// edit mode (live, undoable). The rows are rebuilt when scene->version changes, only while the panel
// is open; drawing allocates nothing.
// Reference: docs/code/groupspanel.md
#include "groupspanel.h"
#include <ctype.h>
#include <math.h>

#define ARM_TIME 3.0f       // DELETE again within this many seconds deletes

static bool Ctrl(void) { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
static bool KeyRepeat(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

// #region The items

// The length of the area of a group name (the text before the first ": "), 0 when it has none
static int AreaLen(const char *name)
{
    const char *c = strstr(name, ": ");
    return c && c > name ? (int)(c - name) : 0;
}

// The order of the rows: by area (a name without one: by the name), the group without an area before
// the area's groups, then by name
static int CompareEntries(const void *a, const void *b)
{
    const GroupEntry *x = a, *y = b;
    int lx = x->areaLen ? x->areaLen : (int)strlen(x->name), ly = y->areaLen ? y->areaLen : (int)strlen(y->name);
    int c = memcmp(x->name, y->name, (size_t)(lx < ly ? lx : ly));
    if (c) return c;
    if (lx != ly) return lx < ly ? -1 : 1;
    if ((x->areaLen > 0) != (y->areaLen > 0)) return x->areaLen > 0 ? 1 : -1;
    return strcmp(x->name, y->name);
}

static int CompareGroupIdx(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    int c = strcmp(scene->groups[x], scene->groups[y]);
    return c ? c : (x > y) - (x < y);
}

static int ComparePairs(const void *a, const void *b)
{
    const int *x = a, *y = b;
    return x[0] != y[0] ? (x[0] > y[0]) - (x[0] < y[0]) : (x[1] > y[1]) - (x[1] < y[1]);
}

// The rows from the scene: the groups by name (their statements, objects, files, lines, bounds), sorted,
// with an area row before each area of several groups. (Temporaries: heap, freed here: a rebuild is an
// event, a load or an edit, never a frame of its own.)
static void Rebuild(GroupsPanel *p)
{
    p->itemCount = 0;
    p->scene = scene;
    p->version = scene->version;
    p->list.dirty = true;
    int n = scene->groupCount, u = 0, pairCount = 0, pairCap = 0;
    int *order = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1)), *uniq = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
    int *lastFile = NULL, *pairs = NULL;
    GroupEntry *g = calloc((size_t)(n > 0 ? n : 1), sizeof(GroupEntry));
    if (!order || !uniq || !g) goto done;

    // The names: one entry per distinct name (uniq[group index] = its entry)
    for (int i = 0; i < n; i++) order[i] = i;
    if (n > 1) qsort(order, (size_t)n, sizeof(int), CompareGroupIdx);
    for (int i = 0; i < n; i++) {
        const char *name = scene->groups[order[i]];
        if (u == 0 || strcmp(g[u - 1].name, name) != 0)
            g[u++] = (GroupEntry){ .name = name, .areaLen = AreaLen(name), .firstFile = -1,
                                   .bounds = { { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } } };
        g[u - 1].statements++;
        uniq[order[i]] = u - 1;
    }
    // Their objects, in map order: count, bounds (lights aside), the first object's file and the lines
    // there, and (entry, file) pairs for the file count
    if (!(lastFile = malloc(sizeof(int) * (size_t)(u > 0 ? u : 1)))) goto done;
    for (int k = 0; k < u; k++) lastFile[k] = -1;
    for (int e = 0; e < scene->entityCount; e++) {
        MS_ObjRef o = scene->entityObjects[e];
        const MS_Ref *r = MS_ObjectRef(scene, o);
        if (!r || r->group < 0 || r->group >= n) continue;
        GroupEntry *it = &g[uniq[r->group]];
        const MS_Source *s = &scene->entitySources[e];
        it->objects++;
        if (o.kind != MS_OBJ_LIGHT) {
            BoundingBox b = MS_ObjectBounds(scene, o);
            it->bounds.min = Vector3Min(it->bounds.min, b.min);
            it->bounds.max = Vector3Max(it->bounds.max, b.max);
        }
        if (it->firstFile < 0) {
            it->firstFile = s->file;
            it->firstLine = it->lastLine = s->line;
        } else if (s->file == it->firstFile) {
            if (s->line < it->firstLine) it->firstLine = s->line;
            if (s->line > it->lastLine) it->lastLine = s->line;
        }
        if (lastFile[uniq[r->group]] != s->file) {
            lastFile[uniq[r->group]] = s->file;
            if (!MS_RESERVE(pairs, pairCap, 2 * pairCount + 2)) goto done;
            pairs[2 * pairCount] = uniq[r->group];
            pairs[2 * pairCount + 1] = s->file;
            pairCount++;
        }
    }
    if (pairCount > 1) qsort(pairs, (size_t)pairCount, 2 * sizeof(int), ComparePairs);
    for (int i = 0; i < pairCount; i++)
        if (i == 0 || pairs[2 * i] != pairs[2 * i - 2] || pairs[2 * i + 1] != pairs[2 * i - 1]) g[pairs[2 * i]].files++;
    if (u > 1) qsort(g, (size_t)u, sizeof(GroupEntry), CompareEntries);

    // The rows: an area of several groups gets a header row before them
    for (int i = 0; i < u; ) {
        int j = i + 1;
        while (g[i].areaLen > 0 && j < u && g[j].areaLen == g[i].areaLen && memcmp(g[j].name, g[i].name, (size_t)g[i].areaLen) == 0) j++;
        if (j - i > 1) {
            GroupEntry *h = MS_PUSH(p->items, p->itemCount, p->itemCap);
            if (!h) goto done;
            *h = (GroupEntry){ .header = true, .name = g[i].name, .areaLen = g[i].areaLen, .statements = j - i,
                               .first = p->itemCount, .count = j - i, .firstFile = -1, .bounds = g[i].bounds };
            for (int k = i; k < j; k++) {
                h->objects += g[k].objects;
                h->bounds.min = Vector3Min(h->bounds.min, g[k].bounds.min);
                h->bounds.max = Vector3Max(h->bounds.max, g[k].bounds.max);
            }
        }
        for (int k = i; k < j; k++) {
            GroupEntry *it = MS_PUSH(p->items, p->itemCount, p->itemCap);
            if (!it) goto done;
            *it = g[k];
            it->inArea = j - i > 1;
        }
        i = j;
    }
done:
    free(order);
    free(uniq);
    free(lastFile);
    free(pairs);
    free(g);
}

// Case-insensitive substring
static bool Contains(const char *s, const char *needle)
{
    if (!s) return false;
    for (; *s; s++) {
        int k = 0;
        while (needle[k] && s[k] && tolower((unsigned char)s[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}

// The package path of a scene's map-text file ("parts/plain.map"; the main file's own name)
static const char *FilePath(int file)
{
    const MS_MapFile *f = file >= 0 ? MS_MapFileAt(scene, file) : NULL;
    return f ? f->path : "";
}

static bool MatchItem(int item, const char *filter, void *user)
{
    const GroupsPanel *p = user;
    const GroupEntry *it = &p->items[item];
    if (it->header) {
        // (an area row stays while one of its groups passes)
        for (int k = it->first; k < it->first + it->count; k++)
            if (MatchItem(k, filter, user)) return true;
        return false;
    }
    return Contains(it->name, filter) || Contains(FilePath(it->firstFile), filter);
}

// The items describe the current scene (after any reload, live edit or renderer restart)
static void Refresh(GroupsPanel *p)
{
    if (p->scene != scene || p->version != scene->version) Rebuild(p);
    if (p->list.dirty) UiListRefilter(&p->list, p->itemCount, MatchItem, p);
}

// #endregion

// #region Open, close, keys

static void Disarm(GroupsPanel *p)
{
    free(p->armName);
    p->armName = NULL;
    p->armTime = 0.0f;
}

static void CloseField(GroupsPanel *p)
{
    p->field = 0;
    free(p->fieldGroup);
    p->fieldGroup = NULL;
}

void GroupsPanelOpen(GroupsPanel *p, const char *filter)
{
    p->open = true;
    UiListClear(&p->list);
    for (const char *c = filter ? filter : ""; *c; c++) {
        if (!MS_RESERVE(p->list.filter, p->list.filterCap, p->list.filterLen + 2)) break;
        p->list.filter[p->list.filterLen++] = *c;
        p->list.filter[p->list.filterLen] = '\0';
    }
    p->scene = NULL;        // (rebuilt on first use)
    CloseField(p);
    Disarm(p);
    while (GetCharPressed()) {}     // (nothing typed before it opened goes into the filter)
    if (p->interactive) EnableCursor();
}

void GroupsPanelClose(GroupsPanel *p)
{
    if (!p->open) return;
    p->open = false;
    CloseField(p);
    Disarm(p);
    p->freezeFrames = 2;    // GLFW can report one big mouse jump when the cursor is captured again
    if (p->interactive) DisableCursor();
}

bool GroupsPanelCapturesInput(const GroupsPanel *p) { return p->open || p->freezeFrames > 0; }

// The group names of a row into p->names (a header: its groups'); their count (0: out of memory). The
// names are the scene's: copy them before the first edit
static int RowNames(GroupsPanel *p, const GroupEntry *it)
{
    int n = it->header ? it->count : 1;
    if (!MS_RESERVE(p->names, p->nameCap, n)) return 0;
    for (int k = 0; k < n; k++) p->names[k] = it->header ? p->items[it->first + k].name : it->name;
    return n;
}

// Every object of the groups with these names into p->objs; their count
static int GroupObjects(GroupsPanel *p, const char *const *names, int n)
{
    int count = 0;
    bool *mask = calloc((size_t)(scene->groupCount > 0 ? scene->groupCount : 1), sizeof(bool));
    if (!mask) return 0;
    for (int g = 0; g < scene->groupCount; g++)
        for (int k = 0; k < n && !mask[g]; k++) mask[g] = strcmp(scene->groups[g], names[k]) == 0;
    for (int e = 0; e < scene->entityCount; e++) {
        const MS_Ref *r = MS_ObjectRef(scene, scene->entityObjects[e]);
        if (!r || r->group < 0 || r->group >= scene->groupCount || !mask[r->group]) continue;
        if (!MS_RESERVE(p->objs, p->objCap, count + 1)) break;
        p->objs[count++] = scene->entityObjects[e];
    }
    free(mask);
    return count;
}

// In front of the bounds: from the side the camera is on (horizontally), looking down 20
// degrees, at the distance where a sphere around the bounds fills the vertical view (the id panel's)
static void FlyTo(Spectator *spec, BoundingBox b)
{
    if (!(b.min.x <= b.max.x)) return;      // (lights only: nowhere to fly)
    Vector3 c = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    float radius = fmaxf(0.5f * Vector3Distance(b.min, b.max), 0.5f);
    float halfFov = 0.5f * SpectatorCamera(spec).fovy * DEG2RAD;
    float dist = fmaxf(radius / sinf(halfFov) * 1.1f, 2.0f);
    Vector3 to = Vector3Subtract(c, spec->pos);
    float yaw = fabsf(to.x) + fabsf(to.z) > 1e-3f ? atan2f(to.x, to.z) : spec->yaw;
    float pitch = -20.0f * DEG2RAD;
    Vector3 eye = Vector3Subtract(c, Vector3Scale(LookDir(yaw, pitch), dist));
    SpectatorFlyToPose(spec, eye, yaw, pitch);
}

// The text field's keys: typed characters (no '"'), BACKSPACE, ESC (closes it), ENTER (the edit:
// a rename of the row's group, or a new group from the designer's selection; it stays open when refused)
static EditModeResult FieldUpdate(GroupsPanel *p, EditMode *m, Designer *d)
{
    if (IsKeyPressed(KEY_ESCAPE)) { CloseField(p); while (GetCharPressed()) {} return EDITMODE_NONE; }
    if (KeyRepeat(KEY_BACKSPACE) && p->textLen > 0) {
        do p->textLen--;
        while (p->textLen > 0 && ((unsigned char)p->text[p->textLen] & 0xC0) == 0x80);     // (a whole UTF-8 character)
        p->text[p->textLen] = '\0';
    }
    for (int c = GetCharPressed(); c; c = GetCharPressed()) {
        if (c < 32 || c == '"' || c == 127) continue;
        int size = 0;
        const char *utf8 = CodepointToUTF8(c, &size);
        if (size <= 0 || !MS_RESERVE(p->text, p->textCap, p->textLen + size + 1)) continue;
        memcpy(p->text + p->textLen, utf8, (size_t)size);
        p->textLen += size;
        p->text[p->textLen] = '\0';
    }
    if (!IsKeyPressed(KEY_ENTER) && !IsKeyPressed(KEY_KP_ENTER)) return EDITMODE_NONE;
    const char *text = p->text ? p->text : "";
    EditOp op = { .kind = p->field == 1 ? EDIT_GROUP_RENAME : EDIT_GROUP_NEW, .groupName = p->fieldGroup, .groupNew = text };
    EditModeResult r = EditModeApplyTo(m, d, KEY_ENTER, d->selected, d->selectedCount, &op);
    if (r == EDITMODE_RELOADED) CloseField(p);
    return r;
}

// Opens the text field (1 rename: prefilled with the group's name; 2 new group: empty)
static void OpenField(GroupsPanel *p, int kind, const char *name)
{
    CloseField(p);
    Disarm(p);
    p->textLen = 0;
    if (kind == 1 && !(p->fieldGroup = MS_StrDup(name))) return;
    const char *pre = kind == 1 ? name : "";
    int len = (int)strlen(pre);
    if (!MS_RESERVE(p->text, p->textCap, len + 1)) { CloseField(p); return; }
    memcpy(p->text, pre, (size_t)len + 1);
    p->textLen = len;
    p->field = kind;
    while (GetCharPressed()) {}     // (CTRL+R's / CTRL+N's character is no part of the name)
}

// DELETE on a row: the first press arms it (3 s), the second deletes its group (an area row: each of
// its groups, one step each)
static EditModeResult DeleteKey(GroupsPanel *p, EditMode *m, Designer *d, const GroupEntry *it)
{
    if (!(p->armName && p->armTime > 0.0f && p->armArea == it->header && strcmp(p->armName, it->name) == 0)) {
        Disarm(p);
        p->armName = MS_StrDup(it->name);
        p->armArea = it->header;
        p->armTime = ARM_TIME;
        return EDITMODE_NONE;
    }
    Disarm(p);
    int n = RowNames(p, it);
    char **own = calloc((size_t)(n > 0 ? n : 1), sizeof(char *));
    EditModeResult res = EDITMODE_NONE;
    for (int k = 0; own && k < n; k++) own[k] = MS_StrDup(p->names[k]);
    for (int k = 0; own && k < n; k++) {
        if (!own[k]) break;
        EditOp op = { .kind = EDIT_GROUP_DELETE, .groupName = own[k] };
        if (EditModeApplyTo(m, d, KEY_DELETE, d->selected, d->selectedCount, &op) != EDITMODE_RELOADED) break;
        res = EDITMODE_RELOADED;
    }
    for (int k = 0; own && k < n; k++) free(own[k]);
    free(own);
    return res;
}

EditModeResult GroupsPanelUpdate(GroupsPanel *p, EditMode *m, Designer *d, Spectator *spec, float dt)
{
    if (!p->open) {
        if (p->freezeFrames > 0) p->freezeFrames--;
        return EDITMODE_NONE;
    }
    if (p->armTime > 0.0f && (p->armTime -= dt) <= 0.0f) Disarm(p);
    if (p->field) return FieldUpdate(p, m, d);
    if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_F6)) { GroupsPanelClose(p); return EDITMODE_NONE; }
    Refresh(p);
    bool ctrl = Ctrl();
    if (ctrl) while (GetCharPressed() > 0) { }      // (CTRL+R is no 'r' in the filter)
    UiListKeys(&p->list, p->visibleRows);
    if (p->list.dirty) UiListRefilter(&p->list, p->itemCount, MatchItem, p);
    bool rename = ctrl && IsKeyPressed(KEY_R), del = IsKeyPressed(KEY_DELETE), dup = ctrl && IsKeyPressed(KEY_D);
    bool make = ctrl && IsKeyPressed(KEY_N), into = ctrl && IsKeyPressed(KEY_G);
    // The view-only build (common.h): the panel only finds and flies; its edits are refused
    if (!MONOLITH_EDITING && (rename || del || dup || make || into)) {
        DesignerMessageStatic(d, true, MSG_VIEW_ONLY);
        return EDITMODE_NONE;
    }
    if (make) {
        if (d->selectedCount == 0) DesignerMessageStatic(d, true, "CTRL+N: select the objects of the new group first");
        else OpenField(p, 2, NULL);
        return EDITMODE_NONE;
    }
    int item = UiListSelected(&p->list);
    if (item < 0) return EDITMODE_NONE;
    const GroupEntry *it = &p->items[item];
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        int n = RowNames(p, it), count = GroupObjects(p, p->names, n);
        DesignerClearSelection(d);
        for (int k = 0; k < count; k++) DesignerSelectObject(d, p->objs[k]);     // (lights can't be selected)
        FlyTo(spec, it->bounds);
        if (it->header)
            SpectatorMessagef(spec, "area %.*s: %d groups (%d objects)", it->areaLen, it->name, it->count, it->objects);
        else
            SpectatorMessagef(spec, "group \"%s\" (%d object%s)", it->name, it->objects, it->objects == 1 ? "" : "s");
        GroupsPanelClose(p);
        return EDITMODE_NONE;
    }
    if (rename) {
        if (it->header) DesignerMessageStatic(d, true, "CTRL+R renames a group: choose one of the area's rows");
        else OpenField(p, 1, it->name);
        return EDITMODE_NONE;
    }
    if (into) {
        if (it->header) { DesignerMessageStatic(d, true, "CTRL+G puts the selection into a group: choose one of the area's rows"); return EDITMODE_NONE; }
        EditOp op = { .kind = EDIT_GROUP_MOVE, .groupName = it->name };
        return EditModeApplyTo(m, d, KEY_G, d->selected, d->selectedCount, &op);
    }
    if (dup) {
        int n = RowNames(p, it);
        return n > 0 ? EditModeDuplicateGroups(m, d, KEY_D, p->names, n) : EDITMODE_NONE;
    }
    if (del) return DeleteKey(p, m, d, it);
    return EDITMODE_NONE;
}

// #endregion

// #region Drawing

#if MONOLITH_EDITING
#define HINTS "{ENTER} fly to, select|{CTRL+R} rename|{DEL} delete|{CTRL+D} duplicate|{CTRL+N} new from the selection|" \
              "{CTRL+G} selection into it|{ESC} close"
#else
// (the view-only build: no edits from the panel)
#define HINTS "{UP}/{DOWN} select|{ENTER} fly to, select|type to filter|{ESC} close"
#endif

// The first n bytes of s, cut by characters until they fit in room px ("..." added); drawn at x
static void FitText(const char *s, int n, int x, int y, int room, int size, Color c, bool bold)
{
    if (room <= 0 || !s || n <= 0) return;
    char cut[160];
    if (n > (int)sizeof(cut) - 4) n = (int)sizeof(cut) - 4;
    snprintf(cut, sizeof(cut), "%.*s", n, s);
    int (*measure)(const char *, int) = bold ? UiMeasureBold : UiMeasure;
    for (int k = n; k > 0 && measure(cut, size) > room; k--) snprintf(cut, sizeof(cut), "%.*s...", k - 1, s);
    if (measure(cut, size) > room) return;
    if (bold) UiTextBold(cut, x, y, size, c);
    else UiText(cut, x, y, size, c);
}

static void DrawItemRow(int item, Rectangle row, bool selected, void *user)
{
    const GroupsPanel *p = user;
    const GroupEntry *it = &p->items[item];
    int ty = (int)row.y + ((int)row.height - UiLineHeight(UI_M)) / 2;
    int sy = (int)row.y + ((int)row.height - UiLineHeight(UI_S)) / 2;
    int x = (int)row.x + 12, right = (int)(row.x + row.width) - 10;
    if (it->header) {
        // An area: its name, its groups and objects
        const char *count = TextFormat("%d groups, %d objects", it->count, it->objects);
        int cw = UiMeasure(count, UI_S);
        UiText(count, right - cw, sy, UI_S, UI_DIM);
        FitText(it->name, it->areaLen, x, ty, right - cw - 14 - x, UI_M, selected ? UI_HOT : UI_INK, true);
        return;
    }
    if (it->inArea) x += 18;
    Color ink = selected ? UI_HOT : UI_BODY;
    // From the right: the lines (or the files), the file, the object count; the name takes the rest
    const char *lines = it->files > 1 ? TextFormat("%d files", it->files)
                      : it->firstLine > 0 ? TextFormat("%d-%d", it->firstLine, it->lastLine) : "-";
    UiText(lines, right - UiMeasure(lines, UI_S), sy, UI_S, UI_DIM);
    right -= 96;
    FitText(FilePath(it->firstFile), (int)strlen(FilePath(it->firstFile)), right - 170, sy, 164, UI_S, UI_DIM, false);
    right -= 176;
    const char *objs = TextFormat("%d obj", it->objects);
    UiText(objs, right - UiMeasure(objs, UI_S), sy, UI_S, UI_DIM);
    right -= 72;
    // (a group whose name repeats: how many statements it has)
    int sw = 0;
    if (it->statements > 1) {
        const char *st = TextFormat("x%d", it->statements);
        sw = UiMeasure(st, UI_S) + 10;
        UiText(st, right - sw + 10, sy, UI_S, UI_DIM);
    }
    FitText(it->name, (int)strlen(it->name), x, ty, right - sw - x, UI_M, ink, false);
}

void GroupsPanelDraw(GroupsPanel *p, const Designer *d, int screenW, int screenH)
{
    if (!p->open) return;
    Refresh(p);
    // On the right like the id panel, wide enough for the hints; the bottom above the help bars
    float w = fminf(fmaxf(760.0f, (float)UiHintsWidth(HINTS, UI_XS) + 40.0f), (float)screenW - 40.0f);
    float h = fminf(640.0f, (float)screenH - 360.0f);
    if (h < 200.0f) h = fminf(200.0f, (float)screenH - 20.0f);
    float y = (float)screenH - 90.0f - h;
    Rectangle r = { (float)screenW - w - 20.0f, y > 110.0f ? y : 110.0f, w, h };
    p->visibleRows = UiListDraw(&p->list, r, "Groups of the map", 24, DrawItemRow, p, HINTS);

    // Above it: the text field, or the armed DELETE
    int x = (int)r.x, top = (int)r.y - 100;
    if (p->field) {
        const char *title = p->field == 1 ? "Rename group" : TextFormat("New group from the selection (%d objects)", d->selectedCount);
        UiSheet((Rectangle){ (float)x, (float)top, w, 88 });
        UiTextBold(title, x + 12, top + 8, UI_S, UI_INK);
        if (p->field == 1 && p->fieldGroup)
            FitText(p->fieldGroup, (int)strlen(p->fieldGroup), x + 24 + UiMeasureBold(title, UI_S), top + 8,
                    (int)w - 36 - UiMeasureBold(title, UI_S), UI_S, UI_DIM, false);
        Rectangle f = { (float)(x + 12), (float)(top + 32), w - 24, 26 };
        UiPanel(f, UI_INSET, UI_DIM);
        int ty = top + 32 + (26 - UiLineHeight(UI_M)) / 2;
        const char *text = p->text ? p->text : "";
        // (a long name shows its end, where the cursor is)
        const char *shown = text;
        while (*shown && UiMeasure(shown, UI_M) > (int)w - 48) shown++;
        UiText(shown, x + 20, ty, UI_M, UI_INK);
        UiText("_", x + 20 + UiMeasure(shown, UI_M), ty, UI_M, UI_HOT);
        UiHints("{ENTER} apply|{ESC} cancel|no quotes in a name", x + 12, top + 64, UI_S, UI_DIM);
    } else if (p->armName) {
        const char *what = p->armArea ? "every group of the area" : "the group";
        int dx = UiHudMark(TextFormat("DELETE again to delete %s", what), x, top + 70, UI_HUD_LINE, UI_S, UI_WARN);
        if (p->armArea) {
            char area[160];
            int n = AreaLen(p->armName) < (int)sizeof(area) - 1 ? AreaLen(p->armName) : (int)sizeof(area) - 1;
            snprintf(area, sizeof(area), "%.*s", n, p->armName);
            UiHudText(area, x + dx, top + 70, UI_HUD_LINE, UI_S, UI_TEXT);
        } else UiHudText(p->armName, x + dx, top + 70, UI_HUD_LINE, UI_S, UI_TEXT);
    }
}

void GroupsPanelFree(GroupsPanel *p)
{
    UiListFree(&p->list);
    free(p->items);
    free(p->objs);
    free(p->names);
    free(p->text);
    free(p->fieldGroup);
    free(p->armName);
    memset(p, 0, sizeof(*p));
}

// #endregion
