# VisIVO VR headset app

The *VisIVO VR* app for the Meta Quest (issues #254, #255, #383). Opened
from the App Library it shows a QR-code scanner: the user looks at the QR
code of the desktop's *Open in VR Headset…* dialog (or types its code), and
the app opens the public pairing loader with that code in Horizon OS's
immersive mode, so the user is in VR in front of the cube with no browser,
no Enter VR press and no cable. The desktop can also start it over adb
with one `am start` on its own backend's VR page (the USB launch, #255).

The app is a thin wrapper made with Meta's Bubblewrap (a Trusted Web
Activity in immersive mode) plus one native panel, the scanner: the VR
page itself, and every update of it, still comes from the user's backend.

## What is here

| File | What it does |
|---|---|
| `build.sh` | The whole build: key, Bubblewrap, patches, Gradle, zipalign, apksigner. |
| `app.json` | The app's identity: packages, names, `versionCode`, the SDK levels and headsets the Meta Horizon Store asks for, the loader it opens (`loaderOrigin` + `startPath`), icons. |
| `make-twa.py` | Writes Bubblewrap's `twa-manifest.json` from `app.json`. |
| `icon-512.png`, `icon-maskable-512.png` | The launcher icons, made from the VisIVO logo by `make-icons.py`. |
| `make-icons.py` | Makes both icons from `icons/VisIVO_512.png` (below). |
| `patch-project.py` | The changes to the generated Android project (below). |
| `android/ScanActivity.java` | The scanner: a 2-D panel, the passthrough camera, ZXing's QR decoder, the code field. |
| `android/PairCode.java` | What a QR code or a typed code means, which addresses the app opens (no Android API). |
| `android/PairCodeTest.java` | `PairCode`'s checks on a plain JVM (run by `backend/tests/test_quest_app_wrapper.py`). |
| `make-assetlinks.py` | Writes `web/vr-loader/.well-known/assetlinks.json`, the statement the loader's host publishes. |
| `check-store.py` | Checks a built APK against the Meta Horizon Store's technical requirements (#386). |
| `make-store-assets.py`, `store/logo-white.png` | The Store's images (hero, covers, logo, icon) and the screenshots' crop (#386). |
| `package.json`, `package-lock.json` | Pin `@meta-quest/bubblewrap-cli` 1.24.1. |

Nothing secret is in the repository. The development key is made on your
computer at the first build; the release key is a CI secret.

## Build

Needs a JDK 17 (`JAVA_HOME`), an Android SDK (`ANDROID_HOME`; build-tools
34.0.0 and platform 34 are installed with its `sdkmanager` when missing),
node with npm, and python3.

```bash
tools/quest-app/build.sh            # development app, your own key
tools/quest-app/build.sh --release  # release app, the project's key (CI)
tools/quest-app/build.sh --loader http://localhost:8790   # development app on a local loader
```

`--loader` builds the development app for a loader of your own (an origin;
the path is `app.json`'s `startPath`), for example `web/vr-loader/` served on
the computer and reached through `adb reverse tcp:8790 tcp:8790`. That
server must also answer `/.well-known/assetlinks.json` with the development
app's statement (`out/assetlinks-dev.json`), or the headset's browser closes
the app.

Output in `tools/quest-app/out/` (ignored by git):

- `visivo-vr-dev.apk` (or `visivo-vr.apk`): the signed APK;
- `visivo-vr-dev.json`: `package`, `version_code`, `version_name`,
  `cert_sha256`, `apk_sha256`, `start_path`, `default_origin` (the loader's
  origin), `loader_url`; the desktop compares `version_code` with the
  installed app's;
- `assetlinks-dev.json` (or `assetlinks.json`): the Digital Asset Links
  statement for that build.

A first build downloads Bubblewrap and Gradle's dependencies (about 1 GB with
the SDK); later builds take about 12 s.

### Development key

`build.sh` makes `dev.keystore` and `dev.pass` (mode 600) in
`$VISIVO_QUEST_APP_DIR`, by default `~/.visivo/quest-app/`, and writes the
certificate's SHA-256 to `dev-sha256.txt` there. A backend on the same
computer reads that file and adds the development app to
`/.well-known/assetlinks.json` by itself, so a development build works with
your local backend at once. Keep the key: an APK signed with another key
cannot be installed over this one (`adb uninstall it.inaf.oact.visivo.vr.dev`
first).

### Icons

The launcher icons are the VisIVO logo (`icons/VisIVO_512.png`, the desktop's
own icon) on white, because the logo's dark-blue text would vanish on Horizon
OS's dark panels:

- `icon-512.png`: the logo on a white rounded square, shown as it is;
- `icon-maskable-512.png`: the logo on a full white square, every visible
  pixel inside the centre circle of 80% of the width, so any launcher mask
  keeps all of it.

They are committed; to make them again (Pillow needed, the logo file is never
changed):

```bash
python3 tools/quest-app/make-icons.py
```

A new icon is a new wrapper: bump `versionCode` in `app.json`, so the desktop
offers **Update** on headsets that have the older app.

### Release key

The project's key exists (made 2026-11-14, alias `visivo-vr`); its secrets
`QUEST_APP_KEYSTORE_B64` and `QUEST_APP_KEYSTORE_PASS` are set, and its
certificate's SHA-256 is in `backend/app/vr_app.json`:

```
A1:54:C4:5B:54:20:5F:B9:6B:B9:80:28:6D:86:F3:A6:78:17:E1:32:2E:4F:68:AB:44:40:30:C0:43:04:D8:D4
```

The steps below are how it was made, for a fork with a key of its own or for
a replacement. Make it once, keep it safe (losing it means users must
uninstall the app):

```bash
keytool -genkeypair -keystore visivo-vr-release.p12 -storetype PKCS12 \
  -alias visivo-vr -keyalg RSA -keysize 3072 -validity 9125 \
  -dname "CN=VisIVO VR, O=VisIVO Visual Analytics"
```

Use the same password for the store and the key. Add the repository secrets
`QUEST_APP_KEYSTORE_B64` (`base64 -i visivo-vr-release.p12`),
`QUEST_APP_KEYSTORE_PASS` and, if the alias is not `visivo-vr`,
`QUEST_APP_KEY_ALIAS`. Then put the certificate's SHA-256 (printed by the
build and in `visivo-vr.json`) in `backend/app/vr_app.json`
(`release_sha256`), so every backend serves the release app's statement. A
backend can also take it from `VISIVO_VR_APP_RELEASE_SHA256`.

### In the releases

`.github/workflows/quest-app.yml` builds and signs the release APK. It runs:

- from `release.yml` (as a reusable workflow), once per release run, before
  the desktop packages: every package bundles that APK, and on a tag it also
  attaches `visivo-vr.apk` and `visivo-vr.json` to the release as assets of
  their own (once: `quest-app.yml` has no tag trigger of its own);
- on demand (**Actions → quest-app → Run workflow**), to check the signing:
  the APK, its `.json` and `assetlinks.json` are a workflow artifact. With
  **Meta Horizon Store build** ticked, the run fails unless `check-store.py`
  passes, and the artifact `visivo-vr-store-<versionCode>` is the APK to
  upload to the Store, with the checks' report
  ([docs/technical/deployment/meta-store.md](../../docs/technical/deployment/meta-store.md)).

Every run checks the APK against the Store's requirements (the run's summary
shows them); outside a Store build a failure is a warning only.

It fails when the certificate it built is not in `vr_app.json`, so no package
ships an app its backend does not list. A tagged release requires the app:
without it every platform's build fails and the release is not published. On
a manual run, or in a fork without the secrets, it ends with a notice and the
packages are built without the app; if its build fails, they are built
without it, with a warning.

Where the APK ends up in each package, as `visivo-vr.apk` with
`visivo-vr.json` beside it (where the desktop looks: `apkCandidates` in
`src/app/VrHeadsetApp.h`):

| Package | Folder |
|---|---|
| macOS `.dmg` | `VisIVOVisualAnalytics.app/Contents/Resources/quest-app/` (added before the bundle is signed) |
| Linux `.tar.gz` | `VisIVOVisualAnalytics-linux-x86_64/app/usr/bin/quest-app/` |
| Windows `.zip` | `VisIVOVisualAnalytics-windows-x86_64/bin/quest-app/` |

## The scanner (issue #383)

The app's icon opens `ScanActivity`, a 2-D panel (category
`com.oculus.intent.category.2D`, its own task). It reads the desktop's QR
code with the headset's passthrough camera: Horizon OS's Passthrough Camera
API, which is Android's Camera2 on a Quest 3 / 3S with Horizon OS v74 or
later. The forward RGB cameras carry the vendor tag
`com.meta.extra_metadata.camera_source` = 0; the left one
(`com.meta.extra_metadata.position` = 0) is used, at 1280×960. Only the
luminance plane is read and decoded on the headset by ZXing's core
(`com.google.zxing:core:3.5.3`, Apache-2.0); nothing is kept or sent. The
viewfinder is that plane at half size, grey, ten times a second.

**Permissions** (manifest, asked for at the first start):
`android.permission.CAMERA` and `horizonos.permission.HEADSET_CAMERA` (Meta
documents either as enough for the passthrough camera; both are asked for).
Refused, the panel says how to allow them and the code can be typed.

A QR code read (or a code typed) opens `LauncherActivity` with
`<loader>/?launch=immersive#pair=<code>`; the panel closes (Horizon OS shows
one mode at a time). When the VR session ends, the browser closes its part
and `LauncherActivity`, back on top, opens the scanner again in Home and
goes. The app declares Horizon OS SDK 69 (`horizonos:uses-horizonos-sdk`),
the level of hybrid apps (a panel and an immersive activity in one APK).

**Why native.** In the immersive app mode the browser shows no 2-D page, so
a web form or a camera view in the loader would never be seen; and the
Quest Browser's camera for web pages (`getUserMedia` on the passthrough
cameras) is still an experiment, not available to every headset.

**A QR code of another loader** (an institute's) is refused with its host
named: a code only means something to the pairing service of the loader
that shows it, and this app opens only the loader it was built for.

## Origin and port

The scanner's launches open the loader of `app.json` (`loaderOrigin`, by
default `https://vr.visivolab.oact.inaf.it`): its host must publish the
app's statement at `/.well-known/assetlinks.json` (the file is
`web/vr-loader/.well-known/assetlinks.json`, made by `make-assetlinks.py`).
The headset's browser fetches it at every launch and closes the app when the
app is not listed. The same statement verifies the app's link to that host,
so `https://<loader>/#pair=<code>` (the desktop's QR code) opens the app
when something hands it to Android (the headset's own QR scanner, if it does).

The desktop's USB launch opens `http://localhost:<port>/vr/?launch=immersive`
through `adb reverse`: `localhost` is a secure context for WebXR without a
certificate. The headset's browser verifies that origin at every launch by
fetching `http://localhost:<port>/.well-known/assetlinks.json` from the
backend, and closes the app when verification fails.

**The port is in the launch intent, not fixed in the app.** Verified on a
Quest 3 (Quest Browser 157, 2026-11-14): an APK built for
`http://localhost:8000` and started with
`-d http://localhost:8781/vr/?launch=immersive#ticket=…` passed verification
(`TWA verification was successful!`) and entered VR. The verification is the
browser's check of the launched origin against that origin's assetlinks, not
of the app's own statement, so any port works as long as the backend there
serves the statement. A fixed port would not do: the desktop's local backend
runs on 8000 or a port of its own (18900 on some setups), a cluster tunnel on
another.

`LauncherActivity` opens only a VR page of a backend on the headset's own
`localhost` or `127.0.0.1` (any port, any path prefix), or the app's loader
with a code; started with anything else (it is exported) it opens the
scanner. Up to version 1.0.1 the icon opened the last backend the desktop
had launched; that remembered address is deleted.

## The patches (`patch-project.py`)

1. `jcenter()` becomes `mavenCentral()`.
2. The launch URL and the asset statement use the loader origin's scheme
   (the template hardcodes `https://`; a development loader may be `http://localhost`).
3. The VIEW intent filter names host and port separately.
4. `LauncherActivity`: `shouldLaunchImmediately()` picks the address (a
   localhost VR page, the loader with a code, else the scanner);
   `getLaunchingUrl()` never calls the library's, which writes the launch
   URL (and so the ticket) to logcat. `onResume()` finishes the activity
   when it comes back on top (no invisible activity stays over the room once
   the VR session ends) and, after a scanner launch, opens the scanner again.
5. The manifest: the scanner is the launcher (2-D), `LauncherActivity` keeps
   its immersive category without the icon, the camera permissions, Horizon
   OS SDK 69.
6. `ScanActivity.java` and `PairCode.java` go into the app's package; ZXing's
   core is a dependency.
7. The Meta Horizon Store's rules (#386): compileSdk and targetSdk from
   `app.json` (34; the template's Quest branch has 32), `installLocation`
   auto, `com.oculus.supportedDevices` (`app.json`), head tracking required,
   a network security configuration refusing clear text (the app has no
   network permission), and a filter on `LauncherActivity` matching the
   desktop's USB launch (`VIEW` on `http(s)://localhost` or `127.0.0.1`, not
   `BROWSABLE`): an app targeting Android 13 or later may refuse an explicit
   intent from adb's shell that matches none of its filters. `build.sh` signs
   with APK Signature Scheme v2 explicitly (with minSdk 32 apksigner would
   sign with v3 alone).

Each patch fails the build loudly when the Bubblewrap template changes under
it. When updating Bubblewrap, bump it in `package.json`, run
`npm install --package-lock-only`, build, test on a Quest, and bump
`versionCode` in `app.json`.

## How the desktop uses it

The contract (install, launch, ticket, timeouts) is on issue #255 and in
[docs/technical/backend-api/vr-launch.md](../../docs/technical/backend-api/vr-launch.md). The
desktop's side is `src/app/VrAppLauncher` and `src/app/VrHeadsetApp.h`.

The desktop looks for the APK (with its `.json` beside it) in
`$VISIVO_QUEST_APK`, then in its own resources (`quest-app/visivo-vr.apk`:
`Contents/Resources` of the macOS app, beside the executable elsewhere),
then in `tools/quest-app/out/` of the source tree its build directory is in
(`visivo-vr-dev.apk` first). So for development, `build.sh` then a desktop
started from `build/` is enough. The release packages bundle the release APK
(above).
