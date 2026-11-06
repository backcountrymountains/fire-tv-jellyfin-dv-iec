# Does the Dolby Vision engine apply the metadata, or only show the HDR10 base layer?

On a TV without Dolby Vision, the Fire TV Cube 3's DV engine renders DV content to HDR10. The HDMI output is
HDR10 whether or not the engine uses the RPU (the dynamic metadata), and the kernel only reports that a DV source
is active (`src_format = DV FORMAT_DOVI`). This test checks whether the metadata changes the pixels.

## Method

- **One picture, many RPUs.** One HDR10 still frame (3840x1604, PQ/BT.2020, 10-bit) was looped for 60 s and
  encoded once with x265 as HDR10. RPUs were generated with `dovi_tool generate` and injected with
  `dovi_tool inject-rpu`, then muxed with mkvmerge (profile 8.1, `dvvC` present). The clips differ **only** in
  their RPU. Every RPU was extracted again and compared byte for byte with what was generated.
  `../test-media/make-dv-trim-clips.sh` rebuilds the set from any HDR10 still.
- **Playback.** Each clip was played in the app through the Dolby Vision decoder (`dvhe.08.06`, confirmed in the
  log). The server's now-playing item was recorded with every capture.
- **Measurement.** A camera photographed the TV 20 s into each clip. The camera was fixed: colour mode, manual
  white balance, fixed exposure and gain. Each play started after 5 s of black so the TV's brightness limiter
  started from the same state. Mean luma and mean chroma (max − min of RGB) were measured in four regions:
  the whole picture, skin, a bright window and a shirt.
- **Controls.** Each variant alternated with its baseline (A, X, A, Y, A …). Repeated baseline runs drifted
  by less than 3 %.

TV: Samsung S89C (HDR10/HDR10+, no DV). Device: Fire TV Cube 3rd gen (AFTGAZL), Fire OS 7.6.4.6. The Cube reports
DV capability through a local system modification; on a stock Cube attached to a non-DV TV this path is not used.

## Results

Baseline A: CM v2.9, L1 only (min 0, max 2442, avg 819 in 12-bit PQ), L6 1000 nits. Baseline E0: CM v4.0 with
neutral L2 and L8 blocks. Trims were applied at every target, for both CM v2.9 L2 (100, 600, 1000, 2000 and 4000
nits) and CM v4.0 L8 (target display indexes 1, 27, 37, 48 and 49).

| Variant (vs baseline) | Whole-picture luma | Skin luma | Skin chroma |
|---|---|---|---|
| L2 `trim_slope` 1024 (vs A) | 46.8 → 32.8 (−30 %) | 45 → 27 | 38 → 13 |
| L2 `trim_saturation_gain` 0 / 4095 (vs A) | no change | no change | no change |
| L2 `trim_chroma_weight` 0 / 4095, and 4095 + saturation 0 (vs A) | no change | no change | no change |
| L6 / source max 4000 nits instead of 1000, L1 only | 46.8 → 46.0 | 45 → 40 (−10 %) | 38 → 32 |
| L8 `trim_slope` 1024 (vs E0) | 47.0 → 21.5 (−54 %) | 46.8 → 17.6 | 39.8 → 0.8 |
| L8 `trim_saturation_gain` 0 (vs E0) | 47.0 → 46.5 | 46.8 → 45.6 | 39.8 → 32.0 (−20 %) |
| L8 saturation vectors 0 instead of 128 (vs E0) | no change | no change | no change |
| HDR10, no RPU, HEVC decoder (vs A) | no change | no change | no change |

"No change" means within 1 % of the neighbouring baseline runs. Values are 8-bit camera code values, not nits.

## Conclusions

- **The engine applies the RPU.** Brightness trims change the picture in both CM v2.9 (L2) and CM v4.0 (L8), and
  the L8 saturation trim reduces chroma. So the DV path is not just the HDR10 base layer, and converting
  profile 7 RPUs to 8.1 changes what the viewer sees.
- **Not every trim field is used.** L2 saturation gain and chroma weight had no effect, and neither did the L8
  saturation vectors (0 against 128). These may be unused in this engine and output mode, or 128 may not be the
  neutral value. Not resolved.
- With a 1000-nit master and HDR10 output, an L1-only RPU looks the same as plain HDR10. That is expected, since
  there is nothing to map.

## Pitfall that produced false negatives

The first two rounds measured the app's **home screen**: after a Stop with another activity in front, the app
silently ignored the server's PlayNow command. The server still answered 204, but no websocket message arrived.
Every variant then "matched" the baseline. Record what is actually playing with every capture, and look at the
frames.
