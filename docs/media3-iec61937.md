# Feature request for androidx/media: IEC 61937 output for TrueHD / DTS-HD

Draft of an issue for [androidx/media](https://github.com/androidx/media) (Media3 / ExoPlayer). Not yet posted.
Background and the working app-side implementation: `iec-passthrough.md`.

Before posting:
- Re-test stock DTS-HD on Media3 1.12 (or `main`). The "-38 on every attempt" result may be androidx/media #3487
  (no AudioTrack retry on devices reporting under 2 GiB of RAM; the Cube reports 1.93 GiB), fixed by #3488. If
  DTS-HD then plays stock, record what the receiver shows and update the table. The TrueHD → Dolby Digital Plus
  result is not affected.
- The Cube is API 28. The `AudioProfile`-based capability path, including the IEC61937 skip quoted below, is API 31+
  and is not the code path on this device. It is cited only as evidence that the gap is known.

---

Title: **Support IEC 61937 encapsulated output (TrueHD in MAT, DTS-HD type IV) for passthrough on devices whose
platform does not bitstream them losslessly**

---

### [REQUIRED] Use case description

On some Android TV devices, passing TrueHD or DTS-HD MA to the platform as a raw encoding
(`ENCODING_DOLBY_TRUEHD`, `ENCODING_DTS_HD`) does not reach the AV receiver losslessly. The device's audio HAL
decides what happens. Opening the AudioTrack as `ENCODING_IEC61937` and packing the bursts in the app does
deliver the lossless stream on the same hardware.

Measured on a Fire TV Cube 3rd gen (AFTGAZL, Amlogic, Fire OS 7.6.4.6 / Android 9, API 28) over HDMI to a Denon AVR-S760H. Results
are the receiver's own format readout:

| Source track | Media3 1.11.1 stock (raw encoding) | App-side IEC 61937, 192 kHz, 7.1 mask |
|---|---|---|
| TrueHD (Atmos) | AudioTrack enc 14, 48 kHz/6 ch → receiver shows **Dolby Digital Plus** (lossy) | **Dolby TrueHD** (MAT) |
| DTS-HD MA | enc 8 → AudioTrack init fails (-38) on every attempt, silent | **DTS-HD Master Audio** |
| DTS:X | (as DTS-HD MA) | **DTS:X Master** |
| DTS core | DTS | DTS (type IV) |

Kodi on the same device gets the same lossless results, because it packs IEC 61937 itself.

What the app-side packing took:
- DTS-HD: IEC 61937 type IV, 768 kHz rate, a 32768-byte burst per 512 samples.
- TrueHD: MAT frames of 61424/61440 bytes, with padding derived from the TrueHD timing fields.
- An extra output latency that the platform does not report. Neither the AudioTrack timestamp nor
  `AudioTrack.getLatency()` includes it: about 50 ms for TrueHD and 100 ms for DTS on this device. With it applied,
  A/V sync measured by camera was in the same range as Kodi.
- Smoothing of the AudioTrack timestamps, which jump about +44 ms roughly once a minute on the IEC track.

Today every Media3 app that wants lossless passthrough on such devices has to write its own packer and audio
output, and work out the latency itself. Related: #3329 (another app doing TrueHD→MAT in a custom `AudioOutput` on
an Amlogic box) and the comment in `AudioCapabilities` (main, line 510): "Skip the IEC61937 encapsulation because we
don't support it yet."

### Proposed solution

An opt-in IEC 61937 output mode in `DefaultAudioSink` / `AudioTrackAudioOutputProvider`:
1. Pack TrueHD into MAT and DTS-HD/DTS into type IV bursts inside the library, and open the AudioTrack with
   `ENCODING_IEC61937`. The app enables it per format, since capability reporting on these devices does not say
   that raw passthrough is lossy.
2. Account for position and pending data in the carrier domain, so the output's position stays consistent (the
   problem in #3329).
3. Provide a hook for an app- or device-supplied extra output latency for IEC tracks.

We have a working implementation built on the 1.11 `AudioOutputProvider` API. It is derived from FFmpeg's
`spdifenc.c` (LGPL-2.1+), so it cannot be contributed as is. If you would accept a contribution, we would write
an Apache-2.0 version from the IEC 61937 and Dolby MAT specifications, and can test it on this hardware.

### Alternatives considered

- **App-side `AudioOutputProvider` (what we do now):** works on this device, but every app needs its own packer,
  and an output that expands its input runs into the pending-data accounting described in #3329.
- **A custom `AudioSink` (`ForwardingAudioSink`) that packs and hands the carrier stream to `DefaultAudioSink`, as
  suggested in #3329:** possible, but it still leaves each app to rediscover the burst formats and the unreported
  latency.
- **FFmpeg extension decoding to PCM:** plays, but loses the lossless bitstream and object audio (Atmos, DTS:X) on
  the receiver.
