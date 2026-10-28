// Editor UI: the font, type scale, colour tokens and drawing helpers shared by everything the
// editor draws over the scene (HUD, designer overlay and prompt box, edit mode, launcher lines,
// settings panel). Editor only: the runtime, the export and the examples keep raylib's font.
#ifndef MONOLITH_UI_H
#define MONOLITH_UI_H

#include "common.h"

// Type scale (em size in pixels). Only these sizes are in the font atlas, in both weights.
#define UI_XS   14      // key caps, pills, small notes
#define UI_S    16      // HUD lines, help bars, panel hints
#define UI_M    18      // launcher lines, settings rows
#define UI_L    20      // prompt text, toasts
#define UI_XL   24      // titles, the spectator toast

#define UI_RADIUS   5   // corner radius of panels, buttons, fields
#define UI_HUD_LINE 20  // line step of the top-left HUD (UI_S text on a backdrop)
#define UI_PILL_H   20  // height of UiPill

// Colour tokens
#define UI_INK      CLITERAL(Color){ 238, 236, 230, 255 }   // main text
#define UI_TEXT     CLITERAL(Color){ 238, 236, 230, 230 }   // HUD text on a backdrop
#define UI_DIM      CLITERAL(Color){ 238, 236, 230, 160 }   // secondary text
#define UI_FAINT    CLITERAL(Color){ 238, 236, 230, 90 }    // placeholders, switched-off toggles
#define UI_ACCENT   CLITERAL(Color){ 255, 196, 120, 255 }   // titles, selection, the preset
#define UI_CHANGED  CLITERAL(Color){ 140, 205, 255, 255 }   // a setting changed from its preset
#define UI_ON       CLITERAL(Color){ 150, 225, 160, 255 }   // a toggle that is on
#define UI_OK       CLITERAL(Color){ 150, 230, 150, 255 }
#define UI_WARN     CLITERAL(Color){ 255, 170, 110, 255 }
#define UI_ASK      CLITERAL(Color){ 255, 225, 120, 255 }
#define UI_ERROR    CLITERAL(Color){ 255, 110, 90, 255 }
#define UI_CURSOR   CLITERAL(Color){ 255, 200, 120, 255 }
#define UI_PANEL    CLITERAL(Color){ 16, 16, 18, 228 }      // panels, boxes, help bars
#define UI_RAISED   CLITERAL(Color){ 40, 40, 44, 255 }      // buttons, fields
#define UI_LINE     CLITERAL(Color){ 238, 236, 230, 56 }    // panel borders, rules
#define UI_BACKDROP CLITERAL(Color){ 10, 10, 12, 150 }      // behind HUD lines

// The font loads on first use (one atlas for every size and weight; also the shapes texture,
// so text and rectangles draw in one batch). Runs that draw no text never load it.
void UiClose(void);     // before CloseWindow (only if the window is open)

Font UiFont(int size, bool bold);
int UiLineHeight(int size);     // line box: text drawn at y covers y .. y + UiLineHeight
float UiAdvance(int codepoint, int size);   // how far UiText moves on after the codepoint
int UiMeasure(const char *text, int size);
int UiMeasureBold(const char *text, int size);
void UiText(const char *text, int x, int y, int size, Color c);
void UiTextBold(const char *text, int x, int y, int size, Color c);
void UiTextShadow(const char *text, int x, int y, int size, Color c);  // 1 px dark drop shadow

// Rounded panel with an optional 1 px border (border.a == 0: none)
void UiPanel(Rectangle r, Color fill, Color border);
// A pill label (UI_XS semibold) with its top-left at x, y; returns its width
int UiPill(const char *text, int x, int y, Color fill, Color ink);
int UiPillWidth(const char *text);
// Centred toast: text on a panel, faded by alpha
void UiToast(const char *text, int centreX, int y, int size, Color c, float alpha);

// Key hints. Markup: {KEY} is drawn as a key cap, | as a dim separator, ~text~ dimmed (a
// switched-off toggle), ^text^ in UI_ON (a switched-on one). Returns / measures the width.
int UiHints(const char *markup, int x, int y, int size, Color ink);
int UiHintsWidth(const char *markup, int size);

// HUD lines over the scene: a dark backdrop strip of height `h` behind the text or hints, so
// they read over bright skies (consecutive lines' strips join into one block)
void UiHudText(const char *text, int x, int y, int h, int size, Color c);
void UiHudHints(const char *markup, int x, int y, int h, int size, Color ink);

// A filterable list in the settings panel's look (the id panel, the model picker, the
// inspector panel; docs/live-editing.md 8.4). The panel owns its items; the list holds the
// selection, the scroll position, the typed filter and the items that pass it.
typedef struct {
    int sel, scroll;                // selected row (index into rows[]), first visible row
    char *filter;                   // typed text (heap; NULL = empty)
    int filterLen, filterCap;
    int *rows;                      // the items that pass the filter, in item order (heap)
    int rowCount, rowCap;
    bool dirty;                     // the filter changed: UiListRefilter before reading rows[]
} UiList;

// Does item i pass the filter (never empty: an empty filter passes everything)?
typedef bool (*UiListMatch)(int item, const char *filter, void *user);
// Draws item i's columns into `row` (the selection highlight is drawn already)
typedef void (*UiListDrawRow)(int item, Rectangle row, bool selected, void *user);

// UP / DOWN / PGUP / PGDN / HOME / END, typed characters, BACKSPACE (CTRL: the whole filter).
// ENTER, ESC, DELETE and every other key are the panel's. visibleRows: what one page scrolls.
void UiListKeys(UiList *l, int visibleRows);
// rows[] = the items 0..itemCount-1 that match (keeps the selected item selected when it still
// passes); call after UiListKeys set dirty and whenever the items change
bool UiListRefilter(UiList *l, int itemCount, UiListMatch match, void *user);
// The selected item, -1 when no row passes
int UiListSelected(const UiList *l);
// The panel at r: title, filter field, rows of rowH px (UI_M text; 24 = the settings panel's
// rows), scroll bar and the hints footer (UiHints markup). Returns the rows that fit.
int UiListDraw(UiList *l, Rectangle r, const char *title, int rowH, UiListDrawRow drawRow, void *user,
               const char *hints);
void UiListClear(UiList *l);        // empty filter, first row
void UiListFree(UiList *l);

#endif
