#!/bin/sh
# Proves the LinkingTo archive works end to end, through the consumer shape
# that actually uses it: tools/zukomplink, which links the installed
# libzukomp.a (<pkg>/lib${R_ARCH})
# statically by its own ./configure and never loads zukomp's namespace.
# That is how zuxlsx consumes zukomp, and the configure/Makevars.in pair
# here is a copy of its wiring on purpose.
#
# tests/testthat/test-linking.R audits the archive's symbol table from
# inside zukomp. This is the other half: that the header, the archive, the
# compiler and R CMD INSTALL agree, which a symbol table cannot show. The
# hand-compiled main() this script used to carry went through none of that --
# not configure under R_HOME, not system.file("lib", .Platform$r_arch, ...),
# not path quoting, not Makevars.in substitution, and not linking into a real
# package shared object -- so every one of those could break with this script
# still green. The arch-aware resolution it did carry now lives where a
# consumer actually needs it, in tools/zukomplink/configure.
#
# Run from the package root.
set -eu

# Relative paths, not mktemp: this runs on Windows too, where the compiler is
# a native mingw binary that does not understand a Git Bash /tmp path.
LIB=./.check-linking/lib
rm -rf ./.check-linking
mkdir -p "$LIB"
trap 'rm -rf ./.check-linking' EXIT

echo "==> installing zukomp"
R CMD INSTALL --preclean --no-multiarch --library="$LIB" . >/dev/null

echo "==> the LinkingTo archive, its header and its licence are installed"
# The archive installs under R_ARCH, so "$LIB/zukomp/lib" is the right answer
# on Unix and the wrong one on Windows, where it is lib/x64. Resolved the way
# a consumer's configure resolves it rather than hardcoded -- and note
# r_arch carries no leading slash where R_ARCH does, so these are
# file.path()ed rather than pasted.
ZUKOMP_LIB=$(R_LIBS="$LIB" Rscript --vanilla -e 'arch <- .Platform$r_arch; d <- if (nzchar(arch)) system.file("lib", arch, package = "zukomp") else ""; if (!nzchar(d)) d <- system.file("lib", package = "zukomp"); cat(d)')
[ -n "$ZUKOMP_LIB" ] || { echo "FAIL: zukomp installed no lib directory" >&2; exit 1; }
[ -f "$ZUKOMP_LIB/libzukomp.a" ] || { echo "FAIL: libzukomp.a is missing from the installation" >&2; exit 1; }

# miniz's licence is a licensing obligation rather than tidiness: miniz.h
# carries no copyright line and no permission notice of its own, and an
# installed zukomp ships that header and a compiled copy of miniz inside the
# archive. tests/testthat/test-linking.R asserts the same from inside.
for f in include/miniz.h include/zukomp.h include/zukomp-r.h licenses/miniz-LICENSE; do
  [ -f "$LIB/zukomp/$f" ] || { echo "FAIL: $f is missing from the installation" >&2; exit 1; }
done

echo "==> the fixture must not declare a run-time dependency on zukomp"
# The whole point of the archive: if either of these creeps back in, the
# fixture would load zukomp's namespace and stop testing the shape zuxlsx
# actually uses -- and the "zukomp uninstalled" step below would start
# passing for the wrong reason.
if grep -qE '^(Imports|Depends):' tools/zukomplink/DESCRIPTION; then
  echo "FAIL: zukomplink declares Imports/Depends; it must link, not load" >&2
  exit 1
fi
if grep -qE '^\s*(import|importFrom)\(' tools/zukomplink/NAMESPACE; then
  echo "FAIL: zukomplink imports a namespace; it must link, not load" >&2
  exit 1
fi

echo "==> the committed ZIP fixture is what zukomp's codecs produce today"
# Closes the round trip across both halves of the package: zukomp's own
# deflate-raw and gzip codecs wrote probe.zip's streams and CRCs, and the
# fixture's tests decompress them through miniz's ZIP reader. If a codec
# change altered either, this says so here rather than as a puzzling
# CRC failure inside the fixture.
R_LIBS="$LIB" Rscript --vanilla tools/make-link-fixture.R --check

# --preclean so ./configure and src/Makevars.in are exercised from scratch on
# every run: a configure that resolves the archive only because a stale
# src/Makevars was left behind is the failure this is here to catch.
echo "==> installing the archive consumer (configure resolves the archive)"
R_LIBS="$LIB" R CMD INSTALL --preclean --no-multiarch --library="$LIB" \
  tools/zukomplink >/dev/null

echo "==> miniz must be linked in, not left for the loader"
# On Linux and Windows a dropped PKG_LIBS fails at link time. On macOS it
# does not: R links package shared objects with -undefined dynamic_lookup,
# so the link succeeds and the loader is left to find mz_* somewhere.
#
# Measured, rather than assumed, because zuxml's equivalent fixture fails
# differently: there the missing symbols were Expat's, the system Expat was
# already in the process, and the fixture built, loaded, parsed and passed
# every behavioural assertion against the wrong library. miniz is not a
# system library, so the same mistake here stops at dlopen with
# "symbol not found in flat namespace '_mz_version'" -- loud, just later
# than on the other platforms.
#
# The check stays anyway, and runs before anything is executed, for the
# regression it can still catch that nothing else would: if zukomp.so were
# ever widened to export mz_zip_* itself, this fixture would resolve against
# it at load time and silently stop testing the archive. tests/testthat/
# test-abi.R forbids that from inside zukomp; this is the same statement
# from outside, where the consequence actually lands.
so=$(find "$LIB/zukomplink/libs" -name 'zukomplink.*' | head -1)
if command -v nm >/dev/null 2>&1 && [ -n "$so" ]; then
  # Does nm say anything at all about this binary? Asked first, and
  # separately, because the two counts below are both zero in two entirely
  # different situations: the archive was not linked in, or nm cannot read
  # this object format. Windows is the second -- R installs a PE .dll whose
  # symbol table mingw nm does not report on -- and conflating them made the
  # *undefined* count pass vacuously for exactly the same reason the defined
  # count failed. A check that cannot fail is worse than no check, and a
  # check that fails when it simply cannot answer is what this build hit.
  total=$(nm "$so" 2>/dev/null | wc -l | tr -d ' ')
  if [ "$total" = "0" ]; then
    # Not a failure. On Windows a dropped PKG_LIBS fails at link time, so
    # the fixture could not have installed at all -- and installing is what
    # the step before this one just proved. The audit is here for macOS,
    # where the link succeeds and the loader is left holding the question;
    # there nm answers fine.
    echo "==> skipping the symbol audit: nm reports no symbols for $so"
    echo "    (the link itself is strict on this platform, so a dropped"
    echo "     PKG_LIBS would already have failed the install above)"
  else
    u=$(nm -u "$so" 2>/dev/null | grep -c 'mz_' || true)
    [ "$u" = "0" ] || {
      echo "FAIL: $u miniz symbols are undefined in $so." >&2
      echo "  libzukomp.a was not linked in; check PKG_LIBS in src/Makevars.in" >&2
      echo "  and what ./configure substituted into it." >&2
      exit 1
    }
    d=$(nm "$so" 2>/dev/null | grep -cE ' [TtDdSs] _*mz_' || true)
    [ "$d" != "0" ] || {
      echo "FAIL: nm reports $total symbols in $so but none of them are" >&2
      echo "  miniz's, so the archive was not linked in. Check PKG_LIBS in" >&2
      echo "  src/Makevars.in and what ./configure substituted into it." >&2
      exit 1
    }
    echo "==> $d miniz symbols are statically linked into the consumer"
  fi
fi

echo "==> running the archive consumer's tests"
# Via a file, not `Rscript -e`: on Windows only the first line of a
# multi-line -e argument reaches R, which then dies on the partial
# expression -- as a segmentation fault, not a diagnosable error.
cat > ./.check-linking/run-tests.R <<'RUN_TESTS_R'
results <- as.data.frame(testthat::test_local(
    "tools/zukomplink", reporter = "summary"))
# A zero-row result is a green job that proved nothing: any() over an empty
# vector is FALSE. If test_local() discovers no files -- a rename, a load
# failure -- that must fail, not pass quietly.
if (nrow(results) == 0L || sum(results$passed) == 0L) {
  stop("no archive-consumer tests ran")
}
quit(status = as.integer(any(results$failed > 0 | results$error)))
RUN_TESTS_R
R_LIBS="$LIB" Rscript ./.check-linking/run-tests.R

echo "==> the archive is built from the same miniz as the shared object"
# Cross-checked at run time rather than pinned a third time. The version is
# already pinned in tools/vendor/manifest.tsv and asserted in
# tests/testthat/test-abi.R, and tools/vendor/verify keeps those two in
# step; a literal here would be a third copy that verify cannot see. This
# compares what the *archive* reports against what the *shared object*
# reports, which is the thing neither pin covers -- src/Makevars compiles
# miniz.c twice, and nothing else would notice the two drifting apart.
cat > ./.check-linking/versions.R <<'VERSIONS_R'
from_archive <- zukomplink::miniz_version()
from_shlib <- zukomp::komp_info()$vendored
row <- from_shlib[from_shlib$source == "miniz", ]
if (nrow(row) != 1L) stop("komp_info() does not report exactly one miniz row")
if (!identical(as.character(row$version), from_archive)) {
  stop("miniz version disagrees: archive says ", from_archive,
       ", zukomp.so says ", row$version)
}
cat("==> both report miniz", from_archive, "\n")
VERSIONS_R
R_LIBS="$LIB" Rscript --vanilla ./.check-linking/versions.R

echo "==> the fixture works with zukomp absent from the library path"
# The strongest statement of "no run-time dependency": move the installed
# zukomp out of the way entirely and the consumer must still read archives,
# because every miniz symbol it needs is inside its own shared object.
#
# R_LIBS_USER='-' is not decoration. R_LIBS only *prepends* to .libPaths(),
# so a zukomp sitting in the developer's own user library -- which is where
# `devtools::install()` puts one, so it is the normal state of a machine
# that works on this package -- still resolves after "$LIB/zukomp" has been
# moved aside, and this step passes having proved nothing. It did exactly
# that the first time it was run. CI never sees a user library, which is
# precisely why the hole would have survived there.
mv "$LIB/zukomp" "$LIB/.zukomp-hidden"
cat > ./.check-linking/without.R <<'WITHOUT_R'
found <- system.file(package = "zukomp")
if (nzchar(found)) {
  stop("zukomp is still reachable at ", found,
       "; this step cannot prove anything while it is. .libPaths(): ",
       paste(.libPaths(), collapse = ", "))
}
library(zukomplink)
zip <- system.file("extdata", "probe.zip", package = "zukomplink")
members <- zip_members(zip)
stopifnot(identical(members$name,
                    c("hello.txt", "text.txt", "lcg.bin", "stored.txt",
                      "empty.txt")))
stopifnot(identical(zip_extract(zip, "hello.txt"),
                    charToRaw("zukomp reads ZIP containers\n")))
# Chunked too: the streaming path is the one zuxlsx needs, so it is the one
# that has to survive zukomp being gone.
stopifnot(identical(zip_extract(zip, "text.txt", chunk = 1L),
                    zip_extract(zip, "text.txt")))
cat("==> consumer reads archives with zukomp uninstalled\n")
WITHOUT_R
R_LIBS="$LIB" R_LIBS_USER='-' R_LIBS_SITE='' \
  Rscript --vanilla ./.check-linking/without.R
mv "$LIB/.zukomp-hidden" "$LIB/zukomp"

echo "==> archive consumer builds, links and reads a real ZIP"
