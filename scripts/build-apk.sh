#!/bin/bash
# Package one game into a signed APK (LOCAL only -- no ssh/scp/pm here).
#
#   scripts/build-apk.sh <game-id>          e.g. scripts/build-apk.sh rio
#
# Reads games/<id>/profile.toml for the package name and the guest-lib hash,
# expects the decoded tree at games/<id>/work/apk_prod (see prepare-tree.sh) and
# the shipping host lib at host/build-android/libabhost.so. DIAG=1 instead uses
# the counter-enabled host/build-android-diag/libabhost.so and writes
# apk_diag.apk (see build-lib.sh --diag).
#
# Signing: apksigner v1+v2+v3 with the shared key in keys/ (override KS=...).
# Set SIGN_SCHEME=v1 for the legacy jarsigner-only path.
set -euo pipefail

GAME="${1:?usage: build-apk.sh <game-id>   (see games/*/profile.toml)}"
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ"

PROF=(python3 scripts/profile.py "$GAME")
PKG="$("${PROF[@]}" apk.package)"
GUEST_LIB="$("${PROF[@]}" guest.lib)"
GUEST_SHA="$("${PROF[@]}" guest.sha256)"
TARGET_SDK="$("${PROF[@]}" apk.target_sdk)"

TREE="games/$GAME/work/apk_prod"
UNS="games/$GAME/work/apk_unsigned.apk"
OUT="games/$GAME/work/apk_signed.apk"
ALIGNED="games/$GAME/work/apk_aligned.apk"
# DIAG=1 builds a throwaway debuggable twin (run-as becomes possible) into
# apk_diag.apk without touching the prod tree or artifacts.
DIAG="${DIAG:-0}"
if [ "$DIAG" = 1 ]; then
    TREE="games/$GAME/work/apk_diag"
    UNS="games/$GAME/work/apk_diag_unsigned.apk"
    OUT="games/$GAME/work/apk_diag.apk"
    ALIGNED="games/$GAME/work/apk_diag_aligned.apk"
    rm -rf "$TREE"
    cp -a "games/$GAME/work/apk_prod" "$TREE"
    sed -i 's/android:debuggable="false"/android:debuggable="true"/' "$TREE/AndroidManifest.xml"
    if ! grep -q 'android:debuggable="true"' "$TREE/AndroidManifest.xml"; then
        echo "build-apk: diag: could not flip android:debuggable" >&2
        exit 1
    fi
fi
# The profile is the single source of truth for the ABIs and the file names
# (they used to be hardcoded here while the profile carried its own copies --
# two places to keep in sync).
HOST_ABI="$("${PROF[@]}" host.abi)"
HOST_LIB="$("${PROF[@]}" host.lib)"
GUEST_ABI="$("${PROF[@]}" guest.abi)"
if [ "$DIAG" = 1 ]; then
    LIB_SRC=host/build-android-diag/libabhost.so
    if [ ! -f "$LIB_SRC" ]; then
        echo "build-apk: DIAG=1 needs the counter-enabled library" >&2
        echo "  run: scripts/build-lib.sh --diag" >&2
        exit 1
    fi
else
    LIB_SRC=host/build-android/libabhost.so
fi
LIB_DST="$TREE/lib/$HOST_ABI/$HOST_LIB"
# OUT_NAME=apk_fill.apk writes the signed artifact under a distinct name
# without touching the prod/diag defaults.
if [ -n "${OUT_NAME:-}" ] && [ "$DIAG" != 1 ]; then
    OUT="games/$GAME/work/$OUT_NAME"
fi
GUEST="$TREE/assets/guestlib/$GUEST_ABI/$GUEST_LIB"
APKTOOL=tools/apktool_3.0.3.jar
BT=tools/android-sdk/build-tools/35.0.0
KS="${KS:-keys/debug.keystore}"
KS_ALIAS="${KS_ALIAS:-dbg}"
KS_PASS="${KS_PASS:-android}"
SIGN_SCHEME="${SIGN_SCHEME:-v123}"

for f in "$TREE" "$LIB_SRC" "$APKTOOL" "$KS"; do
    [ -e "$f" ] || { echo "build-apk: missing $f" >&2; exit 1; }
done

# The guest .so must be the pristine Rovio library, byte for byte. This is the
# cheap guard that catches a tree built from the wrong APK or a patched guest.
if [ -f "$GUEST" ]; then
    GOT="$(sha256sum "$GUEST" | cut -d' ' -f1)"
    if [ "$GOT" != "$GUEST_SHA" ]; then
        echo "build-apk: guest .so mismatch" >&2
        echo "  want $GUEST_SHA" >&2
        echo "  got  $GOT  ($GUEST)" >&2
        echo "  (the guest must never be patched; re-run prepare-tree.sh)" >&2
        exit 1
    fi
else
    echo "build-apk: warning: no guest .so at $GUEST (asset not verified)" >&2
fi

echo "== stripping host lib =="
llvm-strip "$LIB_SRC" -o "$LIB_DST"
# The profile's strip.libs: the guest's own libraries, if the tree still has
# them (Seasons links everything statically, so there is nothing to strip
# there). Stripped in place, when present.
for l in $("${PROF[@]}" strip.libs); do
    for d in "$TREE/lib/$GUEST_ABI" "$TREE/lib/armeabi"; do
        if [ -f "$d/$l" ]; then
            llvm-strip "$d/$l"
            echo "  stripped $d/$l"
        fi
    done
done

echo "== target sdk =="
# The profile is authoritative for the SDK levels. apktool v3 takes them from
# the DECODED manifest, not from apktool.yml's sdkInfo (which it only records),
# so the values are written into the manifest itself. Seasons shipped with
# targetSdk 22 and Android 14+ then refuses to install the package
# ("INSTALL_FAILED_DEPRECATED_SDK_VERSION: App package must target at least SDK
# version 24, but found 22"); the sibling titles already target 26.
MIN_SDK="$("${PROF[@]}" apk.min_sdk)"
python3 - "$TREE/AndroidManifest.xml" "$MIN_SDK" "$TARGET_SDK" << 'PYEOF'
import re
import sys

path, min_sdk, target_sdk = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read()
tag = f'<uses-sdk android:minSdkVersion="{min_sdk}" android:targetSdkVersion="{target_sdk}"/>'
if re.search(r"<uses-sdk\b[^>]*>", text):
    text = re.sub(r"<uses-sdk\b[^>]*/>", tag, text, count=1)
    print(f"manifest uses-sdk -> min {min_sdk} / target {target_sdk}")
else:
    cut = text.find(">", text.find("<manifest")) + 1
    text = text[:cut] + "\n    " + tag + text[cut:]
    print(f"manifest uses-sdk inserted (min {min_sdk} / target {target_sdk})")
open(path, "w").write(text)
PYEOF
# Keep apktool's record in sync with the manifest (it is metadata only).
sed -i "s/^\([[:space:]]*minSdkVersion:[[:space:]]*\).*/\1$MIN_SDK/" "$TREE/apktool.yml"
sed -i "s/^\([[:space:]]*targetSdkVersion:[[:space:]]*\).*/\1$TARGET_SDK/" "$TREE/apktool.yml"

echo "== apktool b =="
# Guest fingerprint: the packaging step writes the guest .so's SHA-256 next to
# it, and the harness verifies it at start-up. The Seasons and Space images
# carry no GNU build-id note, so the build-id check could never confirm their
# identity -- this digest does, per title, at runtime.
if [ -f "$GUEST" ]; then
    GDIR="$(dirname "$GUEST")"
    printf '%s\n' "$GUEST_SHA" > "$GDIR/guest.sha256"
    # The harness reads it straight out of the APK, so it must be STORED:
    # add its path (relative to the tree) to apktool's doNotCompress list.
    GREL="${GUEST#$TREE/}"                 # e.g. assets/guestlib/armeabi-v7a/libX.so
    GDIRREL="$(dirname "$GREL")"           # e.g. assets/guestlib/armeabi-v7a
    sed -i "\|^[^-]*- *$GDIRREL/guest\.sha256$|d" "$TREE/apktool.yml"
    sed -i "s|^doNotCompress:|doNotCompress:\n- $GDIRREL/guest.sha256|" "$TREE/apktool.yml"
    echo "guest fingerprint: $GUEST_SHA -> $GDIRREL/guest.sha256"
    # Network policy (REVIEW R-005): host.network = "offline" (default) or
    # "online". The host reads it from the APK at start; anything but
    # "online" keeps the engine off the network.
    NETWORK="$("${PROF[@]}" host.network 2>/dev/null || echo offline)"
    case "$NETWORK" in
        offline|online) ;;
        *) echo "build-apk: host.network must be offline or online, not '$NETWORK'" >&2; exit 1 ;;
    esac
    printf '%s\n' "$NETWORK" > "$GDIR/network"
    sed -i "\|^[^-]*- *$GDIRREL/network$|d" "$TREE/apktool.yml"
    sed -i "s|^doNotCompress:|doNotCompress:\n- $GDIRREL/network|" "$TREE/apktool.yml"
    echo "network: $NETWORK -> $GDIRREL/network"
fi
java -jar "$APKTOOL" b -o "$UNS" "$TREE" >/dev/null

# Drop any stale signatures from the tree/repack before aligning + signing.
zip -q -d "$UNS" 'META-INF/*' 2>/dev/null || true

echo "== zipalign =="
"$BT/zipalign" -f -p 4 "$UNS" "$ALIGNED"

case "$SIGN_SCHEME" in
v1)
    # Legacy path: v1 (JAR) only, SHA-256 digest/sig -- what shipped through
    # batch G. SHA-384 would make apksigner report DOES NOT VERIFY for
    # minSdk 16 (the platform JAR verifier only knows SHA-384 from API 18).
    cp -f "$ALIGNED" "$OUT"
    jarsigner -keystore "$KS" -storepass "$KS_PASS" -keypass "$KS_PASS" \
        -digestalg SHA-256 -sigalg SHA256withRSA "$OUT" "$KS_ALIAS" >/dev/null
    ;;
*)
    # Default: apksigner, v1 + v2 + v3. Same key -> same certificate identity as
    # the v1-only builds, so `pm install -r` still replaces them. apksigner
    # picks the v1 digest algorithm from the declared minSdk.
    "$BT/apksigner" sign \
        --ks "$KS" --ks-key-alias "$KS_ALIAS" \
        --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
        --v1-signing-enabled true --v2-signing-enabled true \
        --out "$OUT" "$ALIGNED"
    ;;
esac

rm -f "$UNS" "$ALIGNED"

echo "== verify =="
"$BT/apksigner" verify --print-certs -v "$OUT" 2>/dev/null |
    grep -E "^Verified using|^Number of signers|certificate DN|certificate SHA-256"

echo "lib md5 : $(md5sum "$LIB_DST" | cut -d' ' -f1)"
echo "apk     : $OUT ($(stat -c %s "$OUT") bytes)"
echo "package : $PKG"
