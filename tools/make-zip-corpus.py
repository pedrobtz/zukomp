#!/usr/bin/env python3
"""Generate the ZIP seed corpus for fuzz_zip_reader (#37).

    python3 tools/make-zip-corpus.py            regenerate fuzz/corpus/zip/
    python3 tools/make-zip-corpus.py --check    regenerate to a temp dir, diff

Maintainer script, run offline; nothing in R CMD check runs it, and the
corpus is committed. Every archive is written by Python's zipfile -- an
implementation independent of miniz -- or derived from one by editing named
fields, so each hostile seed differs from a valid one in exactly the field
its name gives. MANIFEST.tsv records, per seed, the generator, and `spec`:
what APPNOTE.TXT (6.3.10) says a reader must do with it, "ok" or "reject",
declared from the specification and never from either reader's behaviour.
tools/check-zip-reader.py holds both readers to that column.

Deterministic: fixed timestamps, no system clock, so --check can pass.
"""
import hashlib
import io
import os
import struct
import sys
import tempfile
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEST = os.path.join(ROOT, "fuzz", "corpus", "zip")

# A non-zero DOS time on purpose: miniz's stored-entry size check reads the
# method and the time as one 32-bit word, so it only runs when the time is
# zero (see stored-size-mismatch below). 12:00:00 is 0x6000.
WHEN = (2026, 11, 10, 12, 0, 0)
MIDNIGHT_1980 = (1980, 1, 1, 0, 0, 0)   # DOS time 0x0000


def lcg(n):
    """The Park-Miller stream new_payload("lcg") uses, middle bits."""
    out = bytearray()
    x = 12345
    for _ in range(n):
        x = (16807 * x) % 2147483647
        out.append((x >> 16) & 0xFF)
    return bytes(out)


ASCII = (b"the quick brown fox jumps. " * 200)[:4096]
STRUCTURED = b",".join(b'{"id":%d,"v":"x"}' % i for i in range(1, 8000))[:100000]


def info(name, method=zipfile.ZIP_DEFLATED, when=WHEN):
    zi = zipfile.ZipInfo(name, date_time=when)
    zi.compress_type = method
    zi.create_system = 3
    zi.external_attr = 0o644 << 16
    return zi


def build(entries, comment=b"", stream=False, zip64=False):
    """entries: (ZipInfo, bytes) pairs. stream=True writes through a
    non-seekable file, which makes zipfile set general purpose bit 3 and
    write data descriptors (APPNOTE 4.3.9)."""
    raw = io.BytesIO()
    if stream:
        class Unseekable(io.RawIOBase):
            def writable(self):
                return True

            def write(self, b):
                return raw.write(b)
        fh = io.BufferedWriter(Unseekable())
    else:
        fh = raw
    with zipfile.ZipFile(fh, "w") as zf:
        zf.comment = comment
        for zi, data in entries:
            if zi.is_dir():
                zf.writestr(zi, b"")
            elif zip64:
                with zf.open(zi, "w", force_zip64=True) as w:
                    w.write(data)
            else:
                zf.writestr(zi, data)
    if stream:
        fh.flush()
    return raw.getvalue()


# -- field surgery, for the hostile seeds -------------------------------------
# Offsets are APPNOTE 4.3.7 (local header) and 4.3.12 (central header).

def central_headers(b):
    """Offsets of every central directory header, from the EOCD."""
    eocd = b.rindex(b"PK\x05\x06")
    count = struct.unpack_from("<H", b, eocd + 10)[0]
    pos = struct.unpack_from("<I", b, eocd + 16)[0]
    out = []
    for _ in range(count):
        assert b[pos:pos + 4] == b"PK\x01\x02"
        out.append(pos)
        n, m, k = struct.unpack_from("<HHH", b, pos + 28)
        pos += 46 + n + m + k
    return out


def local_of(b, cen):
    return struct.unpack_from("<I", b, cen + 42)[0]


def data_of(b, cen):
    loc = local_of(b, cen)
    n, m = struct.unpack_from("<HH", b, loc + 26)
    return loc + 30 + n + m


def put(b, off, fmt, *vals):
    b = bytearray(b)
    struct.pack_into(fmt, b, off, *vals)
    return bytes(b)


def set_both(b, cen, cen_off, loc_off, fmt, *vals):
    """The same field in the central header and in its local header, so a
    reader that trusts either one sees the same lie."""
    b = put(b, cen + cen_off, fmt, *vals)
    return put(b, local_of(b, cen) + loc_off, fmt, *vals)


def main():
    check = "--check" in sys.argv[1:]
    dest = tempfile.mkdtemp() if check else DEST
    os.makedirs(dest, exist_ok=True)
    ver = "python%d.%d.%d/zipfile/zlib%s" % (sys.version_info[:3] +
                                              (zlib.ZLIB_VERSION,))
    rows = []

    def emit(name, blob, spec, note):
        path = os.path.join(dest, name + ".zip")
        with open(path, "wb") as fh:
            fh.write(blob)
        rows.append((name + ".zip", "python", ver, spec,
                     str(len(blob)), hashlib.md5(blob).hexdigest(), note))

    # -- valid archives: the forms a writer produces -------------------------
    emit("stored-one", build([(info("a.txt", zipfile.ZIP_STORED), ASCII)]),
         "ok", "one stored entry")
    emit("deflate-one", build([(info("a.txt"), ASCII)]),
         "ok", "one deflated entry")
    emit("deflate-large", build([(info("big.json"), STRUCTURED)]),
         "ok", "100 kB deflated: past the 32 KiB window the iterator wraps in")
    emit("deflate-incompressible", build([(info("noise.bin"), lcg(70000))]),
         "ok", "DEFLATE stored blocks inside a deflated entry")
    emit("mixed", build([
        (info("dir/", zipfile.ZIP_STORED), b""),
        (info("dir/a.txt"), ASCII),
        (info("dir/noise.bin", zipfile.ZIP_STORED), lcg(1000)),
        (info("empty.txt"), b""),
        (info("empty-stored.txt", zipfile.ZIP_STORED), b""),
    ]), "ok", "a directory, stored, deflated and empty entries")
    emit("empty-archive", build([]), "ok", "no entries: the EOCD alone")
    emit("comments", build([(info("a.txt"), ASCII)],
                           comment=b"an archive comment"),
         "ok", "an archive comment (APPNOTE 4.3.16)")
    # The EOCD is found by scanning back from the end in 4 KiB windows; a
    # comment longer than one window puts it in the second, which must
    # still be found while it is within 64 KiB of the end (APPNOTE 4.3.16).
    emit("long-comment", build([(info("a.txt"), b"x")], comment=b"c" * 4500),
         "ok", "a 4500-byte archive comment: the EOCD is past the first scan window")
    emit("utf8-name", build([(info("café/中文.txt"), ASCII)]),
         "ok", "a UTF-8 name, general purpose bit 11")
    emit("data-descriptor", build([(info("a.txt"), ASCII),
                                   (info("b.bin", zipfile.ZIP_STORED),
                                    lcg(500))], stream=True),
         "ok", "bit 3: sizes and CRC after the data (APPNOTE 4.3.9)")
    emit("zip64-entry", build([(info("a.txt"), ASCII)], zip64=True),
         "ok", "a ZIP64 extended information extra field (APPNOTE 4.5.3)")
    emit("many-entries", build([(info("f%03d.txt" % i), b"entry %d\n" % i)
                                for i in range(64)]),
         "ok", "64 small entries")
    emit("duplicate-names", build([(info("a.txt"), b"first\n"),
                                   (info("a.txt"), b"second\n")]),
         "ok", "two entries of one name, which APPNOTE does not forbid")
    emit("method-bzip2", build([(info("a.txt", zipfile.ZIP_BZIP2), ASCII)]),
         "ok", "method 12, which miniz does not implement")

    # -- hostile archives: one field wrong each ------------------------------
    base_s = build([(info("a.txt", zipfile.ZIP_STORED), ASCII),
                    (info("b.txt"), ASCII)])
    base_d = build([(info("a.txt"), ASCII), (info("b.txt"), ASCII)])
    c_s = central_headers(base_s)[0]
    c_d = central_headers(base_d)[0]

    emit("crc-bad-stored", set_both(base_s, c_s, 16, 14, "<I",
                                    zlib.crc32(ASCII) ^ 1),
         "reject", "stored entry, CRC-32 off by one bit (APPNOTE 4.4.7)")
    emit("crc-bad-deflate", set_both(base_d, c_d, 16, 14, "<I",
                                     zlib.crc32(ASCII) ^ 1),
         "reject", "deflated entry, CRC-32 off by one bit")

    d = data_of(base_d, c_d)
    corrupt = bytearray(base_d)
    corrupt[d] = 0x07                       # BTYPE 11: reserved, RFC 1951
    emit("deflate-corrupt", bytes(corrupt),
         "reject", "the member's DEFLATE stream opens with a reserved block type")

    comp = struct.unpack_from("<I", base_d, c_d + 20)[0]
    emit("deflate-truncated-member",
         set_both(base_d, c_d, 20, 18, "<I", comp // 2),
         "reject", "compressed size halved: the DEFLATE stream ends early")

    uncomp = len(ASCII)
    emit("size-short-deflate", set_both(base_d, c_d, 24, 22, "<I", uncomp - 1),
         "reject", "declared uncompressed size one byte short")
    emit("size-long-deflate", set_both(base_d, c_d, 24, 22, "<I", uncomp + 1),
         "reject", "declared uncompressed size one byte long")

    # Stored means the two sizes are equal (APPNOTE 4.4.5, method 0). Here
    # the declared uncompressed size runs 16 bytes past the data, into the
    # next local header, and the CRC-32 is the CRC of exactly those bytes,
    # so a reader that reads the uncompressed size from the data offset
    # gets a CRC match on bytes that belong to another entry.
    ds = data_of(base_s, c_s)
    over = base_s[ds:ds + uncomp + 16]
    lie = set_both(base_s, c_s, 24, 22, "<I", uncomp + 16)
    lie = set_both(lie, c_s, 16, 14, "<I", zlib.crc32(over))
    emit("stored-size-mismatch", lie, "reject",
         "stored, uncompressed size 16 bytes past the data, CRC of the "
         "over-read")
    # The same lie at DOS time 0, the only time miniz's check sees it.
    base_s0 = build([(info("a.txt", zipfile.ZIP_STORED, MIDNIGHT_1980), ASCII),
                     (info("b.txt", when=MIDNIGHT_1980), ASCII)])
    c_s0 = central_headers(base_s0)[0]
    ds0 = data_of(base_s0, c_s0)
    over0 = base_s0[ds0:ds0 + uncomp + 16]
    lie0 = set_both(base_s0, c_s0, 24, 22, "<I", uncomp + 16)
    lie0 = set_both(lie0, c_s0, 16, 14, "<I", zlib.crc32(over0))
    emit("stored-size-mismatch-time0", lie0, "reject",
         "stored-size-mismatch at DOS time 0")

    emit("local-offset-past-end",
         put(base_d, c_d + 42, "<I", len(base_d) + 100),
         "reject", "the local header offset points past the archive")
    emit("comp-size-past-end", set_both(base_d, c_d, 20, 18, "<I",
                                        len(base_d)),
         "reject", "the compressed size runs past the archive")
    emit("encrypted-flag", set_both(base_d, c_d, 8, 6, "<H", 0x0001),
         "reject", "general purpose bit 0, encrypted, with no password")

    empty = build([(info("e.txt", zipfile.ZIP_STORED), b"")])
    c_e = central_headers(empty)[0]
    emit("empty-crc-nonzero", set_both(empty, c_e, 16, 14, "<I", 0x12345678),
         "reject", "an empty entry whose CRC-32 is not the CRC of nothing")

    # Entries with no compressed data, which miniz's extract_to_mem and
    # extract_to_callback used to accept without reading their local header
    # or method (found by mutating mixed.zip; #92).
    c_e0 = central_headers(empty)[0]
    bad_sig = bytearray(empty)
    bad_sig[local_of(empty, c_e0)] ^= 0xFF
    emit("empty-local-header-bad", bytes(bad_sig), "reject",
         "an empty entry whose local header signature is wrong (APPNOTE 4.3.7)")
    emit("deflate-zero-bytes", set_both(empty, c_e0, 10, 8, "<H", 8), "reject",
         "method 8 with no compressed bytes: no DEFLATE stream, which has at "
         "least one block (RFC 1951 3.2.3)")
    emit("truncated-archive", base_d[:len(base_d) // 2],
         "reject", "the second half, with the central directory, is gone")
    eocd = base_d.rindex(b"PK\x05\x06")
    emit("eocd-count-high", put(put(base_d, eocd + 8, "<H", 9),
                                eocd + 10, "<H", 9),
         "reject", "the EOCD claims nine entries for two")

    rows.sort()
    with open(os.path.join(dest, "MANIFEST.tsv"), "w") as fh:
        fh.write("file\tgenerator\tgenerator_version\tspec\tbytes\tmd5\tnote\n")
        for r in rows:
            fh.write("\t".join(r) + "\n")

    if not check:
        print("wrote %d archives to %s" % (len(rows), dest))
        return 0
    old = open(os.path.join(DEST, "MANIFEST.tsv")).read().splitlines()[1:]
    new = [("\t".join(r)) for r in rows]
    oldk = {l.split("\t")[0]: l for l in old}
    newk = {l.split("\t")[0]: l for l in new}
    status = 0
    for k in sorted(set(oldk) | set(newk)):
        o, n = oldk.get(k), newk.get(k)
        if o == n:
            continue
        og = o.split("\t")[2] if o else None
        ng = n.split("\t")[2] if n else None
        if o and n and og != ng:
            print("differs, generator changed: %s (%s -> %s)" % (k, og, ng))
            continue
        print("MISMATCH %s" % k)
        status = 1
    if status == 0:
        print("ok  %d archives match" % len(new))
    return status


if __name__ == "__main__":
    sys.exit(main())
