# Sphinx documentation build for VisIVO Visual Analytics.
#
# Sources live directly in this directory (no `source/` subdir); output goes
# to ./_build/ matching what Read the Docs uses. Run `make html` and open
# _build/html/index.html.

SPHINXOPTS    ?= -n --keep-going
SPHINXBUILD   ?= sphinx-build
SOURCEDIR     = .
BUILDDIR      = _build

.PHONY: help clean

help:
	@$(SPHINXBUILD) -M help "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)

clean:
	rm -rf "$(BUILDDIR)"

# Catch-all target: route any other target name (html, linkcheck, dirhtml, …)
# through sphinx-build's "make mode".
%: Makefile
	@$(SPHINXBUILD) -M $@ "$(SOURCEDIR)" "$(BUILDDIR)" $(SPHINXOPTS) $(O)
