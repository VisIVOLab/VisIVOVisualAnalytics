# Technical documentation

How VisIVO Visual Analytics is built and run: the architecture, the backend
API, deployment on clusters and SRC nodes, the headset pairing service,
configuration, testing, design notes and the methods behind the tools. It
also documents the VisIVO Server command-line tools. For using the
application, see the [use cases](../user/use-cases/index.md) and the
[reference](../user/reference/index.md).

## Architecture and API

- [Architecture note](architecture-note.md)
- [Backend API reference](backend-api.md) and its area pages
- [Configuration reference](configuration.md)
- [Testing](testing.md)
- [Demo datasets](demo-datasets.md)

## Deployment

- [Running the backend on a cluster](deployment/hpc-slurm.md)
- [The backend as an SRC node](deployment/src-node.md)
- [Sharing one backend between users](multi-user-backend.md)
- [Hosting headset pairing](deployment/vr-pairing.md)
- [Headset with a cluster backend](deployment/vr-cluster.md)
- [How the public loader works](deployment/vr-public-loader.md)
- [Publishing VisIVO VR on the Meta Horizon Store](deployment/meta-store.md)
- [Self-hosted copilot LLM](deployment/copilot-self-hosted.md)

## VR headset

- [Technical reference](vr/reference.md)
- [Troubleshooting in depth](vr/troubleshooting.md)

## Methods and implementation

The methods behind the tools described in the user reference: algorithms,
their parameters, validation numbers and file formats.

```{toctree}
:maxdepth: 1
:glob:

methods/*
```

## Design notes

```{toctree}
:maxdepth: 1

design/large-cube-loading
design/brick-pyramid
design/gadget-snapshots
design/distributed-backend-design
design/copilot-backend
design/async-patterns
design/service-mapping-note
design/catalogue3d-viewer
design/velocity-field-viewer
design/kinematic-model
design/kinematic-lasso
design/movie-export
design/linked-views
```

## VisIVO Server (command-line tools)

```{toctree}
:maxdepth: 1

server/visivo_server
server/visivo_importer
server/visivo_filter
server/visivo_viewer
server/visivo_utils
```

## Project

- [Roadmap](roadmap.md)

```{toctree}
:hidden:

architecture-note
backend-api
configuration
testing
demo-datasets
deployment/hpc-slurm
deployment/src-node
multi-user-backend
deployment/vr-pairing
deployment/vr-cluster
deployment/vr-public-loader
deployment/copilot-self-hosted
vr/reference
vr/troubleshooting
roadmap
```
