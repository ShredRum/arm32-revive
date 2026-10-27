#!/bin/bash
# Decode a pristine Rovio APK and apply the complete, verified port edit set.
#
#   scripts/prepare-tree.sh <game-id> [--force] [--tree DIR]
#
# The edits are the *minimal* set (PROVEN for rio by diffing a fresh decode of
# original/ against the shipped work/apk_prod -- nothing else differs):
#   1. lib/armeabi-v7a/<guest>.so -> assets/guestlib/armeabi-v7a/<guest>.so
#      (and every guest.deps library of a several-library title, e.g. Unity)
#   2. delete lib/armeabi-v7a/ and lib/x86/   (guest moved out; x86 unused)
#   3. copy seeds/ -> assets/seeds/
#   4. smali/com/rovio/fusionapp/Gen.smali: loaded lib name -> "abhost"
#      (skipped when tree.smali_lib_name_orig is "": the host then carries
#      the name Java loads, host.lib)
#   5. apktool.yml doNotCompress += the guest .so and the seeds
#      (the loader reads them STORED, so they must not be deflated)
# lib/arm64-v8a/libabhost.so is *not* created here -- build-apk.sh drops it in.
#
# The guest .so is only moved, never modified; its sha256 is checked against the
# profile afterwards.
set -euo pipefail

GAME="${1:?usage: prepare-tree.sh <game-id> [--force] [--tree DIR]}"
shift
FORCE=0
TREE=""
while [ $# -gt 0 ]; do
    case "$1" in
    --force) FORCE=1 ;;
    --tree)
        TREE="${2:?--tree needs a directory}"
        shift
        ;;
    *) echo "prepare-tree: unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ"

PROF=(python3 scripts/profile.py "$GAME")
ORIG_REL="$("${PROF[@]}" apk.original)"
GUEST_LIB="$("${PROF[@]}" guest.lib)"
GUEST_SHA="$("${PROF[@]}" guest.sha256)"
SMALI_ORIG="$("${PROF[@]}" tree.smali_lib_name_orig)"

ORIG="games/$GAME/$ORIG_REL"
TREE="${TREE:-games/$GAME/work/apk_prod}"
APKTOOL=tools/apktool_3.0.3.jar
SEEDS="games/$GAME/seeds"
GEN="smali/com/rovio/fusionapp/Gen.smali"

[ -f "$ORIG" ] || { echo "prepare-tree: no original APK at $ORIG" >&2; exit 1; }
[ -f "$APKTOOL" ] || { echo "prepare-tree: missing $APKTOOL" >&2; exit 1; }

if [ -e "$TREE" ] && [ "$FORCE" != "1" ]; then
    echo "prepare-tree: $TREE exists (use --force to rebuild)" >&2
    exit 1
fi
rm -rf "$TREE"

echo "== apktool d $ORIG =="
java -jar "$APKTOOL" d -f -o "$TREE" "$ORIG" >/dev/null

# A title with several guest libraries (Unity) lists the others in guest.deps.
GUEST_DEPS="$("${PROF[@]}" guest.deps 2>/dev/null || true)"

echo "== moving the guest .so into assets (stored) =="
mkdir -p "$TREE/assets/guestlib/armeabi-v7a"
for l in "$GUEST_LIB" $GUEST_DEPS; do
    mv "$TREE/lib/armeabi-v7a/$l" "$TREE/assets/guestlib/armeabi-v7a/$l"
done
rm -rf "$TREE/lib/armeabi-v7a" "$TREE/lib/x86"
mkdir -p "$TREE/lib/arm64-v8a"

echo "== seeds =="
if [ -d "$SEEDS" ]; then
    mkdir -p "$TREE/assets/seeds"
    cp -f "$SEEDS"/* "$TREE/assets/seeds/"
else
    echo "prepare-tree: note: no $SEEDS (fresh game: capture seeds on first run)"
fi

if [ -z "$SMALI_ORIG" ]; then
    echo "== no smali edit (the host carries the name Java loads) =="
else
echo "== patching $GEN ($SMALI_ORIG -> abhost) =="
[ -f "$TREE/$GEN" ] || {
    echo "prepare-tree: $GEN not found -- check tree.smali_lib_name_orig / the smali layout" >&2
    exit 1
}
python3 - "$TREE/$GEN" "$SMALI_ORIG" <<'PY'
import re, sys
path, orig = sys.argv[1], sys.argv[2]
src = open(path, encoding="utf-8").read()
pat = re.compile(r'(const-string\s+v\d+,\s*)"' + re.escape(orig) + r'"')
new, n = pat.subn(r'\1"abhost"', src)
if n == 0:
    sys.exit(f"prepare-tree: no const-string \"{orig}\" in {path}")
open(path, "w", encoding="utf-8").write(new)
print(f"patched {n} occurrence(s)")
PY
fi

echo "== apktool.yml doNotCompress =="
python3 - "$TREE/apktool.yml" "$SEEDS" "$TREE/assets/seeds" "$GUEST_LIB" $GUEST_DEPS <<'PY'
import os, sys
yml, seeds_dir, tree_seeds = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(yml, encoding="utf-8").read().splitlines()
entries = [f"assets/guestlib/armeabi-v7a/{g}" for g in sys.argv[4:]]
if os.path.isdir(tree_seeds):
    entries += [f"assets/seeds/{n}" for n in sorted(os.listdir(tree_seeds))]
add = [f"- {e}" for e in entries if f"- {e}" not in lines]
if not add:
    print("already present")
    sys.exit(0)
if "doNotCompress:" in lines:
    at = lines.index("doNotCompress:")
    # insert right after the key so the list stays contiguous
    lines[at + 1:at + 1] = add
else:
    lines.append("doNotCompress:")
    lines += add
open(yml, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print(f"added {len(add)} entr(ies)")
PY

echo "== checks =="
GOT="$(sha256sum "$TREE/assets/guestlib/armeabi-v7a/$GUEST_LIB" | cut -d' ' -f1)"
[ "$GOT" = "$GUEST_SHA" ] || {
    echo "prepare-tree: guest sha256 mismatch: want $GUEST_SHA got $GOT" >&2
    exit 1
}
echo "guest sha256 ok: $GOT"
if [ -n "$GUEST_DEPS" ]; then
    i=0
    SHAS=($("${PROF[@]}" guest.deps_sha256))
    for l in $GUEST_DEPS; do
        GOT="$(sha256sum "$TREE/assets/guestlib/armeabi-v7a/$l" | cut -d' ' -f1)"
        [ "$GOT" = "${SHAS[$i]}" ] || {
            echo "prepare-tree: $l sha256 mismatch: want ${SHAS[$i]} got $GOT" >&2
            exit 1
        }
        echo "guest $l sha256 ok"
        i=$((i + 1))
    done
fi
echo "tree ready: $TREE"
