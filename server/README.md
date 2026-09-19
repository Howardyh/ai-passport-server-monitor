[简体中文](README.zh_CN.md)

# Ubuntu 24.04 status agent

Deployment target: the owner's Tencent Cloud VPS. Run this agent on that VPS
to monitor the same machine; it does not connect to or administer other servers.
In the Tencent Cloud security group, allow HTTPS/443 to Nginx as appropriate
and keep 8765 closed. Configure a domain A/AAAA record for the VPS before issuing
its public-CA certificate. API hostname remains configurable on the device.

This folder provides the read-only metrics service and an Nginx example. These
instructions are for a later server deployment; this firmware build task does
not install services on your server or request SSH access.

## Install

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

Replace `PLACEHOLDER` with a cryptographically random 32-128 character token.
Generate it in a local password manager and transfer it to device provisioning
over the temporary WPA2 hotspot. Do not paste it into Git, URLs, shell commands
or screenshots. The service reads the root-owned file through systemd's
`EnvironmentFile` before dropping to `User=passport-status`. The agent process
never runs as root. Installation commands may require administration.

If the service account already exists, keep it and skip useradd. Install the unit:

```sh
sudo install -m 644 passport-status.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now passport-status
sudo systemctl status passport-status
```

Configuration options: `PASSPORT_STATUS_DISK=/`, optional
`PASSPORT_STATUS_INTERFACE` (blank sums non-loopback interfaces), and systemd
unit names for NGINX, MARIADB and PHP_FPM. Defaults are `nginx.service`,
`mariadb.service`, `php8.3-fpm.service`. Set a physical NIC explicitly if bridges
or virtual interfaces would double-count traffic. Unit names are validated and
queried without a shell. Failed/inactive/unavailable units report `false`.

## Nginx / HTTPS

Install a valid public-CA certificate for your own status hostname first. Adapt
`nginx-example.conf`, including hostname and certificate paths, and install it
in the normal Nginx `http` context. Test with `sudo nginx -t` before reloading.
Do not open port 8765 in the firewall. The agent binds only **127.0.0.1:8765**;
the address is fixed in source and cannot be configured as 0.0.0.0.

The example exposes only `GET /api/v1/status` over TLS 1.2/1.3, forwards the
Authorization header, disables caching/access logs, sets timeouts and limits
request rate. It does not redirect HTTP. Use a hostname covered by the device's
CA bundle; never weaken verification for self-signed certificates.

## API

`GET /api/v1/status`, header `Authorization: Bearer <TOKEN>`.
Responses use `application/json`, `Cache-Control: no-store`.
See [status.example.json](status.example.json). Version 1 includes timestamp,
hostname, uptime, CPU usage/load/nullable temperature, memory and disk totals,
network counters and rates, and three service booleans. RX/TX rates are **bytes
per second**, derived using monotonic elapsed time; counter resets clamp to 0.
CPU temperature only uses known CPU sensor groups; absent sensors return null.
Memory used is total minus available, matching the reported percent. Disk
percent follows psutil and may account for filesystem reserved space.

Sampling runs once per second independently of requests; unit states refresh
every five seconds. Sampling errors preserve the cached sample, but samples
older than ten seconds produce 503. The device independently checks timestamps.
HTTP handling uses a bounded single listener with a two-second client timeout.
It never spawns a new process for each HTTP request. Nginx terminates public TLS.

| Code | Meaning |
| --- | --- |
| 200 | Valid cached snapshot |
| 401 | Missing, duplicate or incorrect bearer header |
| 404 | Unknown path or query string |
| 405 | Unsupported method |
| 503 | No recent sample |

## Validation and troubleshooting

From the repository root, run `python tests/test_status_agent.py` with psutil
installed. Tests use a temporary loopback port, synthetic credentials and mocked
Linux metrics; they do not configure a real server. Verify deployment later:
non-root UID, loopback binding, valid certificate/hostname, 401 without auth,
correct metrics and units, temperature null, reboot/counter reset, and Nginx or
agent downtime. Check `journalctl -u passport-status` for sanitized sampling errors.
If PHP-FPM is false, check the installed PHP version and configured unit name.

Token rotation requires updating `/etc/passport-status.env`, restarting the
service, and reprovisioning the device. This is a read-only API, not a server
administration interface. Never grant this account sudo rights.
