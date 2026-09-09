/* The DEFLATE family: `deflate-raw` (RFC 1951) and `zlib` (RFC 1950).
 *
 * One engine, two vtables. The engine drives miniz's streaming API at
 * window_bits = -15, i.e. headerless DEFLATE, and the zlib wrapper -- the
 * two-byte header and the Adler-32 trailer -- is implemented here rather
 * than delegated to miniz.
 *
 * That is a deliberate cost. Letting miniz own the wrapper would be less
 * code, but mz_inflate() reports a corrupt Adler-32 and corrupt compressed
 * data with the same MZ_DATA_ERROR, and design 24 criterion 6 requires
 * every checksum failure to surface as zukomp_checksum_error, distinctly.
 * Owning the wrapper also means Stage 7's gzip wrapper reuses this
 * structure instead of inventing a parallel one.
 *
 * Every state machine here has to survive one-byte input and output
 * buffers, because that is what the chunk-boundary sweeps do to it. Hence
 * the header and trailer are themselves streamed, byte at a time if that is
 * all the room there is. */
#include <string.h>

#include "miniz.h"
#include "zu_internal.h"

typedef enum {
    WRAP_NONE = 0,     /* deflate-raw */
    WRAP_ZLIB = 1,
    WRAP_GZIP = 2
} wrap_kind;

/* Where the wrapper is, independent of where the DEFLATE engine is. */
typedef enum {
    ST_HEADER = 0,     /* emitting or consuming the wrapper header */
    ST_BODY,           /* inside the DEFLATE stream */
    ST_TRAILER,        /* emitting or consuming the checksum trailer */
    ST_MEMBER_END,     /* a member finished; is another one coming? */
    ST_DONE
} wrap_state;

typedef struct {
    mz_stream   mz;
    int         mz_live;      /* has deflateInit/inflateInit been called? */
    int         mz_called;    /* has mz_deflate/mz_inflate been called since
                                 the last init or reset? */
    int         encoder;
    wrap_kind   wrap;
    wrap_state  state;
    int32_t     level;

    uint8_t     hdr[ZU_INT_GZIP_HEADER_LEN];
    size_t      hdr_len;
    size_t      hdr_pos;

    uint8_t     tail[8];      /* zlib: 4 (Adler-32); gzip: 8 (CRC-32 + ISIZE) */
    size_t      tail_len;
    size_t      tail_pos;

    mz_ulong    checksum;     /* Adler-32 or CRC-32 over the *uncompressed*
                                 bytes, depending on the wrapper */
    uint64_t    uncompressed; /* for gzip's ISIZE */
    zu_int_gzip_header gz;    /* decoder-side gzip header parser */
    uint32_t    dec_flags;    /* ZU_DEC_* from the decoder options */
} deflate_state;

/* -- status mapping ------------------------------------------------------ */

static zu_status zu_int_from_mz(int mz)
{
    switch (mz) {
    case MZ_OK:            return ZU_OK;
    case MZ_STREAM_END:    return ZU_STREAM_END;
    case MZ_BUF_ERROR:     return ZU_OK;   /* "no progress"; the caller decides */
    case MZ_DATA_ERROR:    return ZU_ERR_INVALID_DATA;
    case MZ_MEM_ERROR:     return ZU_ERR_MEMORY;
    case MZ_PARAM_ERROR:   return ZU_ERR_INVALID_ARGUMENT;
    case MZ_STREAM_ERROR:  return ZU_ERR_INVALID_DATA;
    case MZ_NEED_DICT:     return ZU_ERR_UNSUPPORTED;  /* preset dictionaries */
    default:               return ZU_ERR_INTERNAL;
    }
}

static int zu_int_to_mz_flush(zu_flush f)
{
    switch (f) {
    case ZU_FLUSH:  return MZ_SYNC_FLUSH;
    case ZU_FINISH: return MZ_FINISH;
    case ZU_RUN:    default: return MZ_NO_FLUSH;
    }
}

/* -- the zlib wrapper, byte for byte ------------------------------------- */

/* RFC 1950 section 2.2. CINFO 7 is a 32K window, which is what miniz uses;
   FLEVEL is advisory only, and FCHECK exists to make the first two bytes a
   multiple of 31. */
static void zu_int_zlib_header(uint8_t out[2], int32_t level)
{
    const uint8_t cmf = 0x78;          /* CM = 8 (deflate), CINFO = 7 (32K) */
    uint8_t flevel;
    if (level <= 1)      { flevel = 0; }
    else if (level <= 5) { flevel = 1; }
    else if (level == 6) { flevel = 2; }
    else                 { flevel = 3; }

    uint8_t flg = (uint8_t) (flevel << 6);
    uint16_t check = (uint16_t) (((uint16_t) cmf << 8) | flg);
    uint8_t rem = (uint8_t) (check % 31);
    if (rem != 0) {
        flg = (uint8_t) (flg + (31 - rem));
    }
    out[0] = cmf;
    out[1] = flg;
}

static zu_status zu_int_zlib_check_header(const uint8_t hdr[2])
{
    const uint8_t cmf = hdr[0], flg = hdr[1];
    if ((cmf & 0x0F) != 8) {
        return ZU_ERR_INVALID_DATA;                 /* CM must be DEFLATE */
    }
    if (((cmf >> 4) & 0x0F) > 7) {
        return ZU_ERR_INVALID_DATA;                 /* window > 32K */
    }
    if (((((uint16_t) cmf) << 8) | flg) % 31 != 0) {
        return ZU_ERR_INVALID_DATA;                 /* FCHECK */
    }
    if (flg & 0x20) {
        /* A preset dictionary we do not have. Unsupported rather than
           invalid: the stream is well-formed, we just cannot decode it. */
        return ZU_ERR_UNSUPPORTED;
    }
    return ZU_OK;
}

/* zlib carries Adler-32, gzip carries CRC-32. Both run over the
   uncompressed bytes, so only the function differs. */
static mz_ulong zu_int_checksum_init(wrap_kind w)
{
    return (w == WRAP_GZIP) ? mz_crc32(0, NULL, 0) : mz_adler32(0, NULL, 0);
}

static mz_ulong zu_int_checksum_update(wrap_kind w, mz_ulong v,
                                       const uint8_t *p, size_t n)
{
    if (n == 0) {
        return v;
    }
    return (w == WRAP_GZIP) ? mz_crc32(v, p, n) : mz_adler32(v, p, n);
}

/* -- lifecycle ----------------------------------------------------------- */

static zu_status zu_int_deflate_new(void **out, wrap_kind wrap, int encoder,
                                    int32_t level, uint32_t dec_flags)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    deflate_state *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return ZU_ERR_MEMORY;
    }
    s->encoder   = encoder;
    s->wrap      = wrap;
    s->dec_flags = dec_flags;
    s->level   = (level == ZU_LEVEL_DEFAULT) ? MZ_DEFAULT_LEVEL : level;
    s->checksum = zu_int_checksum_init(wrap);

    int mz;
    if (encoder) {
        /* Negative window_bits selects headerless DEFLATE. The wrapper, if
           any, is ours. */
        mz = mz_deflateInit2(&s->mz, (int) s->level, MZ_DEFLATED,
                             -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
    } else {
        mz = mz_inflateInit2(&s->mz, -MZ_DEFAULT_WINDOW_BITS);
    }
    if (mz != MZ_OK) {
        free(s);
        return zu_int_from_mz(mz);
    }
    s->mz_live = 1;

    switch (wrap) {
    case WRAP_ZLIB:
        s->state    = ST_HEADER;
        s->hdr_len  = 2;
        s->tail_len = 4;
        if (encoder) {
            zu_int_zlib_header(s->hdr, s->level);
        }
        break;
    case WRAP_GZIP:
        s->state    = ST_HEADER;
        s->hdr_len  = ZU_INT_GZIP_HEADER_LEN;
        s->tail_len = 8;
        if (encoder) {
            zu_int_gzip_write_header(s->hdr);
        } else {
            zu_int_gzip_header_init(&s->gz);
        }
        break;
    case WRAP_NONE:
    default:
        s->state    = ST_BODY;
        s->tail_len = 0;
        break;
    }
    *out = s;
    return ZU_OK;
}

static zu_status deflate_raw_encoder_new(void **st, const zu_encoder_opts *o)
{ return zu_int_deflate_new(st, WRAP_NONE, 1, o->level, 0); }
static zu_status deflate_raw_decoder_new(void **st, const zu_decoder_opts *o)
{ return zu_int_deflate_new(st, WRAP_NONE, 0, ZU_LEVEL_DEFAULT, o->flags); }
static zu_status zlib_encoder_new(void **st, const zu_encoder_opts *o)
{ return zu_int_deflate_new(st, WRAP_ZLIB, 1, o->level, 0); }
static zu_status zlib_decoder_new(void **st, const zu_decoder_opts *o)
{ return zu_int_deflate_new(st, WRAP_ZLIB, 0, ZU_LEVEL_DEFAULT, o->flags); }
static zu_status gzip_encoder_new(void **st, const zu_encoder_opts *o)
{ return zu_int_deflate_new(st, WRAP_GZIP, 1, o->level, 0); }
static zu_status gzip_decoder_new(void **st, const zu_decoder_opts *o)
{ return zu_int_deflate_new(st, WRAP_GZIP, 0, ZU_LEVEL_DEFAULT, o->flags); }

static void deflate_free(void *st)
{
    deflate_state *s = st;
    if (s == NULL) {
        return;
    }
    if (s->mz_live) {
        if (s->encoder) { mz_deflateEnd(&s->mz); } else { mz_inflateEnd(&s->mz); }
    }
    free(s);
}

static zu_status zu_int_deflate_reset(void *st, int32_t level)
{
    deflate_state *s = st;
    if (s == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    /* ZU_LEVEL_DEFAULT on a reset means "whatever this stream was built
       with", not "the codec default": a reset re-parameterises one stream,
       and saying nothing must change nothing. */
    const int32_t want = (s->encoder && level != ZU_LEVEL_DEFAULT)
                       ? level : s->level;

    int mz;
    if (s->encoder && want != s->level) {
        /* mz_deflateReset() re-runs tdefl_init() with the flags baked in at
           mz_deflateInit2() time, so it does *not* re-apply the level: only
           a full re-init does. Resetting alone would leave the payload at
           the old level while zu_int_zlib_header() advertises the new one
           in FLEVEL -- output that is silently not what was asked for. */
        mz_deflateEnd(&s->mz);
        memset(&s->mz, 0, sizeof(s->mz));
        s->mz_live = 0;
        mz = mz_deflateInit2(&s->mz, (int) want, MZ_DEFLATED,
                             -MZ_DEFAULT_WINDOW_BITS, 9, MZ_DEFAULT_STRATEGY);
        if (mz != MZ_OK) {
            return zu_int_from_mz(mz);
        }
        s->mz_live = 1;
        s->level   = want;
    } else {
        mz = s->encoder ? mz_deflateReset(&s->mz) : mz_inflateReset(&s->mz);
        if (mz != MZ_OK) {
            return zu_int_from_mz(mz);
        }
    }
    s->mz_called    = 0;
    s->checksum     = zu_int_checksum_init(s->wrap);
    s->uncompressed = 0;
    s->hdr_pos      = 0;
    s->tail_pos     = 0;
    switch (s->wrap) {
    case WRAP_ZLIB:
        s->state = ST_HEADER;
        if (s->encoder) { zu_int_zlib_header(s->hdr, s->level); }
        break;
    case WRAP_GZIP:
        s->state = ST_HEADER;
        if (s->encoder) { zu_int_gzip_write_header(s->hdr); }
        else            { zu_int_gzip_header_init(&s->gz); }
        break;
    case WRAP_NONE:
    default:
        s->state = ST_BODY;
        break;
    }
    return ZU_OK;
}

static zu_status deflate_encoder_reset(void *st, const zu_encoder_opts *o)
{ return zu_int_deflate_reset(st, o != NULL ? o->level : ZU_LEVEL_DEFAULT); }
static zu_status deflate_decoder_reset(void *st, const zu_decoder_opts *o)
{
    deflate_state *s = st;
    if (s != NULL && o != NULL) { s->dec_flags = o->flags; }
    return zu_int_deflate_reset(st, ZU_LEVEL_DEFAULT);
}

/* -- process ------------------------------------------------------------- */

/* Both directions share this shape: move wrapper bytes when the wrapper
   needs moving, otherwise turn the crank on miniz, and keep going until
   neither can make progress. Returning the right "why did you stop" status
   matters more than it looks: get it wrong and the driver either spins or
   stops early with a silently truncated result. */
static zu_status deflate_process(void *st, zu_buffer *buf, zu_flush flush)
{
    deflate_state *s = st;
    if (s == NULL || buf == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }

    for (;;) {
        size_t avail_in  = buf->src_size - buf->src_pos;
        size_t avail_out = buf->dst_size - buf->dst_pos;

        if (s->state == ST_DONE) {
            return ZU_STREAM_END;
        }

        if (s->state == ST_HEADER) {
            if (s->encoder) {
                if (s->hdr_pos < s->hdr_len) {
                    if (avail_out == 0) { return ZU_NEED_OUTPUT; }
                    buf->dst[buf->dst_pos++] = s->hdr[s->hdr_pos++];
                    continue;
                }
            } else if (s->wrap == WRAP_GZIP) {
                /* Variable length, so the parser -- not a byte count --
                   decides when the header is finished. */
                if (avail_in == 0) {
                    return (flush == ZU_FINISH) ? ZU_ERR_TRUNCATED
                                                : ZU_NEED_INPUT;
                }
                int done = 0;
                zu_status hs = zu_int_gzip_header_feed(
                    &s->gz, buf->src[buf->src_pos++], &done);
                if (hs != ZU_OK) { return hs; }
                if (!done) { continue; }
            } else {
                if (s->hdr_pos < s->hdr_len) {
                    if (avail_in == 0) {
                        return (flush == ZU_FINISH) ? ZU_ERR_TRUNCATED
                                                    : ZU_NEED_INPUT;
                    }
                    s->hdr[s->hdr_pos++] = buf->src[buf->src_pos++];
                    continue;
                }
                zu_status hs = zu_int_zlib_check_header(s->hdr);
                if (hs != ZU_OK) { return hs; }
            }
            s->state = ST_BODY;
            continue;
        }

        if (s->state == ST_BODY) {
            /* An encoder with nowhere to put a byte is always output-starved
               -- DEFLATE emits at least the block header -- and mz_deflate()
               rejects a zero-size output buffer outright. A *decoder* is a
               different question: a stream whose remaining output is empty
               still has to reach MZ_STREAM_END before the trailer can be
               read, so asking miniz with no room is how "finished" is told
               apart from "would have exceeded the cap". Returning
               ZU_NEED_OUTPUT unconditionally here reported decoding an empty
               payload into a zero-capacity sink as an output-limit error. */
            if (avail_out == 0 && s->encoder) {
                return ZU_NEED_OUTPUT;
            }

            /* Never hand miniz a NULL next_in, even with avail_in == 0:
               tinfl computes `pIn_buf_next + *pIn_buf_size` unguarded (it
               does guard the output pointer), which UBSan flags as
               arithmetic on a null pointer. Pointing at a static byte it
               is not permitted to read keeps the fix on our side of the
               boundary rather than adding a second vendored patch. */
            static const unsigned char zu_int_no_input[1] = { 0 };
            const unsigned char *next_in =
                (buf->src == NULL) ? zu_int_no_input
                                   : (const unsigned char *) (buf->src + buf->src_pos);

            s->mz.next_in   = next_in;
            s->mz.avail_in  = (unsigned int) ((avail_in > 0x7FFFFFFFu)
                                              ? 0x7FFFFFFFu : avail_in);
            /* Same reasoning as next_in, from the other side: mz_inflate()
               memcpy()s into next_out even when it is copying zero bytes,
               and a NULL destination is undefined behaviour UBSan flags. */
            static unsigned char zu_int_no_output[1];
            unsigned char *next_out =
                (buf->dst == NULL) ? zu_int_no_output
                                   : (unsigned char *) zu_int_at(buf->dst, buf->dst_pos);

            s->mz.next_out  = next_out;
            s->mz.avail_out = (unsigned int) ((avail_out > 0x7FFFFFFFu)
                                              ? 0x7FFFFFFFu : avail_out);

            const unsigned int in_before  = s->mz.avail_in;
            const unsigned int out_before = s->mz.avail_out;

            int mz_flush = zu_int_to_mz_flush(flush);
            if (!s->encoder && !s->mz_called && mz_flush == MZ_FINISH) {
                /* mz_inflate() has a fast path for MZ_FINISH on the *first*
                   call, and it assumes the output buffer is large enough for
                   the entire result. When it is not -- a zero-byte sink, or
                   one sized from a wrong Content-Length -- miniz does not
                   merely return MZ_BUF_ERROR, it marks the stream
                   permanently failed, so the next call reports MZ_DATA_ERROR
                   and a capacity problem is misreported as corrupt input.
                   MZ_SYNC_FLUSH takes the ordinary streaming path instead:
                   it decompresses through miniz's own dictionary, still
                   reports MZ_STREAM_END when nothing is left to produce, and
                   is the path every call after the first already uses. */
                mz_flush = MZ_SYNC_FLUSH;
            }

            int mz = s->encoder ? mz_deflate(&s->mz, mz_flush)
                                : mz_inflate(&s->mz, mz_flush);
            s->mz_called = 1;

            const size_t used     = in_before - s->mz.avail_in;
            const size_t produced = out_before - s->mz.avail_out;

            /* The checksum is over uncompressed bytes: that is the input
               when encoding and the output when decoding. */
            if (s->wrap != WRAP_NONE) {
                if (s->encoder) {
                    s->checksum = zu_int_checksum_update(
                        s->wrap, s->checksum, zu_int_cat(buf->src, buf->src_pos), used);
                    s->uncompressed += (uint64_t) used;
                } else {
                    s->checksum = zu_int_checksum_update(
                        s->wrap, s->checksum, zu_int_at(buf->dst, buf->dst_pos), produced);
                    s->uncompressed += (uint64_t) produced;
                }
            }
            buf->src_pos += used;
            buf->dst_pos += produced;

            if (mz == MZ_STREAM_END) {
                if (s->wrap != WRAP_NONE) {
                    if (s->encoder) {
                        uint32_t c = (uint32_t) s->checksum;
                        if (s->wrap == WRAP_ZLIB) {
                            /* RFC 1950: Adler-32, big-endian. */
                            s->tail[0] = (uint8_t) (c >> 24);
                            s->tail[1] = (uint8_t) (c >> 16);
                            s->tail[2] = (uint8_t) (c >> 8);
                            s->tail[3] = (uint8_t) c;
                        } else {
                            /* RFC 1952: CRC-32 then ISIZE, both
                               little-endian. ISIZE is the input length
                               modulo 2^32, by definition, not an error for
                               inputs above 4 GiB. */
                            uint32_t isize = (uint32_t) (s->uncompressed & 0xFFFFFFFFu);
                            s->tail[0] = (uint8_t) c;
                            s->tail[1] = (uint8_t) (c >> 8);
                            s->tail[2] = (uint8_t) (c >> 16);
                            s->tail[3] = (uint8_t) (c >> 24);
                            s->tail[4] = (uint8_t) isize;
                            s->tail[5] = (uint8_t) (isize >> 8);
                            s->tail[6] = (uint8_t) (isize >> 16);
                            s->tail[7] = (uint8_t) (isize >> 24);
                        }
                    }
                    s->tail_pos = 0;
                    s->state = ST_TRAILER;
                    continue;
                }
                s->state = ST_DONE;
                return ZU_STREAM_END;
            }
            if (mz != MZ_OK && mz != MZ_BUF_ERROR) {
                return zu_int_from_mz(mz);
            }
            if (used == 0 && produced == 0) {
                /* miniz could not move. Decide why, so the driver knows
                   whether to refill, drain, or give up. */
                if (avail_out == 0) {
                    /* Nowhere to write is not the same as nothing to read:
                       reporting ZU_ERR_TRUNCATED below would blame the input
                       for a stall the output caused. */
                    return ZU_NEED_OUTPUT;
                }
                if (buf->src_pos == buf->src_size) {
                    if (flush != ZU_FINISH) { return ZU_NEED_INPUT; }
                    /* Told there is no more input, yet the stream has not
                       ended: the input is short. Reporting success here is
                       exactly the silent-truncation bug design 24 criterion
                       5 exists to prevent. */
                    return s->encoder ? ZU_ERR_INTERNAL : ZU_ERR_TRUNCATED;
                }
                return ZU_NEED_OUTPUT;
            }
            continue;
        }

        if (s->state == ST_MEMBER_END) {
            /* One member is complete. RFC 1952 permits another to follow
               immediately, and standard tools produce exactly that, so
               "the trailer ended" is not the same question as "the stream
               ended". Retrofitting this into a finished state machine is
               why design 17 insisted on building it in.
             *
             * Deciding requires knowing whether more input exists, which
             * during streaming is only knowable at ZU_FINISH -- before
             * that, an empty buffer means "not yet", not "no more". */
            const int concat = (s->wrap == WRAP_GZIP) &&
                               (s->dec_flags & ZU_DEC_CONCAT_MEMBERS) &&
                               !s->encoder;

            if (!concat || avail_in == 0) {
                if (avail_in == 0 && !s->encoder && concat &&
                    flush != ZU_FINISH) {
                    return ZU_NEED_INPUT;   /* another member may follow */
                }
                s->state = ST_DONE;
                return ZU_STREAM_END;
            }

            /* Bytes remain. A following member must start with the gzip
               magic; anything else is trailing junk, and saying so is far
               more useful than reporting a malformed member header. */
            if (buf->src[buf->src_pos] != 0x1F) {
                s->state = ST_DONE;
                return ZU_STREAM_END;       /* the driver applies the
                                               trailing-bytes policy */
            }

            zu_status rs = zu_int_deflate_reset(s, ZU_LEVEL_DEFAULT);
            if (rs != ZU_OK) { return rs; }
            continue;
        }

        /* ST_TRAILER */
        if (s->encoder) {
            if (s->tail_pos < s->tail_len) {
                if (avail_out == 0) { return ZU_NEED_OUTPUT; }
                buf->dst[buf->dst_pos++] = s->tail[s->tail_pos++];
                continue;
            }
            s->state = ST_DONE;
            return ZU_STREAM_END;
        }

        if (s->tail_pos < s->tail_len) {
            if (avail_in == 0) {
                return (flush == ZU_FINISH) ? ZU_ERR_TRUNCATED : ZU_NEED_INPUT;
            }
            s->tail[s->tail_pos++] = buf->src[buf->src_pos++];
            continue;
        }
        if (s->wrap == WRAP_ZLIB) {
            uint32_t want_adler = ((uint32_t) s->tail[0] << 24) |
                                  ((uint32_t) s->tail[1] << 16) |
                                  ((uint32_t) s->tail[2] << 8)  |
                                  ((uint32_t) s->tail[3]);
            if (want_adler != (uint32_t) s->checksum) {
                return ZU_ERR_CHECKSUM;
            }
        } else {
            uint32_t want_crc = ((uint32_t) s->tail[0])        |
                                ((uint32_t) s->tail[1] << 8)   |
                                ((uint32_t) s->tail[2] << 16)  |
                                ((uint32_t) s->tail[3] << 24);
            uint32_t want_isize = ((uint32_t) s->tail[4])       |
                                  ((uint32_t) s->tail[5] << 8)  |
                                  ((uint32_t) s->tail[6] << 16) |
                                  ((uint32_t) s->tail[7] << 24);
            if (want_crc != (uint32_t) s->checksum) {
                return ZU_ERR_CHECKSUM;
            }
            /* ISIZE is validated against what we actually produced. It is
               never used to size a buffer -- design 20 -- so a lie here
               costs a rejected stream and nothing more. */
            if (want_isize != (uint32_t) (s->uncompressed & 0xFFFFFFFFu)) {
                return ZU_ERR_CHECKSUM;
            }
        }
        s->state = ST_MEMBER_END;
        continue;
    }
}

/* -- bound --------------------------------------------------------------- */

/* Worst case is incompressible input, which DEFLATE emits as stored blocks:
   five bytes of overhead per 65535-byte block. n/64 is comfortably more than
   5*(n/65535) and keeps the arithmetic obvious. */
static zu_status zu_int_deflate_bound(size_t n, size_t wrapper, size_t *out)
{
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    size_t total;
    zu_status st = zu_int_add(n, n / 64, &total);
    if (st != ZU_OK) { return st; }
    st = zu_int_add(total, 64 + wrapper, &total);
    if (st != ZU_OK) { return st; }
    *out = total;
    return ZU_OK;
}

static zu_status deflate_raw_bound(int32_t level, size_t n, size_t *out)
{ (void) level; return zu_int_deflate_bound(n, 0, out); }
static zu_status zlib_bound(int32_t level, size_t n, size_t *out)
{ (void) level; return zu_int_deflate_bound(n, 6, out); }
static zu_status gzip_bound(int32_t level, size_t n, size_t *out)
{ (void) level; return zu_int_deflate_bound(n, ZU_INT_GZIP_HEADER_LEN + 8, out); }

/* -- vtables ------------------------------------------------------------- */

const zu_codec_vtable zu_int_codec_deflate_raw = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_DEFLATE_RAW,
    "deflate-raw",
    NULL,                 /* not an HTTP content-coding: `deflate` means zlib */
    "zukomp",
    0, 9, MZ_DEFAULT_LEVEL,
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    NULL, 0, 0, NULL,     /* headerless, so never detectable (design 5) */
    deflate_raw_encoder_new, deflate_process, deflate_encoder_reset, deflate_free,
    deflate_raw_decoder_new, deflate_process, deflate_decoder_reset, deflate_free,
    deflate_raw_bound
};

/* zlib's header is a validity predicate rather than a constant, so it gets a
   sniff callback instead of magic bytes -- and the registry tries predicate
   sniffers last, because a weak check accepts arbitrary bytes happily. */
static int zlib_sniff(const uint8_t *buf, size_t n)
{
    if (n < 2) { return 0; }
    /* Structural check only. A stream with FDICT set *is* zlib, we simply
       cannot decode it without the dictionary -- so detecting it and then
       failing with ZU_ERR_UNSUPPORTED tells the caller far more than
       refusing to recognise it at all. */
    zu_status st = zu_int_zlib_check_header(buf);
    return st == ZU_OK || st == ZU_ERR_UNSUPPORTED;
}

const zu_codec_vtable zu_int_codec_zlib = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_ZLIB,
    "zlib",
    "deflate",            /* design 3: the HTTP token `deflate` means zlib */
    "zukomp",
    0, 9, MZ_DEFAULT_LEVEL,
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    NULL, 0, 0, zlib_sniff,
    zlib_encoder_new, deflate_process, deflate_encoder_reset, deflate_free,
    zlib_decoder_new, deflate_process, deflate_decoder_reset, deflate_free,
    zlib_bound
};

/* Unlike zlib's, gzip's header starts with two constant bytes, so it is
   sniffable by magic rather than by predicate -- and magic is tried first,
   because a predicate check accepts arbitrary bytes far too readily. */
static const uint8_t zu_int_gzip_magic[2] = { 0x1F, 0x8B };

const zu_codec_vtable zu_int_codec_gzip = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_GZIP,
    "gzip",
    "gzip",
    "zukomp",
    0, 9, MZ_DEFAULT_LEVEL,
    ZU_CAN_ENCODE | ZU_CAN_DECODE | ZU_CAN_FLUSH,
    zu_int_gzip_magic, sizeof(zu_int_gzip_magic), 0, NULL,
    gzip_encoder_new, deflate_process, deflate_encoder_reset, deflate_free,
    gzip_decoder_new, deflate_process, deflate_decoder_reset, deflate_free,
    gzip_bound
};
