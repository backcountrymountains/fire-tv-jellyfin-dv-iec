#!/usr/bin/env bash
# Builds the Dolby Vision trim test clips described in docs/dv-metadata-test.md: one HDR10 still frame, encoded
# once, with a different generated RPU per clip.
#
# Usage: make-dv-trim-clips.sh <hdr10-video> <timestamp> <out-dir> [seconds]
#   <hdr10-video>  any PQ/BT.2020 10-bit source ffmpeg can read
#   <timestamp>    where to take the still frame, e.g. 00:15:00
# Needs: ffmpeg with libx265, dovi_tool (https://github.com/quietvoid/dovi_tool), mkvmerge, python3.
set -euo pipefail

SRC=$1 TS=$2 OUT=$3 SECS=${4:-60}
FPS_NUM=24000 FPS_DEN=1001
FRAMES=$(( SECS * FPS_NUM / FPS_DEN ))
DOVI=${DOVI_TOOL:-dovi_tool}
for t in ffmpeg "$DOVI" mkvmerge python3; do command -v "$t" >/dev/null || { echo "missing: $t" >&2; exit 1; }; done
mkdir -p "$OUT/work"
W=$OUT/work

echo "1/4 still frame at $TS"
ffmpeg -nostdin -v error -y -ss "$TS" -i "$SRC" -frames:v 1 -pix_fmt yuv420p10le -f rawvideo "$W/still.yuv"
if [ ! -s "$W/still.yuv" ]; then   # raw elementary streams have no index: decode up to the timestamp instead
  ffmpeg -nostdin -v error -y -i "$SRC" -ss "$TS" -frames:v 1 -pix_fmt yuv420p10le -f rawvideo "$W/still.yuv"
fi
[ -s "$W/still.yuv" ] || { echo "no frame at $TS" >&2; exit 1; }
SIZE=$(ffprobe -v error -select_streams v:0 -show_entries stream=width,height -of csv=s=x:p=0 "$SRC")

echo "2/4 encode ${SECS}s ($FRAMES frames) as HDR10"
ffmpeg -nostdin -v error -y -f rawvideo -pix_fmt yuv420p10le -s "$SIZE" -r "$FPS_NUM/$FPS_DEN" \
  -stream_loop $(( FRAMES - 1 )) -i "$W/still.yuv" -frames:v "$FRAMES" -c:v libx265 -preset fast -crf 16 \
  -x265-params "keyint=24:min-keyint=24:repeat-headers=1:hdr10=1:hdr10-opt=0:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:master-display=G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1):max-cll=0,0:log-level=error" \
  -f hevc "$W/base.hevc"

echo "3/4 generator configs"
python3 - "$W" "$FRAMES" <<'PY'
import json, sys
w, n = sys.argv[1], int(sys.argv[2])
L2_TARGETS = [2081, 2851, 3079, 3388, 3696]          # 100, 600, 1000, 2000, 4000 nits (12-bit PQ)
L8_TARGETS = [1, 27, 37, 48, 49]                       # dovi_tool allows at most 5 L8 blocks
NEUTRAL = dict(trim_slope=2048, trim_offset=2048, trim_power=2048, trim_chroma_weight=2048,
               trim_saturation_gain=2048, ms_weight=2048)

def l2(**k):
    return [{"Level2": dict(NEUTRAL, target_max_pq=t, **k)} for t in L2_TARGETS]

def l8(**k):
    b = dict(NEUTRAL, length=25, target_mid_contrast=2048, clip_trim=2048)
    for i in range(6):
        b[f"saturation_vector_field{i}"] = 128
        b[f"hue_vector_field{i}"] = 128
    b.update(k)
    return [{"Level8": dict(b, target_display_index=t)} for t in L8_TARGETS]

def gen(name, cm="V29", src_nits=1000, blocks=()):
    j = {"cm_version": cm, "length": n, "source_min_pq": 7, "source_max_pq": 3696 if src_nits == 4000 else 3079,
         "level6": {"max_display_mastering_luminance": src_nits, "min_display_mastering_luminance": 1,
                    "max_content_light_level": 0, "max_frame_average_light_level": 0},
         "shots": [{"start": 0, "duration": n,
                    "metadata_blocks": [{"Level1": {"min_pq": 0, "max_pq": 2442, "avg_pq": 819}}, *blocks]}]}
    json.dump(j, open(f"{w}/{name}.json", "w"), indent=1)

gen("A_L1_only")
gen("B_4000nit_L1_only", src_nits=4000)
gen("L2_slope_half", blocks=l2(trim_slope=1024))
gen("L2_saturation_0", blocks=l2(trim_saturation_gain=0))
gen("L2_saturation_max", blocks=l2(trim_saturation_gain=4095))
gen("L2_chroma_weight_0", blocks=l2(trim_chroma_weight=0))
gen("L2_chroma_weight_max", blocks=l2(trim_chroma_weight=4095))
gen("E0_CM40_L8_neutral", cm="V40", blocks=l2() + l8())
gen("L8_slope_half", cm="V40", blocks=l2() + l8(trim_slope=1024))
gen("L8_saturation_0", cm="V40", blocks=l2() + l8(trim_saturation_gain=0))
gen("L8_sat_vectors_0", cm="V40", blocks=l2() + l8(**{f"saturation_vector_field{i}": 0 for i in range(6)}))
PY

echo "4/4 RPUs, inject, verify, mux"
mkvmerge -q -o "$OUT/C0_HDR10_no_RPU.mkv" --default-duration "0:${FPS_NUM}/${FPS_DEN}p" "$W/base.hevc"
for cfg in "$W"/*.json; do
  name=$(basename "$cfg" .json)
  "$DOVI" generate -j "$cfg" -o "$W/$name.rpu" >/dev/null
  "$DOVI" inject-rpu -i "$W/base.hevc" --rpu-in "$W/$name.rpu" -o "$W/$name.hevc" >/dev/null
  "$DOVI" extract-rpu -i "$W/$name.hevc" -o "$W/$name.check.rpu" >/dev/null
  cmp -s "$W/$name.rpu" "$W/$name.check.rpu" || { echo "RPU round trip failed: $name" >&2; exit 1; }
  mkvmerge -q -o "$OUT/DV_$name.mkv" --default-duration "0:${FPS_NUM}/${FPS_DEN}p" "$W/$name.hevc"
  rm -f "$W/$name.hevc" "$W/$name.check.rpu"
  echo "  DV_$name.mkv"
done
echo "done: $(ls "$OUT"/*.mkv | wc -l) clips in $OUT (work files in $W)"
