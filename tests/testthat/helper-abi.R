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
