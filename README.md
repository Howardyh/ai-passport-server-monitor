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

This initial commit preserves the official platform foundation. The monitor
application, server agent, provisioning and build instructions follow in the
implementation commit. Device flashing is outside the current review stage.
