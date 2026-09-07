/* Internal declarations shared between zukomp's own translation units.
   Never installed, never seen by a consumer: everything here is prefixed
   zu_int_ per design 14 and may change without an ABI bump. */
#ifndef ZU_INTERNAL_H
#define ZU_INTERNAL_H

#include "zukomp.h"

/* A codec this build knows the *name* of, whether or not an implementation
   is registered. Declaring names independently of implementations is what
   lets komp_codecs() advertise "zstd exists, install zukomp.zstd" instead of
   pretending the codec does not exist. */
typedef struct {
    zu_codec    codec;
    const char *name;
    const char *content_encoding;  /* NULL if not an HTTP content-coding */
} zu_int_codec_decl;

size_t                   zu_int_declared_count(void);
const zu_int_codec_decl *zu_int_declared_at(size_t i);
const zu_int_codec_decl *zu_int_declared_for(zu_codec codec);

/* Registered vtable for a codec, or NULL. */
const zu_codec_vtable *zu_int_registry_lookup(zu_codec codec);

/* Registered vtables in registration order, for building the R-side table. */
size_t                 zu_int_registry_count(void);
const zu_codec_vtable *zu_int_registry_at(size_t i);

/* Registers everything zukomp itself implements. Called once from
   R_init_zukomp, before any encoder or decoder can exist. */
zu_status zu_int_register_builtin_codecs(void);

/* The identity codec's vtable. */
extern const zu_codec_vtable zu_int_codec_identity;

#endif /* ZU_INTERNAL_H */
