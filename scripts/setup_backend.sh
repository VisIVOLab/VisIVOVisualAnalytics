#!/usr/bin/env bash
# Setup script for the VisIVO Visual Analytics backend (Linux).
#
# Creates a Python venv at:
#   $XDG_DATA_HOME/VisIVO/.venv  (default: ~/.local/share/VisIVO/.venv)
# and installs the backend dependencies into it. The app's BackendLauncher
# discovers this venv automatically at runtime.
#
# Optional: the release already carries its own Python in ./python. Use this
# only to run the backend on your own Python, or to add extras. The venv lives
# in a user-data dir so it survives upgrades; re-run this after upgrading the
# release to refresh dependencies.

set -e

VENV_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/VisIVO/.venv"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REQ_FILE="$SCRIPT_DIR/requirements.txt"

# The release ships requirements.txt next to this script, and a copy inside
# the application tree (./app/usr/bin/backend/).
if [ ! -f "$REQ_FILE" ] && [ -f "$SCRIPT_DIR/app/usr/bin/backend/requirements.txt" ]; then
  REQ_FILE="$SCRIPT_DIR/app/usr/bin/backend/requirements.txt"
fi

if [ ! -f "$REQ_FILE" ]; then
  echo "Error: requirements.txt was not found next to the script or in"
  echo "       $SCRIPT_DIR/app/usr/bin/backend/."
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: python3 was not found. Install Python 3.11 or newer:"
  echo "  Debian/Ubuntu: sudo apt install python3 python3-venv python3-pip"
  echo "  Fedora/RHEL  : sudo dnf install python3 python3-virtualenv"
  exit 1
fi

PY_VERSION="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
echo "Using Python $PY_VERSION at: $(command -v python3)"
echo "Venv target: $VENV_DIR"
echo "Requirements: $REQ_FILE"
echo

if [ -d "$VENV_DIR" ]; then
  read -r -p "An existing venv was found. Recreate it from scratch? [y/N] " yn
  case "$yn" in
    [Yy]*) echo "Removing existing venv..."; rm -rf "$VENV_DIR" ;;
    *)     echo "Updating existing venv..." ;;
  esac
fi

mkdir -p "$(dirname "$VENV_DIR")"
if [ ! -d "$VENV_DIR" ]; then
  echo "Creating virtual environment..."
  python3 -m venv "$VENV_DIR"
fi

echo
echo "Installing backend dependencies (this can take several minutes)..."
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install -r "$REQ_FILE"

echo
echo "Done. The backend is ready."
echo "Start ./VisIVOVisualAnalytics as usual; it prefers this venv over the bundled Python."
