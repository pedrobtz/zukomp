# The registry's identity invariants, and the R-side table cache.
#
# Both are only testable from a real satellite package: registration is
# process-global, init-time only, and has no removal API, so nothing inside
# zukomp's own suite can reach these paths.

test_that("a satellite implementing a declared codec becomes usable", {
  # snappy-raw is declared by zukomp and implemented here. This is the case
  # the table cache used to miss: it flips an existing row from
  # available = FALSE to TRUE without changing the number of rows.
  expect_true(zukomp::komp_codec_available("snappy-raw"))

  d <- zukomp::komp_codecs()
  row <- d[d$id == "snappy-raw", ]
  expect_equal(nrow(row), 1L)
  expect_true(row$available)
  expect_identical(row$source, "zukomptest")
  expect_true(row$can_encode)
  expect_true(row$can_decode)

  # and it actually works through the ordinary R API, by name
  x <- charToRaw("declared codec, satellite implementation")
  z <- zukomp::komp_compress(x, "snappy-raw")
  expect_identical(zukomp::komp_decompress(z, "snappy-raw"), x)
})

test_that("komp_codecs() has no duplicate ids", {
  # Defence in depth. The registration boundary is the actual fix, but a
  # duplicated id makes R's scalar `if (!row$available)` receive a
  # length-two logical and error out, taking every operation that names the
  # codec with it.
  d <- zukomp::komp_codecs()
  expect_identical(anyDuplicated(d$id), 0L)
})

test_that("the registry refuses every ambiguous identity", {
  # ZU_ERR_INVALID_ARGUMENT is 4 in the status enum.
  invalid <- 4L
  cases <- c(
    "vendor id claiming a built-in name"          = 0L,
    "declared id with the wrong name"             = 1L,
    "unknown id below the vendor base"            = 2L,
    "vendor id squatting a declared absent name"  = 3L,
    "duplicate registered name"                   = 4L,
    "duplicate numeric id"                        = 5L,
    "duplicate declared content-coding token"     = 6L,
    "vtable too short for the required fields"    = 7L
  )
  for (nm in names(cases)) {
    expect_identical(try_bad_registration(cases[[nm]]), invalid, info = nm)
  }
})

test_that("a rejected registration leaves the registry intact", {
  for (i in 0:7) invisible(try_bad_registration(i))

  d <- zukomp::komp_codecs()
  expect_identical(anyDuplicated(d$id), 0L)
  expect_true(all(c("gzip", "zlib", "deflate-raw", "identity",
                    "xor5a", "snappy-raw") %in% d$id))

  # the codecs whose names the rejected registrations tried to claim still work
  x <- charToRaw(strrep("still fine ", 50))
  expect_identical(zukomp::komp_decompress(zukomp::komp_compress(x, "gzip")), x)
  expect_identical(
    zukomp::komp_decompress(zukomp::komp_compress(x, "xor5a"), "xor5a"), x)
  expect_identical(zukomp::komp_codec_available("zstd"), FALSE)
})

test_that("a cold codec table sees a satellite registered after it warmed", {
  # The load-order bug, end to end. It cannot be reproduced in this process,
  # because this package's DLL -- and so its registrations -- is loaded
  # before any test runs. A subprocess is what makes the ordering
  # observable: warm zukomp's table first, then load the satellite.
  skip_on_cran()
  skip_if_not(nzchar(Sys.which("Rscript")))

  # ZUKOMPTEST_DECLARED_ONLY suppresses the xor5a registration. Without
  # that, xor5a -- a vendor id -- adds a row all by itself, which would
  # invalidate even a row-count-keyed cache and mask the bug entirely. With
  # only the declared-codec stub registering, the row count is identical
  # before and after, so the cache has nothing to notice except the
  # registration itself.
  script <- '
    library(zukomp)
    before <- komp_codecs()
    warm <- before$available[before$id == "snappy-raw"]
    loadNamespace("zukomptest")
    after <- komp_codecs()
    cat(sprintf("%s,%s,%s,%s\\n",
        warm,
        after$available[after$id == "snappy-raw"],
        identical(nrow(before), nrow(after)),
        isTRUE(tryCatch(
          identical(komp_decompress(komp_compress(charToRaw("xy"),
                                                  "snappy-raw"),
                                    "snappy-raw"), charToRaw("xy")),
          error = function(e) FALSE))))
  '
  out <- suppressWarnings(withr::with_envvar(
    c(ZUKOMPTEST_DECLARED_ONLY = "1"),
    system2(Sys.which("Rscript"), c("-e", shQuote(script)),
            stdout = TRUE, stderr = FALSE)
  ))
  skip_if(length(out) == 0L, "subprocess produced no output")

  parts <- strsplit(trimws(tail(out, 1L)), ",", fixed = TRUE)[[1]]
  # Fail with the subprocess output rather than a subscript error if the
  # line is malformed -- that is what a regression here looks like.
  expect_length(parts, 4L)
  skip_if(length(parts) != 4L, paste(out, collapse = " | "))
  expect_identical(parts[[1]], "FALSE")  # unavailable before the satellite
  expect_identical(parts[[2]], "TRUE")   # available after, despite the warm
                                         # cache
  expect_identical(parts[[3]], "TRUE")   # and the row count never changed --
                                         # which is what made the old cache
                                         # key miss this entirely
  expect_identical(parts[[4]], "TRUE")   # and it is usable by name
})
