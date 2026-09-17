# Symbol-audit helper. Lives in a helper file rather than at the top of
# test-abi.R because testthat's parallel workers source helper-*.R but do not
# share a test file's file-scope definitions.

# Exported symbols of the installed zukomp shared object, as nm reports them.
# Skips rather than fails wherever the toolchain cannot answer.
exported_symbols <- function() {
  nm <- Sys.which("nm")
  skip_if(!nzchar(nm), "nm is not available on this platform")

  dll <- getLoadedDLLs()[["zukomp"]]
  skip_if(is.null(dll), "zukomp DLL is not loaded")
  path <- dll[["path"]]
  skip_if(!file.exists(path), "zukomp shared object not found on disk")

  syms <- suppressWarnings(
    system2(nm, c("-g", shQuote(path)), stdout = TRUE, stderr = FALSE)
  )
  skip_if(!is.character(syms) || length(syms) == 0L, "nm produced no output")
  syms
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

# Path inside the *installed* package, or a skip when there is no installed
# layout -- under devtools::load_all() there is none, and these tests are
# about what a consumer sees. Same reason as exported_symbols() above for
# living in a helper: a parallel worker sources helper-*.R, not another test
# file's file scope, so a copy at the top of test-linking.R is found in some
# shuffled orders and not others.
installed_path <- function(...) {
  path <- system.file(..., package = "zukomp")
  skip_if(!nzchar(path), paste0("not an installed layout: ", file.path(...)))
  path
}

# Symbols of the installed static archive, the LinkingTo surface. nm over an
# archive interleaves a "member.o:" line before each member's symbols; those
# are not symbols, so keep only lines carrying a symbol type.
archive_symbols <- function() {
  nm <- Sys.which("nm")
  skip_if(!nzchar(nm), "nm is not available on this platform")

  archive <- system.file("lib", "libzukomp.a", package = "zukomp")
  skip_if(!nzchar(archive), "not an installed layout: lib/libzukomp.a")

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

