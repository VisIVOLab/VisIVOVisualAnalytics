# Building VisIVO Visual Analytics (C++ client)

This document covers how to obtain dependencies, configure, and compile the desktop client on macOS, Linux and Windows. The Windows recipe below is the one the release workflow ([`.github/workflows/release.yml`](.github/workflows/release.yml)) runs on every tag, step for step.

---

## Requirements

| Dependency | Minimum version | Notes |
|---|---|---|
| CMake | 3.22 | |
| C++ compiler | C++17 | GCC 11+, Clang 14+, or MSVC 2022 |
| Qt | 6.10 | Modules: Concurrent, Widgets, Xml, Svg, Network, NetworkAuth, PrintSupport, WebEngineWidgets, WebSockets |
| VTK | 9.5 | Modules: CommonColor, FiltersModeling, FiltersStatistics, GUISupportQt, InteractionStyle, RenderingAnnotation, RenderingImage, RenderingVolumeOpenGL2 |
| cfitsio | any recent | Provides FITS I/O |

Qt 6.10 and VTK 9.5 are the pinned minimums enforced in `CMakeLists.txt`. Older versions will not configure.

---

## macOS (recommended: vcpkg or Homebrew + manual Qt)

### Option A — vcpkg (all deps from one manifest)

vcpkg is the simplest way to get a matching VTK + cfitsio build on macOS. Qt must be installed separately because its license terms prevent redistribution in binary form via vcpkg.

```bash
# 1. Install Qt 6.10 from https://www.qt.io/download-qt-installer
#    Typical install prefix: ~/Qt/6.10.x/macos

# 2. Clone vcpkg (or use an existing installation)
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh

# 3. Install VTK and cfitsio
~/vcpkg/vcpkg install vtk cfitsio --triplet x64-osx

# 4. Configure
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=~/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.x/macos"

# 5. Build
cmake --build build --parallel
```

### Option B — Homebrew

```bash
brew install cmake vtk cfitsio

# Install Qt 6 from qt.io installer (Homebrew Qt 6 may be a different minor version)
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.x/macos;$(brew --prefix vtk);$(brew --prefix cfitsio)"

cmake --build build --parallel
```

---

## VLKB sign-in (optional)

Settings ▸ VLKB ▸ *Sign in* uses an OpenID Connect authorization-code flow
against the IA2 single sign-on. Its endpoints and scope are public and are the
CMake defaults; the client credentials are not in the repository. Pass them at
configure time; they end up only in the generated `AuthKeys.h` of the build
tree:

```bash
cmake -S . -B build ... \
  -DVISIVO_VLKB_CLIENT_ID=... \
  -DVISIVO_VLKB_CLIENT_SECRET=...
```

Without them the build works, public VLKB services included, and the Settings
dialog shows the sign-in as not available in this build. The release workflow
reads the same values from the repository secrets `VLKB_CLIENT_ID` and
`VLKB_CLIENT_SECRET`. `VISIVO_VLKB_AUTH_URL`, `VISIVO_VLKB_TOKEN_URL` and
`VISIVO_VLKB_SCOPE` override the endpoints if they ever change.

## Linux

### Ubuntu / Debian

Ubuntu 24.04 LTS ships Qt 6.4 which is too old. Use the Qt online installer for Qt 6.10, and build VTK from source or use vcpkg.

```bash
# System deps
sudo apt-get install -y \
  build-essential cmake ninja-build \
  libgl1-mesa-dev libglu1-mesa-dev \
  libcfitsio-dev

# Qt 6.10 — download from https://www.qt.io/download-qt-installer
# Install to e.g. ~/Qt/6.10.x/gcc_64

# VTK 9.5 — build from source or use vcpkg
git clone --depth 1 -b v9.5.0 https://github.com/Kitware/VTK.git vtk-src
cmake -S vtk-src -B vtk-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DVTK_GROUP_ENABLE_Qt=YES \
  -DQt6_DIR=$HOME/Qt/6.10.x/gcc_64/lib/cmake/Qt6 \
  -DCMAKE_INSTALL_PREFIX=$HOME/opt/vtk
cmake --build vtk-build --parallel
cmake --install vtk-build

# Configure the project
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.10.x/gcc_64;$HOME/opt/vtk"

cmake --build build --parallel
```

### Fedora / RHEL

```bash
sudo dnf install -y cmake ninja-build gcc-c++ \
  cfitsio-devel mesa-libGL-devel mesa-libGLU-devel

# Qt 6.10 and VTK 9.5 via the Qt online installer + vcpkg (same as Ubuntu above)
```

---

## Windows (MSVC 2022, everything from source)

Windows is a supported platform: the release workflow builds and packages it on
every tag, and the commands below are that workflow's steps written out for a
developer machine. Two shortcuts that look natural do **not** work, which is why
this recipe avoids them:

* `vcpkg install vtk` builds VTK **without Qt support** (no `GUISupportQt`), so
  the project refuses to configure; and `vtk[qt]` pulls a second Qt from vcpkg
  that clashes with the one from qt.io. Build VTK from source against your Qt.
* MSVC has no system zlib, and cfitsio's CMake build needs one: build zlib first.

Expect about an hour for VTK; everything else takes minutes.

### 1. Tools

* **Visual Studio 2022** (Community is fine) with the *Desktop development with
  C++* workload. It ships CMake and Ninja.
* **Git**.
* **Qt 6.10.1** for `msvc2022_64`, with the add-ons the client links:
  **Qt WebEngine**, **Qt WebChannel**, **Qt WebSockets**, **Qt Positioning** and
  **Qt Network Authorization**. With the Qt online installer, tick them under
  *Qt 6.10.1 ▸ Additional Libraries*. Or script it the way CI does (any Python):

  ```bat
  py -m pip install aqtinstall
  py -m aqt install-qt windows desktop 6.10.1 win64_msvc2022_64 ^
     -m qtwebengine qtwebchannel qtwebsockets qtpositioning qtnetworkauth -O C:\Qt
  ```
* **Python 3.11+** only if you want to run the backend from your own
  environment (step 6); the source build itself does not need it.

Open **x64 Native Tools Command Prompt for VS 2022** (not the plain Developer
prompt, which may default to 32-bit) and run every command below in it. Keep
the paths short: VTK's build tree is deep and long paths break MSVC tools.
The examples use `C:\src` for sources and `C:\deps` for installed libraries,
with forward slashes in CMake paths.

### 2. zlib 1.3.1

```bat
cd C:\src
git clone --depth 1 --branch v1.3.1 https://github.com/madler/zlib.git zlib-src
cmake -S zlib-src -B zlib-build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON -DCMAKE_INSTALL_PREFIX=C:/deps/zlib
cmake --build zlib-build --parallel
cmake --install zlib-build
```

### 3. cfitsio 4.6.3

`curl` and `tar` are built into Windows 10 and 11.

```bat
cd C:\src
curl -fsSL -o cfitsio.tar.gz https://heasarc.gsfc.nasa.gov/FTP/software/fitsio/c/cfitsio-4.6.3.tar.gz
mkdir cfitsio-src
tar -xzf cfitsio.tar.gz --strip-components=1 -C cfitsio-src
cmake -S cfitsio-src -B cfitsio-build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON -DUSE_BZIP2=OFF -DUSE_CURL=OFF -DTESTS=OFF -DUTILS=OFF ^
  -DCMAKE_PREFIX_PATH=C:/deps/zlib -DCMAKE_INSTALL_PREFIX=C:/deps/cfitsio
cmake --build cfitsio-build --parallel
cmake --install cfitsio-build
```

### 4. VTK 9.5.0 with Qt

```bat
cd C:\src
git clone --depth 1 --branch v9.5.0 https://gitlab.kitware.com/vtk/vtk.git vtk-src
cmake -S vtk-src -B vtk-build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DBUILD_SHARED_LIBS=ON -DBUILD_TESTING=OFF -DVTK_BUILD_EXAMPLES=OFF ^
  -DVTK_GROUP_ENABLE_Qt=YES -DVTK_QT_VERSION=6 ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.10.1/msvc2022_64 ^
  -DCMAKE_INSTALL_PREFIX=C:/deps/vtk
cmake --build vtk-build --parallel
cmake --install vtk-build
```

### 5. VisIVO

From the repository root:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="C:/Qt/6.10.1/msvc2022_64;C:/deps/vtk;C:/deps/cfitsio;C:/deps/zlib" ^
  -DVISIVO_ENABLE_WEBRTC=OFF ^
  -DCMAKE_INSTALL_PREFIX=C:/visivo
cmake --build build --parallel
```

`-DVISIVO_ENABLE_WEBRTC=OFF` gives the same client as the Windows release.
The optional H.265 WebRTC transport for remote rendering needs libdatachannel
(fetched at configure time, and it needs an MSVC OpenSSL) plus FFmpeg
development libraries. The CI runner has OpenSSL preinstalled but no FFmpeg,
so the release is built without WebRTC as well, and remote rendering uses
the JPEG-over-WebSocket transport. Leave the option at its default only if you
have installed OpenSSL (`OPENSSL_ROOT_DIR`) and FFmpeg (`.pc` files that
pkg-config can find).

To get a runnable folder, install it: `cmake --install` runs `windeployqt`,
which puts the Qt DLLs and plugins next to the executable. Then copy the
VTK, cfitsio and zlib DLLs beside it too, as the release does, because the
Qt deploy step does not follow them:

```bat
cmake --install build
for %d in (C:\deps\vtk\bin C:\deps\cfitsio\bin C:\deps\zlib\bin) do copy /Y %d\*.dll C:\visivo\bin\
C:\visivo\bin\VisIVOVisualAnalytics.exe
```

(In a `.bat` file write `%%d` instead of `%d`.) For an edit-build-run loop
straight from `build\`, configure once with `-DVISIVO_AUTO_WINDEPLOYQT=ON`,
which runs `windeployqt` after every build. The post-build step copies the
backend code and the VTK and cfitsio DLLs next to `build\VisIVOVisualAnalytics.exe`,
but not zlib (cfitsio links it by file path, so CMake cannot see the
dependency). Copy it once:

```bat
copy /Y C:\deps\zlib\bin\*.dll build\
```

### 6. Backend (Python)

The client starts the backend itself and looks for an environment at
`%APPDATA%\VisIVO\.venv`. If a Python 3.11+ is on `PATH` it creates that
environment on first launch; to do it by hand:

```bat
py -3.12 -m venv "%APPDATA%\VisIVO\.venv"
"%APPDATA%\VisIVO\.venv\Scripts\python.exe" -m pip install -r backend\requirements.txt
```

`healpy` has no Windows wheels and is skipped there by an environment marker in
`requirements.txt`, so the HiPS tile-query endpoints are unavailable on Windows;
everything else runs. The packaged release avoids all of this by shipping its own
interpreter in `bin\python`.

---

## CMake variables reference

| Variable | Purpose | Example |
|---|---|---|
| `CMAKE_PREFIX_PATH` | Colon/semicolon-separated list of install prefixes searched for `find_package` | `~/Qt/6.10.x/macos;~/opt/vtk` |
| `CMAKE_TOOLCHAIN_FILE` | vcpkg toolchain (only needed when using vcpkg) | `~/vcpkg/scripts/buildsystems/vcpkg.cmake` |
| `CMAKE_BUILD_TYPE` | `Release`, `Debug`, `RelWithDebInfo` | `Release` |
| `CMAKE_INSTALL_PREFIX` | Where `cmake --install` copies the bundle | `/usr/local` |

---

## Running the built binary

The executable is placed at `build/VisIVOVisualAnalytics` (`build\VisIVOVisualAnalytics.exe` on Windows with the Ninja generator above; a Visual Studio generator puts it in `build\Release\`). On Windows see step 5 above for the DLLs it needs beside it.

On macOS you may need to set `DYLD_LIBRARY_PATH` if Qt or VTK were not installed system-wide:

```bash
export DYLD_LIBRARY_PATH="$HOME/Qt/6.10.x/macos/lib:$HOME/opt/vtk/lib:$DYLD_LIBRARY_PATH"
./build/VisIVOVisualAnalytics
```

On Linux:

```bash
export LD_LIBRARY_PATH="$HOME/Qt/6.10.x/gcc_64/lib:$HOME/opt/vtk/lib:$LD_LIBRARY_PATH"
./build/VisIVOVisualAnalytics
```

---

## Backend

The Python backend is in `backend/`. See the "Setup" section in [README.md](README.md) for pip, conda, Apptainer, and SLURM deployment instructions.
