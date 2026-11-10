#!/bin/sh
# The static archive is rebuilt whenever the shared object is (#76).
#
# libzukomp.a's miniz_zip.o sits outside OBJECTS, so R's shlib-clean never
# removed it, and it depended on miniz.c alone. An in-tree build after a
# change to miniz.h -- a re-vendor, a patch -- then installed the new header
# beside the old compiled archive, even under --preclean: exactly the silent
# layout mismatch CLAUDE.md's MINIZ_NO_TIME rule warns consumers about,
# delivered to zuxlsx.
#
# This builds a copy of the tracked tree in place, changes MZ_VERSION in the
# vendored header, and rebuilds -- once with --preclean, once without -- then
# requires the new version string in the installed archive as well as in
# zukomp.so. The string is a literal both compile in (mz_version() returns
# it; komp_info()$vendored reads the macro), so finding it is finding a
# rebuilt object.
#
#   ./tools/check-archive-fresh.sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

copy_tree() {
    rm -rf "$WORK/pkg"
    mkdir -p "$WORK/pkg"
    (cd "$ROOT" && git ls-files -z | xargs -0 tar cf -) | tar xf - -C "$WORK/pkg"
}

install() {
    R CMD INSTALL --no-test-load --library="$WORK/lib" "$@" "$WORK/pkg" \
        > "$WORK/install.log" 2>&1 || { tail -40 "$WORK/install.log"; exit 1; }
}

probe() {
    # $1: the label for the log, $2: the version string to require.
    archive=$(find "$WORK/lib/zukomp/lib" -name libzukomp.a | head -1)
    so=$(find "$WORK/lib/zukomp/libs" -name 'zukomp.so' -o -name 'zukomp.dll' | head -1)
    status=0
    for f in "$archive" "$so"; do
        if LC_ALL=C grep -a -q -F "$2" "$f"; then
            echo "ok    $1: $(basename "$f") carries $2"
        else
            echo "FAIL  $1: $(basename "$f") does not carry $2"
            status=1
        fi
    done
    return $status
}

bump() {
    # Rewrites `#define MZ_VERSION "x"` to "$1", and makes sure the header
    # is newer than every object, whatever the file system's clock grain.
    sed "s/^#define MZ_VERSION \".*\"/#define MZ_VERSION \"$1\"/" \
        "$WORK/pkg/src/vendor/miniz/miniz.h" > "$WORK/miniz.h"
    sleep 1
    cp "$WORK/miniz.h" "$WORK/pkg/src/vendor/miniz/miniz.h"
    grep -q "^#define MZ_VERSION \"$1\"" "$WORK/pkg/src/vendor/miniz/miniz.h"
}

mkdir -p "$WORK/lib"
fail=0

copy_tree
install
bump "9.9.1-zukomp-probe"
install --preclean
probe "after --preclean" "9.9.1-zukomp-probe" || fail=1

copy_tree
install
bump "9.9.2-zukomp-probe"
install
# Without --preclean R rebuilds no object for a header change -- its own
# rules have no header dependencies -- so only the archive is required to
# follow, through the dependency src/Makevars gives miniz_zip.o.
archive=$(find "$WORK/lib/zukomp/lib" -name libzukomp.a | head -1)
if LC_ALL=C grep -a -q -F "9.9.2-zukomp-probe" "$archive"; then
    echo "ok    incremental: libzukomp.a carries 9.9.2-zukomp-probe"
else
    echo "FAIL  incremental: libzukomp.a does not carry 9.9.2-zukomp-probe"
    fail=1
fi

exit $fail
