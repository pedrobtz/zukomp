#!/usr/bin/env bash
# The installed headers, compiled the way a consumer compiles them. Run from
# the repository root; needs a C and a C++ compiler, python3, and R (the two
# consumer probes include R.h through $(R RHOME)).
#
# Called by abi.yaml on Linux and by tools/ci/platform.sh on macOS, so the
# two compilers see the same probes. A warning here is a build failure in a
# downstream package's tree, invisible from inside zukomp.
set -eu
work=./.header-probes
rm -rf "$work"; mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# Compiles as C99 with no R and no codec headers.
printf '#include <zukomp.h>\nint main(void){return 0;}\n' > "$work/probe.c"
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
   -Iinst/include -c "$work/probe.c" -o /dev/null
echo "ok  C99"

# Compiles as C++ through the extern "C" wrapper.
printf '#include <zukomp.h>\nint main(){return 0;}\n' > "$work/probe.cpp"
c++ -std=c++11 -Wall -Wextra -Werror \
    -Iinst/include -c "$work/probe.cpp" -o /dev/null
echo "ok  C++"

# A consumer compiles against zukomp-r.h with -Werror: a stand-in for a
# downstream package's translation unit, built with strict flags.
cat > "$work/probe_consumer.c" <<'C'
#include <zukomp-r.h>
SEXP probe(void) {
    const zukomp_api_v1 *api = zukomp_api();
    if (api == NULL) return Rf_ScalarLogical(0);
    return Rf_ScalarLogical(api->codec_lookup("gzip") == ZU_CODEC_GZIP);
}
C
cc -std=c99 -Wall -Wextra -Werror \
   -Iinst/include -I"$(R RHOME)/include" \
   -c "$work/probe_consumer.c" -o /dev/null
echo "ok  consumer build"

# Including zukomp-r.h without calling every function is warning-free: a
# header-defined plain `static` function is an unused-function error under
# -Werror, a build failure in their tree and invisible in ours.
printf '#include <zukomp-r.h>\nSEXP other(void){ return R_NilValue; }\n' \
  > "$work/probe_unused.c"
cc -std=c99 -Wall -Wextra -Werror \
   -Iinst/include -I"$(R RHOME)/include" \
   -c "$work/probe_unused.c" -o /dev/null
echo "ok  header is inert when unused"

# Declares nothing from R or a vendored codec. Comments are stripped first,
# matching tests/testthat/helper-abi.R: the rule is about what the header
# declares, and a comment saying no miniz type appears here is fine.
python3 - <<'PY'
import re, sys
src = open("inst/include/zukomp.h", encoding="utf-8").read()
code = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
code = re.sub(r"//[^\n]*", " ", code)
bad = [p for p in ("miniz", "mz_", "tdefl", "tinfl", "MZ_",
                   "SEXP", "Rf_", "Rinternals")
       if p in code]
# zstd's and lz4's identifiers, as identifiers: ZU_CODEC_LZ4_FRAME is
# zukomp's own name and must not match.
bad += re.findall(r"(?<![A-Za-z0-9_])(?:ZSTD_|ZSTDLIB|LZ4_|LZ4F|LZ4HC|XXH)\w*",
                  code)
for p in bad:
    print(f"FAIL inst/include/zukomp.h declares '{p}'")
sys.exit(1 if bad else print("ok  no leaked vocabulary") or 0)
PY
