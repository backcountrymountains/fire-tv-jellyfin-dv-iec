"""Build felfix.ko — the Fire OS FEL-enable kprobe module — without kernel headers.
Steps: copy the __versions section of a GPL module from the same device (e.g. amvdec_h265.ko) into a generated
header, compile felfix.c with `zig cc -target aarch64-freestanding-none` (kernel-style flags: no PIC, x18 reserved for
the shadow call stack, no FP/SIMD, no stack protector), then patch the felfix_addr[] placeholders with addresses from
the device's /proc/kallsyms. Data symbols (cur_dispbuf2, vd_layer[0], vd_layer[1]) are not in kallsyms: their
link-time addresses, read from the Fire OS 7.6.4.6 boot Image disassembly, are shifted by the same offset as
dv_toggle_frame.
Usage: python3 build_felfix.py <kallsyms.txt|-> <module.ko from the device> <zig> <out.ko>
(`-` = template: placeholders left in, patched on the device at boot by felfix_reloc.c)
"""
from __future__ import annotations

import logging
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

log = logging.getLogger("build_felfix")

SRC = Path(__file__).resolve().parent / "felfix.c"
# Order must match the A_* enum in felfix.c.
KSYMS = ("register_kprobe", "unregister_kprobe", "dv_toggle_frame", "VSYNC_WR_DV_REG",
         "VSYNC_WR_DV_REG_BITS", "cur_dispbuf2", "printk", "set_layer_display_canvas",
         "vd_layer0", "vd_layer1", "VSYNC_WR_MPEG_REG_BITS", "aml_write_dosbus",
         "kallsyms_lookup_name", "vf_peek", "vf_get", "vf_put", "vf_notify_provider_by_name", "amdv_vf_put", "dv_vf_light_reg_provider", "amdv_parse_metadata_v2_stb",
         "vf_get_provider", "vf_get_provider_by_name",
         "recv_common_dequeue_frame", "vf_notify_provider")
DV_TOGGLE_LINK = 0xFFFFFFC010A3C358
# Data symbols (not in kallsyms): link-time addresses from the boot Image disassembly
# (cur_dispbuf2: dv_toggle_frame+0xe0 store; vd_layer[0]/[1]: config_dvel_position and
# vd_set_dcu(1, ...) call sites in vsync_isr_in).
DATA_LINK = {"cur_dispbuf2": 0xFFFFFFC011ACA780, "vd_layer0": 0xFFFFFFC011ACEFD8,
             "vd_layer1": 0xFFFFFFC011ACF678}
MAGIC = 0xFE1F000000000000
CFLAGS = ["-target", "aarch64-freestanding-none", "-c", "-O2", "-fno-pic", "-fno-pie", "-mcmodel=small",
          "-ffixed-x18", "-mgeneral-regs-only", "-fno-stack-protector", "-fno-jump-tables",
          "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-ffreestanding", "-fno-builtin",
          "-fno-common", "-nostdinc", "-g0", "-Wall", "-Werror"]


def read_kallsyms(path: Path) -> dict[str, int]:
    syms: dict[str, int] = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in "Tt":
            syms.setdefault(parts[2], int(parts[0], 16))
    return syms


def section_bytes(ko: Path, name: str) -> bytes:
    data = ko.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    def hdr(i: int) -> tuple[int, int, int]:
        n, _t, _f, _a, off, size = struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)
        return n, off, size
    stroff = hdr(shstrndx)[1]
    for i in range(shnum):
        n, off, size = hdr(i)
        if data[stroff + n:data.index(0, stroff + n)].decode() == name:
            return data[off:off + size]
    raise ValueError(f"{ko}: no section {name}")


def main(argv: list[str]) -> int:
    logging.basicConfig(level=logging.DEBUG, stream=sys.stderr,
                        format="%(asctime)s - %(levelname)s - %(message)s")
    commit = subprocess.run("git rev-parse --short HEAD 2>/dev/null || echo no-git", shell=True,
                            capture_output=True, text=True, timeout=10).stdout.strip()
    log.info("commit %s", commit)
    if len(argv) != 5:
        print(__doc__)
        print("RESULT: status=usage")
        return 2
    kallsyms, vs12, zig, out = Path(argv[1]), Path(argv[2]), argv[3], Path(argv[4])
    template = str(kallsyms) == "-"
    syms = {} if template else read_kallsyms(kallsyms)
    slide, addrs = 0, []
    try:
        if template:
            log.info("template build: placeholders left for felfix_reloc.c")
        else:
            slide = syms["dv_toggle_frame"] - DV_TOGGLE_LINK
            addrs = [DATA_LINK[s] + slide if s in DATA_LINK else syms[s] for s in KSYMS]
    except KeyError as exc:
        log.error("symbol missing from kallsyms: %s", exc)
        print("RESULT: status=fail error=missing_symbol")
        return 1
    if not template:
        log.info("kaslr slide %#x", slide)
    for s, a in zip(KSYMS, addrs):
        log.info("%-24s %#x", s, a)

    versions = section_bytes(vs12, "__versions")
    with tempfile.TemporaryDirectory() as tmp:
        hdr = Path(tmp) / "felfix_versions.h"
        hdr.write_text("__attribute__((section(\"__versions\"), aligned(8), used))\n"
                       "static const unsigned char felfix_versions[] = {"
                       + ",".join(str(b) for b in versions) + "};\n")
        obj = Path(tmp) / "felfix.o"
        cmd = [zig, "cc", *CFLAGS, *os.environ.get("FELFIX_DEFINES", "").split(), "-I", tmp, "-o", str(obj), str(SRC)]
        log.debug("compile: %s", " ".join(cmd))
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if res.returncode != 0:
            log.error("compile failed:\n%s", res.stderr)
            print("RESULT: status=fail error=compile")
            return 1
        ko = bytearray(obj.read_bytes())

    for i, a in enumerate([] if template else addrs):
        pat = struct.pack("<Q", MAGIC + i)
        n = ko.count(pat)
        if n != 1:
            log.error("placeholder %d found %d times", i, n)
            print("RESULT: status=fail error=placeholder")
            return 1
        ko[ko.index(pat):ko.index(pat) + 8] = struct.pack("<Q", a)
    out.write_bytes(ko)
    und = subprocess.run(["readelf", "-sW", str(out)], capture_output=True, text=True, timeout=30).stdout
    undefined = [l.split()[-1] for l in und.splitlines() if " UND " in l and len(l.split()) >= 8]
    log.info("undefined symbols: %s", undefined or "none")
    if undefined:
        print(f"RESULT: status=fail error=undefined_symbols n={len(undefined)}")
        return 1
    print(f"RESULT: status=ok out={out} bytes={len(ko)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
