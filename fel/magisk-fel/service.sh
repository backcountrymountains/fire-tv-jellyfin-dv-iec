#!/system/bin/sh
# FEL boot setup for a rooted Fire TV Cube 3 (Fire OS 7.6.4.6). Loads felfix (relocated for this boot by
# felfix-reloc) and sets the decoder / DV parameters the FEL player needs.
# Prerequisite NOT handled here: a Dolby Vision core that accepts FEL. Amazon's dovi_vs12.ko rejects every FEL frame
# (green screen); this module does not provide or modify a DV core. See docs/fel-fire-cube.md.
MODDIR=${0%/*}
LOG=$MODDIR/service.log
# Magisk was seen running service.sh twice in parallel on a first boot: mkdir is an atomic lock (tmpfs /dev).
mkdir /dev/cube-fel.lock 2>/dev/null || exit 0
exec > "$LOG" 2>&1 < /dev/null
echo "$(date) start"
until [ "$(getprop sys.boot_completed)" = 1 ]; do sleep 2; done
sleep 10
H=/sys/module/amvdec_h265/parameters
if lsmod | grep -q '^felfix'; then
  echo "felfix already loaded"
else
  echo 0 > /proc/sys/kernel/kptr_restrict
  "$MODDIR/felfix-reloc" "$MODDIR/felfix-template.ko" /dev/felfix-boot.ko
  r=$?
  echo 2 > /proc/sys/kernel/kptr_restrict
  [ $r = 0 ] || { echo "felfix-reloc failed rc=$r"; exit 1; }
  insmod /dev/felfix-boot.ko; echo "felfix rc=$?"
fi
echo 0 > $H/start_decode_buf_level
echo 0 > $H/nal_skip_policy
echo enable_mel 1 > /sys/class/amdolby_vision/debug
echo enable_fel 1 > /sys/class/amdolby_vision/debug
# the FEL player switches the video layer on for a FEL play; root 0664 by default
chmod 666 /sys/class/video/video_global_output
echo "params: $(cat $H/start_decode_buf_level) $(cat $H/nal_skip_policy)"
echo "$(date) done"
