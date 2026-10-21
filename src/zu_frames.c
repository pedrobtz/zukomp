/* Frame continuation for the framed codecs, zstd and lz4-frame.
 *
 * The same problem gzip's members pose (design 17, and the ST_MEMBER_END
 * notes in codec_deflate.c), with a four-byte magic instead of a two-byte
 * one. Whether a following frame exists is only knowable at ZU_FINISH;
 * before that an empty buffer means "not yet". When all four magic bytes are
 * visible the decision is made without consuming anything, so src_pos stays
 * exact. When fewer are visible and more input may follow, the visible
 * bytes are held here and consumed, because the driver refills only once
 * the codec has taken everything it was offered and "wait without
 * consuming" would deadlock.
 *
 * A held prefix that a later byte then disproves stays counted as consumed,
 * so the reported cursor runs up to three bytes past the true end of the
 * stream. It happens only when the bytes after a frame straddle a buffer
 * boundary and begin like a frame magic; the classification -- trailing
 * bytes, or not -- is the same at every chunk size, as for gzip. */
#include <string.h>

#include "zu_internal.h"

void zu_int_frames_init(zu_int_frames *f, zu_int_frames_kind kind,
                        uint32_t flags)
{
    memset(f, 0, sizeof(*f));
    f->state = ZU_INT_FRAME_BODY;
    f->kind  = kind;
    f->flags = flags;
}

void zu_int_frames_ended(zu_int_frames *f)
{
    f->state  = ZU_INT_FRAME_BETWEEN;
    f->n_held = 0;
}

/* Is p[0..n), n <= 4, a prefix of a magic that starts a frame here? The
   format's own magic, or a skippable frame's 0x184D2A5? -- both libraries
   step over skippable frames themselves, so zukomp only has to let them
   through. Little-endian, as both formats store it. */
static int zu_int_frames_is_prefix(zu_int_frames_kind kind,
                                   const uint8_t *p, size_t n)
{
    static const uint8_t zstd_magic[4] = { 0x28, 0xB5, 0x2F, 0xFD };
    static const uint8_t lz4_magic[4]  = { 0x04, 0x22, 0x4D, 0x18 };
    static const uint8_t skip_tail[3]  = { 0x2A, 0x4D, 0x18 };

    const uint8_t *own = (kind == ZU_INT_FRAMES_ZSTD) ? zstd_magic : lz4_magic;
    if (n == 0) {
        return 1;
    }
    if (memcmp(p, own, n) == 0) {
        return 1;
    }
    if ((p[0] & 0xF0) != 0x50) {
        return 0;
    }
    return n == 1 || memcmp(p + 1, skip_tail, n - 1) == 0;
}

zu_status zu_int_frames_between(zu_int_frames *f, zu_buffer *buf,
                                zu_flush flush, zu_int_frames_feed feed,
                                void *ctx)
{
    if (f->state == ZU_INT_FRAME_DONE) {
        return ZU_STREAM_END;
    }
    if (f->state == ZU_INT_FRAME_BODY) {
        return ZU_OK;
    }

    const int concat = (f->flags & ZU_DEC_CONCAT_MEMBERS) != 0;
    size_t avail_in = buf->src_size - buf->src_pos;

    if (!concat) {
        /* Leave whatever follows unconsumed: src_pos is exact and the
           driver applies the trailing policy. */
        f->state = ZU_INT_FRAME_DONE;
        return ZU_STREAM_END;
    }

    if (f->n_held == 0) {
        if (avail_in == 0) {
            if (flush != ZU_FINISH) {
                return ZU_NEED_INPUT;   /* another frame may follow */
            }
            f->state = ZU_INT_FRAME_DONE;
            return ZU_STREAM_END;
        }
        const size_t k = (avail_in < 4) ? avail_in : 4;
        const uint8_t *p = buf->src + buf->src_pos;
        if (!zu_int_frames_is_prefix(f->kind, p, k)) {
            f->state = ZU_INT_FRAME_DONE;
            return ZU_STREAM_END;       /* unconsumed: src_pos exact */
        }
        if (k == 4) {
            /* Confirmed and still unconsumed: the library reads the magic
               itself as the start of the next frame. */
            f->state = ZU_INT_FRAME_BODY;
            return ZU_OK;
        }
        if (flush == ZU_FINISH) {
            /* Nothing more is coming, so fewer than four bytes cannot be a
               frame. Unconsumed, for the driver's policy. */
            f->state = ZU_INT_FRAME_DONE;
            return ZU_STREAM_END;
        }
        memcpy(f->held, p, k);
        f->n_held = k;
        buf->src_pos += k;
        return ZU_NEED_INPUT;
    }

    /* Holding a prefix from an earlier buffer: extend it a byte at a time,
       consuming only bytes that keep it a prefix. */
    while (f->n_held < 4 && avail_in > 0) {
        f->held[f->n_held] = buf->src[buf->src_pos];
        if (!zu_int_frames_is_prefix(f->kind, f->held, f->n_held + 1)) {
            /* Disproved. The mismatching byte is unconsumed, so the driver
               sees a tail and applies the policy to it. */
            f->n_held = 0;
            f->state  = ZU_INT_FRAME_DONE;
            return ZU_STREAM_END;
        }
        f->n_held++;
        buf->src_pos++;
        avail_in--;
    }
    if (f->n_held == 4) {
        zu_status st = feed(ctx, f->held, 4);
        f->n_held = 0;
        if (st != ZU_OK) {
            return st;
        }
        f->state = ZU_INT_FRAME_BODY;
        return ZU_OK;
    }
    if (flush != ZU_FINISH) {
        return ZU_NEED_INPUT;
    }
    /* Input ended on a held prefix. src_pos can no longer express those
       bytes, so the policy is applied here -- otherwise the same tail would
       be rejected at a large chunk size and silently accepted when it
       straddled a buffer boundary. */
    f->n_held = 0;
    f->state  = ZU_INT_FRAME_DONE;
    if (f->flags & ZU_DEC_REJECT_TRAILING) {
        return ZU_ERR_TRAILING;
    }
    return ZU_STREAM_END;
}

/* A framed decoder's library took no input and wrote no output. Neither
   libzstd nor liblz4 says whether it is waiting on input or on output room,
   so the answer comes from what it was offered:
 *
 *   no room, input left    output-starved: it holds decoded bytes it cannot
 *                          write, or will not read ahead of them. Under a
 *                          cap this is what the driver turns into the limit.
 *   no room, no input      ambiguous, and the answer matters both ways. A
 *                          stream ending exactly at max_output (or at the
 *                          expected size) still has its checksum to read
 *                          with no output room left, and libzstd may hold
 *                          its last input byte back until the output is
 *                          flushed -- so "output" here reported valid
 *                          streams as over the limit. A stream cut short
 *                          after its last output byte is the same picture,
 *                          and "output" there reported truncation as a
 *                          limit. Before ZU_FINISH, more input settles it,
 *                          so ask for input. At ZU_FINISH, probe: ask the
 *                          library for a single byte into scratch. A byte
 *                          means output really is pending (the byte is owed
 *                          to the caller's next window); the frame ending
 *                          means it was only the trailer; nothing at all
 *                          means the frame is cut short.
 *   room, no input         input-starved. At ZU_FINISH that is a frame cut
 *                          short: the library parses the frame structure,
 *                          so it can tell "needs more" from "is wrong" --
 *                          the distinction DEFLATE's body cannot draw
 *                          (design 7).
 *   room and input         the library refused both; never expected.
 *
 * ZU_OK means "the frame ended; carry on", for the codec's loop. */
zu_status zu_int_frames_stalled(zu_int_frames *f, size_t avail_in,
                                size_t avail_out, zu_flush flush,
                                zu_int_frames_probe probe, void *ctx)
{
    if (avail_out == 0) {
        if (avail_in > 0) {
            return ZU_NEED_OUTPUT;
        }
        if (flush != ZU_FINISH) {
            return ZU_NEED_INPUT;
        }
        size_t written = 0;
        int    frame_end = 0;
        zu_status st = probe(ctx, &f->owed, &written, &frame_end);
        if (st != ZU_OK) {
            return st;
        }
        if (frame_end) {
            zu_int_frames_ended(f);
        }
        if (written > 0) {
            f->has_owed = 1;
            return ZU_NEED_OUTPUT;
        }
        return frame_end ? ZU_OK : ZU_ERR_TRUNCATED;
    }
    if (avail_in == 0) {
        return (flush == ZU_FINISH) ? ZU_ERR_TRUNCATED : ZU_NEED_INPUT;
    }
    return ZU_ERR_INTERNAL;
}

int zu_int_frames_pay(zu_int_frames *f, zu_buffer *buf)
{
    if (!f->has_owed) {
        return 1;
    }
    if (buf->dst_pos == buf->dst_size) {
        return 0;
    }
    buf->dst[buf->dst_pos++] = f->owed;
    f->has_owed = 0;
    return 1;
}
