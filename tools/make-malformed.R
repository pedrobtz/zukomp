#!/usr/bin/env Rscript
# Generate the committed malformed-DEFLATE corpus.
#
#   Rscript tools/make-malformed.R           regenerate in place
#   Rscript tools/make-malformed.R --check   regenerate to a temp dir and diff
#
# Maintainer script, run offline, .Rbuildignore'd.
#
# Why this corpus exists, separately from tests/testthat/fixtures/{gzip,zlib,
# deflate-raw}: that corpus is round-trip shaped -- "these bytes from a real
# encoder decode to new_payload(kind, n)" -- and every row is valid. This one
# is conformance shaped: hand-built bitstreams, mostly *invalid*, each with an
# explicit expected outcome. The two schemas have almost no columns in common,
# so they get two manifests rather than one table that is half NA per row.
#
# Each vector is built field by field from RFC 1951 rather than copied from
# another project's test suite, so the reason a vector is interesting is in
# this file rather than in a hex blob nobody can read. Every malformation is
# paired with a *control*: the same construction with the bad field corrected,
# which must decode. That pairing is what proves a vector reaches the code
# path its name claims instead of failing earlier for an unrelated reason.

args <- commandArgs(trailingOnly = TRUE)
check_only <- "--check" %in% args

root <- normalizePath(file.path(dirname(sub("^--file=", "", grep("^--file=",
  commandArgs(FALSE), value = TRUE)[1])), ".."), mustWork = TRUE)

corpus_dir <- file.path(root, "tests", "testthat", "fixtures", "malformed")
target <- if (check_only) file.path(tempdir(), "malformed-check") else corpus_dir

# -- bit writer --------------------------------------------------------------
# RFC 1951 3.1.1: data elements other than Huffman codes are packed starting
# with the least-significant bit; Huffman codes are packed starting with the
# most-significant bit of the code. Two methods, because conflating them is
# the single easiest way to write a vector that tests something else.

bw_new   <- function() list(bits = integer(0))
bw_bits  <- function(w, value, n) {
  for (i in seq_len(n)) w$bits <- c(w$bits, bitwAnd(bitwShiftR(value, i - 1L), 1L))
  w
}
bw_code  <- function(w, code, n) {
  for (i in rev(seq_len(n))) w$bits <- c(w$bits, bitwAnd(bitwShiftR(code, i - 1L), 1L))
  w
}
bw_align <- function(w) {
  r <- length(w$bits) %% 8L
  if (r) w$bits <- c(w$bits, integer(8L - r))
  w
}
bw_byte  <- function(w, b) bw_bits(w, b, 8L)
bw_raw   <- function(w) {
  b <- bw_align(w)$bits
  if (!length(b)) return(raw(0))
  as.raw(colSums(matrix(b, nrow = 8L) * 2^(0:7)))
}

# RFC 1951 3.2.6, the fixed Huffman code for the literal/length alphabet.
fixed_sym <- function(w, sym) {
  if (sym <= 143L)      bw_code(w, 0x30L + sym, 8L)
  else if (sym <= 255L) bw_code(w, 0x190L + (sym - 144L), 9L)
  else if (sym <= 279L) bw_code(w, sym - 256L, 7L)
  else                  bw_code(w, 0xC0L + (sym - 280L), 8L)
}
fixed_lit <- function(w, ch) fixed_sym(w, as.integer(charToRaw(ch)))

# RFC 1951 3.2.2, canonical Huffman codes from a length-per-symbol vector.
canon <- function(lens) {
  maxlen <- max(lens)
  bl <- integer(maxlen + 1L)
  for (l in lens[lens > 0L]) bl[l + 1L] <- bl[l + 1L] + 1L
  nx <- integer(maxlen + 1L)
  code <- 0L
  for (b in seq_len(maxlen)) {
    code <- bitwShiftL(code + bl[b], 1L)
    nx[b + 1L] <- code
  }
  out <- integer(length(lens))
  for (s in seq_along(lens)) {
    l <- lens[s]
    if (l > 0L) { out[s] <- nx[l + 1L]; nx[l + 1L] <- nx[l + 1L] + 1L }
  }
  out
}

# RFC 1951 3.2.7's permuted order for the code-length alphabet.
CLEN_ORDER <- c(16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15)

# A dynamic-block header. `clen_lens` gives code lengths for the code-length
# alphabet (symbols 0..18); `syms` is a list of c(symbol, extra, extra_bits)
# describing the literal/length and distance code lengths.
bw_dynamic <- function(w, hlit, hdist, clen_lens, syms, hclen = 19L) {
  w <- bw_bits(w, hlit - 257L, 5L)
  w <- bw_bits(w, hdist - 1L, 5L)
  w <- bw_bits(w, hclen - 4L, 4L)
  for (i in seq_len(hclen)) w <- bw_bits(w, clen_lens[CLEN_ORDER[i] + 1L], 3L)
  codes <- canon(clen_lens)
  for (t in syms) {
    s <- t[1]
    w <- bw_code(w, codes[s + 1L], clen_lens[s + 1L])
    if (length(t) >= 3L && t[3] > 0L) w <- bw_bits(w, t[2], t[3])
  }
  w
}

raw_block   <- function() bw_bits(bw_bits(bw_new(), 1L, 1L), 0L, 2L)  # BFINAL=1 BTYPE=00
fixed_block <- function() bw_bits(bw_bits(bw_new(), 1L, 1L), 1L, 2L)  # BFINAL=1 BTYPE=01
dyn_block   <- function() bw_bits(bw_bits(bw_new(), 1L, 1L), 2L, 2L)  # BFINAL=1 BTYPE=10

# -- the vectors -------------------------------------------------------------
# `rfc` is what RFC 1951 requires of a conforming decoder: "ok" or "reject".
# It is a property of the spec, declared here, never observed. What this
# build actually does is measured below and recorded separately, so a row
# where the two disagree is visible as a deviation rather than silently
# becoming the expectation.

V <- list()
vector_ <- function(case, bytes, rfc, section, note, output = NULL) {
  V[[case]] <<- list(case = case, bytes = bytes, rfc = rfc,
                     rfc_section = section, note = note, output = output)
}

## stored blocks -- RFC 1951 3.2.4
w <- bw_align(raw_block())
w <- bw_byte(bw_byte(w, 0x04L), 0x00L)                 # LEN  = 4
w <- bw_byte(bw_byte(w, 0xFBL), 0xFFL)                 # NLEN = one's complement
for (ch in c("t", "e", "s", "t")) w <- bw_byte(w, as.integer(charToRaw(ch)))
vector_("raw-stored-ok", bw_raw(w), "ok", "3.2.4",
        "control: a stored block with a correct NLEN", charToRaw("test"))

w <- bw_align(raw_block())
w <- bw_byte(bw_byte(w, 0x00L), 0x00L)                 # LEN  = 0
w <- bw_byte(bw_byte(w, 0xFFL), 0xFFL)                 # NLEN = 0xFFFF
vector_("raw-stored-empty-ok", bw_raw(w), "ok", "3.2.4",
        "control: an empty stored block, the shortest valid DEFLATE stream",
        raw(0))

w <- bw_align(raw_block())
w <- bw_byte(bw_byte(w, 0x04L), 0x00L)
w <- bw_byte(bw_byte(w, 0x00L), 0x00L)                 # NLEN = 0, must be 0xFFFB
for (ch in c("t", "e", "s", "t")) w <- bw_byte(w, as.integer(charToRaw(ch)))
vector_("raw-stored-nlen-mismatch", bw_raw(w), "reject", "3.2.4",
        "NLEN is not the one's complement of LEN")

w <- bw_align(raw_block())
w <- bw_byte(bw_byte(w, 0x04L), 0x00L)
w <- bw_byte(bw_byte(w, 0xFBL), 0xFFL)
for (ch in c("t", "e")) w <- bw_byte(w, as.integer(charToRaw(ch)))
vector_("raw-stored-truncated", bw_raw(w), "reject", "3.2.4",
        "stored block whose payload stops two bytes short of LEN")

## block type -- RFC 1951 3.2.3
vector_("raw-btype-reserved", bw_raw(bw_bits(bw_bits(bw_new(), 1L, 1L), 3L, 2L)),
        "reject", "3.2.3", "BTYPE = 11 is reserved")

## fixed Huffman -- RFC 1951 3.2.6
w <- fixed_lit(fixed_block(), "A")
w <- fixed_sym(fixed_lit(w, "B"), 256L)
vector_("raw-fixed-literals-ok", bw_raw(w), "ok", "3.2.6",
        "control: two fixed-Huffman literals and end-of-block",
        charToRaw("AB"))

w <- fixed_lit(fixed_lit(fixed_block(), "A"), "B")
w <- fixed_sym(w, 257L)                                # length 3, no extra bits
w <- bw_code(w, 1L, 5L)                                # distance code 1 -> 2
w <- fixed_sym(w, 256L)
vector_("raw-fixed-match-ok", bw_raw(w), "ok", "3.2.6",
        "control: a legal overlapping match, distance 2 over 2 bytes of output",
        charToRaw("ABABA"))

## dynamic Huffman -- RFC 1951 3.2.7
# Literal/length lengths: 'A' and 'B' at 2 bits, end-of-block at 1 bit, which
# is a complete table. The distance table has a single one-bit code, which is
# incomplete but legal as long as no distance code is used.
cl_ok <- integer(19); cl_ok[c(18, 2, 1) + 1L] <- c(1L, 2L, 2L)
lit_lens <- list(c(18, 54, 7),          # symbols 0..64 absent
                 c(2), c(2),            # 'A', 'B' at 2 bits
                 c(18, 127, 7), c(18, 40, 7),  # symbols 67..255 absent
                 c(1),                  # symbol 256 at 1 bit
                 c(1))                  # the single distance code
w <- bw_dynamic(dyn_block(), 257L, 1L, cl_ok, lit_lens)
codes <- canon(c(rep(0L, 65L), 2L, 2L, rep(0L, 189L), 1L))
w <- bw_code(w, codes[66L], 2L)                        # 'A'
w <- bw_code(w, codes[67L], 2L)                        # 'B'
w <- bw_code(w, codes[257L], 1L)                       # end of block
vector_("raw-dynamic-ok", bw_raw(w), "ok", "3.2.7",
        "control: a hand-built dynamic block, proving the header builder",
        charToRaw("AB"))

cl <- integer(19); cl[c(16, 0) + 1L] <- 1L
vector_("raw-clen-repeat-at-start",
        bw_raw(bw_dynamic(dyn_block(), 257L, 1L, cl, list(c(16, 0, 2)), hclen = 4L)),
        "reject", "3.2.7",
        "code-length symbol 16 (copy previous) as the first symbol, with no previous")

cl <- integer(19); cl[c(16, 17, 18, 0) + 1L] <- 1L
vector_("raw-clen-oversubscribed",
        bw_raw(bw_dynamic(dyn_block(), 257L, 1L, cl, list(c(0)), hclen = 4L)),
        "reject", "3.2.7",
        "four one-bit codes in the code-length alphabet: over-subscribed")

cl <- integer(19); cl[c(1, 18) + 1L] <- 1L
vector_("raw-hlit-too-large",
        bw_raw(bw_dynamic(dyn_block(), 288L, 1L, cl,
                          list(c(1), c(18, 127, 7), c(18, 127, 7), c(18, 25, 7)))),
        "reject", "3.2.7", "HLIT encodes 288 literal/length codes; at most 286 exist")

vector_("raw-hdist-too-large",
        bw_raw(bw_dynamic(dyn_block(), 257L, 32L, cl,
                          list(c(1), c(18, 127, 7), c(18, 120, 7), c(1)))),
        "reject", "3.2.7", "HDIST encodes 32 distance codes; at most 30 exist")

cl <- integer(19); cl[c(0, 1) + 1L] <- 1L
vector_("raw-missing-end-of-block",
        bw_raw(bw_dynamic(dyn_block(), 257L, 1L, cl,
                          c(list(c(1), c(1)), rep(list(c(0)), 255L), list(c(1))),
                          hclen = 4L)),
        "reject", "3.2.7",
        paste("a complete literal/length table with no code for symbol 256;",
              "unbounded output at small input chunks, because miniz pads",
              "exhausted input with zero bits that decode to a valid literal"))

## match distances -- RFC 1951 3.2.5
w <- fixed_lit(fixed_block(), "A")
w <- fixed_sym(w, 257L)                                # length 3
w <- bw_code(w, 1L, 5L)                                # distance 2, one byte out
w <- fixed_sym(w, 256L)
vector_("raw-distance-too-far", bw_raw(w), "reject", "3.2.5",
        "a match reaching one byte before the start of the output")

w <- fixed_lit(fixed_block(), "A")
w <- fixed_sym(w, 264L)                                # length 10
w <- bw_code(w, 29L, 5L); w <- bw_bits(w, 0L, 13L)     # distance 24577
w <- fixed_sym(w, 256L)
vector_("raw-distance-far-window", bw_raw(w), "reject", "3.2.5",
        "a match reaching 24577 bytes before the start of the output")

w <- fixed_lit(fixed_block(), "A")
w <- fixed_sym(w, 265L); w <- bw_bits(w, 1L, 1L)       # length 12
w <- bw_code(w, 30L, 5L)                               # distance code 30
w <- fixed_sym(w, 256L)
vector_("raw-distance-code-reserved", bw_raw(w), "reject", "3.2.5",
        "distance codes 30 and 31 are reserved and have no defined distance")

# -- write, then measure what this build does with each ----------------------
unlink(target, recursive = TRUE)
dir.create(target, recursive = TRUE, showWarnings = FALSE)
for (v in V) writeBin(v$bytes, file.path(target, paste0(v$case, ".bin")))

suppressMessages(devtools::load_all(root, quiet = TRUE))
hexs <- function(r) if (!length(r)) "" else paste(format(r), collapse = "")

rows <- list()
for (v in V) {
  got <- tryCatch(zukomp::komp_decompress(v$bytes, "deflate-raw"),
                  zukomp_error = function(e) structure(class(e)[1], class = "zu_err"))
  errored <- inherits(got, "zu_err")

  expect <- if (!errored && v$rfc == "reject") "deviation"
            else if (errored) "error" else "ok"
  class_ <- if (errored) unclass(got)[1] else ""
  # The byte *values* a deviation returns come from uninitialised heap and
  # must not be pinned; the byte *count* is fixed by the stream's length
  # codes, so it is the one thing about a deviation worth asserting.
  out_n   <- if (errored) "" else as.character(length(got))
  out_hex <- if (expect == "ok") hexs(got) else ""

  # A control that does not decode, or decodes to the wrong bytes, means the
  # builder is wrong -- refuse to write a corpus built on a broken builder.
  if (v$rfc == "ok") {
    if (errored) {
      stop(sprintf("control vector %s failed to decode: %s", v$case, class_))
    }
    if (!identical(got, v$output)) {
      stop(sprintf("control vector %s decoded to %s, expected %s",
                   v$case, hexs(got), hexs(v$output)))
    }
  }

  path <- file.path(target, paste0(v$case, ".bin"))
  rows[[length(rows) + 1L]] <- data.frame(
    case = v$case, codec = "deflate-raw", rfc = v$rfc,
    rfc_section = v$rfc_section, expect = expect, class = class_,
    output_n = out_n, output_hex = out_hex, note = v$note,
    bytes = file.size(path), md5 = unname(tools::md5sum(path)),
    stringsAsFactors = FALSE
  )
}
manifest <- do.call(rbind, rows)
manifest <- manifest[order(manifest$case), ]

write.table(manifest, file.path(target, "MANIFEST.tsv"),
            sep = "\t", quote = FALSE, row.names = FALSE)

dev <- manifest[manifest$expect == "deviation", ]
cat(sprintf("%d vectors: %d controls, %d rejected, %d deviations\n",
            nrow(manifest), sum(manifest$expect == "ok"),
            sum(manifest$expect == "error"), nrow(dev)))
if (nrow(dev)) {
  cat("\nDEVIATIONS -- RFC 1951 requires rejection, this build accepts:\n")
  for (i in seq_len(nrow(dev))) {
    cat(sprintf("  %-28s %s (%s)\n", dev$case[i], dev$note[i], dev$rfc_section[i]))
  }
  cat("\n")
}

if (!check_only) {
  cat(sprintf("wrote %d vectors to %s\n", nrow(manifest), target))
  quit(status = 0)
}

# -- --check: compare against what is committed ------------------------------
committed <- file.path(corpus_dir, "MANIFEST.tsv")
if (!file.exists(committed)) stop("no committed MANIFEST.tsv to compare against")
old <- read.delim(committed, colClasses = "character")

status <- 0L
missing <- setdiff(old$case, manifest$case)
added   <- setdiff(manifest$case, old$case)
if (length(missing)) { cat("MISSING:\n"); cat(paste0("  ", missing, "\n")); status <- 1L }
if (length(added))   { cat("ADDED:\n");   cat(paste0("  ", added, "\n"));   status <- 1L }

both <- intersect(old$case, manifest$case)
o <- old[match(both, old$case), ]
n <- manifest[match(both, manifest$case), ]
for (col in c("md5", "expect", "class", "output_n", "output_hex")) {
  bad <- which(o[[col]] != n[[col]])
  for (i in bad) {
    cat(sprintf("CHANGED %-28s %s: committed %s, local %s\n",
                both[i], col, sQuote(o[[col]][i]), sQuote(n[[col]][i])))
    status <- 1L
  }
}
if (status == 0L) cat(sprintf("ok  %d vectors match\n", length(both)))
quit(status = status)
