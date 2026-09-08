#!/usr/bin/env python3
"""Generate the compressed interop fixtures that need byte-level header control.

Maintainer script, run offline by tools/make-fixtures.R. Never runs during
R CMD check: CRAN guarantees neither Python nor a system gzip, which is the
whole reason the corpus is committed rather than produced at test time.

Writes files into <outdir>/<codec>/ and prints one TSV row per file on
stdout: codec, file, generator, generator_version, payload, members.
"""
import binascii
import os
import sys
import zlib

MTIME = 0          # deterministic: fixtures must be byte-reproducible
XFL = 0
OS_UNKNOWN = 255

FTEXT, FHCRC, FEXTRA, FNAME, FCOMMENT = 1, 2, 4, 8, 16


def raw_deflate(data, level):
    c = zlib.compressobj(level, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def gzip_member(data, level=6, flags=0, extra=b"", name=b"", comment=b""):
    """A single RFC 1952 member, with whichever optional fields are asked for.

    Built by hand rather than with the gzip module because the point is to
    exercise zukomp's header parser against FEXTRA/FNAME/FCOMMENT/FHCRC,
    which the module will not emit."""
    head = bytes([0x1F, 0x8B, 8, flags])
    head += MTIME.to_bytes(4, "little")
    head += bytes([XFL, OS_UNKNOWN])
    if flags & FEXTRA:
        head += len(extra).to_bytes(2, "little") + extra
    if flags & FNAME:
        head += name + b"\x00"
    if flags & FCOMMENT:
        head += comment + b"\x00"
    if flags & FHCRC:
        head += (zlib.crc32(head) & 0xFFFF).to_bytes(2, "little")
    body = raw_deflate(data, level)
    tail = (zlib.crc32(data) & 0xFFFFFFFF).to_bytes(4, "little")
    tail += (len(data) & 0xFFFFFFFF).to_bytes(4, "little")
    return head + body + tail


def main():
    outdir, payloaddir = sys.argv[1], sys.argv[2]
    ver = "python%d.%d.%d/zlib%s" % (sys.version_info[:3] + (zlib.ZLIB_VERSION,))

    def payload(kind):
        with open(os.path.join(payloaddir, kind + ".bin"), "rb") as fh:
            return fh.read()

    rows = []

    def emit(codec, name, blob, payload_kind, members=1):
        d = os.path.join(outdir, codec)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, name), "wb") as fh:
            fh.write(blob)
        rows.append((codec, name, "python", ver, payload_kind, str(members)))

    a = payload("ascii")

    # gzip: one fixture per optional header field, then all of them at once.
    # The header is the most likely place for a parser bug, so it gets the
    # densest coverage in the corpus.
    emit("gzip", "py_plain_ascii.bin", gzip_member(a), "ascii")
    emit("gzip", "py_fextra_ascii.bin",
         gzip_member(a, flags=FEXTRA, extra=b"AB\x04\x00wxyz"), "ascii")
    emit("gzip", "py_fname_ascii.bin",
         gzip_member(a, flags=FNAME, name=b"payload.txt"), "ascii")
    emit("gzip", "py_fcomment_ascii.bin",
         gzip_member(a, flags=FCOMMENT, comment=b"a comment"), "ascii")
    emit("gzip", "py_fhcrc_ascii.bin", gzip_member(a, flags=FHCRC), "ascii")
    emit("gzip", "py_allflags_ascii.bin",
         gzip_member(a, flags=FTEXT | FHCRC | FEXTRA | FNAME | FCOMMENT,
                     extra=b"AB\x02\x00hi", name=b"p.txt",
                     comment=b"c"), "ascii")
    # RFC 1952 permits concatenated members and standard tools produce them.
    emit("gzip", "py_multimember_ascii.bin",
         gzip_member(a, level=1) + gzip_member(a, level=9), "ascii", members=2)
    emit("gzip", "py_empty.bin", gzip_member(payload("empty")), "empty")
    emit("gzip", "py_zeros.bin", gzip_member(payload("zeros"), level=9), "zeros")
    emit("gzip", "py_lcg.bin", gzip_member(payload("lcg"), level=9), "lcg")
    emit("gzip", "py_utf8.bin", gzip_member(payload("utf8")), "utf8")

    for lvl in (1, 6, 9):
        emit("zlib", "py_l%d_ascii.bin" % lvl, zlib.compress(a, lvl), "ascii")
    emit("zlib", "py_l9_zeros.bin", zlib.compress(payload("zeros"), 9), "zeros")
    emit("zlib", "py_l9_lcg.bin", zlib.compress(payload("lcg"), 9), "lcg")
    emit("zlib", "py_l6_empty.bin", zlib.compress(payload("empty"), 6), "empty")

    for lvl in (1, 9):
        emit("deflate-raw", "py_l%d_ascii.bin" % lvl, raw_deflate(a, lvl), "ascii")
    emit("deflate-raw", "py_l9_lcg.bin", raw_deflate(payload("lcg"), 9), "lcg")
    emit("deflate-raw", "py_l6_empty.bin",
         raw_deflate(payload("empty"), 6), "empty")
    emit("deflate-raw", "py_l9_zeros.bin",
         raw_deflate(payload("zeros"), 9), "zeros")

    for row in rows:
        sys.stdout.write("\t".join(row) + "\n")


if __name__ == "__main__":
    main()
