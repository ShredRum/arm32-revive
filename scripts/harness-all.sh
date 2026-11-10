#!/bin/sh
# harness-all.sh: the PC regression gate of REVIEW.md in one command. Runs
# the native harness on every live title's guest library and prints its
# "init done" line and the number of LOUD lines (expected: 0).
cd "$(dirname "$0")/.." || exit 1
for g in rio seasons space star-wars star-wars-ii-amz stella; do
    lib=$(python3 scripts/profile.py "$g" guest.lib)
    f=$(find "games/$g/work" -name "$lib" -path "*armeabi-v7a*" | head -1)
    if [ -z "$f" ]; then
        echo "$g: no guest library under games/$g/work"
        continue
    fi
    out=$(./host/build-native/harness "$f" 2>&1)
    echo "$g: $(echo "$out" | grep -a -m1 'init done') LOUD=$(echo "$out" | grep -a -c LOUD)"
done
