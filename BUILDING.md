# Building VisIVO Visual Analytics (C++ client)

This document covers how to obtain dependencies, configure, and compile the desktop client on macOS and Linux.

---

## Requirements

| Dependency | Minimum version | Notes |
|---|---|---|
| CMake | 3.22 | |
| C++ compiler | C++17 | GCC 11+, Clang 14+, or MSVC 2022 |
| Qt | 6.10 | Modules: Concurrent, Widgets, Svg, Network, NetworkAuth, PrintSupport, WebEngineWidgets |
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

## Windows (MSVC + vcpkg)

```powershell
# 1. Install Qt 6.10 from qt.io installer
#    e.g. C:\Qt\6.10.x\msvc2022_64

# 2. Install vcpkg
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg install vtk cfitsio --triplet x64-windows

# 3. Open Developer Command Prompt for VS 2022, then:
cmake -S . -B build `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
  -DCMAKE_PREFIX_PATH="C:\Qt\6.10.x\msvc2022_64"

cmake --build build --config Release --parallel
```

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

The executable is placed at `build/VisIVOVisualAnalytics` (or `build/Release/VisIVOVisualAnalytics.exe` on Windows).

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
