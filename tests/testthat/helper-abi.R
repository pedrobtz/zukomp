# Symbol-audit helper. Lives in a helper file rather than at the top of
# test-abi.R because testthat's parallel workers source helper-*.R but do not
# share a test file's file-scope definitions.

# Two views of the installed zukomp shared object, because since #34 they
# answer different questions and must not be confused.
#
#   exported_symbols()  what the object *exports*: R_init_zukomp and nothing
#                       else, with $(C_VISIBILITY) in src/Makevars.
#   compiled_symbols()  what the object *contains*, hidden symbols included.
#                       Every "no ZIP / PNG / zlib-ABI code in zukomp.so"
#                       audit reads this one.
#
# The split exists because the audits used to read the export list, and that
# was sound only while everything was exported. With miniz hidden, "no mz_zip
# symbol is exported" is true whether or not the ZIP reader is compiled in --
# an audit its own subject satisfies. compiled_symbols() therefore carries a
# positive control: it must see tinfl_decompress, which zukomp.so cannot work
# without, before any caller may conclude from the *absence* of a name.
#
# Both skip rather than fail wherever the toolchain cannot answer.

zukomp_dll_path <- function() {
  dll <- getLoadedDLLs()[["zukomp"]]
  skip_if(is.null(dll), "zukomp DLL is not loaded")
  path <- dll[["path"]]
  skip_if(!file.exists(path), "zukomp shared object not found on disk")
  path
}

run_nm <- function(args) {
  nm <- Sys.which("nm")
  skip_if(!nzchar(nm), "nm is not available on this platform")
  syms <- suppressWarnings(
    system2(nm, c(args, shQuote(zukomp_dll_path())),
            stdout = TRUE, stderr = FALSE)
  )
  skip_if(!is.character(syms) || length(syms) == 0L, "nm produced no output")
  syms
}

# Defined symbols in the dynamic symbol table. -D reads .dynsym on ELF, which
# is what the loader binds against; Mach-O has no separate table, and -gU is
# its "external and defined". Windows is skipped: R exports from a .def file
# there and $(C_VISIBILITY) is empty, so the export set is not this build's
# to control -- and R's Windows toolchain strips the DLL besides.
exported_symbols <- function() {
  skip_on_os("windows")
  if (Sys.info()[["sysname"]] == "Darwin") {
    run_nm("-gU")
  } else {
    run_nm(c("-D", "--defined-only"))
  }
}

# Every defined symbol, local ones included: -U / --defined-only without -g.
# Hidden symbols survive linking as locals in the ordinary symbol table, so
# they are listed unless the object was stripped -- and a stripped object is
# exactly what the positive control catches, as a skip ("cannot answer")
# rather than as a pass.
compiled_symbols <- function() {
  syms <- if (Sys.info()[["sysname"]] == "Darwin") {
    run_nm("-U")
  } else {
    run_nm("--defined-only")
  }
  skip_if(!any(grepl("\\b_?tinfl_decompress$", syms)),
          "symbol table does not list miniz's own functions (stripped?)")
  syms
}

# Is this build instrumented? It matters for exactly one assertion, the exact
# export set: a coverage or sanitizer runtime linked into the object exports
# symbols of its own. Decided from what the object exports rather than from
# how the job was configured -- the same rule zucrypt's helper uses.
is_instrumented_build <- function() {
  if (identical(Sys.getenv("R_COVR"), "true")) {
    return(TRUE)
  }
  syms <- tryCatch(exported_symbols(), condition = function(e) character())
  # Not only the __asan_ prefix: an ASan build also exports the linker's
  # section bounds __start_asan_globals and __stop_asan_globals, which carry
  # no such prefix. Matching `__asan_` alone let the R-hub clang-asan leg
  # run the exact-export test and fail on exactly those two names.
  any(grepl("gcov|__llvm_prof|asan|ubsan|tsan|msan|sancov", syms))
}

# Text of the installed public header. Reading the *installed* copy, not the
# source tree, is the point: it is what a LinkingTo consumer actually sees.
installed_header <- function(name = "zukomp.h") {
  path <- system.file("include", name, package = "zukomp")
  skip_if(!nzchar(path) || !file.exists(path), paste0(name, " is not installed"))
  readLines(path, warn = FALSE)
}

# The same header with C comments removed. The hygiene rules are about what
# the header *declares*: a comment that says "no miniz type appears here" is
# fine and worth keeping, a declaration that mentions one is not.
installed_header_code <- function(name = "zukomp.h") {
  text <- paste(installed_header(name), collapse = "\n")
  text <- gsub("/\\*.*?\\*/", " ", text)   # block comments, non-greedy
  text <- gsub("//[^\n]*", " ", text)      # line comments
  strsplit(text, "\n", fixed = TRUE)[[1L]]
}

# Is this a real installed layout, or devtools::load_all()? Decided once,
# here, from a file that has nothing to do with the artifacts under test --
# so "there is nothing to test here" can never be confused with "the thing
# under test is missing". That distinction is the whole point: everything
# below is installed by src/install.libs.R, and asking system.file() for each
# artifact and skipping on the empty answer made these tests unable to report
# the only failure they exist to detect.
#
# Same reason as exported_symbols() above for living in a helper: a parallel
# worker sources helper-*.R, not another test file's file scope, so a copy at
# the top of test-linking.R is found in some shuffled orders and not others.
skip_if_not_installed_layout <- function() {
  # Meta/package.rds is written by R's install step and by nothing else. It
  # must be something outside inst/: under load_all() system.file() resolves
  # against the source tree's inst/, so anything installed *from* inst/ --
  # a header, say -- is found there too and would report an installed layout
  # that has no libs/ or lib/ in it.
  skip_if(!nzchar(system.file("Meta", "package.rds", package = "zukomp")),
          "not an installed layout")
}

# Absolute path to something install.libs.R is responsible for, without
# asking system.file() whether it exists -- the caller asserts that, and a
# missing file must fail rather than skip.
installed_path <- function(...) {
  skip_if_not_installed_layout()
  file.path(system.file(package = "zukomp"), ...)
}

# The architecture-specific directory holding the static archive. R_ARCH is
# empty on single-arch Unix, so this is plain "lib" there, and "/x64" on
# Windows, so "lib/x64"; on a multi-arch install each architecture gets its
# own, because the archive is arch-specific object code and the two must not
# overwrite each other.
installed_lib_dir <- function() {
  arch <- .Platform$r_arch
  installed_path(if (nzchar(arch)) file.path("lib", arch) else "lib")
}

# Symbols of the installed static archive, the LinkingTo surface. nm over an
# archive interleaves a "member.o:" line before each member's symbols; those
# are not symbols, so keep only lines carrying a symbol type.
archive_symbols <- function() {
  nm <- Sys.which("nm")
  skip_if(!nzchar(nm), "nm is not available on this platform")

  # Not a skip: by the time we are here the layout exists, so an absent
  # archive is a failure of the install, which is what this audits.
  archive <- file.path(installed_lib_dir(), "libzukomp.a")
  expect_true(file.exists(archive))

  out <- suppressWarnings(
    system2(nm, c("-g", shQuote(archive)), stdout = TRUE, stderr = FALSE)
  )
  skip_if(!is.character(out) || length(out) == 0L, "nm produced no output")
  grep("^[0-9a-fA-F ]*\\s[A-Za-z]\\s", out, value = TRUE)
}

# Of those, the ones this archive defines rather than needs from elsewhere.
archive_defined <- function() {
  grep("\\sU\\s", archive_symbols(), value = TRUE, invert = TRUE)
}
