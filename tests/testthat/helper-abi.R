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
