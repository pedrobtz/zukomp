# Compressing real documents

Compression benchmarks on random bytes tell you nothing, because nobody
transfers random bytes. What travels over HTTP is markup: XML, JSON and
YAML, all of them highly repetitive. This article compresses three real
documents and compares what DEFLATE actually does with each.

The three are the same files used in the sibling packages’ articles, so
the figures line up with what `zuxml`, `zujson` and `zuyaml` report for
them.

``` r

library(zukomp)

sources <- c(
  xml  = "https://www.ibiblio.org/xml/examples/shakespeare/hamlet.xml",
  json = paste0("https://raw.githubusercontent.com/kubernetes/kubernetes/",
                "v1.31.0/api/openapi-spec/swagger.json"),
  yaml = paste0("https://raw.githubusercontent.com/prometheus-operator/",
                "prometheus-operator/v0.76.0/example/prometheus-operator-crd/",
                "monitoring.coreos.com_prometheuses.yaml")
)

paths <- vapply(names(sources), function(fmt) {
  p <- file.path(tempdir(), paste0("sample.", fmt))
  download.file(sources[[fmt]], p, quiet = TRUE)
  p
}, "")

# Bytes in, bytes out: read them as raw, which is what komp_compress() takes.
docs <- lapply(paths, function(p) readBin(p, "raw", file.size(p)))
vapply(docs, length, 1L)
#>     xml    json    yaml
#>  279663 3277085  773063
```

## Three formats, three codecs

``` r

grid <- expand.grid(format = names(docs),
                    codec  = c("gzip", "zlib", "deflate-raw"),
                    stringsAsFactors = FALSE)

tab <- do.call(rbind, Map(function(fmt, codec) {
  raw <- docs[[fmt]]
  z   <- komp_compress(raw, codec)
  data.frame(format = fmt, codec = codec,
             bytes = length(raw), compressed = length(z),
             ratio = round(length(raw) / length(z), 1))
}, grid$format, grid$codec))

print(tab, row.names = FALSE)
#>  format       codec   bytes compressed ratio
#>     xml        gzip  279663      79052   3.5
#>    json        gzip 3277085     239803  13.7
#>    yaml        gzip  773063      85195   9.1
#>     xml        zlib  279663      79040   3.5
#>    json        zlib 3277085     239791  13.7
#>    yaml        zlib  773063      85183   9.1
#>     xml deflate-raw  279663      79034   3.5
#>    json deflate-raw 3277085     239785  13.7
#>    yaml deflate-raw  773063      85177   9.1
```

Two things are worth reading off that table.

**The three codecs are the same compressor.** `gzip`, `zlib` and
`deflate-raw` are one DEFLATE stream under three framings, so the ratios
are identical and the sizes differ only by header and checksum — 18
bytes for gzip, 6 for zlib, 0 for raw. Choosing between them is a
question of what the other end expects, not of compression.

**The format matters far more than the codec.** The Kubernetes spec
compresses 13.7×, the CRD 9.1×, and *Hamlet* only 3.5×. That is the
difference between machine-generated schema — the same keys and
boilerplate descriptions repeated across 635 definitions — and English
prose, which has already had most of its redundancy removed by being
written by a person. A 3.3 MB API response becomes 234 KB on the wire; a
play does not shrink nearly as well.

## Level is a real trade

``` r

for (lv in c(1, 6, 9)) {
  cat(sprintf("level %d: %7d bytes\n", lv,
              length(komp_compress(docs$json, "gzip", level = lv))))
}
#> level 1:  408321 bytes
#> level 6:  239803 bytes
#> level 9:  236218 bytes
```

The jump from level 1 to the default 6 is worth 41% of the output. Going
on to 9 buys another 1.5%, for substantially more CPU — which is the
usual shape of this curve, and the reason 6 is the default nearly
everywhere.

## Detection, and the one case it refuses

``` r

z <- komp_compress(docs$json, "gzip")

komp_detect(z)
#> [1] "gzip"

identical(komp_decompress(z), docs$json)
#> [1] TRUE
```

[`komp_decompress()`](https://pedrobtz.github.io/zukomp/reference/komp_decompress.md)
detects the codec by default, so a response body whose
`Content-Encoding` you did not keep still round-trips. Headerless
DEFLATE is the exception, and it returns `NA` rather than guessing:

``` r

komp_detect(komp_compress(docs$json, "deflate-raw"))
#> [1] NA
```

That refusal is deliberate. There is nothing at the front of a raw
DEFLATE stream to recognise, and a wrong guess there does not fail — it
returns wrong bytes. Naming the codec is the only safe answer.

## Untrusted input

A compressed body from the network can expand enormously, so
decompression is capped. The cap is enforced by the core stream driver
rather than by each codec, so no codec — including one registered by
another package — can bypass it:

``` r

komp_decompress(z, max_output = 1024)
#> Error in komp_decompress(z, max_output = 1024) :
#>   Decompressed output exceeded `max_output`.
```

The default is 1 GiB, `max_ratio` bounds expansion relative to the input
instead, and every failure is a classed condition carrying `codec`,
`input_bytes`, `output_bytes` and `native_status` — so a caller branches
on what went wrong rather than matching on the message.
