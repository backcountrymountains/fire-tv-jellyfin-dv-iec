# Dolby Vision and lossless audio passthrough for Jellyfin on Fire TV (Media3)

Work-in-progress patches, measurements and notes for
[jellyfin-androidtv](https://github.com/jellyfin/jellyfin-androidtv)'s built-in player, plus a feature request
for [androidx/media](https://github.com/androidx/media). This is the supporting material for upstream
discussions. Nothing here is merged anywhere, and the patches are proposals, not finished PRs.

Tested on one setup only: Fire TV Cube 3rd gen (AFTGAZL, Amlogic, Fire OS 7.6.4.6 / Android 9), Denon AVR-S760H,
Samsung S89C (HDR10/HDR10+, no Dolby Vision), Jellyfin server 12.2.0.

## Contents

| Topic | Write-up | Code |
|---|---|---|
| Lossless TrueHD / DTS-HD / DTS:X bitstreaming (IEC 61937 packing in the app) | [docs/iec-passthrough.md](docs/iec-passthrough.md) | [patches/iec](patches/iec) |
| Dolby Vision profile 7 → 8.1 (libdovi), and a "Dolby Vision files" output setting (DV / HDR10+ / HDR10) | [docs/dolby-vision.md](docs/dolby-vision.md) | [patches/dovi](patches/dovi), [native/doviconv](native/doviconv) |
| Does the DV engine apply the metadata? Camera measurements with synthetic clips | [docs/dv-metadata-test.md](docs/dv-metadata-test.md) | [test-media](test-media) |
| Feature request draft for Media3: IEC 61937 output in the library | [docs/media3-iec61937.md](docs/media3-iec61937.md) | — |

## Applying the patches

Each series applies on its own to jellyfin-androidtv `master` (checked against `7ec47a001` and `7ada0ae80`):

```sh
git clone https://github.com/jellyfin/jellyfin-androidtv && cd jellyfin-androidtv
git am /path/to/patches/iec/*.patch      # or: git am /path/to/patches/dovi/*.patch
```

The two series both change the renderers factory in `VideoManager.java`, so applying one on top of the other
needs a small manual merge.

The DV series also needs `libdoviconv.so` in `playback/media3/exoplayer/src/main/jniLibs/<abi>/`; see
[native/doviconv/BUILD.md](native/doviconv/BUILD.md). No binaries are included.

Checks run on the DV series: the app compiles, the unit tests pass (4/4 `Hdr10PlusSeiTests`), Android lint
reports nothing above "note" level in the DV files, and detekt reports nothing in the changed Kotlin files. The
IEC patch has unit tests comparing its output byte for byte with FFmpeg's `spdif` muxer.

## Licence

GPL-2.0 (see [LICENSE](LICENSE)), the same as jellyfin-androidtv, which the patches modify. The IEC packer is
derived from FFmpeg's `libavformat/spdifenc.c` (LGPL-2.1-or-later), with attribution in the source.
`native/doviconv` calls [libdovi](https://github.com/quietvoid/dovi_tool) (MIT).

## How this was made

Written with AI assistance (Claude), including the code and these documents. Every result here was tested on
the hardware listed above. The measurements are from a receiver's own format readout, kernel/sysfs state, and a
fixed-exposure camera pointed at the TV. Test material is synthetic or described, never redistributed.
