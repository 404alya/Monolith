// The render inspector (F3 / SHIFT+F3 / F4, spectator mode): what is actually drawn. Outlines
// and labels per draw range (merged cell mesh, instanced boxes, model group) coloured by
// material, shader or draw kind; a panel to hide / isolate materials and shaders (the runtime's
// draw filter); R3D's buffer views. Editor only, off by default (inspector.c;
// docs/live-editing.md 8.2)
#ifndef MONOLITH_INSPECTOR_H
#define MONOLITH_INSPECTOR_H

#include "common.h"
#include "ui.h"

typedef enum { INSPECT_OFF = 0, INSPECT_MATERIAL, INSPECT_SHADER, INSPECT_DRAW, INSPECT_MODE_COUNT } InspectMode;

typedef struct {
    InspectMode mode;               // outline colours (F3 cycles)
    R3D_OutputMode output;          // R3D buffer view (F4 cycles); R3D_OUTPUT_SCENE = normal
    bool panelOpen;                 // SHIFT+F3
    UiList list;                    // materials, then shaders
    bool *hideMaterial;             // per material (heap, materialCount)
    bool *hideShader;               // per distinct shader (heap)
    const char **shaders;           // distinct shader paths (scene strings; heap array)
    int shaderCount, shaderCap, hideCap;
    bool *rangeHidden;              // the runtime filter, per draw range (heap)
    int rangeCap;
    unsigned int filterGeneration;  // +1 on every filter change (FrameKey)
    unsigned int version;           // scene->version the arrays describe

    // What is hidden, by name (survives reloads and live edits; "m:<material>" / "s:<shader>"),
    // and the item shown alone (I; NULL = none). Heap strings.
    char **hidden;
    int hiddenCount, hiddenCap;
    char *isolated;
    // Rebuilt when scene->version changes (heap arrays that only grow): the draw ranges, the
    // shader index of each material, each object's range, per list item its ranges and triangles,
    // per-frame scratch for the overlay (sized for every object / range, so drawing never allocates)
    const MS_Scene *scene;          // the scene the arrays describe (a renderer restart makes a new one)
    MS_DrawRangeInfo *ranges;
    int rangeCount, rangeInfoCap;
    int *shaderOf, shaderOfCap;
    int *boxRange, *modelRange, boxRangeCap, modelRangeCap;
    int *itemRanges, itemRangesCap;
    long long *itemTris;
    int itemTrisCap;
    void *scratch;                  // outline candidates (boxes + models + ranges)
    int scratchCap;
    int *labelPick, labelPickCap;
    Rectangle *labelRects;
    int labelRectCap;
    bool filterDirty;               // the hidden set changed: InspectorApply sets the runtime's filter
    int visibleRows;                // the panel's rows (UiListDraw), for PGUP / PGDN
} Inspector;

// Keys (F3, SHIFT+F3, F4; the panel's keys while it is open)
void InspectorUpdate(Inspector *in);
bool InspectorCapturesInput(const Inspector *in);
// --inspect <material|shader|draw>, --output-mode <name>, --inspector-panel, --inspect-hide <name>,
// --inspect-isolate <name> (a material name, or a shader path; false = no such name format)
bool InspectorSetMode(Inspector *in, const char *name);
bool InspectorSetOutput(Inspector *in, const char *name);
void InspectorOpenPanel(Inspector *in);
bool InspectorHide(Inspector *in, const char *name);
bool InspectorIsolate(Inspector *in, const char *name);
// The first --inspect-hide / --inspect-isolate name that is no material or shader path of s, NULL = none
const char *InspectorUnknownName(const Inspector *in, const MS_Scene *s);
// Before drawing the frame: applies the output mode and the draw filter when they changed
void InspectorApply(Inspector *in);
// After R3D_End (frame target bound, depth-tested like the designer overlay): outlines, and the
// labels when `labels`
void InspectorDrawOverlay(const Inspector *in, Camera3D cam, int screenW, int screenH, bool labels);
void InspectorDrawPanel(Inspector *in, int screenW, int screenH);
int InspectorDrawHud(const Inspector *in, int x, int y);   // the mode and buffer view line; its height
const char *InspectorOutputName(R3D_OutputMode mode);
void InspectorFree(Inspector *in);

#endif
