# The LinkingTo surface: inst/lib/libzukomp.a and miniz's header, for a
# consumer that has to read a ZIP container rather than a byte buffer. See
# src/Makevars for why it is a second compilation of miniz.c.
#
# These read the *installed* package, which is what a consumer sees. Under
# devtools::load_all() there is no installed layout, so they skip; R CMD check
# runs them against a real installation, which is where they have teeth.
#
# installed_path(), archive_symbols() and archive_defined() are in
# helper-abi.R, not here: this suite runs in parallel, and a worker sources
# helper-*.R but not another test file's file scope.

test_that("the static archive and miniz.h are installed", {
  archive <- installed_path("lib", "libzukomp.a")
  expect_true(file.exists(archive))
  expect_gt(file.size(archive), 0)

  # The merge is the fragile part: install.libs.R writes miniz.h into
  # <pkg>/include during the libs step, and R's own "inst" step copies
  # zukomp.h there afterwards. If that replaced the directory instead of
  # merging into it, the archive would still install and only miniz.h would
  # vanish.
  expect_true(file.exists(installed_path("include", "miniz.h")))
  expect_true(file.exists(installed_path("include", "zukomp.h")))
  expect_true(file.exists(installed_path("include", "zukomp-r.h")))
})

test_that("the archive defines the ZIP reader", {
  defined <- archive_defined()
  # The call sequence a consumer needs to pull one member out of an archive.
  required <- c("mz_zip_reader_init_file", "mz_zip_reader_locate_file_v2",
                "mz_zip_reader_file_stat", "mz_zip_reader_extract_iter_new",
                "mz_zip_reader_extract_iter_read",
                "mz_zip_reader_extract_iter_free", "mz_zip_reader_end")
  for (name in required) {
    expect_length(grep(paste0("\\b_?", name, "\\b"), defined, value = TRUE), 1L)
  }
})

test_that("the archive stops at reading", {
  # Reader only, per the design. A consumer that calls a writer function gets
  # a link error, which is the loud failure; silently shipping a ZIP writer
  # to every consumer is not.
  expect_length(grep("\\bmz_zip_writer_", archive_defined(), value = TRUE), 0L)
})

test_that("the archive exports no zlib-ABI name and no R symbol", {
  # Same reason as for the shared object in test-abi.R, and it matters more
  # here: this object is linked into someone else's binary, next to the zlib
  # the R process already has.
  defined <- archive_defined()
  banned <- c("compress", "compressBound", "uncompress",
              "deflate", "deflateInit", "inflate", "inflateInit",
              "crc32", "adler32")
  for (name in banned) {
    expect_length(grep(paste0("\\b_?", name, "\\b"), defined, value = TRUE), 0L)
  }
  # No zukomp R glue: the archive is miniz and nothing else.
  expect_length(grep("\\b_?(zu_|zukomp_|R_init_)", defined, value = TRUE), 0L)
})

test_that("the shared object and the archive stay different builds", {
  # The split is what lets zukomp.so keep its trim while consumers get ZIP.
  # Asserting both halves in one place makes the relationship explicit: if
  # someone ever widens the trim in src/Makevars instead of adding to the
  # second build, this and test-abi.R fail together.
  expect_length(grep("mz_zip", exported_symbols(), value = TRUE), 0L)
  expect_gt(length(grep("mz_zip_reader", archive_defined(), value = TRUE)), 0L)
})
