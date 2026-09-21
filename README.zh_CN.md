[English](README.md)

# fl0AT AI Passport Server Monitor

**v0.2.0-beta.1 — Architecture Rewrite。Hardware validation pending.**

独立 ESP32-C3 固件：8 MB Flash、无 PSRAM、240×320 ST7789、CW2017 电量计与
ES8311 扬声器。保留官方 BSP 引脚和成熟驱动，只启动 Server Monitor，不包含课程表或
官方 Demo 菜单。许可证见 [LICENSE](LICENSE)。

## 网络与协议

WSS 主通道默认 `wss://status.dyhcn.com/ws`，HTTPS 备用默认
`https://status.dyhcn.com/api/v1/status`，均可配置，共用 ServerState。
[服务器 Agent](server/README.zh_CN.md) 每秒采样、每两秒广播缓存快照；REST 读取同一缓存。
仅监听 127.0.0.1:8765，Nginx 负责 TLS。两种通道都通过 Authorization Bearer header
认证，不把 Token 放 URL。

协议 v1 包含 `v/type/seq/timestamp`，实现 status 和 alert；service/message/update_available
暂保留并安全忽略，更新可用性通过 manifest 查询。temperature 缺失或 null 显示 `--`；
其余必需字段非法时拒绝整条数据并保留上次有效状态。[JSON 示例](server/status.example.json)。

官方 WebSocket 客户端使用 CA bundle 与主机名验证，20 秒 ping、10 秒 pong 超时；
30 秒没有有效应用数据也重建连接。重连基准 1/2/4/8/16/30 秒，加入向下 0–10% 抖动。
三次失败后每 10 秒 HTTPS 轮询，仍持续恢复 WSS；收到新 WSS 数据恢复 LIVE。
15 秒后 STALE，60 秒后 OFFLINE，保留指标。延迟指 HTTPS 请求耗时，不是 ping 或 WSS RTT。

## 首次启动与扫码配网

没有有效 NVS 配置时自动启动；长按 OK 可重新进入。二维码连接随机密码 WPA2 临时热点
`fl0AT-Passport-XXXX`，只含临时 AP 凭据。若手机未自动弹出页面，打开 `http://192.168.4.1`。

页面包含可用网络、SSID/密码、服务器 Host、WSS/HTTPS URL、API Token、设备名称、POSIX
时区与默认音量。支持 2.4 GHz WPA2 兼容网络，Token 32–128 字符。Save & Connect 验证并
提交单个 NVS blob，再关闭门户并连接 STA。保存失败保留旧设置；五分钟超时，最多一部手机。
屏幕不显示已保存密码与 Token，临时热点密码只在二维码内。配置 POST 使用会话 nonce。
DNS 只回答 AP 子网，配网期间 STA 断开。迁移旧配置不删除旧 blob，不自动擦除 NVS。

## 页面与按键

黑白终端风格，绿黄红指示，固定控件，更新不重建屏幕。ASCII 字体不依赖 Emoji/中文字形。

| 页面 | 内容 |
| --- | --- |
| Overview | CPU/RAM/Disk、RX/TX、运行时间、连接、RSSI、电量 |
| Performance | CPU/RAM、可空温度、负载、两分钟趋势 |
| Network | 速率、HTTPS 延迟、RSSI、WSS 消息年龄、重连数 |
| Services | Nginx/MariaDB/PHP-FPM、最新告警 |
| Device | 电池、固件、运行时间、Heap、Wi-Fi、IP |
| Settings | Network/Audio/Display/Power/Update/Diagnostics |

UP/DOWN 切页，OK 请求备用刷新；长按 DOWN 打开设置，长按 UP 静音，长按 OK 配网。
设置中 DOWN 选择、OK 应用、UP 返回；设置主列表末尾再按 DOWN 返回 Overview。
音量每次 +10，100 后回到 0。屏幕 dim/off 后首个按键只唤醒。亮度 10–100%，超时
Never/30s/1m/5m/10m。Live 熄屏保留 WSS，Balanced 调暗为 10% 保留 WSS，Battery Saver
环境模式断开 WSS、每 60 秒 HTTPS。这里不进入深睡眠。

## 音频和告警

17 段本机离线合成中文语音放 SPIFFS VoiceFS，16 kHz 单声道 G.711 μ-law，原始总大小
754,080 字节。每次解码 256 字节为 512 字节 PCM，通过 BSP 设置 ES8311 播放音量。
设备无 TTS、不把巨大 PCM 数组编译进 app。[资源说明](docs/assets/server-monitor-architecture.zh_CN.md)。

独立任务处理 INFO/WARNING/CRITICAL 有界队列；高优先级清理低优先级等待队列并在下一
16 ms 分块抢占。普通冷却 30 秒，严重 60 秒。首次同步每次启动只播一次，正常快照静默。
持久化 Voice、音量、启动/网络/服务器/严重分类、提示音与严重告警绕过静音，绕过默认 OFF。
CPU/RAM/Disk >=90% 边沿触发，低于 90% 重新布防。服务器告警显示消息并按严重度播报。

## OTA 与诊断

两个 3 MB OTA 槽、NVS、OTA Data、VoiceFS、Coredump 均位于 8 MB 内。
[架构与分区分析](docs/assets/server-monitor-architecture.zh_CN.md) 描述迁移及内存预算。
Settings / Update 获取配置 API 同源的 `/firmware/manifest.json`，选择更新后需在设备上
再次按 OK 确认。manifest 包含 version、同源 HTTPS app-only URL、SHA256、size。
下载前暂停 WSS，校验 TLS、大小、SHA256、ESP 镜像/芯片/项目与版本后才切换启动分区。
版本格式 major.minor.patch 或 major.minor.patch-beta.N，只接受更新版本，不跟随重定向。
新固件须通过 NVS、Display/UI 进度、network task 自检才 mark valid，启用 IDF rollback。
旧 factory 分区首次迁移必须另外授权更新分区表，普通 app-only OTA 不能完成；OTA 不更新 VoiceFS。

诊断显示版本、运行时间、free/minimum heap、最大块、RSSI/IP、WSS 年龄/状态、API 延迟、
重连、服务器、错误、音频队列/错误、丢失事件。Coredump/私有日志可能包含秘密，只保留本地。
本任务没有向实际服务器部署 manifest 或 binary。

## 构建验证

ESP-IDF **5.5.3**，LVGL **9.5.0**，WebSocket **1.6.1**。版本统一来自 [version.txt](version.txt)。

```sh
python -m pip install -r server/requirements.txt
./tools/validate.sh --static
./tools/validate.sh --firmware
./tools/validate.sh
```

Windows 在激活 IDF 的环境中通过 Git Bash 使用同一入口，并提供 CC/actionlint/Python。
官方 gate 从 tracked defaults 在全新目录编译、合并、校验分区范围/字节/ELF 身份并归档到
`build/firmware/<SHA256>/`，输出 `build/FoloToy-AI-Passport-full.bin`。
用 `python tools/archive_firmware.py verify <bundle>` 核验；独立 voicefs.bin 留在 validation
目录，合并镜像也包含它。二进制、ELF、MAP、私有配置只留本地；CI 完整验证且不上传固件。

主机测试覆盖 JSON 缺失/null/非法类型、分片、状态保留/通道切换、告警边沿、音频冷却/优先级、
边界/趋势/格式化、版本/manifest、音频解码与 HTTP/WSS 认证/广播。
**Hardware validation pending.** 屏幕可读性、扫码、音质、TLS+音频并发 heap、栈水位、
真实 Wi-Fi 故障、OTA 断电/回滚、电池续航仍待真机验证。

## 安全和发布

无内置 Wi-Fi 密码、API Token、私钥；TLS 验证始终开启，监控前等待 SNTP。
JSON 上限 4096 字节，检查深度/类型/范围/重复键。未知消息不能执行命令。
NVS 原子提交，但 beta 未启用加密，物理读 Flash 可暴露凭据。安全启动/eFuse/加密配置不在此次范围。

每次 commit/push 前运行 `python tools/check_repo.py` 和 `python tools/check-monitor-secrets.py`，
检查 staged diff。GitHub 只发源码 **Pre-release**，不上传固件，见 [CHANGELOG](CHANGELOG.zh_CN.md)。
**本阶段禁止烧录**：不 flash/erase/reset bootloader/连接串口/清除 NVS，后续必须明确授权。

依赖解析完成后运行 `python tools/test-monitor-ui.py`，以合成数据、配置的 32 KiB LVGL 池
和 BSP 大小局部缓冲渲染实际页面，PPM 输出位于 `build/ui-validation/`，不操作设备。

![实际 LVGL 主机渲染；合成数据，非真机照片](assets/images/server-monitor-v020-preview.png)
