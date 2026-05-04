#!/usr/bin/env bash
# Setup script for the VisIVO Visual Analytics backend (macOS).
#
# Creates a Python venv at:
#   ~/Library/Application Support/VisIVO/.venv
# and installs the backend dependencies into it. The app's BackendLauncher
# discovers this venv automatically at runtime.
#
# Run this once after copying VisIVOVisualAnalytics.app to /Applications.
# Re-run it after upgrading the app to refresh dependencies.

set -e

VENV_DIR="$HOME/Library/Application Support/VisIVO/.venv"

# Try to locate the .app bundle so we can read its requirements.txt.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_CANDIDATES=(
  "$SCRIPT_DIR/VisIVOVisualAnalytics.app"
  "/Applications/VisIVOVisualAnalytics.app"
  "$HOME/Applications/VisIVOVisualAnalytics.app"
)
APP_BUNDLE=""
for candidate in "${APP_CANDIDATES[@]}"; do
  if [ -d "$candidate" ]; then
    APP_BUNDLE="$candidate"
    break
  fi
done

if [ -z "$APP_BUNDLE" ]; then
  echo "VisIVOVisualAnalytics.app was not found in any standard location."
  echo "Checked:"
  printf '  %s\n' "${APP_CANDIDATES[@]}"
  echo
  read -r -p "Drag VisIVOVisualAnalytics.app onto this terminal and press Enter: " APP_BUNDLE
  APP_BUNDLE="${APP_BUNDLE%/}"
fi

REQ_FILE="$APP_BUNDLE/Contents/Resources/backend/requirements.txt"
if [ ! -f "$REQ_FILE" ]; then
  echo "Error: requirements.txt not found at:"
  echo "  $REQ_FILE"
  read -r -p "Press Enter to close..."
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "Error: python3 was not found on this system."
  echo "Install Python 3.11 or newer from https://www.python.org/downloads/"
  read -r -p "Press Enter to close..."
  exit 1
fi

PY_VERSION="$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
echo "Using Python $PY_VERSION at: $(command -v python3)"
echo "App bundle:  $APP_BUNDLE"
echo "Venv target: $VENV_DIR"
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
echo "Launch VisIVOVisualAnalytics.app normally; it will use this venv."
read -r -p "Press Enter to close..."
