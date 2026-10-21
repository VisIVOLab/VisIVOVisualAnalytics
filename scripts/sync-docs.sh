#!/usr/bin/env bash
# Manual sync of docs/ + .readthedocs.yaml into the public mirror repo
# (VisIVOLab/VisIVO-doc) that Read the Docs builds for visivo.readthedocs.io.
#
# Same behaviour as .github/workflows/sync-docs.yml — kept as a script so it
# can be triggered locally when GitHub Actions is unavailable (billing limit
# hit, hosted runners down, …) or just to preview what the workflow will do.
#
# Usage (from anywhere; the script resolves the source repo from its own
# location):
#     ./scripts/sync-docs.sh           # confirm before pushing
#     ./scripts/sync-docs.sh --yes     # skip confirmation
#
# Requirements:
#   - Write access to git@github.com:VisIVOLab/VisIVO-doc (SSH key configured
#     in ~/.ssh/config or a personal access token in your git credentials).
#   - clean working tree of the upstream `docs/` and `.readthedocs.yaml` you
#     want to publish — the script does NOT git-add anything locally.

set -euo pipefail

# ── Config ──────────────────────────────────────────────────────────────────
# Default to HTTPS so the script works out of the box on machines that use
# the OS keychain (Mac, Git Credential Manager on Windows) — same protocol
# you already use to clone/push the source repo. If you have an SSH key
# registered with GitHub, set MIRROR_REPO_URL to the git@ form:
#     MIRROR_REPO_URL="git@github.com:VisIVOLab/VisIVO-doc.git" ./sync-docs.sh
MIRROR_REPO_URL="${MIRROR_REPO_URL:-https://github.com/VisIVOLab/VisIVO-doc.git}"
MIRROR_BRANCH="${MIRROR_BRANCH:-main}"
WORKDIR="${TMPDIR:-/tmp}/visivo-doc-mirror"
SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── Pre-flight ──────────────────────────────────────────────────────────────
if [[ ! -d "$SOURCE_DIR/docs" ]]; then
    echo "❌ docs/ not found in $SOURCE_DIR — is this the right repo root?" >&2
    exit 1
fi
if [[ ! -f "$SOURCE_DIR/.readthedocs.yaml" ]]; then
    echo "❌ .readthedocs.yaml not found in $SOURCE_DIR." >&2
    exit 1
fi

CONFIRM="ask"
if [[ "${1:-}" == "--yes" ]] || [[ "${1:-}" == "-y" ]]; then
    CONFIRM="no"
fi

echo "📂 Source: $SOURCE_DIR"
echo "🛰  Mirror: $MIRROR_REPO_URL  (branch: $MIRROR_BRANCH)"
echo "💾 Workdir: $WORKDIR"

# ── Clone or refresh the mirror clone ───────────────────────────────────────
# The clone lives under $TMPDIR, which macOS prunes by age: it will happily
# delete .git/config out of an otherwise intact repository, leaving a clone
# with no `origin` and a fetch that fails with "'origin' does not appear to be
# a git repository". So the existing clone is checked for being usable, not
# merely for having a .git directory, and anything unusable is re-cloned —
# there is nothing in there to lose, it is rebuilt from the source tree below.
reusable_clone() {
    [[ -d "$WORKDIR/.git" ]] || return 1
    git -C "$WORKDIR" rev-parse --git-dir >/dev/null 2>&1 || return 1
    local url
    url="$(git -C "$WORKDIR" remote get-url origin 2>/dev/null)" || return 1
    [[ -n "$url" ]] || return 1
    # A workdir left over from a different mirror URL must not be pushed to.
    [[ "$url" == "$MIRROR_REPO_URL" ]] || return 1
    git -C "$WORKDIR" fetch origin "$MIRROR_BRANCH" >/dev/null 2>&1 || return 1
    return 0
}

if reusable_clone; then
    echo "↺  Refreshing existing clone…"
    git -C "$WORKDIR" checkout "$MIRROR_BRANCH"
    git -C "$WORKDIR" reset --hard "origin/$MIRROR_BRANCH"
else
    if [[ -e "$WORKDIR" ]]; then
        echo "⚠️  Existing workdir is not a usable clone of $MIRROR_REPO_URL — re-cloning."
    fi
    rm -rf "$WORKDIR"
    echo "↓  Cloning mirror…"
    git clone --depth 1 --branch "$MIRROR_BRANCH" "$MIRROR_REPO_URL" "$WORKDIR"
fi

# ── Wipe + repopulate ───────────────────────────────────────────────────────
echo "🧹 Wiping mirror contents (preserving .git/)…"
find "$WORKDIR" -mindepth 1 -maxdepth 1 -not -name ".git" -exec rm -rf {} +

echo "→  Copying docs/ and .readthedocs.yaml…"
cp -R "$SOURCE_DIR/docs"               "$WORKDIR/docs"
cp    "$SOURCE_DIR/.readthedocs.yaml"  "$WORKDIR/.readthedocs.yaml"

# README — kept identical to the one the GitHub Action writes so the mirror
# always carries the same disclaimer regardless of which path produced it.
cat > "$WORKDIR/README.md" <<'README'
# VisIVO documentation (auto-generated mirror)

This repository is the public source that
[Read the Docs](https://visivo.readthedocs.io/) builds for the
VisIVO project documentation. **Do not edit files here directly.**

The contents of this repo are auto-generated from the `docs/`
subtree of the source repository
[`VisIVOLab/VisIVOVisualAnalytics`](https://github.com/VisIVOLab/VisIVOVisualAnalytics)
via scripts/sync-docs.sh
or the matching GitHub Actions workflow. Every sync overwrites the
contents of this repo and re-runs the RTD build.

To propose changes to the docs, open a PR against the source repo.
README

# ── Stage + show diff ────────────────────────────────────────────────────────
cd "$WORKDIR"
git add -A
if git diff --staged --quiet; then
    echo "✅ Nothing changed — mirror already up to date."
    exit 0
fi

echo
echo "📝 Files changed:"
git diff --staged --stat
echo

# ── Confirm + push ──────────────────────────────────────────────────────────
if [[ "$CONFIRM" == "ask" ]]; then
    read -r -p "Push to $MIRROR_REPO_URL#$MIRROR_BRANCH ? [y/N] " reply
    if [[ ! "$reply" =~ ^[Yy]$ ]]; then
        echo "Aborted. Mirror left staged in $WORKDIR."
        exit 0
    fi
fi

UPSTREAM_SHA="$(git -C "$SOURCE_DIR" rev-parse --short HEAD 2>/dev/null || echo manual)"
UPSTREAM_SUBJECT="$(git -C "$SOURCE_DIR" log -1 --pretty=%s 2>/dev/null || echo 'Manual sync')"

git commit -m "Sync docs from upstream @ ${UPSTREAM_SHA}

Subject: ${UPSTREAM_SUBJECT}"
git push origin "$MIRROR_BRANCH"

echo "✅ Pushed. Read the Docs should rebuild within a couple of minutes."
echo "    Watch: https://readthedocs.org/projects/visivo/builds/"
