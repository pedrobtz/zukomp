#!/usr/bin/env python3
"""Compare the ZIP reader libzukomp.a ships with Python's zipfile (#37).

    fuzz/build.sh --standalone
    python3 tools/check-zip-reader.py [fuzz/build-replay/fuzz_zip_reader]

Runs fuzz_zip_reader with ZU_FUZZ_ZIP_DUMP set over every archive in
fuzz/corpus/zip/ and reads what miniz delivered. Then, per archive:

  - the harness's own oracles held (it aborts when one does not), and it
    finished inside a time limit, so a hang is a failure, not a stuck job;
  - miniz agrees with the `spec` column of MANIFEST.tsv, which declares from
    APPNOTE.TXT whether a reader must read the archive ("ok": every entry)
    or refuse it ("reject": the archive or an entry), and a refusal is
    classed (an mz_zip_error other than "no error");
  - on an "ok" archive, zipfile and miniz list the same entries -- names,
    method, both sizes, CRC-32, directory or not -- and deliver the same
    bytes through each of miniz's three extraction paths;
  - on a "reject" archive, zipfile refuses too.

Where a reader departs from the spec on purpose or by known limitation, the
tables below say so and why. A miniz departure is pinned: if it stops
happening, the check fails, so the table cannot go stale. A zipfile
departure is the reference's, varies by Python release, and is reported but
not pinned.

Needs only the standard library. CI runs it in fuzz.yaml's replay job.
"""
import io
import os
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
CORPUS = os.path.join(ROOT, "fuzz", "corpus", "zip")
TIMEOUT = 20

# miniz implements methods 0 (stored) and 8 (deflate) only, and the archive
# build adds none (design 15, two consumption modes): refusing a bzip2
# entry, classed, is its contract, where a reader with bzip2 reads it.
MINIZ_REFUSES = {
    "method-bzip2.zip": "unsupported method",
}

# zipfile is lenient where APPNOTE is not. It walks the central directory
# by its byte size and ignores the EOCD's entry count (APPNOTE 4.3.16), and
# it checks the CRC-32 at the end of the stream but not the uncompressed
# size the entry declared (4.4.9).
ZIPFILE_ACCEPTS = {
    "eocd-count-high.zip": "ignores the EOCD entry count",
    "size-long-deflate.zip": "does not compare the decoded size with the declared one",
}


def manifest():
    with open(os.path.join(CORPUS, "MANIFEST.tsv")) as fh:
        head = fh.readline().rstrip("\n").split("\t")
        return [dict(zip(head, l.rstrip("\n").split("\t"))) for l in fh]


def miniz_view(binary, path):
    env = dict(os.environ, ZU_FUZZ_ZIP_DUMP="1")
    try:
        p = subprocess.run([binary, path], env=env, capture_output=True,
                           timeout=TIMEOUT, text=True)
    except subprocess.TimeoutExpired:
        return None, "hung: no answer in %ds" % TIMEOUT
    if p.returncode != 0:
        return None, "harness failed:\n" + p.stderr.strip()[-600:]
    view = {"archive": None, "entries": []}
    for line in p.stdout.splitlines():
        f = line.split("\t")
        if f[0] == "archive":
            view["archive"] = f[1]
        elif f[0] == "entry" and len(f) == 3:
            view["entries"].append({"stat": f[2]})
        elif f[0] == "entry":
            e = {"name": b"" if f[2] == "-" else bytes.fromhex(f[2]),
                 "dir": f[3] == "1", "method": int(f[4]),
                 "comp": int(f[5]), "size": int(f[6]), "crc": int(f[7], 16)}
            for kv in f[8:]:
                k, v = kv.split("=", 1)
                e[k] = v
            view["entries"].append(e)
    return view, None


def miniz_entry_data(e):
    """The bytes miniz delivered, if both paths that ran agree on success."""
    paths = [e[k] for k in ("iter", "mem", "callback")
             if e.get(k, "skipped") != "skipped"]
    if not paths or not all(p.startswith("ok:") for p in paths):
        return None
    datas = {p[3:] for p in paths}
    if len(datas) != 1:
        return None
    h = datas.pop()
    return b"" if h == "-" else bytes.fromhex(h)


def miniz_refusals(view):
    if view["archive"].startswith("refused:"):
        return [view["archive"][len("refused:"):]]
    out = []
    for e in view["entries"]:
        if "stat" in e:
            out.append(e["stat"].split(":", 1)[1])
            continue
        for k in ("iter", "mem", "callback"):
            v = e.get(k, "skipped")
            if v.startswith("refused:"):
                out.append(v[len("refused:"):])
    return out


def zipfile_view(blob):
    try:
        zf = zipfile.ZipFile(io.BytesIO(blob))
    except Exception as ex:
        return None, "%s: %s" % (type(ex).__name__, ex)
    entries = []
    for zi in zf.infolist():
        enc = "utf-8" if zi.flag_bits & 0x800 else "cp437"
        e = {"name": zi.orig_filename.encode(enc), "dir": zi.is_dir(),
             "method": zi.compress_type, "comp": zi.compress_size,
             "size": zi.file_size, "crc": zi.CRC}
        try:
            e["data"] = zf.read(zi)
        except Exception as ex:
            e["error"] = "%s: %s" % (type(ex).__name__, ex)
        entries.append(e)
    return entries, None


def check(binary, row):
    name, spec = row["file"], row["spec"]
    path = os.path.join(CORPUS, name)
    blob = open(path, "rb").read()
    problems = []

    view, err = miniz_view(binary, path)
    if err:
        return [err]
    refusals = miniz_refusals(view)
    if any(r == "no error" for r in refusals):
        problems.append("miniz refused without classing it")

    if name in MINIZ_REFUSES:
        want = MINIZ_REFUSES[name]
        if want not in refusals:
            problems.append("pinned miniz refusal (%s) no longer happens: "
                            "got %s" % (want, refusals or "success"))
    elif spec == "ok" and refusals:
        problems.append("miniz refused a valid archive: %s" % refusals)
    elif spec == "reject" and not refusals:
        problems.append("miniz read an archive APPNOTE says to refuse")

    zentries, zerr = zipfile_view(blob)
    z_refused = zerr is not None or any("error" in e for e in zentries)
    if spec == "reject" and not z_refused:
        if name in ZIPFILE_ACCEPTS:
            print("    note %s: zipfile reads it (%s)" %
                  (name, ZIPFILE_ACCEPTS[name]))
        else:
            problems.append("zipfile read an archive APPNOTE says to refuse")
    if spec == "ok" and z_refused:
        problems.append("zipfile refused a valid archive: %s" %
                        (zerr or [e.get("error") for e in zentries]))

    # The differential proper: on a valid archive, every field and every
    # byte, entry by entry, in central directory order.
    if spec == "ok" and not z_refused and view["archive"] == "ok":
        mentries = view["entries"]
        if len(mentries) != len(zentries):
            problems.append("%d entries in miniz, %d in zipfile" %
                            (len(mentries), len(zentries)))
        for i, (m, z) in enumerate(zip(mentries, zentries)):
            for k in ("name", "dir", "method", "comp", "size", "crc"):
                if m.get(k) != z[k]:
                    problems.append("entry %d: %s is %r in miniz, %r in "
                                    "zipfile" % (i, k, m.get(k), z[k]))
            if name in MINIZ_REFUSES:
                continue
            data = miniz_entry_data(m)
            if data != z["data"]:
                problems.append("entry %d: miniz delivered %s bytes, "
                                "zipfile %d" % (i, "no" if data is None
                                                else len(data),
                                                len(z["data"])))
    return problems


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        ROOT, "fuzz", "build-replay", "fuzz_zip_reader")
    if not os.access(binary, os.X_OK):
        sys.exit("no %s: run fuzz/build.sh --standalone first" % binary)
    rows = manifest()
    on_disk = sorted(f for f in os.listdir(CORPUS) if f.endswith(".zip"))
    status = 0
    if sorted(r["file"] for r in rows) != on_disk:
        print("FAIL MANIFEST.tsv and fuzz/corpus/zip/ list different archives")
        status = 1
    for row in rows:
        problems = check(binary, row)
        print("    %s %-32s %s" % ("ok  " if not problems else "FAIL",
                                    row["file"], row["spec"]))
        for p in problems:
            print("         " + p.replace("\n", "\n         "))
        status |= bool(problems)
    print("==> %s: %d archives, zipfile from Python %d.%d.%d" %
          ("all agree" if status == 0 else "disagreements", len(rows),
           *sys.version_info[:3]))
    return status


if __name__ == "__main__":
    sys.exit(main())
