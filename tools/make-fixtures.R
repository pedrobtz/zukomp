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

# Clear the previous corpus -- but not fixtures/malformed/, which is a second
# corpus with its own generator (tools/make-malformed.R) and lives under the
# same directory. Unlinking the whole target used to delete it on every
# in-place regeneration.
if (check_only) {
  unlink(target, recursive = TRUE)
} else {
  old_entries <- list.files(target, full.names = TRUE, all.files = FALSE)
  unlink(old_entries[basename(old_entries) != "malformed"], recursive = TRUE)
}
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

# -- the zstd(1) and lz4(1) reference tools ----------------------------------
# Built from the same upstream releases zukomp vendors, which is the point:
# these are the encoders every other implementation is measured against, and
# their defaults (content checksum on, content size when the input is a file)
# are what a consumer will most often be handed. Both are required, not
# optional: without them the corpus would silently lose every zstd and lz4
# fixture and --check would report the regeneration as a deletion.
cli_blob <- function(cmd) {
  con <- pipe(cmd, "rb")
  on.exit(close(con))
  blob <- raw(0)
  repeat {
    chunk <- readBin(con, "raw", n = 1e6)
    if (length(chunk) == 0L) break
    blob <- c(blob, chunk)
  }
  blob
}
payload_path <- function(kind) shQuote(file.path(payload_dir, paste0(kind, ".bin")))

zstd_cli <- Sys.which("zstd")
if (!nzchar(zstd_cli)) stop("zstd(1) is required to regenerate fixtures")
cli_version <- function(tool, path) {
  line <- system2(path, "--version", stdout = TRUE, stderr = TRUE)[1]
  paste(tool, regmatches(line, regexpr("v[0-9][0-9.]*", line)))
}
zstd_ver <- cli_version("zstd", zstd_cli)
dir.create(file.path(target, "zstd"), showWarnings = FALSE)
zstd_specs <- list(
  # name             payload       arguments
  c("cli_l1_ascii",      "ascii",      "-1"),
  c("cli_l19_ascii",     "ascii",      "-19"),
  c("cli_l3_zeros",      "zeros",      "-3"),
  c("cli_l3_lcg",        "lcg",        "-3"),
  c("cli_l3_utf8",       "utf8",       "-3"),
  c("cli_l3_structured", "structured", "-3"),
  c("cli_l3_empty",      "empty",      "-3"),
  c("cli_nocheck_ascii", "ascii",      "-3 --no-check"),
  c("cli_neg5_ascii",    "ascii",      "--fast=5")
)
for (spec in zstd_specs) {
  name <- paste0(spec[1], ".bin")
  blob <- cli_blob(sprintf("%s -q -c %s %s", shQuote(zstd_cli), spec[3],
                           payload_path(spec[2])))
  writeBin(blob, file.path(target, "zstd", name))
  add_row("zstd", name, "zstd", zstd_ver, spec[2])
}
# Streamed from a pipe, so the frame carries no content size; and two frames
# back to back, which RFC 8878 permits and the tool decodes as one.
blob <- cli_blob(sprintf("cat %s | %s -q -c -3", payload_path("ascii"),
                         shQuote(zstd_cli)))
writeBin(blob, file.path(target, "zstd", "cli_stream_ascii.bin"))
add_row("zstd", "cli_stream_ascii.bin", "zstd", zstd_ver, "ascii")
one <- cli_blob(sprintf("%s -q -c -3 %s", shQuote(zstd_cli), payload_path("ascii")))
writeBin(c(one, one), file.path(target, "zstd", "cli_multiframe_ascii.bin"))
add_row("zstd", "cli_multiframe_ascii.bin", "zstd", zstd_ver, "ascii",
        members = 2L)

lz4_cli <- Sys.which("lz4")
if (!nzchar(lz4_cli)) stop("lz4(1) is required to regenerate fixtures")
lz4_ver <- cli_version("lz4", lz4_cli)
dir.create(file.path(target, "lz4-frame"), showWarnings = FALSE)
lz4_specs <- list(
  c("cli_l1_ascii",         "ascii",      "-1"),
  c("cli_l9_ascii",         "ascii",      "-9"),
  c("cli_l12_ascii",        "ascii",      "-12"),
  c("cli_l1_zeros",         "zeros",      "-1"),
  c("cli_l1_lcg",           "lcg",        "-1"),
  c("cli_l1_utf8",          "utf8",       "-1"),
  c("cli_l1_structured",    "structured", "-1"),
  c("cli_l1_empty",         "empty",      "-1"),
  c("cli_fast3_ascii",      "ascii",      "--fast=3"),
  # Every frame-descriptor flag the format has: content size, block
  # checksums, linked blocks, and no content checksum.
  c("cli_contentsize_ascii", "ascii",     "-1 --content-size"),
  c("cli_blockcrc_ascii",   "ascii",      "-1 -BX"),
  c("cli_linked_structured", "structured", "-9 -BD"),
  c("cli_nocrc_ascii",      "ascii",      "-1 --no-frame-crc"),
  c("cli_b4_lcg",           "lcg",        "-1 -B4")
)
for (spec in lz4_specs) {
  name <- paste0(spec[1], ".bin")
  blob <- cli_blob(sprintf("%s -q -c %s %s", shQuote(lz4_cli), spec[3],
                           payload_path(spec[2])))
  writeBin(blob, file.path(target, "lz4-frame", name))
  add_row("lz4-frame", name, "lz4", lz4_ver, spec[2])
}
one <- cli_blob(sprintf("%s -q -c -1 %s", shQuote(lz4_cli), payload_path("ascii")))
writeBin(c(one, one), file.path(target, "lz4-frame", "cli_multiframe_ascii.bin"))
add_row("lz4-frame", "cli_multiframe_ascii.bin", "lz4", lz4_ver, "ascii",
        members = 2L)

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
