[简体中文](CHANGELOG.zh_CN.md)

# Changelog

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
