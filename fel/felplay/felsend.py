#!/usr/bin/env python3
"""felsend.py - cut an HEVC interval from an MKV and emit a FELS framed stream.

Runs on a LAN host with the films (needs ffprobe/ffmpeg).  Stream format: see README.md.
  felsend.py FILM.mkv --ss SEC --t SEC (--port P | --out FILE) [--verify]

Packets come from ffprobe (pts/pos/size/flags); frame bytes are read straight
from the file at `pos` (past the Matroska block header), converted from
length-prefixed to Annex-B, and the hvcC parameter-set/SEI arrays are prepended
to every keyframe record.  --verify compares NAL-by-NAL against
ffmpeg -c copy -bsf:v hevc_mp4toannexb.
"""
from __future__ import annotations

import argparse
import hashlib
import logging
import re
import signal
import socket
import struct
import subprocess
import sys
from typing import BinaryIO, Iterator

log = logging.getLogger("felsend")
SC = b"\x00\x00\x00\x01"
EXPIRY_S = 4 * 3600


def run(cmd: list[str], timeout: int = 600) -> bytes:
    log.debug("run %s", cmd)
    return subprocess.run(cmd, check=True, capture_output=True, timeout=timeout).stdout


def probe_extradata(film: str) -> tuple[bytes, int, int, int, int]:
    """Return (hvcC bytes, width, height, fps_num, fps_den)."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
               "stream=extradata,extradata_size,width,height,r_frame_rate", "-show_data",
               "-of", "json", film]).decode()
    import json
    s = json.loads(out)["streams"][0]
    hexs = ""
    for line in s["extradata"].splitlines():
        m = re.match(r"^[0-9a-f]{8}: ([0-9a-f]{2,4}(?: [0-9a-f]{2,4})*)", line)
        if m:
            hexs += m.group(1).replace(" ", "")
    data = bytes.fromhex(hexs)
    if len(data) != int(s["extradata_size"]):
        raise ValueError(f"extradata parse {len(data)} != {s['extradata_size']}")
    num, den = (int(x) for x in s["r_frame_rate"].split("/"))
    return data, int(s["width"]), int(s["height"]), num, den


def parse_hvcc(hv: bytes) -> tuple[int, bytes]:
    """Return (nal length size, Annex-B concat of all hvcC array NALs)."""
    if hv[0] != 1:
        raise ValueError("not hvcC")
    lsize = (hv[21] & 3) + 1
    n = hv[22]
    p = 23
    out = bytearray()
    for _ in range(n):
        cnt = struct.unpack(">H", hv[p + 1:p + 3])[0]
        p += 3
        for _ in range(cnt):
            ln = struct.unpack(">H", hv[p:p + 2])[0]
            out += SC + hv[p + 2:p + 2 + ln]
            p += 2 + ln
    return lsize, bytes(out)


def list_packets(film: str, ss: float, t: float) -> list[tuple[int, int, int, bool]]:
    """(pts_us, pos, size, key) in decode order, starting at the first keyframe."""
    out = run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-read_intervals",
               f"{ss}%+{t}", "-show_packets", "-show_entries", "packet=pts_time,pos,size,flags",
               "-of", "compact", film]).decode()
    pk: list[tuple[int, int, int, bool]] = []
    for line in out.splitlines():
        if not line.startswith("packet|"):
            continue
        d = dict(kv.split("=", 1) for kv in line.split("|")[1:])
        key = d["flags"].startswith("K")
        if not pk and not key:
            continue
        pk.append((round(float(d["pts_time"]) * 1e6), int(d["pos"]), int(d["size"]), key))
    return pk


def vint(b: bytes, p: int) -> tuple[int, int]:
    """EBML variable-length size at b[p]; return (value, length)."""
    first = b[p]
    ln = 1
    mask = 0x80
    while ln <= 8 and not (first & mask):
        ln += 1
        mask >>= 1
    val = first & (mask - 1)
    for i in range(1, ln):
        val = (val << 8) | b[p + i]
    return val, ln


def read_frame(f: BinaryIO, pos: int, size: int) -> bytes:
    """ffprobe `pos` is the Matroska block payload start (after the 0xA3/0xA1 id and size
    vint, measured on this film): track vint, int16 timecode, flags byte, then the frame."""
    f.seek(pos)
    head = f.read(16)
    _, tln = vint(head, 0)
    skip = tln + 3
    if int.from_bytes(head[skip:skip + 4], "big") > size:
        raise ValueError(f"pos {pos}: frame does not start with a plausible NAL length")
    f.seek(pos + skip)
    data = f.read(size)
    if len(data) != size:
        raise ValueError("short read")
    return data


def to_annexb(data: bytes, lsize: int) -> bytes:
    out = bytearray()
    p = 0
    while p < len(data):
        n = int.from_bytes(data[p:p + lsize], "big")
        p += lsize
        if n == 0 or p + n > len(data):
            raise ValueError("bad NAL length")
        out += SC + data[p:p + n]
        p += n
    return bytes(out)


def records(film: str, pk: list[tuple[int, int, int, bool]], lsize: int, params: bytes
            ) -> Iterator[bytes]:
    with open(film, "rb") as f:
        for pts, pos, size, key in pk:
            ab = to_annexb(read_frame(f, pos, size), lsize)
            if key:
                ab = params + ab
            yield struct.pack("<IIq", len(ab), 1 if key else 0, pts) + ab


def split_nals(b: bytes) -> list[tuple[int, str]]:
    """(type, sha1) per NAL of an Annex-B blob."""
    idx = [m.start() for m in re.finditer(b"\x00\x00\x01", b)]
    out: list[tuple[int, str]] = []
    for i, s in enumerate(idx):
        e = idx[i + 1] if i + 1 < len(idx) else len(b)
        nal = b[s + 3:e].rstrip(b"\x00")
        out.append(((nal[0] >> 1) & 0x3F, hashlib.sha1(nal).hexdigest()))
    return out


def verify(film: str, ss: float, t: float, recs: list[bytes]) -> bool:
    mine: list[tuple[int, str]] = []
    for r in recs:
        mine += split_nals(r[16:])
    ref = split_nals(run(["ffmpeg", "-v", "error", "-ss", str(ss), "-i", film, "-map", "0:v:0",
                          "-t", str(t), "-c", "copy", "-bsf:v", "hevc_mp4toannexb", "-f", "hevc",
                          "-"], timeout=1800))
    from collections import Counter
    log.info("mine: %d NALs types=%s", len(mine), dict(sorted(Counter(x[0] for x in mine).items())))
    log.info("ref : %d NALs types=%s", len(ref), dict(sorted(Counter(x[0] for x in ref).items())))
    # Align on access units (AUD = type 35): ffmpeg -ss/-t may cover a different span.
    def aus(nals: list[tuple[int, str]], skip: set[int]) -> list[list[tuple[int, str]]]:
        out: list[list[tuple[int, str]]] = []
        for x in nals:
            if x[0] == 35:
                out.append([])
            if out and x[0] not in skip:
                out[-1].append(x)
        return out

    ok = True
    for label, skip in (("full", set()), ("minus-PS/SEI39", {32, 33, 34, 39})):
        a, b = aus(mine, skip), aus(ref, skip)
        start = next((i for i, au in enumerate(b) if au == a[0]), -1)
        if start < 0:
            log.info("%s: first AU of mine not found in ref", label)
            ok = False
            continue
        n = min(len(a), len(b) - start)
        bad = [i for i in range(n) if a[i] != b[start + i]]
        log.info("%s: AUs mine=%d ref=%d ref_start=%d compared=%d mismatching_AUs=%d first=%s",
                 label, len(a), len(b), start, n, len(bad), bad[:5])
        if label != "full":
            ok = not bad and n == len(a)
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("film")
    ap.add_argument("--ss", type=float, required=True)
    ap.add_argument("--t", type=float, required=True)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--port", type=int)
    g.add_argument("--out")
    ap.add_argument("--verify", action="store_true", help="NAL-compare against ffmpeg, then exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()
    if not (a.verify or a.port or a.out):
        ap.error("one of --port/--out/--verify is required")
    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO, stream=sys.stderr,
                        format="%(asctime)s - %(levelname)s - %(message)s")
    signal.alarm(EXPIRY_S)  # default SIGALRM action terminates: 4 h self-expiry

    hv, w, h, num, den = probe_extradata(a.film)
    lsize, params = parse_hvcc(hv)
    pk = list_packets(a.film, a.ss, a.t)
    if not pk:
        log.error("no packets")
        print("RESULT: records=0 ok=no")
        return 1
    log.info("%dx%d %d/%d lsize=%d params=%dB packets=%d first_pts_us=%d", w, h, num, den, lsize,
             len(params), len(pk), pk[0][0])
    header = b"FELS" + struct.pack("<IIIII", 1, w, h, num, den)

    if a.verify:
        recs = list(records(a.film, pk, lsize, params))
        ok = verify(a.film, a.ss, a.t, recs)
        print(f"RESULT: records={len(recs)} ok={'yes' if ok else 'no'}")
        return 0 if ok else 1

    if a.out:
        sink: BinaryIO = open(a.out, "wb")
    else:
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("0.0.0.0", a.port))
        srv.listen(1)
        log.info("listening on %d", a.port)
        conn, addr = srv.accept()
        log.info("client %s", addr)
        sink = conn.makefile("wb")
    n = 0
    try:
        sink.write(header)
        for r in records(a.film, pk, lsize, params):
            sink.write(r)
            n += 1
        sink.flush()
    except (BrokenPipeError, ConnectionResetError) as exc:
        log.warning("client went away: %r", exc)
    finally:
        sink.close()
    print(f"RESULT: records={n} ok=yes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
