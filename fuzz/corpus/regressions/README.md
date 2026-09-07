# Regression corpus

Minimised inputs that once caused a finding. Never pruned, replayed by
`fuzz/replay.sh`, and each one has a matching testthat test — a fuzz
finding without a regression test is a finding that can come back.

| input | finding |
|---|---|
| `empty-input-null-pointer-arith` | A zero-length input buffer has a NULL pointer with a zero size, which is legal. `buf->src + buf->src_pos` on it is arithmetic on a null pointer: undefined in C, flagged by UBSan, and reachable from `komp_decompress(raw(0), "deflate-raw")`. Fixed in `codec_deflate.c` by `zu_int_cat()`/`zu_int_at()`, and by never handing miniz a NULL `next_in` — `tinfl` computes `pIn_buf_next + *pIn_buf_size` unguarded. |
| `gzip-header-only` | A gzip stream truncated to its magic plus CM. Exercises the header parser's need-more-input path at the earliest point it can be reached. |
