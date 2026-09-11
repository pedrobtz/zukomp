# Corpus integrity only. Actually *decoding* the fixtures is the interop
# suite's job and needs a real codec, so it arrives with Stage 6. Until then
# these tests make sure the corpus cannot rot unnoticed: a fixture deleted,
# truncated or regenerated without updating MANIFEST.tsv fails here.

test_that("the fixture manifest matches the files on disk", {
  m <- read.delim(test_path("fixtures", "MANIFEST.tsv"), stringsAsFactors = FALSE)
  expect_gt(nrow(m), 0L)

  for (i in seq_len(nrow(m))) {
    path <- test_path("fixtures", m$codec[i], m$file[i])
    expect_true(file.exists(path), info = path)
    expect_identical(unname(tools::md5sum(path)), m$md5[i], info = path)
    expect_identical(as.double(file.size(path)), as.double(m$bytes[i]), info = path)
  }
})

test_that("no fixture on disk is missing from the manifest", {
  m <- read.delim(test_path("fixtures", "MANIFEST.tsv"), stringsAsFactors = FALSE)
  listed <- file.path(m$codec, m$file)
  on_disk <- list.files(test_path("fixtures"), recursive = TRUE)
  on_disk <- setdiff(on_disk, "MANIFEST.tsv")
  # fixtures/malformed/ is a second corpus with its own manifest and its own
  # integrity test below; this manifest does not describe it.
  on_disk <- on_disk[!startsWith(on_disk, "malformed/")]
  expect_setequal(on_disk, listed)
})

test_that("the malformed manifest matches the files on disk", {
  # Same integrity contract as the interop corpus: a vector deleted, edited
  # or regenerated without updating its manifest row fails here rather than
  # quietly testing something other than what its name says.
  m <- malformed_manifest()
  expect_gt(nrow(m), 0L)
  for (i in seq_len(nrow(m))) {
    path <- test_path("fixtures", "malformed", paste0(m$case[i], ".bin"))
    expect_true(file.exists(path), info = path)
    expect_identical(unname(tools::md5sum(path)), m$md5[i], info = path)
    expect_identical(as.double(file.size(path)), as.double(m$bytes[i]), info = path)
  }
  on_disk <- list.files(test_path("fixtures", "malformed"))
  expect_setequal(setdiff(on_disk, "MANIFEST.tsv"), paste0(m$case, ".bin"))
})

test_that("the malformed manifest is read as text, not coerced to numbers", {
  # output_hex and output_n are all-digit strings. read.delim() would type
  # convert the column, turning "" into NA and dropping a hex string's
  # leading zeros; malformed_manifest() passes colClasses to prevent it.
  # Asserted here because the corruption is silent and payload dependent.
  m <- malformed_manifest()
  expect_type(m$output_hex, "character")
  expect_type(m$output_n, "character")
  expect_false(anyNA(m$output_hex))
  expect_identical(m$output_hex[m$case == "raw-stored-ok"], "74657374")
  expect_identical(m$output_hex[m$case == "raw-stored-empty-ok"], "")
})

test_that("the manifest records enough to reproduce every fixture", {
  m <- read.delim(test_path("fixtures", "MANIFEST.tsv"), stringsAsFactors = FALSE)
  expect_true(all(c("codec", "file", "generator", "generator_version",
                    "payload", "n", "members", "bytes", "md5") %in% names(m)))
  expect_false(any(is.na(m$generator_version)))
  expect_false(any(m$generator_version == ""))
  # Every payload must be one this build can reconstruct, or the interop
  # tests have nothing to compare a decoded fixture against.
  expect_true(all(m$payload %in% payload_kinds()))
})

test_that("the corpus covers what design 17 says is risky", {
  m <- read.delim(test_path("fixtures", "MANIFEST.tsv"), stringsAsFactors = FALSE)
  gz <- m$file[m$codec == "gzip"]

  # The gzip header is the single most likely place for a parser bug, so
  # every optional field needs a fixture, not just the minimal 10 bytes.
  expect_true(any(grepl("fextra", gz)))
  expect_true(any(grepl("fname", gz)))
  expect_true(any(grepl("fcomment", gz)))
  expect_true(any(grepl("fhcrc", gz)))
  expect_true(any(grepl("allflags", gz)))
  expect_true(any(m$members > 1L))            # concatenated members
  expect_true(all(c("gzip", "zlib", "deflate-raw") %in% m$codec))

  # More than one implementation, or this proves only self-consistency.
  expect_gt(length(unique(m$generator)), 1L)
  # Incompressible input, which is what exercises DEFLATE's stored blocks.
  expect_true("lcg" %in% m$payload)
  expect_true("empty" %in% m$payload)
})
