[English](README.md)

# Ubuntu 24.04 状态 Agent

部署目标为用户的腾讯云 VPS：Agent 在这台 VPS 上运行，首先监视它自身，
不连接或管理其他服务器。腾讯云安全组按需放行 Nginx HTTPS/443，保持 8765 关闭。
先为 VPS 配置域名 A/AAAA 记录，再签发公共 CA 证书；设备 API 域名保持可配置。

此目录包含只读指标服务和 Nginx 示例，供部署自己的服务器。项目所有者已于
2026-09-21 部署并验证公网 REST/WSS、TLS 和鉴权。公开的是自托管软件，不提供公共
监控账号或 API 凭据。

## 安装

```sh
sudo apt update
sudo apt install python3 python3-venv nginx
sudo useradd --system --no-create-home --shell /usr/sbin/nologin passport-status
sudo install -d -m 755 /opt/passport-status
sudo install -m 644 passport_status.py requirements.txt /opt/passport-status/
sudo python3 -m venv /opt/passport-status/.venv
sudo /opt/passport-status/.venv/bin/pip install -r /opt/passport-status/requirements.txt
sudo install -m 600 .env.example /etc/passport-status.env
sudoedit /etc/passport-status.env
```

将 `PLACEHOLDER` 替换为密码管理器生成的 32–128 字符密码学随机 Token，
通过临时 WPA2 热点的设备配网页面填写同一个值。不要放进 Git、URL、命令历史或截图。
systemd 先通过 `EnvironmentFile` 读取 root 所有的文件，再以
`User=passport-status` 启动进程。Agent 自身不以 root 运行；安装操作需要管理权限。
若用户已存在，保留并跳过 useradd。

```sh
sudo install -m 644 passport-status.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now passport-status
sudo systemctl status passport-status
```

可配置 `PASSPORT_STATUS_DISK=/`、`PASSPORT_STATUS_INTERFACE`（空值汇总除 lo 外接口），
以及 NGINX/MARIADB/PHP_FPM 对应 systemd 单元，默认是 `nginx.service`、
`mariadb.service`、`php8.3-fpm.service`。桥接或虚拟网卡可能重复统计，建议指定物理接口。
单元名经过验证，查询不用 shell；停止、缺失或查询失败均返回 false。

## Nginx / HTTPS

先为自己的状态域名安装有效公共 CA 证书，然后修改 `nginx-example.conf` 的域名和
证书路径，放入 Nginx `http` 上下文。重载前执行 `sudo nginx -t`。
不要对外开放 8765。Agent 固定绑定 **127.0.0.1:8765**，不可配置为 0.0.0.0。
示例仅通过 TLS 1.2/1.3 暴露 GET 状态路径，转发 Authorization，禁用缓存和访问日志，
设置超时及限流。不提供 HTTP 重定向。证书须被设备 CA bundle 信任，禁止为了自签名证书关闭验证。

## API

`GET /api/v1/status`，头为 `Authorization: Bearer <TOKEN>`。
响应使用 `application/json`、`Cache-Control: no-store`，结构见
[status.example.json](status.example.json)。version 1 包含时间戳、主机名、运行时长、
CPU/Load/可空温度、内存与磁盘总量、网络累计及速率、三项服务布尔值。
RX/TX 单位是**字节/秒**，基于单调时间差计算，计数器复位时负差值归零。
温度仅采用已知 CPU 传感器组，无传感器返回 null；内存已用为总量减可用，与百分比一致；
磁盘百分比沿用 psutil，可能计入文件系统保留空间影响。

指标每秒独立采样，服务状态每 5 秒更新。采样异常保留缓存，但超过 10 秒即返回 503；
设备也会检查时间戳。aiohttp 的 REST/WSS 使用同一缓存，最多 16 个 WSS 客户端，发送有超时，不为每个请求启动采样进程。
公网 TLS 由 Nginx 处理。

| 状态码 | 含义 |
| --- | --- |
| 200 | 有效近期快照 |
| 401 | 缺少、重复或错误认证头 |
| 404 | 未知路径或查询参数 |
| 405 | 不支持的方法 |
| 503 | 无近期数据 |

## 验证与排查

安装 psutil 后从仓库根运行 `python tests/test_status_agent.py`。测试使用临时本机端口、
随机测试凭据和模拟 Linux 指标，不配置真实服务器。后续部署需验证：非 root UID、
本机监听、证书和域名、无认证 401、真实指标和单元、温度 null、服务器重启/计数器复位、
Nginx/Agent 停机恢复。查看 `journalctl -u passport-status` 中不含凭据的错误类型。
PHP-FPM 显示 false 时检查实际 PHP 版本和配置的单元名。

Token 轮换需更新 `/etc/passport-status.env`、重启 Agent 并重新配置设备。
这是只读状态 API，不是服务器管理入口，禁止给服务账户 sudo 权限。

## v0.2.0-beta.1 WSS 协议升级

Agent 使用 aiohttp，同一个最新缓存供 GET /api/v1/status 与 /ws，仍只绑定 127.0.0.1:8765。
每秒采样、每两秒广播，最多 16 个客户端，不为每个请求重新采样。协议为 v=1/type=status/seq/
timestamp；CPU/RAM/Disk >=90% 推送边沿 alert。WebSocket 在 upgrade 前验证 Bearer header，
禁止 query Token/重复认证头。20 秒心跳，Nginx 转发 Upgrade/Connection，65 秒读超时。
运行测试前安装 requirements.txt 内的 psutil 与 aiohttp。实际 REST/WSS 服务已部署验证；OTA manifest 和固件未发布。
