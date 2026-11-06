# Building libdoviconv.so

`doviconv.c` is a small JNI shim over [libdovi](https://github.com/quietvoid/dovi_tool/tree/main/dolby_vision)
(MIT). It exposes one call, `DoviConv.convertP7(byte[] nal)`. That call converts a profile 7 RPU NAL to profile 8.1
(mode 2) using the same libdovi calls as Kodi's `BitstreamConverter`.

The patches expect the result at
`playback/media3/exoplayer/src/main/jniLibs/<abi>/libdoviconv.so`. No binary is shipped in this repository.

## What was actually used (armeabi-v7a)

The tested build linked the static `libdovi.a` from Kodi's Android depends tree (NDK 21.4.7075529, API 21):

```sh
T=<ndk>/toolchains/llvm/prebuilt/linux-x86_64/bin
D=<kodi-depends>/arm-linux-androideabi-21-release
$T/armv7a-linux-androideabi21-clang -shared -fPIC -O2 -I$D/include -o libdoviconv.so \
  doviconv.c $D/lib/libdovi.a -llog -lm -ldl -Wl,--gc-sections -Wl,--exclude-libs,ALL
```

## Without Kodi's tree (untested)

libdovi builds as a C library with `cargo-c`:

```sh
rustup target add armv7-linux-androideabi aarch64-linux-android
git clone https://github.com/quietvoid/dovi_tool && cd dovi_tool/dolby_vision
# with the NDK clang set as linker for the target (e.g. via cargo-ndk or .cargo/config.toml):
cargo cinstall --release --target armv7-linux-androideabi --library-type staticlib --prefix /tmp/libdovi-armv7
```

Then link as above with `-I/tmp/libdovi-armv7/include -L/tmp/libdovi-armv7/lib -ldovi`. Only armeabi-v7a (the
Fire TV Cube 3's ABI) has been tested. A real PR would need every ABI the app ships, built in CI. The app's own
FFmpeg AAR (`jellyfin-androidx-media`) is the obvious place to do that.
