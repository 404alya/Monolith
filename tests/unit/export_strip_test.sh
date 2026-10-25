#!/usr/bin/env bash
# tests/unit/export_strip_test.sh [export dir] -- the runtime an export ships has no map-text parser:
# no tokenizer or statement parser, no model-file reference scan, no compiler, no number parsing
# (strtod / strtof / strtol / sscanf ...), no ".map" file lookup, no MONOLITH_EDITOR left
# (docs/compiled-map.md, "The two builds"). Without an argument it exports tests/models with
# build/linux/monolith into build/strip-test/ (moved to the trash afterwards); with one it checks
# that export folder. Exit 1 on any hit.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="${1:-}"
MADE=""
if [[ -z "$DIR" ]]; then
  [[ -x "$ROOT/build/linux/monolith" ]] || { echo "export_strip_test: run ./build.sh first" >&2; exit 1; }
  mkdir -p "$ROOT/build/strip-test"
  DIR="$ROOT/build/strip-test/export-$(date +%Y%m%d%H%M%S)"
  "$ROOT/build/linux/monolith" --export "$DIR" --map "$ROOT/tests/models" >/dev/null 2>&1 ||
    { echo "export_strip_test: the export failed" >&2; exit 1; }
  MADE="$DIR"
fi
FILES=("$DIR/monolith_scene.c" "$DIR/monolith_scene.h")
for f in "${FILES[@]}"; do [[ -f "$f" ]] || { echo "export_strip_test: $f missing" >&2; exit 1; }; done

FAIL=0
Forbid() {  # Forbid <what> <grep -E pattern> [-w]
  local what="$1" pat="$2" hits
  hits="$(grep -nE ${3:-} "$pat" "${FILES[@]}")"
  if [[ -n "$hits" ]]; then echo "FAIL $what:"; echo "$hits" | sed 's/^/    /'; FAIL=1
  else echo "ok   no $what"; fi
}
Forbid "tokenizer / statement parser" 'Tokenize|ParseText|ParseStatement|ParsePlacement|ParsePrimitive|ParseLight|ParseModel|ParseMaterial|ParseEnv|ParseValue|ParseFloat|ParseInt|ParseScene|AddStatement|Parser \*' -w
Forbid "model-file reference scan" 'GltfUriDeps|ObjDeps|MtlDeps|MtlFileDeps|ModelDeps|ModelRef|RefResolve|JsonString|MS_CheckModelReferences' -w
Forbid "compiler" 'WriteCompiled|MS_Compile|CmWriter' -w
Forbid "number parsing" 'strto(d|f|l|ld|ul|ull|ll)|ato(f|i|l)|sscanf' -w
Forbid "\".map\" file lookup" '"\.map"'
Forbid "MONOLITH_EDITOR" 'MONOLITH_EDITOR'
[[ $FAIL == 0 ]] && echo "export_strip_test: OK ($DIR)" || echo "export_strip_test: FAILED ($DIR)"
if [[ -n "$MADE" ]]; then gio trash "$MADE" 2>/dev/null || echo "export_strip_test: move $MADE to the trash by hand" >&2; fi
exit $FAIL
