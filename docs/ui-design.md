# Editor UI design

The look of everything the editor draws over the scene: HUD, designer overlay and prompt box, edit
mode, transform modal, launcher lines, the settings / keys / id / inspector panels, model picker,
draw mode and draw panel. It follows the design language of [notfound404.dev](https://notfound404.dev/):
translucent black surfaces, white text at alpha tiers, a pixel display face for titles, and colour
only where it means something. How it is drawn (atlas, batching, the helpers) is in
[architecture.md](architecture.md), "Editor UI"; this note says what it should look like.

## 1. Principles

- **Monochrome.** Surfaces are black with alpha; text and controls are white with alpha. Focus,
  selection, hover and titles are told apart by **alpha tiers**, never by an accent colour.
- **Colour is meaning.** A status (changed, ok, warning, question, error) is a **status mark**: a
  2 px left bar plus text, both in the status colour. No coloured fills, no pills for labels.
- **The scene stays visible.** Panels are translucent and lift off the scene with a soft dark
  halo instead of a border line.
- **Few shapes.** Radius 6 px for panels, buttons and fields; 4 px for key caps; list rows and
  marks square; discs only for slider thumbs.

## 2. Tokens (`src/ui.h`)

Surfaces:

| Token | RGBA | Use | Site source |
|---|---|---|---|
| `UI_PANEL` | `0 0 0 185` | panels, prompt box, toasts, help bars | `#000000b9` (drawer, tooltip) |
| `UI_BACKDROP` | `0 0 0 140` | strip behind HUD lines | between `#00000071` and `#000000b9`: `#00000071` is too light over a bright sky |
| `UI_SCRIM` | `0 0 0 97` | over the scene behind a modal panel | `#00000061` |
| `UI_INSET` | `0 0 0 97` | fields (a darker well inside a panel) | `#00000061` |
| `UI_HALO_C` | `0 0 0 128` | a panel's halo | `0 0 10px 1px rgba(0,0,0,.5)` |
| `UI_RAISED` | `255 255 255 36` | buttons, key caps | `#ffffff24` (secondary button) |
| `UI_HOVER` | `255 255 255 66` | a hovered button | `#ffffff42` |
| `UI_ROW` | `255 255 255 20` | the selected list row | `from-white/20` card, toned down |
| `UI_SELECT` | `255 255 255 84` | selected text in a field | `selection:bg-[#ffffff54]` |
| `UI_LINE` | `255 255 255 40` | rules, field outlines, slider track | divider `#ffffff45`, toned down |

Text (white at alpha tiers):

| Token | Alpha | Use | Site source |
|---|---|---|---|
| `UI_HOT` | 255 | hover, the selection, the preset, the text cursor, a switched-on toggle | `hover:text-white` |
| `UI_INK` | 221 | titles, values, main text, an "on" button's fill | `#ffffffdd` headings |
| `UI_TEXT` | 208 | HUD text, labels, hints that matter | `#ffffffd0` labels |
| `UI_BODY` | 179 | list rows, row labels, body text | `rgba(255,255,255,.7)` |
| `UI_DIM` | 126 | secondary text, counts, footer hints | `#ffffff7e` nav |
| `UI_FAINT` | 69 | placeholders, switched-off toggles, `|` separators, inactive arrows | `#ffffff45` |
| `UI_ON_INK` | `0 0 0 230` | text on a white (on / pressed) button | |

Meaning (status marks only):

| Token | RGB | Means | Site source |
|---|---|---|---|
| `UI_CHANGED` | `#ff8cf0` | changed from the preset, unsaved edits | pink "still learning" |
| `UI_OK` | `#33ff99` | done, saved, request finished | green "can relearn" |
| `UI_WARN` | `#ff8c1a` | needs a restart, request failed | orange "not used for years" |
| `UI_ASK` | `#fff67e` | a question waits | category border |
| `UI_ERROR` | `#ff4d6d` | an error (broken map, refused edit) | none: the site has no red |
| `UI_MODE_EDIT` | `#8975ff` | the edit mode's bar in the HUD | category border |
| `UI_MODE_DRAW` | `#1184ff` | the draw mode's bar in the HUD | category border |

To fade a token use `UiAlpha(c, a)`, which multiplies its alpha. raylib's `Fade` replaces the
alpha, so a faint token would come out brighter. The old `UI_ACCENT`, `UI_CURSOR` and `UI_ON`
are gone: `UI_HOT` replaced them.

**Scene-side colours stay on their own constants** and are not UI tokens: the selection, hover and
crosshair-hit outlines and the region / volume frames (`designer.c` `SEL_COLOR`, `HIT_COLOR`,
`REGION_COLOR`, `VOLUME_COLOR`), the gizmo axes and ball (`transform.c`), the inspector's outline
colours per draw kind, the model picker's thumbnail lighting, paint colours and the swatch checker.
They draw into the 3D view or the request images, where they must stay saturated and distinct.

## 3. Type

| Role | Face | Size | Tier |
|---|---|---|---|
| Panel titles (settings, lists, keys, model picker, draw panel) | Pixelify Sans Regular, `UiTitle` | its one size: cell 3 px, cap height 21 px, line `UI_TITLE_H` 39 | `UI_INK` |
| Group headers in panels | Outfit SemiBold | `UI_M` | `UI_INK` |
| Rows, values, prompt text, HUD | Outfit Regular | `UI_XS` … `UI_XL` as today | per §2 |
| Key caps, status marks | Outfit SemiBold | `UI_XS` | |

**Pixelify Sans** draws on a grid of about 91 of 1000 units (about 1/11 em). raylib's rasteriser
scales ascender − descender (1200 units) to an integer pixel height, so no size lands exactly on
the grid. The face is therefore rasterised once at 13 px (cell 0.985 px), thresholded at half
coverage into whole pixels, and enlarged ×3 with nearest sampling into the atlas. This gives hard
pixel edges and integer advances at one size only, drawn at integer positions. Only the Regular
weight sits on the grid; bolder weights are not used.

## 4. Shapes and states

- **Panel** (`UiSheet`): `UI_PANEL`, radius 6, a 14 px soft dark halo outside the edge (a baked
  9-slice; nothing under the panel). No border line. A modal panel dims the scene with `UI_SCRIM`.
- **Status mark** (`UiMark`; `UiPill` draws one): 2 px bar, 6 px gap, `UI_XS` SemiBold text, both
  in the status colour, in a 20 px box. In a value column the bar sits left of the value.
- **Key cap**: flat, `UI_RAISED` fill, radius 4, no border; text `UI_XS` SemiBold in the line's
  ink. The baked 3-slice stays, now fill-only.
- **Hints**: `|` separators at 30 % of the ink, `~off~` at 35 % of the ink, `^on^` in `UI_HOT`.
- **List row**: square-edged. Selected: `UI_ROW` fill, a 2 px `UI_HOT` bar on the left, label in
  `UI_HOT`. Others: `UI_BODY`.
- **List** (`UiListDraw`): the panel, a Pixelify title, the count in `UI_DIM`, the filter field
  (`UI_INSET`, `UI_LINE` outline, placeholder `UI_FAINT`, cursor `UI_HOT`), rows, a 2 px scroll
  bar (track `UI_LINE`, thumb `UI_DIM`), a `UI_LINE` rule and the footer hints in `UI_DIM`.
- **Button** (`UiButton`): radius 6, no border. Rest: `UI_RAISED`, text `UI_INK`. Hover:
  `UI_HOVER`, text `UI_HOT`. On / held / keyboard-selected: `UI_INK` fill, `UI_ON_INK` text.
- **Field** (`UiNumberField` / `UiTextField`): `UI_INSET`, a 1 px outline: `UI_LINE` at rest,
  `UI_FAINT` on hover, `UI_DIM` while typing; selection `UI_SELECT`, cursor `UI_HOT`.
- **Slider**: a 2 px track (`UI_LINE`), its filled part `UI_DIM`, a 12 px disc thumb (`UI_INK`;
  hover and held `UI_HOT`), which glows while held (the halo sprite in white, 45 %).
- **Swatch**: selected: 2 px `UI_HOT` outline. Otherwise 1 px `UI_LINE`, `UI_DIM` on hover.
- **Toast** (`UiToast`): the panel with its halo, faded as a whole. A status toast (error, done)
  adds a mark bar in its colour; the text stays Outfit (sentences with paths read badly in a
  pixel face).
- **HUD lines** (`UiHudText`): the `UI_BACKDROP` strip, text `UI_TEXT`. The mode name (EDIT,
  DRAW, SNIP, GRAB …) and status lines (coordinator, requests, edit failures) are marks
  (`UiMark`, `UiHudMark`): Outfit with a 2 px bar in the mode or status colour. Pixelify's one
  size (39 px line) is too tall for the 20 px HUD lines.
- **A HUD line that starts with a mark** (EDIT MODE, GRAB / ROTATE / SCALE, DRAW): `UiHudMark`
  returns how far its strip reaches, and the hints or text go on from there, so the two strips
  join.
- **Per surface:**
  - The selection line ("selected: …") and the volumes line are marks in the outline colours
    they describe (`SEL_COLOR`, `VOLUME_COLOR`). The SNIP and VOLUME help bars do the same with
    `REGION_COLOR` / `VOLUME_COLOR`, so a bar matches its frames.
  - Launcher lines: queued and running in `UI_TEXT`, question `UI_ASK`, done `UI_OK` (fading),
    failed `UI_WARN`, cancelled and "coordinator not running" `UI_DIM`.
  - Errors (a broken map, failed edits, refused strokes) are `UI_ERROR` marks.
  - The keybindings panel's section tabs are buttons on a dark base: the current one is "on".
  - The inspector panel's title shortens to "Render inspector" when the long one doesn't fit
    in the pixel face.
- **Request-image labels** (`UiTag`): the region and volume number tabs keep today's filled pill
  in `REGION_COLOR` / `VOLUME_COLOR`, because `view-marked.png` is read by the request
  subagents. They are pixel-identical to today's.

## 5. Motion (optional phase)

Hover fades from rest to `UI_HOT` over 150 ms; toasts fade in over 150 ms and out over 1 s, as
today. These are driven by `GetFrameTime`. In `--shot` runs every animation snaps to its end
state, so two runs give the same image. The text cursor blinks with `GetTime`, as today.

## 6. Translation from the site

| Site | Editor |
|---|---|
| translucent black panels `#000000b9` / `71` / `61` | kept: `UI_PANEL`, `UI_SCRIM`, `UI_INSET`; the HUD backdrop is darker (140) for bright skies |
| `backdrop-blur` | **dropped**: it needs an extra pass over the scene every frame; translucency plus the halo stand in |
| `0 0 10px 1px rgba(0,0,0,.5)` shadow instead of a border | kept: a baked halo 9-slice (8 quads) |
| white text at alpha tiers | kept, the same values |
| Pixelify Sans for hero, nav and headings | adapted: panel titles only, one crisp size (×3) |
| Outfit body | kept (already the editor's face) |
| `pl-2 border-l-2` coloured badges | kept as the status mark, the only coloured UI element |
| category border colours | adapted: the edit and draw modes' HUD bars |
| `rounded-md` / `rounded-lg`, `rounded-full` icon buttons | radius 6 for panels; discs for slider thumbs |
| hover `opacity-60` / dim → white, 200–300 ms | adapted: dim → `UI_HOT`; the fade is optional (§5) |
| `active:scale-90` | **dropped**: scaling text at non-integer sizes blurs it; pressed = white fill |
| top progress bar `h-1`, white 73 % | kept for anything that shows progress: a 4 px bar, white 73 % |
| hidden scrollbars | adapted: a 2 px scroll bar (a list needs to show that it scrolls) |
| animated pink hero glow (shader) | **dropped**: the editor has no hero; a faint pink glow on the prompt box is an option, not shipped |
| custom cursor canvas | **dropped** (the site's is empty too); the crosshair stays |
| light theme | not in this round |
