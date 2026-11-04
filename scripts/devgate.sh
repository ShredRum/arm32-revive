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
$S "sudo rm -f $F/abflags.txt; T0=\$(sudo ls /data/tombstones 2>/dev/null | wc -l); echo tomb_before=\$T0; sudo /system/bin/am start -n $PKG/$ACT >/dev/null 2>&1"
sleep $WAIT
$S "PID=\$(sudo ps -A | awk -v p=$PKG '\$NF==p {print \$2}'); echo pid=\${PID:-DEAD}; for m in HEAPCORRUPT HEAPASSERT CHUNKD USAGE_ERROR LOUD STUCK FATAL; do echo \$m=\$(sudo grep -ac \$m $F/apphost.log); done; echo death=\$(sudo stat -c %s $F/death.log 2>/dev/null); echo tomb_after=\$(sudo ls /data/tombstones 2>/dev/null | wc -l); sudo grep -a -m1 'build marker' $F/apphost.log; sudo screencap -p /data/local/tmp/gate.png"
scp -q -P $P $DEV:/data/local/tmp/gate.png work/gate_$G.png && echo "shot: work/gate_$G.png"
