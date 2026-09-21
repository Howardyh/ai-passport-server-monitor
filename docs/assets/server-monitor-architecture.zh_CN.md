[English](server-monitor-architecture.md)

# Server Monitor 架构与发布验收

## 范围与启动

版本统一来自 version.txt：v0.2.0-beta.1 Architecture Rewrite。只发布源码预发布，固件保留
本地。Hardware validation pending。BSP 引脚和驱动不变，依赖声明固定 LVGL 9.5.0；原 Demo
源文件仅作参考，不参与应用链接。

monitor_app 按序初始化 Event Bus、NVS 配置、ES8311、Display/LVGL、按键/配置任务、VoiceFS、
Wi-Fi、Monitor、OTA 与电池遥测。Config 独占版本化 NVS blob，读取迁移旧 v1 HTTPS 设置，
不擦除旧数据。无有效配置自动配网，NVS 初始化失败也不擦除。

Event Bus 使用 24 项有界队列，UI 协调层/音频订阅，命令同样经总线：配网、刷新、静音、设置、
OTA。回调复制小消息、不播放、不访问 LVGL；丢失计数显示于 Diagnostics。配置在 worker
持久化，UI 读取 Config/Wi-Fi/ServerState 快照，只有 UI task 在 BSP LVGL 锁下更新控件。

## 网络和状态

Wi-Fi task 管理关联、扫描、随机 WPA2 AP、门户和五分钟到期；MAC 后缀构成 SSID。
DNS 只答 AP 子网 A 请求，不弹门户时手动打开 192.168.4.1。配置 POST 校验 nonce/字段边界。
AP 最多一部手机，配网时 STA 断开，不返回已保存秘密；POSIX 时区、音频、显示和电源设置统一持久化。

官方 WebSocket 1.6.1 负责协议/TLS/ping/pong/关闭，应用做指数退避抖动。三次失败进入 10 秒
HTTPS 备用，30 秒无有效数据也断开重建，避免只响应 pong 的僵尸。CA bundle/主机名验证始终
开启，监控先等待 SNTP。URL 禁止 userinfo/query/fragment/明文，Authorization 只在 header，
HTTP 不跟随重定向。双通道共用解析及提交状态函数。

JSON 最多 4096 字节、深度八层，检查重复键、有限数字、范围、时间戳和序号。temperature 可缺失
或 null，其他必需字段非法时保持旧状态。较新 timestamp 允许 Agent 重启序号归零。
WSS 固定分片缓冲，解析为结构体后进入六项队列，溢出则重新同步。只实现 status/alert，
未来类型安全忽略。alert 不刷新 status 新鲜度。15 秒 STALE、60 秒 OFFLINE，保留指标。
HTTPS 有三秒响应截止时间及 socket watchdog，DNS 可更久，不阻塞 UI。WSS 重连与 HTTP TLS
串行使用资源，避免两条 TLS 同时存在；连接尝试可能延后名义 10 秒轮询周期，受网络/应用超时限制。
仍需真机测试 Heap。OTA 先暂停 Monitor，等资源静止后开始 TLS。

## 音频

assets/music/server-monitor/ 下有 17 段使用本机 Microsoft Huihui Desktop 离线合成的原创短语。
不分发语音引擎，无网络 TTS、无第三方录音。phrases.json 按从零开始编号映射；ulaw 文件为
16 kHz mono G.711 μ-law，无文件头，总计 754,080 字节，约 47.13 秒。SPIFFS 另含元数据。

评估了 Opus，但未引入其解码状态/工作区和未实测的 CPU/RAM 开销，不编造 Opus 内存数字。
μ-law 满足已测 Flash 预算，整数无状态解码只需 256 字节编码 + 512 字节 PCM，沿用 ES8311
驱动和输出音量 API。独立 worker 独占播放，三条优先级队列各四项；高优先级清空低优先级等待
队列，在下一 16ms 分块抢占。普通冷却 30 秒、严重 60 秒，严重绕过静音默认 OFF；首次同步一次，
正常快照不播报。读文件/播放失败计入诊断，缺 VoiceFS 不格式化。严重语音为通用提示，具体内容
在 Services 显示。CPU/RAM/Disk >=90% 边沿触发，回落后重新布防。

## UI、电源与内存

六页、六个设置分类、两页诊断和 QR 共用固定控件树，静态 label、三个 bar、一个固定 I2 趋势画布、
一个 QR。历史为两组 uint8_t[60]；192×80 四色画布含调色板共 3,856 静态字节，纯整数光栅化
避免每条曲线线段分配绘制任务。原 LVGL line chart 在填满曲线的主机测试中耗尽 24 KiB 池，已替换；不重建屏幕、无 framebuffer/PSRAM。
应用 LVGL 池 32 KiB，局部 RGB565 DMA 9,600 字节，只有配网二维码变更时产生 QR 编码分配。

| 任务 | 栈字节 | 用途 |
| --- | ---: | --- |
| main | 6144，启动后释放 | 初始化 |
| event_bus | 3072 | 非阻塞事件分发 |
| config_worker | 4096 | 持久化 |
| monitor_wifi | 5120 | Wi-Fi/AP |
| captive_dns | 2048 | 临时 DNS |
| HTTP portal | 8192，配网期间 | 表单 |
| server_monitor | 8192 | 网络状态/HTTP |
| WebSocket | 6144，活动期间 | 官方客户端 |
| http_deadline | 2048 | 超时中断 |
| audio_worker | 4096 | 流式播放 |
| ota_worker | 6144 | OTA |
| monitor_ui | 4096 | LVGL |
| monitor_telemetry | 3072 | 电池/Heap |

网络 RX/TX 各 1024 字节，HTTP/WSS 共用约 4.1 KiB 接收区，只在 WebSocket 关闭并结束回调后复用，六项结构体网络队列，音频队列仅存
编号。Wi-Fi/lwIP/TLS/LVGL/I2S DMA 另有运行时分配。链接器 RAM 不能证明运行时可用 Heap，
须真机测最小 Heap、最大连续块与每个任务栈水位。

Live 超时熄屏但保留 WSS；Balanced 调暗到 10% 保留 WSS；Battery Saver 环境模式熄屏、断开
WSS、每 60 秒 HTTPS，唤醒恢复实时。首个按键只唤醒；无 light/deep sleep，续航和电流未测。

## 分区与 OTA

变更前：8MB；NVS 0x9000/0x6000、PHY 0xf000/0x1000、factory 0x10000/0x7f0000，
旧 app 1,463,232 字节；语音总计 754,080 字节，另计 SPIFFS 开销。

| 分区 | 偏移 | 大小 |
| --- | --- | --- |
| nvs | 0x9000 | 0x6000 / 24 KiB |
| phy_init | 0xf000 | 0x1000 / 4 KiB |
| otadata | 0x10000 | 0x2000 / 8 KiB |
| ota_0 | 0x20000 | 0x300000 / 3 MiB |
| ota_1 | 0x320000 | 0x300000 / 3 MiB |
| voicefs | 0x620000 | 0x1d0000 / 1856 KiB |
| coredump | 0x7f0000 | 0x10000 / 64 KiB |

app 64 KiB 对齐、双槽等大。gate 检查 8MB 边界、重叠、镜像偏移、槽容量和合并字节。
app-only OTA 不改变分区/VoiceFS。首次 factory→OTA 迁移必须另行审核与授权写设备；
合并镜像填充空隙，未来烧录可能重置 NVS。

manifest 在配置 API 同源 `/firmware/manifest.json`：

```json
{"version":"0.2.0-beta.2","url":"https://status.example.com/firmware/app.bin","sha256":"<64 hex digits>","size":1500000}
```

实际服务端 manifest/镜像尚未部署。仅接受同源 HTTPS、新版本、适合 inactive slot 的 app 镜像。
检查 Content-Length、流式 SHA256、ESP 镜像、项目/版本后才切换 boot slot。TLS 保护 manifest，
独立签名和 secure boot 未实现。网络消息不自动安装，用户在设备 Update 页再次确认。
新固件 30 秒后要求 NVS、显示/UI 和 network task 基本运行，再 mark valid；失败 pending app
重启可回滚。断电/回滚仍待真机验证。

## Agent 与认证

Ubuntu 24.04、Python3、psutil、aiohttp。单采样线程→加锁最新缓存，WSS 每两秒推送、REST
读取相同数据。固定合法 systemctl 参数每五秒查询服务，温度可空。最多 16 个 WSS 客户端，
发送有超时，不为客户端重复采样。Bearer 常量时间比较，拒绝重复认证头和 query，关闭请求日志。
配置由 systemd 从 /etc/passport-status.env 读取，仅监听 127.0.0.1:8765，禁止 root。
Nginx 转发 Upgrade/Connection/Authorization，关闭缓存和缓冲，WSS 65 秒读超时大于 20+10
心跳周期。未修改真实服务器；示例固件路由默认关闭，需运营者另行安全部署。

## 验收边界

主机测试和固件编译是发布 gate。物理显示、扫码、手机配网/重连、音质、并发 Heap、电流、
OTA 断电/回滚均 NOT TESTED。Coredump/Flash 可能含秘密，留本地。NVS 加密、安全启动、独立
签名 manifest 为后续计划；service/message/update_available、任意文字播报、VoiceFS OTA
未实现。此次无烧录、串口连接、擦除或设备复位。
