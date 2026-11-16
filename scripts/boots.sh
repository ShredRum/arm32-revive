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
# The sandbox: DEV (user@host) and P (ssh port) from the environment or
# scripts/local.env (gitignored; see scripts/local.env.example).
[ -f scripts/local.env ] && . scripts/local.env
DEV=${DEV:?set DEV=user@host (or put it in scripts/local.env)}; P=${P:-8022}
PKG=$(python3 scripts/profile.py "$G" apk.package)
ACT=$(python3 scripts/profile.py "$G" apk.activity 2>/dev/null || echo com.rovio.fusion.App)
S="ssh -o BatchMode=yes -p $P $DEV"
F=/data/data/$PKG/files
# The device gate's markers (AGENTS.md section 4), as devgate.sh greps them.
MARKERS="HEAPCORRUPT HEAPASSERT CHUNKD USAGE_ERROR LOUD STUCK FATAL"
mkdir -p work/boots
bad=0
for i in $(seq 1 "$N"); do
    $S "sudo /system/bin/am force-stop $PKG; sudo input keyevent 224; sudo /system/bin/am start -n $PKG/$ACT >/dev/null 2>&1"
    sleep "$WAIT"
    R=$($S "PID=\$(sudo ps -A | awk -v p=$PKG '\$NF==p {print \$2}'); echo pid=\${PID:-DEAD}; for m in $MARKERS; do echo \$m=\$(sudo grep -ac \$m $F/apphost.log); done; echo death=\$(sudo stat -c %s $F/death.log 2>/dev/null)" | tr '\n' ' ')
    # No answer (the sandbox is off, its address changed) is not a clean boot.
    case "$R" in
        pid=*) ;;
        *) echo "boots: no answer from the device at boot $i -- stopping" >&2; exit 1 ;;
    esac
    REST=${R#pid=* }  # the markers and death.log, without the pid field
    BADBOOT=0
    case "$R" in pid=DEAD*) BADBOOT=1 ;; esac
    case "$REST" in *=[1-9]*) BADBOOT=1 ;; esac
    case "$BADBOOT" in
        1)  # dead, a marker counted, or a death.log with bytes
            bad=$((bad + 1))
            $S "sudo cat $F/apphost.log" > "work/boots/${G}_${TAG}_$i.log"
            echo "boot $i: BAD  $R $(grep -a -m1 'LOUD' "work/boots/${G}_${TAG}_$i.log" | cut -c1-150)"
            ;;
        *) echo "boot $i: ok" ;;
    esac
done
$S "sudo /system/bin/am force-stop $PKG"
echo "== $G [$TAG]: $bad bad of $N boots"
