#!/bin/bash
# scripts/speed.sh <game-id> <rounds> <name=apk> <name=apk> ...
#   A/B speed of whole APKs on the sandbox: process CPU per presented frame
#   while the title sits where it lands after boot (the main menu).
#   Variants are interleaved (A,B,A,B...): one run resolves +-0.2..0.3 ms,
#   four rounds about +-0.1 ms. Anything smaller is "below what we resolve".
#   Env: BOOT (s to wait after start, 50), SECS (s measured, 30).
#   A Play Protect dialog on install delays the start: answer it at once, or
#   the run measures a shorter warm-up than its pair.
# Output per run: variant frames cpu_ms ms_per_frame LOUD-count.
set -u
cd "$(dirname "$0")/.."
ID=$1; ROUNDS=$2; shift 2
PKG=$(python3 scripts/profile.py "$ID" apk.package)
# The sandbox: DEV (user@host) and P (ssh port) from the environment or
# scripts/local.env (gitignored; see scripts/local.env.example).
[ -f scripts/local.env ] && . scripts/local.env
DEV=${DEV:?set DEV=user@host (or put it in scripts/local.env)}; P=${P:-8022}
S="ssh -o BatchMode=yes -o ConnectTimeout=8 -p $P $DEV"
BOOT=${BOOT:-50}; SECS=${SECS:-30}
F=/data/data/$PKG/files

# The measuring half runs on the device as root. It only writes
# /data/local/tmp/abm_ts.txt.
$S 'cat > ~/abmeasure.sh' <<'EOF'
PKG=$1; SECS=$2
TS=/data/local/tmp/abm_ts.txt
pid=$(/system/bin/ps | awk -v p=$PKG '$NF==p {print $2}' | head -1)
[ -n "$pid" ] || { echo "DEAD"; exit 0; }
stat_cpu() { sed 's/.*) //' /proc/$pid/stat | awk '{print $12+$13}'; }
layer=$(dumpsys SurfaceFlinger --list | grep SurfaceView | grep "$PKG" | grep -v "Background for" | head -1)
[ -n "$layer" ] || { echo "NOLAYER"; exit 0; }
poll() { dumpsys SurfaceFlinger --latency "$layer" | awk 'NF==3 && $2>0 && $2<9000000000000000000 {print $2}'; }
start=$(poll | sort -n | tail -1); c0=$(stat_cpu)
: > $TS
i=0; while [ $i -lt $SECS ]; do sleep 1; poll >> $TS; i=$((i+1)); done
c1=$(stat_cpu)
[ -n "$c1" ] || { echo "DIED"; exit 0; }
frames=$(sort -u $TS | awk -v s=${start:-0} '$1>s' | wc -l)
tck=$(getconf CLK_TCK 2>/dev/null || echo 100)
awk -v f=$frames -v c=$((c1-c0)) -v t=$tck 'BEGIN { ms=c*1000/t; printf "%d %d %.3f\n", f, ms, (f>0 ? ms/f : 0) }'
EOF

# Every other title of the project is stopped before each run: a game left
# running from an earlier gate takes CPU and skews the numbers.
OTHERS=""
for prof in games/*/profile.toml; do
    op=$(python3 scripts/profile.py "$(basename "$(dirname "$prof")")" apk.package 2>/dev/null) || continue
    [ -n "$op" ] && [ "$op" != "$PKG" ] && OTHERS="$OTHERS $op"
done

for r in $(seq 1 "$ROUNDS"); do
    for v in "$@"; do
        name=${v%%=*}; apk=${v#*=}
        $S true || { echo "speed: device does not answer -- stopping" >&2; exit 1; }
        scp -q -P "$P" "$apk" "$DEV:ab_speed.apk" || exit 1
        $S "for o in $OTHERS; do sudo /system/bin/am force-stop \$o; done; sudo /system/bin/am force-stop $PKG; sudo pm install -r ~/ab_speed.apk >/dev/null && rm ~/ab_speed.apk; sudo input keyevent 224; sudo rm -f $F/abflags.txt; sudo monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1" || exit 1
        sleep "$BOOT"
        res=$($S "sudo sh ~/abmeasure.sh $PKG $SECS; echo LOUD=\$(sudo grep -ac LOUD $F/apphost.log)" | tr '\n' ' ')
        echo "r$r $name $res"
        $S "sudo /system/bin/am force-stop $PKG"
    done
done
$S "rm -f ~/abmeasure.sh; sudo rm -f /data/local/tmp/abm_ts.txt"
