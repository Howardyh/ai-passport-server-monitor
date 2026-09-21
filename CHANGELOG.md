[简体中文](CHANGELOG.zh_CN.md)

# Changelog

## Unreleased — 2026-09-21 (main, after v0.2.0-beta.1)

- Fix the on-device Wi-Fi worker stack overflow that caused repeated resets and screen flashing. Reuse one worker-owned config buffer; the compiled worker frame drops from 2,224 to 192 bytes without disabling stack protection.
- Validate the repaired app on ESP32-C3: segmented flashing preserved NVS/PHY, startup and Wi-Fi connection succeeded, and a 50-second serial observation showed no reset or panic. The user subsequently confirmed live metrics updating.
- Deploy the shared-cache REST/WSS Agent and Nginx WebSocket route. Verify HTTPS 200, WSS 101, 23 increasing snapshots over 43.2 seconds, valid TLS and Bearer authentication. Existing credentials were retained.
- Verify missing/wrong tokens return 401, unknown paths/queries 404, POST 405, and duplicate Authorization headers are rejected by Nginx with 400 (Agent unit tests check 401).
- Complete local build/host/image-validation gates for the repair; the original architecture commit also passed GitHub CI. Keep credentials, device dumps and firmware binaries out of the public source repository.

The v0.2.0-beta.1 tag is immutable and still contains the startup fault. Use current main for the fix; its embedded version remains 0.2.0-beta.1 until the next tagged release. No new firmware assets are published.

Still unverified: phone provisioning, audio quality, extended stability, concurrent workload stack/heap margins, power draw and OTA power-loss/rollback. Basic startup/live monitoring success is not complete hardware acceptance.

## v0.2.0-beta.1 — Architecture Rewrite

- Replace HTTPS-only monitoring with authenticated WSS Primary and HTTPS fallback,
  a shared state model, bounded fragmented-message handling and reconnect backoff.
- Add protocol v1 status/alert, event bus, edge alarms, fixed two-minute history.
- Automatically provision unconfigured devices through random WPA2 SoftAP, Wi-Fi QR,
  DNS/web portal and a unified NVS configuration with prior-format migration.
- Add queued Chinese voice, persistent audio categories/volume/mute, cooldown and priority.
- Replace four-page monitoring with six pages, audio/display/power/update settings,
  diagnostics, fixed LVGL widgets and ambient display/network policies.
- Add VoiceFS, two equal OTA slots, manifest/image/hash validation and boot rollback.
- Upgrade the loopback-only server agent to shared REST/WSS sampling and broadcasts;
  extend authenticated Nginx upgrade configuration.
- Add host logic and local HTTP/WSS integration tests; pin IDF/LVGL/client dependencies.

Experimental: OTA, QR phone behavior, audio and ambient policies await hardware
acceptance. Hardware validation pending. No device flashing performed.
Known limitations: no VoiceFS OTA, no independent manifest signature or encrypted
NVS, reserved service/message/update_available push types, generic critical voice.
Production server/manifest deployment is not part of this source release.
Firmware binaries remain local; GitHub publishes a source-only Pre-release.
