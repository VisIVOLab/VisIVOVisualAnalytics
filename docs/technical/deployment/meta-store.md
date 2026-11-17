# Publishing VisIVO VR on the Meta Horizon Store

This page is for the maintainers who publish the *VisIVO VR* headset app
(`tools/quest-app/`, package `it.inaf.oact.visivo.vr`) on the Meta Horizon
Store ([#386](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/386)).
From the Store a user installs the app on a stock Quest over Wi-Fi, with no
cable and no developer mode. The plan is a private release channel first
(testers join by invite link or email), and a public listing later.

The repository holds the APK built to the Store's rules and checked against
them, the privacy policy, the listing texts, the answers to the dashboard's
questionnaires and the image assets. An administrator of the INAF organisation
does the steps on Meta's developer dashboard by hand, in the order below.

## Meta's requirements and where the app stands

Checked on 2026-11-17 against Meta's documentation (sources at the end).
`tools/quest-app/check-store.py` checks the built APK for every row marked
*checked*, and the quest-app workflow runs it on every build.

| Requirement | The app | |
|---|---|---|
| `targetSdkVersion` 34 (apps created since 2026-03-01; immersive apps 32 to 34) | 34 | checked |
| `minSdkVersion` 29 to 34 (32 recommended) | 32 (was Bubblewrap's 23) | checked |
| `compileSdkVersion` ≥ `targetSdkVersion` | 34 | checked |
| `android:installLocation="auto"` | yes | checked |
| `com.oculus.supportedDevices` | `quest3\|quest3s` (`app.json`) | checked |
| `android.hardware.vr.headtracking` required, version 1 (immersive apps) | yes | checked |
| Not debuggable; a release build | yes | checked |
| Launcher activity with `MAIN` and `excludeFromRecents="true"` | the scanner (`ScanActivity`) | checked |
| `com.oculus.intent.category.VR` on the immersive activity | `LauncherActivity` | checked |
| A network security configuration in the manifest (VRC.Quest.Packaging.4) | clear text refused everywhere; the app itself has no network permission | checked |
| APK Signature Scheme v2 (VRC.Quest.Packaging.2); the same certificate for every version | v2 and v3, the project's release key | checked |
| 64-bit (VRC.Quest.Packaging.6) | no native code | checked |
| APK under 1 GB (VRC.Quest.Packaging.5) | about 0.7 MB | checked |
| A `versionCode` higher than every earlier upload | `app.json` `versionCode` | by hand |
| Prohibited permissions | none. `CAMERA` and `HEADSET_CAMERA` need a justification at submission (below) | checked (exact list) |
| Must not require a camera (VRC.Quest.Packaging.3) | `android.hardware.camera` not required | |
| APK, not AAB | the APK | |

Things Meta's documentation leaves open, to watch at the first upload (the
*Manage builds* page shows the result of its checks within minutes):

- the app declares the Horizon OS SDK with the older
  `horizonos:uses-horizonos-sdk` element, which Meta still accepts. The
  current name is `metavr:uses-metavr-sdk`. The older element is kept because
  it is the one tested on a Quest 3;
- whether `headtracking` required is expected for a hybrid app (a 2-D panel
  plus an immersive activity). The app declares it required, as an immersive
  app;
- what exactly VRC.Quest.Packaging.4 expects of the network security
  configuration;
- whether anything beyond the permission justification is asked for the
  passthrough camera. Meta made the Passthrough Camera API public in v76,
  with apps "published after a review process".

## The build to upload

The APK to upload is built by CI with the project's release key, never on a
laptop:

1. Bump `versionCode` (and `versionName`) in `tools/quest-app/app.json` when
   the wrapper changed since the last upload. Every upload needs a
   `versionCode` higher than any uploaded before, on any channel.
2. **Actions → quest-app → Run workflow**, on `main`, with **Meta Horizon
   Store build** ticked (the `store` input). The run fails unless every Store
   check passes. Its summary shows the checks.
3. Download the artifact `visivo-vr-store-<versionCode>`. It holds
   `visivo-vr.apk` (the file to upload), `visivo-vr.json` (version,
   certificate and APK digest), and `store-checks.txt` / `.json`.

To check an APK yourself (an Android SDK's build-tools 34.0.0):

```bash
python3 tools/quest-app/check-store.py visivo-vr.apk --build-tools "$ANDROID_HOME/build-tools/34.0.0"
```

The Store distributes the APK as uploaded, signed with the project's key, the
same key as the APK the desktop packages install over USB. So both are the same
app to Android: an app installed by the desktop is updated by the Store and the
other way round, whichever `versionCode` is higher. The Digital Asset Links
statement of the loader's host (`web/vr-loader/.well-known/assetlinks.json`)
names that key. If an app installed from the Store did not open the loader,
the first thing to check is that statement (`vr-public-loader.md`).

## On the dashboard, once

At [developers.meta.com/horizon/manage](https://developers.meta.com/horizon/manage/):

1. Organisation: Create (or use) the INAF organisation, with at least two
   administrators. Verify it (*Organization verification*: an administrator's
   government ID, or business documents). Verification is required to
   publish or update Store apps. Accept the developer agreement. A free app
   needs no payment details.
2. Create the app: *Create a new app*, name *VisIVO VR*, platform *Meta
   Horizon Store*. The app ID is in the address of its pages
   (`/manage/applications/<id>/`). The package name is set by the first
   upload, `it.inaf.oact.visivo.vr`, and never changes.
3. Privacy policy URL:
   `https://vr.visivolab.oact.inaf.it/privacy.html` (the file is
   `web/vr-loader/privacy.html`, deployed with the loader). Check that the
   URL answers before submitting.
4. App metadata: the texts and images below. The invite page of a
   private channel shows the app's name, short description and hero image,
   and the metadata can be saved without submitting the app for review.
5. Content rating (IARC) and age group: the answers below.

## Uploading a build

Any of the three works. The build lands on the channel chosen:

- *Manage builds* (or *Release channels*) on the dashboard: upload
  `visivo-vr.apk` to the **BETA** channel.
- Meta Quest Developer Hub: *App distribution*, the app, the channel,
  *Upload*.
- The command line (`ovr-platform-util`, from Meta's downloads):

  ```bash
  ovr-platform-util upload-quest-build --app-id <app id> --app-secret <app secret> \
    --apk visivo-vr.apk --channel BETA --age-group TEENS_AND_ADULTS
  ```

  The app secret is on the dashboard (*API*). Never put it in the
  repository or in a shell history. Without `--age-group` the build stays a
  draft.

After an upload, the *Test status* column of *Manage builds* shows Meta's
automatic checks within minutes. A warning there names the VRC that failed.

## Release channels and testers

- Channels: **ALPHA**, **BETA** and **RC** are private and start empty;
  **Production** is the public listing. Only Production goes to Meta's
  review (technical, then content). Builds on the private channels must still
  meet the packaging requirements above.
- Use **BETA** for the first testers. Add users by email, or turn on the
  invite link (*Release channels → BETA → Users → Invite link*), approved
  automatically or by hand.
- Testers must be 18 or older. A channel takes up to 200 users by default
  (2,500 on request, after which future data-use checkups are reviewed by
  hand).
- The invite link stops working 90 days after the last build uploaded to
  that channel. Upload a new build (or regenerate the link) to keep it
  alive.

### How a tester joins

1. Open the invite link on a phone or computer, logged in to the Meta account
   used on the headset, and press *Join* (or *Request access*). An email
   invitation is accepted from the email.
2. On the app's Store page (phone, computer or headset) press *Install*, or
   *Install on headset*.
3. On the Quest: App Library → *VisIVO VR*. At its first start it asks to
   use the headset's camera. Allow it, or refuse and type the code.
4. On the computer: VisIVO Visual Analytics, *Tools → Views → Open in VR
   Headset…*. Point the headset at the QR code, and the cube opens in VR.

No developer mode and no cable are needed. A tester already in the channel
can switch versions in the app's settings on the headset, or with the version
menu of its Store page.

## A new version

1. Change the wrapper, bump `versionCode` in `app.json` (and `versionName`),
   merge.
2. Run the store build (above) and upload its APK to **BETA**.
3. Testers get the update like any Store update. The desktop packages of the
   next release bundle the same `versionCode`.

The VR page itself is not in the app: it comes from each user's backend and
the loader's host, so a change to the VR page needs no new upload.

## The listing

**Name** (40 characters at most): `VisIVO VR`

**Short description** (shown on the invite page):

> Explore your astronomical data cubes in virtual reality, connected to
> VisIVO Visual Analytics on your computer: scan the QR code and the cube is
> in front of you.

**Long description** (1,500 characters at most):

> VisIVO VR brings the data cubes you open in VisIVO Visual Analytics into
> your Meta Quest. Radio spectral-line cubes, continuum images, catalogues
> and simulation volumes appear in front of you at full scale, and you walk
> around them, grab them, scale and crop them with your hands or
> controllers.
>
> Connecting takes one step. On your computer, choose Open in VR Headset in
> VisIVO Visual Analytics, then open VisIVO VR and look at the QR code on
> the screen. The headset connects directly to your computer and the cube
> appears. No cable, account or developer mode is needed.
>
> In VR you can scrub through spectral channels, change the colour map and
> the threshold, place a clipping plane, draw a region and extract a
> spectrum, a moment map or a position-velocity slice, and send the results
> back to the desktop. The headset renders a reduced copy of the cube so
> that every movement is immediate. When you hold still, your computer
> renders the full-quality view for your head's position.
>
> Your data stay on your computer or your institution's server and travel
> directly to the headset over an encrypted connection. The headset's
> camera is used only to read the QR code, on the headset.
>
> VisIVO VR needs VisIVO Visual Analytics, free and open source, on a
> computer on the same network: https://github.com/VisIVOLab/VisIVOVisualAnalytics
>
> Developed by VisIVOLab at INAF – Osservatorio Astrofisico di Catania.

Before a public listing, check every feature named in the long description
against the release the testers use.

**Category**: Apps. **Genres** (one to three): *Education* (under
*Learning*) first, then *Productivity*. Meta's list has no science genre.

**Website**: `https://visivolab.oact.inaf.it`
**Support**: `https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues`
**Privacy policy**: `https://vr.visivolab.oact.inaf.it/privacy.html`

The dashboard and IARC also ask for a contact email, which must be the same
in both. Use a lab mailbox, never a personal address.

## Permission justifications

Meta reviews these permissions case by case and asks for their use at
submission:

- `android.permission.CAMERA` and `horizonos.permission.HEADSET_CAMERA`:
  *"The app's start panel reads a QR code shown by VisIVO Visual Analytics on
  the user's computer, with the headset's passthrough camera (Passthrough
  Camera API, Camera2). Frames are decoded on the headset in memory (ZXing),
  only the text of the code is used, and no image is stored or transmitted.
  The app has no network permission. The camera runs only while the scanner
  panel is open. Without the permission the user types the code instead."*
- `com.oculus.permission.HAND_TRACKING` (added by Meta's web-app wrapper):
  *"Hands are an input method in the immersive web view: users grab, scale and
  crop the data cube and press menu buttons with their hands."*

## Data use

- **Data Use Checkup**: not required. The app uses no Platform SDK feature
  (no user ID, entitlement check, friends, achievements, leaderboards or
  purchases), and Meta asks for a checkup only for those features. If the
  dashboard asks for one anyway, answer that no Platform SDK feature is
  used.
- **What the app processes**, for any data or privacy question the
  dashboard asks:

  | Data | Processed | Why | Kept |
  |---|---|---|---|
  | Passthrough camera frames | on the headset only | read the pairing QR code | no, nothing leaves the headset |
  | Pairing code, IP addresses and ports (WebRTC set-up) | INAF's pairing service | connect the headset to the user's computer | in memory until the code expires (5 min, 12 h for reconnection codes). The log has IP and outcome, never the code |
  | Web server access log (IP, time, URL, user agent) | INAF's loader host | operation, security | routine log rotation |
  | Head pose, actions in VR, a low-rate copy of the user's view | the user's own backend and desktop | rendering, analysis, desktop mirror | not by INAF |
  | Hand tracking | on the headset | input | no |

  No account, name, email address, contacts, location, advertising
  identifier, analytics or third-party SDK. Nothing is sold or shared.

## Age rating and age group

**Age group** (self-certification, also the CLI's `--age-group`): *Teens and
adults (13+)* (`TEENS_AND_ADULTS`). It is a scientific tool with no content
for children. Private channel testers must be 18+ anyway.

**IARC questionnaire** (*Content ratings* on the dashboard), the expected
answers:

- Type of app: education or reference. If that is not offered, utility or
  productivity. Not a game.
- Violence, blood, fear or horror themes, sexual content or nudity, crude
  humour, strong language, drugs, alcohol or tobacco, gambling or simulated
  gambling: no.
- Users can interact with or exchange content with other users: no. The
  app connects only to the user's own computer and backend.
- Shares the user's location with other users: no.
- Digital purchases, in-app purchases, loot boxes: no.
- Unrestricted internet access (a general web browser): no. The web
  view opens only the pairing loader and the user's own backend.
- Collects or shares personal information: no. Only the connection
  metadata described in the privacy policy.

The expected rating is the lowest everywhere (ESRB Everyone, PEGI 3, USK 0).
The certificate arrives by email at the contact address.

## Images

`tools/quest-app/make-store-assets.py` makes every image Meta asks for,
except the screenshots, from the white VisIVO logo
(`tools/quest-app/store/logo-white.png`, rasterised from
`docs/_static/visivo_logo_white.svg`) over a generated, data-free
background:

```bash
backend/.venv/bin/python tools/quest-app/make-store-assets.py   # → tools/quest-app/out/store/
```

| Dashboard field | File | Size |
|---|---|---|
| Hero | `hero-3000x900.png` | 3000×900, 24-bit PNG |
| Cover Landscape | `cover-landscape-2560x1440.png` | 2560×1440, 24-bit PNG |
| Cover Square | `cover-square-1440x1440.png` | 1440×1440, 24-bit PNG |
| Cover Portrait | `cover-portrait-1008x1440.png` | 1008×1440, 24-bit PNG |
| Mini Landscape | `mini-landscape-1080x360.png` | 1080×360, 24-bit PNG |
| Logo | `logo-transparent.png` | 2387×1440, 32-bit PNG with transparency |
| Icon | `icon-512x512.png` | 512×512, 24-bit PNG, square corners |
| Screenshots | from the headset, below | 2560×1440, 24-bit PNG, five |
| Trailer (optional) | none yet | 30 s to 2 min, 1080p to 2K, MP4 (H.264, AAC) |

Meta's rules for them: no taglines or extra text on cover art, no added text
or logos on screenshots, no other platforms' headsets. Keep text out of the
top and bottom 20%.

**Screenshots**, five, taken in the headset with the release app on real
data. On the Quest: in the *Camera* app's settings, choose landscape (16:9)
captures at the highest resolution. Take each picture with the Meta button
plus the trigger. Copy them from the headset's *Pictures* (or
`adb pull /sdcard/Oculus/Screenshots`), then

```bash
backend/.venv/bin/python tools/quest-app/make-store-assets.py --screenshots <folder with the pictures>
```

crops them to 16:9 and scales them to 2560×1440
(`out/store/screenshot-1-2560x1440.png` …). Pick:

1. a radio spectral-line cube (for example an HI cube from the demo
   datasets) as a volume in front of the user, controllers visible;
2. the same cube with the channel slider or the colour-map window open (the
   hand or wrist menu);
3. an analysis in VR: a region drawn on the cube with its spectrum, or a
   moment map, in a window beside the cube;
4. the clipping plane or the crop box cutting into a cube;
5. the start panel reading the desktop's QR code (the scanner with its
   viewfinder and the code field).

A simulation volume or a catalogue is a good sixth, if its data may be shown
without a credit line.

## When the private channel works

The issue is done when a tester with a stock Quest 3 joins from the link,
installs *VisIVO VR* over Wi-Fi, opens it, scans the desktop's QR code and is
in front of the cube. Then:

- say in the user reference (`docs/user/reference/vr.md`, *The VisIVO VR app*)
  that the app is installed from the Store's link, with the USB install as
  the alternative;
- for the public listing: submit **Production** for review at least two weeks
  before the wanted date, with the full metadata and five screenshots. A
  first submission can be shown with an *Early Access* badge.

## Sources

Meta's documentation, read on 2026-11-17:

- Application manifest requirements: <https://developers.meta.com/horizon/resources/publish-mobile-manifest/>
- Target SDK 34 for apps created since 1 March 2026: <https://developers.meta.com/horizon/blog/meta-quest-apps-android-14-march-1>
- Virtual Reality Checks: <https://developers.meta.com/horizon/resources/publish-quest-req/>; packaging: [Packaging.2](https://developers.meta.com/horizon/resources/vrc-quest-packaging-2/) (signing), [Packaging.3](https://developers.meta.com/horizon/resources/vrc-quest-packaging-3/) (no required camera), [Packaging.4](https://developers.meta.com/horizon/resources/vrc-quest-packaging-4/) (SDK, network security configuration), [Packaging.5](https://developers.meta.com/horizon/resources/vrc-quest-packaging-5/) (size), [Packaging.6](https://developers.meta.com/horizon/resources/vrc-quest-packaging-6/) (64-bit)
- Permissions requiring review: <https://developers.meta.com/horizon/resources/permissions-review-required/>; prohibited: <https://developers.meta.com/horizon/resources/permissions-prohibited/>
- Web apps (Bubblewrap): <https://developers.meta.com/horizon/documentation/web/pwa-packaging/>
- Hybrid apps: <https://developers.meta.com/horizon/documentation/spatial-sdk/hybrid-apps-overview>; Horizon OS SDK versions: <https://developers.meta.com/horizon/documentation/android-apps/horizon-os-sdk-versioning/>
- Passthrough Camera API: <https://developers.meta.com/horizon/documentation/spatial-sdk/spatial-sdk-pca-overview>
- Uploading: <https://developers.meta.com/horizon/resources/publish-upload-overview/>; command line: <https://developers.meta.com/horizon/resources/publish-reference-platform-command-line-utility/>
- Release channels: <https://developers.meta.com/horizon/resources/publish-release-channels/>; adding users: <https://developers.meta.com/horizon/resources/publish-release-channels-add-users/>
- Review: <https://developers.meta.com/horizon/resources/publish-app-review/>; submission: <https://developers.meta.com/horizon/resources/publish-submit/>
- Data use checkup: <https://developers.meta.com/horizon/resources/publish-data-use/>; privacy policy: <https://developers.meta.com/horizon/policy/privacy-policy/>
- Age groups: <https://developers.meta.com/horizon/resources/age-groups/>; metadata and content ratings: <https://developers.meta.com/horizon/resources/publish-app-metadata/>
- Asset guidelines: <https://developers.meta.com/horizon/resources/asset-guidelines/>; genres: <https://developers.meta.com/horizon/resources/publish-taxonomy-guidelines/>
- Organisation verification: <https://developers.meta.com/horizon/resources/publish-organization-verification/>; creating an app: <https://developers.meta.com/horizon/resources/publish-create-app/>
