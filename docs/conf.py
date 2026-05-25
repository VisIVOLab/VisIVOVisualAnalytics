"""Sphinx configuration for VisIVO Visual Analytics docs.

Designed to build cleanly both locally (``make html`` in this directory) and
on Read the Docs (via ``.readthedocs.yaml`` in the repository root). All
content lives as Markdown next to this file; MyST parses ``.md`` into
Sphinx's document tree.
"""

from __future__ import annotations

import datetime
import os
import sys

# ── Project information ──────────────────────────────────────────────────────

# Site name: the docs now cover both the desktop tool (Visual Analytics)
# AND the CLI toolkit (Server), so the top-level project name is just
# "VisIVO". Each component identifies itself in its respective section /
# page titles.
project = "VisIVO"
author = "VisIVOLab"
copyright = f"{datetime.datetime.now().year}, {author}"
# Single-source the release string from the top-level CMakeLists.txt project()
# version when possible; fall back to "dev" if the file is unavailable.
release = "dev"
try:
    _cml = os.path.join(os.path.dirname(__file__), "..", "CMakeLists.txt")
    with open(_cml, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line.lower().startswith("project("):
                # project(VisIVOVisualAnalytics VERSION 0.0.2 …)
                tokens = line.split()
                for i, tok in enumerate(tokens):
                    if tok.upper() == "VERSION" and i + 1 < len(tokens):
                        release = tokens[i + 1].rstrip(")")
                        break
                break
except OSError:
    pass

version = release  # short X.Y; same as full release here

# ── General configuration ────────────────────────────────────────────────────

extensions = [
    # Markdown via MyST. Most content in docs/ is `.md`; MyST lets Sphinx
    # ingest it without a conversion step.
    "myst_parser",
    # Generates :ref:`label` targets automatically from every section heading
    # so cross-document links Just Work.
    "sphinx.ext.autosectionlabel",
    # "Copy" button on code blocks.
    "sphinx_copybutton",
    # Grids / cards / dropdowns / tabs (modern docs UI primitives).
    "sphinx_design",
]

# Treat both .rst and .md as source documents.
source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

# MyST extensions used in the markdown sources.
myst_enable_extensions = [
    "colon_fence",      # ::: admonitions
    "deflist",          # term/definition lists
    "linkify",          # auto-link bare URLs
    "substitution",     # |variables|
    "tasklist",         # GitHub-style task lists
    "attrs_inline",     # inline attributes
    "fieldlist",        # field lists in markdown
]
myst_heading_anchors = 4   # generate slugs for h1..h4

# autosectionlabel: prefix labels with the document path so identical headings
# in different files don't collide. Cap at depth 2 so we don't generate labels
# for repeated sub-headings (e.g. "Status" / "Contract" appear under every
# service in service-mapping-note.md and would otherwise produce duplicate
# label warnings).
autosectionlabel_prefix_document = True
autosectionlabel_maxdepth = 2

templates_path = ["_templates"]

# Skip the sphinx scaffolding directory (left over from sphinx-quickstart),
# the build output, and the docs python project files / lock files which are
# not documentation content.
exclude_patterns = [
    "build",
    "source",
    "_build",
    "Thumbs.db",
    ".DS_Store",
    "requirements.txt",
    "pyproject.toml",
    "uv.lock",
    "Makefile",
    ".gitignore",
    # Session-local scratch notes — git-ignored, never published. Excluded
    # here so Sphinx does not warn about a stray file when a developer has
    # one in their working tree.
    "context-chat.md",
]

# ── HTML output ──────────────────────────────────────────────────────────────

html_theme = "furo"
html_title = f"{project} {release}"
html_static_path = ["_static"]
html_show_sourcelink = False

# Furo customisation — light + dark palettes consistent with the desktop app's
# "deep blue + accent" theme. Two logo variants: dark wordmark on light theme,
# white wordmark on dark theme — both live in docs/_static/.
html_theme_options = {
    # When a logo is set, hide the redundant text title in the sidebar header.
    "sidebar_hide_name": True,
    "navigation_with_keys": True,
    # Light theme uses the brand-coloured wordmark (orange "i" dot + blue
    # orbit + dark-blue lettering) — same SVG shipped as the app icon at
    # icons/VisIVO.svg. Dark theme uses the all-white variant.
    "light_logo": "visivo_logo_light.svg",
    "dark_logo":  "visivo_logo_white.svg",
    "light_css_variables": {
        "color-brand-primary":  "#0b5fff",
        "color-brand-content":  "#0b5fff",
    },
    "dark_css_variables": {
        "color-brand-primary":  "#6aa6ff",
        "color-brand-content":  "#6aa6ff",
    },
    "footer_icons": [
        {
            "name": "GitHub",
            "url":  "https://github.com/VisIVOLab/ViaLacteaVisualAnalytics",
            "html": "",  # furo uses an svg icon by default
            "class": "fa-brands fa-github",
        },
    ],
}

# Favicon shown in browser tabs (same logo, tiny scale). Use the coloured
# variant so it's recognisable on both light and dark browser chromes.
html_favicon = "_static/visivo_logo_light.svg"

# Disable Sphinx's noisy warning about Pygments lexers it can't infer from a
# fenced block language tag (most code blocks here are illustrative).
#
# Also suppress `myst.xref_missing` for inter-document `page#anchor` links.
# MyST emits this warning whenever a link's anchor cannot be statically
# verified against another document's heading slug (it can't follow them
# across pages with the current parser settings), but the generated HTML
# links *do* work because `myst_heading_anchors = 4` creates the matching
# slugs on the target pages. Suppressing keeps the strict build (`-W`) green
# without papering over real broken cross-references between Sphinx labels.
suppress_warnings = [
    "misc.highlighting_failure",
    "myst.xref_missing",
]
