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
    # A manifest without the attribute (Minecraft: Story Mode): add it.
    if ! grep -q 'android:debuggable=' "$TREE/AndroidManifest.xml"; then
        sed -i '0,/<application /s//<application android:debuggable="true" /' "$TREE/AndroidManifest.xml"
    fi
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
else
    LIB_SRC=host/build-android/libabhost.so
fi
# SANDBOX=1: the API 26 build for the sandbox (build-lib.sh --sandbox [--diag]).
if [ "${SANDBOX:-0}" = 1 ]; then
    LIB_SRC="${LIB_SRC%/libabhost.so}-sandbox/libabhost.so"
fi
if [ ! -f "$LIB_SRC" ]; then
    FLAGS=""
    if [ "${SANDBOX:-0}" = 1 ]; then FLAGS="$FLAGS --sandbox"; fi
    if [ "$DIAG" = 1 ]; then FLAGS="$FLAGS --diag"; fi
    echo "build-apk: needs $LIB_SRC" >&2
    echo "  run: scripts/build-lib.sh$FLAGS" >&2
    exit 1
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
# host.stub_libs: names Java loads besides the host's own; each gets an empty
# arm64 library so System.loadLibrary finds it (the guest's code for that
# name runs inside the host).
CLANG="$(ls tools/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/clang 2>/dev/null | head -1)"
for l in $("${PROF[@]}" host.stub_libs 2>/dev/null || true); do
    [ -x "$CLANG" ] || { echo "build-apk: no NDK clang for the stub libraries" >&2; exit 1; }
    "$CLANG" --target=aarch64-linux-android24 -shared -nostdlib -Wl,-z,max-page-size=16384 -x c /dev/null \
        -o "$(dirname "$LIB_DST")/$l"
    echo "  stub $l"
done
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

# A title that is another version of an installed title's package (Rio 1.0.0
# and Rio) gets its own package name, so the two install side by side and
# never share a data directory: apk.package is the name the port installs,
# apk.original_package the one in the original manifest. Only the manifest's
# package attribute changes; the R classes keep their Java package, and the
# check below refuses a manifest that depends on the name anywhere else
# (relative component names, authorities, permissions). apk.label, when set,
# is the launcher name (two icons with one name cannot be told apart).
ORIG_PKG="$("${PROF[@]}" apk.original_package 2>/dev/null || true)"
LABEL="$("${PROF[@]}" apk.label 2>/dev/null || true)"
if [ -n "$ORIG_PKG" ] && [ "$ORIG_PKG" != "$PKG" ]; then
    echo "== package name =="
    python3 - "$TREE/AndroidManifest.xml" "$ORIG_PKG" "$PKG" << 'PYEOF'
import re
import sys

path, orig, new = sys.argv[1:4]
text = open(path).read()
m = re.search(r'(<manifest\b[^>]*\bpackage=")([^"]*)(")', text)
if not m:
    sys.exit("build-apk: no package attribute in the manifest")
if m.group(2) not in (orig, new):
    sys.exit(f"build-apk: manifest package is {m.group(2)}, expected {orig}")
rest = text[:m.start(2)] + text[m.end(2):]
# Android resolves a component name that starts with a dot, or has no dot at
# all, against the package: such a class would no longer be found.
relative = [n for tag, n in re.findall(
    r'<(application|activity|activity-alias|service|receiver|provider)\b[^>]*?\bandroid:name="([^"]+)"', rest)
    if n.startswith(".") or "." not in n]
if orig in rest or relative:
    sys.exit("build-apk: the manifest uses the package name outside the package attribute "
             f"(relative component names {relative}, an authority or a permission): "
             "renaming it needs more than this")
open(path, "w").write(text[:m.start(2)] + new + text[m.end(2):])
print(f"manifest package {m.group(2)} -> {new}")
PYEOF
    # Resource XML can name the package too (custom attributes:
    # xmlns ".../apk/res/<package>"); those would resolve to nothing.
    if grep -rlF "apk/res/$ORIG_PKG" "$TREE/res" >/dev/null 2>&1; then
        echo "build-apk: resource XML refers to apk/res/$ORIG_PKG: renaming the package needs more than this" >&2
        grep -rlF "apk/res/$ORIG_PKG" "$TREE/res" | head -5 >&2
        exit 1
    fi
fi
if [ -n "$LABEL" ]; then
    # Every locale's app_name, or a device in that language shows the old one.
    python3 - "$TREE/res" "$LABEL" << 'PYEOF'
import glob
import re
import sys
from xml.sax.saxutils import escape

res, label = sys.argv[1:3]
done = 0
for path in sorted(glob.glob(res + "/values*/strings.xml")):
    text = open(path).read()
    new, n = re.subn(r'(<string name="app_name">)[^<]*(</string>)',
                     lambda m: m.group(1) + escape(label) + m.group(2), text)
    if n:
        open(path, "w").write(new)
        done += n
print(f"app label -> {label} ({done} app_name string(s))")
PYEOF
    # The launcher shows the manifest's labels, which may be text of their
    # own rather than @string/app_name (Seasons 3.1.1: android:label="Angry
    # Birds"). Every literal label equal to the application's becomes the
    # new one; a label pointing at a resource follows the strings above.
    python3 - "$TREE/AndroidManifest.xml" "$LABEL" << 'PYEOF'
import re
import sys
from xml.sax.saxutils import quoteattr

path, label = sys.argv[1:3]
text = open(path).read()
m = re.search(r'<application\b[^>]*?\bandroid:label="([^"]*)"', text)
if m and not m.group(1).startswith("@"):
    old = m.group(1)
    text, n = re.subn(r'\bandroid:label="' + re.escape(old) + '"', "android:label=" + quoteattr(label), text)
    open(path, "w").write(text)
    print(f"manifest label {old!r} -> {label!r} ({n} place(s))")
elif not m:
    sys.exit("build-apk: the manifest's <application> has no android:label")
PYEOF
fi

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
    # The ORIGINAL app's targetSdkVersion (ours may be raised to install):
    # the guest was built against the libc behaviour of that target (bionic
    # sem_wait returns EINTR only from target 24 on). Read from the original.
    ORIG_APK="games/$GAME/$("${PROF[@]}" apk.original)"
    ORIG_TSDK="$("$BT/aapt" dump badging "$ORIG_APK" | sed -n "s/^targetSdkVersion:'\([0-9]*\)'.*/\1/p")"
    [ -n "$ORIG_TSDK" ] || { echo "build-apk: no targetSdkVersion in $ORIG_APK" >&2; exit 1; }
    printf '%s\n' "$ORIG_TSDK" > "$GDIR/target_sdk"
    sed -i "\|^[^-]*- *$GDIRREL/target_sdk$|d" "$TREE/apktool.yml"
    sed -i "s|^doNotCompress:|doNotCompress:\n- $GDIRREL/target_sdk|" "$TREE/apktool.yml"
    echo "guest target sdk: $ORIG_TSDK -> $GDIRREL/target_sdk"
    # Shared JIT code cache (host.jitshare): "on" (default) or "off" for a
    # guest with a JIT of its own (Mono); the host reads it at boot.
    JITSHARE="$("${PROF[@]}" host.jitshare 2>/dev/null || echo on)"
    case "$JITSHARE" in
        on|off) ;;
        *) echo "build-apk: host.jitshare must be on or off, not '$JITSHARE'" >&2; exit 1 ;;
    esac
    printf '%s\n' "$JITSHARE" > "$GDIR/jitshare"
    sed -i "\|^[^-]*- *$GDIRREL/jitshare$|d" "$TREE/apktool.yml"
    sed -i "s|^doNotCompress:|doNotCompress:\n- $GDIRREL/jitshare|" "$TREE/apktool.yml"
    echo "jitshare: $JITSHARE -> $GDIRREL/jitshare"
    # host.private_sdcard = "on": shared storage is the app's files/sdcard
    # (a title whose data lives there); default off, nothing packaged.
    if [ "$("${PROF[@]}" host.private_sdcard 2>/dev/null || echo off)" = "on" ]; then
        printf 'private\n' > "$GDIR/sdcard"
        sed -i "\|^[^-]*- *$GDIRREL/sdcard$|d" "$TREE/apktool.yml"
        sed -i "s|^doNotCompress:|doNotCompress:\n- $GDIRREL/sdcard|" "$TREE/apktool.yml"
        echo "shared storage: private -> $GDIRREL/sdcard"
    fi
    # Several guest libraries (guest.deps): every one is checked, and
    # guest.libs lists them in guest.load_order as "<sha256> <name>", the
    # primary (guest.lib) marked; the host loads them in that order.
    DEPS="$("${PROF[@]}" guest.deps 2>/dev/null || true)"
    if [ -n "$DEPS" ]; then
        DSHAS=($("${PROF[@]}" guest.deps_sha256))
        declare -A SHA_OF=(["$GUEST_LIB"]="$GUEST_SHA")
        i=0
        for l in $DEPS; do
            GOT="$(sha256sum "$GDIR/$l" | cut -d' ' -f1)"
            if [ "$GOT" != "${DSHAS[$i]}" ]; then
                echo "build-apk: guest $l mismatch: want ${DSHAS[$i]} got $GOT" >&2
                exit 1
            fi
            SHA_OF["$l"]="$GOT"
            i=$((i + 1))
        done
        # guest.java_loads: the libraries Java loads itself (each runs its
        # JNI_OnLoad), marked "java".
        JAVA_LOADS=" $("${PROF[@]}" guest.java_loads 2>/dev/null || true) "
        : > "$GDIR/guest.libs"
        for l in $("${PROF[@]}" guest.load_order); do
            [ -n "${SHA_OF[$l]:-}" ] || { echo "build-apk: load_order names unknown $l" >&2; exit 1; }
            FLAGS=""
            [ "$l" = "$GUEST_LIB" ] && FLAGS=" primary"
            case "$JAVA_LOADS" in *" $l "*) FLAGS="$FLAGS java" ;; esac
            printf '%s %s%s\n' "${SHA_OF[$l]}" "$l" "$FLAGS" >> "$GDIR/guest.libs"
        done
        sed -i "\|^[^-]*- *$GDIRREL/guest\.libs$|d" "$TREE/apktool.yml"
        sed -i "s|^doNotCompress:|doNotCompress:\n- $GDIRREL/guest.libs|" "$TREE/apktool.yml"
        echo "guest libraries: $(wc -l < "$GDIR/guest.libs") -> $GDIRREL/guest.libs"
    fi
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
