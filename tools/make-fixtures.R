#!/usr/bin/env Rscript
# Generate the committed interop corpus.
#
#   Rscript tools/make-fixtures.R           regenerate in place
#   Rscript tools/make-fixtures.R --check   regenerate to a temp dir and diff
#
# Maintainer script, run offline, .Rbuildignore'd. It shells out to python3,
# the system gzip and R's own memCompress; tests never do, because CRAN
# guarantees none of them. That is the entire point of committing the corpus:
# interop is proved against bytes real encoders produced, without needing
# those encoders present at check time.
#
# Payloads come from tests/testthat/helper-corpus.R so the generator and the
# tests cannot drift: a test decompresses a fixture and compares against
# new_payload(kind, n), which only works if both call the same constructor.

args <- commandArgs(trailingOnly = TRUE)
check_only <- "--check" %in% args

root <- normalizePath(file.path(dirname(sub("^--file=", "", grep("^--file=",
  commandArgs(FALSE), value = TRUE)[1])), ".."), mustWork = TRUE)
source(file.path(root, "tests", "testthat", "helper-corpus.R"))

fixtures_dir <- file.path(root, "tests", "testthat", "fixtures")
target <- if (check_only) file.path(tempdir(), "fixtures-check") else fixtures_dir

# The payload kinds and sizes the corpus covers. Kept small: the suite must
# stay under 60 seconds and the tarball small. "random" is deliberately
# absent -- it depends on R's RNG stream, so "lcg" is the reproducible
# stand-in for incompressible input.
payload_specs <- list(
  ascii      = 4096L,
  zeros      = 8192L,
  utf8       = 4096L,
  lcg        = 4096L,
  structured = 4096L,
  empty      = 0L
)

unlink(target, recursive = TRUE)
dir.create(target, recursive = TRUE, showWarnings = FALSE)

# Payload files, for the generators that take a path.
payload_dir <- file.path(tempdir(), "zukomp-payloads")
unlink(payload_dir, recursive = TRUE)
dir.create(payload_dir, recursive = TRUE, showWarnings = FALSE)
for (kind in names(payload_specs)) {
  writeBin(new_payload(kind, payload_specs[[kind]]),
           file.path(payload_dir, paste0(kind, ".bin")))
}

rows <- list()
add_row <- function(codec, file, generator, version, payload, members = 1L) {
  rows[[length(rows) + 1L]] <<- data.frame(
    codec = codec, file = file, generator = generator,
    generator_version = version, payload = payload,
    n = payload_specs[[payload]], members = as.integer(members),
    stringsAsFactors = FALSE
  )
}

# -- python: everything needing byte-level header control --------------------
py <- Sys.which("python3")
if (!nzchar(py)) stop("python3 is required to regenerate fixtures")
out <- system2(py, c(shQuote(file.path(root, "tools", "make-fixtures.py")),
                     shQuote(target), shQuote(payload_dir)), stdout = TRUE)
for (line in out) {
  f <- strsplit(line, "\t", fixed = TRUE)[[1]]
  add_row(f[1], f[2], f[3], f[4], f[5], as.integer(f[6]))
}

# -- the real gzip(1), as a cross-check against a second implementation ------
gz <- Sys.which("gzip")
if (nzchar(gz)) {
  # Apple's gzip prints its version to stderr, GNU's to stdout; take both.
  gzver <- sub("^\\s+", "",
               system2(gz, "--version", stdout = TRUE, stderr = TRUE)[1])
  dir.create(file.path(target, "gzip"), showWarnings = FALSE)
  for (spec in list(c("ascii", "1"), c("ascii", "9"),
                    c("zeros", "9"), c("lcg", "9"))) {
    kind <- spec[1]; lvl <- spec[2]
    name <- sprintf("cli_l%s_%s.bin", lvl, kind)
    # -n suppresses the name and timestamp, without which the output is not
    # byte-reproducible and --check could never pass.
    # Read as raw through a pipe; system2(stdout=TRUE) would mangle binary.
    con <- pipe(sprintf("%s -%s -n -c %s", shQuote(gz), lvl,
                        shQuote(file.path(payload_dir, paste0(kind, ".bin")))),
                "rb")
    blob <- readBin(con, "raw", n = 10e6)
    close(con)
    writeBin(blob, file.path(target, "gzip", name))
    add_row("gzip", name, "gzip", gzver, kind)
  }
} else {
  message("note: gzip(1) not found; skipping its fixtures")
}

# -- R's memCompress ---------------------------------------------------------
# Worth recording explicitly: memCompress(type = "gzip") emits *zlib* format
# (78 9c), not gzip. It is therefore a zlib fixture, and zuhttp users who
# reach for it expecting gzip are in for a surprise.
dir.create(file.path(target, "zlib"), showWarnings = FALSE)
rver <- paste0("R", getRversion())
for (kind in c("ascii", "zeros")) {
  blob <- memCompress(new_payload(kind, payload_specs[[kind]]), "gzip")
  name <- sprintf("r_memcompress_%s.bin", kind)
  writeBin(blob, file.path(target, "zlib", name))
  add_row("zlib", name, "R memCompress(type=\"gzip\")", rver, kind)
}

manifest <- do.call(rbind, rows)
manifest$bytes <- vapply(
  seq_len(nrow(manifest)),
  function(i) file.size(file.path(target, manifest$codec[i], manifest$file[i])),
  numeric(1)
)
manifest$md5 <- vapply(
  seq_len(nrow(manifest)),
  function(i) unname(tools::md5sum(
    file.path(target, manifest$codec[i], manifest$file[i]))),
  character(1)
)
manifest <- manifest[order(manifest$codec, manifest$file), ]

write.table(manifest, file.path(target, "MANIFEST.tsv"),
            sep = "\t", quote = FALSE, row.names = FALSE)

if (!check_only) {
  cat(sprintf("wrote %d fixtures to %s\n", nrow(manifest), target))
  quit(status = 0)
}

# -- --check: compare against what is committed ------------------------------
committed <- file.path(fixtures_dir, "MANIFEST.tsv")
if (!file.exists(committed)) stop("no committed MANIFEST.tsv to compare against")
old <- read.delim(committed, stringsAsFactors = FALSE)

key <- function(d) paste(d$codec, d$file, sep = "/")
missing <- setdiff(key(old), key(manifest))
added <- setdiff(key(manifest), key(old))
status <- 0L
if (length(missing)) { cat("MISSING:\n"); cat(paste0("  ", missing, "\n")); status <- 1L }
if (length(added))   { cat("ADDED:\n");   cat(paste0("  ", added, "\n"));   status <- 1L }

both <- intersect(key(old), key(manifest))
o <- old[match(both, key(old)), ]
n <- manifest[match(both, key(manifest)), ]
differs <- o$md5 != n$md5
if (any(differs)) {
  for (i in which(differs)) {
    same_gen <- identical(o$generator_version[i], n$generator_version[i])
    cat(sprintf("%s %s  (committed %s, local %s)\n",
                if (same_gen) "MISMATCH" else "differs, generator changed:",
                both[i], o$generator_version[i], n$generator_version[i]))
    # A different generator version producing different bytes is expected and
    # not a failure; the same version doing so means something really changed.
    if (same_gen) status <- 1L
  }
}
if (status == 0L) cat(sprintf("ok  %d fixtures match\n", length(both)))
quit(status = status)
