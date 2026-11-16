# `src/transform.c`: the transform modal and the rotate gizmo

The edit mode's transform modal: G / ALT+R / S grab, rotate and scale the selection, and the rotate gizmo ([live-editing.md](../live-editing.md) 7, 11.10).

## The modal

G / ALT+R / S in edit mode start it on the selection. The camera is frozen and the cursor hidden, and the raw mouse motion (SHIFT: ten times slower) drives the value every frame:

| Kind | What the mouse does |
|---|---|
| grab | the cursor's motion on the plane through the pivot facing the camera, on the locked axis (closest point between the cursor's ray and the axis line) or on the locked plane |
| rotate | see below |
| scale | the ratio of the cursor's distance to the pivot's screen position now and at the start: uniform, one axis (axis lock) or two (plane lock) |

**Rotate** turns about the view axis, or the locked axis (a plane lock: the axis normal to it).

- An axis across the view (a vertical one seen from eye height) follows the motion along the screen direction the near side moves in (0.5 deg / px: drag right, the side facing you goes right).
- An axis pointing at the camera takes the angle the cursor sweeps around the pivot's screen position (or the horizontal motion when the cursor starts near the pivot).
- Boxes turn about any axis as far as `MapEditPreviewTransform` accepts the turn (what it refuses is named in red and stays).
- A value that changes nothing (a square box turned 90 degrees about its centre, any box turned 180) is named in red, and LMB / ENTER write nothing.

### Locks, typed values and snapping

- X / Y / Z lock an axis, SHIFT+X / Y / Z a plane (the other two axes). The same key again uses the active object's own axes (local: the first selected box, block or model); a third time unlocks.
- Digits, `.`, `-` (toggles the sign) and BACKSPACE type the value (metres along the locked axis or the view's right, degrees, the factor); typed values are exact.
- CTRL held snaps: the pivot onto the N grid, the angle step (SHIFT+N: 1 / 5 / 15 / 45 / 90 degrees), factors to 0.1.
- G / R / S switch kind (from the original transforms, with the lock and the typed value cleared).
- LMB / ENTER commit one edit (one `EditOp` through `MapEditApply`: one undo step); RMB / ESC cancel (`MS_EditPreviewEnd`: everything back exactly).

## The rotate gizmo

Shown while no modal runs and the selection has a box, block or model; 2D, a constant 120 px: three axis rings (their front halves; global axes or, with L, the active object's), a white view ring at 1.15 x and a grey trackball disc.

LMB on a part starts the same modal with a source:

- a ring turns by the angle the cursor sweeps in its plane (edge-on, under 15 degrees: the turntable);
- the view ring is the dial;
- the trackball turns about the axis perpendicular to the drag on screen (0.5 deg / px, one axis-angle `EDIT_ROTATE`).

A drag (moved more than 4 px) commits on release unless a value was typed; a click, or a drag with typed text, stays open as the modal (the mouse still turns; LMB / ENTER commit, RMB / ESC cancel).

## Pivot and preview

The pivot is the centre of the selection's world bounds (rounded to 0.1 mm), or one model's origin when it is the whole selection.

Each frame the op is built as for the commit, and every object's transform comes from `MapEditPreviewTransform` (the same math `mapedit.c` writes with), so the preview shows what the commit writes. Objects that can't follow (stairs and waffles turned or scaled, turns `MapEditPreviewTransform` refuses, directional lights moved) keep their transform and are named in red in the HUD; lights ignore rotate and scale. Mouse values are rounded to what the HUD shows (mm, 0.01 degree, 0.001) before they are written.
