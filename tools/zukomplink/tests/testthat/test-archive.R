# The archive consumption mode: miniz's ZIP reader, linked statically out of
# inst/lib/libzukomp.a, with zukomp's namespace never loaded. See ../../
# NAMESPACE for why there is no importFrom here to make that true.

test_that("the linked archive answers with miniz's version", {
  v <- miniz_version()
  expect_type(v, "character")
  expect_length(v, 1L)
  # Shape only. The pinned version is asserted in zukomp's own test-abi.R,
  # and tools/check-linking.sh cross-checks this string against that same
  # installed zukomp -- a third literal here would be a third place to
  # update on a vendor bump, which tools/vendor/verify could not see.
  expect_match(v, "^[0-9]+\\.[0-9]+\\.[0-9]+$")
})

test_that("the central directory lists every member with its declared size", {
  m <- zip_members(probe_zip())

  expect_identical(m$name,
                   c("hello.txt", "text.txt", "lcg.bin", "stored.txt",
                     "empty.txt"))
  expect_identical(m$uncomp_size,
                   c(28, 180 * 28, 3000, 47, 0))

  # The compressible members must actually have compressed, or the fixture
  # would be reading stored blocks throughout and never exercise inflate.
  expect_lt(m$comp_size[[2]], m$uncomp_size[[2]] / 4)
  # ...and the incompressible one must not have, which is what proves
  # lcg.bin is the stand-in it claims to be.
  expect_gte(m$comp_size[[3]], m$uncomp_size[[3]])
})

test_that("mtime is a real timestamp rather than padding", {
  # The load-bearing assertion of this whole fixture, and the one no symbol
  # table can make. zukomp's src/Makevars compiles this archive with
  # -UMINIZ_NO_TIME so that mz_zip_archive_file_stat ends in m_time. With
  # the trim left in place it ends in m_padding instead -- same size, same
  # offset, never written -- and a consumer compiling miniz.h at its
  # defaults would read uninitialised bytes and call them a date.
  m <- zip_members(probe_zip())

  expect_false(any(m$mtime == 0))
  stamped <- as.POSIXct(m$mtime, origin = "1970-01-01", tz = "")
  # Month precision, not day: miniz converts the DOS fields through mktime,
  # so the value is local time, and the fixture's 2026-09-21 12:34:56 stays
  # inside September 2026 in every timezone on earth.
  expect_identical(unique(format(stamped, "%Y-%m")), "2026-09")
})

test_that("a deflated member decompresses to the bytes zukomp compressed", {
  # The round trip that spans both halves of the package: zukomp's own
  # deflate-raw codec wrote these streams in tools/make-link-fixture.R, and
  # miniz's ZIP reader -- code zukomp.so deliberately does not contain --
  # reads them back here, inside a different shared object.
  expect_identical(zip_extract(probe_zip(), "hello.txt"),
                   charToRaw("zukomp reads ZIP containers\n"))
  expect_identical(zip_extract(probe_zip(), "text.txt"), payload_text())
  expect_identical(zip_extract(probe_zip(), "lcg.bin"), payload_lcg(3000L))
})

test_that("a stored member reads, and so does an empty one", {
  # Method 0 takes a different path through the extraction iterator than
  # method 8, and an empty member is the boundary where "read returned 0"
  # means "done" rather than "stalled".
  expect_identical(
    zip_extract(probe_zip(), "stored.txt"),
    charToRaw("this member is not compressed at all, method 0\n"))
  expect_identical(zip_extract(probe_zip(), "empty.txt"), raw(0))
})

test_that("extraction does not depend on the chunk size", {
  # What a pull-style reader such as xlsxio needs, and the reason a
  # downstream package links this archive rather than going through the
  # codec registry at all. Swept the same way zukomp sweeps its own driver.
  for (payload in c("hello.txt", "text.txt", "lcg.bin", "stored.txt",
                    "empty.txt")) {
    whole <- zip_extract(probe_zip(), payload)
    for (k in chunk_sizes()) {
      expect_identical(zip_extract(probe_zip(), payload, chunk = k), whole,
                       info = paste0(payload, " at chunk = ", k))
    }
  }
})

test_that("a member that is not there is an error, not empty bytes", {
  expect_error(zip_extract(probe_zip(), "absent.txt"),
               class = "zukomplink_error")
})

test_that("the classed error still carries what the C layer said", {
  # The other half of asserting on classes: every test above would pass just
  # as well if zl_classed() re-raised with an empty message, and the person
  # debugging a broken archive would be left with a bare condition class.
  # Checked once, here, so the rest can assert on the class alone.
  err <- tryCatch(zip_extract(probe_zip(), "absent.txt"), error = identity)
  expect_s3_class(err, "zukomplink_error")
  expect_match(conditionMessage(err), "zukomplink: no such member")
})

test_that("a file that is not an archive is rejected", {
  # Not a crash and not silence: the reader has to notice. A consumer that
  # gets this wrong reports a corrupt .xlsx as an empty one.
  not_zip <- withr::local_tempfile()
  writeBin(charToRaw("PK not really\n"), not_zip)
  expect_error(zip_members(not_zip), class = "zukomplink_error")
  expect_error(zip_extract(not_zip, "hello.txt"), class = "zukomplink_error")
})
