# Dolby Vision FEL on a rooted Fire TV Cube 3 (FireOS)

Code: `../fel/` (kernel fixes, player core, boot module) and `../patches/fel/` (jellyfin-androidtv glue).

## Read this first: who this is for

This is **not** a Jellyfin feature other people can use. It only works on:

- a Fire TV Cube 3rd gen (AFTGAZL) with an **unlocked bootloader and root** (Magisk), which most Fire TVs cannot get;
- **Fire OS 7.6.4.6** exactly. The kernel module patches fixed instruction offsets in Amazon's kernel and decoder
  modules, checks the bytes before patching, and refuses to load (or skips that fix) on any other build;
- a Dolby Vision core that accepts FEL in place of Amazon's. DV cores are proprietary Dolby binaries; none is
  included, and nothing here modifies one. See "Prerequisite: a FEL-capable DV core" below.

The Jellyfin part is about 520 lines of glue. Everything that makes FEL work is Amlogic- and Fire-OS-specific.
What is reusable elsewhere is mostly the findings: how to tell FEL from MEL, how to check that the two layers are
paired correctly, and three bugs in this kernel's dual-layer path (one of them in Amazon's decoder build).

For profile 7 on an unrooted device, use the profile 7 → 8.1 conversion in `dolby-vision.md`. It is lossless for
MEL and drops the enhancement layer for FEL.

## FEL vs MEL, and which films need this

Profile 7 (UHD Blu-ray) carries a 4K base layer (BL), a 1080p enhancement layer (EL) and the RPU. In a **MEL** file
the EL residual is neutral and the RPU applies none of it: BL + RPU gives the same picture, so dropping the EL loses
nothing (the Amlogic driver detects MEL from the RPU, logs "bypass MEL" and stops the EL decoder). In a **FEL** file
the EL carries real picture data.

A survey of 15 profile-7 films from one library (`dovi_tool` summary plus the EL residual over three 10 s windows per film): 7 MEL,
7 FEL (2 with a large EL, 1 medium, 4 small), 1 mislabelled (the container says P7, the stream has no RPU or EL).

## Why stock Fire OS cannot play FEL

- The OMX Dolby Vision decoder takes the BL only. A FEL file played identically to the same file converted with
  `dovi_tool -m 2 convert --discard` (whole-image camera ratio 1.001, correlation 0.998), and EL-only test content
  (a figure that exists only in the EL of the Kodi wiki FEL sample) never appears.
- The kernel *can* build the dual decoder through the legacy stream port (`/dev/amstream_dves_hevc`, the path Kodi
  uses on CoreELEC): two `amvdec_h265` instances (BL 3840, EL 1920), and the DV module pairs them. But Amazon's DV
  core (`dovi_vs12.ko`) returns -2 for every FEL frame (green screen).
- Even with a DV core that accepts FEL, the kernel never composes the EL: four display-path gaps (below).

## What the pieces do

| Piece | Path | What it does |
|---|---|---|
| DV core | not included | must accept FEL; Amazon's does not (prerequisite below) |
| felfix | `fel/felfix/` | kprobe module (no kernel headers; addresses patched in from `/proc/kallsyms` at boot by `felfix_reloc.c`) |
| libfelplay | `fel/felplay/` | drives the dual-layer stream port: open, write, pause, reset, clock (`pts_pcrscr`), state readback; JNI front end for the app; `felplay-cli` + `felsend.py` for testing without the app |
| Boot module | `fel/magisk-fel/` | swaps the DV core, loads felfix, sets decoder parameters, SELinux rules for the app |
| App glue | `patches/fel/` | `FelVideoRenderer`: Media3 keeps audio, subtitles and the UI; FEL video goes to the stream port |

### felfix kprobes

| # | Fixes | Notes |
|---|---|---|
| 1 | `dv_toggle_frame` only fetches the EL when not in "multi" mode, and the stb 2.6 cores always run multi | take the EL-fetch branch |
| 2 | core1a register writes lack the composer / EL bits | rewritten with CoreELEC's values |
| 3 | the T7 EL routing (VD2 path) is never set | rewritten |
| 4 | the EL canvas is set on video layer 0 instead of 1, and `VPP_VD2_DSC_CTRL` bit 4 (VD2 bypass) stays 1 | found by diffing register dumps of the same clip on CoreELEC and Fire OS; with bit 4 left at 1, real FEL content is black or green |
| 5 | the EL (slave) decoder never gets its stream write pointer programmed on parserless SoCs (T7), loops on "h265 data skipped" and stalls the play | SamuriHL media_modules 0002, ported; seeks: 0 stalls in 10 vs 3 in 19 without it |
| 6-8 | EL queue realign by put counts (SamuriHL common_drivers 0013) | **off**: hung the SoC on this kernel and retires correct ELs |
| 9-10 | pair BL and EL by picture order count instead of queue position, skip the vendor's own realign | **off by default**: correct in ~3000 traced pairs, but made unnecessary by 11 |
| 11 | Amazon's `vh265_work_implement` reads `switch_dvlayer_flag` (hand the turn to the other layer) but never clears it | see below; this is the fix that made starts and seeks pair correctly |

Build: `python3 fel/felfix/build_felfix.py - <amvdec_h265.ko from the device> <zig 0.14> felfix-template.ko`, and
`zig cc -target arm-linux-musleabihf -static -O2 -o felfix-reloc fel/felfix/felfix_reloc.c`. The builder only copies
module version metadata from a GPL module on the device (felfix imports no symbols); a build from `amvdec_h265.ko`
loads and passes the same pairing test as the deployed build.

## The bug that broke pairing after every start and seek

Symptom: after any start or seek, the EL decoder lost 3-11 pictures of the first GOP, so the queue-position pairing
put every later BL with a wrong EL for the rest of the play (about 170 of 172 pairs wrong at a typical start), with
zero "not found el" errors. The kernel logs "Error config_mc_buffer, 0th poc (N) not in list1": a reference picture
was missing.

Cause, from the decoder's status log and a disassembly of Amazon's `amvdec_h265.ko`:

1. At a start the decoders burst faster than the input arrives, and the EL runs short mid-picture
   (BUFEMPTY → `DEC_RESULT_AGAIN`, `restore_decode_state` rewinds it).
2. The two layers take turns on one input. A decoder hands over the turn when `switch_dvlayer_flag` is set. Both
   Amlogic reference sources clear the flag where it is read (`if (flag) { flag = 0; ... }`); Amazon's build never
   clears it there, so it still holds the value from the EL's previous, completed picture.
3. So on the AGAIN the turn goes to the BL, which starts at the EL picture's rewound position, skips its EL data
   and decodes ahead. That EL picture is gone, and the pictures that reference it fail.

It only happens on an input underrun, so it is timing-dependent: turning on the decoder's buffer-manager debug
printing slows it enough to hide the bug (0 of 3 runs dropped pictures, against 2 of 2 without it).

Fix: kprobe 11 zeroes the flag right after the read. Results: command-line starts 6/6 clean (every unpatched run
on the same boot dropped pictures, 9/9); in the app, 6 films × (start + 3 seeks) = 24/24 plays with 0 wrong pairs
(~2900 pairs traced), and 11/11 again after a cold boot. Profile 8 plays are unaffected (the flag is only ever set
when a decoder has a dual-layer partner; counter confirmed 0).

## How to check pairing (and how not to)

- **Not** by counting "not found el": positional pairing can be wrong on every frame with zero misses.
- Fire OS: `echo 0x4000 > /sys/module/amvdec_h265/parameters/debug` prints each `vh265_vf_get` with its POC
  (`[0]` = BL, `[1]` = EL); the EL get right after each BL get is its pair. Never use `0x400000` (per-byte DV
  printing in the vsync interrupt resets the device).
- CoreELEC: get order is *not* pairing order (the receiver queues BLs ahead). Use `debug_dolby 2` ("get bl(vf)
  with el(vf)") and join both vf pointers to their POCs. Capture with a `/dev/kmsg` reader on the device:
  `ssh … cat /proc/kmsg` drops lines and invents mismatches.

## CoreELEC as the reference

On the same Cube, CoreELEC 22 (5.15 kernel) with Kodi pairs correctly from the first picture (546/546 and 423/423
at a start that broke on Fire OS; 396/397 at another start that sometimes froze on Fire OS), with
no missing references in a buffer-manager trace. Its decoder clears the flag. It also keeps the input full:
in stream mode Kodi holds `pre_decode_buf_level` at a tenth of the ring until the ring is 90 % full. CoreELEC's
kernel does show the slave write-pointer stall (fix 5) after long seeks, and Kodi recovers by re-creating the
decoder.

## Pacing and the app

- Video is paced by the kernel: tsync on, and `pts_pcrscr` written every 20-100 ms as the playback clock relative to
  the first picture written since the port was (re)opened. Feeding the absolute clock makes the display race ~2.4×
  to catch up. A seek closes and reopens the port and restarts on a keyframe.
- The app also needs: SELinux allow rules plus `mlstrustedobject` on the device nodes and sysfs (`sepolicy.rule`);
  `/sys/class/video/video_global_output` = 1 (0 after boot: decoded and paced, black screen); `disable_video` reset
  after every port close; a SurfaceView with no buffer as a hole, because the video plane is under the Android UI.
- A/V: no gross offset (flash/beep clip within the normal player's run-to-run spread); fine offset not measured.

## Prerequisite: a FEL-capable DV core

Amazon's DV core (`dovi_vs12.ko`) returns -2 for every FEL frame. FEL composes only with a core that accepts it
(this work used CoreELEC's generic DV module on the same device). DV cores are proprietary Dolby binaries: none is
included here, nothing here modifies one, and `fel/magisk-fel/service.sh` does not install one. If a core is
replaced at runtime, do it with DV disabled: `rmmod dovi` while DV is enabled panics this kernel.

## Status and open items

- Plays FEL with the EL composed and correctly paired after starts and seeks, on one device, one TV (Samsung S89C,
  via the DV engine to HDR10), the server listed in the top-level README.
- Not done: fine A/V offset; whether CoreELEC's core renders profile 8 differently from Amazon's (camera A/B);
  the player's info panel shows no decoder for FEL plays; a start freeze seen before fix 11 has not reappeared in
  7 starts, but it was never proven to be the same bug.
- `patches/fel/` apply to our local jellyfin-androidtv branch, which also carries the `dovi` and `iec` series and
  other local patches; they are not a standalone series against upstream master.
