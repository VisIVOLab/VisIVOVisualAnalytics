# Ray-traced particles: methods and implementation

User reference: [Ray-traced particles](../../user/reference/raytraced-particles.md).
Routes: [Rendering API](../backend-api/particle-render.md).

## The method

Each particle is a Gaussian kernel whose width is its smoothing length. A ray
through the scene meets the particles back to front; each one adds its light
and, optionally, dims what lies behind it, as a slab that emits and absorbs.
The light is kept in floating point, without a ceiling, and turned into a
picture by a tone curve. This is the ray-traced splatting of SPH particles
described by Dolag et al. (2008, *New J. Phys.* 10, 125006).

Two details keep a picture faithful at any zoom. A particle smaller than a pixel
is drawn one pixel wide with the same light, so dense cores neither vanish nor
flicker; and a particle's light is weighted by what it measures, so a gas shows
its projected density and stars their mass.

The projected map style shows a simulation as cosmological simulations are
usually shown, for instance the Millennium Simulation's images (Springel et
al. 2005, *Nature* 435, 629): the density summed along the line of sight
without perspective, its brightness the logarithm of that column, its colour
the density itself or a second quantity averaged along the ray, such as the
local velocity dispersion. The Millennium images use a 15 Mpc/h slab.

The automatic exposure is the 99.5th percentile of the lit pixels. The
default neighbour count for kernel sizes is N = 32, as an SPH kernel's
neighbours.

## Data path to the GPU view

The particles are prepared by the backend once (sizes, light, colours), then
sent to the desktop: about 120 MB for 3.7 million particles. Above 8 million an
even sample is sent, each particle carrying the light of those it stands for.

Positions far from 0 keep their separations. A snapshot in metres, or one at a
large offset (1e9, 1e9 + 1, 1e9 + 2), is drawn as three particles in the GPU
view, the backend's preview, stills and movies, where the remote point view
draws them and with the same sizes. The camera, the box and the slab stay in
the table's own units.

A render in progress is cancelled when the application quits (⌘Q, the Dock's
*Quit*): the render stops on the backend, a cluster's included, before the
application exits, and its frames there are removed.

## Performance

On an Apple M-series laptop a 3.7-million-particle frame takes about a second
at 1920 × 1080, and a 21-million-particle one about seven seconds at
3840 × 2160.

Since issue #264 the renderer bins the particles into tiles in parallel and
evaluates each kernel's Gaussian once per row and column of its footprint: the
same image (within 3e-6 of the peak) about 1.5 times faster. A
3.9-million-particle 1280 × 720 frame went from 448 to 274 ms without
absorption, and from 867 to 589 ms with it.
