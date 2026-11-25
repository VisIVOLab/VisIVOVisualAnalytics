# Working with a backend on a cluster

You will start the VisIVO Visual Analytics backend on a GPU node of a SLURM
cluster from the desktop, open data that lives on the cluster, and see what
changes while you work.

## You need

- An account on a SLURM cluster with a GPU partition, and SSH access to its
  login node (the frontend) that asks for nothing when it connects: a key set
  up once with `ssh-copy-id <user>@<frontend>`, or the site's SSH certificate
  (see below).
- Your data on the cluster's disk, for example a large cube under
  `/scratch/<user>/data`.

Nothing has to be installed on the cluster. The client sends the launcher that
starts the backend, and pulls the backend's container image of its own version
the first time.

## CINECA Leonardo in three steps

1. Sign in to Leonardo's SSH certificate from a terminal with two-factor
   authentication (smallstep `step ssh login`, as CINECA describes). The
   certificate lasts for a while, so this is not needed at every connection.
2. In *Connect to a cluster…*, set *Site* to *Leonardo (CINECA)*. The login
   node, partition, GPU, resources, time and queue wait are filled in. Enter
   your *User*, the project *Account* and the *Data root* (a folder under your
   work or scratch area).
3. Press *Connect*. The first time the client pulls its backend image into the
   project's work area, which takes minutes and is shown as *Pulling the
   backend image*; the next connections reuse it. A job can wait in the queue
   for a while: the window says *Waiting in the queue* with SLURM's reason, and
   *Cancel* gives it up.

If `ssh` is refused, the window says so: on Leonardo it means the certificate
expired, so repeat step 1. The first connection to a host also needs its key
accepted once from a terminal (`ssh <user>@login.leonardo.cineca.it`, answer
yes). `login.leonardo.cineca.it` points to several login nodes, each with its
own host keys, so a key accepted on one node can be refused on the next
connection. If the window says the host key *differs from the one recorded*,
do not just replace it: compare the new fingerprint with the ones CINECA
publishes, then record the keys of every login node once (connect once to each
`loginNN-ext.leonardo.cineca.it` node from a terminal, or `ssh-keyscan` them and
check the fingerprints).

## Steps

1. In the main window's *Data Hub*, press *Connect to a cluster…*. The
   *Connect to a cluster* dialog keeps named profiles.

2. Fill in a profile:
   - *Name*: the name shown in the status bar, for example the cluster's.
   - *Site*: *Leonardo (CINECA)* or *PLEIADI (INAF)* fill in the fields the
     site defines; *Other* leaves them to you. Switching from one known site
     to another also clears what belongs to the old cluster (*VisIVO clone*,
     *Backend image file*, *Data root*, *Workspace*, *SRC node code*, *More
     options*), turns *Send the launcher from this client* on, and lists the
     fields it reset under the box. Your login, account and a `docker://`
     image stay.
   - *Frontend* and *User*: the login node and your login there.
   - *Partition*, *Account*, *QOS*, *GPUs (gres)*, *CPUs*, *Memory*, *Time
     limit* and *Wait in queue*: the SLURM resources of the job and how long
     to wait for a node. *Account* is required on some sites.
   - *Image to pull*: leave empty to use the backend image of this client's
     version (the `-egl` one when you ask for a GPU). The released packages
     do this; a build made from the repository has no image of its own, so it
     needs a `docker://...` reference here (or a *Backend image file*, or a
     clone). The same applies when you want another version.
   - *Backend image file*: a `.sif` already on the cluster, used instead of
     pulling (for example PLEIADI's own build).
   - *Data root*: the folder the backend may open data from.
   - *Workspace*: where the backend writes exports, brick pyramids and logs.
     Put it on a large disk, not in your home.
   - *Send the launcher from this client* and *VisIVO clone*: leave the box
     checked. Only developers uncheck it, to run the launcher of a VisIVO clone
     on the frontend instead of the client's. Profiles saved by an older
     client load unchecked, so they keep using their clone (empty folder:
     `~/VisIVOVisualAnalytics`); check the box to stop needing it.

   Press *Save* to keep the profile.

   ```{figure} img/09-backend-on-a-cluster-1.png
   :alt: The Connect to a cluster dialog with a filled-in profile for a GPU partition.
   :width: 70%

   A cluster profile: login node, SLURM resources, image and folders.
   ```

3. Press *Connect*. A new VisIVO Visual Analytics window starts for the
   cluster; the window you were using stays on its own backend. The new
   window submits the backend job over SSH, waits for SLURM to give it a
   node, opens an SSH tunnel to it and waits for the backend to answer. The
   progress screen says which step it is on (*Submitting the backend job
   on…*, *Waiting in the queue* with SLURM's reason, *Pulling the backend
   image*, *Starting the backend*, then the tunnel). *Cancel* stops at any
   step and cancels the job.

4. When the window opens, its status bar and backend chip name the cluster
   (*Backend: NAME*). Choose *File ▸ Open…*: the file browser lists the
   cluster's files under the data root, and its title names the backend.
   Open a cube.

   ```{figure} img/09-backend-on-a-cluster-2.png
   :alt: The Open dialog listing files on the cluster, with the backend named in the title, and the status bar of the window naming the cluster.
   :width: 100%

   The file browser on the cluster's disk.
   ```

5. In the cube window, the 3-D view's *Auto / Local / Remote* switch is set
   to *Auto*. A cube above the local limit is drawn by the cluster's GPU
   (*Remote*), and only the frames cross the network. A cube larger than
   about 6 GB is drawn from its brick pyramid, which the backend builds once
   in the workspace the first time (see
   [Large cubes and simulations](05-large-cubes-and-simulations.md)).

   ```{figure} img/09-backend-on-a-cluster-3.png
   :alt: A large cube rendered remotely on the cluster's GPU, with Remote selected in the 3-D pane header and the backend chip naming the cluster.
   :width: 100%

   A cube rendered by the cluster's GPU.
   ```

6. Work as on your own machine. Moment maps, PV diagrams, SoFiA-2, the lasso
   and the exports run on the node, next to the data. If the laptop sleeps or
   the tunnel drops, the Remote view reconnects by itself once the tunnel is
   back, with the same colours and camera.

7. When you are done, close the window, or press *Disconnect from NAME* in
   its Data Hub. Both close the tunnel and cancel the SLURM job, so the node
   is released.

## What changes for you

Paths are on the cluster. The file browser, *Recent Datasets* and the
Workspace Exports refer to the cluster's disk. *Recent Datasets* marks the
entries opened on a cluster with its name, and clicking one from another
window offers to open a window on that cluster.

The data stays there. Large cubes are not copied to your machine: what
travels is a preview, the frames of the remote view and the products you ask
for (maps, spectra, diagrams).

The window owns the job, and there is one window per profile. If a window did
not close normally, the next connection with that profile cancels the job it
left behind. Your SLURM time limit still applies: when it runs out, the
backend stops.

The window sends the cluster backend its own token, never the one of your
local backend.

The same can be started from a terminal: `VisIVOVisualAnalytics
--connect-cluster NAME` opens a window on the profile `NAME`, and a path after
it is opened on the cluster once connected. If you prefer to start the job
and the tunnel yourself with the scripts, register the backend under
*Settings ▸ Backends ▸ Remote Backends* instead; the cluster deployment page
describes both.

A Meta Quest can show cubes from the cluster backend too; see
[Headset with a cluster backend](../../technical/deployment/vr-cluster.md).

## Next

- [Running the backend on a cluster](../../technical/deployment/hpc-slurm.md): the
  launcher, the tunnel script, container images and troubleshooting.
- [Getting started](../reference/getting-started.md): connecting to a
  remote backend.
- [Spectral cube viewer](../reference/cube-viewer.md): Auto, Local and
  Remote rendering.
