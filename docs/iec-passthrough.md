# Lossless TrueHD / DTS-HD passthrough in jellyfin-androidtv (IEC 61937 packing in the app)

Patch: `../patches/iec/0001-*.patch`, one commit on jellyfin-androidtv master `7ec47a001`. It also applies
cleanly to `7ada0ae80` (2026-11-06).

## Problem

- **Media3 doesn't pack these formats.** It passes TrueHD and DTS-HD to Android as raw encodings
  (`ENCODING_DOLBY_TRUEHD`, `ENCODING_DTS_HD`), and the device's audio HAL decides what reaches the receiver.
  Media3 has no IEC 61937 output of its own (`AudioCapabilities`: "Skip the IEC61937 encapsulation because we
  don't support it yet").
- **On a Fire TV Cube 3 with a Denon AVR-S760H, stock app:**
  - TrueHD → the receiver shows Dolby Digital Plus (a lossy re-encode).
  - DTS-HD MA → DTS core, or no audio at all (AudioTrack init `-38` on every retry with Media3 1.11.1; this may be
    androidx/media #3487, fixed for 1.12, not yet re-tested).
- **Kodi on the same box is lossless,** because it packs IEC 61937 itself and opens the AudioTrack as
  `ENCODING_IEC61937`.

## What the patch does

- **Hook:** a Media3 `AudioOutputProvider` (a public extension point since Media3 1.11; no Media3 fork). It claims
  TrueHD, DTS-HD and DTS and plays them through its own `AudioOutput`.
- **Packing:**
  - TrueHD goes into MAT frames (61424/61440 bytes, padded from the TrueHD timing fields).
  - DTS-HD and DTS go into type IV bursts at the 768 kHz rate (32768 bytes per 512 samples).
- **Output:** an `ENCODING_IEC61937` AudioTrack at 192 kHz with the 7.1 channel mask.
- **Setting:** off by default; when on, it is honoured by the device profile's AUTO mode. Hooked into both the
  legacy player (`VideoManager`) and the new one (`ExoPlayerBackend`).
- **Clock:** the IEC track's timestamps are smoothed. The HAL's timestamps jump about +44 ms roughly once a
  minute.
- **Seeks:** a released IEC track is parked for a few seconds and reused, so the receiver doesn't re-lock on every
  seek. Media3 releases the output on every flush.

## Results

- **At the receiver:** "DTS-HD MSTR", "DTS:X MSTR", "Dolby Audio - TrueHD" (Atmos titles too) and "DTS". E-AC-3 is
  unchanged.
- **Tests:** the packer output is byte-identical to FFmpeg `-f spdif`. Unit-test fixtures are synthetic 440 Hz
  sines made with FFmpeg; real 20 s DTS-HD MA and TrueHD Atmos excerpts were also checked but aren't included.
- **Lint:** detekt and Android lint report 0 issues in the new files.
- **Playback:** 8-minute plays with 0 underruns; seeks recover.
- **A/V sync:** measured with a camera plus a microphone, in the same range as Kodi on the same box. That's after
  adding an output latency the platform does not report: about 50 ms for TrueHD and 100 ms for DTS, measured on
  this one device.

## Known limits

- Playback speed stays 1x while bitstreaming.
- One unreproduced TrueHD failure after a very early seek.
- DTS sync on a reused track is bimodal (two states about 27 ms apart).
- Tested on one device and one receiver only.

## Licensing

The packer is derived from FFmpeg's `libavformat/spdifenc.c` (LGPL-2.1-or-later). Its attribution header names
Bartlomiej Wolowiec, Anssi Hannula and Carl Eugen Hoyos. LGPL code can go into the GPL-2.0 app as-is.
Contributing the same idea to Media3 (Apache-2.0) would need a fresh implementation; see `media3-iec61937.md`.

## Related

- jellyfin-androidtv #5593 and #281 ask for lossless audio.
- Open PR #5892 adds DTS-HD mime mapping and device-profile entries but no packing, so it's complementary.
- androidx/media #3329: another app packing TrueHD → MAT on an Amlogic box.

## Questions for the maintainers

1. Is in-app packing acceptable, given it only helps devices whose HAL can't pass these formats (Fire TV, many
   Amlogic boxes), while Shield-class devices don't need it?
2. Opt-in setting (as now) or auto-detect?
3. Is parking the AudioTrack across seeks acceptable?
4. How should the unreported HAL latency be handled: a default of 0 plus the existing A/V offset, or per-format
   defaults?
5. Should this wait for, or build on, #5892?
