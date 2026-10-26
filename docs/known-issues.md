# Known issues

Open questions that need a decision, and what automated checks can't cover. Limitations by design are in [architecture.md](architecture.md) §6, GPU memory costs in §7.

## Open decisions
1. **R3D's contrast step clips dark values to black.** `color.contrast` is applied in linear space after tonemapping (`submodules/r3d/shaders/post/output.frag`, `Adjustments()`): everything below 0.5·(1−1/c) becomes pure black. Fixing it in R3D (contrast around mid-grey in a perceptual space) would change the default map's look, so maps keep contrast 1 where it matters instead.
2. **R3D texture path overflow:** `r3d_importer_texture.c` joins the model folder and the texture path into a 256-byte stack buffer without a bounds check. The map loader rejects joins that would reach 256 bytes, so maps can't trigger it; R3D itself is unpatched.
3. **An assimp leak** of about 1.4 KB when a glTF buffer file is shorter than declared, in assimp's own glTF code; not patched.
4. **Not checked yet:** FBX / DAE external references; symlinks inside a package that point outside it.
5. An i3 / sway `for_window` rule for `monolith-claude:` terminals (avoids a brief flash before the window moves to its workspace); the Quaternius licence policy; the medium / low shadow map size and the low preset's render scale.

## Only testable by hand

 snip mode (C, drag, Shift+drag, Esc / right click / C, Enter); the edit mode (M: arrows / PgUp / PgDn with and without Ctrl, N, R / Shift+R, Delete, Ctrl+D, [ ] and I, Ctrl+I and its id panel (typing, Enter, Esc, Backspace), Ctrl+Z / Ctrl+Shift+Z, the warning while a request runs on the map; its operations themselves are covered by `--apply-edits`); closing request terminals (K, Shift+K, Ctrl+K n); the F2 panel (presets, rows, Reset, a restart to check the settings stick); walking through the maps (collision, passing under tree crowns, steps).
