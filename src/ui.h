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

#define UI_RADIUS   6   // corner radius of panels, buttons, fields (list rows are square)
#define UI_CAP_R    4   // corner radius of key caps
#define UI_HUD_LINE 20  // line step of the top-left HUD (UI_S text on a backdrop)
#define UI_PILL_H   20  // height of UiPill / UiMark / UiTag
#define UI_HALO     14  // reach of a panel's dark halo beyond its edge (UiSheet)

// Colour tokens (docs/ui-design.md). Surfaces are translucent black, text is white at alpha
// tiers, colour only means something (a status mark: a 2 px left bar + text in its colour).
#define UI_PANEL    CLITERAL(Color){ 0, 0, 0, 185 }         // panels, prompt box, toasts (#000000b9)
#define UI_BACKDROP CLITERAL(Color){ 0, 0, 0, 140 }         // behind HUD lines
#define UI_SCRIM    CLITERAL(Color){ 0, 0, 0, 97 }          // over the scene behind a modal panel (#00000061)
#define UI_INSET    CLITERAL(Color){ 0, 0, 0, 97 }          // fields: a darker well in a panel
#define UI_RAISED   CLITERAL(Color){ 255, 255, 255, 36 }    // buttons, caps (#ffffff24)
#define UI_HOVER    CLITERAL(Color){ 255, 255, 255, 66 }    // a hovered button (#ffffff42)
#define UI_ROW      CLITERAL(Color){ 255, 255, 255, 20 }    // the selected list row
#define UI_SELECT   CLITERAL(Color){ 255, 255, 255, 84 }    // selected text in a field (#ffffff54)
#define UI_LINE     CLITERAL(Color){ 255, 255, 255, 40 }    // rules, field outlines
#define UI_HALO_C   CLITERAL(Color){ 0, 0, 0, 128 }         // a panel's halo (0 0 10px 1px rgba(0,0,0,.5))
#define UI_HOT      CLITERAL(Color){ 255, 255, 255, 255 }   // hover, the selection, the text cursor
#define UI_INK      CLITERAL(Color){ 255, 255, 255, 221 }   // titles, values, main text (#ffffffdd)
#define UI_TEXT     CLITERAL(Color){ 255, 255, 255, 208 }   // HUD text, labels (#ffffffd0)
#define UI_BODY     CLITERAL(Color){ 255, 255, 255, 179 }   // list rows, body text (.7)
#define UI_DIM      CLITERAL(Color){ 255, 255, 255, 126 }   // secondary text, hints (#ffffff7e)
#define UI_FAINT    CLITERAL(Color){ 255, 255, 255, 69 }    // placeholders, off toggles, separators (#ffffff45)
#define UI_ON_INK   CLITERAL(Color){ 0, 0, 0, 230 }         // text on a white (pressed) button
// Meaning (status marks)
#define UI_CHANGED  CLITERAL(Color){ 255, 140, 240, 255 }   // changed from the preset / unsaved (#ff8cf0)
#define UI_OK       CLITERAL(Color){ 51, 255, 153, 255 }    // done, saved (#33ff99)
#define UI_WARN     CLITERAL(Color){ 255, 140, 26, 255 }    // failed request, needs a restart (#ff8c1a)
#define UI_ASK      CLITERAL(Color){ 255, 246, 126, 255 }   // a question waits (#fff67e)
#define UI_ERROR    CLITERAL(Color){ 255, 77, 109, 255 }    // an error (the site has no red)
// Modes (the HUD's mode name: its left bar)
#define UI_MODE_EDIT CLITERAL(Color){ 137, 117, 255, 255 }  // #8975ff
#define UI_MODE_DRAW CLITERAL(Color){ 17, 132, 255, 255 }   // #1184ff
// c with its alpha multiplied by a (0..1): fades a token without losing its tier (raylib's Fade
// replaces the alpha instead, which makes a faint token brighter)
Color UiAlpha(Color c, float a);

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
// The editor's panel: UI_PANEL with a soft dark halo outside its edge (no border)
void UiSheet(Rectangle r);
// The soft halo alone, in any colour (dark under a panel; white: a glow), reach UI_HALO
void UiHalo(Rectangle r, Color c);
// Pixel-face title (Pixelify Sans, cell 3 px: crisp only at its one size; its line box is
// UI_TITLE_H high, the capitals ~21 px from ~6 px below its top)
#define UI_TITLE_H 39
void UiTitle(const char *text, int x, int y, Color c);
int UiTitleWidth(const char *text);
// A status mark: 2 px left bar + UI_XS semibold text, both in c, vertically in a UI_PILL_H box;
// returns its width
int UiMark(const char *text, int x, int y, Color c);
int UiMarkWidth(const char *text);
// A filled label tab (the request images' region and volume numbers: kept as they were)
int UiTag(const char *text, int x, int y, Color fill, Color ink);
int UiTagWidth(const char *text);
// A label (UI_XS semibold) with its top-left at x, y, drawn as a UiMark in `fill` (ink is
// ignored; kept for the callers); returns its width
int UiPill(const char *text, int x, int y, Color fill, Color ink);
int UiPillWidth(const char *text);
// Centred toast: text on a panel, faded by alpha
void UiToast(const char *text, int centreX, int y, int size, Color c, float alpha);

// Key hints. Markup: {KEY} is drawn as a key cap, | as a faint separator, ~text~ faint (a
// switched-off toggle), ^text^ in UI_HOT (a switched-on one). Returns / measures the width.
int UiHints(const char *markup, int x, int y, int size, Color ink);
int UiHintsWidth(const char *markup, int size);

// HUD lines over the scene: a dark backdrop strip of height `h` behind the text or hints, so
// they read over bright skies (consecutive lines' strips join into one block)
void UiHudText(const char *text, int x, int y, int h, int size, Color c);
void UiHudHints(const char *markup, int x, int y, int h, int size, Color ink);
// A status line over the scene: the backdrop strip, a 2 px bar and the text, both in c (a mode
// name, a request's state, an edit that failed; docs/ui-design.md 4). Returns how far right of x
// the next HUD text on the same line starts so that the backdrop strips join.
int UiHudMark(const char *text, int x, int y, int h, int size, Color c);

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

// Mouse widgets (the draw panel; docs/painting.md 13.2). Immediate mode: each call draws the
// widget and handles its input this frame; `id` names it (unique within its panel, > 0) so a drag
// or an edit in progress stays with it. Text is UI_M (UI_S where UI_M doesn't fit), fields
// UI_RAISED with a UI_LINE border. Nothing is allocated.
typedef struct { int active; float grab; } UiDrag;     // the slider being dragged (0: none)
typedef struct {
    int active;                     // the field being typed into (0: none)
    int claim;                      // a field clicked while another was open: it opens next frame
    char buf[32];                   // what is typed (one number or a hex colour: stack-sized)
    int len, cursor;
    bool all;                       // the whole text is selected (just opened): typing replaces it
} UiEdit;

// A panel that handles its input in an update step and draws later (the draw panel: before the
// draw mode, then with the HUD) lays itself out twice: once with UI_WIDGETS_INPUT (nothing is
// drawn), once with UI_WIDGETS_DRAW (no input is read; the values only show). mouse false: the
// mouse isn't the panel's (no hover, no clicks; an open field still takes the typing). Hover
// counts only inside clip (width 0: anywhere), so widgets scrolled out of a panel take nothing.
// UiWidgetsEnd goes back to the default: input and drawing in one call, the mouse everywhere.
typedef enum { UI_WIDGETS_BOTH = 0, UI_WIDGETS_INPUT, UI_WIDGETS_DRAW } UiWidgetPass;
void UiWidgetsBegin(UiWidgetPass pass, bool mouse, Rectangle clip);
void UiWidgetsEnd(void);

// A horizontal slider of *value in lo..hi (step > 0: snapped to it); LMB drags, the wheel steps
// (step, or 1/50 of the range). True when the value changed this frame.
bool UiSlider(UiDrag *drag, int id, Rectangle r, float *value, float lo, float hi, float step);
// A number field: a click starts typing (the whole value selected), ENTER or a click elsewhere
// applies (clamped to lo..hi, rounded to `decimals`), ESC cancels, the wheel steps while hovered.
// True when the value changed.
bool UiNumberField(UiEdit *edit, int id, Rectangle r, float *value, float lo, float hi, float step, int decimals);
// A text field with the same editing rules: true when ENTER (or a click elsewhere) applied it.
bool UiTextField(UiEdit *edit, int id, Rectangle r, char *text, int textSize);
// A colour swatch (alpha shown over a checker), outlined in UI_HOT when selected; true when clicked
bool UiSwatch(Rectangle r, Color c, bool selected);
// A button in the panel's look (active: drawn on, as a pressed toggle); true when clicked
// (LMB pressed on it). label NULL or "": no text (the caller draws an icon on it).
bool UiButton(Rectangle r, const char *label, bool active);
// A widget had the mouse this frame (a drag, a field being typed into): the panel keeps it
bool UiWidgetsBusy(const UiDrag *drag, const UiEdit *edit);
// Rounded rectangle of any radius (radius >= half the shorter side: a disc or a pill), filled,
// with an optional 1 px border (border.a == 0: none); from the atlas, like UiPanel
void UiRoundRect(Rectangle r, int radius, Color fill, Color border);

#endif
