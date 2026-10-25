# monolith: editor fonts

The fonts the editor draws its UI with (HUD, prompt box, panels). They are compiled into the
binary (`src/font_outfit.h`, generated from these files by `tools/embed_font.py`), so nothing
here is read at run time. Editor only: the scene runtime, `--export` packages and the examples
use raylib's built-in font and contain none of these files.

Fonts are the one exception to the CC0 / CC-BY rule for assets: the **SIL Open Font License 1.1**
is allowed for editor fonts (not for anything in a map package). It permits embedding in a
program as long as the font isn't sold on its own and the licence travels with it: `build.sh`
copies `outfit/OFL.txt` next to the binary as `OFL-Outfit.txt`.

| File | Source | Author(s) | Licence | Notes |
|---|---|---|---|---|
| `outfit/Outfit-Regular.ttf` | [Outfitio/Outfit-Fonts](https://github.com/Outfitio/Outfit-Fonts) @ `9027738`, `fonts/ttf/` | The Outfit Project Authors (Smartsheet Inc, Rodrigo Fuenzalida) | SIL OFL 1.1 (`outfit/OFL.txt`) | Version 1.100, static instance; sidecar `outfit/Outfit-Regular.ttf.asset.md` |
| `outfit/Outfit-SemiBold.ttf` | same | same | SIL OFL 1.1 (`outfit/OFL.txt`) | Version 1.100, static instance; sidecar `outfit/Outfit-SemiBold.ttf.asset.md` |
