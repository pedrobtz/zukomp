#!/usr/bin/env bash
# The real archive consumer, not a fixture of it (#35): zuxlsx@main built
# against this checkout's zukomp. Run from the repository root.
#
# zuxlsx tracks zukomp at @main (`Remotes: pedrobtz/zukomp@main`), so every
# merge here is a build input there. On 2026-09-19 the lib/ -> lib${R_ARCH}
# move broke zuxlsx's Windows build and zuxlsx found out; this is how zukomp
# finds out first.
#
# Every sibling zuxlsx links besides zukomp (zuxml, zucrypt) comes from its
# own main, as zuxlsx's Remotes: says. They are installed before zukomp, so
# nothing they pull in can replace the zukomp under test; zuxlsx itself is
# built with plain R CMD INSTALL rather than through pak, which would honour
# its Remotes: and install main's zukomp. When zuxlsx gains a LinkingTo
# sibling it has to be added to install-siblings.R, or this fails at
# "dependency is not available" (zucrypt, 2026-09-29).
#
# A red zuxlsx@main fails this too, whatever the change here does. That is
# accepted: the log says which package failed.
set -eu
Rscript tools/ci/install-siblings.R
R CMD INSTALL .

# Inside this checkout, never ../zuxlsx: on a developer's machine that is
# likely their own zuxlsx clone. Relative, not mktemp, for the reason
# tools/check-linking.sh gives: this runs on Windows too, where a native
# temporary path and the shell's disagree. .gitignore and .Rbuildignore
# list it, and zukomp is installed above, before it exists.
tmp=./.ci-zuxlsx
rm -rf "$tmp"
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT
git clone --depth 1 https://github.com/pedrobtz/zuxlsx.git "$tmp/zuxlsx"
git -C "$tmp/zuxlsx" log -1 --format='zuxlsx at %h: %s'

# Its own configure resolves both archives with system.file(), which is
# exactly the wiring that broke on Windows.
R CMD INSTALL "$tmp/zuxlsx"
Rscript tools/ci/run-tests.R "$tmp/zuxlsx" installed
