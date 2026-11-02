`fixed.m4a` repeats the first AAC packet from `tone.m4a` 32 times. Its MP4 sample table stores a single nonzero `stsz.sample_size` and a sample count of 32, with no per-sample size array. The chunk offsets, timing tables and durations describe 32 × 1024 stereo frames at 22050 Hz. This covers the constant-size table form independently of the variable-size table in `tone.m4a`.

`cenc.m4a` contains the unchanged AAC packets from `tone.m4a`, encrypted independently by FFmpeg with CENC AES-128-CTR and 8-byte per-sample IVs:

```sh
ffmpeg -i tone.m4a -c copy -encryption_scheme cenc-aes-ctr -encryption_key 00112233445566778899aabbccddeeff -encryption_kid ffeeddccbbaa99887766554433221100 cenc.m4a
```

`cenc-subsamples.m4a` uses the same AAC packets and test key. Each sample has a 16-byte IV (`0x102030405060708090a0b0c0fffffffc + sample_index * 0x100000000`) and two subsamples: 7 clear / 17 encrypted bytes, then 5 clear / all remaining encrypted bytes. The encrypted pieces were concatenated, encrypted with `openssl enc -aes-128-ctr`, and restored around the clear bytes. This exercises CTR carry, partial-block continuation across subsamples, and clear regions that do not consume keystream. Both files must decode to the same PCM as the original and preserve forward/backward seeks. These generated tones and keys contain no platform media or credentials.

`seek-vbr.mp3` is a generated 24-second stereo frequency sweep at 22050 Hz, encoded with LAME variable bitrate. It exercises seeks on a fresh decoder, before an accurate frame index exists. Approximate Xing-table seeks report the requested time while returning different audio samples.

Generation command:

```sh
ffmpeg -f lavfi -i "aevalsrc=exprs='0.3*sin(2*PI*(210+mod(t,7)*90)*t)*(0.5+0.5*sin(2*PI*t/5))|0.2*sin(2*PI*(330+mod(t,5)*60)*t)':s=22050:d=24" -c:a libmp3lame -q:a 5 seek-vbr.mp3
```

`cover.webp` is a lossless 4×2 RGBA image encoded with `WebPEncodeLosslessRGBA`. Its two outer columns are opaque magenta and cyan; the center square has pixels `(200, 100, 50, 128)`, `(20, 40, 80, 255)`, `(0, 0, 0, 0)` and `(100, 200, 60, 64)` in row order. It verifies centered cropping, premultiplied alpha, downscaling and no upscaling.

`cover-animated.webp` packages that same image and a solid `(10, 40, 250, 255)` second frame with `WebPMuxPushFrame`, each lasting 100 ms with no blending or disposal. It verifies that animated covers decode the first canvas.

`wide.gif` is a 4×2 GIF with one transparent palette entry: the left 2×2 square is opaque red, the right one transparent. It verifies GIF first-frame decoding with transparency, centered cropping of covers, and that whole pictures keep their aspect when scaled down.

```sh
magick -size 2x2 xc:'#ff0000' -size 2x2 xc:none +append wide.gif
```
