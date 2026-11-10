# fuzz_lifecycle seeds

Operation sequences for `fuzz/fuzz_lifecycle.c`, written by
`tools/make-lifecycle-seeds.py` (`--check` verifies them). Each
`regress-<rule>-<codec>` seed reaches one rule of design 9's handle
lifecycle (decision 28, #71) for one codec, and was shown to make the target
abort when that rule is broken in `src/zu_stream.c`:

| seeds | rule | broken by |
|---|---|---|
| `regress-sticky-limit-*` | a failure (here the output limit) repeats on every later kind of call | removing the decoder's `failed` check |
| `regress-sticky-expected-*` | an expected-size failure at the end is not followed by `ZU_STREAM_END` (#83) | the same |
| `regress-inject-decoder-*` | an injected codec failure is sticky, and a reset clears it | the same; a reset that keeps `failed` |
| `regress-inject-encoder-*` | the same for an encoder | removing the encoder's `failed` check |
| `regress-finish-final-*` | `ZU_RUN` and `ZU_FLUSH` after `ZU_FINISH` are refused, both directions | removing either `finishing` check |
| `regress-trailing-after-end-*` | input after the end is `ZU_ERR_TRAILING`, a failure | not recording it as one |
| `regress-refused-*` | a null buffer, or `ZU_FLUSH` where the codec cannot, changes nothing | |
| `regress-renew-*` | freeing part-way and starting again frees everything | a decoder free that skips the codec's state |

`seed-*` are ordinary sequences to start the search from: a round trip of
the encoder's own flushed output, and corrupt streams fed a byte at a time.
