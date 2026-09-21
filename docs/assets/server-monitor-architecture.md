[简体中文](server-monitor-architecture.zh_CN.md)

# Server Monitor architecture and release acceptance

## Scope

Version is authoritative in `version.txt`: v0.2.0-beta.1, Architecture Rewrite.
This is a source pre-release with local firmware only. Hardware validation pending.
BSP drivers and pin definitions are unchanged; the BSP dependency manifest pins LVGL
9.5.0 to prevent an unreviewed upgrade. Original demo sources remain reference-only
and are absent from the linked application.

## Ownership and startup

`monitor_app` initializes the dedicated event bus, NVS configuration, ES8311,
display/LVGL, input/control workers, VoiceFS, Wi-Fi, monitor, OTA and telemetry.
The configuration center owns the versioned NVS blob and migrates the prior v1
HTTPS configuration without erase. Failed NVS initialization is reported and never
repaired by erasing storage. First boot with no valid credentials starts provisioning.

`events/` uses an ESP event loop with a 24-item bounded queue. Producers copy small
messages and never block for playback or LVGL. Audio and the UI coordinator subscribe.
Commands use the same bus: setup, refresh, mute, settings and OTA. Drop counters are
visible in Diagnostics. Config writes occur in workers, outside the LVGL lock.
The UI reads coherent config, Wi-Fi and ServerState snapshots; callbacks cannot access
LVGL. Only the UI task performs widget updates under the official BSP lock.

## Network and state

Wi-Fi worker owns association, scanning, random WPA2 AP, portal and five-minute
expiry. Device MAC suffix names the AP. DNS responds only to A queries on the AP
subnet; phones without captive detection use 192.168.4.1. Setup POST validates a
fresh nonce and bounded fields. The AP has one client, and STA remains disconnected
during setup. Saved secrets are never returned to the browser or shown on screen.
POSIX timezone is persisted and applied. Display/audio/power fields share Config.

Espressif WebSocket client 1.6.1 owns framing, TLS, ping/pong and connection teardown.
Application reconnection uses capped exponential delays with jitter; three failures
activate ten-second HTTPS fallback. A 30-second accepted-data timeout prevents
zombies even if transport pongs continue. TLS uses the IDF CA bundle and hostname
verification; monitoring waits for SNTP. Configuration rejects URL userinfo, queries,
fragments and plaintext schemes. Authorization stays in headers. HTTP redirects
are disabled. Both transports use the same parser and state commit operation.

Status input is at most 4096 bytes, depth at most eight, with duplicate-key,
finite-number, range, timestamp and monotonic sequence checks. Optional temperature
is nullable/missing. Invalid required fields preserve the old snapshot. A newer
server timestamp permits an Agent sequence restart. WSS fragments accumulate into
one fixed buffer, then parsed typed records enter a six-item queue. Queue overflow
forces resync instead of silently accepting an incomplete stream. Future message
kinds are ignored; status and alert are implemented. Alerts do not refresh the
status snapshot timestamp. Last values survive STALE (>15s) and OFFLINE (>60s).

HTTP uses a three-second response deadline and a socket watchdog. DNS resolution
can outlast that response deadline; no network operation blocks LVGL. Fallback TLS and WSS attempts are serialized to avoid two TLS sessions at once.
A pending WSS attempt can delay the nominal ten-second HTTP interval; failed attempts
are bounded by transport and application timeouts. Runtime heap still requires
hardware measurements. OTA pauses the monitor and waits for quiescence before TLS.

## Audio

17 clips in `assets/music/server-monitor/` were synthesized locally from the
project's original Chinese phrases with the installed Microsoft Huihui Desktop voice.
The speech engine is not redistributed; no network TTS or third-party recording is
used. `phrases.json` maps zero-based clip numbers to phrases. Committed `.ulaw` files
are reproducible playback inputs: 16 kHz mono G.711 mu-law, no container/header.
Total audio payload: 754,080 bytes (about 47.13 seconds); SPIFFS includes its metadata.

Opus was considered but not integrated: this no-PSRAM target would add a decoder
state/workspace and a dependency with hardware-specific CPU/RAM cost not yet measured.
Mu-law fits the measured Flash budget and has a stateless integer decoder using
256 encoded bytes + 512 PCM bytes. That avoids claiming an unmeasured Opus RAM figure.
The ES8311 driver and playback-volume API are reused without modification.

One worker owns codec writes. Three queues hold four items each. Higher severity
flushes lower waiting queues; active playback checks for preemption/mute between
16 ms chunks. Cooldown is 30 seconds normally and 60 seconds for critical alerts.
Critical bypass mute defaults OFF. Normal snapshots never trigger repeated sync
voice. Audio file/open/write errors increment diagnostics; missing VoiceFS never
triggers formatting. Critical alert speech is generic; the specific text remains
on the Services page. CPU/RAM/disk alerts use edge triggers, rearming below 90%.

## UI, power and memory

Six pages, six settings sections, a two-page Diagnostics view and QR provisioning
share a single widget tree. Static labels, three bars, a fixed I2 trend canvas and one QR widget are created
once. The history ring uses two uint8_t[60] arrays. A 192 x 80 four-color canvas
uses 3,856 static bytes including its palette; a pure integer rasterizer avoids
allocating a draw task for each chart line segment. The original LVGL line-chart
approach exceeded a 24 KiB pool during populated-chart host rendering and was replaced. No per-update screen
recreation, framebuffer or PSRAM. The application LVGL pool is 32 KiB, partial RGB565
DMA buffer 9,600 bytes. QR canvas/encoding allocations occur only when setup changes.

| Worker | Stack bytes | Role |
| --- | ---: | --- |
| main | 6144, released after startup | Initialization |
| event_bus | 3072 | Nonblocking event dispatch |
| config_worker | 4096 | Persist settings |
| monitor_wifi | 5120 | Wi-Fi/AP lifecycle |
| captive_dns | 2048 | Temporary DNS responses |
| HTTP portal | 8192, setup only | Bounded form requests |
| server_monitor | 8192 | Transport coordination/state/HTTP |
| WebSocket | 6144, while active | Official client callbacks |
| http_deadline | 2048 | Interrupt stalled response socket |
| audio_worker | 4096 | Streaming decode/playback |
| ota_worker | 6144 | Manifest/download verification |
| monitor_ui | 4096 | Snapshot to LVGL |
| monitor_telemetry | 3072 | Battery/heap telemetry |

Network RX and TX buffers are 1024 bytes per client. HTTP payload and WSS reassembly share one 4.1 KiB workspace, reused only after
WebSocket teardown has joined its callback task. Typed-message queue is bounded to six records;
audio queues contain only clip identifiers. IDF Wi-Fi/lwIP/TLS, BSP LVGL task and
I2S DMA add their own allocations. Linker RAM is not a runtime free-heap estimate.
Measure minimum heap, largest contiguous block and task stack watermarks on-device
before treating concurrent WSS/audio/QR/OTA as accepted.

Live: configured brightness until timeout then off, WSS retained. Balanced: dim
to 10%, WSS retained. Battery Saver: ambient screen off, WSS closed, HTTPS every
60 seconds; wake restores live behavior. Any key wakes before acting. These policies
do not use light/deep sleep. Battery savings and current draw are unmeasured.

## Partition and OTA assessment

Pre-change: 8 MB Flash; NVS 0x9000/0x6000, PHY 0xf000/0x1000, factory
0x10000/0x7f0000. Previous app binary was 1,463,232 bytes. Voice payload is 754,080
bytes, comfortably below the reserved filesystem partition (allow SPIFFS overhead).

| Partition | Offset | Size |
| --- | --- | --- |
| nvs | 0x9000 | 0x6000 (24 KiB) |
| phy_init | 0xf000 | 0x1000 (4 KiB) |
| otadata | 0x10000 | 0x2000 (8 KiB) |
| ota_0 | 0x20000 | 0x300000 (3 MiB) |
| ota_1 | 0x320000 | 0x300000 (3 MiB) |
| voicefs | 0x620000 | 0x1d0000 (1,856 KiB) |
| coredump | 0x7f0000 | 0x10000 (64 KiB) |

The gap aligns the first app to 64 KiB. Both slots are equal. Gate checks enforce
8 MB bounds, overlap, image offsets, slot capacity and matching merged image bytes.
App-only OTA cannot change this layout or VoiceFS. The initial factory-to-OTA
migration needs a separately reviewed, explicitly authorized device write.
A merged image pads gaps, so later flashing it can reset existing NVS contents.

OTA manifest at the configured HTTPS API origin's `/firmware/manifest.json`:

```json
{"version":"0.2.0-beta.2","url":"https://status.example.com/firmware/app.bin","sha256":"<64 hex digits>","size":1500000}
```

The endpoint is an operator deployment prerequisite, not deployed by this change.
Only same-origin HTTPS app images, newer supported versions and sizes fitting the
inactive slot are accepted. The worker checks HTTP length, streaming SHA-256,
ESP image validation, project and embedded version before selecting the boot slot.
TLS authenticates the manifest; an independently signed manifest/secure boot is
not implemented. No automatic install on network messages. User selects Update,
then confirms installation on the device. New boot validates NVS, display/UI
progress and network task progress after 30 seconds; only then marks valid.
A failing pending image can roll back on reset. Hardware/power-loss tests are pending.

## Server and authentication

Python 3 + psutil + aiohttp supports Ubuntu 24.04. One sampling thread feeds a
locked latest-snapshot cache; WSS broadcasts every two seconds and REST reads it.
Service health refreshes every five seconds using fixed validated systemctl arguments.
CPU temperature may be null. There are at most 16 WSS clients; each send has a bounded
timeout. Slow clients do not resample system metrics. Auth uses constant-time Bearer
comparison, rejects duplicate headers and token queries, disables request logging,
and loads configuration from `/etc/passport-status.env` through systemd.
Nginx forwards Upgrade/Connection/Authorization, disables cache/buffering and uses
65-second WSS read timeout, longer than 20+10-second heartbeat. The production Agent
and Nginx route were deployed and validated on 2026-09-21. The firmware route stays disabled until an operator publishes
reviewed app-only images and manifests securely.

## Acceptance boundaries

Host tests and firmware compilation are mandatory release gates. Physical display,
QR scanning, provisioning/reconnection on a phone, audio quality, mixed-workload
heap, power drain and interrupted OTA/rollback are NOT TESTED. Coredumps/flash contain
potential secrets; keep them local. NVS encryption, secure boot and signed independent
manifests remain planned hardening. Service/message/update_available push types are
reserved; general-purpose pushed text/TTS and VoiceFS OTA are not implemented.
Follow-up authorized device testing found a Wi-Fi worker stack overflow in the original tag.
Current main reuses one worker-owned command/config buffer, reducing its compiled frame
from 2,224 to 192 bytes. The repaired app passed the complete local gate and device startup
observation; the owner confirmed live metrics after server deployment. NVS/PHY sectors
were preserved during segmented flashing. Long-duration and broader hardware tests remain pending.
