#!/usr/bin/env python3
"""Writes the seeds for fuzz/fuzz_lifecycle.c into fuzz/corpus/lifecycle/.

    python3 tools/make-lifecycle-seeds.py           write them
    python3 tools/make-lifecycle-seeds.py --check   fail if they differ

Each `regress-<rule>-<codec>` seed drives one rule of design 9's handle
lifecycle (decision 20) to the point where breaking that rule in
src/zu_stream.c makes the target abort; fuzz/corpus/lifecycle/README.md says
which. The `seed-*` inputs are ordinary sequences to start the search from.
The layout is the one fuzz_lifecycle.c's header comment documents.
"""
import os
import sys

OUT = os.path.join(os.path.dirname(__file__), "..", "fuzz", "corpus",
                   "lifecycle")

CODECS = ["identity", "deflate-raw", "zlib", "gzip", "zstd", "lz4-frame",
          "lz4-block"]
CAN_FLUSH = {c: c != "lz4-block" for c in CODECS}

REJECT, CONCAT, SMALL_MAX, CORRUPT, JUNK, EXPECTED, OWN, X16 = (
    0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80)
RUN, FLUSH, FINISH, INJECT, RESET, NULLBUF, EMPTY, RENEW = range(8)
MEMORY, INTERNAL = 0, 1          # indices into lc_injectable[]


def op(kind, arg=0, room=31):
    """One operation: `room` is in units of 8 bytes (0..31), plus one."""
    return bytes([kind | (room << 3), arg])


def seed(codec, bits, param, enc_ops, dec_ops, payload=b"ab" * 100):
    ops = b"".join(enc_ops) + b"".join(dec_ops)
    return (bytes([CODECS.index(codec), bits, param, len(payload),
                   len(enc_ops)]) + payload + ops)


def seeds():
    out = {}
    for c in CODECS:
        # Rule 1, through a limit every codec has: max_output 21 fails the
        # first call, and every later kind of call must repeat it.
        out[f"regress-sticky-limit-{c}"] = seed(
            c, SMALL_MAX | REJECT, 20, [],
            [op(FINISH, 255), op(RUN, 10), op(FINISH), op(NULLBUF),
             op(EMPTY), op(FLUSH, 5)])
        # Rule 1 for the expected size, one byte long (#83): the failure
        # comes with the end of the stream, which used to make the next
        # call answer ZU_STREAM_END.
        out[f"regress-sticky-expected-{c}"] = seed(
            c, EXPECTED | REJECT, 2, [],
            [op(FINISH, 255), op(FINISH), op(EMPTY)])
        # Rule 3: ZU_RUN and ZU_FLUSH after ZU_FINISH are refused, in both
        # directions, and the stream still ends correctly afterwards.
        out[f"regress-finish-final-{c}"] = seed(
            c, OWN, 0,
            [op(FINISH, 200, room=0), op(RUN, 5), op(FLUSH, 5), op(EMPTY)],
            [op(FINISH, 255, room=0), op(RUN, 5), op(FLUSH, 5),
             op(FINISH)])
        # Rule 1 for an injected encoder failure, then a reset.
        out[f"regress-inject-encoder-{c}"] = seed(
            c, 0, 0,
            [op(RUN, 50), op(INJECT, INTERNAL), op(RUN, 50), op(FINISH),
             op(NULLBUF), op(FLUSH, 1), op(RESET), op(FINISH, 200)], [])
        # The same for a decoder, part-way through its stream.
        out[f"regress-inject-decoder-{c}"] = seed(
            c, REJECT | CONCAT, 0, [],
            [op(RUN, 3), op(INJECT, MEMORY), op(RUN, 3), op(FINISH),
             op(RESET), op(RUN, 255), op(FINISH)])
        # Rule 2: refused calls change nothing. A null buffer under every
        # flush, and ZU_FLUSH where the codec cannot.
        out[f"regress-refused-{c}"] = seed(
            c, REJECT, 0,
            [op(NULLBUF, 0), op(NULLBUF, 2), op(FLUSH, 10), op(RUN, 255)],
            [op(NULLBUF, 1), op(FLUSH, 4), op(NULLBUF, 2), op(RUN, 255)])
        # Free part-way and start again: the allocation balance.
        out[f"regress-renew-{c}"] = seed(
            c, CONCAT, 0,
            [op(RUN, 60), op(RENEW), op(RUN, 60), op(FLUSH, 7), op(RENEW)],
            [op(RUN, 7), op(RENEW), op(RUN, 9), op(RENEW), op(RUN, 255)])
        # A round trip of the encoder's own flushed output, sixteen-fold.
        out[f"seed-roundtrip-{c}"] = seed(
            c, OWN | X16 | REJECT | CONCAT, 0,
            [op(RUN, 70, room=3), op(FLUSH, 30, room=2), op(RUN, 100),
             op(FINISH, 255)],
            [op(RUN, 9, room=1), op(RUN, 40, room=5), op(FINISH, 255)])
    for c in CODECS:
        if c in ("identity", "lz4-block"):
            continue   # no end of their own before ZU_FINISH
        # Rule 4: the stream ends under ZU_RUN, one byte at a time, so the
        # junk arrives in a later call than the end, and is a failure.
        out[f"regress-trailing-after-end-{c}"] = seed(
            c, REJECT | JUNK, 0x55, [],
            [op(RUN, 1)] * 90 + [op(FINISH), op(EMPTY)])
    # A corrupt stream, decoded one byte at a time: the codec's own error.
    for c in CODECS[1:]:
        out[f"seed-corrupt-{c}"] = seed(
            c, CORRUPT | REJECT | CONCAT, 9, [],
            [op(RUN, 1)] * 40 + [op(FINISH, 255), op(RUN, 1)])
    return out


def main():
    check = "--check" in sys.argv[1:]
    os.makedirs(OUT, exist_ok=True)
    bad = 0
    for name, data in sorted(seeds().items()):
        path = os.path.join(OUT, name)
        if check:
            try:
                with open(path, "rb") as f:
                    same = f.read() == data
            except OSError:
                same = False
            if not same:
                print(f"differs: {path}")
                bad += 1
        else:
            with open(path, "wb") as f:
                f.write(data)
    if check and bad:
        sys.exit(1)
    print(f"{'checked' if check else 'wrote'} {len(seeds())} seeds in {OUT}")


if __name__ == "__main__":
    main()
