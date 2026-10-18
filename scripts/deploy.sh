#!/bin/bash
# FULL local pipeline for one game + the exact device commands to run by hand.
#
#   scripts/deploy.sh <game-id>              e.g. scripts/deploy.sh rio
#   SKIP_LIB=1 scripts/deploy.sh rio         reuse the last host lib build
#
# The script deliberately does NOT touch the device (no ssh/scp/pm): the device
# half is printed for a human, so a broken local step can never half-install.
#
# Layout (see games/<id>/profile.toml and AGENTS.md):
#   games/<id>/original/            pristine Rovio APK   (gitignored)
#   games/<id>/work/apk_prod/       decoded tree         (gitignored)
#   games/<id>/work/apk_signed.apk  output
#   keys/debug.keystore             shared signing key   (gitignored)
#
# Device overrides: DEV=user@host:port  APK_NAME=...  SSH_PORT=...
set -euo pipefail

GAME="${1:?usage: deploy.sh <game-id>   (see games/*/profile.toml)}"
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJ"

PKG="$(python3 scripts/profile.py "$GAME" apk.package)"
LIB_DST="games/$GAME/work/apk_prod/lib/arm64-v8a/libabhost.so"
APK_OUT="games/$GAME/work/apk_signed.apk"
DEV="${DEV:-user@host:8022}"
APK_NAME="${APK_NAME:-ab_$GAME.apk}"
SSH_PORT="${SSH_PORT:-8022}"

if [ "${SKIP_LIB:-0}" != "1" ]; then
    scripts/build-lib.sh
fi

scripts/build-apk.sh "$GAME"

MD5="$(md5sum "$LIB_DST" | cut -d' ' -f1)"
cat <<EOF

--- device half (run by hand) --------------------------------------------
scp -P $SSH_PORT "$APK_OUT" $DEV:$APK_NAME
ssh -p $SSH_PORT $DEV "sudo pm install -r /data/data/com.termux/files/home/$APK_NAME; pm path $PKG"
# ALWAYS verify the INSTALLED lib (the app dir alternates -1/-2; the path above
# is authoritative). Expect: $MD5
ssh -p $SSH_PORT $DEV "APK=\$(pm path $PKG | sed 's/^package://'); sudo unzip -p \$APK lib/arm64-v8a/libabhost.so | md5sum"
# mark the boundary in the log, then launch:
#   echo === NEW RUN === >> files/apphost.log   (or just note the boot time)
ssh -p $SSH_PORT $DEV "sudo am start -n $PKG/com.rovio.fusion.App"
# marker checks -- grep SEPARATELY (Termux grep does not honour \\|):
#   for p in HEAPCORRUPT HEAPASSERT CHUNKD USAGE_ERROR LOUD; do ...; done
#   also: sudo ls -la files/death.log   (must be 0 B) and sudo ls -la /data/tombstones
EOF
