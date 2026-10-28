`fixed.m4a` repeats the first AAC packet from `tone.m4a` 32 times. Its MP4 sample table stores a single nonzero `stsz.sample_size` and a sample count of 32, with no per-sample size array. The chunk offsets, timing tables and durations describe 32 × 1024 stereo frames at 22050 Hz. This covers the constant-size table form independently of the variable-size table in `tone.m4a`.

`seek-vbr.mp3` is a generated 24-second stereo frequency sweep at 22050 Hz, encoded with LAME variable bitrate. It exercises seeks on a fresh decoder, before an accurate frame index exists. Approximate Xing-table seeks report the requested time while returning different audio samples.

Generation command:

```sh
ffmpeg -f lavfi -i "aevalsrc=exprs='0.3*sin(2*PI*(210+mod(t,7)*90)*t)*(0.5+0.5*sin(2*PI*t/5))|0.2*sin(2*PI*(330+mod(t,5)*60)*t)':s=22050:d=24" -c:a libmp3lame -q:a 5 seek-vbr.mp3
```

`cover.webp` is a lossless 4×2 RGBA image encoded with `WebPEncodeLosslessRGBA`. Its two outer columns are opaque magenta and cyan; the center square has pixels `(200, 100, 50, 128)`, `(20, 40, 80, 255)`, `(0, 0, 0, 0)` and `(100, 200, 60, 64)` in row order. It verifies centered cropping, premultiplied alpha, downscaling and no upscaling.

`cover-animated.webp` packages that same image and a solid `(10, 40, 250, 255)` second frame with `WebPMuxPushFrame`, each lasting 100 ms with no blending or disposal. It verifies that animated covers decode the first canvas.
