# cube-fel (Magisk module)

Files for `/data/adb/modules/cube-fel/`:
- `service.sh` — loads felfix, sets `start_decode_buf_level` 0, `nal_skip_policy` 0, `enable_mel`/`enable_fel` 1,
  makes `video_global_output` writable for the app. Log: `service.log`.
- `sepolicy.rule` — lets the app (untrusted_app) use the amstream/amvideo nodes and tsync/video sysfs, including the
  `mlstrustedobject` attributes (without them MLS denies the app even with the allow rules). On the Cube this was
  developed on, Magisk did not load module `sepolicy.rule` files at boot ("Unable to find preinit dir"), so the rule
  was applied at post-fs-data with `magiskpolicy --live --apply sepolicy.rule`. Check how your setup handles it.
- `felfix-template.ko` — `python3 ../felfix/build_felfix.py - <amvdec_h265.ko from the device> <zig> felfix-template.ko`
- `felfix-reloc` — `zig cc -target arm-linux-musleabihf -static -O2 -o felfix-reloc ../felfix/felfix_reloc.c`

Not included and not handled: a Dolby Vision core that accepts FEL (see `../../docs/fel-fire-cube.md`).
