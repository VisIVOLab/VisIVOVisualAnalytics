# VisIVO VR headset app

The backend's VR page (`/vr/`) packaged as an immersive Horizon OS app for the
Meta Quest (issue #255, after the spike in #254). The desktop installs it over
adb and starts it with one `am start`: the user is in VR, with no browser
window and no Enter VR press. The app is a thin wrapper made with Meta's
Bubblewrap (a Trusted Web Activity in immersive mode): the page itself, and
every update of it, still comes from the backend.

## What is here

| File | What it does |
|---|---|
| `build.sh` | The whole build: key, Bubblewrap, patches, Gradle, zipalign, apksigner. |
| `app.json` | The app's identity: packages, names, `versionCode`, start path, icon. |
| `make-twa.py` | Writes Bubblewrap's `twa-manifest.json` from `app.json`. |
| `patch-project.py` | The changes to the generated Android project (below). |
| `package.json`, `package-lock.json` | Pin `@meta-quest/bubblewrap-cli` 1.24.1. |

Nothing secret is in the repository. The development key is made on your
computer at the first build; the release key is a CI secret.

## Build

Needs a JDK 17 (`JAVA_HOME`), an Android SDK (`ANDROID_HOME`; build-tools
34.0.0 and platform 32 are installed with its `sdkmanager` when missing),
node with npm, and python3.

```bash
tools/quest-app/build.sh            # development app, your own key
tools/quest-app/build.sh --release  # release app, the project's key (CI)
```

Output in `tools/quest-app/out/` (ignored by git):

- `visivo-vr-dev.apk` (or `visivo-vr.apk`): the signed APK;
- `visivo-vr-dev.json`: `package`, `version_code`, `version_name`,
  `cert_sha256`, `apk_sha256`, `start_path`, `default_origin`; the desktop
  compares `version_code` with the installed app's;
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

### Release key

Make it once, keep it safe (losing it means users must uninstall the app):

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

The workflow `.github/workflows/quest-app.yml` builds the release APK on every
tag and on demand when the secrets exist, adds it to the tag's release, and
otherwise ends with a notice and builds nothing.

## Origin and port

The app opens `http://localhost:<port>/vr/?launch=immersive` through
`adb reverse`: `localhost` is a secure context for WebXR without a
certificate. The headset's browser verifies the origin at every launch by
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

`app.json`'s `defaultOrigin` (`http://localhost:8000`) is only what a launch
from the app's icon opens the first time; afterwards the icon opens the last
backend the desktop launched it with (the app remembers that page, never its
fragment). The launcher accepts only a VR page of a backend on the headset's
own `localhost` or `127.0.0.1`, at any port and under any path prefix; any
other address is ignored.

## The patches (`patch-project.py`)

1. `jcenter()` becomes `mavenCentral()`.
2. The launch URL and the asset statement use `http://` (the template
   hardcodes `https://`).
3. The VIEW intent filter names host and port separately.
4. `LauncherActivity.getLaunchingUrl()` never calls the library's, which
   writes the launch URL (and so the ticket) to logcat; it opens only
   localhost VR pages and remembers the last one for icon launches.
   `LauncherActivity.onResume()` finishes the activity when it comes back on
   top, so no invisible activity stays over the room once the VR session ends.

Each patch fails the build loudly when the Bubblewrap template changes under
it. When updating Bubblewrap, bump it in `package.json`, run
`npm install --package-lock-only`, build, test on a Quest, and bump
`versionCode` in `app.json`.

## How the desktop uses it

The contract (install, launch, ticket, timeouts) is on issue #255 and in
[docs/backend-api/vr-launch.md](../../docs/backend-api/vr-launch.md). The
desktop's side is `src/app/VrAppLauncher` and `src/app/VrHeadsetApp.h`.

The desktop looks for the APK (with its `.json` beside it) in
`$VISIVO_QUEST_APK`, then in its own resources (`quest-app/visivo-vr.apk`:
`Contents/Resources` of the macOS app, beside the executable elsewhere),
then in `tools/quest-app/out/` of the source tree its build directory is in
(`visivo-vr-dev.apk` first). So for development, `build.sh` then a desktop
started from `build/` is enough. Bundling `visivo-vr.apk` in the release
packages is still a `release.yml` change to make.
