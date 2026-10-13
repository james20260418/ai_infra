#!/usr/bin/env bash
# Fetch the CC0 texture atlases used by build_tree.py.
#
# All sources are CC0 (public domain, no attribution, commercial use OK).
# Source website: https://ambientcg.com  ("get?file=<ID>_<res>-<fmt>.zip")
#
# Layout produced (relative to the destination, default ./textures):
#   leaf001/    Leaf001_1K-JPG_{Color,Opacity}.jpg      (single tree leaf, 2 per card)
#   leafset024/ LeafSet024_1K-JPG_{Color,Opacity}.jpg   (9-leaf cluster patch)
#   bark001/    Bark001_1K-JPG_{Color,...}.jpg          (tree bark)
#
# Usage:
#   bash fetch_textures.sh [dest_dir]     # default: ./textures
set -euo pipefail

DEST="${1:-./textures}"
BASE="https://ambientcg.com/get?file="
mkdir -p "$DEST"

fetch() {
    local id="$1" sub="$2"
    echo "== $id -> $DEST/$sub =="
    mkdir -p "$DEST/$sub"
    curl -sSL --max-time 120 -o "/tmp/${id}_1K-JPG.zip" "${BASE}${id}_1K-JPG.zip"
    unzip -o -q "/tmp/${id}_1K-JPG.zip" -d "$DEST/$sub"
    rm -f "/tmp/${id}_1K-JPG.zip"
}

fetch "Leaf001"    "leaf001"
fetch "LeafSet024" "leafset024"
fetch "Bark001"    "bark001"

echo
echo "Done. Textures in: $DEST"
echo "Now build, e.g.:"
echo "  blender -b --python build_tree.py -- japanese_maple --textured --textures $DEST"
