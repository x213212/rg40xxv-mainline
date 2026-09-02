# RG40XX V native YouTube exact-binary device evidence

Recorded: `2026-08-31T00:35:27+08:00`

Scope: p7 hot-test only. No reboot and no p8 write occurred.

## Identity

```text
bundle_id=22f00dd901fb5605
bundle_archive_sha256=ba6d01d27429aeb57ee9059f46eaf559cc75041c879d0fef14dd44023f646d88
texture_binary_sha256=a2ccc6b14b712641441888468bbe4657ad8d53bc63bf15ba93c73b74eecfde57
yt_dlp_server_sha256=ac1ced5684520c3aa903d652b95103b7850d7bac04bf6942d54a6c48238e44f3
endpoint_broker_sha256=3108a45b982160a937e4003d3b2ee1d0f77b41880fd0b5bb48fc9ea7745b7521
p8_before_sha256=28e9875ead57fabd1a333f9895ed87b8570c19b2b10a808dfb9f00fb8e8f53fb
p8_after_sha256=28e9875ead57fabd1a333f9895ed87b8570c19b2b10a808dfb9f00fb8e8f53fb
p8_write=NONE
reboot=NONE
```

The three installed executable/tool hashes were read back from
`/usr/local/rg40xxv-youtube-texture-v1` and exactly matched the host files.

## Exact-binary playback acceptance

The exact texture binary above was launched through its installed wrapper and
broker against `https://www.youtube.com/watch?v=fsIBaT1jCx8` in an isolated
transient service. The normal HOME service was restored automatically.

Current invocation journal sequence:

```text
[23759.196336] YOUTUBE_TEXTURE_BROKER endpoint=READY
[23759.365163] YOUTUBE_TEXTURE_MEDIA loaded=1
[23759.366005] YOUTUBE_PLAYER_TIMELINE position=0 advancing=1
[23760.227800] YOUTUBE_TEXTURE_MEDIA first-frame=READY
```

The test started at monotonic uptime `23756.36`, so process start to first
frame was approximately `3.87 s`.

While the video was playing, two ALSA samples one second apart reported:

```text
state=RUNNING hw_ptr=16256
state=RUNNING hw_ptr=65312
```

The advancing hardware pointer is machine evidence of an active audio stream.

A SIGUSR1 render-memory capture produced:

```text
path=/run/rg40xxv-youtube-accept/texture.bmp
size=1228922
sha256=94e9a05cda4ab1b8b34ce776478a7dc7f9f0a7869b6ec46cd3c0025e2732bc86
sampled_colors=33
```

This proves a non-uniform rendered frame was present. It is not a substitute
for user visual/color acceptance.

## Repeated playback and latency

One HOME process (`PID 22073`) completed more than fourteen distinct or
repeated playback cycles. Every completed cycle logged `first-frame=READY`,
then returned with `mpv_alive=1 broker=retained`. No process restart or
third-play degradation was observed.

Representative selection-to-first-frame times:

```text
prefetched/replayed: 1.09 s, 1.50 s, 1.67 s, 1.69 s
cold/non-prefetched: 3.44 s, 3.70 s, 3.78 s, 3.91 s, 4.19 s, 4.47 s, 5.19 s, 5.22 s
```

Persistent resolver results seen on the device were `1.25-1.78 s`. The HOME
service remained `active/running` with `NRestarts=0` after acceptance, and a
two-worker `yt_dlp_server.py` was present with a mode-0600 AF_UNIX socket.

## Evidence boundary

The exact binary has machine-verified endpoint resolution, media load, first
frame, advancing timeline, active ALSA output, retained mpv lifetime, and
repeated playback. User visual/color acceptance for this exact build remains
`PENDING_DEVICE_USER` until explicitly reported; it must not be inferred from
the screenshot hash.
