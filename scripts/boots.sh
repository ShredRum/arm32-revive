#!/bin/bash
# scripts/boots.sh <game-id> <boots> [seconds] [tag]  -- start the INSTALLED
# build of a title on the sandbox <boots> times (default 30 s each) and count
# the boots that died or logged a marker. For intermittent failures: run the
# same batch on two builds (install each with devgate.sh first). The host
# log of every bad boot is kept in work/boots/<id>_<tag>_<n>.log.
# Never writes to /storage/emulated.
set -uo pipefail
cd "$(dirname "$0")/.."
G=$1; N=$2; WAIT=${3:-30}; TAG=${4:-run}
DEV=${DEV:-u0_a41@192.168.1.202}; P=8022
PKG=$(python3 scripts/profile.py "$G" apk.package)
ACT=$(python3 scripts/profile.py "$G" apk.activity 2>/dev/null || echo com.rovio.fusion.App)
S="ssh -o BatchMode=yes -p $P $DEV"
F=/data/data/$PKG/files
mkdir -p work/boots
bad=0
for i in $(seq 1 "$N"); do
    $S "sudo /system/bin/am force-stop $PKG; sudo input keyevent 224; sudo /system/bin/am start -n $PKG/$ACT >/dev/null 2>&1"
    sleep "$WAIT"
    R=$($S "PID=\$(sudo ps -A | awk -v p=$PKG '\$NF==p {print \$2}'); echo pid=\${PID:-DEAD} LOUD=\$(sudo grep -ac LOUD $F/apphost.log) FATAL=\$(sudo grep -ac FATAL $F/apphost.log) STUCK=\$(sudo grep -ac STUCK $F/apphost.log) death=\$(sudo stat -c %s $F/death.log 2>/dev/null)")
    case "$R" in
        *DEAD*|*LOUD=[1-9]*|*FATAL=[1-9]*|*STUCK=[1-9]*|*death=[1-9]*)
            bad=$((bad + 1))
            $S "sudo cat $F/apphost.log" > "work/boots/${G}_${TAG}_$i.log"
            echo "boot $i: BAD  $R  $(grep -a -m1 'LOUD' "work/boots/${G}_${TAG}_$i.log" | cut -c1-150)"
            ;;
        *) echo "boot $i: ok" ;;
    esac
done
$S "sudo /system/bin/am force-stop $PKG"
echo "== $G [$TAG]: $bad bad of $N boots"
