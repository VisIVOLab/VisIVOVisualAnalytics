# Roadmap

Planned work on VisIVO Visual Analytics, grouped by area and by horizon. Each item is a GitHub issue, where the motivation, scope and acceptance criteria are discussed; comments and contributions are welcome there.

Horizons:

- **Next**: within about six months.
- **2027**: substantial work planned for 2027.
- **Long term**: research items, or work that depends on external access.

The full list, with current status, is on the [issue tracker](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues?q=is%3Aissue+label%3Aarea%3Aska%2Carea%3Aecogal%2Carea%3Asimulations%2Carea%3Avisualization%2Carea%3Across-cutting) and the [milestones](https://github.com/VisIVOLab/VisIVOVisualAnalytics/milestones) page.

## Delivered

Items of this roadmap that are now in the software:

- [Backend health and metrics endpoints for node operation](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/2) (#2): `/v1/ping`, `/v1/node/status`, `/v1/metrics` ([The backend as an SRC node](deployment/src-node.md)).
- [Show which node serves a dataset and fall back to the next replica](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/1) (#1): the *Served by* column, *Open via…*, and the fallback to the next node when one refuses ([SKAVA discovery](user-guide/skava-discovery.md)).

## SKA and SRCNet

Running VisIVO Visual Analytics as part of the SKA Regional Centre Network: IVOA discovery and access, node services, source finders near the data, scale.

**Next**

- [Benchmark out-of-core moments at SKA scale on an HPC node](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/3) (#3)
- [Concurrent remote rendering sessions per backend](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/4) (#4): sessions in their own processes, capped per backend and spread over the node's GPUs; still to do: a queue above the cap, with its position shown in the client

**2027**

- [SRCNet sign-in through IAM (OIDC)](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/6) (#6): the client signs in to an OIDC provider with configurable endpoints (tested with Keycloak); still to do against the SRCNet IAM
- [Standard SODA service on the backend, with moments and PV as remote operations](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/7) (#7): sync and UWS async with `OPERATION=MOMENT|PV` are in place; still to do: `POL`, and a check with TOPCAT
- [Deploy the backend as an SRCNet node service](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/8) (#8)
- [Plugin registry for source finders (SoFiA-2, PyBDSF, Aegean, photutils)](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/9) (#9): the registry, SoFiA-2 and photutils are in place; PyBDSF and Aegean remain
- [Kinematic lasso: streaming connected components for very large objects](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/10) (#10)
- [Kinematic lasso: multi-scale selection, 2-D edit growth and velocity conventions](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/11) (#11)
- [Scaling remote 3-D rendering beyond one GPU and one node](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/73) (#73), one step at a time, each measured before the next:
  - [Brick pyramid for cubes, built out of core](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/69) (#69)
  - [Server-side out-of-core volume rendering from the brick pyramid](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/70) (#70)

**Long term**

- [Run source finders near the data through the SRCNet Computing Broker](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/12) (#12): the client submits finder jobs to a broker that follows the SRCNet job API (tested with a mock); still to do against the SRCNet broker
- [Interface for deep-learning source finders](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/14) (#14)
- [Bricked level-of-detail rendering for very large cubes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/15) (#15)
- [Multi-GPU rendering of one cube on one node](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/71) (#71), step 2 of #73
- [Distributed rendering of one view across nodes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/72) (#72), step 3 of #73

## SKAVA

SKA Visual Analytics (SKAVA): finding SKA data across SRC nodes and choosing the node that should serve it, from the VisIVO Visual Analytics client.

**2027**

- [IVOA discovery provider: TAP ObsCore and DataLink](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/5) (#5): the client queries SKAVA through TAP ObsCore and DataLink; still to do against other TAP services

**Long term**

- [Interactive placement advisor for SRCNet sessions](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/13) (#13)

## Galactic plane and multi-wavelength (ECOGAL)

Working across Galactic-plane surveys: aligned multi-band cutouts, compact sources and SEDs, filaments, molecular cubes and kinematics.

**Next**

- [VLKB service URL: one default for queries and batch cutouts](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/76) (#76)
- [Open multi-wavelength cutouts of one source on a common grid](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/16) (#16)
- [Cross-match compact-source catalogues across surveys in the GUI](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/17) (#17)
- [Polygon and rotated-box regions for VLKB cutouts](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/18) (#18)

**2027**

- [Multi-band aperture photometry feeding the SED fit](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/19) (#19)
- [Compare SED models](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/20) (#20)
- [Filament profiles and widths](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/21) (#21)
- [Associate compact sources with filaments](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/22) (#22)
- [Separate velocity components along the line of sight in molecular cubes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/23) (#23)
- [Kinematic distances](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/24) (#24)
- [Survey coverage (MOC), per-survey colour maps and pixel values in HiPS](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/25) (#25)

**Long term**

- [CAESAR as an extended-source finder](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/26) (#26)
- [Refer to datasets by identifier instead of local path](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/27) (#27)

## Simulations and cosmology

Simulation formats, particle data at scale, flow visualisation, kinematic models and cosmology tools.

**Next**

- [Open a velocity field from three vx/vy/vz files in one step](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/28) (#28)
- [Make the cosmology tools reachable from the GUI](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/29) (#29)
- [Kinematic model: several overlays and sky position angle](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/30) (#30)

**2027**

- [Native Gadget snapshot reader](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/31) (#31)
- [Out-of-core octree for particle catalogues](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/32) (#32)
- [Sparse and adaptive-mesh data in the volume renderer](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/33) (#33)
- [Velocity vectors and streamlines from particle data](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/34) (#34)
- [3-D kinematic model fit with beam smearing, on large cubes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/35) (#35)

**Long term**

- [Non-flat cosmologies in the cosmology tools](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/36) (#36)
- [Time series: light-curve viewer and dynamic-spectrum masks](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/37) (#37)

## Standard visualisation

What users expect from any astronomical viewer: linked frames, regions, scaling, publication figures, sessions, catalogues and comparison tools.

**Next**

- [Link 2-D image viewers: WCS match, pan/zoom, crosshair, colour scale](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/38) (#38)
- [Region list: several regions, loading as editable regions, export all](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/39) (#39)
- [Scale presets: zscale, asinh and percentile buttons](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/40) (#40)
- [Publication figure: DPI, PDF/SVG, beam, scale bar and compass](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/41) (#41)
- [WCS coordinate grid in the interactive image viewer, with ICRS](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/42) (#42)
- [Spectral axis units and rest frequency in the spectrum pane](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/43) (#43)

**2027**

- [Save and restore sessions, with undo/redo](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/44) (#44)
- [RGB composite of three images](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/45) (#45)
- [Line profile along a segment on 2-D images](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/46) (#46)
- [Catalogue filtering, linked plots and SAMP selections](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/47) (#47)
- [Compare images: blink any layers, swipe, multi-panel layout](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/48) (#48)
- [Contour levels by σ, logarithmic or list, and contour export](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/49) (#49)
- [Median and boxcar filters for 2-D images](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/50) (#50)
- [Pixel table around the cursor](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/51) (#51)

**Long term**

- [Python console and scripting in the GUI](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/52) (#52)

## Machine learning

Learned models that assist the user, beyond the copilot: object segmentation, source finding, similarity search, quality checks. Models propose, the user decides; data stay where they are.

**Next**

- [Self-hosted copilot: model default and base URL validation from the environment](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/75) (#75)
- [Suggest SoFiA-2 parameters from the data](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/64) (#64)

**2027**

- [Click-to-segment: learned object masks in cubes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/60) (#60)
- [Deep-learning source finders from the SKA Data Challenges](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/61) (#61)
- [Flag anomalies and artefacts when a cube opens](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/63) (#63)
- [Project and cluster catalogue columns (UMAP, t-SNE)](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/65) (#65)

**Long term**

- [Find similar objects across archives](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/62) (#62)

## Cross-cutting

Infrastructure shared by all areas.

**Next**

- [Common framework for asynchronous jobs: progress, cancel, outlive the dialog](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/53) (#53)
- [Asynchronous movie export with chosen resolution](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/54) (#54)
- [Documentation gaps: RM synthesis and time-series data](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/55) (#55)
- [Linux releases: keep the sign-in across restarts](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/56) (#56): the code is in place; still to do: libsecret in the Linux release build
- [Publish a backend container image for HPC and SRC nodes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/66) (#66)
- [Contributing guide and developer onboarding](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/67) (#67)

**2027**

- [One convention for shapes and bounds across the binary volume routes](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/74) (#74)
- [Open compressed FITS directly](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/57) (#57)
- [VR off the UI thread; server-side rendering on macOS and CPU-only hosts](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/58) (#58)
- [Evaluate the scientific copilot](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/59) (#59)
- [User study of the main analysis workflows](https://github.com/VisIVOLab/VisIVOVisualAnalytics/issues/68) (#68)
