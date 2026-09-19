#!/usr/bin/env python3
"""Read-only, loopback-only Linux status service. No shell, SSH or root."""
from __future__ import annotations

import hmac
import json
import logging
import math
import os
from pathlib import Path
import re
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import psutil

LOG = logging.getLogger("passport-status")
ENV_FILE = Path("/etc/passport-status.env")
MAX_SNAPSHOT_AGE = 10.0
TOKEN_RE = re.compile(r"[A-Za-z0-9._~+/=-]{32,128}\Z")
UNIT_RE = re.compile(r"[A-Za-z0-9_.@-]+\.service\Z")


def settings() -> dict[str, str]:
    # systemd reads EnvironmentFile as root before running this process as User=.
    values = {key: value for key, value in os.environ.items() if key.startswith("PASSPORT_STATUS_")}
    if "PASSPORT_STATUS_TOKEN" not in values:
        for line in ENV_FILE.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, sep, value = line.partition("=")
            if not sep or not key.startswith("PASSPORT_STATUS_"):
                raise ValueError("Invalid settings file")
            values[key] = value.strip().strip('"').strip("'")
    if not TOKEN_RE.fullmatch(values.get("PASSPORT_STATUS_TOKEN", "")):
        raise ValueError("Configure a 32-128 character random token")
    return values


def active(unit: str) -> bool:
    if not UNIT_RE.fullmatch(unit) or unit.startswith("-"):
        raise ValueError("Invalid systemd unit name")
    try:
        result = subprocess.run(
            ["/usr/bin/systemctl", "is-active", "--quiet", unit],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, timeout=0.75, check=False,
        )
        return result.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def cpu_temperature() -> float | None:
    try:
        groups = psutil.sensors_temperatures()
        # Never substitute a disk/NVMe sensor for CPU temperature.
        for group in ("coretemp", "k10temp", "cpu_thermal", "zenpower"):
            for sensor in groups.get(group, []):
                value = sensor.current
                if value is not None and math.isfinite(value) and -100 <= value <= 250:
                    return round(float(value), 1)
    except (AttributeError, OSError, NotImplementedError):
        pass
    return None


def network_totals(interface: str) -> tuple[int, int]:
    counters = psutil.net_io_counters(pernic=True)
    if interface:
        if interface not in counters:
            raise ValueError("Configured network interface is unavailable")
        selected = [counters[interface]]
    else:
        selected = [value for name, value in counters.items() if name != "lo"]
    return sum(v.bytes_recv for v in selected), sum(v.bytes_sent for v in selected)


class Collector:
    def __init__(self, config: dict[str, str]):
        self.disk_path = config.get("PASSPORT_STATUS_DISK", "/")
        self.interface = config.get("PASSPORT_STATUS_INTERFACE", "")
        self.units = {
            "nginx": config.get("PASSPORT_STATUS_NGINX_UNIT", "nginx.service"),
            "mariadb": config.get("PASSPORT_STATUS_MARIADB_UNIT", "mariadb.service"),
            "php_fpm": config.get("PASSPORT_STATUS_PHP_FPM_UNIT", "php8.3-fpm.service"),
        }
        if not all(UNIT_RE.fullmatch(unit) and not unit.startswith("-") for unit in self.units.values()):
            raise ValueError("Invalid unit name")
        self.previous = network_totals(self.interface)
        self.previous_at = time.monotonic()
        self.services = {name: False for name in self.units}
        self.next_services = 0.0
        psutil.cpu_percent(interval=None)  # prime measurement on the sampling thread

    def sample(self) -> dict:
        if time.monotonic() >= self.next_services:
            self.services = {name: active(unit) for name, unit in self.units.items()}
            self.next_services = time.monotonic() + 5
        now = time.monotonic()
        rx, tx = network_totals(self.interface)
        elapsed = max(now - self.previous_at, 0.001)
        rx_rate = max(0, rx - self.previous[0]) / elapsed
        tx_rate = max(0, tx - self.previous[1]) / elapsed
        self.previous, self.previous_at = (rx, tx), now
        memory = psutil.virtual_memory()
        disk = psutil.disk_usage(self.disk_path)
        load1, load5, load15 = os.getloadavg()
        hostname = socket.gethostname()
        hostname = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in hostname)[:63] or "server"
        # RAM used matches the utilization percentage (total minus available).
        return {
            "version": 1, "timestamp": int(time.time()), "hostname": hostname,
            "uptime": max(0, int(time.time() - psutil.boot_time())),
            "cpu": {"usage": psutil.cpu_percent(interval=None), "load1": load1, "load5": load5,
                    "load15": load15, "temperature": cpu_temperature()},
            "memory": {"total": memory.total, "used": memory.total - memory.available, "percent": memory.percent},
            "disk": {"total": disk.total, "used": disk.used, "percent": disk.percent},
            "network": {"rx_bytes": rx, "tx_bytes": tx, "rx_bps": round(rx_rate, 2), "tx_bps": round(tx_rate, 2)},
            "services": dict(self.services),
        }


class Snapshot:
    def __init__(self):
        self.lock = threading.Lock()
        self.body: bytes | None = None
        self.updated = 0.0

    def store(self, data: dict) -> None:
        body = json.dumps(data, separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(body) > 4096:
            raise ValueError("Snapshot exceeds firmware response limit")
        with self.lock:
            self.body, self.updated = body, time.monotonic()

    def read(self) -> bytes | None:
        with self.lock:
            return self.body if self.body is not None and time.monotonic() - self.updated <= MAX_SNAPSHOT_AGE else None


def collect(config: dict[str, str], snapshot: Snapshot, stop: threading.Event) -> None:
    collector = None
    while not stop.is_set():
        try:
            if collector is None:
                collector = Collector(config)
                if stop.wait(1):
                    break
            snapshot.store(collector.sample())
        except Exception as exc:
            # Log the exception type only: environment values never enter logs.
            LOG.warning("Sampling failed (%s)", type(exc).__name__)
        stop.wait(1)


def handler_for(token: str, snapshot: Snapshot):
    expected = ("Bearer " + token).encode("ascii")

    class Handler(BaseHTTPRequestHandler):
        server_version = "PassportStatus/1"
        sys_version = ""

        def setup(self):
            self.request.settimeout(2)
            super().setup()

        def log_message(self, fmt, *args):
            pass  # No request path, Authorization header, or query is logged.

        def reply(self, code: int, body: bytes):
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            if code == 401:
                self.send_header("WWW-Authenticate", "Bearer")
            if code == 405:
                self.send_header("Allow", "GET")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)
            self.close_connection = True

        def do_GET(self):
            headers = self.headers.get_all("Authorization", [])
            provided = headers[0].encode("utf-8") if len(headers) == 1 else b""
            if len(provided) > 256 or not hmac.compare_digest(provided, expected):
                self.reply(401, b'{"error":"unauthorized"}')
                return
            if self.path != "/api/v1/status":
                self.reply(404, b'{"error":"not_found"}')
                return
            body = snapshot.read()
            self.reply(200, body) if body is not None else self.reply(503, b'{"error":"status_unavailable"}')

        def reject(self):
            self.reply(405, b'{"error":"method_not_allowed"}')

        do_POST = do_PUT = do_DELETE = do_PATCH = do_HEAD = do_OPTIONS = reject

    return Handler


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    if not hasattr(os, "geteuid") or os.geteuid() == 0:
        raise SystemExit("Run on Linux as a dedicated unprivileged user; root is prohibited")
    try:
        config = settings()
    except (OSError, ValueError):
        raise SystemExit("Cannot load valid settings from /etc/passport-status.env") from None
    snapshot, stop = Snapshot(), threading.Event()
    thread = threading.Thread(target=collect, args=(config, snapshot, stop), daemon=True, name="sampler")
    server = HTTPServer(("127.0.0.1", 8765), handler_for(config["PASSPORT_STATUS_TOKEN"], snapshot))
    thread.start()
    LOG.info("Listening on 127.0.0.1:8765")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    main()
