#!/bin/bash
# LOCAL half of the deploy: strip the host lib into the decoded tree, rebuild the
# APK, drop META-INF, sign it, and print md5 + the exact device commands.
# Deliberately does NOT touch the device (no ssh/scp/pm here): the device half is
# printed for a human to run, so a broken local step can never half-install.
#
# Layout it expects (see AGENTS.md HANDOFF):
#   work/apk_prod/                      decoded APK tree (apktool d) with our libs
#   work/apk_prod_signed.apk            output
#   work/gadget/debug.keystore          alias dbg, store/key pass "android"
#   tools/apktool_3.0.3.jar             tools/ is gitignored -> keep it locally
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ"

APK_IN=work/apk_prod
APK_OUT=work/apk_prod_signed.apk
LIB_SRC=host/build-android/libabhost.so
LIB_DST=work/apk_prod/lib/arm64-v8a/libabhost.so
APKTOOL=tools/apktool_3.0.3.jar
KS=work/gadget/debug.keystore
DEV="u0_a41@192.168.1.202"
APK_NAME=abprod.ab.apk

for f in "$APK_IN" "$LIB_SRC" "$APKTOOL" "$KS"; do
    if [ ! -e "$f" ]; then
        echo "deploy: missing $f" >&2
        exit 1
    fi
done

llvm-strip "$LIB_SRC" -o "$LIB_DST"
java -jar "$APKTOOL" b -o work/apk_prod_unsigned.apk "$APK_IN" >/dev/null
cp -f work/apk_prod_unsigned.apk "$APK_OUT"
zip -q -d "$APK_OUT" 'META-INF/*' || true
jarsigner -keystore "$KS" -storepass android -keypass android "$APK_OUT" dbg >/dev/null

MD5=$(md5sum "$LIB_DST" | cut -d' ' -f1)
echo "stripped lib md5: $MD5"
echo "apk: $(stat -c %s "$APK_OUT") bytes"

cat <<EOF

--- device half (run by hand) -------------------------------------------
scp -P 8022 "$APK_OUT" $DEV:$APK_NAME
ssh -p 8022 $DEV "sudo pm install -r /data/data/com.termux/files/home/$APK_NAME; pm path com.rovio.angrybirdsrio"
# ALWAYS verify the INSTALLED lib (the app dir alternates -1/-2; the path above
# is authoritative). Expect: $MD5
ssh -p 8022 $DEV "APK=\$(pm path com.rovio.angrybirdsrio | sed 's/^package://'); sudo unzip -p \$APK lib/arm64-v8a/libabhost.so | md5sum"
# mark the boundary in the log, then launch:
#   echo === NEW RUN === >> files/apphost.log   (or just note the boot time)
ssh -p 8022 $DEV "sudo am start -n com.rovio.angrybirdsrio/com.rovio.fusion.App"
# marker checks -- grep SEPARATELY (Termux grep does not honour \\|):
#   for p in HEAPCORRUPT HEAPASSERT CHUNKD USAGE_ERROR LOUD; do ...; done
#   also: sudo ls -la files/death.log   (must be 0 B) and sudo ls -la /data/tombstones
EOF
