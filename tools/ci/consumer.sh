#!/usr/bin/env bash
# Both downstream consumption modes, from this checkout (CLAUDE.md, "The
# consumer packages"). Run from the repository root with testthat installed.
# Called by consumer.yaml on Linux and by tools/ci/platform.sh on macOS and
# Windows.
set -eu

# The table mode: tools/zukomptest registers its own codec and drives the
# C-callable table; its tests include "core limits apply to a third-party
# codec".
R CMD INSTALL .
R CMD INSTALL tools/zukomptest
Rscript tools/ci/run-tests.R tools/zukomptest

# The archive mode: tools/zukomplink links the installed libzukomp.a for the
# ZIP reader zukomp.so deliberately does not contain. The script installs it
# through its own ./configure, audits that it declares no run-time
# dependency, and moves the installed zukomp away to prove it needs none.
./tools/check-linking.sh
