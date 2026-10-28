// The model picker (I in edit mode): the package's models with a licence row in ASSETS.md,
// with thumbnails, triangles, bounds and licence; ENTER places one at the cursor as a live
// edit (modelpicker.c; docs/live-editing.md 8.3)
#ifndef MONOLITH_MODELPICKER_H
#define MONOLITH_MODELPICKER_H

#include "common.h"
#include "designer.h"
#include "editmode.h"
#include "ui.h"

#define PICKER_THUMB_SIZE   128     // rendered px (RGBA8, no depth buffer: 64 KB each)
#define PICKER_THUMBS_PER_FRAME 2   // the interactive step's budget

typedef struct {
    char *path;                     // package-relative ("models/x.glb"), heap
    char *licence;                  // from its ASSETS.md row, heap ("" = no row found)
    bool infoReady;                 // loaded once: triangles, size, thumbnail
    bool failed;                    // the file did not load (no thumbnail, not retried until a load)
    int triangles;
    Vector3 size;                   // metres (the file's bounds)
    RenderTexture2D thumb;          // PICKER_THUMB_SIZE square (id 0 = not rendered yet)
} PickerModel;

typedef struct {
    bool open;
    UiList list;
    PickerModel *models;            // heap; the edit mode's model list (EditModeScanModels) + info
    int modelCount, modelCap;
    unsigned int loadVersion;       // scene->loadVersion the thumbnails belong to
    const MS_Scene *scene;          // ... and the scene (a renderer restart makes a new one)
    int visibleRows;                // what the last draw showed (PGUP / PGDN)
} ModelPicker;

// Opens the picker (I in edit mode; --model-picker [filter]): rescans the package's models
// (thumbnails and info of files still listed are kept), the filter typed (NULL = empty)
void ModelPickerOpen(ModelPicker *p, EditMode *m, const char *filter);
void ModelPickerClose(ModelPicker *p);
// Open: every key is the picker's (the designer, the edit mode and the camera wait)
bool ModelPickerCapturesInput(const ModelPicker *p);
// Input while open: the list keys and the filter; ESC (or I with an empty filter) closes; ENTER
// adds the selected model where the designer's hit (the cursor) is and closes
EditModeResult ModelPickerUpdate(ModelPicker *p, EditMode *m, Designer *d);
// ENTER without a key (--model-picker-key enter): the same placement
EditModeResult ModelPickerEnter(ModelPicker *p, EditMode *m, Designer *d);
// Once per frame, outside R3D_Begin/End and before the frame's scene draw: drops every
// thumbnail when scene->loadVersion (or the scene) changed; while open, renders up to
// maxPerFrame missing ones of the rows that pass the filter (allocates only then)
void ModelPickerRenderThumbs(ModelPicker *p, int maxPerFrame);
// Rows that pass the filter and still wait for their thumbnail / info
int ModelPickerPending(ModelPicker *p);
void ModelPickerDropThumbs(ModelPicker *p);     // a renderer restart: every GL object is gone
void ModelPickerDraw(ModelPicker *p, int screenW, int screenH);
void ModelPickerFree(ModelPicker *p);           // frees the thumbnails too

#endif
