# Angry Birds Star Wars II, rework "v2.1" — journal

Original APK: sent by a forum user (`original/AB_StarWars2_v2.1.apk`,
113 273 257 B): a YouTuber's rework of Star Wars II 1.8.1 (versionCode 1811),
abandoned by its author. Package `angrybirds_starwars2v2.v1` (its own, so it
lives next to `star-wars-ii-amz`), signed with the rework's own key; minSdk 9,
targetSdk 25. Rovio fusion engine, GLES2. The APK also carries
`assets/classes.dex` (a copy of the main dex, not loaded: no custom
Application class) and `assets/natives_sec_blob.dat` (unknown tool's file,
not read by the game's code). Whether the author changed the engine library
cannot be checked: there is no Rovio 1.8.1 here.

## 2026-11-16: added

- Label "Angry Birds Star Wars II v2.1"; targetSdk 26.
- Guest `libAngryBirdsStarWarsII.so` (7 964 256 B): no import the host lacks.
  74 JNI exports, 73 declared by the Java side; the guest's
  `SocialSharingViewWrapper.onSharingViewStartSharing` has no Java caller.
- JNI forwarders regenerated: six new names, none of the old lines changed.
- Sandbox: boots to its menu first try (the "V2.1!" badge, the game's own
  Carbonite Reward dialog), LOUD 0. Plain Star Wars II was retired because
  its content servers are dead (R-004); whether this rework needs them in
  play is not known yet.

Not yet: levels and sound played, the phone.

## 2026-11-16: crash after a few level restarts — fixed (331ca42)

The owner saw it on the phone and again on the sandbox by restarting one
level: the host halted with `[jni] LOUD object-handle arena exhausted`. The
engine asks GetStaticMethodID again for every file it checks or reads (one
45 s boot: FileReader.exists 5607, CameraWrapper.isSupported/isAvailable
2037 each, readFile 1149), and the host made a new handle per ask; this
mod reads more files than the originals, so the ~13k handles ran out. Not
the mod's fault and not a regression (the allocation dates from the first
JNI bridge). Now one ID per member. Sandbox: 120 s from boot alive at the
menu, LOUD 0, past the string count where it used to die.

Still growing, harmless for hours: the engine's file-name jstrings are kept
(R-003, owner's wontfix): ~85 KB per 10 s while loading here.
