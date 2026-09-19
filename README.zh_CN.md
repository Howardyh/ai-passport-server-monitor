[English](README.md)

# AI Passport Server Monitor

适用于 FoloToy AI Passport 的实时服务器监控固件。

基于 FoloToy AI Passport 固件与硬件平台，是独立社区项目，并非 FoloToy
官方产品。课程表固件是另一个独立项目，本仓库不包含其源码或历史。

官方基础：FoloToy/ai-passport main，提交
`31759c4d63dd0d0d6580d6f74639ed5315bcd2c3`。

硬件：ESP32-C3，8 MB Flash，无 PSRAM；ST7789P3 240 x 320 竖屏 RGB565；
CW2017 电量计；ESP-IDF 5.5.3。许可证见 [LICENSE](LICENSE)。

## Hardware / 硬件

引脚、ADC 按键阈值和 CW2017 操作全部来自未修改的官方 `components/bsp`。
应用 CMake 只编译 Server Monitor，唯一链接的入口是 `main/monitor_app.c`。
官方参考测试菜单、示例页面、音频和蓝牙不进入应用；无 ICS、课程数据、课程页面或提醒。

## Architecture / 架构

```text
Ubuntu 24.04 -> psutil 采样 -> 127.0.0.1:8765 -> Nginx HTTPS
                                                  |
                                  Wi-Fi -> server_monitor_task
                                                  |
                              有界 JSON -> ServerState 互斥锁
                                                  |
                              monitor_ui -> LVGL 锁 -> 屏幕
```

设备不使用 SSH，仅通过带认证的 HTTPS GET 读取指标。配网页面仅存在于临时 WPA2 热点。

## Screenshots / UI placeholder

黑底白字，四个固定页面和 Settings。[布局示意](assets/images/server-monitor-preview.svg)
供设计审核，并非真机截图；实际显示仍待人工批准后的真机验证。

| 页面 | 内容 |
| --- | --- |
| Overview | CPU/RAM/DISK 百分比、进度条、NORMAL/WARNING/CRITICAL；RX/TX、运行时长、更新年龄 |
| System | CPU 使用率、可空温度、Load 1/5/15、RAM 已用/总量/百分比 |
| Network + Storage | 磁盘已用/总量/百分比、RX/TX、HTTPS API 延迟 |
| Services | Nginx/MariaDB/PHP-FPM，本机 SSID、RSSI、CW2017 电量 |
| Settings | SSID、RSSI、API Host、固件版本、电量、重连、配网 |

UP/DOWN 翻页；OK 短按立即刷新（最快每秒一次）；OK 长按进入或退出 Settings。
Settings 中 UP/DOWN 选择，OK 执行。临时配网期间才显示热点 setup key；
保存的家庭 Wi-Fi 密码和 API Token 不显示、不记录。无数据使用 `--`，不可用使用 `N/A`。
CPU/RAM 达到 70% 警告，磁盘达到 80% 警告，全部达到 90% 严重；颜色同时配合数值和文字。
网络单位为十进制 B/s、KB/s、MB/s；存储为二进制 GiB；API 延迟不是 ICMP ping。

## Build / Firmware Build

使用 **ESP-IDF 5.5.3**。Linux/macOS 激活环境后运行：

```sh
python -m pip install -r server/requirements.txt
./tools/validate.sh --static
./tools/validate.sh --firmware
sha256sum build/FoloToy-AI-Passport-full.bin
```

Windows 先激活 ESP-IDF PowerShell，再通过 Git Bash 调用同一入口。
Windows 适配器执行相同的 build、merge、布局验证、调试归档流程，每次使用新的
`build/validation-时间戳/sdkconfig`，保留编译目录供审查。项目及 IDF 路径应无空格，
可使用 Windows 盘符别名。主机测试需要 `CC`、安装 psutil 的 Python 和 actionlint。
固件阶段使用激活的 IDF Python。

配置来自跟踪的 `sdkconfig.defaults`，不读取被忽略的本机 sdkconfig。
产物是 `build/FoloToy-AI-Passport-full.bin`；对应 ELF、MAP、应用、bootloader、
分区表和 `flash_args` 保存在 `build/firmware/<SHA256>/`。
用 `python tools/archive_firmware.py verify <bundle>` 校验归档。
保持官方 8 MB NVS/PHY/factory 分区布局，无 OTA。固件和调试文件不进 Git，不由 CI 上传。
[CI](.github/workflows/build.yml) 固定 ESP-IDF 5.5.3，只运行编译验证，不发布 Release。

## Server Agent / Nginx / API

参阅 [Ubuntu 部署指南](server/README.zh_CN.md)。Python Agent 必须非 root 运行，
独立采样，只提供 `GET /api/v1/status`。Nginx 使用有效公共 CA 证书提供 HTTPS。
JSON 响应设置 `Cache-Control: no-store`，认证为 `Authorization: Bearer <TOKEN>`。
Token 通过 `/etc/passport-status.env` 读取，不编入固件。
[完整 API 示例](server/status.example.json) 使用 version 1，CPU 温度允许 `null`。
电量和 RSSI 由设备本地读取。

## Wi-Fi Setup / 配网

1. 启动直接进入 Server Monitor，从 NVS 加载已有 STA 凭据。
2. 长按 OK，在 Settings 选择 **Wi-Fi Provisioning** 后按 OK。
3. 用屏幕显示的临时 setup key 连接 `Passport-xxxx` WPA2 热点。
4. 打开 `http://192.168.4.1`，填写 2.4 GHz WPA2/WPA3 兼容网络 SSID、
   Wi-Fi 密码（8–63 字节）、HTTPS API URL 和随机 Token（32–128 字符）。
5. 保存后查看设备是否成功；热点关闭，设备连接 STA、通过 SNTP 校时，再发起严格校验证书的 HTTPS。

热点仅容许一个客户端，5 分钟自动关闭。期间 STA 禁用，配网页面不会暴露到正常局域网。
每个会话使用随机 nonce 防止跨站配置请求。验证后整体保存单个带版本的 NVS blob，
失败保留旧设置；不会自动擦除 NVS。重新配网可替换设置。本版不支持开放网络或 64 位原始 PSK。

## 可靠性与内存

Wi-Fi 退避 2/4/8/16/30 秒，单次关联最多 20 秒。HTTPS 成功后每 5 秒刷新，
失败退避 5/10/20/30/60 秒。HTTP I/O 约 3 秒超时，独立 socket 截止任务中断慢响应头和响应体。
DNS 使用 IDF/lwIP 解析器，可能超过 HTTP I/O 时限，但不会阻塞 LVGL 或按键。

最近成功接收未超过 15 秒为 ONLINE；之后为 STALE 并保留全部旧数据；从未获取有效
数据为 OFFLINE。更换 API 配置会清空上一端点数据。未知字段忽略，必需字段缺失或错误
会原子拒绝整个响应；拒绝超过 4096 字节、嵌套超过 8 层、重复字段、非有限数值、
越界百分比，以及落后当前时间超过 15 秒或超前超过 30 秒的时间戳。温度 `null` 合法。

控件仅创建一次，复用静态 label 缓冲和三个 bar。BSP 显示缓冲约 9.6 KB，LVGL 池 24 KB，
无完整 framebuffer 或 PSRAM。JSON 静态缓冲 4097 字节。任务栈：HTTPS 8192、
Wi-Fi 5120、UI 4096、电池/遥测 3072、截止保护 2048 字节；配网 HTTP 仅活动期间使用 6144 字节栈。
每 30 秒记录空闲堆、最低空闲堆、最大连续块。运行时内存及栈水位仍需真机测量，
不能仅凭链接器 RAM 数字宣称稳定。

## Security / 安全

使用 `esp_crt_bundle_attach`、正常域名验证和 SNTP 时间；不禁用证书验证。
拒绝重定向，避免把 Authorization 转给其他主机。API URL 必须 HTTPS，不能含
用户信息、查询参数或 fragment。设备无 SSH、服务器命令执行或远程控制接口。
Agent 只以参数数组和限时查询固定 systemd 单元，不启停服务，拒绝 root 运行。

`.gitignore` 排除凭据、私有配置、密钥、证书、构建和日志，example 仅含占位值。
提交/推送前执行 `python tools/check_repo.py`、`python tools/check-monitor-secrets.py`
并审查 `git diff --cached`。不要把 Token 放入 URL 或 shell 历史。
开发版本 NVS 未加密，物理读取 Flash 可能获取凭据；Secure Boot、Flash/NVS 加密
和 Token 轮换属于后续部署决策，本轮不会改变设备安全状态。

## Flash / 烧录

**当前未获准烧录。** 本轮仅构建、验证、提交并推送源码，然后停止，等待人工审核。
不探测串口、不重启、不擦除、不烧录。只有后续明确说“可以烧录”才能进行设备写入。
合并镜像用于偏移 `0x0`，写入可能替换 NVS 数据，未来烧录前需要核对设备与存储影响。
详见官方 [分区策略](docs/development/engineering/firmware-layout.zh_CN.md)。

## Troubleshooting / 排查

- Wi-Fi 断线：检查 2.4 GHz、密码和 RSSI，Settings 重连或配网。
- 等待校时：检查互联网和 UDP/123，TLS 不会绕过验证。
- HTTPS/TLS/DNS：检查 DNS、证书链、到期时间和域名。
- HTTP 401：核对或轮换两端 Token；404：检查路径。
- HTTP 500/503：检查 Nginx、`journalctl -u passport-status` 和采样配置。
- JSON 错误：核对 version 1 结构和大小；VPS 温度 N/A 正常。
- 电量 N/A：获准真机测试后通过官方 BSP 检查 CW2017。
- NVS 保存失败：检查存储，不会自动擦除或进入重启循环。

## License / 许可证

MIT，完整保留 FoloToy 版权声明和 [LICENSE](LICENSE)。
官方项目：[FoloToy/ai-passport](https://github.com/FoloToy/ai-passport)。
