#!/usr/bin/env bash
# Replays the committed corpus through the standalone drivers, then holds
# the ZIP reader to Python's zipfile. Run from the repository root; needs
# clang or cc with ASan and UBSan, and python3. Called by fuzz.yaml on Linux
# and by tools/ci/platform.sh on macOS at the full profile.
set -eu

# The standalone drivers need only ASan and UBSan, so the committed crashers
# stay checkable on platforms without libFuzzer -- Apple's clang among them.
./tools/make-fuzz-corpus.sh
./fuzz/build.sh --standalone
./fuzz/replay.sh

# The ZIP reader libzukomp.a ships (#37), against Python's zipfile over the
# committed seeds in fuzz/corpus/zip/: the same entries, the same bytes, and
# each archive read or refused as APPNOTE.TXT says. The standard library is
# all it needs.
python3 tools/check-zip-reader.py
