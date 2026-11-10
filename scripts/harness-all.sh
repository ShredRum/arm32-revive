#!/bin/sh
# harness-all.sh: the PC regression gate of REVIEW.md in one command. Runs
# the native harness on every live title's guest library (the copy under
# games/<id>/work whose sha256 is the one in profile.toml) and prints its
# "init done" line and the number of LOUD lines (expected: 0). Exits 1 when
# a title could not be run.
cd "$(dirname "$0")/.." || exit 1
H=./host/build-native/harness
[ -x "$H" ] || { echo "harness-all: $H is missing (scripts/build-lib.sh --native)" >&2; exit 1; }
rc=0
for g in rio seasons space star-wars star-wars-ii-amz stella classic; do
    lib=$(python3 scripts/profile.py "$g" guest.lib)
    want=$(python3 scripts/profile.py "$g" guest.sha256)
    f=""
    for c in $(find "games/$g/work" -name "$lib" -path "*armeabi-v7a*" 2>/dev/null); do
        if [ "$(sha256sum "$c" | cut -c1-64)" = "$want" ]; then
            f=$c
            break
        fi
    done
    if [ -z "$f" ]; then
        echo "$g: no copy of $lib under games/$g/work has the profile's sha256"
        rc=1
        continue
    fi
    out=$("$H" "$f" 2>&1)
    line=$(echo "$out" | grep -a -m1 'init done')
    if [ -z "$line" ]; then
        echo "$g: the harness printed no 'init done' line"
        rc=1
        continue
    fi
    echo "$g: $line LOUD=$(echo "$out" | grep -a -c LOUD)"
done
exit $rc
