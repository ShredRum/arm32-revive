#!/bin/bash
# scripts/devgate.sh <game-id> [seconds]  -- install games/<id>/work/apk_signed.apk on the sandbox,
# launch, wait, then report pid / markers / death.log and pull a screenshot.
# Never writes to /storage/emulated (host phone storage).
set -euo pipefail
cd "/home/shredrum/Рабочий стол/ab-port"
G=$1; WAIT=${2:-40}
DEV=${DEV:-u0_a41@192.168.1.202}; P=8022
PKG=$(python3 scripts/profile.py $G apk.package)
APK=games/$G/work/apk_signed.apk
HOSTLIB=$(python3 scripts/profile.py $G host.lib)
ACT=$(python3 scripts/profile.py $G apk.activity 2>/dev/null || echo com.rovio.fusion.App)
WANT=$(unzip -p $APK lib/arm64-v8a/$HOSTLIB | md5sum | cut -c1-32)
# A phone build (API 29, ELF TLS) does not load on Android 7-9: say so here
# instead of a crash at the first start.
READELF=$(ls tools/android-sdk/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf | head -1)
TMPLIB=$(mktemp)
unzip -p $APK lib/arm64-v8a/$HOSTLIB > "$TMPLIB"
if "$READELF" -lW "$TMPLIB" | grep -q " TLS "; then
    SDK=$(ssh -o BatchMode=yes -p $P $DEV getprop ro.build.version.sdk)
    if [ "${SDK:-0}" -lt 29 ]; then
        rm -f "$TMPLIB"
        echo "devgate: $APK holds the phone build (ELF TLS, Android 10+); this device is API $SDK." >&2
        echo "  build the sandbox one: scripts/build-lib.sh --sandbox && SANDBOX=1 scripts/swap-lib.sh $G" >&2
        exit 1
    fi
fi
rm -f "$TMPLIB"
S="ssh -o BatchMode=yes -p $P $DEV"
# One session at a time: every other title of the project is stopped first
# (a game left running from an earlier gate skews markers and fps).
OTHERS=""
for prof in games/*/profile.toml; do
    gid=$(basename "$(dirname "$prof")")
    [ "$gid" = "$G" ] && continue
    op=$(python3 scripts/profile.py "$gid" apk.package 2>/dev/null) || continue
    [ -n "$op" ] && [ "$op" != "$PKG" ] && OTHERS="$OTHERS $op"
done
scp -q -P $P $APK $DEV:ab_gate.apk || exit 1
$S "for o in $OTHERS; do sudo /system/bin/am force-stop \$o; done; sudo /system/bin/am force-stop $PKG; sudo pm install -r ~/ab_gate.apk >/dev/null && rm ~/ab_gate.apk" || exit 1
GOT=$($S "A=\$(pm path $PKG | sed 's/^package://'); sudo unzip -p \$A lib/arm64-v8a/$HOSTLIB | md5sum" | cut -c1-32)
echo "lib want=$WANT installed=$GOT $([ "$WANT" = "$GOT" ] && echo OK || echo MISMATCH)"
[ "$WANT" = "$GOT" ] || { echo "devgate: installed lib differs from the APK -- stopping" >&2; exit 1; }
F=/data/data/$PKG/files
# Make sure the display is on (a screenshot of a sleeping display is all
# black, which reads as a rendering failure); "screen=" in the report says
# what it was at the screenshot.
$S "sudo input keyevent 224; sudo rm -f $F/abflags.txt; T0=\$(sudo ls /data/tombstones 2>/dev/null | wc -l); echo tomb_before=\$T0; sudo /system/bin/am start -n $PKG/$ACT >/dev/null 2>&1"
sleep $WAIT
$S "PID=\$(sudo ps -A | awk -v p=$PKG '\$NF==p {print \$2}'); echo pid=\${PID:-DEAD}; for m in HEAPCORRUPT HEAPASSERT CHUNKD USAGE_ERROR LOUD STUCK FATAL; do echo \$m=\$(sudo grep -ac \$m $F/apphost.log); done; echo death=\$(sudo stat -c %s $F/death.log 2>/dev/null); echo tomb_after=\$(sudo ls /data/tombstones 2>/dev/null | wc -l); sudo grep -a -m1 'build marker' $F/apphost.log; echo screen=\$(sudo dumpsys power | grep -m1 -o 'mWakefulness=[A-Za-z]*' | cut -d= -f2); sudo screencap -p /data/local/tmp/gate.png"
scp -q -P $P $DEV:/data/local/tmp/gate.png work/gate_$G.png && echo "shot: work/gate_$G.png"
# A (nearly) black screenshot of an awake screen: keep evidence before the
# next run replaces it (REVIEW R-014): frames the game's surface posted,
# every thread's stack, the host log and the app's logcat.
if python3 -c "
import sys
from PIL import Image
h = Image.open('work/gate_$G.png').convert('L').histogram()
sys.exit(0 if sum(h[16:]) < 0.01 * sum(h) else 1)" 2>/dev/null; then
    EV=work/gate_${G}_black.txt
    $S "PID=\$(sudo pidof $PKG); L=\$(sudo dumpsys SurfaceFlinger --list | grep -m1 \"SurfaceView.*$PKG\"); echo layer=\$L; echo frames_posted=\$(sudo dumpsys SurfaceFlinger --latency \"\$L\" | awk 'NF==3 && \$2>0' | wc -l); sleep 2; echo frames_posted_2s_later=\$(sudo dumpsys SurfaceFlinger --latency \"\$L\" | awk 'NF==3 && \$2>0' | wc -l); sudo debuggerd -b \$PID 2>/dev/null; echo ===APPHOST; sudo cat $F/apphost.log; echo ===LOGCAT; sudo logcat -d | grep -a \" \$PID \" | grep -av 'art     :   at' | tail -300" > "$EV" 2>&1
    echo "BLACK SCREEN with the screen awake: evidence in $EV"
fi
