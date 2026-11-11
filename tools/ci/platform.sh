#!/usr/bin/env bash
# zukomp's own per-OS checks, one job per OS: r-actions' platforms.yml runs
#
#   bash tools/ci/platform.sh <macos | windows | ubuntu>
#
# from the repository root, with R installed (Rtools on Windows), zukomp's
# dependencies installed, and R_ACTIONS_PROFILE set to `quick` (a pull
# request) or `full` (main, the weekly schedule, a dispatch, the `full-ci`
# label). A free account runs about five macOS jobs at once, so the checks
# that used to be a macOS or Windows leg of their own workflow (abi.yaml,
# consumer.yaml, fuzz.yaml) run here instead, one after another. Their Linux
# legs stay in those workflows and call the same tools/ci/ scripts.
#
# What is secondary on a platform -- a check Linux already runs on every pull
# request -- waits for the full profile.
set -eu
os=${1:?usage: platform.sh macos|windows|ubuntu}
profile=${R_ACTIONS_PROFILE:-full}
echo "platform.sh: $os, profile $profile"

step() { printf '\n==> %s\n' "$*"; }

case "$os" in
macos)
    # Apple's clang is a different compiler from Linux's gcc and clang, and
    # the headers are what downstream packages compile.
    step "zukomp.h and zukomp-r.h, compiled as a consumer compiles them"
    bash tools/ci/header-probes.sh

    step "both consumption modes: the C-callable table and libzukomp.a"
    bash tools/ci/consumer.sh

    step "zuxlsx@main builds against this zukomp"
    bash tools/ci/zuxlsx.sh

    # A second OS for the corpus replay: Linux replays it on every pull
    # request in fuzz.yaml.
    if [ "$profile" = full ]; then
        step "the fuzz corpus through the standalone drivers"
        bash tools/ci/fuzz-replay.sh
    else
        step "skipped at the quick profile: fuzz corpus replay (Linux runs it)"
    fi
    ;;
windows)
    # The platform where the archive wiring broke (lib/ -> lib${R_ARCH},
    # 2026-09-19): Rtools, a multi-arch-shaped install, configure.win.
    step "both consumption modes: the C-callable table and libzukomp.a"
    bash tools/ci/consumer.sh

    step "zuxlsx@main builds against this zukomp"
    bash tools/ci/zuxlsx.sh
    ;;
ubuntu)
    # Linux runs these in abi.yaml, consumer.yaml and fuzz.yaml already; this
    # branch is for running the script by hand on a Linux machine.
    step "zukomp.h and zukomp-r.h, compiled as a consumer compiles them"
    bash tools/ci/header-probes.sh
    step "both consumption modes: the C-callable table and libzukomp.a"
    bash tools/ci/consumer.sh
    ;;
*)
    echo "platform.sh: unknown os '$os'" >&2
    exit 2
    ;;
esac
echo
echo "platform.sh: $os done"
