/* Two codecs that break the status discipline on purpose.
 *
 * zukomp's drive loop has to tell a codec that is waiting from one that is
 * stuck, because a stuck codec in a loop with no exit hangs the R session
 * until the user presses Ctrl-C. No built-in codec gets stuck, so these
 * are the only way to show that a registered one -- code nobody here
 * reviewed (design 11) -- cannot hang the session either:
 *
 *   stall-ok          answers ZU_OK and moves nothing, with input to read
 *                     and room to write: a codec that forgot to say which
 *                     it is waiting for.
 *   stall-lookahead   answers ZU_NEED_INPUT without consuming its slice: a
 *                     codec that wants more bytes before it takes any. The
 *                     drive loop refills only a spent slice, so it would
 *                     offer the same bytes forever.
 *
 * Both encode and decode the same way, so the one-shot paths are reached
 * too. */
#include <zukomp-r.h>

static int stall_state;

static zu_status stall_new(void **st)
{
    if (st == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *st = &stall_state;
    return ZU_OK;
}

static zu_status stall_encoder_new(void **st, const zu_encoder_opts *o)
{ (void) o; return stall_new(st); }
static zu_status stall_decoder_new(void **st, const zu_decoder_opts *o)
{ (void) o; return stall_new(st); }
static zu_status stall_encoder_reset(void *st, const zu_encoder_opts *o)
{ (void) st; (void) o; return ZU_OK; }
static zu_status stall_decoder_reset(void *st, const zu_decoder_opts *o)
{ (void) st; (void) o; return ZU_OK; }
static void stall_free(void *st) { (void) st; }

static zu_status stall_bound(int32_t level, size_t n, size_t *out)
{
    (void) level;
    if (out == NULL) {
        return ZU_ERR_INVALID_ARGUMENT;
    }
    *out = n + 16;
    return ZU_OK;
}

static zu_status stall_ok_process(void *st, zu_buffer *buf, zu_flush flush)
{
    (void) st; (void) flush;
    return (buf == NULL) ? ZU_ERR_INVALID_ARGUMENT : ZU_OK;
}

static zu_status stall_lookahead_process(void *st, zu_buffer *buf,
                                         zu_flush flush)
{
    (void) st; (void) flush;
    return (buf == NULL) ? ZU_ERR_INVALID_ARGUMENT : ZU_NEED_INPUT;
}

/* Vendor ids clear of xor5a (the base) and of the invalid registrations
   test-registry-identity.R attempts (base + 7 to base + 11). */
static const zu_codec_vtable stall_ok_vtable = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_VENDOR_BASE + 20,
    "stall-ok",
    NULL,
    "zukomptest",
    0, 0, 0,
    ZU_CAN_ENCODE | ZU_CAN_DECODE,
    NULL, 0, 0, NULL,
    stall_encoder_new, stall_ok_process, stall_encoder_reset, stall_free,
    stall_decoder_new, stall_ok_process, stall_decoder_reset, stall_free,
    stall_bound
};

static const zu_codec_vtable stall_lookahead_vtable = {
    (uint32_t) sizeof(zu_codec_vtable),
    (uint32_t) ZU_CODEC_VENDOR_BASE + 21,
    "stall-lookahead",
    NULL,
    "zukomptest",
    0, 0, 0,
    ZU_CAN_ENCODE | ZU_CAN_DECODE,
    NULL, 0, 0, NULL,
    stall_encoder_new, stall_lookahead_process, stall_encoder_reset, stall_free,
    stall_decoder_new, stall_lookahead_process, stall_decoder_reset, stall_free,
    stall_bound
};

const zu_codec_vtable *zukomptest_stall_ok_vtable(void)
{
    return &stall_ok_vtable;
}

const zu_codec_vtable *zukomptest_stall_lookahead_vtable(void)
{
    return &stall_lookahead_vtable;
}
