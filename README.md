[简体中文](README.zh_CN.md)

# AI Passport Server Monitor

Real-time server monitoring firmware for FoloToy AI Passport.

Based on FoloToy AI Passport firmware/hardware platform. This independent
community project is not an official FoloToy product. Course Schedule Firmware
is a separate project and its source and history are not included here.

Official base: FoloToy/ai-passport main at
`31759c4d63dd0d0d6580d6f74639ed5315bcd2c3`.

Target: ESP32-C3, 8 MB Flash, no PSRAM; ST7789P3 240 x 320 portrait RGB565;
CW2017 battery gauge; ESP-IDF 5.5.3. See [LICENSE](LICENSE).

## Hardware

All pins, ADC button thresholds and CW2017 transactions come from the unchanged
official `components/bsp`. Audio, Bluetooth, the official test-menu entry and
all demo pages are excluded from the application's CMake source list.
Only `main/monitor_app.c` supplies the linked `app_main`. No ICS parser,
course data, timetable page or course notification is present.

## Architecture

```text
Ubuntu 24.04 -> psutil sampler -> 127.0.0.1:8765 -> Nginx HTTPS
                                                     |
                                    Wi-Fi -> server_monitor_task
                                                     |
                              bounded JSON -> ServerState mutex
                                                     |
                           monitor_ui task -> LVGL lock -> display
```

The device never uses SSH. Only authenticated HTTPS GET requests fetch server
metrics. The setup portal is local to a temporary WPA2 access point.

## Screenshots / UI placeholder

Black background, white text, four fixed pages and Settings. Device screenshots
are pending hardware review; the [layout illustration](assets/images/server-monitor-preview.svg)
is a design preview, not a captured device screenshot.

| Page | Content |
| --- | --- |
| Overview | CPU, RAM, disk bars with NORMAL/WARNING/CRITICAL; RX, TX, uptime, update age |
| System | CPU utilization, nullable CPU temperature, load 1/5/15, RAM used/total/percent |
| Network + Storage | Disk used/total/percent, RX/TX bytes per second, HTTPS API latency |
| Services | Nginx, MariaDB, PHP-FPM; device SSID, RSSI and CW2017 battery |
| Settings | SSID, RSSI, API host, firmware version, battery, reconnect, provisioning |

UP/DOWN move between pages. OK short press requests an immediate refresh
(rate-limited to one attempt per second). OK long press enters/exits Settings.
In Settings, UP/DOWN select an action and OK applies it. A setup key appears
only while temporary provisioning is active. Saved Wi-Fi passwords and API
tokens are never rendered or logged. Missing values use `--` or `N/A`.

CPU/RAM warning starts at 70%, disk warning at 80%; all become critical at 90%.
Numeric values and words accompany color. Network units are decimal B/s, KB/s,
MB/s; storage/memory use binary GiB. API latency is an HTTPS measurement, not ping.

## Build / Firmware Build

Activate ESP-IDF **5.5.3**. On Linux/macOS:

```sh
python -m pip install -r server/requirements.txt
./tools/validate.sh --static
./tools/validate.sh --firmware
sha256sum build/FoloToy-AI-Passport-full.bin
```

Native Windows: activate an ESP-IDF 5.5.3 PowerShell environment and use Git Bash
for the same `tools/validate.sh` entry point. The Windows firmware adapter invokes
the same IDF build, merge, layout verification and debug-archive tools with a
fresh `build/validation-<timestamp>/sdkconfig`. It retains that build directory
for audit. Keep both IDF and project paths free of spaces (a Windows drive alias
is sufficient). For host tests provide `CC`, Python with psutil, and actionlint.
The activated IDF Python is used for the firmware stage.

The gate uses tracked `sdkconfig.defaults`, not a developer's ignored sdkconfig.
Output: `build/FoloToy-AI-Passport-full.bin`, plus matching ELF, MAP, application,
bootloader, partitions and `flash_args` in `build/firmware/<SHA256>/`.
Verify the bundle with `python tools/archive_firmware.py verify <bundle>`.
The app keeps the upstream 8 MB NVS/PHY/factory partition layout; no OTA is enabled.
Firmware and debugging artifacts are Git-ignored and are not uploaded by CI.

The [CI workflow](.github/workflows/build.yml) pins ESP-IDF 5.5.3 and invokes the
firmware gate. It neither publishes a Release nor uploads a firmware artifact.

## Server Agent / Nginx / API

Follow the [Ubuntu deployment guide](server/README.md). The non-root Python
agent samples independently of requests and serves only `GET /api/v1/status`.
Nginx exposes HTTPS using a valid public CA certificate. All status responses
are JSON with `Cache-Control: no-store`; auth is `Authorization: Bearer <TOKEN>`.
The token is read through `/etc/passport-status.env`, never compiled into firmware.
See the [API example](server/status.example.json) for the complete version 1 shape.
CPU temperature may be JSON `null`. Battery and RSSI are device-local readings.

## Wi-Fi Setup

1. Boot opens Server Monitor immediately; saved STA credentials load from NVS.
2. Hold OK, choose **Wi-Fi Provisioning**, then press OK.
3. Join the displayed `Passport-xxxx` WPA2 hotspot using its temporary setup key.
4. Open `http://192.168.4.1` and enter the 2.4 GHz WPA2/WPA3-compatible SSID,
   Wi-Fi password (8-63 bytes), HTTPS API URL and random bearer token (32-128 characters).
5. Save and check the device's confirmation. The portal closes, the device
   reconnects as STA, synchronizes time via SNTP, then starts verified HTTPS.

The AP allows one station and expires after five minutes. During setup STA is
disabled, so the portal is not exposed on the normal LAN. A per-session nonce
protects the configuration POST against cross-origin requests. Credentials are
saved as one versioned NVS blob only after validation; failures keep previous
settings. No automatic NVS erase occurs. Reprovisioning replaces settings; open
Wi-Fi networks and 64-digit raw PSKs are intentionally unsupported in this version.

## Reliability and memory

Wi-Fi attempts back off 2/4/8/16/30 seconds; association is bounded by 20 seconds.
HTTPS polls every 5 seconds after success, with 5/10/20/30/60 second failure delays.
The client has a 3-second I/O deadline; a separate socket watchdog interrupts
slow response headers and bodies. DNS resolution uses the IDF/lwIP resolver and
can take longer than the request I/O deadline. It never blocks LVGL or buttons.

ONLINE means the last valid receipt was within 15 seconds. Later it becomes
STALE and retains all metrics. OFFLINE means no valid snapshot has been received.
Changing the endpoint clears the previous endpoint's data. Unknown fields are
ignored; missing or invalid required fields reject the entire response atomically.
Responses over 4096 bytes, nesting over eight levels, duplicate fields, nonfinite
numbers, invalid percentages, and timestamps older than 15 seconds or over 30
seconds in the future are rejected. CPU `temperature:null` is accepted.

Widgets are created once. Static label buffers and three bars are reused.
The BSP uses a 9.6 KB partial RGB565 buffer and a 24 KB LVGL pool; no framebuffer
or PSRAM is used. HTTPS JSON has one 4097-byte static buffer. Task stacks:
HTTPS 8192, Wi-Fi 5120, UI 4096, battery/telemetry 3072, deadline guard 2048 bytes; the setup HTTP
task uses 6144 bytes only while active. The telemetry task logs free heap, minimum heap
and largest block every 30 seconds. Actual runtime heap and stack watermarks
require hardware tests; linker RAM figures alone cannot establish stability.

## Security

The TLS client uses `esp_crt_bundle_attach`, normal hostname verification and
SNTP-established wall time. Certificate verification is never disabled.
Redirects are refused to prevent forwarding Authorization to another host.
API URLs must use HTTPS and cannot contain userinfo, queries or fragments.
No SSH client, server command execution or remote-control endpoint exists on
the device. The agent only queries fixed systemd units using argument arrays
with timeouts; it neither starts nor stops services. It refuses root execution.

`.gitignore` excludes credentials, private configuration, keys, certificates,
builds and logs. Examples contain placeholders only. Before committing or pushing,
run `python tools/check_repo.py`, `python tools/check-monitor-secrets.py` and
review `git diff --cached`. Never provide a token in a URL or shell history.
NVS is not encrypted in this development build; physical flash access can reveal
stored credentials. Secure Boot, flash/NVS encryption and token rotation policies
remain deployment decisions. Do not enable them during this build-only review.

## Flash

**Not authorized in the current stage.** Build, validate, commit/push source and
stop for manual review. No serial discovery, reset, erase or flash is performed.
A later explicit instruction authorizing flashing is required. The merged image is intended
for offset `0x0`; writing it may replace NVS data. Device and stored-data review
must precede any future device write. See the upstream [layout policy](docs/development/engineering/firmware-layout.md).

## Troubleshooting

- Wi-Fi disconnected: check 2.4 GHz, credentials and RSSI; retry or reprovision in Settings.
- Waiting for time sync: check Internet access and UDP/123; TLS stays fail-closed.
- HTTPS/TLS/DNS error: check DNS, certificate chain, certificate expiry and hostname.
- HTTP 401: verify or rotate the bearer token on both sides. HTTP 404: verify the path.
- HTTP 500/503: inspect Nginx and `journalctl -u passport-status`; check sampler/unit configuration.
- Invalid JSON: compare the version 1 schema and response size. N/A temperature is normal on many VPSs.
- N/A battery: check CW2017 via the official BSP after hardware testing is authorized.
- NVS save failed: investigate storage; the firmware never auto-erases it or enters a reboot loop.

## License

MIT; the original FoloToy copyright and complete [LICENSE](LICENSE) are retained.
Official source: [FoloToy/ai-passport](https://github.com/FoloToy/ai-passport).
