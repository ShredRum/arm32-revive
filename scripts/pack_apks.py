#!/usr/bin/env python3
"""Build a split-APK set whose game data is read in place by the host.

    scripts/pack_apks.py <game-id>

Input: games/<id>/work/apk_signed.apk (scripts/build-apk.sh) and the data
tree named by the profile's [packed] section. Output, in games/<id>/work/apks/:
base.apk (the program plus the data no split claims), split_<name>.apk per
split, and <id>.apks, a plain zip of them for SAI (Split APKs Installer)
or `pm install-create`/`install-write`/`install-commit`.

Each data file goes in as assets/abport/<root>/<path>, STORED and aligned to
16 KiB (host/src/guest/apk_vfs.h reads it in place). Each APK stays under
2 GB so no zip64 record is ever needed; the build stops otherwise.

[packed] keys:
  source  the data tree, relative to games/<id>/ (or absolute)
  map     [[source subdir, packed root/path], ...]; roots: files, ext, sdcard
  splits  [[split name, regex on the packed root/path], ...]; first match wins
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tomllib
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BT = ROOT / "tools/android-sdk/build-tools/35.0.0"
ANDROID_JAR = ROOT / "tools/android-sdk/platforms/android-35/android.jar"
PREFIX = "assets/abport/"
ALIGN = 16384
MAX_APK = 2_000_000_000  # below zipfile's zip64 switch (2 GiB - 1)


def die(msg):
    sys.exit(f"pack_apks: {msg}")


def run(*args):
    r = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if r.returncode != 0:
        die(f"{args[0]} failed:\n{r.stdout}{r.stderr}")
    return r.stdout


def add_stored(apk, files):
    """Appends (name, host file) pairs as stored entries."""
    with zipfile.ZipFile(apk, "a", compression=zipfile.ZIP_STORED, allowZip64=False) as z:
        have = set(z.namelist())
        for name, full in files:
            if name in have:
                die(f"{apk.name}: {name} is already in the APK")
            z.write(full, arcname=name, compress_type=zipfile.ZIP_STORED)


def align_and_sign(unsigned, out, ks):
    aligned = unsigned.with_suffix(".aligned.apk")
    run(BT / "zipalign", "-f", ALIGN, unsigned, aligned)
    run(BT / "apksigner", "sign", "--ks", ks["path"], "--ks-key-alias", ks["alias"],
        "--ks-pass", "pass:" + ks["pass"], "--key-pass", "pass:" + ks["pass"],
        "--v1-signing-enabled", "true", "--v2-signing-enabled", "true",
        "--alignment-preserved", "true", "--out", out, aligned)
    aligned.unlink()
    unsigned.unlink()


def check(apk):
    """Every packed entry stored, 16 KiB aligned; the APK under the zip64 line."""
    size = apk.stat().st_size
    if size >= MAX_APK:
        die(f"{apk.name} is {size} bytes: move data to more splits")
    n = 0
    with open(apk, "rb") as fh, zipfile.ZipFile(apk) as z:
        for info in z.infolist():
            if not info.filename.startswith(PREFIX) or info.is_dir():
                continue
            if info.compress_type != zipfile.ZIP_STORED:
                die(f"{apk.name}: {info.filename} is compressed")
            fh.seek(info.header_offset)
            local = fh.read(30)
            nlen = int.from_bytes(local[26:28], "little")
            xlen = int.from_bytes(local[28:30], "little")
            data = info.header_offset + 30 + nlen + xlen
            if data % ALIGN:
                die(f"{apk.name}: {info.filename} data at {data}, not {ALIGN}-aligned")
            n += 1
    run(BT / "apksigner", "verify", apk)
    return n, size


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    game = sys.argv[1]
    gdir = ROOT / "games" / game
    prof = tomllib.load(open(gdir / "profile.toml", "rb"))
    if "packed" not in prof:
        die(f"{game}: no [packed] section in profile.toml")
    pk, apk = prof["packed"], prof["apk"]
    src = pathlib.Path(os.path.expanduser(pk["source"]))
    if not src.is_absolute():
        src = gdir / src
    src = src.resolve()
    base_in = gdir / "work/apk_signed.apk"
    if not base_in.exists():
        die(f"{base_in} missing: run scripts/build-apk.sh {game} first")
    ks = {"path": os.environ.get("KS", str(ROOT / "keys/debug.keystore")),
          "alias": os.environ.get("KS_ALIAS", "dbg"),
          "pass": os.environ.get("KS_PASS", "android")}

    # Collect the data, then give each file to the first split that claims it.
    entries = []
    for sub, dst in pk["map"]:
        top = src / sub
        if not top.is_dir():
            die(f"no data directory {top}")
        for dirpath, dirs, files in os.walk(top):
            dirs.sort()
            for f in sorted(files):
                full = pathlib.Path(dirpath) / f
                rel = full.relative_to(top).as_posix()
                entries.append((f"{dst.strip('/')}/{rel}", full))
    splits = [(name, re.compile(rx)) for name, rx in pk.get("splits", [])]
    groups = {"base": []}
    for name, _ in splits:
        groups[name] = []
    for packed, full in entries:
        owner = next((n for n, rx in splits if rx.search(packed)), "base")
        groups[owner].append((PREFIX + packed, full))
    for name, files in groups.items():
        total = sum(f.stat().st_size for _, f in files)
        print(f"  {name:<14} {len(files):4d} files {total / 1e6:9.1f} MB")
        if name != "base" and not files:
            die(f"split {name} claims no file")

    out = gdir / "work/apks"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    # base.apk: the program with its old signature dropped, plus base data.
    tmp = out / "base.unsigned.apk"
    shutil.copyfile(base_in, tmp)
    run("zip", "-q", "-d", tmp, "META-INF/*")
    add_stored(tmp, groups["base"])
    align_and_sign(tmp, out / "base.apk", ks)

    # split_<name>.apk: a manifest-only APK (no code) plus its data.
    for name, files in groups.items():
        if name == "base":
            continue
        manifest = out / f"{name}.xml"
        manifest.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<manifest xmlns:android="http://schemas.android.com/apk/res/android"\n'
            f'    package="{apk["package"]}" split="{name}"\n'
            f'    android:versionCode="{apk["version_code"]}"'
            f' android:versionName="{apk["version_name"]}">\n'
            f'    <uses-sdk android:minSdkVersion="{apk["min_sdk"]}"'
            f' android:targetSdkVersion="{apk["target_sdk"]}"/>\n'
            '    <application android:hasCode="false"/>\n'
            '</manifest>\n')
        tmp = out / f"split_{name}.unsigned.apk"
        run(BT / "aapt2", "link", "--manifest", manifest, "-I", ANDROID_JAR, "-o", tmp)
        manifest.unlink()
        add_stored(tmp, files)
        align_and_sign(tmp, out / f"split_{name}.apk", ks)

    apks = sorted(out.glob("*.apk"), key=lambda p: (p.name != "base.apk", p.name))
    for a in apks:
        n, size = check(a)
        print(f"  {a.name:<24} {size / 1e6:9.1f} MB, {n} packed files, stored and aligned, signed")
    bundle = out / f"{game}.apks"
    with zipfile.ZipFile(bundle, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        for a in apks:
            z.write(a, arcname=a.name)
    print(f"apks    : {bundle} ({bundle.stat().st_size / 1e6:.1f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
