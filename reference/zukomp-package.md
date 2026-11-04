# zukomp: Portable Byte Compression with a Pluggable Codec Registry

Compresses and decompresses raw vectors through a single codec-neutral
interface backed by a runtime registry, so the set of available codecs
is discovered rather than fixed at compile time. The 'DEFLATE' family
('gzip', 'zlib', and headerless 'DEFLATE'), 'Zstandard' and 'LZ4' (frame
and block formats) are built in from vendored 'miniz', 'zstd' and 'lz4'
sources, requiring no system compression library. Decompression enforces
configurable output-size and expansion-ratio limits and reports failures
through structured conditions. A registered C-callable interface lets
other packages drive the codecs from C and register codecs of their own.
The formats are specified in Deutsch (1996)
[doi:10.17487/RFC1951](https://doi.org/10.17487/RFC1951) , Deutsch and
Gailly (1996) [doi:10.17487/RFC1950](https://doi.org/10.17487/RFC1950) ,
Deutsch (1996) [doi:10.17487/RFC1952](https://doi.org/10.17487/RFC1952)
, and Collet and Kucherawy (2021)
[doi:10.17487/RFC8878](https://doi.org/10.17487/RFC8878) .

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

- Yann Collet (author of the bundled LZ4 library and of the bundled
  Zstandard library's xxHash) \[contributor, copyright holder\]

- Meta Platforms, Inc. and affiliates (copyright holder of the bundled
  Zstandard library) \[copyright holder\]

- Tino Reichardt (threading code in the bundled Zstandard library)
  \[contributor, copyright holder\]
