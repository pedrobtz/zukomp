# zukomp: Portable Byte Compression with a Pluggable Codec Registry

Compresses and decompresses raw vectors through a single codec-neutral
interface backed by a runtime registry, so the set of available codecs
is discovered rather than fixed at compile time. The 'DEFLATE' family
('gzip', 'zlib', and headerless 'DEFLATE') is built in from vendored
'miniz' sources, requiring no system compression library. Decompression
enforces configurable output-size and expansion-ratio limits and reports
failures through structured conditions. A registered C-callable
interface lets other packages drive the codecs from C and register
codecs of their own. The formats are specified in Deutsch (1996)
[doi:10.17487/RFC1951](https://doi.org/10.17487/RFC1951) , Deutsch and
Gailly (1996) [doi:10.17487/RFC1950](https://doi.org/10.17487/RFC1950) ,
and Deutsch (1996)
[doi:10.17487/RFC1952](https://doi.org/10.17487/RFC1952) .

## See also

Useful links:

- <https://github.com/pedrobtz/zukomp>

- <https://pedrobtz.github.io/zukomp/>

- Report bugs at <https://github.com/pedrobtz/zukomp/issues>

## Author

**Maintainer**: Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Authors:

- Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Other contributors:

- Rich Geldreich (author of the bundled miniz library) \[contributor,
  copyright holder\]

- Tenacious Software LLC (copyright holder of the bundled miniz library)
  \[copyright holder\]

- RAD Game Tools (copyright holder of the bundled miniz library)
  \[copyright holder\]

- Valve Software (copyright holder of the bundled miniz library)
  \[copyright holder\]

- Martin Raiber (contributor to and copyright holder of the bundled
  miniz library) \[contributor, copyright holder\]

- Alex Evans (author of the PNG writer in the bundled miniz library)
  \[contributor\]

- Alistair Moffat (co-author of the code-length algorithm in the bundled
  miniz library) \[contributor\]

- Jyrki Katajainen (co-author of the code-length algorithm in the
  bundled miniz library) \[contributor\]
