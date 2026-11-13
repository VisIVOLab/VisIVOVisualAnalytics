# Controller models

The VR page draws each controller with its real model from the WebXR Input
Profiles project: <https://github.com/immersive-web/webxr-input-profiles>,
package `@webxr-input-profiles/assets` version 1.0.20 (npm), directory
`dist/profiles/`. The files are served by the backend with the page; nothing
is fetched from a CDN.

| Profile | Files | Used for |
|---|---|---|
| `meta-quest-touch-plus` | `left.glb`, `right.glb`, `profile.json` | Quest 3 and 3S |
| `meta-quest-touch-pro` | `left.glb`, `right.glb`, `profile.json` | Quest Pro, Touch Pro on a Quest 3 |
| `oculus-touch-v3` | `left.glb`, `right.glb`, `profile.json` | Quest 2 |
| `generic-trigger-squeeze-thumbstick` | `left.glb`, `right.glb`, `profile.json` | any controller with those three inputs |

The `.glb` and `profile.json` files are copied unchanged from the package.
`profilesList.json` is the package's list cut down to these four profiles.
The generic profile's `none.glb` (a controller without handedness) is left
out; such a controller gets the page's own model.

Licence: MIT, Copyright (c) 2019 Amazon, in `LICENSE.md` (the package's
`LICENSE.md`, unchanged). The package README notes that some assets portray
registered trademarks of the device manufacturers in the interest of
accurately depicting the device; the licence does not grant permission to use
those trademarks in derivative works, and they may not be used to endorse or
promote products derived from this software without specific prior written
permission.

To update: download the package (`npm pack @webxr-input-profiles/assets`),
copy the four profile directories from `dist/profiles/` (without
`generic-trigger-squeeze-thumbstick/none.glb`), check the licence is still
MIT, and run `node --test backend/tests/vr/gltf.test.mjs
backend/tests/vr/controller-profiles.test.mjs`.
