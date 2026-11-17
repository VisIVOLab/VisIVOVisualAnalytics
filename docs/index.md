# VisIVO

VisIVO is developed by [VisIVOLab](https://visivolab.oact.inaf.it) at INAF,
Osservatorio Astrofisico di Catania. It has two components that read the same
formats (FITS, and VisIVO's Volume Binary Table, VBT):

- **VisIVO Visual Analytics**, a desktop application for spectral cubes, 2-D
  maps, 3-D catalogues, HiPS surveys and simulations. Its compute service runs
  with the application or on a remote cluster, and cubes can be explored in a
  VR headset.
- **VisIVO Server**, command-line tools that build, filter and render large
  particle tables and volumes as VBT files, for example to prepare
  simulations that the desktop application then opens.

::::{grid} 1 2 2 2
:gutter: 3
:class-container: sd-mb-4

:::{grid-item-card} Use cases
:link: user/use-cases/index
:link-type: doc

Nine tasks step by step, with screenshots: open a cube, measure a source,
moment maps and PV diagrams, regions, simulations, catalogues, polarimetry,
the VR headset, a backend on a cluster.
:::

:::{grid-item-card} Reference
:link: user/reference/index
:link-type: doc

What each command of each viewer and tool does. Start with
*Getting started* to install the application.
:::

:::{grid-item-card} Technical documentation
:link: technical/index
:link-type: doc

Architecture, backend API, deployment on clusters and SRC nodes, headset
pairing, configuration, testing, design notes and methods.
:::

:::{grid-item-card} VisIVO Server
:link: technical/server/visivo_server
:link-type: doc

The command-line tools: Importer, Filter, Viewer and Utils.
:::
::::

```{toctree}
:caption: User documentation
:hidden:
:maxdepth: 2

user/reference/getting-started
user/use-cases/index
user/reference/index
```

```{toctree}
:caption: Technical documentation
:hidden:
:maxdepth: 2

technical/index
```

## Status

| | |
|---|---|
| Active branch | `main` |
| Backend | FastAPI + Python 3.11+ |
| Client | Qt 6.10+ / VTK 9.5+ / C++17 |
| License | See repository `LICENSE` |
| Repo | <https://github.com/VisIVOLab/VisIVOVisualAnalytics> |
