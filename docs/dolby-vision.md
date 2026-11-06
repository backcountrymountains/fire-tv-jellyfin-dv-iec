# Dolby Vision in jellyfin-androidtv on a Fire TV Cube 3

Patches: `../patches/dovi/` (2 commits on jellyfin-androidtv master `7ec47a001`). Native shim:
`../native/doviconv/`. Evidence that the device applies the metadata: `dv-metadata-test.md`.

## Setup and an important caveat

Fire TV Cube 3rd gen (AFTGAZL, Amlogic, Fire OS 7.6.4.6 = Android 9 / API 28) on a Samsung S89C (HDR10 and
HDR10+, **no Dolby Vision**). The Cube uses its DV decoder and engine here only because a local system
modification makes it report DV capability. The engine then renders DV to HDR10 for the TV. A stock Cube on a
non-DV TV does not take this path, and the patches are untested on a DV TV.

## 1. Profile 7 (UHD Blu-ray)

- **Default:** `dovi_profile_7_override` = AUTO leaves DOVIWithEL out of the device profile. The server then
  transcodes (`TranscodeReasons=VideoRangeTypeNotSupported`) and the output is SDR.
- **With the override on:** the file direct-plays, but the DV decoder rejects profile 7 and only the HDR10 base
  layer is shown.
- **Patch 1:** a `MediaCodecVideoRenderer` subclass (`DvVideoRenderer`). For `dvhe.07` on the DV decoder it:
  - configures the decoder as `DvheSt`;
  - drops the enhancement-layer NALs (type 63);
  - converts each RPU (NAL 62) to profile 8.1 with libdovi's `dovi_convert_rpu_with_mode(rpu, 2)`. These are the
    same calls Kodi's `BitstreamConverter` makes.

  When a DV track lands on a plain HEVC decoder instead, NALs 62/63 are stripped so it plays as HDR10.
- **Result on a ~2 h FEL title:** 11501/11501 RPUs converted, the engine reports `DV FORMAT_DOVI`, 0 dropped
  frames, and the picture is correct on camera.
- **Why converting matters:** the engine does apply the dynamic metadata. Brightness trims carried only in the
  RPU change the picture by 30–55 % on camera (`dv-metadata-test.md`).
- **Native code:** libdovi is Rust, called through a ~30-line C JNI shim. Only armeabi-v7a has been built. See
  `../native/doviconv/BUILD.md`.
- Related: jellyfin-androidtv issue #2807.

## 2. "Dolby Vision files" setting (patch 2)

Some files are hybrid DV profile 8.1 + HDR10+. On a TV without DV, the device's DV → HDR10 render drops the
HDR10+ metadata. The setting chooses how DV files are decoded:

| Option | Decoder | What the TV gets |
|---|---|---|
| Dolby Vision (default) | DV decoder | the device's DV render (HDR10 here) |
| Base layer | HEVC | HDR10, plus HDR10+ when the file has it (kernel `hdmitx_set_hdr10plus_pkt: flag = 1`, TV shows HDR10+) |
| Base layer, HDR10 only | HEVC, HDR10+ SEI (ITU-T T.35) removed | plain HDR10 |

Camera brightness A/B of the three outputs (19 runs, frame-aligned):
- TV tone mapping "Static": all three within 3 %.
- TV tone mapping "Active": the TV lifts static HDR10 about 31 % but HDR10+ only about 6 %.

So the visible difference is mostly the TV's setting.

Unit tests: `Hdr10PlusSeiTests` (SEI removal on synthetic samples), 4/4 passing.

## 3. HDR10+ on Android 9 (no patch; a setting works around it)

- **The check:** `supportsHevcHDR10Plus()` requires API 29 (`isAtLeastQ`). Fire OS 7 is API 28, so AUTO always
  leaves HDR10_PLUS out.
- **The effect:** `VideoRangeTypeNotSupported`, so the server remuxes every HDR10+ title into HLS. The picture is
  unaffected, but direct play and its audio passthrough are lost.
- **The workaround:** `hdr10_plus_override` = ENABLE, and the device outputs HDR10+ correctly.
- **Possible fixes:** a known-defects style entry for AFTGAZL, or probing the decoder's profile levels on API 28.

## Known gaps before this could be a PR

- **Java, not Kotlin.** The rest of the playback module is Kotlin.
- **Legacy player only.** Wired into the legacy player (`VideoManager`) only, not `ExoPlayerBackend`.
- **Renderer rebuilt by hand.** `buildVideoRenderers` rebuilds `MediaCodecVideoRenderer.Builder` and copies its
  settings; anything `DefaultRenderersFactory` adds later would be missed.
- **Per-frame allocations** in the P7 path.
- **Single ABI.** One prebuilt ABI; packaging and building in CI is an open question for the maintainers.
- **Single device and TV** tested.

## Questions for the maintainers

1. Would a native libdovi dependency be acceptable for P7 → 8.1, and how should it be built and shipped
   (alongside the FFmpeg AAR)? Or should the no-native "strip and play as HDR10" part come first?
2. A user setting for "send DV files as", or automatic from the display's HDR capabilities?
3. HDR10+ on API 28: device allow-list, profile-level probing, or leave it to the override?
