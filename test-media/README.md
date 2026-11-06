# Test media

`make-dv-trim-clips.sh <hdr10-video> <timestamp> <out-dir> [seconds]` builds the clip set used in
[../docs/dv-metadata-test.md](../docs/dv-metadata-test.md). It takes one still frame from any HDR10 source, encodes
it once as HDR10 (x265), and injects a different generated RPU into each copy:

- `C0_HDR10_no_RPU`: control, no Dolby Vision metadata
- `DV_A_L1_only` and `DV_B_4000nit_L1_only`: baselines with a 1000-nit and a 4000-nit mastering display
- `DV_L2_*`: CM v2.9 trims (slope, saturation gain, chroma weight) at five target displays
- `DV_E0_CM40_L8_neutral` and `DV_L8_*`: CM v4.0 trims (slope, saturation gain, saturation vectors)

The script checks every RPU by extracting it again and comparing it byte for byte. Requires ffmpeg with libx265,
[dovi_tool](https://github.com/quietvoid/dovi_tool), mkvmerge and python3.

No clips are included: the source frame should be your own HDR10 material.
