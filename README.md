# permastream

A Linux command-line radio relay that keeps sending audio when its upstream
stations disconnect or stall. One executable, one TOML configuration file, and
ordinary HTTP stream URLs for Roon and other players. No Icecast, Liquidsoap,
external FFmpeg processes, or sound device is needed at runtime.

Sources are listed in priority order. Both outputs play the same selected source:

```text
HTTP(S) sources → decode/resample → buffered priority selection
                                             ├→ 320 kbps MP3 → local listeners
                                             └→  64 kbps MP3 → remote listeners
```

If every source fails, the application continues encoding silence and keeps
existing client connections open. When audio returns, playback resumes without
requiring the player to reconnect.

## Build and run

On Debian/Devuan/Ubuntu, install the build dependencies:

```sh
sudo apt-get install build-essential pkg-config libavformat-dev libavcodec-dev libavutil-dev libswresample-dev
make -j
cp permastream.example.toml permastream.toml
```

Edit `permastream.toml` with your real station URLs, then:

```sh
./permastream --check permastream.toml
./permastream permastream.toml
```

The config argument defaults to `./permastream.toml`. Logs go to stderr; Ctrl-C
or SIGTERM stops the application. There is no daemon setup requirement.

Add these URLs to your players, replacing `HOST` with this machine's address:

* `http://HOST:8642/local.mp3` — 320 kbps
* `http://HOST:8642/remote.mp3` — 64 kbps

Both outputs support multiple simultaneous listeners. Output encoding is shared,
so additional listeners require network bandwidth but no additional encoders.
At most 64 clients are accepted by default, across both outputs. A full server
rejects new connections with HTTP 503.

`make install` installs only the executable under `/usr/local/bin` (override with
`PREFIX` or stage packaging with `DESTDIR`). It never replaces your configuration.
The build uses the system FFmpeg shared libraries, including the `libmp3lame`
encoder. It is a single running application, **not a statically linked portable
binary**. Tested with FFmpeg 7.1 on Linux.

## Configuration

This is sufficient; the two outputs and listen address have defaults:

```toml
streams = [
    "https://primary.example/radio",
    "https://backup.example/radio",
]
```

To choose paths, bitrates or operational settings:

```toml
listen = "0.0.0.0:8642"
streams = [
    "https://primary.example/radio",
    "https://backup.example/radio",
]

# These are the defaults. All global keys go before [[outputs]].
buffer_seconds = 10
timeout_seconds = 10
recovery_seconds = 15
retry_seconds = 1
retry_max_seconds = 30
client_buffer_seconds = 5
max_clients = 64

[[outputs]]
path = "/local.mp3"
bitrate = 320

[[outputs]]
path = "/remote.mp3"
bitrate = 64
```

| Setting | Behaviour |
| --- | --- |
| `listen` | IPv4/hostname with port, or bracketed IPv6 such as `[::]:8642`. |
| `streams` | 1–32 direct HTTP(S) audio or HLS URLs, highest priority first. |
| `buffer_seconds` | Audio required before a new or exhausted source is ready. Default 10; range 0.1–120. Adds playback latency. |
| `timeout_seconds` | Deadline for connecting/probing, then for producing decoded audio. Default 10; range 0.1–300. For HLS, allow for segment publication delays. |
| `recovery_seconds` | Healthy delivery required before replacing a working source with a higher-priority source. Default 15; range 0–600. |
| `retry_seconds` | Initial reconnect delay. Default 1; range 0.1–300. |
| `retry_max_seconds` | Retry delay doubles after failures up to this limit. Default 30; range 0.1–600, at least the initial delay. |
| `client_buffer_seconds` | Outgoing ring buffer duration, plus 4096 bytes for frame overhead. Default 5; range 0.1–60. Slow clients are disconnected when they fall behind it. OS socket buffers add some allowance. |
| `max_clients` | Concurrent HTTP clients, including requests still being read. Default 64; range 1–4096. |
| `outputs` | 1–8 `[[outputs]]` tables. Omitting them supplies `/local.mp3` at 320 and `/remote.mp3` at 64 kbps. |
| `path` | Unique absolute URL path using letters, digits, `/`, `.`, `_`, `-`. |
| `bitrate` | MP3 kbps: 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320. |

Unknown keys, invalid types/ranges and duplicate output paths are rejected.
`--check` validates configuration without opening connections; it does not test
DNS, station availability, encoder availability, or whether the port can bind.
Configuration changes require a restart, which disconnects listeners.

## Failure and recovery behaviour

* Every configured source has its own connection/decoder worker. Sources are
  started in list order and kept connected as warm standbys; a slow or broken
  primary cannot delay a working backup. **Upstream bandwidth includes all
  configured sources**, even when there are no listeners.
* The highest-priority ready source is chosen. At startup, a lower-priority source
  can play while a preferred source is still preparing. Promotion waits for the
  preferred source's recovery period and sufficient buffered audio.
* Audio already buffered from an active source can play out after disconnection.
  A ready backup takes over when that buffer is exhausted. Exhausted sources must
  refill before reuse. Standby buffers retain only recent audio.
* Connection failures, invalid audio and missing decoded audio trigger retries.
  Actual musical silence is valid audio and does not trigger failover.
* If nothing is ready, silence fills the output indefinitely. Neither encoder is
  restarted during source changes. A short fade out/in reduces switching clicks.
* Sources are resampled to 44.1 kHz stereo. Every output uses one persistent CBR
  MP3 encoder with its bit reservoir disabled so clients can join at frame
  boundaries. No source bitstream passthrough or volume normalization is used.
* Output pacing follows a monotonic clock, independently of source timestamps.
  Each output has one bounded encoded-data ring with an independent cursor per
  client. Slow clients cannot block encoding or other listeners.

Different stations, or mirrors with different delays, can still produce audible
programme jumps. Buffering cannot recover programme material lost upstream.
Silence keeps the stream alive; this version does not provide a local music
fallback, FLAC output, song-title/ICY metadata forwarding, or M3U/PLS station-list
resolution. Use the actual media URL (an HLS `.m3u8` media/master URL is supported
through FFmpeg).

The output is plain HTTP without authentication or TLS. Use a VPN for private
remote access. `0.0.0.0` exposes the port on all IPv4 interfaces; use a specific
address to restrict binding. HTTPS upstream certificates are verified using the
system trust store.

Upstream outages are isolated from the output connection. Process crashes,
machine suspension, resource starvation, application restarts, and a network
failure between permastream and the player can still interrupt playback. Actual
Roon compatibility needs a playback test on your installation; the automated
tests use real HTTP clients and FFmpeg decoders.

## Tests

Install the `ffmpeg` and `openssl` command-line tools and Python 3 for tests only, then:

```sh
make test
```

The integration test uses local HTTP fixture servers with generated MP3 and AAC
audio. It checks priority failover, an open-but-stalled connection, recovery,
total outage with encoded silence, multiple listeners at both bitrates, slow
client eviction, valid MP3 frame boundaries, full decoding and clean shutdown.
Additional fixtures check live HLS and certificate validation on HTTPS segments.
It does not contact public stations. Tests require permission to bind loopback
sockets.

For a memory/undefined-behaviour check without changing the regular build:

```sh
cc -D_GNU_SOURCE -Isrc -Ivendor $(pkg-config --cflags libavformat libavcodec libavutil libswresample) \
  -std=c11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
  src/*.c vendor/toml.c -o /tmp/permastream-asan \
  $(pkg-config --libs libavformat libavcodec libavutil libswresample) -lm -pthread
PERMASTREAM_BIN=/tmp/permastream-asan python3 tests/integration.py
PERMASTREAM_BIN=/tmp/permastream-asan python3 tests/hls.py
```

The small MIT-licensed TOML parser is vendored under `vendor/`; no parser package
or generated configuration is required. See `vendor/README.md` for provenance.
