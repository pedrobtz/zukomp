#!/bin/sh
# Fuzz reach (#67): what the corpus runs of zukomp's C core, per file, with
# the gap lists, against a floor.
#
#   fuzz/reach.sh                  report, and fail below fuzz/reach-floor.tsv
#   fuzz/reach.sh --no-floor       report only
#
# A fuzzed line ran on hostile input. Reaching it says nothing about whether
# it was right -- the oracles in fuzz_oracle.h say that -- so this number is
# not evidence of correctness. It answers which input-handling code no
# fuzz target reaches at all, and the floor catches a harness or seed
# change that stops reaching something: delete the gzip seeds and
# src/zu_gzip.c falls below its floor.
#
# It replays the same corpus as fuzz/replay.sh (run tools/make-fuzz-corpus.sh
# first) through drivers built by `fuzz/build.sh --coverage`, merges the
# profiles, and exports lcov for src/ with the vendored libraries left out.
# Then it prints, and appends to $GITHUB_STEP_SUMMARY when that is set:
#
#   1. per file: lines, branch outcomes and functions reached;
#   2. functions no target enters;
#   3. one-sided conditions: lines a target ran where some branch outcome
#      was never taken -- #64 was a condition like that, its line covered
#      and its other side never;
#   4. lines no target reaches, as ranges.
#
# The floors are per file and in total, percentages of lines, a little
# under what was measured. A floor is raised by editing reach-floor.tsv in
# the change that adds the seeds or the harness, never lowered to make a
# red job green without saying why in the same change.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

check_floor=1
[ "${1:-}" = "--no-floor" ] && check_floor=0

if [ "$(uname -s)" = "Darwin" ]; then
    profdata="xcrun llvm-profdata"; llvmcov="xcrun llvm-cov"
else
    # The llvm tools that match the clang that built the drivers: Ubuntu
    # ships them unversioned only beside a versioned clang.
    bindir=$(dirname "$(readlink -f "$(command -v "${CC:-clang}")")")
    profdata="$bindir/llvm-profdata"; llvmcov="$bindir/llvm-cov"
    [ -x "$profdata" ] || profdata=llvm-profdata
    [ -x "$llvmcov" ] || llvmcov=llvm-cov
fi

./fuzz/build.sh --coverage >/dev/null
BUILD=fuzz/build-cov
out=$BUILD/reach
rm -rf "$out"
mkdir -p "$out/prof"

inputs=$(find fuzz/corpus -type f ! -name '*.md' | sort)
[ -n "$inputs" ] || { echo "no corpus inputs: run tools/make-fuzz-corpus.sh" >&2; exit 1; }
objs=""
for bin in "$BUILD"/fuzz_*; do
    [ -f "$bin" ] && [ -x "$bin" ] || continue
    name=$(basename "$bin")
    # shellcheck disable=SC2086
    printf '%s\n' $inputs |
        LLVM_PROFILE_FILE="$out/prof/$name-%p.profraw" xargs "$bin" >/dev/null
    objs="$objs -object $bin"
done
$profdata merge -sparse "$out"/prof/*.profraw -o "$out/all.profdata"
# shellcheck disable=SC2086
$llvmcov export $objs -instr-profile="$out/all.profdata" -format=lcov \
    -ignore-filename-regex='(vendor|fuzz|inst)/' > "$out/fuzz.lcov"

# The report. Paths are matched from src/ on, so the floors file need not
# know where the checkout is.
awk -F'[:,]' -v summary="${GITHUB_STEP_SUMMARY:-}" -v floors=fuzz/reach-floor.tsv \
    -v check="$check_floor" '
    function rel(p) { sub(/^.*\/src\//, "src/", p); return p }
    function fname(r) { return substr(r, index(r, ",") + 1) }
    function pct(h, n) { return n ? 100 * h / n : 0 }
    function fmt(h, n) { return n ? sprintf("%5.1f%%", pct(h, n)) : "    -" }
    function emit(s) { print s; if (summary != "") print s >> summary }
    BEGIN {
        while ((getline l < floors) > 0) {
            if (l ~ /^#/ || l == "") continue
            split(l, fa, "\t"); floor[fa[1]] = fa[2] + 0
        }
    }
    /^SF:/ { f = rel(substr($0, 4)); files[f] = 1; next }
    /^DA:/ {
        k = f ":" $2
        if (!(k in line)) { line[k] = 0; nl[f]++; if ($2 > maxl[f]) maxl[f] = $2 }
        if ($3 > 0 && !line[k]) { line[k] = 1; hl[f]++ }
        next
    }
    # A header's static inline helpers are one function per includer, so
    # they are left out of the function counts; their lines still count.
    /^FN/ && f ~ /\.h$/ { next }
    /^FN:/ {
        k = f ":" fname($0)
        if (!(k in fn)) { fn[k] = 0; nf[f]++; fnl[k] = $2 }
        next
    }
    /^FNDA:/ {
        k = f ":" fname($0)
        if ($2 > 0 && !fn[k]) { fn[k] = 1; hf[f]++ }
        next
    }
    /^BRDA:/ {
        k = f ":" $2 ":" $3 ":" $4
        if (!(k in br)) { br[k] = 0; nb[f]++ }
        if ($5 != "-" && $5 > 0 && !br[k]) { br[k] = 1; hb[f]++ }
        lk = f ":" $2
        if ($5 != "-" && $5 > 0) taken[lk] = 1; else untaken[lk] = 1
        next
    }
    END {
        n = 0
        for (f in files) order[++n] = f
        for (i = 2; i <= n; i++)
            for (j = i; j > 1 && order[j - 1] > order[j]; j--) {
                t = order[j]; order[j] = order[j - 1]; order[j - 1] = t
            }

        emit("## Fuzz reach by file")
        emit("")
        emit("| file | lines | floor | branch outcomes | functions |")
        emit("|---|---:|---:|---:|---:|")
        bad = ""
        for (i = 1; i <= n; i++) {
            f = order[i]
            fl = (f in floor) ? sprintf("%.1f%%", floor[f]) : "-"
            if ((f in floor) && pct(hl[f] + 0, nl[f] + 0) < floor[f])
                bad = bad sprintf("\n  %s: %.1f%% of lines, floor %.1f%%",
                                  f, pct(hl[f] + 0, nl[f] + 0), floor[f])
            emit(sprintf("| %s | %s of %d | %s | %s of %d | %d of %d |", f,
                 fmt(hl[f] + 0, nl[f] + 0), nl[f] + 0, fl,
                 fmt(hb[f] + 0, nb[f] + 0), nb[f] + 0, hf[f] + 0, nf[f] + 0))
            TL += nl[f]; TH += hl[f]; TB += nb[f]; TBH += hb[f]
            TF += nf[f]; TFH += hf[f]
        }
        fl = ("total" in floor) ? sprintf("%.1f%%", floor["total"]) : "-"
        if (("total" in floor) && pct(TH, TL) < floor["total"])
            bad = bad sprintf("\n  total: %.1f%% of lines, floor %.1f%%",
                              pct(TH, TL), floor["total"])
        emit(sprintf("| **total** | %s of %d | %s | %s of %d | %d of %d |",
             fmt(TH, TL), TL, fl, fmt(TBH, TB), TB, TFH, TF))
        emit("")

        emit("## Functions no fuzz target enters")
        emit("")
        emit("```")
        for (i = 1; i <= n; i++) {
            f = order[i]; m = 0
            for (k in fn) if (index(k, f ":") == 1 && !fn[k]) miss[++m] = k
            for (a = 2; a <= m; a++)
                for (b = a; b > 1 && fnl[miss[b - 1]] > fnl[miss[b]]; b--) {
                    t = miss[b]; miss[b] = miss[b - 1]; miss[b - 1] = t
                }
            for (a = 1; a <= m; a++)
                emit(sprintf("%s:%d %s", f, fnl[miss[a]],
                             substr(miss[a], length(f) + 2)))
        }
        emit("```")
        emit("")

        emit("## One-sided conditions: a target ran the line, some outcome never")
        emit("")
        emit("```")
        for (i = 1; i <= n; i++) {
            f = order[i]; s = ""
            for (l = 1; l <= maxl[f]; l++) {
                lk = f ":" l
                if ((lk in taken) && (lk in untaken)) s = s " " l
            }
            if (s != "") emit(f ":" s)
        }
        emit("```")
        emit("")

        emit("## Lines no fuzz target reaches")
        emit("")
        emit("```")
        for (i = 1; i <= n; i++) {
            f = order[i]; s = ""; a = 0; b = 0
            for (l = 1; l <= maxl[f] + 1; l++) {
                k = f ":" l
                if (l <= maxl[f] && !(k in line)) continue
                if (l <= maxl[f] && !line[k]) { if (!a) a = l; b = l; continue }
                if (a) s = s " " (a == b ? a : a "-" b)
                a = 0
            }
            if (s != "") emit(f ":" s)
        }
        emit("```")

        printf "fuzz reach: %.1f%% of %d lines, %.1f%% of %d branch outcomes\n",
               pct(TH, TL), TL, pct(TBH, TB), TB
        if (check && bad != "") {
            printf "fuzz reach is below its floor (fuzz/reach-floor.tsv):%s\n", bad > "/dev/stderr"
            exit 1
        }
    }' "$out/fuzz.lcov"
