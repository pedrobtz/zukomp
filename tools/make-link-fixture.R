#!/usr/bin/env Rscript
# Builds tools/zukomplink/inst/extdata/probe.zip, the archive that fixture's
# tests read. Maintainer-only and offline, like tools/make-fixtures.R.
#
# Committed rather than built during the test run, for one reason: the
# fixture must work with zukomp absent from the library path entirely --
# that is the sharpest statement of "links, does not load", and
# tools/check-linking.sh makes it by moving the installed zukomp away. A
# fixture that called komp_compress() at test time could not survive that.
#
# So zukomp builds the DEFLATE streams here, once, and miniz's ZIP reader
# decompresses them there, later, inside someone else's shared object. The
# round trip spans both halves of the package.
#
#   Rscript tools/make-link-fixture.R            # write the fixture
#   Rscript tools/make-link-fixture.R --check    # verify it is reproducible
#
# No external zip program: CRAN guarantees none. A DEFLATE stream is exactly
# what ZIP method 8 stores, and the CRC-32 comes out of a gzip trailer,
# which is where zukomp already computes one.

suppressPackageStartupMessages(library(zukomp))

out <- file.path("tools", "zukomplink", "inst", "extdata", "probe.zip")
check <- "--check" %in% commandArgs(trailingOnly = TRUE)

# --- payloads -------------------------------------------------------------
# Byte-explicit and trivially reconstructible, because the fixture's own
# tests rebuild them to compare against and cannot reach zukomp's
# helper-corpus.R from another package. Keep these two constructors
# character-for-character in step with the copies in
# tools/zukomplink/tests/testthat/helper-payload.R.

payload_text <- function() {
  charToRaw(paste(rep("zukomp reads ZIP containers\n", 180L), collapse = ""))
}

# Lehmer generator, chosen so every intermediate stays inside a double: the
# point is an incompressible payload that any language can reproduce exactly,
# not a good RNG. DEFLATE falls back to stored blocks on it.
payload_lcg <- function(n) {
  x <- 1L
  out <- raw(n)
  for (i in seq_len(n)) {
    x <- (75 * x + 74) %% 65537
    out[i] <- as.raw(x %% 256)
  }
  out
}

members <- list(
  list(name = "hello.txt",  data = charToRaw("zukomp reads ZIP containers\n"),
       method = 8L),
  list(name = "text.txt",   data = payload_text(),      method = 8L),
  list(name = "lcg.bin",    data = payload_lcg(3000L),  method = 8L),
  list(name = "stored.txt", data = charToRaw("this member is not compressed at all, method 0\n"),
       method = 0L),
  list(name = "empty.txt",  data = raw(0),              method = 0L)
)

# --- ZIP writing ----------------------------------------------------------

u16 <- function(v) as.raw(c(v %% 256L, v %/% 256L %% 256L))
u32 <- function(v) as.raw(c(v %% 256L, v %/% 256L %% 256L,
                            v %/% 65536L %% 256L, v %/% 16777216L %% 256L))

# CRC-32 via a gzip trailer, which is little-endian CRC in the last 8 bytes.
crc32_of <- function(x) {
  gz <- komp_compress(x, "gzip")
  gz[seq(length(gz) - 7L, length(gz) - 4L)]
}

# Fixed DOS timestamp: 2026-09-21 12:34:56. Not zero, deliberately -- the
# fixture asserts that miniz reports a real mtime, which is what proves this
# archive was compiled with -UMINIZ_NO_TIME. With the trim in place
# mz_zip_archive_file_stat ends in m_padding instead, the struct is the same
# size, and nothing else would notice.
DOS_TIME <- bitwOr(bitwOr(bitwShiftL(12L, 11L), bitwShiftL(34L, 5L)), 28L)
DOS_DATE <- bitwOr(bitwOr(bitwShiftL(2026L - 1980L, 9L), bitwShiftL(9L, 5L)), 21L)

local <- raw(0)
central <- raw(0)
offsets <- integer(length(members))

for (i in seq_along(members)) {
  m <- members[[i]]
  stored <- if (m$method == 8L) komp_compress(m$data, "deflate-raw") else m$data
  crc <- if (length(m$data) == 0L) u32(0L) else crc32_of(m$data)
  name <- charToRaw(m$name)
  offsets[i] <- length(local)

  local <- c(local,
             as.raw(c(0x50, 0x4b, 0x03, 0x04)), u16(20L), u16(0L),
             u16(m$method), u16(DOS_TIME), u16(DOS_DATE), crc,
             u32(length(stored)), u32(length(m$data)),
             u16(length(name)), u16(0L), name, stored)

  central <- c(central,
               as.raw(c(0x50, 0x4b, 0x01, 0x02)), u16(20L), u16(20L), u16(0L),
               u16(m$method), u16(DOS_TIME), u16(DOS_DATE), crc,
               u32(length(stored)), u32(length(m$data)),
               u16(length(name)), u16(0L), u16(0L), u16(0L), u16(0L),
               u32(0L), u32(offsets[i]), name)
}

n <- length(members)
eocd <- c(as.raw(c(0x50, 0x4b, 0x05, 0x06)), u16(0L), u16(0L), u16(n), u16(n),
          u32(length(central)), u32(length(local)), u16(0L))

bytes <- c(local, central, eocd)

if (check) {
  if (!file.exists(out)) stop("no fixture to check: ", out)
  have <- readBin(out, "raw", file.size(out))
  if (!identical(have, bytes)) {
    stop("probe.zip is not reproducible from this generator and this zukomp")
  }
  cat("probe.zip: reproducible,", length(bytes), "bytes,", n, "members\n")
} else {
  writeBin(bytes, out)
  cat("wrote", out, "--", length(bytes), "bytes,", n, "members\n")
}
