## Installs the shared object, and beside it the LinkingTo surface a consumer
## needs to read a ZIP container: libzukomp.a and miniz's header. See
## src/Makevars for why that archive is a second compilation of miniz.c.
##
## Defining this file makes R stop installing the shared object by itself, so
## the first block below is not optional boilerplate -- without it the package
## installs with no compiled code at all. (Writing R Extensions 1.2.1.1.)

libs <- file.path(R_PACKAGE_DIR, paste0("libs", R_ARCH))
dir.create(libs, recursive = TRUE, showWarnings = FALSE)
file.copy(Sys.glob(paste0("*", SHLIB_EXT)), libs, overwrite = TRUE)
if (file.exists("symbols.rds")) {
  file.copy("symbols.rds", libs, overwrite = TRUE)
}

## The archive is arch-specific but installs to a single arch-neutral path,
## which is what every current platform needs. It would have to move under
## R_ARCH before zukomp could support a multi-arch installation again.
lib <- file.path(R_PACKAGE_DIR, "lib")
dir.create(lib, recursive = TRUE, showWarnings = FALSE)
if (!file.copy("libzukomp.a", lib, overwrite = TRUE)) {
  stop("zukomp: failed to install libzukomp.a; src/Makevars should have built it")
}

## miniz.h is copied from the vendored tree rather than kept as a second copy
## under inst/include/, so it cannot drift from the source the archive was
## compiled from. It lands beside zukomp.h and zukomp-r.h, which R's own
## "inst" step copies here afterwards -- that step merges into this directory
## rather than replacing it, which tests/testthat/test-linking.R checks from
## the installed package.
include <- file.path(R_PACKAGE_DIR, "include")
dir.create(include, recursive = TRUE, showWarnings = FALSE)
if (!file.copy(file.path("vendor", "miniz", "miniz.h"), include, overwrite = TRUE)) {
  stop("zukomp: failed to install miniz.h from src/vendor/miniz/")
}
