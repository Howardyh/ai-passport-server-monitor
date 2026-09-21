[English](CHANGELOG.md)

# 更新记录

## 未发布 — 2026-09-21（main，v0.2.0-beta.1 之后）

- 修复真机 Wi-Fi 任务栈溢出导致的反复重启和屏幕闪烁：复用任务独占配置缓冲区，编译后的任务自身栈帧从 2,224 降至 192 字节，保持栈保护开启。
- 完成 ESP32-C3 修正版烧录验收：分段写入保留 NVS/PHY，正常启动并连接 Wi-Fi；50 秒串口观察无重启或 panic，用户随后确认实时指标开始更新。
- 部署共享缓存 REST/WSS Agent 与 Nginx WebSocket 路由。验证 HTTPS 200、WSS 101、43.2 秒内 23 次递增快照、有效 TLS 和 Bearer 鉴权，保留原有凭据。
- 验证缺失/错误 Token 返回 401、未知路径/查询参数 404、POST 405；重复认证头由 Nginx 以 400 拒绝（Agent 单元测试检查 401）。
- 修正版通过本地完整构建、主机测试和镜像验证；原架构提交也已通过 GitHub CI。公开源码不包含凭据、设备转储或固件二进制。

原 v0.2.0-beta.1 标签保持不变，其中仍有启动缺陷。修正位于当前 main；固件内嵌版本在下个标签发布前仍为 0.2.0-beta.1。本次不发布新的固件附件。

仍待验证：手机配网、音质、长时间稳定性、并发负载栈/Heap 余量、电流和 OTA 断电/回滚。基础启动和实时监控通过，不代表全部硬件验收完成。

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
