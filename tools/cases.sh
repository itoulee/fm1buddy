#!/bin/sh
# The case as STLs, in build/case/: fm1buddy-case-BOARD[-battery]-PART.stl (BOARD: s3 for the 2.8", s3-35, s3-4).
# With --png, also the pictures the installer page shows (site/case/BOARD.png, background made transparent by
# png_key.py; kept in git: rendering pictures needs a screen, which CI hasn't)
#   tools/cases.sh            needs openscad (a 2025+ snapshot renders in seconds), or:
#   docker run --rm -v "$PWD:/w" -w /w openscad/openscad:dev.2026-09-28 tools/cases.sh
set -e
cd "$(dirname "$0")/.."
out=build/case
mkdir -p "$out" site/case
backend=
openscad --help 2>&1 | grep -q -- --backend && backend=--backend=manifold
for board in 2.8 3.5 4; do
  case $board in
    2.8) id=s3 ;;
    3.5) id=s3-35 ;;
    4) id=s3-4 ;;
  esac
  for battery in false true; do
    name=$id
    [ "$battery" = true ] && name=$id-battery
    for part in base lid; do
      openscad $backend -q -D "board=\"$board\"" -D "battery=$battery" -D "part=\"$part\"" \
        --export-format=binstl -o "$out/fm1buddy-case-$name-$part.stl" case/fm1buddy-case.scad
    done
    echo "$name"
  done
  if [ "$1" = --png ]; then
    openscad $backend -q -D "board=\"$board\"" -D 'part="preview"' --render --imgsize=900,700 --viewall --autocenter \
      --camera=0,0,0,55,0,25,0 --colorscheme=Tomorrow -o "$out/$id.png" case/fm1buddy-case.scad
    python3 -I tools/png_key.py "$out/$id.png" "site/case/$id.png"
  fi
done
