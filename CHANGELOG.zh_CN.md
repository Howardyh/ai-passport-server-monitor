[English](CHANGELOG.md)

# 更新记录

## v0.2.0-beta.1 — Architecture Rewrite

- HTTPS 单通道升级为认证 WSS 主通道/HTTPS 备用，统一状态、有限分片缓冲、退避重连。
- 增加协议 v1 status/alert、Event Bus、边沿告警、固定两分钟历史。
- 首次启动自动随机 WPA2 SoftAP、Wi-Fi 二维码、DNS/Web 门户，统一 NVS 配置并迁移旧格式。
- 增加队列中文语音、持久化分类/音量/静音、冷却与优先级。
- 四页升级为六页，加入音频/显示/电源/更新设置与诊断，固定 LVGL 控件及环境模式策略。
- 增加 VoiceFS、等大双 OTA 槽、manifest/镜像/hash 校验与启动回滚。
- loopback-only Agent 共用 REST/WSS 采样广播，Nginx 增加认证 WebSocket 配置。
- 增加纯逻辑与本地 HTTP/WSS 集成测试，固定 IDF/LVGL/client 依赖。

Experimental：OTA、手机扫码、音频、环境模式待硬件验收。Hardware validation pending。
无任何烧录。Known limitations：无 VoiceFS OTA、独立 manifest 签名、NVS 加密；
service/message/update_available 类型保留，严重告警使用通用语音。
不包含实际服务器/manifest 部署；固件仅本地，GitHub 只发布源码 Pre-release。
