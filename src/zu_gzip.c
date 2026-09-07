/* RFC 1952: the gzip wrapper.
 *
 * miniz supplies no gzip mode -- its window_bits accepts only +/-15, zlib or
 * raw -- which was verified against the pinned release at vendoring time and
 * recorded in design 22. So this wrapper is ours, and the payload is
 * delegated to the Stage 6 DEFLATE engine.
 *
 * This file is deliberately small and separate. The header is the single
 * most likely place in the package for a parser bug: it is variable length,
 * it has four optional fields, two of them NUL-terminated and
 * attacker-controlled, and it is the first thing that touches untrusted
 * bytes. Keeping it isolated is what makes it separately fuzzable in
 * Stage 14.
 *
 * The parser is fed one byte at a time. A header field can straddle any
 * number of process() calls -- the chunk sweeps run at one byte per call --
 * and byte-at-a-time is the only shape where that is obviously correct
 * rather than merely tested. Headers are tens of bytes, so the cost is
 * irrelevant. */
#include <string.h>

#include "miniz.h"
#include "zu_internal.h"

/* FLG bits (RFC 1952 section 2.3.1). Bits 5-7 are reserved and MUST be zero;
   a stream that sets them is malformed, not merely unusual. */
#define GZ_FTEXT     0x01
#define GZ_FHCRC     0x02
#define GZ_FEXTRA    0x04
#define GZ_FNAME     0x08
#define GZ_FCOMMENT  0x10
#define GZ_RESERVED  0xE0

void zu_int_gzip_write_header(uint8_t out[ZU_INT_GZIP_HEADER_LEN])
{
    /* Deterministic by construction: no mtime, no name, no comment, OS
       "unknown". Two calls with the same input must produce identical bytes
       (design 18), which rules out the timestamp a normal gzip writes. */
    out[0] = 0x1F;
    out[1] = 0x8B;
    out[2] = 8;      /* CM: DEFLATE */
    out[3] = 0;      /* FLG: no optional fields */
    out[4] = 0;      /* MTIME = 0 */
    out[5] = 0;
    out[6] = 0;
    out[7] = 0;
    out[8] = 0;      /* XFL */
    out[9] = 255;    /* OS: unknown */
}

void zu_int_gzip_header_init(zu_int_gzip_header *h)
{
    memset(h, 0, sizeof(*h));
    h->state = ZU_INT_GZ_FIXED;
    h->crc = mz_crc32(0, NULL, 0);
}

/* Consumes one header byte. Sets *done when the header is complete; the
   caller then moves on to the DEFLATE payload. Returns an error the moment
   a field is invalid, so nothing downstream ever sees a half-validated
   header. */
zu_status zu_int_gzip_header_feed(zu_int_gzip_header *h, uint8_t byte, int *done)
{
    *done = 0;

    /* The FHCRC covers every header byte before the checksum itself. */
    if (h->state != ZU_INT_GZ_HCRC) {
        h->crc = mz_crc32(h->crc, &byte, 1);
    }

    switch (h->state) {
    case ZU_INT_GZ_FIXED:
        h->fixed[h->fixed_pos++] = byte;
        if (h->fixed_pos == 1 && byte != 0x1F) {
            return ZU_ERR_INVALID_DATA;
        }
        if (h->fixed_pos == 2 && byte != 0x8B) {
            return ZU_ERR_INVALID_DATA;
        }
        if (h->fixed_pos == 3 && byte != 8) {
            return ZU_ERR_INVALID_DATA;      /* only DEFLATE is defined */
        }
        if (h->fixed_pos == 4) {
            if (byte & GZ_RESERVED) {
                return ZU_ERR_INVALID_DATA;
            }
            h->flg = byte;
        }
        if (h->fixed_pos < ZU_INT_GZIP_HEADER_LEN) {
            return ZU_OK;
        }
        h->state = (h->flg & GZ_FEXTRA) ? ZU_INT_GZ_EXTRA_LEN
                 : (h->flg & GZ_FNAME) ? ZU_INT_GZ_NAME
                 : (h->flg & GZ_FCOMMENT) ? ZU_INT_GZ_COMMENT
                 : (h->flg & GZ_FHCRC) ? ZU_INT_GZ_HCRC
                 : ZU_INT_GZ_DONE;
        break;

    case ZU_INT_GZ_EXTRA_LEN:
        /* XLEN is little-endian, and is a length we must *skip*, never a
           size we allocate from -- design 20's rule about never trusting a
           length claimed by the input. */
        h->xlen |= (uint16_t) ((uint16_t) byte << (8 * h->xlen_pos));
        if (++h->xlen_pos < 2) {
            return ZU_OK;
        }
        h->state = (h->xlen > 0) ? ZU_INT_GZ_EXTRA
                 : (h->flg & GZ_FNAME) ? ZU_INT_GZ_NAME
                 : (h->flg & GZ_FCOMMENT) ? ZU_INT_GZ_COMMENT
                 : (h->flg & GZ_FHCRC) ? ZU_INT_GZ_HCRC
                 : ZU_INT_GZ_DONE;
        break;

    case ZU_INT_GZ_EXTRA:
        if (++h->xpos < h->xlen) {
            return ZU_OK;
        }
        h->state = (h->flg & GZ_FNAME) ? ZU_INT_GZ_NAME
                 : (h->flg & GZ_FCOMMENT) ? ZU_INT_GZ_COMMENT
                 : (h->flg & GZ_FHCRC) ? ZU_INT_GZ_HCRC
                 : ZU_INT_GZ_DONE;
        break;

    case ZU_INT_GZ_NAME:
        /* NUL-terminated and unbounded in the format. We discard the bytes
           rather than buffering them, so a hostile multi-megabyte filename
           costs time and nothing else. */
        if (byte != 0) {
            if (++h->field_len > ZU_INT_GZIP_MAX_FIELD) {
                return ZU_ERR_INVALID_DATA;
            }
            return ZU_OK;
        }
        h->field_len = 0;
        h->state = (h->flg & GZ_FCOMMENT) ? ZU_INT_GZ_COMMENT
                 : (h->flg & GZ_FHCRC) ? ZU_INT_GZ_HCRC
                 : ZU_INT_GZ_DONE;
        break;

    case ZU_INT_GZ_COMMENT:
        if (byte != 0) {
            if (++h->field_len > ZU_INT_GZIP_MAX_FIELD) {
                return ZU_ERR_INVALID_DATA;
            }
            return ZU_OK;
        }
        h->field_len = 0;
        h->state = (h->flg & GZ_FHCRC) ? ZU_INT_GZ_HCRC : ZU_INT_GZ_DONE;
        break;

    case ZU_INT_GZ_HCRC:
        h->hcrc[h->hcrc_pos++] = byte;
        if (h->hcrc_pos < 2) {
            return ZU_OK;
        }
        {
            uint16_t want = (uint16_t) ((uint16_t) h->hcrc[0] |
                                        ((uint16_t) h->hcrc[1] << 8));
            if (want != (uint16_t) (h->crc & 0xFFFF)) {
                return ZU_ERR_CHECKSUM;
            }
        }
        h->state = ZU_INT_GZ_DONE;
        break;

    case ZU_INT_GZ_DONE:
    default:
        return ZU_ERR_INTERNAL;
    }

    if (h->state == ZU_INT_GZ_DONE) {
        *done = 1;
    }
    return ZU_OK;
}
