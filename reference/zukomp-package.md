# zukomp: Portable Byte Compression with a Pluggable Codec Registry

Compresses and decompresses raw vectors through a single codec-neutral
interface backed by a runtime registry, so the set of available codecs
is discovered rather than fixed at compile time. The 'DEFLATE' family
('gzip', 'zlib', and headerless 'DEFLATE') is built in from vendored
'miniz' sources, requiring no system compression library. Decompression
enforces configurable output-size and expansion-ratio limits and reports
failures through structured conditions. A registered C-callable
interface lets other packages drive the codecs from C and register
codecs of their own.

## See also

Useful links:

- <https://github.com/pedrobtz/zukomp>

- Report bugs at <https://github.com/pedrobtz/zukomp/issues>

## Author

**Maintainer**: Pedro Baltazar <pedrobtz@gmail.com> \[copyright holder\]

Other contributors:

- Rich Geldreich (author of the bundled miniz library) \[contributor,
  copyright holder\]

- Tenacious Software LLC (copyright holder of the bundled miniz library)
  \[copyright holder\]

- RAD Game Tools (copyright holder of the bundled miniz library)
  \[copyright holder\]

- Valve Software (copyright holder of the bundled miniz library)
  \[copyright holder\]
