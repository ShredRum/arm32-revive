#!/bin/bash
# Fast path for host-only changes: put the current host/build-android/libabhost.so
# into an already built games/<id>/work/apk_signed.apk, then re-align and re-sign.
# ~10 s instead of a full apktool rebuild. Use build-apk.sh when anything other
# than the host library changed (smali, assets, seeds, manifest).
#
#   scripts/swap-lib.sh <game-id> [<game-id>...]
set -euo pipefail
cd "$(dirname "$0")/.."
BT=tools/android-sdk/build-tools/35.0.0
KS="${KS:-keys/debug.keystore}"
KS_ALIAS="${KS_ALIAS:-dbg}"
KS_PASS="${KS_PASS:-android}"
LIB_SRC=host/build-android/libabhost.so
[ -f "$LIB_SRC" ] || { echo "swap-lib: $LIB_SRC missing (run scripts/build-lib.sh)" >&2; exit 1; }
for G in "$@"; do
    APK="games/$G/work/apk_signed.apk"
    [ -f "$APK" ] || { echo "swap-lib: $APK missing (run scripts/build-apk.sh $G once)" >&2; exit 1; }
    # The packaged name is the profile's host.lib (libmain.so for a title
    # whose Java loads "main"); a fixed libabhost.so left the old host in place.
    HL=$(python3 scripts/profile.py "$G" host.lib)
    TMP=$(mktemp -d)
    mkdir -p "$TMP/lib/arm64-v8a"
    llvm-strip "$LIB_SRC" -o "$TMP/lib/arm64-v8a/$HL"
    cp "$APK" "$TMP/in.apk"
    # Replace the library (stored uncompressed like the original build) and
    # drop the old signature files; apksigner writes fresh ones.
    (cd "$TMP" && zip -q -0 in.apk "lib/arm64-v8a/$HL" && zip -q -d in.apk 'META-INF/*' >/dev/null 2>&1 || true)
    "$BT/zipalign" -f -p 4 "$TMP/in.apk" "$TMP/aligned.apk"
    "$BT/apksigner" sign --ks "$KS" --ks-key-alias "$KS_ALIAS" \
        --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
        --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true \
        --out "$APK" "$TMP/aligned.apk"
    rm -rf "$TMP"
    echo "$G: $(unzip -p "$APK" "lib/arm64-v8a/$HL" | md5sum | cut -c1-32)"
done
