# Test video attribution

## `big_buck_bunny_720p_20s.mp4`

- Work: **Big Buck Bunny — Sunflower version**
- Original creators: Blender Foundation (2008)
- Sunflower version: Janus Bager Kristensen (2013)
- Official source: <https://download.blender.org/demo/movies/BBB/bbb_sunflower_1080p_30fps_normal.mp4.zip>
- License: [Creative Commons Attribution 3.0](https://creativecommons.org/licenses/by/3.0/)
- Selected source interval: `00:02:20`–`00:02:40`
- Derived format: H.264 High, 1280×720, 30 FPS, YUV420P; AAC-LC stereo, 48 kHz
- Duration: 20 seconds / 600 video frames

Attribution: © 2008 Blender Foundation, Big Buck Bunny; Sunflower version © 2013 Janus Bager Kristensen. Used and adapted under CC BY 3.0.

### Checksums

- Official ZIP SHA-256: `e320fef389ec749117d0c1583945039266a40f25483881c2ff0d33207e62b362`
- Derived MP4 SHA-256: `177cb61a933fcafcfe545deb64501e73232b56d280462c53442c34f84af36ffd`

### Reproduction command

After extracting `bbb_sunflower_1080p_30fps_normal.mp4` from the official ZIP:

```bash
ffmpeg \
  -ss 140 \
  -i bbb_sunflower_1080p_30fps_normal.mp4 \
  -t 20 \
  -map 0:v:0 \
  -map 0:a:0 \
  -vf scale=1280:720 \
  -c:v libx264 \
  -preset medium \
  -crf 20 \
  -pix_fmt yuv420p \
  -c:a aac \
  -b:a 128k \
  -movflags +faststart \
  big_buck_bunny_720p_20s.mp4
```
