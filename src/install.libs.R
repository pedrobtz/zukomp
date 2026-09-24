## Installs the shared object, and beside it the LinkingTo surface a consumer
## needs to read a ZIP container: libzukomp.a, miniz's header, and miniz's
## licence. See src/Makevars for why that archive is a second compilation of
## miniz.c.
##
## Defining this file makes R stop installing the shared object by itself, so
## the first block below is not optional boilerplate -- without it the package
## installs with no compiled code at all. (Writing R Extensions 1.2.1.1.)

install_or_stop <- function(from, to, what) {
  dir.create(to, recursive = TRUE, showWarnings = FALSE)
  ## file.copy() returns a logical per source and never signals, so an
  ## unchecked call is how a package installs with a piece silently missing.
  ok <- file.copy(from, to, overwrite = TRUE)
  if (length(ok) == 0L || !all(ok)) {
    stop("zukomp: failed to install ", what, " into ", to)
  }
  invisible(TRUE)
}

libs <- file.path(R_PACKAGE_DIR, paste0("libs", R_ARCH))
## Checked like everything else here: Sys.glob() returning nothing is exactly
## the "no compiled code at all" failure the comment above describes, and an
## unchecked copy of zero files succeeds quietly.
install_or_stop(Sys.glob(paste0("*", SHLIB_EXT)), libs, "the shared object")
if (file.exists("symbols.rds")) {
  install_or_stop("symbols.rds", libs, "symbols.rds")
}

## The archive is arch-specific object code, so it installs under R_ARCH the
## way the shared object does. R_ARCH is empty on single-arch Unix, so this
## is plain <pkg>/lib there. It is NOT empty on Windows: R 4.2 dropped 32-bit
## Windows but kept the sub-architecture layout, so R_ARCH is "/x64" and the
## archive lands in <pkg>/lib/x64 -- which is what broke zuxlsx's configure
## on 2026-09-19 when it looked only in <pkg>/lib (#35). On a genuine
## multi-arch install it is also what stops the second architecture's archive
## from overwriting the first's, leaving consumers of one of them linking
## object code for the other.
##
## Consumers resolve it with system.file("lib", .Platform$r_arch, package =
## "zukomp"), which is correct on both -- r_arch is "" on a single-arch
## platform. See "Using zukomp from C" in README.md.
lib <- file.path(R_PACKAGE_DIR, paste0("lib", R_ARCH))
install_or_stop("libzukomp.a", lib, "libzukomp.a (src/Makevars should have built it)")

## miniz.h is copied from the vendored tree rather than kept as a second copy
## under inst/include/, so it cannot drift from the source the archive was
## compiled from. It lands beside zukomp.h and zukomp-r.h, which R's own
## "inst" step copies here afterwards -- that step merges into this directory
## rather than replacing it, which tests/testthat/test-linking.R checks from
## the installed package.
include <- file.path(R_PACKAGE_DIR, "include")
install_or_stop(file.path("vendor", "miniz", "miniz.h"), include, "miniz.h")

## And miniz's licence, for the same reason and from the same tree. This is a
## licensing obligation rather than tidiness: miniz.h carries no copyright
## line and no permission notice of its own -- both live in miniz.c and in the
## upstream LICENSE -- while an installed zukomp ships that header and a
## compiled copy of miniz inside libzukomp.a. Nothing outside inst/ is
## installed, so without this the MIT notice would reach the source tarball
## and stop there.
licenses <- file.path(R_PACKAGE_DIR, "licenses")
dir.create(licenses, recursive = TRUE, showWarnings = FALSE)
if (!file.copy(file.path("vendor", "miniz", "LICENSE"),
               file.path(licenses, "miniz-LICENSE"), overwrite = TRUE)) {
  stop("zukomp: failed to install miniz's LICENSE from src/vendor/miniz/")
}
