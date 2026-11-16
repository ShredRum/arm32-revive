#!/usr/bin/env python3
# Generates host/src/apphost_gen.inc: the JNI forwarders (Java -> Dynarmic guest).
# Each forwarder passes (name, sig, env, thiz, ...) to abr::AppHost_Call, which
# walks the sig at runtime (varargs: jlong raw, jfloat promoted to double,
# jint words) and marshals AAPCS (env, thiz, words; jlong 8-aligned; spill to
# guest stack).
# Inputs: games/<id>/work/jni_signatures.txt + the decoded smali (return + static).
# Rule: NEVER hand-write these; regenerate.
#
# One libabhost.so serves several titles: the forwarders are the UNION of the
# per-title JNI signature sets. Extra forwarders are inert -- AppHost_Call only
# runs for names the running Java side actually declares, and a name that is
# declared but absent from the loaded guest .so would have failed in the real
# app too. A (class, method) pair MUST NOT appear with two different signatures
# across titles: that would need two C functions with one symbol. The union mode
# hard-fails on such a conflict.
#
#   python3 host/gen_apphost.py [game-id]   # single game (default rio)
#   python3 host/gen_apphost.py --union     # every games/*/work/jni_signatures.txt
#
# NOTE (E6, 2026-10-18): the OUTPUT host/src/apphost_gen.inc is COMMITTED, and a
# normal build does not run this script -- the .inc is compiled as-is. That is
# deliberate and safe: regenerate only when the JNI signature set actually
# changes. Because both inputs live under games/<id>/work/ (gitignored, derived
# from the original APK), a fresh clone cannot regenerate the file on its own; it
# needs the APK, an apktool decode at games/<id>/work/apk_prod and the signature
# inventory next to it.
# Run from the project root.
import re, glob, sys

UNION = '--union' in sys.argv[1:]
GAME = None if UNION else (sys.argv[1] if len(sys.argv) > 1 else 'rio')
OUT = 'host/src/apphost_gen.inc'


def smali_natives(game):
    """game: dict method -> {class: (ret, is_static)} for one title."""
    smali_nat = {}
    # Every smali tree (smali, smali_classes2, ...): natives may live outside
    # com/rovio (SDL titles: org/libsdl, com/telltalegames).
    for f in glob.glob(f'games/{game}/work/apk_prod/smali*/**/*.smali', recursive=True):
        cls = re.sub(r'^.*?/smali[^/]*/', '', f).replace('/', '_').replace('.smali', '')
        for ln in open(f, errors='ignore'):
            # A package-private native has no flag before "native"
            # (FMOD's `.method native fmodProcessMicData(...)`).
            m = re.search(r'\.method\s+(?:(.*?)\s+)?native\s+(\w+)\((.*?)\)(.)', ln)
            if m:
                flags, meth, args, ret = m.groups()
                flags = flags or ''
                smali_nat.setdefault(meth, {})[cls] = (ret, 'static' in flags)
    return smali_nat

# 'D' (double) and 'F' (float) must be mapped: without them a native
# method that takes a double fell into the "anything else" branch below and
# was typed as jobject. No guest has such a native today (measured: no 'D'
# in any of the 197 signatures), but the mapping has to be right.
J2C = {'I': 'jint', 'F': 'jfloat', 'D': 'jdouble', 'Z': 'jboolean',
       'B': 'jbyte', 'S': 'jshort', 'C': 'jchar', 'J': 'jlong'}
R2C = {'V': 'void', 'I': 'jint', 'D': 'jdouble', 'Z': 'jboolean',
       'B': 'jbyte', 'S': 'jshort', 'C': 'jchar', 'J': 'jlong'}

def split_args(sig):
    out = []
    i = 0
    while i < len(sig):
        c = sig[i]
        if c == 'L':
            j = sig.index(';', i)
            out.append(('L', sig[i:j + 1]))
            i = j + 1
        elif c == '[':
            j = i
            while sig[j] == '[':
                j += 1
            if sig[j] == 'L':
                k = sig.index(';', j)
                out.append(('[L', sig[i:k + 1]))
                i = k + 1
            else:
                out.append(('[P', sig[i:j + 1]))
                i = j + 1
        else:
            out.append((c, c))
            i += 1
    return out

def jni_mangle(s):
    # JNI spec, "Resolving Native Method Names": _ -> _1, ; -> _2, [ -> _3,
    # / -> _ (other characters do not occur in these names).
    return s.replace('_', '_1').replace(';', '_2').replace('[', '_3').replace('/', '_')

def jni_name(cls, meth):
    return 'Java_' + jni_mangle(cls) + '_' + jni_mangle(meth)

def jni_long_name(cls, meth, args):
    # The overloaded form the VM tries after the short one:
    # Java_<class>_<method>__<mangled argument signature>.
    return jni_name(cls, meth) + '__' + jni_mangle(args)

# Collect (class, method) -> {args: game} from the selected inventories.
# Two titles may declare one native with different arguments (Stella's
# MyLegacyRenderer.nativeInit(II) and Seasons 3.1.1's (IILjava/lang/String;)).
# One C symbol cannot carry both, so such a method gets one forwarder per
# signature under the JNI long name, which the VM looks up when the short
# name is absent; each passes the guest's (short) export name on.
if UNION:
    games = sorted(d.split('/')[1] for d in glob.glob('games/*/work/jni_signatures.txt'))
    assert games, 'no games/*/work/jni_signatures.txt found'
else:
    games = [GAME]
entries = {}
n_lines = 0
for g in games:
    for ln in open(f'games/{g}/work/jni_signatures.txt'):
        ln = ln.strip()
        if not ln:
            continue
        n_lines += 1
        m = re.match(r'(\S+) (\w+) \((.*)\)$', ln)
        assert m, (g, ln)
        cls, meth, args = m.groups()
        entries.setdefault((cls, meth), {}).setdefault(args, g)

_nat_cache = {}
def resolve(game):
    if game not in _nat_cache:
        _nat_cache[game] = smali_natives(game)
    return _nat_cache[game]

lines = ['// GENERATED by host/gen_apphost.py — do not hand-edit.']
n = 0
variants = [(cls, meth, args, g, len(sigs) > 1)
            for (cls, meth), sigs in entries.items() for args, g in sigs.items()]
for cls, meth, args, g, overloaded in variants:
    smali_nat = resolve(g)
    key = cls.replace('/', '_')
    cands = smali_nat.get(meth, {})
    if key in cands:
        ret, is_static = cands[key]
    else:
        assert len(cands) == 1, 'ambiguous/missing smali for %s %s: %s' % (cls, meth, cands)
        ret, is_static = next(iter(cands.values()))
    assert ret in R2C or ret.startswith('L') or ret.startswith('['), ret
    objty = 'jclass' if is_static else 'jobject'
    params = ['JNIEnv* env', '%s thiz' % objty]
    callargs = []
    k = 0
    for kind, full in split_args(args):
        nm = 'a%d' % k
        k += 1
        if kind == 'J':
            params.append('jlong %s' % nm)
        elif kind in J2C:
            params.append('%s %s' % (J2C[kind], nm))
        else:
            params.append('jobject %s' % nm)
        callargs.append(nm)
    sigstr = '(%s)%s' % (args, ret)
    call = 'abr::AppHost_Call("%s", "%s", env, thiz%s)' % (
        jni_name(cls, meth), sigstr, ''.join(', %s' % a for a in callargs))
    cname = jni_long_name(cls, meth, args) if overloaded else jni_name(cls, meth)
    lines.append('extern "C" %s %s(%s) {' % (
        {'V': 'void'}.get(ret, R2C.get(ret, 'jobject')), cname,
        ', '.join(params)))
    if ret == 'V':
        lines.append('    %s; return;' % call)
    elif ret == 'J':
        lines.append('    std::uint64_t rv_ = %s;' % call)
        lines.append('    jlong o_; std::memcpy(&o_, &rv_, 8); return o_;')
    elif ret == 'F':
        lines.append('    std::uint64_t rv_ = %s;' % call)
        lines.append('    jfloat o_; std::uint32_t lo_ = (std::uint32_t)rv_;')
        lines.append('    std::memcpy(&o_, &lo_, 4); return o_;')
    elif ret in ('I', 'Z', 'B', 'S', 'C'):
        lines.append('    return (%s)%s;' % (R2C[ret], call))
    else:
        lines.append('    return (jobject)(std::uintptr_t)%s;' % call)
    lines.append('}')
    n += 1
lines.append('// GEN_COUNT=%d' % n)
open(OUT, 'w').write('\n'.join(lines) + '\n')
print('forwarders=%d (from %d signature lines in %s)' % (n, n_lines, ','.join(games)))
assert n == len(variants), 'generated %d != %d (class, method, signature)' % (n, len(variants))
